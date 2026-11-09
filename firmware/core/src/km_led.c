/*
 * km_led.c - LED lighting core, see km_led.h. Integer math only, no allocation, no waiting.
 */
#include "km/km_led.h"

#include <string.h>

#include "km/km_lightscene.h"

_Static_assert(KM_LED_CHANNEL_CAP >= 1u && KM_LED_CHANNEL_CAP <= 255u, "the per-channel cap is a byte value");
_Static_assert(KM_LED_PIXEL_CAP >= KM_LED_CHANNEL_CAP, "a light must be able to show at least one full channel");
_Static_assert(KM_LED_BUDGET_SUM >= 255u, "the current budget must allow at least one full channel");
_Static_assert(KM_LED_BUDGET_SUM < 65536u, "the budget factor is computed in 16.16 fixed point");

#define STAGE_IDLE   0u
#define STAGE_RENDER 1u
#define STAGE_ENCODE 2u
#define STAGE_READY  3u

#define HEAT_REST_MM100   20u     /* below this the key counts as up (the idle noise of a sensor is about 15) */
#define HEAT_FULL_MM100   340u    /* nominal travel */
#define HEAT_HUE_BLUE     170u    /* hue of "up" on the 0..255 wheel */
#define HEAT_REST_VALUE   24u     /* an up key still shows a faint blue */
#define HEAT_MIN_VALUE    80u     /* brightness of a key that has just left the rest zone */

/* The scene a light core uses until km_led_set_scene(): no rules, every light on palette colour 0 (= the default light colour). Same values as
 * km_lightscene_default(); tests/test_led.c checks that. */
static const km_lightscene_t k_default_scene = { 0, { { 0, 0, 0, 0, { 0, 0, 0 }, 0 } }, { { 0, 150, 255 } }, { 0 }, 0, 0, 0 };

/* ---------------------------------------------------------------- geometry */

/* Light order = serpentine over the five key rows (LED_NOTES.md section 3); entries are sensing slots (slotmap_measured.json). */
static const uint8_t k_slot_of_led[KM_LED_COUNT] = {
    18, 56, 22, 4, 60, 31, 67, 70, 19, 55, 57, 3, 26, 8,
    64, 44, 62, 39, 21, 1, 37, 16, 13, 42, 6, 75, 2, 54, 36, 73,
    72, 74, 49, 34, 52, 9, 47, 12, 48, 32, 10, 28, 30, 11, 29, 27,
    45, 61, 7, 25, 43, 38, 0, 5, 59, 23, 63, 65, 66, 14, 68,
};
/* NOTE: this table is identical to the manufacturer's. It is a fact of the board's wiring (which light sits under which key), so any correct
 * table is the same; ours was derived from the measured slot map and the serpentine order, not copied (see DISCLAIMER.md section 5). */
/* The same table seen from the slot side (KM_LED_NONE: the 15 slots without a key). Kept as data so a key press costs a lookup;
 * tests/test_led.c checks that the two tables are inverses of each other. */
static const uint8_t k_led_of_slot[KM_LED_SLOTS] = {
    52, 19, 26, 11, 3, 53, 24, 48, 13, 35, 40, 43, 37, 22, 59, 0xFF,
    21, 0xFF, 0, 8, 0xFF, 18, 2, 55, 0xFF, 49, 12, 45, 41, 44, 42, 5,
    39, 0xFF, 33, 0xFF, 28, 20, 51, 17, 0xFF, 0xFF, 23, 50, 15, 46, 0xFF, 36,
    38, 32, 0xFF, 0xFF, 34, 0xFF, 27, 9, 1, 10, 0xFF, 54, 4, 47, 16, 56,
    14, 57, 58, 6, 60, 0xFF, 7, 0xFF, 30, 29, 31, 25,
};
/* Horizontal centre of every key in 1/8 of a key width (standard ANSI 60 %: 15 key widths per row, rows 1, 3 run right to left). */
static const uint8_t k_x8[KM_LED_COUNT] = {
    4, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 112,
    114, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 6,
    7, 18, 26, 34, 42, 50, 58, 66, 74, 82, 90, 98, 111,
    109, 94, 86, 78, 70, 62, 54, 46, 38, 30, 22, 9,
    5, 15, 25, 55, 85, 95, 105, 115,
};
static const uint8_t k_row_start[6] = { 0, 14, 28, 41, 53, KM_LED_COUNT };

unsigned km_led_slot_of_led(unsigned led)
{
    return led < KM_LED_COUNT ? k_slot_of_led[led] : KM_LED_SLOTS;
}

unsigned km_led_led_of_slot(unsigned slot)
{
    return slot < KM_LED_SLOTS ? k_led_of_slot[slot] : KM_LED_NONE;
}

unsigned km_led_x8(unsigned led)
{
    return led < KM_LED_COUNT ? k_x8[led] : 0u;
}

unsigned km_led_row(unsigned led)
{
    unsigned r = 0;
    while (r < 4u && led >= k_row_start[r + 1u]) {
        r++;
    }
    return r;
}

/* ---------------------------------------------------------------- colour helpers */

void km_led_hsv(uint8_t h, uint8_t s, uint8_t v, km_led_rgb_t *out)
{
    const unsigned region = (unsigned)h / 43u;
    const unsigned rem = ((unsigned)h - region * 43u) * 6u;      /* 0..252 */
    const unsigned p = ((unsigned)v * (255u - s)) / 255u;
    const unsigned q = ((unsigned)v * (255u - (s * rem) / 255u)) / 255u;
    const unsigned t = ((unsigned)v * (255u - (s * (255u - rem)) / 255u)) / 255u;
    unsigned r, g, b;
    switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    out->r = (uint8_t)r;
    out->g = (uint8_t)g;
    out->b = (uint8_t)b;
}

/* two data bits -> one stream byte: a 0 bit is the nibble 1000, a 1 bit is 1110 */
static const uint8_t k_pair[4] = { 0x88, 0x8E, 0xE8, 0xEE };

static void encode_channel(uint8_t v, uint8_t *dst)
{
    dst[0] = k_pair[(v >> 6) & 3u];
    dst[1] = k_pair[(v >> 4) & 3u];
    dst[2] = k_pair[(v >> 2) & 3u];
    dst[3] = k_pair[v & 3u];
}

void km_led_encode_pixel(const km_led_rgb_t *c, uint8_t *dst)
{
#if KM_LED_WIRE_ORDER == KM_LED_WIRE_RGB
    encode_channel(c->r, dst);
    encode_channel(c->g, dst + 4);
#else
    encode_channel(c->g, dst);
    encode_channel(c->r, dst + 4);
#endif
    encode_channel(c->b, dst + 8);
}

static uint8_t scale8(unsigned v, unsigned s)      /* v * s / 255, rounded to nearest; s <= 255 */
{
    return (uint8_t)((v * s + 127u) / 255u);
}

/* ---------------------------------------------------------------- configuration */

void km_led_cfg_default(km_led_cfg_t *cfg)
{
    cfg->effect = KM_LED_FX_DEMO;
    cfg->brightness = 255;
    cfg->speed = 128;
    cfg->color.r = 0;
    cfg->color.g = 150;
    cfg->color.b = 255;
}

bool km_led_cfg_valid(const km_led_cfg_t *cfg)
{
    return cfg->effect < KM_LED_FX_COUNT && cfg->speed != 0u;
}

void km_led_init(km_led_t *l)
{
    memset(l, 0, sizeof *l);
    km_led_cfg_default(&l->cfg);
    l->inflight = 0xFF;
    l->scene = &k_default_scene;
    l->idle_s = KM_LED_IDLE_DEFAULT_S;
    l->gain = KM_LED_IDLE_GAIN_FULL;
    l->scale16 = 65536u;
    l->out16 = 65536u;
    l->last_scale16 = 65536u;
}

bool km_led_set_cfg(km_led_t *l, const km_led_cfg_t *cfg)
{
    if (!km_led_cfg_valid(cfg)) {
        return false;
    }
    l->cfg = *cfg;
    l->started = false;                     /* the next work call begins a frame at once */
    return true;
}

void km_led_set_scene(km_led_t *l, const struct km_lightscene *scene)
{
    l->scene = scene != NULL ? scene : &k_default_scene;
    l->started = false;                     /* the next work call begins a frame at once */
}

void km_led_set_inputs(km_led_t *l, const km_led_inputs_t *in)
{
    const km_led_inputs_t n = { (uint8_t)(in->host_leds & (KM_LED_HOST_NUM | KM_LED_HOST_CAPS | KM_LED_HOST_SCROLL)), in->layer, in->profile,
                                (uint8_t)(in->flags & (KM_LED_IN_PAD | KM_LED_IN_TOGGLE)) };
    if (n.host_leds != l->in.host_leds || n.layer != l->in.layer || n.profile != l->in.profile || n.flags != l->in.flags) {
        l->in = n;
        km_led_wake(l);                     /* a change is something to look at (the host's Caps Lock, a layer switch) */
        l->started = false;
    }
}

static void ext_store(km_led_t *l, unsigned source, uint32_t now_ms, bool gray)
{
    if (!l->ext_valid[source]) {
        km_led_wake(l);                     /* the first frame of a stream / clip brings faded-out lights back */
    }
    l->ext_valid[source] = true;
    l->ext_gray[source] = gray;
    l->ext_ms[source] = now_ms;
}

void km_led_ext_rgb(km_led_t *l, unsigned source, const km_led_rgb_t px[KM_LED_COUNT], uint32_t now_ms)
{
    if (source < KM_LED_EXT_SOURCES && px != NULL) {
        memcpy(l->ext[source], px, sizeof l->ext[source]);
        ext_store(l, source, now_ms, false);
    }
}

void km_led_ext_gray(km_led_t *l, unsigned source, const uint8_t gray[KM_LED_COUNT], uint32_t now_ms)
{
    if (source < KM_LED_EXT_SOURCES && gray != NULL) {
        for (unsigned i = 0; i < KM_LED_COUNT; i++) {
            l->ext[source][i].r = gray[i];
            l->ext[source][i].g = 0;
            l->ext[source][i].b = 0;
        }
        ext_store(l, source, now_ms, true);
    }
}

void km_led_ext_end(km_led_t *l, unsigned source)
{
    if (source < KM_LED_EXT_SOURCES) {
        l->ext_valid[source] = false;
    }
}

bool km_led_ext_valid(const km_led_t *l, unsigned source)
{
    return source < KM_LED_EXT_SOURCES && l->ext_valid[source];
}

void km_led_set_blank(km_led_t *l, bool blank)
{
    if (l->blank != blank) {
        l->blank = blank;
        l->started = false;
    }
}

void km_led_set_idle(km_led_t *l, uint16_t seconds)
{
    if (seconds != l->idle_s) {
        l->idle_s = seconds;
        l->idle = false;                    /* the next frame decides again from the time since the last key: shorter acts at once, longer lights up */
    }
}

void km_led_wake(km_led_t *l)
{
    l->activity = true;
    if (l->idle || l->gain != KM_LED_IDLE_GAIN_FULL) {
        l->started = false;                 /* the next work call begins a frame at once: no wait for the slow frame pace of a dark static frame */
    }
}

bool km_led_idle(const km_led_t *l)
{
    return l->idle || l->gain != KM_LED_IDLE_GAIN_FULL;
}

void km_led_note_keys(km_led_t *l, const uint8_t *down_slots, unsigned n)
{
    if (n != 0u) {
        km_led_wake(l);                     /* any key counts, also one without a light */
    }
    for (unsigned i = 0; i < n; i++) {
        const unsigned slot = down_slots[i];
        if (slot < KM_LED_SLOTS) {
            const uint8_t led = k_led_of_slot[slot];
            if (led != KM_LED_NONE) {
                l->lvl[led] = 255;
            }
        }
    }
}

/* ---------------------------------------------------------------- frame pipeline */

static bool is_animated(unsigned fx)
{
    return fx != KM_LED_FX_OFF && fx != KM_LED_FX_STATIC && fx != KM_LED_FX_CUSTOM;
}

static uint8_t demo_effect(uint32_t t_ms)
{
    static const uint8_t k_cycle[5] = { KM_LED_FX_RAINBOW, KM_LED_FX_REACTIVE, KM_LED_FX_DEPTH, KM_LED_FX_BREATH, KM_LED_FX_STATIC };
    return k_cycle[(t_ms / KM_LED_DEMO_PHASE_MS) % 5u];
}

static uint32_t lerp_u32(uint32_t at_slow, uint32_t at_fast, unsigned speed)     /* speed 1 -> at_slow, 255 -> at_fast */
{
    if (speed < 1u) {
        speed = 1u;                                                         /* only reachable if cfg was written around km_led_set_cfg */
    }
    return at_slow - (uint32_t)(speed - 1u) * (at_slow - at_fast) / 254u;
}

/* Idle-off: decide from the key activity whether the lights are idle and move the gain towards its target by the time that passed. */
static void idle_step(km_led_t *l, uint32_t now_ms, uint32_t dt)
{
    if (l->activity || !l->act_armed) {                                     /* the first frame counts as activity: the clock starts at boot */
        l->last_act_ms = now_ms;
        l->act_armed = true;
        l->activity = false;
        l->idle = false;
    } else if (l->idle_s == 0u) {
        l->idle = false;
    } else if (!l->idle && (uint32_t)(now_ms - l->last_act_ms) >= (uint32_t)l->idle_s * 1000u) {
        l->idle = true;                                                     /* latched: a 49 day wrap of the clock can not undo it */
    }
    if (l->idle) {
        const uint32_t step = (uint32_t)KM_LED_IDLE_GAIN_FULL * dt / KM_LED_IDLE_FADE_OUT_MS;
        l->gain = (l->gain > step) ? (uint16_t)(l->gain - step) : 0u;
    } else {
        const uint32_t up = (uint32_t)l->gain + (uint32_t)KM_LED_IDLE_GAIN_FULL * dt / KM_LED_IDLE_FADE_IN_MS;
        l->gain = (up > KM_LED_IDLE_GAIN_FULL) ? KM_LED_IDLE_GAIN_FULL : (uint16_t)up;
    }
}

static bool rule_cond(const km_led_t *l, const km_ls_rule_t *r)
{
    switch (r->source) {
    case KM_LS_SRC_HOST_NUM:    return (l->in.host_leds & KM_LED_HOST_NUM) != 0u;
    case KM_LS_SRC_HOST_CAPS:   return (l->in.host_leds & KM_LED_HOST_CAPS) != 0u;
    case KM_LS_SRC_HOST_SCROLL: return (l->in.host_leds & KM_LED_HOST_SCROLL) != 0u;
    case KM_LS_SRC_LAYER:       return l->in.layer == r->param;
    case KM_LS_SRC_PROFILE:     return l->in.profile == r->param;
    case KM_LS_SRC_PAD:         return (l->in.flags & KM_LED_IN_PAD) != 0u;
    case KM_LS_SRC_TOGGLE:      return (l->in.flags & KM_LED_IN_TOGGLE) != 0u;
    case KM_LS_SRC_EXTERNAL:    return l->ext_use != 0u;
    default:                    return false;
    }
}

/* Per-frame decisions of the layers above the base effect: which external frame is drawn, which indicator rules show their colour. */
static void prepare_layers(km_led_t *l, uint32_t now_ms, unsigned fx)
{
    if (l->ext_valid[KM_LED_EXT_STREAM] && (int32_t)(now_ms - l->ext_ms[KM_LED_EXT_STREAM]) > (int32_t)KM_LED_EXT_TIMEOUT_MS) {
        l->ext_valid[KM_LED_EXT_STREAM] = false;                            /* the stream went quiet: back to dark */
    }
    l->ext_use = 0;
    if (!l->blank) {                                                       /* a fault shows nothing, not even an indicator */
        if (fx == KM_LED_FX_EXTERNAL && l->ext_valid[KM_LED_EXT_STREAM]) {
            l->ext_use = 1u + KM_LED_EXT_STREAM;
        } else if (fx == KM_LED_FX_ANIM && l->ext_valid[KM_LED_EXT_ANIM]) {
            l->ext_use = 1u + KM_LED_EXT_ANIM;
        }
    }
    l->nrules = l->blank ? 0u : l->scene->n_rules;
    if (l->nrules > KM_LS_MAX_RULES) {
        l->nrules = KM_LS_MAX_RULES;                                       /* only reachable with a scene that was not validated */
    }
    l->shown = 0;
    l->blink = false;
    const bool phase_on = ((now_ms / KM_LED_BLINK_HALF_MS) & 1u) == 0u;
    for (unsigned k = 0; k < l->nrules; k++) {
        const km_ls_rule_t *r = &l->scene->rule[k];
        if (rule_cond(l, r)) {
            const bool blinks = (r->flags & KM_LS_FLAG_BLINK) != 0u;
            l->blink = l->blink || blinks;
            if (!blinks || phase_on) {
                l->shown |= (uint8_t)(1u << k);
            }
        }
    }
}

static void begin_frame(km_led_t *l, uint32_t now_ms)
{
    const km_led_cfg_t *c = &l->cfg;
    uint32_t dt = l->started ? now_ms - l->last_frame_ms : KM_LED_FRAME_MS_ANIM;

    unsigned fx = c->effect;
    if (fx == KM_LED_FX_DEMO) {
        fx = demo_effect(now_ms);
    }
    if (l->blank) {
        fx = KM_LED_FX_OFF;
    }
    l->fx = (uint8_t)fx;
    l->t_ms = now_ms;
    prepare_layers(l, now_ms, fx);
    l->mscale =(uint8_t)(((unsigned)c->brightness * KM_LED_CHANNEL_CAP + 127u) / 255u);
    idle_step(l, now_ms, dt > 4000u ? 4000u : dt);

    const uint32_t cycle = lerp_u32(12000u, 1500u, c->speed);               /* rainbow: one full hue cycle */
    l->phase8 = (uint8_t)((now_ms % cycle) * 256u / cycle);
    const uint32_t period = lerp_u32(6000u, 1200u, c->speed);               /* breathing: one up-down */
    const unsigned ph = (unsigned)((now_ms % period) * 512u / period);      /* 0..511 */
    const unsigned tri = ph < 256u ? ph : 511u - ph;                        /* 0..255 */
    l->breath = (uint8_t)((tri * tri) >> 8);
    const uint32_t fade_ms = lerp_u32(1800u, 270u, c->speed);               /* reactive: full to dark */
    if (dt > 4000u) {
        dt = 4000u;
    }
    const uint32_t dec = (255u * dt + fade_ms - 1u) / fade_ms;
    l->dec = (uint16_t)(dec > 255u ? 255u : dec);

    l->total = 0;
    l->scale16 = 65536u;
    l->next_led = 0;
    l->stage = STAGE_RENDER;
    l->out_idx = (l->inflight == 0xFFu) ? 0u : (uint8_t)(l->inflight ^ 1u);
    l->last_frame_ms = now_ms;
    {   /* a fade in progress needs the fast pace; a dark frame needs no animation */
        const bool fading = l->gain != 0u && l->gain != KM_LED_IDLE_GAIN_FULL;
        const bool anim = ((is_animated(fx) || l->blink) && l->gain != 0u) || fading;
        l->next_frame_ms = now_ms + (anim ? KM_LED_FRAME_MS_ANIM : KM_LED_FRAME_MS_STATIC);
    }
    l->started = true;
}

static void heat_color(unsigned d, km_led_rgb_t *out)
{
    if (d > HEAT_FULL_MM100) {
        d = HEAT_FULL_MM100;
    }
    if (d < HEAT_REST_MM100) {
        km_led_hsv(HEAT_HUE_BLUE, 255, HEAT_REST_VALUE, out);
        return;
    }
    const unsigned t = d - HEAT_REST_MM100;                                 /* 0..320 */
    const unsigned span = HEAT_FULL_MM100 - HEAT_REST_MM100;
    km_led_hsv((uint8_t)(HEAT_HUE_BLUE - t * HEAT_HUE_BLUE / span), 255, (uint8_t)(HEAT_MIN_VALUE + t * (255u - HEAT_MIN_VALUE) / span), out);
}

/* Indicator layer of light i: the first rule of this light whose colour is shown wins; if none is shown, the first rule of this light that
 * asks for black or dim decides what happens to the colour below. Pre-limit: the shaping that follows can only lower the result. */
static void apply_rules(const km_led_t *l, unsigned i, km_led_rgb_t *raw)
{
    int off = -1;
    for (unsigned k = 0; k < l->nrules; k++) {
        const km_ls_rule_t *r = &l->scene->rule[k];
        if (r->led != i) {
            continue;
        }
        if ((l->shown >> k) & 1u) {
            *raw = r->on;
            return;
        }
        if (off < 0 && (r->flags & KM_LS_FLAG_OFF_MASK) != KM_LS_OFF_KEEP) {
            off = (int)k;
        }
    }
    if (off >= 0) {
        const km_ls_rule_t *r = &l->scene->rule[off];
        if ((r->flags & KM_LS_FLAG_OFF_MASK) == KM_LS_OFF_BLACK) {
            raw->r = raw->g = raw->b = 0;
        } else {
            raw->r = scale8(raw->r, r->dim);
            raw->g = scale8(raw->g, r->dim);
            raw->b = scale8(raw->b, r->dim);
        }
    }
}

static void render_one(km_led_t *l, unsigned i, km_led_depth_fn depth, void *ctx)
{
    const km_led_cfg_t *c = &l->cfg;
    km_led_rgb_t raw = { 0, 0, 0 };

    if (l->lvl[i] != 0u) {                                                  /* the fade-out runs for every effect */
        l->lvl[i] = (l->lvl[i] > l->dec) ? (uint8_t)(l->lvl[i] - l->dec) : 0u;
    }

    switch (l->fx) {
    case KM_LED_FX_STATIC:
        raw = c->color;
        break;
    case KM_LED_FX_BREATH:
        raw.r = scale8(c->color.r, l->breath);
        raw.g = scale8(c->color.g, l->breath);
        raw.b = scale8(c->color.b, l->breath);
        break;
    case KM_LED_FX_RAINBOW:
        km_led_hsv((uint8_t)(l->phase8 - ((k_x8[i] * 35u) >> 4)), 255, 255, &raw);
        break;
    case KM_LED_FX_REACTIVE:
        raw.r = scale8(c->color.r, l->lvl[i]);
        raw.g = scale8(c->color.g, l->lvl[i]);
        raw.b = scale8(c->color.b, l->lvl[i]);
        break;
    case KM_LED_FX_DEPTH:
        heat_color(depth != NULL ? depth(ctx, k_slot_of_led[i]) : 0u, &raw);
        break;
    case KM_LED_FX_CUSTOM:
        raw = l->scene->palette[l->scene->key_idx[i] % KM_LS_PALETTE];
        break;
    case KM_LED_FX_EXTERNAL:
    case KM_LED_FX_ANIM:
        if (l->ext_use != 0u) {
            const unsigned src = l->ext_use - 1u;
            raw = l->ext[src][i];
            if (l->ext_gray[src]) {                                         /* grey level tinted with the configured colour */
                const unsigned g = raw.r;
                raw.r = scale8(c->color.r, g);
                raw.g = scale8(c->color.g, g);
                raw.b = scale8(c->color.b, g);
            }
        }
        break;
    default:
        break;
    }
    if (l->nrules != 0u) {
        apply_rules(l, i, &raw);
    }

    /* shape: user brightness + hard channel cap in one factor, then the white limit */
    unsigned r = scale8(raw.r, l->mscale), g = scale8(raw.g, l->mscale), b = scale8(raw.b, l->mscale);
    unsigned sum = r + g + b;
    if (sum > KM_LED_PIXEL_CAP) {
        r = r * KM_LED_PIXEL_CAP / sum;
        g = g * KM_LED_PIXEL_CAP / sum;
        b = b * KM_LED_PIXEL_CAP / sum;
        sum = r + g + b;
    }
    l->px[i].r = (uint8_t)r;
    l->px[i].g = (uint8_t)g;
    l->px[i].b = (uint8_t)b;
    l->total += sum;
}

static void encode_one(km_led_t *l, unsigned i)
{
    km_led_rgb_t c = l->px[i];
    if (l->out16 != 65536u) {
        c.r = (uint8_t)(((uint32_t)c.r * l->out16) >> 16);
        c.g = (uint8_t)(((uint32_t)c.g * l->out16) >> 16);
        c.b = (uint8_t)(((uint32_t)c.b * l->out16) >> 16);
    }
    km_led_encode_pixel(&c, &l->buf[l->out_idx][KM_LED_LEAD_BYTES + i * KM_LED_BYTES_PER_LED]);
}

bool km_led_work(km_led_t *l, uint32_t now_ms, km_led_depth_fn depth, void *ctx, unsigned max_units)
{
    unsigned done = 0;
    if (l->stage == STAGE_READY) {
        l->units = 0;
        return true;
    }
    if (l->stage == STAGE_IDLE) {
        if (l->started && (int32_t)(now_ms - l->next_frame_ms) < 0) {
            l->units = 0;
            return false;
        }
        begin_frame(l, now_ms);
    }
    while (done < max_units && l->stage != STAGE_READY) {
        if (l->stage == STAGE_RENDER) {
            render_one(l, l->next_led, depth, ctx);
            done++;
            if (++l->next_led == KM_LED_COUNT) {
                if (l->total > KM_LED_BUDGET_SUM) {
                    l->scale16 = (uint32_t)(((uint32_t)KM_LED_BUDGET_SUM << 16) / l->total);
                }
                /* the idle fade comes AFTER the current budget: a keyboard that is held back by the budget would otherwise keep its look
                 * for most of the fade and then drop; (gain + 1) / 65536 is exactly 1 at full gain and a factor below 1 otherwise */
                l->out16 = (l->gain == KM_LED_IDLE_GAIN_FULL) ? l->scale16 : (uint32_t)(((uint64_t)l->scale16 * ((uint32_t)l->gain + 1u)) >> 16);
                l->next_led = 0;
                l->stage = STAGE_ENCODE;
            }
        } else {
            encode_one(l, l->next_led);
            done++;
            if (++l->next_led == KM_LED_COUNT) {
                l->last_total = l->total;
                l->last_scale16 = l->scale16;
                l->frames++;
                l->stage = STAGE_READY;
            }
        }
    }
    l->units = done;
    if (done > l->units_max) {
        l->units_max = done;
    }
    return l->stage == STAGE_READY;
}

const uint8_t *km_led_frame(const km_led_t *l)
{
    return l->stage == STAGE_READY ? l->buf[l->out_idx] : NULL;
}

void km_led_frame_taken(km_led_t *l)
{
    if (l->stage == STAGE_READY) {
        l->inflight = l->out_idx;
        l->stage = STAGE_IDLE;
    }
}
