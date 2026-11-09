/* km_settings: format, round trip, and refusal of everything that is not exactly a blob we wrote. */
#include <stdlib.h>

#include "km/km_settings.h"
#include "km/km_store.h"
#include "km_test.h"

#define SZ KM_SETTINGS_ENCODED_SIZE
#define PROFILE_OFF(p)   (KM_SETTINGS_HEADER_BYTES + (p) * KM_SETTINGS_PROFILE_BYTES)
#define CFG_OFF(p, s)    (PROFILE_OFF(p) + (s) * KM_SETTINGS_KEYCFG_BYTES)
#define OPTS_OFF(p)      (PROFILE_OFF(p) + KM_KEYMAP_SLOTS * KM_SETTINGS_KEYCFG_BYTES)
#define LAYER_OFF(p, l, s) (OPTS_OFF(p) + KM_SETTINGS_OPTS_BYTES + (l) * KM_KEYMAP_SLOTS + (s))
#define ARG_OFF(p, l, s, i) (OPTS_OFF(p) + KM_SETTINGS_OPTS_BYTES + KM_LAYERS * KM_KEYMAP_SLOTS + ((l) * KM_KEYMAP_SLOTS + (s)) * KM_KEYMAP_ARGS + (i))

static uint32_t g_rng = 12345u;

static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static void fix_crc(uint8_t *b)
{
    const uint32_t c = km_crc32(0, b, SZ - 4);
    b[SZ - 4] = (uint8_t)c;
    b[SZ - 3] = (uint8_t)(c >> 8);
    b[SZ - 2] = (uint8_t)(c >> 16);
    b[SZ - 1] = (uint8_t)(c >> 24);
}

static bool cfg_eq(const km_engine_key_cfg_t *a, const km_engine_key_cfg_t *b)
{
    return a->press_mm100 == b->press_mm100 && a->release_mm100 == b->release_mm100 && a->mode == b->mode &&
           a->rt_down_mm100 == b->rt_down_mm100 && a->rt_up_mm100 == b->rt_up_mm100 &&
           a->top_dead_mm100 == b->top_dead_mm100 && a->bottom_dead_mm100 == b->bottom_dead_mm100 &&
           a->continuous == b->continuous;
}

static bool settings_eq(const km_settings_t *a, const km_settings_t *b)
{
    if (a->version != b->version || a->active != b->active) {
        return false;
    }
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            if (!cfg_eq(&a->profile[p].key[s], &b->profile[p].key[s])) {
                return false;
            }
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                if (a->profile[p].layer_usage[l][s] != b->profile[p].layer_usage[l][s] ||
                    a->profile[p].layer_arg[l][s][0] != b->profile[p].layer_arg[l][s][0] ||
                    a->profile[p].layer_arg[l][s][1] != b->profile[p].layer_arg[l][s][1]) {
                    return false;
                }
            }
        }
        if (a->profile[p].layer_opts.tap_term_ms != b->profile[p].layer_opts.tap_term_ms ||
            a->profile[p].layer_opts.oneshot_ms != b->profile[p].layer_opts.oneshot_ms ||
            a->profile[p].layer_opts.tap_policy != b->profile[p].layer_opts.tap_policy) {
            return false;
        }
    }
    return true;
}

static uint8_t rnd_usage(void)
{
    static const uint8_t extra[] = { 0, 0x04, 0x1D, 0xE0, 0xE7, 0xF0, 0xF1, 0xFA, 0xFB, 0xFC, 0xFF, 0x2C };
    return (rnd() & 1u) ? extra[rnd() % sizeof extra] : (uint8_t)(0x04u + rnd() % (0xE7u - 0x04u + 1u));
}

/* A random but valid settings object. */
void km_test_random_settings(km_settings_t *s, uint32_t seed);
void km_test_random_settings(km_settings_t *s, uint32_t seed)
{
    g_rng = seed;
    km_settings_default(s);
    s->active = (uint8_t)(rnd() % KM_PROFILES);
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        for (unsigned sl = 0; sl < KM_KEYMAP_SLOTS; sl++) {
            km_engine_key_cfg_t *c = &s->profile[p].key[sl];
            for (unsigned tries = 0; tries < 200u; tries++) {   /* the engine owns the rule: draw until it accepts */
                c->press_mm100 = (uint16_t)(30u + rnd() % 300u);
                c->release_mm100 = (uint16_t)(10u + rnd() % (c->press_mm100 - 10u));
                c->mode = (uint8_t)(rnd() & 1u);
                c->continuous = (uint8_t)(rnd() & 1u);
                c->rt_down_mm100 = (uint16_t)(10u + rnd() % 200u);
                c->rt_up_mm100 = (uint16_t)(10u + rnd() % 200u);
                c->top_dead_mm100 = (uint16_t)(rnd() % 100u);
                c->bottom_dead_mm100 = (uint16_t)(rnd() % 100u);
                if (km_engine_cfg_valid(c)) {
                    break;
                }
                km_engine_key_cfg_default(c);
            }
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                s->profile[p].layer_usage[l][sl] = rnd_usage();
                s->profile[p].layer_arg[l][sl][0] = s->profile[p].layer_arg[l][sl][1] = 0;
                if (rnd() % 7u == 0) {                          /* some extended entries with valid arguments */
                    uint8_t *code = &s->profile[p].layer_usage[l][sl];
                    uint8_t *a = s->profile[p].layer_arg[l][sl];
                    switch (rnd() % 3u) {
                    case 0:
                        *code = KM_CODE_TOGGLE;
                        a[0] = (uint8_t)(1u + rnd() % 3u);
                        break;
                    case 1:
                        *code = KM_CODE_TAP_HOLD;
                        a[0] = (rnd() & 1u) ? (uint8_t)(0x04u + rnd() % 0xE4u) : (uint8_t)(KM_CODE_VOL_DOWN + rnd() % 6u);
                        a[1] = (rnd() & 1u) ? (uint8_t)(0xE0u + rnd() % 8u) : (uint8_t)((uint8_t[]){ KM_CODE_HOLD_L1, KM_CODE_HOLD_L2, KM_CODE_HOLD_L3 }[rnd() % 3u]);
                        break;
                    default:
                        *code = KM_CODE_ONESHOT_MOD;
                        a[0] = (uint8_t)(0xE0u + rnd() % 8u);
                        break;
                    }
                }
            }
        }
        s->profile[p].layer_opts.tap_term_ms = (uint16_t)(KM_LAYER_TAP_TERM_MIN + rnd() % 900u);
        s->profile[p].layer_opts.oneshot_ms = (rnd() & 1u) ? 0u : (uint16_t)(KM_LAYER_ONESHOT_MIN + rnd() % 5000u);
        s->profile[p].layer_opts.tap_policy = (uint8_t)(rnd() % 3u);
    }
}

static void reject_after_fix(const uint8_t *good, unsigned off, uint8_t val, const char *what, int line)
{
    uint8_t b[SZ];
    km_settings_t s;
    km_settings_default(&s);
    memcpy(b, good, SZ);
    b[off] = val;
    fix_crc(b);
    if (km_settings_decode(&s, b, SZ)) {
        printf("  FAIL test_settings.c:%d: accepted %s\n", line, what);
        km_test_failures++;
    }
    km_test_checks++;
}
#define REJECT(good, off, val, what) reject_after_fix(good, off, val, what, __LINE__)

/* ================================================================ settings format version 3 (M6 task P0) ================================================================ */

#define V3MAX KM_SETTINGS_V3_MAX_SIZE

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fix_crc_n(uint8_t *b, unsigned n)
{
    const uint32_t c = km_crc32(0, b, n - 4u);
    b[n - 4] = (uint8_t)c;
    b[n - 3] = (uint8_t)(c >> 8);
    b[n - 2] = (uint8_t)(c >> 16);
    b[n - 1] = (uint8_t)(c >> 24);
}

/* payload offset of section (id, scope) in a version 3 blob, 0 when there is none (a payload never starts at 0) */
static unsigned find_sec(const uint8_t *b, unsigned len, uint8_t id, uint8_t scope)
{
    unsigned p = 16;
    for (unsigned i = 0; i < b[12] && p + 4 <= len; i++) {
        if (b[p] == id && b[p + 1] == scope) {
            return p + 4;
        }
        p += 4u + rd16(b + p + 2);
    }
    return 0;
}

static unsigned sec_len(const uint8_t *b, unsigned len, uint8_t id, uint8_t scope)
{
    const unsigned o = find_sec(b, len, id, scope);
    return o ? rd16(b + o - 2) : 0;
}

/* the settings a version 3 round trip must give back: key slots as they were, slots without a key as the factory default */
static bool v3_eq(const km_settings_t *a, const km_settings_t *b)
{
    uint8_t ks[KM_KEYMAP_KEYS];
    km_settings_t def;
    bool iskey[KM_KEYMAP_SLOTS] = { false };
    CHECK_EQ(km_settings_key_slots(ks), KM_KEYMAP_KEYS);
    for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
        iskey[ks[i]] = true;
    }
    km_settings_default(&def);
    if (a->active != b->active || b->version != KM_SETTINGS_VERSION || memcmp(&a->device, &b->device, sizeof a->device) != 0) {
        return false;
    }
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        const km_profile_t *x = &a->profile[p], *y = &b->profile[p];
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            const km_profile_t *ref = iskey[s] ? x : &def.profile[p];
            if (!cfg_eq(&ref->key[s], &y->key[s])) {
                return false;
            }
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                if (ref->layer_usage[l][s] != y->layer_usage[l][s] || ref->layer_arg[l][s][0] != y->layer_arg[l][s][0] ||
                    ref->layer_arg[l][s][1] != y->layer_arg[l][s][1]) {
                    return false;
                }
            }
        }
        if (memcmp(&x->layer_opts, &y->layer_opts, sizeof x->layer_opts) != 0 || x->dks_n != y->dks_n || x->morph_n != y->morph_n ||
            x->socd_n != y->socd_n || memcmp(x->dks, y->dks, x->dks_n * sizeof x->dks[0]) != 0 ||
            memcmp(x->morph, y->morph, x->morph_n * sizeof x->morph[0]) != 0 || memcmp(x->socd, y->socd, x->socd_n * sizeof x->socd[0]) != 0 ||
            memcmp(x->pad_curve, y->pad_curve, sizeof x->pad_curve) != 0 || memcmp(&x->pad_opts, &y->pad_opts, sizeof x->pad_opts) != 0) {
            return false;
        }
    }
    return true;
}

static void random_dks(km_dks_rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->t_act_mm100 = (uint16_t)(KM_DKS_ACT_MIN_MM100 + rnd() % 230u);
    r->t_bot_mm100 = (rnd() & 1u) ? 0u : (uint16_t)(r->t_act_mm100 + KM_DKS_GAP_MIN_MM100 + rnd() % (340u - r->t_act_mm100 - KM_DKS_GAP_MIN_MM100 + 1u));
    r->flags = (uint8_t)(rnd() & 1u);
    for (unsigned a = 0; a < 4u; a++) {
        if (rnd() % 4u != 0u) {
            r->act[a].usage = (uint8_t)(0x04u + rnd() % 0xE4u);
            r->act[a].ev = (uint8_t)rnd();
        }
    }
}

/* A random valid settings object that uses version 3 features (some of the time), on top of the version 2 random object. */
static void random_v3(km_settings_t *s, uint32_t seed)
{
    uint8_t ks[KM_KEYMAP_KEYS];
    km_test_random_settings(s, seed);
    km_settings_key_slots(ks);
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        km_profile_t *q = &s->profile[p];
        q->dks_n = (uint8_t)(rnd() % (KM_DKS_MAX + 1u));
        for (unsigned i = 0; i < q->dks_n; i++) {
            random_dks(&q->dks[i]);
        }
        q->morph_n = (uint8_t)(rnd() % (KM_MORPH_MAX + 1u));
        for (unsigned i = 0; i < q->morph_n; i++) {
            q->morph[i].mod_mask = (uint8_t)(1u + rnd() % 255u);
            q->morph[i].usage = (uint8_t)(0x04u + rnd() % 0xE4u);
            q->morph[i].flags = (uint8_t)(rnd() & 1u);
        }
        q->socd_n = (uint8_t)(rnd() % (KM_SOCD_MAX + 1u));
        for (unsigned i = 0; i < q->socd_n; i++) {          /* the keys 2i and 2i+1 of a shuffled-by-offset list: disjoint pairs */
            const unsigned off = rnd() % 8u;
            q->socd[i].slot_a = ks[(2u * i + off) % KM_KEYMAP_KEYS];
            q->socd[i].slot_b = ks[(2u * i + 1u + off + 8u) % KM_KEYMAP_KEYS];
            q->socd[i].mode = (uint8_t)(rnd() % (KM_SOCD_MODE_LAST + 1u));
            q->socd[i].flags = (uint8_t)(rnd() & 1u);
        }
        for (unsigned i = 0; i < q->socd_n; i++) {         /* make the pairs disjoint whatever the offsets were */
            for (unsigned j = 0; j < i; j++) {
                if (q->socd[i].slot_a == q->socd[j].slot_a || q->socd[i].slot_a == q->socd[j].slot_b || q->socd[i].slot_b == q->socd[j].slot_a ||
                    q->socd[i].slot_b == q->socd[j].slot_b || q->socd[i].slot_a == q->socd[i].slot_b) {
                    q->socd_n = (uint8_t)i;
                    break;
                }
            }
        }
        if (rnd() & 1u) {
            for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
                q->pad_curve[i].start_mm100_5 = (uint8_t)(KM_PAD_CURVE_START_MIN + rnd() % 20u);
                q->pad_curve[i].end_mm100_5 = (uint8_t)(q->pad_curve[i].start_mm100_5 + KM_PAD_CURVE_SPAN_MIN + rnd() % (KM_PAD_CURVE_END_MAX - q->pad_curve[i].start_mm100_5 - KM_PAD_CURVE_SPAN_MIN + 1u));
                q->pad_curve[i].shape = (uint8_t)(rnd() % (KM_PAD_SHAPE_LAST + 1u));
                q->pad_curve[i].flags = (uint8_t)(rnd() & 1u);
            }
            q->pad_opts.stick_deadzone = (uint8_t)(rnd() % (KM_PAD_DEADZONE_MAX + 1u));
            q->pad_opts.circular = (uint8_t)(rnd() & 1u);
            q->pad_opts.smooth = (uint8_t)(rnd() % (KM_PAD_SMOOTH_MAX + 1u));
        }
        for (unsigned k = 0; k < 24u; k++) {                  /* new codes on random key slots */
            const unsigned l = rnd() % KM_LAYERS, sl = ks[rnd() % KM_KEYMAP_KEYS];
            uint8_t *c = &q->layer_usage[l][sl], *a = q->layer_arg[l][sl];
            const uint8_t u = (uint8_t)(0x04u + rnd() % 0xE4u);
            a[0] = a[1] = 0;
            *c = 0;
            switch (rnd() % 8u) {
            case 0: *c = KM_CODE_TOGGLE_KEY; a[0] = u; break;
            case 1: if (q->morph_n) { *c = KM_CODE_MOD_MORPH; a[0] = u; a[1] = (uint8_t)(rnd() % q->morph_n); } break;
            case 2: if (q->dks_n) { *c = KM_CODE_DKS; a[0] = (uint8_t)(rnd() % q->dks_n); } break;
            case 3: *c = KM_CODE_PAD_BTN; a[0] = (uint8_t)(1u + rnd() % 20u); a[1] = (rnd() & 1u) ? 0u : (uint8_t)(1u + rnd() % 68u); break;
            case 4: {
                const unsigned axis = rnd() % 6u;
                *c = KM_CODE_PAD_AXIS;
                a[0] = KM_PAD_AXIS_ARG(axis, axis < 4u && (rnd() & 1u), rnd() & 1u);
                a[1] = (uint8_t)(rnd() % KM_PAD_CURVES);
                break;
            }
            case 5: *c = KM_CODE_LIGHT; a[0] = (uint8_t)(rnd() % (KM_LIGHT_ACT_LAST + 1u)); break;
            case 6: *c = KM_CODE_TAP_HOLD; a[0] = u; a[1] = (uint8_t)(u == 0x05 ? 0x06 : 0x05); break;
            default: break;
            }
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {            /* the version 2 random object may hold entries that the new records now break: none do, but be sure */
            for (unsigned sl = 0; sl < KM_KEYMAP_SLOTS; sl++) {
                if (q->layer_usage[l][sl] == KM_CODE_TAP_HOLD && !km_keymap_entry_valid(KM_CODE_TAP_HOLD, q->layer_arg[l][sl][0], q->layer_arg[l][sl][1])) {
                    q->layer_usage[l][sl] = 0;
                    q->layer_arg[l][sl][0] = q->layer_arg[l][sl][1] = 0;
                }
            }
        }
    }
    if (rnd() % 3u != 0u) {
        s->device.pad_mode = (uint8_t)(rnd() % (KM_PAD_MODE_LAST + 1u));
        s->device.filter_mode = (uint8_t)(rnd() % (KM_FILTER_MODE_LAST + 1u));
    }
}

/* every section present in every profile, every list full */
static void full_v3(km_settings_t *s)
{
    uint8_t ks[KM_KEYMAP_KEYS];
    km_settings_default(s);
    km_settings_key_slots(ks);
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        km_profile_t *q = &s->profile[p];
        q->dks_n = KM_DKS_MAX;
        for (unsigned i = 0; i < KM_DKS_MAX; i++) {
            q->dks[i].t_act_mm100 = (uint16_t)(60 + 10 * i);
            q->dks[i].t_bot_mm100 = (uint16_t)(200 + 10 * i);
            q->dks[i].act[0].usage = (uint8_t)(0x04 + i);
            q->dks[i].act[0].ev = (uint8_t)(KM_DKS_EV_SET(0, KM_DKS_EV_PRESS) | KM_DKS_EV_SET(3, KM_DKS_EV_RELEASE));
        }
        q->morph_n = KM_MORPH_MAX;
        for (unsigned i = 0; i < KM_MORPH_MAX; i++) {
            q->morph[i].mod_mask = (uint8_t)(1u << i);
            q->morph[i].usage = (uint8_t)(0x04 + i);
        }
        q->socd_n = KM_SOCD_MAX;
        for (unsigned i = 0; i < KM_SOCD_MAX; i++) {
            q->socd[i].slot_a = ks[2 * i];
            q->socd[i].slot_b = ks[2 * i + 1];
            q->socd[i].mode = (uint8_t)(1 + i);
        }
        q->pad_curve[1].shape = KM_PAD_SHAPE_S;
        q->pad_opts.stick_deadzone = 7;
        q->layer_usage[0][ks[0]] = KM_CODE_DKS;
        q->layer_arg[0][ks[0]][0] = 7;
        q->layer_usage[1][ks[3]] = KM_CODE_MOD_MORPH;
        q->layer_arg[1][ks[3]][0] = 0x04;
        q->layer_arg[1][ks[3]][1] = 7;
    }
    s->device.pad_mode = KM_PAD_MODE_HID;
    s->device.filter_mode = KM_FILTER_MODE_QUIET;
}

/* ---- a tiny builder for blobs that the writer would never produce ---- */

typedef struct {
    uint8_t  b[V3MAX + 64];
    unsigned n, nsec;
} blob_t;

static blob_t g_blob;

static void blob_begin(blob_t *x, uint8_t active)
{
    memset(x, 0, sizeof *x);
    memcpy(x->b, "KMCS", 4);
    x->b[4] = 3;
    x->b[8] = active;
    x->b[9] = 4;
    x->b[10] = 4;
    x->b[11] = 61;
    x->n = 16;
}

static void blob_sec(blob_t *x, uint8_t id, uint8_t scope, const uint8_t *pl, unsigned len)
{
    x->b[x->n] = id;
    x->b[x->n + 1] = scope;
    wr16(x->b + x->n + 2, len);
    if (len) {
        memcpy(x->b + x->n + 4, pl, len);
    }
    x->n += 4u + len;
    x->nsec++;
}

static unsigned blob_end(blob_t *x)
{
    x->b[12] = (uint8_t)x->nsec;
    x->n += 4;
    wr16(x->b + 6, x->n);
    fix_crc_n(x->b, x->n);
    return x->n;
}

/* copy section (id, scope) of a good blob into the builder */
static void blob_copy(blob_t *x, const uint8_t *good, unsigned len, uint8_t id, uint8_t scope)
{
    const unsigned o = find_sec(good, len, id, scope);
    CHECK(o != 0);
    if (o) {
        blob_sec(x, id, scope, good + o, rd16(good + o - 2));
    }
}

/* ---- an independent statement of the entry rules, written from the plan / header text, not from km_keymap.c ---- */

static bool spec_usage(unsigned a)
{
    return a >= 0x04 && a <= 0xE7;
}

static bool spec_v2(unsigned c, unsigned a0, unsigned a1)
{
    const bool plain = c == 0 || (c >= 0x04 && c <= 0xE7) || c >= 0xF0;
    if (plain) {
        return a0 == 0 && a1 == 0;
    }
    if (c == 0xE8) {
        return a0 >= 1 && a0 <= 3 && a1 == 0;
    }
    if (c == 0xE9) {
        const bool tap = spec_usage(a0) || (a0 >= 0xF5 && a0 <= 0xFA);
        const bool hold = a1 == 0xF0 || a1 == 0xFB || a1 == 0xFC || (a1 >= 0xE0 && a1 <= 0xE7);
        return tap && hold;
    }
    if (c == 0xEA) {
        return a0 >= 0xE0 && a0 <= 0xE7 && a1 == 0;
    }
    return false;
}

static bool spec_v3(unsigned c, unsigned a0, unsigned a1)
{
    switch (c) {
    case 0xE9: {
        const bool tap = spec_usage(a0) || (a0 >= 0xF5 && a0 <= 0xFA);
        const bool hold = a1 == 0xF0 || a1 == 0xFB || a1 == 0xFC || (a1 >= 0xE0 && a1 <= 0xE7) || (spec_usage(a1) && a1 != a0);
        return tap && hold;
    }
    case 0xEB: return spec_usage(a0) && a1 == 0;
    case 0xEC: return spec_usage(a0) && a1 <= 7;
    case 0xED: return a0 <= 7 && a1 == 0;
    case 0xEE: return a0 >= 1 && a0 <= 20 && a1 <= 68;
    case 0xEF: {
        const unsigned axis = a0 & 7u;
        const bool neg = (a0 & 8u) != 0;
        return (a0 & 0xE0u) == 0 && axis <= 5 && !(neg && axis >= 4) && a1 <= 3;
    }
    case 0xFD: return a0 <= 7 && a1 == 0;
    default: return spec_v2(c, a0, a1);
    }
}

static void test_entry_rules_exhaustive(void)
{
    unsigned valid3 = 0, only3 = 0;
    for (unsigned c = 0; c < 256; c++) {
        for (unsigned a0 = 0; a0 < 256; a0++) {
            for (unsigned a1 = 0; a1 < 256; a1++) {
                const bool v3 = spec_v3(c, a0, a1), v2 = spec_v2(c, a0, a1);
                if (km_keymap_entry_valid((uint8_t)c, (uint8_t)a0, (uint8_t)a1) != v3 ||
                    km_keymap_entry_valid_v2((uint8_t)c, (uint8_t)a0, (uint8_t)a1) != v2 ||
                    km_keymap_entry_needs_v3((uint8_t)c, (uint8_t)a0, (uint8_t)a1) != (v3 && !v2) ||
                    km_settings_entry_valid((uint8_t)c, (uint8_t)a0, (uint8_t)a1) != v3) {
                    CHECK(false);
                    printf("    entry %02x %02x %02x: spec v3=%d v2=%d\n", c, a0, a1, v3, v2);
                    return;
                }
                valid3 += v3;
                only3 += v3 && !v2;
            }
        }
    }
    km_test_checks++;
    CHECK(valid3 > 1000u && only3 > 100u);          /* the loop really saw both kinds */
    /* everything valid in version 2 is still valid, nothing valid before is lost */
    for (unsigned c = 0; c < 256; c++) {
        CHECK_EQ(km_keymap_code_valid((uint8_t)c), km_keymap_code_valid_v2((uint8_t)c) || (c >= 0xEB && c <= 0xEF));
    }
}

/* ---- the record rules, boundary by boundary ---- */

static km_dks_rec_t good_dks(void)
{
    km_dks_rec_t r;
    memset(&r, 0, sizeof r);
    r.t_act_mm100 = KM_DKS_ACT_DEFAULT_MM100;
    r.t_bot_mm100 = 0;
    r.act[0].usage = 0x04;
    r.act[0].ev = KM_DKS_EV_SET(0, KM_DKS_EV_PRESS);
    return r;
}

static void test_record_rules(void)
{
    km_dks_rec_t d = good_dks();
    CHECK(km_settings_dks_valid(&d));
    CHECK(!km_settings_dks_valid(NULL));
    d.t_act_mm100 = 39;      CHECK(!km_settings_dks_valid(&d));
    d.t_act_mm100 = 40;      CHECK(km_settings_dks_valid(&d));
    d.t_act_mm100 = 270;     CHECK(km_settings_dks_valid(&d));          /* t_bot 0 = 310: exactly 40 above */
    d.t_act_mm100 = 271;     CHECK(!km_settings_dks_valid(&d));
    d.t_act_mm100 = 120; d.t_bot_mm100 = 159;   CHECK(!km_settings_dks_valid(&d));
    d.t_bot_mm100 = 160;     CHECK(km_settings_dks_valid(&d));
    d.t_bot_mm100 = 340;     CHECK(km_settings_dks_valid(&d));
    d.t_bot_mm100 = 341;     CHECK(!km_settings_dks_valid(&d));
    d.t_act_mm100 = 340; d.t_bot_mm100 = 340;   CHECK(!km_settings_dks_valid(&d));
    d = good_dks();
    d.flags = KM_DKS_FLAG_RETRIGGER;  CHECK(km_settings_dks_valid(&d));
    d.flags = 2;                      CHECK(!km_settings_dks_valid(&d));
    d.flags = 0x80;                   CHECK(!km_settings_dks_valid(&d));
    d = good_dks(); d.rsvd[0] = 1;    CHECK(!km_settings_dks_valid(&d));
    d = good_dks(); d.rsvd[2] = 1;    CHECK(!km_settings_dks_valid(&d));
    for (unsigned a = 0; a < 4; a++) {
        d = good_dks();
        d.act[a].usage = 0x04;  d.act[a].ev = 0xFF;      CHECK(km_settings_dks_valid(&d));
        d.act[a].usage = 0xE7;                           CHECK(km_settings_dks_valid(&d));
        d.act[a].usage = 0xE8;                           CHECK(!km_settings_dks_valid(&d));
        d.act[a].usage = 0x03;                           CHECK(!km_settings_dks_valid(&d));
        d.act[a].usage = 0xFF;                           CHECK(!km_settings_dks_valid(&d));
        d.act[a].usage = 0;  d.act[a].ev = 1;            CHECK(!km_settings_dks_valid(&d));        /* an unused action has no events */
        d.act[a].ev = 0;                                 CHECK(km_settings_dks_valid(&d));
    }
    CHECK_EQ(KM_DKS_EV_GET(KM_DKS_EV_SET(2, KM_DKS_EV_TAP), 2), KM_DKS_EV_TAP);
    CHECK_EQ(KM_DKS_EV_GET(KM_DKS_EV_SET(2, KM_DKS_EV_TAP), 1), KM_DKS_EV_NONE);

    km_morph_rec_t m = { 0x02, 0x4C, 0, 0 };
    CHECK(km_settings_morph_valid(&m) && !km_settings_morph_valid(NULL));
    m.mod_mask = 0;        CHECK(!km_settings_morph_valid(&m));
    m.mod_mask = 0xFF;     CHECK(km_settings_morph_valid(&m));
    m.usage = 0x03;        CHECK(!km_settings_morph_valid(&m));
    m.usage = 0xE8;        CHECK(!km_settings_morph_valid(&m));
    m.usage = 0xE7;        CHECK(km_settings_morph_valid(&m));
    m.flags = 1;           CHECK(km_settings_morph_valid(&m));
    m.flags = 2;           CHECK(!km_settings_morph_valid(&m));
    m.flags = 0; m.rsvd = 1; CHECK(!km_settings_morph_valid(&m));

    uint8_t ks[KM_KEYMAP_KEYS];
    km_settings_key_slots(ks);
    CHECK_EQ(km_settings_key_slots(ks), 61);
    for (unsigned i = 1; i < KM_KEYMAP_KEYS; i++) {
        CHECK(ks[i] > ks[i - 1] && km_keymap_slot_is_key(ks[i]));
    }
    km_socd_pair_t sp = { ks[0], ks[1], KM_SOCD_MODE_LAST_WINS, 0 };
    CHECK(km_settings_socd_valid(&sp) && !km_settings_socd_valid(NULL));
    sp.slot_b = sp.slot_a;                                            CHECK(!km_settings_socd_valid(&sp));
    sp.slot_b = ks[1]; sp.slot_a = 15;                                CHECK(!km_settings_socd_valid(&sp));      /* slot 15 has no key */
    sp.slot_a = ks[0]; sp.slot_b = 76;                                CHECK(!km_settings_socd_valid(&sp));
    sp.slot_b = 255;                                                  CHECK(!km_settings_socd_valid(&sp));
    sp.slot_b = ks[1]; sp.mode = KM_SOCD_MODE_DEEPER_WINS;            CHECK(km_settings_socd_valid(&sp));
    sp.mode = KM_SOCD_MODE_LAST + 1;                                  CHECK(!km_settings_socd_valid(&sp));
    sp.mode = KM_SOCD_MODE_OFF;                                       CHECK(km_settings_socd_valid(&sp));
    sp.flags = KM_SOCD_FLAG_LAYER0_ONLY;                              CHECK(km_settings_socd_valid(&sp));
    sp.flags = 2;                                                     CHECK(!km_settings_socd_valid(&sp));

    km_pad_curve_t c = { 4, 64, KM_PAD_SHAPE_LINEAR, 0 };
    CHECK(km_settings_pad_curve_valid(&c) && !km_settings_pad_curve_valid(NULL));
    c.start_mm100_5 = 3;                      CHECK(!km_settings_pad_curve_valid(&c));
    c.start_mm100_5 = 64; c.end_mm100_5 = 68; CHECK(km_settings_pad_curve_valid(&c));
    c.start_mm100_5 = 65;                     CHECK(!km_settings_pad_curve_valid(&c));       /* span 3 < 4 */
    c.start_mm100_5 = 4; c.end_mm100_5 = 69;  CHECK(!km_settings_pad_curve_valid(&c));
    c.end_mm100_5 = 8;                        CHECK(km_settings_pad_curve_valid(&c));
    c.end_mm100_5 = 7;                        CHECK(!km_settings_pad_curve_valid(&c));
    c.end_mm100_5 = 64; c.shape = KM_PAD_SHAPE_S;  CHECK(km_settings_pad_curve_valid(&c));
    c.shape = 4;                              CHECK(!km_settings_pad_curve_valid(&c));
    c.shape = 0; c.flags = KM_PAD_FLAG_REVERSE;    CHECK(km_settings_pad_curve_valid(&c));
    c.flags = 2;                              CHECK(!km_settings_pad_curve_valid(&c));
    c.flags = 0; c.start_mm100_5 = 0; c.end_mm100_5 = 0;  CHECK(!km_settings_pad_curve_valid(&c));

    km_pad_curve_t dc[KM_PAD_CURVES];
    km_pad_opts_t po;
    km_settings_pad_default(dc, &po);
    CHECK(km_settings_pad_opts_valid(&po) && !km_settings_pad_opts_valid(NULL));
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        CHECK(km_settings_pad_curve_valid(&dc[i]));
    }
    po.stick_deadzone = 50;  CHECK(km_settings_pad_opts_valid(&po));
    po.stick_deadzone = 51;  CHECK(!km_settings_pad_opts_valid(&po));
    po.stick_deadzone = 0; po.circular = 2;  CHECK(!km_settings_pad_opts_valid(&po));
    po.circular = 0; po.smooth = 3;          CHECK(km_settings_pad_opts_valid(&po));
    po.smooth = 4;                           CHECK(!km_settings_pad_opts_valid(&po));
    po.smooth = 0; po.rsvd[4] = 1;           CHECK(!km_settings_pad_opts_valid(&po));

    km_device_opts_t dv;
    memset(&dv, 0, sizeof dv);
    CHECK(km_settings_device_valid(&dv) && !km_settings_device_valid(NULL));
    dv.pad_mode = KM_PAD_MODE_XINPUT;  CHECK(km_settings_device_valid(&dv));
    dv.pad_mode = 3;                   CHECK(!km_settings_device_valid(&dv));
    dv.pad_mode = 0; dv.filter_mode = KM_FILTER_MODE_QUIET;  CHECK(km_settings_device_valid(&dv));
    dv.filter_mode = 3;                CHECK(!km_settings_device_valid(&dv));
    dv.filter_mode = 0; dv.rsvd[13] = 1;  CHECK(!km_settings_device_valid(&dv));

    /* whole-object rules: counts, references to records, SOCD slots shared between pairs */
    km_settings_t s;
    km_settings_default(&s);
    s.profile[1].layer_usage[0][ks[5]] = KM_CODE_DKS;                 /* index 0 but no DKS record */
    CHECK(!km_settings_valid(&s));
    CHECK_EQ(km_settings_encode(&s, g_blob.b, sizeof g_blob.b), 0);
    s.profile[1].dks_n = 1;
    s.profile[1].dks[0] = good_dks();
    CHECK(km_settings_valid(&s));
    s.profile[1].layer_arg[0][ks[5]][0] = 1;                          /* index 1 but one record */
    CHECK(!km_settings_valid(&s));
    s.profile[1].layer_arg[0][ks[5]][0] = 0;
    s.profile[1].layer_usage[3][ks[6]] = KM_CODE_MOD_MORPH;
    s.profile[1].layer_arg[3][ks[6]][0] = 0x04;
    CHECK(!km_settings_valid(&s));                                    /* no Mod-Morph record */
    s.profile[1].morph_n = 1;
    s.profile[1].morph[0] = (km_morph_rec_t){ 0x02, 0x4C, 0, 0 };
    CHECK(km_settings_valid(&s));
    s.profile[1].layer_arg[3][ks[6]][1] = 1;
    CHECK(!km_settings_valid(&s));
    s.profile[1].layer_arg[3][ks[6]][1] = 0;
    s.profile[1].dks_n = KM_DKS_MAX + 1u;  CHECK(!km_settings_valid(&s));
    s.profile[1].dks_n = 1;
    s.profile[1].morph_n = KM_MORPH_MAX + 1u;  CHECK(!km_settings_valid(&s));
    s.profile[1].morph_n = 1;
    s.profile[2].socd_n = 2;
    s.profile[2].socd[0] = (km_socd_pair_t){ ks[0], ks[1], 1, 0 };
    s.profile[2].socd[1] = (km_socd_pair_t){ ks[2], ks[3], 2, 0 };
    CHECK(km_settings_valid(&s));
    s.profile[2].socd[1].slot_a = ks[1];                              /* a slot in two pairs */
    CHECK(!km_settings_valid(&s));
    s.profile[2].socd[1].slot_a = ks[0];
    CHECK(!km_settings_valid(&s));
    s.profile[2].socd[1].slot_a = ks[2];
    s.profile[2].socd_n = KM_SOCD_MAX + 1u;  CHECK(!km_settings_valid(&s));
    s.profile[2].socd_n = 2;
    s.profile[3].pad_curve[3].shape = 9;  CHECK(!km_settings_valid(&s));
    s.profile[3].pad_curve[3].shape = 0;
    s.device.pad_mode = 5;  CHECK(!km_settings_valid(&s));
    s.device.pad_mode = 1;
    CHECK(km_settings_valid(&s) && km_settings_needs_v3(&s));
}

/* ---- tests ---- */

static void check_roundtrip_v3(const km_settings_t *s, unsigned want_size)
{
    static uint8_t b[V3MAX + 64], b2[V3MAX + 64];
    static km_settings_t back;
    CHECK(km_settings_valid(s));
    const unsigned n = km_settings_encode(s, b, sizeof b);
    CHECK_EQ(n, km_settings_encoded_size(s));
    if (want_size) {
        CHECK_EQ(n, want_size);
    }
    CHECK(n > 0 && n <= KM_SETTINGS_V3_MAX_ENCODED);
    if (n == 0) {
        return;
    }
    CHECK_EQ(b[4], 3);
    CHECK_EQ(rd16(b + 6), n);
    CHECK(km_settings_decode(&back, b, n));
    CHECK(v3_eq(s, &back));
    CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), n);              /* canonical: the same bytes again */
    CHECK(memcmp(b, b2, n) == 0);
    CHECK_EQ(km_settings_encode_v2(s, b2, sizeof b2), 0);               /* version 2 cannot hold it */
}

static void test_v3_basic(void)
{
    static uint8_t b[V3MAX + 64];
    static km_settings_t s, back;

    /* the numbers of the format */
    CHECK_EQ(KM_SETTINGS_V3_KEYCFG_BYTES, 854);
    CHECK_EQ(KM_SETTINGS_V3_LAYERS_BYTES, 738);
    CHECK_EQ(KM_SETTINGS_V3_MAX_ENCODED, 7308);
    CHECK_EQ(KM_SETTINGS_V3_MAX_SIZE, 8176);
    CHECK(KM_SETTINGS_V3_MAX_ENCODED <= KM_SETTINGS_V3_MAX_SIZE);

    /* a settings object that uses no new feature is written as version 2, byte for byte as before */
    km_settings_default(&s);
    CHECK(!km_settings_needs_v3(&s));
    CHECK_EQ(km_settings_encoded_size(&s), KM_SETTINGS_ENCODED_SIZE);
    CHECK_EQ(km_settings_encode(&s, b, sizeof b), KM_SETTINGS_ENCODED_SIZE);
    CHECK_EQ(b[4], 2);
    CHECK_EQ(km_settings_encode_v2(&s, b, sizeof b), KM_SETTINGS_ENCODED_SIZE);

    /* ... but the section writer can write it too: header and size are exactly the documented ones, and nothing is lost */
    const unsigned plain = 16 + 4 + 4 * (4 + 854 + 4 + 738);
    CHECK_EQ(plain, 6420);
    CHECK_EQ(km_settings_encode_v3(&s, b, sizeof b), plain);
    CHECK(b[0] == 'K' && b[1] == 'M' && b[2] == 'C' && b[3] == 'S');
    CHECK(b[4] == 3 && b[5] == 0);
    CHECK_EQ(rd16(b + 6), plain);
    CHECK(b[8] == 0 && b[9] == 4 && b[10] == 4 && b[11] == 61 && b[12] == 8 && b[13] == 0 && b[14] == 0 && b[15] == 0);
    CHECK(b[16] == 0x01 && b[17] == 0 && rd16(b + 18) == 854);                   /* profile 0 KEYCFG first */
    CHECK(b[16 + 4 + 854] == 0x02 && b[16 + 4 + 854 + 1] == 0 && rd16(b + 16 + 4 + 854 + 2) == 738);
    CHECK_EQ(find_sec(b, plain, 0x01, 3), 16 + 3 * 1600 + 4);
    CHECK(km_settings_decode(&back, b, plain));
    CHECK(v3_eq(&s, &back));
    CHECK_EQ(km_settings_encode_v3(&s, b, plain - 1), 0);                         /* a short buffer */
    CHECK_EQ(km_settings_encode_v3(&s, NULL, plain), 0);
    CHECK_EQ(km_settings_encode(&s, b, KM_SETTINGS_ENCODED_SIZE - 1), 0);

    /* the layout of the first key: slot order of the keys, 14 bytes each; the first key slot is 0 (LSHIFT) */
    {
        uint8_t ks[KM_KEYMAP_KEYS];
        km_settings_default(&s);
        km_settings_key_slots(ks);
        s.profile[2].key[ks[5]].press_mm100 = 200;
        s.profile[2].key[ks[5]].release_mm100 = 150;
        s.profile[2].layer_usage[3][ks[60]] = 0x1D;
        CHECK_EQ(km_settings_encode_v3(&s, b, sizeof b), plain);
        const unsigned kc = find_sec(b, plain, 0x01, 2), ly = find_sec(b, plain, 0x02, 2);
        CHECK_EQ(rd16(b + kc + 5 * 14), 200);
        CHECK_EQ(rd16(b + kc + 5 * 14 + 2), 150);
        CHECK_EQ(b[ly + 6 + 3 * 61 + 60], 0x1D);                                  /* layer 3, key 60 */
        CHECK_EQ(ks[0], 0);
    }

    /* each feature on its own makes the object need version 3, and costs exactly its section */
    typedef void (*mut_fn)(km_settings_t *);
    struct { const char *what; unsigned extra; } dummy;
    (void)dummy;
#define FEATURE(what_, extra_, ...)                                                \
    do {                                                                               \
        uint8_t ks_[KM_KEYMAP_KEYS];                                                   \
        km_settings_default(&s);                                                       \
        km_settings_key_slots(ks_);                                                    \
        __VA_ARGS__;                                                                   \
        CHECK(km_settings_valid(&s));                                                  \
        CHECK(km_settings_needs_v3(&s));                                               \
        check_roundtrip_v3(&s, plain + (extra_));                                      \
        (void)what_;                                                                   \
    } while (0)
    FEATURE("one DKS record", 4 + 1 + 16, { s.profile[0].dks_n = 1; s.profile[0].dks[0] = good_dks(); });
    FEATURE("one DKS record and a key that uses it", 4 + 1 + 16, { s.profile[0].dks_n = 1; s.profile[0].dks[0] = good_dks();
                                                                  s.profile[0].layer_usage[0][ks_[3]] = KM_CODE_DKS; });
    FEATURE("8 DKS records", 4 + 1 + 128, { s.profile[3].dks_n = 8; for (unsigned i = 0; i < 8; i++) s.profile[3].dks[i] = good_dks(); });
    FEATURE("one Mod-Morph record", 4 + 4, { s.profile[1].morph_n = 1; s.profile[1].morph[0] = (km_morph_rec_t){ 2, 0x4C, 1, 0 }; });
    FEATURE("a key that morphs", 4 + 4, { s.profile[1].morph_n = 1; s.profile[1].morph[0] = (km_morph_rec_t){ 2, 0x4C, 1, 0 };
                                         s.profile[1].layer_usage[2][ks_[9]] = KM_CODE_MOD_MORPH; s.profile[1].layer_arg[2][ks_[9]][0] = 0x2A; });
    FEATURE("two SOCD pairs", 4 + 8, { s.profile[2].socd_n = 2; s.profile[2].socd[0] = (km_socd_pair_t){ ks_[1], ks_[2], 1, 0 };
                                       s.profile[2].socd[1] = (km_socd_pair_t){ ks_[3], ks_[4], 4, 1 }; });
    FEATURE("a changed gamepad curve", 4 + 24, { s.profile[0].pad_curve[2].shape = KM_PAD_SHAPE_FAST_SLOW; });
    FEATURE("a changed gamepad option", 4 + 24, { s.profile[3].pad_opts.smooth = 2; });
    FEATURE("gamepad on", 4 + 16, { s.device.pad_mode = KM_PAD_MODE_HID; });
    FEATURE("filter mode", 4 + 16, { s.device.filter_mode = KM_FILTER_MODE_LOW_LATENCY; });
    FEATURE("toggle key", 0, { s.profile[0].layer_usage[1][ks_[7]] = KM_CODE_TOGGLE_KEY; s.profile[0].layer_arg[1][ks_[7]][0] = 0x39; });
    FEATURE("gamepad button", 0, { s.profile[0].layer_usage[0][ks_[8]] = KM_CODE_PAD_BTN; s.profile[0].layer_arg[0][ks_[8]][0] = KM_PAD_HAT_LEFT; });
    FEATURE("gamepad axis", 0, { s.profile[0].layer_usage[3][ks_[8]] = KM_CODE_PAD_AXIS;
                                 s.profile[0].layer_arg[3][ks_[8]][0] = KM_PAD_AXIS_ARG(KM_PAD_AXIS_RY, true, true); s.profile[0].layer_arg[3][ks_[8]][1] = 3; });
    FEATURE("light action", 0, { s.profile[2].layer_usage[0][ks_[20]] = KM_CODE_LIGHT; s.profile[2].layer_arg[0][ks_[20]][0] = KM_LIGHT_ACT_TOGGLE; });
    FEATURE("tap-hold with an ordinary key as the hold action", 0, { s.profile[1].layer_usage[0][ks_[21]] = KM_CODE_TAP_HOLD;
                                                                     s.profile[1].layer_arg[0][ks_[21]][0] = 0x2C; s.profile[1].layer_arg[0][ks_[21]][1] = 0x05; });
    FEATURE("the PAD section in every profile", 4 * (4 + 24), { for (unsigned p = 0; p < 4; p++) s.profile[p].pad_opts.stick_deadzone = 0; });
#undef FEATURE
    (void)sizeof(mut_fn);

    /* things that look like a feature but are not: they stay version 2 */
    km_settings_default(&s);
    s.profile[0].layer_usage[0][3] = KM_CODE_LIGHT;            /* action 0 = the old reserved meaning */
    CHECK(km_settings_valid(&s) && !km_settings_needs_v3(&s));
    CHECK_EQ(km_settings_encode(&s, b, sizeof b), KM_SETTINGS_ENCODED_SIZE);
    CHECK_EQ(b[4], 2);
    km_settings_default(&s);
    s.profile[0].layer_usage[0][15] = KM_CODE_TAP_HOLD;        /* a version 3 entry on a slot without a key still forces version 3 (a version 2 blob stores it) */
    s.profile[0].layer_arg[0][15][0] = 0x2C;
    s.profile[0].layer_arg[0][15][1] = 0x05;
    CHECK(km_settings_valid(&s) && km_settings_needs_v3(&s));
    CHECK(km_settings_encode(&s, b, sizeof b) > 0);
    CHECK(km_settings_decode(&back, b, rd16(b + 6)));
    CHECK_EQ(back.profile[0].layer_usage[0][15], 0);           /* ... and the slot itself comes back as the default */

    /* the biggest blob there is */
    full_v3(&s);
    check_roundtrip_v3(&s, KM_SETTINGS_V3_MAX_ENCODED);
    CHECK(KM_SETTINGS_V3_MAX_ENCODED <= KM_STORE_MAX_PAYLOAD);

    /* slots without a key are not stored in version 3 (and are kept by version 2) */
    {
        uint8_t ks[KM_KEYMAP_KEYS];
        km_settings_key_slots(ks);
        km_settings_default(&s);
        s.profile[0].key[15].press_mm100 = 222;                      /* slot 15 has no key */
        s.profile[0].layer_usage[2][15] = 0x1D;
        s.profile[0].key[ks[0]].press_mm100 = 222;
        CHECK(!km_settings_needs_v3(&s));
        CHECK_EQ(km_settings_encode(&s, b, sizeof b), KM_SETTINGS_ENCODED_SIZE);
        CHECK(km_settings_decode(&back, b, KM_SETTINGS_ENCODED_SIZE));
        CHECK_EQ(back.profile[0].key[15].press_mm100, 222);          /* version 2 keeps it */
        CHECK_EQ(back.profile[0].layer_usage[2][15], 0x1D);
        s.device.pad_mode = 1;
        CHECK(km_settings_encode(&s, b, sizeof b) > 0);
        CHECK(km_settings_decode(&back, b, rd16(b + 6)));
        CHECK_EQ(back.profile[0].key[15].press_mm100, 120);          /* version 3 drops it: the default */
        CHECK_EQ(back.profile[0].layer_usage[2][15], 0);
        CHECK_EQ(back.profile[0].key[ks[0]].press_mm100, 222);
    }

    /* the settings that version 2 firmware wrote and the version 1 migration still produce what they did, and need no version 3 */
    for (uint32_t seed = 1; seed <= 20; seed++) {
        km_test_random_settings(&s, seed * 31u);
        CHECK(!km_settings_needs_v3(&s));
        CHECK_EQ(km_settings_encode(&s, b, sizeof b), KM_SETTINGS_ENCODED_SIZE);
        CHECK_EQ(b[4], 2);
        static uint8_t b3[V3MAX + 64];
        const unsigned n3 = km_settings_encode_v3(&s, b3, sizeof b3);   /* version 3 holds the same key data */
        CHECK(n3 > 0 && n3 < KM_SETTINGS_ENCODED_SIZE);
        CHECK(km_settings_decode(&back, b3, n3));
        CHECK(v3_eq(&s, &back));
        km_settings_t from2;
        CHECK(km_settings_decode(&from2, b, KM_SETTINGS_ENCODED_SIZE));
        CHECK(v3_eq(&from2, &back));                                    /* version 2 and version 3 of the same keys agree */
    }
}

static void test_v3_random(void)
{
    static km_settings_t s;
    unsigned uses_v3 = 0, v2_only = 0;
    for (uint32_t seed = 1; seed <= 400; seed++) {
        random_v3(&s, seed * 104729u);
        CHECK(km_settings_valid(&s));
        if (km_settings_needs_v3(&s)) {
            uses_v3++;
            check_roundtrip_v3(&s, 0);
        } else {
            v2_only++;
        }
    }
    CHECK(uses_v3 > 300 && v2_only < 100);
}

/* reject helper: patch a copy of `good`, repair the CRC, the blob must be refused */
static uint8_t g_good[V3MAX + 64];
static unsigned g_good_len;

static void reject3(unsigned off, unsigned val, const char *what, int line)
{
    static uint8_t b[V3MAX + 64];
    static km_settings_t s;
    memcpy(b, g_good, g_good_len);
    b[off] = (uint8_t)val;
    fix_crc_n(b, g_good_len);
    km_test_checks++;
    if (km_settings_decode(&s, b, g_good_len)) {
        printf("  FAIL test_settings.c:%d: accepted %s\n", line, what);
        km_test_failures++;
    }
}
#define REJECT3(off, val, what) reject3((off), (val), (what), __LINE__)

/* a patch that must be ACCEPTED (so the REJECT tests are not refused for some other reason) */
static void accept3(unsigned off, unsigned val, const char *what, int line)
{
    static uint8_t b[V3MAX + 64];
    static km_settings_t s;
    memcpy(b, g_good, g_good_len);
    b[off] = (uint8_t)val;
    fix_crc_n(b, g_good_len);
    km_test_checks++;
    if (!km_settings_decode(&s, b, g_good_len)) {
        printf("  FAIL test_settings.c:%d: refused %s\n", line, what);
        km_test_failures++;
    }
}
#define ACCEPT3(off, val, what) accept3((off), (val), (what), __LINE__)

static void test_v3_strict_decode(void)
{
    static km_settings_t s, back;
    uint8_t ks[KM_KEYMAP_KEYS];
    km_settings_key_slots(ks);

    /* the blob under attack: every section in every profile (profile 0 has DKS 8, MORPH 8, SOCD 4, PAD, device) */
    full_v3(&s);
    s.profile[0].layer_usage[2][ks[10]] = KM_CODE_PAD_AXIS;
    s.profile[0].layer_arg[2][ks[10]][0] = KM_PAD_AXIS_ARG(KM_PAD_AXIS_LX, true, false);
    g_good_len = km_settings_encode(&s, g_good, sizeof g_good);
    CHECK_EQ(g_good_len, KM_SETTINGS_V3_MAX_ENCODED);
    CHECK(km_settings_decode(&back, g_good, g_good_len));
    const unsigned dks0 = find_sec(g_good, g_good_len, 0x10, 0), mor0 = find_sec(g_good, g_good_len, 0x11, 0);
    const unsigned soc0 = find_sec(g_good, g_good_len, 0x12, 0), pad0 = find_sec(g_good, g_good_len, 0x20, 0);
    const unsigned kc0 = find_sec(g_good, g_good_len, 0x01, 0), ly0 = find_sec(g_good, g_good_len, 0x02, 0);
    const unsigned dev = find_sec(g_good, g_good_len, 0x30, 0xFF);
    CHECK(dks0 && mor0 && soc0 && pad0 && kc0 && ly0 && dev);
    CHECK_EQ(sec_len(g_good, g_good_len, 0x10, 0), 129);
    CHECK_EQ(sec_len(g_good, g_good_len, 0x11, 0), 32);
    CHECK_EQ(sec_len(g_good, g_good_len, 0x12, 0), 16);
    CHECK_EQ(sec_len(g_good, g_good_len, 0x20, 0), 24);
    CHECK_EQ(sec_len(g_good, g_good_len, 0x30, 0xFF), 16);
    CHECK_EQ(g_good[12], 4 * 6 + 1);

    /* header */
    REJECT3(0, 'X', "wrong magic");
    REJECT3(4, 4, "version 4");
    REJECT3(4, 2, "version 2 label on a version 3 body");
    REJECT3(4, 1, "version 1 label on a version 3 body");
    REJECT3(4, 0, "version 0");
    REJECT3(5, 1, "version 259");
    REJECT3(6, g_good_len - 1 - 256 * 0, "length field short (low byte)");
    REJECT3(7, (g_good_len >> 8) + 1, "length field long (high byte)");
    REJECT3(8, 4, "active profile 4");
    REJECT3(8, 255, "active profile 255");
    ACCEPT3(8, 3, "active profile 3 is fine");
    REJECT3(9, 3, "3 profiles");
    REJECT3(9, 5, "5 profiles");
    REJECT3(10, 3, "3 layers");
    REJECT3(10, 5, "5 layers");
    REJECT3(11, 60, "60 keys");
    REJECT3(11, 62, "62 keys");
    REJECT3(11, 76, "76 slots (a version 2 header on a version 3 body)");
    REJECT3(12, 0, "no sections declared but sections present");
    REJECT3(12, g_good[12] - 1u, "one section fewer declared than present");
    REJECT3(12, g_good[12] + 1u, "one section more declared than present");
    REJECT3(12, 65, "65 sections");
    REJECT3(12, 255, "255 sections");
    REJECT3(13, 1, "reserved byte 13");
    REJECT3(14, 1, "reserved byte 14");
    REJECT3(15, 0x80, "reserved byte 15");

    /* section headers: first section (profile 0 KEYCFG) is at 16 */
    ACCEPT3(18, 854 & 0xFF, "(KEYCFG length unchanged)");
    REJECT3(18, (854 & 0xFF) + 1, "KEYCFG one byte longer than the format says");
    REJECT3(18, (854 & 0xFF) - 1, "KEYCFG one byte shorter");
    REJECT3(19, 0xFF, "KEYCFG length 0xFF56: longer than the blob");
    REJECT3(16, 0x03, "unknown required section id 3 in place of KEYCFG");
    REJECT3(16, 0x40, "unknown required section id 0x40");
    REJECT3(16, 0x7F, "unknown required section id 0x7F");
    REJECT3(16, 0x00, "section id 0");
    REJECT3(16, 0x80, "KEYCFG turned into an optional unknown section: a required section is then missing");
    REJECT3(17, 4, "scope 4");
    REJECT3(17, 0xFE, "scope 0xFE");
    REJECT3(17, 0xFF, "a per-profile section with the global scope");
    REJECT3(17, 1, "profile 1 KEYCFG twice (this one moved to profile 1)");
    REJECT3(dks0 - 4, 0x11, "DKS turned into a second MORPH of profile 0");
    REJECT3(dks0 - 4, 0x12, "DKS turned into a second SOCD");
    REJECT3(dks0 - 3, 1, "DKS moved to profile 1: duplicate");
    REJECT3(dev - 3, 0, "DEVICE with profile scope");
    REJECT3(dev - 3, 3, "DEVICE with profile scope 3");
    REJECT3(dev - 4, 0x20, "DEVICE turned into a PAD section of profile 0xFF");
    REJECT3(dev - 2, 15, "DEVICE shorter than 16");
    REJECT3(dev - 2, 17, "DEVICE longer than 16 (runs into the CRC)");
    REJECT3(g_good_len - 5, g_good[g_good_len - 5] ^ 1u, "last payload byte flipped: device rsvd");

    /* section lengths with correct headers elsewhere */
    REJECT3(dks0 - 2, 128, "DKS length 128");
    REJECT3(dks0 - 2, 130, "DKS length 130");
    REJECT3(dks0 - 2, 0, "DKS length 0");
    REJECT3(mor0 - 2, 31, "MORPH length 31");
    REJECT3(mor0 - 2, 33, "MORPH length 33 (8 records and a byte)");
    REJECT3(soc0 - 2, 15, "SOCD length 15");
    REJECT3(soc0 - 2, 20, "SOCD length 20 (5 pairs)");
    REJECT3(pad0 - 2, 23, "PAD length 23");
    REJECT3(pad0 - 2, 28, "PAD length 28");
    REJECT3(ly0 - 2, 0xE1, "LAYERS length 737");

    /* DKS contents */
    REJECT3(dks0, 9, "9 DKS records");
    REJECT3(dks0, 7, "DKS count 7 but 8 records of bytes");
    REJECT3(dks0, 0, "DKS count 0 but 8 records of bytes");
    ACCEPT3(dks0 + 1 + 4, 1, "DKS re-trigger flag");
    REJECT3(dks0 + 1 + 4, 2, "DKS flags bit 1");
    REJECT3(dks0 + 1 + 5, 1, "DKS reserved byte");
    REJECT3(dks0 + 1 + 7, 1, "DKS reserved byte 2");
    REJECT3(dks0 + 1 + 0, 39, "DKS start point 39 (record 0 holds 60)");
    REJECT3(dks0 + 1 + 1, 1, "DKS start point 316");
    REJECT3(dks0 + 1 + 2, 99, "DKS bottom point below start + 40");
    REJECT3(dks0 + 1 + 3, 2, "DKS bottom point 712");
    REJECT3(dks0 + 1 + 8, 0x03, "DKS action usage 3");
    REJECT3(dks0 + 1 + 8, 0xE8, "DKS action usage 0xE8");
    REJECT3(dks0 + 1 + 8, 0x00, "DKS action 0 with events");
    REJECT3(dks0 + 1 + 11, 0x07, "DKS action 1 is empty but has events");
    ACCEPT3(dks0 + 1 + 9, 0x00, "DKS action without events");
    REJECT3(dks0 + 1 + 16 * 7 + 8 + 6, 0xFF, "DKS record 7 action 3: events without a usage");

    /* MORPH contents */
    REJECT3(mor0 + 0, 0, "Mod-Morph mask 0");
    REJECT3(mor0 + 1, 0x03, "Mod-Morph usage 3");
    REJECT3(mor0 + 1, 0xE8, "Mod-Morph usage 0xE8");
    REJECT3(mor0 + 2, 2, "Mod-Morph flags bit 1");
    ACCEPT3(mor0 + 2, 1, "Mod-Morph suppress flag");
    REJECT3(mor0 + 3, 1, "Mod-Morph reserved byte");
    REJECT3(mor0 + 4 * 7 + 3, 0x80, "Mod-Morph record 7 reserved byte");

    /* SOCD contents */
    REJECT3(soc0 + 1, g_good[soc0], "SOCD pair of one slot");
    REJECT3(soc0 + 0, 15, "SOCD slot 15 has no key");
    REJECT3(soc0 + 0, 76, "SOCD slot 76");
    REJECT3(soc0 + 1, 255, "SOCD slot 255");
    REJECT3(soc0 + 2, 5, "SOCD mode 5");
    ACCEPT3(soc0 + 2, 0, "SOCD mode off");
    REJECT3(soc0 + 3, 2, "SOCD flags bit 1");
    ACCEPT3(soc0 + 3, 1, "SOCD layer-0-only flag");
    REJECT3(soc0 + 4, g_good[soc0 + 1], "SOCD second pair shares a slot with the first");
    REJECT3(soc0 + 4 * 3 + 1, g_good[soc0], "SOCD last pair shares a slot with the first");

    /* PAD contents */
    REJECT3(pad0 + 0, 3, "pad curve start 3");
    REJECT3(pad0 + 1, 69, "pad curve end 69");
    REJECT3(pad0 + 1, 7, "pad curve end < start + 4");
    REJECT3(pad0 + 2, 4, "pad curve shape 4");
    ACCEPT3(pad0 + 2, 3, "pad curve shape 3");
    REJECT3(pad0 + 3, 2, "pad curve flags bit 1");
    REJECT3(pad0 + 4 * 3 + 2, 200, "pad curve 3 shape 200");
    REJECT3(pad0 + 16, 51, "pad dead zone 51");
    ACCEPT3(pad0 + 16, 50, "pad dead zone 50");
    REJECT3(pad0 + 17, 2, "pad circular 2");
    REJECT3(pad0 + 18, 4, "pad smooth 4");
    REJECT3(pad0 + 19, 1, "pad reserved byte");
    REJECT3(pad0 + 23, 1, "pad last reserved byte");

    /* DEVICE contents */
    REJECT3(dev + 0, 3, "pad mode 3");
    ACCEPT3(dev + 0, 2, "pad mode 2 (XInput, reserved for later, but a legal value)");
    REJECT3(dev + 1, 3, "filter mode 3");
    REJECT3(dev + 2, 1, "device reserved byte");
    REJECT3(dev + 15, 1, "device last reserved byte");

    /* KEYCFG and LAYERS contents */
    REJECT3(kc0 + 4, 2, "trigger mode 2");
    REJECT3(kc0 + 60 * 14 + 4, 0xFF, "trigger mode 255 on the last key");
    REJECT3(kc0 + 2, 120, "release == press");
    REJECT3(kc0 + 1, 2, "press over the travel");
    REJECT3(ly0 + 5, 1, "layer options reserved byte");
    REJECT3(ly0 + 4, 3, "tap policy 3");
    REJECT3(ly0 + 0, 29, "tap term 29");
    REJECT3(ly0 + 3, 0x30, "one-shot 12496 ms (over 10000)");
    REJECT3(ly0 + 6 + 0, 0x01, "key code 1 on layer 0 key 0");
    REJECT3(ly0 + 6 + 3 * 61 + 60, 0x03, "key code 3 on layer 3 key 60");
    REJECT3(ly0 + 6 + 5, 0xE8, "toggle layer without arguments");
    REJECT3(ly0 + 6 + 5, KM_CODE_PAD_BTN, "gamepad button without arguments");
    REJECT3(ly0 + 6 + 5, KM_CODE_TOGGLE_KEY, "toggle key without arguments");
    ACCEPT3(ly0 + 6 + 5, KM_CODE_LIGHT, "light action 0 (no arguments) is a plain reserved-style entry");
    REJECT3(ly0 + 6 + 244 + 2 * 5, 1, "argument on an ordinary key");
    REJECT3(ly0 + 6 + 244 + 2 * 5 + 1, 1, "argument 1 on an ordinary key");

    /* references to records */
    {
        const unsigned code_of_dks_key = ly0 + 6 + 0 * 61 + 0;               /* layer 0, key 0 = KM_CODE_DKS idx 7 (full_v3) */
        const unsigned arg0_of_dks_key = ly0 + 6 + 4 * 61 + 2 * 0;
        CHECK_EQ(g_good[code_of_dks_key], KM_CODE_DKS);
        CHECK_EQ(g_good[arg0_of_dks_key], 7);
        ACCEPT3(arg0_of_dks_key, 0, "DKS index 0");
        REJECT3(arg0_of_dks_key, 8, "DKS index 8");
        REJECT3(arg0_of_dks_key, 255, "DKS index 255");
        REJECT3(arg0_of_dks_key + 1, 1, "DKS second argument");
        const unsigned arg1_of_morph_key = ly0 + 6 + 4 * 61 + 2 * (61 + 3) + 1;   /* layer 1, key 3 */
        CHECK_EQ(g_good[ly0 + 6 + 61 + 3], KM_CODE_MOD_MORPH);
        ACCEPT3(arg1_of_morph_key, 0, "Mod-Morph index 0");
        REJECT3(arg1_of_morph_key, 8, "Mod-Morph index 8");
        REJECT3(arg1_of_morph_key - 1, 0x03, "Mod-Morph normal usage 3");
        const unsigned axis_arg = ly0 + 6 + 4 * 61 + 2 * (2 * 61 + 10);           /* layer 2, key 10 = PAD_AXIS LX- */
        CHECK_EQ(g_good[ly0 + 6 + 2 * 61 + 10], KM_CODE_PAD_AXIS);
        CHECK_EQ(g_good[axis_arg], KM_PAD_AXIS_ARG(KM_PAD_AXIS_LX, true, false));
        ACCEPT3(axis_arg, KM_PAD_AXIS_ARG(KM_PAD_AXIS_RT, false, true), "right trigger, digital");
        REJECT3(axis_arg, KM_PAD_AXIS_ARG(KM_PAD_AXIS_RT, true, false), "a trigger pushed to the negative side");
        REJECT3(axis_arg, 6, "axis 6");
        REJECT3(axis_arg, 0x20, "axis argument bit 5");
        REJECT3(axis_arg + 1, 4, "curve 4");
    }
    {
        /* the references are checked against the records OF THE SAME PROFILE: the same key with no DKS records in its profile */
        static km_settings_t t;
        full_v3(&t);
        t.profile[2].dks_n = 0;
        t.profile[2].layer_usage[0][ks[0]] = KM_CODE_DKS;
        CHECK(!km_settings_valid(&t));
        t.profile[2].dks_n = 8;
        CHECK(km_settings_valid(&t));
    }

    /* every structural truncation: shorter blob, length field and CRC made right */
    {
        static uint8_t b[V3MAX + 64];
        unsigned accepted = 0;
        for (unsigned n = 0; n < g_good_len; n++) {
            if (n < 4) {
                continue;
            }
            memcpy(b, g_good, n);
            if (n >= 8) {
                wr16(b + 6, n);
            }
            fix_crc_n(b, n);
            accepted += km_settings_decode(&back, b, n);
        }
        CHECK_EQ(accepted, 0);
    }
    /* every single-bit flip (CRC not repaired) */
    {
        static uint8_t b[V3MAX + 64];
        unsigned accepted = 0;
        for (unsigned i = 0; i < g_good_len; i++) {
            for (unsigned bit = 0; bit < 8; bit++) {
                memcpy(b, g_good, g_good_len);
                b[i] ^= (uint8_t)(1u << bit);
                accepted += km_settings_decode(&back, b, g_good_len);
            }
        }
        CHECK_EQ(accepted, 0);
    }
    /* trailing bytes, a longer buffer than the blob says */
    {
        static uint8_t b[V3MAX + 64];
        memcpy(b, g_good, g_good_len);
        memset(b + g_good_len, 0xFF, 32);
        CHECK(!km_settings_decode(&back, b, g_good_len + 1));
        CHECK(!km_settings_decode(&back, b, g_good_len + 32));
        CHECK(km_settings_decode(&back, b, g_good_len));
    }
    /* a failed decode leaves the destination as it was */
    {
        static uint8_t b[V3MAX + 64];
        km_settings_t keep;
        km_settings_default(&keep);
        keep.active = 2;
        back = keep;
        memcpy(b, g_good, g_good_len);
        b[dks0 + 1 + 5] = 1;
        fix_crc_n(b, g_good_len);
        CHECK(!km_settings_decode(&back, b, g_good_len));
        CHECK(memcmp(&back, &keep, sizeof back) == 0);
    }
}

static void test_v3_builder_cases(void)
{
    static km_settings_t s, back, back2;
    static uint8_t good[V3MAX + 64], b2[V3MAX + 64];
    full_v3(&s);
    const unsigned gl = km_settings_encode(&s, good, sizeof good);
    CHECK_EQ(gl, KM_SETTINGS_V3_MAX_ENCODED);
    const uint8_t secs[6] = { 0x01, 0x02, 0x10, 0x11, 0x12, 0x20 };

    /* the same sections in a different order are the same settings */
    blob_begin(&g_blob, s.active);
    blob_copy(&g_blob, good, gl, 0x30, 0xFF);
    for (int p = 3; p >= 0; p--) {
        for (int k = 5; k >= 0; k--) {
            blob_copy(&g_blob, good, gl, secs[k], (uint8_t)p);
        }
    }
    unsigned n = blob_end(&g_blob);
    CHECK_EQ(n, gl);
    CHECK(km_settings_decode(&back, g_blob.b, n));
    CHECK(v3_eq(&s, &back));
    CHECK(memcmp(g_blob.b, good, n) != 0);
    CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), gl);          /* written out again: the canonical order */
    CHECK(memcmp(b2, good, gl) == 0);

    /* a required section missing */
    for (unsigned p = 0; p < 4; p++) {
        for (unsigned drop = 0; drop < 2; drop++) {
            blob_begin(&g_blob, 0);
            for (unsigned q = 0; q < 4; q++) {
                for (unsigned k = 0; k < 6; k++) {
                    if (!(q == p && k == drop)) {
                        blob_copy(&g_blob, good, gl, secs[k], (uint8_t)q);
                    }
                }
            }
            n = blob_end(&g_blob);
            CHECK(!km_settings_decode(&back, g_blob.b, n));
        }
    }
    /* an optional section missing is fine and means "none" / "default" (a clean base: the references of full_v3 would dangle) */
    km_settings_default(&s);
    const unsigned cl = km_settings_encode_v3(&s, good, sizeof good);
    blob_begin(&g_blob, 1);
    for (unsigned q = 0; q < 4; q++) {
        blob_copy(&g_blob, good, cl, 0x01, (uint8_t)q);
        blob_copy(&g_blob, good, cl, 0x02, (uint8_t)q);
    }
    n = blob_end(&g_blob);
    CHECK(km_settings_decode(&back, g_blob.b, n));
    s.active = 1;
    CHECK(v3_eq(&s, &back));
    CHECK(!km_settings_needs_v3(&back));

    /* empty lists, a default PAD section and a default DEVICE section are accepted (the writer would not write them) */
    {
        uint8_t zero_dks[1] = { 0 };
        uint8_t dev[16] = { 0 };
        km_pad_curve_t dc[KM_PAD_CURVES];
        km_pad_opts_t po;
        uint8_t pad[24];
        km_settings_pad_default(dc, &po);
        for (unsigned i = 0; i < 4; i++) {
            pad[4 * i] = dc[i].start_mm100_5; pad[4 * i + 1] = dc[i].end_mm100_5; pad[4 * i + 2] = dc[i].shape; pad[4 * i + 3] = dc[i].flags;
        }
        pad[16] = po.stick_deadzone; pad[17] = po.circular; pad[18] = po.smooth;
        memset(pad + 19, 0, 5);
        blob_begin(&g_blob, 0);
        for (unsigned q = 0; q < 4; q++) {
            blob_copy(&g_blob, good, cl, 0x01, (uint8_t)q);
            blob_copy(&g_blob, good, cl, 0x02, (uint8_t)q);
        }
        blob_sec(&g_blob, 0x10, 2, zero_dks, 1);
        blob_sec(&g_blob, 0x11, 1, NULL, 0);
        blob_sec(&g_blob, 0x12, 3, NULL, 0);
        blob_sec(&g_blob, 0x20, 0, pad, 24);
        blob_sec(&g_blob, 0x30, 0xFF, dev, 16);
        n = blob_end(&g_blob);
        s.active = 0;
        CHECK(km_settings_decode(&back, g_blob.b, n));
        CHECK(v3_eq(&s, &back));
        CHECK(!km_settings_needs_v3(&back));
        CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), KM_SETTINGS_ENCODED_SIZE);       /* so it is written as version 2 again */
        pad[2] = 3;                                                                          /* a PAD section that differs is a change */
        blob_begin(&g_blob, 0);
        for (unsigned q = 0; q < 4; q++) {
            blob_copy(&g_blob, good, cl, 0x01, (uint8_t)q);
            blob_copy(&g_blob, good, cl, 0x02, (uint8_t)q);
        }
        blob_sec(&g_blob, 0x20, 3, pad, 24);
        n = blob_end(&g_blob);
        CHECK(km_settings_decode(&back, g_blob.b, n));
        CHECK_EQ(back.profile[3].pad_curve[0].shape, 3);
        CHECK(km_settings_needs_v3(&back));
    }

    /* a section one byte longer (or shorter) than its format says, with a correct structure around it: refused */
    {
        km_settings_t t;
        km_settings_default(&t);
        t.profile[0].dks_n = 1;
        t.profile[0].dks[0] = good_dks();
        t.profile[0].morph_n = 1;
        t.profile[0].morph[0] = (km_morph_rec_t){ 2, 0x4C, 0, 0 };
        t.profile[0].socd_n = 1;
        t.profile[0].socd[0] = (km_socd_pair_t){ 0, 1, 1, 0 };
        t.profile[0].pad_curve[0].shape = 2;
        t.device.pad_mode = 1;
        const unsigned tl = km_settings_encode(&t, good, sizeof good);
        CHECK(tl > 0 && good[4] == 3);
        static const struct { uint8_t id, scope; int delta; } cases[] = {
            { 0x10, 0, 0 }, { 0x10, 0, 1 }, { 0x11, 0, 0 }, { 0x11, 0, 1 }, { 0x12, 0, 0 }, { 0x12, 0, 1 }, { 0x20, 0, 0 }, { 0x20, 0, 1 }, { 0x20, 0, -1 },
            { 0x30, 0xFF, 0 }, { 0x30, 0xFF, 1 }, { 0x30, 0xFF, -1 }, { 0x01, 0, 0 }, { 0x01, 0, 1 }, { 0x01, 0, -1 }, { 0x02, 0, 0 }, { 0x02, 0, 1 }, { 0x02, 0, -1 },
        };
        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            static uint8_t pl[V3MAX];
            blob_begin(&g_blob, 0);
            for (unsigned q = 0; q < 4; q++) {
                for (unsigned k = 0; k < 6; k++) {
                    static const uint8_t ids[6] = { 0x01, 0x02, 0x10, 0x11, 0x12, 0x20 };
                    if (q == 0 && ids[k] == cases[c].id) {
                        continue;
                    }
                    if (find_sec(good, tl, ids[k], (uint8_t)q)) {
                        blob_copy(&g_blob, good, tl, ids[k], (uint8_t)q);
                    }
                }
            }
            if (cases[c].id == 0x30) {
                /* the DEVICE section is the one under test: the profile sections stay as they are */
            } else {
                blob_copy(&g_blob, good, tl, 0x30, 0xFF);
            }
            unsigned len = sec_len(good, tl, cases[c].id, cases[c].scope);
            const unsigned off = find_sec(good, tl, cases[c].id, cases[c].scope);
            CHECK(off != 0);
            memset(pl, 0, sizeof pl);
            memcpy(pl, good + off, len);
            len = (unsigned)((int)len + cases[c].delta);
            blob_sec(&g_blob, cases[c].id, cases[c].scope, pl, len);
            n = blob_end(&g_blob);
            CHECK_EQ((int)km_settings_decode(&back, g_blob.b, n), cases[c].delta == 0);
        }
    }

    /* unknown OPTIONAL sections are skipped (and not written again); unknown REQUIRED ones are refused */
    {
        uint8_t junk[300];
        for (unsigned i = 0; i < sizeof junk; i++) {
            junk[i] = (uint8_t)(i * 7u + 1u);
        }
        km_settings_default(&s);
        s.profile[2].morph_n = 1;
        s.profile[2].morph[0] = (km_morph_rec_t){ 4, 0x2A, 0, 0 };
        const unsigned sl = km_settings_encode_v3(&s, good, sizeof good);
        for (unsigned variant = 0; variant < 8; variant++) {
            blob_begin(&g_blob, 0);
            if (variant & 1u) {
                blob_sec(&g_blob, 0x80, 0xFF, junk, 300);                                    /* in front of everything */
            }
            for (unsigned q = 0; q < 4; q++) {
                blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
                if (variant & 2u) {
                    blob_sec(&g_blob, (uint8_t)(0x81 + q), (uint8_t)q, junk, q * 20u);          /* between profiles, per-profile scope */
                }
                blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
                if (q == 2) {
                    blob_copy(&g_blob, good, sl, 0x11, 2);
                }
            }
            if (variant & 4u) {
                blob_sec(&g_blob, 0xFF, 0xFF, NULL, 0);                                      /* empty, at the end */
            }
            n = blob_end(&g_blob);
            CHECK(km_settings_decode(&back, g_blob.b, n));
            CHECK(v3_eq(&s, &back));
            CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), sl);
            CHECK(memcmp(b2, good, sl) == 0);                                                 /* the unknown sections are gone */
            blob_begin(&g_blob, 0);
            for (unsigned q = 0; q < 4; q++) {
                blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
                blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
            }
            blob_sec(&g_blob, (uint8_t)(variant == 0 ? 0x03 : variant == 1 ? 0x7F : variant == 2 ? 0x31 : 0x13), variant < 4 ? 0xFF : 0, junk, 4);
            n = blob_end(&g_blob);
            CHECK(!km_settings_decode(&back, g_blob.b, n));                                   /* unknown required */
        }
        /* optional sections: same id and scope twice, scope out of range */
        blob_begin(&g_blob, 0);
        for (unsigned q = 0; q < 4; q++) {
            blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
            blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
        }
        blob_sec(&g_blob, 0x90, 0xFF, junk, 3);
        blob_sec(&g_blob, 0x90, 0xFF, junk, 3);
        n = blob_end(&g_blob);
        CHECK(!km_settings_decode(&back, g_blob.b, n));
        blob_begin(&g_blob, 0);
        for (unsigned q = 0; q < 4; q++) {
            blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
            blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
        }
        blob_sec(&g_blob, 0x90, 4, junk, 3);
        n = blob_end(&g_blob);
        CHECK(!km_settings_decode(&back, g_blob.b, n));
        blob_begin(&g_blob, 0);
        for (unsigned q = 0; q < 4; q++) {
            blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
            blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
        }
        blob_sec(&g_blob, 0x90, 0x7F, junk, 3);
        n = blob_end(&g_blob);
        CHECK(!km_settings_decode(&back, g_blob.b, n));

        /* section count limit: 64 sections are fine, 65 are not (the count byte is checked against the limit, not only against the data) */
        for (unsigned total = 63; total <= 65; total++) {
            blob_begin(&g_blob, 0);
            for (unsigned q = 0; q < 4; q++) {
                blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
                blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
            }
            blob_copy(&g_blob, good, sl, 0x11, 2);
            for (unsigned i = 0; g_blob.nsec < total; i++) {
                blob_sec(&g_blob, (uint8_t)(0x80 + i), 0xFF, NULL, 0);
            }
            n = blob_end(&g_blob);
            CHECK_EQ((int)km_settings_decode(&back, g_blob.b, n), total <= 64);
        }

        /* total size limit: exactly 8176 bytes is accepted, 8177 is not */
        for (unsigned over = 0; over <= 1; over++) {
            blob_begin(&g_blob, 0);
            for (unsigned q = 0; q < 4; q++) {
                blob_copy(&g_blob, good, sl, 0x01, (uint8_t)q);
                blob_copy(&g_blob, good, sl, 0x02, (uint8_t)q);
            }
            blob_copy(&g_blob, good, sl, 0x11, 2);
            const unsigned room = V3MAX + over - (g_blob.n + 4u + 4u);
            static uint8_t big[V3MAX + 64];
            memset(big, 0x5A, sizeof big);
            blob_sec(&g_blob, 0xA0, 0xFF, big, room);
            n = blob_end(&g_blob);
            CHECK_EQ(n, V3MAX + over);
            CHECK_EQ((int)km_settings_decode(&back, g_blob.b, n), over == 0);
            if (over == 0) {
                CHECK(v3_eq(&s, &back));
            }
        }
    }

    /* the section length field and the body disagree: a section that claims to reach past the CRC, a gap before the CRC */
    {
        km_settings_default(&s);
        const unsigned sl = km_settings_encode_v3(&s, good, sizeof good);
        memcpy(g_blob.b, good, sl);
        g_blob.n = sl;
        /* insert two zero bytes after the last section (before the CRC): the sections no longer fill the blob */
        memmove(g_blob.b + sl - 4 + 2, g_blob.b + sl - 4, 4);
        g_blob.b[sl - 4] = g_blob.b[sl - 3] = 0;
        wr16(g_blob.b + 6, sl + 2);
        fix_crc_n(g_blob.b, sl + 2);
        CHECK(!km_settings_decode(&back, g_blob.b, sl + 2));
        /* a blob that is only a header and a CRC */
        blob_begin(&g_blob, 0);
        n = blob_end(&g_blob);
        CHECK_EQ(n, 20);
        CHECK(!km_settings_decode(&back, g_blob.b, n));
        (void)back2;
    }
}

static void test_v3_fuzz(void)
{
    static uint8_t b[V3MAX + 64], b2[V3MAX + 64];
    static km_settings_t s, back, back2;
    unsigned ok = 0, bad = 0;
    for (unsigned n = 0; n < 6000; n++) {
        random_v3(&s, 50000u + n);
        if (!km_settings_needs_v3(&s)) {
            continue;
        }
        const unsigned len = km_settings_encode(&s, b, sizeof b);
        CHECK(len > 0);
        const unsigned hits = 1u + rnd() % 4u;
        for (unsigned h = 0; h < hits; h++) {
            unsigned pos;
            switch (rnd() % 4u) {
            case 0:  pos = rnd() % 16u; break;                              /* the header */
            case 1: {                                                       /* a section header */
                unsigned p = 16;
                const unsigned want = rnd() % (b[12] ? b[12] : 1u);
                for (unsigned i = 0; i < want && p + 4 < len; i++) {
                    p += 4u + rd16(b + p + 2);
                }
                pos = p + rnd() % 4u;
                break;
            }
            default: pos = rnd() % (len - 4u); break;
            }
            if (pos < len - 4u) {
                b[pos] = (rnd() & 1u) ? (uint8_t)rnd() : (uint8_t)(b[pos] ^ (1u << (rnd() % 8u)));
            }
        }
        fix_crc_n(b, len);
        if (km_settings_decode(&back, b, len)) {
            ok++;
            CHECK(km_settings_valid(&back));
            const unsigned l2 = km_settings_encode(&back, b2, sizeof b2);   /* whatever was accepted can be written and read again */
            CHECK(l2 > 0);
            CHECK(km_settings_decode(&back2, b2, l2));
            CHECK(memcmp(&back, &back2, sizeof back) == 0 || v3_eq(&back, &back2));
        } else {
            bad++;
        }
    }
    CHECK(ok > 100 && bad > 100);
    /* garbage behind a valid header and CRC */
    for (unsigned n = 0; n < 4000; n++) {
        const unsigned len = 20u + rnd() % 400u;
        for (unsigned i = 0; i < len; i++) {
            b[i] = (uint8_t)rnd();
        }
        memcpy(b, "KMCS", 4);
        b[4] = 3;
        b[5] = 0;
        wr16(b + 6, len);
        b[9] = 4;
        b[10] = 4;
        b[11] = 61;
        b[12] = (uint8_t)(rnd() % 6u);
        b[13] = b[14] = b[15] = 0;
        fix_crc_n(b, len);
        CHECK(!km_settings_decode(&back, b, len));                         /* a required section is always missing in such a blob */
    }
}

void test_settings(void)
{
    uint8_t good[SZ];
    km_settings_t s, d, back;

    /* the CRC is the standard one */
    CHECK_EQ(km_crc32(0, (const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ(km_crc32(0, (const uint8_t *)"", 0), 0);
    CHECK_EQ(km_crc32(km_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5), 0xCBF43926u);

    /* sizes and the documented layout */
    CHECK_EQ(KM_SETTINGS_PROFILE_BYTES, 76 * 14 + 6 + 4 * 76 + 4 * 76 * 2);
    CHECK_EQ(SZ, 12 + 4 * 1982 + 4);
    CHECK_EQ(SZ, 7944);
    CHECK_EQ(KM_SETTINGS_V1_SIZE, 5488);
    CHECK(SZ <= 0x2000 - 16);                       /* fits one store sector */

    /* defaults are valid and round-trip */
    km_settings_default(&d);
    CHECK(km_settings_valid(&d));
    CHECK_EQ(d.version, KM_SETTINGS_VERSION);
    CHECK_EQ(d.active, 0);
    {
        /* the default layers are the keymap's raw default table: function codes survive (FN key, media, profile keys,
           transparent), they are not turned into 'no key' */
        uint8_t table[KM_LAYERS][KM_KEYMAP_SLOTS];
        km_keymap_default_table(table);
        for (unsigned p = 0; p < KM_PROFILES; p++) {
            CHECK(memcmp(d.profile[p].layer_usage, table, sizeof table) == 0);
        }
        unsigned fn_slot = KM_KEYMAP_SLOTS, transparent = 0, funcs = 0;
        for (unsigned sl = 0; sl < KM_KEYMAP_SLOTS; sl++) {
            if (d.profile[0].layer_usage[0][sl] == KM_CODE_HOLD_L1) {
                fn_slot = sl;
            }
            for (unsigned l = 1; l < KM_LAYERS; l++) {
                transparent += d.profile[0].layer_usage[l][sl] == KM_CODE_TRANSPARENT;
            }
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                funcs += d.profile[0].layer_usage[l][sl] >= KM_CODE_FUNC_FIRST && d.profile[0].layer_usage[l][sl] != KM_CODE_TRANSPARENT;
            }
        }
        CHECK(fn_slot < KM_KEYMAP_SLOTS);               /* layer 0 has a 'hold layer 1' key */
        CHECK(transparent > 100u);                      /* layers 1..3 are mostly transparent */
        CHECK(funcs >= 5u);                             /* at least the FN key and some media / profile keys */
    }
    CHECK_EQ(km_settings_encode(&d, good, sizeof good), SZ);
    CHECK(good[0] == 'K' && good[1] == 'M' && good[2] == 'C' && good[3] == 'S');
    CHECK(good[4] == 2 && good[5] == 0);            /* version 2, little endian */
    CHECK(good[6] == (SZ & 0xFF) && good[7] == (SZ >> 8));
    CHECK(good[8] == 0 && good[9] == 4 && good[10] == 4 && good[11] == 76);
    CHECK(good[12] == 120 && good[13] == 0 && good[14] == 80 && good[15] == 0);   /* slot 0: press 1.20 mm, release 0.80 mm */
    memset(&back, 0x5A, sizeof back);
    CHECK(km_settings_decode(&back, good, SZ));
    CHECK(settings_eq(&d, &back));
    for (unsigned p = 1; p < KM_PROFILES; p++) {    /* profiles start identical */
        CHECK(memcmp(good + PROFILE_OFF(0), good + PROFILE_OFF(p), KM_SETTINGS_PROFILE_BYTES) == 0);
    }

    /* random valid settings round-trip, and the encoding is canonical (decode then encode gives the same bytes) */
    for (uint32_t seed = 1; seed <= 40; seed++) {
        uint8_t b2[SZ];
        km_test_random_settings(&s, seed * 7919u);
        CHECK(km_settings_valid(&s));
        CHECK_EQ(km_settings_encode(&s, good, sizeof good), SZ);
        CHECK(km_settings_decode(&back, good, SZ));
        CHECK(settings_eq(&s, &back));
        CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), SZ);
        CHECK(memcmp(good, b2, SZ) == 0);
    }

    /* encode refuses a short buffer and invalid settings */
    km_settings_default(&s);
    CHECK_EQ(km_settings_encode(&s, good, SZ - 1), 0);
    CHECK_EQ(km_settings_encode(&s, good, 0), 0);
    s.active = KM_PROFILES;
    CHECK_EQ(km_settings_encode(&s, good, sizeof good), 0);
    km_settings_default(&s);
    s.profile[2].key[10].release_mm100 = s.profile[2].key[10].press_mm100;
    CHECK_EQ(km_settings_encode(&s, good, sizeof good), 0);
    km_settings_default(&s);
    s.profile[3].layer_usage[3][75] = 0xE8;       /* reserved: not a keyboard usage, not a function code */
    CHECK_EQ(km_settings_encode(&s, good, sizeof good), 0);

    /* ---- refusal: foreign and damaged content ---- */
    km_settings_default(&d);
    km_test_random_settings(&s, 99);
    CHECK_EQ(km_settings_encode(&s, good, sizeof good), SZ);
    uint8_t buf[SZ + 64];

    memset(buf, 0xFF, sizeof buf);                  /* erased flash */
    CHECK(!km_settings_decode(&back, buf, SZ));
    CHECK(!km_settings_decode(&back, buf, 0x2000));
    memset(buf, 0x00, sizeof buf);                  /* zeros */
    CHECK(!km_settings_decode(&back, buf, SZ));
    CHECK(!km_settings_decode(&back, buf, 16));

    for (unsigned i = 0; i < sizeof buf; i++) {     /* "stock firmware settings": structured non-random bytes, no magic */
        buf[i] = (uint8_t)((i % 61u) * 3u + (i >> 6));
    }
    CHECK(!km_settings_decode(&back, buf, SZ));
    memset(buf, 0, sizeof buf);                     /* the stock app-ID block shape: VID/PID/version, rest erased */
    buf[0] = 0xF5; buf[1] = 0x19; buf[2] = 0x04; buf[3] = 0x01;
    memset(buf + 14, 0xFF, sizeof buf - 14);
    CHECK(!km_settings_decode(&back, buf, SZ));

    CHECK(!km_settings_decode(NULL, good, SZ));     /* argument errors */
    CHECK(!km_settings_decode(&back, NULL, SZ));

    for (unsigned n = 0; n < 2000; n++) {           /* random bytes, random lengths, some with our magic */
        for (unsigned i = 0; i < sizeof buf; i++) {
            buf[i] = (uint8_t)rnd();
        }
        if (n & 1u) {
            memcpy(buf, good, 8);                   /* right magic, version and length field, wrong everything else */
        }
        CHECK(!km_settings_decode(&back, buf, (n % 3u == 0) ? SZ : rnd() % (SZ + 32u)));
    }

    /* truncation at every length, and extra bytes */
    for (unsigned n = 0; n < SZ; n++) {
        if (km_settings_decode(&back, good, n)) {
            CHECK(false);
        }
    }
    memcpy(buf, good, SZ);
    memset(buf + SZ, 0xFF, 64);
    CHECK(!km_settings_decode(&back, buf, SZ + 1));
    CHECK(!km_settings_decode(&back, buf, SZ + 64));
    CHECK(km_settings_decode(&back, buf, SZ));      /* the right length still works with trailing flash bytes after it */

    /* every single-bit flip is caught (CRC32 guarantees it; this pins the whole path down) */
    unsigned accepted = 0;
    for (unsigned i = 0; i < SZ; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            memcpy(buf, good, SZ);
            buf[i] ^= (uint8_t)(1u << bit);
            accepted += km_settings_decode(&back, buf, SZ);
        }
    }
    CHECK_EQ(accepted, 0);

    /* a failed decode leaves the destination untouched */
    km_settings_default(&back);
    km_test_random_settings(&s, 5);
    memcpy(buf, good, SZ);
    buf[100] ^= 1;
    CHECK(!km_settings_decode(&back, buf, SZ));
    CHECK(settings_eq(&back, &d));

    /* ---- field checks with a correct CRC: validation, not the CRC, must refuse these ---- */
    km_settings_default(&s);
    CHECK_EQ(km_settings_encode(&s, good, sizeof good), SZ);
    CHECK(km_settings_decode(&back, good, SZ));
    REJECT(good, 0, 'X', "wrong magic");
    REJECT(good, 4, 0, "version 0");
    REJECT(good, 4, 3, "version 3 (unknown future version)");
    REJECT(good, 4, 1, "version 1 label on a version 2 body (the length is wrong for it)");
    REJECT(good, 4, 0xFF, "version 255");
    REJECT(good, 5, 1, "version 257");
    REJECT(good, 6, (uint8_t)(SZ & 0xFF) - 1, "length field 1 short");
    REJECT(good, 7, 0, "length field 0x00xx");
    REJECT(good, 8, KM_PROFILES, "active profile 4");
    REJECT(good, 8, 0xFF, "active profile 255");
    REJECT(good, 9, 3, "3 profiles");
    REJECT(good, 9, 5, "5 profiles");
    REJECT(good, 10, 3, "3 layers");
    REJECT(good, 11, 75, "75 slots");
    REJECT(good, CFG_OFF(0, 0) + 4, 2, "trigger mode 2");
    REJECT(good, CFG_OFF(3, 75) + 4, 0xFF, "trigger mode 255");
    REJECT(good, CFG_OFF(0, 0) + 0, 0, "press low byte 0 -> press 0");
    REJECT(good, CFG_OFF(0, 0) + 1, 2, "press 0x278 > 3.40 mm");
    REJECT(good, CFG_OFF(2, 40) + 2, 120, "release == press");
    REJECT(good, CFG_OFF(2, 40) + 2, 200, "release > press");
    REJECT(good, CFG_OFF(2, 40) + 2, 5, "release 0.05 mm (below the noise floor)");
    {
        /* The RAPID fields only count for a key in RAPID mode (the engine ignores them under FIXED), so these
           refusals use a blob whose key 1 of profile 0 is a valid RAPID key; its fields are at CFG_OFF(0, 1) + 5..13 */
        uint8_t gr[SZ];
        km_settings_t r;
        km_settings_default(&r);
        r.profile[0].key[1].mode = KM_ENGINE_TRIG_RAPID;
        r.profile[0].key[1].rt_down_mm100 = 30;
        r.profile[0].key[1].rt_up_mm100 = 30;
        r.profile[0].key[1].top_dead_mm100 = 0;
        r.profile[0].key[1].bottom_dead_mm100 = 0;
        CHECK_EQ(km_settings_encode(&r, gr, sizeof gr), SZ);
        CHECK(km_settings_decode(&back, gr, SZ));
        REJECT(gr, CFG_OFF(0, 1) + 5, 2, "continuous 2");
        REJECT(gr, CFG_OFF(0, 1) + 6, 5, "rt_down 0.05 mm (below the rapid-trigger noise floor)");
        REJECT(gr, CFG_OFF(0, 1) + 7, 2, "rt_down > travel");
        REJECT(gr, CFG_OFF(0, 1) + 8, 0, "rt_up 0");
        REJECT(gr, CFG_OFF(0, 1) + 9, 2, "rt_up > travel");
        REJECT(gr, CFG_OFF(0, 1) + 11, 2, "top dead zone > travel");
        REJECT(gr, CFG_OFF(0, 1) + 13, 2, "bottom dead zone > travel");
    }
    for (unsigned bad = 0; bad < 256; bad++) {      /* every unassigned usage byte, at the first and the last position */
        if (km_settings_usage_valid((uint8_t)bad)) {
            continue;
        }
        REJECT(good, LAYER_OFF(0, 0, 0), (uint8_t)bad, "reserved usage in layer 0");
        REJECT(good, LAYER_OFF(3, 3, 75), (uint8_t)bad, "reserved usage in the last layer slot");
    }
    CHECK(km_settings_usage_valid(0) && km_settings_usage_valid(0x04) && km_settings_usage_valid(0xE7) &&
          km_settings_usage_valid(0xF0) && km_settings_usage_valid(0xFA) && km_settings_usage_valid(0xFB) &&
          km_settings_usage_valid(0xFC) && km_settings_usage_valid(0xFF));   /* the same codes km_layers understands */
    CHECK(!km_settings_usage_valid(0x01) && !km_settings_usage_valid(0x03));
    CHECK(km_settings_usage_valid(0xEB) && km_settings_usage_valid(0xEF));      /* the M6 codes (version 3) */
    CHECK(km_settings_usage_valid(0xE8) && km_settings_usage_valid(0xE9) && km_settings_usage_valid(0xEA));   /* extended: the byte is fine, the entry needs arguments */

    /* ---- version 2 fields: extended entries, their arguments, the layer options ---- */
    {
        uint8_t gx[SZ];
        km_settings_t x;
        km_settings_default(&x);
        x.profile[1].layer_usage[2][10] = KM_CODE_TOGGLE;      x.profile[1].layer_arg[2][10][0] = 3;
        x.profile[1].layer_usage[0][18] = KM_CODE_TAP_HOLD;    x.profile[1].layer_arg[0][18][0] = 0x29;   x.profile[1].layer_arg[0][18][1] = KM_CODE_HOLD_L1;
        x.profile[2].layer_usage[0][0] = KM_CODE_ONESHOT_MOD;  x.profile[2].layer_arg[0][0][0] = 0xE1;
        x.profile[3].layer_opts.tap_term_ms = 123;
        x.profile[3].layer_opts.oneshot_ms = 0;
        x.profile[3].layer_opts.tap_policy = KM_TAP_POLICY_PERMISSIVE;
        CHECK(km_settings_valid(&x));
        CHECK_EQ(km_settings_encode(&x, gx, sizeof gx), SZ);
        CHECK_EQ(gx[ARG_OFF(1, 2, 10, 0)], 3);
        CHECK_EQ(gx[LAYER_OFF(1, 0, 18)], KM_CODE_TAP_HOLD);
        CHECK_EQ(gx[ARG_OFF(1, 0, 18, 0)], 0x29);
        CHECK_EQ(gx[ARG_OFF(1, 0, 18, 1)], KM_CODE_HOLD_L1);
        CHECK_EQ(gx[OPTS_OFF(3)], 123);
        CHECK_EQ(gx[OPTS_OFF(3) + 1], 0);
        CHECK_EQ(gx[OPTS_OFF(3) + 2], 0);                      /* oneshot_ms 0 */
        CHECK_EQ(gx[OPTS_OFF(3) + 4], KM_TAP_POLICY_PERMISSIVE);
        CHECK_EQ(gx[OPTS_OFF(3) + 5], 0);                      /* reserved */
        CHECK(km_settings_decode(&back, gx, SZ));
        CHECK(settings_eq(&x, &back));
        /* argument rules, one bad value at a time (CRC repaired) */
        REJECT(gx, ARG_OFF(1, 2, 10, 0), 0, "toggle layer 0");
        REJECT(gx, ARG_OFF(1, 2, 10, 0), 4, "toggle layer 4");
        REJECT(gx, ARG_OFF(1, 2, 10, 1), 1, "toggle with a second argument");
        REJECT(gx, ARG_OFF(1, 0, 18, 0), 0, "tap-hold tap action 0");
        REJECT(gx, ARG_OFF(1, 0, 18, 0), 0x03, "tap-hold tap action 3 (HID error code)");
        REJECT(gx, ARG_OFF(1, 0, 18, 0), KM_CODE_HOLD_L1, "tap-hold tap action = a layer hold");
        REJECT(gx, ARG_OFF(1, 0, 18, 0), KM_CODE_PROFILE_1, "tap-hold tap action = a profile key");
        REJECT(gx, ARG_OFF(1, 0, 18, 1), 0, "tap-hold hold action 0");
        REJECT(gx, ARG_OFF(1, 0, 18, 1), 0x29, "tap-hold hold action = an ordinary key");
        REJECT(gx, ARG_OFF(1, 0, 18, 1), KM_CODE_PROFILE_1, "tap-hold hold action = a profile key");
        REJECT(gx, ARG_OFF(1, 0, 18, 1), 0xFD, "tap-hold hold action = reserved");
        REJECT(gx, ARG_OFF(2, 0, 0, 0), 0x04, "one-shot of a letter");
        REJECT(gx, ARG_OFF(2, 0, 0, 0), 0xE8, "one-shot of 0xE8");
        REJECT(gx, ARG_OFF(2, 0, 0, 1), 1, "one-shot with a second argument");
        REJECT(gx, ARG_OFF(0, 0, 5, 0), 1, "argument on a plain key");
        REJECT(gx, ARG_OFF(0, 3, 75, 1), 1, "argument on a plain key, last position");
        REJECT(gx, LAYER_OFF(1, 2, 10), KM_CODE_TAP_HOLD, "toggle entry turned into tap-hold (arguments no longer fit)");
        REJECT(gx, LAYER_OFF(1, 0, 18), 0xEB, "unassigned code 0xEB");
        REJECT(gx, LAYER_OFF(1, 0, 18), 0xEF, "unassigned code 0xEF");
        REJECT(gx, OPTS_OFF(3) + 5, 1, "reserved options byte");
        REJECT(gx, OPTS_OFF(3) + 4, 3, "tap policy 3");
        REJECT(gx, OPTS_OFF(0), KM_LAYER_TAP_TERM_MIN - 1, "tap term 29 ms");
        REJECT(gx, OPTS_OFF(0) + 1, 0x04, "tap term over 1000 ms");
        REJECT(gx, OPTS_OFF(3) + 2, 99, "one-shot timeout 99 ms (non-zero below the minimum)");
        km_layer_opts_t o = { KM_LAYER_TAP_TERM_MIN, 0, 0 };
        CHECK(km_settings_opts_valid(&o));
        o.tap_term_ms = KM_LAYER_TAP_TERM_MAX; o.oneshot_ms = KM_LAYER_ONESHOT_MAX; o.tap_policy = KM_TAP_POLICY_OTHER_KEY;
        CHECK(km_settings_opts_valid(&o));
        o.oneshot_ms = KM_LAYER_ONESHOT_MAX + 1;
        CHECK(!km_settings_opts_valid(&o));
        o.oneshot_ms = KM_LAYER_ONESHOT_MIN - 1;
        CHECK(!km_settings_opts_valid(&o));
        o.oneshot_ms = 0; o.tap_term_ms = KM_LAYER_TAP_TERM_MAX + 1;
        CHECK(!km_settings_opts_valid(&o));
        CHECK(!km_settings_opts_valid(NULL));
        /* the in-memory rule refuses the same things (encode never writes an invalid object) */
        km_settings_default(&x);
        x.profile[0].layer_usage[0][0] = KM_CODE_TOGGLE;       /* arguments left 0 */
        CHECK_EQ(km_settings_encode(&x, gx, sizeof gx), 0);
        km_settings_default(&x);
        x.profile[2].layer_opts.tap_policy = 9;
        CHECK_EQ(km_settings_encode(&x, gx, sizeof gx), 0);
        CHECK(km_settings_entry_valid(KM_CODE_TAP_HOLD, 0x2C, 0xE0) && !km_settings_entry_valid(KM_CODE_TAP_HOLD, 0x2C, 0));
        CHECK(km_settings_entry_valid(KM_CODE_TAP_HOLD, KM_CODE_PLAY_PAUSE, KM_CODE_HOLD_L3));   /* a media key as the tap action */
        CHECK(km_settings_entry_valid(0xFD, 0, 0) && km_settings_entry_valid(0xFD, 1, 0));      /* 0xFD is KM_CODE_LIGHT now: action 0 = nothing, 1..7 need version 3 */
        CHECK(km_settings_entry_valid(0xFE, 0, 0) && !km_settings_entry_valid(0xFE, 1, 0));      /* 0xFE is still reserved, no arguments */
    }

    /* ---- version 1 blobs are still read and migrated ---- */
    {
        uint8_t v1[KM_SETTINGS_V1_SIZE + 16], out[SZ];
        km_settings_t src;
        for (unsigned variant = 0; variant < 3; variant++) {
            if (variant == 0) {
                km_settings_default(&src);
            } else {
                km_test_random_settings(&src, 7000u + variant);
                for (unsigned p = 0; p < KM_PROFILES; p++) {                      /* a version 1 table has no extended codes */
                    for (unsigned l = 0; l < KM_LAYERS; l++) {
                        for (unsigned sl = 0; sl < KM_KEYMAP_SLOTS; sl++) {
                            if (src.profile[p].layer_usage[l][sl] >= KM_CODE_EXT_FIRST && src.profile[p].layer_usage[l][sl] <= KM_CODE_EXT_LAST) {
                                src.profile[p].layer_usage[l][sl] = 0x2C;
                            }
                            src.profile[p].layer_arg[l][sl][0] = src.profile[p].layer_arg[l][sl][1] = 0;
                        }
                    }
                    km_layer_opts_default(&src.profile[p].layer_opts);
                }
            }
            CHECK(km_settings_valid(&src));
            CHECK_EQ(km_settings_encode(&src, out, sizeof out), SZ);
            /* build the version 1 blob from the version 2 bytes: same header (version 1, length 5488), per profile the key
               configs and the code bytes only */
            memset(v1, 0, sizeof v1);
            memcpy(v1, out, KM_SETTINGS_HEADER_BYTES);
            v1[4] = 1; v1[5] = 0; v1[6] = (uint8_t)(KM_SETTINGS_V1_SIZE & 0xFF); v1[7] = (uint8_t)(KM_SETTINGS_V1_SIZE >> 8);
            for (unsigned p = 0; p < KM_PROFILES; p++) {
                memcpy(v1 + KM_SETTINGS_HEADER_BYTES + p * KM_SETTINGS_V1_PROFILE_BYTES, out + PROFILE_OFF(p), KM_KEYMAP_SLOTS * KM_SETTINGS_KEYCFG_BYTES);
                memcpy(v1 + KM_SETTINGS_HEADER_BYTES + p * KM_SETTINGS_V1_PROFILE_BYTES + KM_KEYMAP_SLOTS * KM_SETTINGS_KEYCFG_BYTES,
                       out + LAYER_OFF(p, 0, 0), KM_LAYERS * KM_KEYMAP_SLOTS);
            }
            const uint32_t c = km_crc32(0, v1, KM_SETTINGS_V1_SIZE - 4);
            v1[KM_SETTINGS_V1_SIZE - 4] = (uint8_t)c; v1[KM_SETTINGS_V1_SIZE - 3] = (uint8_t)(c >> 8);
            v1[KM_SETTINGS_V1_SIZE - 2] = (uint8_t)(c >> 16); v1[KM_SETTINGS_V1_SIZE - 1] = (uint8_t)(c >> 24);
            memset(&back, 0x5A, sizeof back);
            CHECK(km_settings_decode(&back, v1, KM_SETTINGS_V1_SIZE));
            CHECK_EQ(back.version, KM_SETTINGS_VERSION);                          /* migrated into the current in-memory form */
            CHECK(settings_eq(&src, &back));                                      /* args 0, options default, everything else the same */
            uint8_t re[SZ];
            CHECK_EQ(km_settings_encode(&back, re, sizeof re), SZ);
            CHECK(memcmp(re, out, SZ) == 0);                                      /* written out again it is the version 2 blob */
        }
        /* a version 1 blob follows the version 1 rules: no extended codes, no trailing bytes, right length */
        CHECK(!km_settings_decode(&back, v1, KM_SETTINGS_V1_SIZE + 1));
        CHECK(!km_settings_decode(&back, v1, KM_SETTINGS_V1_SIZE - 1));
        uint8_t bad1[KM_SETTINGS_V1_SIZE];
        memcpy(bad1, v1, sizeof bad1);
        bad1[KM_SETTINGS_HEADER_BYTES + KM_SETTINGS_V1_PROFILE_BYTES - 1] = 0xE8;     /* an extended code in a version 1 table */
        {
            const uint32_t c = km_crc32(0, bad1, KM_SETTINGS_V1_SIZE - 4);
            bad1[KM_SETTINGS_V1_SIZE - 4] = (uint8_t)c; bad1[KM_SETTINGS_V1_SIZE - 3] = (uint8_t)(c >> 8);
            bad1[KM_SETTINGS_V1_SIZE - 2] = (uint8_t)(c >> 16); bad1[KM_SETTINGS_V1_SIZE - 1] = (uint8_t)(c >> 24);
        }
        CHECK(!km_settings_decode(&back, bad1, KM_SETTINGS_V1_SIZE));
        CHECK(!km_settings_decode(&back, out, KM_SETTINGS_V1_SIZE));                  /* a version 2 blob cut to the version 1 size */
    }

    /* ---- fuzz: mutate valid blobs, fix the CRC; whatever is accepted must be valid and canonical ---- */
    unsigned n_ok = 0, n_rej = 0;
    for (unsigned n = 0; n < 3000; n++) {
        uint8_t b2[SZ];
        km_test_random_settings(&s, n + 1000u);
        km_settings_encode(&s, buf, SZ);
        const unsigned hits = 1u + rnd() % 4u;
        for (unsigned h = 0; h < hits; h++) {
            buf[rnd() % (SZ - 4u)] = (uint8_t)rnd();
        }
        fix_crc(buf);
        if (km_settings_decode(&back, buf, SZ)) {
            n_ok++;
            CHECK(km_settings_valid(&back));
            CHECK_EQ(km_settings_encode(&back, b2, sizeof b2), SZ);
            CHECK(memcmp(buf, b2, SZ) == 0);
        } else {
            n_rej++;
        }
    }
    CHECK(n_ok > 100 && n_rej > 100);               /* the fuzz really exercised both outcomes */

    test_entry_rules_exhaustive();
    test_record_rules();
    test_v3_basic();
    test_v3_random();
    test_v3_strict_decode();
    test_v3_builder_cases();
    test_v3_fuzz();
}
