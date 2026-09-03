/*
 * VMflow.xyz
 *
 * mdb_debug.h - MDB bus observability
 *
 * Passive instrumentation for the bit-banged MDB link. Every 9-bit word
 * that crosses the bus (received or transmitted) is handed to this module,
 * which maintains cheap counters, a per-address command map and a rolling
 * byte trace, and derives a one-word verdict about why the reader is (or
 * is not) talking to the VMC.
 *
 * Wiring context that makes the numbers interpretable:
 *
 *   MDB is a 4-wire bus with *separate* directions - the VMC drives
 *   `mdb_master_transmit`, peripherals answer on `mdb_master_receive`
 *   (see kicad/mdb-slave-esp32s3.kicad_sch). PIN_MDB_RX is connected to
 *   the master-transmit line only, so:
 *
 *     - every byte we receive was sent by the VMC (never by another
 *       peripheral), which makes the address map exact rather than a guess;
 *     - we cannot see other peripherals' answers, so a missing reply from
 *       another device is invisible to us - but ACK/NAK/RET *to us* is
 *       visible, and that is what tells us whether our own transmit path
 *       works at all.
 *
 * Threading: all the note/rx/tx entry points are called from the MDB task
 * only. Readers (MQTT diag timer, HTTP handlers) read the counters and the
 * trace without locking - a torn read costs a wrong digit in a debug
 * report, which is cheaper than adding a lock to the bit-bang hot path.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- debug levels ----------
 *
 * Runtime-settable (MQTT config cmd 0x34 / POST /api/v1/mdb/debug) and
 * persisted in NVS key `mdb_dbg`, so a device left in a verbose mode keeps
 * it across the reboots that field debugging tends to involve.
 */
typedef enum {
    MDB_DBG_COUNTERS = 0,  /* counters + address map + verdict only        */
    MDB_DBG_TRACE    = 1,  /* + rolling byte trace ring                    */
    MDB_DBG_SNAPSHOT = 2,  /* + automatic trace snapshot around bus errors */
    MDB_DBG_VERBOSE  = 3,  /* + periodic trace dump to the serial console  */
} mdb_debug_level_t;

/* Flag bits stored alongside the 9-bit word in a trace entry. */
#define MDB_TRACE_TX       (1u << 9)   /* we transmitted this byte        */
#define MDB_TRACE_FRAMING  (1u << 10)  /* stop bit was not high on RX     */

/* Verdict strings returned by mdb_debug_verdict(). */
#define MDB_VERDICT_OK          "ok"           /* VMC is addressing us          */
#define MDB_VERDICT_RESET_LOOP  "reset_loop"   /* addressed, never polled       */
#define MDB_VERDICT_NOT_READY   "polled_not_enabled" /* polled, never enabled   */
#define MDB_VERDICT_WRONG_ADDR  "wrong_addr"   /* other cashless address polled */
#define MDB_VERDICT_NOT_ENABLED "not_enabled"  /* bus busy, no cashless polled  */
#define MDB_VERDICT_BUS_SILENT  "bus_silent"   /* traffic seen, then nothing    */
#define MDB_VERDICT_NO_RX       "no_rx"        /* nothing ever received         */

/* ---------- lifecycle ---------- */

/* own_addr / other_addr are full address bytes (0x10, 0x60), not selectors. */
void mdb_debug_init(uint8_t own_addr, uint8_t other_addr);

void    mdb_debug_set_level(uint8_t level);
uint8_t mdb_debug_get_level(void);

/* Sets the level, optionally persisting it to NVS (`vmflow`/`mdb_dbg`), and
 * starts the verbose serial dumper the first time level 3 is asked for.
 * This is the entry point runtime callers (MQTT config command, captive
 * portal) should use; mdb_debug_set_level only flips the variable. */
void    mdb_debug_apply_level(uint8_t level, bool persist);

/* Clears every counter, the address map and the trace. */
void mdb_debug_reset(void);

/* ---------- hot path (MDB task only) ---------- */

/* One received 9-bit word. `stop_ok` is false when the stop bit sampled
 * low, i.e. a framing error: wrong baud rate, noise, or a collision. */
void mdb_debug_rx_byte(uint16_t word, bool stop_ok);

/* The block we just put on the wire: `len` payload bytes plus the trailing
 * CHK* byte. `t_start_us` is esp_timer_get_time() taken immediately before
 * the first start bit, and is used to derive the VMC-command-to-response
 * latency (MDB allows 5 ms; anything close to that is a problem). */
void mdb_debug_tx_block(const uint8_t *payload, uint8_t len, uint8_t chk, int64_t t_start_us);

/* The start-bit wait timed out - the bus has gone quiet for `us`. */
void mdb_debug_note_silence(uint32_t us);

/* Protocol-level annotations. */
void mdb_debug_note_chk_err(void);      /* checksum mismatch on a frame for us */
void mdb_debug_note_unknown_cmd(void);  /* command we don't implement          */
void mdb_debug_note_drain(void);        /* mdb_drain_bus() resynchronisation   */

/* READER ENABLE accepted. Until this has happened at least once the VMC is
 * polling a reader it has not switched on, which is a different fault from
 * "the VMC is not talking to us" and needs a different fix - see
 * MDB_VERDICT_NOT_READY. */
void mdb_debug_note_reader_enabled(void);

/* A sale as it crossed the bus, recorded where the firmware hands it to the
 * sale queue.
 *
 * `raw` is the 16-bit price exactly as the VMC sent it, in the scale-factor
 * units *we* advertised in SETUP CONFIG_DATA; `cents` is what we publish
 * after conversion. Keeping both, plus the running min/max of `raw`, is what
 * separates the three ways a price can come out wrong: the VMC really is
 * sending that number (raw varies and matches the shelf price), we mangle it
 * in conversion (raw is right, cents is not), or the units are too coarse to
 * express the shelf price at all (raw is a small constant). None of that is
 * recoverable after the fact from the sales table alone. */
void mdb_debug_note_vend(uint8_t cmd, uint16_t raw, uint16_t item, uint32_t cents);

/* One VEND subcommand addressed to us, by its subcommand byte (MDB/ICP 4.2
 * section 7.4: 0x00 REQUEST, 0x01 CANCEL, 0x02 SUCCESS, 0x03 FAILURE,
 * 0x04 SESSION COMPLETE, 0x05 CASH SALE).
 *
 * CASH SALE is the VMC's optional report of a coin/note purchase to the
 * cashless device, and plenty of VMCs simply never send it. A `cash` count
 * still at zero after a day of cash trade says so outright, which is the
 * difference between "our cash handling is broken" and "this machine does
 * not report cash over MDB, so the DEX audit is the only route".
 *
 * VEND REQUEST additionally freezes a trace snapshot, so the price bytes are
 * kept exactly as they arrived. A vend is far rarer than a bus error and the
 * ring holds only seconds of idle polling, so it takes the snapshot slot
 * unconditionally - by the time anyone looks at a disputed price, the bytes
 * would otherwise be long gone. */
void mdb_debug_note_vend_cmd(uint8_t sub);

/* ---------- reporting (any task) ---------- */

/* Micro-verdict for the current bus situation - see MDB_VERDICT_*. */
const char *mdb_debug_verdict(void);

/* Address byte the VMC seems to expect, or 0 when there is nothing to
 * suggest (either we are being polled, or no cashless address is polled). */
uint8_t mdb_debug_addr_hint(void);

/* Renders the counter block as a self-contained JSON object (including the
 * braces) into `out`. Returns the number of characters written, 0 on
 * failure. Typically ~700 bytes; the worst case — every address slot
 * populated with saturated counters, plus `scale` and `lastVend` — is 1309,
 * so give it 1408. Rendering is bounded and stops rather than truncating
 * mid-token, but a clipped object is still invalid JSON downstream. */
size_t mdb_debug_json(char *out, size_t cap);

/* Renders up to `max_entries` of the most recent trace bytes, oldest
 * first, in the compact wire notation:
 *
 *   *10 03 13 ...   byte in hex; '*' = mode bit set (address/ACK/NAK/RET)
 *   >00 >*1F        '>' = transmitted by us
 *   0A!             '!' = framing error (stop bit low)
 *   /12             gap of 12 ms before the next byte (>= 1 ms only)
 *
 * Stops early rather than overflowing `cap`. Returns characters written. */
size_t mdb_debug_trace_render(char *out, size_t cap, uint16_t max_entries);

/* Number of entries currently held in the rolling trace. */
uint16_t mdb_debug_trace_count(void);

/* ---------- error snapshots ---------- */

/* True when a bus error armed a snapshot and the post-trigger window has
 * closed, i.e. there is a frozen trace waiting to be reported. */
bool   mdb_debug_snapshot_ready(void);
size_t mdb_debug_snapshot_render(char *out, size_t cap);
void   mdb_debug_snapshot_clear(void);
/* What tripped the snapshot ("chk", "framing", "gap", "silence"). */
const char *mdb_debug_snapshot_cause(void);

/* ---------- provided by the MDB task (mdb-slave-esp32s3.c) ----------
 *
 * The protocol state proper belongs to the state machine, not here. These
 * accessors let the captive-portal debug endpoints render exactly what the
 * MQTT heartbeat publishes without reaching into the bus task's globals. */
uint8_t     mdb_configured_address(void);
const char *mdb_state_name(void);
const char *mdb_last_command(void);
uint32_t    mdb_poll_total(void);
uint32_t    mdb_checksum_error_total(void);
uint8_t     mdb_vmc_level(void);

/* DEX audit health, for the same two surfaces. `try_ms`/`ok_ms` come back
 * negative when the thing has never happened — which on a machine whose
 * audit port was never wired up is the whole answer. */
void        mdb_dex_stats(uint32_t *polls, uint32_t *ok, uint32_t *bytes,
                          long *try_ms, long *ok_ms);

/* Clears the bus counters and the protocol tallies together, so a "reset
 * counters" action leaves no half-old numbers behind. */
void        mdb_reset_counters(void);

#ifdef __cplusplus
}
#endif
