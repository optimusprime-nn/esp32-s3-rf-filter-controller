"""Guided command-line setup. Works the same on Windows and Linux.

Every question shows a sensible default in [brackets]; press Enter to accept
it. Nothing that writes to the board happens without a plain yes.
"""

from __future__ import annotations

import getpass
import os
import sys

from . import core, flow
from .core import SetupError

BOLD = "\033[1m" if sys.stdout.isatty() and os.name != "nt" else ""
RESET = "\033[0m" if BOLD else ""
TOTAL = 8


def say(text: str = "") -> None:
    print(text, flush=True)


def heading(step: int, title: str) -> None:
    say()
    say(f"{BOLD}Step {step} of {TOTAL}: {title}{RESET}")
    say("-" * (len(title) + 14))


def ask(question: str, default: str = "", check=None, error: str = "") -> str:
    while True:
        suffix = f" [{default}]" if default else ""
        answer = input(f"{question}{suffix}: ").strip() or default
        if not answer:
            say("  Please enter a value.")
            continue
        if check and not check(answer):
            say(f"  {error or 'That does not look right; try again.'}")
            continue
        return answer


def yes(question: str, default: bool = True) -> bool:
    hint = "Y/n" if default else "y/N"
    while True:
        answer = input(f"{question} ({hint}): ").strip().lower()
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False


def secret(question: str, confirm: bool = False, allow_empty: bool = False) -> str:
    while True:
        first = getpass.getpass(f"{question}: ")
        if not first and not allow_empty:
            say("  Please enter a value.")
            continue
        if not confirm:
            return first
        if len(first) < 8:
            say("  Use at least 8 characters.")
            continue
        if getpass.getpass("Type it again: ") == first:
            return first
        say("  Those did not match; try again.")


def run(firmware_dir: str | None = None) -> int:
    s = flow.Session(log=lambda m: say("  " + m))
    say("RF filter controller setup")
    say("==========================")
    say("This sets up one controller: firmware, Wi-Fi, ownership, name and network.")
    say("Have ready: the board on USB (its native USB port), and your Wi-Fi name and password.")

    try:
        # 1 -- serial port --------------------------------------------------------
        heading(1, "Connect the board")
        ports = core.list_serial_ports()
        if not ports:
            raise SetupError("No serial ports found. Connect the board's USB cable and run this again.")
        for i, (dev, desc) in enumerate(ports, 1):
            say(f"  {i}. {dev}  {desc}")
        say("Choose the board's native USB port (USB Serial/JTAG), not a USB-UART adapter.")
        pick = ask("Which port is the board on? Enter the number", "1",
                   lambda a: a.isdigit() and 1 <= int(a) <= len(ports), "Enter one of the numbers above.")
        s.port = ports[int(pick) - 1][0]

        # 2 -- firmware -------------------------------------------------------------
        heading(2, "Firmware")
        say("A new board needs the firmware written once over USB. A board that already")
        say("runs it does not; later updates go over the network. Writing keeps the board's")
        say("settings, Wi-Fi and ownership.")
        if yes("Write the firmware to this board now?", default=False):
            if not yes(f"Write the firmware over {s.port}?", default=True):
                raise SetupError("Stopped before flashing; nothing was changed.")
            s.flash(firmware_dir)
        s.read_board()

        # 3 -- Wi-Fi ----------------------------------------------------------------
        heading(3, "Wi-Fi")
        if s.needs_wifi():
            say("The board needs your Wi-Fi network (2.4 GHz). The password is sent to the board")
            say("over USB and isn't shown or saved anywhere else.")
            change = True
        else:
            say(f"The board is on Wi-Fi at {s.info.ip}.")
            change = yes("Change its Wi-Fi network?", default=False)
        if change:
            ssid = ask("Network name (SSID)")
            password = secret("Wi-Fi password (input is hidden; Enter for an open network)", allow_empty=True)
            s.wifi(ssid, password)
            password = ""

        # 4 -- credentials ----------------------------------------------------------
        heading(4, "Your administrator certificate")
        say("Management needs a client certificate from your management CA. Setup can make a")
        say("new CA, or use one you already have, such as the time server's credentials folder.")
        existing = s.info.provisioned or yes("Use an existing credentials folder?", default=False)
        if existing:
            if s.info.provisioned:
                say("This board is already claimed. Open the folder of the CA it was claimed with.")
            folder = ask("Credentials folder", str(core.default_credentials_folder("rf-controller")))
            password = ""
            try:
                s.existing_credentials(folder)
            except SetupError as exc:
                if "password" not in str(exc).lower():
                    raise
                password = secret("Password for that folder")
                s.existing_credentials(folder, password)
        else:
            say("Keep the folder and password safe: if both are lost, the board's management")
            say("cannot be recovered remotely.")
            name = ask("Short name for this board (used for the folder)", "rf-controller", core.valid_hostname,
                       "Use letters, digits and hyphens.")
            folder = ask("Save the credentials in", str(core.default_credentials_folder(name)))
            s.new_credentials(folder, secret("Choose a password (at least 8 characters)", confirm=True))

        # 5 -- claim --------------------------------------------------------------
        heading(5, "Claim the board")
        if not s.info.provisioned:
            say(f"The board at {s.info.ip} will accept management only from your certificate.")
            say(f"Its identity is checked against the fingerprint on its console ({s.info.fingerprint[:16]}...).")
            if not yes("Claim it now?", default=True):
                raise SetupError("Stopped before claiming; nothing was sent.")
        s.claim()

        # 6 -- identity -----------------------------------------------------------
        heading(6, "Name and time")
        host0, ntp0, ntp1 = s.current_identity()
        hostname = ask("Hostname", host0, core.valid_hostname, "Use letters, digits and hyphens.")
        ntp0 = ask("NTP server 1", ntp0, core.valid_server_name, "Enter a server name or address.")
        ntp1 = ask("NTP server 2", ntp1, core.valid_server_name, "Enter a server name or address.")
        s.identity(hostname, ntp0, ntp1)

        # 7 -- network ------------------------------------------------------------
        heading(7, "Network address")
        net = s.current_network()
        say(f"The board is at {net['address']}" + (" from DHCP." if net["dhcp"] else " (static)."))
        say("Either reserve that address on your router (keep DHCP), or give the board a static one.")
        say("The board restarts at the end of this step to apply its name, NTP servers and network.")
        if yes("Set a static address on the board?", default=not net["dhcp"]):
            address = ask("Static address", net["address"], core.valid_ipv4, "Enter an address like 192.168.1.10.")
            netmask = ask("Netmask", net["netmask"], core.valid_ipv4, "Enter a mask like 255.255.255.0.")
            gateway = ask("Gateway", net["gateway"], core.valid_ipv4, "Enter an address like 192.168.1.1.")
            dns = ask("DNS server", net["dns"] or gateway, core.valid_ipv4, "Enter one address, not a name.")
            say("If these are wrong, the board will be unreachable over the network until the")
            say("setting is changed again; the USB console's status command shows its address.")
            if not yes(f"Apply {address} / {netmask} via {gateway} and restart the board?", default=True):
                raise SetupError("Stopped before changing the network. Run setup again to finish.")
            s.network(True, address, netmask, gateway, dns)
        else:
            s.network(False)

        # 8 -- finish ---------------------------------------------------------------
        heading(8, "Finish")
        do_import = s.can_import() and yes(
            "Install the administrator certificate in Windows so your browser can open the console?",
            default=True)
        say()
        for line in s.finish(do_import):
            say("  " + line)
        say()
        say(f"{BOLD}Open the console in your browser{RESET}")
        for title, steps in s.browser_steps(do_import):
            say()
            say(f"  {title}:")
            for i, step in enumerate(steps, 1):
                say(f"    {i}. {step}")
        say()
        say("Done.")
        return 0
    except SetupError as exc:
        say()
        say(f"Stopped: {exc}")
        say("You can run setup again; it picks up from a board that is already on Wi-Fi or claimed.")
        return 1
    except KeyboardInterrupt:
        say()
        say("Cancelled.")
        return 130
