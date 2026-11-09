#include "km/km_config.h"

#include <string.h>

/* Request data: [0] op, [1] token, [2..] arguments. Reply data: [0] op, [1] token, [2] status, [3..] payload. */
#define ARG0 2u
#define PAYLOAD0 3u

typedef struct {
    const uint8_t *a;      /* request data (bytes beyond len are zero) */
    unsigned len;          /* request data length */
    uint8_t *pl;           /* reply payload buffer (KM_HID_MAX_DATA - PAYLOAD0 bytes) */
    unsigned pn;           /* reply payload length, set by the handler */
} call_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

uint32_t km_config_crc32(const uint8_t *data, size_t len)
{
    static const uint32_t nib[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ nib[crc & 0xFu];
        crc = (crc >> 4) ^ nib[crc & 0xFu];
    }
    return ~crc;
}

void km_config_init(km_config_t *c, const km_config_ops_t *ops)
{
    memset(c, 0, sizeof *c);
    c->ops = ops;
}

void km_config_set_led_ops(km_config_t *c, const km_config_led_ops_t *led)
{
    c->led = led;
}

void km_config_set_led_ex_ops(km_config_t *c, const km_config_led_ex_ops_t *ex)
{
    c->led_ex = ex;
}

/* A callback result as the host may see it: anything outside the table becomes FAILED. */
static km_cfg_status_t cb_status(km_cfg_status_t s)
{
    return (unsigned)s <= (unsigned)KM_CFG_ERR_FAILED ? s : KM_CFG_ERR_FAILED;
}

static bool confirmed(const call_t *k, const char word[4])
{
    return memcmp(k->a + ARG0, word, 4) == 0;
}

/* One paged blob: the settings (staging and snapshot buffers of km_config_t) or the light scene (its own small buffers). */
typedef struct {
    km_cfg_blob_t *b;
    uint8_t *stage;
    uint8_t *snap;
    unsigned cap;
} blob_t;

static blob_t settings_blob(km_config_t *c)
{
    const blob_t v = { &c->set, c->stage, c->snap, KM_CONFIG_MAX_SETTINGS };
    return v;
}

static blob_t scene_blob(km_config_t *c)
{
    const blob_t v = { &c->sc, c->sc_stage, c->sc_snap, KM_CONFIG_MAX_SCENE };
    return v;
}

static void drop_snapshot(km_cfg_blob_t *b)
{
    b->snap_len = 0;                              /* 0 = no snapshot; snap_id keeps counting so old ids never come back soon */
}

static void clear_stage(km_cfg_blob_t *b)
{
    b->stage_state = KM_CFG_STAGE_EMPTY;
    b->stage_total = 0;
    b->stage_next = 0;
    b->last_off = 0;
    b->last_n = 0;
}

void km_config_set_device_ops(km_config_t *c, const km_config_device_ops_t *dev)
{
    c->dev = dev;
}

void km_config_set_scene_ops(km_config_t *c, const km_config_scene_ops_t *scene)
{
    c->scene = scene;
}

void km_config_set_extra_features(km_config_t *c, uint16_t bits)
{
    c->extra_feat = (uint16_t)(bits & KM_CFG_FEAT_EXTRA_MASK);
}

/* ---- sub-commands. Each checks everything first and changes state only at the end. ---- */

static km_cfg_status_t op_get_info(km_config_t *c, call_t *k)
{
    const km_config_ops_t *o = c->ops;
    if (k->len != ARG0) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    unsigned feat = 0;
    feat |= o->read_settings ? KM_CFG_FEAT_READ_SETTINGS : 0u;
    feat |= o->commit_settings ? KM_CFG_FEAT_WRITE_SETTINGS : 0u;
    feat |= o->select_profile ? KM_CFG_FEAT_SELECT_PROFILE : 0u;
    feat |= o->recalibrate ? KM_CFG_FEAT_RECALIBRATE : 0u;
    feat |= o->save_calibration ? KM_CFG_FEAT_SAVE_CAL : 0u;
    feat |= o->live_depth ? KM_CFG_FEAT_LIVE_DEPTH : 0u;
    feat |= (c->led != NULL && c->led->get != NULL && c->led->set != NULL) ? KM_CFG_FEAT_LED : 0u;
    feat |= (c->dev != NULL && c->dev->get != NULL) ? KM_CFG_FEAT_DEVICE_STATUS : 0u;
    feat |= (c->led != NULL && c->led_ex != NULL && c->led_ex->get_p != NULL && c->led_ex->set_p != NULL) ? KM_CFG_FEAT_LED_PROFILE : 0u;
    feat |= (c->scene != NULL && c->scene->read_scene != NULL && c->scene->commit_scene != NULL) ? KM_CFG_FEAT_SCENE : 0u;
    feat |= c->extra_feat;

    uint8_t *p = k->pl;
    p[0] = KM_CONFIG_PROTO_VERSION;
    p[1] = o->profile_count;
    p[2] = o->active_profile ? (uint8_t)o->active_profile(o->ctx) : 0xFFu;
    wr16(p + 3, KM_CONFIG_MAX_SETTINGS);
    p[5] = KM_CONFIG_GET_PAGE_BYTES;
    p[6] = KM_CONFIG_SET_PAGE_BYTES;
    wr16(p + 7, (uint16_t)feat);
    p[9] = c->set.stage_state;
    wr16(p + 10, c->set.stage_total);
    wr16(p + 12, c->set.stage_next);
    wr32(p + 14, c->commits);
    wr16(p + 18, (feat & KM_CFG_FEAT_SCENE) != 0u ? KM_CONFIG_MAX_SCENE : 0u);   /* protocol 2: the scene size limit, 0 = no scene ops */
    k->pn = 20;
    return KM_CFG_OK;
}

/* GET_SETTINGS / SCENE_GET: read a blob page by page from a snapshot taken at offset 0. */
static km_cfg_status_t blob_get(const blob_t *v, call_t *k, void *ctx, unsigned (*read)(void *ctx, uint8_t *buf, unsigned cap))
{
    km_cfg_blob_t *b = v->b;
    if (k->len != ARG0 + 4u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    const unsigned off = rd16(k->a + ARG0);
    const unsigned want = k->a[ARG0 + 2];
    const unsigned id = k->a[ARG0 + 3];
    if (want == 0 || want > KM_CONFIG_GET_PAGE_BYTES || (off == 0 && id != 0)) {
        return KM_CFG_ERR_BAD_ARG;
    }
    if (read == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    if (off == 0) {
        /* page 0 takes a fresh snapshot; all following pages of this read come from it */
        const unsigned n = read(ctx, v->snap, v->cap);
        if (n == 0 || n > v->cap) {
            drop_snapshot(b);
            return KM_CFG_ERR_FAILED;
        }
        b->snap_len = (uint16_t)n;
        b->snap_crc = km_config_crc32(v->snap, n);
        b->snap_id = (uint8_t)(b->snap_id % 255u + 1u);   /* 1..255 */
    } else {
        if (b->snap_len == 0 || id != b->snap_id) {
            return KM_CFG_ERR_STALE;
        }
        if (off > b->snap_len) {
            return KM_CFG_ERR_RANGE;
        }
    }
    unsigned n = b->snap_len - off;
    if (n > want) {
        n = want;
    }
    uint8_t *p = k->pl;
    p[0] = b->snap_id;
    wr16(p + 1, b->snap_len);
    wr32(p + 3, b->snap_crc);
    wr16(p + 7, (uint16_t)off);
    memcpy(p + 9, v->snap + off, n);
    k->pn = 9u + n;
    return KM_CFG_OK;
}

static km_cfg_status_t blob_begin(const blob_t *v, call_t *k, bool can_commit)
{
    if (k->len != ARG0 + 2u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    const unsigned total = rd16(k->a + ARG0);
    if (total == 0) {
        return KM_CFG_ERR_BAD_ARG;
    }
    if (total > v->cap) {
        return KM_CFG_ERR_TOO_BIG;
    }
    if (!can_commit) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    clear_stage(v->b);
    v->b->stage_total = (uint16_t)total;
    v->b->stage_state = KM_CFG_STAGE_RECEIVING;
    wr16(k->pl, (uint16_t)v->cap);
    k->pn = 2;
    return KM_CFG_OK;
}

static km_cfg_status_t blob_page(const blob_t *v, call_t *k)
{
    km_cfg_blob_t *b = v->b;
    if (k->len < ARG0 + 3u) {                    /* offset (2) + at least one data byte */
        return KM_CFG_ERR_BAD_LENGTH;
    }
    const unsigned off = rd16(k->a + ARG0);
    const unsigned n = k->len - (ARG0 + 2u);     /* at most 52 because a request carries at most 56 data bytes */
    const uint8_t *data = k->a + ARG0 + 2u;
    if (b->stage_state == KM_CFG_STAGE_EMPTY) {
        return KM_CFG_ERR_NOT_STAGED;
    }
    wr16(k->pl, b->stage_next);
    k->pn = 2;
    if (b->last_n != 0 && off == b->last_off && n == b->last_n && memcmp(v->stage + off, data, n) == 0) {
        return KM_CFG_OK;                         /* the same page again (its reply was lost): acknowledged, no effect */
    }
    if (off != b->stage_next) {
        return KM_CFG_ERR_SEQUENCE;
    }
    if (off + n > b->stage_total) {
        return KM_CFG_ERR_RANGE;
    }
    memcpy(v->stage + off, data, n);
    b->last_off = (uint16_t)off;
    b->last_n = (uint16_t)n;
    b->stage_next = (uint16_t)(off + n);
    if (b->stage_next == b->stage_total) {
        b->stage_state = KM_CFG_STAGE_COMPLETE;
    }
    wr16(k->pl, b->stage_next);
    return KM_CFG_OK;
}

/* COMMIT / SCENE_COMMIT: the confirmed, complete, checksummed staging buffer goes through validate to commit. On success the commit
 * counter is incremented and returned. */
static km_cfg_status_t blob_commit(const blob_t *v, call_t *k, const char word[4], void *ctx,
                                   km_cfg_status_t (*validate)(void *ctx, const uint8_t *blob, unsigned len),
                                   km_cfg_status_t (*commit)(void *ctx, const uint8_t *blob, unsigned len), uint32_t *counter)
{
    km_cfg_blob_t *b = v->b;
    if (k->len != ARG0 + 4u + 2u + 4u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (!confirmed(k, word)) {
        return KM_CFG_ERR_NEED_CONFIRM;
    }
    if (commit == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    if (b->stage_state != KM_CFG_STAGE_COMPLETE || b->stage_next != b->stage_total) {
        return KM_CFG_ERR_NOT_STAGED;
    }
    if (rd16(k->a + ARG0 + 4) != b->stage_total || rd32(k->a + ARG0 + 6) != km_config_crc32(v->stage, b->stage_total)) {
        return KM_CFG_ERR_BAD_CRC;
    }
    if (validate != NULL) {
        const km_cfg_status_t r = validate(ctx, v->stage, b->stage_total);
        if (r != KM_CFG_OK) {
            return r == KM_CFG_ERR_BUSY ? KM_CFG_ERR_BUSY : KM_CFG_ERR_INVALID;
        }
    }
    const km_cfg_status_t r = cb_status(commit(ctx, v->stage, b->stage_total));
    if (r != KM_CFG_OK) {
        return r;                                 /* the staged bytes stay: the host may retry */
    }
    clear_stage(b);
    drop_snapshot(b);                             /* the live data changed: older read pages are stale */
    (*counter)++;
    wr32(k->pl, *counter);
    k->pn = 4;
    return KM_CFG_OK;
}

static km_cfg_status_t op_get_settings(km_config_t *c, call_t *k)
{
    const blob_t v = settings_blob(c);
    return blob_get(&v, k, c->ops->ctx, c->ops->read_settings);
}

static km_cfg_status_t op_set_begin(km_config_t *c, call_t *k)
{
    const blob_t v = settings_blob(c);
    return blob_begin(&v, k, c->ops->commit_settings != NULL);
}

static km_cfg_status_t op_set_page(km_config_t *c, call_t *k)
{
    const blob_t v = settings_blob(c);
    return blob_page(&v, k);
}

static km_cfg_status_t op_commit(km_config_t *c, call_t *k)
{
    const km_config_ops_t *o = c->ops;
    const blob_t v = settings_blob(c);
    return blob_commit(&v, k, KM_CFG_CONFIRM_COMMIT, o->ctx, o->validate_settings, o->commit_settings, &c->commits);
}

/* ---- protocol 2: the light scene blob, with its own staging buffers ---- */
static bool scene_ok(const km_config_t *c)
{
    return c->scene != NULL && c->scene->read_scene != NULL && c->scene->commit_scene != NULL;
}

static km_cfg_status_t op_scene_get(km_config_t *c, call_t *k)
{
    const blob_t v = scene_blob(c);
    return blob_get(&v, k, scene_ok(c) ? c->scene->ctx : NULL, scene_ok(c) ? c->scene->read_scene : NULL);
}

static km_cfg_status_t op_scene_begin(km_config_t *c, call_t *k)
{
    const blob_t v = scene_blob(c);
    return blob_begin(&v, k, scene_ok(c));
}

static km_cfg_status_t op_scene_page(km_config_t *c, call_t *k)
{
    const blob_t v = scene_blob(c);
    return blob_page(&v, k);
}

static km_cfg_status_t op_scene_commit(km_config_t *c, call_t *k)
{
    const blob_t v = scene_blob(c);
    const km_config_scene_ops_t *so = scene_ok(c) ? c->scene : NULL;
    return blob_commit(&v, k, KM_CFG_CONFIRM_SCENE, so ? so->ctx : NULL, so ? so->validate_scene : NULL, so ? so->commit_scene : NULL,
                       &c->scene_commits);
}

/* DEVICE_STATUS: the 16 bytes of the backend, read-only. */
static km_cfg_status_t op_device_status(km_config_t *c, call_t *k)
{
    if (k->len != ARG0) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (c->dev == NULL || c->dev->get == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    uint8_t v[KM_CONFIG_DEVICE_STATUS_BYTES] = { 0 };
    const km_cfg_status_t r = cb_status(c->dev->get(c->dev->ctx, v));
    if (r != KM_CFG_OK) {
        return r;
    }
    memcpy(k->pl, v, sizeof v);
    k->pn = sizeof v;
    return KM_CFG_OK;
}

static km_cfg_status_t op_select_profile(km_config_t *c, call_t *k)
{
    const km_config_ops_t *o = c->ops;
    if (k->len != ARG0 + 4u + 1u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (!confirmed(k, KM_CFG_CONFIRM_PROFILE)) {
        return KM_CFG_ERR_NEED_CONFIRM;
    }
    if (o->select_profile == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    const unsigned index = k->a[ARG0 + 4];
    if (index >= o->profile_count) {
        return KM_CFG_ERR_RANGE;
    }
    const km_cfg_status_t r = cb_status(o->select_profile(o->ctx, index));
    if (r != KM_CFG_OK) {
        return r;
    }
    drop_snapshot(&c->set);
    k->pl[0] = o->active_profile ? (uint8_t)o->active_profile(o->ctx) : (uint8_t)index;
    k->pn = 1;
    return KM_CFG_OK;
}

static km_cfg_status_t op_calibration(km_config_t *c, call_t *k, const char word[4],
                                      km_cfg_status_t (*fn)(void *ctx))
{
    if (k->len != ARG0 + 4u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (!confirmed(k, word)) {
        return KM_CFG_ERR_NEED_CONFIRM;
    }
    if (fn == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    return cb_status(fn(c->ops->ctx));
}

static km_cfg_status_t op_live_depth(km_config_t *c, call_t *k)
{
    const km_config_ops_t *o = c->ops;
    if (k->len != ARG0 + 2u) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    const unsigned first = k->a[ARG0];
    const unsigned count = k->a[ARG0 + 1];
    if (count == 0 || count > KM_CONFIG_MAX_DEPTHS) {
        return KM_CFG_ERR_BAD_ARG;
    }
    if (first + count > KM_CONFIG_MAX_SLOTS) {
        return KM_CFG_ERR_RANGE;
    }
    if (o->live_depth == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    uint16_t v[KM_CONFIG_MAX_DEPTHS];
    const km_cfg_status_t r = cb_status(o->live_depth(o->ctx, first, count, v));
    if (r != KM_CFG_OK) {
        return r;
    }
    k->pl[0] = (uint8_t)first;
    k->pl[1] = (uint8_t)count;
    for (unsigned i = 0; i < count; i++) {
        wr16(k->pl + 2u + 2u * i, v[i]);
    }
    k->pn = 2u + 2u * count;
    return KM_CFG_OK;
}

/* The extended payload of LED_GET / LED_SET (12 bytes). */
static bool led_prof_ops(const km_config_t *c)
{
    return c->led_ex != NULL && c->led_ex->get_p != NULL && c->led_ex->set_p != NULL;
}

/* profile argument: 0..profile_count-1 or ACTIVE; anything else is BAD_ARG */
static bool led_prof_arg_ok(const km_config_t *c, uint8_t p)
{
    return p == KM_CFG_LED_PROFILE_ACTIVE || p < c->ops->profile_count;
}

static km_cfg_status_t led_reply_ex(km_config_t *c, call_t *k, bool with_profile, uint8_t profile)
{
    uint8_t v[KM_CONFIG_LED_GET_EX_BYTES] = { 0 };
    const km_cfg_status_t gr = with_profile ? c->led_ex->get_p(c->led_ex->ctx, profile, v) : c->led_ex->get(c->led_ex->ctx, v);
    if (cb_status(gr) != KM_CFG_OK) {
        return KM_CFG_ERR_FAILED;
    }
    memcpy(k->pl, v, sizeof v);
    k->pn = sizeof v;
    return KM_CFG_OK;
}

/* LED_GET: no arguments = the six light bytes; one argument byte 1 = the extended 12 bytes. */
static km_cfg_status_t op_led_get(km_config_t *c, call_t *k)
{
    const bool prof = k->len == ARG0 + KM_CONFIG_LED_GET_PROF_ARGS;
    const bool ex = k->len == ARG0 + 1u || prof;
    if (k->len != ARG0 && !ex) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (ex && c->led != NULL && (c->led_ex == NULL || c->led_ex->get == NULL || (prof && !led_prof_ops(c)))) {
        return KM_CFG_ERR_BAD_LENGTH;                /* a build without the extended form answers like the old firmware did */
    }
    if (c->led == NULL || c->led->get == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    if (ex) {
        if (k->a[ARG0] != 1u || (prof && !led_prof_arg_ok(c, k->a[ARG0 + 1u]))) {
            return KM_CFG_ERR_BAD_ARG;
        }
        return led_reply_ex(c, k, prof, prof ? k->a[ARG0 + 1u] : 0u);
    }
    uint8_t v[KM_CONFIG_LED_BYTES] = { 0 };
    const km_cfg_status_t r = cb_status(c->led->get(c->led->ctx, v));
    if (r != KM_CFG_OK) {
        return r;
    }
    memcpy(k->pl, v, sizeof v);
    k->pn = sizeof v;
    return KM_CFG_OK;
}

/* LED_SET: six light bytes (RAM only), or nine (the extended form: + idle seconds and flags). The backend validates them and either
 * applies all or none. Payload = the values now in force (6 or 12 bytes, as LED_GET in the same form). */
static km_cfg_status_t op_led_set(km_config_t *c, call_t *k)
{
    const bool prof = k->len == ARG0 + KM_CONFIG_LED_SET_PROF_BYTES;
    const bool ex = k->len == ARG0 + KM_CONFIG_LED_SET_EX_BYTES || prof;
    if (k->len != ARG0 + KM_CONFIG_LED_BYTES && !ex) {
        return KM_CFG_ERR_BAD_LENGTH;
    }
    if (ex && c->led != NULL && (c->led_ex == NULL || c->led_ex->get == NULL || c->led_ex->set == NULL || (prof && !led_prof_ops(c)))) {
        return KM_CFG_ERR_BAD_LENGTH;                /* as the old firmware answered */
    }
    if (c->led == NULL || c->led->get == NULL || c->led->set == NULL) {
        return KM_CFG_ERR_UNSUPPORTED;
    }
    if (ex) {
        if ((k->a[ARG0 + 8u] & (uint8_t)~KM_CFG_LED_FLAG_SAVE) != 0u) {
            return KM_CFG_ERR_BAD_ARG;               /* unknown flag bits: nothing is applied */
        }
        if (prof && !led_prof_arg_ok(c, k->a[ARG0 + 9u])) {
            return KM_CFG_ERR_BAD_ARG;
        }
        const uint8_t pf = prof ? k->a[ARG0 + 9u] : 0u;
        const km_cfg_status_t r = cb_status(prof ? c->led_ex->set_p(c->led_ex->ctx, pf, k->a + ARG0) : c->led_ex->set(c->led_ex->ctx, k->a + ARG0));
        if (r != KM_CFG_OK) {
            return r;
        }
        return led_reply_ex(c, k, prof, pf);
    }
    const km_cfg_status_t r = cb_status(c->led->set(c->led->ctx, k->a + ARG0));
    if (r != KM_CFG_OK) {
        return r;
    }
    uint8_t v[KM_CONFIG_LED_BYTES] = { 0 };
    if (cb_status(c->led->get(c->led->ctx, v)) != KM_CFG_OK) {
        return KM_CFG_ERR_FAILED;
    }
    memcpy(k->pl, v, sizeof v);
    k->pn = sizeof v;
    return KM_CFG_OK;
}

bool km_config_handle(km_config_t *c, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    uint8_t d[KM_HID_MAX_DATA];
    call_t k;

    memset(d, 0, sizeof d);
    k.a = req->data;
    k.len = req->len;
    k.pl = d + PAYLOAD0;
    k.pn = 0;
    d[0] = req->len >= 1 ? req->data[0] : 0;
    d[1] = req->len >= 2 ? req->data[1] : 0;

    km_cfg_status_t st;
    if (req->len < ARG0) {
        st = KM_CFG_ERR_BAD_LENGTH;
    } else if (req->offset != 0 || req->xor_key != 0) {
        st = KM_CFG_ERR_BAD_ARG;                  /* the offset field and the XOR key are not used by this protocol */
    } else {
        switch (req->data[0]) {
        case KM_CFG_OP_GET_INFO:                 st = op_get_info(c, &k); break;
        case KM_CFG_OP_GET_SETTINGS:             st = op_get_settings(c, &k); break;
        case KM_CFG_OP_SET_BEGIN:                st = op_set_begin(c, &k); break;
        case KM_CFG_OP_SET_PAGE:                 st = op_set_page(c, &k); break;
        case KM_CFG_OP_COMMIT:                   st = op_commit(c, &k); break;
        case KM_CFG_OP_SELECT_PROFILE:           st = op_select_profile(c, &k); break;
        case KM_CFG_OP_RECALIBRATE_REQUEST:
            st = op_calibration(c, &k, KM_CFG_CONFIRM_RECALIBRATE, c->ops->recalibrate);
            break;
        case KM_CFG_OP_SAVE_CALIBRATION_REQUEST:
            st = op_calibration(c, &k, KM_CFG_CONFIRM_SAVE_CAL, c->ops->save_calibration);
            break;
        case KM_CFG_OP_LIVE_DEPTH:               st = op_live_depth(c, &k); break;
        case KM_CFG_OP_LED_GET:                  st = op_led_get(c, &k); break;
        case KM_CFG_OP_LED_SET:                  st = op_led_set(c, &k); break;
        case KM_CFG_OP_DEVICE_STATUS:            st = op_device_status(c, &k); break;
        case KM_CFG_OP_SCENE_GET:                st = op_scene_get(c, &k); break;
        case KM_CFG_OP_SCENE_BEGIN:              st = op_scene_begin(c, &k); break;
        case KM_CFG_OP_SCENE_PAGE:               st = op_scene_page(c, &k); break;
        case KM_CFG_OP_SCENE_COMMIT:             st = op_scene_commit(c, &k); break;
        default:                                 st = KM_CFG_ERR_UNKNOWN_OP; break;
        }
    }
    d[2] = (uint8_t)st;
    return km_hid_build_response(out, KM_HID_RESP_OK, KM_CMD_CONFIG, 0, d, (uint8_t)(PAYLOAD0 + k.pn)) == KM_HID_OK;
}
