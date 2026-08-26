#!/usr/bin/env sh
# Host-side tests for main/mdb_debug.c.
#
# The bus observability module is plain C with no hardware dependencies, so
# it can be compiled and exercised with the system gcc — no ESP-IDF, no
# board. `stub/` supplies the handful of IDF headers it includes.
#
#   ./run.sh
set -e
cd "$(dirname "$0")"
gcc -std=gnu11 -Wall -Wextra -Werror -O1 \
    -I stub -I ../main \
    test_mdb_debug.c ../main/mdb_debug.c \
    -o /tmp/test_mdb_debug
exec /tmp/test_mdb_debug
