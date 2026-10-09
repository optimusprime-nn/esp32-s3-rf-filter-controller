# Proposed management web settings
Network: Wi-Fi SSID (secret write-only passphrase), DHCP/static radio selection, IPv4, netmask, gateway, DNS, hostname and FQDN. Apply/rollback timer to prevent lockout; the recovery AP requires physical provisioning authorization.

Time: External NTP server FQDN/IP, sync status, last synchronization and current UTC. Accurate time must be available before certificate validity checks.

TLS/ACME: Current certificate SAN, issuer, expiry, thumbprint and active slot; Let's Encrypt staging/production selection; Cloudflare zone, masked API-token status, DNS-01 lifecycle, issue/renew and audit outcomes. Cloudflare token accepted through authenticated HTTPS POST only and encrypted at rest.

Network and ACME settings must not interrupt RF switching or reset the Radxa. No unauthenticated state-changing endpoints. HTML dashboard not yet wired to these settings modules.
