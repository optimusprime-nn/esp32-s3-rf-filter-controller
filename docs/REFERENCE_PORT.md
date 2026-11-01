# Reference architecture: ESP32-P4 GNSS/NTS server
Source branch: https://github.com/Optimusprimeums/esp32p4_nts_time_server/tree/9-Final-Board-Implementation

Inspected reference source files: main/acme_client.h, main/cloudflare_client.h, main/device_config.h, main/network_identity.c, main/web_console.c.

Reuse strategy:
1. Keep ACME implementation generic; split DNS-01 provider operations into prepare/cleanup hooks as in source acme_client.h.
2. Cloudflare scoped token must be protected, never included in GET responses, logs, public repos or URLs; limit to DNS Edit and Zone Read on one zone.
3. Use Let's Encrypt staging until full renewal/error handling is tested; production must be an explicit selection.
4. On challenge create: verify type, hostname and exact TXT content; on cleanup: delete only the challenge record whose ID/name/value match.
5. Track order, challenge, issuance, staging/production slots, and persist last-known-good server TLS keys. Roll back if activation fails.
6. ACME requires valid synchronized time, DNS resolution and internet connectivity; DNS-01 proves domain ownership but does NOT require the ESP32 to be publicly accessible.
7. FQDN and local DHCP hostname are separate: changing them requires checking SAN and certificate validity.
8. Never bind HTTP credential-writing or RF/RESET mutations until an authenticated HTTPS management boundary is verified.
9. This is an adaptation plan, not an assertion that the P4 code is already compiled for ESP32-S3. The full 100kB+ ACME implementation has external dependencies to port/test first.
