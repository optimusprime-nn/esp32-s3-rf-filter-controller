# Dual four-wire PWM fan control

Two independent 4-wire PWM fan headers are planned: FAN1 and FAN2. Firmware now provides 25 kHz LEDC PWM on provisional ESP32-S3 GPIO15 and GPIO16, with separate duty settings. Both initialize at 100% cooling. Duty changes are currently internal API only; no remote unauthenticated controls.

## Hardware requirements (not yet implemented in KiCad)
- Each fan needs its own rated 12 V supply and ground, PWM control input, and tachometer output. Never power a fan from an ESP32 GPIO.
- PWM pin must be driven through a suitable open-collector/open-drain sink transistor, with fan-side pull-up appropriate to the actual fan specification; no ESP32 push-pull 5 V interface. The LEDC output is inverted to compensate for the inverting sink stage. Confirm the transistor gate/base and reset pull states so a reboot or unpowered MCU leaves PWM high / fans at full speed.
- Add two independent tachometer sense circuits, suitable pull-ups and 3.3 V-safe level conditioning. Tach capture/RPM calculation and stall alarms are NOT implemented yet.
- Confirm actual fan part numbers, operating voltage, startup minimum duty, PWM polarity and electrical limits before PCB capture.
- Cooling failure must not permit unsafe RF operation. Thermal sensing, tach watchdog, fan stall handling and RF inhibit integration remain to be implemented.
- GPIO15/16 are provisional; verify against selected module, bootstraps, and other board functions.
- Current firmware rejects duty below 20%, but this is not a substitute for real thermal protection.

## Software
`rf_fans_init()` initializes both channels at 100%. `rf_fan_set_duty(0, percent)` and `rf_fan_set_duty(1, percent)` allow 20..100%. No persistent duty setting or web write endpoint exists. The outputs are independently controllable, while sharing a single 25 kHz timer.
