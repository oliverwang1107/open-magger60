/*
 * Own depth model (M4-T-A): km_depth_model_mm100() against the 61 measured (released, bottom) pairs of
 * core/data/slot_ranges_s2.json, plus synthetic calibrations. No vendor data is used here.
 *
 * Cost note (arm-none-eabi-gcc 16, -mcpu=cortex-m4 -Os): km_depth_model_curve_mm100 is 90 bytes in
 * total with the wrapper, 30 instructions on the main path (3 multiplies, one UDIV), about 40 cycles.
 */
#include "km/km_depth_model.h"
#include "km_test.h"
#include "gen/depth_cal_data.h"

#include <stdlib.h>

static double ref_depth(unsigned released, unsigned bottom, unsigned value, unsigned a_q6)
{
    double n = ((double)released - (double)value) / ((double)released - (double)bottom);
    const double a = (double)a_q6 / 64.0;
    if (n < 0.0) {
        n = 0.0;
    }
    if (n > 1.0) {
        n = 1.0;
    }
    return (double)KM_TRAVEL_MM100 * n * (1.0 + a) / (1.0 + a * n);
}

static km_depth_cal_t cal_of(unsigned released, unsigned bottom)
{
    km_depth_cal_t c = { (uint16_t)released, (uint16_t)bottom };
    return c;
}

static unsigned absdiff(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

static void test_constants(void)
{
    CHECK_EQ(KM_TRAVEL_MM100, 340);
    CHECK_EQ(KM_DEPTH_CURVE_A_Q6, 128);
    CHECK_EQ(KM_DEPTH_CURVE_A_Q6_MAX, 1024);
    CHECK_EQ(KM_DEPTH_MIN_SPAN, 64);
    CHECK_EQ(KM_DEPTH_MAX_SPAN, 4095);
}

static void test_endpoints_and_clamp(void)
{
    CHECK_EQ(DEPTH_CAL_COUNT, 61);
    for (unsigned i = 0; i < DEPTH_CAL_COUNT; i++) {
        const unsigned rel = k_depth_cal[i].released, bot = k_depth_cal[i].bottom;
        const km_depth_cal_t c = cal_of(rel, bot);
        CHECK(rel > bot);
        CHECK_EQ(km_depth_model_mm100(&c, (uint16_t)rel), 0);
        CHECK_EQ(km_depth_model_mm100(&c, (uint16_t)bot), KM_TRAVEL_MM100);
        /* outside the calibrated range: clamped, not extrapolated */
        CHECK_EQ(km_depth_model_mm100(&c, (uint16_t)(rel + 1)), 0);
        CHECK_EQ(km_depth_model_mm100(&c, (uint16_t)(rel + 400)), 0);
        CHECK_EQ(km_depth_model_mm100(&c, 65535), 0);
        CHECK_EQ(km_depth_model_mm100(&c, (uint16_t)(bot - 1)), KM_TRAVEL_MM100);
        CHECK_EQ(km_depth_model_mm100(&c, 0), KM_TRAVEL_MM100);
        /* one count from each end: positive and small at the bottom side, small at the rest side */
        CHECK(km_depth_model_mm100(&c, (uint16_t)(rel - 1)) <= 2);
        CHECK(km_depth_model_mm100(&c, (uint16_t)(bot + 1)) >= KM_TRAVEL_MM100 - 2);
    }
}

/* Non-increasing in the reading, bounded, and no flat stretch longer than a few counts. */
static void check_monotonic(const km_depth_cal_t *c, unsigned a_q6, unsigned max_flat)
{
    unsigned prev = 0, flat = 0;
    for (unsigned v = 0; v <= 5000; v++) {
        const unsigned d = km_depth_model_curve_mm100(c, (uint16_t)v, (uint16_t)a_q6);
        CHECK(d <= KM_TRAVEL_MM100);
        if (v > 0) {
            CHECK(d <= prev);
        }
        if (v > c->bottom && v <= c->released) {
            flat = (d == prev) ? flat + 1 : 0;
            CHECK(flat <= max_flat);
        }
        prev = d;
    }
}

static void test_monotonic_all(void)
{
    for (unsigned i = 0; i < DEPTH_CAL_COUNT; i++) {
        const km_depth_cal_t c = cal_of(k_depth_cal[i].released, k_depth_cal[i].bottom);
        check_monotonic(&c, KM_DEPTH_CURVE_A_Q6, 13); /* bottom end: T/(1+a)/span = 0.12 unit per count at worst */
    }
    static const km_depth_cal_t odd[] = { { 3000, 2000 }, { 4095, 0 }, { 3450, 3386 }, { 5000, 4000 }, { 3300, 1795 } };
    for (unsigned i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        check_monotonic(&odd[i], KM_DEPTH_CURVE_A_Q6, 70);
        check_monotonic(&odd[i], 0, 70);
        check_monotonic(&odd[i], 1024, 4095);
        check_monotonic(&odd[i], 7, 70);
    }
}

/* Against the ideal curve in double: the integer rounding is the only difference. */
static void test_matches_reference(void)
{
    static const unsigned a_list[] = { 0, 16, 64, 128, 300, 1024 };
    for (unsigned k = 0; k < sizeof a_list / sizeof a_list[0]; k++) {
        for (unsigned i = 0; i < DEPTH_CAL_COUNT; i += 5) {
            const unsigned rel = k_depth_cal[i].released, bot = k_depth_cal[i].bottom;
            const km_depth_cal_t c = cal_of(rel, bot);
            for (unsigned v = bot - 5; v <= rel + 5; v++) {
                const unsigned got = km_depth_model_curve_mm100(&c, (uint16_t)v, (uint16_t)a_list[k]);
                const double want = ref_depth(rel, bot, v, a_list[k]);
                CHECK(((double)got - want) <= 0.5001 && (want - (double)got) <= 0.5001);
            }
        }
    }
}

/* Exactly round-half-up of the rational formula, computed in 64 bit: catches any off-by-one in the rounding. */
static void test_exact_rounding(void)
{
    static const unsigned a_list[] = { 0, 1, 2, 5, 128, 777, 1024 };
    /* the last five hit the one case per ~1e6 where the rounding tie sits one below half (spans 66, 69, 106, 163, 181) */
    static const km_depth_cal_t cals[] = { { 3300, 2300 }, { 3382, 2384 }, { 4095, 0 }, { 2064, 2000 }, { 3451, 2086 },
                                           { 2066, 2000 }, { 2069, 2000 }, { 2106, 2000 }, { 2163, 2000 }, { 2181, 2000 } };
    for (unsigned k = 0; k < sizeof a_list / sizeof a_list[0]; k++) {
        for (unsigned i = 0; i < sizeof cals / sizeof cals[0]; i++) {
            const uint64_t span = (uint64_t)cals[i].released - cals[i].bottom;
            for (unsigned v = cals[i].bottom; v <= cals[i].released; v++) {
                const uint64_t num = (uint64_t)cals[i].released - v;
                const uint64_t den = 64u * span + (uint64_t)a_list[k] * num;
                const uint64_t top = (uint64_t)KM_TRAVEL_MM100 * (64u + a_list[k]) * num;
                const uint64_t want = (2u * top + den) / (2u * den);
                CHECK_EQ(km_depth_model_curve_mm100(&cals[i], (uint16_t)v, (uint16_t)a_list[k]), want);
            }
        }
    }
}

static void test_hand_values(void)
{
    const km_depth_cal_t c = { 3300, 2300 };
    /* a = 2: n = 0.5 -> 340 * 0.5 * 3 / 2 = 255; a = 0: linear -> 170 */
    CHECK_EQ(km_depth_model_curve_mm100(&c, 2800, 128), 255);
    CHECK_EQ(km_depth_model_curve_mm100(&c, 2800, 0), 170);
    CHECK_EQ(km_depth_model_mm100(&c, 2800), 255);
    /* n = 0.25, a = 2: 340 * 0.25 * 3 / 1.5 = 170 */
    CHECK_EQ(km_depth_model_mm100(&c, 3050), 170);
    /* n = 0.75, a = 2: 340 * 0.75 * 3 / 2.5 = 306 */
    CHECK_EQ(km_depth_model_mm100(&c, 2550), 306);
    /* concave: always above the straight line inside the travel */
    for (unsigned v = 2301; v < 3300; v += 7) {
        CHECK(km_depth_model_mm100(&c, (uint16_t)v) >= km_depth_model_curve_mm100(&c, (uint16_t)v, 0));
    }
}

/* A larger a bends the curve more (deeper at the same reading); the cap is a = 16. */
static void test_curve_parameter(void)
{
    const km_depth_cal_t c = { 3300, 2300 };
    unsigned prev = 0;
    for (unsigned a = 0; a <= 1024; a += 64) {
        const unsigned d = km_depth_model_curve_mm100(&c, 3000, (uint16_t)a);
        CHECK(d >= prev);
        prev = d;
    }
    CHECK(km_depth_model_curve_mm100(&c, 3000, 1024) > km_depth_model_curve_mm100(&c, 3000, 128));
    CHECK_EQ(km_depth_model_curve_mm100(&c, 3000, 1025), km_depth_model_curve_mm100(&c, 3000, 1024));
    CHECK_EQ(km_depth_model_curve_mm100(&c, 3000, 65535), km_depth_model_curve_mm100(&c, 3000, 1024));
    /* no 32-bit overflow at the largest span and curvature */
    const km_depth_cal_t big = { 4095, 0 };
    CHECK_EQ(km_depth_model_curve_mm100(&big, 0, 1024), KM_TRAVEL_MM100);
    CHECK(km_depth_model_curve_mm100(&big, 1, 1024) >= KM_TRAVEL_MM100 - 1u);
    CHECK(km_depth_model_curve_mm100(&big, 2048, 1024) > 300);
    CHECK(km_depth_model_curve_mm100(&big, 4094, 1024) <= 40);
}

/* The result depends only on the normalised press, not on the key's absolute readings. */
static void test_scale_invariance(void)
{
    const km_depth_cal_t a = { 3300, 2300 }, b = { 3000, 2000 }, c = { 3600, 2100 };
    for (unsigned n = 0; n <= 1000; n += 50) {
        const unsigned da = km_depth_model_mm100(&a, (uint16_t)(3300 - n));
        const unsigned db = km_depth_model_mm100(&b, (uint16_t)(3000 - n));
        const unsigned dc = km_depth_model_mm100(&c, (uint16_t)(3600 - n * 15 / 10));
        CHECK_EQ(da, db);
        CHECK(absdiff(da, dc) <= 1);
    }
}

/* Depth jitter for +-15 counts of noise. Near rest the curve is steepest (about 0.010 mm per count at
 * span 1000), near the bottom it is flat (about 0.001 mm per count). */
static void test_noise(void)
{
    unsigned worst_all = 0, worst_mid = 0, worst_bottom = 0;
    for (unsigned i = 0; i < DEPTH_CAL_COUNT; i++) {
        const unsigned rel = k_depth_cal[i].released, bot = k_depth_cal[i].bottom;
        const km_depth_cal_t c = cal_of(rel, bot);
        for (unsigned v = bot + 15; v + 15 <= rel; v++) {
            const unsigned d = km_depth_model_mm100(&c, (uint16_t)v);
            const unsigned hi = km_depth_model_mm100(&c, (uint16_t)(v - 15));
            const unsigned lo = km_depth_model_mm100(&c, (uint16_t)(v + 15));
            const unsigned j = (hi - d) > (d - lo) ? hi - d : d - lo;
            if (j > worst_all) {
                worst_all = j;
            }
            if (d >= 120 && d <= 220 && j > worst_mid) {
                worst_mid = j;
            }
            if (d >= 300 && j > worst_bottom) {
                worst_bottom = j;
            }
        }
    }
    printf("  noise +-15 counts: worst jitter %u (0.01 mm) anywhere, %u at 1.2..2.2 mm, %u below 3.0 mm travel\n",
           worst_all, worst_mid, worst_bottom);
    CHECK(worst_mid <= 10);    /* mid travel: at most 0.10 mm (the key with the smallest span, 998 counts) */
    CHECK(worst_mid >= 8);     /* the bound is not vacuous */
    CHECK(worst_all <= 15);    /* near rest: at most 0.15 mm */
    CHECK(worst_all >= 13);
    CHECK(worst_bottom <= 3);  /* the last 0.4 mm is quiet */
}

static void test_degenerate(void)
{
    static const km_depth_cal_t bad[] = {
        { 2000, 2000 }, { 2000, 2100 }, { 0, 0 }, { 0, 65535 }, { 65535, 65535 },
        { 2063, 2000 },                       /* span 63 < min */
        { 2001, 2000 }, { 3000, 2999 },       /* tiny spans */
        { 5000, 904 },                        /* span 4096 > max */
        { 65535, 0 }, { 65535, 100 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        for (unsigned v = 0; v < 65536; v += 97) {
            CHECK_EQ(km_depth_model_mm100(&bad[i], (uint16_t)v), 0);
        }
        CHECK_EQ(km_depth_model_mm100(&bad[i], bad[i].bottom), 0);
        CHECK_EQ(km_depth_model_mm100(&bad[i], bad[i].released), 0);
        CHECK_EQ(km_depth_model_curve_mm100(&bad[i], 2000, 1024), 0);
    }
    CHECK_EQ(km_depth_model_mm100(NULL, 2000), 0);
    /* the boundaries themselves are usable */
    const km_depth_cal_t lo = { 2064, 2000 }, hi = { 4095, 0 };
    CHECK_EQ(km_depth_model_mm100(&lo, 2000), KM_TRAVEL_MM100);
    CHECK_EQ(km_depth_model_mm100(&lo, 2064), 0);
    CHECK(km_depth_model_mm100(&lo, 2032) > 0);
    CHECK_EQ(km_depth_model_mm100(&hi, 0), KM_TRAVEL_MM100);
    CHECK_EQ(km_depth_model_mm100(&hi, 4095), 0);
    CHECK(km_depth_model_mm100(&hi, 2048) > 0);
}

void test_depth_model(void)
{
    test_constants();
    test_endpoints_and_clamp();
    test_monotonic_all();
    test_matches_reference();
    test_exact_rounding();
    test_hand_values();
    test_curve_parameter();
    test_scale_invariance();
    test_noise();
    test_degenerate();
}
