#!/bin/sh
set -eu
out=$(mktemp -d)
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_settings.c main/management_settings.c -o "$out/test_settings"
"$out/test_settings"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_provisioning.c main/provisioning.c main/console_line.c -o "$out/test_provisioning"
"$out/test_provisioning"
