/*
 * test_led.c - M5 task L5: the LED core (km_led.c: geometry, encoder, effects, limits, frame pipeline) and the kill-switch policy
 * (km_led_guard.c). The expected values are computed here from the documented formulas, separately from the code under test.
 * The hardware side (led_hw.c, m5_led.c) is in test_led_hw.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_keymap.h"
#include "km/km_led.h"
#include "km/km_led_guard.h"
#include "km/km_lightscene.h"
#include "km_test.h"

/* ---------------------------------------------------------------- helpers */

static unsigned nibble_bit(uint8_t n)         /* 0x8 -> 0, 0xE -> 1, anything else -> 2 (an error) */
{
    return n == 0x8u ? 0u : (n == 0xEu ? 1u : 2u);
}

/* Inverse of the encoder for one channel; 0x100 if the bytes are not a valid stream. */
static unsigned decode_channel(const uint8_t *p)
{
    unsigned v = 0;
    for (unsigned i = 0; i < 4; i++) {
        const unsigned hi = nibble_bit((uint8_t)(p[i] >> 4)), lo = nibble_bit((uint8_t)(p[i] & 0xFu));
        if (hi > 1u || lo > 1u) {
            return 0x100u;
        }
        v = (v << 2) | (hi << 1) | lo;
    }
    return v;
}

static bool decode_frame(const uint8_t *f, km_led_rgb_t out[KM_LED_COUNT])
{
    bool ok = true;
    for (unsigned i = 0; i < KM_LED_LEAD_BYTES; i++) {
        ok = ok && f[i] == 0;
    }
    for (unsigned i = 0; i < KM_LED_TRAIL_BYTES; i++) {
        ok = ok && f[KM_LED_LEAD_BYTES + KM_LED_COUNT * KM_LED_BYTES_PER_LED + i] == 0;
    }
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        const uint8_t *p = f + KM_LED_LEAD_BYTES + i * KM_LED_BYTES_PER_LED;
        const unsigned a = decode_channel(p), b = decode_channel(p + 4), c = decode_channel(p + 8);
        if (a > 255u || b > 255u || c > 255u) {
            return false;
        }
#if KM_LED_WIRE_ORDER == KM_LED_WIRE_RGB
        out[i].r = (uint8_t)a; out[i].g = (uint8_t)b; out[i].b = (uint8_t)c;
#else
        out[i].g = (uint8_t)a; out[i].r = (uint8_t)b; out[i].b = (uint8_t)c;
#endif
    }
    return ok;
}

/* Run the pipeline until a frame is ready, time held at *now (units per call as given). NULL if it never finishes. */
static const uint8_t *make_frame(km_led_t *l, uint32_t now, km_led_depth_fn d, void *ctx, unsigned units)
{
    for (unsigned i = 0; i < 1000u; i++) {
        if (km_led_work(l, now, d, ctx, units)) {
            return km_led_frame(l);
        }
    }
    return NULL;
}

/* One frame at time `now`, decoded and handed on (taken). */
static bool frame_at(km_led_t *l, uint32_t now, km_led_depth_fn d, void *ctx, km_led_rgb_t out[KM_LED_COUNT])
{
    const uint8_t *f = make_frame(l, now, d, ctx, 1000);
    if (f == NULL) {
        return false;
    }
    const bool ok = decode_frame(f, out);
    km_led_frame_taken(l);
    return ok;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static km_led_cfg_t cfg_of(unsigned fx, unsigned bright, unsigned speed, uint8_t r, uint8_t g, uint8_t b)
{
    km_led_cfg_t c;
    c.effect = (uint8_t)fx;
    c.brightness = (uint8_t)bright;
    c.speed = (uint8_t)speed;
    c.color.r = r;
    c.color.g = g;
    c.color.b = b;
    return c;
}

static unsigned sum_of(const km_led_rgb_t *c)
{
    return (unsigned)c->r + c->g + c->b;
}

static bool all_equal(const km_led_rgb_t *px)
{
    for (unsigned i = 1; i < KM_LED_COUNT; i++) {
        if (px[i].r != px[0].r || px[i].g != px[0].g || px[i].b != px[0].b) {
            return false;
        }
    }
    return true;
}

static bool all_black(const km_led_rgb_t *px)
{
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        if (sum_of(&px[i]) != 0u) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- geometry */

static void test_geometry(void)
{
    /* the light order, written as key names straight from the physical layout: rows 2 and 4 run right to left */
    static const char *const k_names[KM_LED_COUNT] = {
        "ESC", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "BACKSPACE",
        "\\", "]", "[", "P", "O", "I", "U", "Y", "T", "R", "E", "W", "Q", "TAB",
        "CAPS", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "ENTER",
        "RSHIFT", "/", ".", ",", "M", "N", "B", "V", "C", "X", "Z", "LSHIFT",
        "LCTRL", "LWIN", "LALT", "SPACE", "RALT", "RWIN", "FN", "RCTRL",
    };
    for (unsigned led = 0; led < KM_LED_COUNT; led++) {
        const unsigned slot = km_led_slot_of_led(led);
        CHECK(slot < KM_LED_SLOTS);
        CHECK(strcmp(km_keymap_slot_name(slot), k_names[led]) == 0);
        CHECK_EQ(km_led_led_of_slot(slot), led);                  /* the two tables are inverses */
    }
    CHECK_EQ(km_led_slot_of_led(KM_LED_COUNT), KM_LED_SLOTS);
    unsigned none = 0;
    for (unsigned s = 0; s < KM_LED_SLOTS; s++) {
        const unsigned led = km_led_led_of_slot(s);
        CHECK_EQ(led == KM_LED_NONE, !km_keymap_slot_is_key(s));    /* a light exactly behind every key */
        none += (led == KM_LED_NONE);
    }
    CHECK_EQ(none, 15);
    CHECK_EQ(km_led_led_of_slot(KM_LED_SLOTS), KM_LED_NONE);

    /* rows and horizontal positions, recomputed from the key widths (in 1/8 key width) */
    static const struct { unsigned first, n; bool rev; unsigned w[16]; } k_rows[5] = {
        { 0, 14, false, { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 16 } },
        { 14, 14, true, { 12, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 12 } },
        { 28, 13, false, { 14, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 18 } },
        { 41, 12, true, { 18, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 22 } },
        { 53, 8, false, { 10, 10, 10, 50, 10, 10, 10, 10 } },
    };
    for (unsigned r = 0; r < 5; r++) {
        unsigned edge = 0;
        for (unsigned k = 0; k < k_rows[r].n; k++) {
            const unsigned centre = edge + k_rows[r].w[k] / 2u;
            edge += k_rows[r].w[k];
            const unsigned led = k_rows[r].first + (k_rows[r].rev ? k_rows[r].n - 1u - k : k);
            CHECK_EQ(km_led_x8(led), centre);
            CHECK_EQ(km_led_row(led), r);
        }
        CHECK_EQ(edge, 120);                                      /* every row is 15 key widths */
    }
    CHECK_EQ(km_led_x8(KM_LED_COUNT), 0);
}

/* ---------------------------------------------------------------- encoder */

static void test_encoder(void)
{
    uint8_t b[KM_LED_BYTES_PER_LED];
    km_led_rgb_t c = { 0, 0, 0 };
    km_led_encode_pixel(&c, b);
    for (unsigned i = 0; i < 12; i++) {
        CHECK_EQ(b[i], 0x88);                                     /* every data 0 is the nibble 1000 */
    }
    c.r = 0xFF;
    km_led_encode_pixel(&c, b);
#if KM_LED_WIRE_ORDER == KM_LED_WIRE_GRB
    static const uint8_t want_red[12] = { 0x88, 0x88, 0x88, 0x88, 0xEE, 0xEE, 0xEE, 0xEE, 0x88, 0x88, 0x88, 0x88 };
#else
    static const uint8_t want_red[12] = { 0xEE, 0xEE, 0xEE, 0xEE, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 };
#endif
    CHECK(memcmp(b, want_red, 12) == 0);

    /* one channel, every value 0..255: 4 bytes, most significant bit first, two data bits per byte */
    for (unsigned v = 0; v < 256; v++) {
        km_led_rgb_t x = { 0, 0, (uint8_t)v };
        km_led_encode_pixel(&x, b);
        CHECK_EQ(decode_channel(b + 8), v);
    }
    km_led_rgb_t x = { 0xA5, 0x3C, 0x0F };
    km_led_encode_pixel(&x, b);
    unsigned got[3] = { decode_channel(b), decode_channel(b + 4), decode_channel(b + 8) };
#if KM_LED_WIRE_ORDER == KM_LED_WIRE_GRB
    CHECK_EQ(got[0], 0x3C); CHECK_EQ(got[1], 0xA5); CHECK_EQ(got[2], 0x0F);
#else
    CHECK_EQ(got[0], 0xA5); CHECK_EQ(got[1], 0x3C); CHECK_EQ(got[2], 0x0F);
#endif

    CHECK_EQ(KM_LED_FRAME_BYTES, 808);                            /* 4 + 61 * 12 + 72 */
    km_led_t l;
    km_led_init(&l);
    const uint8_t *f = make_frame(&l, 0, NULL, NULL, 1000);
    CHECK(f != NULL);
    km_led_rgb_t px[KM_LED_COUNT];
    CHECK(f != NULL && decode_frame(f, px));
}

static void test_hsv(void)
{
    km_led_rgb_t c;
    km_led_hsv(0, 255, 255, &c);
    CHECK(c.r == 255 && c.g == 0 && c.b == 0);
    km_led_hsv(85, 255, 255, &c);
    CHECK(c.g == 255 && c.r < 12 && c.b == 0);
    km_led_hsv(170, 255, 255, &c);
    CHECK(c.b == 255 && c.r == 0 && c.g < 12);
    km_led_hsv(42, 0, 200, &c);                                   /* no saturation: grey */
    CHECK(c.r == 200 && c.g == 200 && c.b == 200);
    km_led_hsv(123, 255, 0, &c);
    CHECK(c.r == 0 && c.g == 0 && c.b == 0);
    for (unsigned h = 0; h < 256; h++) {                          /* never brighter than v, the largest channel is v */
        km_led_hsv((uint8_t)h, 255, 200, &c);
        const unsigned m = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        CHECK_EQ(m, 200);
    }
}

/* ---------------------------------------------------------------- configuration */

static void test_cfg(void)
{
    km_led_t l;
    km_led_init(&l);
    CHECK_EQ(l.cfg.effect, KM_LED_FX_DEMO);
    km_led_cfg_t c;
    km_led_cfg_default(&c);
    CHECK(km_led_cfg_valid(&c));
    c.effect = KM_LED_FX_COUNT;
    CHECK(!km_led_cfg_valid(&c));
    km_led_cfg_default(&c);
    c.speed = 0;
    CHECK(!km_led_cfg_valid(&c));
    const km_led_cfg_t before = l.cfg;
    CHECK(!km_led_set_cfg(&l, &c));
    CHECK(memcmp(&before, &l.cfg, sizeof before) == 0);          /* a rejected config changes nothing */
    c = cfg_of(KM_LED_FX_STATIC, 7, 9, 1, 2, 3);
    CHECK(km_led_set_cfg(&l, &c));
    CHECK(memcmp(&c, &l.cfg, sizeof c) == 0);
}

/* ---------------------------------------------------------------- limits */

static void test_static_and_limits(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];

    /* small colour: no limit bites, only brightness and the channel cap */
    km_led_init(&l);
    km_led_cfg_t c = cfg_of(KM_LED_FX_STATIC, 255, 128, 10, 20, 30);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, px));
    const unsigned ms = (255u * KM_LED_CHANNEL_CAP + 127u) / 255u;
    CHECK_EQ(ms, KM_LED_CHANNEL_CAP);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK_EQ(px[i].r, (10u * ms + 127u) / 255u);
        CHECK_EQ(px[i].g, (20u * ms + 127u) / 255u);
        CHECK_EQ(px[i].b, (30u * ms + 127u) / 255u);
    }

    /* brightness 0 = dark; half brightness is about half */
    c = cfg_of(KM_LED_FX_STATIC, 0, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 1000, NULL, NULL, px));
    CHECK(all_black(px));
    c = cfg_of(KM_LED_FX_STATIC, 128, 128, 40, 0, 0);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 2000, NULL, NULL, px));
    CHECK_EQ(px[0].r, (40u * ((128u * KM_LED_CHANNEL_CAP + 127u) / 255u) + 127u) / 255u);

    /* white at full brightness: first the white limit (per light), then the current budget (all lights) */
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 3000, NULL, NULL, px));
    const unsigned each = KM_LED_CHANNEL_CAP * KM_LED_PIXEL_CAP / (3u * KM_LED_CHANNEL_CAP);      /* 140 * 280 / 420 */
    const unsigned total = KM_LED_COUNT * 3u * each;
    CHECK(total > KM_LED_BUDGET_SUM);                                                              /* so the budget bites */
    const uint32_t scale = (uint32_t)(((uint32_t)KM_LED_BUDGET_SUM << 16) / total);
    CHECK(all_equal(px));
    CHECK_EQ(px[0].r, ((uint32_t)each * scale) >> 16);
    CHECK_EQ(px[0].g, px[0].r);
    CHECK_EQ(px[0].b, px[0].r);
    CHECK_EQ(l.last_scale16, scale);
    unsigned t = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        t += sum_of(&px[i]);
    }
    CHECK(t <= KM_LED_BUDGET_SUM);
    CHECK(t + 3u * KM_LED_COUNT >= KM_LED_BUDGET_SUM * 9u / 10u);                                   /* not wastefully low either */

    /* a pure red light is limited by the channel cap only (and by the budget over 61 of them) */
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 255, 0, 0);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 4000, NULL, NULL, px));
    CHECK(px[0].r <= KM_LED_CHANNEL_CAP && px[0].g == 0 && px[0].b == 0 && px[0].r > 0);
}

/* A property over many random configurations, times, keys and depths: no channel above the cap, no light above the white limit, no
 * frame above the budget, brightness 0 is dark, and the stream always decodes. */
static uint16_t rnd_depth(void *ctx, unsigned slot)
{
    (void)ctx;
    return (uint16_t)((slot * 37u + rnd()) % 400u);               /* includes values above the nominal travel */
}

static void test_limits_property(void)
{
    km_led_t l;
    km_led_init(&l);
    uint32_t now = 0;
    unsigned bad = 0, fx_seen[KM_LED_FX_COUNT] = { 0 };
    for (unsigned n = 0; n < 700; n++) {
        const km_led_cfg_t c = cfg_of(rnd() % KM_LED_FX_COUNT, (rnd() & 3u) == 0 ? (rnd() & 1u ? 255u : 0u) : rnd() % 256u, 1u + rnd() % 255u,
                                      (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd());
        km_led_set_cfg(&l, &c);
        fx_seen[c.effect]++;
        uint8_t keys[8];
        const unsigned nk = rnd() % 8u;
        for (unsigned k = 0; k < nk; k++) {
            keys[k] = (uint8_t)(rnd() % 90u);                     /* includes slots without a light and out of range */
        }
        km_led_note_keys(&l, keys, nk);
        km_led_set_blank(&l, (rnd() % 16u) == 0);
        now += 1u + rnd() % 3000u;
        km_led_rgb_t px[KM_LED_COUNT];
        if (!frame_at(&l, now, rnd_depth, NULL, px)) {
            bad++;
            continue;
        }
        unsigned total = 0;
        for (unsigned i = 0; i < KM_LED_COUNT; i++) {
            const unsigned s = sum_of(&px[i]);
            if (px[i].r > KM_LED_CHANNEL_CAP || px[i].g > KM_LED_CHANNEL_CAP || px[i].b > KM_LED_CHANNEL_CAP || s > KM_LED_PIXEL_CAP) {
                bad++;
            }
            total += s;
        }
        if (total > KM_LED_BUDGET_SUM) {
            bad++;
        }
        if ((c.brightness == 0 || l.blank || c.effect == KM_LED_FX_OFF) && total != 0u) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
    for (unsigned fx = 0; fx < KM_LED_FX_COUNT; fx++) {
        CHECK(fx_seen[fx] > 20);                                  /* every effect was part of the sweep */
    }
}

/* ---------------------------------------------------------------- effects */

static void test_off_blank(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    km_led_init(&l);
    km_led_cfg_t c = cfg_of(KM_LED_FX_OFF, 255, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, px));
    CHECK(all_black(px));
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 100, 100, 100);
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 10, NULL, NULL, px));
    CHECK(!all_black(px));
    km_led_set_blank(&l, true);                                   /* a fault: lights off at the next frame, at once */
    CHECK(frame_at(&l, 20, NULL, NULL, px));
    CHECK(all_black(px));
    km_led_set_blank(&l, false);
    CHECK(frame_at(&l, 30, NULL, NULL, px));
    CHECK(!all_black(px));
}

static void test_breath(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    km_led_init(&l);
    const km_led_cfg_t c = cfg_of(KM_LED_FX_BREATH, 255, 128, 60, 30, 0);          /* speed 128: period 3600 ms (dim, so the current budget does not flatten it) */
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, px));
    CHECK(all_equal(px));
    const unsigned s0 = sum_of(&px[0]);
    CHECK_EQ(s0, 0);                                                           /* dark at the start of a breath */
    CHECK(frame_at(&l, 900, NULL, NULL, px));
    const unsigned s1 = sum_of(&px[0]);
    CHECK(frame_at(&l, 1800, NULL, NULL, px));
    const unsigned s2 = sum_of(&px[0]);
    CHECK(frame_at(&l, 2700, NULL, NULL, px));
    const unsigned s3 = sum_of(&px[0]);
    CHECK(frame_at(&l, 3600, NULL, NULL, px));
    CHECK_EQ(sum_of(&px[0]), s0);                                              /* one period later: the same */
    CHECK(s1 > s0 && s2 > s1 && s2 > s3 && s3 > s0);                           /* up to the middle, down again */
    CHECK(s1 + 3u >= s3 && s3 + 3u >= s1);                                     /* (nearly) symmetric */
}

static void test_rainbow(void)
{
    km_led_t l;
    km_led_rgb_t a[KM_LED_COUNT], b[KM_LED_COUNT], c2[KM_LED_COUNT];
    km_led_init(&l);
    km_led_cfg_t c = cfg_of(KM_LED_FX_RAINBOW, 255, 128, 0, 0, 0);               /* speed 128: one hue cycle in 6750 ms */
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, a));
    CHECK(!all_equal(a));
    unsigned differ = 0;
    for (unsigned i = 1; i < KM_LED_COUNT; i++) {
        differ += (a[i].r != a[0].r || a[i].g != a[0].g || a[i].b != a[0].b);
    }
    CHECK(differ > 50);
    CHECK(frame_at(&l, 3375, NULL, NULL, c2));
    CHECK(memcmp(a, c2, sizeof a) != 0);                                        /* it does move */
    CHECK(frame_at(&l, 6750, NULL, NULL, b));
    CHECK(memcmp(a, b, sizeof a) == 0);                                         /* and it is periodic */

    /* the wave travels to the right: the colour of key x at a later time is the colour of a key further left earlier. Compare the
     * ORDER of two keys of the same row: the hue of the right one lags behind the hue of the left one. */
    c = cfg_of(KM_LED_FX_RAINBOW, 255, 255, 0, 0, 0);                            /* speed 255: cycle 1500 ms */
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, a));
    CHECK(frame_at(&l, 1500, NULL, NULL, b));
    CHECK(memcmp(a, b, sizeof a) == 0);
    /* all lights are lit at full value; at most one channel is 0..cap, none is above the cap */
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK(sum_of(&a[i]) > 0);
    }

    /* the colour order of the hue: key 0 (x = 4) at time 0 has the hue phase8(0) - 8 = 248 (just before red: red with a little blue) */
    CHECK(a[0].r > a[0].g);
}

static void test_reactive(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    km_led_init(&l);
    const km_led_cfg_t c = cfg_of(KM_LED_FX_REACTIVE, 255, 128, 0, 150, 255);     /* speed 128: full to dark in 1035 ms */
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 0, NULL, NULL, px));
    CHECK(all_black(px));                                                       /* nothing pressed: dark */

    const unsigned slot_a = km_led_slot_of_led(10), slot_b = km_led_slot_of_led(40);
    uint8_t keys[3] = { (uint8_t)slot_a, 15 /* no light */, 200 /* no such slot */ };
    km_led_note_keys(&l, keys, 3);
    CHECK(frame_at(&l, 20, NULL, NULL, px));
    CHECK(px[10].g > 0 && px[10].b > 0 && px[10].r == 0);                        /* lit in the configured colour */
    const unsigned first = sum_of(&px[10]);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK(i == 10u || sum_of(&px[i]) == 0u);                               /* nothing else, slots without a light ignored */
    }
    unsigned prev = first, t;
    bool monotone = true;
    for (t = 40; t <= 600; t += 20) {                                           /* released: the glow fades, never brightens */
        CHECK(frame_at(&l, t, NULL, NULL, px));
        const unsigned s = sum_of(&px[10]);
        monotone = monotone && s <= prev;
        prev = s;
    }
    CHECK(monotone);
    CHECK(prev < first && prev > 0);
    for (; t <= 1300; t += 20) {
        CHECK(frame_at(&l, t, NULL, NULL, px));
    }
    CHECK(all_black(px));                                                       /* gone after the fade time */

    /* a key that is held stays lit however long; a quick tap between two frames is still seen */
    for (t = 1320; t < 3000; t += 20) {
        km_led_note_keys(&l, &keys[0], 1);
        CHECK(frame_at(&l, t, NULL, NULL, px));
    }
    CHECK(sum_of(&px[10]) >= first * 9u / 10u);
    uint8_t other = (uint8_t)slot_b;
    km_led_note_keys(&l, &other, 1);
    km_led_note_keys(&l, &other, 0);                                            /* released again before the frame is made */
    CHECK(frame_at(&l, t, NULL, NULL, px));
    CHECK(sum_of(&px[40]) > 0);
}

typedef struct {
    unsigned slot, depth;
} depth_probe_t;

static uint16_t probe_depth(void *ctx, unsigned slot)
{
    const depth_probe_t *p = (const depth_probe_t *)ctx;
    return slot == p->slot ? (uint16_t)p->depth : 0u;
}

static void test_depth(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    km_led_init(&l);
    const km_led_cfg_t c = cfg_of(KM_LED_FX_DEPTH, 255, 128, 0, 0, 0);
    km_led_set_cfg(&l, &c);
    depth_probe_t p = { km_led_slot_of_led(30), 0 };
    uint32_t t = 0;
    unsigned prev_sum = 0;

    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK(px[30].b > 0 && px[30].r == 0 && px[30].g == 0);                      /* up: a faint blue */
    CHECK(all_equal(px));
    const unsigned rest_sum = sum_of(&px[30]);

    p.depth = 19;                                                               /* still the rest zone (sensor noise) */
    t += 20;
    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK_EQ(sum_of(&px[30]), rest_sum);

    bool bright_up = true;
    for (unsigned d = 40; d <= 340; d += 20) {
        p.depth = d;
        t += 20;
        CHECK(frame_at(&l, t, probe_depth, &p, px));
        const unsigned s = px[30].r > px[30].g ? (px[30].r > px[30].b ? px[30].r : px[30].b) : (px[30].g > px[30].b ? px[30].g : px[30].b);
        bright_up = bright_up && s >= prev_sum;                                /* the value (largest channel) only grows */
        prev_sum = s;
        CHECK(sum_of(&px[29]) == rest_sum);                                    /* the other keys stay at rest */
    }
    CHECK(bright_up);

    p.depth = 340;
    t += 20;
    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK(px[30].r > 100 && px[30].g == 0 && px[30].b == 0);                    /* bottom: red */
    p.depth = 1000;                                                             /* over travel: same as the bottom */
    t += 20;
    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK(px[30].r > 100 && px[30].g == 0 && px[30].b == 0);
    p.depth = 180;
    t += 20;
    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK(px[30].g > px[30].r && px[30].g > px[30].b);                          /* in between: green-ish */
    p.depth = 100;
    t += 20;
    CHECK(frame_at(&l, t, probe_depth, &p, px));
    CHECK(px[30].b > px[30].r);                                                 /* upper part still on the blue side */

    CHECK(frame_at(&l, t + 20, NULL, NULL, px));                                /* no depth source: everything at rest */
    CHECK(all_equal(px) && sum_of(&px[0]) == rest_sum);
}

static void test_demo(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    km_led_init(&l);
    km_led_cfg_t c = cfg_of(KM_LED_FX_DEMO, 255, 128, 0, 150, 255);
    km_led_set_cfg(&l, &c);
    depth_probe_t p = { km_led_slot_of_led(5), 340 };

    CHECK(frame_at(&l, 0, probe_depth, &p, px));                                /* 0..8 s: rainbow */
    CHECK(!all_equal(px) && !all_black(px));
    CHECK(frame_at(&l, KM_LED_DEMO_PHASE_MS, probe_depth, &p, px));             /* reactive: dark without a key */
    CHECK(all_black(px));
    CHECK(frame_at(&l, 2 * KM_LED_DEMO_PHASE_MS, probe_depth, &p, px));         /* depth heat */
    CHECK(px[5].r > 100 && px[5].g == 0 && px[5].b == 0);
    CHECK(frame_at(&l, 3 * KM_LED_DEMO_PHASE_MS + 1200, probe_depth, &p, px));  /* breathing: 25200 ms = 7 periods of 3600, dark */
    CHECK(all_black(px));
    CHECK(frame_at(&l, 3 * KM_LED_DEMO_PHASE_MS + 3000, probe_depth, &p, px));  /* 27000 ms = 7.5 periods: at its peak (the budget shows) */
    CHECK(all_equal(px) && sum_of(&px[0]) > 30);
    CHECK(frame_at(&l, 4 * KM_LED_DEMO_PHASE_MS, probe_depth, &p, px));         /* static colour */
    CHECK(all_equal(px) && px[0].g > 0 && px[0].b > 0 && px[0].r == 0);
    CHECK(frame_at(&l, 5 * KM_LED_DEMO_PHASE_MS, probe_depth, &p, px));         /* and round again */
    CHECK(!all_equal(px) && !all_black(px));
}

/* ---------------------------------------------------------------- pipeline: slices, pacing, buffers */

static void test_slicing(void)
{
    /* the frame does not depend on how the work is cut into calls */
    static const unsigned k_units[] = { 1, 3, 8, 61, 122, 1000 };
    for (unsigned fx = 0; fx < KM_LED_FX_COUNT; fx++) {
        uint8_t first[KM_LED_FRAME_BYTES];
        for (unsigned u = 0; u < sizeof k_units / sizeof k_units[0]; u++) {
            km_led_t l;
            km_led_init(&l);
            const km_led_cfg_t c = cfg_of(fx, 200, 90, 30, 200, 90);
            km_led_set_cfg(&l, &c);
            const uint8_t keys[2] = { (uint8_t)km_led_slot_of_led(7), (uint8_t)km_led_slot_of_led(33) };
            km_led_note_keys(&l, keys, 2);
            depth_probe_t p = { km_led_slot_of_led(20), 250 };
            const uint8_t *f = make_frame(&l, 12345u, probe_depth, &p, k_units[u]);
            CHECK(f != NULL);
            if (f == NULL) {
                continue;
            }
            if (u == 0) {
                memcpy(first, f, sizeof first);
            } else {
                CHECK(memcmp(first, f, sizeof first) == 0);
            }
        }
    }
}

static void test_work_bound(void)
{
    km_led_t l;
    km_led_init(&l);
    const km_led_cfg_t c = cfg_of(KM_LED_FX_RAINBOW, 255, 128, 0, 0, 0);
    km_led_set_cfg(&l, &c);
    unsigned total = 0, calls = 0;
    while (!km_led_work(&l, 0, NULL, NULL, 8)) {
        CHECK(l.units <= 8u);
        total += l.units;
        calls++;
        if (calls > 100) {
            break;
        }
    }
    total += l.units;
    CHECK_EQ(total, 2 * KM_LED_COUNT);                            /* one render and one encode unit per light */
    CHECK_EQ(l.units_max, 8);
    CHECK(km_led_work(&l, 0, NULL, NULL, 8));                     /* waiting for the consumer: no further work */
    CHECK_EQ(l.units, 0);
    CHECK_EQ(l.frames, 1);
}

static void test_pacing(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT];
    /* still effect: one frame per 100 ms; moving effect: one per 20 ms */
    for (unsigned pass = 0; pass < 2; pass++) {
        km_led_init(&l);
        const km_led_cfg_t c = cfg_of(pass == 0 ? KM_LED_FX_STATIC : KM_LED_FX_RAINBOW, 255, 128, 50, 50, 50);
        km_led_set_cfg(&l, &c);
        unsigned frames = 0;
        for (uint32_t t = 0; t < 1000; t++) {
            if (km_led_work(&l, t, NULL, NULL, 1000)) {
                frames++;
                km_led_frame_taken(&l);
            }
        }
        CHECK_EQ(frames, pass == 0 ? 10 : 50);
    }

    /* a finished frame is kept, unchanged, until it is taken */
    km_led_init(&l);
    km_led_cfg_t c = cfg_of(KM_LED_FX_STATIC, 255, 128, 90, 0, 0);
    km_led_set_cfg(&l, &c);
    const uint8_t *a = make_frame(&l, 0, NULL, NULL, 1000);
    CHECK(a != NULL);
    uint8_t snap[KM_LED_FRAME_BYTES];
    memcpy(snap, a, sizeof snap);
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 0, 90, 0);
    km_led_set_cfg(&l, &c);
    for (uint32_t t = 100; t < 2000; t += 50) {
        CHECK(km_led_work(&l, t, NULL, NULL, 1000));
        CHECK(km_led_frame(&l) == a);
        CHECK(memcmp(a, snap, sizeof snap) == 0);
    }

    /* the frame handed out is never touched while the next one is made (the DMA may still be reading it) */
    km_led_frame_taken(&l);
    CHECK(km_led_frame(&l) == NULL);
    const uint8_t *b = make_frame(&l, 2000, NULL, NULL, 1000);
    CHECK(b != NULL && b != a);
    CHECK(memcmp(a, snap, sizeof snap) == 0);
    CHECK(decode_frame(b, px) && px[0].g > 0 && px[0].r == 0);                   /* the new colour is in the other buffer */
    km_led_frame_taken(&l);
    const uint8_t *a2 = make_frame(&l, 2200, NULL, NULL, 1000);
    CHECK(a2 == a);                                                              /* buffers alternate */

    /* a new configuration starts a frame at once, even in the middle of a long still interval */
    km_led_frame_taken(&l);
    CHECK(!km_led_work(&l, 2210, NULL, NULL, 1000));                             /* the next still frame is not due yet */
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 0, 0, 90);
    km_led_set_cfg(&l, &c);
    CHECK(km_led_work(&l, 2211, NULL, NULL, 1000));
    CHECK(decode_frame(km_led_frame(&l), px) && px[0].b > 0 && px[0].g == 0);

    /* time wrap of the millisecond clock */
    km_led_init(&l);
    c = cfg_of(KM_LED_FX_RAINBOW, 255, 128, 0, 0, 0);
    km_led_set_cfg(&l, &c);
    unsigned frames = 0;
    for (uint32_t t = 0xFFFFFF00u; t != 0x00000100u; t++) {
        if (km_led_work(&l, t, NULL, NULL, 1000)) {
            frames++;
            km_led_frame_taken(&l);
        }
    }
    CHECK(frames >= 24 && frames <= 26);                                         /* 512 ms at 20 ms */
}

/* ---------------------------------------------------------------- the guard */

static void test_guard_boot_loop(void)
{
    km_led_rec_t rec;
    km_led_guard_t g;
    memset(&rec, 0xA5, sizeof rec);                                              /* garbage, as after a power cycle */
    km_led_guard_boot(&g, &rec, 0x1111u);
    CHECK(km_led_guard_may_start(&g));
    CHECK_EQ(g.strikes, 0);

    /* boot 1 starts the hardware and hangs (watchdog reset): no "stable" was ever recorded */
    km_led_guard_hw_starting(&g, 1500);
    km_led_guard_boot(&g, &rec, 0x1111u);                                        /* boot 2 */
    CHECK(km_led_guard_may_start(&g));
    CHECK_EQ(g.strikes, 1);
    km_led_guard_hw_starting(&g, 1500);
    km_led_guard_boot(&g, &rec, 0x1111u);                                        /* boot 3 */
    CHECK(!km_led_guard_may_start(&g));                                          /* two strikes: LED off for the boot */
    CHECK_EQ(g.why, KM_LED_WHY_BOOT_LOOP);
    CHECK(km_led_guard_step(&g, 0, 0) == KM_LED_KILL);
    km_led_guard_boot(&g, &rec, 0x1111u);                                        /* and it stays off over further resets */
    CHECK(!km_led_guard_may_start(&g));
    km_led_guard_boot(&g, &rec, 0x2222u);                                        /* a new build starts clean */
    CHECK(km_led_guard_may_start(&g));
    CHECK_EQ(g.strikes, 0);

    /* a boot that reaches "stable" clears the strikes */
    km_led_guard_hw_starting(&g, 0);
    km_led_guard_boot(&g, &rec, 0x2222u);                                        /* one strike (that boot did not get there) */
    CHECK_EQ(g.strikes, 1);
    km_led_guard_hw_starting(&g, 100);
    CHECK(km_led_guard_step(&g, 100 + KM_LED_GUARD_STABLE_MS - 1u, 0) == KM_LED_RUN);
    CHECK(!g.stable);
    CHECK(km_led_guard_step(&g, 100 + KM_LED_GUARD_STABLE_MS, 0) == KM_LED_RUN);
    CHECK(g.stable);
    km_led_guard_boot(&g, &rec, 0x2222u);
    CHECK(km_led_guard_may_start(&g));
    CHECK_EQ(g.strikes, 0);

    /* a damaged record counts as no record */
    km_led_guard_hw_starting(&g, 0);
    rec.check ^= 1u;
    km_led_guard_boot(&g, &rec, 0x2222u);
    CHECK(km_led_guard_may_start(&g));
    CHECK_EQ(g.strikes, 0);

    /* without a record the check is skipped */
    km_led_guard_boot(&g, NULL, 1);
    CHECK(km_led_guard_may_start(&g));
    km_led_guard_hw_starting(&g, 0);
    CHECK(km_led_guard_step(&g, KM_LED_GUARD_STABLE_MS, 0) == KM_LED_RUN);

    CHECK(km_led_build_id("Oct 28 2026 12:00:00") != km_led_build_id("Oct 28 2026 12:00:01"));
    CHECK_EQ(km_led_build_id("abc"), km_led_build_id("abc"));
}

static void test_guard_faults(void)
{
    km_led_rec_t rec;
    km_led_guard_t g;
    memset(&rec, 0, sizeof rec);
    km_led_guard_boot(&g, &rec, 7);
    km_led_guard_hw_starting(&g, 0);
    uint32_t t = 0;

    /* a long fault is ONE episode; the LED is paused for as long as it lasts, and "stable" only counts calm time */
    for (unsigned i = 0; i < 5000; i++) {
        t += 1;
        CHECK(km_led_guard_step(&g, t, 0x01) == KM_LED_PAUSE);
    }
    CHECK_EQ(g.episodes, 1);
    t += KM_LED_GUARD_STABLE_MS - 1u;
    CHECK(km_led_guard_step(&g, t, 0) == KM_LED_RUN);
    CHECK(!g.stable);
    t += 1;
    CHECK(km_led_guard_step(&g, t, 0) == KM_LED_RUN);
    CHECK(g.stable);

    /* three separate episodes switch the LED off for the boot */
    CHECK(km_led_guard_step(&g, t + 1, 0x02) == KM_LED_PAUSE);                   /* episode 2 */
    CHECK(km_led_guard_step(&g, t + 2, 0) == KM_LED_RUN);
    CHECK_EQ(g.episodes, 2);
    CHECK(km_led_guard_step(&g, t + 3, 0x10) == KM_LED_KILL);                    /* episode 3 */
    CHECK_EQ(g.why, KM_LED_WHY_FAULTS);
    CHECK(km_led_guard_step(&g, t + 4, 0) == KM_LED_KILL);                       /* a latch */

    /* calm for a minute forgives earlier episodes */
    km_led_guard_boot(&g, NULL, 7);
    km_led_guard_hw_starting(&g, 0);
    CHECK(km_led_guard_step(&g, 10, 1) == KM_LED_PAUSE);
    CHECK(km_led_guard_step(&g, 11, 0) == KM_LED_RUN);
    CHECK(km_led_guard_step(&g, 12, 1) == KM_LED_PAUSE);
    CHECK_EQ(g.episodes, 2);
    CHECK(km_led_guard_step(&g, 13, 0) == KM_LED_RUN);
    CHECK(km_led_guard_step(&g, 13 + KM_LED_GUARD_FORGIVE_MS, 0) == KM_LED_RUN);
    CHECK_EQ(g.episodes, 0);
    CHECK(km_led_guard_step(&g, 14 + KM_LED_GUARD_FORGIVE_MS, 1) == KM_LED_PAUSE);
    CHECK_EQ(g.episodes, 1);

    /* faults before the hardware was started are not episodes of the LED */
    km_led_guard_boot(&g, NULL, 7);
    for (unsigned i = 0; i < 10; i++) {
        (void)km_led_guard_step(&g, i * 2, 1);
        (void)km_led_guard_step(&g, i * 2 + 1, 0);
    }
    CHECK_EQ(g.episodes, 0);
}

static void test_guard_dma(void)
{
    km_led_guard_t g;
    km_led_guard_boot(&g, NULL, 1);
    km_led_guard_hw_starting(&g, 0);
    km_led_guard_dma_result(&g, true);
    km_led_guard_dma_result(&g, true);
    km_led_guard_dma_result(&g, false);                                          /* an ok frame resets the run */
    km_led_guard_dma_result(&g, true);
    km_led_guard_dma_result(&g, true);
    CHECK(km_led_guard_step(&g, 1, 0) == KM_LED_RUN);
    km_led_guard_dma_result(&g, true);                                           /* third in a row */
    CHECK(km_led_guard_step(&g, 2, 0) == KM_LED_KILL);
    CHECK_EQ(g.why, KM_LED_WHY_DMA);

    km_led_guard_boot(&g, NULL, 1);
    km_led_guard_hw_starting(&g, 0);
    km_led_guard_hw_failed(&g);
    CHECK(km_led_guard_step(&g, 1, 0) == KM_LED_KILL);
    CHECK_EQ(g.why, KM_LED_WHY_HW_START);
    km_led_guard_trip(&g, KM_LED_WHY_MANUAL);                                    /* the first reason is kept */
    CHECK_EQ(g.why, KM_LED_WHY_HW_START);
}

/* ---------------------------------------------------------------- idle-off (LED_NOTES.md section 14) */

/* Advance the clock to t and let the pipeline run; true = a new frame came out (decoded into out and handed on). */
static bool tick(km_led_t *l, uint32_t t, km_led_rgb_t out[KM_LED_COUNT])
{
    if (!km_led_work(l, t, NULL, NULL, 1000)) {
        return false;
    }
    const bool ok = decode_frame(km_led_frame(l), out);
    km_led_frame_taken(l);
    CHECK(ok);
    return true;
}

static unsigned frame_sum(const km_led_rgb_t *px)
{
    unsigned s = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        s += px[i].r + px[i].g + px[i].b;
    }
    return s;
}

/* Run from time *t for `ms` milliseconds in 20 ms steps; out holds the latest frame. Returns its channel sum. */
static unsigned run_for(km_led_t *l, uint32_t *t, uint32_t ms, km_led_rgb_t out[KM_LED_COUNT])
{
    unsigned sum = 0;
    const uint32_t end = *t + ms;
    while ((int32_t)(end - *t) > 0) {
        *t += 20u;
        if (tick(l, *t, out)) {
            sum = frame_sum(out);
        }
    }
    return sum;
}

static void test_idle(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT], ref[KM_LED_COUNT];
    uint32_t t = 0;

    /* defaults: ten minutes, full gain, not idle */
    km_led_init(&l);
    CHECK_EQ(KM_LED_IDLE_DEFAULT_S, 600);
    CHECK_EQ(l.idle_s, 600);
    CHECK_EQ(l.gain, KM_LED_IDLE_GAIN_FULL);
    CHECK(!km_led_idle(&l));

    /* a static colour: untouched until the idle time has run out, then a smooth fade to black within the fade time */
    const km_led_cfg_t c = cfg_of(KM_LED_FX_STATIC, 255, 128, 200, 100, 50);
    CHECK(km_led_set_cfg(&l, &c));
    km_led_set_idle(&l, 10);
    const unsigned full = run_for(&l, &t, 200, ref);
    CHECK(full > 0u);
    CHECK_EQ(frame_sum(ref), full);
    for (unsigned i = 0; i < 40u; i++) {                                  /* up to t = 8.2 s: not idle yet */
        run_for(&l, &t, 200, px);
        CHECK(memcmp(px, ref, sizeof px) == 0);
    }
    CHECK(!km_led_idle(&l));
    unsigned prev = full, drops = 0;
    bool monotone = true, any_partial = false;
    while (t < 10000u + KM_LED_IDLE_FADE_OUT_MS + 300u) {                 /* the idle time ends at 10 s */
        t += 20u;
        if (tick(&l, t, px)) {
            const unsigned s = frame_sum(px);
            monotone = monotone && s <= prev;
            drops += (s < prev);
            any_partial = any_partial || (s > 0u && s < full);
            for (unsigned i = 0; i < KM_LED_COUNT; i++) {                 /* the fade only lowers: never above the full-brightness colour */
                CHECK(px[i].r <= ref[i].r && px[i].g <= ref[i].g && px[i].b <= ref[i].b);
            }
            prev = s;
        }
    }
    CHECK(monotone);
    CHECK(any_partial);
    CHECK(drops >= 20u);                                                   /* a fade over many frames, not a jump */
    CHECK_EQ(prev, 0);
    CHECK(km_led_idle(&l));
    CHECK(all_black(px));
    CHECK_EQ(run_for(&l, &t, 5000, px), 0);                                /* and stays dark */

    /* the first key activity brings it back, within the fade-in time plus a frame; a key WITHOUT a light counts too */
    unsigned no_light = 0;
    while (km_led_led_of_slot(no_light) != KM_LED_NONE) {
        no_light++;
    }
    const uint8_t dark_key[1] = { (uint8_t)no_light };
    km_led_note_keys(&l, dark_key, 1);
    (void)run_for(&l, &t, 40u, px);
    CHECK(km_led_idle(&l));                                                /* coming back, not yet at full: still reported */
    CHECK(frame_sum(px) > 0u && frame_sum(px) < full);
    const unsigned back = run_for(&l, &t, KM_LED_IDLE_FADE_IN_MS + 20u, px);
    CHECK_EQ(back, full);
    CHECK(memcmp(px, ref, sizeof px) == 0);
    CHECK(!km_led_idle(&l));

    /* ... and the idle clock starts again from that key */
    run_for(&l, &t, 9000u, px);
    CHECK(memcmp(px, ref, sizeof px) == 0);
    run_for(&l, &t, 1000u + KM_LED_IDLE_FADE_OUT_MS + 200u, px);
    CHECK(all_black(px));

    /* a key held down keeps the lights on for ever */
    const uint8_t held[1] = { (uint8_t)km_led_slot_of_led(5) };
    for (unsigned i = 0; i < 400u; i++) {                                  /* 40 s, a key down in every pass */
        km_led_note_keys(&l, held, 1);
        run_for(&l, &t, 100u, px);
    }
    CHECK_EQ(frame_sum(px), full);
    CHECK(!km_led_idle(&l));

    /* the host's wake (a LED_SET) restarts the clock without a key */
    run_for(&l, &t, 9000u, px);
    km_led_wake(&l);
    run_for(&l, &t, 9000u, px);
    CHECK_EQ(frame_sum(px), full);
    run_for(&l, &t, 1500u + KM_LED_IDLE_FADE_OUT_MS, px);
    CHECK(all_black(px));

    /* 0 = never: a latched idle state is released at once and nothing fades any more */
    km_led_set_idle(&l, 0);
    run_for(&l, &t, 1000u, px);
    CHECK_EQ(frame_sum(px), full);
    CHECK(!km_led_idle(&l));
    for (unsigned i = 0; i < 60u; i++) {                                   /* a simulated hour in 60 s jumps (the frame time step is clamped) */
        t += 60000u;
        (void)tick(&l, t, px);
        (void)tick(&l, t + 20u, px);
    }
    CHECK_EQ(frame_sum(px), full);
    CHECK(!km_led_idle(&l));
    /* a new time shorter than the quiet time already behind us acts at once */
    km_led_set_idle(&l, 5);
    run_for(&l, &t, 5000u + KM_LED_IDLE_FADE_OUT_MS + 200u, px);
    CHECK(all_black(px));
    km_led_set_idle(&l, 600);                                              /* an hour has passed since the last key: still idle */
    run_for(&l, &t, 600u, px);
    CHECK(all_black(px));
    km_led_set_idle(&l, 5000);                                             /* longer than the quiet time so far (about 63 minutes): back on */
    run_for(&l, &t, 600u, px);
    CHECK_EQ(frame_sum(px), full);

    /* animated effects fade too */
    km_led_init(&l);
    t = 0;
    const km_led_cfg_t rb = cfg_of(KM_LED_FX_RAINBOW, 255, 200, 0, 0, 0);
    CHECK(km_led_set_cfg(&l, &rb));
    km_led_set_idle(&l, 3);
    run_for(&l, &t, 2900u, px);
    CHECK(frame_sum(px) > 0u);
    run_for(&l, &t, 100u + KM_LED_IDLE_FADE_OUT_MS + 200u, px);
    CHECK(all_black(px));
    km_led_wake(&l);
    run_for(&l, &t, 400u, px);
    CHECK(frame_sum(px) > 0u);

    /* the hard limits still hold during the whole fade (property over every frame) */
    km_led_init(&l);
    t = 0;
    const km_led_cfg_t wh = cfg_of(KM_LED_FX_STATIC, 255, 128, 255, 255, 255);
    CHECK(km_led_set_cfg(&l, &wh));
    km_led_set_idle(&l, 1);
    for (unsigned i = 0; i < 400u; i++) {
        t += 20u;
        if (tick(&l, t, px)) {
            CHECK(frame_sum(px) <= KM_LED_BUDGET_SUM);
            for (unsigned k = 0; k < KM_LED_COUNT; k++) {
                CHECK(px[k].r <= KM_LED_CHANNEL_CAP && px[k].g <= KM_LED_CHANNEL_CAP && px[k].b <= KM_LED_CHANNEL_CAP);
                CHECK((unsigned)px[k].r + px[k].g + px[k].b <= KM_LED_PIXEL_CAP);
            }
        }
    }

    /* the idle clock starts with the first frame, not at time 0 (the lights start 1.5 s or more after the processor) */
    km_led_init(&l);
    CHECK(km_led_set_cfg(&l, &c));
    km_led_set_idle(&l, 10);
    t = 100000u;
    const unsigned lit = run_for(&l, &t, 200u, px);
    CHECK(lit > 0u);
    run_for(&l, &t, 8000u, px);
    CHECK_EQ(frame_sum(px), lit);
    CHECK(!km_led_idle(&l));
    run_for(&l, &t, 2000u + KM_LED_IDLE_FADE_OUT_MS + 200u, px);
    CHECK(all_black(px));

    /* the millisecond clock wraps after 49 days: the idle test is a subtraction, so it still fires across the wrap */
    km_led_init(&l);
    CHECK(km_led_set_cfg(&l, &c));
    km_led_set_idle(&l, 1);
    t = 0xFFFFFF00u;
    const unsigned wlit = run_for(&l, &t, 100u, px);                       /* the first frame is near the end of the range */
    CHECK(wlit > 0u);
    run_for(&l, &t, 600u, px);                                             /* crosses 2^32 after 256 ms; 700 ms of the 1000 ms are over: lit */
    CHECK(t < 0x10000u);
    CHECK_EQ(frame_sum(px), wlit);
    CHECK(!km_led_idle(&l));
    run_for(&l, &t, 2800u, px);                                            /* crosses 2^32, the idle time (1 s) and the fade (2 s) pass */
    CHECK(t < 0x10000u);
    CHECK(all_black(px));
    CHECK(km_led_idle(&l));
}

/* ---------------------------------------------------------------- layers (M6 P6c): scene, indicators, external frames */

/* Reference of the stages after the layers, written from the documented formulas (km_led.h): brightness and the channel cap in one factor, the
 * white limit, then the current budget; idle gain full. raw = what the layers produced (pre-limit). */
static void expect_frame(const km_led_rgb_t raw[KM_LED_COUNT], unsigned brightness, km_led_rgb_t out[KM_LED_COUNT])
{
    const unsigned ms = (brightness * KM_LED_CHANNEL_CAP + 127u) / 255u;
    unsigned total = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        unsigned r = (raw[i].r * ms + 127u) / 255u, g = (raw[i].g * ms + 127u) / 255u, b = (raw[i].b * ms + 127u) / 255u;
        unsigned sum = r + g + b;
        if (sum > KM_LED_PIXEL_CAP) {
            r = r * KM_LED_PIXEL_CAP / sum;
            g = g * KM_LED_PIXEL_CAP / sum;
            b = b * KM_LED_PIXEL_CAP / sum;
            sum = r + g + b;
        }
        out[i].r = (uint8_t)r;
        out[i].g = (uint8_t)g;
        out[i].b = (uint8_t)b;
        total += sum;
    }
    if (total > KM_LED_BUDGET_SUM) {
        const uint32_t scale16 = ((uint32_t)KM_LED_BUDGET_SUM << 16) / total;
        for (unsigned i = 0; i < KM_LED_COUNT; i++) {
            out[i].r = (uint8_t)(((uint32_t)out[i].r * scale16) >> 16);
            out[i].g = (uint8_t)(((uint32_t)out[i].g * scale16) >> 16);
            out[i].b = (uint8_t)(((uint32_t)out[i].b * scale16) >> 16);
        }
    }
}

static bool frames_equal(const km_led_rgb_t *a, const km_led_rgb_t *b)
{
    return memcmp(a, b, KM_LED_COUNT * sizeof a[0]) == 0;
}

static void fill_raw(km_led_rgb_t raw[KM_LED_COUNT], uint8_t r, uint8_t g, uint8_t b)
{
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        raw[i].r = r;
        raw[i].g = g;
        raw[i].b = b;
    }
}

static unsigned scale_ref(unsigned v, unsigned s)
{
    return (v * s + 127u) / 255u;
}

/* One frame at time t of core `l`, compared with the reference of `raw` at the core's brightness. */
static bool frame_is(km_led_t *l, uint32_t t, const km_led_rgb_t raw[KM_LED_COUNT])
{
    km_led_rgb_t got[KM_LED_COUNT], want[KM_LED_COUNT];
    if (!frame_at(l, t, NULL, NULL, got)) {
        return false;
    }
    expect_frame(raw, l->cfg.brightness, want);
    return frames_equal(got, want);
}

static km_ls_rule_t mk_rule(unsigned src, unsigned param, unsigned led, unsigned flags, uint8_t r, uint8_t g, uint8_t b, unsigned dim)
{
    km_ls_rule_t x;
    x.source = (uint8_t)src;
    x.param = (uint8_t)param;
    x.led = (uint8_t)led;
    x.flags = (uint8_t)flags;
    x.on.r = r;
    x.on.g = g;
    x.on.b = b;
    x.dim = (uint8_t)dim;
    return x;
}

static km_led_inputs_t mk_in(unsigned host, unsigned layer, unsigned profile, unsigned flags)
{
    km_led_inputs_t i;
    i.host_leds = (uint8_t)host;
    i.layer = (uint8_t)layer;
    i.profile = (uint8_t)profile;
    i.flags = (uint8_t)flags;
    return i;
}

static void set_in(km_led_t *l, km_led_inputs_t v)
{
    km_led_set_inputs(l, &v);
}

static void test_layers_default_and_custom(void)
{
    km_led_t l;
    km_lightscene_t sc, dflt;
    km_led_rgb_t raw[KM_LED_COUNT];

    /* the core's built-in scene is the documented default */
    km_led_init(&l);
    km_lightscene_default(&dflt);
    uint8_t a[KM_LS_MAX_ENCODED], b[KM_LS_MAX_ENCODED];
    CHECK_EQ(km_lightscene_encode(l.scene, a, sizeof a), km_lightscene_encode(&dflt, b, sizeof b));
    CHECK(memcmp(a, b, 150) == 0);
    CHECK_EQ(KM_LED_FX_CUSTOM, 7);                                              /* the wire numbers of the new effects */
    CHECK_EQ(KM_LED_FX_EXTERNAL, 8);
    CHECK_EQ(KM_LED_FX_ANIM, 9);
    CHECK_EQ(KM_LED_FX_COUNT, 10);
    km_led_cfg_t c = cfg_of(KM_LED_FX_CUSTOM, 255, 128, 1, 2, 3);
    CHECK(km_led_set_cfg(&l, &c));
    fill_raw(raw, 0, 150, 255);
    CHECK(frame_is(&l, 10, raw));                                               /* default scene: every light on palette colour 0 */
    c.effect = (uint8_t)(KM_LED_FX_COUNT - 1u);
    CHECK(km_led_cfg_valid(&c));
    c.effect = KM_LED_FX_COUNT;
    CHECK(!km_led_cfg_valid(&c));

    /* a colour per key */
    km_lightscene_default(&sc);
    sc.palette[3] = (km_led_rgb_t){ 10, 20, 30 };
    sc.palette[4] = (km_led_rgb_t){ 30, 0, 0 };
    sc.palette[15] = (km_led_rgb_t){ 0, 0, 40 };
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        sc.key_idx[i] = (uint8_t)(i % 5u == 0 ? 3u : i % 5u == 1 ? 4u : i % 5u == 2 ? 15u : 0u);
        raw[i] = sc.palette[sc.key_idx[i]];
    }
    sc.palette[0] = (km_led_rgb_t){ 5, 5, 5 };
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        raw[i] = sc.palette[sc.key_idx[i]];
    }
    km_led_set_scene(&l, &sc);
    CHECK(frame_is(&l, 20, raw));
    c.effect = KM_LED_FX_CUSTOM;
    c.brightness = 100;
    CHECK(km_led_set_cfg(&l, &c));
    CHECK(frame_is(&l, 30, raw));                                               /* brightness scales the palette colours down */
    sc.key_idx[7] = 4;                                                          /* a change in the caller's scene shows from the next frame on */
    raw[7] = sc.palette[4];
    km_led_set_cfg(&l, &c);
    CHECK(frame_is(&l, 40, raw));
    km_led_set_scene(&l, NULL);                                                 /* NULL = back to the default scene */
    fill_raw(raw, 0, 150, 255);
    CHECK(frame_is(&l, 50, raw));
    CHECK(l.scene != &sc);

    /* custom is a still effect: slow frame pace; a bright palette still obeys all three limits */
    km_led_set_scene(&l, &sc);
    for (unsigned i = 0; i < 16; i++) {
        sc.palette[i] = (km_led_rgb_t){ 255, 255, 255 };
    }
    c.brightness = 255;
    km_led_set_cfg(&l, &c);
    km_led_rgb_t px[KM_LED_COUNT];
    CHECK(frame_at(&l, 60, NULL, NULL, px));
    unsigned total = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK(px[i].r <= KM_LED_CHANNEL_CAP && px[i].g <= KM_LED_CHANNEL_CAP && px[i].b <= KM_LED_CHANNEL_CAP);
        CHECK(sum_of(&px[i]) <= KM_LED_PIXEL_CAP);
        total += sum_of(&px[i]);
    }
    CHECK(total <= KM_LED_BUDGET_SUM);
    CHECK(total > KM_LED_BUDGET_SUM - 200u);                                    /* all-white palette: the budget is what limits it */
    CHECK_EQ(l.next_frame_ms - l.last_frame_ms, KM_LED_FRAME_MS_STATIC);
}

static void test_layers_indicators(void)
{
    km_led_t l;
    km_lightscene_t sc;
    km_led_rgb_t raw[KM_LED_COUNT];
    km_led_cfg_t c = cfg_of(KM_LED_FX_STATIC, 255, 128, 0, 0, 20);
    uint32_t t = 0;

    /* every source, condition false and true; the base is a dim static blue so the current budget does not interfere */
    struct { unsigned src, param; km_led_inputs_t on, off; } k_src[] = {
        { KM_LS_SRC_HOST_NUM, 0, mk_in(1, 0, 0, 0), mk_in(6, 0, 0, 0) },
        { KM_LS_SRC_HOST_CAPS, 0, mk_in(2, 0, 0, 0), mk_in(5, 0, 0, 0) },
        { KM_LS_SRC_HOST_SCROLL, 0, mk_in(4, 0, 0, 0), mk_in(3, 0, 0, 0) },
        { KM_LS_SRC_LAYER, 2, mk_in(0, 2, 0, 0), mk_in(0, 1, 0, 0) },
        { KM_LS_SRC_LAYER, 0, mk_in(0, 0, 3, 0), mk_in(0, 3, 3, 0) },
        { KM_LS_SRC_PROFILE, 3, mk_in(0, 0, 3, 0), mk_in(0, 0, 2, 0) },
        { KM_LS_SRC_PAD, 0, mk_in(0, 0, 0, KM_LED_IN_PAD), mk_in(0, 0, 0, KM_LED_IN_TOGGLE) },
        { KM_LS_SRC_TOGGLE, 0, mk_in(0, 0, 0, KM_LED_IN_TOGGLE), mk_in(0, 0, 0, KM_LED_IN_PAD) },
    };
    for (unsigned k = 0; k < sizeof k_src / sizeof k_src[0]; k++) {
        km_lightscene_default(&sc);
        sc.n_rules = 1;
        sc.rule[0] = mk_rule(k_src[k].src, k_src[k].param, 28, KM_LS_OFF_KEEP, 60, 10, 5, 0);
        CHECK(km_lightscene_valid(&sc));
        km_led_init(&l);
        km_led_set_scene(&l, &sc);
        CHECK(km_led_set_cfg(&l, &c));
        fill_raw(raw, 0, 0, 20);
        km_led_set_inputs(&l, &k_src[k].off);
        CHECK(frame_is(&l, t += 200, raw));                                    /* condition false: the base only */
        raw[28] = sc.rule[0].on;
        km_led_set_inputs(&l, &k_src[k].on);
        CHECK(frame_is(&l, t += 200, raw));                                    /* condition true: the rule's colour on light 28 only */
        fill_raw(raw, 0, 0, 20);
        km_led_set_inputs(&l, &k_src[k].off);
        CHECK(frame_is(&l, t += 200, raw));
    }
    {   /* the host LED byte: bits above Scroll are not a state (no change, no wake) */
        km_lightscene_default(&sc);
        sc.n_rules = 1;
        sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 3, KM_LS_OFF_KEEP, 60, 10, 5, 0);
        km_led_init(&l);
        km_led_set_scene(&l, &sc);
        km_led_set_cfg(&l, &c);
        km_led_set_inputs(&l, &(km_led_inputs_t){ 0xF8, 0, 0, 0xFC });
        CHECK_EQ(l.in.host_leds, 0);
        CHECK_EQ(l.in.flags, 0);
        km_led_set_inputs(&l, &(km_led_inputs_t){ 0xFA, 0, 0, 0 });
        CHECK_EQ(l.in.host_leds, 2);
    }

    /* what the light does while the condition is false: keep, black, dim */
    for (unsigned mode = 0; mode < 3; mode++) {
        km_lightscene_default(&sc);
        sc.n_rules = 1;
        sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 5, mode, 60, 10, 5, mode == KM_LS_OFF_DIM ? 100 : 0);
        km_led_init(&l);
        km_led_set_scene(&l, &sc);
        c.color.r = 25;
        c.color.g = 30;
        km_led_set_cfg(&l, &c);
        set_in(&l, mk_in(0, 0, 0, 0));
        fill_raw(raw, 25, 30, 20);
        if (mode == KM_LS_OFF_BLACK) {
            raw[5] = (km_led_rgb_t){ 0, 0, 0 };
        } else if (mode == KM_LS_OFF_DIM) {
            raw[5].r = (uint8_t)scale_ref(25, 100);
            raw[5].g = (uint8_t)scale_ref(30, 100);
            raw[5].b = (uint8_t)scale_ref(20, 100);
        }
        CHECK(frame_is(&l, t += 200, raw));
        set_in(&l, mk_in(2, 0, 0, 0));
        raw[5] = sc.rule[0].on;
        CHECK(frame_is(&l, t += 200, raw));
    }
    c.color.r = 0;
    c.color.g = 0;

    /* blinking: on for 500 ms, off for 500 ms (the frame clock), only while the condition is true; the off phase uses the off behaviour */
    for (unsigned mode = 0; mode < 2; mode++) {
        km_lightscene_default(&sc);
        sc.n_rules = 1;
        sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 9, mode | KM_LS_FLAG_BLINK, 60, 10, 5, 0);
        static const struct { uint32_t t; bool on; } k_ph[] = { { 0, true }, { 499, true }, { 500, false }, { 999, false }, { 1000, true }, { 1499, true }, { 1500, false }, { 2000, true } };
        for (unsigned i = 0; i < sizeof k_ph / sizeof k_ph[0]; i++) {
            km_led_init(&l);                                         /* a fresh core per instant, so every frame is the first of its core */
            km_led_set_scene(&l, &sc);
            km_led_set_cfg(&l, &c);
            set_in(&l, mk_in(2, 0, 0, 0));
            fill_raw(raw, 0, 0, 20);
            if (k_ph[i].on) {
                raw[9] = sc.rule[0].on;
            } else if (mode == KM_LS_OFF_BLACK) {
                raw[9] = (km_led_rgb_t){ 0, 0, 0 };
            }
            CHECK(frame_is(&l, 100000u + k_ph[i].t, raw));
        }
        km_led_init(&l);
        km_led_set_scene(&l, &sc);
        km_led_set_cfg(&l, &c);
        set_in(&l, mk_in(0, 0, 0, 0));                              /* condition false: never the colour, whatever the phase */
        fill_raw(raw, 0, 0, 20);
        if (mode == KM_LS_OFF_BLACK) {
            raw[9] = (km_led_rgb_t){ 0, 0, 0 };
        }
        CHECK(frame_is(&l, 100000u + 3000u, raw));
        CHECK(frame_is(&l, 100000u + 3500u, raw));
    }
    {   /* a blinking indicator needs the fast frame pace even over a still effect */
        km_lightscene_default(&sc);
        sc.n_rules = 1;
        sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 9, KM_LS_FLAG_BLINK, 60, 10, 5, 0);
        km_led_init(&l);
        km_led_set_scene(&l, &sc);
        km_led_set_cfg(&l, &c);
        set_in(&l, mk_in(0, 0, 0, 0));
        km_led_rgb_t pp[KM_LED_COUNT];
        CHECK(frame_at(&l, 1, NULL, NULL, pp));
        CHECK_EQ(l.next_frame_ms - l.last_frame_ms, KM_LED_FRAME_MS_STATIC);
        set_in(&l, mk_in(2, 0, 0, 0));
        CHECK(frame_at(&l, 2, NULL, NULL, pp));
        CHECK_EQ(l.next_frame_ms - l.last_frame_ms, KM_LED_FRAME_MS_ANIM);
    }

    /* several rules for one light: a shown colour beats an off behaviour; the first shown rule wins; else the first black/dim rule decides */
    km_lightscene_default(&sc);
    sc.n_rules = 3;
    sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 10, KM_LS_OFF_BLACK, 50, 0, 0, 0);
    sc.rule[1] = mk_rule(KM_LS_SRC_HOST_NUM, 0, 10, KM_LS_OFF_DIM, 0, 50, 0, 128);
    sc.rule[2] = mk_rule(KM_LS_SRC_HOST_SCROLL, 0, 10, KM_LS_OFF_KEEP, 0, 0, 50, 0);
    km_led_init(&l);
    km_led_set_scene(&l, &sc);
    km_led_set_cfg(&l, &c);
    for (unsigned h = 0; h < 8; h++) {
        set_in(&l, mk_in(h, 0, 0, 0));
        fill_raw(raw, 0, 0, 20);
        if (h & 2u) {
            raw[10] = sc.rule[0].on;                                            /* Caps shown: rule 0 */
        } else if (h & 1u) {
            raw[10] = sc.rule[1].on;                                            /* Num shown (rule 0 would black it out, but a shown colour beats that) */
        } else if (h & 4u) {
            raw[10] = sc.rule[2].on;
        } else {
            raw[10] = (km_led_rgb_t){ 0, 0, 0 };                                /* nothing shown: the first off behaviour (rule 0, black) decides */
        }
        CHECK(frame_is(&l, t += 200, raw));
    }

    /* a fault shows nothing, not even an indicator; effect "off" still shows an indicator; brightness 0 shows nothing */
    km_lightscene_default(&sc);
    sc.n_rules = 1;
    sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 28, KM_LS_OFF_KEEP, 60, 10, 5, 0);
    km_led_init(&l);
    km_led_set_scene(&l, &sc);
    km_led_set_cfg(&l, &c);
    set_in(&l, mk_in(2, 0, 0, 0));
    km_led_set_blank(&l, true);
    km_led_rgb_t px[KM_LED_COUNT];
    CHECK(frame_at(&l, t += 200, NULL, NULL, px));
    CHECK(all_black(px));
    km_led_set_blank(&l, false);
    c.effect = KM_LED_FX_OFF;
    km_led_set_cfg(&l, &c);
    fill_raw(raw, 0, 0, 0);
    raw[28] = sc.rule[0].on;
    CHECK(frame_is(&l, t += 200, raw));
    c.effect = KM_LED_FX_STATIC;
    c.brightness = 0;
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, t += 200, NULL, NULL, px));
    CHECK(all_black(px));

    /* the brightness setting scales the indicator like everything else */
    c.brightness = 128;
    km_led_set_cfg(&l, &c);
    fill_raw(raw, 0, 0, 20);
    raw[28] = sc.rule[0].on;
    CHECK(frame_is(&l, t += 200, raw));

    /* the three limits hold for the loudest possible rules: all eight lit white, on a white base, full brightness */
    km_lightscene_default(&sc);
    sc.n_rules = KM_LS_MAX_RULES;
    for (unsigned i = 0; i < KM_LS_MAX_RULES; i++) {
        sc.rule[i] = mk_rule(KM_LS_SRC_HOST_NUM + (i % 3u), 0, i * 7u, 0, 255, 255, 255, 0);
    }
    km_led_init(&l);
    km_led_set_scene(&l, &sc);
    c = cfg_of(KM_LED_FX_STATIC, 255, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    set_in(&l, mk_in(7, 0, 0, 0));
    CHECK(frame_at(&l, t += 200, NULL, NULL, px));
    unsigned total = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK(px[i].r <= KM_LED_CHANNEL_CAP && px[i].g <= KM_LED_CHANNEL_CAP && px[i].b <= KM_LED_CHANNEL_CAP);
        CHECK(sum_of(&px[i]) <= KM_LED_PIXEL_CAP);
        total += sum_of(&px[i]);
    }
    CHECK(total <= KM_LED_BUDGET_SUM);

    /* an invalid rule table in force (a scene that was not validated) can not read outside the array */
    km_lightscene_default(&sc);
    sc.n_rules = 200;
    for (unsigned i = 0; i < KM_LS_MAX_RULES; i++) {
        sc.rule[i] = mk_rule(0, 0, 4, KM_LS_OFF_KEEP, 255, 0, 0, 0);           /* source 0 is nothing: never true, so light 4 stays like its neighbour */
    }
    sc.rule[3].source = 77;
    km_led_set_scene(&l, &sc);
    CHECK(frame_at(&l, t += 200, NULL, NULL, px));
    CHECK(l.nrules == KM_LS_MAX_RULES);
    CHECK_EQ(px[4].r, px[5].r);
    CHECK_EQ(px[4].g, px[5].g);
}

static void test_layers_idle_and_wake(void)
{
    km_led_t l;
    km_lightscene_t sc;
    km_led_rgb_t px[KM_LED_COUNT];
    uint32_t t = 0;
    km_led_init(&l);
    km_lightscene_default(&sc);
    sc.n_rules = 1;
    sc.rule[0] = mk_rule(KM_LS_SRC_HOST_CAPS, 0, 28, KM_LS_OFF_KEEP, 60, 10, 5, 0);
    km_led_set_scene(&l, &sc);
    const km_led_cfg_t c = cfg_of(KM_LED_FX_STATIC, 255, 128, 0, 0, 20);
    km_led_set_cfg(&l, &c);
    km_led_set_idle(&l, 2);
    set_in(&l, mk_in(2, 0, 0, 0));
    t = 100000u;
    run_for(&l, &t, 100u, px);
    CHECK(sum_of(&px[28]) > 0u);
    run_for(&l, &t, 2000u + KM_LED_IDLE_FADE_OUT_MS + 300u, px);
    CHECK(all_black(px));                                                      /* the indicator fades out with everything else */
    CHECK(km_led_idle(&l));
    set_in(&l, mk_in(2, 0, 0, 0));                                 /* the same state again: not activity */
    run_for(&l, &t, 200u, px);
    CHECK(all_black(px));
    set_in(&l, mk_in(0, 0, 0, 0));                                 /* the host switched Caps Lock off: look at the keyboard */
    run_for(&l, &t, KM_LED_IDLE_FADE_IN_MS + 100u, px);
    CHECK(!all_black(px));
    CHECK(!km_led_idle(&l));
    run_for(&l, &t, 2000u + KM_LED_IDLE_FADE_OUT_MS + 300u, px);
    CHECK(all_black(px));
    set_in(&l, mk_in(0, 1, 0, 0));                                 /* a layer change wakes them too */
    run_for(&l, &t, KM_LED_IDLE_FADE_IN_MS + 100u, px);
    CHECK(!all_black(px));
}

static void test_layers_external(void)
{
    km_led_t l;
    km_led_rgb_t px[KM_LED_COUNT], raw[KM_LED_COUNT], fr[KM_LED_COUNT];
    uint8_t gray[KM_LED_COUNT];
    km_led_cfg_t c = cfg_of(KM_LED_FX_EXTERNAL, 255, 128, 200, 100, 50);
    uint32_t t = 1000;
    km_led_init(&l);
    km_led_set_idle(&l, 0);                                                     /* never fade out: these tests jump over hours */
    km_led_set_cfg(&l, &c);

    /* effect 8 without a frame is dark */
    CHECK(frame_at(&l, t += 200, NULL, NULL, px));
    CHECK(all_black(px));
    CHECK(!km_led_ext_valid(&l, KM_LED_EXT_STREAM));

    /* a stream frame is drawn, pre-limit, through brightness and the limits */
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        fr[i].r = (uint8_t)(i % 7u * 3u);
        fr[i].g = (uint8_t)(i % 5u * 4u);
        fr[i].b = (uint8_t)(i % 3u * 5u);
    }
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, t);
    CHECK(km_led_ext_valid(&l, KM_LED_EXT_STREAM));
    CHECK(frame_is(&l, t + 20, fr));
    fr[0].r = 99;                                                               /* the core kept a COPY: later changes of the caller's array are not seen */
    CHECK(frame_at(&l, t + 40, NULL, NULL, px));
    CHECK_EQ(px[0].r, 0);
    fr[0].r = 0;
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, t + 50);

    /* brightness acts after the layers */
    c.brightness = 100;
    km_led_set_cfg(&l, &c);
    CHECK(frame_is(&l, t + 200, fr));
    c.brightness = 255;
    km_led_set_cfg(&l, &c);

    /* it expires KM_LED_EXT_TIMEOUT_MS after the latest push (valid at exactly the timeout, gone one millisecond later) */
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 5000);
    CHECK(frame_is(&l, 5000u + KM_LED_EXT_TIMEOUT_MS, fr));
    CHECK(km_led_ext_valid(&l, KM_LED_EXT_STREAM));
    CHECK(frame_at(&l, 5000u + KM_LED_EXT_TIMEOUT_MS + 21u, NULL, NULL, px));
    CHECK(all_black(px));
    CHECK(!km_led_ext_valid(&l, KM_LED_EXT_STREAM));
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 9000);                             /* a new frame brings it back */
    CHECK(frame_is(&l, 9020, fr));
    km_led_ext_end(&l, KM_LED_EXT_STREAM);                                      /* and the sender can end it at once */
    CHECK(frame_at(&l, 9040, NULL, NULL, px));
    CHECK(all_black(px));

    /* the clock wraps: a push shortly before 2^32 is still valid shortly after (a fresh core, whose frame clock starts there) */
    {
        km_led_t w;
        km_led_init(&w);
        km_led_set_idle(&w, 0);
        km_led_set_cfg(&w, &c);
        km_led_ext_rgb(&w, KM_LED_EXT_STREAM, fr, 0xFFFFFF00u);
        CHECK(frame_is(&w, 0xFFFFFF00u + 20u, fr));
        CHECK(frame_is(&w, 100u, fr));                                         /* 356 ms later */
        CHECK(frame_at(&w, 100u + KM_LED_EXT_TIMEOUT_MS, NULL, NULL, px));
        CHECK(all_black(px));
    }

    /* the other effects ignore the stream; effect 9 ignores it too */
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 20000);
    c.effect = KM_LED_FX_STATIC;
    c.color = (km_led_rgb_t){ 0, 0, 20 };
    km_led_set_cfg(&l, &c);
    fill_raw(raw, 0, 0, 20);
    CHECK(frame_is(&l, 20020, raw));
    c.effect = KM_LED_FX_ANIM;
    km_led_set_cfg(&l, &c);
    CHECK(frame_at(&l, 20040, NULL, NULL, px));
    CHECK(all_black(px));

    /* the animation player's grey frame (effect 9): tinted with the colour, no expiry, ended by the player */
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        gray[i] = (uint8_t)(i * 4u);
    }
    c.color = (km_led_rgb_t){ 100, 60, 20 };
    c.brightness = 200;
    km_led_set_cfg(&l, &c);
    km_led_ext_gray(&l, KM_LED_EXT_ANIM, gray, 30000);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        raw[i].r = (uint8_t)scale_ref(100, gray[i]);
        raw[i].g = (uint8_t)scale_ref(60, gray[i]);
        raw[i].b = (uint8_t)scale_ref(20, gray[i]);
    }
    CHECK(frame_is(&l, 30020, raw));
    CHECK(frame_is(&l, 30000u + 3600000u, raw));                                 /* an hour later: a still picture stays */
    c.color = (km_led_rgb_t){ 20, 60, 100 };                                    /* the tint follows the configuration */
    km_led_set_cfg(&l, &c);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        raw[i].r = (uint8_t)scale_ref(20, gray[i]);
        raw[i].g = (uint8_t)scale_ref(60, gray[i]);
        raw[i].b = (uint8_t)scale_ref(100, gray[i]);
    }
    CHECK(frame_is(&l, 30000u + 3600000u + 20u, raw));
    km_led_ext_end(&l, KM_LED_EXT_ANIM);
    CHECK(frame_at(&l, 30000u + 3600000u + 40u, NULL, NULL, px));
    CHECK(all_black(px));
    km_led_ext_rgb(&l, KM_LED_EXT_ANIM, fr, 31000000u);                         /* the animation source also takes colours */
    CHECK(frame_is(&l, 31000020u, fr));
    CHECK(km_led_ext_valid(&l, KM_LED_EXT_ANIM));

    /* bad calls change nothing */
    km_led_ext_end(&l, KM_LED_EXT_ANIM);
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 31000100u);
    CHECK(km_led_ext_valid(&l, KM_LED_EXT_STREAM));
    CHECK(!km_led_ext_valid(&l, KM_LED_EXT_SOURCES));                           /* out of range is never valid, whatever source 0 holds */
    CHECK(!km_led_ext_valid(&l, 4));
    km_led_ext_end(&l, KM_LED_EXT_STREAM);
    km_led_ext_rgb(&l, KM_LED_EXT_SOURCES, fr, 1);
    km_led_ext_gray(&l, 77, gray, 1);
    km_led_ext_rgb(&l, KM_LED_EXT_ANIM, NULL, 1);
    km_led_ext_gray(&l, KM_LED_EXT_ANIM, NULL, 1);
    CHECK(!km_led_ext_valid(&l, KM_LED_EXT_ANIM));
    CHECK(!km_led_ext_valid(&l, KM_LED_EXT_SOURCES));
    km_led_ext_end(&l, 5);

    /* an external frame keeps the limits whatever it holds: all lights 255 white, full brightness */
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        fr[i] = (km_led_rgb_t){ 255, 255, 255 };
    }
    c = cfg_of(KM_LED_FX_EXTERNAL, 255, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 40000000u);
    CHECK(frame_is(&l, 40000020u, fr));
    CHECK(frame_at(&l, 40000040u, NULL, NULL, px));
    unsigned total = 0;
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        CHECK(px[i].r <= KM_LED_CHANNEL_CAP && sum_of(&px[i]) <= KM_LED_PIXEL_CAP);
        total += sum_of(&px[i]);
    }
    CHECK(total <= KM_LED_BUDGET_SUM && total > KM_LED_BUDGET_SUM - 200u);

    /* the "external frame shown" indicator source follows the frame, and the indicator stays on top of it */
    km_lightscene_t sc;
    km_lightscene_default(&sc);
    sc.n_rules = 1;
    sc.rule[0] = mk_rule(KM_LS_SRC_EXTERNAL, 0, 0, KM_LS_OFF_KEEP, 255, 0, 0, 0);
    km_led_set_scene(&l, &sc);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        fr[i] = (km_led_rgb_t){ 0, 20, 0 };
    }
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 50000000u);
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        raw[i] = fr[i];
    }
    raw[0] = sc.rule[0].on;
    CHECK(frame_is(&l, 50000020u, raw));                                            /* the stream is shown: light 0 red over it */
    km_led_ext_end(&l, KM_LED_EXT_STREAM);
    fill_raw(raw, 0, 0, 0);
    CHECK(frame_is(&l, 50000040u, raw));                                            /* no frame: no indicator, dark */
    c.effect = KM_LED_FX_STATIC;                                                /* and a stream frame that is not drawn does not count */
    km_led_set_cfg(&l, &c);
    km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, 50000060u);
    CHECK(frame_at(&l, 50000080u, NULL, NULL, px));
    CHECK(all_equal(px));                                                       /* light 0 is not red: same as all the others */

    /* the first frame of a stream wakes faded-out lights; later frames do not count as activity */
    km_led_init(&l);
    c = cfg_of(KM_LED_FX_EXTERNAL, 255, 128, 255, 255, 255);
    km_led_set_cfg(&l, &c);
    km_led_set_idle(&l, 1);
    t = 200000u;
    run_for(&l, &t, 4000u, px);
    CHECK(km_led_idle(&l));
    for (unsigned i = 0; i < KM_LED_COUNT; i++) {
        fr[i] = (km_led_rgb_t){ 30, 0, 0 };
    }
    for (unsigned n = 0; n < 8; n++) {
        t += 20;
        km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, t);
        tick(&l, t, px);
    }
    CHECK(l.gain > 0u);
    for (unsigned n = 0; n < 8; n++) {                                         /* 160 ms of frames: the fade-in (150 ms) is done */
        t += 20;
        km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, t);
        tick(&l, t, px);
    }
    CHECK(sum_of(&px[0]) > 0u);
    for (unsigned n = 0; n < 200; n++) {                                       /* a stream that keeps coming, with no key: idle-off still applies */
        t += 20;
        km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, t);
        tick(&l, t, px);
    }
    CHECK(km_led_idle(&l));
    CHECK(all_black(px));
}

/* The limits hold for any scene, input, frame, configuration, time and fault; the stream always decodes (property test). */
static void test_layers_limits_property(void)
{
    km_led_t l;
    km_lightscene_t sc;
    km_led_rgb_t fr[KM_LED_COUNT], px[KM_LED_COUNT];
    uint8_t gray[KM_LED_COUNT];
    uint32_t now = 0;
    unsigned bad = 0, lit = 0, fx_seen[KM_LED_FX_COUNT] = { 0 };
    km_led_init(&l);
    for (unsigned n = 0; n < 1500; n++) {
        km_lightscene_default(&sc);
        sc.n_rules = (uint8_t)(rnd() % (KM_LS_MAX_RULES + 1u));
        for (unsigned i = 0; i < KM_LS_MAX_RULES; i++) {
            const unsigned src = 1u + rnd() % KM_LS_SRC_LAST;
            const unsigned off = rnd() % 3u;
            sc.rule[i] = mk_rule(src, (src == KM_LS_SRC_LAYER || src == KM_LS_SRC_PROFILE) ? rnd() % 4u : 0u, rnd() % KM_LED_COUNT,
                                 off | ((rnd() & 1u) ? KM_LS_FLAG_BLINK : 0u), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), off == 2u ? rnd() % 256u : 0u);
        }
        for (unsigned i = 0; i < KM_LS_PALETTE; i++) {
            sc.palette[i] = (km_led_rgb_t){ (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd() };
        }
        for (unsigned i = 0; i < KM_LED_COUNT; i++) {
            sc.key_idx[i] = (uint8_t)(rnd() % KM_LS_PALETTE);
            fr[i] = (km_led_rgb_t){ (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd() };
            gray[i] = (uint8_t)rnd();
        }
        if (!km_lightscene_valid(&sc)) {
            bad++;
        }
        km_led_set_scene(&l, &sc);
        const km_led_cfg_t c = cfg_of(rnd() % KM_LED_FX_COUNT, (rnd() & 3u) == 0 ? (rnd() & 1u ? 255u : 0u) : rnd() % 256u, 1u + rnd() % 255u,
                                      (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd());
        km_led_set_cfg(&l, &c);
        fx_seen[c.effect]++;
        set_in(&l, mk_in(rnd() % 8u, rnd() % 4u, rnd() % 4u, rnd() % 4u));
        now += 1u + rnd() % 1500u;
        if (rnd() % 3u != 0u) {
            km_led_ext_rgb(&l, KM_LED_EXT_STREAM, fr, now);
        }
        if (rnd() % 3u != 0u) {
            km_led_ext_gray(&l, KM_LED_EXT_ANIM, gray, now);
        }
        if (rnd() % 8u == 0u) {
            km_led_ext_end(&l, rnd() % KM_LED_EXT_SOURCES);
        }
        km_led_set_blank(&l, (rnd() % 16u) == 0);
        if (!frame_at(&l, now, NULL, NULL, px)) {
            bad++;
            continue;
        }
        unsigned total = 0;
        for (unsigned i = 0; i < KM_LED_COUNT; i++) {
            const unsigned s = sum_of(&px[i]);
            if (px[i].r > KM_LED_CHANNEL_CAP || px[i].g > KM_LED_CHANNEL_CAP || px[i].b > KM_LED_CHANNEL_CAP || s > KM_LED_PIXEL_CAP) {
                bad++;
            }
            total += s;
        }
        if (total > KM_LED_BUDGET_SUM) {
            bad++;
        }
        if ((c.brightness == 0 || l.blank) && total != 0u) {
            bad++;
        }
        lit += total != 0u;
    }
    CHECK_EQ(bad, 0);
    CHECK(lit > 700);
    for (unsigned fx = 0; fx < KM_LED_FX_COUNT; fx++) {
        CHECK(fx_seen[fx] > 80);
    }
}

void test_led(void)
{
    _Static_assert(KM_LED_SLOTS == 76u, "km_engine.h KM_ENGINE_SLOTS");
    test_geometry();
    test_encoder();
    test_hsv();
    test_cfg();
    test_static_and_limits();
    test_limits_property();
    test_off_blank();
    test_breath();
    test_rainbow();
    test_reactive();
    test_depth();
    test_demo();
    test_slicing();
    test_work_bound();
    test_pacing();
    test_guard_boot_loop();
    test_guard_faults();
    test_guard_dma();
    test_idle();
    test_layers_default_and_custom();
    test_layers_indicators();
    test_layers_idle_and_wake();
    test_layers_external();
    test_layers_limits_property();
}
