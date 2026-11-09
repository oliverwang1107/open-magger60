/*
 * test_anim.c - M6 package P7b: the animation decoder (km_anim.c).
 * The clips are made here by a small reference encoder that is written from the format description in km_anim.h and shares no
 * code with the decoder; the golden clips in golden/anim_vectors.h are made by the other implementation of the format,
 * tools/badapple_convert.py. The expected values are computed here from the documented formulas.
 *
 * Registration (done by the integrator): `void test_anim(void);` in km_test.h and `run("anim", test_anim);` in test_main.c.
 * Stand-alone build for this package alone (also what tools/mutate_anim.py uses):
 *   gcc -DKM_ANIM_STANDALONE -Icore/include -Itests tests/test_anim.c core/src/km_anim.c -o anim_tests
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "km/km_anim.h"
#include "km/km_led.h"
#include "km_test.h"
#include "golden/anim_vectors.h"

_Static_assert(KM_ANIM_LIGHTS == KM_LED_COUNT, "one sample per light");

/* ---------------------------------------------------------------- helpers: random numbers, reference CRC */

static uint32_t rng_state = 1u;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static unsigned rnd_below(unsigned n)
{
    return (unsigned)(rnd() % n);
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)   /* bit by bit, the textbook form */
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        }
    }
    return ~c;
}

/* ---------------------------------------------------------------- reference encoder */

#define MAXF 70u
#define MAXG 70u
#define GBUF 4096u

typedef struct {
    uint8_t s[MAXF][KM_ANIM_LIGHTS];
    unsigned n;
} frames_t;

typedef struct {
    uint8_t *buf;
    unsigned nbits;
} bw_t;

static void bw_put(bw_t *w, unsigned v, unsigned n)   /* n bits of v, most significant first */
{
    for (unsigned i = n; i-- > 0;) {
        const unsigned bit = (v >> i) & 1u;
        if ((w->nbits & 7u) == 0u) {
            w->buf[w->nbits >> 3] = 0;
        }
        if (bit) {
            w->buf[w->nbits >> 3] = (uint8_t)(w->buf[w->nbits >> 3] | (0x80u >> (w->nbits & 7u)));
        }
        w->nbits++;
    }
}

static void bw_eg(bw_t *w, unsigned v, unsigned k)   /* Exp-Golomb of order k: write v + 2^k with as many leading zeros as it has extra bits */
{
    const unsigned x = v + (1u << k);
    unsigned len = 0;
    while ((x >> (len + 1u)) != 0u) {
        len++;
    }
    bw_put(w, 0, len - k);
    bw_put(w, x, len + 1u);
}

typedef struct {
    const uint8_t *p;
    unsigned len;
} grp_t;

static void default_palette(uint8_t *pal, unsigned bits)
{
    const unsigned levels = 1u << bits;
    for (unsigned i = 0; i < levels; i++) {
        pal[i] = (uint8_t)((i * 255u) / (levels - 1u));
    }
}

/* Header + palette + index + groups + checksum around already encoded groups. */
static unsigned assemble(uint8_t *out, unsigned bits, unsigned key, unsigned n, unsigned fps, unsigned kc, unsigned kg,
                         unsigned flags, const grp_t *g, unsigned m)
{
    unsigned data_len = 0;
    for (unsigned i = 0; i < m; i++) {
        data_len += g[i].len;
    }
    const unsigned levels = 1u << bits;
    memset(out, 0, 20);
    memcpy(out, "KMAN", 4);
    out[4] = 1;
    out[5] = 61;
    out[6] = (uint8_t)bits;
    out[7] = (uint8_t)fps;
    out[8] = (uint8_t)n; out[9] = (uint8_t)(n >> 8);
    out[10] = (uint8_t)key;
    out[11] = (uint8_t)flags;
    out[12] = (uint8_t)kc;
    out[13] = (uint8_t)kg;
    out[14] = (uint8_t)m; out[15] = (uint8_t)(m >> 8);
    out[16] = (uint8_t)data_len; out[17] = (uint8_t)(data_len >> 8);
    default_palette(out + 20, bits);
    unsigned pos = 20u + levels, off = 0;
    for (unsigned i = 0; i < m; i++) {
        out[pos++] = (uint8_t)off;
        out[pos++] = (uint8_t)(off >> 8);
        off += g[i].len;
    }
    for (unsigned i = 0; i < m; i++) {
        memcpy(out + pos, g[i].p, g[i].len);
        pos += g[i].len;
    }
    const uint32_t crc = crc32_ref(out, pos);
    for (unsigned i = 0; i < 4; i++) {
        out[pos++] = (uint8_t)(crc >> (8u * i));
    }
    return pos;
}

static uint8_t gbuf[MAXG][GBUF];

static unsigned encode(uint8_t *out, const frames_t *fr, unsigned bits, unsigned key, unsigned kc, unsigned kg, unsigned fps,
                       unsigned flags)
{
    grp_t g[MAXG];
    const unsigned m = (fr->n + key - 1u) / key;
    for (unsigned gi = 0; gi < m; gi++) {
        bw_t w = { gbuf[gi], 0 };
        for (unsigned f = gi * key; f < fr->n && f < (gi + 1u) * key; f++) {
            if (f == gi * key) {
                for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
                    bw_put(&w, fr->s[f][i], bits);
                }
            } else {
                unsigned count = 0;
                for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
                    count += fr->s[f][i] != fr->s[f - 1u][i];
                }
                bw_eg(&w, count, kc);
                unsigned last = 0;
                for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
                    if (fr->s[f][i] != fr->s[f - 1u][i]) {
                        bw_eg(&w, i - last, kg);
                        if (bits > 1u) {
                            bw_put(&w, fr->s[f][i], bits);
                        }
                        last = i + 1u;
                    }
                }
            }
        }
        g[gi].p = gbuf[gi];
        g[gi].len = (w.nbits + 7u) / 8u;
    }
    return assemble(out, bits, key, fr->n, fps, kc, kg, flags, g, m);
}

/* ---------------------------------------------------------------- test content */

typedef enum { C_STATIC, C_SPARSE, C_DENSE, C_ONE, C_BLOCKS, C_COUNT } content_t;

static void make_frames(frames_t *fr, unsigned n, unsigned bits, content_t kind)
{
    const unsigned levels = 1u << bits;
    fr->n = n;
    for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
        fr->s[0][i] = (uint8_t)rnd_below(levels);
    }
    for (unsigned f = 1; f < n; f++) {
        memcpy(fr->s[f], fr->s[f - 1u], KM_ANIM_LIGHTS);
        switch (kind) {
        case C_STATIC:
            break;
        case C_SPARSE:
            for (unsigned k = rnd_below(4); k > 0; k--) {
                const unsigned c = rnd_below(KM_ANIM_LIGHTS);
                fr->s[f][c] = (uint8_t)((fr->s[f][c] + 1u + rnd_below(levels - 1u)) % levels);
            }
            break;
        case C_DENSE:
            for (unsigned c = 0; c < KM_ANIM_LIGHTS; c++) {
                fr->s[f][c] = (uint8_t)((fr->s[f][c] + 1u + rnd_below(levels - 1u)) % levels);   /* every cell changes */
            }
            break;
        case C_ONE:
            fr->s[f][(f * 7u) % KM_ANIM_LIGHTS] = (uint8_t)((fr->s[f][(f * 7u) % KM_ANIM_LIGHTS] + 1u) % levels);
            break;
        default: {                       /* a run of neighbours, one end of the light list, the other end */
            const unsigned a = rnd_below(KM_ANIM_LIGHTS), b = a + rnd_below(KM_ANIM_LIGHTS - a);
            for (unsigned c = a; c <= b; c++) {
                fr->s[f][c] = (uint8_t)((fr->s[f][c] + 1u) % levels);
            }
            fr->s[f][0] = (uint8_t)((fr->s[f][0] + 1u) % levels);
            fr->s[f][KM_ANIM_LIGHTS - 1u] = (uint8_t)((fr->s[f][KM_ANIM_LIGHTS - 1u] + 1u) % levels);
            break;
        }
        }
    }
}

static uint8_t level_of(unsigned bits, unsigned sample)
{
    return (uint8_t)((sample * 255u) / ((1u << bits) - 1u));
}

static uint32_t expect_frame(uint32_t ms, unsigned fps)   /* the frame that is shown at ms */
{
    return (uint32_t)(((uint64_t)ms * fps) / 1000u);
}

static uint8_t clipbuf[40000];

static bool open_clip(km_anim_t *a, km_anim_info_t *info, const uint8_t *buf, unsigned len)
{
    if (km_anim_parse(buf, len, info) != KM_ANIM_OK) {
        return false;
    }
    km_anim_init(a, info);
    return true;
}

static bool out_is(const uint8_t *out, const frames_t *fr, unsigned bits, unsigned f)
{
    for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
        if (out[i] != level_of(bits, fr->s[f][i])) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- round trip */

static void test_roundtrip(void)
{
    static const unsigned bits_set[] = { 1, 2, 4 };
    static const unsigned keys[] = { 1, 2, 3, 7, 30, 255 };
    static const unsigned counts[] = { 1, 30, 31, 64 };
    static const unsigned ks[][2] = { { 0, 0 }, { 2, 1 }, { 4, 3 } };
    static frames_t fr;
    int bad = 0, clips = 0;
    for (unsigned bi = 0; bi < 3; bi++) {
        for (unsigned ki = 0; ki < 6; ki++) {
            for (unsigned ci = 0; ci < 4; ci++) {
                for (unsigned kk = 0; kk < 3; kk++) {
                    for (unsigned kind = 0; kind < C_COUNT; kind++) {
                        const unsigned bits = bits_set[bi], key = keys[ki], n = counts[ci], fps = 20;
                        make_frames(&fr, n, bits, (content_t)kind);
                        const unsigned len = encode(clipbuf, &fr, bits, key, ks[kk][0], ks[kk][1], fps, 0);
                        km_anim_info_t info;
                        km_anim_t a;
                        clips++;
                        if (km_anim_validate(clipbuf, len) != KM_ANIM_OK || !open_clip(&a, &info, clipbuf, len)) {
                            bad++;
                            continue;
                        }
                        km_anim_start(&a, 1000, 0, 0);
                        uint8_t out[KM_ANIM_LIGHTS];
                        for (unsigned f = 0; f < n; f++) {
                            const uint32_t ms = f * 50u;                        /* first millisecond of frame f */
                            if (!km_anim_tick(&a, 1000u + ms, out) || !out_is(out, &fr, bits, f) || km_anim_frame(&a) != f) {
                                bad++;
                                break;
                            }
                        }
                        /* a clip that is played with a loop reaches the same frames again */
                        km_anim_start(&a, 0, 0, KM_ANIM_PLAY_LOOP);
                        for (unsigned f = 0; f < n; f++) {
                            const uint32_t ms = f * 50u + n * 50u * 3u;       /* three laps later */
                            if (!km_anim_tick(&a, ms, out) || !out_is(out, &fr, bits, f)) {
                                bad++;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(clips == 3 * 6 * 4 * 3 * (int)C_COUNT);
    CHECK_EQ(bad, 0);
}

/* The decoder never writes outside `out`, and never touches it when it returns false. */
static void test_out_untouched(void)
{
    static frames_t fr;
    make_frames(&fr, 10, 2, C_SPARSE);
    const unsigned len = encode(clipbuf, &fr, 2, 4, 2, 1, 10, 0);
    km_anim_info_t info;
    km_anim_t a;
    CHECK(open_clip(&a, &info, clipbuf, len));
    uint8_t out[KM_ANIM_LIGHTS + 2];
    memset(out, 0xA5, sizeof out);
    CHECK(!km_anim_tick(&a, 0, out));                   /* IDLE */
    km_anim_start(&a, 0, 0, 0);
    CHECK(km_anim_tick(&a, 0, out));
    CHECK(out[KM_ANIM_LIGHTS] == 0xA5 && out[KM_ANIM_LIGHTS + 1] == 0xA5);
    memset(out, 0xA5, sizeof out);
    CHECK(!km_anim_tick(&a, 50, out));                  /* same frame */
    CHECK(out[0] == 0xA5 && out[KM_ANIM_LIGHTS - 1] == 0xA5);
    CHECK(km_anim_tick(&a, 100, out));
    CHECK_EQ(km_anim_frame(&a), 1);
}

/* ---------------------------------------------------------------- clock */

static void test_clock(void)
{
    static const unsigned fps_set[] = { 1, 10, 12, 15, 24, 30, 60 };
    static frames_t fr;
    int bad = 0;
    for (unsigned fi = 0; fi < 7; fi++) {
        const unsigned fps = fps_set[fi], n = 40;
        make_frames(&fr, n, 4, C_DENSE);               /* every frame differs from the one before: each tick is distinguishable */
        const unsigned len = encode(clipbuf, &fr, 4, 9, 0, 0, fps, 0);
        km_anim_info_t info;
        km_anim_t a;
        if (!open_clip(&a, &info, clipbuf, len)) {
            bad++;
            continue;
        }
        const uint32_t end_ms = (uint32_t)(((uint64_t)n * 1000u + fps - 1u) / fps);   /* first ms at/after the duration */
        const uint32_t t0 = 0xFFFFF000u;                /* the 32-bit clock wraps during the clip: differences must be used, not absolute times */
        km_anim_start(&a, t0, 0, 0);
        int have = 0;
        unsigned shown = 0;
        uint8_t out[KM_ANIM_LIGHTS];
        for (uint32_t ms = 0; ms < end_ms + 30u && km_anim_state(&a) != KM_ANIM_DONE; ms++) {
            const bool changed = km_anim_tick(&a, t0 + ms, out);
            const unsigned f = (unsigned)expect_frame(ms, fps);
            if (f >= n) {
                break;
            }
            if (!have || f != shown) {
                have = 1;
                shown = f;
                if (!changed || !out_is(out, &fr, 4, f)) {
                    bad++;
                }
            } else if (changed) {
                bad++;
            }
        }
        /* the clip ends on the first millisecond that is at/after n frames */
        CHECK(km_anim_state(&a) == KM_ANIM_DONE);
        CHECK_EQ(km_anim_duration_ms(&info), (uint32_t)((uint64_t)n * 1000u / fps));
    }
    CHECK_EQ(bad, 0);
}

static void test_start_stop_loop(void)
{
    static frames_t fr;
    make_frames(&fr, 40, 2, C_SPARSE);
    const unsigned len = encode(clipbuf, &fr, 2, 8, 1, 1, 20, 0);   /* 2000 ms long */
    km_anim_info_t info;
    km_anim_t a;
    uint8_t out[KM_ANIM_LIGHTS];
    CHECK(open_clip(&a, &info, clipbuf, len));
    CHECK(km_anim_state(&a) == KM_ANIM_IDLE);

    /* a clip that does not loop ends with DONE and then stays quiet */
    km_anim_start(&a, 100, 0, 0);
    CHECK(km_anim_tick(&a, 100, out) && out_is(out, &fr, 2, 0));
    CHECK(km_anim_tick(&a, 100 + 1999, out) && out_is(out, &fr, 2, 39));
    memset(out, 0x5A, sizeof out);
    CHECK(!km_anim_tick(&a, 100 + 2000, out));
    CHECK(km_anim_state(&a) == KM_ANIM_DONE && out[0] == 0x5A);
    CHECK(!km_anim_tick(&a, 100 + 2100, out) && km_anim_state(&a) == KM_ANIM_DONE);

    /* start again restarts from the beginning */
    km_anim_start(&a, 5000, 0, 0);
    CHECK(km_anim_state(&a) == KM_ANIM_PLAYING);
    CHECK(km_anim_tick(&a, 5000, out) && out_is(out, &fr, 2, 0));

    /* stop */
    km_anim_stop(&a);
    CHECK(km_anim_state(&a) == KM_ANIM_IDLE);
    CHECK(!km_anim_tick(&a, 5100, out));

    /* loop: the same frame again one duration later, any number of loops later, across the 32 bit wrap of the clock */
    km_anim_start(&a, 0xFFFFFF00u, 0, KM_ANIM_PLAY_LOOP);
    for (unsigned lap = 0; lap < 5; lap++) {
        for (unsigned f = 0; f < 40; f += 13) {
            const uint32_t ms = 0xFFFFFF00u + lap * 2000u + f * 50u;
            CHECK(km_anim_tick(&a, ms, out));            /* the frame index differs from the one before every time */
            CHECK(out_is(out, &fr, 2, f));
        }
    }
    CHECK(km_anim_state(&a) == KM_ANIM_PLAYING);

    /* offset into the clip (seek), negative offset (wait on frame 0), offset past the end */
    km_anim_start(&a, 0, 1000, 0);                       /* frame 20 */
    unsigned ticks = 0;
    do {
        km_anim_tick(&a, 0, out);
        CHECK(km_anim_last_work(&a) <= KM_ANIM_WORK_MAX);
        ticks++;
    } while (!out_is(out, &fr, 2, 20) && ticks < 10);
    CHECK_EQ(ticks, 1);                                  /* key frame 16 + 4 steps: reached on the first tick */
    km_anim_start(&a, 1000, -500, 0);
    CHECK(km_anim_tick(&a, 1000, out) && out_is(out, &fr, 2, 0));
    CHECK(!km_anim_tick(&a, 1499, out));
    CHECK(km_anim_tick(&a, 1500 + 50, out) && out_is(out, &fr, 2, 1));
    km_anim_start(&a, 0, 2000, 0);
    CHECK(!km_anim_tick(&a, 0, out) && km_anim_state(&a) == KM_ANIM_DONE);
    km_anim_start(&a, 0, 2000 + 1000, KM_ANIM_PLAY_LOOP);  /* a loop reduces the offset */
    CHECK(km_anim_tick(&a, 0, out) && out_is(out, &fr, 2, 20));

    /* re-sync while playing moves the clock but keeps the decoder */
    km_anim_start(&a, 0, 0, 0);
    km_anim_tick(&a, 0, out);
    km_anim_start(&a, 500, 100, 0);
    CHECK(km_anim_tick(&a, 500, out) && out_is(out, &fr, 2, 2) && km_anim_frame(&a) == 2);
}

/* ---------------------------------------------------------------- blend */

static void test_blend(void)
{
    static frames_t fr;
    make_frames(&fr, 12, 4, C_DENSE);
    const unsigned fps = 10, n = 12;
    const unsigned len = encode(clipbuf, &fr, 4, 5, 0, 0, fps, 0);
    km_anim_info_t info;
    km_anim_t a;
    uint8_t out[KM_ANIM_LIGHTS];
    int bad = 0;
    CHECK(open_clip(&a, &info, clipbuf, len));
    for (unsigned loop = 0; loop < 2; loop++) {
        km_anim_start(&a, 0, 0, (uint8_t)(KM_ANIM_PLAY_BLEND | (loop ? KM_ANIM_PLAY_LOOP : 0u)));
        const uint32_t last_ms = loop ? 2u * n * 100u - 1u : n * 100u - 1u;
        for (uint32_t ms = 0; ms <= last_ms; ms += 7u) {
            const uint64_t x = ((uint64_t)ms * fps) % ((uint64_t)n * 1000u);
            const unsigned f = (unsigned)(x / 1000u);
            unsigned alpha = (unsigned)((x % 1000u) * 16u / 1000u);
            unsigned nf = f + 1u;
            if (nf >= n) {
                nf = loop ? 0u : n - 1u;
                alpha = loop ? alpha : 16u;
            }
            km_anim_tick(&a, ms, out);                  /* may be false when nothing changed: out then still holds the last frame */
            /* the output after this call is what the clock asks for */
            for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
                const unsigned p = level_of(4, fr.s[f][i]), c = level_of(4, fr.s[nf][i]);
                const unsigned want = (alpha >= 16u) ? c : (p * (16u - alpha) + c * alpha + 8u) / 16u;
                if (out[i] != want) {
                    bad++;
                    break;
                }
            }
        }
    }
    CHECK_EQ(bad, 0);
    /* a start in the middle of a blend lands on a key frame: the picture before it is not known, the key frame is shown as it is */
    km_anim_stop(&a);
    km_anim_start(&a, 0, 450, KM_ANIM_PLAY_BLEND);        /* frame 4.5, the next frame (5) is the key frame of the second group */
    CHECK(km_anim_tick(&a, 0, out) && out_is(out, &fr, 4, 5));
    /* blending changes the output only in alpha steps: at most 16 changes per source frame */
    km_anim_start(&a, 0, 0, KM_ANIM_PLAY_BLEND);
    unsigned changes = 0;
    for (uint32_t ms = 0; ms < 100u; ms++) {
        changes += km_anim_tick(&a, ms, out);
    }
    CHECK(changes >= 8u && changes <= 16u + 1u);
}

/* ---------------------------------------------------------------- bounded work, stalls, clock going backwards */

static void test_bounded_work(void)
{
    static frames_t fr;
    make_frames(&fr, 64, 4, C_DENSE);
    const unsigned len = encode(clipbuf, &fr, 4, 30, 0, 0, 15, 0);
    km_anim_info_t info;
    km_anim_t a;
    uint8_t out[KM_ANIM_LIGHTS];
    CHECK(open_clip(&a, &info, clipbuf, len));
    for (unsigned flags = 0; flags < 4; flags++) {
        km_anim_start(&a, 0, 0, (uint8_t)flags);
        uint32_t now = 0;
        unsigned worst = 0;
        for (unsigned i = 0; i < 400 && km_anim_state(&a) == KM_ANIM_PLAYING; i++) {
            const unsigned jump = rnd_below(8) == 0 ? 1000u + rnd_below(9000u) : rnd_below(70u);   /* sometimes a stall of seconds */
            now += jump;
            if (rnd_below(10) == 0 && now > 5000u) {
                now -= 4000u;                           /* the clock goes backwards */
            }
            km_anim_tick(&a, now, out);
            if (km_anim_last_work(&a) > worst) {
                worst = km_anim_last_work(&a);
            }
        }
        CHECK(worst <= KM_ANIM_WORK_MAX);
        CHECK(km_anim_state(&a) != KM_ANIM_ERROR);
    }
    /* after a long stall the output catches up within a known number of calls */
    km_anim_start(&a, 0, 0, 0);
    km_anim_tick(&a, 0, out);
    const uint32_t later = 3000u;                        /* frame 45: the key frame of its group is 30, the cursor is at 0 */
    const unsigned want = (unsigned)expect_frame(later, 15);
    CHECK_EQ(want, 45);
    unsigned calls = 0;
    do {
        km_anim_tick(&a, later, out);
        calls++;
    } while (!out_is(out, &fr, 4, want) && calls < 20);
    CHECK(calls <= 1 + (30 - 1 + KM_ANIM_MAX_STEPS - 1) / KM_ANIM_MAX_STEPS);
    CHECK(calls >= 2);                                   /* 15 steps cannot be done in one call */

    /* while the decoder is behind the clock the frame it has is shown as it is, not mixed with the one before it */
    km_anim_start(&a, 0, 0, KM_ANIM_PLAY_BLEND | KM_ANIM_PLAY_LOOP);
    km_anim_tick(&a, 0, out);
    CHECK(km_anim_tick(&a, 3033, out));
    bool whole = false;
    for (unsigned f = 0; f < 64; f++) {
        whole = whole || out_is(out, &fr, 4, f);
    }
    CHECK(whole);
}

/* ---------------------------------------------------------------- damaged data */

static void check_parse(const char *what, const uint8_t *buf, unsigned len, km_anim_err_t want)
{
    km_anim_info_t info;
    const km_anim_err_t got = km_anim_parse(buf, len, &info);
    if (got != want) {
        printf("  FAIL parse(%s): got %d, want %d\n", what, (int)got, (int)want);
        km_test_failures++;
    }
    km_test_checks++;
}

static void test_header_errors(void)
{
    static frames_t fr;
    static uint8_t b[40000];
    make_frames(&fr, 20, 2, C_SPARSE);
    const unsigned len = encode(clipbuf, &fr, 2, 8, 1, 1, 15, 0);
    km_anim_info_t info;
    CHECK(km_anim_parse(clipbuf, len, &info) == KM_ANIM_OK);
    CHECK_EQ(info.frames, 20);
    CHECK_EQ(info.groups, 3);
    CHECK_EQ(info.bits, 2);
    CHECK_EQ(info.fps, 15);
    CHECK_EQ(info.key_interval, 8);

    CHECK(km_anim_parse(NULL, len, &info) == KM_ANIM_E_ARG);
    CHECK(km_anim_parse(clipbuf, len, NULL) == KM_ANIM_E_ARG);
    CHECK(km_anim_parse(clipbuf, 0, &info) == KM_ANIM_E_ARG);
    check_parse("short", clipbuf, 20, KM_ANIM_E_SIZE);
    check_parse("one byte more", clipbuf, len + 1, KM_ANIM_E_SIZE);
    check_parse("one byte less", clipbuf, len - 1, KM_ANIM_E_SIZE);

    struct { const char *what; unsigned at; uint8_t val; km_anim_err_t err; } m[] = {
        { "magic", 0, 'X', KM_ANIM_E_MAGIC },
        { "version 0", 4, 0, KM_ANIM_E_VERSION },
        { "version 2", 4, 2, KM_ANIM_E_VERSION },
        { "lights", 5, 60, KM_ANIM_E_HEADER },
        { "bits 0", 6, 0, KM_ANIM_E_HEADER },
        { "bits 3", 6, 3, KM_ANIM_E_HEADER },
        { "bits 8", 6, 8, KM_ANIM_E_HEADER },
        { "fps 0", 7, 0, KM_ANIM_E_HEADER },
        { "fps 61", 7, 61, KM_ANIM_E_HEADER },
        { "frames 0", 8, 0, KM_ANIM_E_HEADER },
        { "frames 40", 8, 40, KM_ANIM_E_HEADER },         /* 40 frames in groups of 8 need 5 groups, the header says 3 */
        { "key 0", 10, 0, KM_ANIM_E_HEADER },
        { "key 5", 10, 5, KM_ANIM_E_HEADER },             /* 20 frames in groups of 5 need 4 groups */
        { "flags", 11, 2, KM_ANIM_E_HEADER },
        { "flags high", 11, 0x80, KM_ANIM_E_HEADER },
        { "kc 5", 12, 5, KM_ANIM_E_HEADER },
        { "kg 5", 13, 5, KM_ANIM_E_HEADER },
        { "groups", 14, 4, KM_ANIM_E_HEADER },
        { "groups hi", 15, 1, KM_ANIM_E_HEADER },
        { "data_len", 16, 0, KM_ANIM_E_HEADER },
        { "reserved", 18, 1, KM_ANIM_E_HEADER },
        { "reserved hi", 19, 1, KM_ANIM_E_HEADER },
        { "palette dup", 22, 0, KM_ANIM_E_PALETTE },       /* entry 2 = 0 (<= entry 1) */
        { "palette equal", 22, 85, KM_ANIM_E_PALETTE },
        { "palette order", 21, 255, KM_ANIM_E_PALETTE },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        memcpy(b, clipbuf, len);
        b[m[i].at] = m[i].val;
        check_parse(m[i].what, b, len, m[i].err);
    }
    /* the index */
    const unsigned idx = 20u + 4u;                         /* 2-bit palette is 4 bytes */
    memcpy(b, clipbuf, len); b[idx] = 1;                   check_parse("index[0] != 0", b, len, KM_ANIM_E_INDEX);
    memcpy(b, clipbuf, len); b[idx + 2] = b[idx + 4];
    b[idx + 3] = b[idx + 5];                               check_parse("index not increasing", b, len, KM_ANIM_E_INDEX);
    memcpy(b, clipbuf, len); b[idx + 4] = 0xFF; b[idx + 5] = 0xFF; check_parse("index beyond data", b, len, KM_ANIM_E_INDEX);
    memcpy(b, clipbuf, len); b[idx + 4] = (uint8_t)(b[idx + 2] + 1); b[idx + 5] = b[idx + 3];
    check_parse("group smaller than a key frame", b, len, KM_ANIM_E_INDEX);

    /* zero frames with a consistent group count (0): the player would divide by zero */
    memcpy(b, clipbuf, len);
    b[8] = 0; b[9] = 0; b[14] = 0; b[15] = 0;
    memmove(b + idx, b + idx + 6, len - idx - 6);
    check_parse("no frames, no groups", b, len - 6, KM_ANIM_E_HEADER);

    /* the group limit: 1024 groups are fine, 1025 are not */
    static uint8_t zero8[8];
    static grp_t many[1025];
    for (unsigned i = 0; i < 1025; i++) {
        many[i].p = zero8;
        many[i].len = 8;
    }
    unsigned big = assemble(b, 1, 1, 1024, 10, 0, 0, 0, many, 1024);
    check_parse("1024 groups", b, big, KM_ANIM_OK);
    big = assemble(b, 1, 1, 1025, 10, 0, 0, 0, many, 1025);
    check_parse("1025 groups", b, big, KM_ANIM_E_HEADER);
}

/* A hand-made one-group clip: bits, n frames in one group, `bytes` as the group. */
static unsigned craft(uint8_t *out, unsigned bits, unsigned n, unsigned kc, unsigned kg, const uint8_t *grp, unsigned glen)
{
    grp_t g = { grp, glen };
    return assemble(out, bits, n, n, 10, kc, kg, 0, &g, 1);
}

static void put_key_zero(bw_t *w, unsigned bits)
{
    for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
        bw_put(w, 0, bits);
    }
}

static void test_stream_errors(void)
{
    static uint8_t g[64], c[256];
    km_anim_info_t info;
    km_anim_t a;
    uint8_t out[KM_ANIM_LIGHTS];
    bw_t w;
    unsigned len;

    /* a good one first: 2 bits, key frame of zeros, then one change (cell 5 -> 3) */
    memset(g, 0, sizeof g); w.buf = g; w.nbits = 0;
    put_key_zero(&w, 2);
    bw_eg(&w, 1, 0); bw_eg(&w, 5, 0); bw_put(&w, 3, 2);
    len = craft(c, 2, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);
    CHECK(open_clip(&a, &info, c, len));
    km_anim_start(&a, 0, 0, 0);
    CHECK(km_anim_tick(&a, 0, out) && out[5] == 0);
    CHECK(km_anim_tick(&a, 100, out) && out[5] == 255 && out[4] == 0 && out[6] == 0);

    /* no-op change */
    memset(g, 0, sizeof g); w.nbits = 0;
    put_key_zero(&w, 2);
    bw_eg(&w, 1, 0); bw_eg(&w, 5, 0); bw_put(&w, 0, 2);
    len = craft(c, 2, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_NOOP);
    CHECK(open_clip(&a, &info, c, len));
    km_anim_start(&a, 0, 0, 0);
    CHECK(km_anim_tick(&a, 0, out));
    CHECK(!km_anim_tick(&a, 100, out) && km_anim_state(&a) == KM_ANIM_ERROR && km_anim_error(&a) == KM_ANIM_E_NOOP);
    CHECK(!km_anim_tick(&a, 5000, out) && km_anim_state(&a) == KM_ANIM_ERROR);   /* stays down */
    km_anim_start(&a, 6000, 0, 0);                                                 /* a new start tries again and fails again */
    CHECK(km_anim_tick(&a, 6000, out));
    CHECK(!km_anim_tick(&a, 6100, out) && km_anim_state(&a) == KM_ANIM_ERROR);
    km_anim_stop(&a);
    CHECK(km_anim_state(&a) == KM_ANIM_IDLE);

    /* cell past the last light: cell 60 is the last one; a gap that reaches 61 is not */
    memset(g, 0, sizeof g); w.nbits = 0;
    put_key_zero(&w, 1);
    bw_eg(&w, 1, 0); bw_eg(&w, 60, 0);
    len = craft(c, 1, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);
    memset(g, 0, sizeof g); w.nbits = 0;
    put_key_zero(&w, 1);
    bw_eg(&w, 1, 0); bw_eg(&w, 61, 0);
    len = craft(c, 1, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);
    /* two changes where the second falls off the end */
    memset(g, 0, sizeof g); w.nbits = 0;
    put_key_zero(&w, 1);
    bw_eg(&w, 2, 0); bw_eg(&w, 59, 0); bw_eg(&w, 1, 0);   /* cells 59, then 61 */
    len = craft(c, 1, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);
    /* more than 61 changes */
    memset(g, 0, sizeof g); w.nbits = 0;
    put_key_zero(&w, 1);
    bw_eg(&w, 62, 0);
    for (int i = 0; i < 40; i++) { bw_eg(&w, 0, 0); }
    len = craft(c, 1, 2, 0, 0, g, (w.nbits + 7) / 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);

    /* the stream ends inside a frame: a group of one byte more than the key frame, whose rest is zeros (an endless Exp-Golomb prefix) */
    memset(g, 0, sizeof g);
    len = craft(c, 1, 2, 0, 0, g, 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);
    /* the key frame itself is cut short by the end of its group (index says 7 bytes but 61 bits need 8) */
    len = craft(c, 1, 1, 0, 0, g, 7);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_INDEX);

    /* padding: the three bits after a 61-bit key frame must be zero; a group longer than its frames is also refused */
    memset(g, 0, sizeof g);
    g[7] = 0x01;
    len = craft(c, 1, 1, 0, 0, g, 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    memset(g, 0, sizeof g);
    len = craft(c, 1, 1, 0, 0, g, 9);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    memset(g, 0, sizeof g);
    len = craft(c, 1, 1, 0, 0, g, 8);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);

    /* checksum: parse accepts, the verifier refuses, the player plays on */
    c[len - 1] ^= 0x40;
    CHECK(km_anim_parse(c, len, &info) == KM_ANIM_OK);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_CRC);
    CHECK(open_clip(&a, &info, c, len));
    km_anim_start(&a, 0, 0, 0);
    CHECK(km_anim_tick(&a, 0, out));
    /* a flipped bit in the frame data is caught by the checksum even when the structure is still fine */
    memset(g, 0, sizeof g);
    len = craft(c, 1, 1, 0, 0, g, 8);
    c[20 + 2 + 2 + 3] ^= 0x10;
    CHECK(km_anim_parse(c, len, &info) == KM_ANIM_OK);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_CRC);
}

/* Reads stop at the end of their own group; the group end check applies to the last frame of every group; pad bits one by one. */
static void test_stream_errors2(void)
{
    static uint8_t g0[16], g1[16], c[512];
    grp_t gs[2];
    km_anim_info_t info;
    km_anim_t a;
    uint8_t out[KM_ANIM_LIGHTS];
    bw_t w;
    unsigned len;

    /* group 0 is a key frame and three empty difference frames, exactly 8 bytes; the fifth frame of the group has no bits left,
     * and the next group (0xFF...) would happily supply them */
    memset(g0, 0, sizeof g0); w.buf = g0; w.nbits = 0;
    put_key_zero(&w, 1);
    for (int i = 0; i < 3; i++) { bw_eg(&w, 0, 0); }
    CHECK_EQ(w.nbits, 64);
    memset(g1, 0xFF, sizeof g1);
    gs[0].p = g0; gs[0].len = 8; gs[1].p = g1; gs[1].len = 8;
    len = assemble(c, 1, 5, 6, 10, 0, 0, 0, gs, 2);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);
    memset(g1, 0, sizeof g1);                              /* the same with one frame less in the group: fine */
    len = assemble(c, 1, 4, 5, 10, 0, 0, 0, gs, 2);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);

    /* the last group of a clip is shorter than the others: its end is checked as well */
    memset(g0, 0, sizeof g0); w.nbits = 0;
    put_key_zero(&w, 1); bw_eg(&w, 0, 0);
    memset(g1, 0, sizeof g1); g1[7] = 0x01;                /* key frame with a non-zero pad bit */
    gs[0].len = 8; gs[1].len = 8;
    len = assemble(c, 1, 2, 3, 10, 0, 0, 0, gs, 2);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    g1[7] = 0;
    len = assemble(c, 1, 2, 3, 10, 0, 0, 0, gs, 2);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);

    /* a difference frame at the end of a group: its pad bits and its length are checked */
    memset(g0, 0, sizeof g0); w.nbits = 0;
    put_key_zero(&w, 1); bw_eg(&w, 0, 0);                  /* 62 bits */
    g0[7] |= 0x01;
    gs[0].len = 8;
    len = assemble(c, 1, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    g0[7] = 0x04 | 0x02;
    len = assemble(c, 1, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    g0[7] = 0x04;                                           /* the frame ends on bit 62: the clean pad is 00 */
    gs[0].len = 8;
    len = assemble(c, 1, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_validate(c, len) == KM_ANIM_OK);
    gs[0].len = 9;
    len = assemble(c, 1, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);

    /* every pad bit of a 61-bit key frame */
    for (unsigned bit = 0; bit < 3; bit++) {
        memset(g0, 0, sizeof g0);
        g0[7] = (uint8_t)(1u << bit);
        gs[0].len = 8;
        len = assemble(c, 1, 1, 1, 10, 0, 0, 0, gs, 1);
        CHECK(km_anim_validate(c, len) == KM_ANIM_E_PAD);
    }

    /* an error in the middle of a frame leaves a half-changed picture behind; starting again must not show it */
    memset(g0, 0, sizeof g0); w.nbits = 0;
    put_key_zero(&w, 1);
    bw_eg(&w, 2, 0); bw_eg(&w, 3, 0); bw_eg(&w, 70, 0);    /* toggles cell 3, then wants cell 74 */
    gs[0].len = (w.nbits + 7) / 8;
    len = assemble(c, 1, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_validate(c, len) == KM_ANIM_E_STREAM);
    CHECK(km_anim_parse(c, len, &info) == KM_ANIM_OK);
    km_anim_init(&a, &info);
    km_anim_start(&a, 0, 0, 0);
    CHECK(km_anim_tick(&a, 0, out));
    CHECK(!km_anim_tick(&a, 100, out) && km_anim_state(&a) == KM_ANIM_ERROR && km_anim_error(&a) == KM_ANIM_E_STREAM);
    km_anim_start(&a, 6000, 0, 0);
    CHECK(km_anim_tick(&a, 6000, out));
    int lit = 0;
    for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
        lit += out[i] != 0;
    }
    CHECK_EQ(lit, 0);                                       /* frame 0 as it is in the file */

    /* the incremental verifier finds an error in the very last frame even when it is stepped one frame at a time */
    memset(g0, 0, sizeof g0); w.nbits = 0;
    put_key_zero(&w, 2);
    bw_eg(&w, 1, 0); bw_eg(&w, 5, 0); bw_put(&w, 0, 2);   /* no-op change in frame 1 */
    gs[0].len = (w.nbits + 7) / 8;
    len = assemble(c, 2, 2, 2, 10, 0, 0, 0, gs, 1);
    CHECK(km_anim_parse(c, len, &info) == KM_ANIM_OK);
    km_anim_verify_t v;
    km_anim_verify_begin(&v, &info);
    km_anim_verify_result_t r;
    unsigned steps = 0;
    do {
        r = km_anim_verify_step(&v, 1);
        steps++;
    } while (r == KM_ANIM_V_RUNNING && steps < 100);
    CHECK(r == KM_ANIM_V_BAD && km_anim_verify_error(&v) == KM_ANIM_E_NOOP);
}

/* The incremental verifier does the same as validate(), and a step costs a bounded amount. */
static void test_verify_steps(void)
{
    static frames_t fr;
    make_frames(&fr, 64, 4, C_BLOCKS);
    const unsigned len = encode(clipbuf, &fr, 4, 30, 2, 1, 15, 0);
    km_anim_info_t info;
    km_anim_verify_t v;
    CHECK(km_anim_parse(clipbuf, len, &info) == KM_ANIM_OK);
    km_anim_verify_begin(&v, &info);
    unsigned steps = 0;
    km_anim_verify_result_t r;
    unsigned worst = 0;
    do {
        r = km_anim_verify_step(&v, 1);
        steps++;
        if (v.dec.work > worst) {
            worst = v.dec.work;
        }
    } while (r == KM_ANIM_V_RUNNING && steps < 100000);
    CHECK(r == KM_ANIM_V_OK);
    CHECK(steps >= 64u + (len - 4u) / 256u);
    CHECK(worst <= KM_ANIM_LIGHTS + 1u);
    CHECK(km_anim_verify_step(&v, 1) == KM_ANIM_V_OK);     /* stays finished */

    /* a damaged clip */
    clipbuf[len - 6] ^= 0x01;
    CHECK(km_anim_parse(clipbuf, len, &info) == KM_ANIM_OK);
    km_anim_verify_begin(&v, &info);
    do {
        r = km_anim_verify_step(&v, 3);
    } while (r == KM_ANIM_V_RUNNING);
    CHECK(r == KM_ANIM_V_BAD);
    CHECK(km_anim_verify_error(&v) == KM_ANIM_E_CRC);
    CHECK(km_anim_verify_step(&v, 3) == KM_ANIM_V_BAD);
}

/* ---------------------------------------------------------------- fuzz: damaged data can never crash or hang the player */

#define FNV_INIT 2166136261u

static uint32_t fnv(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

/* Play the clip at `data` for a while with a seeded random clock; return a hash of everything observable. */
static uint32_t play_trace(const uint8_t *data, unsigned len, uint32_t seed, unsigned *violations)
{
    uint32_t h = FNV_INIT;
    const uint32_t saved = rng_state;
    rng_state = seed;
    km_anim_info_t info;
    const km_anim_err_t pe = km_anim_parse(data, len, &info);
    h = fnv(h, (uint32_t)pe);
    h = fnv(h, (uint32_t)km_anim_validate(data, len));
    if (pe == KM_ANIM_OK) {
        km_anim_t a;
        uint8_t out[KM_ANIM_LIGHTS];
        uint32_t now = rnd();
        km_anim_init(&a, &info);
        km_anim_start(&a, now, (int32_t)rnd_below(3000) - 500, (uint8_t)rnd_below(4));
        for (unsigned i = 0; i < 120; i++) {
            now += rnd_below(5) == 0 ? rnd_below(20000) : rnd_below(120);
            if (rnd_below(25) == 0) {
                km_anim_start(&a, now, (int32_t)rnd_below(60000) - 1000, (uint8_t)rnd_below(4));
            }
            memset(out, 0xEE, sizeof out);
            const bool changed = km_anim_tick(&a, now, out);
            h = fnv(h, changed);
            h = fnv(h, km_anim_state(&a));
            h = fnv(h, km_anim_error(&a));
            h = fnv(h, km_anim_last_work(&a));
            if (km_anim_last_work(&a) > KM_ANIM_WORK_MAX || km_anim_state(&a) > KM_ANIM_ERROR ||
                (km_anim_state(&a) == KM_ANIM_PLAYING && km_anim_frame(&a) >= info.frames)) {
                (*violations)++;
            }
            for (unsigned k = 0; k < KM_ANIM_LIGHTS; k++) {
                if (!changed && out[k] != 0xEE) {
                    (*violations)++;                                 /* false = untouched */
                }
                h = fnv(h, out[k]);
            }
            if (km_anim_state(&a) == KM_ANIM_ERROR && rnd_below(3) == 0) {
                break;
            }
        }
    }
    rng_state = saved;
    return h;
}

static uint8_t arena[64 + 40000 + 64];

/* The arena around the clip is filled with `fill`: a decoder that read outside the clip would give different hashes for two fills. */
static uint32_t run_trace(const uint8_t *clip, unsigned len, uint8_t fill, uint32_t seed, unsigned *violations)
{
    memset(arena, fill, sizeof arena);
    memcpy(arena + 64, clip, len);
    return play_trace(arena + 64, len, seed, violations);
}

#ifdef _WIN32
#include <windows.h>
/* The clip flush against an inaccessible page (before or after it): a read outside the clip is an access violation. */
static uint32_t guarded_trace(const uint8_t *clip, unsigned len, bool at_end, uint32_t seed, unsigned *violations)
{
    SYSTEM_INFO si;
    DWORD old;
    GetSystemInfo(&si);
    const size_t pg = si.dwPageSize, body = ((len + pg - 1u) / pg + 1u) * pg;
    uint8_t *base = VirtualAlloc(NULL, body + 2u * pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == NULL) {
        return 0;
    }
    VirtualProtect(base, pg, PAGE_NOACCESS, &old);
    VirtualProtect(base + pg + body, pg, PAGE_NOACCESS, &old);
    uint8_t *at = at_end ? base + pg + body - len : base + pg;
    memcpy(at, clip, len);
    const uint32_t h = play_trace(at, len, seed, violations);
    VirtualFree(base, 0, MEM_RELEASE);
    return h;
}
#endif

static void test_fuzz(void)
{
    static frames_t fr;
    static uint8_t seed_clips[6][6000];
    unsigned seed_len[6];
    static const unsigned bits_set[6] = { 1, 2, 4, 2, 1, 4 }, keys[6] = { 30, 8, 5, 1, 255, 13 }, kcs[6] = { 0, 2, 3, 1, 4, 0 }, kgs[6] = { 1, 0, 2, 3, 0, 4 };
    for (unsigned i = 0; i < 6; i++) {
        make_frames(&fr, 48, bits_set[i], (content_t)(i % C_COUNT));
        seed_len[i] = encode(seed_clips[i], &fr, bits_set[i], keys[i], kcs[i], kgs[i], 12, 0);
        CHECK(km_anim_validate(seed_clips[i], seed_len[i]) == KM_ANIM_OK);
    }
    static uint8_t bad[6000];
    unsigned violations = 0, differs = 0, parsed = 0, errored = 0;
    rng_state = 0x1234567u;
    for (unsigned it = 0; it < 12000; it++) {
        const unsigned s = rnd_below(6);
        unsigned len = seed_len[s];
        memcpy(bad, seed_clips[s], len);
        switch (rnd_below(6)) {
        case 0:                                              /* flip bits anywhere */
            for (unsigned k = 1 + rnd_below(4); k > 0; k--) {
                bad[rnd_below(len)] ^= (uint8_t)(1u << rnd_below(8));
            }
            break;
        case 1:                                              /* overwrite bytes */
            for (unsigned k = 1 + rnd_below(8); k > 0; k--) {
                bad[rnd_below(len)] = (uint8_t)rnd();
            }
            break;
        case 2:                                              /* truncate or extend */
            len = rnd_below(len + 8u);
            for (unsigned k = seed_len[s]; k < len; k++) {
                bad[k] = (uint8_t)rnd();
            }
            break;
        case 3:                                              /* garbage with a valid-looking start */
            len = rnd_below(300);
            for (unsigned k = 0; k < len; k++) {
                bad[k] = (uint8_t)rnd();
            }
            if (len > 8 && rnd_below(2)) {
                memcpy(bad, "KMAN\x01\x3d", 6);
            }
            break;
        default:                                             /* the frame data only: the header stays valid, the decoder meets the damage */
            for (unsigned k = 1 + rnd_below(6); k > 0; k--) {
                const unsigned data_off = 20u + (1u << bits_set[s]) + 2u * ((48u + keys[s] - 1u) / keys[s]);
                const unsigned at = data_off + rnd_below(len - 4u - data_off);
                bad[at] = (rnd_below(2) != 0u) ? (uint8_t)rnd() : (uint8_t)(bad[at] ^ (1u << rnd_below(8)));
            }
            break;
        }
        const uint32_t seed = rnd() | 1u;
        const uint32_t h0 = run_trace(bad, len, 0x00, seed, &violations);
        const uint32_t h1 = run_trace(bad, len, 0xFF, seed, &violations);
        differs += h0 != h1;
#ifdef _WIN32
        if (it % 4u == 0u && len > 0u) {
            differs += guarded_trace(bad, len, true, seed, &violations) != h0;
            differs += guarded_trace(bad, len, false, seed, &violations) != h0;
        }
#endif
        km_anim_info_t info;
        parsed += km_anim_parse(bad, len, &info) == KM_ANIM_OK;
        errored += km_anim_validate(bad, len) != KM_ANIM_OK;
    }
    CHECK_EQ(violations, 0);
    CHECK_EQ(differs, 0);
    CHECK(parsed > 4000);          /* the fuzz is not all rejected at the door */
    CHECK(errored > 4000);         /* and it does hit errors */
}

/* ---------------------------------------------------------------- golden clips from tools/badapple_convert.py */

static void test_golden(void)
{
    for (unsigned v = 0; v < ANIM_VEC_COUNT; v++) {
        const anim_vec_t *g = &anim_vecs[v];
        km_anim_info_t info;
        km_anim_t a;
        CHECK(km_anim_parse(g->data, g->len, &info) == KM_ANIM_OK);
        CHECK(km_anim_validate(g->data, g->len) == KM_ANIM_OK);
        CHECK_EQ(info.frames, g->frames);
        CHECK_EQ(info.fps, g->fps);
        km_anim_init(&a, &info);
        km_anim_start(&a, 0, 0, 0);
        uint8_t out[KM_ANIM_LIGHTS];
        int bad = 0;
        for (unsigned f = 0; f < g->frames; f++) {
            const uint32_t ms = (f * 1000u + g->fps - 1u) / g->fps;
            if (!km_anim_tick(&a, ms, out) || memcmp(out, g->levels + f * KM_ANIM_LIGHTS, KM_ANIM_LIGHTS) != 0) {
                bad++;
            }
        }
        CHECK_EQ(bad, 0);
        /* also in reverse order: every frame by seeking */
        km_anim_start(&a, 0, 0, KM_ANIM_PLAY_LOOP);
        for (unsigned f = g->frames; f-- > 0;) {
            const uint32_t ms = (f * 1000u + g->fps - 1u) / g->fps;
            unsigned calls = 0;
            do {
                km_anim_tick(&a, ms, out);
            } while (memcmp(out, g->levels + f * KM_ANIM_LIGHTS, KM_ANIM_LIGHTS) != 0 && ++calls < 20);
            if (calls >= 20) {
                bad++;
            }
        }
        CHECK_EQ(bad, 0);
    }
}

void test_anim(void)
{
    rng_state = 0x9E3779B9u;
    test_roundtrip();
    test_out_untouched();
    test_clock();
    test_start_stop_loop();
    test_blend();
    test_bounded_work();
    test_header_errors();
    test_stream_errors();
    test_stream_errors2();
    test_verify_steps();
    test_golden();
    test_fuzz();
}

#ifdef KM_ANIM_STANDALONE
int km_test_checks;
int km_test_failures;

int main(void)
{
    test_anim();
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
#endif
