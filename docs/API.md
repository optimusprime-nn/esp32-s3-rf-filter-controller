# Management API (v1)

Everything the web page does goes through this HTTPS API on port 443, so anything you can do in the browser you can also script. This document is the specification; the firmware implements it. Access works the same way as on esp32p4_nts_time_server, so the same management CA, client certificates and tools can manage both.

- **Before the controller is claimed**, only two addresses exist: `GET /` (an information page) and `PUT /api/v1/provisioning/claim`. No client certificate is asked for.
- **After the claim**, every address needs a client certificate signed by your management CA (mutual TLS). Connections without one are refused during the TLS handshake, before any HTTP request is read. There are no passwords or sessions.

Request bodies are JSON with `Content-Type: application/json` unless stated; a JSON call without that type is refused with `415`. Successful responses are JSON, errors are plain text with a `4xx` or `5xx` status, and every response carries `Cache-Control: no-store`. Settings that need a restart say so with `"reboot_required":true`, and apply after `POST /api/v1/system/reboot` or a manual reset.

## Server certificate

The controller makes its own self-signed bootstrap certificate at first boot (P-256, `CN=esp32s3-rf-bootstrap`) and keeps it across reboots. No client trusts it, so check it by fingerprint. The SHA-256 fingerprint is printed on the serial console at every boot, and on the USB console by `enrollment`:

```text
W (…) web_tls: Bootstrap certificate SHA-256: 3F2A…
```

Compare that with what the controller serves, then use `curl -k` for that address:

```sh
openssl s_client -connect 192.0.2.10:443 </dev/null 2>/dev/null | openssl x509 -noout -fingerprint -sha256
```

A certificate stored with `PUT /api/v1/config/tls` or the USB console's `set-tls` replaces the bootstrap certificate from the next boot. Its fingerprint is then printed as `Server certificate SHA-256:`. `DELETE /api/v1/config/tls` goes back to the bootstrap certificate.

## Enrollment

An unclaimed controller prints a one-time 32-character enrollment code on the serial console at every boot, and on the USB console with `enrollment`. A new code is made at each boot until the controller is claimed.

```text
W (…) OWNERSHIP: One-time management enrollment code: 9C41…
```

| Method | Path | Purpose |
|---|---|---|
| PUT | `/api/v1/provisioning/claim` | Claim an unclaimed controller |

The body is your management CA certificate in PEM, which must be allowed to sign certificates (`keyUsage` includes `keyCertSign`), with the `X-Provisioning-Code` header. Use `Content-Type: application/x-pem-file`.

| Status | Meaning |
|---|---|
| `202` | Claimed. `{"claimed":true,"reboot_required":true,"management_mode_after_reboot":"mtls_authenticated"}`. The controller reboots about a second later and from then on requires mTLS |
| `400` | The CA certificate is missing, too long (over 6144 bytes) or invalid |
| `401` | No `X-Provisioning-Code` header |
| `403` | Wrong code |
| `429` | Five wrong codes: enrollment is locked until the controller reboots, which prints a new code |
| `404` | Already claimed |

A claim can't be undone over the network. To release a controller, erase its flash and flash it again; this also removes Wi-Fi credentials and every setting.

## Confirmation headers

Destructive actions need a header with a fixed word. Without it the device answers `412 Precondition Failed`.

| Header | Value | Used by |
|---|---|---|
| `X-Confirm-Reboot` | `REBOOT` | `POST /api/v1/system/reboot` |

## Pages

| Method | Path | Page |
|---|---|---|
| GET | `/` | Dashboard (before the claim: the enrollment information page). It refreshes every second from `GET /api/v1/status` |
| GET | `/weather` | Weather station page: current conditions every 5 seconds, and charts of the history over 1, 6, 24 or 48 hours |

To use the dashboard in a browser, import your client certificate (for example `admin-client.p12`) into the browser or the operating system's certificate store.

## Status and health

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/v1/status` | Everything the dashboard shows |
| GET | `/api/v1/health` | Readiness check. `200` `{"status":"ok","hostname":"rf","network_ready":true}` once Wi-Fi has an address, otherwise `503` with `"status":"not-ready"` |
| GET | `/metrics` | Prometheus text format. See [Metrics](#metrics). A scraper needs a client certificate too |

`/api/v1/status` has these sections:

| Section | Fields |
|---|---|
| `device` | `hostname`, `fqdn`, `ipv4` (`null` without an address), `mac`, `firmware`, `idf` |
| `system` | `uptime_ms`, `reset_reason`, `heap_internal_free`, `heap_internal_min_free`, `heap_psram_free` |
| `time` | `synchronized`, `unix_time`, `ntp_server` |
| `rf` | `filter` (1 to 4) |
| `fan` | `percent`, `rpm` (`null` without a tachometer), `pwm_gpio`, `tach_gpio` (`-1` for none) |
| `weather` | As `GET /api/v1/sensors/weather` |
| `temperature` | `sensors`: a list of `{"id","source","address","ok","celsius"}`. `address` is `null` and `source` is `internal` for the ESP32-S3's own sensor; I2C sensors have `source` `lm75` and their 7-bit address. `celsius` is `null` while `ok` is `false` |
| `provisioning` | `wifi` and `tls` (each `true` once stored), `management_claimed` |
| `console` | `mode` (`mtls`), `server_certificate` (`bootstrap` or `provisioned`) |

## RF filter

| Method | Path | Purpose | Body |
|---|---|---|---|
| GET | `/api/v1/rf/filter` | `{"filter":2}` | |
| PUT | `/api/v1/rf/filter` | Switch filter. Saved for the next boot. `400` unless 1 to 4 | `{"filter":3}` |

## Fan

| Method | Path | Purpose | Body |
|---|---|---|---|
| GET | `/api/v1/fan` | Same fields as the status `fan` section | |
| PUT | `/api/v1/fan` | Set the speed in percent. Saved for the next boot. `400` unless a whole number from 0 to 100 | `{"percent":60}` |

The tachometer is sampled once a second. Some fans don't stop at 0%.

## Temperature

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/v1/sensors/temperature` | Same as the status `temperature` section. Sensors are read every 2 seconds |

## Weather station

A VEVOR 7-in-1 weather station's outdoor sensor, received by a CC1101. See [HARDWARE.md](HARDWARE.md#weather-station-receiver).

| Method | Path | Purpose | Body |
|---|---|---|---|
| GET | `/api/v1/sensors/weather` | The latest reading and the receiver's state | |
| GET | `/api/v1/sensors/weather/history?hours=24` | Readings recorded on the controller, one a minute, for the last 1 to 48 hours (default 24) | |
| PUT | `/api/v1/sensors/weather/station` | Accept only this station, or any with `null`. Saved for the next boot. `409` if the receiver isn't running | `{"station":"f8f7"}` |

| Field | Meaning |
|---|---|
| `receiver` | `running`, `missing` (no CC1101 answered at boot) or `disabled` (turned off in `menuconfig`) |
| `frequency_mhz` | Receive frequency |
| `station_filter` | The only station accepted, or `null` for any |
| `packets` | `ok` (new readings), `rejected` (failed checks, including noise that looks like a packet, so this grows slowly on its own) and `ignored` (other stations) |
| `heard` | Up to four stations heard recently, accepted or not: `station`, `rssi_dbm`, `age_s` |
| `reading` | `null` until the first packet, otherwise `station`, `channel`, `age_s`, `stale` (no packet for 90 s), `rssi_dbm`, `battery_ok`, `temperature_c`, `humidity_percent`, `wind_avg_kmh`, `wind_gust_kmh`, `wind_dir_deg` (where the wind comes from), `rain_mm`, `uv_index`, `light_lux` |

The history is `{"interval_s":60,"capacity_hours":48,"fields":[...],"points":[[...],...]}`, oldest first. Each point lists the values named in `fields`: `age_s`, `temperature_c`, `humidity_percent`, `wind_avg_kmh`, `wind_gust_kmh`, `wind_dir_deg`, `rain_mm`, `uv_index`, `light_lux`, `rssi_dbm`. A sample is recorded only when a reading arrives, so gaps mean the station wasn't heard. The history is kept in PSRAM, or for 6 hours in internal RAM if PSRAM is missing, and is lost at every reboot.

`rain_mm` is the sensor's running total since its batteries were last fitted, so work out rainfall from the difference between two readings. `battery_ok` is decoded as rtl_433 does, but rtl_433 marks the meaning of that bit as unconfirmed. A station's ID can change when its batteries are replaced; set the filter again from the `heard` list.

## Configuration

| Method | Path | Purpose | Body |
|---|---|---|---|
| GET | `/api/v1/config` | `hostname`, `fqdn`, `ntp_server`, `network` (`dhcp`, `address`, `netmask`, `gateway`, `dns`), `acme` (`enabled`, `cloudflare_zone`) and `provisioning` (as in status). Never returns secrets | |
| PUT | `/api/v1/config/hostname` | Set the hostname. Applies at the next reboot | `{"hostname":"rf"}` |
| PUT | `/api/v1/config/ntp` | Set the NTP server. Applies at the next reboot | `{"server":"pool.ntp.org"}` |
| PUT | `/api/v1/config/network` | DHCP or static IPv4. Applies at the next reboot. With `"dhcp":true` the other fields are ignored | `{"dhcp":false,"address":"192.0.2.10","netmask":"255.255.255.0","gateway":"192.0.2.1","dns":"192.0.2.1"}` |
| PUT | `/api/v1/config/wifi` | Store Wi-Fi credentials. `200` `{"stored":true,"reboot_required":true}` | `{"ssid":"...","password":"..."}` |
| PUT | `/api/v1/config/tls` | Use this HTTPS certificate and key, in PEM, instead of the bootstrap certificate. `200` `{"stored":true,"reboot_required":true}` | `{"certificate":"-----BEGIN...","private_key":"-----BEGIN..."}` |
| DELETE | `/api/v1/config/tls` | Remove the stored certificate and go back to the bootstrap certificate. `200` `{"removed":true,"reboot_required":true}` | |

The hostname, NTP and network calls answer with the same JSON as `GET /api/v1/config`. Every configuration call answers `400` with a reason when a value is invalid, and stores nothing in that case.

The ACME certificate engine isn't implemented, so `acme` is read-only.

## System

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/v1/system/reboot` | Reboot about a second after replying. Needs `X-Confirm-Reboot: REBOOT`. `202` `{"scheduled":true,"action":"reboot"}` |

## Metrics

`GET /metrics` returns:

| Metric | Type | Labels |
|---|---|---|
| `rf_uptime_seconds` | gauge | |
| `rf_filter_selected` | gauge | |
| `rf_fan_duty_percent` | gauge | |
| `rf_fan_rpm` | gauge | Absent without a tachometer |
| `rf_temperature_celsius` | gauge | `sensor`, for each sensor reading `ok` |
| `rf_temperature_sensor_ok` | gauge | `sensor` |
| `rf_weather_packets_total` | counter | `result`: `ok`, `rejected` or `ignored`. Only while the receiver runs |
| `rf_weather_last_seen_seconds`, `rf_weather_rssi_dbm`, `rf_weather_battery_ok` | gauge | `station`. These and the weather metrics below appear once a reading has arrived |
| `rf_weather_temperature_celsius`, `rf_weather_humidity_percent` | gauge | `station` |
| `rf_weather_wind_avg_kmh`, `rf_weather_wind_gust_kmh`, `rf_weather_wind_direction_degrees` | gauge | `station` |
| `rf_weather_rain_mm_total` | counter | `station`. Resets when the sensor loses power |
| `rf_weather_uv_index`, `rf_weather_light_lux` | gauge | `station` |
| `rf_heap_internal_free_bytes` | gauge | |

## Examples

```sh
B=https://192.0.2.10
J='Content-Type: application/json'

# Claim (once, before the first reboot after setting up Wi-Fi)
curl -sk -X PUT -H 'Content-Type: application/x-pem-file' -H 'X-Provisioning-Code: <code>' \
     --data-binary @management-ca.crt.pem $B/api/v1/provisioning/claim

# From then on, every call uses the client certificate
# (-k only while the controller uses its bootstrap certificate; check its fingerprint first)
read -rs KEYPASS
C="--cert admin-client.crt.pem --key admin-client.key.pem --pass $KEYPASS"

curl -sk $C $B/api/v1/status
curl -sk $C -X PUT -H "$J" -d '{"filter":3}' $B/api/v1/rf/filter
curl -sk $C -X PUT -H "$J" -d '{"percent":60}' $B/api/v1/fan
curl -sk $C $B/api/v1/sensors/weather
curl -sk $C -X PUT -H "$J" -d '{"station":"f8f7"}' $B/api/v1/sensors/weather/station
curl -sk $C -X POST -H 'X-Confirm-Reboot: REBOOT' $B/api/v1/system/reboot
```

## Making a management CA by hand

The esp32p4_nts_time_server setup tool makes these files for you. Without it:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -days 3650 \
  -keyout management-ca.key.pem -out management-ca.crt.pem -subj "/CN=RF management CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout admin-client.key.pem -out admin-client.csr.pem -subj "/CN=rf-admin"
openssl x509 -req -in admin-client.csr.pem -CA management-ca.crt.pem -CAkey management-ca.key.pem \
  -days 825 -out admin-client.crt.pem -extfile <(printf "keyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth")
openssl pkcs12 -export -inkey admin-client.key.pem -in admin-client.crt.pem -out admin-client.p12
```

The same in Windows PowerShell, which has no `\` line continuation or `<(...)`:

```powershell
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -days 3650 -keyout management-ca.key.pem -out management-ca.crt.pem -subj "/CN=RF management CA" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout admin-client.key.pem -out admin-client.csr.pem -subj "/CN=rf-admin"
Set-Content -Encoding ascii admin-client.ext "keyUsage=critical,digitalSignature`nextendedKeyUsage=clientAuth"
openssl x509 -req -in admin-client.csr.pem -CA management-ca.crt.pem -CAkey management-ca.key.pem -days 825 -out admin-client.crt.pem -extfile admin-client.ext
openssl pkcs12 -export -inkey admin-client.key.pem -in admin-client.crt.pem -out admin-client.p12
```

Each command asks for a password to protect its private key. Keep `management-ca.key.pem` offline: anyone holding it can make certificates that manage the controller.
