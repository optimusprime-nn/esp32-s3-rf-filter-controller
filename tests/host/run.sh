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
    -Imain tests/host/test_fan.c main/fan_math.c -o "$out/test_fan"
"$out/test_fan"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain tests/host/test_temp.c main/temp_math.c -o "$out/test_temp"
"$out/test_temp"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain -Itests/host/stubs tests/host/test_der.c main/der_util.c -o "$out/test_der"
"$out/test_der"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain tests/host/test_weather.c main/weather_decode.c main/cc1101_config.c -lm -o "$out/test_weather"
"$out/test_weather"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -Imain tests/host/test_weather_history.c main/weather_history.c -o "$out/test_weather_history"
"$out/test_weather_history"
