# WeeWX driver for the RF controller

Feeds [WeeWX](https://weewx.com) from the RF controller's weather feed. The controller receives a VEVOR 7-in-1 station (YT60231, YT60233 or YT60234) with a CC1101 and sends each reading as a line of JSON on TCP port 30090. This driver connects to that port, so WeeWX needs no radio of its own.

Needs WeeWX 5. It doesn't need a client certificate: the feed is read-only weather data.

## Install

On the machine that runs WeeWX, from a copy of this folder (or the `weewx` folder in a release zip):

```bash
weectl extension install /path/to/weewx
weectl station reconfigure --driver=user.rfcontroller
```

Answer the prompts with the controller's address (default `192.168.2.51`) and port (`30090`). Then restart WeeWX, for example with `sudo systemctl restart weewx`. If WeeWX already has data from another station, read the WeeWX guide on changing hardware first.

The settings end up in `weewx.conf`:

```ini
[Station]
    station_type = RFController

[RFController]
    host = 192.168.2.51
    port = 30090
    station = ""         # accept only this station ID (4 hex digits); empty for any
    lux_per_wm2 = 126.7  # lux per W/m2 for estimating radiation; 0 leaves radiation out
    driver = user.rfcontroller
```

## What it records

Packets are in WeeWX's `METRICWX` units, one for every new reading (about every 20 s):

| WeeWX field | From the feed |
|---|---|
| `outTemp` | `temperature_c` |
| `outHumidity` | `humidity_percent` |
| `windSpeed`, `windGust` | `wind_avg_kmh`, `wind_gust_kmh`, in m/s |
| `windDir`, `windGustDir` | `wind_dir_deg` (where the wind comes from) |
| `rain` | The rise in `rain_mm` since the last reading. The station counts from power-up; after a battery change its count restarts and the driver carries on from the new total |
| `UV` | `uv_index` |
| `luminosity` | `light_lux` |
| `radiation` | `light_lux / lux_per_wm2`, an estimate |
| `outTempBatteryStatus` | `0` when `battery_ok`, otherwise `1` |

The first reading after WeeWX starts only sets the rain baseline, so its `rain` is empty. Repeats of the same reading (the feed resends its last line every 30 s, and again after a reconnect), stale readings and the feed's `waiting` lines are skipped. The station has no barometer, so WeeWX shows no pressure.

## Troubleshooting

- Check the feed from the WeeWX machine with `nc 192.168.2.51 30090`. A JSON line should appear at once.
- The driver reconnects by itself every 10 s. After 30 failed tries in a row it stops, and WeeWX logs the error and restarts it.
- The controller accepts up to four feed clients at once.
- To watch packets without WeeWX running: `python bin/user/rfcontroller.py 192.168.2.51 30090`, with WeeWX's Python packages installed.

## Tests

`python -m unittest discover -s tests` with `pip install weewx`, and `sh tests/install_test.sh` for a full install and `weewxd` run against a fake feed (Linux). CI runs both.
