# Security gate
The firmware runs Wi-Fi station only; there is no AP, open or otherwise. A recovery AP is not implemented and must not ship open. Do not connect the RF source or Radxa reset driver while this firmware is deployed to an untrusted network.

Before enabling remote write endpoints:
- Provision a unique device password through an authenticated local setup channel.
- Use HTTPS and secure session handling, CSRF protection, and rate limiting.
- Disable or secure recovery AP after provisioning, or use a time-limited physical provisioning trigger.
- Reject unauthenticated filter switching, restart, and Radxa reset requests.
- Never expose unauthenticated OTA or store secrets in source control.
- Configure NVS encryption and ESP32 secure boot/flash encryption where supported.
- Fail closed on missing configuration and power glitches.
