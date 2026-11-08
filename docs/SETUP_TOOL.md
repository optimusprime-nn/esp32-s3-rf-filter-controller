# Setup tool

The setup tool takes a controller from new to managed: firmware, Wi-Fi, ownership, name, NTP servers, network address and a Let's Encrypt certificate. It runs on Windows and Linux, as a window or as questions in a terminal. Both run the same steps in the same order. It works like esp32p4_nts_time_server's setup tool, and it can use that tool's credentials, so one management CA can manage both.

It lives in [`tools/setup`](../tools/setup). Each release zip holds it next to the firmware.

## Start it

You need Python 3.10 or newer. The first run makes a virtual environment in the tool's folder and installs `cryptography`, `pyserial` and `esptool` into it, which takes about a minute.

| | Window | Terminal |
|---|---|---|
| Windows | double-click `setup\setup.cmd` | `setup\setup.cmd --cli` |
| Linux | `setup/setup.sh` | `setup/setup.sh --cli` |

On Linux the tool uses the terminal by itself when there's no desktop, or when Tkinter isn't installed (`sudo apt install python3-tk`). Reading the serial port needs membership of the `dialout` group.

| Option | Effect |
|---|---|
| `--cli` | Ask the questions in the terminal instead of the window |
| `--firmware <folder>` | Firmware to write. It must hold `flasher_args.json` and the images it lists, as the release's `firmware` folder and an ESP-IDF `build` folder do. The default is the `firmware` folder next to the tool |

## What it does

Connect the board by its **native USB** port (USB Serial/JTAG). The tool talks to the board's `rf>` console on that port, which isn't on a USB-UART adapter.

| Step | What happens | Skipped when |
|---|---|---|
| 1. Connect | You choose the serial port. The native USB port is listed first | |
| 2. Firmware | Optionally writes the bootloader, partition table and firmware with esptool, then reads the board's state with the console's `status` and `enrollment` commands | Writing is optional. It keeps settings, Wi-Fi and ownership, and is refused on a board with flash encryption on |
| 3. Wi-Fi | Sends the network name and password with the console's `set-wifi`, restarts the board and waits for its address | The board is already on the network and you keep it there |
| 4. Certificate | Creates a management CA and an administrator certificate, protected by a password you choose. Or opens a folder you already have, such as the time server's | |
| 5. Claim | Sends the CA certificate with the enrollment code. The board restarts, and from then on accepts management only with a certificate from that CA | The board is already claimed |
| 6. Name | Sets the hostname and the two NTP servers | |
| 7. Network | Keeps DHCP or sets a static address, then restarts the board to apply this step and the last one | |
| 8. DNS | Sets the board's full DNS name, finds the Cloudflare zone with your API token, optionally creates the DNS `A` record, and stores the token on the board, which checks it from its own network | You don't want a Let's Encrypt certificate |
| 9. Let's Encrypt | The board requests its certificate (optionally a staging one first) and switches HTTPS to it. The tool then checks it the way a browser would | As above, or the board already has a valid one for the same name |
| 10. Finish | Optionally imports the administrator certificate into Windows, saves notes, and shows how to open the dashboard | |

The tool never touches the RF mute, the filter selection or the SDR board.

## How the board is protected during setup

- The board prints the SHA-256 fingerprint of the certificate it serves on its USB console. The tool reads it over USB and checks every HTTPS connection against it. If it doesn't match, the tool stops before sending anything.
- Only the CA **certificate** leaves your computer. Private keys stay in the credentials folder, encrypted with your password.
- The Wi-Fi password goes to the board over USB only. It isn't logged or saved on the computer, and the board doesn't echo it.
- Five wrong enrollment codes lock enrollment until the board restarts. The tool reads a fresh code right before it claims, so this only happens if something else is guessing.

## The credentials folder

A new folder defaults to `Documents/management-credentials-<name>`. The tool never overwrites one that already holds credentials.

| File | What it is |
|---|---|
| `management-ca.crt.pem` | The CA certificate. A copy is on every board claimed with it |
| `management-ca.key.pem` | The CA private key, encrypted. Needed to make more administrator certificates |
| `admin-client.crt.pem`, `admin-client.key.pem` | Your administrator certificate and its encrypted key, for scripts |
| `admin-client.p12` | The same certificate and key in one file, for a browser or Windows |
| `rf-controller-<fingerprint>.json` | Non-secret notes about one controller: hostname, address, fingerprint, firmware. One file per board, so several controllers and a time server can share a folder |

An existing folder only needs `management-ca.crt.pem`, `admin-client.crt.pem` and `admin-client.key.pem`. A key that isn't encrypted, as in the hand-made folder in [API.md](API.md#making-a-management-ca-by-hand), needs no password.

Keep the folder and its password safe. They are the only key to the board's management. There is no remote recovery if both the administrator certificate and the CA key are lost: the board then has to be erased over USB and claimed again.

## Running it again

Run it again on a board that is already set up to change its Wi-Fi network, name, NTP servers or address. Leave **Write the firmware** unticked. The tool sees the board is claimed, asks for the credentials folder it was claimed with, and goes on from the Name step.

## Tests

```sh
cd tools/setup
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
```

They need no board or network, and run in CI on Windows and Linux.
