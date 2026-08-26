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
| `ok` | The VMC is addressing this reader. | The bus is fine; look at the counters for quality problems. |
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
| `gapErr` | Inter-byte gap over 2 ms *inside* a command block (MDB allows 1 ms). |
| `unk`, `drain` | Commands we don't implement / bus resynchronisations. |
| `sil`, `maxSilMs` | Times the bus went quiet, and the longest such gap. |
| `lastRxMs`, `lastMineMs` | Milliseconds since the last byte / the last command addressed to us. |
| `rspUs`, `rspMaxUs` | Delay between the VMC's command and the first bit of our answer. MDB allows 5000 µs. |
| `addr` | Per-address command counts, keyed by address byte (`"08"`, `"10"`, `"30"` …). |

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
| `/N` | A gap of N milliseconds before the next byte. |

So the line above reads: RESET to cashless #1 (`*10` + checksum), our ACK, a
POLL, a four-byte answer from us, the VMC's ACK, then the changer being
polled at `0x08`.

Address bytes carry the peripheral in bits 7..3 and the command in bits 2..0
(MDB/ICP 4.2 §2.3), so `*12` is address `0x10` command `2` — a POLL to
cashless device #1.

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
| Machine keeps resetting the reader | `mine` climbing, `nak`/`ret` > 0, `rspMaxUs` near 5000 | Our answers are late or arrive corrupted. |
| We answer, the machine ignores us | `txBlk` > 0 but `ack` = 0 | Strongly suggests the transmit path is dead — the VMC never confirms a single block of ours. |
| Intermittent failures, occasional vends | `frmErr`, `chkErr`, `gapErr`, `shortBlk` climbing | Electrical: grounding, cable length, interference. Pull a trace and look for `!` and large `/N` gaps mid-block. |
| Worked, then stopped | `sil` > 0, `maxSilMs` large, `verdict=bus_silent` | The machine or the harness dropped out. The snapshot taken at that moment shows the last bytes before it went quiet. |

## Testing the analysis code

`mdb_debug.c` is plain C over a byte stream, so it is unit-tested on the host
without a board — see `mdb-slave-esp32s3/test/`:

```sh
mdb-slave-esp32s3/test/run.sh
```
