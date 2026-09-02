/**
 * Sale price conversion for the MQTT ingest path.
 *
 * The firmware publishes prices as whole cents (MDB scale factor 1, two
 * decimal places), and `sales.item_price` stores the currency amount, not
 * cents. That is the entire conversion.
 *
 * It used to be written `cents * Math.pow(10, -2)`, multiplying by the double
 * just above 1/100, which lands a digit of noise in the stored value for
 * about one price in seven: 0.35 went into the row as 0.35000000000000003 and
 * came back out that way in exports and API responses. It still round-tripped
 * to the right number of cents, so no money was lost — this is tidiness, not
 * a repair. Dividing an integer by 100 gives the nearest double to the true
 * value, which is as close as a float8 column gets.
 *
 * Pure functions only — no DB, no I/O — so they stay unit-testable.
 */

/** Whole cents from the device -> the amount stored in sales.item_price. */
export function centsToCurrency(cents: number): number {
  return Math.round(cents) / 100;
}
