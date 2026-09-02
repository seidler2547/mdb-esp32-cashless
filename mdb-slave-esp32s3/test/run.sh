#!/usr/bin/env sh
# Host-side tests for main/mdb_debug.c and main/mdb_price.h.
#
# Both are plain C with no hardware dependencies, so they can be compiled and
# exercised with the system gcc — no ESP-IDF, no board. `stub/` supplies the
# handful of IDF headers mdb_debug.c includes.
#
#   ./run.sh
set -e
cd "$(dirname "$0")"

echo "=== mdb_debug ==="
gcc -std=gnu11 -Wall -Wextra -Werror -O1 \
    -I stub -I ../main \
    test_mdb_debug.c ../main/mdb_debug.c \
    -o /tmp/test_mdb_debug
/tmp/test_mdb_debug

# The price conversion is compile-time specialised on the scale factor and
# decimal places, so every configuration menuconfig can produce gets its own
# build. Scale factor 100 with 0 decimals is the coarsest (1.00 units) and
# 1 with 3 decimals the finest; the shipping default is 1 / 2.
echo
echo "=== mdb_price ==="
for sf in 1 10 100; do
    for dp in 0 1 2 3; do
        gcc -std=gnu11 -Wall -Wextra -Werror -O1 \
            -I stub -I ../main \
            -DCONFIG_MDB_SCALE_FACTOR=$sf -DCONFIG_MDB_DECIMAL_PLACES=$dp \
            test_mdb_price.c -o /tmp/test_mdb_price
        /tmp/test_mdb_price
    done
done
