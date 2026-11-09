/*
 * Runs the REAL port/n32h48x/store_flash.c against a simulated flash controller (registers and flash window are
 * accessed through km_ssim_reg32 / km_ssim_read8, see tests/sim/store_sim_regs.h), underneath the real km_store.
 * Same controller model as test_flash_boot.c: unlock with the two keys, clear the status flags, PER + ADDR + STRT
 * erases an 8 KB sector, PG + two word writes program 8 bytes, programming can only clear bits.
 */
#include <stdbool.h>
#include <stdlib.h>

#include "km/km_calib.h"
#include "km/km_flash_guard.h"
#include "km/km_lightscene.h"
#include "km/km_settings.h"
#include "km/km_store.h"
#include "km_test.h"
#include "store_flash.h"

#define FLASH_SIZE 0x80000u
#define FLASH_BASE 0x08000000u
#define REG_CTRL   0x40022004u
#define REG_STS    0x40022008u
#define REG_ADDR   0x4002200Cu
#define REG_KEY    0x40022010u

void km_test_random_settings(km_settings_t *s, uint32_t seed);   /* test_settings.c */

static uint8_t g_flash[FLASH_SIZE];
static uint8_t g_unit_programmed[FLASH_SIZE / 8u];   /* a program event happened on this 8-byte unit since the last erase (even one that wrote 0xFF) */
static uint32_t g_ctrl, g_sts, g_addr;
static int g_key_state;
static bool g_unlocked;
static unsigned g_unlock_count, g_key_writes;
static unsigned g_violations, g_feeds, g_reg_accesses, g_read_accesses;
static unsigned g_erases, g_programs;
static unsigned g_erase_addrs[64], g_program_min = 0xFFFFFFFFu, g_program_max;
static unsigned g_pending_kind;                 /* 0 none, 1 key, 2 flash word, 3 status */
static uint32_t g_pending_addr, g_tmp, g_pending_orig;
static int g_fail_program_at, g_corrupt_program_at, g_fail_erase_at, g_bad_erase_at;
static bool g_stuck_busy;
static unsigned g_sts_reads_while_busy;

static void commit_pending(void)
{
    if (g_pending_kind == 1) {
        g_key_writes++;
        if (g_tmp == 0x45670123u) {
            g_key_state = 1;
        } else if (g_tmp == 0xCDEF89ABu && g_key_state == 1) {
            g_unlocked = true;
            g_unlock_count++;
            g_key_state = 0;
        } else {
            g_key_state = 0;
        }
    } else if (g_pending_kind == 3) {
        if (g_tmp != g_pending_orig) {
            g_sts &= ~g_tmp;
        }
    } else if (g_pending_kind == 2) {
        const uint32_t a = g_pending_addr - FLASH_BASE;
        if (!(g_ctrl & 2u) || !g_unlocked) {
            g_violations++;                      /* a flash write without PG or without unlock */
        } else {
            for (unsigned b = 0; b < 4; b++) {
                const uint8_t nv = (uint8_t)(g_tmp >> (8 * b));
                if (g_flash[a + b] != 0xFF && nv != 0xFF) {
                    g_violations++;              /* programming over data that was not erased */
                }
                g_flash[a + b] &= nv;
            }
            if ((g_pending_addr & 4u) == 0) {
                if ((g_pending_addr & 8u) != 0 && !g_unit_programmed[(a - 8u) / 8u]) {
                    g_violations++;              /* the hardware refuses an upper unit whose lower unit of the 16-byte line was never programmed */
                }
                g_unit_programmed[a / 8u] = 1;
                if ((g_pending_addr & 8u) == 0) {    /* the hardware refuses a unit below one already programmed in its 16-byte line */
                    for (unsigned b = 0; b < 8; b++) {
                        if (g_flash[a + 8 + b] != 0xFF) {
                            g_violations++;
                            break;
                        }
                    }
                }
                g_programs++;
                if (g_pending_addr < g_program_min) g_program_min = g_pending_addr;
                if (g_pending_addr > g_program_max) g_program_max = g_pending_addr;
                if ((g_pending_addr & 7u) != 0) {
                    g_violations++;
                }
                if (g_corrupt_program_at != 0 && (int)g_programs == g_corrupt_program_at) {
                    for (unsigned b = 0; b < 4; b++) {
                        if (g_flash[a + b] != 0) {
                            g_flash[a + b] = (uint8_t)(g_flash[a + b] & (g_flash[a + b] - 1u));
                            break;
                        }
                    }
                }
                if (g_fail_program_at != 0 && (int)g_programs == g_fail_program_at) {
                    g_sts |= 0x4u;
                }
            }
        }
    }
    g_pending_kind = 0;
}

static void run_erase_if_started(void)
{
    if ((g_ctrl & 4u) && (g_ctrl & 0x10u)) {
        g_ctrl &= ~0x10u;
        if (!g_unlocked || g_addr < FLASH_BASE || g_addr >= FLASH_BASE + FLASH_SIZE || (g_addr & 0x1FFFu) != 0) {
            g_violations++;
            return;
        }
        memset(&g_flash[g_addr - FLASH_BASE], 0xFF, 0x2000);
        memset(&g_unit_programmed[(g_addr - FLASH_BASE) / 8u], 0, 0x2000u / 8u);
        if (g_erases < 64) {
            g_erase_addrs[g_erases] = g_addr;
        }
        g_erases++;
        if (g_bad_erase_at != 0 && (int)g_erases == g_bad_erase_at) {
            g_flash[g_addr - FLASH_BASE + 0x1FFF] = 0x7F;      /* status says OK but the last byte is not erased */
        }
        if (g_fail_erase_at != 0 && (int)g_erases == g_fail_erase_at) {
            g_sts |= 0x4u;
        }
    }
}

uint32_t *km_ssim_reg32(uintptr_t addr)
{
    commit_pending();
    switch (addr) {
    case REG_CTRL: g_reg_accesses++; return &g_ctrl;
    case REG_ADDR: g_reg_accesses++; return &g_addr;
    case REG_STS:
        g_reg_accesses++;
        run_erase_if_started();
        if (g_stuck_busy) {
            g_sts |= 0x2u;
            g_sts_reads_while_busy++;
        } else {
            g_sts &= ~0x2u;
        }
        g_tmp = g_pending_orig = g_sts;
        g_pending_kind = 3;
        return &g_tmp;
    case REG_KEY:
        g_reg_accesses++;
        g_pending_kind = 1;
        return &g_tmp;
    default:
        break;
    }
    if (addr >= FLASH_BASE && addr + 4 <= FLASH_BASE + FLASH_SIZE) {
        g_reg_accesses++;
        g_pending_kind = 2;
        g_pending_addr = (uint32_t)addr;
        g_tmp = 0xFFFFFFFFu;
        return &g_tmp;
    }
    printf("  unexpected register access 0x%08X\n", (unsigned)addr);
    g_violations++;
    static uint32_t scratch;
    return &scratch;
}

uint8_t km_ssim_read8(uintptr_t addr)
{
    commit_pending();
    g_read_accesses++;
    if (addr < FLASH_BASE || addr >= FLASH_BASE + FLASH_SIZE) {
        g_violations++;
        return 0;
    }
    return g_flash[addr - FLASH_BASE];
}

void km_ssim_feed(void)
{
    g_feeds++;
}

static void sim_init(bool unlocked)
{
    for (uint32_t i = 0; i < FLASH_SIZE; i++) {
        g_flash[i] = (uint8_t)((i * 2654435761u) >> 24);        /* deterministic non-trivial content everywhere */
    }
    memset(&g_flash[0x30000], 0xFF, 0x6000);                    /* settings A, B and calibration start erased */
    g_ctrl = g_sts = g_addr = 0;
    g_key_state = 0;
    g_unlocked = unlocked;                      /* the driver unlocks once per power-up: later runs start unlocked */
    g_unlock_count = g_key_writes = 0;
    g_violations = g_feeds = g_reg_accesses = g_read_accesses = 0;
    g_erases = g_programs = 0;
    g_program_min = 0xFFFFFFFFu;
    g_program_max = 0;
    g_pending_kind = 0;
    g_fail_program_at = g_corrupt_program_at = g_fail_erase_at = g_bad_erase_at = 0;
    g_stuck_busy = false;
    g_sts_reads_while_busy = 0;
}

static bool outside_store_unchanged(const uint8_t *before)
{
    for (uint32_t i = 0; i < FLASH_SIZE; i++) {
        const uint32_t a = FLASH_BASE + i;
        if (!(a >= 0x08030000u && a < 0x0803C000u) && g_flash[i] != before[i]) {
            return false;
        }
    }
    return true;
}

/* The driver must refuse before it touches any register. */
static void expect_refused_untouched(const km_store_ops_t *o, uint32_t addr, bool erase)
{
    const unsigned regs = g_reg_accesses, viol = g_violations, kw = g_key_writes;
    uint8_t unit[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const bool r = erase ? o->erase(o->ctx, addr) : o->program(o->ctx, addr, unit);
    CHECK(!r);
    CHECK_EQ(g_reg_accesses, regs);
    CHECK_EQ(g_key_writes, kw);
    CHECK_EQ(g_violations, viol);
}

void test_store_flash(void)
{
    static uint8_t before[FLASH_SIZE];
    const km_store_ops_t *o = km_store_flash_ops();
    const km_store_t *st = km_store_flash();
    km_settings_t s, back;
    uint8_t scratch[KM_SETTINGS_ENCODED_SIZE];

    /* ---- the real store on the real driver: saves, alternation, only the two settings sectors touched ---- */
    sim_init(false);
    memcpy(before, g_flash, sizeof before);
    km_settings_default(&s);
    CHECK(!km_store_load_settings(st, &back, scratch, sizeof scratch));          /* nothing there yet */
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_OK);
    CHECK_EQ(g_erases, 1);
    CHECK_EQ(g_erase_addrs[0], 0x08030000u);                                     /* first save: settings A */
    km_test_random_settings(&s, 321);
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_OK);
    CHECK_EQ(g_erase_addrs[1], 0x08032000u);                                     /* then settings B */
    km_test_random_settings(&s, 322);
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_OK);
    CHECK_EQ(g_erase_addrs[2], 0x08030000u);                                     /* and back to A */
    CHECK(km_store_load_settings(st, &back, scratch, sizeof scratch));
    CHECK_EQ(back.active, s.active);
    CHECK_EQ(back.profile[1].key[9].press_mm100, s.profile[1].key[9].press_mm100);
    const unsigned erases = g_erases, programs = g_programs;
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_UNCHANGED);   /* identical: no flash traffic */
    CHECK_EQ(g_erases, erases);
    CHECK_EQ(g_programs, programs);
    CHECK_EQ(g_violations, 0);
    CHECK(outside_store_unchanged(before));
    CHECK(g_program_min >= 0x08030000u && g_program_max < 0x08034000u);
    CHECK(g_feeds > 0);
    CHECK_EQ(g_unlock_count, 1);                                                 /* unlocked once, never twice */
    CHECK_EQ(g_key_writes, 2);
    CHECK(g_programs > 3 * 600);

    /* ---- addresses outside the six sectors are refused before any register is touched ---- */
    sim_init(true);
    static const uint32_t bad[] = { 0x08000000u, 0x08006000u, 0x08007FF8u, 0x08008000u, 0x08027FF8u, 0x08028000u,
                                    0x0802A000u, 0x0802C000u, 0x0802DFF8u, 0x0802E000u, 0x0802FFF8u, 0x0803C000u,
                                    0x0803E000u, 0x08040000u, 0x08080000u, 0x20000000u, 0x40022000u, 0xFFFFFFF8u, 0u };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        expect_refused_untouched(o, bad[i], true);
        expect_refused_untouched(o, bad[i], false);
    }
    expect_refused_untouched(o, 0x08030001u, true);                              /* inside the sector but misaligned */
    expect_refused_untouched(o, 0x08031FFFu, true);
    expect_refused_untouched(o, 0x08030004u, false);                             /* program unit not 8-aligned */
    expect_refused_untouched(o, 0x08033FFCu, false);
    expect_refused_untouched(o, 0x08033FFFu, false);
    uint8_t scratch8[16];
    CHECK(!o->read(o->ctx, 0x08008000u, scratch8, 8));                           /* reads outside the store sectors too */
    CHECK(!o->read(o->ctx, 0x0802C000u, scratch8, 8));
    CHECK(!o->read(o->ctx, 0x08035FFCu, scratch8, 8));
    CHECK(o->read(o->ctx, 0x08035FF8u, scratch8, 8));                            /* the last 8 bytes of the calibration sector */
    CHECK_EQ(g_read_accesses, 8);
    CHECK_EQ(g_violations, 0);
    CHECK_EQ(g_erases + g_programs, 0);

    /* the calibration sector is writable (task T-F will use it) */
    uint8_t unit[8] = { 0xA5, 1, 2, 3, 4, 5, 6, 7 };
    CHECK(o->erase(o->ctx, 0x08034000u));
    CHECK(o->program(o->ctx, 0x08034000u, unit));                                /* lower unit of the line first, then the upper */
    CHECK(o->program(o->ctx, 0x08034008u, unit));
    CHECK(memcmp(&g_flash[0x34008], unit, 8) == 0);
    CHECK_EQ(g_violations, 0);

    /* ---- hardware error paths: every one must return false (and recover on the next call) ---- */
    sim_init(true);
    CHECK(o->erase(o->ctx, 0x08032000u));
    g_fail_program_at = 1;                                                       /* controller error flag on the first unit */
    CHECK(!o->program(o->ctx, 0x08032000u, unit));
    CHECK(o->program(o->ctx, 0x08032008u, unit));                                /* flags are cleared again by the next call */

    sim_init(true);
    CHECK(o->erase(o->ctx, 0x08032000u));
    g_corrupt_program_at = 1;                                                    /* status OK but a stored bit is wrong */
    CHECK(!o->program(o->ctx, 0x08032000u, unit));                               /* caught by the read-back */

    sim_init(true);
    g_fail_erase_at = 1;
    CHECK(!o->erase(o->ctx, 0x08030000u));
    CHECK(o->erase(o->ctx, 0x08030000u));

    sim_init(true);
    g_bad_erase_at = 1;                                                          /* erase reports OK but leaves a byte programmed */
    CHECK(!o->erase(o->ctx, 0x08030000u));                                       /* blank check catches it */

    /* a controller that stays busy forever: bounded wait, watchdog fed, false instead of a hang */
    sim_init(true);
    g_stuck_busy = true;
    CHECK(!o->erase(o->ctx, 0x08030000u));
    CHECK(g_feeds > 100);
    CHECK_EQ(g_erases, 0);
    CHECK(g_sts_reads_while_busy > 1000);
    const unsigned feeds = g_feeds;
    CHECK(!o->program(o->ctx, 0x08030000u, unit));
    CHECK(g_feeds >= feeds);
    CHECK_EQ(g_programs, 0);

    /* ---- through the store: a failing program mid-save reports the error and keeps the old record ---- */
    sim_init(true);
    km_settings_default(&s);
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_OK);
    km_test_random_settings(&s, 9);
    g_fail_program_at = (int)g_programs + 100;
    CHECK(km_store_save_settings(st, &s, scratch, sizeof scratch) == KM_STORE_ERR_PROGRAM);
    km_settings_default(&s);
    CHECK(km_store_load_settings(st, &back, scratch, sizeof scratch));           /* old (default) settings still load */
    CHECK_EQ(back.active, 0);
    CHECK_EQ(g_violations, 0);

    /* ---- the calibration log on the real driver: only 0x08034000 is erased or programmed, one erase per 24 records ---- */
    sim_init(true);
    memcpy(before, g_flash, sizeof before);
    const km_clog_t *cl = km_calstore_flash();
    CHECK(cl != NULL);
    CHECK_EQ(cl->slots, 24);
    km_cal_record_t rec, rec_back;
    memset(&rec, 0, sizeof rec);
    CHECK(!km_cal_load(cl, &rec_back));
    for (unsigned i = 0; i < 49; i++) {
        for (unsigned k = 0; k < 40; k++) {
            rec.base[k] = (uint16_t)(3000u + k);
            rec.bottom[k] = (uint16_t)(2000u - (i & 0x3Fu) * 3u + k);
        }
        CHECK(km_cal_save(cl, &rec) == KM_CLOG_OK);
        CHECK(km_cal_load(cl, &rec_back));
        CHECK(memcmp(&rec, &rec_back, sizeof rec) == 0);
    }
    CHECK_EQ(g_erases, 2);                                                       /* the 25th and the 49th record */
    CHECK_EQ(g_erase_addrs[0], 0x08034000u);
    CHECK_EQ(g_erase_addrs[1], 0x08034000u);
    CHECK(km_cal_save(cl, &rec) == KM_CLOG_UNCHANGED);
    CHECK_EQ(g_erases, 2);
    CHECK_EQ(g_violations, 0);
    CHECK(outside_store_unchanged(before));
    CHECK(g_program_min >= 0x08034000u && g_program_max < 0x08036000u);          /* settings A and B were never touched */
    CHECK(g_feeds > 0);
    /* a controller error in the middle of a record: the previous record stays the newest valid one */
    km_cal_record_t keep = rec;
    rec.bottom[3] = (uint16_t)(rec.bottom[3] - 40u);
    g_fail_program_at = (int)g_programs + 7;
    CHECK(km_cal_save(cl, &rec) == KM_CLOG_ERR_PROGRAM);
    CHECK(km_cal_load(cl, &rec_back));
    CHECK(memcmp(&keep, &rec_back, sizeof keep) == 0);
    CHECK(km_cal_save(cl, &rec) == KM_CLOG_OK);                                  /* the torn slot is skipped */
    CHECK(km_cal_load(cl, &rec_back));
    CHECK(memcmp(&rec, &rec_back, sizeof rec) == 0);
    CHECK_EQ(g_violations, 0);
    /* blank 8-byte units inside the data (a lower unit next to data, a whole blank line, a blank upper unit): the controller refuses
     * an upper unit whose lower unit was never programmed, so a blank lower unit still gets its (0xFF) program event */
    {
        sim_init(true);
        const unsigned plen = (unsigned)cl->payload_len;
        uint8_t pat[KM_CLOG_MAX_PAYLOAD], got[KM_CLOG_MAX_PAYLOAD];
        for (unsigned i = 0; i < plen; i++) {
            pat[i] = (uint8_t)(i * 7u + 3u);
        }
        memset(&pat[0], 0xFF, 8);                                                /* first unit blank, its upper unit has data */
        memset(&pat[32], 0xFF, 16);                                              /* a whole blank line */
        memset(&pat[72], 0xFF, 8);                                               /* blank lower unit */
        memset(&pat[112], 0xFF, 8);                                              /* blank upper unit of line 96..111 + lower of the next is data */
        km_clog_job_t job;
        km_clog_result_t r = km_clog_job_begin(&job, cl, pat);
        while (r == KM_CLOG_PENDING) {
            r = km_clog_job_step(&job);
        }
        CHECK(r == KM_CLOG_OK);
        CHECK(km_clog_read(cl, got, sizeof got));
        CHECK(memcmp(got, pat, plen) == 0);
        CHECK_EQ(g_violations, 0);
        /* the same holes through the settings store */
        const km_store_t *st2 = km_store_flash();
        static uint8_t blob[600];
        for (unsigned i = 0; i < sizeof blob; i++) {
            blob[i] = (uint8_t)(i * 5u + 1u);
        }
        memset(&blob[0], 0xFF, 8);
        memset(&blob[40], 0xFF, 8);
        memset(&blob[104], 0xFF, 16);
        memset(&blob[200], 0xFF, 8);
        CHECK(km_store_write(st2, blob, sizeof blob) == KM_STORE_OK);
        static uint8_t rb[600];
        uint32_t rlen = 0;
        CHECK(km_store_read(st2, rb, sizeof rb, &rlen));
        CHECK_EQ(rlen, (uint32_t)sizeof blob);
        CHECK(memcmp(rb, blob, sizeof blob) == 0);
        CHECK_EQ(g_violations, 0);
    }
    /* ---- the light scene on the real driver: only 0x08038000 / 0x0803A000 are erased or programmed, the records alternate ---- */
    sim_init(true);
    memcpy(before, g_flash, sizeof before);
    const km_store_t *ss = km_lightscene_store_flash();
    CHECK(ss != NULL && ss == km_lightscene_store_flash());
    km_lightscene_t sc, sc_back;
    km_lightscene_default(&sc);
    CHECK(!km_lightscene_load(ss, &sc_back));
    for (unsigned i = 0; i < 5; i++) {
        sc.n_rules = (uint8_t)(i % 3u);
        for (unsigned k = 0; k < sc.n_rules; k++) {
            sc.rule[k] = (km_ls_rule_t){ KM_LS_SRC_HOST_CAPS, 0, (uint8_t)(20u + i + k), 0, { 255, (uint8_t)i, 0 }, 0 };
        }
        sc.anim_clip = (uint8_t)i;
        CHECK(km_lightscene_save(ss, &sc) == KM_STORE_OK);
        CHECK(km_lightscene_load(ss, &sc_back));
        CHECK_EQ(sc_back.n_rules, sc.n_rules);
        CHECK_EQ(sc_back.anim_clip, i);
    }
    CHECK(km_lightscene_save(ss, &sc) == KM_STORE_UNCHANGED);
    CHECK_EQ(g_erases, 5);                                                       /* one erase per save, A B A B A */
    CHECK_EQ(g_erase_addrs[0], 0x08038000u);
    CHECK_EQ(g_erase_addrs[1], 0x0803A000u);
    CHECK_EQ(g_violations, 0);
    CHECK(outside_store_unchanged(before));
    CHECK(g_program_min >= 0x08038000u && g_program_max < 0x0803C000u);          /* settings, calibration and lights were never touched */
    CHECK(g_feeds > 0);
    {   /* a controller error in the middle of a save keeps the previous scene */
        const unsigned keep_clip = sc.anim_clip;
        sc.anim_clip = 99;
        g_fail_program_at = (int)g_programs + 5;
        CHECK(km_lightscene_save(ss, &sc) == KM_STORE_ERR_PROGRAM);
        CHECK(km_lightscene_load(ss, &sc_back));
        CHECK_EQ(sc_back.anim_clip, keep_clip);
        CHECK(km_lightscene_save(ss, &sc) == KM_STORE_OK);
        CHECK(km_lightscene_load(ss, &sc_back));
        CHECK_EQ(sc_back.anim_clip, 99);
        CHECK_EQ(g_violations, 0);
    }
}
