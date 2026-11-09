#include "km/km_settings.h"

#include <string.h>

#include "km/km_keymap.h"
#include "km/km_store.h"   /* KM_STORE_MAX_PAYLOAD: the settings blob must fit one store record */

_Static_assert(KM_SETTINGS_V3_MAX_SIZE == KM_STORE_MAX_PAYLOAD, "the largest settings blob is one store record payload");
_Static_assert(KM_SETTINGS_ENCODED_SIZE <= KM_SETTINGS_V3_MAX_SIZE, "a version 2 blob fits one store record");
_Static_assert(KM_SETTINGS_V3_MAX_ENCODED ==
               KM_SETTINGS_V3_HEADER_BYTES + 4u + (KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_DEVICE_BYTES) +
               KM_PROFILES * ((KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_V3_KEYCFG_BYTES) +
                              (KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_V3_LAYERS_BYTES) +
                              (KM_SETTINGS_V3_SECTION_HEADER + 1u + KM_DKS_MAX * KM_SETTINGS_DKS_BYTES) +
                              (KM_SETTINGS_V3_SECTION_HEADER + KM_MORPH_MAX * KM_SETTINGS_MORPH_BYTES) +
                              (KM_SETTINGS_V3_SECTION_HEADER + KM_SOCD_MAX * KM_SETTINGS_SOCD_BYTES) +
                              (KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_PAD_BYTES)),
               "the documented worst-case size of a version 3 blob");
_Static_assert(KM_SETTINGS_V3_MAX_ENCODED <= KM_SETTINGS_ENCODED_SIZE, "the largest blob stays the version 2 size");


/* ---- CRC32 ---- */

uint32_t km_crc32(uint32_t crc, const uint8_t *data, unsigned len)
{
    static const uint32_t nib[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    crc = ~crc;
    for (unsigned i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ nib[crc & 0xFu];
        crc = (crc >> 4) ^ nib[crc & 0xFu];
    }
    return ~crc;
}

/* ---- little-endian helpers ---- */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

/* ---- defaults ---- */

void km_settings_pad_default(km_pad_curve_t curve[KM_PAD_CURVES], km_pad_opts_t *opts)
{
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        curve[i].start_mm100_5 = KM_PAD_CURVE_START_DEFAULT;
        curve[i].end_mm100_5 = KM_PAD_CURVE_END_DEFAULT;
        curve[i].shape = KM_PAD_SHAPE_LINEAR;
        curve[i].flags = 0;
    }
    memset(opts, 0, sizeof *opts);
    opts->stick_deadzone = 10;
    opts->circular = 1;
    opts->smooth = 0;
}

void km_settings_default(km_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->version = KM_SETTINGS_VERSION;
    s->active = 0;
    /* The layer tables are the keymap's raw default codes: keyboard usages AND function codes (FN, media, profile keys,
     * 'transparent'). km_keymap_usage() would turn every function code into 0 and leave the keyboard without an FN key. */
    uint8_t table[KM_LAYERS][KM_KEYMAP_SLOTS];
    km_keymap_default_table(table);
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
            km_engine_key_cfg_default(&s->profile[p].key[slot]);
        }
        memcpy(s->profile[p].layer_usage, table, sizeof table);
        km_layer_opts_default(&s->profile[p].layer_opts);     /* layer_arg and the M6 records stay 0 (memset above) ... */
        km_settings_pad_default(s->profile[p].pad_curve, &s->profile[p].pad_opts);   /* ... except the gamepad curves */
    }                                                                              /* the device options are all 0 = defaults */
}

unsigned km_settings_key_slots(uint8_t out[KM_KEYMAP_KEYS])
{
    unsigned n = 0;
    for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS && n < KM_KEYMAP_KEYS; slot++) {
        if (km_keymap_slot_is_key(slot)) {
            out[n++] = (uint8_t)slot;
        }
    }
    return n;
}

/* ---- validation ---- */

/* One key config: the engine owns the rule (km_engine_cfg_valid). */
bool km_settings_cfg_valid(const km_engine_key_cfg_t *c)
{
    return km_engine_cfg_valid(c);
}

/* One layer code byte: the keymap owns the rule (km_keymap_code_valid): 0 = no key, 0x04..0xE7 keyboard usage,
 * 0xE8..0xEF extended codes (their arguments are checked by km_settings_entry_valid), 0xF0..0xFF function codes. */
bool km_settings_usage_valid(uint8_t u)
{
    return km_keymap_code_valid(u);
}

/* One whole table entry: code and its two argument bytes. */
bool km_settings_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1)
{
    return km_keymap_entry_valid(code, arg0, arg1);
}

bool km_settings_opts_valid(const km_layer_opts_t *o)
{
    return km_layer_opts_valid(o);
}

static bool usage_ok(uint8_t u)
{
    return u >= 0x04u && u <= 0xE7u;
}

bool km_settings_dks_valid(const km_dks_rec_t *r)
{
    if (r == NULL || (r->flags & ~KM_DKS_FLAGS_MASK) != 0u || r->rsvd[0] != 0u || r->rsvd[1] != 0u || r->rsvd[2] != 0u) {
        return false;
    }
    const unsigned act = r->t_act_mm100;
    const unsigned bot = r->t_bot_mm100 == 0u ? KM_DKS_BOT_DEFAULT_MM100 : r->t_bot_mm100;
    if (act < KM_DKS_ACT_MIN_MM100 || bot > KM_M6_TRAVEL_MM100 || bot < act + KM_DKS_GAP_MIN_MM100) {
        return false;
    }
    for (unsigned i = 0; i < 4u; i++) {
        if (r->act[i].usage == 0u ? r->act[i].ev != 0u : !usage_ok(r->act[i].usage)) {
            return false;
        }
    }
    return true;
}

bool km_settings_morph_valid(const km_morph_rec_t *r)
{
    return r != NULL && r->mod_mask != 0u && usage_ok(r->usage) && (r->flags & ~KM_MORPH_FLAGS_MASK) == 0u && r->rsvd == 0u;
}

bool km_settings_socd_valid(const km_socd_pair_t *p)
{
    return p != NULL && p->slot_a < KM_KEYMAP_SLOTS && p->slot_b < KM_KEYMAP_SLOTS && p->slot_a != p->slot_b &&
           km_keymap_slot_is_key(p->slot_a) && km_keymap_slot_is_key(p->slot_b) && p->mode <= KM_SOCD_MODE_LAST &&
           (p->flags & ~KM_SOCD_FLAGS_MASK) == 0u;
}

bool km_settings_pad_curve_valid(const km_pad_curve_t *c)
{
    return c != NULL && c->start_mm100_5 >= KM_PAD_CURVE_START_MIN && c->end_mm100_5 <= KM_PAD_CURVE_END_MAX &&
           (unsigned)c->end_mm100_5 >= (unsigned)c->start_mm100_5 + KM_PAD_CURVE_SPAN_MIN && c->shape <= KM_PAD_SHAPE_LAST &&
           (c->flags & ~KM_PAD_FLAGS_MASK) == 0u;
}

bool km_settings_pad_opts_valid(const km_pad_opts_t *o)
{
    if (o == NULL || o->stick_deadzone > KM_PAD_DEADZONE_MAX || o->circular > 1u || o->smooth > KM_PAD_SMOOTH_MAX) {
        return false;
    }
    for (unsigned i = 0; i < sizeof o->rsvd; i++) {
        if (o->rsvd[i] != 0u) {
            return false;
        }
    }
    return true;
}

bool km_settings_device_valid(const km_device_opts_t *d)
{
    if (d == NULL || d->pad_mode > KM_PAD_MODE_LAST || d->filter_mode > KM_FILTER_MODE_LAST) {
        return false;
    }
    for (unsigned i = 0; i < sizeof d->rsvd; i++) {
        if (d->rsvd[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* The M6 records of one profile and the references the layer tables make to them. */
static bool profile_m6_valid(const km_profile_t *p)
{
    if (p->dks_n > KM_DKS_MAX || p->morph_n > KM_MORPH_MAX || p->socd_n > KM_SOCD_MAX) {
        return false;
    }
    for (unsigned i = 0; i < p->dks_n; i++) {
        if (!km_settings_dks_valid(&p->dks[i])) {
            return false;
        }
    }
    for (unsigned i = 0; i < p->morph_n; i++) {
        if (!km_settings_morph_valid(&p->morph[i])) {
            return false;
        }
    }
    bool used[KM_KEYMAP_SLOTS] = { false };                 /* a slot belongs to at most one pair: pairs never chain */
    for (unsigned i = 0; i < p->socd_n; i++) {
        if (!km_settings_socd_valid(&p->socd[i]) || used[p->socd[i].slot_a] || used[p->socd[i].slot_b]) {
            return false;
        }
        used[p->socd[i].slot_a] = used[p->socd[i].slot_b] = true;
    }
    for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
        if (!km_settings_pad_curve_valid(&p->pad_curve[i])) {
            return false;
        }
    }
    return km_settings_pad_opts_valid(&p->pad_opts);
}

bool km_settings_valid(const km_settings_t *s)
{
    if (s == NULL || s->active >= KM_PROFILES || !km_settings_device_valid(&s->device)) {
        return false;
    }
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        const km_profile_t *pr = &s->profile[p];
        for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
            if (!km_settings_cfg_valid(&pr->key[slot])) {
                return false;
            }
            for (unsigned l = 0; l < KM_LAYERS; l++) {
                const uint8_t code = pr->layer_usage[l][slot];
                const uint8_t a0 = pr->layer_arg[l][slot][0];
                const uint8_t a1 = pr->layer_arg[l][slot][1];
                if (!km_settings_entry_valid(code, a0, a1)) {
                    return false;
                }
                if ((code == KM_CODE_DKS && a0 >= pr->dks_n) || (code == KM_CODE_MOD_MORPH && a1 >= pr->morph_n)) {
                    return false;                            /* the entry points at a record that does not exist */
                }
            }
        }
        if (!km_settings_opts_valid(&pr->layer_opts) || !profile_m6_valid(pr)) {
            return false;
        }
    }
    return true;
}

static bool pad_is_default(const km_profile_t *p)
{
    km_pad_curve_t c[KM_PAD_CURVES];
    km_pad_opts_t o;
    km_settings_pad_default(c, &o);
    return memcmp(c, p->pad_curve, sizeof c) == 0 && memcmp(&o, &p->pad_opts, sizeof o) == 0;
}

static bool device_is_default(const km_device_opts_t *d)
{
    static const km_device_opts_t zero;
    return memcmp(d, &zero, sizeof zero) == 0;
}

bool km_settings_needs_v3(const km_settings_t *s)
{
    if (!device_is_default(&s->device)) {
        return true;
    }
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        const km_profile_t *pr = &s->profile[p];
        if (pr->dks_n != 0u || pr->morph_n != 0u || pr->socd_n != 0u || !pad_is_default(pr)) {
            return true;
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {   /* every slot: a version 2 blob stores them all */
                if (km_keymap_entry_needs_v3(pr->layer_usage[l][slot], pr->layer_arg[l][slot][0], pr->layer_arg[l][slot][1])) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* ---- encode, version 2 ---- */

static uint8_t *put_cfg(uint8_t *p, const km_engine_key_cfg_t *c)
{
    put16(p, c->press_mm100);
    put16(p + 2, c->release_mm100);
    p[4] = c->mode;
    p[5] = c->continuous;
    put16(p + 6, c->rt_down_mm100);
    put16(p + 8, c->rt_up_mm100);
    put16(p + 10, c->top_dead_mm100);
    put16(p + 12, c->bottom_dead_mm100);
    return p + KM_SETTINGS_KEYCFG_BYTES;
}

static uint8_t *put_opts(uint8_t *p, const km_layer_opts_t *o)
{
    put16(p, o->tap_term_ms);
    put16(p + 2, o->oneshot_ms);
    p[4] = o->tap_policy;
    p[5] = 0;
    return p + KM_SETTINGS_OPTS_BYTES;
}

unsigned km_settings_encode_v2(const km_settings_t *s, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || cap < KM_SETTINGS_ENCODED_SIZE || !km_settings_valid(s) || km_settings_needs_v3(s)) {
        return 0;
    }
    uint8_t *p = buf;
    put32(p, KM_SETTINGS_MAGIC);
    put16(p + 4, KM_SETTINGS_VERSION_2);
    put16(p + 6, KM_SETTINGS_ENCODED_SIZE);
    p[8] = s->active;
    p[9] = KM_PROFILES;
    p[10] = KM_LAYERS;
    p[11] = KM_KEYMAP_SLOTS;
    p += KM_SETTINGS_HEADER_BYTES;
    for (unsigned pr = 0; pr < KM_PROFILES; pr++) {
        for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
            p = put_cfg(p, &s->profile[pr].key[slot]);
        }
        p = put_opts(p, &s->profile[pr].layer_opts);
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            memcpy(p, s->profile[pr].layer_usage[l], KM_KEYMAP_SLOTS);
            p += KM_KEYMAP_SLOTS;
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            memcpy(p, s->profile[pr].layer_arg[l], KM_KEYMAP_SLOTS * KM_KEYMAP_ARGS);
            p += KM_KEYMAP_SLOTS * KM_KEYMAP_ARGS;
        }
    }
    put32(p, km_crc32(0, buf, KM_SETTINGS_ENCODED_SIZE - 4u));
    return KM_SETTINGS_ENCODED_SIZE;
}

/* ---- encode, version 3 ---- */

/* Which optional sections a profile gets: the writer and the size calculation use the same answers. */
static bool has_dks(const km_profile_t *p)   { return p->dks_n != 0u; }
static bool has_morph(const km_profile_t *p) { return p->morph_n != 0u; }
static bool has_socd(const km_profile_t *p)  { return p->socd_n != 0u; }
static bool has_pad(const km_profile_t *p)   { return !pad_is_default(p); }

static unsigned v3_sections(const km_settings_t *s)
{
    unsigned n = device_is_default(&s->device) ? 0u : 1u;
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        const km_profile_t *pr = &s->profile[p];
        n += 2u + has_dks(pr) + has_morph(pr) + has_socd(pr) + has_pad(pr);
    }
    return n;
}

static unsigned v3_size(const km_settings_t *s)
{
    unsigned n = KM_SETTINGS_V3_HEADER_BYTES + 4u;
    if (!device_is_default(&s->device)) {
        n += KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_DEVICE_BYTES;
    }
    for (unsigned p = 0; p < KM_PROFILES; p++) {
        const km_profile_t *pr = &s->profile[p];
        n += 2u * KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_V3_KEYCFG_BYTES + KM_SETTINGS_V3_LAYERS_BYTES;
        n += has_dks(pr) ? KM_SETTINGS_V3_SECTION_HEADER + 1u + pr->dks_n * KM_SETTINGS_DKS_BYTES : 0u;
        n += has_morph(pr) ? KM_SETTINGS_V3_SECTION_HEADER + pr->morph_n * KM_SETTINGS_MORPH_BYTES : 0u;
        n += has_socd(pr) ? KM_SETTINGS_V3_SECTION_HEADER + pr->socd_n * KM_SETTINGS_SOCD_BYTES : 0u;
        n += has_pad(pr) ? KM_SETTINGS_V3_SECTION_HEADER + KM_SETTINGS_PAD_BYTES : 0u;
    }
    return n;
}

static uint8_t *put_section(uint8_t *p, uint8_t id, uint8_t scope, unsigned len)
{
    p[0] = id;
    p[1] = scope;
    put16(p + 2, len);
    return p + KM_SETTINGS_V3_SECTION_HEADER;
}

unsigned km_settings_encode_v3(const km_settings_t *s, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || !km_settings_valid(s)) {
        return 0;
    }
    const unsigned size = v3_size(s);
    if (cap < size) {
        return 0;
    }
    uint8_t ks[KM_KEYMAP_KEYS];
    if (km_settings_key_slots(ks) != KM_KEYMAP_KEYS) {
        return 0;
    }
    uint8_t *p = buf;
    put32(p, KM_SETTINGS_MAGIC);
    put16(p + 4, KM_SETTINGS_VERSION_3);
    put16(p + 6, size);
    p[8] = s->active;
    p[9] = KM_PROFILES;
    p[10] = KM_LAYERS;
    p[11] = KM_KEYMAP_KEYS;
    p[12] = (uint8_t)v3_sections(s);
    p[13] = p[14] = p[15] = 0;
    p += KM_SETTINGS_V3_HEADER_BYTES;
    for (unsigned pr = 0; pr < KM_PROFILES; pr++) {
        const km_profile_t *q = &s->profile[pr];
        p = put_section(p, KM_SETTINGS_SEC_KEYCFG, (uint8_t)pr, KM_SETTINGS_V3_KEYCFG_BYTES);
        for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
            p = put_cfg(p, &q->key[ks[i]]);
        }
        p = put_section(p, KM_SETTINGS_SEC_LAYERS, (uint8_t)pr, KM_SETTINGS_V3_LAYERS_BYTES);
        p = put_opts(p, &q->layer_opts);
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
                *p++ = q->layer_usage[l][ks[i]];
            }
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
                *p++ = q->layer_arg[l][ks[i]][0];
                *p++ = q->layer_arg[l][ks[i]][1];
            }
        }
        if (has_dks(q)) {
            p = put_section(p, KM_SETTINGS_SEC_DKS, (uint8_t)pr, 1u + q->dks_n * KM_SETTINGS_DKS_BYTES);
            *p++ = q->dks_n;
            for (unsigned i = 0; i < q->dks_n; i++) {
                put16(p, q->dks[i].t_act_mm100);
                put16(p + 2, q->dks[i].t_bot_mm100);
                p[4] = q->dks[i].flags;
                p[5] = p[6] = p[7] = 0;
                for (unsigned a = 0; a < 4u; a++) {
                    p[8 + 2u * a] = q->dks[i].act[a].usage;
                    p[9 + 2u * a] = q->dks[i].act[a].ev;
                }
                p += KM_SETTINGS_DKS_BYTES;
            }
        }
        if (has_morph(q)) {
            p = put_section(p, KM_SETTINGS_SEC_MORPH, (uint8_t)pr, q->morph_n * KM_SETTINGS_MORPH_BYTES);
            for (unsigned i = 0; i < q->morph_n; i++) {
                p[0] = q->morph[i].mod_mask;
                p[1] = q->morph[i].usage;
                p[2] = q->morph[i].flags;
                p[3] = 0;
                p += KM_SETTINGS_MORPH_BYTES;
            }
        }
        if (has_socd(q)) {
            p = put_section(p, KM_SETTINGS_SEC_SOCD, (uint8_t)pr, q->socd_n * KM_SETTINGS_SOCD_BYTES);
            for (unsigned i = 0; i < q->socd_n; i++) {
                p[0] = q->socd[i].slot_a;
                p[1] = q->socd[i].slot_b;
                p[2] = q->socd[i].mode;
                p[3] = q->socd[i].flags;
                p += KM_SETTINGS_SOCD_BYTES;
            }
        }
        if (has_pad(q)) {
            p = put_section(p, KM_SETTINGS_SEC_PAD, (uint8_t)pr, KM_SETTINGS_PAD_BYTES);
            for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
                p[0] = q->pad_curve[i].start_mm100_5;
                p[1] = q->pad_curve[i].end_mm100_5;
                p[2] = q->pad_curve[i].shape;
                p[3] = q->pad_curve[i].flags;
                p += 4;
            }
            p[0] = q->pad_opts.stick_deadzone;
            p[1] = q->pad_opts.circular;
            p[2] = q->pad_opts.smooth;
            memset(p + 3, 0, 5);
            p += 8;
        }
    }
    if (!device_is_default(&s->device)) {
        p = put_section(p, KM_SETTINGS_SEC_DEVICE, KM_SETTINGS_SCOPE_GLOBAL, KM_SETTINGS_DEVICE_BYTES);
        p[0] = s->device.pad_mode;
        p[1] = s->device.filter_mode;
        memset(p + 2, 0, 14);
        p += KM_SETTINGS_DEVICE_BYTES;
    }
    put32(p, km_crc32(0, buf, size - 4u));
    return size;
}

unsigned km_settings_encoded_size(const km_settings_t *s)
{
    if (!km_settings_valid(s)) {
        return 0;
    }
    return km_settings_needs_v3(s) ? v3_size(s) : KM_SETTINGS_ENCODED_SIZE;
}

unsigned km_settings_encode(const km_settings_t *s, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || !km_settings_valid(s)) {
        return 0;
    }
    return km_settings_needs_v3(s) ? km_settings_encode_v3(s, buf, cap) : km_settings_encode_v2(s, buf, cap);
}

/* ---- decode ---- */

static const uint8_t *get_cfg(const uint8_t *p, km_engine_key_cfg_t *c)
{
    c->press_mm100 = get16(p);
    c->release_mm100 = get16(p + 2);
    c->mode = p[4];
    c->continuous = p[5];
    c->rt_down_mm100 = get16(p + 6);
    c->rt_up_mm100 = get16(p + 8);
    c->top_dead_mm100 = get16(p + 10);
    c->bottom_dead_mm100 = get16(p + 12);
    return p + KM_SETTINGS_KEYCFG_BYTES;
}

static km_settings_t tmp;    /* ~9 KB of decode scratch: static so it is not on the stack; single-threaded use only */

/* Version 1 reader: the first format, still accepted. Its codes are a subset of version 2's, the arguments and the layer
 * options did not exist and get their defaults. */
static bool decode_v1(km_settings_t *out, const uint8_t *buf)
{
    if (buf[9] != KM_PROFILES || buf[10] != KM_LAYERS || buf[11] != KM_KEYMAP_SLOTS) {
        return false;
    }
    km_settings_default(&tmp);                               /* the M6 members keep their defaults */
    tmp.active = buf[8];
    const uint8_t *p = buf + KM_SETTINGS_HEADER_BYTES;
    for (unsigned pr = 0; pr < KM_PROFILES; pr++) {
        for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
            p = get_cfg(p, &tmp.profile[pr].key[slot]);
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
                if (!km_keymap_code_valid_v1(p[slot])) {      /* 0xE8..0xEF did not exist in version 1 */
                    return false;
                }
            }
            memcpy(tmp.profile[pr].layer_usage[l], p, KM_KEYMAP_SLOTS);
            p += KM_KEYMAP_SLOTS;
        }
        km_layer_opts_default(&tmp.profile[pr].layer_opts);
    }
    if (!km_settings_valid(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

/* Version 2 reader. */
static bool decode_v2(km_settings_t *out, const uint8_t *buf)
{
    if (buf[9] != KM_PROFILES || buf[10] != KM_LAYERS || buf[11] != KM_KEYMAP_SLOTS) {
        return false;
    }
    km_settings_default(&tmp);                               /* the M6 members keep their defaults */
    tmp.active = buf[8];
    const uint8_t *p = buf + KM_SETTINGS_HEADER_BYTES;
    for (unsigned pr = 0; pr < KM_PROFILES; pr++) {
        for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
            p = get_cfg(p, &tmp.profile[pr].key[slot]);
        }
        tmp.profile[pr].layer_opts.tap_term_ms = get16(p);
        tmp.profile[pr].layer_opts.oneshot_ms = get16(p + 2);
        tmp.profile[pr].layer_opts.tap_policy = p[4];
        if (p[5] != 0) {                                     /* reserved: must be 0 */
            return false;
        }
        p += KM_SETTINGS_OPTS_BYTES;
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            memcpy(tmp.profile[pr].layer_usage[l], p, KM_KEYMAP_SLOTS);
            p += KM_KEYMAP_SLOTS;
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            memcpy(tmp.profile[pr].layer_arg[l], p, KM_KEYMAP_SLOTS * KM_KEYMAP_ARGS);
            p += KM_KEYMAP_SLOTS * KM_KEYMAP_ARGS;
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {           /* a version 2 blob follows the version 2 rule of an entry */
            for (unsigned slot = 0; slot < KM_KEYMAP_SLOTS; slot++) {
                if (!km_keymap_entry_valid_v2(tmp.profile[pr].layer_usage[l][slot], tmp.profile[pr].layer_arg[l][slot][0],
                                              tmp.profile[pr].layer_arg[l][slot][1])) {
                    return false;
                }
            }
        }
    }
    if (!km_settings_valid(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

/* The sections of a version 3 blob that a profile may carry, in the order the writer emits them. */
enum { S_KEYCFG, S_LAYERS, S_DKS, S_MORPH, S_SOCD, S_PAD, S_COUNT };

typedef struct {
    const uint8_t *p;
    unsigned       len;
    bool           present;
} sec_t;

static int known_section(uint8_t id)
{
    switch (id) {
    case KM_SETTINGS_SEC_KEYCFG: return S_KEYCFG;
    case KM_SETTINGS_SEC_LAYERS: return S_LAYERS;
    case KM_SETTINGS_SEC_DKS:    return S_DKS;
    case KM_SETTINGS_SEC_MORPH:  return S_MORPH;
    case KM_SETTINGS_SEC_SOCD:   return S_SOCD;
    case KM_SETTINGS_SEC_PAD:    return S_PAD;
    default:                     return -1;
    }
}

/* Version 3 reader. Every length is checked against the end of the blob BEFORE anything is read from it. */
static bool decode_v3(km_settings_t *out, const uint8_t *buf, unsigned len)
{
    if (len < KM_SETTINGS_V3_HEADER_BYTES + 4u || len > KM_SETTINGS_V3_MAX_SIZE) {
        return false;
    }
    if (buf[9] != KM_PROFILES || buf[10] != KM_LAYERS || buf[11] != KM_KEYMAP_KEYS) {
        return false;
    }
    const unsigned n = buf[12];
    if (n > KM_SETTINGS_V3_MAX_SECTIONS || buf[13] != 0u || buf[14] != 0u || buf[15] != 0u) {
        return false;
    }
    uint8_t ks[KM_KEYMAP_KEYS];
    if (km_settings_key_slots(ks) != KM_KEYMAP_KEYS) {
        return false;
    }

    sec_t sec[KM_PROFILES][S_COUNT];
    sec_t dev = { NULL, 0, false };
    memset(sec, 0, sizeof sec);
    uint8_t seen_id[KM_SETTINGS_V3_MAX_SECTIONS], seen_scope[KM_SETTINGS_V3_MAX_SECTIONS];
    const uint8_t *p = buf + KM_SETTINGS_V3_HEADER_BYTES;
    const uint8_t *const end = buf + len - 4u;                /* the payload area ends where the CRC starts */
    for (unsigned i = 0; i < n; i++) {
        if ((unsigned)(end - p) < KM_SETTINGS_V3_SECTION_HEADER) {
            return false;
        }
        const uint8_t id = p[0];
        const uint8_t scope = p[1];
        const unsigned slen = get16(p + 2);
        p += KM_SETTINGS_V3_SECTION_HEADER;
        if ((unsigned)(end - p) < slen) {
            return false;                                     /* the section claims more bytes than the blob has */
        }
        if (scope != KM_SETTINGS_SCOPE_GLOBAL && scope >= KM_PROFILES) {
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            if (seen_id[j] == id && seen_scope[j] == scope) {
                return false;                                 /* the same section twice */
            }
        }
        seen_id[i] = id;
        seen_scope[i] = scope;
        const int k = known_section(id);
        if (k >= 0) {
            if (scope >= KM_PROFILES) {
                return false;                                 /* a per-profile section needs a profile */
            }
            sec[scope][k].p = p;
            sec[scope][k].len = slen;
            sec[scope][k].present = true;
        } else if (id == KM_SETTINGS_SEC_DEVICE) {
            if (scope != KM_SETTINGS_SCOPE_GLOBAL) {
                return false;
            }
            dev.p = p;
            dev.len = slen;
            dev.present = true;
        } else if (id < KM_SETTINGS_SEC_OPTIONAL) {
            return false;                                     /* a required section this firmware does not know */
        }                                                     /* else: unknown and optional: skipped */
        p += slen;
    }
    if (p != end) {
        return false;                                         /* bytes between the last section and the CRC */
    }

    km_settings_default(&tmp);
    tmp.active = buf[8];
    for (unsigned pr = 0; pr < KM_PROFILES; pr++) {
        km_profile_t *q = &tmp.profile[pr];
        const sec_t *kc = &sec[pr][S_KEYCFG];
        const sec_t *ly = &sec[pr][S_LAYERS];
        if (!kc->present || kc->len != KM_SETTINGS_V3_KEYCFG_BYTES || !ly->present || ly->len != KM_SETTINGS_V3_LAYERS_BYTES) {
            return false;
        }
        const uint8_t *c = kc->p;
        for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
            c = get_cfg(c, &q->key[ks[i]]);
        }
        c = ly->p;
        q->layer_opts.tap_term_ms = get16(c);
        q->layer_opts.oneshot_ms = get16(c + 2);
        q->layer_opts.tap_policy = c[4];
        if (c[5] != 0u) {
            return false;
        }
        c += KM_SETTINGS_OPTS_BYTES;
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
                q->layer_usage[l][ks[i]] = *c++;
            }
        }
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            for (unsigned i = 0; i < KM_KEYMAP_KEYS; i++) {
                q->layer_arg[l][ks[i]][0] = c[0];
                q->layer_arg[l][ks[i]][1] = c[1];
                c += KM_KEYMAP_ARGS;
            }
        }

        const sec_t *d = &sec[pr][S_DKS];
        if (d->present) {
            if (d->len < 1u || d->p[0] > KM_DKS_MAX || d->len != 1u + d->p[0] * KM_SETTINGS_DKS_BYTES) {
                return false;
            }
            q->dks_n = d->p[0];
            c = d->p + 1;
            for (unsigned i = 0; i < q->dks_n; i++, c += KM_SETTINGS_DKS_BYTES) {
                q->dks[i].t_act_mm100 = get16(c);
                q->dks[i].t_bot_mm100 = get16(c + 2);
                q->dks[i].flags = c[4];
                q->dks[i].rsvd[0] = c[5];
                q->dks[i].rsvd[1] = c[6];
                q->dks[i].rsvd[2] = c[7];
                for (unsigned a = 0; a < 4u; a++) {
                    q->dks[i].act[a].usage = c[8 + 2u * a];
                    q->dks[i].act[a].ev = c[9 + 2u * a];
                }
            }
        }
        const sec_t *m = &sec[pr][S_MORPH];
        if (m->present) {
            if (m->len % KM_SETTINGS_MORPH_BYTES != 0u || m->len / KM_SETTINGS_MORPH_BYTES > KM_MORPH_MAX) {
                return false;
            }
            q->morph_n = (uint8_t)(m->len / KM_SETTINGS_MORPH_BYTES);
            for (unsigned i = 0; i < q->morph_n; i++) {
                c = m->p + i * KM_SETTINGS_MORPH_BYTES;
                q->morph[i].mod_mask = c[0];
                q->morph[i].usage = c[1];
                q->morph[i].flags = c[2];
                q->morph[i].rsvd = c[3];
            }
        }
        const sec_t *so = &sec[pr][S_SOCD];
        if (so->present) {
            if (so->len % KM_SETTINGS_SOCD_BYTES != 0u || so->len / KM_SETTINGS_SOCD_BYTES > KM_SOCD_MAX) {
                return false;
            }
            q->socd_n = (uint8_t)(so->len / KM_SETTINGS_SOCD_BYTES);
            for (unsigned i = 0; i < q->socd_n; i++) {
                c = so->p + i * KM_SETTINGS_SOCD_BYTES;
                q->socd[i].slot_a = c[0];
                q->socd[i].slot_b = c[1];
                q->socd[i].mode = c[2];
                q->socd[i].flags = c[3];
            }
        }
        const sec_t *pa = &sec[pr][S_PAD];
        if (pa->present) {
            if (pa->len != KM_SETTINGS_PAD_BYTES) {
                return false;
            }
            c = pa->p;
            for (unsigned i = 0; i < KM_PAD_CURVES; i++, c += 4) {
                q->pad_curve[i].start_mm100_5 = c[0];
                q->pad_curve[i].end_mm100_5 = c[1];
                q->pad_curve[i].shape = c[2];
                q->pad_curve[i].flags = c[3];
            }
            q->pad_opts.stick_deadzone = c[0];
            q->pad_opts.circular = c[1];
            q->pad_opts.smooth = c[2];
            memcpy(q->pad_opts.rsvd, c + 3, sizeof q->pad_opts.rsvd);
        }
    }
    if (dev.present) {
        if (dev.len != KM_SETTINGS_DEVICE_BYTES) {
            return false;
        }
        tmp.device.pad_mode = dev.p[0];
        tmp.device.filter_mode = dev.p[1];
        memcpy(tmp.device.rsvd, dev.p + 2, sizeof tmp.device.rsvd);
    }
    if (!km_settings_valid(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

bool km_settings_decode(km_settings_t *s, const uint8_t *buf, unsigned len)
{
    if (s == NULL || buf == NULL || len < KM_SETTINGS_HEADER_BYTES + 4u) {
        return false;
    }
    if (get32(buf) != KM_SETTINGS_MAGIC) {
        return false;
    }
    const unsigned version = get16(buf + 4);
    if (get16(buf + 6) != len) {                         /* the blob must be exactly as long as it says */
        return false;
    }
    if (get32(buf + len - 4u) != km_crc32(0, buf, len - 4u)) {
        return false;
    }
    switch (version) {
    case KM_SETTINGS_VERSION_1:
        return len == KM_SETTINGS_V1_SIZE && decode_v1(s, buf);
    case KM_SETTINGS_VERSION_2:
        return len == KM_SETTINGS_ENCODED_SIZE && decode_v2(s, buf);
    case KM_SETTINGS_VERSION_3:
        return decode_v3(s, buf, len);
    default:
        return false;                                    /* newer (or never defined) version: refuse */
    }
}
