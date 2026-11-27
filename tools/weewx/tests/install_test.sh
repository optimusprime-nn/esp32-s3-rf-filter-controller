#!/bin/sh
# End-to-end check on Linux: create a WeeWX station, install this extension with weectl,
# select the driver, then run weewxd against a fake feed and look for its loop packets.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d)
port=30190
trap 'kill $feed 2>/dev/null || true; rm -rf "$root"' EXIT

weectl station create "$root" --no-prompt --driver=weewx.drivers.simulator >/dev/null
weectl extension install "$here" --config="$root/weewx.conf" --yes
weectl station reconfigure --config="$root/weewx.conf" --driver=user.rfcontroller --no-prompt >/dev/null

# Point the driver at the fake feed, print every loop packet, and log to the console.
python - "$root/weewx.conf" "$port" <<'EOF'
import sys, configobj
conf = configobj.ConfigObj(sys.argv[1], encoding='utf-8')
assert conf['Station']['station_type'] == 'RFController', conf['Station']['station_type']
conf['RFController']['host'] = '127.0.0.1'
conf['RFController']['port'] = sys.argv[2]
svc = conf['Engine']['Services']
rs = svc.get('report_services', [])
svc['report_services'] = (rs if isinstance(rs, list) else [rs]) + ['weewx.engine.StdPrint']
conf['Logging'] = {'root': {'level': 'INFO', 'handlers': ['console']}}
conf.write()
EOF

python "$here/tests/fake_feed.py" "$port" &
feed=$!
sleep 1
out="$root/weewxd.out"
timeout 30 weewxd --config="$root/weewx.conf" >"$out" 2>&1 || true
cat "$out"

loops=$(grep -c '^LOOP:' "$out" || true)
echo "loop packets: $loops"
[ "$loops" -ge 3 ] || { echo "FAIL: expected at least 3 loop packets"; exit 1; }
grep -q 'RFController\|rfcontroller' "$out" || { echo "FAIL: driver not loaded"; exit 1; }
# The station's units are US: 20 C is 68 F, the fake 12670 lux is 100 W/m2, and the rain
# total rises 0.2 mm (0.00787 in) per reading after the first, which only sets the baseline.
last=$(grep '^LOOP:' "$out" | tail -1)
echo "$last" | grep -q 'outTemp: 6[89]\.' || { echo "FAIL: outTemp"; exit 1; }
echo "$last" | grep -q 'luminosity: 12670.0' || { echo "FAIL: luminosity"; exit 1; }
echo "$last" | grep -q 'radiation: 100.0' || { echo "FAIL: radiation"; exit 1; }
echo "$last" | grep -q 'rain: 0.0078' || { echo "FAIL: rain per reading"; exit 1; }
echo "PASS"
