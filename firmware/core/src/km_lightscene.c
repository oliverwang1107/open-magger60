/*
 * km_lightscene.c - the light scene blob "KMLS" and its storage, see km_lightscene.h. Integer math only, no allocation, no waiting.
 */
#include "km/km_lightscene.h"

#include <string.h>

#include "km/km_settings.h"      /* km_crc32 */

_Static_assert(KM_LS_MAX_ENCODED == KM_LS_HEADER_BYTES + (4u + 1u + KM_LS_MAX_RULES * KM_LS_RULE_BYTES) + (4u + KM_LS_KEYS_BYTES) +
                                    (4u + KM_LS_ANIM_BYTES) + KM_LS_CRC_BYTES, "the documented maximum size");
_Static_assert(KM_LS_MAX_DECODE <= KM_STORE_MAX_PAYLOAD, "a blob must fit one store record");

#define LAYERS_MAX    4u       /* KM_LAYERS */
#define PROFILES_MAX  4u       /* KM_PROFILES */
#define COLOR_DEFAULT_R 0u
#define COLOR_DEFAULT_G 150u
#define COLOR_DEFAULT_B 255u

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void km_lightscene_default(km_lightscene_t *s)
{
    memset(s, 0, sizeof *s);
    s->palette[0].r = COLOR_DEFAULT_R;
    s->palette[0].g = COLOR_DEFAULT_G;
    s->palette[0].b = COLOR_DEFAULT_B;
}

bool km_ls_rule_valid(const km_ls_rule_t *r)
{
    if (r->source < KM_LS_SRC_HOST_NUM || r->source > KM_LS_SRC_LAST) {
        return false;
    }
    const unsigned param_max = (r->source == KM_LS_SRC_LAYER) ? LAYERS_MAX : (r->source == KM_LS_SRC_PROFILE) ? PROFILES_MAX : 1u;
    if (r->param >= param_max || r->led >= KM_LED_COUNT || (r->flags & ~KM_LS_FLAG_MASK) != 0u) {
        return false;
    }
    const unsigned off = r->flags & KM_LS_FLAG_OFF_MASK;
    if (off > KM_LS_OFF_DIM) {
        return false;
    }
    return off == KM_LS_OFF_DIM || r->dim == 0u;
}

bool km_lightscene_valid(const km_lightscene_t *s)
{
    if (s->n_rules > KM_LS_MAX_RULES || (s->anim_flags & ~KM_LS_ANIM_MASK) != 0u ||
        s->anim_offset_ms < -KM_LS_ANIM_OFFSET_MAX || s->anim_offset_ms > KM_LS_ANIM_OFFSET_MAX) {
        return false;
    }
    for (unsigned i = 0; i < s->n_rules; i++) {
        if (!km_ls_rule_valid(&s->rule[i])) {
            return false;
        }
    }
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        if (s->key_idx[i] >= KM_LS_PALETTE) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- encode */

unsigned km_lightscene_encode(const km_lightscene_t *s, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || s == NULL || !km_lightscene_valid(s)) {
        return 0;
    }
    const unsigned rules_len = 1u + s->n_rules * KM_LS_RULE_BYTES;
    const unsigned total = KM_LS_HEADER_BYTES + (4u + rules_len) + (4u + KM_LS_KEYS_BYTES) + (4u + KM_LS_ANIM_BYTES) + KM_LS_CRC_BYTES;
    if (cap < total) {
        return 0;
    }
    memset(buf, 0, total);
    memcpy(buf, "KMLS", 4);
    put16(buf + 4, KM_LS_VERSION);
    put16(buf + 6, total);
    buf[8] = 3;
    unsigned o = KM_LS_HEADER_BYTES;

    buf[o] = KM_LS_SEC_RULES;
    put16(buf + o + 2, rules_len);
    o += 4;
    buf[o++] = s->n_rules;
    for (unsigned i = 0; i < s->n_rules; i++) {
        const km_ls_rule_t *r = &s->rule[i];
        buf[o + 0] = r->source;
        buf[o + 1] = r->param;
        buf[o + 2] = r->led;
        buf[o + 3] = r->flags;
        buf[o + 4] = r->on.r;
        buf[o + 5] = r->on.g;
        buf[o + 6] = r->on.b;
        buf[o + 7] = r->dim;
        o += KM_LS_RULE_BYTES;
    }

    buf[o] = KM_LS_SEC_KEYS;
    put16(buf + o + 2, KM_LS_KEYS_BYTES);
    o += 4;
    for (unsigned i = 0; i < KM_LS_PALETTE; i++) {
        buf[o++] = s->palette[i].r;
        buf[o++] = s->palette[i].g;
        buf[o++] = s->palette[i].b;
    }
    memcpy(buf + o, s->key_idx, KM_LED_COUNT);
    o += KM_LED_COUNT;

    buf[o] = KM_LS_SEC_ANIM;
    put16(buf + o + 2, KM_LS_ANIM_BYTES);
    o += 4;
    buf[o + 0] = s->anim_flags;
    buf[o + 1] = s->anim_clip;
    put32(buf + o + 2, (uint32_t)s->anim_offset_ms);
    o += KM_LS_ANIM_BYTES;

    put32(buf + o, km_crc32(0, buf, o));
    return total;
}

/* ---------------------------------------------------------------- decode */

static bool parse_rules(km_lightscene_t *t, const uint8_t *p, unsigned len)
{
    if (len < 1u || p[0] > KM_LS_MAX_RULES || len != 1u + p[0] * KM_LS_RULE_BYTES) {
        return false;
    }
    t->n_rules = p[0];
    for (unsigned i = 0; i < t->n_rules; i++) {
        const uint8_t *q = p + 1u + i * KM_LS_RULE_BYTES;
        km_ls_rule_t *r = &t->rule[i];
        r->source = q[0];
        r->param = q[1];
        r->led = q[2];
        r->flags = q[3];
        r->on.r = q[4];
        r->on.g = q[5];
        r->on.b = q[6];
        r->dim = q[7];
        if (!km_ls_rule_valid(r)) {
            return false;
        }
    }
    return true;
}

static bool parse_keys(km_lightscene_t *t, const uint8_t *p, unsigned len)
{
    if (len != KM_LS_KEYS_BYTES) {
        return false;
    }
    for (unsigned i = 0; i < KM_LS_PALETTE; i++) {
        t->palette[i].r = p[3u * i];
        t->palette[i].g = p[3u * i + 1u];
        t->palette[i].b = p[3u * i + 2u];
    }
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        t->key_idx[i] = p[3u * KM_LS_PALETTE + i];
        if (t->key_idx[i] >= KM_LS_PALETTE) {
            return false;
        }
    }
    return true;
}

static bool parse_anim(km_lightscene_t *t, const uint8_t *p, unsigned len)
{
    if (len != KM_LS_ANIM_BYTES || (p[0] & ~KM_LS_ANIM_MASK) != 0u || get16(p + 6) != 0u) {
        return false;
    }
    const int32_t off = (int32_t)get32(p + 2);
    if (off < -KM_LS_ANIM_OFFSET_MAX || off > KM_LS_ANIM_OFFSET_MAX) {
        return false;
    }
    t->anim_flags = p[0];
    t->anim_clip = p[1];
    t->anim_offset_ms = off;
    return true;
}

bool km_lightscene_decode(km_lightscene_t *s, const uint8_t *buf, unsigned len)
{
    if (s == NULL || buf == NULL || len < KM_LS_HEADER_BYTES + KM_LS_CRC_BYTES || len > KM_LS_MAX_DECODE ||
        memcmp(buf, "KMLS", 4) != 0 || get16(buf + 4) != KM_LS_VERSION || get16(buf + 6) != len || buf[8] > KM_LS_MAX_RULES) {
        return false;
    }
    for (unsigned i = 9; i < KM_LS_HEADER_BYTES; i++) {
        if (buf[i] != 0u) {
            return false;
        }
    }
    if (km_crc32(0, buf, len - KM_LS_CRC_BYTES) != get32(buf + len - KM_LS_CRC_BYTES)) {
        return false;
    }

    km_lightscene_t t;
    km_lightscene_default(&t);
    const unsigned end = len - KM_LS_CRC_BYTES;
    unsigned o = KM_LS_HEADER_BYTES;
    unsigned prev = 0;                                    /* ids start at 1, so "strictly increasing" starts above 0 */
    for (unsigned n = 0; n < buf[8]; n++) {
        if (end - o < 4u) {
            return false;
        }
        const unsigned id = buf[o], flags = buf[o + 1], plen = get16(buf + o + 2);
        o += 4;
        if (flags != 0u || id <= prev || plen > end - o) {
            return false;
        }
        prev = id;
        bool ok = true;
        switch (id) {
        case KM_LS_SEC_RULES: ok = parse_rules(&t, buf + o, plen); break;
        case KM_LS_SEC_KEYS:  ok = parse_keys(&t, buf + o, plen); break;
        case KM_LS_SEC_ANIM:  ok = parse_anim(&t, buf + o, plen); break;
        default:              ok = id >= 0x80u; break;       /* an unknown required section refuses the blob, an optional one is skipped */
        }
        if (!ok) {
            return false;
        }
        o += plen;
    }
    if (o != end) {
        return false;                                         /* bytes between the last section and the checksum */
    }
    *s = t;
    return true;
}

/* ---------------------------------------------------------------- storage */

bool km_lightscene_load(const km_store_t *st, km_lightscene_t *s)
{
    uint8_t buf[KM_LS_MAX_DECODE];
    uint32_t len = 0;
    if (st == NULL || s == NULL || !km_store_read(st, buf, sizeof buf, &len)) {
        return false;
    }
    return km_lightscene_decode(s, buf, len);
}

km_store_result_t km_lightscene_save(const km_store_t *st, const km_lightscene_t *s)
{
    uint8_t buf[KM_LS_MAX_ENCODED];
    if (st == NULL || s == NULL) {
        return KM_STORE_ERR_ARG;
    }
    const unsigned n = km_lightscene_encode(s, buf, sizeof buf);
    return n == 0u ? KM_STORE_ERR_ARG : km_store_write(st, buf, n);
}
