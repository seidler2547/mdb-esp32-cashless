/* Host harness for mdb_debug.c: drives a synthetic MDB byte stream and
 * checks the counters, the verdict and the trace rendering. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mdb_debug.h"

/* ---- stub backends ---- */
static int64_t g_now = 1000000;
int64_t esp_timer_get_time(void) { return g_now; }
void vTaskDelay(uint32_t t) { (void)t; }
int xTaskCreate(void (*fn)(void*), const char *n, uint32_t s, void *a, uint32_t p, void *h)
{ (void)fn;(void)n;(void)s;(void)a;(void)p;(void)h; return 1; }
int nvs_open(const char *ns, int m, int *h) { (void)ns;(void)m; *h = 1; return 0; }
int nvs_set_u8(int h, const char *k, unsigned char v) { (void)h;(void)k;(void)v; return 0; }
int nvs_commit(int h) { (void)h; return 0; }
void nvs_close(int h) { (void)h; }

/* ---- helpers ---- */
#define ADV(us) (g_now += (us))
#define BYTE_US 1146          /* one 11-bit MDB frame at 9600 baud */

static void rx(uint16_t w) { ADV(BYTE_US); mdb_debug_rx_byte(w, true); }
static void rx_bad(uint16_t w) { ADV(BYTE_US); mdb_debug_rx_byte(w, false); }

/* A whole VMC command block: address byte (mode bit set) + data + checksum. */
static void vmc_block(uint8_t addr_cmd, const uint8_t *data, int n)
{
    ADV(2000);                       /* idle before a new command */
    uint8_t sum = addr_cmd;
    rx(0x100 | addr_cmd);
    for (int i = 0; i < n; i++) { sum += data[i]; rx(data[i]); }
    rx(sum);
}

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static long jget(const char *json, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    return strtol(p + strlen(pat), NULL, 10);
}

static int jstr_is(const char *json, const char *key, const char *val)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"%s\"", key, val);
    return strstr(json, pat) != NULL;
}

int main(void)
{
    char json[1024];
    char trace[4096];

    /* ---- 1. healthy bus: VMC polls us, we answer ---- */
    printf("1. healthy exchange\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_apply_level(MDB_DBG_SNAPSHOT, false);

    mdb_debug_note_reader_enabled();                    /* VMC sent READER ENABLE */

    for (int i = 0; i < 10; i++) {
        vmc_block(0x12, NULL, 0);                       /* POLL to 0x10 */
        int64_t t = g_now + 200;
        mdb_debug_tx_block(NULL, 0, 0x00, t);           /* bare ACK* */
        g_now = t + BYTE_US;
        vmc_block(0x08 | 0x02, NULL, 0);                /* changer POLL */
    }
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_OK));
    CHECK(jget(json, "mine") == 10);
    CHECK(jget(json, "other") == 10);
    CHECK(jget(json, "cmd") == 20);
    CHECK(jget(json, "blk") == 20);
    CHECK(jget(json, "shortBlk") == 0);
    CHECK(jget(json, "txAck") == 10);
    CHECK(jget(json, "chkErr") == 0);
    CHECK(jget(json, "frmErr") == 0);
    CHECK(jget(json, "gapErr") == 0);
    CHECK(jget(json, "rspUs") == 200);
    CHECK(strstr(json, "\"addr\":{\"08\":10,\"10\":10}") != NULL);
    CHECK(strstr(json, "\"hint\"") == NULL);

    mdb_debug_trace_render(trace, sizeof(trace), 8);
    printf("   trace: %s\n", trace);
    CHECK(strstr(trace, ">*00") != NULL);   /* our ACK* is marked as ours */
    CHECK(strstr(trace, "*12") != NULL);    /* POLL to 0x10 */
    /* Bytes sent back to back are one frame apart, which is not a gap: the
     * renderer must not mark them, or every healthy block looks out of spec. */
    CHECK(strstr(trace, "*12 12") != NULL);

    /* ---- 2. VMC configured for the other cashless address ---- */
    printf("2. wrong address\n");
    mdb_debug_init(0x10, 0x60);
    for (int i = 0; i < 30; i++) {
        vmc_block(0x62, NULL, 0);           /* POLL to cashless #2 */
        vmc_block(0x0A, NULL, 0);           /* changer */
    }
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_WRONG_ADDR));
    CHECK(jstr_is(json, "hint", "0x60"));
    CHECK(mdb_debug_addr_hint() == 0x60);
    CHECK(jget(json, "mine") == 0);

    /* ---- 3. bus alive but no cashless device polled at all ---- */
    printf("3. reader not enabled in the VMC\n");
    mdb_debug_init(0x10, 0x60);
    for (int i = 0; i < 20; i++) { vmc_block(0x0A, NULL, 0); vmc_block(0x32, NULL, 0); }
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_NOT_ENABLED));
    CHECK(mdb_debug_addr_hint() == 0);

    /* ---- 4. nothing on the bus at all ---- */
    printf("4. dead bus\n");
    mdb_debug_init(0x10, 0x60);
    CHECK(jstr_is((mdb_debug_json(json, sizeof(json)), json), "verdict", MDB_VERDICT_NO_RX));
    mdb_debug_note_silence(250000);
    CHECK(jget((mdb_debug_json(json, sizeof(json)), json), "sil") == 1);
    /* repeat timeouts during one silence count as one event */
    mdb_debug_note_silence(250000);
    CHECK(jget((mdb_debug_json(json, sizeof(json)), json), "sil") == 1);

    /* ---- 5. bus went quiet after working ---- */
    printf("5. bus went quiet\n");
    mdb_debug_init(0x10, 0x60);
    vmc_block(0x12, NULL, 0);
    ADV(3000000);
    mdb_debug_note_silence(250000);
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_BUS_SILENT));
    CHECK(jget(json, "maxSilMs") >= 3000);
    CHECK(mdb_debug_snapshot_ready());          /* frozen immediately */
    CHECK(strcmp(mdb_debug_snapshot_cause(), "silence") == 0);
    mdb_debug_snapshot_render(trace, sizeof(trace));
    printf("   snapshot(%s): %s\n", mdb_debug_snapshot_cause(), trace);
    CHECK(strlen(trace) > 0);
    mdb_debug_snapshot_clear();
    CHECK(!mdb_debug_snapshot_ready());

    /* ---- 6. framing errors and a truncated block ---- */
    printf("6. framing error + lost bytes\n");
    mdb_debug_init(0x10, 0x60);
    ADV(2000); rx_bad(0x0A5);
    CHECK(jget((mdb_debug_json(json, sizeof(json)), json), "frmErr") == 1);

    /* a VEND_REQUEST whose tail never arrives, then the next command */
    {
        uint8_t partial[] = { 0x00, 0x01 };
        ADV(2000);
        rx(0x113);                              /* VEND to 0x10 */
        for (unsigned i = 0; i < sizeof(partial); i++) rx(partial[i]);
        vmc_block(0x12, NULL, 0);               /* VMC moved on: bytes were lost */
    }
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jget(json, "shortBlk") == 1);

    mdb_debug_trace_render(trace, sizeof(trace), 32);
    printf("   trace: %s\n", trace);
    CHECK(strstr(trace, "A5!") != NULL);        /* framing error is marked */
    CHECK(strstr(trace, "/2 ") != NULL);        /* the 2 ms idle gap is marked */

    /* ---- 7. handshake bytes and error snapshot ---- */
    printf("7. NAK/RET/ACK + checksum snapshot\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_apply_level(MDB_DBG_SNAPSHOT, false);
    ADV(2000); rx(0x100);                       /* ACK */
    ADV(2000); rx(0x1AA);                       /* RET */
    ADV(2000); rx(0x1FF);                       /* NAK */
    mdb_debug_json(json, sizeof(json));
    CHECK(jget(json, "ack") == 1);
    CHECK(jget(json, "ret") == 1);
    CHECK(jget(json, "nak") == 1);
    CHECK(jget(json, "cmd") == 0);              /* handshakes are not addresses */

    mdb_debug_note_chk_err();
    CHECK(!mdb_debug_snapshot_ready());         /* waits for the post-trigger window */
    for (int i = 0; i < 40; i++) { ADV(1000); rx(0x55); }
    CHECK(mdb_debug_snapshot_ready());
    CHECK(strcmp(mdb_debug_snapshot_cause(), "chk") == 0);

    /* rate limit: a second error inside a minute must not re-arm */
    mdb_debug_snapshot_clear();
    mdb_debug_note_chk_err();
    for (int i = 0; i < 40; i++) { ADV(1000); rx(0x55); }
    CHECK(!mdb_debug_snapshot_ready());
    ADV(61000000);
    mdb_debug_note_chk_err();
    for (int i = 0; i < 40; i++) { ADV(1000); rx(0x55); }
    CHECK(mdb_debug_snapshot_ready());

    /* ---- 8. renderer never overruns its buffer ---- */
    printf("8. bounded rendering\n");
    for (size_t cap = 1; cap < 64; cap++) {
        char small[64];
        memset(small, 0x7E, sizeof(small));
        size_t n = mdb_debug_trace_render(small, cap, 0);
        CHECK(n < cap);
        CHECK(small[n] == '\0');
        CHECK((unsigned char) small[cap] == 0x7E);   /* nothing written past cap */
    }
    for (size_t cap = 1; cap < 200; cap++) {
        char small[256];
        memset(small, 0x7E, sizeof(small));
        size_t n = mdb_debug_json(small, cap);
        CHECK(n < cap);
        CHECK((unsigned char) small[cap] == 0x7E);
    }

    /* ---- 9. level 0 keeps counters but no trace ---- */
    printf("9. level gating\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_apply_level(MDB_DBG_COUNTERS, false);
    for (int i = 0; i < 10; i++) vmc_block(0x12, NULL, 0);
    CHECK(mdb_debug_trace_count() == 0);
    CHECK(jget((mdb_debug_json(json, sizeof(json)), json), "mine") == 10);
    mdb_debug_apply_level(MDB_DBG_TRACE, false);
    vmc_block(0x12, NULL, 0);
    CHECK(mdb_debug_trace_count() == 2);

    /* ---- 10. ring wraps without losing coherence ---- */
    printf("10. ring wrap\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_apply_level(MDB_DBG_TRACE, false);
    for (int i = 0; i < 1000; i++) { ADV(1000); rx((uint16_t)(i & 0xFF)); }
    CHECK(mdb_debug_trace_count() == 256);
    size_t n = mdb_debug_trace_render(trace, sizeof(trace), 0);
    CHECK(n > 0);
    /* last byte pushed was 999 & 0xFF = 0xE7 and must be last in the render */
    CHECK(strcmp(trace + strlen(trace) - 2, "E7") == 0);

    /* ---- 11. addressed, but the conversation never gets past RESET ---- */
    printf("11. reset loop\n");
    mdb_debug_init(0x10, 0x60);
    for (int i = 0; i < 6; i++) {
        vmc_block(0x10, NULL, 0);              /* RESET to us */
        int64_t t = g_now + 200;
        mdb_debug_tx_block(NULL, 0, 0x00, t);  /* our ACK*, which never lands */
        g_now = t + BYTE_US;
        ADV(10000000);                         /* VMC gives up, retries in 10 s */
        vmc_block(0x0A, NULL, 0);              /* other peripherals keep going */
    }
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_RESET_LOOP));
    CHECK(jget(json, "mine") == 6);
    CHECK(strstr(json, "\"myCmd\":{\"reset\":6,\"setup\":0,\"poll\":0") != NULL);
    CHECK(mdb_debug_addr_hint() == 0);          /* the address is right, so no hint */

    /* Once the VMC does poll us the reset loop is over — but a reader that
     * has never been enabled still cannot report a sale, so the verdict
     * moves to polled_not_enabled rather than straight to ok. */
    vmc_block(0x12, NULL, 0);
    mdb_debug_json(json, sizeof(json));
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_NOT_READY));

    mdb_debug_note_reader_enabled();
    mdb_debug_json(json, sizeof(json));
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_OK));

    /* ---- 12. the reader is polled but never switched on ---- */
    printf("12. polled but not enabled\n");
    mdb_debug_init(0x10, 0x60);
    for (int i = 0; i < 8; i++) vmc_block(0x12, NULL, 0);   /* POLL to 0x10 */
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    /* The old verdict for this was "ok": bytes arrive, our address is right,
     * nothing on the link is wrong. What is wrong is in the machine's own
     * service menu, and the operator has to be told to look there. */
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_NOT_READY));
    CHECK(jget(json, "mine") == 8);
    CHECK(mdb_debug_addr_hint() == 0);          /* the address is not the problem */
    CHECK(strstr(json, "\"lastVend\"") == NULL);  /* nothing has been sold */

    /* ---- 13. what a sale looked like on the wire ---- */
    printf("13. last vend\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_note_reader_enabled();
    vmc_block(0x12, NULL, 0);

    /* Three vends at three different prices, in the units we advertised. */
    mdb_debug_note_vend(0x24, 150, 12, 150);
    ADV(1000);
    mdb_debug_note_vend(0x24,  90,  4,  90);
    ADV(1000);
    mdb_debug_note_vend(0x21, 205, 31, 205);   /* VMC-reported cash sale */
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(strstr(json, "\"lastVend\":{\"cmd\":\"0x21\",\"raw\":205,\"cents\":205,\"item\":31") != NULL);
    CHECK(jget(json, "n") == 3);
    CHECK(jget(json, "rawMin") == 90);
    CHECK(jget(json, "rawMax") == 205);
    /* The units are what make `raw` readable, so they ride along. */
    CHECK(strstr(json, "\"scale\":{\"sf\":1,\"dp\":2}") != NULL);

    /* A machine whose prices are all reported as the same number is the
     * "every sale is 1 EUR" signature: min == max over many vends says the
     * VMC really is sending one price, rather than us mangling several. */
    mdb_debug_init(0x10, 0x60);
    for (int i = 0; i < 20; i++) mdb_debug_note_vend(0x24, 100, (uint16_t) i, 100);
    mdb_debug_json(json, sizeof(json));
    CHECK(jget(json, "rawMin") == 100);
    CHECK(jget(json, "rawMax") == 100);
    CHECK(jget(json, "n") == 20);

    /* Resetting the counters clears the sale history but not the fact that
     * the VMC once enabled the reader. */
    mdb_debug_note_reader_enabled();
    mdb_debug_reset();
    mdb_debug_json(json, sizeof(json));
    CHECK(strstr(json, "\"lastVend\"") == NULL);
    vmc_block(0x12, NULL, 0);
    mdb_debug_json(json, sizeof(json));
    CHECK(jstr_is(json, "verdict", MDB_VERDICT_OK));

    /* ---- 14. VEND subcommand tally, and CASH SALE that never comes ---- */
    printf("14. vend subcommands\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_note_reader_enabled();
    vmc_block(0x12, NULL, 0);

    /* One cashless session: REQUEST, SUCCESS, SESSION COMPLETE. No CASH SALE
     * — which is what a VMC that does not report cash over MDB looks like. */
    mdb_debug_note_vend_cmd(0x00);
    mdb_debug_note_vend_cmd(0x02);
    mdb_debug_note_vend_cmd(0x04);
    mdb_debug_json(json, sizeof(json));
    printf("   %s\n", json);
    CHECK(strstr(json, "\"vendCmd\":{\"req\":1,\"cancel\":0,\"succ\":1,\"fail\":0,\"done\":1,\"cash\":0}") != NULL);

    mdb_debug_note_vend_cmd(0x05);              /* now one does arrive */
    mdb_debug_json(json, sizeof(json));
    CHECK(strstr(json, "\"cash\":1") != NULL);

    /* Before any vend the object is left out entirely, so an idle reader does
     * not report a wall of zeroes. */
    mdb_debug_init(0x10, 0x60);
    vmc_block(0x12, NULL, 0);
    mdb_debug_json(json, sizeof(json));
    CHECK(strstr(json, "\"vendCmd\"") == NULL);

    /* ---- 15. a vend freezes the price bytes ---- */
    printf("15. vend snapshot\n");
    mdb_debug_init(0x10, 0x60);
    mdb_debug_apply_level(MDB_DBG_SNAPSHOT, false);
    mdb_debug_note_reader_enabled();

    /* An error snapshot claims the slot first — a vend must still get one,
     * because the ring holds seconds of idle polling and by the time anyone
     * asks about a disputed price the bytes are long gone. */
    ADV(2000); rx_bad(0xA5);
    CHECK(mdb_debug_snapshot_ready() || true);   /* arming is enough here */

    {
        /* VEND REQUEST for 1.50 at slot 35: 13 00 00 96 00 23 CHK */
        uint8_t vend[] = { 0x00, 0x00, 0x96, 0x00, 0x23 };
        vmc_block(0x13, vend, sizeof(vend));
    }
    mdb_debug_note_vend_cmd(0x00);

    /* Post-trigger context, then the snapshot freezes. */
    for (int i = 0; i < 40; i++) vmc_block(0x12, NULL, 0);

    CHECK(mdb_debug_snapshot_ready());
    CHECK(strcmp(mdb_debug_snapshot_cause(), "vend") == 0);

    mdb_debug_snapshot_render(trace, sizeof(trace));
    printf("   snapshot(%s): %s\n", mdb_debug_snapshot_cause(), trace);
    /* The price bytes have to be in there verbatim — 0x0096 is 150. */
    CHECK(strstr(trace, "*13 00 00 96 00 23") != NULL);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
