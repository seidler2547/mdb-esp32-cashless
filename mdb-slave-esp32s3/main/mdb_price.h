/*
 * VMflow.xyz
 *
 * mdb_price.h - exact conversion between MDB price units and cents
 *
 * MDB carries prices as a 16-bit count of "scale factor" units, and the
 * *reader* dictates the unit: the scale factor and decimal-place bytes we
 * put in our SETUP CONFIG_DATA answer (bytes 4 and 5) tell the VMC how to
 * encode every price it later sends us. So
 *
 *     value_in_currency = raw * SCALE_FACTOR * 10^-DECIMAL_PLACES
 *
 * and the wire payload we publish carries cents, i.e. scale factor 1 with
 * two decimal places, which is what mqtt-webhook decodes.
 *
 * This used to be done with
 *
 *     #define TO_SCALE_FACTOR(p, s, d)   (p / s / pow(10, -(d)))
 *     #define FROM_SCALE_FACTOR(p, s, d) (p * s * pow(10, -(d)))
 *
 * chained together and truncated into an integer. pow(10, -2) is the double
 * nearest 0.01, which is slightly *above* it, so dividing by it lands just
 * below the whole number and the truncation drops a cent: 2.05 EUR arrived
 * as 205 raw units and was published as 204. At the shipping configuration
 * (scale factor 1, two decimals - a conversion that is mathematically the
 * identity) 16 of the first 401 prices came out a cent short, among them
 * 0.29, 1.16..1.19 and 2.05. Nothing about the fault is visible in a log:
 * the sale simply lands in the database one cent light.
 *
 * The ratio is fixed at compile time and is always a power of ten times the
 * scale factor, so the exact form is a rational multiply. Every conversion
 * rounds half-up rather than truncating, and the intermediates are 64-bit
 * because raw * NUM reaches 6.5e8 and cents * DEN reaches 6.5e9.
 *
 * One configuration is lossy no matter how the arithmetic is done: at three
 * decimal places an MDB unit is a tenth of a cent, and the sale payload
 * carries whole cents, so a price can only be reported to the nearest cent.
 * Carrying it faithfully would mean changing the payload, mqtt-webhook and
 * the sales column together. Configurations of two decimal places or fewer -
 * which includes the shipping default - are exact in both directions.
 */

#pragma once

#include <stdint.h>

#ifndef CONFIG_MDB_SCALE_FACTOR
#define CONFIG_MDB_SCALE_FACTOR 1
#endif
#ifndef CONFIG_MDB_DECIMAL_PLACES
#define CONFIG_MDB_DECIMAL_PLACES 2
#endif

/* cents = raw * MDB_PRICE_NUM / MDB_PRICE_DEN, exactly. */
#if CONFIG_MDB_DECIMAL_PLACES == 0
#define MDB_PRICE_NUM  ((uint64_t) CONFIG_MDB_SCALE_FACTOR * 100)
#define MDB_PRICE_DEN  ((uint64_t) 1)
#elif CONFIG_MDB_DECIMAL_PLACES == 1
#define MDB_PRICE_NUM  ((uint64_t) CONFIG_MDB_SCALE_FACTOR * 10)
#define MDB_PRICE_DEN  ((uint64_t) 1)
#elif CONFIG_MDB_DECIMAL_PLACES == 2
#define MDB_PRICE_NUM  ((uint64_t) CONFIG_MDB_SCALE_FACTOR)
#define MDB_PRICE_DEN  ((uint64_t) 1)
#else   /* 3 decimal places: the unit is finer than a cent */
#define MDB_PRICE_NUM  ((uint64_t) CONFIG_MDB_SCALE_FACTOR)
#define MDB_PRICE_DEN  ((uint64_t) 10)
#endif

/* Raw MDB price units -> cents, for the sale we publish. */
static inline uint32_t mdb_price_to_cents(uint16_t raw)
{
    uint64_t v = (uint64_t) raw * MDB_PRICE_NUM;
    return (uint32_t) ((v + MDB_PRICE_DEN / 2) / MDB_PRICE_DEN);
}

/* Cents -> raw MDB price units, for credit arriving from the backend.
 * Saturates rather than wrapping: a 16-bit field cannot carry more, and a
 * wrapped credit would silently authorise the wrong amount. */
static inline uint16_t mdb_price_from_cents(uint32_t cents)
{
    uint64_t v = (uint64_t) cents * MDB_PRICE_DEN;
    uint64_t raw = (v + MDB_PRICE_NUM / 2) / MDB_PRICE_NUM;
    return raw > 0xFFFF ? 0xFFFF : (uint16_t) raw;
}
