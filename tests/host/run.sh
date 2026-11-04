#!/bin/sh
set -eu
out=$(mktemp -d)
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_settings.c main/management_settings.c -o "$out/test_settings"
"$out/test_settings"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_provisioning.c main/provisioning.c main/console_line.c -o "$out/test_provisioning"
"$out/test_provisioning"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_session.c main/session.c -o "$out/test_session"
"$out/test_session"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain tests/host/test_fan.c main/fan_math.c -o "$out/test_fan"
"$out/test_fan"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain tests/host/test_temp.c main/temp_math.c -o "$out/test_temp"
"$out/test_temp"
