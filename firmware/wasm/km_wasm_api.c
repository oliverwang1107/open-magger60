/*
 * km_wasm_api.c - the keyboard's own settings / key-map / layer code as a WebAssembly module (prototype).
 * Original work.
 *
 * This file is the only thing written for the browser: it wraps the PUBLIC functions of km_settings.h, km_keymap.h,
 * km_layers.h and km_engine.h (the very sources the keyboard runs) in a few flat exports. No C library, no heap, no
 * globals except one settings object and one I/O buffer. The browser copies bytes into the buffer (km_io), calls an
 * export, and reads the answer from the same buffer. Build: tools/build_wasm.py; JS side: ui/wasm/km_wasm.js.
 *
 * Coupling to the core (what must be adapted when a core struct changes): km_w_set_cfg and km_w_cfg_valid
 * (km_engine_key_cfg_t fields), km_w_set_opts / km_w_opts_valid (km_layer_opts_t), km_w_set_entry and km_w_layers_load
 * (km_profile_t members layer_usage / layer_arg / layer_opts), the km_w_set_dks .. km_w_set_device group (the M6 members of
 * km_profile_t / km_settings_t and their record types in km_m6_types.h). Everything else only calls km_settings_default / _decode /
 * _encode / _valid, km_crc32 and km_layers_*.
 */
#include <stddef.h>
#include <stdint.h>

#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_settings.h"

#define EXPORT(name) __attribute__((export_name(name)))

#define KM_WASM_ABI 3u   /* 2: settings format version 3 (M6); 3: layer output with toggle keys / suppressed modifiers (P2) */
#define IO_SIZE     (KM_SETTINGS_V3_MAX_SIZE + 64u)   /* the largest blob a store record holds, plus slack to test over-long input */

static km_settings_t g_s;
static uint8_t       g_io[IO_SIZE];
static int           g_init;

static void ensure_init(void)
{
    if (!g_init) {
        km_settings_default(&g_s);
        g_init = 1;
    }
}

/* ---- information ---- */

EXPORT("km_abi") uint32_t km_abi(void) { return KM_WASM_ABI; }
EXPORT("km_io") uint8_t *km_io(void) { return g_io; }
EXPORT("km_io_size") uint32_t km_io_size(void) { return IO_SIZE; }
EXPORT("km_settings_size") uint32_t km_settings_size(void) { return KM_SETTINGS_ENCODED_SIZE; }
EXPORT("km_settings_version") uint32_t km_settings_version(void) { return KM_SETTINGS_VERSION; }
EXPORT("km_dims") uint32_t km_dims(uint32_t what)   /* 0 profiles, 1 layers, 2 slots, 3 argument bytes per entry */
{
    switch (what) {
    case 0: return KM_PROFILES;
    case 1: return KM_LAYERS;
    case 2: return KM_KEYMAP_SLOTS;
    case 3: return KM_KEYMAP_ARGS;
    default: return 0;
    }
}

/* ---- whole blobs ---- */

/* Load the factory default and write its blob to the I/O buffer. Returns the length. */
EXPORT("km_w_default") uint32_t km_w_default(void)
{
    km_settings_default(&g_s);
    g_init = 1;
    return km_settings_encode(&g_s, g_io, IO_SIZE);
}

/* Decode io[0..len). On success the blob of the decoded settings (the current format; an older format is migrated) replaces
 * the I/O buffer and its length is returned; on refusal 0 is returned and the held settings are unchanged. */
EXPORT("km_w_decode") uint32_t km_w_decode(uint32_t len)
{
    ensure_init();
    if (len > IO_SIZE) return 0;
    km_settings_t t;
    if (!km_settings_decode(&t, g_io, len)) return 0;
    g_s = t;
    return km_settings_encode(&g_s, g_io, IO_SIZE);
}

/* Write the held settings as a blob to the I/O buffer; 0 when they are not valid (nothing is written then). */
EXPORT("km_w_encode") uint32_t km_w_encode(void)
{
    ensure_init();
    return km_settings_encode(&g_s, g_io, IO_SIZE);
}

/* ---- edits of the held settings (not validated here: encode and valid say whether the result is acceptable) ---- */

EXPORT("km_w_set_active") uint32_t km_w_set_active(uint32_t active)
{
    ensure_init();
    if (active > 255u) return 0;
    g_s.active = (uint8_t)active;
    return 1;
}

EXPORT("km_w_set_cfg") uint32_t km_w_set_cfg(uint32_t p, uint32_t slot, uint32_t press, uint32_t release, uint32_t mode,
                                             uint32_t continuous, uint32_t rt_down, uint32_t rt_up, uint32_t top_dead,
                                             uint32_t bottom_dead)
{
    ensure_init();
    if (p >= KM_PROFILES || slot >= KM_KEYMAP_SLOTS) return 0;
    km_engine_key_cfg_t *c = &g_s.profile[p].key[slot];
    c->press_mm100 = (uint16_t)press;
    c->release_mm100 = (uint16_t)release;
    c->mode = (uint8_t)mode;
    c->continuous = (uint8_t)continuous;
    c->rt_down_mm100 = (uint16_t)rt_down;
    c->rt_up_mm100 = (uint16_t)rt_up;
    c->top_dead_mm100 = (uint16_t)top_dead;
    c->bottom_dead_mm100 = (uint16_t)bottom_dead;
    return 1;
}

EXPORT("km_w_set_entry") uint32_t km_w_set_entry(uint32_t p, uint32_t layer, uint32_t slot, uint32_t code, uint32_t a0,
                                                 uint32_t a1)
{
    ensure_init();
    if (p >= KM_PROFILES || layer >= KM_LAYERS || slot >= KM_KEYMAP_SLOTS) return 0;
    g_s.profile[p].layer_usage[layer][slot] = (uint8_t)code;
    g_s.profile[p].layer_arg[layer][slot][0] = (uint8_t)a0;
    g_s.profile[p].layer_arg[layer][slot][1] = (uint8_t)a1;
    return 1;
}

EXPORT("km_w_set_opts") uint32_t km_w_set_opts(uint32_t p, uint32_t tap_term, uint32_t oneshot, uint32_t policy)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    g_s.profile[p].layer_opts.tap_term_ms = (uint16_t)tap_term;
    g_s.profile[p].layer_opts.oneshot_ms = (uint16_t)oneshot;
    g_s.profile[p].layer_opts.tap_policy = (uint8_t)policy;
    return 1;
}

/* The M6 members (settings format version 3). Index arguments are masked like the test generator does, so the same command
 * lists drive both; the count of records in use grows with the highest index written (the *_n calls set it directly). */
EXPORT("km_w_set_dks") uint32_t km_w_set_dks(uint32_t p, uint32_t i, uint32_t t_act, uint32_t t_bot, uint32_t flags,
                                             uint32_t u0, uint32_t e0, uint32_t u1, uint32_t e1, uint32_t u2, uint32_t e2,
                                             uint32_t u3, uint32_t e3, uint32_t rsvd)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    km_profile_t *q = &g_s.profile[p];
    km_dks_rec_t *r = &q->dks[i & 7u];
    r->t_act_mm100 = (uint16_t)t_act;
    r->t_bot_mm100 = (uint16_t)t_bot;
    r->flags = (uint8_t)flags;
    r->act[0].usage = (uint8_t)u0; r->act[0].ev = (uint8_t)e0;
    r->act[1].usage = (uint8_t)u1; r->act[1].ev = (uint8_t)e1;
    r->act[2].usage = (uint8_t)u2; r->act[2].ev = (uint8_t)e2;
    r->act[3].usage = (uint8_t)u3; r->act[3].ev = (uint8_t)e3;
    r->rsvd[0] = (uint8_t)rsvd;
    if (q->dks_n < i + 1u) q->dks_n = (uint8_t)(i + 1u);
    return 1;
}

EXPORT("km_w_set_dks_n") uint32_t km_w_set_dks_n(uint32_t p, uint32_t n)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    g_s.profile[p].dks_n = (uint8_t)n;
    return 1;
}

EXPORT("km_w_set_morph") uint32_t km_w_set_morph(uint32_t p, uint32_t i, uint32_t mask, uint32_t usage, uint32_t flags,
                                                 uint32_t rsvd)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    km_profile_t *q = &g_s.profile[p];
    km_morph_rec_t *r = &q->morph[i & 7u];
    r->mod_mask = (uint8_t)mask;
    r->usage = (uint8_t)usage;
    r->flags = (uint8_t)flags;
    r->rsvd = (uint8_t)rsvd;
    if (q->morph_n < i + 1u) q->morph_n = (uint8_t)(i + 1u);
    return 1;
}

EXPORT("km_w_set_morph_n") uint32_t km_w_set_morph_n(uint32_t p, uint32_t n)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    g_s.profile[p].morph_n = (uint8_t)n;
    return 1;
}

EXPORT("km_w_set_socd") uint32_t km_w_set_socd(uint32_t p, uint32_t i, uint32_t a, uint32_t b, uint32_t mode, uint32_t flags)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    km_profile_t *q = &g_s.profile[p];
    km_socd_pair_t *r = &q->socd[i & 3u];
    r->slot_a = (uint8_t)a;
    r->slot_b = (uint8_t)b;
    r->mode = (uint8_t)mode;
    r->flags = (uint8_t)flags;
    if (q->socd_n < i + 1u) q->socd_n = (uint8_t)(i + 1u);
    return 1;
}

EXPORT("km_w_set_socd_n") uint32_t km_w_set_socd_n(uint32_t p, uint32_t n)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    g_s.profile[p].socd_n = (uint8_t)n;
    return 1;
}

EXPORT("km_w_set_pad") uint32_t km_w_set_pad(uint32_t p, uint32_t i, uint32_t start, uint32_t end, uint32_t shape, uint32_t flags)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    km_pad_curve_t *r = &g_s.profile[p].pad_curve[i & 3u];
    r->start_mm100_5 = (uint8_t)start;
    r->end_mm100_5 = (uint8_t)end;
    r->shape = (uint8_t)shape;
    r->flags = (uint8_t)flags;
    return 1;
}

EXPORT("km_w_set_padopt") uint32_t km_w_set_padopt(uint32_t p, uint32_t deadzone, uint32_t circular, uint32_t smooth, uint32_t rsvd)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    km_pad_opts_t *o = &g_s.profile[p].pad_opts;
    o->stick_deadzone = (uint8_t)deadzone;
    o->circular = (uint8_t)circular;
    o->smooth = (uint8_t)smooth;
    for (unsigned k = 0; k < sizeof o->rsvd; k++) o->rsvd[k] = (uint8_t)rsvd;
    return 1;
}

EXPORT("km_w_set_device") uint32_t km_w_set_device(uint32_t pad_mode, uint32_t filter_mode, uint32_t rsvd)
{
    ensure_init();
    g_s.device.pad_mode = (uint8_t)pad_mode;
    g_s.device.filter_mode = (uint8_t)filter_mode;
    for (unsigned k = 0; k < sizeof g_s.device.rsvd; k++) g_s.device.rsvd[k] = (uint8_t)rsvd;
    return 1;
}

/* True when the held settings can only be written as version 3. */
EXPORT("km_w_needs_v3") uint32_t km_w_needs_v3(void)
{
    ensure_init();
    return km_settings_needs_v3(&g_s);
}

/* The sensor slots that carry a key, ascending, written to io[0..61); returns how many. */
EXPORT("km_w_key_slots") uint32_t km_w_key_slots(void)
{
    uint8_t slots[KM_KEYMAP_KEYS];
    unsigned n = km_settings_key_slots(slots);
    for (unsigned i = 0; i < n; i++) g_io[i] = slots[i];
    return n;
}

/* ---- the rules, one call each ---- */

EXPORT("km_w_valid") uint32_t km_w_valid(void)
{
    ensure_init();
    return km_settings_valid(&g_s);
}

EXPORT("km_w_usage_valid") uint32_t km_w_usage_valid(uint32_t usage)
{
    return usage < 256u && km_settings_usage_valid((uint8_t)usage);
}

EXPORT("km_w_entry_valid") uint32_t km_w_entry_valid(uint32_t code, uint32_t a0, uint32_t a1)
{
    return code < 256u && a0 < 256u && a1 < 256u && km_settings_entry_valid((uint8_t)code, (uint8_t)a0, (uint8_t)a1);
}

/* The rule of settings format version 2 (a blob of that version is read with it). */
EXPORT("km_w_entry_valid_v2") uint32_t km_w_entry_valid_v2(uint32_t code, uint32_t a0, uint32_t a1)
{
    return code < 256u && a0 < 256u && a1 < 256u && km_keymap_entry_valid_v2((uint8_t)code, (uint8_t)a0, (uint8_t)a1);
}

EXPORT("km_w_opts_valid") uint32_t km_w_opts_valid(uint32_t tap_term, uint32_t oneshot, uint32_t policy)
{
    km_layer_opts_t o;
    if (tap_term > 0xFFFFu || oneshot > 0xFFFFu || policy > 255u) return 0;
    o.tap_term_ms = (uint16_t)tap_term;
    o.oneshot_ms = (uint16_t)oneshot;
    o.tap_policy = (uint8_t)policy;
    return km_settings_opts_valid(&o);
}

EXPORT("km_w_cfg_valid") uint32_t km_w_cfg_valid(uint32_t press, uint32_t release, uint32_t mode, uint32_t continuous,
                                                 uint32_t rt_down, uint32_t rt_up, uint32_t top_dead, uint32_t bottom_dead)
{
    km_engine_key_cfg_t c;
    if (press > 0xFFFFu || release > 0xFFFFu || mode > 255u || continuous > 255u || rt_down > 0xFFFFu || rt_up > 0xFFFFu ||
        top_dead > 0xFFFFu || bottom_dead > 0xFFFFu)
        return 0;
    c.press_mm100 = (uint16_t)press;
    c.release_mm100 = (uint16_t)release;
    c.mode = (uint8_t)mode;
    c.continuous = (uint8_t)continuous;
    c.rt_down_mm100 = (uint16_t)rt_down;
    c.rt_up_mm100 = (uint16_t)rt_up;
    c.top_dead_mm100 = (uint16_t)top_dead;
    c.bottom_dead_mm100 = (uint16_t)bottom_dead;
    return km_settings_cfg_valid(&c);
}

EXPORT("km_w_crc32") uint32_t km_w_crc32(uint32_t len)
{
    return len <= IO_SIZE ? km_crc32(0, g_io, len) : 0;
}

/* ---- layers: what a typist's keys turn into ---- */

/* Make profile p of the held settings the loaded key map (as the keyboard does on a profile switch) and reset the layer
 * state. 1 = loaded, 0 = refused (bad index, or a table the keymap rules reject; the built-in table is used then). */
EXPORT("km_w_layers_load") uint32_t km_w_layers_load(uint32_t p)
{
    ensure_init();
    if (p >= KM_PROFILES) return 0;
    uint32_t ok = km_layers_set_table(g_s.profile[p].layer_usage, g_s.profile[p].layer_arg, &g_s.profile[p].layer_opts) ? 1u : 0u;
    km_layers_set_morph(g_s.profile[p].morph, g_s.profile[p].morph_n);   /* the Mod-Morph records go with the table (P2) */
    km_layers_reset();   /* also after a refusal: the built-in table is then in use and no key may stay latched from before */
    return ok;
}

/* The table entry the key on this slot is locked to after the last km_w_layers_update (km_layers_locked_entry): 0 when there is
 * none, otherwise (1 << 24) | (arg1 << 16) | (arg0 << 8) | code. The M6 modules (DKS, pad, toggle key ...) read their keys this way. */
EXPORT("km_w_layers_locked") uint32_t km_w_layers_locked(uint32_t slot)
{
    km_layers_entry_t e;
    if (!km_layers_locked_entry(slot, &e)) return 0;
    return (1u << 24) | ((uint32_t)e.arg1 << 16) | ((uint32_t)e.arg0 << 8) | e.code;
}

EXPORT("km_w_layers_reset") void km_w_layers_reset(void)
{
    km_layers_reset();
}

/* Slots currently down are io[0..n); now_ms is the free-running millisecond clock. The result replaces the I/O buffer:
 *   [0] kbd_n  [1] layer  [2] toggled mask  [3] armed mask  [4] profile_request  [5] pending (0/1)
 *   [6..7] consumer usage, little endian  [8] toggle_keys  [9] suppressed modifier mask  [10 .. 10+kbd_n) the keyboard usages
 * Returns the number of bytes written. */
EXPORT("km_w_layers_update") uint32_t km_w_layers_update(uint32_t n, uint32_t now_ms)
{
    km_layers_out_t o;
    uint8_t down[KM_KEYMAP_SLOTS];
    if (n > KM_KEYMAP_SLOTS) n = KM_KEYMAP_SLOTS;
    for (uint32_t i = 0; i < n; i++) down[i] = g_io[i];
    km_layers_update_ms(down, n, now_ms, &o);
    g_io[0] = o.kbd_n;
    g_io[1] = o.layer;
    g_io[2] = o.toggled;
    g_io[3] = o.armed;
    g_io[4] = o.profile_request;
    g_io[5] = o.pending ? 1u : 0u;
    g_io[6] = (uint8_t)o.consumer;
    g_io[7] = (uint8_t)(o.consumer >> 8);
    g_io[8] = o.toggle_keys;
    g_io[9] = o.suppressed;
    for (uint32_t i = 0; i < o.kbd_n; i++) g_io[10 + i] = o.kbd[i];
    return 10u + o.kbd_n;
}
