/*
 * VMflow.xyz
 *
 * mdb_debug.c - MDB bus observability (see mdb_debug.h for the rationale)
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include "mdb_debug.h"
#include "mdb_price.h"   /* CONFIG_MDB_SCALE_FACTOR / _DECIMAL_PLACES fallbacks */

#define TAG "mdb_dbg"

/* MDB address bytes carry the peripheral address in bits 7..3 and the
 * command in bits 2..0 (MDB/ICP 4.2 section 2.3). */
#define ADDR_MASK   0xF8
#define ADDR_SLOTS  32          /* 0x00, 0x08 ... 0xF8 */

/* VMC handshake bytes. All three arrive with the mode bit set, which is
 * how they are told apart from the peripheral data they acknowledge. */
/* Command bits of an address byte (MDB/ICP 4.2 section 7). */
#define MDB_CMD_RESET   0
#define MDB_CMD_POLL    2

#define WORD_ACK    0x100
#define WORD_RET    0x1AA
#define WORD_NAK    0x1FF

/* One 11-bit MDB frame at 9600 baud (MDB/ICP 4.2 section 2.1).
 *
 * A byte's timestamp is taken when the byte finishes, so the difference
 * between two consecutive timestamps is the second byte's own transmission
 * time plus whatever idle sat between them. Subtracting a frame turns that
 * period into the idle gap the spec actually bounds — without it, two bytes
 * sent back to back look like a 1.15 ms gap and every well-behaved VMC on
 * the bus appears to be sitting on the 1 ms inter-byte limit. */
#define MDB_FRAME_US        1146

/* An idle gap this long inside one command block means bytes went missing or
 * the VMC stalled mid-block. MDB/ICP 4.2 section 3.2 puts t_inter-byte(max)
 * at 1.0 ms; 1.5 ms leaves half a millisecond of slop for a sloppy VMC. */
#define GAP_ERR_US          1500

/* Post-trigger window: keep recording this many bytes after a bus error
 * before freezing the snapshot, so the dump shows the recovery too. */
#define SNAP_POST_ENTRIES   32

/* Don't arm more than one automatic snapshot per minute - a bus that is
 * failing continuously would otherwise publish a dump every few seconds. */
#define SNAP_MIN_INTERVAL_US (60 * 1000000LL)

/* Kconfig supplies both, but keep the module buildable if it is included
 * from a project whose sdkconfig predates these options. */
#ifndef CONFIG_MDB_DEBUG_TRACE_DEPTH
#define CONFIG_MDB_DEBUG_TRACE_DEPTH 256
#endif
#ifndef CONFIG_MDB_DEBUG_DEFAULT_LEVEL
#define CONFIG_MDB_DEBUG_DEFAULT_LEVEL 2
#endif

#define TRACE_DEPTH  CONFIG_MDB_DEBUG_TRACE_DEPTH
#define SNAP_DEPTH   96

/* Level-3 serial dumper: how often it prints and how much. Kept modest
 * because a kilobyte of text is ~90 ms of blocking UART writes at 115200. */
#define TRACE_LOG_INTERVAL_MS  10000
#define TRACE_LOG_TEXT_LEN     1024
#define TRACE_LOG_MAX_ENTRIES  96

typedef struct {
    uint32_t t_us;   /* low 32 bits of esp_timer_get_time() */
    uint16_t w;      /* 9-bit word + MDB_TRACE_* flag bits  */
} trace_entry_t;

/* ---------- state ---------- */

static uint8_t s_own_addr;
static uint8_t s_other_addr;
static uint8_t s_level = CONFIG_MDB_DEBUG_DEFAULT_LEVEL;

static struct {
    uint32_t rx_words;      /* 9-bit words received from the VMC       */
    uint32_t tx_words;      /* 9-bit words we put on the wire          */
    uint32_t cmds;          /* address bytes seen (any peripheral)     */
    uint32_t cmds_mine;     /* address bytes carrying our address      */
    uint32_t cmds_other;    /* address bytes for someone else          */
    uint32_t blocks;        /* VMC command blocks that reached their CHK */
    uint32_t short_blocks;  /* blocks cut short by the next address byte */
    uint32_t ack;           /* VMC ACK  after one of our data blocks   */
    uint32_t nak;           /* VMC NAK  - our block arrived corrupted  */
    uint32_t ret;           /* VMC RET  - VMC asks for a retransmit    */
    uint32_t tx_blocks;     /* data blocks we sent (payload > 0)       */
    uint32_t tx_acks;       /* bare ACK* answers (nothing to report)   */
    uint32_t framing_err;   /* stop bit sampled low                    */
    uint32_t chk_err;       /* checksum mismatch on a frame for us     */
    uint32_t gap_err;       /* oversized inter-byte gap inside a block */
    uint32_t unknown_cmd;   /* commands for us that we don't implement */
    uint32_t drains;        /* mdb_drain_bus() resynchronisations      */
    uint32_t silences;      /* times the bus went quiet                */
    uint32_t max_silence_ms;
    uint32_t rsp_last_us;   /* latency of our most recent answer       */
    uint32_t rsp_max_us;
} s_c;

static uint32_t s_addr_map[ADDR_SLOTS];

/* Commands the VMC sends to *our* address, split by the command bits. The
 * split is what separates "the VMC is talking to us" from "the VMC is
 * talking to us and getting nowhere": a reader whose answers never arrive
 * sees RESET over and over and a POLL count stuck at zero. */
static uint32_t s_own_cmd[8];

/* Reader-enable and last-sale bookkeeping. Neither is on the bit-bang hot
 * path: both are written once per command block at most. */
static bool s_reader_enabled;

static struct {
    uint32_t n;         /* sales recorded since the last counter reset */
    uint8_t  cmd;       /* 0x21 cash / 0x23 sniffed card / 0x24 cashless */
    uint16_t raw;       /* price as the VMC sent it, in advertised units */
    uint16_t item;
    uint32_t cents;     /* what we published for it                      */
    uint16_t raw_min;
    uint16_t raw_max;
    int64_t  t_us;
} s_vend;

static int64_t s_t_last_rx   = 0;   /* last byte from the VMC          */
static int64_t s_t_last_mine = 0;   /* last address byte that was ours */
static bool    s_in_silence  = false;

/* Running reconstruction of the current VMC command block, used to tell a
 * block's CHK byte from its payload. Only the VMC transmits on the line we
 * listen to, so the sum is exact as long as no byte is lost - and when one
 * is, that is precisely what short_blocks/gap_err are there to report. */
static bool    s_in_block = false;
static uint8_t s_blk_sum  = 0;
static uint8_t s_blk_len  = 0;

#if TRACE_DEPTH > 0
static trace_entry_t s_trace[TRACE_DEPTH];
static uint16_t      s_trace_head;   /* next slot to write */
static uint16_t      s_trace_len;

static trace_entry_t s_snap[SNAP_DEPTH];
static uint16_t      s_snap_len;
static uint8_t       s_snap_post;    /* entries still to record before freezing */
static bool          s_snap_ready;
static const char   *s_snap_cause = "";
static int64_t       s_snap_last_us = 0;
#endif

/* ---------- helpers ---------- */

/* Byte-to-byte period -> the idle time between them. */
static inline uint32_t idle_gap_us(uint32_t period_us)
{
    return period_us > MDB_FRAME_US ? period_us - MDB_FRAME_US : 0;
}

/* Bounded append. Returns the new length, or the old one when the text
 * would not fit (leaving `out` NUL-terminated and unchanged). The format
 * attribute is what makes -Wformat check the long argument list in
 * mdb_debug_json() below. */
__attribute__((format(printf, 4, 5)))
static size_t appendf(char *out, size_t cap, size_t used, const char *fmt, ...)
{
    if (!out || cap == 0 || used >= cap - 1) return used;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t) n >= cap - used) {
        out[used] = '\0';
        return used;
    }
    return used + (size_t) n;
}

#if TRACE_DEPTH > 0
static void snapshot_freeze(void)
{
    uint16_t n = s_trace_len < SNAP_DEPTH ? s_trace_len : SNAP_DEPTH;
    uint16_t start = (uint16_t) ((s_trace_head + TRACE_DEPTH - n) % TRACE_DEPTH);

    for (uint16_t i = 0; i < n; i++) {
        s_snap[i] = s_trace[(start + i) % TRACE_DEPTH];
    }
    s_snap_len   = n;
    s_snap_ready = true;
    s_snap_post  = 0;
}

/* Freeze the trace around a bus error. `post` entries of extra context are
 * recorded first, unless the caller wants the snapshot taken immediately
 * (a silent bus produces no further entries to wait for). */
static void snapshot_arm(const char *cause, bool immediate)
{
    if (s_level < MDB_DBG_SNAPSHOT) return;
    if (s_snap_ready || s_snap_post) return;   /* one in flight already */

    int64_t now = esp_timer_get_time();
    if (s_snap_last_us && now - s_snap_last_us < SNAP_MIN_INTERVAL_US) return;
    s_snap_last_us = now;

    s_snap_cause = cause;
    if (immediate) {
        snapshot_freeze();
    } else {
        s_snap_post = SNAP_POST_ENTRIES;
    }
}

static inline void trace_push(uint16_t word, uint32_t t_us)
{
    if (s_level < MDB_DBG_TRACE) return;

    s_trace[s_trace_head].t_us = t_us;
    s_trace[s_trace_head].w    = word;
    s_trace_head = (uint16_t) ((s_trace_head + 1) % TRACE_DEPTH);
    if (s_trace_len < TRACE_DEPTH) s_trace_len++;

    if (s_snap_post && --s_snap_post == 0) snapshot_freeze();
}

static size_t render_entries(const trace_entry_t *buf, uint16_t n,
                             char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    uint32_t prev_t = n ? buf[0].t_us : 0;

    for (uint16_t i = 0; i < n; i++) {
        char tok[16];
        size_t k = 0;

        uint32_t gap = idle_gap_us(buf[i].t_us - prev_t);   /* wraps cleanly */
        prev_t = buf[i].t_us;

        if (i > 0 && gap >= 1000) {
            uint32_t ms = gap / 1000;
            if (ms > 9999) ms = 9999;
            k += (size_t) snprintf(tok, sizeof(tok), "/%lu ", (unsigned long) ms);
        }

        uint16_t w = buf[i].w;
        if (w & MDB_TRACE_TX)      tok[k++] = '>';
        if (w & 0x100)             tok[k++] = '*';
        tok[k++] = hex[(w >> 4) & 0x0F];
        tok[k++] = hex[w & 0x0F];
        if (w & MDB_TRACE_FRAMING) tok[k++] = '!';
        if (i + 1 < n)             tok[k++] = ' ';
        tok[k]   = '\0';

        if (used + k >= cap) break;            /* stop rather than truncate mid-token */
        memcpy(out + used, tok, k + 1);
        used += k;
    }

    if (cap) out[used] = '\0';
    return used;
}
#endif /* TRACE_DEPTH > 0 */

/* ---------- lifecycle ---------- */

void mdb_debug_init(uint8_t own_addr, uint8_t other_addr)
{
    s_own_addr   = own_addr & ADDR_MASK;
    s_other_addr = other_addr & ADDR_MASK;
    /* A fresh boot genuinely has not been enabled by anyone yet — unlike
     * mdb_debug_reset(), which only zeroes counters and must not forget it. */
    s_reader_enabled = false;
    mdb_debug_reset();
    ESP_LOGI(TAG, "MDB debug ready: own=0x%02X other=0x%02X level=%u traceDepth=%d",
             s_own_addr, s_other_addr, s_level, TRACE_DEPTH);
}

void mdb_debug_set_level(uint8_t level)
{
    if (level > MDB_DBG_VERBOSE) level = MDB_DBG_VERBOSE;
    s_level = level;
    ESP_LOGW(TAG, "debug level -> %u", level);
}

uint8_t mdb_debug_get_level(void) { return s_level; }

/* Level-3 serial dumper.
 *
 * A task rather than an esp_timer callback on purpose: rendering and
 * printing a kilobyte of trace blocks the caller for ~90 ms of UART writes,
 * and the timer task also carries the MQTT heartbeat and the cellular
 * watchdogs. It stays parked at one wakeup per 10 s once the level drops
 * back below 3, so it costs nothing to leave running. */
static void trace_log_task(void *arg)
{
    (void) arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TRACE_LOG_INTERVAL_MS));
        if (s_level < MDB_DBG_VERBOSE) continue;

        char *buf = malloc(TRACE_LOG_TEXT_LEN);
        if (!buf) continue;
        if (mdb_debug_trace_render(buf, TRACE_LOG_TEXT_LEN, TRACE_LOG_MAX_ENTRIES) > 0)
            ESP_LOGI(TAG, "trace: %s", buf);
        free(buf);
    }
}

void mdb_debug_apply_level(uint8_t level, bool persist)
{
    if (level > MDB_DBG_VERBOSE) level = MDB_DBG_VERBOSE;
    mdb_debug_set_level(level);

    if (persist) {
        nvs_handle_t h;
        if (nvs_open("vmflow", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "mdb_dbg", level);
            nvs_commit(h);
            nvs_close(h);
        } else {
            ESP_LOGW(TAG, "could not persist debug level to NVS");
        }
    }

    static bool tracer_started = false;
    if (level >= MDB_DBG_VERBOSE && !tracer_started) {
        if (xTaskCreate(trace_log_task, "mdb_trace", 3072, NULL, 2, NULL) == pdPASS)
            tracer_started = true;
        else
            ESP_LOGW(TAG, "could not start the verbose trace dumper");
    }
}

void mdb_debug_reset(void)
{
    memset(&s_c, 0, sizeof(s_c));
    memset(s_addr_map, 0, sizeof(s_addr_map));
    memset(s_own_cmd, 0, sizeof(s_own_cmd));
    memset(&s_vend, 0, sizeof(s_vend));
    /* s_reader_enabled deliberately survives: it records something the VMC
     * did, not a counter, and clearing it would make a mid-session reset of
     * the counters report a reader that is plainly working as never enabled. */
    s_t_last_rx   = 0;
    s_t_last_mine = 0;
    s_in_silence  = false;
    s_in_block    = false;
    s_blk_sum     = 0;
    s_blk_len     = 0;
#if TRACE_DEPTH > 0
    s_trace_head   = 0;
    s_trace_len    = 0;
    s_snap_len     = 0;
    s_snap_post    = 0;
    s_snap_ready   = false;
    s_snap_cause   = "";
    s_snap_last_us = 0;
#endif
}

/* ---------- hot path ---------- */

void mdb_debug_rx_byte(uint16_t word, bool stop_ok)
{
    int64_t  now  = esp_timer_get_time();
    uint32_t t32  = (uint32_t) now;
    uint8_t  byte = (uint8_t) word;

    s_c.rx_words++;
    s_in_silence = false;

    if (!stop_ok) {
        s_c.framing_err++;
        word |= MDB_TRACE_FRAMING;
    }

    /* Inter-byte gap, evaluated only inside a command block: the gap
     * between two blocks legitimately covers our own response time. */
    if (s_t_last_rx && s_in_block) {
        if (idle_gap_us((uint32_t) (now - s_t_last_rx)) > GAP_ERR_US) s_c.gap_err++;
    }

    if (word & 0x100) {
        /* Mode bit set: either a VMC handshake byte or a new address. */
        if (word == WORD_ACK || word == WORD_RET || word == WORD_NAK) {
            if (word == WORD_ACK) s_c.ack++;
            else if (word == WORD_RET) s_c.ret++;
            else s_c.nak++;
            s_in_block = false;
        } else {
            /* A block that never reached its checksum lost bytes on the
             * way - the VMC has already moved on to the next command. */
            if (s_in_block && s_blk_len > 1) s_c.short_blocks++;

            s_c.cmds++;
            s_addr_map[(byte & ADDR_MASK) >> 3]++;

            if ((byte & ADDR_MASK) == s_own_addr) {
                s_c.cmds_mine++;
                s_own_cmd[byte & 0x07]++;
                s_t_last_mine = now;
            } else {
                s_c.cmds_other++;
            }

            s_in_block = true;
            s_blk_sum  = byte;
            s_blk_len  = 1;
        }
    } else if (s_in_block) {
        if (byte == s_blk_sum) {
            /* Matches the running sum, so this is the block's CHK. */
            s_c.blocks++;
            s_in_block = false;
        } else {
            s_blk_sum = (uint8_t) (s_blk_sum + byte);
            if (s_blk_len < 255) s_blk_len++;
        }
    }

    s_t_last_rx = now;

#if TRACE_DEPTH > 0
    trace_push(word, t32);
    if (!stop_ok) snapshot_arm("framing", false);
#else
    (void) t32;
#endif
}

void mdb_debug_tx_block(const uint8_t *payload, uint8_t len, uint8_t chk, int64_t t_start_us)
{
    if (len > 0) s_c.tx_blocks++;
    else         s_c.tx_acks++;
    s_c.tx_words += (uint32_t) len + 1;

    if (s_t_last_rx && t_start_us > s_t_last_rx) {
        uint32_t lat = (uint32_t) (t_start_us - s_t_last_rx);
        s_c.rsp_last_us = lat;
        if (lat > s_c.rsp_max_us) s_c.rsp_max_us = lat;
    }

#if TRACE_DEPTH > 0
    if (s_level < MDB_DBG_TRACE) return;

    /* One timestamp per byte, spaced by the real 10-bit frame time so the
     * rendered trace keeps our answer in proportion to the VMC's traffic. */
    uint32_t t = (uint32_t) t_start_us;
    for (uint8_t i = 0; i < len; i++) {
        trace_push((uint16_t) payload[i] | MDB_TRACE_TX, t);
        t += 1042;
    }
    trace_push((uint16_t) chk | 0x100 | MDB_TRACE_TX, t);
#else
    (void) payload; (void) chk;
#endif
}

void mdb_debug_note_silence(uint32_t us)
{
    if (s_in_silence) return;          /* count the event, not each retry */
    s_in_silence = true;
    s_c.silences++;

    uint32_t ms = us / 1000;
    if (s_t_last_rx) {
        int64_t quiet = esp_timer_get_time() - s_t_last_rx;
        ms = (uint32_t) (quiet / 1000);
    }
    if (ms > s_c.max_silence_ms) s_c.max_silence_ms = ms;

    /* Whatever happened just before the bus went quiet is the interesting
     * part, and no further bytes are coming to pad the window with. */
#if TRACE_DEPTH > 0
    if (s_c.rx_words) snapshot_arm("silence", true);
#endif
}

void mdb_debug_note_chk_err(void)
{
    s_c.chk_err++;
#if TRACE_DEPTH > 0
    snapshot_arm("chk", false);
#endif
}

void mdb_debug_note_unknown_cmd(void) { s_c.unknown_cmd++; }
void mdb_debug_note_drain(void)       { s_c.drains++; }
void mdb_debug_note_reader_enabled(void) { s_reader_enabled = true; }

void mdb_debug_note_vend(uint8_t cmd, uint16_t raw, uint16_t item, uint32_t cents)
{
    if (s_vend.n == 0 || raw < s_vend.raw_min) s_vend.raw_min = raw;
    if (s_vend.n == 0 || raw > s_vend.raw_max) s_vend.raw_max = raw;

    s_vend.n++;
    s_vend.cmd   = cmd;
    s_vend.raw   = raw;
    s_vend.item  = item;
    s_vend.cents = cents;
    s_vend.t_us  = esp_timer_get_time();
}

/* ---------- reporting ---------- */

const char *mdb_debug_verdict(void)
{
    if (s_c.rx_words == 0) return MDB_VERDICT_NO_RX;

    int64_t quiet_ms = (esp_timer_get_time() - s_t_last_rx) / 1000;
    if (quiet_ms > 2000) return MDB_VERDICT_BUS_SILENT;

    if (s_c.cmds_mine > 0) {
        /* The VMC keeps resetting us and never advances to POLL: it is not
         * accepting our answers. Reporting this as "ok" because bytes are
         * arriving would hide the actual fault. */
        if (s_own_cmd[MDB_CMD_POLL] == 0 && s_own_cmd[MDB_CMD_RESET] >= 3)
            return MDB_VERDICT_RESET_LOOP;

        /* Polled, answering, never switched on. The link is healthy and the
         * old verdict said so - but a reader the VMC has not enabled can
         * hold no session, so it reports neither cashless vends nor the
         * VMC's cash sales, and "ok" sent the operator looking at the wiring
         * instead of at the machine's own service menu. */
        if (!s_reader_enabled)
            return MDB_VERDICT_NOT_READY;

        return MDB_VERDICT_OK;
    }
    if (s_other_addr && s_addr_map[s_other_addr >> 3] > 0) return MDB_VERDICT_WRONG_ADDR;

    return MDB_VERDICT_NOT_ENABLED;
}

uint8_t mdb_debug_addr_hint(void)
{
    if (s_c.cmds_mine > 0) return 0;
    if (s_other_addr && s_addr_map[s_other_addr >> 3] > 0) return s_other_addr;
    return 0;
}

size_t mdb_debug_json(char *out, size_t cap)
{
    if (!out || cap < 2) return 0;

    int64_t  now       = esp_timer_get_time();
    uint32_t last_rx   = s_t_last_rx   ? (uint32_t) ((now - s_t_last_rx) / 1000)   : 0;
    uint32_t last_mine = s_t_last_mine ? (uint32_t) ((now - s_t_last_mine) / 1000) : 0;

    size_t n = 0;
    n = appendf(out, cap, n, "{\"lvl\":%u,\"verdict\":\"%s\"", s_level, mdb_debug_verdict());

    uint8_t hint = mdb_debug_addr_hint();
    if (hint) n = appendf(out, cap, n, ",\"hint\":\"0x%02X\"", hint);

    n = appendf(out, cap, n,
        ",\"up\":%lu,\"rx\":%lu,\"tx\":%lu,\"cmd\":%lu,\"mine\":%lu,\"other\":%lu"
        ",\"blk\":%lu,\"shortBlk\":%lu,\"ack\":%lu,\"nak\":%lu,\"ret\":%lu"
        ",\"txBlk\":%lu,\"txAck\":%lu,\"frmErr\":%lu,\"chkErr\":%lu,\"gapErr\":%lu"
        ",\"unk\":%lu,\"drain\":%lu,\"sil\":%lu,\"maxSilMs\":%lu"
        ",\"lastRxMs\":%lu,\"lastMineMs\":%lu,\"rspUs\":%lu,\"rspMaxUs\":%lu",
        (unsigned long) (now / 1000000),
        (unsigned long) s_c.rx_words,    (unsigned long) s_c.tx_words,
        (unsigned long) s_c.cmds,        (unsigned long) s_c.cmds_mine,
        (unsigned long) s_c.cmds_other,  (unsigned long) s_c.blocks,
        (unsigned long) s_c.short_blocks,(unsigned long) s_c.ack,
        (unsigned long) s_c.nak,         (unsigned long) s_c.ret,
        (unsigned long) s_c.tx_blocks,   (unsigned long) s_c.tx_acks,
        (unsigned long) s_c.framing_err, (unsigned long) s_c.chk_err,
        (unsigned long) s_c.gap_err,     (unsigned long) s_c.unknown_cmd,
        (unsigned long) s_c.drains,      (unsigned long) s_c.silences,
        (unsigned long) s_c.max_silence_ms,
        (unsigned long) last_rx,         (unsigned long) last_mine,
        (unsigned long) s_c.rsp_last_us, (unsigned long) s_c.rsp_max_us);

    n = appendf(out, cap, n,
        ",\"myCmd\":{\"reset\":%lu,\"setup\":%lu,\"poll\":%lu,\"vend\":%lu"
        ",\"reader\":%lu,\"exp\":%lu}",
        (unsigned long) s_own_cmd[0], (unsigned long) s_own_cmd[1],
        (unsigned long) s_own_cmd[2], (unsigned long) s_own_cmd[3],
        (unsigned long) s_own_cmd[4], (unsigned long) s_own_cmd[7]);

    /* The units every `raw` on this bus is expressed in. Constant, but it is
     * the key to reading the numbers above it and is not otherwise knowable
     * from a report - a device flashed with a coarser scale factor looks
     * identical until you see this. */
    n = appendf(out, cap, n, ",\"scale\":{\"sf\":%u,\"dp\":%u}",
                (unsigned) CONFIG_MDB_SCALE_FACTOR,
                (unsigned) CONFIG_MDB_DECIMAL_PLACES);

    if (s_vend.n) {
        n = appendf(out, cap, n,
            ",\"lastVend\":{\"cmd\":\"0x%02X\",\"raw\":%u,\"cents\":%lu,\"item\":%u"
            ",\"n\":%lu,\"rawMin\":%u,\"rawMax\":%u,\"ageMs\":%lu}",
            s_vend.cmd, s_vend.raw, (unsigned long) s_vend.cents, s_vend.item,
            (unsigned long) s_vend.n, s_vend.raw_min, s_vend.raw_max,
            (unsigned long) ((now - s_vend.t_us) / 1000));
    }

    n = appendf(out, cap, n, ",\"addr\":{");
    bool first = true;
    for (int i = 0; i < ADDR_SLOTS; i++) {
        if (!s_addr_map[i]) continue;
        n = appendf(out, cap, n, "%s\"%02X\":%lu",
                    first ? "" : ",", i << 3, (unsigned long) s_addr_map[i]);
        first = false;
    }
    n = appendf(out, cap, n, "}}");

    return n;
}

uint16_t mdb_debug_trace_count(void)
{
#if TRACE_DEPTH > 0
    return s_trace_len;
#else
    return 0;
#endif
}

size_t mdb_debug_trace_render(char *out, size_t cap, uint16_t max_entries)
{
#if TRACE_DEPTH > 0
    uint16_t n = s_trace_len;
    if (max_entries && n > max_entries) n = max_entries;
    if (n == 0) { if (cap) out[0] = '\0'; return 0; }

    /* Copy the window out of the ring first: the MDB task keeps writing
     * while we render, and a contiguous copy keeps the output coherent. */
    trace_entry_t *win = malloc(sizeof(trace_entry_t) * n);
    if (!win) { if (cap) out[0] = '\0'; return 0; }

    uint16_t start = (uint16_t) ((s_trace_head + TRACE_DEPTH - n) % TRACE_DEPTH);
    for (uint16_t i = 0; i < n; i++) win[i] = s_trace[(start + i) % TRACE_DEPTH];

    size_t used = render_entries(win, n, out, cap);
    free(win);
    return used;
#else
    (void) max_entries;
    if (cap) out[0] = '\0';
    return 0;
#endif
}

bool mdb_debug_snapshot_ready(void)
{
#if TRACE_DEPTH > 0
    return s_snap_ready;
#else
    return false;
#endif
}

const char *mdb_debug_snapshot_cause(void)
{
#if TRACE_DEPTH > 0
    return s_snap_cause;
#else
    return "";
#endif
}

size_t mdb_debug_snapshot_render(char *out, size_t cap)
{
#if TRACE_DEPTH > 0
    if (!s_snap_ready) { if (cap) out[0] = '\0'; return 0; }
    return render_entries(s_snap, s_snap_len, out, cap);
#else
    if (cap) out[0] = '\0';
    return 0;
#endif
}

void mdb_debug_snapshot_clear(void)
{
#if TRACE_DEPTH > 0
    s_snap_ready = false;
    s_snap_len   = 0;
    s_snap_cause = "";
#endif
}
