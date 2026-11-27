"""A stand-in for the controller's weather feed, for the WeeWX install test.

Serves one client at a time on 127.0.0.1:<port>, sending a new reading every 3 s,
with the rain total rising, in the controller's line format."""
import json
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 30090
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', port))
server.listen(1)
rain = 231.1
n = 0
while True:
    conn, _ = server.accept()
    try:
        while True:
            reading = {"type": "reading", "model": "VEVOR 7-in-1", "station": "faf8", "channel": 0,
                       "age_s": 0, "stale": False, "rssi_dbm": -90, "battery_ok": True,
                       "temperature_c": 20.0 + n / 10, "humidity_percent": 66, "wind_avg_kmh": 7.2,
                       "wind_gust_kmh": 10.8, "wind_dir_deg": 90, "rain_mm": round(rain, 1),
                       "uv_index": 1, "light_lux": 12670}
            conn.sendall((json.dumps(reading) + '\n').encode())
            n += 1
            rain += 0.2
            time.sleep(3)
    except OSError:
        conn.close()
