/*
 * test_engine_filter.c - the adaptive filter modes of the key engine (KM_ENGINE_FILTER_MODES, FILTER_NOTES.md).
 *
 * Two builds run this file: the default one (flag 0: the modes are compiled out, every mode must behave as the original filter) and
 * `run_tests.py --filter-modes` (flag 1: the modes exist). The recurrences below are written out independently of the engine.
 */
#include <string.h>

#include "km/km_depth_model.h"
#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km_test.h"

#define REST 3300u

static unsigned first_key(void)
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            return s;
        }
    }
    return 0;
}

static void rig(km_engine_t *e, unsigned mode)
{
    uint16_t idle[KM_ENGINE_SLOTS];
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        idle[s] = REST;
    }
    km_engine_init(e);
    CHECK(km_engine_set_filter_mode(e, mode));
    km_engine_calibrate_begin(e);
    for (unsigned i = 0; i < 64; i++) {
        km_engine_calibrate_idle(e, idle);
    }
    CHECK_EQ(km_engine_calibrate_finish(e), KM_ENGINE_CAL_OK);
}

/* the filter model: gain shift per frame from the distance of the input to the state (Q4 counts) */
static int model_shift(int d_q4, unsigned mode)
{
    if (mode == 0) {
        return 2;
    }
    const int ad = d_q4 < 0 ? -d_q4 : d_q4;
    if (mode == 3) {
        return ad < (3 << 4) ? 3 : ad < (8 << 4) ? 2 : 1;
    }
    return ad < (12 << 4) ? 3 : ad < (32 << 4) ? 2 : 1;    /* the documented thresholds, not the macros */
}

/* a key held at REST - offset: the engine's filtered reading follows the model frame by frame */
static void follow(unsigned mode_set, unsigned mode_model, unsigned offset)
{
    km_engine_t e;
    uint16_t in[KM_ENGINE_SLOTS];
    const unsigned slot = first_key();
    rig(&e, mode_set);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        in[s] = REST;
    }
    in[slot] = (uint16_t)(REST - offset);
    int f = (int)(REST << 4);
    for (unsigned i = 0; i < 60; i++) {
        km_engine_update(&e, in);
        const int x = (int)((REST - offset) << 4);
        f += (x - f) >> model_shift(x - f, mode_model);
        CHECK_EQ(km_engine_filtered(&e, slot), (unsigned)((f + 8) >> 4));
    }
}

static void test_mode_api(void)
{
    km_engine_t e;
    km_engine_init(&e);
    CHECK_EQ(km_engine_get_filter_mode(&e), KM_ENGINE_FMODE_FIXED);       /* the default is the original filter */
    for (unsigned m = 0; m <= KM_ENGINE_FMODE_LAST; m++) {
        CHECK(km_engine_set_filter_mode(&e, m));
        CHECK_EQ(km_engine_get_filter_mode(&e), m);
    }
    CHECK(!km_engine_set_filter_mode(&e, KM_ENGINE_FMODE_LAST + 1u));
    CHECK(!km_engine_set_filter_mode(&e, 255u));
    CHECK_EQ(km_engine_get_filter_mode(&e), KM_ENGINE_FMODE_LAST);        /* a refused value changes nothing */
}

static void test_follow(void)
{
    static const unsigned offsets[] = { 2, 3, 4, 7, 8, 9, 11, 12, 20, 31, 32, 60, 400 };      /* around both thresholds, and a stroke */
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        follow(KM_ENGINE_FMODE_FIXED, 0, offsets[i]);
#if KM_ENGINE_FILTER_MODES
        follow(KM_ENGINE_FMODE_LOW_LATENCY, 1, offsets[i]);
        follow(KM_ENGINE_FMODE_QUIET, 2, offsets[i]);
        follow(KM_ENGINE_FMODE_FAST, 3, offsets[i]);
#else
        follow(KM_ENGINE_FMODE_LOW_LATENCY, 0, offsets[i]);                     /* compiled out: the mode is ignored */
        follow(KM_ENGINE_FMODE_QUIET, 0, offsets[i]);
        follow(KM_ENGINE_FMODE_FAST, 0, offsets[i]);
#endif
    }
}

/* a quiet key: the adaptive filters pass less of a small disturbance (gain 1/8 under 12 counts) and follow a stroke faster (gain 1/2 over 32) */
static void test_behaviour(void)
{
#if KM_ENGINE_FILTER_MODES
    km_engine_t e0, e2;
    uint16_t in[KM_ENGINE_SLOTS];
    const unsigned slot = first_key();
    rig(&e0, 0);
    rig(&e2, KM_ENGINE_FMODE_QUIET);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        in[s] = REST;
    }
    /* a 6-count blip for 3 frames: the adaptive filter moves less */
    in[slot] = REST - 6u;
    for (unsigned i = 0; i < 3; i++) {
        km_engine_update(&e0, in);
        km_engine_update(&e2, in);
    }
    CHECK(km_engine_filtered(&e2, slot) >= km_engine_filtered(&e0, slot));
    CHECK(REST - km_engine_filtered(&e2, slot) < REST - km_engine_filtered(&e0, slot));
    /* a 400-count stroke: the adaptive filter is deeper (a smaller reading) after 3 frames, and both arrive */
    rig(&e0, 0);
    rig(&e2, KM_ENGINE_FMODE_QUIET);
    in[slot] = REST - 400u;
    for (unsigned i = 0; i < 3; i++) {
        km_engine_update(&e0, in);
        km_engine_update(&e2, in);
    }
    CHECK(km_engine_filtered(&e2, slot) < km_engine_filtered(&e0, slot));
    for (unsigned i = 0; i < 80; i++) {
        km_engine_update(&e0, in);
        km_engine_update(&e2, in);
    }
    CHECK(km_engine_filtered(&e0, slot) <= REST - 399u);
    CHECK(km_engine_filtered(&e2, slot) <= REST - 399u);
#endif
}

/* switching the mode while running leaves the engine in a sane state: the key still presses and releases */
static void test_switch_midflight(void)
{
    km_engine_t e;
    uint16_t in[KM_ENGINE_SLOTS];
    const unsigned slot = first_key();
    rig(&e, 0);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        in[s] = REST;
    }
    km_engine_key_cfg_t c;
    km_engine_key_cfg_default(&c);                                    /* FIXED 1.20 / 0.80 mm */
    CHECK(km_engine_set_key_cfg(&e, slot, &c));
    for (unsigned m = 0; m <= KM_ENGINE_FMODE_LAST; m++) {
        CHECK(km_engine_set_filter_mode(&e, m));
        in[slot] = REST - 700u;
        for (unsigned i = 0; i < 40; i++) {
            km_engine_update(&e, in);
        }
        CHECK(km_engine_slot_down(&e, slot));
        in[slot] = REST;
        for (unsigned i = 0; i < 60; i++) {
            km_engine_update(&e, in);
        }
        CHECK(!km_engine_slot_down(&e, slot));
    }
}


/* ---- fast confirm ---- */

/* the reading at which the engine's default model (released REST, bottom REST - 1000) shows `mm100` of depth */
static uint16_t counts_at(unsigned mm100)
{
    const km_depth_cal_t cal = { REST, REST - KM_ENGINE_DEFAULT_SPAN };
    unsigned v = REST;
    while (v > REST - KM_ENGINE_DEFAULT_SPAN && km_depth_model_mm100(&cal, (uint16_t)v) < mm100) {
        v--;
    }
    return (uint16_t)v;
}

/* frames until slot goes down (press = 1) / up (press = 0) after the key jumps to `depth` (mm100); 0 if it never does within 80 frames */
static unsigned frames_to_event_sw(unsigned mode, bool fast, unsigned from_depth, unsigned to_depth, bool want_down, unsigned rt, bool continuous,
                                   unsigned switch_after, unsigned switch_to)
{
    km_engine_t e;
    uint16_t in[KM_ENGINE_SLOTS];
    const unsigned slot = first_key();
    rig(&e, mode);
    km_engine_set_fast_confirm(&e, fast);
    km_engine_key_cfg_t c;
    km_engine_key_cfg_default(&c);
    c.mode = KM_ENGINE_TRIG_RAPID;
    c.rt_down_mm100 = (uint16_t)rt;
    c.rt_up_mm100 = (uint16_t)rt;
    c.press_mm100 = 70;
    c.top_dead_mm100 = 0;
    c.bottom_dead_mm100 = 0;
    c.continuous = continuous ? 1u : 0u;
    CHECK(km_engine_set_key_cfg(&e, slot, &c));
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        in[s] = REST;
    }
    in[slot] = counts_at(from_depth);
    for (unsigned i = 0; i < 80; i++) {
        km_engine_update(&e, in);
    }
    if (want_down) {
        CHECK(!km_engine_slot_down(&e, slot));
    } else {
        CHECK(km_engine_slot_down(&e, slot));
    }
    in[slot] = counts_at(to_depth);
    for (unsigned i = 1; i <= 80; i++) {
        km_engine_update(&e, in);
        if (i == switch_after) {
            CHECK(km_engine_set_filter_mode(&e, switch_to));
        }
        if (km_engine_slot_down(&e, slot) == want_down) {
            return i;
        }
    }
    return 0;
}

static unsigned frames_to_event(unsigned mode, bool fast, unsigned from_depth, unsigned to_depth, bool want_down, unsigned rt, bool continuous)
{
    return frames_to_event_sw(mode, fast, from_depth, to_depth, want_down, rt, continuous, 0, 0);
}

/* Pinned frame counts (the engine's own numbers, with the filter equivalence to the lab candidates proved in test_filter_traces.c):
 * a regression net for the second stage, which no accessor shows. Index: mode * 2 + fast; columns: long press, long release. */
static void test_pinned_timing(void)
{
    static const unsigned pin[8][2] = { { 14, 10 }, { 9, 9 }, { 8, 6 }, { 3, 3 }, { 9, 7 }, { 4, 4 }, { 4, 3 }, { 3, 3 } };
    for (unsigned mode = 0; mode <= KM_ENGINE_FMODE_LAST; mode++) {
        for (unsigned fast = 0; fast < 2; fast++) {
            const unsigned p = frames_to_event(mode, fast != 0, 0, 150, true, 25, false);
            const unsigned r = frames_to_event(mode, fast != 0, 150, 30, false, 25, false);
#if KM_ENGINE_FILTER_MODES
            CHECK_EQ(p, pin[mode * 2 + fast][0]);
            CHECK_EQ(r, pin[mode * 2 + fast][1]);
#else
            CHECK_EQ(p, pin[0][0]);                     /* compiled out: only the original filter exists */
            CHECK_EQ(r, pin[0][1]);
#endif
        }
    }
#if KM_ENGINE_FILTER_MODES
    /* a mode change in the middle of a stroke: the second stage restarts from the first, so the press is still found */
    CHECK_EQ(frames_to_event_sw(0, false, 0, 150, true, 25, false, 3, KM_ENGINE_FMODE_LOW_LATENCY), 11u);
    CHECK_EQ(frames_to_event_sw(0, false, 0, 150, true, 25, false, 3, KM_ENGINE_FMODE_QUIET), 11u);
#endif
}

static void test_fast_confirm(void)
{
    CHECK(!km_engine_get_fast_confirm(&(km_engine_t){ 0 }));
    for (unsigned mode = 0; mode <= KM_ENGINE_FMODE_LAST; mode++) {
        /* a long press stroke (0 -> 1.5 mm, rt 0.25 mm) and a long release stroke (1.5 -> 0.3 mm) */
        const unsigned p_off = frames_to_event(mode, false, 0, 150, true, 25, false);
        const unsigned p_on = frames_to_event(mode, true, 0, 150, true, 25, false);
        const unsigned r_off = frames_to_event(mode, false, 150, 30, false, 25, false);
        const unsigned r_on = frames_to_event(mode, true, 150, 30, false, 25, false);
        CHECK(p_off > 0 && p_on > 0 && r_off > 0 && r_on > 0);
        /* a small stroke (about 1.2 x rt): no difference at all */
        const unsigned s_off = frames_to_event(mode, false, 0, 30, true, 25, true);
        const unsigned s_on = frames_to_event(mode, true, 0, 30, true, 25, true);
        CHECK(s_off > 0);
        CHECK_EQ(s_off, s_on);
#if KM_ENGINE_FILTER_MODES
        /* the debounce goes from 8 / 6 frames to 3; the slow fixed-gain filter has often counted most of them before the stroke is long enough */
        CHECK(p_on < p_off);
        CHECK(r_on <= r_off);
        if (mode == KM_ENGINE_FMODE_LOW_LATENCY || mode == KM_ENGINE_FMODE_QUIET) {
            CHECK(p_on + 4u <= p_off);
            CHECK(r_on + 2u <= r_off);
        }
        CHECK(p_on >= KM_ENGINE_FAST_FRAMES && r_on >= KM_ENGINE_FAST_FRAMES);
#else
        CHECK_EQ(p_on, p_off);                      /* compiled out */
        CHECK_EQ(r_on, r_off);
#endif
    }
}

void test_engine_filter(void)
{
    test_mode_api();
    test_follow();
    test_behaviour();
    test_switch_midflight();
    test_fast_confirm();
    test_pinned_timing();
}
