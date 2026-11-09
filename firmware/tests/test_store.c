/*
 * km_store on a simulated flash that can lose power after any single erase/program step.
 *
 * The simulation follows what flash_boot.c shows the N32H48x controller does: erase works on 8 KB sectors, a
 * program unit is 8 bytes (two 32-bit words, 8-byte aligned), programming can only clear bits, and a unit is only
 * ever programmed while it is still erased (the store never programs a unit twice). A power cut is modelled as the
 * worst plausible partial effect of the step in progress:
 *   program  v0 nothing, v1 low word done / high word not, v2 low word done / high word half-programmed,
 *            v3 low word half-programmed
 *   erase    v0 nothing, v1 first half erased, v2 whole sector garbage, v3 second half erased,
 *            v4 first half erased / second half garbage
 * For EVERY step of a save, under every variant, the flash is "rebooted" and must load as the old settings or the
 * new ones (never garbage, never a mix); then a follow-up save must succeed and load back exactly.
 */
#include <stdlib.h>

#include "km/km_flash_guard.h"
#include "km/km_settings.h"
#include "km/km_store.h"
#include "km_test.h"

void km_test_random_settings(km_settings_t *s, uint32_t seed);   /* test_settings.c */

#define BASE   KM_FLASH_SETTINGS_A
#define SECT   0x2000u
#define SZ     KM_SETTINGS_ENCODED_SIZE

typedef struct {
    uint8_t  mem[2 * SECT];
    bool     dead;                  /* power is gone: nothing changes any more */
    int      ev;                    /* erase/program steps so far */
    int      cut_ev, cut_var;       /* power cut during step cut_ev with this variant, -1 = never */
    int      fail_ev, fail_var;     /* step reports an error (power stays on) after the partial effect of fail_var */
    int      corrupt_ev;            /* step reports success but stores one bit wrong */
    unsigned violations;            /* accesses outside the two sectors, misaligned, over non-erased data, guard breaches */
    unsigned erases[2], programs;
} sim_t;

static sim_t g_sim, g_start;

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

static bool in_range(uint32_t addr, uint32_t len)
{
    return addr >= BASE && addr + len <= BASE + 2 * SECT && addr + len >= addr;
}

static bool sim_read(void *c, uint32_t addr, uint8_t *dst, uint32_t len)
{
    sim_t *s = c;
    if (!in_range(addr, len)) {
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
        const uint32_t clear = old & ~want;          /* the bits this word would clear */
        uint32_t done = clear;
        if (variant == 0 || (variant == 1 && w == 1) || (variant == 3 && w == 1)) {
            done = 0;
        } else if ((variant == 2 && w == 1) || (variant == 3 && w == 0)) {
            done = clear & 0x55555555u;              /* half-programmed word */
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
    if ((addr & 8u) == 0) {                          /* the hardware refuses a unit below one already programmed in its 16-byte line */
        for (unsigned i = 0; i < 8; i++) {
            if (s->mem[off + 8 + i] != 0xFF) {
                s->violations++;
                break;
            }
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
        unsigned i = 0;
        while (i < 8 && d[i] == 0) {
            i++;
        }
        if (i < 8) {
            s->mem[off + i] = (uint8_t)(d[i] & (d[i] - 1u));       /* clear the lowest set bit */
        } else {
            s->mem[off] = 1;                                        /* an all-zero unit: one bit comes out wrong the other way */
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
    if ((addr & (SECT - 1u)) != 0 || !in_range(addr, SECT) || !km_flash_store_allowed(addr, SECT)) {
        s->violations++;
        return false;
    }
    const uint32_t off = addr - BASE;
    const int ev = s->ev++;
    if (ev == s->cut_ev || ev == s->fail_ev) {
        const int v = (ev == s->cut_ev) ? s->cut_var : s->fail_var;
        for (uint32_t i = 0; i < SECT; i++) {
            const bool first = i < SECT / 2;
            switch (v) {
            case 1: if (first) s->mem[off + i] = 0xFF; break;
            case 2: s->mem[off + i] = (uint8_t)mix(addr + i); break;
            case 3: if (!first) s->mem[off + i] = 0xFF; break;
            case 4: s->mem[off + i] = first ? 0xFF : (uint8_t)mix(addr + i); break;
            default: break;
            }
        }
        if (ev == s->cut_ev) {
            s->dead = true;
        }
        return false;
    }
    memset(&s->mem[off], 0xFF, SECT);
    s->erases[(addr - BASE) / SECT]++;
    return true;
}

static km_store_t g_store;

static void sim_blank(void)
{
    memset(&g_sim, 0, sizeof g_sim);
    memset(g_sim.mem, 0xFF, sizeof g_sim.mem);
    g_sim.cut_ev = g_sim.fail_ev = g_sim.corrupt_ev = -1;
    static const km_store_ops_t ops = { NULL, sim_read, sim_erase, sim_program };
    km_store_ops_t o = ops;
    o.ctx = &g_sim;
    km_store_init(&g_store, &o, BASE, BASE + SECT);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Write a finished record straight into the memory, from the documented layout (independent of km_store.c). */
static void forge(unsigned idx, uint32_t seq, const uint8_t *data, uint32_t len)
{
    uint8_t *r = &g_sim.mem[idx * SECT];
    memset(r, 0xFF, SECT);
    put32(r + 0, 0x53524D4Bu);                       /* "KMRS" */
    put32(r + 8, seq);
    put32(r + 12, len);
    memcpy(r + 16, data, len);
    put32(r + 4, km_crc32(0, r + 8, 8 + len));       /* seq, len and payload are contiguous */
}

static void fill_junk(unsigned idx, uint32_t seed)
{
    for (uint32_t i = 0; i < SECT; i++) {
        g_sim.mem[idx * SECT + i] = (uint8_t)mix(seed + i);
    }
}

static uint8_t pay[4][SZ];                           /* four distinct settings blobs */

static void make_payloads(void)
{
    for (unsigned i = 0; i < 4; i++) {
        km_settings_t s;
        km_test_random_settings(&s, 500u + i * 31u);
        CHECK_EQ(km_settings_encode(&s, pay[i], SZ), SZ);
    }
}

typedef enum { NONE, IS_OLD, IS_NEW, IS_OTHER } which_t;

static which_t classify(const uint8_t *old, const uint8_t *nw)
{
    uint8_t buf[SZ];
    uint32_t len = 0;
    if (!km_store_read(&g_store, buf, sizeof buf, &len)) {
        return NONE;
    }
    if (len == SZ && nw != NULL && memcmp(buf, nw, SZ) == 0) {
        return IS_NEW;
    }
    if (len == SZ && old != NULL && memcmp(buf, old, SZ) == 0) {
        return IS_OLD;
    }
    return IS_OTHER;
}

static unsigned g_cut_points;

/*
 * g_start holds the flash before the save. Dry run to count the steps, then cut power at every step under every
 * variant. 'old' is the payload that must still be there (NULL = none).
 */
static void cut_everywhere(const char *name, const uint8_t *old, const uint8_t *nw, const uint8_t *third)
{
    memcpy(&g_sim, &g_start, sizeof g_sim);
    CHECK(km_store_write(&g_store, nw, SZ) == KM_STORE_OK);
    CHECK_EQ(g_sim.violations, 0);
    CHECK(classify(old, nw) == IS_NEW);
    const int steps = g_sim.ev;
    CHECK(steps > 600);                              /* ~5.5 KB payload = ~690 units + header + the erase */

    unsigned seen_old = 0, seen_new = 0, bad = 0;
    for (int ev = 0; ev < steps; ev++) {
        const int nvar = (ev == 0) ? 5 : 4;          /* step 0 is the erase */
        for (int v = 0; v < nvar; v++) {
            memcpy(&g_sim, &g_start, sizeof g_sim);
            g_sim.cut_ev = ev;
            g_sim.cut_var = v;
            const km_store_result_t r = km_store_write(&g_store, nw, SZ);
            CHECK(r != KM_STORE_OK && r != KM_STORE_UNCHANGED);   /* power was lost: the save cannot have succeeded */
            CHECK(g_sim.dead);
            CHECK_EQ(g_sim.violations, 0);
            g_cut_points++;

            /* power is back: either the old settings, the new ones, or (first save only) nothing */
            g_sim.dead = false;
            g_sim.cut_ev = -1;
            const which_t w = classify(old, nw);
            const bool ok = (w == IS_OLD) || (w == IS_NEW) || (w == NONE && old == NULL);
            if (!ok) {
                bad++;
                printf("  FAIL %s: cut at step %d variant %d loads %d\n", name, ev, v, (int)w);
            }
            seen_old += (w == IS_OLD);
            seen_new += (w == IS_NEW);
            /* the next save after the accident must work and read back exactly */
            if (km_store_write(&g_store, third, SZ) != KM_STORE_OK || classify(NULL, third) != IS_NEW ||
                g_sim.violations != 0) {
                bad++;
                printf("  FAIL %s: follow-up save after cut at step %d variant %d\n", name, ev, v);
            }
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(seen_old > 0 || old == NULL);              /* the sweep really covered "still the old one" */
    (void)seen_new;
}

/* Fail (power stays on) or silently corrupt each step in turn: the old record must stay the current one. */
static void fail_everywhere(const char *name, const uint8_t *old, const uint8_t *nw)
{
    memcpy(&g_sim, &g_start, sizeof g_sim);
    CHECK(km_store_write(&g_store, nw, SZ) == KM_STORE_OK);
    const int steps = g_sim.ev;
    unsigned bad = 0;
    for (int ev = 0; ev < steps; ev++) {
        for (int mode = 0; mode < 3; mode++) {       /* 0 clean error, 1 error after half a unit, 2 silent corruption */
            memcpy(&g_sim, &g_start, sizeof g_sim);
            if (mode == 2) {
                g_sim.corrupt_ev = ev;
                if (ev == 0) {
                    continue;                        /* an erase cannot be "corrupted" in this model */
                }
            } else {
                g_sim.fail_ev = ev;
                g_sim.fail_var = (mode == 0) ? 0 : (ev == 0 ? 4 : 2);
            }
            const km_store_result_t r = km_store_write(&g_store, nw, SZ);
            const km_store_result_t want = (mode == 2) ? KM_STORE_ERR_VERIFY : (ev == 0 ? KM_STORE_ERR_ERASE : KM_STORE_ERR_PROGRAM);
            if (r != want) {
                bad++;
                printf("  FAIL %s: step %d mode %d returned %d, want %d\n", name, ev, mode, (int)r, (int)want);
            }
            g_sim.fail_ev = g_sim.corrupt_ev = -1;
            const which_t w = classify(old, nw);
            if (!((w == IS_OLD) || (w == NONE && old == NULL))) {
                bad++;
                printf("  FAIL %s: step %d mode %d left %d\n", name, ev, mode, (int)w);
            }
            if (g_sim.violations != 0) {
                bad++;
            }
        }
    }
    CHECK_EQ(bad, 0);
}

static void scenario_states(unsigned which)
{
    sim_blank();
    switch (which) {
    case 0: break;                                                   /* both sectors erased */
    case 1: fill_junk(0, 11); fill_junk(1, 22); break;               /* both hold foreign data (stock firmware's) */
    case 2: forge(0, 1, pay[0], SZ); break;                          /* one record in A, B erased */
    case 3: forge(0, 7, pay[0], SZ); forge(1, 8, pay[1], SZ); break; /* both valid, B newer: next save goes to A */
    case 4: forge(1, 3, pay[0], SZ); forge(0, 4, pay[1], SZ); break; /* A newer: next save goes to B */
    case 5: forge(0, 0xFFFFFFFFu, pay[0], SZ); forge(1, 0xFFFFFFFEu, pay[1], SZ); break;   /* sequence about to wrap */
    case 6: forge(1, 0xFFFFFFFFu, pay[0], SZ); forge(0, 0xFFFFFFFEu, pay[1], SZ); fill_junk(0, 5); break; /* older one damaged */
    default: break;
    }
    memcpy(&g_start, &g_sim, sizeof g_sim);
}

static const uint8_t *scenario_old(unsigned which)
{
    switch (which) {
    case 0: case 1: return NULL;
    case 2: return pay[0];
    case 3: return pay[1];
    case 4: return pay[1];
    case 5: return pay[0];
    case 6: return pay[0];
    default: return NULL;
    }
}

static void test_basic(void)
{
    uint8_t buf[SZ];
    uint32_t len;
    km_store_info_t info;

    /* blank flash: nothing to load */
    sim_blank();
    CHECK(!km_store_find(&g_store, &info));
    CHECK(!km_store_read(&g_store, buf, sizeof buf, &len));

    /* alternation, sequence numbers, wear */
    for (unsigned i = 0; i < 100; i++) {
        pay[0][100] = (uint8_t)i;                                    /* a different payload every time */
        CHECK(km_store_write(&g_store, pay[0], SZ) == KM_STORE_OK);
        CHECK(km_store_find(&g_store, &info));
        CHECK_EQ(info.seq, i + 1);
        CHECK_EQ(info.index, i & 1u);                                /* first save goes to sector 0, then they alternate */
        CHECK_EQ(info.len, SZ);
        CHECK(km_store_read(&g_store, buf, sizeof buf, &len) && len == SZ && memcmp(buf, pay[0], SZ) == 0);
    }
    CHECK_EQ(g_sim.erases[0], 50);
    CHECK_EQ(g_sim.erases[1], 50);
    CHECK_EQ(g_sim.violations, 0);
    km_store_info_t other;
    CHECK(km_store_check_sector(&g_store, 0, &other) && km_store_check_sector(&g_store, 1, &other));   /* both hold a record */

    /* saving the same bytes again writes nothing */
    const int ev_before = g_sim.ev;
    const unsigned prog_before = g_sim.programs;
    CHECK(km_store_write(&g_store, pay[0], SZ) == KM_STORE_UNCHANGED);
    CHECK_EQ(g_sim.ev, ev_before);
    CHECK_EQ(g_sim.programs, prog_before);
    pay[0][SZ - 1] ^= 1;                                             /* one changed byte does write */
    CHECK(km_store_write(&g_store, pay[0], SZ) == KM_STORE_OK);
    CHECK(g_sim.ev > ev_before);
    pay[0][SZ - 1] ^= 1;

    /* payload sizes: 1, 7, 8, 9, odd, and the maximum */
    static const uint32_t sizes[] = { 1, 7, 8, 9, 63, 64, 65, 1000, KM_STORE_MAX_PAYLOAD };
    static uint8_t big[KM_STORE_MAX_PAYLOAD + 8];
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        sim_blank();
        for (uint32_t i = 0; i < sizes[k]; i++) {
            big[i] = (uint8_t)mix(i + k);
        }
        CHECK(km_store_write(&g_store, big, sizes[k]) == KM_STORE_OK);
        static uint8_t rd[KM_STORE_MAX_PAYLOAD + 8];
        CHECK(km_store_read(&g_store, rd, sizeof rd, &len) && len == sizes[k] && memcmp(rd, big, len) == 0);
        CHECK(!km_store_read(&g_store, rd, sizes[k] - 1, &len));     /* buffer too small */
        big[0] ^= 0x80;
        CHECK(km_store_write(&g_store, big, sizes[k]) == KM_STORE_OK);
        CHECK(km_store_read(&g_store, rd, sizeof rd, &len) && len == sizes[k] && memcmp(rd, big, len) == 0);
        CHECK_EQ(g_sim.violations, 0);
    }
    CHECK(km_store_write(&g_store, big, 0) == KM_STORE_ERR_ARG);
    CHECK(km_store_write(&g_store, big, KM_STORE_MAX_PAYLOAD + 1) == KM_STORE_ERR_ARG);
    CHECK(km_store_write(&g_store, NULL, 10) == KM_STORE_ERR_ARG);
    CHECK_EQ(g_sim.violations, 0);

    /* damage to the current record falls back to the previous one */
    sim_blank();
    CHECK(km_store_write(&g_store, pay[0], SZ) == KM_STORE_OK);      /* sector 0, seq 1 */
    CHECK(km_store_write(&g_store, pay[1], SZ) == KM_STORE_OK);      /* sector 1, seq 2 */
    CHECK(classify(pay[0], pay[1]) == IS_NEW);
    g_sim.mem[SECT + 16 + 2000] ^= 0x01;                             /* bit rot in the newest record's payload */
    CHECK(classify(pay[0], pay[1]) == IS_OLD);
    g_sim.mem[16 + 100] ^= 0x80;                                     /* ... and now in the old one too */
    CHECK(classify(pay[0], pay[1]) == NONE);

    /* equal sequence numbers (cannot happen on its own): deterministic, sector 0 wins */
    sim_blank();
    forge(0, 9, pay[0], SZ);
    forge(1, 9, pay[1], SZ);
    CHECK(km_store_find(&g_store, &info) && info.index == 0);
    CHECK(classify(pay[0], pay[1]) == IS_OLD);

    /* sequence wrap: 0xFFFFFFFF -> 0, and 0 counts as newer */
    sim_blank();
    forge(0, 0xFFFFFFFFu, pay[0], SZ);
    CHECK(km_store_write(&g_store, pay[1], SZ) == KM_STORE_OK);
    CHECK(km_store_find(&g_store, &info) && info.index == 1 && info.seq == 0);
    CHECK(classify(pay[0], pay[1]) == IS_NEW);
    CHECK(km_store_write(&g_store, pay[2], SZ) == KM_STORE_OK);
    CHECK(km_store_find(&g_store, &info) && info.index == 0 && info.seq == 1);

    /* foreign content in both sectors: no record; the first save replaces it */
    sim_blank();
    fill_junk(0, 1);
    fill_junk(1, 2);
    CHECK(!km_store_find(&g_store, &info));
    CHECK(km_store_write(&g_store, pay[0], SZ) == KM_STORE_OK);
    CHECK(classify(NULL, pay[0]) == IS_NEW);
    memset(g_sim.mem, 0x00, sizeof g_sim.mem);                       /* all zeros */
    CHECK(!km_store_find(&g_store, &info));
    memset(g_sim.mem, 0xFF, sizeof g_sim.mem);
    CHECK(!km_store_find(&g_store, &info));

    /* a header that claims a huge or zero length is refused without reading past the sector */
    sim_blank();
    forge(0, 1, pay[0], SZ);
    put32(&g_sim.mem[12], 0xFFFFFFF0u);
    CHECK(!km_store_find(&g_store, &info));
    put32(&g_sim.mem[12], 0);
    CHECK(!km_store_find(&g_store, &info));
    CHECK_EQ(g_sim.violations, 0);
}

static void test_settings_layer(void)
{
    km_settings_t s, d, back;
    uint8_t scratch[SZ];

    sim_blank();
    km_settings_default(&d);
    CHECK(!km_store_load_settings(&g_store, &back, scratch, sizeof scratch));       /* nothing stored yet */
    CHECK(km_store_save_settings(&g_store, &d, scratch, sizeof scratch) == KM_STORE_OK);
    CHECK(km_store_save_settings(&g_store, &d, scratch, sizeof scratch) == KM_STORE_UNCHANGED);
    km_test_random_settings(&s, 4242);
    CHECK(km_store_save_settings(&g_store, &s, scratch, sizeof scratch) == KM_STORE_OK);
    memset(&back, 0, sizeof back);
    CHECK(km_store_load_settings(&g_store, &back, scratch, sizeof scratch));
    CHECK_EQ(back.active, s.active);
    CHECK_EQ(back.profile[2].key[17].rt_down_mm100, s.profile[2].key[17].rt_down_mm100);
    CHECK_EQ(back.profile[3].layer_usage[3][75], s.profile[3].layer_usage[3][75]);

    s.active = KM_PROFILES;                                                         /* invalid settings never reach the flash */
    const int ev = g_sim.ev;
    CHECK(km_store_save_settings(&g_store, &s, scratch, sizeof scratch) == KM_STORE_ERR_ARG);
    CHECK_EQ(g_sim.ev, ev);
    CHECK(km_store_save_settings(&g_store, &d, scratch, SZ - 1) == KM_STORE_ERR_ARG);

    /* a valid store record that is not a settings blob: load says no, the caller uses defaults */
    sim_blank();
    forge(0, 1, pay[0], 1000);
    CHECK(!km_store_load_settings(&g_store, &back, scratch, sizeof scratch));
    memcpy(&back, &d, sizeof back);
    uint8_t blob[SZ];
    memcpy(blob, pay[0], SZ);
    blob[50] ^= 1;                                                                  /* settings blob with a wrong CRC inside a good record */
    forge(0, 2, blob, SZ);
    CHECK(!km_store_load_settings(&g_store, &back, scratch, sizeof scratch));

    /* the argument-less API */
    sim_blank();
    km_store_bind(&g_store);
    CHECK(!km_store_load(&back));
    CHECK(km_store_save(&d));
    CHECK(km_store_save(&d));                                                       /* identical: true, nothing written */
    km_test_random_settings(&s, 77);
    CHECK(km_store_save(&s));
    CHECK(km_store_load(&back));
    CHECK_EQ(back.active, s.active);
    s.active = 99;
    CHECK(!km_store_save(&s));
}

void test_store(void)
{
    make_payloads();
    test_basic();
    test_settings_layer();

    static const char *names[] = { "first save on erased flash", "first save over foreign data", "second save, record in A",
                                   "both valid, B newer", "both valid, A newer", "sequence wrap", "older record damaged" };
    g_cut_points = 0;
    for (unsigned sc = 0; sc < 7; sc++) {
        scenario_states(sc);
        cut_everywhere(names[sc], scenario_old(sc), pay[2], pay[3]);
        scenario_states(sc);
        fail_everywhere(names[sc], scenario_old(sc), pay[2]);
    }
    printf("  store: %u power-cut points (every erase/program step x partial-effect variants), 7 start states\n", g_cut_points);
    CHECK(g_cut_points > 15000);
}
