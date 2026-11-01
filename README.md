# ESP32-S3 Dual RF Filter Controller

ESP-IDF firmware for two HMC7992 SP4T RF switches controlling four filter paths, web management, and Radxa Zero 3 reset.

Development is in progress. Hardware and security validation required before deployment.

## Build

Use ESP-IDF **v6.1** with the ESP32-S3 toolchain. From an activated ESP-IDF shell:

```sh
idf.py set-target esp32s3
idf.py build size
```

CI uses the official `espressif/idf:v6.1` Docker Hub image and builds pushes to
`main`/`feature/**` and all pull requests, including stacked PRs. Successful runs
upload firmware, ELF/map files, flash arguments and the generated configuration.
See [the migration notes](docs/ESP_IDF_6_1_MIGRATION.md) for scope and validation.
