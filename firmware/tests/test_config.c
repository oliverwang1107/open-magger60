/* Configuration protocol (M5 T-B, km_config.h / CONFIG_PROTOCOL.md): every request, every error path, paging with lost,
 * duplicated and out-of-order packets, the confirmation words, buffer bounds, the vendor-channel integration, and a
 * random-request run that proves nothing persistent happens without a valid confirmation. All against a fake backend. */
#include <stdlib.h>

#include "km/km_config.h"
#include "km/km_scope.h"
#include "km/km_settings.h"
#include "km/km_vendor.h"
#include "km_test.h"
#include "golden/config_vectors.h"

/* ---- fake backend ---- */
typedef struct {
    uint8_t blob[KM_CONFIG_MAX_SETTINGS];      /* the "live" serialized settings */
    unsigned blob_len;
    unsigned active;
    unsigned n_read, n_validate, n_commit, n_select, n_recal, n_savecal, n_depth;
    km_cfg_status_t validate_result, commit_result, select_result, recal_result, savecal_result, depth_result;
    uint8_t committed[KM_CONFIG_MAX_SETTINGS];
    unsigned committed_len;
    unsigned slots;                             /* number of live slots the fake has */
} fake_t;

static fake_t g_fake;

static unsigned f_read(void *ctx, uint8_t *buf, unsigned cap)
{
    fake_t *f = ctx;
    f->n_read++;
    if (f->blob_len > cap) {
        return 0;
    }
    memcpy(buf, f->blob, f->blob_len);
    return f->blob_len;
}

static km_cfg_status_t f_validate(void *ctx, const uint8_t *b, unsigned len)
{
    fake_t *f = ctx;
    (void)b;
    (void)len;
    f->n_validate++;
    return f->validate_result;
}

static km_cfg_status_t f_commit(void *ctx, const uint8_t *b, unsigned len)
{
    fake_t *f = ctx;
    f->n_commit++;
    if (f->commit_result == KM_CFG_OK) {
        memcpy(f->committed, b, len);
        f->committed_len = len;
        memcpy(f->blob, b, len);
        f->blob_len = len;
    }
    return f->commit_result;
}

static km_cfg_status_t f_select(void *ctx, unsigned i)
{
    fake_t *f = ctx;
    f->n_select++;
    if (f->select_result == KM_CFG_OK) {
        f->active = i;
    }
    return f->select_result;
}

static unsigned f_active(void *ctx) { return ((fake_t *)ctx)->active; }

static km_cfg_status_t f_recal(void *ctx)
{
    ((fake_t *)ctx)->n_recal++;
    return ((fake_t *)ctx)->recal_result;
}

static km_cfg_status_t f_savecal(void *ctx)
{
    ((fake_t *)ctx)->n_savecal++;
    return ((fake_t *)ctx)->savecal_result;
}

static km_cfg_status_t f_depth(void *ctx, unsigned first, unsigned count, uint16_t *out)
{
    fake_t *f = ctx;
    f->n_depth++;
    if (f->depth_result != KM_CFG_OK) {
        return f->depth_result;
    }
    if (first + count > f->slots) {
        return KM_CFG_ERR_RANGE;
    }
    for (unsigned i = 0; i < count; i++) {
        out[i] = (uint16_t)(1000u + (first + i) * 7u);
    }
    return KM_CFG_OK;
}

static const km_config_ops_t k_ops = {
    &g_fake, 4, f_read, f_validate, f_commit, f_select, f_active, f_recal, f_savecal, f_depth
};
static const km_config_ops_t k_ops_empty = { NULL, 4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* canaries around the handler state: nothing may write outside the struct */
static struct { uint8_t pre[64]; km_config_t cfg; uint8_t post[64]; } g_box;
#define cfg (g_box.cfg)

static const km_vendor_info_t k_info = { 0x0104, "Jan  1 2026,00:00:00KMFW-TEST" };

static void setup_with(const km_config_ops_t *ops)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.active = 2;
    g_fake.slots = 76;
    g_fake.blob_len = 300;
    for (unsigned i = 0; i < g_fake.blob_len; i++) {
        g_fake.blob[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(&g_box, 0xA5, sizeof g_box);
    km_config_init(&cfg, ops);
    memset(g_box.pre, 0xA5, sizeof g_box.pre);
    memset(g_box.post, 0xA5, sizeof g_box.post);
    km_vendor_set_config(&cfg);
}

static void setup(void) { setup_with(&k_ops); }

static void canaries_ok(void)
{
    for (unsigned i = 0; i < sizeof g_box.pre; i++) {
        CHECK(g_box.pre[i] == 0xA5);
        CHECK(g_box.post[i] == 0xA5);
    }
}

/* ---- request/reply helpers ---- */
typedef struct {
    bool answered;
    uint8_t raw[KM_HID_REPORT_LEN];
    unsigned op, token, status, pn;
    const uint8_t *pl;
} rep_t;

static unsigned u16of(const uint8_t *p) { return (unsigned)(p[0] | (p[1] << 8)); }
static uint32_t u32of(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void check_reply_frame(const rep_t *r)
{
    CHECK(r->raw[0] == 0xAA && r->raw[1] == KM_CMD_CONFIG && r->raw[2] == 0);
    CHECK(r->raw[4] >= 3 && r->raw[4] <= KM_HID_MAX_DATA);
    CHECK_EQ(r->raw[3], km_hid_checksum(r->raw[4], (uint32_t)r->raw[5] | ((uint32_t)r->raw[6] << 8) | ((uint32_t)r->raw[7] << 16),
                                        r->raw + 8));
    CHECK(r->raw[5] == 0 && r->raw[6] == 0 && r->raw[7] == 0);
    for (unsigned i = 8u + r->raw[4]; i < KM_HID_REPORT_LEN; i++) {
        CHECK(r->raw[i] == 0);
    }
}

static rep_t send_pkt(const uint8_t in[KM_HID_REPORT_LEN])
{
    rep_t r;
    memset(&r, 0, sizeof r);
    memset(r.raw, 0x77, sizeof r.raw);
    r.answered = km_vendor_handle(&k_info, in, r.raw);
    if (r.answered) {
        check_reply_frame(&r);
        r.op = r.raw[8];
        r.token = r.raw[9];
        r.status = r.raw[10];
        r.pn = r.raw[4] - 3u;
        r.pl = r.raw + 11;
    }
    return r;
}

static rep_t call(unsigned op, unsigned token, const uint8_t *args, unsigned nargs)
{
    uint8_t in[KM_HID_REPORT_LEN], data[KM_HID_MAX_DATA];
    CHECK(2u + nargs <= KM_HID_MAX_DATA);
    data[0] = (uint8_t)op;
    data[1] = (uint8_t)token;
    if (nargs != 0) {
        memcpy(data + 2, args, nargs);
    }
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, data, (uint8_t)(2u + nargs)) == KM_HID_OK);
    rep_t r = send_pkt(in);
    CHECK(r.answered);
    CHECK_EQ(r.op, op);
    CHECK_EQ(r.token, token);
    return r;
}

static unsigned st(unsigned op, const uint8_t *args, unsigned nargs)
{
    return call(op, 0x5A, args, nargs).status;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFFu); put16(p + 2, v >> 16); }

static void commit_args(uint8_t a[10], const char *word, unsigned len, uint32_t crc)
{
    memcpy(a, word, 4);
    put16(a + 4, len);
    put32(a + 6, crc);
}

/* begin + all pages of `data` (page size `page`); every reply must be OK. */
static void stage_all(const uint8_t *data, unsigned len, unsigned page)
{
    uint8_t a[2 + KM_CONFIG_SET_PAGE_BYTES];
    put16(a, len);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    for (unsigned off = 0; off < len; off += page) {
        const unsigned n = len - off < page ? len - off : page;
        put16(a, off);
        memcpy(a + 2, data + off, n);
        rep_t r = call(KM_CFG_OP_SET_PAGE, 0x20, a, 2 + n);
        CHECK_EQ(r.status, KM_CFG_OK);
        CHECK_EQ(r.pn, 2);
        CHECK_EQ(u16of(r.pl), off + n);
    }
}

static unsigned do_commit(const char *word, unsigned len, uint32_t crc)
{
    uint8_t a[10];
    commit_args(a, word, len, crc);
    return st(KM_CFG_OP_COMMIT, a, 10);
}

static void make_data(uint8_t *d, unsigned n, unsigned seed)
{
    for (unsigned i = 0; i < n; i++) {
        d[i] = (uint8_t)((i * 31u + seed * 17u + (i >> 5)) & 0xFFu);
    }
}

static unsigned stage_state(void)
{
    return call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl[9];
}

/* ---- protocol 2 (M6 P8): DEVICE_STATUS, SCENE_*, feature bits ---- */
static uint8_t g_dev_v[KM_CONFIG_DEVICE_STATUS_BYTES];
static km_cfg_status_t g_dev_result;
static unsigned g_dev_gets;

static km_cfg_status_t dev_get(void *ctx, uint8_t v[KM_CONFIG_DEVICE_STATUS_BYTES])
{
    (void)ctx;
    g_dev_gets++;
    if (g_dev_result == KM_CFG_OK) {
        memcpy(v, g_dev_v, KM_CONFIG_DEVICE_STATUS_BYTES);
    }
    return g_dev_result;
}

static const km_config_device_ops_t k_dev_ops = { NULL, dev_get };

typedef struct {
    uint8_t live[KM_CONFIG_MAX_SCENE];
    unsigned live_len;
    unsigned n_read, n_validate, n_commit;
    km_cfg_status_t validate_result, commit_result;
    uint8_t committed[KM_CONFIG_MAX_SCENE];
    unsigned committed_len;
} scene_fake_t;
static scene_fake_t g_sc;

static unsigned sc_read(void *ctx, uint8_t *buf, unsigned cap)
{
    (void)ctx;
    g_sc.n_read++;
    if (g_sc.live_len > cap) {
        return 0;
    }
    memcpy(buf, g_sc.live, g_sc.live_len);
    return g_sc.live_len;
}

static km_cfg_status_t sc_validate(void *ctx, const uint8_t *b, unsigned len)
{
    (void)ctx;
    (void)b;
    (void)len;
    g_sc.n_validate++;
    return g_sc.validate_result;
}

static km_cfg_status_t sc_commit(void *ctx, const uint8_t *b, unsigned len)
{
    (void)ctx;
    g_sc.n_commit++;
    if (g_sc.commit_result == KM_CFG_OK) {
        memcpy(g_sc.committed, b, len);
        g_sc.committed_len = len;
        memcpy(g_sc.live, b, len);
        g_sc.live_len = len;
    }
    return g_sc.commit_result;
}

static const km_config_scene_ops_t k_scene_ops = { NULL, sc_read, sc_validate, sc_commit };

static void setup_p2(void)
{
    setup();
    memset(&g_sc, 0, sizeof g_sc);
    memset(g_dev_v, 0, sizeof g_dev_v);
    g_dev_result = KM_CFG_OK;
    g_dev_gets = 0;
    km_config_set_device_ops(&cfg, &k_dev_ops);
    km_config_set_scene_ops(&cfg, &k_scene_ops);
}

/* ---- tests ---- */

static void test_crc(void)
{
    CHECK(km_config_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(km_config_crc32(NULL, 0) == 0u);
    const uint8_t z[4] = { 0, 0, 0, 0 };
    CHECK(km_config_crc32(z, 4) == 0x2144DF1Cu);
}

static void test_info(void)
{
    setup();
    rep_t r = call(KM_CFG_OP_GET_INFO, 0x11, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 20);                             /* protocol 2: two more bytes (scene size limit, 0 = no scene ops) */
    static const uint8_t want[20] = { 2, 4, 2, 0x00, 0x20, 44, 52, 0x3F, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(memcmp(r.pl, want, 20) == 0);
    /* extra byte -> BAD_LENGTH, no payload */
    uint8_t one = 0;
    r = call(KM_CFG_OP_GET_INFO, 0x11, &one, 1);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(r.pn, 0);

    /* a backend with nothing: features 0, active profile unknown */
    setup_with(&k_ops_empty);
    r = call(KM_CFG_OP_GET_INFO, 0x12, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pl[2], 0xFF);
    CHECK_EQ(u16of(r.pl + 7), 0);
    canaries_ok();
}

static void test_get_settings(void)
{
    setup();
    g_fake.blob_len = 1000;
    make_data(g_fake.blob, 1000, 3);
    const uint32_t crc = km_config_crc32(g_fake.blob, 1000);
    uint8_t got[1000];
    unsigned have = 0, id = 0, pages = 0;
    while (have < 1000u) {
        uint8_t a[4];
        put16(a, have);
        a[2] = KM_CONFIG_GET_PAGE_BYTES;
        a[3] = (uint8_t)id;
        rep_t r = call(KM_CFG_OP_GET_SETTINGS, 0x30, a, 4);
        CHECK_EQ(r.status, KM_CFG_OK);
        CHECK(r.pn >= 9);
        CHECK_EQ(u16of(r.pl + 1), 1000);
        CHECK(u32of(r.pl + 3) == crc);
        CHECK_EQ(u16of(r.pl + 7), have);
        const unsigned n = r.pn - 9u;
        CHECK(n >= 1 && n <= KM_CONFIG_GET_PAGE_BYTES);
        if (have == 0) {
            id = r.pl[0];
            CHECK(id != 0);
        } else {
            CHECK_EQ(r.pl[0], id);
        }
        CHECK(have + n <= 1000u);
        memcpy(got + have, r.pl + 9, n);
        have += n;
        pages++;
    }
    CHECK(memcmp(got, g_fake.blob, 1000) == 0);
    CHECK_EQ(pages, (1000 + 43) / 44);
    CHECK_EQ(g_fake.n_read, 1);             /* the backend is asked once per read, not once per page */

    /* offset == length: empty page, still OK (end marker); the blob changing in between does not disturb the read */
    uint8_t a[4];
    g_fake.blob[0] ^= 0xFF;
    put16(a, 1000);
    a[2] = 10;
    a[3] = (uint8_t)id;
    rep_t r = call(KM_CFG_OP_GET_SETTINGS, 1, a, 4);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 9);
    put16(a, 0);
    a[3] = (uint8_t)id;
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, a, 4).status, KM_CFG_ERR_BAD_ARG);   /* page 0 must carry id 0 */
    a[3] = 0;
    r = call(KM_CFG_OP_GET_SETTINGS, 1, a, 4);                                    /* fresh snapshot, new id, new content */
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK(r.pl[0] != id);
    CHECK(u32of(r.pl + 3) != crc);
    const unsigned id2 = r.pl[0];
    /* the older snapshot id is stale now */
    put16(a, 44);
    a[3] = (uint8_t)id;
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, a, 4).status, KM_CFG_ERR_STALE);
    a[3] = (uint8_t)id2;
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, a, 4).status, KM_CFG_OK);
    /* offset past the end */
    put16(a, 1001);
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, a, 4).status, KM_CFG_ERR_RANGE);
    put16(a, 0xFFFF);
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, a, 4).status, KM_CFG_ERR_RANGE);
    canaries_ok();
}

static void test_get_settings_errors(void)
{
    uint8_t a[4];
    setup();
    /* no snapshot yet: any page except page 0 is stale */
    put16(a, 10); a[2] = 10; a[3] = 1;
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_ERR_STALE);
    /* bad want */
    put16(a, 0); a[2] = 0; a[3] = 0;
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_ERR_BAD_ARG);
    a[2] = 45;
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_ERR_BAD_ARG);
    a[2] = 44;
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_OK);
    CHECK_EQ(g_fake.n_read, 1);              /* the two bad requests did not touch the backend */
    /* wrong lengths */
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 3), KM_CFG_ERR_BAD_LENGTH);
    uint8_t b[5] = { 0, 0, 44, 0, 0 };
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, b, 5), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, NULL, 0), KM_CFG_ERR_BAD_LENGTH);
    /* backend failure (returns 0) -> FAILED and the old snapshot is gone */
    g_fake.blob_len = 0;
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_ERR_FAILED);
    uint8_t c[4] = { 44, 0, 44, 1 };
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, c, 4), KM_CFG_ERR_STALE);
    /* not provided */
    setup_with(&k_ops_empty);
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, a, 4), KM_CFG_ERR_UNSUPPORTED);
    canaries_ok();
}

static void test_snapshot_id_wraps_and_skips_zero(void)
{
    setup();
    uint8_t a[4] = { 0, 0, 1, 0 };
    bool seen_zero = false;
    for (unsigned i = 0; i < 600; i++) {
        rep_t r = call(KM_CFG_OP_GET_SETTINGS, 1, a, 4);
        CHECK_EQ(r.status, KM_CFG_OK);
        if (r.pl[0] == 0) {
            seen_zero = true;
        }
        CHECK_EQ(r.pl[0], (i % 255u) + 1u);
    }
    CHECK(!seen_zero);
}

static void test_set_and_commit(void)
{
    uint8_t d[700];
    setup();
    make_data(d, 700, 5);
    {
        uint8_t q[4] = { 0, 0, 44, 0 };                                  /* a read in progress while a write is staged */
        CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, q, 4), KM_CFG_OK);
    }
    stage_all(d, 700, 52);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
    rep_t inf = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(u16of(inf.pl + 10), 700);
    CHECK_EQ(u16of(inf.pl + 12), 700);
    CHECK_EQ(g_fake.n_commit, 0);            /* staging alone never reaches the backend's commit */
    CHECK_EQ(g_fake.n_validate, 0);

    uint8_t a[10];
    commit_args(a, KM_CFG_CONFIRM_COMMIT, 700, km_config_crc32(d, 700));
    rep_t r = call(KM_CFG_OP_COMMIT, 0x44, a, 10);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 4);
    CHECK(u32of(r.pl) == 1u);
    CHECK_EQ(g_fake.n_commit, 1);
    CHECK_EQ(g_fake.n_validate, 1);
    CHECK_EQ(g_fake.committed_len, 700);
    CHECK(memcmp(g_fake.committed, d, 700) == 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);
    {
        uint8_t q[4] = { 44, 0, 44, (uint8_t)cfg.set.snap_id };
        CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, q, 4), KM_CFG_ERR_STALE);   /* the snapshot taken before the commit is gone */
    }
    /* the same commit again (duplicate packet): nothing staged -> refused, backend not called a second time */
    r = call(KM_CFG_OP_COMMIT, 0x44, a, 10);
    CHECK_EQ(r.status, KM_CFG_ERR_NOT_STAGED);
    CHECK_EQ(g_fake.n_commit, 1);
    CHECK(u32of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 14) == 1u);
    /* the live settings read back are the committed ones */
    uint8_t g[4] = { 0, 0, 44, 0 };
    r = call(KM_CFG_OP_GET_SETTINGS, 1, g, 4);
    CHECK_EQ(u16of(r.pl + 1), 700);
    CHECK(u32of(r.pl + 3) == km_config_crc32(d, 700));
    canaries_ok();
}

static void test_commit_confirmation(void)
{
    uint8_t d[200];
    setup();
    make_data(d, 200, 9);
    stage_all(d, 200, 52);
    const uint32_t crc = km_config_crc32(d, 200);
    static const char *const bad[] = { "\0\0\0\0", "SAVF", "save", "PROF", "RCAL", "SCAL", "SAV\0", "\0AVE", "EVAS" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ(do_commit(bad[i], 200, crc), KM_CFG_ERR_NEED_CONFIRM);
    }
    CHECK_EQ(g_fake.n_commit, 0);
    CHECK_EQ(g_fake.n_validate, 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);   /* a refused commit leaves the staging intact */
    /* confirmation is checked before anything else: wrong word + wrong crc + wrong len = still NEED_CONFIRM */
    CHECK_EQ(do_commit("SAVF", 1, 0), KM_CFG_ERR_NEED_CONFIRM);
    /* wrong word with nothing staged */
    setup();
    CHECK_EQ(do_commit("XXXX", 5, 0), KM_CFG_ERR_NEED_CONFIRM);
    /* too short / too long commit requests */
    uint8_t a[12];
    commit_args(a, KM_CFG_CONFIRM_COMMIT, 200, crc);
    CHECK_EQ(st(KM_CFG_OP_COMMIT, a, 9), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_COMMIT, a, 11), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_COMMIT, a, 0), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(g_fake.n_commit, 0);
    canaries_ok();
}

static void test_commit_checks(void)
{
    uint8_t d[300];
    setup();
    make_data(d, 300, 11);
    const uint32_t crc = km_config_crc32(d, 300);
    /* nothing staged */
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_NOT_STAGED);
    /* incomplete: 299 of 300 bytes */
    uint8_t a[2 + 52];
    put16(a, 300);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    for (unsigned off = 0; off < 299; off += 52) {
        const unsigned n = 299 - off < 52 ? 299 - off : 52;
        put16(a, off);
        memcpy(a + 2, d + off, n);
        CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 2 + n), KM_CFG_OK);
    }
    CHECK_EQ(stage_state(), KM_CFG_STAGE_RECEIVING);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_NOT_STAGED);
    CHECK_EQ(g_fake.n_commit, 0);
    /* last byte arrives */
    put16(a, 299);
    a[2] = d[299];
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 3), KM_CFG_OK);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
    /* wrong length, wrong crc (one bit), crc of other data */
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 299, crc), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 301, crc), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc ^ 1u), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc ^ 0x80000000u), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, 0), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(g_fake.n_commit, 0);
    CHECK_EQ(g_fake.n_validate, 0);
    /* validator says no -> INVALID, backend commit not called, staging stays */
    g_fake.validate_result = KM_CFG_ERR_FAILED;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_INVALID);
    g_fake.validate_result = KM_CFG_ERR_INVALID;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_INVALID);
    g_fake.validate_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_BUSY);
    CHECK_EQ(g_fake.n_commit, 0);
    g_fake.validate_result = KM_CFG_OK;
    /* the backend refuses (keys down): BUSY, staging kept, retry works */
    g_fake.commit_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_BUSY);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
    g_fake.commit_result = KM_CFG_ERR_FAILED;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_FAILED);
    g_fake.commit_result = (km_cfg_status_t)99;              /* a value outside the table is reported as FAILED */
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_ERR_FAILED);
    CHECK_EQ(g_fake.n_commit, 3);
    CHECK_EQ(g_fake.committed_len, 0);
    CHECK(u32of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 14) == 0u);   /* failed commits are not counted */
    g_fake.commit_result = KM_CFG_OK;
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 300, crc), KM_CFG_OK);
    CHECK(memcmp(g_fake.committed, d, 300) == 0);
    /* no commit callback: begin is already refused */
    setup_with(&k_ops_empty);
    put16(a, 10);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 10, 0), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(do_commit("XXXX", 10, 0), KM_CFG_ERR_NEED_CONFIRM);
    canaries_ok();
}

static void test_begin(void)
{
    uint8_t a[2];
    setup();
    put16(a, 0);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_ERR_BAD_ARG);
    put16(a, 8193);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_ERR_TOO_BIG);
    put16(a, 0xFFFF);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_ERR_TOO_BIG);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);       /* refused begins change nothing */
    put16(a, 8192);
    rep_t r = call(KM_CFG_OP_SET_BEGIN, 1, a, 2);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(u16of(r.pl), 8192);
    CHECK(KM_SETTINGS_ENCODED_SIZE <= KM_CONFIG_MAX_SETTINGS);          /* the real settings blob fits the protocol buffer */
    put16(a, 1);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 1), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 0), KM_CFG_ERR_BAD_LENGTH);
    uint8_t b[3] = { 1, 0, 0 };
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, b, 3), KM_CFG_ERR_BAD_LENGTH);
    /* a begin in the middle of a transfer discards everything staged so far */
    uint8_t d[120];
    make_data(d, 120, 1);
    stage_all(d, 120, 52);
    put16(a, 120);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_RECEIVING);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 120, km_config_crc32(d, 120)), KM_CFG_ERR_NOT_STAGED);
    canaries_ok();
}

static void test_paging(void)
{
    uint8_t d[400], a[2 + 52], bad[2 + 52];
    setup();
    make_data(d, 400, 2);
    const uint32_t crc = km_config_crc32(d, 400);
    /* page before begin */
    put16(a, 0);
    memcpy(a + 2, d, 10);
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 12), KM_CFG_ERR_NOT_STAGED);
    put16(bad, 400);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, bad, 2), KM_CFG_OK);

    /* lost packet: page 0 never arrives, page 1 does -> SEQUENCE with the next expected offset, no effect */
    put16(a, 52);
    memcpy(a + 2, d + 52, 52);
    rep_t r = call(KM_CFG_OP_SET_PAGE, 1, a, 54);
    CHECK_EQ(r.status, KM_CFG_ERR_SEQUENCE);
    CHECK_EQ(r.pn, 2);
    CHECK_EQ(u16of(r.pl), 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_RECEIVING);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 12), 0);

    /* page 0, then a duplicate of page 0 (its reply was lost) is acknowledged with the same next offset */
    put16(a, 0);
    memcpy(a + 2, d, 52);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 2, a, 54).status, KM_CFG_OK);
    r = call(KM_CFG_OP_SET_PAGE, 3, a, 54);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(u16of(r.pl), 52);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 12), 52);

    /* a repeat of page 0 with DIFFERENT bytes is not a repeat: SEQUENCE and the staged bytes are not touched */
    memcpy(bad, a, sizeof bad);
    bad[5] ^= 0x40;
    r = call(KM_CFG_OP_SET_PAGE, 4, bad, 54);
    CHECK_EQ(r.status, KM_CFG_ERR_SEQUENCE);
    CHECK_EQ(u16of(r.pl), 52);
    /* page 2 before page 1 (out of order) */
    put16(a, 104);
    memcpy(a + 2, d + 104, 52);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 5, a, 54).status, KM_CFG_ERR_SEQUENCE);
    /* page 1, then a late duplicate of page 0 arrives: refused, no rewind */
    put16(a, 52);
    memcpy(a + 2, d + 52, 52);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 6, a, 54).status, KM_CFG_OK);
    put16(a, 0);
    memcpy(a + 2, d, 52);
    r = call(KM_CFG_OP_SET_PAGE, 7, a, 54);
    CHECK_EQ(r.status, KM_CFG_ERR_SEQUENCE);
    CHECK_EQ(u16of(r.pl), 104);
    /* a page that overlaps the next expected offset, and one that skips one byte */
    put16(a, 103);
    memcpy(a + 2, d + 103, 20);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 8, a, 22).status, KM_CFG_ERR_SEQUENCE);
    put16(a, 105);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 9, a, 22).status, KM_CFG_ERR_SEQUENCE);
    put16(a, 0xFFFF);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 9, a, 22).status, KM_CFG_ERR_SEQUENCE);
    /* a page with no data bytes, and the reserved offset header alone */
    put16(a, 104);
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 2), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 1), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 0), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 12), 104);

    /* finish with odd page sizes; the last page is exactly the remaining bytes */
    unsigned off = 104;
    const unsigned sizes[] = { 1, 7, 52, 33, 52, 52, 52, 52, 52, 52 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0] && off < 400; i++) {
        unsigned n = sizes[i];
        if (off + n > 400) {
            n = 400 - off;
        }
        put16(a, off);
        memcpy(a + 2, d + off, n);
        CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 10, a, 2 + n).status, KM_CFG_OK);
        off += n;
    }
    CHECK_EQ(off, 400);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
    /* a page beyond the end (even one byte) is refused with RANGE after the stage is full; duplicates of the last page are fine */
    put16(a, 400);
    a[2] = 0xEE;
    r = call(KM_CFG_OP_SET_PAGE, 11, a, 3);
    CHECK_EQ(r.status, KM_CFG_ERR_RANGE);
    CHECK_EQ(u16of(r.pl), 400);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, 400, crc), KM_CFG_OK);
    CHECK(memcmp(g_fake.committed, d, 400) == 0);
    canaries_ok();
}

static void test_page_range_and_bounds(void)
{
    uint8_t a[2 + 52];
    setup();
    /* declared length 100: a page that would run past 100 is RANGE and changes nothing */
    put16(a, 100);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    memset(a + 2, 0x11, 52);
    put16(a, 0);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 1, a, 54).status, KM_CFG_OK);       /* 0..51 */
    put16(a, 52);
    memset(a + 2, 0x22, 52);
    rep_t r = call(KM_CFG_OP_SET_PAGE, 2, a, 54);                         /* 52..103 > 100 */
    CHECK_EQ(r.status, KM_CFG_ERR_RANGE);
    CHECK_EQ(u16of(r.pl), 52);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_RECEIVING);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 12), 52);
    memset(a + 2, 0x22, 48);
    CHECK_EQ(call(KM_CFG_OP_SET_PAGE, 3, a, 50).status, KM_CFG_OK);       /* 52..99 */
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);

    /* the maximum, 8192 bytes in the largest pages, ending exactly at the end of the buffer */
    uint8_t *d = malloc(KM_CONFIG_MAX_SETTINGS);
    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    uint8_t g[4] = { 0, 0, 44, 0 };
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, g, 4).status, KM_CFG_OK);    /* a snapshot exists while we stage */
    const uint32_t snap_crc_before = cfg.set.snap_crc;
    make_data(d, KM_CONFIG_MAX_SETTINGS, 7);
    stage_all(d, KM_CONFIG_MAX_SETTINGS, 52);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
    CHECK(memcmp(cfg.stage, d, KM_CONFIG_MAX_SETTINGS) == 0);
    CHECK(cfg.set.snap_crc == snap_crc_before);                                /* staging does not disturb a read in progress */
    uint8_t g2[4] = { 44, 0, 44, 1 };
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, g2, 4).status, KM_CFG_OK);
    put16(a, KM_CONFIG_MAX_SETTINGS);
    a[2] = 1;
    CHECK_EQ(st(KM_CFG_OP_SET_PAGE, a, 3), KM_CFG_ERR_RANGE);
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, KM_CONFIG_MAX_SETTINGS, km_config_crc32(d, KM_CONFIG_MAX_SETTINGS)), KM_CFG_OK);
    CHECK_EQ(g_fake.committed_len, KM_CONFIG_MAX_SETTINGS);
    CHECK(memcmp(g_fake.committed, d, KM_CONFIG_MAX_SETTINGS) == 0);
    /* the backend returning a blob bigger than the buffer is a failure, not an overrun */
    g_fake.blob_len = KM_CONFIG_MAX_SETTINGS;                              /* exactly the maximum is fine */
    uint8_t z[4] = { 0, 0, 44, 0 };
    CHECK_EQ(call(KM_CFG_OP_GET_SETTINGS, 1, z, 4).status, KM_CFG_OK);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_SETTINGS, 1, z, 4).pl + 1), KM_CONFIG_MAX_SETTINGS);
    free(d);
    canaries_ok();
}

static void test_select_profile(void)
{
    uint8_t a[5];
    setup();
    memcpy(a, KM_CFG_CONFIRM_PROFILE, 4);
    a[4] = 3;
    rep_t r = call(KM_CFG_OP_SELECT_PROFILE, 1, a, 5);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 1);
    CHECK_EQ(r.pl[0], 3);
    CHECK_EQ(g_fake.active, 3);
    CHECK_EQ(g_fake.n_select, 1);
    /* range: 4 and 255 are out; the backend is not called */
    a[4] = 4;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_RANGE);
    a[4] = 255;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_RANGE);
    CHECK_EQ(g_fake.n_select, 1);
    /* missing / wrong confirmation: nothing changes, even for an index that would be fine */
    static const char *const bad[] = { "\0\0\0\0", "SAVE", "RCAL", "SCAL", "prof", "PROG" };
    a[4] = 0;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(a, bad[i], 4);
        CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_NEED_CONFIRM);
    }
    CHECK_EQ(g_fake.n_select, 1);
    CHECK_EQ(g_fake.active, 3);
    /* confirm comes before range */
    memcpy(a, "PROG", 4);
    a[4] = 200;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_NEED_CONFIRM);
    /* lengths */
    memcpy(a, KM_CFG_CONFIRM_PROFILE, 4);
    a[4] = 1;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 4), KM_CFG_ERR_BAD_LENGTH);
    uint8_t b[6] = { 'P', 'R', 'O', 'F', 1, 0 };
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, b, 6), KM_CFG_ERR_BAD_LENGTH);
    /* backend busy / failure is passed on and the snapshot is kept */
    g_fake.select_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_BUSY);
    g_fake.select_result = KM_CFG_ERR_FAILED;
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_FAILED);
    CHECK_EQ(g_fake.active, 3);
    /* a successful switch invalidates a read in progress */
    g_fake.select_result = KM_CFG_OK;
    uint8_t g[4] = { 0, 0, 44, 0 };
    rep_t s = call(KM_CFG_OP_GET_SETTINGS, 1, g, 4);
    uint8_t g2[4] = { 44, 0, 44, s.pl[0] };
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_OK);
    CHECK_EQ(st(KM_CFG_OP_GET_SETTINGS, g2, 4), KM_CFG_ERR_STALE);
    /* unsupported backend */
    setup_with(&k_ops_empty);
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_UNSUPPORTED);
    memcpy(a, "NOPE", 4);
    CHECK_EQ(st(KM_CFG_OP_SELECT_PROFILE, a, 5), KM_CFG_ERR_NEED_CONFIRM);
    canaries_ok();
}

static void test_calibration_requests(void)
{
    uint8_t a[4];
    setup();
    memcpy(a, KM_CFG_CONFIRM_RECALIBRATE, 4);
    CHECK_EQ(call(KM_CFG_OP_RECALIBRATE_REQUEST, 1, a, 4).status, KM_CFG_OK);
    CHECK_EQ(call(KM_CFG_OP_RECALIBRATE_REQUEST, 1, a, 4).pn, 0);
    CHECK_EQ(g_fake.n_recal, 2);
    CHECK_EQ(st(KM_CFG_OP_SAVE_CALIBRATION_REQUEST, a, 4), KM_CFG_ERR_NEED_CONFIRM);   /* RCAL is not SCAL */
    memcpy(a, KM_CFG_CONFIRM_SAVE_CAL, 4);
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, a, 4), KM_CFG_ERR_NEED_CONFIRM);        /* SCAL is not RCAL */
    CHECK_EQ(g_fake.n_recal, 2);
    CHECK_EQ(call(KM_CFG_OP_SAVE_CALIBRATION_REQUEST, 1, a, 4).status, KM_CFG_OK);
    CHECK_EQ(g_fake.n_savecal, 1);
    static const char *const bad[] = { "\0\0\0\0", "SAVE", "PROF", "rcal", "scal" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(a, bad[i], 4);
        CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, a, 4), KM_CFG_ERR_NEED_CONFIRM);
        CHECK_EQ(st(KM_CFG_OP_SAVE_CALIBRATION_REQUEST, a, 4), KM_CFG_ERR_NEED_CONFIRM);
    }
    CHECK_EQ(g_fake.n_recal, 2);
    CHECK_EQ(g_fake.n_savecal, 1);
    /* lengths */
    memcpy(a, KM_CFG_CONFIRM_RECALIBRATE, 4);
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, a, 3), KM_CFG_ERR_BAD_LENGTH);
    uint8_t b[5] = { 'R', 'C', 'A', 'L', 0 };
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, b, 5), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, NULL, 0), KM_CFG_ERR_BAD_LENGTH);
    /* busy and failures are passed on */
    g_fake.recal_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, a, 4), KM_CFG_ERR_BUSY);
    g_fake.savecal_result = KM_CFG_ERR_FAILED;
    memcpy(a, KM_CFG_CONFIRM_SAVE_CAL, 4);
    CHECK_EQ(st(KM_CFG_OP_SAVE_CALIBRATION_REQUEST, a, 4), KM_CFG_ERR_FAILED);
    setup_with(&k_ops_empty);
    CHECK_EQ(st(KM_CFG_OP_SAVE_CALIBRATION_REQUEST, a, 4), KM_CFG_ERR_UNSUPPORTED);
    memcpy(a, KM_CFG_CONFIRM_RECALIBRATE, 4);
    CHECK_EQ(st(KM_CFG_OP_RECALIBRATE_REQUEST, a, 4), KM_CFG_ERR_UNSUPPORTED);
    canaries_ok();
}

static void test_live_depth(void)
{
    uint8_t a[2];
    setup();
    a[0] = 3;
    a[1] = 25;
    rep_t r = call(KM_CFG_OP_LIVE_DEPTH, 1, a, 2);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 2 + 50);
    CHECK_EQ(r.pl[0], 3);
    CHECK_EQ(r.pl[1], 25);
    for (unsigned i = 0; i < 25; i++) {
        CHECK_EQ(u16of(r.pl + 2 + 2 * i), 1000 + (3 + i) * 7);
    }
    a[0] = 75;
    a[1] = 1;
    CHECK_EQ(call(KM_CFG_OP_LIVE_DEPTH, 1, a, 2).status, KM_CFG_OK);
    g_fake.slots = 128;                          /* the hard bound is slot 127: 103 + 25 = 128 is the last allowed request */
    a[0] = 103;
    a[1] = 25;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_OK);
    a[0] = 104;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_RANGE);
    g_fake.slots = 76;
    a[0] = 75;
    a[1] = 1;
    a[0] = 76;                                   /* the fake has 76 slots: backend RANGE is passed on */
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_RANGE);
    a[0] = 0;
    a[1] = 0;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_BAD_ARG);
    a[1] = 26;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_BAD_ARG);
    a[0] = 120;
    a[1] = 9;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_RANGE);    /* 129 > 128 hard bound, backend not asked */
    a[0] = 255;
    a[1] = 25;
    const unsigned asked = g_fake.n_depth;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 2), KM_CFG_ERR_RANGE);
    CHECK_EQ(g_fake.n_depth, asked);
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, a, 1), KM_CFG_ERR_BAD_LENGTH);
    uint8_t b[3] = { 0, 1, 0 };
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, b, 3), KM_CFG_ERR_BAD_LENGTH);
    g_fake.depth_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, b, 2), KM_CFG_ERR_BUSY);
    setup_with(&k_ops_empty);
    CHECK_EQ(st(KM_CFG_OP_LIVE_DEPTH, b, 2), KM_CFG_ERR_UNSUPPORTED);
    canaries_ok();
}

static void test_malformed_requests(void)
{
    setup();
    /* unknown sub-commands: error reply that echoes op and token; nothing else happens */
    static const uint8_t unk[] = { 0x00, 0x0B, 0x0C, 0x10, 0x7F, 0x80, 0xC5, 0xC6, 0xFF };
    for (unsigned i = 0; i < sizeof unk; i++) {
        rep_t r = call(unk[i], 0xC3, NULL, 0);
        CHECK_EQ(r.status, KM_CFG_ERR_UNKNOWN_OP);
        CHECK_EQ(r.pn, 0);
    }
    /* request without op / token */
    uint8_t in[KM_HID_REPORT_LEN];
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, NULL, 0) == KM_HID_OK);
    rep_t r = send_pkt(in);
    CHECK(r.answered);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(r.op, 0);
    CHECK_EQ(r.token, 0);
    const uint8_t only_op = KM_CFG_OP_COMMIT;
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, &only_op, 1) == KM_HID_OK);
    r = send_pkt(in);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(r.op, KM_CFG_OP_COMMIT);
    CHECK_EQ(r.token, 0);
    /* the offset field must be 0 */
    const uint8_t gi[2] = { KM_CFG_OP_GET_INFO, 9 };
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 1, gi, 2) == KM_HID_OK);
    r = send_pkt(in);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_ARG);
    CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_CONFIG, 0, 0x10000, true, gi, 2) == KM_HID_OK);
    r = send_pkt(in);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_ARG);
    /* a XOR-masked request is refused (the protocol does not use the mask) */
    CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_CONFIG, 0x5A, 0, false, gi, 2) == KM_HID_OK);
    r = send_pkt(in);
    CHECK(r.answered);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_ARG);
    /* longest possible request (56 data bytes) on every op with a fixed length: BAD_LENGTH */
    uint8_t big[54];
    memset(big, 0, sizeof big);
    memcpy(big, KM_CFG_CONFIRM_COMMIT, 4);
    for (unsigned op = 1; op <= 9; op++) {
        if (op == KM_CFG_OP_SET_PAGE) {
            continue;
        }
        CHECK_EQ(st(op, big, 54), KM_CFG_ERR_BAD_LENGTH);
    }
    CHECK_EQ(g_fake.n_commit + g_fake.n_select + g_fake.n_recal + g_fake.n_savecal + g_fake.n_depth, 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);
    canaries_ok();
}

static void test_vendor_integration(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN], out0[KM_HID_REPORT_LEN];
    setup();
    /* not registered: 0xC6 is answered "unknw" like any unknown command */
    km_vendor_set_config(NULL);
    const uint8_t gi[2] = { KM_CFG_OP_GET_INFO, 1 };
    CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, gi, 2) == KM_HID_OK);
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK(out[0] == 0xAA && out[1] == KM_CMD_CONFIG && out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);
    /* cmd 3 and the scope command answer identically before and after registration */
    uint8_t c3[KM_HID_REPORT_LEN], sc[KM_HID_REPORT_LEN], r3a[KM_HID_REPORT_LEN], r3b[KM_HID_REPORT_LEN],
        rsa[KM_HID_REPORT_LEN], rsb[KM_HID_REPORT_LEN];
    uint16_t vals[3] = { 11, 22, 33 };
    km_scope_reset();
    km_scope_publish(2, vals, 3);
    CHECK(km_hid_build_read(c3, KM_CMD_GET_INFO, 0, 0) == KM_HID_OK);
    CHECK(km_hid_build_read(sc, KM_CMD_SCOPE, 0, 56) == KM_HID_OK);
    CHECK(km_vendor_handle(&k_info, c3, r3a));
    CHECK(km_vendor_handle(&k_info, sc, rsa));
    km_vendor_set_config(&cfg);
    CHECK(km_vendor_handle(&k_info, c3, r3b));
    CHECK(km_vendor_handle(&k_info, sc, rsb));
    CHECK(memcmp(r3a, r3b, sizeof r3a) == 0);
    CHECK(memcmp(rsa, rsb, sizeof rsa) == 0);
    km_scope_reset();
    /* registered: the config reply */
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK(out[1] == KM_CMD_CONFIG && out[8] == KM_CFG_OP_GET_INFO && out[10] == 0);
    /* a bad checksum is answered 0xAB / "cscr" and never reaches the handler */
    memcpy(out0, in, sizeof out0);
    out0[3] ^= 1;
    const unsigned reads = g_fake.n_read;
    CHECK(km_vendor_handle(&k_info, out0, out));
    CHECK(out[0] == 0xAB && memcmp(out + 8, "cscr", 4) == 0);
    CHECK_EQ(g_fake.n_read, reads);
    /* raw '_' packets are ignored, even with cmd 0xC6 */
    const uint8_t raw[1] = { 0x51 };
    CHECK(km_hid_build(in, KM_HID_HDR_RAW, KM_CMD_CONFIG, 0, 0, false, raw, 1) == KM_HID_OK);
    CHECK(!km_vendor_handle(&k_info, in, out));
    /* the stock-protocol commands that write are still "unknw", registered or not */
    static const uint8_t writers[] = { 0x06, 0x09, 0x0B, 0x0D, 0xA1, 0xA3, 0xA5, 0xA7, 0xB1, 0xE1, 0xEE, 0xF2 };
    for (unsigned i = 0; i < sizeof writers; i++) {
        CHECK(km_hid_build_write(in, writers[i], 0, NULL, 0) == KM_HID_OK);
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK(memcmp(out + 8, "unknw", 5) == 0);
    }
    CHECK(!km_cmd_is_read_only(KM_CMD_CONFIG));       /* the stock-protocol backup tool must never send it */
    km_vendor_set_config(NULL);
}

/* golden packets: requests and replies computed by hand / by the independent Python model (golden/config_vectors.h) */
static void test_golden(void)
{
    for (unsigned i = 0; i < CONFIG_VECTOR_COUNT; i++) {
        const config_vector_t *v = &config_vectors[i];
        uint8_t in[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN];
        setup_p2();
        km_config_set_extra_features(&cfg, KM_CFG_FEAT_STREAM | KM_CFG_FEAT_HOST_LEDS);
        g_fake.blob_len = 100;
        for (unsigned j = 0; j < 100; j++) {
            g_fake.blob[j] = (uint8_t)(j * 3u + 1u);
        }
        /* protocol 2 fakes: a 50-byte live scene (j*5+2) and a fixed device status */
        g_sc.live_len = 50;
        for (unsigned j = 0; j < 50; j++) {
            g_sc.live[j] = (uint8_t)(j * 5u + 2u);
        }
        static const uint8_t devst[KM_CONFIG_DEVICE_STATUS_BYTES] = { 2, 1, 0, 0x09, 2, 1, 2, 0, 0x34, 0x12, 3, 0, 7, 0, 0, 0 };
        memcpy(g_dev_v, devst, sizeof g_dev_v);
        /* prerequisite packets (replies not compared): lines whose name starts with '+' */
        for (unsigned p = 0; p < i; p++) {
            if (config_vectors[p].group != v->group) {
                continue;
            }
            CHECK_EQ(km_hex(config_vectors[p].req_hex, in, sizeof in), 64);
            rep_t pre = send_pkt(in);
            CHECK(pre.answered);
        }
        CHECK_EQ(km_hex(v->req_hex, in, sizeof in), 64);
        CHECK_EQ(km_hex(v->reply_hex, want, sizeof want), 64);
        rep_t r = send_pkt(in);
        CHECK(r.answered);
        if (memcmp(r.raw, want, 64) != 0) {
            printf("  golden mismatch: %s\n", v->name);
            km_test_failures++;
        }
        km_test_checks++;
    }
    km_vendor_set_config(NULL);
}

/* 20000 random requests with random confirmation words: nothing persistent may happen unless the word is exactly right */
static void test_random_never_commits_unconfirmed(void)
{
    setup();
    uint32_t s = 0x12345678u;
    unsigned answered = 0, ok_status = 0;
    for (unsigned i = 0; i < 20000; i++) {
        uint8_t data[KM_HID_MAX_DATA];
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        s = s * 1664525u + 1013904223u;
        const unsigned len = (s >> 24) % 57u;
        for (unsigned j = 0; j < len; j++) {
            s = s * 1664525u + 1013904223u;
            data[j] = (uint8_t)(s >> 24);
        }
        if (len >= 1) {
            data[0] = (uint8_t)(1u + (data[0] % 10u));       /* mostly valid op numbers */
        }
        if (len >= 6 && (i & 1u)) {
            memcpy(data + 2, (i & 2u) ? "SAVF" : "PROG", 4);   /* near-miss words */
        }
        CHECK(km_hid_build_write(in, KM_CMD_CONFIG, 0, data, (uint8_t)len) == KM_HID_OK);
        if (km_vendor_handle(&k_info, in, out)) {
            answered++;
            if (out[10] == KM_CFG_OK) {
                ok_status++;
            }
        }
    }
    CHECK_EQ(answered, 20000);
    CHECK(ok_status > 0);
    CHECK_EQ(g_fake.n_commit, 0);
    CHECK_EQ(g_fake.n_select, 0);
    CHECK_EQ(g_fake.n_recal, 0);
    CHECK_EQ(g_fake.n_savecal, 0);
    CHECK_EQ(g_fake.active, 2);
    canaries_ok();
    km_vendor_set_config(NULL);
}

/* ---- the optional light settings (ops 0x20 / 0x21) ---- */
static uint8_t g_led_v[KM_CONFIG_LED_BYTES];
static unsigned g_led_sets;

static km_cfg_status_t led_get(void *ctx, uint8_t v[KM_CONFIG_LED_BYTES])
{
    (void)ctx;
    memcpy(v, g_led_v, KM_CONFIG_LED_BYTES);
    return KM_CFG_OK;
}

static km_cfg_status_t led_set(void *ctx, const uint8_t v[KM_CONFIG_LED_BYTES])
{
    (void)ctx;
    if (v[0] >= 7u || v[2] == 0u) {
        return KM_CFG_ERR_RANGE;                   /* the backend's own validation */
    }
    memcpy(g_led_v, v, KM_CONFIG_LED_BYTES);
    g_led_sets++;
    return KM_CFG_OK;
}

static const km_config_led_ops_t k_led_ops = { NULL, led_get, led_set };

static void test_led_ops(void)
{
    uint8_t one = 0;
    static const uint8_t six[6] = { 3, 200, 90, 1, 2, 3 };

    /* without registered light ops: UNSUPPORTED, no feature bit */
    setup();
    CHECK_EQ(st(KM_CFG_OP_LED_GET, NULL, 0), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, six, 6), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 7) & KM_CFG_FEAT_LED, 0);

    /* with them */
    setup();
    km_config_set_led_ops(&cfg, &k_led_ops);
    memset(g_led_v, 0, sizeof g_led_v);
    g_led_v[0] = 1; g_led_v[1] = 255; g_led_v[2] = 128;
    g_led_sets = 0;
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 7) & KM_CFG_FEAT_LED, KM_CFG_FEAT_LED);
    rep_t r = call(KM_CFG_OP_LED_GET, 0x31, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 6);
    CHECK(memcmp(r.pl, g_led_v, 6) == 0);

    r = call(KM_CFG_OP_LED_SET, 0x32, six, 6);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 6);
    CHECK(memcmp(r.pl, six, 6) == 0);                 /* the values now in force come back */
    CHECK(memcmp(g_led_v, six, 6) == 0);
    CHECK_EQ(g_led_sets, 1);

    /* an invalid value is refused by the backend and changes nothing */
    static const uint8_t bad_fx[6] = { 9, 1, 1, 1, 1, 1 }, bad_speed[6] = { 1, 1, 0, 1, 1, 1 };
    CHECK_EQ(st(KM_CFG_OP_LED_SET, bad_fx, 6), KM_CFG_ERR_RANGE);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, bad_speed, 6), KM_CFG_ERR_RANGE);
    CHECK(memcmp(g_led_v, six, 6) == 0);
    CHECK_EQ(g_led_sets, 1);

    /* wrong lengths: BAD_LENGTH, nothing happens */
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &one, 1), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, six, 5), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, NULL, 0), KM_CFG_ERR_BAD_LENGTH);
    uint8_t seven[7] = { 3, 200, 90, 1, 2, 3, 4 };
    CHECK_EQ(st(KM_CFG_OP_LED_SET, seven, 7), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(g_led_sets, 1);

    /* it never touches the settings: no staging, no commit, no flash */
    CHECK_EQ(g_fake.n_commit, 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);

    /* a byte-exact example (CONFIG_PROTOCOL.md): LED_SET effect 3, brightness 200, speed 90, colour 1,2,3 */
    uint8_t req[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN];
    memset(req, 0, sizeof req);
    km_hex("55c600d208000000" "217e" "03c85a010203", req, 16);
    rep_t g = send_pkt(req);
    CHECK(g.answered);
    CHECK_EQ(g.status, KM_CFG_OK);
    memset(want, 0, sizeof want);
    km_hex("aac600d309000000" "217e00" "03c85a010203", want, 17);
    CHECK(memcmp(g.raw, want, KM_HID_REPORT_LEN) == 0);

    /* the light ops can be removed again */
    km_config_set_led_ops(&cfg, NULL);
    CHECK_EQ(st(KM_CFG_OP_LED_GET, NULL, 0), KM_CFG_ERR_UNSUPPORTED);
    canaries_ok();
}

/* ---- the extended light form: idle-off time, save flag, status (LED_GET with arg 1, LED_SET with 9 argument bytes) ---- */

static uint8_t g_ex_v[KM_CONFIG_LED_SET_EX_BYTES];     /* what the fake holds: six light bytes, idle u16, flags are not kept */
static unsigned g_ex_sets, g_ex_gets, g_ex_last_flags;
static km_cfg_status_t g_ex_result;

static km_cfg_status_t ex_get(void *ctx, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES])
{
    (void)ctx;
    g_ex_gets++;
    memcpy(v, g_ex_v, 8);
    v[8] = 0x11;                                         /* status bits the fake reports */
    v[9] = 0;
    v[10] = (uint8_t)g_ex_sets;
    v[11] = 0;
    return KM_CFG_OK;
}

static km_cfg_status_t ex_set(void *ctx, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES])
{
    (void)ctx;
    if (g_ex_result != KM_CFG_OK) {
        return g_ex_result;
    }
    memcpy(g_ex_v, v, 8);
    g_ex_last_flags = v[8];
    g_ex_sets++;
    return KM_CFG_OK;
}

static const km_config_led_ex_ops_t k_led_ex_ops = { NULL, ex_get, ex_set, NULL, NULL };

static void test_led_ex_ops(void)
{
    static const uint8_t six[6] = { 3, 200, 90, 1, 2, 3 };
    static const uint8_t nine[9] = { 3, 200, 90, 1, 2, 3, 0x58, 0x02, KM_CFG_LED_FLAG_SAVE };   /* idle 600 s, save */
    uint8_t one = 1;

    /* no light ops at all: both extended forms are UNSUPPORTED like the plain ones */
    setup();
    km_config_set_led_ex_ops(&cfg, &k_led_ex_ops);                    /* extended ops alone are not enough */
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &one, 1), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine, 9), KM_CFG_ERR_UNSUPPORTED);

    /* plain ops only (an older build): the extended forms answer BAD_LENGTH, exactly as before they existed; the plain forms work */
    setup();
    km_config_set_led_ops(&cfg, &k_led_ops);
    memset(g_led_v, 0, sizeof g_led_v);
    g_led_v[0] = 1; g_led_v[1] = 255; g_led_v[2] = 128;
    g_led_sets = 0;
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &one, 1), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine, 9), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(g_led_sets, 0);
    CHECK_EQ(call(KM_CFG_OP_LED_GET, 1, NULL, 0).pn, 6);

    /* both registered */
    setup();
    km_config_set_led_ops(&cfg, &k_led_ops);
    km_config_set_led_ex_ops(&cfg, &k_led_ex_ops);
    memset(g_led_v, 0, sizeof g_led_v);
    g_led_v[0] = 1; g_led_v[1] = 255; g_led_v[2] = 128;
    memset(g_ex_v, 0, sizeof g_ex_v);
    g_ex_v[0] = 5; g_ex_v[1] = 77; g_ex_v[2] = 66; g_ex_v[3] = 9; g_ex_v[4] = 8; g_ex_v[5] = 7; g_ex_v[6] = 0x2C; g_ex_v[7] = 0x01;
    g_ex_sets = g_ex_gets = g_led_sets = g_ex_last_flags = 0;
    g_ex_result = KM_CFG_OK;

    /* the plain GET is still the six bytes of the plain backend */
    rep_t r = call(KM_CFG_OP_LED_GET, 0x11, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 6);
    CHECK(memcmp(r.pl, g_led_v, 6) == 0);
    CHECK_EQ(g_ex_gets, 0);

    /* GET with the argument 1: twelve bytes from the extended backend */
    r = call(KM_CFG_OP_LED_GET, 0x12, &one, 1);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 12);
    CHECK(memcmp(r.pl, g_ex_v, 8) == 0);
    CHECK_EQ(r.pl[8], 0x11);
    CHECK_EQ(g_ex_gets, 1);
    uint8_t two = 2, zero = 0, two_args[2] = { 1, 1 };
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &two, 1), KM_CFG_ERR_BAD_ARG);
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &zero, 1), KM_CFG_ERR_BAD_ARG);
    CHECK_EQ(st(KM_CFG_OP_LED_GET, two_args, 2), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(g_ex_gets, 1);

    /* SET with six bytes: the plain path, the extended backend is not called, the reply is six bytes */
    r = call(KM_CFG_OP_LED_SET, 0x13, six, 6);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 6);
    CHECK_EQ(g_led_sets, 1);
    CHECK_EQ(g_ex_sets, 0);

    /* SET with nine bytes: the extended backend gets exactly the nine, the reply is the twelve bytes of the extended GET */
    r = call(KM_CFG_OP_LED_SET, 0x14, nine, 9);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 12);
    CHECK_EQ(g_ex_sets, 1);
    CHECK_EQ(g_led_sets, 1);
    CHECK(memcmp(g_ex_v, nine, 8) == 0);
    CHECK_EQ(g_ex_last_flags, KM_CFG_LED_FLAG_SAVE);
    CHECK(memcmp(r.pl, nine, 8) == 0);
    CHECK_EQ(r.pl[10], 1);
    uint8_t nine0[9];
    memcpy(nine0, nine, 9);
    nine0[8] = 0;                                                     /* no flag: live change only */
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine0, 9), KM_CFG_OK);
    CHECK_EQ(g_ex_last_flags, 0);
    CHECK_EQ(g_ex_sets, 2);

    /* unknown flag bits: BAD_ARG before anything is applied */
    for (unsigned f = 2; f < 256u; f++) {
        uint8_t bad[9];
        memcpy(bad, nine, 9);
        bad[8] = (uint8_t)f;
        const unsigned want = (f & ~1u) != 0u ? KM_CFG_ERR_BAD_ARG : KM_CFG_OK;
        const unsigned before = g_ex_sets;
        CHECK_EQ(st(KM_CFG_OP_LED_SET, bad, 9), want);
        CHECK_EQ(g_ex_sets, before + (want == KM_CFG_OK ? 1u : 0u));
    }

    /* other lengths: BAD_LENGTH */
    const unsigned sets = g_ex_sets;
    uint8_t buf[12] = { 0 };
    CHECK_EQ(st(KM_CFG_OP_LED_SET, buf, 7), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, buf, 8), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, buf, 10), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, buf, 12), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(g_ex_sets, sets);

    /* the backend's refusals are passed on and the reply carries no light bytes */
    g_ex_result = KM_CFG_ERR_RANGE;
    r = call(KM_CFG_OP_LED_SET, 0x15, nine, 9);
    CHECK_EQ(r.status, KM_CFG_ERR_RANGE);
    CHECK_EQ(r.pn, 0);
    g_ex_result = KM_CFG_ERR_UNSUPPORTED;
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine, 9), KM_CFG_ERR_UNSUPPORTED);
    g_ex_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine, 9), KM_CFG_ERR_BUSY);
    g_ex_result = KM_CFG_OK;
    CHECK_EQ(g_ex_sets, sets);

    /* it never touches the settings: no staging, no commit */
    CHECK_EQ(g_fake.n_commit, 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);

    /* a byte-exact example (CONFIG_PROTOCOL.md): LED_SET effect 3, brightness 200, speed 90, colour 1,2,3, idle 600 s, save; token 0x7e */
    memset(g_ex_v, 0, sizeof g_ex_v);
    g_ex_sets = 0;
    uint8_t req[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN];
    memset(req, 0, sizeof req);
    km_hex("55c60030" "0b000000" "217e" "03c85a010203" "5802" "01", req, 19);
    rep_t g = send_pkt(req);
    CHECK(g.answered);
    CHECK_EQ(g.status, KM_CFG_OK);
    memset(want, 0, sizeof want);
    km_hex("aac60045" "0f000000" "217e00" "03c85a010203" "5802" "11" "00" "01" "00", want, 23);
    CHECK(memcmp(g.raw, want, KM_HID_REPORT_LEN) == 0);
    /* and the matching extended GET: op 0x20, token 0x7f, argument 1 */
    memset(req, 0, sizeof req);
    km_hex("55c600" "a3" "03000000" "207f" "01", req, 11);
    g = send_pkt(req);
    CHECK(g.answered);
    CHECK_EQ(g.status, KM_CFG_OK);
    CHECK_EQ(g.pn, 12);
    CHECK(memcmp(g.pl, "\x03\xc8\x5a\x01\x02\x03\x58\x02\x11\x00\x01\x00", 12) == 0);

    /* the extended ops can be removed again: BAD_LENGTH like an older build */
    km_config_set_led_ex_ops(&cfg, NULL);
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &one, 1), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, nine, 9), KM_CFG_ERR_BAD_LENGTH);
    canaries_ok();
}

/* ---- the per-profile light form: LED_GET with 2 argument bytes (1, profile), LED_SET with 10 (the nine + profile) ---- */

static unsigned g_pf_get_profile, g_pf_set_profile, g_pf_gets, g_pf_sets;

static km_cfg_status_t pf_get(void *ctx, unsigned profile, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES])
{
    (void)ctx;
    g_pf_get_profile = profile;
    g_pf_gets++;
    memcpy(v, g_ex_v, 8);
    v[8] = 0x21;
    v[9] = v[10] = v[11] = 0;
    return KM_CFG_OK;
}

static km_cfg_status_t pf_set(void *ctx, unsigned profile, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES])
{
    (void)ctx;
    g_pf_set_profile = profile;
    g_pf_sets++;
    memcpy(g_ex_v, v, 8);
    return KM_CFG_OK;
}

static const km_config_led_ex_ops_t k_led_pf_ops = { NULL, ex_get, ex_set, pf_get, pf_set };
static const km_config_led_ex_ops_t k_led_half_ops = { NULL, ex_get, ex_set, pf_get, NULL };

static void test_led_profile_ops(void)
{
    static const uint8_t ten[10] = { 3, 200, 90, 1, 2, 3, 0x58, 0x02, KM_CFG_LED_FLAG_SAVE, 2 };
    uint8_t get2[2] = { 1, 3 };

    /* the feature bit: only when both per-profile callbacks are there */
    setup();
    km_config_set_led_ops(&cfg, &k_led_ops);
    km_config_set_led_ex_ops(&cfg, &k_led_ex_ops);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 7) & KM_CFG_FEAT_LED_PROFILE, 0);
    CHECK_EQ(st(KM_CFG_OP_LED_GET, get2, 2), KM_CFG_ERR_BAD_LENGTH);            /* an older build answers like before */
    CHECK_EQ(st(KM_CFG_OP_LED_SET, ten, 10), KM_CFG_ERR_BAD_LENGTH);
    km_config_set_led_ex_ops(&cfg, &k_led_half_ops);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 7) & KM_CFG_FEAT_LED_PROFILE, 0);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, ten, 10), KM_CFG_ERR_BAD_LENGTH);

    setup();
    km_config_set_led_ops(&cfg, &k_led_ops);
    km_config_set_led_ex_ops(&cfg, &k_led_pf_ops);
    CHECK_EQ(u16of(call(KM_CFG_OP_GET_INFO, 1, NULL, 0).pl + 7) & KM_CFG_FEAT_LED_PROFILE, KM_CFG_FEAT_LED_PROFILE);
    memset(g_ex_v, 0, sizeof g_ex_v);
    g_ex_v[0] = 5; g_ex_v[3] = 9;
    g_pf_gets = g_pf_sets = g_ex_sets = g_ex_gets = 0;
    g_ex_result = KM_CFG_OK;

    /* GET of a profile: the profile callback gets the index, the reply is the usual twelve bytes */
    rep_t r = call(KM_CFG_OP_LED_GET, 0x31, get2, 2);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 12);
    CHECK_EQ(r.pl[0], 5);
    CHECK_EQ(r.pl[8], 0x21);
    CHECK_EQ(g_pf_gets, 1);
    CHECK_EQ(g_pf_get_profile, 3);
    CHECK_EQ(g_ex_gets, 0);
    get2[1] = KM_CFG_LED_PROFILE_ACTIVE;
    CHECK_EQ(st(KM_CFG_OP_LED_GET, get2, 2), KM_CFG_OK);
    CHECK_EQ(g_pf_get_profile, KM_CFG_LED_PROFILE_ACTIVE);
    get2[1] = 4;                                                      /* profile_count is 4: index 4 does not exist */
    CHECK_EQ(st(KM_CFG_OP_LED_GET, get2, 2), KM_CFG_ERR_BAD_ARG);
    get2[1] = 0xFE;
    CHECK_EQ(st(KM_CFG_OP_LED_GET, get2, 2), KM_CFG_ERR_BAD_ARG);
    get2[0] = 2; get2[1] = 0;
    CHECK_EQ(st(KM_CFG_OP_LED_GET, get2, 2), KM_CFG_ERR_BAD_ARG);
    CHECK_EQ(g_pf_gets, 2);

    /* SET of a profile: ten bytes; the nine go to the backend, the profile separately; the reply is the profile's twelve bytes */
    r = call(KM_CFG_OP_LED_SET, 0x32, ten, 10);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 12);
    CHECK_EQ(g_pf_sets, 1);
    CHECK_EQ(g_pf_set_profile, 2);
    CHECK_EQ(g_ex_sets, 0);
    CHECK(memcmp(g_ex_v, ten, 8) == 0);
    CHECK_EQ(g_pf_gets, 3);
    uint8_t bad[10];
    memcpy(bad, ten, 10);
    bad[9] = 4;
    CHECK_EQ(st(KM_CFG_OP_LED_SET, bad, 10), KM_CFG_ERR_BAD_ARG);        /* bad profile: nothing applied */
    bad[9] = 2;
    bad[8] = 2;
    CHECK_EQ(st(KM_CFG_OP_LED_SET, bad, 10), KM_CFG_ERR_BAD_ARG);        /* unknown flag */
    bad[8] = 1;
    bad[9] = KM_CFG_LED_PROFILE_ACTIVE;
    CHECK_EQ(st(KM_CFG_OP_LED_SET, bad, 10), KM_CFG_OK);
    CHECK_EQ(g_pf_set_profile, KM_CFG_LED_PROFILE_ACTIVE);
    CHECK_EQ(g_pf_sets, 2);

    /* the nine-byte and one-argument forms are unchanged and go to the plain extended callbacks */
    uint8_t one = 1;
    CHECK_EQ(st(KM_CFG_OP_LED_GET, &one, 1), KM_CFG_OK);
    CHECK_EQ(g_ex_gets, 1);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, ten, 9), KM_CFG_OK);
    CHECK_EQ(g_ex_sets, 1);
    CHECK_EQ(g_pf_sets, 2);
    CHECK_EQ(st(KM_CFG_OP_LED_SET, ten, 11), KM_CFG_ERR_BAD_LENGTH);

    /* a backend refusal is passed on; a byte-exact example (CONFIG_PROTOCOL.md): LED_SET profile 2, token 0x7e */
    g_ex_result = KM_CFG_OK;
    uint8_t req[KM_HID_REPORT_LEN];
    memset(req, 0, sizeof req);
    km_hex("55c60033" "0c000000" "217e" "03c85a010203" "5802" "01" "02", req, 20);
    rep_t g = send_pkt(req);
    CHECK(g.answered);
    CHECK_EQ(g.status, KM_CFG_OK);
    CHECK_EQ(g.pn, 12);
    CHECK(memcmp(g.pl, "\x03\xc8\x5a\x01\x02\x03\x58\x02\x21\x00\x00\x00", 12) == 0);
    canaries_ok();
}

static void test_protocol2_info(void)
{
    /* nothing registered: version 2, none of the protocol-2 feature bits, scene limit 0 */
    setup();
    rep_t r = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(r.pl[0], KM_CONFIG_PROTO_VERSION);
    CHECK_EQ(KM_CONFIG_PROTO_VERSION, 2);
    CHECK_EQ(u16of(r.pl + 7) & 0x0780u, 0);
    CHECK_EQ(u16of(r.pl + 18), 0);

    /* both registered: bits 7 and 8, scene limit 1024 */
    setup_p2();
    r = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(u16of(r.pl + 7), 0x3Fu | KM_CFG_FEAT_DEVICE_STATUS | KM_CFG_FEAT_SCENE);
    CHECK_EQ(u16of(r.pl + 18), KM_CONFIG_MAX_SCENE);

    /* the extra bits come only from km_config_set_extra_features and only bits 9 and 10 are accepted */
    km_config_set_extra_features(&cfg, 0xFFFFu);
    r = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(u16of(r.pl + 7), 0x3Fu | KM_CFG_FEAT_DEVICE_STATUS | KM_CFG_FEAT_SCENE | KM_CFG_FEAT_STREAM | KM_CFG_FEAT_HOST_LEDS);
    km_config_set_extra_features(&cfg, KM_CFG_FEAT_STREAM);
    r = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(u16of(r.pl + 7) & 0x0600u, KM_CFG_FEAT_STREAM);

    /* scene ops without a commit callback do not announce the scene feature */
    static const km_config_scene_ops_t read_only = { NULL, sc_read, NULL, NULL };
    km_config_set_scene_ops(&cfg, &read_only);
    r = call(KM_CFG_OP_GET_INFO, 1, NULL, 0);
    CHECK_EQ(u16of(r.pl + 7) & KM_CFG_FEAT_SCENE, 0);
    CHECK_EQ(u16of(r.pl + 18), 0);
    canaries_ok();
}

static void test_device_status(void)
{
    /* without a backend: UNSUPPORTED (and BAD_LENGTH first for a malformed request) */
    setup();
    CHECK_EQ(st(KM_CFG_OP_DEVICE_STATUS, NULL, 0), KM_CFG_ERR_UNSUPPORTED);
    uint8_t one = 0;
    CHECK_EQ(st(KM_CFG_OP_DEVICE_STATUS, &one, 1), KM_CFG_ERR_BAD_LENGTH);

    setup_p2();
    for (unsigned i = 0; i < sizeof g_dev_v; i++) {
        g_dev_v[i] = (uint8_t)(0x10u + i);
    }
    rep_t r = call(KM_CFG_OP_DEVICE_STATUS, 0x44, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, KM_CONFIG_DEVICE_STATUS_BYTES);
    CHECK(memcmp(r.pl, g_dev_v, sizeof g_dev_v) == 0);
    CHECK_EQ(g_dev_gets, 1);

    /* an argument is a BAD_LENGTH and the backend is not asked */
    r = call(KM_CFG_OP_DEVICE_STATUS, 0x45, &one, 1);
    CHECK_EQ(r.status, KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(r.pn, 0);
    CHECK_EQ(g_dev_gets, 1);

    /* backend errors are passed on, unknown codes become FAILED; no payload */
    g_dev_result = KM_CFG_ERR_BUSY;
    r = call(KM_CFG_OP_DEVICE_STATUS, 0x46, NULL, 0);
    CHECK_EQ(r.status, KM_CFG_ERR_BUSY);
    CHECK_EQ(r.pn, 0);
    g_dev_result = (km_cfg_status_t)99;
    CHECK_EQ(st(KM_CFG_OP_DEVICE_STATUS, NULL, 0), KM_CFG_ERR_FAILED);

    /* read-only: it has no effect on the staging of the settings or the scene, nor on any commit counter */
    g_dev_result = KM_CFG_OK;
    uint8_t a[2] = { 10, 0 };
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, a, 2), KM_CFG_OK);
    (void)call(KM_CFG_OP_DEVICE_STATUS, 0x47, NULL, 0);
    CHECK_EQ(stage_state(), KM_CFG_STAGE_RECEIVING);
    CHECK_EQ(g_fake.n_commit + g_sc.n_commit, 0);
    canaries_ok();
}

static unsigned scene_get_all(uint8_t *out, unsigned max, uint32_t *crc_out)
{
    unsigned have = 0, id = 0, total = 0;
    do {
        uint8_t a[4];
        put16(a, have);
        a[2] = KM_CONFIG_GET_PAGE_BYTES;
        a[3] = (uint8_t)id;
        rep_t r = call(KM_CFG_OP_SCENE_GET, 0x60, a, 4);
        CHECK_EQ(r.status, KM_CFG_OK);
        if (r.status != KM_CFG_OK || r.pn < 9) {
            return 0;
        }
        if (have == 0) {
            id = r.pl[0];
            total = u16of(r.pl + 1);
            *crc_out = u32of(r.pl + 3);
        }
        CHECK_EQ(u16of(r.pl + 7), have);
        const unsigned n = r.pn - 9u;
        if (n == 0 || have + n > max) {
            return 0;
        }
        memcpy(out + have, r.pl + 9, n);
        have += n;
    } while (have < total);
    return have;
}

static void test_scene_roundtrip(void)
{
    setup_p2();
    g_sc.live_len = 214;
    make_data(g_sc.live, 214, 9);
    uint8_t got[KM_CONFIG_MAX_SCENE];
    uint32_t crc = 0;
    CHECK_EQ(scene_get_all(got, sizeof got, &crc), 214);
    CHECK(memcmp(got, g_sc.live, 214) == 0);
    CHECK(crc == km_config_crc32(g_sc.live, 214));
    CHECK_EQ(g_sc.n_read, 1);                                   /* one snapshot for the whole read */

    /* the settings and the scene do not share anything: reading one does not touch the other's snapshot */
    uint8_t q[4] = { 0, 0, 10, 0 };
    rep_t r = call(KM_CFG_OP_GET_SETTINGS, 1, q, 4);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(g_fake.n_read, 1);
    uint8_t q2[4] = { 44, 0, 10, r.pl[0] };                     /* the settings snapshot id */
    /* the scene snapshot id is the first one (1), the settings one is also 1: use a wrong id on purpose */
    q2[3] = 77;
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, q2, 4), KM_CFG_ERR_STALE);
}

static void scene_stage_all(const uint8_t *d, unsigned len, unsigned page)
{
    uint8_t a[2 + KM_CONFIG_SET_PAGE_BYTES];
    put16(a, len);
    rep_t b = call(KM_CFG_OP_SCENE_BEGIN, 0x61, a, 2);
    CHECK_EQ(b.status, KM_CFG_OK);
    CHECK_EQ(b.pn, 2);
    CHECK_EQ(u16of(b.pl), KM_CONFIG_MAX_SCENE);                 /* the scene limit, not the settings limit */
    for (unsigned off = 0; off < len; off += page) {
        const unsigned n = len - off < page ? len - off : page;
        put16(a, off);
        memcpy(a + 2, d + off, n);
        rep_t r = call(KM_CFG_OP_SCENE_PAGE, 0x62, a, 2 + n);
        CHECK_EQ(r.status, KM_CFG_OK);
        CHECK_EQ(u16of(r.pl), off + n);
    }
}

static unsigned scene_commit(const char *word, unsigned len, uint32_t crc)
{
    uint8_t a[10];
    commit_args(a, word, len, crc);
    return st(KM_CFG_OP_SCENE_COMMIT, a, 10);
}

static void test_scene_write(void)
{
    setup_p2();
    uint8_t d[214];
    make_data(d, sizeof d, 5);
    const uint32_t crc = km_config_crc32(d, sizeof d);

    scene_stage_all(d, sizeof d, 52);
    /* the settings staging is untouched */
    CHECK_EQ(stage_state(), KM_CFG_STAGE_EMPTY);

    /* confirmation: none / the settings word / garbage -> NEED_CONFIRM, nothing committed */
    CHECK_EQ(scene_commit("\0\0\0\0", sizeof d, crc), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_COMMIT, sizeof d, crc), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(scene_commit("LITF", sizeof d, crc), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(g_sc.n_commit, 0);
    /* the scene word does not commit the settings either */
    CHECK_EQ(do_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(g_fake.n_commit, 0);
    /* wrong length / CRC */
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d - 1, crc), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc ^ 1u), KM_CFG_ERR_BAD_CRC);
    CHECK_EQ(g_sc.n_commit, 0);
    /* the validator refuses -> INVALID, staging stays so the host can retry after fixing the cause */
    g_sc.validate_result = KM_CFG_ERR_INVALID;
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc), KM_CFG_ERR_INVALID);
    g_sc.validate_result = KM_CFG_ERR_BUSY;
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc), KM_CFG_ERR_BUSY);
    g_sc.validate_result = KM_CFG_OK;
    CHECK_EQ(g_sc.n_commit, 0);
    /* a failing commit is passed on and keeps the staging */
    g_sc.commit_result = KM_CFG_ERR_FAILED;
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc), KM_CFG_ERR_FAILED);
    CHECK_EQ(g_sc.n_commit, 1);
    g_sc.commit_result = KM_CFG_OK;
    /* the real thing; reply = scene commit counter */
    uint8_t a[10];
    commit_args(a, KM_CFG_CONFIRM_SCENE, sizeof d, crc);
    rep_t r = call(KM_CFG_OP_SCENE_COMMIT, 0x63, a, 10);
    CHECK_EQ(r.status, KM_CFG_OK);
    CHECK_EQ(r.pn, 4);
    CHECK_EQ(u32of(r.pl), 1);
    CHECK_EQ(g_sc.n_commit, 2);
    CHECK_EQ(g_sc.committed_len, sizeof d);
    CHECK(memcmp(g_sc.committed, d, sizeof d) == 0);
    CHECK_EQ(cfg.commits, 0);                                   /* the settings commit counter is separate */
    /* a second commit has nothing staged */
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, sizeof d, crc), KM_CFG_ERR_NOT_STAGED);

    /* limits: 0 -> BAD_ARG, above 1024 -> TOO_BIG (although the settings accept up to 8192) */
    uint8_t b[2];
    put16(b, 0);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, b, 2), KM_CFG_ERR_BAD_ARG);
    put16(b, KM_CONFIG_MAX_SCENE + 1u);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, b, 2), KM_CFG_ERR_TOO_BIG);
    put16(b, KM_CONFIG_MAX_SCENE);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, b, 2), KM_CFG_OK);
    put16(b, KM_CONFIG_MAX_SCENE + 1u);
    CHECK_EQ(st(KM_CFG_OP_SET_BEGIN, b, 2), KM_CFG_OK);         /* the settings channel is not limited by the scene limit */

    /* page rules are the settings' rules: out of order -> SEQUENCE with the next offset; repeat -> acknowledged; past the end -> RANGE */
    setup_p2();
    put16(b, 100);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, b, 2), KM_CFG_OK);
    uint8_t pg[2 + 20];
    put16(pg, 10);
    memset(pg + 2, 1, 20);
    rep_t q = call(KM_CFG_OP_SCENE_PAGE, 1, pg, 22);
    CHECK_EQ(q.status, KM_CFG_ERR_SEQUENCE);
    CHECK_EQ(u16of(q.pl), 0);
    put16(pg, 0);
    q = call(KM_CFG_OP_SCENE_PAGE, 2, pg, 22);
    CHECK_EQ(q.status, KM_CFG_OK);
    q = call(KM_CFG_OP_SCENE_PAGE, 3, pg, 22);                  /* the same page again */
    CHECK_EQ(q.status, KM_CFG_OK);
    CHECK_EQ(u16of(q.pl), 20);
    put16(pg, 20);
    uint8_t big[2 + 52];
    put16(big, 20);
    memset(big + 2, 2, 52);
    q = call(KM_CFG_OP_SCENE_PAGE, 4, big, 54);
    CHECK_EQ(q.status, KM_CFG_OK);
    q = call(KM_CFG_OP_SCENE_PAGE, 5, pg, 22);                  /* a stale offset */
    CHECK_EQ(q.status, KM_CFG_ERR_SEQUENCE);
    /* a begin without any page leaves nothing committable */
    put16(b, 10);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, b, 2), KM_CFG_OK);
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, 10, 0), KM_CFG_ERR_NOT_STAGED);
    canaries_ok();
}

static void test_scene_unsupported_and_lengths(void)
{
    /* no scene backend: every scene op answers UNSUPPORTED after its length/confirmation checks, nothing is staged */
    setup();
    uint8_t a[10] = { 0 };
    uint8_t g[4] = { 0, 0, 10, 0 };
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_UNSUPPORTED);
    put16(a, 10);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, a, 2), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(scene_commit(KM_CFG_CONFIRM_SCENE, 10, 0), KM_CFG_ERR_UNSUPPORTED);
    CHECK_EQ(scene_commit("NOPE", 10, 0), KM_CFG_ERR_NEED_CONFIRM);
    CHECK_EQ(st(KM_CFG_OP_SCENE_PAGE, a, 5), KM_CFG_ERR_NOT_STAGED);

    /* length errors */
    setup_p2();
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 3), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 0), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, a, 3), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_BEGIN, a, 0), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_PAGE, a, 2), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_COMMIT, a, 9), KM_CFG_ERR_BAD_LENGTH);
    CHECK_EQ(st(KM_CFG_OP_SCENE_COMMIT, NULL, 0), KM_CFG_ERR_BAD_LENGTH);
    /* get arguments */
    g[2] = 0;
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_BAD_ARG);
    g[2] = KM_CONFIG_GET_PAGE_BYTES + 1;
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_BAD_ARG);
    g[2] = 10;
    g[3] = 5;                                                   /* a snapshot id with offset 0 */
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_BAD_ARG);
    g[3] = 0;
    g[0] = 44;                                                  /* offset > 0 without a snapshot */
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_STALE);
    /* a scene that does not fit the buffer: the backend returns 0 -> FAILED, no snapshot */
    g_sc.live_len = KM_CONFIG_MAX_SCENE + 1u;
    g[0] = 0;
    CHECK_EQ(st(KM_CFG_OP_SCENE_GET, g, 4), KM_CFG_ERR_FAILED);
    canaries_ok();
}

/* every protocol-1 op answers exactly as before when protocol-2 backends are registered (they are additive) */
static void test_protocol2_is_additive(void)
{
    uint8_t d[100];
    make_data(d, sizeof d, 2);
    for (unsigned with = 0; with < 2; with++) {
        if (with) {
            setup_p2();
        } else {
            setup();
        }
        stage_all(d, sizeof d, 52);
        CHECK_EQ(stage_state(), KM_CFG_STAGE_COMPLETE);
        CHECK_EQ(do_commit(KM_CFG_CONFIRM_COMMIT, sizeof d, km_config_crc32(d, sizeof d)), KM_CFG_OK);
        CHECK_EQ(g_fake.committed_len, sizeof d);
        CHECK(memcmp(g_fake.committed, d, sizeof d) == 0);
        uint8_t pa[5];
        memcpy(pa, KM_CFG_CONFIRM_PROFILE, 4);
        pa[4] = 1;
        rep_t r = call(KM_CFG_OP_SELECT_PROFILE, 1, pa, 5);
        CHECK_EQ(r.status, KM_CFG_OK);
        CHECK_EQ(r.pl[0], 1);
    }
    /* an unused op number next to the new ones is still UNKNOWN_OP */
    CHECK_EQ(st(0x0B, NULL, 0), KM_CFG_ERR_UNKNOWN_OP);
    CHECK_EQ(st(0x2F, NULL, 0), KM_CFG_ERR_UNKNOWN_OP);
    CHECK_EQ(st(0x34, NULL, 0), KM_CFG_ERR_UNKNOWN_OP);
    canaries_ok();
}

void test_config(void)
{
    test_crc();
    test_info();
    test_get_settings();
    test_get_settings_errors();
    test_snapshot_id_wraps_and_skips_zero();
    test_set_and_commit();
    test_commit_confirmation();
    test_commit_checks();
    test_begin();
    test_paging();
    test_page_range_and_bounds();
    test_select_profile();
    test_calibration_requests();
    test_live_depth();
    test_malformed_requests();
    test_vendor_integration();
    test_golden();
    test_random_never_commits_unconfirmed();
    test_led_ops();
    test_led_ex_ops();
    test_led_profile_ops();
    test_protocol2_info();
    test_device_status();
    test_scene_roundtrip();
    test_scene_write();
    test_scene_unsupported_and_lengths();
    test_protocol2_is_additive();
}
