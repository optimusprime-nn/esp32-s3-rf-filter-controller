# First flash and bench test

Run this with no RF connected. It checks the firmware and the A/B control lines only.

## What you need
- ESP32-S3 N16R8 board. Its native USB port and its UART port are separate connectors on most dev boards; connect both if you have both.
- ESP-IDF v6.1 and a multimeter or logic probe.

The UART port (often a CH343 or CP210x adapter) carries flashing and logs. The provisioning console is on the **native USB** port only, which appears as its own COM port.

## 1. Build and flash
From an activated ESP-IDF shell. The full erase is required on any board with older firmware, and it wipes everything in NVS.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 erase-flash flash monitor
```

Replace `COM7` with your board's UART port.

**Pass:** the log shows 16 MB flash and 8 MB PSRAM found, `Selected filter 1`, and `USB provisioning console ready`, with no boot loop. Without Wi-Fi credentials it also logs that the network is disabled, which is expected.

## 2. Open the console
Connect to the native USB port. Any serial terminal works; the IDF Python environment includes one:

```sh
python -m serial.tools.miniterm COM8 115200
```

Press Enter for the `rf>` prompt, then run `status`.

**Pass:** it shows the firmware version and filter 1, with Wi-Fi, device password and TLS all `missing`.

## 3. Check filter switching
Measure GPIO4 (A) and GPIO5 (B) against ground. High is about 3.3 V.

| Command | GPIO4 (A) | GPIO5 (B) |
|---|---|---|
| `filter 1` | low | low |
| `filter 2` | high | low |
| `filter 3` | low | high |
| `filter 4` | high | high |

Then run `filter 3`, press reset, and run `filter`. **Pass:** it still shows 3 and the pins match the table.

## 4. Provision
```text
set-wifi
set-password
```

The device password must be 12 to 64 characters. The Wi-Fi network must be WPA2 on 2.4 GHz. Press reset, then run `status` and note the IPv4 address.

Create a certificate for that address on your PC, replacing the IP:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout rf-key.pem -out rf-cert.pem -days 825 -subj "/CN=rf-controller" -addext "subjectAltName=IP:192.168.1.50"
```

Run `set-tls`, paste `rf-cert.pem`, then paste `rf-key.pem` when asked. Delete `rf-key.pem` from the PC afterwards if you don't need it, then press reset.

**Pass:** `status` shows everything provisioned, and the log has no `HTTPS management disabled` line.

## 5. Test HTTPS
Open `https://<device-ip>/` and accept the self-signed certificate warning. Then check:

| Action | Expected |
|---|---|
| Press a filter button before logging in | `Failed: 401`, pins don't move |
| Log in with a wrong password | `Failed: 401` |
| Fail six logins in a row, then retry at once | `Failed: 429` |
| Log in correctly after waiting, press Filter 2 | `OK`, status shows filter 2, pins match the table |
| Log out, press a filter button | `Failed: 401` |

## Report back
Send me anything that failed, with the serial log. Also note how long a login takes, since that's the first timing of the 20,000-iteration password hash. If `filter` prompts and log lines get mixed up on the USB console, tell me that too.
