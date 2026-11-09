/*
 * km_ledprof (per-profile light table: codec, lookup rules, boot migration from the version-1 record) and the glue in m5_led.c
 * (profile switch, per-profile LED_SET / LED_GET, persistence) on a simple RAM flash. The power-cut behaviour of the log itself is
 * covered by test_calstore.c / test_ledstore.c (same km_clog code); here the point is WHAT is stored and WHEN it is applied.
 */
#include "km/km_calstore.h"
#include "km/km_config.h"
#include "km/km_flash_guard.h"
#include "km/km_ledprof.h"
#include "km/km_ledstore.h"
#include "km_test.h"
#include "m5_led.h"

#define BASE KM_FLASH_LED_CONFIG
#define SECT 0x2000u

static uint8_t g_mem[SECT];
static unsigned g_erases, g_violations;
static km_clog_t g_log2, g_log1;

static bool mem_read(void *c, uint32_t addr, uint8_t *dst, uint32_t len)
{
    (void)c;
    if (addr < BASE || addr + len > BASE + SECT || !km_flash_store_allowed(addr, len)) {
        g_violations++;
        return false;
    }
    memcpy(dst, &g_mem[addr - BASE], len);
    return true;
}

static bool mem_program(void *c, uint32_t addr, const uint8_t d[8])
{
    (void)c;
    if ((addr & 7u) != 0u || addr < BASE || addr + 8u > BASE + SECT || !km_flash_store_allowed(addr, 8)) {
        g_violations++;
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {
        if (g_mem[addr - BASE + i] != 0xFFu) {
            g_violations++;                              /* programmed twice between erases */
        }
        g_mem[addr - BASE + i] = (uint8_t)(g_mem[addr - BASE + i] & d[i]);
    }
    return true;
}

static bool mem_erase(void *c, uint32_t addr)
{
    (void)c;
    if (addr != BASE) {
        g_violations++;
        return false;
    }
    memset(g_mem, 0xFF, sizeof g_mem);
    g_erases++;
    return true;
}

static void flash_blank(void)
{
    memset(g_mem, 0xFF, sizeof g_mem);
    g_erases = g_violations = 0;
    const km_store_ops_t ops = { NULL, mem_read, mem_erase, mem_program };
    CHECK(km_clog_init(&g_log2, &ops, BASE, KM_LEDPROF_PAYLOAD_BYTES));
    CHECK(km_clog_init(&g_log1, &ops, BASE, KM_LEDSTORE_PAYLOAD_BYTES));
}

static km_ledstore_rec_t rec_of(uint8_t effect, uint8_t r, uint8_t g, uint8_t b, uint16_t idle)
{
    km_ledstore_rec_t x;
    km_ledstore_rec_default(&x);
    x.cfg.effect = effect;
    x.cfg.color.r = r;
    x.cfg.color.g = g;
    x.cfg.color.b = b;
    x.idle_s = idle;
    return x;
}

static bool same_rec(const km_ledstore_rec_t *a, const km_ledstore_rec_t *b)
{
    return a->cfg.effect == b->cfg.effect && a->cfg.brightness == b->cfg.brightness && a->cfg.speed == b->cfg.speed &&
           a->cfg.color.r == b->cfg.color.r && a->cfg.color.g == b->cfg.color.g && a->cfg.color.b == b->cfg.color.b && a->idle_s == b->idle_s;
}

static void test_codec(void)
{
    CHECK_EQ(KM_LEDPROF_PAYLOAD_BYTES, 64);
    CHECK(KM_LEDPROF_PAYLOAD_BYTES <= KM_LEDSTORE_WANT_MAX);

    km_ledprof_t t, u;
    km_ledprof_default(&t);
    CHECK_EQ(t.mask, 0);
    km_ledstore_rec_t r;
    CHECK(!km_ledprof_effective(&t, 0, &r));              /* nothing set: no answer */

    const km_ledstore_rec_t g = rec_of(1, 10, 20, 30, 600), p2 = rec_of(2, 255, 0, 0, 0);
    CHECK(km_ledprof_set_global(&t, &g));
    CHECK(km_ledprof_set(&t, 2, &p2));
    CHECK(!km_ledprof_set(&t, 4, &p2));                   /* only profiles 0..3 */
    CHECK(km_ledprof_effective(&t, 0, &r) && same_rec(&r, &g));     /* no own entry: the global one */
    CHECK(km_ledprof_effective(&t, 3, &r) && same_rec(&r, &g));
    CHECK(km_ledprof_effective(&t, 2, &r) && same_rec(&r, &p2));    /* own entry wins */
    CHECK(km_ledprof_has_own(&t, 2) && !km_ledprof_has_own(&t, 0));
    CHECK(!km_ledprof_effective(&t, 4, &r));

    uint8_t b[KM_LEDPROF_PAYLOAD_BYTES + 8];
    CHECK_EQ(km_ledprof_encode(&t, b, sizeof b), KM_LEDPROF_PAYLOAD_BYTES);
    CHECK_EQ(km_ledprof_encode(&t, b, 63), 0);
    CHECK_EQ(b[0], 2);
    CHECK_EQ(b[1], 0x09);                                 /* global (bit 0) + profile 2 (bit 3) */
    km_ledprof_default(&u);
    CHECK(km_ledprof_decode(&u, b, KM_LEDPROF_PAYLOAD_BYTES));
    CHECK_EQ(u.mask, t.mask);
    CHECK(km_ledprof_effective(&u, 2, &r) && same_rec(&r, &p2));
    CHECK(km_ledprof_effective(&u, 1, &r) && same_rec(&r, &g));

    /* a cleared entry is stored as zeros, whatever RAM holds */
    CHECK(km_ledprof_clear(&t, 2));
    CHECK(km_ledprof_effective(&t, 2, &r) && same_rec(&r, &g));
    CHECK_EQ(km_ledprof_encode(&t, b, sizeof b), KM_LEDPROF_PAYLOAD_BYTES);
    for (unsigned i = 4u + 3u * 12u; i < 4u + 4u * 12u; i++) {
        CHECK_EQ(b[i], 0);
    }

    /* strictness: wrong version, mask bits, header bytes, scene reference / reserved bytes, stray data in a cleared entry, bad effect, length */
    uint8_t good[KM_LEDPROF_PAYLOAD_BYTES];
    CHECK_EQ(km_ledprof_encode(&u, good, sizeof good), KM_LEDPROF_PAYLOAD_BYTES);
    CHECK(km_ledprof_decode(&u, good, sizeof good));
    CHECK(!km_ledprof_decode(&u, good, 63));
    CHECK(!km_ledprof_decode(&u, good, 65));
    CHECK(!km_ledprof_decode(&u, NULL, 64));
    for (unsigned i = 0; i < sizeof good; i++) {
        uint8_t m[KM_LEDPROF_PAYLOAD_BYTES];
        memcpy(m, good, sizeof m);
        m[i] = (uint8_t)(m[i] ^ 0x80u);
        km_ledprof_t x;
        const bool ok = km_ledprof_decode(&x, m, sizeof m);
        /* the only bytes where flipping bit 7 stays valid are the free numeric fields: brightness, speed, colours, idle seconds (set entries) */
        bool free_field = false;
        if (i >= 4u) {
            const unsigned e = (i - 4u) / 12u, k = (i - 4u) % 12u;
            free_field = (good[1] & (1u << e)) != 0u && (k == 1u || k == 3u || k == 4u || k == 5u || k == 6u || k == 7u || (k == 2u && (m[i] != 0u)));
        }
        if (ok && !free_field) {
            printf("  flip of byte %u was accepted\n", i);
        }
        CHECK(!ok || free_field);
    }
    {
        uint8_t m[KM_LEDPROF_PAYLOAD_BYTES];
        memcpy(m, good, sizeof m);
        m[0] = 1;                                         /* the version-1 number is not a table */
        CHECK(!km_ledprof_decode(&u, m, sizeof m));
        memcpy(m, good, sizeof m);
        m[4 + 2] = 0;                                /* speed 0 is invalid */
        CHECK(!km_ledprof_decode(&u, m, sizeof m));
        memcpy(m, good, sizeof m);
        m[4 + 1] = 0x10;
        m[4] = 0xFF;                                 /* effect out of range */
        CHECK(!km_ledprof_decode(&u, m, sizeof m));
    }
}

static void test_flash_and_migration(void)
{
    km_ledprof_t t;
    km_ledprof_default(&t);

    /* blank sector: nothing */
    flash_blank();
    CHECK(!km_ledprof_load(&g_log2, &g_log1, &t));

    /* an old keyboard: only version-1 records. The newest becomes the global entry. */
    const km_ledstore_rec_t old1 = rec_of(1, 1, 2, 3, 30), old2 = rec_of(3, 4, 5, 6, 90);
    uint8_t pl[KM_LEDSTORE_PAYLOAD_BYTES];
    CHECK_EQ(km_ledstore_encode(&old1, pl, sizeof pl), 16);
    CHECK_EQ(km_clog_append(&g_log1, pl), KM_CLOG_OK);
    CHECK_EQ(km_ledstore_encode(&old2, pl, sizeof pl), 16);
    CHECK_EQ(km_clog_append(&g_log1, pl), KM_CLOG_OK);
    CHECK(km_ledprof_load(&g_log2, &g_log1, &t));
    CHECK_EQ(t.mask, 1);
    km_ledstore_rec_t r;
    CHECK(km_ledprof_effective(&t, 0, &r) && same_rec(&r, &old2));
    CHECK(km_ledprof_effective(&t, 3, &r) && same_rec(&r, &old2));
    CHECK(!km_ledprof_load(&g_log2, NULL, &t) || t.mask == 1);       /* without the legacy reader there is nothing to migrate */
    {
        km_ledprof_t none;
        km_ledprof_default(&none);
        none.mask = 0xFF;                                  /* untouched on failure */
        CHECK(!km_ledprof_load(&g_log2, NULL, &none));
        CHECK_EQ(none.mask, 0xFF);
    }

    /* the first table save lands behind the old records (they read as foreign slots), the old sector is not erased, and the table wins */
    const unsigned erases0 = g_erases;
    uint8_t tb[KM_LEDPROF_PAYLOAD_BYTES];
    const km_ledstore_rec_t p1 = rec_of(2, 255, 0, 128, 0);
    CHECK(km_ledprof_set(&t, 1, &p1));
    CHECK_EQ(km_ledprof_encode(&t, tb, sizeof tb), 64);
    CHECK_EQ(km_clog_append(&g_log2, tb), KM_CLOG_OK);
    CHECK_EQ(g_erases, erases0);
    km_ledprof_t back;
    km_ledprof_default(&back);
    CHECK(km_ledprof_load(&g_log2, &g_log1, &back));
    CHECK_EQ(back.mask, 5);
    CHECK(km_ledprof_effective(&back, 1, &r) && same_rec(&r, &p1));
    CHECK(km_ledprof_effective(&back, 0, &r) && same_rec(&r, &old2));
    CHECK_EQ(g_violations, 0);

    /* a sector completely full of version-1 records: the first table save erases first, and nothing is lost but the old record itself */
    flash_blank();
    for (unsigned i = 0; i < g_log1.slots; i++) {
        const km_ledstore_rec_t x = rec_of((uint8_t)(i % 4u), (uint8_t)i, 0, 0, 0);
        CHECK_EQ(km_ledstore_encode(&x, pl, sizeof pl), 16);
        CHECK_EQ(km_clog_append(&g_log1, pl), KM_CLOG_OK);
    }
    CHECK_EQ(g_erases, 0);
    km_ledprof_default(&t);
    CHECK(km_ledprof_load(&g_log2, &g_log1, &t));
    CHECK(km_ledprof_set(&t, 0, &p1));
    CHECK_EQ(km_ledprof_encode(&t, tb, sizeof tb), 64);
    CHECK_EQ(km_clog_append(&g_log2, tb), KM_CLOG_OK);
    CHECK_EQ(g_erases, 1);
    km_ledprof_default(&back);
    CHECK(km_ledprof_load(&g_log2, &g_log1, &back));
    CHECK_EQ(back.mask, 0x03);
    CHECK_EQ(g_violations, 0);

    /* many table saves: the sector holds 102 records, then one erase, and the newest table always loads */
    flash_blank();
    CHECK_EQ(g_log2.slots, 102);
    km_ledprof_default(&t);
    for (unsigned i = 0; i < 250u; i++) {
        const km_ledstore_rec_t x = rec_of((uint8_t)(i % 4u), (uint8_t)i, (uint8_t)(i >> 1), 7, (uint16_t)i);
        CHECK(km_ledprof_set(&t, i % 4u, &x));
        CHECK_EQ(km_ledprof_encode(&t, tb, sizeof tb), 64);
        CHECK_EQ(km_clog_append(&g_log2, tb), KM_CLOG_OK);
        km_ledprof_default(&back);
        CHECK(km_ledprof_load(&g_log2, &g_log1, &back));
        CHECK_EQ(memcmp(&back.mask, &t.mask, 1), 0);
        for (unsigned p = 0; p < 4; p++) {
            km_ledstore_rec_t a, c;
            CHECK(km_ledprof_effective(&t, p, &a) == km_ledprof_effective(&back, p, &c));
            if (km_ledprof_has_own(&t, p)) {
                CHECK(same_rec(&a, &c));
            }
        }
    }
    CHECK_EQ(g_erases, 2);
    CHECK_EQ(g_violations, 0);
}

/* ---- the glue in m5_led.c ---- */

static bool led_is(uint8_t effect, uint8_t r, uint8_t g, uint8_t b)
{
    km_led_cfg_t c;
    m5_led_get_cfg(&c);
    return c.effect == effect && c.color.r == r && c.color.g == g && c.color.b == b;
}

static void ex_vec(uint8_t v[KM_CONFIG_LED_SET_EX_BYTES], uint8_t effect, uint8_t r, uint8_t g, uint8_t b, uint16_t idle, uint8_t flags)
{
    v[0] = effect; v[1] = 200; v[2] = 90; v[3] = r; v[4] = g; v[5] = b;
    v[6] = (uint8_t)(idle & 0xFFu); v[7] = (uint8_t)(idle >> 8); v[8] = flags;
}

static unsigned status_of(const km_config_led_ex_ops_t *ex, unsigned profile)
{
    uint8_t v[KM_CONFIG_LED_GET_EX_BYTES];
    CHECK_EQ(ex->get_p(NULL, profile, v), KM_CFG_OK);
    return v[8];
}

static void test_glue(void)
{
    flash_blank();
    m5_led_init();
    m5_led_persist_set_legacy(&g_log1);
    m5_led_persist_attach(&g_log2, NULL, NULL);
    const km_config_led_ex_ops_t *ex = m5_led_cfg_ex_ops();
    CHECK(ex->get_p != NULL && ex->set_p != NULL);
    m5_led_set_profile(0);

    uint8_t v[KM_CONFIG_LED_SET_EX_BYTES], o[KM_CONFIG_LED_GET_EX_BYTES];
    const uint8_t SAVE = KM_CFG_LED_FLAG_SAVE, ACT = KM_CFG_LED_PROFILE_ACTIVE;

    /* profile 0 active: a plain save applies now and is the profile 0 entry */
    ex_vec(v, 2, 255, 0, 0, 600, SAVE);
    CHECK_EQ(ex->set(NULL, v), KM_CFG_OK);
    CHECK(led_is(2, 255, 0, 0));
    CHECK((status_of(ex, ACT) & KM_CFG_LED_ST_PROFILE_OWN) != 0u);
    CHECK((status_of(ex, ACT) & KM_CFG_LED_ST_SAVE_PENDING) != 0u);
    CHECK((status_of(ex, 1) & KM_CFG_LED_ST_PROFILE_OWN) == 0u);

    /* another profile: stored, NOT applied; needs the save flag */
    ex_vec(v, 3, 0, 0, 255, 0, SAVE);
    CHECK_EQ(ex->set_p(NULL, 1, v), KM_CFG_OK);
    CHECK(led_is(2, 255, 0, 0));
    ex_vec(v, 3, 0, 0, 255, 0, 0);
    CHECK_EQ(ex->set_p(NULL, 2, v), KM_CFG_ERR_RANGE);
    ex_vec(v, 3, 0, 0, 255, 0, SAVE);
    CHECK_EQ(ex->set_p(NULL, 4, v), KM_CFG_ERR_RANGE);
    v[0] = 99;
    CHECK_EQ(ex->set_p(NULL, 1, v), KM_CFG_ERR_RANGE);             /* invalid values change nothing */
    CHECK(ex->get_p(NULL, 1, o) == KM_CFG_OK && o[0] == 3 && o[3] == 0 && o[5] == 255 && (o[8] & KM_CFG_LED_ST_PROFILE_OWN) != 0u);
    CHECK(ex->get_p(NULL, 0, o) == KM_CFG_OK && o[0] == 2 && o[3] == 255);
    CHECK(ex->get_p(NULL, 2, o) == KM_CFG_OK && (o[8] & KM_CFG_LED_ST_PROFILE_OWN) == 0u);    /* no entry, no global: the defaults */
    CHECK_EQ(ex->get_p(NULL, 4, o), KM_CFG_ERR_RANGE);

    /* switching profile loads that profile's lights (RAM only) */
    m5_led_set_profile(1);
    CHECK(led_is(3, 0, 0, 255));
    m5_led_set_profile(2);                                          /* no entry, no global: the built-in defaults */
    {
        km_led_cfg_t d;
        km_led_cfg_default(&d);
        CHECK(led_is(d.effect, d.color.r, d.color.g, d.color.b));
    }
    m5_led_set_profile(0);
    CHECK(led_is(2, 255, 0, 0));

    /* an unsaved preview is gone at the next switch, the saved entry is back */
    ex_vec(v, 1, 1, 2, 3, 0, 0);
    CHECK_EQ(ex->set(NULL, v), KM_CFG_OK);
    CHECK(led_is(1, 1, 2, 3));
    m5_led_set_profile(1);
    m5_led_set_profile(0);
    CHECK(led_is(2, 255, 0, 0));

    /* the flash gets it in the background; a "reboot" restores all of it */
    for (uint32_t t = 100; t < 400u; t++) {
        (void)m5_led_persist_pass(t, true, true);
    }
    CHECK((status_of(ex, ACT) & KM_CFG_LED_ST_SAVE_PENDING) == 0u);
    CHECK_EQ(g_violations, 0);

    m5_led_init();
    m5_led_persist_set_legacy(&g_log1);
    m5_led_persist_attach(&g_log2, NULL, NULL);
    ex = m5_led_cfg_ex_ops();
    m5_led_set_profile(1);
    CHECK(led_is(3, 0, 0, 255));
    m5_led_set_profile(0);
    CHECK(led_is(2, 255, 0, 0));
    CHECK((status_of(ex, ACT) & KM_CFG_LED_ST_LOADED) != 0u);

    /* an old single-record sector: it becomes the lights of every profile without an entry */
    flash_blank();
    uint8_t pl[KM_LEDSTORE_PAYLOAD_BYTES];
    const km_ledstore_rec_t old = rec_of(2, 9, 99, 199, 120);
    CHECK_EQ(km_ledstore_encode(&old, pl, sizeof pl), 16);
    CHECK_EQ(km_clog_append(&g_log1, pl), KM_CLOG_OK);
    m5_led_init();
    m5_led_persist_set_legacy(&g_log1);
    m5_led_persist_attach(&g_log2, NULL, NULL);
    m5_led_set_profile(0);
    CHECK(led_is(2, 9, 99, 199));
    m5_led_set_profile(3);
    CHECK(led_is(2, 9, 99, 199));
    ex = m5_led_cfg_ex_ops();
    ex_vec(v, 1, 5, 6, 7, 0, SAVE);
    CHECK_EQ(ex->set(NULL, v), KM_CFG_OK);                          /* profile 3 now has its own, the others still follow the old record */
    m5_led_set_profile(2);
    CHECK(led_is(2, 9, 99, 199));
    m5_led_set_profile(3);
    CHECK(led_is(1, 5, 6, 7));

    /* a build without persistence: the save flag is refused, the profile calls still work for the active profile */
    m5_led_init();
    m5_led_persist_attach(NULL, NULL, NULL);
    ex = m5_led_cfg_ex_ops();
    ex_vec(v, 1, 5, 6, 7, 0, SAVE);
    CHECK_EQ(ex->set_p(NULL, ACT, v), KM_CFG_ERR_UNSUPPORTED);
    ex_vec(v, 1, 5, 6, 7, 0, 0);
    CHECK_EQ(ex->set_p(NULL, ACT, v), KM_CFG_OK);
    CHECK_EQ(ex->set_p(NULL, 1, v), KM_CFG_ERR_UNSUPPORTED);

    /* the older 16-byte log: one set of lights, profile switches change nothing, other profiles are not addressable */
    flash_blank();
    m5_led_init();
    m5_led_persist_attach(&g_log1, NULL, NULL);
    ex = m5_led_cfg_ex_ops();
    ex_vec(v, 3, 1, 1, 1, 0, SAVE);
    CHECK_EQ(ex->set(NULL, v), KM_CFG_OK);
    m5_led_set_profile(2);
    CHECK(led_is(3, 1, 1, 1));
    CHECK_EQ(ex->set_p(NULL, 2, v), KM_CFG_OK);
    CHECK_EQ(ex->set_p(NULL, 1, v), KM_CFG_ERR_UNSUPPORTED);

    m5_led_init();
    m5_led_persist_attach(NULL, NULL, NULL);                         /* leave the module as the other tests expect it */
}

void test_ledprof(void)
{
    test_codec();
    test_flash_and_migration();
    test_glue();
}
