# Integrated firmware status

Boot flow: safe GPIO setup and asserted RF inhibit -> NVS init -> saved filter port -> load validated management settings -> Wi-Fi STA -> configured SNTP -> read-only HTTPS diagnostics, started only when a certificate and key are provisioned in NVS namespace `tls`.

If NVS cannot initialise the device stays offline with RF inhibit asserted; it does not panic, because every reset would release the inhibit output. Wi-Fi reconnects back off from 1 s to 60 s.

Network uses Wi-Fi credentials in NVS namespace `wifi` keys `ssid` and `password`. These credentials are provisioned over the USB console; no open AP or default passwords are enabled. Without credentials the device continues RF-inhibited and offline. The configuration schema contains DHCP/static IPv4, gateway, netmask, DNS, hostname, FQDN, NTP server and ACME zone/enable flag.

## USB provisioning
The console runs on the ESP32-S3 native USB port (USB Serial/JTAG, GPIO19 and GPIO20) from boot, once NVS is up. Commands: `status`, `set-wifi`, `set-password`, `set-tls`; Ctrl-C cancels. Passwords and the private key are not echoed. Changes take effect at the next reset. The firmware does not reset itself, because a reset releases the RF inhibit output until `app_main` runs.

`set-password` stores a salted PBKDF2-HMAC-SHA256 record (20,000 iterations) in namespace `auth` key `password`. `set-tls` checks PEM markers only; a bad certificate or mismatched key shows up as HTTPS failing to start at the next boot.

## Filter control
Filters are selected with `filter 1` to `filter 4` on the USB console, or over HTTPS after login. The selection is saved and restored at boot. HTTPS allows one session at a time, which ends after 15 minutes idle or when the device password changes. After three failed logins, each further failure locks login out, from 1 second doubling to 5 minutes. State-changing requests need the session cookie and an `X-RF-Request` header. See [BRINGUP.md](BRINGUP.md) for the first flash.

## Explicit nonfunctional features
- No write-capable settings form, and no USB command for network settings.
- No ACME transaction engine or Cloudflare API token storage ported yet; reference implementation is substantially larger and needs heap/flash review.
- No validated reset driver; Radxa reset pin remains inactive.
- RF source inhibit is deliberately not released.
- No KiCad schematic/ERC.
- `/` and `/api/status` remain readable without login. TLS keys and Wi-Fi credentials are in unencrypted NVS.
- The flash layout has two OTA slots, but no OTA update code or rollback exists yet. Firmware boots from `ota_0`.
- Stored settings have no schema version; add one before the settings struct changes.
- Static IPv4 is applied at startup only; no network rollback yet.
- New integration commits require their own CI validation before merge.

## Required hardware validation
Verify selected ESP32-S3 module, GPIO boot strapping, HMC7992 control voltage and power, filter paths, Radxa reset electrical interface, and fail-safe RF mute. No active-RF bench tests until this is complete.
