# Management API (v1)

Everything the web page does goes through this HTTPS API on port 443, so anything you can do in the browser you can also script. This document is the specification; the firmware implements it.

- Request bodies are JSON with `Content-Type: application/json`. A body without that type is refused with `415`.
- Successful responses are JSON. Errors are plain text with a `4xx` or `5xx` status.
- Every response carries `Cache-Control: no-store`.
- Settings that need a restart say so with `"reboot_required":true`, and apply after `POST /api/v1/system/reboot` or a manual reset.

## Authentication

The API uses the device password set over the USB console (`set-password`). Logging in returns a session cookie, `rf_session`, which is `Secure`, `HttpOnly` and `SameSite=Strict`.

- One session exists at a time. A new login replaces it, and it ends after 15 minutes without a request or when the password changes.
- After three failed logins in a row, each further failure locks login, starting at 1 second and doubling up to 5 minutes. A locked login answers `429`.
- Without a valid session, protected addresses answer `401`.
- `GET /`, `GET /api/v1/health`, `GET /metrics` and `POST /api/v1/session` need no session.

Unlike the time server this API is modelled on, it has no client certificates: the password is the credential. Cross-site requests are blocked because every state change is a `PUT`, a `DELETE`, a JSON `POST`, or a `POST` with a confirmation header, and a browser must ask permission before sending any of those to another site. The device never grants that permission.

| Method | Path | Purpose | Body |
|---|---|---|---|
| POST | `/api/v1/session` | Log in. `200` `{"authenticated":true}` and the cookie; `401` wrong password; `429` locked | `{"password":"..."}` |
| GET | `/api/v1/session` | `{"authenticated":true}` or `false`. Never `401` | |
| DELETE | `/api/v1/session` | Log out. `200` `{"authenticated":false}` | |

## Confirmation headers

Destructive actions need a header with a fixed word. Without it the device answers `412 Precondition Failed`.

| Header | Value | Used by |
|---|---|---|
| `X-Confirm-Reboot` | `REBOOT` | `POST /api/v1/system/reboot` |

## Pages

| Method | Path | Page |
|---|---|---|
| GET | `/` | Login, then the dashboard. It refreshes every second from `GET /api/v1/status` |

## Status and health

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/v1/status` | Everything the dashboard shows |
| GET | `/api/v1/health` | Readiness check, no login. `200` `{"status":"ok","hostname":"rf","network_ready":true}` once Wi-Fi has an address, otherwise `503` with `"status":"not-ready"` |
| GET | `/metrics` | Prometheus text format, no login. See [Metrics](#metrics) |

`/api/v1/status` has these sections:

| Section | Fields |
|---|---|
| `device` | `hostname`, `fqdn`, `ipv4` (`null` without an address), `mac`, `firmware`, `idf` |
| `system` | `uptime_ms`, `reset_reason`, `heap_internal_free`, `heap_internal_min_free`, `heap_psram_free` |
| `time` | `synchronized`, `unix_time`, `ntp_server` |
| `rf` | `filter` (1 to 4) |
| `fan` | `percent`, `rpm` (`null` without a tachometer), `pwm_gpio`, `tach_gpio` (`-1` for none) |
| `temperature` | `sensors`: a list of `{"id","source","address","ok","celsius"}`. `address` is `null` and `source` is `internal` for the ESP32-S3's own sensor; I2C sensors have `source` `lm75` and their 7-bit address. `celsius` is `null` while `ok` is `false` |
| `provisioning` | `wifi`, `device_password`, `tls` (each `true` once stored) |

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

## Configuration

| Method | Path | Purpose | Body |
|---|---|---|---|
| GET | `/api/v1/config` | `hostname`, `fqdn`, `ntp_server`, `network` (`dhcp`, `address`, `netmask`, `gateway`, `dns`), `acme` (`enabled`, `cloudflare_zone`) and `provisioning` (as in status). Never returns secrets | |
| PUT | `/api/v1/config/hostname` | Set the hostname. Applies at the next reboot | `{"hostname":"rf"}` |
| PUT | `/api/v1/config/ntp` | Set the NTP server. Applies at the next reboot | `{"server":"pool.ntp.org"}` |
| PUT | `/api/v1/config/network` | DHCP or static IPv4. Applies at the next reboot. With `"dhcp":true` the other fields are ignored | `{"dhcp":false,"address":"192.0.2.10","netmask":"255.255.255.0","gateway":"192.0.2.1","dns":"192.0.2.1"}` |
| PUT | `/api/v1/config/wifi` | Store Wi-Fi credentials. `200` `{"stored":true,"reboot_required":true}` | `{"ssid":"...","password":"..."}` |
| PUT | `/api/v1/config/password` | Change the device password. Ends the session. `401` if `current` is wrong, counted like a failed login | `{"current":"...","new":"..."}` |
| PUT | `/api/v1/config/tls` | Replace the HTTPS certificate and key, in PEM. `200` `{"stored":true,"reboot_required":true}` | `{"certificate":"-----BEGIN...","private_key":"-----BEGIN..."}` |

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
| `rf_heap_internal_free_bytes` | gauge | |

## Examples

```sh
B=https://192.0.2.10
J='Content-Type: application/json'

# Log in and keep the cookie (-k only while using a self-signed certificate)
curl -sk -c jar -H "$J" -d '{"password":"..."}' $B/api/v1/session

# Read everything
curl -sk -b jar $B/api/v1/status

# Select filter 3 and set the fan to 60%
curl -sk -b jar -X PUT -H "$J" -d '{"filter":3}' $B/api/v1/rf/filter
curl -sk -b jar -X PUT -H "$J" -d '{"percent":60}' $B/api/v1/fan

# Reboot
curl -sk -b jar -X POST -H 'X-Confirm-Reboot: REBOOT' $B/api/v1/system/reboot
```
