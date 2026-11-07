# Hardware baseline (provisional)
MCU: ESP32-S3 N16R8, 16 MB quad flash and 8 MB octal PSRAM. Octal PSRAM occupies GPIO33 to GPIO37, so those pins are not available for control signals.

Flash layout (`partitions.csv`): 256 KiB NVS, an NVS key partition reserved for encryption, two 4 MiB OTA app slots, and about 7.7 MiB left unallocated for later use.

Two HMC7992 SP4T modules: common RFC of switch A is RF input; common RFC of switch B is RF output. RF1..RF4 connect through respective external filters. Both modules share A/B control bits, 3.3 V supply, and ground.

| Port | A | B |
|---|---|---|
| 1 | 0 | 0 |
| 2 | 1 | 0 |
| 3 | 0 | 1 |
| 4 | 1 | 1 |

GPIO4=A, GPIO5=B, GPIO6=Radxa reset driver (active low), GPIO7=RF mute (active high). These are **provisional** assignments. The reset GPIO must NOT be connected directly to an unknown Radxa reset pad or power rail. Use a correctly rated isolated/open-drain interface after identifying the Radxa board's documented reset circuit.

HMC7992 has no all-off code. Unselected RF ports are terminated. A mute interlock is only effective when connected to an actual upstream source-inhibit input; firmware delays alone do not prevent live-RF switching. Ensure the mute circuit defaults to inhibit while MCU is unpowered, resetting, or in fault. GPIO startup cannot guarantee RF safety.

RF modules need clean 3.3 V and common reference; verify board header orientation, power draw, voltage levels, and filter power ratings before connecting RF.

## Fan
Defaults assume a 4-wire 12 V PC fan. The fan's PWM input has its own pull-up, often to 5 V, so never connect it straight to a GPIO.

| Fan wire | Connect to |
|---|---|
| GND | Ground, shared with the ESP32-S3 |
| +12 V | 12 V supply |
| PWM | Collector of an NPN transistor, such as a 2N3904 or MMBT3904, or the drain of a logic-level N-MOSFET such as a 2N7002. Emitter or source to ground. Drive the base from GPIO8 through 1 kΩ, or the gate directly, with a 10 kΩ pull-down to ground |
| Tach | GPIO9, with a 10 kΩ pull-up to **3.3 V**. Never pull it up to 12 V |

The transistor inverts the signal, which is why "Invert PWM output" is on by default. The pull-down keeps the transistor off while the GPIO isn't driven, during reset or before the firmware starts, so the fan's own pull-up runs it at full speed. A 3-wire fan needs a low-side MOSFET switching its ground instead: turn the inversion off, and expect tach readings to be unreliable while it's being switched.

GPIO8, GPIO9 and the PWM frequency can be changed in `menuconfig` under "RF controller fan".

## Temperature sensors
LM75-family sensors (LM75, TMP75, TMP102, PCT2075) on I2C: SDA on GPIO10 and SCL on GPIO11, both with 4.7 kΩ pull-ups to 3.3 V. The internal pull-ups are enabled but are too weak to rely on. Each sensor needs its own address in 0x48 to 0x4F, and every address must be listed in `menuconfig` under "RF controller temperature sensors" (default `0x48`). Only list addresses that hold one of these sensors. The firmware also reads the ESP32-S3's own temperature sensor.

## Weather station receiver
A CC1101 module receives the outdoor sensor of a VEVOR 7-in-1 weather station (YT60233 or YT60234 in the US on 915 MHz, YT60231 in Europe on 868 MHz). Buy a module built for that band: most cheap CC1101 boards are matched for 433 MHz and lose much of their sensitivity at 915 MHz.

| CC1101 | ESP32-S3 |
|---|---|
| VCC | 3.3 V only. The CC1101 is not 5 V tolerant |
| GND | GND |
| SCK | GPIO12 |
| SI (MOSI) | GPIO13 |
| SO (MISO, GDO1) | GPIO14 |
| CSn | GPIO15 |
| GDO0 | GPIO16 |
| GDO2 | Not connected |

Pins, frequency and a station ID filter are set under "RF controller weather station receiver" in `menuconfig`. The antenna can be a quarter-wave wire, 78 mm long for 915 MHz or 82 mm for 868 MHz. If the CC1101 shares an antenna with the filter bank, select a filter path that passes the station's frequency.

Radio settings: 2-FSK, 11.26 kBaud, 38 kHz deviation, 203 kHz receive bandwidth, sync word `0xCA54`, 21-byte packets. The station sends one burst every 20 seconds. The protocol comes from rtl_433's `vevor_7in1` decoder.
