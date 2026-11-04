# Security gate
The firmware runs Wi-Fi station only; there is no AP, open or otherwise. A recovery AP is not implemented and must not ship open. Do not connect the RF source or Radxa reset driver while this firmware is deployed to an untrusted network.

## Management access
Management follows esp32p4_nts_time_server. Until it is claimed, the controller serves only an information page and the claim call, and the claim needs the one-time code printed on its consoles. After the claim, the TLS handshake refuses any client without a certificate signed by the claimed management CA, so no request reaches the API without one. There are no passwords.

- Whoever can read the console can claim an unclaimed controller. Claim it before putting it on an untrusted network.
- The bootstrap certificate is self-signed. Clients must check its SHA-256 fingerprint against the console before trusting it, as the setup tool does, or a machine in the middle could take the claim.
- Anyone holding the management CA's private key can manage the controller. Keep it offline.
- A claim can only be undone by erasing the flash.

## USB console
The USB provisioning console has no login: physical access to the ESP32-S3 USB port is the authorisation. It can show the enrollment code and replace Wi-Fi credentials and the HTTPS certificate, but it can't release or change a claim. Anything wired to that port, including the Radxa, is fully trusted. The port also exposes USB-JTAG. Keep it inside the enclosure.

## Still to do
- NVS encryption and flash encryption. The bootstrap private key, any stored TLS key and Wi-Fi credentials are in plain NVS, unlike on the reference, which keeps them encrypted.
- Secure boot.
- OTA updates with signature checking and rollback.
- Reject unauthenticated Radxa reset requests once a reset driver exists.
- Fail closed on missing configuration and power glitches.
