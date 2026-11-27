# WeeWX driver for the ESP32-S3 RF controller's weather feed.
#
# The controller receives a VEVOR 7-in-1 weather station (YT60231/YT60233/YT60234)
# with a CC1101 and sends every reading as one line of JSON on a plain TCP port
# (30090 by default). This driver connects to that port and turns each new reading
# into a WeeWX loop packet. See docs/API.md, "Weather feed (TCP)", in the
# esp32-s3-rf-filter-controller repository for the line format.

import json
import logging
import socket
import time

import weewx
import weewx.drivers

DRIVER_NAME = 'RFController'
DRIVER_VERSION = '1.0'

log = logging.getLogger(__name__)

# A reading received less than this many seconds after the last one is the same
# reading again: the feed repeats its last line every 30 s, and sends the latest
# reading again after a reconnect.
SAME_READING_S = 5
# Rough conversion from illuminance to solar radiation for daylight.
DEFAULT_LUX_PER_WM2 = 126.7


def loader(config_dict, engine):
    return RFControllerDriver(**config_dict[DRIVER_NAME])


def confeditor_loader():
    return RFControllerConfEditor()


def to_float(v):
    try:
        return None if v is None else float(v)
    except (TypeError, ValueError):
        return None


class ReadingConverter(object):
    """Turns feed lines into WeeWX packets. Kept apart from the socket so it can be tested."""

    def __init__(self, station=None, lux_per_wm2=DEFAULT_LUX_PER_WM2):
        self.station = station.lower() if station else None
        self.lux_per_wm2 = lux_per_wm2
        self.last_rx_time = None
        self.last_rain_total = None

    def convert(self, line, now=None):
        """Returns a loop packet for a new reading, or None for anything else."""
        now = time.time() if now is None else now
        try:
            r = json.loads(line)
        except ValueError:
            log.debug("ignoring line that isn't JSON: %r", line[:80])
            return None
        if not isinstance(r, dict) or r.get('type') != 'reading':
            return None
        if r.get('stale'):
            return None
        if self.station and str(r.get('station', '')).lower() != self.station:
            return None
        rx_time = now - (to_float(r.get('age_s')) or 0.0)
        if self.last_rx_time is not None and rx_time - self.last_rx_time < SAME_READING_S:
            return None
        self.last_rx_time = rx_time

        avg = to_float(r.get('wind_avg_kmh'))
        gust = to_float(r.get('wind_gust_kmh'))
        lux = to_float(r.get('light_lux'))
        packet = {
            'dateTime': int(rx_time + 0.5),
            'usUnits': weewx.METRICWX,
            'outTemp': to_float(r.get('temperature_c')),
            'outHumidity': to_float(r.get('humidity_percent')),
            'windSpeed': None if avg is None else avg / 3.6,
            'windGust': None if gust is None else gust / 3.6,
            'windDir': to_float(r.get('wind_dir_deg')),
            'windGustDir': to_float(r.get('wind_dir_deg')),
            'UV': to_float(r.get('uv_index')),
            'luminosity': lux,
            'rain': self._rain(to_float(r.get('rain_mm'))),
        }
        if lux is not None and self.lux_per_wm2:
            packet['radiation'] = lux / self.lux_per_wm2
        if 'battery_ok' in r:
            packet['outTempBatteryStatus'] = 0 if r['battery_ok'] else 1
        return packet

    def _rain(self, total):
        """The station counts rain from power-up; WeeWX wants the rain since the last packet."""
        if total is None:
            return None
        last, self.last_rain_total = self.last_rain_total, total
        if last is None:
            return None
        if total < last:
            # The station restarted (new batteries) and its count began again at zero.
            log.info("rain total went from %.1f to %.1f mm: station counter reset", last, total)
            return total
        return total - last


class RFControllerDriver(weewx.drivers.AbstractDevice):
    """Reads the controller's TCP weather feed."""

    def __init__(self, **stn_dict):
        self.host = stn_dict.get('host', '192.168.2.51')
        self.port = int(stn_dict.get('port', 30090))
        # The feed sends something at least every 30 s; this long without a line means it's gone.
        self.timeout = float(stn_dict.get('timeout', 90))
        self.retry_wait = float(stn_dict.get('retry_wait', 10))
        # Give up after this many failed connections in a row, so WeeWX logs it and restarts.
        self.max_tries = int(stn_dict.get('max_tries', 30))
        lux = to_float(stn_dict.get('lux_per_wm2', DEFAULT_LUX_PER_WM2))
        self.converter = ReadingConverter(stn_dict.get('station') or None, lux)
        self.sock = None
        log.info("driver version %s, feed %s:%d", DRIVER_VERSION, self.host, self.port)
        if self.converter.station:
            log.info("accepting only station %s", self.converter.station)

    @property
    def hardware_name(self):
        return "RF Controller (VEVOR 7-in-1)"

    def _lines(self):
        """Yields feed lines, reconnecting as needed."""
        failures = 0
        while True:
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
                log.info("connected to %s:%d", self.host, self.port)
                failures = 0
                buf = b''
                while True:
                    data = self.sock.recv(4096)
                    if not data:
                        raise socket.error("feed closed the connection")
                    buf += data
                    while b'\n' in buf:
                        line, buf = buf.split(b'\n', 1)
                        yield line.decode('utf-8', 'replace').strip()
                    if len(buf) > 65536:
                        raise socket.error("line too long")
            except (socket.error, socket.timeout, OSError) as e:
                self.closePort()
                failures += 1
                log.error("feed %s:%d: %s (attempt %d of %d)", self.host, self.port, e, failures, self.max_tries)
                if failures >= self.max_tries:
                    raise weewx.WeeWxIOError("no weather feed from %s:%d" % (self.host, self.port))
                time.sleep(self.retry_wait)

    def genLoopPackets(self):
        for line in self._lines():
            packet = self.converter.convert(line)
            if packet is not None:
                yield packet

    def closePort(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


class RFControllerConfEditor(weewx.drivers.AbstractConfEditor):
    @property
    def default_stanza(self):
        return """
[RFController]
    # The ESP32-S3 RF controller's weather feed (a VEVOR 7-in-1 station received with a CC1101)

    # The controller's address and feed port
    host = 192.168.2.51
    port = 30090

    # Accept only this station ID (4 hex digits), or leave empty for any.
    # The controller can also be locked to one station.
    station = ""

    # Lux per W/m2 for estimating solar radiation from the light sensor; 0 to leave radiation out
    lux_per_wm2 = 126.7

    # The driver to use
    driver = user.rfcontroller
"""

    def prompt_for_settings(self):
        settings = {}
        print("Address of the RF controller")
        settings['host'] = self._prompt('host', '192.168.2.51')
        print("Port of its weather feed")
        settings['port'] = self._prompt('port', '30090')
        return settings


if __name__ == '__main__':
    # Print packets from a live feed: python rfcontroller.py [host] [port]
    import sys
    import weeutil.weeutil
    weewx.debug = 1
    # Plain console logging: WeeWX's own setup wants syslog, which Windows doesn't have.
    logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(name)s: %(message)s')
    args = {'host': sys.argv[1]} if len(sys.argv) > 1 else {}
    if len(sys.argv) > 2:
        args['port'] = sys.argv[2]
    driver = RFControllerDriver(**args)
    for p in driver.genLoopPackets():
        print(weeutil.weeutil.timestamp_to_string(p['dateTime']), p)
