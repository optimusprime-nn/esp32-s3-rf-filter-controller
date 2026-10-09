# Radxa Zero 3 reset design gate
Radxa reset must use a confirmed documented reset/power interface. GPIO6 is a provisional control signal only, NOT a direct connection to the Radxa.

Recommended interface: normally inactive transistor/open-drain or opto-isolated driver with external pull-up referenced to the Radxa's documented reset voltage, plus power-off isolation. Ensure MCU boot/reset cannot assert reset.

A web reset action must require authentication, confirmation, cooldown, and a bounded pulse duration. A graceful SSH/API shutdown is preferred before hardware reset where feasible. Hardware reset and power-cycle designs are alternatives, not interchangeable.
