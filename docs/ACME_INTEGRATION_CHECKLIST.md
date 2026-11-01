# Certificate/Cloudflare DNS-01 integration acceptance

Source reference: ESP32-P4 NTP/NTS time server, branch 9-Final-Board-Implementation.

## Requirements
- [ ] HTTPS management with authenticated roles, anti-CSRF, request rate limits, secure cookies and no secret-bearing GET responses.
- [ ] Unique device identity and provisioning; fail-safe recovery with physical presence; Wi-Fi setup must never use persistent open AP.
- [ ] Separate DHCP client hostname (single DNS label) and certificate FQDN (fully qualified DNS name), verify SAN before activation.
- [ ] Obtain authoritative time via configured external SNTP server and report sync age/state before any ACME request or certificate date checks.
- [ ] Cloudflare API token minimal zone-scoped permissions (Zone:Read and DNS:Edit), stored encrypted at rest; never exposed on status API or in logs.
- [ ] Cloudflare DNS-01 TXT create, exact-value readback, propagation check, trigger, validate, and ownership-aware cleanup on every transaction outcome.
- [ ] Separate Let's Encrypt staging and production accounts, account-key persistence, throttling, retry/cooldown and explicit promotion.
- [ ] Save production certificate/key with atomic dual-slot last-known-good strategy and independent chain, key pair, SAN and date validation.
- [ ] Keep existing valid listener during renewal; health-check replacement before activation and roll back if replacement fails.
- [ ] Define policy for device without external Internet; allow manually provisioned certificates without making RF switching inaccessible.
- [ ] Verify ESP32-S3 heap/flash capacity and port compatibility before importing substantial source modules.
- [ ] Test network connectivity, DNS failures, power losses during issuance, TXT cleanup and reboot during TLS transition.

These are specification/verification gates, not claims of implemented behavior.
