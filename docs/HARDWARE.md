# Hardware baseline (provisional)
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
