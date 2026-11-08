"""Shared setup logic for the ESP32-S3 RF filter controller.

Everything that talks to the board, the serial port or esptool lives here. The
guided CLI (cli.py) and the window (gui.py) only collect answers and call these
functions, so both always behave the same way.

Nothing here prints. Progress goes through the `log` callback each function
takes, so a front end can show it however it likes.

Modelled on esp32p4_nts_time_server's setup tool: the board proves who it is by
the certificate fingerprint it prints on its USB console, is claimed with an
enrollment code read from the same console, and afterwards only accepts
management from a client certificate signed by your management CA.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import http.client
import ipaddress
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Optional

Log = Callable[[str], None]


def _quiet(_message: str) -> None:
    pass


class SetupError(Exception):
    """A step failed in a way the user can act on. The message says how."""


# ---------------------------------------------------------------------------
# Console output
# ---------------------------------------------------------------------------

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Boot log lines, in the time server's wording.
RE_CODE = re.compile(r"enrollment code:\s*([0-9A-F]{32})\b")
RE_FINGERPRINT = re.compile(r"(?:Bootstrap|Server) certificate SHA-256:\s*([0-9A-Fa-f]{64})\b")
RE_IPV4 = re.compile(r"(?:DHCP|Static) IPv4 acquired:\s*(\d{1,3}(?:\.\d{1,3}){3})")
RE_PROVISIONED = re.compile(r"Management ownership is provisioned")
RE_VERSION = re.compile(r"App version:\s*(\S+)")
# Lines from the USB console's status command.
RE_FIRMWARE = re.compile(r"^Firmware:\s*(\S+)")
RE_STATUS_IP = re.compile(r"^IPv4 address:\s*(\d{1,3}(?:\.\d{1,3}){3})")
RE_WIFI = re.compile(r"^Wi-Fi credentials:\s*(provisioned|missing)")
RE_MANAGEMENT = re.compile(r"^Management:\s*(claimed|unclaimed)")


@dataclass
class BoardInfo:
    """What the board reported on its USB console."""
    enrollment_code: Optional[str] = None
    fingerprint: Optional[str] = None
    ip: Optional[str] = None
    provisioned: bool = False
    wifi: Optional[bool] = None  # None: not reported
    version: Optional[str] = None

    def complete(self) -> bool:
        """Enough to reach the board over HTTPS: its address and fingerprint,
        and the enrollment code while it is unclaimed."""
        if not (self.ip and self.fingerprint):
            return False
        return self.provisioned or bool(self.enrollment_code)


def parse_console_line(line: str, info: BoardInfo) -> None:
    """Update `info` from one line of the board's console output."""
    text = ANSI.sub("", line).strip()
    if (m := RE_CODE.search(text)):
        info.enrollment_code = m.group(1)
    if (m := RE_FINGERPRINT.search(text)):
        info.fingerprint = m.group(1).upper()
    if (m := RE_IPV4.search(text)) or (m := RE_STATUS_IP.search(text)):
        info.ip = m.group(1)
    if RE_PROVISIONED.search(text):
        info.provisioned = True
    if (m := RE_VERSION.search(text)) or (m := RE_FIRMWARE.search(text)):
        info.version = m.group(1)
    if (m := RE_WIFI.search(text)):
        info.wifi = m.group(1) == "provisioned"
    if (m := RE_MANAGEMENT.search(text)):
        info.provisioned = m.group(1) == "claimed"


def parse_console(text: str, info: Optional[BoardInfo] = None) -> BoardInfo:
    info = info or BoardInfo()
    for line in text.replace("\r", "\n").split("\n"):
        parse_console_line(line, info)
    return info


# ---------------------------------------------------------------------------
# Serial port
# ---------------------------------------------------------------------------

def list_serial_ports() -> list[tuple[str, str]]:
    """(device, description) for each serial port, the board's native USB port first."""
    from serial.tools import list_ports
    ports = []
    for p in list_ports.comports():
        desc = p.description or ""
        if p.manufacturer and p.manufacturer not in desc:
            desc = f"{desc} ({p.manufacturer})"
        ports.append((p.device, desc))

    def rank(d):
        low = d[1].lower()
        if "jtag" in low or "espressif" in low:
            return 0
        return 1 if any(k in low for k in ("usb", "uart", "cp210", "ch34", "ftdi")) else 2
    ports.sort(key=lambda d: (rank(d), d[0]))
    return ports


PROMPT = "rf> "


class Console:
    """The board's USB console, opened without touching its reset lines."""

    def __init__(self, port: str):
        self.port = port
        self.ser = None

    def __enter__(self) -> "Console":
        import serial
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = self.port, 115200, 0.2
        # Opening a USB Serial/JTAG port with these lines asserted resets the chip.
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
        except serial.SerialException as exc:
            raise SetupError(f"Cannot open {self.port}: {exc}. Close any serial monitor using it "
                             "and try again.") from exc
        self.ser = ser
        return self

    def __exit__(self, *exc) -> None:
        self.ser.close()

    def read_until(self, pattern: str, timeout: float) -> str:
        """Read until the regular expression `pattern` matches, or the timeout passes."""
        rx = re.compile(pattern)
        out = ""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk.decode("utf-8", "replace")
                if rx.search(ANSI.sub("", out)):
                    break
        return out

    def send(self, text: str) -> None:
        self.ser.write(text.encode("utf-8") + b"\r")
        self.ser.flush()

    def command(self, text: str, timeout: float = 4.0) -> str:
        self.send(text)
        return self.read_until(re.escape(PROMPT) + r"$", timeout)

    def wake(self) -> bool:
        """Get a fresh prompt (a half-typed line is cancelled first)."""
        self.ser.reset_input_buffer()
        self.ser.write(b"\x03")
        self.send("")
        return PROMPT in self.read_until(re.escape(PROMPT), 3.0)


def query_board(port: str) -> BoardInfo:
    """Ask the running board for its state with the status and enrollment commands."""
    with Console(port) as con:
        if not con.wake():
            raise SetupError(f"The board did not answer on {port}. Choose its native USB port "
                             "(USB Serial/JTAG), and check it runs the RF controller firmware.")
        text = con.command("status") + con.command("enrollment")
    info = parse_console(text)
    if not info.version:
        raise SetupError(f"{port} answered, but not like an RF controller. Check the port.")
    return info


def wait_for_board(port: str, until: Callable[[BoardInfo], bool], timeout: float = 90.0,
                   log: Log = _quiet) -> BoardInfo:
    """Query the console until `until(info)` holds. The port disappears while the
    board restarts, so failures to open it are retried."""
    deadline = time.monotonic() + timeout
    last: Optional[BoardInfo] = None
    error: Optional[Exception] = None
    while time.monotonic() < deadline:
        try:
            last = query_board(port)
            if until(last):
                return last
        except SetupError as exc:
            error = exc
        except OSError as exc:  # the port vanished mid-read during a restart
            error = exc
        time.sleep(2)
    if last is not None:
        return last
    raise SetupError(f"The board did not come back on {port} within {int(timeout)} s ({error}).")


def reset_board(port: str) -> None:
    """Restart the board through its USB port's reset line, as esptool does."""
    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate = port, 115200
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as exc:
        raise SetupError(f"Cannot open {port}: {exc}") from exc
    def set_rts(state: bool) -> None:
        ser.rts = state
        # Windows' usbser.sys only sends the line state when DTR is written too (esptool does the same).
        ser.dtr = ser.dtr

    try:
        set_rts(True)
        time.sleep(0.2)
        set_rts(False)
        time.sleep(0.2)
    finally:
        try:
            ser.close()
        except (OSError, serial.SerialException):
            pass  # the port can vanish as the chip resets


SSID_MAX = 32
WIFI_PASSWORD_MAX = 64


def set_wifi(port: str, ssid: str, password: str, log: Log = _quiet) -> None:
    """Store Wi-Fi credentials with the console's set-wifi command. The password
    is typed into the console, never logged, and the board doesn't echo it."""
    if not 1 <= len(ssid.encode()) <= SSID_MAX:
        raise SetupError(f"The network name must be 1 to {SSID_MAX} characters.")
    if len(password.encode()) > WIFI_PASSWORD_MAX or (password and len(password) < 8):
        raise SetupError("A Wi-Fi password is 8 to 63 characters, or empty for an open network.")
    if any(ord(c) < 32 or ord(c) == 127 for c in ssid + password):
        raise SetupError("The network name and password cannot contain control characters.")
    with Console(port) as con:
        if not con.wake():
            raise SetupError(f"The board did not answer on {port}.")
        con.send("set-wifi")
        if "SSID:" not in con.read_until(r"SSID: ", 3):
            raise SetupError("The board did not ask for the network name. Is its firmware current?")
        con.send(ssid)
        if "Password" not in con.read_until(r"Password[^:]*: ", 3):
            raise SetupError("The board did not ask for the Wi-Fi password.")
        con.send(password)
        reply = ANSI.sub("", con.read_until(r"(OK;[^\r\n]*|ERROR[^\r\n]*)\r?\n", 5))
    if "ERROR" in reply:
        raise SetupError("The board refused the Wi-Fi settings: " +
                         reply[reply.index("ERROR"):].splitlines()[0])
    if "OK" not in reply:
        raise SetupError("The board did not confirm the Wi-Fi settings.")
    log(f"Stored Wi-Fi network {ssid} on the board.")


# ---------------------------------------------------------------------------
# Flashing (a new board, or one being brought back to a known state)
# ---------------------------------------------------------------------------

CHIP = "esp32s3"


def _is_image_folder(c: Path) -> bool:
    return (c / "flasher_args.json").is_file()


def find_firmware(directory: Optional[str] = None) -> Path:
    """Locate a firmware folder: the release's `firmware` folder, or an ESP-IDF
    build folder. Either holds flasher_args.json and the images it names."""
    candidates = []
    if directory:
        candidates.append(Path(directory))
    here = Path(__file__).resolve().parent
    candidates += [here.parent / "firmware", here.parent.parent / "firmware", Path.cwd() / "firmware"]
    for c in candidates:
        if _is_image_folder(c):
            return c
    raise SetupError("Firmware not found. Point the tool at the release's 'firmware' folder, or at an "
                     "ESP-IDF build folder; either holds flasher_args.json.")


def flash_arguments(folder: Path) -> list[str]:
    """esptool write-flash arguments from flasher_args.json, with full image paths."""
    data = json.loads((folder / "flasher_args.json").read_text())
    chip = data.get("extra_esptool_args", {}).get("chip", CHIP)
    if chip != CHIP:
        raise SetupError(f"That firmware is for {chip}, not the RF controller's ESP32-S3.")
    args = []
    for a in data.get("write_flash_args", []):
        args.append(a.replace("_", "-") if a.startswith("--") else a)
    files = data.get("flash_files", {})
    if not files:
        raise SetupError("flasher_args.json lists no images.")
    for offset, rel in sorted(files.items(), key=lambda kv: int(kv[0], 16)):
        path = folder / rel
        if not path.is_file():
            # A release folder keeps every image at its top level.
            path = folder / Path(rel).name
        if not path.is_file():
            raise SetupError(f"The firmware folder is missing {rel}.")
        args += [offset, str(path)]
    return args


def _esptool(args: list[str], log: Log) -> str:
    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP] + args
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    for line in out.splitlines():
        if line.strip():
            log("  " + line.strip())
    if proc.returncode != 0:
        raise SetupError("esptool failed. Check the cable and port, hold BOOT while pressing RESET if the "
                         "board will not connect, and close any serial monitor.")
    return out


def parse_flash_encryption(out: str) -> bool:
    """True when esptool's get-security-info output says flash encryption is on."""
    if re.search(r"Flash Encryption:\s*Enabled", out):
        return True
    if re.search(r"Flash Encryption:\s*Disabled", out):
        return False
    raise SetupError("Could not read the board's flash-encryption state, so it was not flashed.")


def flash_board(port: str, folder: Path, log: Log = _quiet) -> None:
    """Write the bootloader, partition table and firmware over USB.

    Settings, Wi-Fi credentials, ownership and the bootstrap certificate live
    in NVS, which isn't written, so re-flashing a claimed board keeps its
    claim. A board with flash encryption on is refused: this firmware doesn't
    use it, and plain images would not boot on it.
    """
    args = flash_arguments(folder)
    if parse_flash_encryption(_esptool(["-p", port, "get-security-info"], log)):
        raise SetupError("This board has flash encryption turned on, which this firmware doesn't use. "
                         "Nothing was written.")
    log("Writing firmware (about a minute)...")
    _esptool(["-p", port, "-b", "460800", "--before", "default-reset", "--after", "hard-reset",
              "write-flash"] + args, log)


# ---------------------------------------------------------------------------
# Management credentials
# ---------------------------------------------------------------------------

CREDENTIAL_FILES = ("management-ca.key.pem", "management-ca.crt.pem",
                    "admin-client.key.pem", "admin-client.crt.pem", "admin-client.p12")
CA_NAME = "RF Controller Management CA"
ADMIN_NAME = "rf-admin"


@dataclass
class Credentials:
    folder: Path
    password: Optional[str] = field(default=None, repr=False)  # None: keys not encrypted

    @property
    def ca_cert(self) -> Path: return self.folder / "management-ca.crt.pem"
    @property
    def client_cert(self) -> Path: return self.folder / "admin-client.crt.pem"
    @property
    def client_key(self) -> Path: return self.folder / "admin-client.key.pem"
    @property
    def p12(self) -> Path: return self.folder / "admin-client.p12"

    def names(self) -> tuple[str, str]:
        """(administrator certificate name, issuing CA name), for the browser steps."""
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        cert = x509.load_pem_x509_certificate(self.client_cert.read_bytes())

        def cn(name):
            attrs = name.get_attributes_for_oid(NameOID.COMMON_NAME)
            return attrs[0].value if attrs else name.rfc4514_string()
        return cn(cert.subject), cn(cert.issuer)


def create_credentials(folder: str | Path, password: str, admin_name: str = ADMIN_NAME,
                       log: Log = _quiet) -> Credentials:
    """Create a management CA and an administrator certificate.

    Only the CA certificate ever leaves this computer. Private keys are written
    encrypted with `password`. Existing credentials are never overwritten.
    """
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.serialization import pkcs12
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    if len(password) < 8:
        raise SetupError("Use a password of at least 8 characters.")
    path = Path(folder).expanduser().resolve()
    if any((path / f).exists() for f in CREDENTIAL_FILES):
        raise SetupError(f"{path} already holds credentials. To use them, choose 'use existing "
                         "credentials'; existing credentials are never overwritten.")
    path.mkdir(parents=True, exist_ok=True)
    now = _dt.datetime.now(_dt.timezone.utc) - _dt.timedelta(minutes=5)
    enc = serialization.BestAvailableEncryption(password.encode())

    log("Creating the management certificate authority...")
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, CA_NAME)])
    ca_cert = (x509.CertificateBuilder()
               .subject_name(ca_name).issuer_name(ca_name)
               .public_key(ca_key.public_key())
               .serial_number(x509.random_serial_number())
               .not_valid_before(now).not_valid_after(now + _dt.timedelta(days=3650))
               .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
               .add_extension(x509.KeyUsage(digital_signature=False, content_commitment=False,
                                            key_encipherment=False, data_encipherment=False,
                                            key_agreement=False, key_cert_sign=True, crl_sign=True,
                                            encipher_only=False, decipher_only=False), critical=True)
               .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
               .sign(ca_key, hashes.SHA256()))

    log("Creating the administrator certificate...")
    admin_key = ec.generate_private_key(ec.SECP256R1())
    admin_cert = (x509.CertificateBuilder()
                  .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, admin_name)]))
                  .issuer_name(ca_name)
                  .public_key(admin_key.public_key())
                  .serial_number(x509.random_serial_number())
                  .not_valid_before(now).not_valid_after(now + _dt.timedelta(days=825))
                  .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                  .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                                               key_encipherment=False, data_encipherment=False,
                                               key_agreement=False, key_cert_sign=False, crl_sign=False,
                                               encipher_only=False, decipher_only=False), critical=True)
                  .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
                  .add_extension(x509.SubjectKeyIdentifier.from_public_key(admin_key.public_key()), critical=False)
                  .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
                  .sign(ca_key, hashes.SHA256()))

    pem = serialization.Encoding.PEM
    pkcs8 = serialization.PrivateFormat.PKCS8
    (path / "management-ca.key.pem").write_bytes(ca_key.private_bytes(pem, pkcs8, enc))
    (path / "management-ca.crt.pem").write_bytes(ca_cert.public_bytes(pem))
    (path / "admin-client.key.pem").write_bytes(admin_key.private_bytes(pem, pkcs8, enc))
    (path / "admin-client.crt.pem").write_bytes(admin_cert.public_bytes(pem))
    # AES-256 / SHA-256 protection, which current Windows, macOS and browsers import.
    p12_enc = (serialization.PrivateFormat.PKCS12.encryption_builder()
               .kdf_rounds(50000)
               .key_cert_algorithm(pkcs12.PBES.PBESv2SHA256AndAES256CBC)
               .hmac_hash(hashes.SHA256())
               .build(password.encode()))
    (path / "admin-client.p12").write_bytes(
        pkcs12.serialize_key_and_certificates(admin_name.encode(), admin_key, admin_cert, [ca_cert], p12_enc))
    log(f"Credentials saved in {path}")
    return Credentials(path, password)


def load_credentials(folder: str | Path, password: str = "") -> Credentials:
    """Open an existing credentials folder, such as one the time server's setup
    tool made, and check the password opens its administrator key. A folder
    whose key isn't encrypted needs no password."""
    from cryptography.hazmat.primitives import serialization
    path = Path(folder).expanduser().resolve()
    for f in ("management-ca.crt.pem", "admin-client.key.pem", "admin-client.crt.pem"):
        if not (path / f).is_file():
            raise SetupError(f"{path} has no {f}. Choose a folder made by this tool or the time "
                             "server's setup tool.")
    key = (path / "admin-client.key.pem").read_bytes()
    if b"ENCRYPTED" not in key:
        serialization.load_pem_private_key(key, None)
        return Credentials(path, None)
    if not password:
        raise SetupError("Enter the password for these credentials.")
    try:
        serialization.load_pem_private_key(key, password.encode())
    except (ValueError, TypeError) as exc:
        raise SetupError("That password does not open these credentials.") from exc
    return Credentials(path, password)


# ---------------------------------------------------------------------------
# Talking to the board
# ---------------------------------------------------------------------------

def _fingerprint(der: bytes) -> str:
    return hashlib.sha256(der).hexdigest().upper()


class _Connection(http.client.HTTPSConnection):
    pass


class Board:
    """HTTPS to one board's management API, pinned to the certificate
    fingerprint the board printed on its USB console."""

    def __init__(self, host: str, *, pin: str, credentials: Optional[Credentials] = None,
                 timeout: float = 30.0):
        self.host = host
        self.pin = pin.upper()
        self.credentials = credentials
        self.timeout = timeout

    def _context(self) -> ssl.SSLContext:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # replaced by the fingerprint check below
        if self.credentials:
            ctx.load_cert_chain(str(self.credentials.client_cert), str(self.credentials.client_key),
                                password=self.credentials.password)
        return ctx

    def request(self, method: str, path: str, body: Optional[bytes] = None,
                headers: Optional[dict] = None, timeout: Optional[float] = None) -> tuple[int, bytes]:
        conn = _Connection(self.host, 443, context=self._context(), timeout=timeout or self.timeout)
        try:
            conn.connect()
            seen = _fingerprint(conn.sock.getpeercert(binary_form=True))
            if seen != self.pin:
                raise SetupError(
                    "The board's certificate does not match the fingerprint it printed on its USB "
                    f"console (expected {self.pin[:16]}..., got {seen[:16]}...). Something on the "
                    "network may be intercepting the connection; nothing was sent.")
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def json(self, method: str, path: str, payload=None, headers: Optional[dict] = None,
             timeout: Optional[float] = None, ok=(200, 201, 202)):
        body = json.dumps(payload).encode() if payload is not None else None
        hdrs = {"Content-Type": "application/json"} if body is not None else {}
        hdrs.update(headers or {})
        status, data = self.request(method, path, body, hdrs, timeout)
        if status not in ok:
            text = data.decode("utf-8", "replace").strip()
            raise SetupError(f"{method} {path} was refused ({status}): {text}")
        if not data:
            return {}
        try:
            return json.loads(data)
        except ValueError:
            return {"text": data.decode("utf-8", "replace")}

    # -- enrollment -------------------------------------------------------------
    def claim(self, code: str, ca_pem: bytes) -> None:
        status, data = self.request("PUT", "/api/v1/provisioning/claim", ca_pem,
                                    {"X-Provisioning-Code": code, "Content-Type": "application/x-pem-file"})
        if status == 404:
            raise SetupError("This board is already claimed.")
        if status == 403:
            raise SetupError("The board rejected the enrollment code. Run setup again to read a fresh one.")
        if status == 429:
            raise SetupError("Too many wrong codes: the board has locked enrollment until it is reset.")
        if status not in (200, 201, 202):
            raise SetupError(f"Claim failed ({status}): {data.decode('utf-8', 'replace').strip()}")

    # -- management -------------------------------------------------------------
    def wait_ready(self, timeout: float = 120.0) -> dict:
        """Poll /api/v1/status until the board answers."""
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                return self.json("GET", "/api/v1/status", timeout=10)
            except SetupError as exc:
                if "does not match the fingerprint" in str(exc):
                    raise
                last = exc
            except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
                last = exc
            time.sleep(3)
        raise SetupError(f"The board at {self.host} did not answer within {int(timeout)} s ({last}).")

    def config(self) -> dict:
        return self.json("GET", "/api/v1/config")

    def status(self) -> dict:
        return self.json("GET", "/api/v1/status")

    def set_identity(self, hostname: str, ntp0: str, ntp1: str) -> None:
        self.json("PUT", "/api/v1/config/hostname", {"hostname": hostname})
        self.json("PUT", "/api/v1/config/ntp", {"servers": [ntp0, ntp1]})

    def set_network(self, static: bool, address: str = "", netmask: str = "",
                    gateway: str = "", dns: str = "") -> None:
        payload = {"dhcp": not static}
        if static:
            payload.update(address=address, netmask=netmask, gateway=gateway, dns=dns)
        self.json("PUT", "/api/v1/config/network", payload)

    def reboot(self) -> None:
        self.json("POST", "/api/v1/system/reboot", headers={"X-Confirm-Reboot": "REBOOT"})


# ---------------------------------------------------------------------------
# Small helpers used by both front ends
# ---------------------------------------------------------------------------

def valid_ipv4(text: str) -> bool:
    try:
        ipaddress.IPv4Address(text)
        return True
    except ValueError:
        return False


def valid_hostname(text: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", text))


def valid_server_name(text: str) -> bool:
    """An NTP server: a DNS name or an IPv4 address."""
    if valid_ipv4(text):
        return True
    labels = text.rstrip(".").split(".")
    return len(text) <= 63 and all(valid_hostname(label) for label in labels)


def default_credentials_folder(name: str) -> Path:
    return Path.home() / "Documents" / f"management-credentials-{name or 'rf-controller'}"


def import_into_windows_store(credentials: Credentials, log: Log = _quiet) -> str:
    """Import the administrator certificate into the current user's store so
    Edge and Chrome offer it. Returns the certificate thumbprint."""
    if os.name != "nt":
        raise SetupError("Automatic import is only available on Windows.")
    if not credentials.password or not credentials.p12.is_file():
        raise SetupError("These credentials have no password-protected .p12 to import.")
    script = ("$p = ConvertTo-SecureString $env:RF_SETUP_P12_PASSWORD -AsPlainText -Force; "
              f"$c = Import-PfxCertificate -FilePath '{credentials.p12}' "
              "-CertStoreLocation Cert:\\CurrentUser\\My -Password $p; $c.Thumbprint")
    env = dict(os.environ, RF_SETUP_P12_PASSWORD=credentials.password)
    proc = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                          capture_output=True, text=True, env=env)
    thumb = (proc.stdout or "").strip().splitlines()
    if proc.returncode != 0 or not thumb:
        raise SetupError("Importing the certificate failed: " + (proc.stderr or "").strip())
    log(f"Imported the administrator certificate (thumbprint {thumb[-1]}).")
    return thumb[-1]


@dataclass
class SetupRecord:
    """Non-secret notes about one controller, saved beside the credentials.
    One file per board, named by its certificate fingerprint, so several boards
    (and a time server) can share one credentials folder."""
    hostname: str = ""
    ip: str = ""
    fingerprint: str = ""
    static_ip: bool = False
    firmware: str = ""
    updated: str = ""

    def path(self, folder: Path) -> Path:
        return Path(folder) / f"rf-controller-{(self.fingerprint or 'unknown')[:12].lower()}.json"

    def save(self, folder: Path) -> Path:
        self.updated = _dt.datetime.now().isoformat(timespec="seconds")
        p = self.path(folder)
        p.write_text(json.dumps(asdict(self), indent=2))
        return p
