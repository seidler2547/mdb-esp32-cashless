# Diagnosing the MDB bus

When a cashless reader "doesn't work" in a vending machine, the useful
question is almost never *is the firmware running* — it is *what is actually
happening on the four wires*. This firmware answers that itself: it watches
every byte that crosses the MDB link, keeps counters and a rolling byte
trace, and reduces the whole picture to a one-word **verdict**.

Everything here is passive. The reader does not probe, retry differently, or
change addresses on its own; it only reports.

## What the device can see

MDB is a four-wire bus with separate directions. The VMC drives
*master transmit*; peripherals answer on *master receive*. Our RX pin is
connected to master transmit only (see
`kicad/mdb-slave-esp32s3/mdb-slave-esp32s3.kicad_sch`), which has two
consequences worth remembering when reading a trace:

* **Every received byte came from the VMC.** The address map is therefore
  exact, not a guess — if the counters say the machine polls `0x60`, it does.
* **We cannot see other peripherals' replies.** A coin changer that has
  stopped answering is invisible to us. What *is* visible is the VMC's
  ACK / NAK / RET to *our* blocks, which is what tells us whether our own
  transmit path works at all.

## Start with the verdict

| Verdict | Meaning | What to do |
|---|---|---|
| `ok` | The VMC is addressing this reader and has enabled it. | The bus is fine; look at the counters for quality problems. |
| `polled_not_enabled` | The VMC polls us and we answer, but it has never sent READER ENABLE. | The link is healthy — the reader is switched off in the machine's own service menu. Until it is enabled the reader can hold no session, so it reports neither card sales **nor the VMC's cash sales**. |
| `reset_loop` | The VMC addresses us but never gets past RESET — it never issues a POLL. | Our replies are not reaching the VMC. Check the transmit side of the harness, and anything that delays the answer past MDB's 5 ms deadline. |
| `wrong_addr` | The VMC polls the *other* cashless address and never ours. | The machine is configured for the other cashless device. Switch this reader with config command `0x31` (or the `/machines/[id]` MDB address toggle). |
| `not_enabled` | The bus is busy, but no cashless address is polled at all. | The card reader was never enabled in the vending machine's own setup menu. Nothing on our side will help. |
| `bus_silent` | Traffic was seen, then stopped for over two seconds. | Machine powered down, harness came loose, or the receive opto-coupler failed. |
| `no_rx` | Nothing has ever arrived. | Check the MDB cable and that the machine is switched on. |

`wrong_addr` also fills in `hint` with the address the VMC does poll, and the
firmware logs a standing `MDB ADDR MISMATCH` line naming the fix.

## Where to read it

**Over MQTT** — the existing `mdb-log` heartbeat (every 5 minutes, plus on
every state change) now carries a nested `bus` object. The `mqtt-webhook`
edge function merges the whole payload into `embeddeds.mdb_diagnostics`, so
it lands in `mdb_diagnostics.bus` with no migration, and in `mdb_log.raw` on
state changes. Firmware that predates the object simply doesn't send it.

**Over the SoftAP** — three endpoints on the captive portal, which is what
you want when the machine has no uplink (an MDB fault and a dead uplink often
arrive together):

| Endpoint | Purpose |
|---|---|
| `GET /api/v1/mdb/diag` | Counters, address map and verdict as JSON. |
| `GET /api/v1/mdb/trace` | The byte trace as plain text, legend included. |
| `POST /api/v1/mdb/debug` | `{"level":0-3}` and/or `{"reset":true}`. |

The portal itself renders all of this in an **MDB bus** panel — the verdict
in plain language, the counters, the named address map, a level selector and
a trace viewer — so a phone joined to `VMflow-XXXXXX` is a complete
diagnostic tool. On cellular boards the SoftAP is always up; on WiFi boards
config command `0x36` brings it up on demand without disturbing the STA link.

**Over serial** — a summary line every 5 minutes, a warning whenever the bus
goes quiet, the address-mismatch line, and at debug level 3 a trace dump
every 10 seconds.

## The counters

| Key | Meaning |
|---|---|
| `verdict`, `hint` | See above. |
| `rx`, `tx` | 9-bit words received from the VMC / put on the wire by us. |
| `cmd`, `mine`, `other` | Address bytes seen in total, for us, for anyone else. |
| `blk`, `shortBlk` | VMC command blocks that reached their checksum / were cut short by the next address byte — i.e. **bytes were lost**. |
| `ack`, `nak`, `ret` | VMC handshakes to our data blocks. `nak`/`ret` mean it received ours corrupted. |
| `txBlk`, `txAck` | Data blocks we sent / bare ACK\* answers ("nothing to report"). |
| `frmErr` | Stop bit sampled low: baud mismatch, noise, or two devices talking at once. |
| `chkErr` | Checksum mismatch on a frame addressed to us. |
| `gapErr` | Idle gap over 1.5 ms *inside* a command block (MDB/ICP 4.2 §3.2 allows 1.0 ms). |
| `unk`, `drain` | Commands we don't implement / bus resynchronisations. |
| `sil`, `maxSilMs` | Times the bus went quiet for over a second, and the longest such gap. Real VMCs idle longer than you would guess — one measured in the field polls its changer every 226 ms and pauses up to 400 ms between scan cycles — so anything below a second is normal traffic, not a dropout. |
| `lastRxMs`, `lastMineMs` | Milliseconds since the last byte / the last command addressed to us. |
| `rspUs`, `rspMaxUs` | Delay between the VMC's command and the first bit of our answer. MDB allows 5000 µs. |
| `myCmd` | The commands addressed to *us*, split by type (`reset`, `setup`, `poll`, `vend`, `reader`, `exp`). A high `reset` with `poll` at zero is the `reset_loop` signature. |
| `addr` | Per-address command counts, keyed by address byte (`"08"`, `"10"`, `"30"` …). |
| `scale` | The price units we advertised in SETUP CONFIG_DATA: `sf` scale factor, `dp` decimal places. Every `raw` price below is in these units — `{"sf":1,"dp":2}` means cents. Constant for a given build, and the key to reading the numbers next to it. |
| `vendCmd` | VEND subcommands addressed to us: `req`, `cancel`, `succ`, `fail`, `done`, `cash`. Omitted until the first vend. **`cash` is the one to read when cash sales are missing** — CASH SALE is optional in MDB and many VMCs never send it, so `req > 0` with `cash` stuck at 0 means this machine does not report cash over the bus at all and the DEX audit is the only route. |
| `lastVend` | The most recent sale as it crossed the bus: `cmd` (`0x21` cash, `0x23` sniffed card, `0x24` cashless), `raw` (the price the VMC sent, in `scale` units), `cents` (what we published), `item`, `ageMs`, plus `n` / `rawMin` / `rawMax` over all sales since the counters were reset. |

Two more objects ride on the `mdb-log` heartbeat and `/api/v1/mdb/diag`
alongside `bus`:

| Key | Meaning |
|---|---|
| `saleQueue` | `lastSeq` is a monotonic lifetime count of sales the firmware has recorded — **zero means it has never seen one**, which places the fault upstream of MQTT entirely. `pending` climbing with `lastSeq` growing is the opposite: sales exist and are not being delivered. `overflow` counts sales that did not fit the 512-entry offline buffer. |
| `dex` | Audit health: `polls` attempted, `ok` that produced bytes, `bytes` in the last snapshot, `lastTryMs` / `lastOkMs` ages (`-1` = never). The first audit runs two minutes after the uplink comes up, then hourly. The audit is the only sales path that does not need the reader enabled, so on a machine reporting nothing it is the other half of the answer. `polls` climbing with `lastOkMs` at `-1` means the audit port was never wired up or does not answer. |

## Reading a trace

```
*10 10 /1 >*00 /4 *12 12 /1 >*03 >00 >64 >*67 /1 *00 /8 *0A 0A
```

| Notation | Meaning |
|---|---|
| `XX` | A byte, in hex. |
| `*XX` | Mode bit set: a VMC address byte, or `ACK 00` / `RET AA` / `NAK FF`. |
| `>XX` | Transmitted by this device. |
| `XX!` | Framing error — the stop bit sampled low. |
| `/N` | N milliseconds of **idle** before the next byte. Bytes sent back to back show no marker: the frame's own 1.15 ms transmission time is subtracted, so `/N` is comparable directly against the spec's 1.0 ms inter-byte limit. |

So the line above reads: RESET to cashless #1 (`*10` + checksum), our ACK, a
POLL, a four-byte answer from us, the VMC's ACK, then the changer being
polled at `0x08`.

Address bytes carry the peripheral in bits 7..3 and the command in bits 2..0
(MDB/ICP 4.2 §2.3), so `*12` is address `0x10` command `2` — a POLL to
cashless device #1.

### Snapshots

At level 2 and above the firmware freezes a window of the trace by itself,
so the interesting bytes survive being scrolled out of the ring. Two things
arm one:

- **A bus error** — framing, checksum, a lost block, a silence. Rate-limited
  to one a minute so a continuously failing bus does not publish a dump every
  few seconds.
- **A VEND REQUEST** — cause `vend`. This one ignores both the rate limit and
  any snapshot already in flight, because a vend is far rarer than a bus
  error and the price bytes are the whole reason anyone opens a trace after a
  disputed sale. At 256 entries the ring holds only seconds of idle polling;
  without this, a price queried even a minute later is long gone.

The frozen block carries the VEND REQUEST verbatim, which is what settles an
argument about a price:

```
*13 00 00 96 00 23 CC
 ^   ^   ^^^^^ ^^^^^
 |   |   price item
 |   VEND REQUEST
 VEND to 0x10
```

`00 96` is 150, and at `scale {"sf":1,"dp":2}` that is 1.50 — so the machine
really did send 1.50. If the machine's front label says something else, the
fault is in its price programming, not in the reader.

## Is the DEX port even wired?

`dex.polls` climbing with `lastOkMs` stuck at `-1` means the audit ran and
nothing answered. Before suspecting the machine, check that the board has a
DEX interface at all — the two revisions differ:

| Board | DEX connector | Interface |
|---|---|---|
| `kicad/mdb-slave-esp32s3` (WiFi) | **none** | none — the schematic has no `dex_*` net. GPIO8/9 appear only on the 2×11 expansion header `J4`, pins 4 and 6 (silkscreen `08` / `09`), as bare 3.3 V logic. |
| `kicad/mdb-slave-esp32s3-sim7080g` (cellular) | `J3`, 3-pin: GND / `dex_rx` / `dex_tx` | Q5, Q6 (MMBT3904) open-collector buffer with R20–R23 pull-ups to +3V3. No galvanic isolation. |

The UART is GPIO9 TX (to the machine), GPIO8 RX (from the machine), 9600 8N1,
DDCMP framing. Pin names on `J3` are from the board's point of view: `dex_rx`
is the line the board listens on.

On a WiFi board, "no DEX data" is the expected result, not a fault — there is
nothing to plug in to. Check the levels and connector of the specific
machine's audit port against its own manual and `EVA-DTS 6.1.1 NEW.pdf` in
the repo root before connecting anything: GPIO8 is a bare SoC pin on that
board, with no series resistor, no clamp and no isolation between it and
whatever the machine presents.

### Retrofitting the audit port onto a WiFi board

Use the **I2C connector `J8`** — GND, +3V3, GPIO10, GPIO11. It is the only
connector that carries two free signals, a ground and a logic rail, and the
firmware never touches GPIO10/11 (the only I2C in the codebase is the
cellular board's PMU, on GPIO15/7), so nothing is lost by taking it. Set
`DEX_RX_GPIO` to 10 and `DEX_TX_GPIO` to 11 in menuconfig; both are reported
back as `dex.rx` / `dex.tx` so a device can be checked against its wiring.

**Do not use the pulse connector `J3`.** Only its pin 3 is a signal, and that
is the collector of Q7 — an output-only open-collector transistor driven from
GPIO13 through a 4k7 base resistor, so there is no way to receive on it and
no second line for the other direction. Its pin 2 is `vin`: the **raw MDB
supply**, feeding an LM2594HV that tolerates up to 60 V. There is no 5 V rail
anywhere on the board — the only two are +3V3 and `vin` — so the high side of
any level shifter has to be powered from the machine or from a separate
supply, never from `J3`.

A level shifter alone is not automatically enough. It assumes the machine's
audit port is push-pull logic at the voltage you shift to. Audit ports are
also built as RS-232 (±12 V, inverted) and as opto-isolated current loops,
and a plain 3.3↔5 V shifter will be destroyed by the first and will not work
with the second. Meter the port before wiring. Note also that the MDB signal
lines are opto-isolated on this board (U1/U4, TLP785) while the board's
ground is MDB *power* ground — bonding a machine ground to it through the
audit port is a decision to make deliberately, not by accident.

## Debug levels

Set with config command `0x34` or `POST /api/v1/mdb/debug`; stored in NVS
(`vmflow`/`mdb_dbg`) so it survives the reboots that field debugging
involves. The compile-time default is level 2
(`CONFIG_MDB_DEBUG_DEFAULT_LEVEL`).

| Level | Adds |
|---|---|
| 0 | Counters, address map and verdict only. |
| 1 | A rolling ring of the last `CONFIG_MDB_DEBUG_TRACE_DEPTH` bus bytes (256 by default, ~0.25 s of continuous traffic, 2 KiB of RAM). |
| 2 | Automatic snapshots: a bus error freezes the trace around it — 32 bytes of context after the trigger — and the next heartbeat publishes it. Rate-limited to one per minute. |
| 3 | A trace dump to the serial console every 10 s. |

Nothing is logged or published from inside the bit-sampling critical section
at any level; the bus task only writes to memory.

## Remote commands

All four are XOR-encrypted 19-byte payloads on `/{company}/{device}/config`,
alongside the existing `0x30` restart, `0x31` MDB address and `0x32` MDB soft
reset.

| Command | Effect |
|---|---|
| `0x33` | Publish a diagnostics heartbeat and a trace dump now. |
| `0x34` | Set the debug level (parameter 0–3), persisted. |
| `0x35` | Reset the bus counters and the trace. |
| `0x36` | Bring the SoftAP up for on-site access. |

## Common signatures

| Symptom in the field | What the numbers look like | Cause |
|---|---|---|
| Reader never shows up, machine otherwise fine | `verdict=wrong_addr`, `mine=0`, `addr` contains `60` | Machine configured for cashless #2; switch the address. |
| Reader never shows up, bus clearly busy | `verdict=not_enabled`, no `10`/`60` in `addr` | Card reader not enabled in the VMC's setup menu. |
| Nothing happens at all | `verdict=no_rx`, `rx=0` | Harness, machine power, or the RX opto-coupler. |
| Machine keeps resetting the reader | `verdict=reset_loop`, `myCmd.reset` climbing, `myCmd.poll` = 0 | Our answers never reach the VMC. Transmit path, or an answer delayed past 5 ms — see the note on console logging below. |
| Machine resets the reader intermittently | `mine` climbing, `nak`/`ret` > 0, `rspMaxUs` near 5000 | Our answers are late or arrive corrupted. |
| We answer, the machine ignores us | `txBlk` > 0 but `ack` = 0 | Strongly suggests the transmit path is dead — the VMC never confirms a single block of ours. |
| Intermittent failures, occasional vends | `frmErr`, `chkErr`, `gapErr`, `shortBlk` climbing | Electrical: grounding, cable length, interference. Pull a trace and look for `!` and large `/N` gaps mid-block. |
| Worked, then stopped | `sil` > 0, `maxSilMs` large, `verdict=bus_silent` | The machine or the harness dropped out. The snapshot taken at that moment shows the last bytes before it went quiet. |
| No sales at all, neither card nor cash | `verdict=polled_not_enabled`, `myCmd.reader` = 0, `saleQueue.lastSeq` = 0 | The reader is not switched on in the machine's service menu. A VMC will not send CASH SALE to a reader it has not enabled either, which is why *both* kinds of sale vanish together. |
| No sales, and the reader is enabled | `verdict=ok`, `myCmd.vend` = 0, `saleQueue.lastSeq` = 0, `dex.lastOkMs` = -1 | Nothing has been sold, or nothing reports it: no VEND has ever been addressed to us and the audit port has never answered. Check the DEX harness before suspecting the MDB side. |
| Card sales arrive, cash sales never do | `vendCmd.req` > 0, `vendCmd.cash` = 0 | This VMC does not send CASH SALE — it is optional in MDB and plenty of machines omit it. Nothing on the bus side will change that; cash has to come from the DEX audit, so check `dex` instead. |
| Everything missing on a device that was just power-cycled | `dex.polls` = 0 | Expected for the first two minutes only. The boot audit is armed when the uplink comes up; before that fix, a periodic-only timer meant no audit at all for the first hour after every reboot. |
| Sales arrive but every price is the same | `lastVend.rawMin` == `rawMax` over many sales | The VMC really is sending one price. Either its selections are all programmed to that price, or `scale` is too coarse to express them: at `{"sf":100,"dp":2}` one unit **is** 1.00, so every shelf price rounds to a whole euro. Compare `raw` against the price on the front of the machine. |
| Prices land one cent low | `lastVend.raw` right, `cents` one less | Firmware from before the `mdb_price.h` fix. The old `pow()`-based conversion truncated: 2.05 was published as 2.04. Sixteen of the first 401 prices were affected. |

## Why nothing logs from the bus task

The console on this board is USB-Serial-JTAG, where a write blocks until the
USB host drains the endpoint — milliseconds with a terminal attached, longer
once the buffer backs up, and the project builds at `CONFIG_LOG_DEFAULT_LEVEL`
DEBUG. A log line between a VMC command and our answer therefore pushes that
answer past the 5 ms response deadline, and a log line anywhere else in the
loop makes us miss the command that follows. The result is a VMC that resets
the reader forever and never polls it — the `reset_loop` verdict above.

So the bus task never touches the console: it formats into a ring slot and
`mdb_log_task` does the writing, dropping lines rather than stalling the bus
(it reports how many when it does). The same reasoning is why the diagnostics
publish is deferred to an esp_timer one-shot that fires only after the answer
is on the wire. Anything added to `vTaskMdbEvent` must follow the same rule.

## Testing the analysis code

`mdb_debug.c` is plain C over a byte stream, so it is unit-tested on the host
without a board — see `mdb-slave-esp32s3/test/`:

```sh
mdb-slave-esp32s3/test/run.sh
```
