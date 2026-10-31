# ESP-IDF 6.1 migration

This migration is stacked on PR #3 (`feature/network-time-acme-settings`) at
`19ba93882ada1ed163b150494b8269ab07fdecfa`, which includes PRs #1 and #2.
Those draft branches retain their original history; review this migration after
the preceding PRs, or include it when consolidating the stack. No PR is merged
by this change.

## Build and dependencies

- CI uses the official Docker Hub image `espressif/idf:v6.1`, replacing the
  inaccessible `ghcr.io/espressif/idf:v5.4.2` image (old run 39016306144 failed
  before compilation with a registry `denied` response).
- The component manifest accepts ESP-IDF 6.1.x only; CI pins the v6.1 release.
- CMake now requires 3.22, matching the ESP-IDF 6.1 project template.
- GPIO explicitly depends on `esp_driver_gpio` rather than the deprecated
  umbrella `driver` component. No external managed libraries are required.
- Application code already includes FreeRTOS headers directly and uses
  `esp_sntp.h`/`esp_sntp_*`; removed legacy SNTP headers/APIs are not used.
- HTTPS uses the public `esp_https_server` API; there is no direct Mbed TLS API
  usage or precompiled third-party library to migrate.
- CI handles stacked PR bases and exports build artifacts plus `sdkconfig`.

## Validation and limits

Run `idf.py set-target esp32s3` followed by `idf.py build size` in a v6.1 shell.
CI repeats the firmware build in a clean official v6.1 container. Build success
does not validate Wi-Fi reconnection, NVS persistence, SNTP or provisioned TLS
on hardware. GPIO mapping, reset electrical behavior and RF inhibit still need
bench validation. This migration does not enable RF output or management writes.

## Upstream references

- [ESP-IDF v6.1 release](https://github.com/espressif/esp-idf/releases/tag/v6.1)
- [6.0 migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/index.html)
- [6.1 migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.1/index.html)
- [Official Docker image](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/tools/idf-docker-image.html)
