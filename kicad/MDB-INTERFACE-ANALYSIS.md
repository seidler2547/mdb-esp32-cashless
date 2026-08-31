# MDB bus interface — electrical analysis

Traced from the `mdb-slave-esp32s3.kicad_pcb` netlist and checked against
MDB/ICP 4.2 §4.2 (Bus Transmitter / Receiver Specification), §4.4 (Example
Schematic) and the ESP32-S3 datasheet v1.8 §4.4 (DC Characteristics).

**Finding: the slave→VMC transmit path is under-driven by roughly 4×.**
The receive path is fine. The `-sim7080g` variant has an electrically
identical front end (`R16`/`R18`/`R17`, `U4`/`U5`) so it carries the same
defect.

## As built

| | Receive (VMC → board) | Transmit (board → VMC) |
|---|---|---|
| Optocoupler | `U4` TLP785 | `U1` TLP785 |
| LED loop | `mdb_master_transmit` → `R2` 470 Ω → LED → `mdb_communications_common` | `+3V3` → `R5` 470 Ω → LED → GPIO5 (sinks) |
| Transistor | collector → GPIO4, emitter → GND, `R10` 1 kΩ pull-up to `+3V3` | collector → `mdb_master_receive`, emitter → `mdb_communications_common` |
| LED current | ≈ 8.2 mA (6.1 mA at the spec's 4 V floor) | **≈ 4.5 mA** |
| Must sink | 3.3 mA into a 1 kΩ pull-up | **≥ 15 mA at ≤ 1 V** (§4.2) |
| CTR required | ≈ 40 % | **≈ 333 %** raw, ≈ 800 % to actually saturate |
| TLP785 offers | 50–200 % (rank unspecified in the schematic) | same |

Topology, polarity, connector pinout (§4.3), idle bus release (GPIO5 →
`GPIO_MODE_INPUT`, satisfies the 30 µA inactive-leakage limit) and the
bit-banged 104 µs timing are all correct. The `LM2594HVM-3.3` is the 60 V
variant, correct against the 45 V peak bus allowance.

## The arithmetic

    I_F = (3.3 V − V_F(LED) − V_OL(GPIO5)) / 470 Ω
        = (3.3 − 1.10 − 0.08) / 470  ≈  4.5 mA      (2.9 mA worst case)

At 4.5 mA a TLP785 passes 2.3 mA (50 % CTR) to 9.0 mA (200 % CTR) — 15 % to
60 % of the required 15 mA, and nowhere near saturation. Against a minimally
compliant VMC receive input (5 V, opto V_F 1.2 V, 187 Ω for 15 mA at 1 V) the
Master Receive line settles at **2.1–3.4 V** instead of ≤ 1 V.

Failure mode is therefore machine-dependent, not universal: a VMC whose own
receive optocoupler saturates at ~5 mA still works, a spec-compliant one does
not. It degrades further with temperature and with LED ageing, so a unit can
pass install-day testing and drift below threshold months later.

The same 470 Ω is correct on the receive side (it divides ~5 V from the VMC
and only has to move 3.3 mA) and wrong on the transmit side (it divides 3.3 V
and has to move 15 mA). The value looks carried over between the two loops.

## Fix

- **`R5`: 470 Ω → 100 Ω** → I_F ≈ 17 mA. Within the TLP785's 50 mA limit;
  29 mW in the existing 0603.
- **Firmware**: add `gpio_set_drive_capability(PIN_MDB_TX, GPIO_DRIVE_CAP_3);`
  — the datasheet's 28 mA I_OL figure is specified at `PAD_DRIVER = 3`, and
  nothing currently sets it.
- **BOM**: pin the CTR rank. `TLP785(GB)` (100–200 %) at minimum,
  `TLP785(BL)` (200–600 %) preferred. The schematic says only `TLP785`, so a
  50 % part is a legal substitution today. There is no BOM file in the repo.
- **Optional**: sink the LED through an MMBT3904 (already stocked as `Q7`)
  instead of directly into GPIO5, removing the dependence on drive-strength
  configuration. This inverts the drive sense, so `write_9()` and the idle
  handling in `write_payload_9()` need their polarity flipped.

Apply the same change to `R18`/`U5` on the `-sim7080g` board.

## Adjacent — verify against the real BOM

Not transmit faults, but a board that browns out cannot transmit. Neither can
be settled from the design files (no voltage ratings in the schematic, no BOM),
but the footprints imply parts that cannot survive their nets:

- **`C3`** — 100 µF in `CP_Elec_6.3x5.8`, on the raw MDB rail (34 V nominal,
  42.5 V max, 45 V peak). 100 µF above ~16 V does not exist in a 6.3 × 5.8 mm
  can.
- **`C2`** — 220 µF tantalum in `CP_EIA-3528-21`, on the 3.3 V rail. 220 µF in
  a 3528 B case is a 2.5 V part in every catalogue; 4 V needs a 7343 D case.
  Tantalums fail over-voltage as a short.
