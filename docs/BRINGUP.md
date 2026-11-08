# First flash and bench test

Run this with no RF connected. It checks the firmware and the A/B control lines only.

## What you need
- ESP32-S3 N16R8 board, connected by its **native USB** port (GPIO19/20, often labelled `USB` rather than `UART` or `COM`).
- ESP-IDF v6.1 and a multimeter or logic probe.

The native USB port is all you need: it flashes the board, carries the boot log and runs the `rf>` provisioning console. It appears as a USB Serial/JTAG COM port (`USB JTAG/serial debug unit` in Device Manager).

If your board also has a UART port (often a CH343 or CP210x adapter), you can leave it unplugged. The log goes to both ports, but the console is on native USB only.

## 1. Build and flash
From an activated ESP-IDF shell. Keep the full erase even on a new board: most come with a factory test program, and the erase removes it and anything it left in NVS. On a board that has run this firmware before, the erase also wipes its claim, Wi-Fi credentials and settings.

A new board needs no other preparation; the ESP32-S3's ROM bootloader can be flashed over native USB as it ships. If the factory program keeps the port busy or reboots too fast to connect, use the BOOT and RESET buttons as below.

The first boot takes longer than later ones, because the board generates its bootstrap key and certificate. That only happens once.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <usb-port> erase-flash flash monitor
```

Replace `<usb-port>` with the board's native USB port. Check it first: on a computer with other boards attached, the wrong port erases the wrong board. Unplug the board, list the ports, plug it in and list them again; the new one is yours.

If flashing can't connect, hold **BOOT**, press and release **RESET**, release **BOOT**, and run the command again. After flashing, press **RESET** if the board stays in download mode. The port disappears briefly at every reset, and the monitor reconnects by itself, so the first lines of a boot can be missing; `enrollment` and `status` repeat what you need.

**Pass:** the log shows 16 MB flash and 8 MB PSRAM found, `Selected filter 1`, and `USB provisioning console ready`, with no boot loop. Without Wi-Fi credentials it also logs that the network is disabled, which is expected.

## 2. Open the console
The monitor from step 1 is already the console: press Enter for the `rf>` prompt, then run `status`. Leave the monitor with Ctrl-].

Later, without rebuilding, any serial terminal works on the same port. The IDF Python environment includes one:

```sh
python -m serial.tools.miniterm <usb-port> 115200
```

Log lines can appear in the middle of what you type; the command still works.

**Pass:** it shows the firmware version, filter 1, Wi-Fi and TLS `missing`, and `Management: unclaimed`.

## 3. Check filter switching
Measure GPIO6 (A) and GPIO7 (B) against ground. High is about 3.3 V. GPIO8 (RF mute) stays high throughout.

| Command | GPIO6 (A) | GPIO7 (B) |
|---|---|---|
| `filter 1` | low | low |
| `filter 2` | high | low |
| `filter 3` | low | high |
| `filter 4` | high | high |

Then run `filter 3`, press reset, and run `filter`. **Pass:** it still shows 3 and the pins match the table.

## 4. Connect to Wi-Fi
Run `set-wifi`. The network must be WPA2 on 2.4 GHz. Press reset, then run `status` and note the IPv4 address. The serial log also shows `DHCP IPv4 acquired: <address>`.

## 5. Claim the controller
The commands in this step and the next are for bash (Linux, macOS, Git Bash or WSL). In Windows PowerShell, use the PowerShell versions given with them: PowerShell has no `<` redirect, and its `curl` is a different command, so call `curl.exe`.

You need a management CA and an admin client certificate. Use the files the esp32p4_nts_time_server setup tool made, if you already have them, or make them as in [API.md](API.md#making-a-management-ca-by-hand).

1. Run `enrollment` on the USB console and note the code and the bootstrap certificate fingerprint.
2. Check the controller is serving that certificate. The fingerprint must match, ignoring colons and case:

   ```sh
   openssl s_client -connect <device-ip>:443 </dev/null 2>/dev/null | openssl x509 -noout -fingerprint -sha256
   ```

   PowerShell:

   ```powershell
   '' | openssl s_client -connect <device-ip>:443 2>$null | openssl x509 -noout -fingerprint -sha256
   ```

3. Open `https://<device-ip>/`. **Pass:** the enrollment information page. Nothing else answers: `curl -sk https://<device-ip>/api/v1/status` gives `404`.
4. Claim it, with the code from step 1:

   ```sh
   curl -sk -X PUT -H 'Content-Type: application/x-pem-file' -H 'X-Provisioning-Code: <code>' --data-binary @management-ca.crt.pem https://<device-ip>/api/v1/provisioning/claim
   ```

   PowerShell (keep the quotes around `@management-ca.crt.pem`, or PowerShell reads the `@` itself):

   ```powershell
   curl.exe -sk -X PUT -H 'Content-Type: application/x-pem-file' -H 'X-Provisioning-Code: <code>' --data-binary '@management-ca.crt.pem' https://<device-ip>/api/v1/provisioning/claim
   ```

**Pass:** `{"claimed":true,...}`, the controller reboots by itself, and the log shows `Management ownership is provisioned` and `mTLS management console active`.

Also check the lockout once, before claiming: five wrong codes give `429 Enrollment locked until reboot`, and a reset prints a new code.

## 6. Test HTTPS
| Action | Expected |
|---|---|
| `curl -sk https://<device-ip>/api/v1/status` with no client certificate | Connection refused during the TLS handshake |
| The same with `--cert admin-client.crt.pem --key admin-client.key.pem` | Status JSON |
| A client certificate from a different CA | Refused during the handshake |
| Import `admin-client.p12` into your browser, open `https://<device-ip>/`, accept the certificate warning, choose the certificate | The dashboard |
| Press Filter 2 | Filter 2 highlighted, pins match the table |
| Watch the dashboard for 10 seconds | Uptime, fan RPM and temperatures update every second |
| `curl` with the client certificate and `-X POST -H 'X-Confirm-Reboot: REBOOT' .../api/v1/system/reboot` | `202`, then the controller reboots and its bootstrap fingerprint is unchanged |

In PowerShell, write `curl.exe` and quote `@file` arguments, as in step 5. Windows' own `curl.exe` can't load PEM client certificates (it fails with `0x80092002`), so give it the `.p12` instead: `--cert-type P12 --cert admin-client.p12 --pass <p12-password>`. The `curl` in Git Bash or WSL takes the PEM files as shown. JSON bodies need their inner quotes escaped for Windows PowerShell 5.1, for example `-d '{\"filter\":2}'`.

[API.md](API.md) has `curl` examples for every call.

## 7. Check the fan and temperature sensors
Wire them as in [HARDWARE.md](HARDWARE.md#fan). With no fan connected, `status` shows `Fan: 100%, 0 RPM` and the log warns once that the fan reports 0 RPM.

| Check | Expected |
|---|---|
| Measure GPIO1, then the level shifter's 5 V output | GPIO1 close to 3.3 V and the output close to 5 V at `fan 100`; both close to 0 V at `fan 0` |
| `fan 30`, wait 3 seconds, then `fan` | The RPM falls and settles |
| Press reset and watch the fan | Full speed until the firmware starts, then the saved speed |
| `temps` with an LM75-family sensor at `0x48` | `internal` and `i2c-0x48` both show plausible readings |
| Unplug the I2C sensor, then `temps` | `i2c-0x48: not responding`; reconnecting restores readings |

## 8. Check the weather station receiver
Wire the CC1101 as in [HARDWARE.md](HARDWARE.md#weather-station-receiver). Without one, the log shows `No CC1101 found` and `weather` shows `receiver missing`, which is expected.

| Check | Expected |
|---|---|
| Boot log | `CC1101 version 0x14 listening on 915.000 MHz` (the version may be `0x04`) |
| Put the station's outdoor sensor within a few metres, fit its batteries, wait 30 seconds, then `weather` | The station's ID under `Heard station`, and a reading that matches the station's own display |
| `weather` again a minute later | `readings` has gone up by about 3 |
| The dashboard's weather panel | Updates within 20 seconds of each transmission; wind arrow points where the wind is blowing to |
| `weather-station <id>` with the ID from `weather`, then reset and `weather` | `station <id> only`, kept after the reset |
| `weather-station any` | Back to accepting any station |
| Open `https://<device-ip>/weather` and leave it for 10 minutes | Current conditions, and the charts fill in with one point a minute |

If nothing is heard after a few minutes with the sensor close by, check the module's band and the antenna first. Then turn on "Invert the FSK data" in `menuconfig`, rebuild and flash, and try again. Tell me which setting worked, so the default can be fixed.

## 8. Check the SDR resets
Do this before the Radxa is wired, with a meter on GPIO4 and GPIO5. See [HARDWARE.md](HARDWARE.md#sdr-board-resets) for the wiring.

| Check | Expected |
|---|---|
| At rest | GPIO5 low. GPIO4 floating (open drain); with a 10 kΩ pull-up to 3.3 V fitted for the test, it reads high |
| `sdr-reboot` on the USB console | GPIO4 low for about 1 s, then released; `sdr` shows 1 reboot |
| `sdr-power-cycle` | GPIO5 high for about 5 s, then low; the relay clicks twice once it's fitted |
| Another `sdr-power-cycle` within 10 s | `ERROR: an SDR reset is in progress or just finished` |
| Press reset on the controller | GPIO5 stays low, so the Radxa's power isn't cut |

## 9. Test a firmware update
The first time, flash over USB as in step 1, because rollback needs the new bootloader; OTA can't replace the bootloader. Then make a small change, such as the version, build, and upload it from the dashboard's Firmware section, or:

```sh
curl -sk $C -X POST -H 'Content-Type: application/octet-stream' --data-binary @build/rf_filter_controller.bin https://<device-ip>/api/v1/ota/image
```

| Check | Expected |
|---|---|
| Upload | `202`; `/api/v1/ota/status` shows `boot_partition` `ota_1` and `last_result` `staged_reboot_required` |
| Upload a file that isn't this firmware, such as a time server image | `400 Not a firmware image for this controller`, nothing written |
| Reboot | Runs the new version on `ota_1` with `pending_verify` `true`; the log says it confirms in 300 s |
| Wait 5 minutes | `pending_verify` `false`, `last_result` `running_image_confirmed` |
| Roll back from the dashboard | Reboots into the previous version on `ota_0` |
| Upload again, reboot, and press reset within 5 minutes | The bootloader returns to the previous image; `rollbacks_detected` goes up by one |

In PowerShell, Windows' `curl.exe` needs the `.p12` client certificate, as in step 6.

## Report back
Send me anything that failed, with the serial log. Also note how long the first boot takes to print the bootstrap fingerprint, since it generates a key, and the fan's RPM at 100% next to its rated speed, and the weather station's RSSI and distance. If prompts and log lines get mixed up on the USB console, tell me that too.
