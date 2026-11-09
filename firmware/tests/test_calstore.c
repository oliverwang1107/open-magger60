/*
 * km_calstore (append-only log in the calibration sector) and km_calib (record, restore, save policy) on a simulated
 * flash that can lose power after any single erase/program step.
 *
 * The flash model is the one of test_store.c (read the comment there): 8 KB erase, 8-byte program units that are only
 * programmed while erased, a power cut leaves the worst plausible partial effect of the step in progress
 *   program  v0 nothing, v1 low word done / high word not, v2 low word done / high word half-programmed,
 *            v3 low word half-programmed
 *   erase    v0 nothing, v1 first half erased, v2 whole sector garbage, v3 second half erased,
 *            v4 first half erased / second half garbage
 * Here the flash is the ONE calibration sector 0x08034000 and every access is also checked against km_flash_guard.h.
 *
 * Properties proven by the sweep (every append of a 50-record run, every erase/program step of it, every variant):
 *   - after the cut the log loads the previous record or the new one, never anything else; the single exception is the
 *     append that has to ERASE the full sector: there an older record or no record at all is also accepted (documented
 *     in km_calstore.h; the calibration record is derived data);
 *   - the next append after the accident succeeds and reads back exactly;
 *   - no unit is ever programmed twice between erases, no access leaves the sector, no guard breach.
 */
#include "km/km_calib.h"
#include "km/km_calmgr.h"
#include "km/km_config.h"
#include "km/km_vendor.h"
#include "km/km_calstore.h"
#include "km/km_flash_guard.h"
#include "km/km_keymap.h"
#include "km_test.h"

#define BASE KM_FLASH_CALIBRATION
#define SECT 0x2000u
#define PL   KM_CAL_PAYLOAD_BYTES
#define NPAY 64u

typedef struct {
    uint8_t  mem[SECT];
    bool     dead;                  /* power is gone: nothing changes any more */
    int      ev;                    /* erase/program steps so far */
    int      cut_ev, cut_var;       /* power cut during step cut_ev with this variant, -1 = never */
    int      fail_ev, fail_var;     /* step reports an error (power stays on) after the partial effect of fail_var */
    int      corrupt_ev;            /* step reports success but stores one bit wrong */
    unsigned violations;
    unsigned erases, programs;
    int      last_erase_ev;
} sim_t;

static sim_t g_sim, g_start;
static km_clog_t g_log;

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
    s->last_erase_ev = ev;
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
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Write a finished record straight into slot i, from the documented layout (independent of km_calstore.c). */
static void forge(unsigned slot, uint32_t seq, const uint8_t *data)
{
    uint8_t *r = &g_sim.mem[slot * g_log.stride];
    memset(r, 0xFF, g_log.stride);
    put32(r + 0, 0x4C434D4Bu);                       /* "KMCL" */
    put32(r + 8, seq);
    put32(r + 12, PL);
    memcpy(r + 16, data, PL);
    put32(r + 4, km_crc32(0, r + 8, 8 + PL));
}

static uint8_t pay[NPAY][PL];

static void make_payloads(void)
{
    for (unsigned i = 0; i < NPAY; i++) {
        for (unsigned j = 0; j < PL; j++) {
            pay[i][j] = (uint8_t)mix(i * 1000u + j);
        }
        pay[i][0] = (uint8_t)i;                      /* always distinct */
        if (i % 3 == 0) {
            memset(&pay[i][16], 0xFF, 24);           /* all-0xFF units: the writer skips them */
        }
        if (i % 5 == 0) {
            memset(&pay[i][PL - 16], 0xFF, 16);      /* ... also at the end */
        }
    }
}

/* index of the payload the log loads: -1 none, -2 something that is none of ours */
static int load_idx(void)
{
    uint8_t buf[PL];
    if (!km_clog_read(&g_log, buf, sizeof buf)) {
        return -1;
    }
    for (unsigned i = 0; i < NPAY; i++) {
        if (memcmp(buf, pay[i], PL) == 0) {
            return (int)i;
        }
    }
    return -2;
}

static void test_basic(void)
{
    km_clog_t t;
    const km_store_ops_t ops = { &g_sim, sim_read, sim_erase, sim_program };
    CHECK(!km_clog_init(&t, &ops, BASE, 0));
    CHECK(!km_clog_init(&t, &ops, BASE, KM_CLOG_MAX_PAYLOAD + 1));
    CHECK(!km_clog_init(&t, &ops, BASE + 8, PL));
    CHECK(!km_clog_init(&t, NULL, BASE, PL));
    CHECK(km_clog_init(&t, &ops, BASE, KM_CLOG_MAX_PAYLOAD));
    CHECK_EQ(t.slots, 15);                           /* 528 bytes per slot */

    sim_blank();
    CHECK_EQ(g_log.stride, 336);                     /* 16 + 312 rounded up to a flash line (16 bytes) */
    CHECK_EQ(g_log.stride % 16, 0);
    CHECK_EQ(g_log.slots, 24);
    km_clog_scan_t sc;
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(!sc.valid && sc.next == 0 && sc.used == 0 && sc.dirty == 0);
    CHECK_EQ(load_idx(), -1);

    /* 24 records fill the sector without a single erase */
    for (unsigned i = 0; i < 24; i++) {
        CHECK(km_clog_append(&g_log, pay[i]) == KM_CLOG_OK);
        CHECK(km_clog_scan(&g_log, &sc));
        CHECK(sc.valid && sc.index == i && sc.seq == i + 1 && sc.next == i + 1 && sc.used == i + 1 && sc.dirty == 0);
        CHECK_EQ(load_idx(), (int)i);
    }
    CHECK_EQ(g_sim.erases, 0);
    CHECK_EQ(g_sim.violations, 0);

    /* the same payload again writes nothing */
    const int ev = g_sim.ev;
    CHECK(km_clog_append(&g_log, pay[23]) == KM_CLOG_UNCHANGED);
    CHECK_EQ(g_sim.ev, ev);
    km_clog_job_t job;
    CHECK(km_clog_job_begin(&job, &g_log, pay[23]) == KM_CLOG_UNCHANGED);
    CHECK(!km_clog_job_active(&job));

    /* the 25th record needs the erase, then starts at slot 0 with the next sequence number */
    CHECK(km_clog_job_begin(&job, &g_log, pay[24]) == KM_CLOG_PENDING);
    CHECK(km_clog_job_active(&job) && km_clog_job_next_is_erase(&job));
    CHECK(km_clog_job_step(&job) == KM_CLOG_PENDING);
    CHECK(!km_clog_job_next_is_erase(&job));
    km_clog_result_t r = KM_CLOG_PENDING;
    while (r == KM_CLOG_PENDING) {
        r = km_clog_job_step(&job);
    }
    CHECK(r == KM_CLOG_OK);
    CHECK(!km_clog_job_active(&job));
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.valid && sc.index == 0 && sc.seq == 25 && sc.next == 1 && sc.used == 1);
    CHECK_EQ(g_sim.erases, 1);

    /* wear: 1000 more records, one erase per 24 */
    for (unsigned i = 25; i < 1025; i++) {
        pay[40][1] = (uint8_t)i;
        pay[40][2] = (uint8_t)(i >> 8);
        CHECK(km_clog_append(&g_log, pay[40]) == KM_CLOG_OK);
    }
    CHECK_EQ(g_sim.erases, 1 + 1000 / 24 + (1000 % 24 > 23 ? 1 : 0));   /* 25th..1024th record: erases at 49, 73, ... */
    CHECK_EQ(g_sim.violations, 0);

    /* argument errors */
    CHECK(km_clog_job_begin(&job, &g_log, NULL) == KM_CLOG_ERR_ARG);
    CHECK(km_clog_job_begin(NULL, &g_log, pay[0]) == KM_CLOG_ERR_ARG);
    CHECK(km_clog_job_step(&job) == KM_CLOG_ERR_ARG);                    /* idle job */
    CHECK(!km_clog_read(&g_log, (uint8_t *)pay[1], PL - 1));
    CHECK(!km_clog_read(&g_log, NULL, PL));
}

static void test_states(void)
{
    km_clog_scan_t sc;

    /* foreign content everywhere: no record, the first save erases and replaces it */
    sim_blank();
    for (uint32_t i = 0; i < SECT; i++) {
        g_sim.mem[i] = (uint8_t)mix(7u + i);
    }
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(!sc.valid && sc.dirty == 24 && sc.next == 24);
    CHECK_EQ(load_idx(), -1);
    CHECK(km_clog_append(&g_log, pay[1]) == KM_CLOG_OK);
    CHECK_EQ(g_sim.erases, 1);
    CHECK_EQ(load_idx(), 1);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.index == 0 && sc.seq == 1);

    /* all zeros (what a wiped region can look like), and fully erased */
    memset(g_sim.mem, 0x00, SECT);
    CHECK_EQ(load_idx(), -1);
    memset(g_sim.mem, 0xFF, SECT);
    CHECK_EQ(load_idx(), -1);

    /* foreign data only at the start: the blank tail is used, no erase */
    sim_blank();
    for (uint32_t i = 0; i < 3 * g_log.stride - 5; i++) {
        g_sim.mem[i] = (uint8_t)mix(99u + i);
    }
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(!sc.valid && sc.dirty == 3 && sc.next == 3);
    CHECK(km_clog_append(&g_log, pay[2]) == KM_CLOG_OK);
    CHECK_EQ(g_sim.erases, 0);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.valid && sc.index == 3 && sc.seq == 1 && sc.dirty == 3);

    /* one stray bit in the padding of a slot makes it dirty and is never programmed over */
    sim_blank();
    forge(0, 1, pay[0]);
    g_sim.mem[3 * g_log.stride + g_log.stride - 3] = 0x7F;
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.valid && sc.index == 0 && sc.used == 1 && sc.dirty == 1 && sc.next == 4);
    CHECK(km_clog_append(&g_log, pay[1]) == KM_CLOG_OK);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.index == 4 && sc.seq == 2);
    CHECK_EQ(g_sim.violations, 0);

    /* payload written, commit unit missing (a cut at the last moment): dirty, skipped */
    sim_blank();
    forge(0, 1, pay[0]);
    forge(1, 2, pay[1]);
    memset(&g_sim.mem[g_log.stride], 0xFF, 8);       /* erase the commit unit of slot 1 */
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.valid && sc.index == 0 && sc.dirty == 1 && sc.next == 2);
    CHECK_EQ(load_idx(), 0);

    /* the newest is decided by sequence number, not by position; the next slot follows the LAST used one */
    sim_blank();
    forge(0, 9, pay[3]);
    forge(1, 4, pay[2]);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.index == 0 && sc.seq == 9 && sc.next == 2);
    CHECK_EQ(load_idx(), 3);
    CHECK(km_clog_append(&g_log, pay[4]) == KM_CLOG_OK);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.index == 2 && sc.seq == 10);

    /* sequence wrap: 0 is newer than 0xFFFFFFFF */
    sim_blank();
    forge(0, 0xFFFFFFFFu, pay[5]);
    forge(1, 0, pay[6]);
    CHECK_EQ(load_idx(), 6);
    CHECK(km_clog_append(&g_log, pay[7]) == KM_CLOG_OK);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.seq == 1 && sc.index == 2);

    /* a valid-looking header with the wrong length is not a record */
    sim_blank();
    forge(0, 1, pay[8]);
    put32(&g_sim.mem[12], PL + 8);
    CHECK_EQ(load_idx(), -1);

    /* bit rot in the newest payload: the previous record is the newest valid one; the rotted slot is dirty */
    sim_blank();
    CHECK(km_clog_append(&g_log, pay[9]) == KM_CLOG_OK);
    CHECK(km_clog_append(&g_log, pay[10]) == KM_CLOG_OK);
    g_sim.mem[g_log.stride + 16 + 100] ^= 0x04;
    CHECK_EQ(load_idx(), 9);
    CHECK(km_clog_scan(&g_log, &sc));
    CHECK(sc.dirty == 1 && sc.next == 2);
    g_sim.mem[16 + 100] ^= 0x04;
    CHECK_EQ(load_idx(), -1);
    CHECK_EQ(g_sim.violations, 0);
}

/* ---- the power-cut sweep ---- */

static unsigned g_cut_points;

/* the flash as it was before the append under test, with the step counter and the fault injection reset */
static void restore_start(void)
{
    memcpy(&g_sim, &g_start, sizeof g_sim);
    g_sim.ev = 0;
    g_sim.violations = 0;
    g_sim.dead = false;
    g_sim.cut_ev = g_sim.fail_ev = g_sim.corrupt_ev = -1;
}

typedef struct { int old_idx, new_idx; bool erase_case; } sweep_t;

static bool outcome_ok(int got, const sweep_t *w)
{
    if (got == w->old_idx || got == w->new_idx) {
        return true;
    }
    /* the append that had to erase the full sector: an older record or none is allowed too (never garbage) */
    return w->erase_case && (got == -1 || (got >= 0 && got < w->new_idx));
}

static bool run_job(const uint8_t *data, km_clog_result_t *res)
{
    km_clog_job_t job;
    km_clog_result_t r = km_clog_job_begin(&job, &g_log, data);
    while (r == KM_CLOG_PENDING) {
        r = km_clog_job_step(&job);
    }
    *res = r;
    return r == KM_CLOG_OK;
}

/* g_start holds the flash before the append of pay[new_idx]. */
static void cut_everywhere(const char *name, const sweep_t *w, unsigned *seen_old, unsigned *seen_new)
{
    km_clog_result_t r;
    restore_start();
    CHECK(run_job(pay[w->new_idx], &r));
    CHECK_EQ(g_sim.violations, 0);
    CHECK_EQ(load_idx(), w->new_idx);
    const int steps = g_sim.ev;
    CHECK(steps >= 30);
    unsigned bad = 0;
    for (int ev = 0; ev < steps; ev++) {
        const bool is_erase = w->erase_case && ev == 0;
        const int nvar = is_erase ? 5 : 4;
        for (int v = 0; v < nvar; v++) {
            restore_start();
            g_sim.cut_ev = ev;
            g_sim.cut_var = v;
            const bool done = run_job(pay[w->new_idx], &r);
            if (done || !g_sim.dead || g_sim.violations != 0) {
                bad++;
                printf("  FAIL %s: cut at step %d variant %d: done=%d dead=%d violations=%u\n", name, ev, v, done, g_sim.dead, g_sim.violations);
            }
            g_cut_points++;
            /* power is back */
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            const int got = load_idx();
            if (!outcome_ok(got, w)) {
                bad++;
                printf("  FAIL %s: cut at step %d variant %d loads %d (old %d new %d)\n", name, ev, v, got, w->old_idx, w->new_idx);
            }
            *seen_old += (got == w->old_idx);
            *seen_new += (got == w->new_idx);
            /* the next save after the accident works and reads back exactly */
            const int nxt = w->new_idx + 1;
            if (!run_job(pay[nxt], &r) || load_idx() != nxt || g_sim.violations != 0) {
                bad++;
                printf("  FAIL %s: follow-up save after cut at step %d variant %d (result %d)\n", name, ev, v, (int)r);
            }
        }
    }
    CHECK_EQ(bad, 0);
}

/* an error (power stays on) or a silent bit error at each step: the previous record must stay the newest valid one */
static void fail_everywhere(const char *name, const sweep_t *w)
{
    km_clog_result_t r;
    restore_start();
    CHECK(run_job(pay[w->new_idx], &r));
    const int steps = g_sim.ev;
    unsigned bad = 0;
    for (int ev = 0; ev < steps; ev++) {
        const bool is_erase = w->erase_case && ev == 0;
        for (int mode = 0; mode < 3; mode++) {      /* 0 clean error, 1 error after half a unit, 2 silent corruption */
            if (mode == 2 && is_erase) {
                continue;
            }
            restore_start();
            if (mode == 2) {
                g_sim.corrupt_ev = ev;
            } else {
                g_sim.fail_ev = ev;
                g_sim.fail_var = (mode == 0) ? 0 : (is_erase ? 4 : 2);
            }
            run_job(pay[w->new_idx], &r);
            km_clog_result_t want = mode == 2 ? KM_CLOG_ERR_VERIFY : (is_erase ? KM_CLOG_ERR_ERASE : KM_CLOG_ERR_PROGRAM);
            if (r != want) {
                bad++;
                printf("  FAIL %s: step %d mode %d returned %d, want %d\n", name, ev, mode, (int)r, (int)want);
            }
            g_sim.fail_ev = g_sim.corrupt_ev = -1;
            const int got = load_idx();
            /* once the sector is erased the old record is gone until the new one is committed: any step of the erase case */
            const bool ok = w->erase_case ? outcome_ok(got, w) : (got == w->old_idx);
            if (!ok) {
                bad++;
                printf("  FAIL %s: step %d mode %d left %d (old %d)\n", name, ev, mode, got, w->old_idx);
            }
            if (g_sim.violations != 0) {
                bad++;
            }
            /* and the keyboard can save again afterwards */
            if (!run_job(pay[w->new_idx + 1], &r) || load_idx() != w->new_idx + 1) {
                bad++;
                printf("  FAIL %s: step %d mode %d: follow-up save failed (%d)\n", name, ev, mode, (int)r);
            }
        }
    }
    CHECK_EQ(bad, 0);
}

static void test_sweeps(void)
{
    unsigned seen_old = 0, seen_new = 0;
    g_cut_points = 0;
    sim_blank();
    km_clog_result_t r;
    /* a run of 50 records: before each one, sweep every cut point of its append (n = 24, 48 have to erase) */
    for (int n = 0; n < 50; n++) {
        memcpy(&g_start, &g_sim, sizeof g_sim);
        char name[40];
        sprintf(name, "append %d", n);
        sweep_t w = { n - 1, n, n > 0 && n % 24 == 0 };
        cut_everywhere(name, &w, &seen_old, &seen_new);
        fail_everywhere(name, &w);
        memcpy(&g_sim, &g_start, sizeof g_sim);
        CHECK(run_job(pay[n], &r));
    }
    CHECK(seen_old > 1000);                          /* the sweep really saw "still the old record" (the new one exists only after the last program) */

    /* the same from foreign content: the first save erases */
    sim_blank();
    for (uint32_t i = 0; i < SECT; i++) {
        g_sim.mem[i] = (uint8_t)mix(31u + i);
    }
    memcpy(&g_start, &g_sim, sizeof g_sim);
    sweep_t first = { -1, 1, true };
    cut_everywhere("first save over foreign data", &first, &seen_old, &seen_new);
    fail_everywhere("first save over foreign data", &first);

    /* the same with a torn slot already in the way (an earlier accident), and a full sector whose last slot is torn */
    sim_blank();
    CHECK(run_job(pay[0], &r));
    CHECK(run_job(pay[1], &r));
    memset(&g_sim.mem[2 * g_log.stride], 0xAA, 100);                  /* slot 2: half written */
    memcpy(&g_start, &g_sim, sizeof g_sim);
    sweep_t torn = { 1, 2, false };
    cut_everywhere("torn slot in the way", &torn, &seen_old, &seen_new);
    fail_everywhere("torn slot in the way", &torn);

    sim_blank();
    for (int i = 0; i < 23; i++) {
        CHECK(run_job(pay[i], &r));
    }
    memset(&g_sim.mem[23 * g_log.stride + 40], 0x00, 8);              /* last slot torn */
    memcpy(&g_start, &g_sim, sizeof g_sim);
    sweep_t lastbad = { 22, 23, true };
    cut_everywhere("last slot torn", &lastbad, &seen_old, &seen_new);
    fail_everywhere("last slot torn", &lastbad);

    printf("  calstore: %u power-cut points (every erase/program step x partial-effect variants), 50 consecutive appends + 3 start states\n",
           g_cut_points);
    CHECK(g_cut_points > 8000);
}

/* ---- the flash guard: nothing but the calibration sector is reachable from the log ---- */

static void test_guard(void)
{
    sim_blank();
    km_clog_result_t r;
    for (int i = 0; i < 60; i++) {
        CHECK(run_job(pay[i], &r));
    }
    CHECK_EQ(g_sim.violations, 0);                                   /* every access passed km_flash_store_allowed and the sector range */
    CHECK(g_sim.erases >= 2);
    /* a log aimed at the neighbouring sectors still cannot touch anything outside the six store sectors */
    static const uint32_t forbidden[] = { 0x0803C000u, 0x0802E000u, 0x0802C000u, 0x08028000u, 0x08008000u, 0x08000000u };
    for (unsigned i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
        CHECK(!km_flash_store_allowed(forbidden[i], 8));
        CHECK(!km_flash_store_allowed(forbidden[i], KM_CLOG_SECTOR_BYTES));
    }
    CHECK(km_flash_store_allowed(KM_FLASH_CALIBRATION, KM_CLOG_SECTOR_BYTES));
    CHECK(!km_flash_store_allowed(KM_FLASH_CALIBRATION + 1, KM_CLOG_SECTOR_BYTES));
    CHECK(!km_flash_store_allowed(KM_FLASH_CALIBRATION + KM_CLOG_SECTOR_BYTES + 1, KM_CLOG_SECTOR_BYTES));   /* may not run into the next store sector */
    CHECK(km_flash_store_allowed(KM_FLASH_LED_CONFIG, KM_CLOG_SECTOR_BYTES));                 /* the light configuration sector is the next one */
    CHECK(km_flash_store_allowed(KM_FLASH_LED_CONFIG + KM_CLOG_SECTOR_BYTES, 8));             /* scene A follows it (km_lightscene.h) */
    CHECK(!km_flash_store_allowed(KM_FLASH_SCENE_B + KM_CLOG_SECTOR_BYTES, 8));               /* and nothing after the scene sectors */
}

/* ---- km_calib: record, engine glue, policy ---- */

static uint16_t key_base(unsigned s) { return (uint16_t)(3150u + (s * 7u) % 250u); }
static uint16_t key_span(unsigned s) { return (uint16_t)(1050u + (s * 13u) % 300u); }

static void live_frame(uint16_t live[KM_ENGINE_SLOTS], int shift)
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        live[s] = km_keymap_slot_is_key(s) ? (uint16_t)((int)key_base(s) + shift) : 3300u;
    }
}

static void calibrate_normally(km_engine_t *e, int shift)
{
    uint16_t live[KM_ENGINE_SLOTS];
    live_frame(live, shift);
    km_engine_init(e);
    km_engine_calibrate_begin(e);
    for (unsigned i = 0; i < 64; i++) {
        km_engine_calibrate_idle(e, live);
    }
    CHECK(km_engine_calibrate_finish(e) == KM_ENGINE_CAL_OK);
}

/* Press the keys in groups of four (a group of 8 at once would trip the glitch guard) all the way to their true bottom. */
static void press_all(km_engine_t *e, int shift)
{
    uint16_t frame[KM_ENGINE_SLOTS];
    unsigned group[4], ng = 0;
    for (unsigned s = 0; s <= KM_ENGINE_SLOTS; s++) {
        if (s < KM_ENGINE_SLOTS && km_keymap_slot_is_key(s)) {
            group[ng++] = s;
        }
        if (ng == 4 || (s == KM_ENGINE_SLOTS && ng > 0)) {
            for (unsigned f = 0; f < 80; f++) {
                live_frame(frame, shift);
                const unsigned ramp = f < 20 ? f : (f < 60 ? 20 : 79 - f > 20 ? 20 : 79 - f);   /* down 20 frames, hold 40, up 20 */
                for (unsigned i = 0; i < ng; i++) {
                    const unsigned sp = key_span(group[i]);
                    frame[group[i]] = (uint16_t)(frame[group[i]] - sp * ramp / 20u);
                }
                km_engine_update(e, frame);
            }
            ng = 0;
        }
    }
}

static unsigned n_keys(void)
{
    unsigned n = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        n += km_keymap_slot_is_key(s);
    }
    return n;
}

static void test_record_format(void)
{
    km_cal_record_t r, back;
    uint8_t buf[KM_CAL_PAYLOAD_BYTES + 4];
    CHECK_EQ(KM_CAL_PAYLOAD_BYTES, 308);
    memset(&r, 0, sizeof r);
    CHECK(km_cal_record_valid(&r));                                  /* nothing known is a valid record */
    CHECK_EQ(km_cal_encode(&r, buf, sizeof buf), 308);
    CHECK(buf[0] == 1 && buf[1] == 76 && buf[2] == 0 && buf[3] == 0);
    for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
        r.base[s] = (uint16_t)(2000u + 17u * s);
        r.bottom[s] = (uint16_t)(r.base[s] - 600u - s);
    }
    CHECK(km_cal_record_valid(&r));
    CHECK_EQ(km_cal_encode(&r, buf, sizeof buf), 308);
    CHECK_EQ(buf[4], 2000 & 255);                                    /* little endian, slot 0 first: base then bottom */
    CHECK_EQ(buf[5], 2000 >> 8);
    CHECK_EQ(buf[6], (2000 - 600) & 255);
    CHECK_EQ(buf[7], (2000 - 600) >> 8);
    CHECK(km_cal_decode(&back, buf, 308));
    CHECK(memcmp(&back, &r, sizeof r) == 0);
    CHECK_EQ(km_cal_encode(&r, buf, 307), 0);                        /* buffer too small */
    CHECK(!km_cal_decode(&back, buf, 307));
    CHECK(!km_cal_decode(&back, buf, 309));
    CHECK(!km_cal_decode(&back, NULL, 308));

    uint8_t ok[KM_CAL_PAYLOAD_BYTES];
    km_cal_encode(&r, ok, sizeof ok);
    for (unsigned i = 0; i < 4; i++) {                               /* header fields */
        memcpy(buf, ok, sizeof ok);
        buf[i] ^= 0x01;
        CHECK(!km_cal_decode(&back, buf, 308));
    }
    /* entry rules: base range, bottom below base, span range, and 0/0 only as a pair */
    static const struct { uint16_t base, bottom; bool ok; } cases[] = {
        { 0, 0, true }, { 3000, 2000, true }, { 1500, 1100, true }, { 4000, 2200, true }, { 3000, 1200, true },
        { 1499, 1000, false }, { 4001, 3000, false }, { 3000, 2601, false }, { 3000, 2600, true }, { 3000, 1199, false },
        { 3000, 3000, false }, { 3000, 3100, false }, { 0, 100, false }, { 3000, 0, false }, { 0xFFFF, 0xFFFF, false },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memcpy(buf, ok, sizeof ok);
        buf[4 + 4 * 17] = (uint8_t)cases[i].base;
        buf[5 + 4 * 17] = (uint8_t)(cases[i].base >> 8);
        buf[6 + 4 * 17] = (uint8_t)cases[i].bottom;
        buf[7 + 4 * 17] = (uint8_t)(cases[i].bottom >> 8);
        CHECK_EQ(km_cal_decode(&back, buf, 308), cases[i].ok);
    }
    memset(buf, 0xFF, sizeof buf);                                   /* erased flash, as a payload */
    CHECK(!km_cal_decode(&back, buf, 308));
    memset(buf, 0x00, sizeof buf);                                   /* zeros are not a record either (version 0) */
    CHECK(!km_cal_decode(&back, buf, 308));
    CHECK(!km_cal_record_valid(NULL));
}

static void test_capture_restore(void)
{
    km_engine_t a, b;
    km_cal_record_t rec, rec2;

    calibrate_normally(&a, 0);
    CHECK_EQ(km_engine_bottom(&a, 5), key_base(5) - 1000);          /* default stroke before any press */
    uint16_t ref[KM_CAL_SLOTS];
    km_cal_snapshot(&a, ref);
    CHECK(!km_cal_changed(&a, ref));
    press_all(&a, 0);
    unsigned keys = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!km_keymap_slot_is_key(s)) {
            continue;
        }
        keys++;
        const int want = (int)key_base(s) - (int)key_span(s);
        const int got = km_engine_bottom(&a, s);
        CHECK(got >= want - 2 && got <= want + (int)KM_ENGINE_LEARN_MARGIN);   /* learned the true bottom (to within the learn margin) */
    }
    CHECK_EQ(keys, n_keys());
    CHECK(km_cal_changed(&a, ref));

    km_cal_capture(&a, NULL, &rec);
    CHECK(km_cal_record_valid(&rec));
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ(rec.base[s], key_base(s));
            CHECK_EQ(rec.bottom[s], km_engine_bottom(&a, s));
        } else {
            CHECK(rec.base[s] == 0 && rec.bottom[s] == 0);
        }
    }

    /* an engine that is not calibrated captures nothing new: the previous record is kept slot by slot */
    km_engine_t cold;
    km_engine_init(&cold);
    km_cal_capture(&cold, &rec, &rec2);
    CHECK(memcmp(&rec, &rec2, sizeof rec) == 0);
    km_cal_capture(&cold, NULL, &rec2);
    km_cal_record_t zero;
    memset(&zero, 0, sizeof zero);
    CHECK(memcmp(&zero, &rec2, sizeof zero) == 0);
    CHECK(!km_cal_changed(&cold, ref));

    /* restore on the same sensor: same bottoms, calibrated at once */
    km_engine_init(&b);
    uint16_t live[KM_ENGINE_SLOTS];
    live_frame(live, 0);
    unsigned applied = 0;
    CHECK(km_cal_restore(&b, &rec, live, &applied) == KM_CAL_RESTORE_OK);
    CHECK(b.calibrated);
    CHECK_EQ(applied, keys);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        CHECK_EQ(km_engine_bottom(&b, s), km_engine_bottom(&a, s));
        CHECK_EQ(km_engine_baseline(&b, s), km_engine_baseline(&a, s));
    }
    /* ... and the restored engine types: a press reaches the matching bottom and the key goes down and up */
    uint16_t frame[KM_ENGINE_SLOTS];
    for (unsigned f = 0; f < 100; f++) {
        km_engine_update(&b, live);                                 /* flushes the threshold recomputes of the 61 dirty keys */
    }
    unsigned n = 0;
    const unsigned s0 = 10;
    CHECK(km_keymap_slot_is_key(s0));
    for (unsigned f = 0; f < 60; f++) {
        live_frame(frame, 0);
        frame[s0] = (uint16_t)(key_base(s0) - key_span(s0));
        km_engine_update(&b, frame);
    }
    CHECK(km_engine_slot_down(&b, s0));
    CHECK_EQ(km_engine_depth(&b, s0), 340);                         /* the learned bottom is a full 3.40 mm stroke */
    km_engine_down_slots(&b, &n);
    CHECK_EQ(n, 1);

    /* a different temperature: the baseline is the live one, the stroke is kept */
    for (int shift = -140; shift <= 150; shift += 10) {
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, shift);
        CHECK(km_cal_restore(&c, &rec, live, &applied) == KM_CAL_RESTORE_OK);
        CHECK_EQ(applied, keys);
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (km_keymap_slot_is_key(s)) {
                CHECK_EQ((int)km_engine_baseline(&c, s), (int)key_base(s) + shift);
                CHECK_EQ((int)km_engine_baseline(&c, s) - (int)km_engine_bottom(&c, s), (int)rec.base[s] - (int)rec.bottom[s]);
            }
        }
    }

    /* disagreement: the engine stays untouched and uncalibrated */
    static const int bad_shift[] = { 151, 200, -151, -400 };
    for (unsigned i = 0; i < sizeof bad_shift / sizeof bad_shift[0]; i++) {
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, bad_shift[i]);
        CHECK(km_cal_restore(&c, &rec, live, &applied) == KM_CAL_RESTORE_DISAGREE);
        CHECK(!c.calibrated);
        CHECK_EQ(applied, 0);
    }
    for (unsigned count = 5; count <= 8; count++) {                   /* 6 slots may differ, 7 may not */
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, 0);
        unsigned done = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS && done < count; s++) {
            if (km_keymap_slot_is_key(s)) {
                live[s] = (uint16_t)(live[s] + 300u);
                done++;
            }
        }
        const km_cal_restore_t res = km_cal_restore(&c, &rec, live, &applied);
        if (count <= KM_ENGINE_CAL_MAX_BAD) {
            CHECK(res == KM_CAL_RESTORE_OK);
            CHECK_EQ(applied, keys - count);                      /* the ones that differ get no stored bottom */
            CHECK(c.calibrated);
        } else {
            CHECK(res == KM_CAL_RESTORE_DISAGREE);
            CHECK(!c.calibrated);
        }
    }
    {   /* keys held while the keyboard boots: those few slots are guessed (ASSUMED), no stored bottom for them */
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, 0);
        unsigned held[3], nh = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS && nh < 3; s++) {
            if (km_keymap_slot_is_key(s)) {
                held[nh++] = s;
                live[s] = (uint16_t)(live[s] - 1000u);
            }
        }
        CHECK(km_cal_restore(&c, &rec, live, &applied) == KM_CAL_RESTORE_OK);
        CHECK_EQ(applied, keys - 3);
        for (unsigned i = 0; i < 3; i++) {
            CHECK(km_engine_slot_status(&c, held[i]) == KM_ENGINE_SLOT_ASSUMED);
        }
    }
    {   /* everything reads low (all keys down / dead sensor): nothing agrees */
        km_engine_t c;
        km_engine_init(&c);
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            live[s] = 2000;
        }
        CHECK(km_cal_restore(&c, &rec, live, &applied) == KM_CAL_RESTORE_DISAGREE);
        CHECK(!c.calibrated);
    }
    {   /* too few slots to compare: 29 stored entries are not enough, 30 are */
        km_cal_record_t few = rec;
        unsigned kept = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (km_keymap_slot_is_key(s) && ++kept > 29) {
                few.base[s] = few.bottom[s] = 0;
            }
        }
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, 0);
        CHECK(km_cal_restore(&c, &few, live, &applied) == KM_CAL_RESTORE_DISAGREE);
        kept = 0;
        few = rec;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (km_keymap_slot_is_key(s) && ++kept > 30) {
                few.base[s] = few.bottom[s] = 0;
            }
        }
        CHECK(km_cal_restore(&c, &few, live, &applied) == KM_CAL_RESTORE_OK);
        CHECK_EQ(applied, 30);
    }
    {   /* entries on slots without a key (a foreign or damaged record) do not count as evidence */
        km_cal_record_t odd;
        memset(&odd, 0, sizeof odd);
        unsigned put = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS && put < 40; s++) {
            if (!km_keymap_slot_is_key(s)) {
                odd.base[s] = 3300;
                odd.bottom[s] = 2200;
                put++;
            }
        }
        km_engine_t c;
        km_engine_init(&c);
        live_frame(live, 0);
        CHECK(km_cal_restore(&c, &odd, live, &applied) == KM_CAL_RESTORE_DISAGREE);
    }
    {   /* the record agrees with the live values but the values themselves look like held keys: the engine refuses */
        km_cal_record_t low;
        memset(&low, 0, sizeof low);
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (km_keymap_slot_is_key(s)) {
                low.base[s] = 2000;
                low.bottom[s] = 1400;
            }
        }
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            live[s] = 2000;
        }
        km_engine_t c;
        km_engine_init(&c);
        CHECK(km_cal_restore(&c, &low, live, &applied) == KM_CAL_RESTORE_REFUSED);
        CHECK(!c.calibrated);
    }

    /* after a normal calibration (e.g. the host's recalibrate) the stored strokes are applied slot by slot */
    km_engine_t c;
    calibrate_normally(&c, 60);
    CHECK(km_cal_apply(&c, &rec) == keys);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ((int)km_engine_baseline(&c, s) - (int)km_engine_bottom(&c, s), (int)rec.base[s] - (int)rec.bottom[s]);
        }
    }
    CHECK_EQ(km_cal_apply(&c, &rec), 0);                           /* nothing left to change */
    calibrate_normally(&c, 400);                                   /* the sensor moved too far: nothing is applied */
    CHECK_EQ(km_cal_apply(&c, &rec), 0);
}

static void test_policy(void)
{
    km_engine_t e;
    km_cal_policy_t p;
    uint16_t ref[KM_CAL_SLOTS];
    calibrate_normally(&e, 0);
    km_cal_snapshot(&e, ref);
    km_cal_policy_init(&p);

    /* nothing learned: never due, however long we wait (no writes for a keyboard that is only typed on) */
    for (uint32_t t = 0; t < 4 * 3600; t++) {
        CHECK(!km_cal_policy_due(&p, &e, ref, t));
    }

    /* the delta rule at its boundary: 15 counts deeper is not noticeable, 16 is */
    unsigned s = 0;
    while (!km_keymap_slot_is_key(s)) {
        s++;
    }
    km_engine_set_bottom(&e, s, (uint16_t)(ref[s] - 15u));
    CHECK(!km_cal_changed(&e, ref));
    km_engine_set_bottom(&e, s, (uint16_t)(ref[s] - 16u));
    CHECK(km_cal_changed(&e, ref));
    km_engine_set_bottom(&e, s, (uint16_t)(ref[s] + 40u));          /* shallower never counts (only learning is saved) */
    CHECK(!km_cal_changed(&e, ref));

    /* learning in progress: not due until the bottoms were still for KM_CAL_QUIET_S seconds */
    km_cal_policy_init(&p);
    uint32_t t = 10000;
    CHECK(!km_cal_policy_due(&p, &e, ref, t));
    for (unsigned i = 0; i < 100; i++) {
        t++;
        km_engine_set_bottom(&e, s, (uint16_t)(ref[s] - 20u - i)); /* still getting deeper every second */
        CHECK(!km_cal_policy_due(&p, &e, ref, t));
    }
    const uint32_t last_change = t;
    for (uint32_t k = 1; k < KM_CAL_QUIET_S; k++) {
        CHECK(!km_cal_policy_due(&p, &e, ref, last_change + k));
    }
    CHECK(km_cal_policy_due(&p, &e, ref, last_change + KM_CAL_QUIET_S));    /* first save after a boot: no interval */
    CHECK(km_cal_policy_due(&p, &e, ref, last_change + 5000));

    /* saved: the reference moves, nothing is due; a new change must wait for the minimum interval */
    km_cal_snapshot(&e, ref);
    const uint32_t saved_at = last_change + KM_CAL_QUIET_S;
    km_cal_policy_saved(&p, saved_at);
    CHECK(!km_cal_policy_due(&p, &e, ref, saved_at + 1));
    km_engine_set_bottom(&e, s, (uint16_t)(ref[s] - 100u));
    for (uint32_t k = 2; k < KM_CAL_MIN_INTERVAL_S; k += 7) {
        CHECK(!km_cal_policy_due(&p, &e, ref, saved_at + k));
    }
    CHECK(!km_cal_policy_due(&p, &e, ref, saved_at + KM_CAL_MIN_INTERVAL_S - 1));
    CHECK(km_cal_policy_due(&p, &e, ref, saved_at + KM_CAL_MIN_INTERVAL_S));
    CHECK(km_cal_policy_due(&p, &e, ref, saved_at + KM_CAL_MIN_INTERVAL_S + 1));

    /* the host's command: one attempt per KM_CAL_REQUEST_GAP_S */
    km_cal_policy_init(&p);
    CHECK(km_cal_policy_request(&p, 0));                           /* the very first one, even at uptime 0 */
    CHECK(!km_cal_policy_request(&p, 1));
    CHECK(!km_cal_policy_request(&p, KM_CAL_REQUEST_GAP_S - 1));
    CHECK(km_cal_policy_request(&p, KM_CAL_REQUEST_GAP_S));
    CHECK(!km_cal_policy_request(&p, KM_CAL_REQUEST_GAP_S + 1));
}

/* The whole path on the simulated flash: learn, policy, background job steps with a key down in between, "reboot", restore. */
static void test_end_to_end(void)
{
    sim_blank();
    km_engine_t a, b;
    km_cal_record_t rec, loaded, held_rec;
    km_cal_policy_t pol;
    uint16_t ref[KM_CAL_SLOTS];
    uint16_t live[KM_ENGINE_SLOTS];
    unsigned applied = 0;

    /* first boot: nothing stored */
    CHECK(!km_cal_load(&g_log, &loaded));
    calibrate_normally(&a, 0);
    memset(&held_rec, 0, sizeof held_rec);
    km_cal_snapshot(&a, ref);
    km_cal_policy_init(&pol);

    /* the user presses every key to the bottom */
    press_all(&a, 0);
    uint32_t t = 1000;
    bool due = false;
    unsigned ticks = 0;
    while (!due && ticks < 200) {
        due = km_cal_policy_due(&pol, &a, ref, t++);
        ticks++;
    }
    CHECK(due);
    CHECK(ticks <= KM_CAL_QUIET_S + 2);                              /* learning had stopped already: only the quiet time is waited */
    km_cal_capture(&a, &held_rec, &rec);
    km_clog_job_t job;
    CHECK(km_cal_job_begin(&job, &g_log, &rec) == KM_CLOG_PENDING);
    unsigned steps = 0;
    km_clog_result_t r = KM_CLOG_PENDING;
    while (r == KM_CLOG_PENDING) {
        r = km_clog_job_step(&job);                                  /* the caller would run its main loop between the steps */
        steps++;
    }
    CHECK(r == KM_CLOG_OK);
    CHECK(steps >= 18 && steps <= 28);                               /* ~42 flash operations, one line (two units) per step */
    km_cal_policy_saved(&pol, t);
    km_cal_snapshot(&a, ref);
    CHECK(!km_cal_changed(&a, ref));
    CHECK_EQ(g_sim.erases, 0);

    /* an unchanged state writes nothing on the next attempt */
    const int ev = g_sim.ev;
    CHECK(km_cal_job_begin(&job, &g_log, &rec) == KM_CLOG_UNCHANGED);
    CHECK_EQ(g_sim.ev, ev);

    /* power cycle: the log loads, the restore applies it on a slightly different temperature */
    CHECK(km_cal_load(&g_log, &loaded));
    CHECK(memcmp(&loaded, &rec, sizeof rec) == 0);
    km_engine_init(&b);
    live_frame(live, 25);
    CHECK(km_cal_restore(&b, &loaded, live, &applied) == KM_CAL_RESTORE_OK);
    CHECK_EQ(applied, n_keys());
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ((int)km_engine_baseline(&b, s) - (int)km_engine_bottom(&b, s), (int)loaded.base[s] - (int)loaded.bottom[s]);
            CHECK_EQ((int)km_engine_baseline(&b, s), (int)key_base(s) + 25);
        }
    }

    /* a foreign record with a good log CRC but not a calibration payload is not loaded */
    sim_blank();
    uint8_t junk[KM_CAL_PAYLOAD_BYTES];
    memset(junk, 0x5A, sizeof junk);
    CHECK(km_clog_append(&g_log, junk) == KM_CLOG_OK);
    CHECK(!km_cal_load(&g_log, &loaded));
    CHECK(km_cal_save(&g_log, &rec) == KM_CLOG_OK);
    CHECK(km_cal_load(&g_log, &loaded));
    memset(&loaded, 0, sizeof loaded);
    loaded.base[0] = 1;                                             /* an invalid record is refused before it reaches the flash */
    CHECK(km_cal_save(&g_log, &loaded) == KM_CLOG_ERR_ARG);
    CHECK_EQ(g_sim.violations, 0);
}

/* ---- km_calmgr: boot restore, background save, the host command, on the simulated flash ---- */

static km_engine_t g_eng;
static km_calmgr_t g_mgr;
static unsigned g_hook_calls;

static void hook(void *ctx)
{
    (void)ctx;
    g_hook_calls++;
}

/* The boot sequence of m5_main.c: restore from the record if there is one, else the normal calibration. */
static void mgr_boot(int shift)
{
    uint16_t live[KM_ENGINE_SLOTS];
    live_frame(live, shift);
    km_engine_init(&g_eng);
    km_calmgr_init(&g_mgr, &g_log, &g_eng, hook, NULL);
    g_hook_calls = 0;
    if (km_calmgr_try_restore(&g_mgr, live)) {
        return;
    }
    km_engine_calibrate_begin(&g_eng);
    for (unsigned i = 0; i < 64; i++) {
        km_engine_calibrate_idle(&g_eng, live);
    }
    CHECK(km_engine_calibrate_finish(&g_eng) == KM_ENGINE_CAL_OK);
    km_calmgr_armed(&g_mgr, true);
}

/* One simulated second: the policy tick, then 60 loop passes (a save needs about 24 of them). Returns the flash operations done. */
static unsigned mgr_second(uint32_t *t, bool idle_now, bool idle_long)
{
    unsigned ops = 0;
    km_calmgr_second(&g_mgr, ++*t);
    for (unsigned i = 0; i < 60; i++) {
        const int ev = g_sim.ev;
        const bool did = km_calmgr_pass(&g_mgr, *t, idle_now, idle_long);
        CHECK(g_sim.ev - ev <= 2);                                  /* never more than one flash line (two units, back to back) per pass */
        CHECK(!(g_sim.ev != ev && !did));                           /* a pass that touched the flash says so */
        ops += (unsigned)(g_sim.ev - ev);
    }
    return ops;
}

static const km_vendor_info_t k_mgr_info = { 0x0104, "Jan  1 2026,00:00:00KMFW-TEST" };
static bool g_busy_for_cfg;
static uint32_t g_cfg_now;

static km_cfg_status_t cfg_scal(void *ctx)
{
    (void)ctx;
    switch (km_calmgr_save_request(&g_mgr, g_cfg_now, !g_busy_for_cfg)) {
    case KM_CALMGR_OK: return KM_CFG_OK;
    case KM_CALMGR_BUSY: return KM_CFG_ERR_BUSY;
    default: return KM_CFG_ERR_FAILED;
    }
}

/* SCAL through the real protocol handler: returns the status byte of the reply, 0xFF = no reply */
static unsigned send_scal(const char *word)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN], data[6];
    data[0] = KM_CFG_OP_SAVE_CALIBRATION_REQUEST;
    data[1] = 0x42;
    memcpy(data + 2, word, 4);
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, data, 6) == KM_HID_OK);
    memset(out, 0x77, sizeof out);
    if (!km_vendor_handle(&k_mgr_info, in, out)) {
        return 0xFFu;
    }
    CHECK(out[8] == KM_CFG_OP_SAVE_CALIBRATION_REQUEST && out[9] == 0x42);
    return out[10];
}

static unsigned feature_bits(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN], data[2] = { KM_CFG_OP_GET_INFO, 0x43 };
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, data, 2) == KM_HID_OK);
    CHECK(km_vendor_handle(&k_mgr_info, in, out));
    return (unsigned)(out[11 + 7] | (out[11 + 8] << 8));
}

static void test_manager_flow(void)
{
    uint32_t t = 100;

    /* ---- first boot on an erased sector: normal calibration, nothing restored ---- */
    sim_blank();
    mgr_boot(0);
    CHECK_EQ(g_mgr.boot, KM_CALMGR_BOOT_NONE);
    CHECK(!g_mgr.rec_valid && !g_mgr.try_restore);
    CHECK(!g_mgr.scan0.valid && g_mgr.scan0.next == 0);
    const int ev0 = g_sim.ev;
    for (unsigned i = 0; i < 3 * 3600; i++) {                       /* three hours of typing on a calibrated keyboard: no flash traffic */
        CHECK_EQ(mgr_second(&t, true, true), 0);
    }
    CHECK_EQ(g_sim.ev, ev0);

    /* ---- the user presses every key to the bottom; the save waits for quiet, for idle, and does one operation per pass ---- */
    press_all(&g_eng, 0);
    unsigned quiet = 0;
    while (!g_mgr.due && quiet < 60) {
        CHECK_EQ(mgr_second(&t, true, false), 0);                   /* learning is over, but not "idle_long": nothing is written */
        quiet++;
    }
    CHECK(g_mgr.due);
    CHECK(quiet >= KM_CAL_QUIET_S && quiet <= KM_CAL_QUIET_S + 2);
    CHECK_EQ(g_sim.ev, ev0);
    CHECK_EQ(mgr_second(&t, false, false), 0);                      /* a key is down: still nothing */
    (void)km_calmgr_pass(&g_mgr, t, true, true);                    /* idle: the job starts (reads only) ... */
    CHECK(km_calmgr_job_active(&g_mgr));
    CHECK_EQ(g_sim.ev, ev0);
    unsigned passes = 0, did_ops = 0;
    const unsigned hooks0 = g_hook_calls;
    while (km_calmgr_job_active(&g_mgr) && passes < 200) {          /* ... then one flash operation per pass */
        const int ev = g_sim.ev;
        const bool did = km_calmgr_pass(&g_mgr, t, true, false);    /* idle_long is only needed to START a job */
        CHECK(g_sim.ev - ev <= 2);
        did_ops += did;
        passes++;
        if (passes == 10) {                                         /* a key goes down in the middle: the job pauses, no flash access */
            const int e1 = g_sim.ev;
            for (unsigned i = 0; i < 50; i++) {
                CHECK(!km_calmgr_pass(&g_mgr, t, false, false));
            }
            CHECK_EQ(g_sim.ev, e1);
            CHECK(km_calmgr_job_active(&g_mgr));
        }
    }
    CHECK(!km_calmgr_job_active(&g_mgr));
    CHECK_EQ(g_mgr.saves_ok, 1);
    CHECK_EQ(g_mgr.last, KM_CLOG_OK);
    CHECK(did_ops >= 18 && did_ops <= 28);
    CHECK_EQ(g_hook_calls - hooks0, did_ops);                       /* the scan chain was repaired after every flash operation */
    CHECK_EQ(g_sim.erases, 0);
    CHECK_EQ(g_sim.violations, 0);
    CHECK(!km_calmgr_changed(&g_mgr));
    CHECK(g_mgr.rec_valid);

    /* ---- reboot at another temperature: restored from one frame, strokes applied ---- */
    km_cal_record_t flash_rec;
    CHECK(km_cal_load(&g_log, &flash_rec));
    mgr_boot(35);
    CHECK_EQ(g_mgr.boot, KM_CALMGR_BOOT_RESTORED);
    CHECK(g_eng.calibrated && g_mgr.rec_valid);
    CHECK_EQ(g_mgr.applied, n_keys());
    CHECK_EQ(g_mgr.scan0.used, 1);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ((int)km_engine_baseline(&g_eng, s) - (int)km_engine_bottom(&g_eng, s), (int)flash_rec.base[s] - (int)flash_rec.bottom[s]);
        }
    }
    CHECK(!km_calmgr_changed(&g_mgr));                              /* a restored keyboard does not rewrite what it just read */
    t = 5000;
    for (unsigned i = 0; i < 3600; i++) {
        CHECK_EQ(mgr_second(&t, true, true), 0);
    }

    /* ---- a record from a keyboard / temperature the sensor no longer matches is dropped, the normal calibration runs ---- */
    mgr_boot(220);
    CHECK_EQ(g_mgr.boot, KM_CALMGR_BOOT_DISAGREED);
    CHECK(!g_mgr.rec_valid);
    CHECK(g_eng.calibrated);                                        /* the normal calibration ran in mgr_boot */
    CHECK_EQ(g_mgr.applied, 0);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ((int)km_engine_baseline(&g_eng, s) - (int)km_engine_bottom(&g_eng, s), 1000);   /* default strokes */
        }
    }
    CHECK_EQ(g_sim.violations, 0);

    /* ---- recalibration keeps what was learned ---- */
    mgr_boot(0);
    CHECK_EQ(g_mgr.boot, KM_CALMGR_BOOT_RESTORED);
    km_engine_set_bottom(&g_eng, 22, (uint16_t)(km_engine_bottom(&g_eng, 22) - 40u));   /* learned since the last save */
    uint16_t before_bottom[KM_ENGINE_SLOTS];
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        before_bottom[s] = km_engine_bottom(&g_eng, s);
    }
    km_calmgr_keep_learned(&g_mgr);
    g_eng.calibrated = false;
    {
        uint16_t live[KM_ENGINE_SLOTS];
        live_frame(live, 10);
        km_engine_calibrate_begin(&g_eng);
        for (unsigned i = 0; i < 64; i++) {
            km_engine_calibrate_idle(&g_eng, live);
        }
        CHECK(km_engine_calibrate_finish(&g_eng) == KM_ENGINE_CAL_OK);
        km_calmgr_armed(&g_mgr, true);
    }
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            CHECK_EQ(km_engine_bottom(&g_eng, s), before_bottom[s] + 10);   /* same stroke, baseline moved by 10 */
        }
    }

    /* ---- a failing flash: the error is counted, the old record stays, and there is no retry storm ---- */
    mgr_boot(0);
    t = 100000;
    km_engine_set_bottom(&g_eng, 12, (uint16_t)(km_engine_bottom(&g_eng, 12) - 60u));
    km_cal_record_t old_rec, now_rec;
    CHECK(km_cal_load(&g_log, &old_rec));
    g_sim.fail_ev = g_sim.ev + 5;
    g_sim.fail_var = 0;
    unsigned guard = 0;
    while (g_mgr.saves_failed == 0 && guard++ < 200) {
        mgr_second(&t, true, true);
    }
    g_sim.fail_ev = -1;
    CHECK_EQ(g_mgr.saves_failed, 1);
    CHECK_EQ(g_mgr.last, KM_CLOG_ERR_PROGRAM);
    CHECK(km_cal_load(&g_log, &now_rec) && memcmp(&now_rec, &old_rec, sizeof now_rec) == 0);
    const int ev_fail = g_sim.ev;
    for (unsigned i = 0; i < KM_CAL_MIN_INTERVAL_S - 25; i++) {     /* the change is still pending, but the interval protects the flash */
        CHECK_EQ(mgr_second(&t, true, true), 0);
    }
    CHECK_EQ(g_sim.ev, ev_fail);
    guard = 0;
    while (g_mgr.saves_ok == 0 && guard++ < 100) {
        mgr_second(&t, true, true);
    }
    CHECK_EQ(g_mgr.saves_ok, 1);                                    /* ... and it is written once the interval has passed */
    CHECK(km_cal_load(&g_log, &now_rec));
    CHECK(now_rec.bottom[12] < old_rec.bottom[12]);

    /* ---- the sector is full: once the erase is done the job is followed through without pausing ---- */
    sim_blank();
    km_cal_record_t r2;
    memset(&r2, 0, sizeof r2);
    for (unsigned i = 0; i < 24; i++) {
        for (unsigned s = 0; s < KM_CAL_SLOTS; s++) {
            if (km_keymap_slot_is_key(s)) {
                r2.base[s] = key_base(s);
                r2.bottom[s] = (uint16_t)(key_base(s) - 1000u - i);
            }
        }
        CHECK(km_cal_save(&g_log, &r2) == KM_CLOG_OK);
    }
    mgr_boot(0);
    CHECK_EQ(g_mgr.boot, KM_CALMGR_BOOT_RESTORED);
    CHECK_EQ(g_mgr.scan0.next, 24);
    t = 200000;
    press_all(&g_eng, 0);
    guard = 0;
    while (!g_mgr.due && guard++ < 100) {
        mgr_second(&t, true, false);
    }
    CHECK(g_mgr.due);
    km_calmgr_pass(&g_mgr, t, true, true);                          /* starts the job */
    CHECK(km_calmgr_job_active(&g_mgr) && km_clog_job_next_is_erase(&g_mgr.job));
    CHECK(!km_clog_job_must_finish(&g_mgr.job));
    CHECK(!km_calmgr_pass(&g_mgr, t, false, false));                /* not idle: even the erase waits */
    CHECK_EQ(g_sim.erases, 0);
    CHECK(km_calmgr_pass(&g_mgr, t, true, true));                   /* the erase */
    CHECK_EQ(g_sim.erases, 1);
    CHECK(km_clog_job_must_finish(&g_mgr.job));
    unsigned forced = 0;
    while (km_calmgr_job_active(&g_mgr) && forced < 100) {          /* a key is down, a fault is active: the job still runs to the end */
        CHECK(km_calmgr_pass(&g_mgr, t, false, false));
        forced++;
    }
    CHECK(forced >= 18 && forced <= 28);
    CHECK_EQ(g_mgr.saves_ok, 1);
    CHECK(km_cal_load(&g_log, &now_rec));
    CHECK_EQ(g_sim.violations, 0);
}

static void test_manager_scal(void)
{
    sim_blank();
    mgr_boot(0);
    static km_config_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.profile_count = 4;
    ops.save_calibration = cfg_scal;
    static km_config_t cfg;
    km_config_init(&cfg, &ops);
    km_vendor_set_config(&cfg);
    CHECK(feature_bits() & KM_CFG_FEAT_SAVE_CAL);
    g_busy_for_cfg = false;
    g_cfg_now = 1000;
    km_cal_record_t now_rec;

    CHECK_EQ(send_scal("SCAL"), KM_CFG_OK);                         /* nothing learned yet: OK, nothing written */
    CHECK_EQ(g_sim.ev, 0);
    g_cfg_now += 10;
    CHECK_EQ(send_scal("RCAL"), KM_CFG_ERR_NEED_CONFIRM);           /* wrong word: refused before the backend */
    CHECK_EQ(send_scal("scal"), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(g_sim.ev, 0);

    press_all(&g_eng, 0);
    g_busy_for_cfg = true;                                          /* a key is down */
    g_cfg_now += 10;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_ERR_BUSY);
    CHECK_EQ(g_sim.ev, 0);
    g_busy_for_cfg = false;
    g_eng.calibrated = false;                                       /* not calibrated: nothing to save */
    CHECK_EQ(send_scal("SCAL"), KM_CFG_ERR_BUSY);
    g_eng.calibrated = true;
    g_cfg_now += 1;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_OK);                         /* the write, to the end, no quiet time needed */
    CHECK(g_sim.ev >= 35);
    CHECK_EQ(g_mgr.saves_ok, 1);
    CHECK(km_cal_load(&g_log, &now_rec));
    CHECK_EQ(now_rec.bottom[10], km_engine_bottom(&g_eng, 10));
    const int ev_scal = g_sim.ev;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_ERR_BUSY);                   /* again at once: the rate limit */
    g_cfg_now += KM_CAL_REQUEST_GAP_S - 2;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_ERR_BUSY);
    g_cfg_now += 2;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_OK);                         /* allowed again, but nothing new was learned: no write */
    CHECK_EQ(g_sim.ev, ev_scal);

    /* new learning, then SCAL: a failing flash gives FAILED and keeps the old record, the retry works */
    km_engine_set_bottom(&g_eng, 10, (uint16_t)(km_engine_bottom(&g_eng, 10) - 30u));
    g_cfg_now += KM_CAL_REQUEST_GAP_S;
    g_sim.fail_ev = g_sim.ev + 3;
    g_sim.fail_var = 0;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_ERR_FAILED);
    g_sim.fail_ev = -1;
    CHECK_EQ(g_mgr.saves_failed, 1);
    CHECK(km_cal_load(&g_log, &now_rec));
    CHECK(now_rec.bottom[10] > km_engine_bottom(&g_eng, 10));       /* the previous record is intact */
    g_cfg_now += KM_CAL_REQUEST_GAP_S;
    CHECK_EQ(send_scal("SCAL"), KM_CFG_OK);
    CHECK(km_cal_load(&g_log, &now_rec));
    CHECK_EQ(now_rec.bottom[10], km_engine_bottom(&g_eng, 10));
    CHECK_EQ(g_sim.violations, 0);

    /* power cut at every step of a SCAL save: the log holds the old or the new record, and the keyboard boots from it */
    km_engine_set_bottom(&g_eng, 22, (uint16_t)(km_engine_bottom(&g_eng, 22) - 50u));
    km_cal_record_t oldr, newr;
    CHECK(km_cal_load(&g_log, &oldr));
    km_cal_capture(&g_eng, &oldr, &newr);
    memcpy(&g_start, &g_sim, sizeof g_sim);
    unsigned cuts = 0, bad = 0;
    for (int ev = 0; ev < 60; ev++) {
        for (int v = 0; v < 4; v++) {
            restore_start();
            g_sim.cut_ev = ev;
            g_sim.cut_var = v;
            static km_calmgr_t m;
            static km_engine_t e2;
            e2 = g_eng;
            km_calmgr_init(&m, &g_log, &e2, hook, NULL);
            memcpy(m.ref, g_mgr.ref, sizeof m.ref);
            m.pol = g_mgr.pol;
            const km_calmgr_status_t st = km_calmgr_save_request(&m, 99999, true);
            if (!g_sim.dead) {
                continue;                                           /* the save was shorter than this step number */
            }
            cuts++;
            CHECK(st == KM_CALMGR_FAILED);
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            km_cal_record_t got;
            if (!km_cal_load(&g_log, &got) || (memcmp(&got, &oldr, sizeof got) != 0 && memcmp(&got, &newr, sizeof got) != 0)) {
                bad++;
            }
            uint16_t live[KM_ENGINE_SLOTS];
            live_frame(live, 5);
            static km_engine_t e3;
            static km_calmgr_t m3;
            km_engine_init(&e3);
            km_calmgr_init(&m3, &g_log, &e3, hook, NULL);
            if (!km_calmgr_try_restore(&m3, live) || !e3.calibrated || m3.boot != KM_CALMGR_BOOT_RESTORED) {
                bad++;
            }
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(cuts >= 4 * 40);
    printf("  calmgr: %u power cuts during the host command, every one boots from the old or the new record\n", cuts);
    km_vendor_set_config(NULL);
}

static void test_manager_wear(void)
{
    /* a month of use: learning steps far apart, each noticeable: one record each, one erase per 24, nothing in between */
    sim_blank();
    mgr_boot(0);
    uint32_t t = 1000;
    unsigned changes = 0;
    for (unsigned hour = 0; hour < 30 * 24; hour++) {
        if (hour % 6 == 0 && changes < 120) {                       /* every 6 hours a key learns 17 counts more */
            static const unsigned slots6[6] = { 10, 11, 12, 13, 14, 16 };
            const unsigned s = slots6[changes % 6];
            km_engine_set_bottom(&g_eng, s, (uint16_t)(km_engine_bottom(&g_eng, s) - 17u));
            changes++;
        }
        for (unsigned i = 0; i < 3600; i++) {
            km_calmgr_second(&g_mgr, ++t);
            if (km_calmgr_job_active(&g_mgr) || g_mgr.due) {
                for (unsigned k = 0; k < 60; k++) {
                    km_calmgr_pass(&g_mgr, t, true, true);
                }
            }
        }
    }
    CHECK(km_keymap_slot_is_key(10) && km_keymap_slot_is_key(16) && km_keymap_slot_is_key(22));
    CHECK_EQ(g_mgr.saves_ok, changes);                              /* one record per change, none for the idle hours in between */
    CHECK_EQ(g_mgr.saves_failed, 0);
    CHECK_EQ(g_sim.erases, (changes - 1) / 24);
    CHECK_EQ(g_sim.violations, 0);
    printf("  calmgr: 30 days, %u noticeable changes -> %u records, %u erases\n", changes, g_mgr.saves_ok, g_sim.erases);
}

void test_calstore(void)
{
    make_payloads();
    sim_blank();
    test_basic();
    test_states();
    test_sweeps();
    test_guard();
    test_record_format();
    test_capture_restore();
    test_policy();
    test_end_to_end();
    test_manager_flow();
    test_manager_scal();
    test_manager_wear();
}
