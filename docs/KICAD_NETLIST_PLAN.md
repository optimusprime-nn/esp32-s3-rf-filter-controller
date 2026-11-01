# KiCad schematic capture plan
Sheets: (1) ESP32-S3 module, USB-C power/input and regulated 3.3 V, (2) dual HMC7992 module control headers and ESD/power filtering, (3) fail-safe RF mute and Radxa reset interface, (4) service/programming/test points.

Interface nets: RF_A, RF_B, RF_MUTE, RADXA_RESET_N, 3V3, GND, USB_D+, USB_D-, EN, BOOT.
The two RF modules are external and connect via control headers; SMA RF filter interconnects remain coaxial. No KiCad schematic has yet been captured or ERC validated. Confirm chosen ESP32-S3 module and physical RF header pinout before PCB-ready work.
