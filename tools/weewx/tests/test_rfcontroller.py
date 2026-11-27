"""Tests for the WeeWX driver. Needs weewx installed (pip install weewx)."""
import json
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'bin'))

import weewx  # noqa: E402
from user import rfcontroller as rc  # noqa: E402

# A line exactly as the controller sent it on 2026-11-12.
SAMPLE = {"type": "reading", "model": "VEVOR 7-in-1", "station": "faf8", "channel": 0, "age_s": 0,
          "stale": False, "rssi_dbm": -90, "battery_ok": True, "temperature_c": 25.6,
          "humidity_percent": 66, "wind_avg_kmh": 5.6, "wind_gust_kmh": 7.2, "wind_dir_deg": 84,
          "rain_mm": 231.1, "uv_index": 0, "light_lux": 1653}


def line(**changes):
    d = dict(SAMPLE)
    d.update(changes)
    return json.dumps(d)


class ConverterTest(unittest.TestCase):
    def setUp(self):
        self.c = rc.ReadingConverter()

    def test_units_and_fields(self):
        p = self.c.convert(line(), now=1000.0)
        self.assertEqual(p['usUnits'], weewx.METRICWX)
        self.assertEqual(p['dateTime'], 1000)
        self.assertEqual(p['outTemp'], 25.6)
        self.assertEqual(p['outHumidity'], 66)
        self.assertAlmostEqual(p['windSpeed'], 5.6 / 3.6)
        self.assertAlmostEqual(p['windGust'], 2.0)
        self.assertEqual(p['windDir'], 84)
        self.assertEqual(p['luminosity'], 1653)
        self.assertAlmostEqual(p['radiation'], 1653 / 126.7)
        self.assertEqual(p['outTempBatteryStatus'], 0)
        self.assertIsNone(p['rain'])  # the first total only sets the baseline

    def test_rain_is_the_difference_between_totals(self):
        self.c.convert(line(rain_mm=231.1), now=1000.0)
        p = self.c.convert(line(rain_mm=231.6), now=1020.0)
        self.assertAlmostEqual(p['rain'], 0.5)
        p = self.c.convert(line(rain_mm=231.6), now=1040.0)
        self.assertEqual(p['rain'], 0)

    def test_rain_counter_reset(self):
        self.c.convert(line(rain_mm=231.1), now=1000.0)
        p = self.c.convert(line(rain_mm=0.4), now=1020.0)
        self.assertAlmostEqual(p['rain'], 0.4)

    def test_repeats_and_reconnects_are_dropped(self):
        self.assertIsNotNone(self.c.convert(line(age_s=2), now=1000.0))
        # The 30 s repeat of the same reading, and the same reading sent again on reconnect.
        self.assertIsNone(self.c.convert(line(age_s=32), now=1030.0))
        self.assertIsNone(self.c.convert(line(age_s=40), now=1038.0))
        self.assertIsNotNone(self.c.convert(line(age_s=0), now=1019.0 + 20))

    def test_ignores_waiting_stale_bad_and_other_stations(self):
        self.assertIsNone(self.c.convert('{"type":"waiting","receiver":"running","frequency_mhz":915.000}', now=1000.0))
        self.assertIsNone(self.c.convert(line(stale=True), now=1000.0))
        self.assertIsNone(self.c.convert('not json', now=1000.0))
        only = rc.ReadingConverter(station='FAF8')
        self.assertIsNone(only.convert(line(station='1234'), now=1000.0))
        self.assertIsNotNone(only.convert(line(), now=1000.0))

    def test_low_battery_and_no_radiation(self):
        c = rc.ReadingConverter(lux_per_wm2=0)
        p = c.convert(line(battery_ok=False), now=1000.0)
        self.assertEqual(p['outTempBatteryStatus'], 1)
        self.assertNotIn('radiation', p)


class DriverTest(unittest.TestCase):
    def test_reads_packets_from_a_feed(self):
        server = socket.socket()
        server.bind(('127.0.0.1', 0))
        server.listen(1)
        port = server.getsockname()[1]

        def feed():
            conn, _ = server.accept()
            # A split line, a repeat, then a new reading: by their ages the first arrived 25 s before the last.
            data = (line(age_s=25) + '\n' + line(age_s=55) + '\n' + line(age_s=0, temperature_c=26.0) + '\n').encode()
            conn.sendall(data[:50])
            conn.sendall(data[50:])
            conn.close()
            server.close()

        threading.Thread(target=feed, daemon=True).start()
        driver = rc.RFControllerDriver(host='127.0.0.1', port=str(port), retry_wait='0', max_tries='1', timeout='5')
        packets = []
        with self.assertRaises(weewx.WeeWxIOError):
            for p in driver.genLoopPackets():
                packets.append(p)
        self.assertEqual([p['outTemp'] for p in packets], [25.6, 26.0])


if __name__ == '__main__':
    unittest.main()
