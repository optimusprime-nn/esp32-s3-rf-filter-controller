# Security gate
The firmware runs Wi-Fi station only; there is no AP, open or otherwise. A recovery AP is not implemented and must not ship open.

## Management access
Management follows esp32p4_nts_time_server. Until it is claimed, the controller serves only an information page and the claim call, and the claim needs the one-time code printed on its consoles. After the claim, the TLS handshake refuses any client without a certificate signed by the claimed management CA, so no request reaches the API without one. There are no passwords.

- Whoever can read the console can claim an unclaimed controller. Claim it before putting it on an untrusted network.
- The bootstrap certificate is self-signed. Clients must check its SHA-256 fingerprint against the console before trusting it, as the setup tool does, or a machine in the middle could take the claim.
- Anyone holding the management CA's private key can manage the controller. Keep it offline.
- A claim can only be undone by erasing the flash.

## Weather feed
The one exception to mTLS is the read-only weather feed on TCP 30090, for displays on the LAN. It sends the accepted station's readings and nothing else, and ignores anything a client sends, so it can't change the board. Anyone on the LAN can read the weather from it. Turn it off in `menuconfig` if that matters, and don't forward the port to the internet.

## SDR board resets
Over the network, rebooting or power-cycling the SDR board needs mTLS like every other call, plus a confirmation header. Each reset has a fixed length and a 10-second cooldown, so even a valid client can't hold the Radxa off or cycle it rapidly.

## USB console
The USB provisioning console has no login: physical access to the ESP32-S3 USB port is the authorisation. It can show the enrollment code, replace Wi-Fi credentials and the HTTPS certificate, and reboot or power-cycle the SDR board, but it can't release or change a claim. Anything wired to that port, including the Radxa, is fully trusted. The port also exposes USB-JTAG. Keep it inside the enclosure.

## Still to do
- NVS encryption and flash encryption. The bootstrap private key, any stored TLS key, the ACME account key, the Cloudflare API token and Wi-Fi credentials are in plain NVS, unlike on the reference, which keeps them encrypted. Anyone with the board and a USB cable can read them. Limit the Cloudflare token to Zone:Read and DNS:Edit on one zone.
- Secure boot.
- Signed OTA images. Updates and rollback work, and an upload must be an image for this project with a valid SHA-256, but anyone with a management client certificate can install firmware.
- Fail closed on missing configuration and power glitches.
