"""Offline tests for the setup tool. No board, serial port or network needed.

    cd tools/setup && python -m unittest discover -s tests
"""

import json
import ssl
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from rf_setup import core, flow
from rf_setup.core import Board, BoardInfo, SetupError

FP = "AB" * 32

# What the board's USB console prints for "status" then "enrollment".
UNCLAIMED = (
    "status\r\nFirmware: bc438aa\r\nFilter: 1\r\nIPv4 address: 192.0.2.10\r\n"
    "Wi-Fi credentials: provisioned\r\nManagement: unclaimed; run enrollment\r\n"
    "TLS certificate and key: missing\r\nBootstrap certificate SHA-256: " + FP + "\r\nrf> "
    "enrollment\r\nOne-time management enrollment code: 0123456789ABCDEF0123456789ABCDEF\r\n"
    "Bootstrap certificate SHA-256: " + FP + "\r\nrf> ")
CLAIMED = (
    "Firmware: bc438aa\r\nIPv4 address: 192.0.2.11\r\nWi-Fi credentials: provisioned\r\n"
    "Management: claimed (mTLS)\r\nServer certificate SHA-256: " + "cd" * 32 + "\r\nrf> "
    "Already claimed. Management needs a client certificate from your management CA.\r\nrf> ")
NO_WIFI = (
    "Firmware: bc438aa\r\nIPv4 address: none\r\nWi-Fi credentials: missing\r\n"
    "Management: unclaimed; run enrollment\r\nrf> "
    "One-time management enrollment code: 0123456789ABCDEF0123456789ABCDEF\r\n"
    "Bootstrap certificate SHA-256: " + FP + "\r\nrf> ")


class ConsoleParsing(unittest.TestCase):
    def test_unclaimed_status(self):
        info = core.parse_console(UNCLAIMED)
        self.assertTrue(info.complete())
        self.assertEqual(info.enrollment_code, "0123456789ABCDEF0123456789ABCDEF")
        self.assertEqual(info.ip, "192.0.2.10")
        self.assertEqual(info.fingerprint, FP)
        self.assertEqual(info.version, "bc438aa")
        self.assertTrue(info.wifi)
        self.assertFalse(info.provisioned)

    def test_claimed_status_pins_server_certificate(self):
        info = core.parse_console(CLAIMED)
        self.assertTrue(info.provisioned)
        self.assertTrue(info.complete())
        self.assertEqual(info.fingerprint, "CD" * 32)
        self.assertIsNone(info.enrollment_code)

    def test_no_wifi(self):
        info = core.parse_console(NO_WIFI)
        self.assertFalse(info.wifi)
        self.assertIsNone(info.ip)
        self.assertFalse(info.complete())

    def test_boot_log_lines(self):
        info = BoardInfo()
        for line in ("\x1b[0;33mW (1235) ownership: One-time management enrollment code: "
                     "0123456789ABCDEF0123456789ABCDEF\x1b[0m",
                     "I (4000) network: DHCP IPv4 acquired: 192.0.2.12",
                     "W (5000) web: Bootstrap certificate SHA-256: " + "ef" * 32,
                     "I (300) app_init: App version:      bc438aa"):
            core.parse_console_line(line, info)
        self.assertTrue(info.complete())
        self.assertEqual((info.ip, info.fingerprint, info.version), ("192.0.2.12", "EF" * 32, "bc438aa"))


class FakeConsole:
    """Stands in for the board's USB console in set_wifi."""

    def __init__(self, reply):
        self.sent, self.reply = [], reply
        self.prompts = iter(["rf> ", "SSID: ", "Password (not echoed): ", reply])

    def __call__(self, port):
        return self

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        pass

    def wake(self):
        next(self.prompts)
        return True

    def send(self, text):
        self.sent.append(text)

    def read_until(self, pattern, timeout):
        return next(self.prompts)


class WiFi(unittest.TestCase):
    def _run(self, reply, ssid="home", password="correct horse"):
        fake = FakeConsole(reply)
        orig = core.Console
        core.Console = fake
        try:
            core.set_wifi("COM0", ssid, password)
        finally:
            core.Console = orig
        return fake.sent

    def test_answers_the_prompts(self):
        self.assertEqual(self._run("OK; takes effect at next reset\r\n"), ["set-wifi", "home", "correct horse"])

    def test_refusal_is_reported(self):
        with self.assertRaisesRegex(SetupError, "refused"):
            self._run("ERROR: value rejected\r\n")

    def test_bad_values_never_reach_the_board(self):
        for ssid, pw in (("", "correct horse"), ("x" * 33, "correct horse"), ("home", "short"),
                         ("home", "x" * 65), ("ho\x03me", "correct horse")):
            with self.assertRaises(SetupError):
                self._run("OK\r\n", ssid, pw)

    def test_open_network(self):
        self.assertEqual(self._run("OK; takes effect at next reset\r\n", password=""), ["set-wifi", "home", ""])


class Validation(unittest.TestCase):
    def test_names_and_addresses(self):
        self.assertTrue(core.valid_hostname("rf-controller"))
        self.assertFalse(core.valid_hostname("-bad"))
        self.assertFalse(core.valid_hostname("rf.controller"))
        self.assertTrue(core.valid_server_name("time.cloudflare.com"))
        self.assertTrue(core.valid_server_name("192.0.2.1"))
        self.assertFalse(core.valid_server_name("bad name"))
        self.assertTrue(core.valid_ipv4("192.0.2.10"))
        self.assertFalse(core.valid_ipv4("192.0.2.300"))


class CredentialFiles(unittest.TestCase):
    def test_create_load_and_refuse_overwrite(self):
        from cryptography import x509
        from cryptography.hazmat.primitives.serialization import pkcs12
        with tempfile.TemporaryDirectory() as d:
            creds = core.create_credentials(d, "correct horse")
            ca = x509.load_pem_x509_certificate(creds.ca_cert.read_bytes())
            admin = x509.load_pem_x509_certificate(creds.client_cert.read_bytes())
            self.assertTrue(ca.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)
            self.assertEqual(admin.issuer, ca.subject)
            key, cert, extra = pkcs12.load_key_and_certificates(creds.p12.read_bytes(), b"correct horse")
            self.assertEqual(cert, admin)
            for f in ("management-ca.key.pem", "admin-client.key.pem"):
                self.assertIn(b"ENCRYPTED", (Path(d) / f).read_bytes())
            self.assertEqual(creds.names(), (core.ADMIN_NAME, core.CA_NAME))
            core.load_credentials(d, "correct horse")
            for bad in ("wrong password", ""):
                with self.assertRaises(SetupError):
                    core.load_credentials(d, bad)
            with self.assertRaises(SetupError):
                core.create_credentials(d, "correct horse")

    def test_unencrypted_folder_needs_no_password(self):
        # Folders made by hand, as in docs/API.md, keep the key unencrypted.
        from cryptography.hazmat.primitives import serialization
        with tempfile.TemporaryDirectory() as d:
            creds = core.create_credentials(Path(d) / "src", "correct horse")
            plain = Path(d) / "plain"
            plain.mkdir()
            for f in ("management-ca.crt.pem", "admin-client.crt.pem"):
                (plain / f).write_bytes((creds.folder / f).read_bytes())
            key = serialization.load_pem_private_key(creds.client_key.read_bytes(), b"correct horse")
            (plain / "admin-client.key.pem").write_bytes(key.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            self.assertIsNone(core.load_credentials(plain).password)

    def test_short_password_refused(self):
        with tempfile.TemporaryDirectory() as d, self.assertRaises(SetupError):
            core.create_credentials(d, "short")


def _self_signed(directory: Path):
    """A server certificate standing in for the board's bootstrap certificate."""
    import datetime as dt
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "rf-controller-bootstrap")])
    now = dt.datetime.now(dt.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(1)
            .not_valid_before(now - dt.timedelta(days=1)).not_valid_after(now + dt.timedelta(days=1))
            .sign(key, hashes.SHA256()))
    (directory / "srv.crt").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (directory / "srv.key").write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                                          serialization.PrivateFormat.PKCS8,
                                                          serialization.NoEncryption()))
    return core._fingerprint(cert.public_bytes(serialization.Encoding.DER))


class FakeBoard:
    """A local HTTPS server that behaves like the board's claim and v1 API."""

    def __init__(self, directory: Path, ca_file=None):
        self.received = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def _reply(self, status, body=b"{}"):
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _record(self, method):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                outer.received.append((method, self.path, dict(self.headers), body))
                return body

            def do_PUT(self):
                self._record("PUT")
                if self.path == "/api/v1/provisioning/claim":
                    ok = self.headers.get("X-Provisioning-Code") == "0" * 32
                    return self._reply(202 if ok else 403, b"{}" if ok else b"bad code")
                self._reply(200)

            def do_POST(self):
                self._record("POST")
                if self.path == "/api/v1/system/reboot":
                    ok = self.headers.get("X-Confirm-Reboot") == "REBOOT"
                    return self._reply(202 if ok else 412)
                self._reply(202 if self.path == "/api/v1/acme/issue" else 404)

            def do_GET(self):
                self._record("GET")
                if self.path == "/api/v1/status":
                    peer = self.connection.getpeercert()
                    return self._reply(200, json.dumps({"device": {"ipv4": "127.0.0.1",
                                                                    "client": bool(peer)}}).encode())
                self._reply(404, b"not found")

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(directory / "srv.crt", directory / "srv.key")
        if ca_file:
            ctx.verify_mode = ssl.CERT_REQUIRED
            ctx.load_verify_locations(str(ca_file))
        self.httpd = HTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


class PinnedConnections(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fingerprint = _self_signed(self.dir)

    def tearDown(self):
        self.tmp.cleanup()

    def _board(self, server, **kw):
        orig = core._Connection.__init__

        def init(conn, host, port, **k):
            orig(conn, host, server.port, **k)
        core._Connection.__init__ = init
        self.addCleanup(setattr, core._Connection, "__init__", orig)
        return Board("127.0.0.1", **kw)

    def _server(self, **kw):
        server = FakeBoard(self.dir, **kw)
        self.addCleanup(server.close)
        return server

    def test_claim_with_matching_pin(self):
        server = self._server()
        self._board(server, pin=self.fingerprint).claim("0" * 32, b"-----BEGIN CERTIFICATE-----")
        method, path, headers, body = server.received[-1]
        self.assertEqual(path, "/api/v1/provisioning/claim")
        self.assertEqual(headers["Content-Type"], "application/x-pem-file")
        self.assertEqual(body, b"-----BEGIN CERTIFICATE-----")

    def test_wrong_pin_sends_nothing(self):
        server = self._server()
        with self.assertRaisesRegex(SetupError, "does not match"):
            self._board(server, pin="00" * 32).claim("0" * 32, b"secret")
        self.assertEqual(server.received, [])

    def test_bad_code_message(self):
        server = self._server()
        with self.assertRaisesRegex(SetupError, "rejected the enrollment code"):
            self._board(server, pin=self.fingerprint).claim("1" * 32, b"x")

    def test_mutual_tls_after_claim(self):
        creds = core.create_credentials(self.dir / "creds", "correct horse")
        server = self._server(ca_file=creds.ca_cert)
        status = self._board(server, pin=self.fingerprint, credentials=creds).status()
        self.assertTrue(status["device"]["client"])
        with self.assertRaises((ssl.SSLError, OSError, SetupError)):
            self._board(server, pin=self.fingerprint).status()

    def test_settings_payloads(self):
        server = self._server()
        board = self._board(server, pin=self.fingerprint)
        board.set_identity("rf1", "ntp1.example.com", "192.0.2.1")
        board.set_network(False)
        board.set_network(True, "192.0.2.20", "255.255.255.0", "192.0.2.1", "192.0.2.1")
        board.reboot()
        sent = [(m, p, json.loads(b) if b else None) for m, p, h, b in server.received]
        self.assertEqual(sent, [
            ("PUT", "/api/v1/config/hostname", {"hostname": "rf1"}),
            ("PUT", "/api/v1/config/ntp", {"servers": ["ntp1.example.com", "192.0.2.1"]}),
            ("PUT", "/api/v1/config/network", {"dhcp": True}),
            ("PUT", "/api/v1/config/network", {"dhcp": False, "address": "192.0.2.20",
                                               "netmask": "255.255.255.0", "gateway": "192.0.2.1",
                                               "dns": "192.0.2.1"}),
            ("POST", "/api/v1/system/reboot", None),
        ])
        self.assertEqual(server.received[-1][2]["X-Confirm-Reboot"], "REBOOT")


class CertificateCalls(unittest.TestCase):
    setUp, tearDown = PinnedConnections.setUp, PinnedConnections.tearDown
    _board, _server = PinnedConnections._board, PinnedConnections._server

    def test_acme_payloads(self):
        server = self._server()
        board = self._board(server, pin=self.fingerprint)
        board.set_fqdn("rf1.example.com")
        board.set_cloudflare("tok", "example.com")
        board.issue_certificate(False)
        sent = [(m, p, json.loads(b) if b else None) for m, p, h, b in server.received]
        self.assertEqual(sent, [
            ("PUT", "/api/v1/config/hostname", {"fqdn": "rf1.example.com"}),
            ("PUT", "/api/v1/config/cloudflare", {"api_token": "tok", "zone_name": "example.com"}),
            ("POST", "/api/v1/acme/issue", {"production": False}),
        ])

    def test_name_checked_when_not_pinning(self):
        # Without a pin the certificate must chain to a public root and match the name,
        # so a self-signed one is refused.
        server = self._server()
        with self.assertRaises(ssl.SSLError):
            self._board(server, tls_name="rf1.example.com").status()

    def test_board_needs_pin_or_name(self):
        with self.assertRaises(ValueError):
            Board("192.0.2.1")


class CloudflareStep(unittest.TestCase):
    def test_name_must_be_in_zone(self):
        with tempfile.TemporaryDirectory() as d:
            s = flow.Session(log=lambda m: None)
            s.new_credentials(d, "correct horse")
            s.board = Board("192.0.2.10", pin=FP)
            with self.assertRaisesRegex(SetupError, "not inside the zone"):
                s.cloudflare("rf1.example.com", "tok", "example.org", False)
            with self.assertRaisesRegex(SetupError, "full DNS name"):
                s.cloudflare("rf1", "tok", "example.com", False)

    def test_fqdn_validation(self):
        self.assertTrue(core.valid_fqdn("rf1.example.com"))
        self.assertFalse(core.valid_fqdn("rf1"))
        self.assertFalse(core.valid_fqdn("RF1.example.com"))


class Firmware(unittest.TestCase):
    ARGS = {
        "write_flash_args": ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"],
        "flash_files": {"0x50000": "rf_filter_controller.bin", "0x0": "bootloader/bootloader.bin",
                        "0x8000": "partition_table/partition-table.bin", "0x4a000": "ota_data_initial.bin"},
        "extra_esptool_args": {"chip": "esp32s3"},
    }

    def _folder(self, root: Path, nested: bool, args=None):
        root.mkdir(parents=True, exist_ok=True)
        (root / "flasher_args.json").write_text(json.dumps(args or self.ARGS))
        for rel in self.ARGS["flash_files"].values():
            p = root / (rel if nested else Path(rel).name)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"\0")

    def test_build_and_release_layouts(self):
        for nested in (True, False):
            with tempfile.TemporaryDirectory() as d:
                with self.assertRaises(SetupError):
                    core.find_firmware(d)
                self._folder(Path(d), nested)
                folder = core.find_firmware(d)
                args = core.flash_arguments(folder)
                self.assertEqual(args[:6], ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"])
                self.assertEqual(args[6::2], ["0x0", "0x8000", "0x4a000", "0x50000"])
                self.assertTrue(all(Path(p).is_file() for p in args[7::2]))

    def test_other_chip_refused(self):
        with tempfile.TemporaryDirectory() as d:
            self._folder(Path(d), True, dict(self.ARGS, extra_esptool_args={"chip": "esp32p4"}))
            with self.assertRaisesRegex(SetupError, "esp32p4"):
                core.flash_arguments(Path(d))

    def test_encrypted_board_refused_before_writing(self):
        with tempfile.TemporaryDirectory() as d:
            self._folder(Path(d), True)
            calls = []
            orig = core._esptool

            def fake(args, log):
                calls.append(args)
                return "Flash Encryption: Enabled\n"
            core._esptool = fake
            try:
                with self.assertRaisesRegex(SetupError, "Nothing was written"):
                    core.flash_board("COM0", Path(d))
            finally:
                core._esptool = orig
            self.assertFalse(any("write-flash" in a for a in calls))

    def test_security_info_parsing(self):
        self.assertFalse(core.parse_flash_encryption("Flash Encryption: Disabled\n"))
        self.assertTrue(core.parse_flash_encryption("Flash Encryption: Enabled\n"))
        with self.assertRaises(SetupError):
            core.parse_flash_encryption("Chip ID: 9")


class Finish(unittest.TestCase):
    def test_records_and_browser_steps(self):
        with tempfile.TemporaryDirectory() as d:
            s = flow.Session(log=lambda m: None)
            s.new_credentials(d, "correct horse")
            s.record = core.SetupRecord(hostname="rf1", ip="192.0.2.10", fingerprint=FP)
            notes = s.finish(import_windows=False)
            self.assertTrue((Path(d) / "rf-controller-abababababab.json").is_file())
            self.assertTrue(any("192.0.2.10" in n for n in notes))
            for imported in (True, False):
                text = " ".join(" ".join(st) for _, st in s.browser_steps(imported))
                self.assertIn("https://192.0.2.10/", text)
                self.assertIn(core.ADMIN_NAME, text)
                self.assertIn(FP[:16], text)

    def test_network_rejects_bad_addresses(self):
        s = flow.Session(log=lambda m: None)
        s.board = Board("192.0.2.10", pin=FP)
        for bad in ("1.1.1.1, 8.8.8.8", "dns.example.com", ""):
            with self.assertRaisesRegex(SetupError, "DNS server"):
                s.network(True, "192.0.2.10", "255.255.255.0", "192.0.2.1", bad)


if __name__ == "__main__":
    unittest.main()
