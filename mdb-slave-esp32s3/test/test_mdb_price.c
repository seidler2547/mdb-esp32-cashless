/* Host harness for mdb_price.h.
 *
 * The header is compiled once per scale-factor / decimal-place pair by
 * run.sh, which -D's the two CONFIG_ symbols, so a single build of this file
 * checks one configuration and the loop in run.sh covers the matrix.
 *
 * What is being pinned: the conversion is exact and round-trips. The old
 * pow()-based macros failed both - at the shipping configuration (scale
 * factor 1, two decimals, where the conversion is the identity) 16 of the
 * first 401 prices came back a cent short. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "mdb_price.h"

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* The reference the firmware promises the VMC in SETUP CONFIG_DATA:
 * value = raw * scale_factor * 10^-decimal_places, expressed in cents and
 * rounded half-up. Computed here in long double so it is independent of the
 * integer trickery under test. */
static uint32_t expected_cents(uint16_t raw)
{
    long double v = (long double) raw * CONFIG_MDB_SCALE_FACTOR;
    for (int i = CONFIG_MDB_DECIMAL_PLACES; i < 2; i++) v *= 10.0L;
    for (int i = 2; i < CONFIG_MDB_DECIMAL_PLACES; i++) v /= 10.0L;
    return (uint32_t) (v + 0.5L);
}

int main(void)
{
    printf("mdb_price: scale factor %d, %d decimal places\n",
           CONFIG_MDB_SCALE_FACTOR, CONFIG_MDB_DECIMAL_PLACES);

    /* 1. Every raw price the 16-bit field can carry converts exactly. */
    for (uint32_t raw = 0; raw <= 0xFFFF; raw++) {
        uint32_t got = mdb_price_to_cents((uint16_t) raw);
        uint32_t want = expected_cents((uint16_t) raw);
        if (got != want) {
            printf("  FAIL raw=%" PRIu32 " -> %" PRIu32 " cents, want %" PRIu32 "\n",
                   raw, got, want);
            if (++fails > 5) { printf("  (further mismatches suppressed)\n"); break; }
        }
    }

    /* 2. The prices the old macros got wrong. At scale factor 1 / 2 decimals
     *    these are cent-for-cent identities; the pow() chain returned one
     *    less for each. Kept as an explicit regression list because this is
     *    the bug that reached the field. */
    if (CONFIG_MDB_SCALE_FACTOR == 1 && CONFIG_MDB_DECIMAL_PLACES == 2) {
        static const uint16_t regressions[] = {
            29, 58, 59, 116, 117, 118, 119, 205, 207, 209, 211, 213,
            232, 234, 236, 238,
        };
        for (unsigned i = 0; i < sizeof(regressions) / sizeof(*regressions); i++) {
            uint16_t p = regressions[i];
            CHECK(mdb_price_to_cents(p) == p);
        }
    }

    /* 3. Round-trip raw -> cents -> raw. A credit of 1.50 EUR must not come
     *    back as 1.49 after a trip through the reader.
     *
     *    Exact whenever one advertised unit is a whole number of cents,
     *    which covers every configuration but one. At three decimal places
     *    the unit is a tenth of a cent and the sale payload — which carries
     *    cents — simply cannot express it, so the best the conversion can do
     *    is land on the nearest cent, half a unit away at worst. That is a
     *    property of the wire format, not of this arithmetic; see the note
     *    in mdb_price.h. */
    const uint32_t tolerance = (MDB_PRICE_DEN > 1) ? (uint32_t) (MDB_PRICE_DEN / 2) : 0;

    for (uint32_t raw = 0; raw <= 0xFFFF; raw++) {
        uint32_t cents = mdb_price_to_cents((uint16_t) raw);
        uint16_t back  = mdb_price_from_cents(cents);
        uint32_t err   = back > raw ? back - raw : raw - back;
        if (err > tolerance) {
            printf("  FAIL round-trip raw=%" PRIu32 " -> %" PRIu32 " cents -> %u"
                   " (off by %" PRIu32 ", tolerance %" PRIu32 ")\n",
                   raw, cents, back, err, tolerance);
            if (++fails > 5) { printf("  (further mismatches suppressed)\n"); break; }
        }
    }

    /* The shipping configuration, and every other whole-cent one, round-trips
     * with no tolerance at all. */
    if (CONFIG_MDB_DECIMAL_PLACES <= 2) CHECK(tolerance == 0);

    /* 4. A credit larger than the 16-bit field saturates instead of wrapping:
     *    a wrapped value would authorise a vend at the wrong amount. */
    CHECK(mdb_price_from_cents(0xFFFFFFFFu) == 0xFFFF);

    if (fails) { printf("FAILED (%d)\n", fails); return 1; }
    printf("PASSED\n");
    return 0;
}
