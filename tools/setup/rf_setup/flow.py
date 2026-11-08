"""The setup sequence, as steps both front ends run in the same order.

Each step takes the answers it needs and does its work through core.py. The
front ends decide how to ask; this module decides what happens and in which
order, so the CLI and the window cannot drift apart.

    1. flash        (optional: a new board, or one that needs this firmware)
    2. read_board   (USB console: firmware, Wi-Fi, address, fingerprint, code)
    3. wifi         (store the network name and password, restart, wait for an address)
    4. credentials  (create new, or open an existing folder such as the time server's)
    5. claim        (skipped if the board is already claimed)
    6. identity     (hostname and the two NTP servers)
    7. network      (keep DHCP, or a static address; restarts the board to apply)
    8. dns          (certificate name, Cloudflare token checked on the board, optional A record)
    9. certificate  (Let's Encrypt; the board switches HTTPS to it, checked by name)
   10. finish       (optional certificate import, saved notes)
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass
from typing import Optional

from . import core
from .core import Board, BoardInfo, Credentials, Log, SetupError, SetupRecord

DEFAULT_NTP = ("time.cloudflare.com", "time.google.com")
DEFAULT_HOSTNAME = "rf-controller"


@dataclass
class Session:
    log: Log
    port: str = ""
    info: Optional[BoardInfo] = None
    credentials: Optional[Credentials] = None
    board: Optional[Board] = None
    record: Optional[SetupRecord] = None
    config: Optional[dict] = None

    # 1 ---------------------------------------------------------------------
    def flash(self, firmware_dir: Optional[str]) -> None:
        folder = core.find_firmware(firmware_dir)
        self.log(f"Firmware: {folder}")
        core.flash_board(self.port, folder, self.log)
        self.log("Firmware written; the board is starting.")

    # 2 ---------------------------------------------------------------------
    def read_board(self, timeout: float = 60.0) -> BoardInfo:
        """Read the board's state from its console. Waits for the address when
        Wi-Fi is set up, because joining the network takes a few seconds."""
        self.log("Reading the board's USB console...")
        info = core.wait_for_board(self.port, lambda i: bool(i.version) and (i.wifi is False or i.complete()),
                                   timeout=timeout, log=self.log)
        self.info = info
        if not info.version:
            raise SetupError(f"The board did not answer on {self.port}.")
        self.log(f"Firmware {info.version}; " + ("claimed" if info.provisioned else "waiting to be claimed") +
                 "; " + (f"on the network at {info.ip}." if info.ip else
                         "Wi-Fi not set up yet." if info.wifi is False else "no network address yet."))
        return info

    def needs_wifi(self) -> bool:
        i = self._info()
        return i.wifi is False or not i.ip

    # 3 ---------------------------------------------------------------------
    def wifi(self, ssid: str, password: str, timeout: float = 90.0) -> BoardInfo:
        core.set_wifi(self.port, ssid, password, self.log)
        self.log("Restarting the board to join the network...")
        core.reset_board(self.port)
        time.sleep(3)
        info = core.wait_for_board(self.port, lambda i: i.complete(), timeout=timeout, log=self.log)
        if not info.ip:
            raise SetupError(f"The board did not join {ssid}. Check the network name and password "
                             "(2.4 GHz networks only) and try again.")
        self.info = info
        self.log(f"The board is on the network at {info.ip}.")
        return info

    # 4 ---------------------------------------------------------------------
    def new_credentials(self, folder: str, password: str) -> Credentials:
        self.credentials = core.create_credentials(folder, password, log=self.log)
        return self.credentials

    def existing_credentials(self, folder: str, password: str = "") -> Credentials:
        self.credentials = core.load_credentials(folder, password)
        admin, ca = self.credentials.names()
        self.log(f"Opened {self.credentials.folder}: {admin}, issued by {ca}.")
        return self.credentials

    # 5 ---------------------------------------------------------------------
    def claim(self) -> None:
        creds = self._creds()
        # A fresh read: the enrollment code changes whenever the board restarts.
        info = core.query_board(self.port)
        if not info.complete():
            raise SetupError("The board isn't on the network yet, or didn't report its certificate "
                             "fingerprint. Go back to the Wi-Fi step.")
        self.info = info
        if info.provisioned:
            self.log("Skipping the claim: the board is already claimed.")
        else:
            self.log(f"Claiming the board at {info.ip}...")
            Board(info.ip, pin=info.fingerprint).claim(info.enrollment_code, creds.ca_cert.read_bytes())
            self.log("Claimed. The board restarts with management locked to your certificate.")
            time.sleep(5)
            self.info = core.wait_for_board(self.port, lambda i: i.provisioned and i.complete(),
                                            timeout=90, log=self.log)
            if not self.info.provisioned:
                raise SetupError("The board did not come back claimed. Run setup again.")
        self.board = Board(self.info.ip, pin=self.info.fingerprint, credentials=creds)
        try:
            self.board.wait_ready(timeout=60)
        except SetupError as exc:
            if "does not match the fingerprint" in str(exc):
                raise
            raise SetupError(f"{exc} If this board was claimed with a different management CA, go back "
                             "and open that CA's credentials folder instead.") from exc
        self.config = self.board.config()
        self.config["acme"] = self.board.acme()
        self.record = SetupRecord(hostname=self.config.get("hostname", ""), ip=self.info.ip,
                                  fingerprint=self.info.fingerprint, firmware=self.info.version or "",
                                  static_ip=not self.config.get("network", {}).get("dhcp", True))
        self.record.save(creds.folder)
        self.log("Connected to the management API with your certificate.")

    # 6 ---------------------------------------------------------------------
    def current_identity(self) -> tuple[str, str, str]:
        cfg = self.config or {}
        servers = list(cfg.get("ntp_servers") or []) + list(DEFAULT_NTP)
        return cfg.get("hostname") or DEFAULT_HOSTNAME, servers[0], servers[1]

    def identity(self, hostname: str, ntp0: str, ntp1: str) -> None:
        if not core.valid_hostname(hostname):
            raise SetupError("The hostname must be letters, digits and hyphens, for example rf-controller.")
        for server in (ntp0, ntp1):
            if not core.valid_server_name(server):
                raise SetupError(f"{server!r} isn't a valid NTP server name or address.")
        self._board().set_identity(hostname, ntp0, ntp1)
        rec = self._rec()
        rec.hostname = hostname
        rec.save(self._creds().folder)
        self.log(f"Saved hostname {hostname} and NTP servers {ntp0}, {ntp1}; they apply at the restart.")

    # 7 ---------------------------------------------------------------------
    def current_network(self) -> dict:
        """The address in use now, and the stored static settings, to prefill the questions."""
        device = self._board().status().get("device", {})
        net = (self.config or {}).get("network", {})
        return {
            "dhcp": net.get("dhcp", True),
            "address": device.get("ipv4") or self._board().host,
            "netmask": device.get("netmask") or net.get("netmask") or "255.255.255.0",
            "gateway": device.get("gateway") or net.get("gateway") or "",
            "dns": net.get("dns") or device.get("gateway") or "",
        }

    def network(self, static: bool, address: str = "", netmask: str = "", gateway: str = "",
                dns: str = "") -> None:
        board = self._board()
        if static:
            for name, value in (("address", address), ("netmask", netmask), ("gateway", gateway),
                                ("DNS server", dns)):
                if not core.valid_ipv4(value):
                    raise SetupError(f"The {name} must look like 192.168.1.10.")
        board.set_network(static, address, netmask, gateway, dns)
        self.log("Network settings saved. Restarting the board to apply the name, NTP servers and network...")
        board.reboot()
        time.sleep(5)
        expect = address if static else None
        info = core.wait_for_board(self.port, lambda i: i.complete() and (expect is None or i.ip == expect),
                                   timeout=120, log=self.log)
        if not info.ip:
            raise SetupError("The board did not come back on the network. Check the static settings "
                             "from the USB console's status command.")
        if expect and info.ip != expect:
            raise SetupError(f"The board came back on {info.ip}, not {expect}.")
        self.info = info
        board.host, board.pin = info.ip, info.fingerprint
        board.wait_ready()
        self.config = board.config()
        rec = self._rec()
        rec.ip, rec.static_ip, rec.fingerprint = info.ip, static, info.fingerprint
        rec.save(self._creds().folder)
        self.log(f"The board is back at {info.ip}" + (" (static)." if static else
                 ". Reserve this address on your router so it doesn't change."))

    # 8 ---------------------------------------------------------------------
    def current_fqdn(self) -> tuple[str, str]:
        """(certificate name, Cloudflare zone) stored on the board, to prefill."""
        cfg = self.config or {}
        fqdn = cfg.get("fqdn") or ""
        if fqdn.endswith(".local"):
            fqdn = ""
        zone = (cfg.get("acme") or {}).get("cloudflare_zone") or (fqdn.split(".", 1)[1] if "." in fqdn else "")
        return fqdn, zone

    def cloudflare(self, fqdn: str, token: str, zone_name: str, create_dns: bool) -> None:
        board = self._board()
        fqdn = fqdn.strip().rstrip(".").lower()
        zone_name = zone_name.strip().rstrip(".").lower()
        if not core.valid_fqdn(fqdn):
            raise SetupError("The certificate name must be a full DNS name, for example rf1.example.com.")
        if not (fqdn == zone_name or fqdn.endswith("." + zone_name)):
            raise SetupError(f"{fqdn} is not inside the zone {zone_name}.")
        zone_id = core.cloudflare_zone_id(token, zone_name)
        self.log(f"Found zone {zone_name}.")
        if create_dns:
            core.cloudflare_set_a_record(token, zone_id, fqdn, board.host, self.log)
        board.set_fqdn(fqdn)
        self.log("Saving the token on the board (it checks it with Cloudflare first)...")
        board.set_cloudflare(token, zone_name)
        rec = self._rec()
        rec.fqdn = fqdn
        rec.save(self._creds().folder)
        self.log("Cloudflare is set up on the board.")

    # 9 ---------------------------------------------------------------------
    def certificate(self, production: bool = True, timeout: float = 600.0) -> dict:
        """Ask the board for its certificate, wait for it, and switch to checking it."""
        board = self._board()
        rec = self._rec()
        if not rec.fqdn:
            raise SetupError("Set the certificate name and Cloudflare token first.")
        current = board.acme().get("certificate", {})
        if (production and current.get("present") and current.get("serving") and current.get("source") == "letsencrypt"
                and current.get("hostname") == rec.fqdn and (current.get("days_left") or 0) > 30):
            self.log(f"The board already has a certificate for {rec.fqdn}; keeping it.")
        else:
            board.issue_certificate(production)
            self.log("Requesting the certificate. This usually takes 1 to 3 minutes...")
            deadline = time.monotonic() + timeout
            last_stage = ""
            while True:
                if time.monotonic() > deadline:
                    raise SetupError(f"The certificate didn't arrive within {int(timeout)} s.")
                time.sleep(5)
                try:
                    req = board.acme().get("request", {})
                except (SetupError, OSError) as exc:
                    # HTTPS restarts on the new certificate, so the pinned fingerprint stops matching.
                    if "does not match" in str(exc) or isinstance(exc, OSError):
                        info = core.query_board(self.port)
                        if info.fingerprint and info.fingerprint != board.pin:
                            board.pin = info.fingerprint
                            self.log("The board switched HTTPS to the new certificate.")
                        continue
                    raise
                if req.get("stage") != last_stage:
                    last_stage = req.get("stage", "")
                    self.log(f"  {last_stage}")
                if req.get("state") == "failed":
                    raise SetupError("The board couldn't get the certificate: " + (req.get("error") or "unknown error"))
                if req.get("state") == "done":
                    break
        info = core.query_board(self.port)
        if info.fingerprint:
            board.pin = info.fingerprint
        cert = board.acme().get("certificate", {})
        if not cert.get("serving"):
            raise SetupError("The board stored the certificate but isn't serving it. Restart it and run setup again.")
        if production:
            # The way a browser would: by name, trusting only public roots.
            Board(board.host, credentials=self._creds(), tls_name=rec.fqdn).status()
            self.log(f"Checked: {rec.fqdn} serves a publicly trusted certificate.")
        rec.fingerprint = board.pin
        rec.certificate = production
        rec.save(self._creds().folder)
        self.log(f"Certificate for {cert.get('hostname')}, valid to {cert.get('valid_to', '?')}.")
        return cert

    # 10 --------------------------------------------------------------------
    def can_import(self) -> bool:
        c = self._creds()
        return os.name == "nt" and bool(c.password) and c.p12.is_file()

    def finish(self, import_windows: bool) -> list[str]:
        creds = self._creds()
        rec = self._rec()
        notes = []
        if import_windows:
            thumb = core.import_into_windows_store(creds, self.log)
            notes.append(f"Administrator certificate thumbprint: {thumb}")
        path = rec.save(creds.folder)
        notes += [
            f"Console: https://{rec.fqdn if rec.certificate else rec.ip}/",
            f"Certificate fingerprint: {rec.fingerprint}",
            f"Credentials: {creds.folder}",
            f"Board notes: {path.name}",
            "Keep that folder and its password safe; there is no remote recovery without them.",
            "The RF mute stays on until you release it; setup doesn't change it.",
        ]
        return notes

    def browser_steps(self, imported: bool) -> list[tuple[str, list[str]]]:
        """How to open the console in a browser with the administrator
        certificate, as (heading, numbered steps). Shown at the end of setup."""
        creds = self._creds()
        admin, ca = creds.names()
        url = f"https://{self._rec().fqdn if self._rec().certificate else self._rec().ip}/"
        open_steps = [
            f"Open {url}",
            f"When the browser asks you to choose a certificate, pick {admin} (issued by {ca}) and click OK.",
            "If you get a connection error instead of that question, close every window of the "
            "browser and open it again: browsers remember a 'no certificate' choice.",
        ]
        if not self._rec().certificate:
            open_steps.insert(1, "The browser warns that the connection isn't private, because the board uses its own "
                              f"certificate. Check its SHA-256 fingerprint starts {self._rec().fingerprint[:16]}, then continue.")
        p12_note = ("Enter the password you chose in setup." if creds.password else
                    "Enter the password that protects that .p12 file.")
        sections: list[tuple[str, list[str]]] = []
        if not creds.p12.is_file():
            return [("Make a .p12 for your browser first", [
                f"{creds.folder} has no admin-client.p12. Make one from admin-client.crt.pem and "
                "admin-client.key.pem with openssl pkcs12 -export, as in docs/API.md, then import it "
                "into your browser.",
            ] + open_steps)]
        if os.name == "nt":
            if imported:
                sections.append(("Edge or Chrome (the certificate is already installed)", open_steps))
            else:
                sections.append(("Install the certificate for Edge and Chrome", [
                    f"Double-click {creds.p12} to open the Certificate Import Wizard.",
                    "Choose Current User, then Next, then Next again.",
                    p12_note + " Leave the other options as they are. Next.",
                    "Choose 'Automatically select the certificate store', then Next and Finish.",
                ] + open_steps))
        else:
            sections.append(("Chrome or Chromium", [
                "Open Settings > Privacy and security > Security > Manage certificates.",
                f"Under Your certificates, click Import and choose {creds.p12}.",
                p12_note,
            ] + open_steps))
        sections.append(("Firefox (keeps its own certificates on every system)", [
            "Open Settings > Privacy & Security, scroll to Certificates and click View Certificates.",
            f"On Your Certificates, click Import and choose {creds.p12}.",
            p12_note,
        ] + open_steps))
        return sections

    # helpers -----------------------------------------------------------------
    def _info(self) -> BoardInfo:
        if not self.info:
            raise SetupError("Read the board first.")
        return self.info

    def _creds(self) -> Credentials:
        if not self.credentials:
            raise SetupError("Create or open credentials first.")
        return self.credentials

    def _board(self) -> Board:
        if not self.board:
            raise SetupError("Claim or connect to the board first.")
        return self.board

    def _rec(self) -> SetupRecord:
        if self.record is None:
            self.record = SetupRecord()
        return self.record
