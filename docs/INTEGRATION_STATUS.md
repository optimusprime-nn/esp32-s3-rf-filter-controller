# Integrated firmware status

Boot flow: safe GPIO setup and asserted RF inhibit -> NVS init -> saved filter port -> load validated management settings -> Wi-Fi STA -> configured SNTP -> read-only HTTPS diagnostics, started only when a certificate and key are provisioned in NVS namespace `tls`.

If NVS cannot initialise the device stays offline with RF inhibit asserted; it does not panic, because every reset would release the inhibit output. Wi-Fi reconnects back off from 1 s to 60 s.

Network uses Wi-Fi credentials in NVS namespace `wifi` keys `ssid` and `password`. These credentials are provisioned over the USB console; no open AP or default passwords are enabled. Without credentials the device continues RF-inhibited and offline. The configuration schema contains DHCP/static IPv4, gateway, netmask, DNS, hostname, FQDN, NTP server and ACME zone/enable flag.

## USB provisioning
The console runs on the ESP32-S3 native USB port (USB Serial/JTAG, GPIO19 and GPIO20) from boot, once NVS is up. Commands: `status`, `set-wifi`, `set-password`, `set-tls`; Ctrl-C cancels. Passwords and the private key are not echoed. Changes take effect at the next reset. The firmware does not reset itself, because a reset releases the RF inhibit output until `app_main` runs.

`set-password` stores a salted PBKDF2-HMAC-SHA256 record (20,000 iterations) in namespace `auth` key `password`. `set-tls` checks PEM markers only; a bad certificate or mismatched key shows up as HTTPS failing to start at the next boot.

## Filter control
Filters are selected with `filter 1` to `filter 4` on the USB console, or through the API after login. The selection is saved and restored at boot. See [BRINGUP.md](BRINGUP.md) for the first flash.

## Management API and web page
The HTTPS API is specified in [API.md](API.md), and the web page uses nothing else. It covers status, health, Prometheus metrics, filter, fan, temperature, network settings, Wi-Fi, TLS, password change and reboot. Everything except `/`, health, metrics and login needs a session. The dashboard refreshes every second.

## Fan and temperature
The fan runs from 25 kHz PWM on GPIO8, with its tachometer on GPIO9. The speed is saved and defaults to 100%. Temperatures come from the ESP32-S3's internal sensor and from LM75-family I2C sensors on GPIO10 (SDA) and GPIO11 (SCL) at the addresses set in `menuconfig`, read every 2 seconds. Pins, polarity and addresses are set under "RF controller fan" and "RF controller temperature sensors" in `menuconfig`. The fan isn't driven by temperature: its speed is set by hand.

## Explicit nonfunctional features
- No USB command for network settings; use the API.
- No ACME transaction engine or Cloudflare API token storage ported yet; reference implementation is substantially larger and needs heap/flash review.
- No validated reset driver; Radxa reset pin remains inactive.
- RF source inhibit is deliberately not released.
- No KiCad schematic/ERC.
- TLS keys and Wi-Fi credentials are in unencrypted NVS.
- The flash layout has two OTA slots, but no OTA update code or rollback exists yet. Firmware boots from `ota_0`.
- Stored settings have no schema version; add one before the settings struct changes.
- Static IPv4 is applied at startup only; no network rollback yet.
- New integration commits require their own CI validation before merge.

## Required hardware validation
Verify selected ESP32-S3 module, GPIO boot strapping, HMC7992 control voltage and power, filter paths, Radxa reset electrical interface, and fail-safe RF mute. No active-RF bench tests until this is complete.
