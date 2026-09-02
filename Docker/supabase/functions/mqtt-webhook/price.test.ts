/**
 * Tests for the sale price conversion.
 *
 * Run: deno test Docker/supabase/functions/mqtt-webhook/price.test.ts
 */

import { assertEquals } from 'jsr:@std/assert'
import { centsToCurrency } from './price.ts'

Deno.test('centsToCurrency: whole cents become the currency amount', () => {
  assertEquals(centsToCurrency(0), 0)
  assertEquals(centsToCurrency(1), 0.01)
  assertEquals(centsToCurrency(100), 1)
  assertEquals(centsToCurrency(150), 1.5)
  assertEquals(centsToCurrency(205), 2.05)
  assertEquals(centsToCurrency(99999), 999.99)
})

Deno.test('centsToCurrency: no float noise on the stored amount', () => {
  // The old `cents * Math.pow(10, -2)` form stored 0.35 as
  // 0.35000000000000003 — right to the cent, but the noise showed up in
  // exports and API responses.
  assertEquals(centsToCurrency(35), 0.35)
  assertEquals(centsToCurrency(35).toString(), '0.35')
  assertEquals(centsToCurrency(57).toString(), '0.57')

  // Every price up to 100 EUR must round-trip back to the cents the device
  // sent, so reconciliation against a Nayax export can match on equality.
  for (let cents = 0; cents <= 10000; cents++) {
    assertEquals(Math.round(centsToCurrency(cents) * 100), cents)
  }
})
