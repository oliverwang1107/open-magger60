/*
 * km_ledstore (record format, boot load, coalesced background save) and the glue in m5_led.c (extended LED ops, attach, status)
 * on a simulated flash that can lose power after any single erase/program step. Persistence of the light configuration, LED_NOTES.md
 * section 11.
 *
 * The flash model is the one of test_calstore.c (read the comment there), here for the ONE sector 0x08036000, every access also checked
 * against km_flash_guard.h:
 *   program  v0 nothing, v1 low word done / high word not, v2 low word done / high word half-programmed, v3 low word half-programmed,
 *            v4 the whole unit done (power cut right after)
 *   erase    v0 nothing, v1 first half erased, v2 whole sector garbage, v3 second half erased, v4 first half erased / second half garbage
 *
 * Properties proven by the sweeps (the next save of a sector that holds 0, 1, 2, 100, 254, 255 or 256 records, and a sector full of foreign
 * data; every erase/program step of it, every variant):
 *   - after the cut the log loads the previous record or the new one, never anything else; the one exception is the save that has to
 *     ERASE (full or foreign sector), where "no record" is also accepted: the lights then start with the built-in defaults;
 *   - the next save after the accident succeeds and reads back exactly;
 *   - no unit is programmed twice between erases, no access leaves the sector, no guard breach;
 *   - an error reported by the flash (power stays on) and a silent bit error are never taken for a saved record.
 */
#include <stdlib.h>

#include "km/km_calstore.h"
#include "km/km_config.h"
#include "km/km_flash_guard.h"
#include "km/km_ledstore.h"
#include "km/km_settings.h"
#include "km_test.h"
#include "m5_led.h"

#define BASE KM_FLASH_LED_CONFIG
#define SECT 0x2000u
#define PL   KM_LEDSTORE_PAYLOAD_BYTES

typedef struct {
    uint8_t  mem[SECT];
    bool     dead;
    int      ev;
    int      cut_ev, cut_var;
    int      fail_ev, fail_var;
    int      corrupt_ev;
    unsigned violations;
    unsigned erases, programs;
} sim_t;

static sim_t g_sim, g_start;
static km_clog_t g_log;
static km_ledstore_t g_m;
static uint32_t g_now;                 /* seconds */
static unsigned g_multi_op_passes;     /* passes that did more than one flash step (an erase, or one 16-byte line = two units) */
static unsigned g_hooks;               /* after-flash hook calls */

static void hook_count(void *ctx)
{
    (void)ctx;
    g_hooks++;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

static bool in_range(uint32_t addr, uint32_t len)
{
    return addr >= BASE && addr + len <= BASE + SECT && addr + len >= addr;
}

static bool sim_read(void *c, uint32_t addr, uint8_t *dst, uint32_t len)
{
    sim_t *s = c;
    if (!in_range(addr, len) || !km_flash_store_allowed(addr, len)) {
        s->violations++;
        return false;
    }
    memcpy(dst, &s->mem[addr - BASE], len);
    return true;
}

static void half_program(sim_t *s, uint32_t off, const uint8_t d[8], int variant)
{
    for (unsigned w = 0; w < 2; w++) {
        uint32_t old = 0, want = 0;
        memcpy(&old, &s->mem[off + 4 * w], 4);
        memcpy(&want, d + 4 * w, 4);
        const uint32_t clear = old & ~want;
        uint32_t done = clear;
        if (variant == 0 || (variant == 1 && w == 1) || (variant == 3 && w == 1)) {
            done = 0;
        } else if ((variant == 2 && w == 1) || (variant == 3 && w == 0)) {
            done = clear & 0x55555555u;
        }
        old &= ~done;
        memcpy(&s->mem[off + 4 * w], &old, 4);
    }
}

static bool sim_program(void *c, uint32_t addr, const uint8_t d[8])
{
    sim_t *s = c;
    if (s->dead) {
        return false;
    }
    if ((addr & 7u) != 0 || !in_range(addr, 8) || !km_flash_store_allowed(addr, 8)) {
        s->violations++;
        return false;
    }
    const uint32_t off = addr - BASE;
    for (unsigned i = 0; i < 8; i++) {
        if (s->mem[off + i] != 0xFF) {
            s->violations++;                         /* a unit is only programmed while erased */
        }
    }
    const int ev = s->ev++;
    s->programs++;
    if (ev == s->cut_ev) {
        half_program(s, off, d, s->cut_var);
        s->dead = true;
        return false;
    }
    if (ev == s->fail_ev) {
        half_program(s, off, d, s->fail_var);
        return false;
    }
    memcpy(&s->mem[off], d, 8);
    if (ev == s->corrupt_ev) {
        for (unsigned i = 0; i < 8; i++) {
            if (d[i] != 0) {
                s->mem[off + i] = (uint8_t)(d[i] & (d[i] - 1u));
                break;
            }
        }
    }
    return true;
}

static bool sim_erase(void *c, uint32_t addr)
{
    sim_t *s = c;
    if (s->dead) {
        return false;
    }
    if (addr != BASE || !km_flash_store_allowed(addr, SECT)) {
        s->violations++;
        return false;
    }
    const int ev = s->ev++;
    if (ev == s->cut_ev || ev == s->fail_ev) {
        const int v = (ev == s->cut_ev) ? s->cut_var : s->fail_var;
        for (uint32_t i = 0; i < SECT; i++) {
            const bool first = i < SECT / 2;
            switch (v) {
            case 1: if (first) s->mem[i] = 0xFF; break;
            case 2: s->mem[i] = (uint8_t)mix(addr + i); break;
            case 3: if (!first) s->mem[i] = 0xFF; break;
            case 4: s->mem[i] = first ? 0xFF : (uint8_t)mix(addr + i); break;
            default: break;
            }
        }
        if (ev == s->cut_ev) {
            s->dead = true;
        }
        return false;
    }
    memset(s->mem, 0xFF, SECT);
    s->erases++;
    return true;
}

static void sim_blank(void)
{
    memset(&g_sim, 0, sizeof g_sim);
    memset(g_sim.mem, 0xFF, sizeof g_sim.mem);
    g_sim.cut_ev = g_sim.fail_ev = g_sim.corrupt_ev = -1;
    const km_store_ops_t ops = { &g_sim, sim_read, sim_erase, sim_program };
    CHECK(km_clog_init(&g_log, &ops, BASE, PL));
    CHECK_EQ(g_log.slots, 256);
}

/* a distinct valid record for every i */
static km_ledstore_rec_t rec_of(unsigned i)
{
    const uint32_t h = mix(i * 7919u + 13u);
    km_ledstore_rec_t r;
    r.cfg.effect = (uint8_t)(i % KM_LED_FX_COUNT);
    r.cfg.brightness = (uint8_t)(h >> 8);
    r.cfg.speed = (uint8_t)(1u + (h >> 16) % 255u);
    r.cfg.color.r = (uint8_t)(h >> 24);
    r.cfg.color.g = (uint8_t)(h >> 4);
    r.cfg.color.b = (uint8_t)h;
    r.idle_s = (uint16_t)(i % 5u == 0u ? 0u : (mix(i) >> 8));
    return r;
}

static bool rec_eq(const km_ledstore_rec_t *a, const km_ledstore_rec_t *b)
{
    return a->cfg.effect == b->cfg.effect && a->cfg.brightness == b->cfg.brightness && a->cfg.speed == b->cfg.speed &&
           a->cfg.color.r == b->cfg.color.r && a->cfg.color.g == b->cfg.color.g && a->cfg.color.b == b->cfg.color.b &&
           a->idle_s == b->idle_s;
}

static void prefill(unsigned n)           /* n records through the plain log API, record i = rec_of(i) */
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t p[PL];
        const km_ledstore_rec_t r = rec_of(i);
        CHECK_EQ(km_ledstore_encode(&r, p, sizeof p), PL);
        const km_clog_result_t res = km_clog_append(&g_log, p);
        CHECK(res == KM_CLOG_OK);
    }
}

static bool loaded_is(const km_ledstore_rec_t *want)
{
    km_ledstore_rec_t got;
    return km_ledstore_load(&g_log, &got) && rec_eq(&got, want);
}

/* ---- the manager driver: 20 passes per second; every pass is checked for "at most one flash operation" ---- */

static bool one_pass(bool idle_now, bool idle_long)
{
    const int e0 = g_sim.ev;
    const bool did = km_ledstore_pass(&g_m, g_now, idle_now, idle_long);
    if (g_sim.ev - e0 > 2) {
        g_multi_op_passes++;
    }
    return did;
}

/* Run passes for up to max_s seconds or until nothing is pending and no job is active, or the power is gone. Returns seconds used. */
static unsigned drive(unsigned max_s, bool idle_now, bool idle_long)
{
    unsigned used = 0;
    for (; used < max_s; used++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            (void)one_pass(idle_now, idle_long);
        }
        if (g_sim.dead || (!km_ledstore_pending(&g_m) && !km_ledstore_job_active(&g_m))) {
            used++;
            break;
        }
    }
    return used;
}

static void mgr_boot(void)
{
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    km_ledstore_rec_t r;
    (void)km_ledstore_boot(&g_m, &r);
    g_now += 1000u;                      /* a restart: time counts from a fresh moment, far from any earlier request */
}

/* ---- tests ---- */

static void test_record(void)
{
    /* defaults */
    km_ledstore_rec_t d;
    km_ledstore_rec_default(&d);
    CHECK_EQ(d.idle_s, 600);
    CHECK_EQ(d.cfg.effect, KM_LED_FX_DEMO);
    CHECK(km_ledstore_rec_valid(&d));

    /* a golden payload, written out by hand from the documented layout */
    km_ledstore_rec_t r;
    r.cfg.effect = 3; r.cfg.brightness = 200; r.cfg.speed = 90; r.cfg.color.r = 1; r.cfg.color.g = 2; r.cfg.color.b = 3;
    r.idle_s = 600;                                                   /* 0x0258 */
    uint8_t p[PL];
    CHECK_EQ(km_ledstore_encode(&r, p, sizeof p), 16);
    static const uint8_t gold[16] = { 0x01, 3, 200, 90, 1, 2, 3, 0, 0x58, 0x02, 0, 0, 0, 0, 0, 0 };
    CHECK(memcmp(p, gold, 16) == 0);
    km_ledstore_rec_t back;
    memset(&back, 0, sizeof back);
    CHECK(km_ledstore_decode(&back, gold, 16));
    CHECK(rec_eq(&back, &r));
    r.idle_s = 0xFFFF;                                                /* the full u16 range is valid */
    CHECK_EQ(km_ledstore_encode(&r, p, sizeof p), 16);
    CHECK_EQ(p[8], 0xFF);
    CHECK_EQ(p[9], 0xFF);
    CHECK(km_ledstore_decode(&back, p, 16));
    CHECK_EQ(back.idle_s, 0xFFFF);

    /* round trip over many records */
    for (unsigned i = 0; i < 2000u; i++) {
        const km_ledstore_rec_t a = rec_of(i);
        CHECK_EQ(km_ledstore_encode(&a, p, sizeof p), 16);
        km_ledstore_rec_t b;
        CHECK(km_ledstore_decode(&b, p, 16));
        CHECK(rec_eq(&a, &b));
    }

    /* strict decoding: every deviation from the documented payload is refused and leaves the output alone */
    static const km_ledstore_rec_t sentinel = { { 5, 6, 7, { 8, 9, 10 } }, 1234 };
    {
        uint8_t g[16];
        memcpy(g, gold, 16);
        km_ledstore_rec_t o = sentinel;
        CHECK(!km_ledstore_decode(&o, g, 15));                        /* length */
        CHECK(!km_ledstore_decode(&o, g, 17));
        CHECK(!km_ledstore_decode(&o, g, 0));
        CHECK(!km_ledstore_decode(&o, NULL, 16));
        CHECK(!km_ledstore_decode(NULL, g, 16));
        for (unsigned v = 0; v < 256u; v++) {                         /* version */
            g[0] = (uint8_t)v;
            CHECK_EQ(km_ledstore_decode(&o, g, 16), v == 1u);
            o = sentinel;
        }
        g[0] = 1;
        for (unsigned i = 7; i < 16u; i++) {
            if (i == 8u || i == 9u) {
                continue;                                             /* the idle time: any value */
            }
            g[i] = 1;                                                 /* reserved bytes must be zero */
            CHECK(!km_ledstore_decode(&o, g, 16));
            g[i] = 0;
        }
        for (unsigned fx = 0; fx < 256u; fx++) {                      /* effect */
            g[1] = (uint8_t)fx;
            CHECK_EQ(km_ledstore_decode(&o, g, 16), fx < KM_LED_FX_COUNT);
            o = sentinel;
        }
        g[1] = 3;
        g[3] = 0;                                                     /* speed 0 */
        CHECK(!km_ledstore_decode(&o, g, 16));
        CHECK(rec_eq(&o, &sentinel));
        g[3] = 90;
        for (unsigned b = 0; b < 256u; b++) {                         /* brightness and colour: any value */
            g[2] = (uint8_t)b; g[4] = (uint8_t)b; g[5] = (uint8_t)(255u - b); g[6] = (uint8_t)b;
            CHECK(km_ledstore_decode(&o, g, 16));
        }
    }

    /* encode refuses an invalid record or a short buffer */
    km_ledstore_rec_t bad = rec_of(1);
    bad.cfg.effect = KM_LED_FX_COUNT;
    CHECK_EQ(km_ledstore_encode(&bad, p, sizeof p), 0);
    bad = rec_of(1);
    bad.cfg.speed = 0;
    CHECK_EQ(km_ledstore_encode(&bad, p, sizeof p), 0);
    r = rec_of(2);
    CHECK_EQ(km_ledstore_encode(&r, p, 15), 0);
    CHECK_EQ(km_ledstore_encode(&r, NULL, 16), 0);
    CHECK_EQ(km_ledstore_encode(NULL, p, 16), 0);
}

static void test_load(void)
{
    km_ledstore_rec_t out = { { 9, 9, 9, { 9, 9, 9 } }, 99 };
    const km_ledstore_rec_t keep = out;

    /* blank flash, no log, wrong log: no record, output untouched */
    sim_blank();
    CHECK(!km_ledstore_load(&g_log, &out));
    CHECK(!km_ledstore_load(NULL, &out));
    CHECK(rec_eq(&out, &keep));
    km_clog_t wrong;
    const km_store_ops_t ops = { &g_sim, sim_read, sim_erase, sim_program };
    CHECK(km_clog_init(&wrong, &ops, BASE, 32));
    CHECK(!km_ledstore_load(&wrong, &out));

    /* a sector full of foreign data (the stock firmware's macros): no record, and no crash */
    for (unsigned seed = 0; seed < 50u; seed++) {
        for (unsigned i = 0; i < SECT; i++) {
            g_sim.mem[i] = (uint8_t)mix(seed * 100000u + i);
        }
        CHECK(!km_ledstore_load(&g_log, &out));
        CHECK(rec_eq(&out, &keep));
    }
    CHECK_EQ(g_sim.violations, 0);

    /* a record with a valid CRC but a payload this code does not accept: defaults. Forged straight into slot 0 from the documented layout. */
    static const uint8_t base[16] = { 0x01, 3, 200, 90, 1, 2, 3, 0, 0x58, 0x02, 0, 0, 0, 0, 0, 0 };
    for (unsigned k = 0; k < 6u; k++) {
        sim_blank();
        uint8_t pl[16];
        memcpy(pl, base, 16);
        switch (k) {
        case 0: pl[0] = 2; break;                  /* a future version */
        case 1: pl[1] = KM_LED_FX_COUNT; break;
        case 2: pl[3] = 0; break;
        case 3: pl[12] = 1; break;                 /* reserved byte */
        case 4: pl[7] = 0x80; break;
        default: break;                            /* k == 5: the control, valid */
        }
        uint8_t *s = g_sim.mem;
        memset(s, 0xFF, g_log.stride);
        uint32_t magic = 0x4C434D4Bu, seq = 1, len = 16;
        memcpy(s + 0, &magic, 4);
        memcpy(s + 8, &seq, 4);
        memcpy(s + 12, &len, 4);
        memcpy(s + 16, pl, 16);
        const uint32_t crc = km_crc32(0, s + 8, 8 + 16);
        memcpy(s + 4, &crc, 4);
        out = keep;
        CHECK_EQ(km_ledstore_load(&g_log, &out), k == 5u);
        if (k != 5u) {
            CHECK(rec_eq(&out, &keep));
        } else {
            CHECK_EQ(out.idle_s, 600);
            CHECK_EQ(out.cfg.effect, 3);
        }
    }

    /* the newest of several valid records wins */
    sim_blank();
    prefill(40);
    const km_ledstore_rec_t last = rec_of(39);
    CHECK(loaded_is(&last));
}

static void test_manager(void)
{
    /* ---- nothing happens without a request; a request alone does not touch flash ---- */
    sim_blank();
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 100;
    for (unsigned i = 0; i < 100u; i++) {
        (void)one_pass(true, true);
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    CHECK(!km_ledstore_pending(&g_m));
    km_ledstore_rec_t r1 = rec_of(1);
    CHECK(km_ledstore_request(&g_m, &r1));
    CHECK(km_ledstore_pending(&g_m));
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);                      /* RAM only */

    /* ---- the settle time: nothing for 2 s, never while the keyboard is not quiet; a key down pauses the job ---- */
    for (unsigned i = 0; i < 40u; i++) {                              /* about 2 s of passes at the time of the request */
        (void)one_pass(true, true);
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    g_now += 3;
    for (unsigned i = 0; i < 100u; i++) {
        (void)one_pass(true, false);                                  /* not idle_long: it may not start */
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    CHECK(!km_ledstore_job_active(&g_m));
    (void)one_pass(true, true);                                       /* starts: reads only */
    CHECK(km_ledstore_job_active(&g_m));
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    for (unsigned i = 0; i < 100u; i++) {
        (void)one_pass(false, false);                                 /* a key is down: the job is paused */
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    CHECK(km_ledstore_job_active(&g_m));
    unsigned passes = 0;
    while (km_ledstore_job_active(&g_m) && passes < 100u) {           /* quiet again: ONE operation per pass until done */
        const int e0 = g_sim.ev;
        (void)one_pass(true, false);
        CHECK(g_sim.ev - e0 <= 2);                                   /* one step = one flash line (two units, back to back) or the erase */
        passes++;
    }
    CHECK(!km_ledstore_job_active(&g_m));
    CHECK(passes >= 3u && passes <= 5u);                              /* payload line, header line, read-back: three steps, no erase */
    CHECK_EQ(g_sim.erases, 0);
    CHECK(loaded_is(&r1));
    CHECK(!km_ledstore_pending(&g_m));
    CHECK_EQ(g_m.saves_ok, 1);
    CHECK_EQ(g_m.last, KM_CLOG_OK);
    CHECK_EQ(g_multi_op_passes, 0);

    /* ---- coalescing: a slider drag = a request every 100 ms for 10 s; nothing is written during it, ONE record afterwards ---- */
    const unsigned prog0 = g_sim.programs, er0 = g_sim.erases;
    km_ledstore_rec_t rl = r1;
    for (unsigned i = 0; i < 100u; i++) {
        rl = rec_of(100u + i);
        CHECK(km_ledstore_request(&g_m, &rl));
        for (unsigned p = 0; p < 2u; p++) {
            (void)one_pass(true, true);
        }
        if (i % 10u == 9u) {
            g_now++;
        }
        CHECK_EQ(g_sim.programs, prog0);                              /* not a single flash operation while it is being dragged */
        CHECK_EQ(g_sim.erases, er0);
    }
    CHECK(km_ledstore_pending(&g_m));
    drive(60, true, true);
    CHECK(!km_ledstore_pending(&g_m));
    CHECK(loaded_is(&rl));                                            /* the LAST request */
    CHECK(g_sim.programs - prog0 <= 4u);                              /* one slot */
    CHECK_EQ(g_sim.erases, er0);
    CHECK_EQ(g_m.saves_ok, 2);
    CHECK_EQ(g_multi_op_passes, 0);

    /* ---- the minimum gap between two save starts ---- */
    const uint32_t start1 = g_m.last_start_s;
    const km_ledstore_rec_t r3 = rec_of(300);
    g_now++;
    CHECK(km_ledstore_request(&g_m, &r3));
    for (unsigned s = 0; s < 8u; s++) {                               /* the settle time (3 s) passes, the gap (10 s from the last start) not */
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            (void)one_pass(true, true);
        }
        if ((uint32_t)(g_now - start1) < KM_LEDSTORE_MIN_GAP_S) {
            CHECK(!km_ledstore_job_active(&g_m));
            CHECK(!loaded_is(&r3));
        }
    }
    drive(60, true, true);
    CHECK(loaded_is(&r3));
    CHECK(g_m.last_start_s - start1 >= KM_LEDSTORE_MIN_GAP_S);

    /* ---- a request that arrives while a save is running is not lost and not taken for saved: it is written by the next save ---- */
    {
        const km_ledstore_rec_t ra = rec_of(401), rb = rec_of(402);
        CHECK(km_ledstore_request(&g_m, &ra));
        (void)one_pass(true, true);                                   /* the settle time starts with this pass */
        g_now += 12;
        for (unsigned i = 0; i < 40u && !km_ledstore_job_active(&g_m); i++) {
            (void)one_pass(true, true);
        }
        CHECK(km_ledstore_job_active(&g_m));
        CHECK(km_ledstore_request(&g_m, &rb));                        /* in the middle of the job */
        unsigned guard = 0;
        while (km_ledstore_job_active(&g_m) && guard++ < 50u) {
            (void)one_pass(true, true);
        }
        CHECK(loaded_is(&ra));                                        /* the job wrote what it was started with */
        CHECK(km_ledstore_pending(&g_m));                             /* and the newer request is still to do */
        drive(60, true, true);
        CHECK(!km_ledstore_pending(&g_m));
        CHECK(loaded_is(&rb));
    }

    /* ---- a request equal to what the flash holds writes nothing ---- */
    const km_ledstore_rec_t rnow = rec_of(402);                       /* what the flash holds now */
    CHECK(loaded_is(&rnow));
    const unsigned prog1 = g_sim.programs, er1 = g_sim.erases;
    const unsigned ok1 = g_m.saves_ok;
    CHECK(km_ledstore_request(&g_m, &rnow));
    drive(60, true, true);
    CHECK(!km_ledstore_pending(&g_m));
    CHECK_EQ(g_sim.programs, prog1);
    CHECK_EQ(g_sim.erases, er1);
    CHECK_EQ(g_m.saves_ok, ok1);
    CHECK_EQ(g_m.last, KM_CLOG_UNCHANGED);

    /* ---- an invalid request, and a manager without a log ---- */
    km_ledstore_rec_t bad = rec_of(5);
    bad.cfg.speed = 0;
    const uint32_t seq = g_m.req_seq;
    CHECK(!km_ledstore_request(&g_m, &bad));
    bad.cfg.speed = 5;
    bad.cfg.effect = 77;
    CHECK(!km_ledstore_request(&g_m, &bad));
    CHECK_EQ(g_m.req_seq, seq);
    CHECK(!km_ledstore_request(&g_m, NULL));
    km_ledstore_t none;
    km_ledstore_init(&none, NULL, NULL, NULL);
    CHECK(!km_ledstore_request(&none, &rnow));
    CHECK(!km_ledstore_pass(&none, 5, true, true));
    CHECK(!km_ledstore_pending(&none));
    km_ledstore_rec_t o;
    CHECK(!km_ledstore_boot(&none, &o));

    /* ---- a restart reads the newest record back ---- */
    mgr_boot();
    CHECK(g_m.loaded);
    CHECK(loaded_is(&rnow));

    /* ---- the hook runs after every flash operation (the integration restarts the scan chain there) ---- */
    sim_blank();
    g_hooks = 0;
    km_ledstore_init(&g_m, &g_log, hook_count, NULL);
    g_now = 5000;
    const km_ledstore_rec_t rh = rec_of(8);
    CHECK(km_ledstore_request(&g_m, &rh));
    const int ev0 = g_sim.ev;
    drive(60, true, true);
    CHECK(loaded_is(&rh));
    CHECK(g_hooks >= 3u && g_hooks <= (unsigned)(g_sim.ev - ev0) + 1u);   /* once per step: payload line, header line, read-back */
}

/* A save that has to erase (the sector is full) runs on without pausing once the erase is done: until its commit unit is in,
 * no record exists. A key that goes down in that window does not stop it; a key down BEFORE the erase does. */
static void test_must_finish(void)
{
    sim_blank();
    prefill(256);                                                     /* every slot used */
    const km_ledstore_rec_t pre = rec_of(255);
    CHECK(loaded_is(&pre));
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 1000;
    const km_ledstore_rec_t q = rec_of(900);
    CHECK(km_ledstore_request(&g_m, &q));
    (void)one_pass(true, true);                                       /* the settle time starts with this pass */
    g_now += 10;
    (void)one_pass(true, true);                                       /* begin: reads only */
    CHECK(km_ledstore_job_active(&g_m));
    CHECK(!km_ledstore_must_finish(&g_m));
    for (unsigned i = 0; i < 20u; i++) {
        CHECK(!one_pass(false, false));                               /* key down before the erase: paused, the old record is intact */
    }
    CHECK_EQ(g_sim.erases, 0);
    CHECK(loaded_is(&pre));
    CHECK(one_pass(true, false));                                     /* the erase */
    CHECK_EQ(g_sim.erases, 1);
    CHECK(km_ledstore_must_finish(&g_m));
    unsigned n = 0;
    while (km_ledstore_job_active(&g_m) && n < 50u) {
        CHECK(one_pass(false, false));                                /* a key is down now, the job goes on anyway */
        n++;
    }
    CHECK(!km_ledstore_job_active(&g_m));
    CHECK(!km_ledstore_must_finish(&g_m));
    CHECK(loaded_is(&q));
    CHECK_EQ(g_sim.violations, 0);

    /* with the integration's priority rule: urgent first */
    CHECK(!km_ledstore_must_finish(&g_m));
}

static void test_wear_and_giveup(void)
{
    /* 2000 saves, one request every 20 s: every request is saved (the gap and the settle time allow it), one erase per 256 records */
    sim_blank();
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 10000;
    for (unsigned i = 0; i < 2000u; i++) {
        const km_ledstore_rec_t r = rec_of(i);
        CHECK(km_ledstore_request(&g_m, &r));
        g_now += 20;
        drive(15, true, true);
        if (i % 97u == 0u) {
            CHECK(loaded_is(&r));
        }
    }
    CHECK_EQ(g_m.saves_ok, 2000);
    CHECK_EQ(g_m.saves_failed, 0);
    CHECK(g_sim.erases == 7u || g_sim.erases == 8u);                  /* 2000 / 256 */
    CHECK_EQ(g_sim.violations, 0);
    CHECK_EQ(g_multi_op_passes, 0);

    /* a host that asks as fast as it can: the number of save STARTS is bounded by the minimum gap */
    sim_blank();
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 20000;
    unsigned starts = 0;
    uint32_t last_start = g_m.last_start_s;
    for (unsigned s = 0; s < 600u; s++) {                             /* ten minutes, a request in every pass */
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            const km_ledstore_rec_t r = rec_of(s * 20u + p);
            (void)km_ledstore_request(&g_m, &r);
            (void)one_pass(true, true);
        }
        if (g_m.last_start_s != last_start) {
            last_start = g_m.last_start_s;
            starts++;
        }
    }
    CHECK_EQ(starts, 0);                                              /* never quiet for the settle time: nothing at all was written */
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    /* ... and with a pause of 4 s after every 5 s of requests: at most one start per 10 s */
    for (unsigned s = 0; s < 600u; s++) {
        g_now++;
        if (s % 9u < 5u) {
            const km_ledstore_rec_t r = rec_of(s);
            (void)km_ledstore_request(&g_m, &r);
        }
        for (unsigned p = 0; p < 20u; p++) {
            (void)one_pass(true, true);
        }
        if (g_m.last_start_s != last_start) {
            CHECK(g_m.last_start_s - last_start >= KM_LEDSTORE_MIN_GAP_S);
            last_start = g_m.last_start_s;
            starts++;
        }
    }
    CHECK(starts >= 10u && starts <= 60u);
    CHECK_EQ(g_sim.violations, 0);

    /* ---- failures: at most KM_LEDSTORE_MAX_TRIES attempts per request, then it gives up until the next request ---- */
    sim_blank();
    prefill(3);
    const km_ledstore_rec_t pre = rec_of(2);
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 500;
    const km_ledstore_rec_t q = rec_of(700);
    g_sim.fail_var = 0;
    CHECK(km_ledstore_request(&g_m, &q));
    unsigned guard = 0;
    while (km_ledstore_pending(&g_m) && guard++ < 2000u) {
        g_sim.fail_ev = g_sim.ev;                                     /* the next flash operation fails, whatever it is */
        drive(1, true, true);
    }
    CHECK(!km_ledstore_pending(&g_m));                                /* given up */
    CHECK(g_m.gave_up);
    CHECK_EQ(g_m.saves_failed, KM_LEDSTORE_MAX_TRIES);
    CHECK_EQ(g_m.saves_ok, 0);
    CHECK(g_m.last >= KM_CLOG_ERR_ARG);
    CHECK(loaded_is(&pre));                                           /* the old record is still the newest valid one */
    const unsigned ops_after_giveup = (unsigned)g_sim.ev;
    g_sim.fail_ev = -1;
    drive(100, true, true);                                           /* nothing more happens by itself */
    CHECK_EQ((unsigned)g_sim.ev, ops_after_giveup);
    CHECK(km_ledstore_request(&g_m, &q));                             /* a new request tries again, and now it works */
    CHECK(km_ledstore_pending(&g_m));
    drive(100, true, true);
    CHECK(!km_ledstore_pending(&g_m));
    CHECK(loaded_is(&q));
    CHECK_EQ(g_m.last, KM_CLOG_OK);
}

/* ---- power-cut and error sweeps ---- */

typedef struct {
    unsigned fill;          /* records already in the sector (rec_of(0..fill-1)); 0 with foreign = garbage instead */
    bool     foreign;
} start_t;

static void make_start(const start_t *s)
{
    sim_blank();
    if (s->foreign) {
        for (unsigned i = 0; i < SECT; i++) {
            g_sim.mem[i] = (uint8_t)mix(0x5EED0000u + i);
        }
    } else {
        prefill(s->fill);
    }
    g_sim.violations = 0;
    g_sim.ev = 0;
    g_start = g_sim;
}

static void restore_start(void)
{
    g_sim = g_start;
    g_sim.cut_ev = g_sim.fail_ev = g_sim.corrupt_ev = -1;
    g_sim.dead = false;
}

static unsigned save_one(const km_ledstore_rec_t *q)
{
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 7000;
    CHECK(km_ledstore_request(&g_m, q));
    const int e0 = g_sim.ev;
    drive(60, true, true);
    return (unsigned)(g_sim.ev - e0);
}

/* One of the records the sector held before (rec_of(0 .. fill-1)). */
static bool is_older(const km_ledstore_rec_t *got, unsigned fill)
{
    for (unsigned j = 0; j < fill; j++) {
        const km_ledstore_rec_t r = rec_of(j);
        if (rec_eq(got, &r)) {
            return true;
        }
    }
    return false;
}

static void cut_sweep(const start_t *s, unsigned *cuts)
{
    make_start(s);
    const km_ledstore_rec_t q = rec_of(5000);
    const unsigned n = save_one(&q);
    CHECK(n >= 3u && n <= 5u);                                        /* 3 to 4 units, +1 when the sector had to be erased */
    CHECK(loaded_is(&q));
    const bool erases = g_sim.erases > g_start.erases;
    const km_ledstore_rec_t pre = rec_of(s->fill > 0u ? s->fill - 1u : 0u);
    const bool has_pre = !s->foreign && s->fill > 0u;
    const km_ledstore_rec_t again = rec_of(6000);

    for (int ev = 0; ev < (int)n; ev++) {
        for (int var = 0; var < 5; var++) {
            restore_start();
            g_sim.cut_ev = g_sim.ev + ev;
            g_sim.cut_var = var;
            (void)save_one(&q);
            CHECK(g_sim.dead);
            (*cuts)++;
            /* power is back */
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            km_ledstore_rec_t got;
            const bool have = km_ledstore_load(&g_log, &got);
            if (have) {
                if (erases) {                                         /* a cut inside the erase can leave an OLDER saved record (km_calstore.h) */
                    CHECK(rec_eq(&got, &q) || is_older(&got, s->fill));
                } else {
                    CHECK(rec_eq(&got, &q) || (has_pre && rec_eq(&got, &pre)));
                }
            } else {
                CHECK(erases || !has_pre);                            /* "no record" only in a save that erases (or when there was none) */
            }
            /* the lights boot with a valid configuration either way */
            km_ledstore_rec_t eff = got;
            if (!have) {
                km_ledstore_rec_default(&eff);
            }
            CHECK(km_ledstore_rec_valid(&eff));
            /* and the next save works */
            km_ledstore_init(&g_m, &g_log, NULL, NULL);
            g_now = 90000;
            CHECK(km_ledstore_request(&g_m, &again));
            drive(60, true, true);
            CHECK(loaded_is(&again));
            CHECK_EQ(g_sim.violations, 0);
        }
    }
}

static void fail_sweep(const start_t *s, unsigned *fails)
{
    make_start(s);
    const km_ledstore_rec_t q = rec_of(5000);
    const unsigned n = save_one(&q);
    const km_ledstore_rec_t pre = rec_of(s->fill > 0u ? s->fill - 1u : 0u);
    const bool has_pre = !s->foreign && s->fill > 0u;
    const bool erases = g_sim.erases > g_start.erases;

    for (int ev = 0; ev < (int)n; ev++) {
        for (int var = 0; var < 5; var++) {
            /* the flash reports an error at this step (power stays on): the state afterwards is the old or the new record, and the
             * module retries and ends up with the new one */
            restore_start();
            g_sim.fail_ev = g_sim.ev + ev;
            g_sim.fail_var = var;
            km_ledstore_init(&g_m, &g_log, NULL, NULL);
            g_now = 7000;
            CHECK(km_ledstore_request(&g_m, &q));
            km_ledstore_rec_t got;
            bool seen = false;
            for (unsigned sec = 0; sec < 120u && km_ledstore_pending(&g_m); sec++) {
                drive(1, true, true);
                if (!seen && g_m.saves_failed > 0u) {
                    seen = true;
                    const bool have = km_ledstore_load(&g_log, &got);
                    if (have) {
                        if (erases) {
                            CHECK(rec_eq(&got, &q) || is_older(&got, s->fill));
                        } else {
                            CHECK(rec_eq(&got, &q) || (has_pre && rec_eq(&got, &pre)));
                        }
                    } else {
                        CHECK(erases || !has_pre);
                    }
                }
            }
            (*fails)++;
            CHECK(seen);
            CHECK(loaded_is(&q));                                     /* the retry succeeded */
            CHECK(g_m.saves_failed >= 1u && g_m.saves_failed < KM_LEDSTORE_MAX_TRIES);
            CHECK_EQ(g_sim.violations, 0);
        }

        /* a silent bit error: the read-back catches it, the bad slot is not taken for a record, the retry writes the next one */
        restore_start();
        g_sim.corrupt_ev = g_sim.ev + ev;
        km_ledstore_init(&g_m, &g_log, NULL, NULL);
        g_now = 7000;
        CHECK(km_ledstore_request(&g_m, &q));
        for (unsigned sec = 0; sec < 120u && km_ledstore_pending(&g_m); sec++) {
            drive(1, true, true);
        }
        (*fails)++;
        CHECK(loaded_is(&q));
        CHECK_EQ(g_sim.violations, 0);
    }
}

static void test_sweeps(void)
{
    static const start_t starts[] = {
        { 0, false }, { 1, false }, { 2, false }, { 100, false }, { 254, false }, { 255, false }, { 256, false }, { 0, true },
    };
    unsigned cuts = 0, fails = 0;
    for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        cut_sweep(&starts[i], &cuts);
        fail_sweep(&starts[i], &fails);
    }
    CHECK(cuts > 150u);
    CHECK(fails > 150u);
    printf("  ledstore: %u power-cut points and %u error points (every erase/program step x variants), 8 start states\n", cuts, fails);
}

/* A power cut at EVERY step of a long run of saves in a row (with erases in it): after each cut the record read back is one of the
 * last two saved. */
static void test_long_run_cuts(void)
{
    sim_blank();
    km_ledstore_init(&g_m, &g_log, NULL, NULL);
    g_now = 100;
    km_ledstore_rec_t prev = rec_of(0), cur = rec_of(0);
    bool have_prev = false;
    unsigned cuts = 0;
    for (unsigned i = 1; i <= 270u; i++) {
        cur = rec_of(i);
        /* try the cut at the first and the last step of this save (the middle ones are covered by the sweeps above) */
        g_start = g_sim;
        const unsigned e0 = (unsigned)g_sim.ev;
        CHECK(km_ledstore_request(&g_m, &cur));
        g_now += 20;
        drive(15, true, true);
        const unsigned n = (unsigned)g_sim.ev - e0;
        CHECK(n >= 3u && n <= 5u);
        const bool erased_here = g_sim.erases > g_start.erases;
        const sim_t after = g_sim;
        const km_ledstore_t mgr_after = g_m;
        for (unsigned which = 0; which < 2u; which++) {
            g_sim = g_start;
            g_sim.cut_ev = g_sim.ev + (int)(which == 0u ? 0u : n - 1u);
            g_sim.cut_var = (int)(i % 5u);
            km_ledstore_init(&g_m, &g_log, NULL, NULL);
            g_m.started_once = false;
            g_now += 1000;
            CHECK(km_ledstore_request(&g_m, &cur));
            drive(60, true, true);
            CHECK(g_sim.dead);
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            km_ledstore_rec_t got;
            if (km_ledstore_load(&g_log, &got)) {
                CHECK(rec_eq(&got, &cur) || (have_prev && rec_eq(&got, &prev)));
            } else {
                CHECK(erased_here || !have_prev);
            }
            cuts++;
        }
        g_sim = after;                                                /* the run goes on without the accident */
        g_m = mgr_after;
        CHECK(loaded_is(&cur));
        prev = cur;
        have_prev = true;
    }
    CHECK_EQ(g_sim.violations, 0);
    printf("  ledstore: %u extra cuts in a run of 270 saves (one erase inside)\n", cuts);
}

/* ---- the glue in m5_led.c: extended ops, attach, status ---- */

static km_cfg_status_t ex_get(uint8_t v[KM_CONFIG_LED_GET_EX_BYTES])
{
    const km_config_led_ex_ops_t *ex = m5_led_cfg_ex_ops();
    return ex->get(ex->ctx, v);
}

static km_cfg_status_t ex_set(const km_led_cfg_t *c, unsigned idle_s, uint8_t flags)
{
    const km_config_led_ex_ops_t *ex = m5_led_cfg_ex_ops();
    const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES] = { c->effect, c->brightness, c->speed, c->color.r, c->color.g, c->color.b,
                                                     (uint8_t)idle_s, (uint8_t)(idle_s >> 8), flags };
    return ex->set(ex->ctx, v);
}

km_led_t *m5_led_test_core(void);          /* m5_led.c, host tests only */

static void test_glue(void)
{
    sim_blank();
    g_now = 300;
    km_led_t *core = m5_led_test_core();

    /* a build without persistence (km_l41*): a save request is refused and changes nothing; the plain set works */
    m5_led_init();
    m5_led_persist_attach(NULL, NULL, NULL);
    uint8_t v[KM_CONFIG_LED_GET_EX_BYTES];
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[0], KM_LED_FX_DEMO);                                   /* the defaults */
    CHECK_EQ(v[1], 255);
    CHECK_EQ(v[2], 128);
    CHECK_EQ(v[6] | (v[7] << 8), 600);
    CHECK_EQ(v[8], 0);                                                /* no flag: not persistent, nothing pending, not loaded, not idle */
    const km_led_cfg_t c1 = { KM_LED_FX_STATIC, 100, 50, { 1, 2, 3 } };
    CHECK_EQ(ex_set(&c1, 30, KM_CFG_LED_FLAG_SAVE), KM_CFG_ERR_UNSUPPORTED);
    km_led_cfg_t now;
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_DEMO);                             /* nothing applied */
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[6] | (v[7] << 8), 600);
    CHECK_EQ(ex_set(&c1, 30, 0), KM_CFG_OK);                          /* RAM only is fine */
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_STATIC);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[6] | (v[7] << 8), 30);
    CHECK(!m5_led_persist_pass(1000, true, true));

    /* with persistence: nothing in flash -> defaults, flags say persistent and not loaded */
    m5_led_init();
    m5_led_persist_attach(&g_log, NULL, NULL);
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_DEMO);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8], KM_CFG_LED_ST_PERSISTENT);
    CHECK_EQ(v[9], 0);

    /* invalid values change nothing, whatever the flags */
    const km_led_cfg_t bad_fx = { KM_LED_FX_COUNT, 1, 1, { 0, 0, 0 } }, bad_speed = { KM_LED_FX_STATIC, 1, 0, { 0, 0, 0 } };
    CHECK_EQ(ex_set(&bad_fx, 5, KM_CFG_LED_FLAG_SAVE), KM_CFG_ERR_RANGE);
    CHECK_EQ(ex_set(&bad_speed, 5, 0), KM_CFG_ERR_RANGE);
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_DEMO);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, 0);

    /* a live set (no save flag) never asks for a save */
    CHECK_EQ(ex_set(&c1, 45, 0), KM_CFG_OK);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, 0);
    for (unsigned s = 0; s < 30u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            CHECK(!m5_led_persist_pass(g_now, true, true));
        }
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);

    /* the legacy six-byte set does not touch the idle time, the save state or flash */
    const km_config_led_ops_t *legacy = m5_led_cfg_ops();
    const uint8_t six[6] = { KM_LED_FX_RAINBOW, 77, 66, 5, 6, 7 };
    CHECK_EQ(legacy->set(legacy->ctx, six), KM_CFG_OK);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[0], KM_LED_FX_RAINBOW);
    CHECK_EQ(v[6] | (v[7] << 8), 45);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, 0);
    for (unsigned s = 0; s < 30u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            CHECK(!m5_led_persist_pass(g_now, true, true));
        }
    }
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);

    /* every change from the host counts as activity: the lights come back and the idle clock restarts (plain set, extended set, API) */
    core->activity = false;
    CHECK_EQ(legacy->set(legacy->ctx, six), KM_CFG_OK);
    CHECK(core->activity);
    core->activity = false;
    CHECK_EQ(ex_set(&c1, 45, 0), KM_CFG_OK);
    CHECK(core->activity);
    core->activity = false;
    CHECK(m5_led_set_cfg(&c1));
    CHECK(core->activity);
    core->activity = false;
    CHECK_EQ(ex_set(&bad_fx, 45, 0), KM_CFG_ERR_RANGE);               /* a refused change is no activity */
    CHECK(!core->activity);

    /* the status bit follows the lights: idle after the idle time, off again the moment a key comes */
    {
        const km_led_t saved = *core;
        CHECK_EQ(ex_set(&c1, 2, 0), KM_CFG_OK);                       /* 2 s */
        uint32_t t = 0;
        for (unsigned i = 0; i < 100u; i++) {                         /* 2 s of nothing: still lit */
            t += 20;
            (void)km_led_work(core, t, NULL, NULL, 1000);
            if (km_led_frame(core) != NULL) {
                km_led_frame_taken(core);
            }
        }
        CHECK_EQ(ex_get(v), KM_CFG_OK);
        CHECK_EQ(v[8] & KM_CFG_LED_ST_IDLE_OFF, 0);
        for (unsigned i = 0; i < 200u; i++) {
            t += 20;
            (void)km_led_work(core, t, NULL, NULL, 1000);
            if (km_led_frame(core) != NULL) {
                km_led_frame_taken(core);
            }
        }
        CHECK_EQ(ex_get(v), KM_CFG_OK);
        CHECK_EQ(v[8] & KM_CFG_LED_ST_IDLE_OFF, KM_CFG_LED_ST_IDLE_OFF);
        const uint8_t key[1] = { 5 };
        km_led_note_keys(core, key, 1);
        for (unsigned i = 0; i < 20u; i++) {
            t += 20;
            (void)km_led_work(core, t, NULL, NULL, 1000);
            if (km_led_frame(core) != NULL) {
                km_led_frame_taken(core);
            }
        }
        CHECK_EQ(ex_get(v), KM_CFG_OK);
        CHECK_EQ(v[8] & KM_CFG_LED_ST_IDLE_OFF, 0);
        *core = saved;
        CHECK_EQ(ex_set(&c1, 45, 0), KM_CFG_OK);
    }

    /* a save request: pending, then in flash after the quiet time, status follows */
    const km_led_cfg_t c2 = { KM_LED_FX_BREATH, 180, 99, { 10, 20, 30 } };
    CHECK_EQ(ex_set(&c2, 1200, KM_CFG_LED_FLAG_SAVE), KM_CFG_OK);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, KM_CFG_LED_ST_SAVE_PENDING);
    CHECK_EQ(v[0], KM_LED_FX_BREATH);                                 /* applied at once */
    CHECK_EQ(g_sim.programs + g_sim.erases, 0);
    unsigned flash_passes = 0;
    for (unsigned s = 0; s < 40u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            flash_passes += m5_led_persist_pass(g_now, true, true) ? 1u : 0u;
        }
    }
    CHECK(flash_passes >= 3u);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, 0);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_FAILED, 0);
    CHECK_EQ(v[9], KM_CLOG_OK);
    CHECK_EQ(v[10], 1);
    CHECK_EQ(v[11], 0);
    km_ledstore_rec_t flash;
    CHECK(km_ledstore_load(&g_log, &flash));
    CHECK_EQ(flash.cfg.effect, KM_LED_FX_BREATH);
    CHECK_EQ(flash.cfg.brightness, 180);
    CHECK_EQ(flash.cfg.speed, 99);
    CHECK_EQ(flash.cfg.color.g, 20);
    CHECK_EQ(flash.idle_s, 1200);

    /* the "restart": the record comes back (settings, colour, idle time), the status says loaded, nothing is pending, no flash traffic */
    const unsigned ops_before = g_sim.programs + g_sim.erases;
    m5_led_init();
    m5_led_persist_attach(&g_log, NULL, NULL);
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_BREATH);
    CHECK_EQ(now.brightness, 180);
    CHECK_EQ(now.speed, 99);
    CHECK_EQ(now.color.r, 10);
    CHECK_EQ(now.color.b, 30);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[6] | (v[7] << 8), 1200);
    CHECK_EQ(v[8], KM_CFG_LED_ST_PERSISTENT | KM_CFG_LED_ST_LOADED);
    CHECK_EQ(g_sim.programs + g_sim.erases, ops_before);

    /* ... and a damaged record boots the defaults: wipe the sector with noise, restart */
    for (unsigned i = 0; i < SECT; i++) {
        g_sim.mem[i] = (uint8_t)mix(i);
    }
    m5_led_init();
    m5_led_persist_attach(&g_log, NULL, NULL);
    m5_led_get_cfg(&now);
    CHECK_EQ(now.effect, KM_LED_FX_DEMO);
    CHECK_EQ(now.brightness, 255);
    CHECK_EQ(now.speed, 128);
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[6] | (v[7] << 8), 600);
    CHECK_EQ(v[8], KM_CFG_LED_ST_PERSISTENT);
    /* the save after that first erases the garbage and works; between the erase and its end it is urgent (it must be given every pass) */
    CHECK_EQ(ex_set(&c2, 0, KM_CFG_LED_FLAG_SAVE), KM_CFG_OK);        /* idle 0 = never */
    g_now += 100;
    bool urgent_seen = false;
    for (unsigned s = 0; s < 40u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            (void)m5_led_persist_pass(g_now, true, true);
            urgent_seen = urgent_seen || m5_led_persist_urgent();
        }
    }
    CHECK(urgent_seen);
    CHECK(!m5_led_persist_urgent());
    CHECK(km_ledstore_load(&g_log, &flash));
    CHECK_EQ(flash.idle_s, 0);
    CHECK_EQ(g_sim.erases, 1);

    /* a flash that keeps failing: the status says so (failed bit, result code, failure counter), the lights keep their configuration */
    m5_led_init();
    m5_led_persist_attach(&g_log, NULL, NULL);
    const km_led_cfg_t c3 = { KM_LED_FX_REACTIVE, 60, 70, { 11, 22, 33 } };
    CHECK_EQ(ex_set(&c3, 99, KM_CFG_LED_FLAG_SAVE), KM_CFG_OK);
    for (unsigned s = 0; s < 120u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            g_sim.fail_ev = g_sim.ev;                                 /* the next flash operation fails */
            (void)m5_led_persist_pass(g_now, true, true);
        }
    }
    g_sim.fail_ev = -1;
    CHECK_EQ(ex_get(v), KM_CFG_OK);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_FAILED, KM_CFG_LED_ST_SAVE_FAILED);
    CHECK_EQ(v[8] & KM_CFG_LED_ST_SAVE_PENDING, 0);                   /* gave up: not pending for ever */
    CHECK(v[9] >= KM_CLOG_ERR_ARG);
    CHECK_EQ(v[11], KM_LEDSTORE_MAX_TRIES);
    CHECK_EQ(v[0], KM_LED_FX_REACTIVE);                               /* still applied in RAM */
    CHECK_EQ(v[6] | (v[7] << 8), 99);

    /* the after-flash hook is passed on */
    g_hooks = 0;
    m5_led_init();
    m5_led_persist_attach(&g_log, hook_count, NULL);
    CHECK_EQ(ex_set(&c1, 77, KM_CFG_LED_FLAG_SAVE), KM_CFG_OK);
    g_now += 100;
    for (unsigned s = 0; s < 40u; s++) {
        g_now++;
        for (unsigned p = 0; p < 20u; p++) {
            (void)m5_led_persist_pass(g_now, true, true);
        }
    }
    CHECK(g_hooks >= 3u);
    CHECK(!m5_led_persist_urgent());
    m5_led_persist_attach(NULL, NULL, NULL);                          /* leave the module as the other tests expect it */
    CHECK_EQ(g_sim.violations, 0);
}

void test_ledstore(void)
{
    test_record();
    test_load();
    test_manager();
    test_must_finish();
    test_wear_and_giveup();
    test_sweeps();
    test_long_run_cuts();
    test_glue();
}
