# Integrated firmware status

Boot flow: safe GPIO setup and asserted RF inhibit -> NVS init -> saved filter port -> load validated management settings -> Wi-Fi STA -> configured SNTP -> read-only HTTP diagnostics.

Network uses Wi-Fi credentials in NVS namespace `wifi` keys `ssid` and `password`. These credentials must be provisioned through a future secure physical/USB flow; no open AP or default passwords are enabled. Without credentials the device continues RF-inhibited and offline. The configuration schema contains DHCP/static IPv4, gateway, netmask, DNS, hostname, FQDN, NTP server and ACME zone/enable flag.

## Explicit nonfunctional features
- No credential provisioning UX, no authenticated HTTPS management, no write-capable settings form.
- No ACME transaction engine or Cloudflare API token storage ported yet; reference implementation is substantially larger and needs heap/flash review.
- No validated reset driver; Radxa reset pin remains inactive.
- RF source inhibit is deliberately not released.
- No KiCad schematic/ERC.
- Current HTTP diagnostic listener is **unencrypted and read-only**; never transmit credentials to it.
- Static IPv4 is applied at startup only; no network rollback yet.
- New integration commits require their own CI validation before merge.

## Required hardware validation
Verify selected ESP32-S3 module, GPIO boot strapping, HMC7992 control voltage and power, filter paths, Radxa reset electrical interface, and fail-safe RF mute. No active-RF bench tests until this is complete.
