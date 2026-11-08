# SDR board (Radxa Zero 3) resets
The controller has a soft reset, which asks Linux to reboot cleanly, on GPIO4, and a hard reset, a relay that cuts the Radxa's power, on GPIO5. The wiring and Radxa-side setup are in [HARDWARE.md](HARDWARE.md#sdr-board-resets), and the API in [API.md](API.md#sdr-board).

Design rules the firmware follows:
- Both lines are idle while the controller is off, resetting or booting: GPIO4 is open drain and released, and the relay's normally-closed contact keeps the Radxa powered while GPIO5 is low.
- GPIO4 never drives high, so it can't back-power the Radxa while its power is cut.
- Every reset needs mTLS and a confirmation header over the API, has a fixed length, and is followed by a 10-second cooldown.
- Try the soft reset first. Use the power cycle when the Radxa doesn't respond, since cutting power can corrupt its storage if it's writing.
