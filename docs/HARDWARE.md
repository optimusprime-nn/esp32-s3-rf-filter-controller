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
