# Integrated firmware status

Boot flow: safe GPIO setup and asserted RF inhibit -> NVS init -> management ownership (prints the enrollment code until claimed) -> saved filter port -> fan and temperature sensors -> USB console -> load validated management settings -> Wi-Fi STA -> configured SNTP -> HTTPS: enrollment only until claimed, then the mTLS management API.

If NVS cannot initialise the device stays offline with RF inhibit asserted; it does not panic, because every reset would release the inhibit output. Wi-Fi reconnects back off from 1 s to 60 s.

Network uses Wi-Fi credentials in NVS namespace `wifi` keys `ssid` and `password`. These credentials are provisioned over the USB console or, after the claim, the API; no open AP or default passwords are enabled. Without credentials the device continues RF-inhibited and offline. The configuration schema contains DHCP/static IPv4, gateway, netmask, DNS, hostname, FQDN, NTP server and ACME zone/enable flag.

## Management access
Matches esp32p4_nts_time_server, so the same tools and management CA work for both. The controller makes a self-signed P-256 bootstrap certificate at first boot and prints its SHA-256 fingerprint. An unclaimed controller prints a one-time enrollment code and serves only `GET /` and `PUT /api/v1/provisioning/claim`. Once claimed with a management CA it requires mTLS for everything. Log lines use the reference's wording so its setup tool can read them. See [API.md](API.md) and [SECURITY.md](SECURITY.md).

## USB console
The console runs on the ESP32-S3 native USB port (USB Serial/JTAG, GPIO19 and GPIO20) from boot, once NVS is up. Commands: `status`, `filter`, `fan`, `temps`, `weather`, `weather-station`, `sdr`, `sdr-reboot`, `sdr-power-cycle`, `enrollment`, `set-wifi`, `set-tls`; Ctrl-C cancels. The Wi-Fi password and the private key are not echoed. Changes take effect at the next reset. The firmware does not reset itself from the console, because a reset releases the RF inhibit output until `app_main` runs.

`set-tls` checks PEM markers only; a bad certificate or mismatched key makes HTTPS fail to start at the next boot. Until one is stored, HTTPS uses the bootstrap certificate.

## Filter control
Filters are selected with `filter 1` to `filter 4` on the USB console, or through the API. The selection is saved and restored at boot. See [BRINGUP.md](BRINGUP.md) for the first flash.

## Management API and web page
The HTTPS API is specified in [API.md](API.md), and the web page uses nothing else. It covers status, health, Prometheus metrics, filter, fan, temperature, the weather station, network settings, Wi-Fi, TLS and reboot. The dashboard refreshes every second.

## Fan and temperature
The fan runs from 25 kHz PWM on GPIO1, with its tachometer on GPIO9. The speed is saved and defaults to 100%. Temperatures come from the ESP32-S3's internal sensor, from LM75-family I2C sensors on GPIO10 (SDA) and GPIO11 (SCL) at the addresses set in `menuconfig`, and from an AHT20 or AHT30 at 0x38 if one answers at boot, which also gives relative humidity. A second AHT20/AHT30 can sit on a second I2C bus on GPIO17 (SDA) and GPIO18 (SCL). All are read every 2 seconds. Pins, polarity and addresses are set under "RF controller fan" and "RF controller temperature sensors" in `menuconfig`. The fan isn't driven by temperature: its speed is set by hand.

## Weather station
A CC1101 on SPI (GPIO12 to GPIO16) receives a VEVOR 7-in-1 weather station (YT60233/YT60234 on 915 MHz by default). Readings appear in the API, metrics, dashboard and the console's `weather` command. The station filter can be changed at run time and is saved. The `/weather` page shows current conditions and charts of up to 48 hours of history, recorded once a minute in PSRAM and lost at reboot. Without a CC1101 the firmware runs as before and reports the receiver as missing. Decoding is host-tested against rtl_433's sample packet, and the board receives the user's station (2026-11-11), once the receive filter was widened to 325 kHz. The frequency and FSK sense can be changed at run time. Other devices on the LAN, such as the ESP32-P4 ADS-B display, can read each reading as a line of JSON from the read-only TCP feed on port 30090.

## Explicit nonfunctional features
- OTA images aren't signed (no secure boot). Rollback needs the bootloader built with this firmware, so a board flashed before OTA support needs one more USB flash.
- Let's Encrypt by DNS-01 through Cloudflare is ported from the reference, but hasn't issued a real certificate on this board yet. It keeps one certificate, not the reference's two slots.
- No USB command for network settings; use the API.
- The SDR resets are untested with a Radxa connected, and the Radxa needs a `gpio-shutdown` overlay or service for the soft reset.
- RF source inhibit is deliberately not released.
- No KiCad schematic/ERC.
- The bootstrap key, TLS keys, the ACME account key, the Cloudflare token and Wi-Fi credentials are in unencrypted NVS.
- Stored settings have no schema version; add one before the settings struct changes.
- Static IPv4 is applied at startup only; no network rollback yet.
- New integration commits require their own CI validation before merge.

## Required hardware validation
Verify selected ESP32-S3 module, GPIO boot strapping, HMC7992 control voltage and power, filter paths, Radxa reset electrical interface, and fail-safe RF mute. No active-RF bench tests until this is complete.
