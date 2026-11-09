/* Filter lab (M6 P12b): the harness itself is tested here, not the candidates' merits. The lab copy of the engine must equal the real
 * engine, the noise models must have the standard deviations they claim, the ideal reference and the event matching must be right,
 * traces must load (and bad files must be refused), and the candidates' hooks must do what core/FILTER_NOTES.md says. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <km/km_depth_model.h>
#include <km/km_keymap.h>

#include "filter_lab.h"
#include "filter_lab_hooks.h"
#include "km_test.h"

static void rapid_cfg(km_engine_key_cfg_t *c, unsigned rt)
{
    km_engine_key_cfg_default(c);
    c->mode = KM_ENGINE_TRIG_RAPID;
    c->rt_down_mm100 = c->rt_up_mm100 = (uint16_t)rt;
    c->top_dead_mm100 = LAB_TD;
    c->continuous = 1;
    c->press_mm100 = 0;
}

static void first_key_slots(lab_run_t *r, unsigned n)
{
    unsigned k = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && k < n; s++) {
        if (km_keymap_slot_is_key(s)) {
            r->slot[k++] = s;
        }
    }
    r->n_keys = n;
}

/* ------------------------------------------------------------------ the pieces */

static void test_maths_and_rng(void)
{
    CHECK(lab_abs(lab_sqrt(2.0) - 1.4142135623730951) < 1e-12);
    CHECK(lab_sqrt(0) == 0 && lab_sqrt(-1) == 0);
    lab_rng_t r;
    lab_rng_seed(&r, 7);
    double n = 0, s = 0, ss = 0, u = 0;
    for (int i = 0; i < 200000; i++) {
        const double g = lab_rng_gauss(&r);
        n++; s += g; ss += g * g;
        u += lab_rng_uniform(&r);
    }
    CHECK(lab_abs(s / n) < 0.02);
    CHECK(lab_abs(lab_stat_sd(n, s, ss) - 1.0) < 0.02);
    CHECK(lab_abs(u / n - 0.5) < 0.01);
    lab_rng_t a, b;
    lab_rng_seed(&a, 3);
    lab_rng_seed(&b, 3);
    CHECK(lab_rng_uniform(&a) == lab_rng_uniform(&b));
    lab_rng_seed(&b, 4);
    CHECK(lab_rng_uniform(&a) != lab_rng_uniform(&b));
    double x[5] = { 5, 1, 4, 2, 3 };
    CHECK(lab_median(x, 5) == 3);
    CHECK(lab_percentile(x, 5, 0) == 1 && lab_percentile(x, 5, 100) == 5 && lab_abs(lab_percentile(x, 5, 25) - 2) < 1e-9);
    CHECK(lab_stat_sd(1, 5, 25) == 0);
}

static void test_sensor_truth(void)
{
    lab_sensor_t s;
    lab_sensor_init(&s, 3300, 1000);
    const km_depth_cal_t cal = { 3300, 2300 };
    CHECK_EQ(s.counts_at[0], 3300);
    CHECK_EQ(s.counts_at[KM_TRAVEL_MM100], 2300);
    for (unsigned d = 0; d <= KM_TRAVEL_MM100; d += 5) {
        const double c = lab_sensor_counts(&s, d);
        const int back = km_depth_model_mm100(&cal, (uint16_t)(c + 0.5));
        CHECK(back >= (int)d - 2 && back <= (int)d + 2);               /* the integer model is within a unit or two of the inverse */
    }
    CHECK(lab_sensor_counts(&s, -5) == 3300 && lab_sensor_counts(&s, 400) == 2300);
    for (unsigned d = 1; d <= KM_TRAVEL_MM100; d++) {                   /* deeper = lower reading */
        CHECK(lab_sensor_counts(&s, d) <= lab_sensor_counts(&s, d - 1u));
    }
}

static void test_paths_and_ideal(void)
{
    lab_path_t p;
    CHECK(lab_path_init(&p, 200000) == 0);
    lab_path_hold(&p, 0.1);
    lab_path_ramp(&p, 150, 300.0);                                       /* 0.3 m/s = 30 mm100 per ms */
    const unsigned long n_down = p.n;
    CHECK(n_down > 2330 + 100 && n_down < 2330 + 130);                   /* 0.1 s = 2330 frames, then 150 / 30 ms = 5 ms = 116 frames */
    CHECK_EQ(p.d[p.n - 1], 150);
    lab_path_ramp(&p, 0, 300.0);
    CHECK_EQ(p.d[p.n - 1], 0);
    lab_path_hold(&p, 0.05);
    lab_event_t ev[16];
    unsigned n = lab_ideal_rapid(&p, 25, 25, ev, 16);
    CHECK_EQ(n, 2);
    CHECK(ev[0].type == LAB_EV_PRESS && ev[1].type == LAB_EV_RELEASE);
    CHECK(p.d[ev[0].frame] >= 25 && p.d[ev[0].frame - 1] < 25);          /* the press at exactly rt_down below the shallowest point */
    CHECK(p.d[ev[1].frame] <= 125 && p.d[ev[1].frame - 1] > 125);        /* the release rt_up above the deepest point */
    /* a key that never gets deeper than rt_down does not press; one that reverses by less than rt_up does not release */
    lab_path_t q;
    CHECK(lab_path_init(&q, 100000) == 0);
    lab_path_ramp(&q, 22, 100.0);
    CHECK_EQ(lab_ideal_rapid(&q, 25, 25, ev, 16), 0);
    lab_path_ramp(&q, 60, 100.0);
    lab_path_ramp(&q, 45, 100.0);                                        /* 15 back: no release at rt_up 25 */
    CHECK_EQ(lab_ideal_rapid(&q, 25, 25, ev, 16), 1);
    lab_path_ramp(&q, 30, 100.0);                                        /* 30 back from 60 */
    CHECK_EQ(lab_ideal_rapid(&q, 25, 25, ev, 16), 2);
    lab_path_ramp(&q, 70, 100.0);                                        /* 40 down from the shallowest 30: pressed again */
    CHECK_EQ(lab_ideal_rapid(&q, 25, 25, ev, 16), 3);
    n = lab_ideal_fixed(&p, 120, 80, ev, 16);
    CHECK_EQ(n, 2);
    CHECK(p.d[ev[0].frame] >= 120 && p.d[ev[1].frame] <= 80);
    /* wiggle and step */
    lab_path_t w;
    CHECK(lab_path_init(&w, 100000) == 0);
    lab_path_step(&w, 100);
    lab_path_wiggle(&w, 0.1, 10, 8);
    double lo = 1e9, hi = -1e9;
    for (unsigned long i = 0; i < w.n; i++) {
        if (w.d[i] < lo) lo = w.d[i];
        if (w.d[i] > hi) hi = w.d[i];
    }
    CHECK(lo > 89.5 && lo < 90.5 && hi > 109.5 && hi < 110.5);            /* wiggle: centre = where the path was, +-amplitude */
    CHECK_EQ(w.d[w.n - 1], 100);
    lab_path_free(&p); lab_path_free(&q); lab_path_free(&w);
}

static void test_matching(void)
{
    lab_event_t ideal[3] = { { 1000, 0, LAB_EV_PRESS }, { 3000, 0, LAB_EV_RELEASE }, { 5000, 0, LAB_EV_PRESS } };
    lab_event_t real[5] = { { 1023, 0, LAB_EV_PRESS }, { 1500, 0, LAB_EV_RELEASE }, { 3046, 0, LAB_EV_RELEASE }, { 1200, 1, LAB_EV_PRESS },
                            { 9999, 0, LAB_EV_PRESS } };
    lab_match_t *m = (lab_match_t *)malloc(sizeof *m);
    lab_match(ideal, 3, real, 5, 0, 10, 200, m);
    CHECK_EQ(m->matched, 2);
    CHECK_EQ(m->missed, 1);                                              /* the press at 5000 has no partner within 200 frames */
    CHECK_EQ(m->extra, 2);                                               /* the release at 1500 and the press at 9999; key 1 is not counted */
    CHECK_EQ(m->n_lat[LAB_EV_PRESS], 1);
    CHECK(lab_abs(m->lat_ms[LAB_EV_PRESS][0] - 23.0 * 1000.0 / LAB_FS) < 1e-9);
    CHECK(lab_abs(m->lat_ms[LAB_EV_RELEASE][0] - 46.0 * 1000.0 / LAB_FS) < 1e-9);
    lab_match(ideal, 3, real, 5, 0, 10, 10, m);                          /* a window too tight for 23 frames */
    CHECK_EQ(m->matched, 0);
    /* an event a few frames BEFORE the ideal one still pairs (negative latency) inside the early window, and not outside it */
    lab_event_t id1[1] = { { 1000, 0, LAB_EV_PRESS } }, re1[1] = { { 990, 0, LAB_EV_PRESS } };
    lab_match(id1, 1, re1, 1, 0, 10, 50, m);
    CHECK_EQ(m->matched, 1);
    CHECK(m->lat_ms[LAB_EV_PRESS][0] < 0);
    lab_match(id1, 1, re1, 1, 0, 5, 50, m);
    CHECK_EQ(m->matched, 0);
    CHECK_EQ(m->extra, 1);
    free(m);
}

/* ------------------------------------------------------------------ the candidates' hooks */

static void test_hooks(void)
{
    lab_set_variant(&lab_variants[0]);
    for (int f = 0; f < 6000; f += 37) {
        for (int x = -4000; x < 70000; x += 997) {
            CHECK_EQ(lab_hook_stage1(f, x), f + ((x - f) >> KM_ENGINE_FILTER_SHIFT));
            CHECK_EQ(lab_hook_stage2(f, x), f + (((x - f) + (1 << (KM_ENGINE_RT_FILTER_SHIFT - 1))) >> KM_ENGINE_RT_FILTER_SHIFT));
        }
    }
    const lab_variant_t *a = lab_variant_by_name("F1A"), *b = lab_variant_by_name("F1B"), *c = lab_variant_by_name("F1C");
    CHECK(a != NULL && b != NULL && c != NULL && lab_variant_by_name("nope") == NULL);
    lab_set_variant(a);
    CHECK_EQ(lab_hook_stage1(1000, 5000), 1000 + (4000 >> 2));            /* F1A leaves the main filter alone */
    /* adaptive gain: |x - g| < 12 counts -> 1/8, < 32 -> 1/4, else 1/2 (thresholds t1 = 12, t2 = 32; Q4 values) */
    CHECK_EQ(lab_hook_stage2(0, 11 * 16), (11 * 16 + 4) >> 3);
    CHECK_EQ(lab_hook_stage2(0, 12 * 16), (12 * 16 + 2) >> 2);
    CHECK_EQ(lab_hook_stage2(0, 31 * 16), (31 * 16 + 2) >> 2);
    CHECK_EQ(lab_hook_stage2(0, 32 * 16), (32 * 16 + 1) >> 1);
    CHECK_EQ(lab_hook_stage2(0, -32 * 16), (-32 * 16 + 1) >> 1);
    CHECK_EQ(lab_hook_stage2(100, 100), 100);
    lab_set_variant(b);
    CHECK_EQ(lab_hook_stage2(123, 456), 456);                             /* F1B has no second stage */
    CHECK_EQ(lab_hook_stage1(0, 11 * 16), (11 * 16) >> 3);
    CHECK_EQ(lab_hook_stage1(0, 12 * 16), (12 * 16) >> 2);
    CHECK_EQ(lab_hook_stage1(0, 32 * 16), (32 * 16) >> 1);
    lab_set_variant(NULL);
    CHECK_EQ(lab_hook_stage1(0, 1600), 400);                              /* back to the engine's own filter */
}

/* ------------------------------------------------------------------ the lab engine is the real engine */

static void test_lab_equals_real(void)
{
    static const char *const profiles[] = { "white", "mixed", "spiky", "legacy", "commonmode" };
    for (unsigned pi = 0; pi < 5; pi++) {
        lab_path_t p[5];
        lab_run_t r;
        memset(&r, 0, sizeof r);
        r.noise = lab_profile_by_name(profiles[pi]);
        r.noise_scale = 1.8;                                                /* noisy enough that the events are not just the clean ones */
        r.seed = 11 + pi;
        r.n_frames = (unsigned long)(4.0 * LAB_FS);
        r.n_ref = 8;
        first_key_slots(&r, 5);
        for (unsigned k = 0; k < 5; k++) {
            lab_path_init(&p[k], r.n_frames + 16);
            lab_path_hold(&p[k], 0.2 + 0.05 * k);
            lab_path_ramp(&p[k], 60.0 + 40.0 * k, 100.0 + 80.0 * k);
            lab_path_hold(&p[k], 0.3);
            lab_path_ramp(&p[k], 20.0 + 5.0 * k, 150.0);
            lab_path_ramp(&p[k], 280.0, 300.0);
            lab_path_ramp(&p[k], 0.0, 200.0);
            lab_path_hold(&p[k], 5.0);
            r.path[k] = &p[k];
            if (k == 4) {
                km_engine_key_cfg_default(&r.cfg[k]);                     /* one FIXED key among the rapid ones */
            } else {
                rapid_cfg(&r.cfg[k], 20 + 5 * k);
            }
        }
        /* the real engine in each of its filter modes == the lab copy running the matching candidate. Without KM_ENGINE_FILTER_MODES the modes
         * are compiled out, so every mode must equal F0. */
        static const char *const cand[3] = { "F0", "F1B", "F1C" };
        for (unsigned mode = 0; mode < 3; mode++) {
        lab_result_t a, b;
        r.use_real_engine = 1;
        r.real_filter_mode = mode;
        r.variant = NULL;
        CHECK_EQ(lab_run(&r, &a), 0);
        r.use_real_engine = 0;
        r.variant = lab_variant_by_name(KM_ENGINE_FILTER_MODES ? cand[mode] : "F0");
        CHECK(r.variant != NULL);
        CHECK_EQ(lab_run(&r, &b), 0);
        CHECK(a.n_ev > 0);
        CHECK_EQ(a.n_ev, b.n_ev);
        for (unsigned i = 0; i < a.n_ev && i < b.n_ev; i++) {
            CHECK(a.ev[i].frame == b.ev[i].frame && a.ev[i].key == b.ev[i].key && a.ev[i].type == b.ev[i].type);
        }
        for (unsigned k = 0; k < 5; k++) {
            CHECK(a.key[k].sum_f == b.key[k].sum_f && a.key[k].sumsq_f == b.key[k].sumsq_f);
            CHECK(a.key[k].sum_g == b.key[k].sum_g);
            for (int i = 0; i < LAB_NUM_A; i++) {
                CHECK(a.key[k].a_sum[i] == b.key[k].a_sum[i] && a.key[k].a_jumps[i] == b.key[k].a_jumps[i]);
            }
        }
        CHECK_EQ(a.glitches, b.glitches);
        lab_result_free(&a);
        lab_result_free(&b);
        }
        for (unsigned k = 0; k < 5; k++) lab_path_free(&p[k]);
    }
}

/* ------------------------------------------------------------------ noise models have the sd they claim */

/* sd of the main filter f (and of the rapid-trigger stage g) of a key at rest (hold = 0) or held at depth `hold` (mm100) */
static double rest_sd(const char *profile, double scale, unsigned seconds, const char *filter, double *sd_g, double hold)
{
    lab_path_t path;
    lab_path_init(&path, (unsigned long)(seconds * LAB_FS) + 16);
    if (hold > 0) {
        lab_path_ramp(&path, hold, 100.0);
    }
    lab_path_hold(&path, seconds);
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.noise = lab_profile_by_name(profile);
    r.noise_scale = scale;
    r.seed = 5;
    r.n_frames = (unsigned long)(seconds * LAB_FS);
    r.stat_from = 2000;
    r.variant = lab_variant_by_name(filter);
    r.n_ref = 16;
    r.path[0] = &path;
    first_key_slots(&r, 1);
    rapid_cfg(&r.cfg[0], 20);
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    const lab_key_stats_t *s = &res.key[0];
    if (sd_g != NULL) {
        *sd_g = lab_stat_sd(s->n, s->sum_g, s->sumsq_g);
    }
    const double sd = lab_stat_sd(s->n, s->sum_f, s->sumsq_f);
    if (scale <= 1.0) {
        CHECK(res.n_ev <= (hold > 0 ? 1u : 0u));                          /* documented-size noise: at rest nothing, held only the press */
    }
    lab_result_free(&res);
    lab_path_free(&path);
    return sd;
}

static void test_noise_models(void)
{
    double sg = 0;
    /* the legacy model is ENGINE_NOTES.md's: +-15 uniform at the engine input -> f sd 3.3, rapid-trigger stage g sd 1.9 */
    const double sd = rest_sd("legacy", 1.0, 12, "F0", NULL, 0);
    rest_sd("legacy", 1.0, 12, "F0", &sg, 150);
    CHECK(sd > 3.1 && sd < 3.5);
    CHECK(sg > 1.7 && sg < 2.1);
    /* every other profile is scaled to the same f sd at scale 1 ... */
    static const char *const p[] = { "white", "mixed", "lowfreq", "commonmode", "spiky", "tonal" };
    for (unsigned i = 0; i < 6; i++) {
        const double s = rest_sd(p[i], 1.0, 20, "F0", NULL, 0);
        if (s <= 2.7 || s >= 4.2) printf("  profile %s sd_f %.3f\n", p[i], s);
        CHECK(s > 2.7 && s < 4.2);
    }
    /* ... and scale is linear */
    const double s1 = rest_sd("white", 1.0, 12, "F0", NULL, 0), s2 = rest_sd("white", 2.0, 12, "F0", NULL, 0);
    CHECK(lab_abs(s2 / s1 - 2.0) < 0.2);
    /* no noise: f sits on the baseline exactly */
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.n_frames = 3000;
    first_key_slots(&r, 1);
    rapid_cfg(&r.cfg[0], 20);
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    CHECK(lab_stat_sd(res.key[0].n, res.key[0].sum_f, res.key[0].sumsq_f) < 1e-9);
    lab_result_free(&res);
    /* the same seed gives the same run, another seed another one */
    r.noise = lab_profile_by_name("mixed");
    r.n_frames = 20000;
    r.stat_from = 1000;
    r.seed = 9;
    lab_result_t a, b, c;
    CHECK_EQ(lab_run(&r, &a), 0);
    CHECK_EQ(lab_run(&r, &b), 0);
    r.seed = 10;
    CHECK_EQ(lab_run(&r, &c), 0);
    CHECK(a.key[0].sumsq_f == b.key[0].sumsq_f && a.key[0].sumsq_f != c.key[0].sumsq_f);
    lab_result_free(&a); lab_result_free(&b); lab_result_free(&c);
}

/* the candidates that are only about noise do what they should on the noise they target */
static void test_candidates_on_their_noise(void)
{
    double g0 = 0, g1 = 0;
    const double f0 = rest_sd("spikeonly", 1.0, 20, "F0", &g0, 150);
    const double fm = rest_sd("spikeonly", 1.0, 20, "F4M", &g1, 150);
    CHECK(f0 > 0.5 && fm < 0.25 * f0 && g1 < 0.5 * g0);                                           /* the median removes isolated spikes */
    const double fc = rest_sd("spikeonly", 1.0, 20, "F4C", NULL, 150);
    CHECK(fc < 0.6 * f0);                                                /* the clamp limits them to 45 counts (they are 48..96) */
    const double c0 = rest_sd("commonmode", 1.0, 20, "F0", NULL, 0);
    const double c5 = rest_sd("commonmode", 1.0, 20, "F5", NULL, 0);
    CHECK(c5 < c0);                                                      /* subtracting the common mode lowers the sd when there is one */
    /* a deeply pressed key is not part of the common mode: the others keep their baseline */
    {
        lab_path_t pk;
        lab_path_init(&pk, (unsigned long)(10 * LAB_FS) + 16);
        lab_path_ramp(&pk, 200, 100.0);
        lab_path_hold(&pk, 10);
        lab_run_t r;
        memset(&r, 0, sizeof r);
        r.noise = lab_profile_by_name("commonmode");
        r.seed = 5;
        r.n_frames = (unsigned long)(10 * LAB_FS);
        r.stat_from = 4000;
        r.variant = lab_variant_by_name("F5");
        r.n_ref = 16;
        r.path[1] = &pk;
        first_key_slots(&r, 2);
        rapid_cfg(&r.cfg[0], 20);
        rapid_cfg(&r.cfg[1], 20);
        lab_result_t res;
        CHECK_EQ(lab_run(&r, &res), 0);
        const double mean0 = res.key[0].sum_f / res.key[0].n;
        CHECK(lab_abs(mean0 - 3300.0) < 4.0);
        lab_result_free(&res);
        lab_path_free(&pk);
    }
    const double w0 = rest_sd("white", 1.0, 20, "F0", NULL, 0);
    const double w5 = rest_sd("white", 1.0, 20, "F5", NULL, 0);
    CHECK(w5 > 0.8 * w0 && w5 < 1.3 * w0);                               /* ... and does little harm when there is none (independent noise) */
}

/* ------------------------------------------------------------------ latency on noise-free strokes */

static void stroke_latency(const char *filter, double speed, int fixed, double *press_ms, double *rel_ms, unsigned *missed, unsigned *extra)
{
    lab_path_t p;
    lab_path_init(&p, 400000);
    lab_path_hold(&p, 0.15);
    for (int c = 0; c < 3; c++) {
        lab_path_ramp(&p, 150, speed);
        lab_path_ramp(&p, 0, speed);
        lab_path_hold(&p, 0.15);
    }
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.variant = lab_variant_by_name(filter);
    r.n_frames = p.n;
    r.path[0] = &p;
    first_key_slots(&r, 1);
    if (fixed) {
        km_engine_key_cfg_default(&r.cfg[0]);
    } else {
        rapid_cfg(&r.cfg[0], 25);
    }
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    lab_event_t ideal[16];
    const unsigned ni = fixed ? lab_ideal_fixed(&p, 120, 80, ideal, 16) : lab_ideal_rapid(&p, 25, 25, ideal, 16);
    CHECK_EQ(ni, 6);
    lab_match_t *m = (lab_match_t *)malloc(sizeof *m);
    lab_match(ideal, ni, res.ev, res.n_ev, 0, (unsigned)(0.01 * LAB_FS), (unsigned)(0.1 * LAB_FS), m);
    *missed = m->missed;
    *extra = m->extra;
    double pm = 0, rm = 0;
    for (unsigned i = 0; i < m->n_lat[1]; i++) pm += m->lat_ms[1][i];
    for (unsigned i = 0; i < m->n_lat[0]; i++) rm += m->lat_ms[0][i];
    *press_ms = m->n_lat[1] ? pm / m->n_lat[1] : -1;
    *rel_ms = m->n_lat[0] ? rm / m->n_lat[0] : -1;
    CHECK_EQ(m->n_lat[1], 3);
    CHECK_EQ(m->n_lat[0], 3);
    free(m);
    lab_result_free(&res);
    lab_path_free(&p);
}

static void test_latency(void)
{
    double p0, r0, p1, r1, pb, rb;
    unsigned m, e;
    stroke_latency("F0", 300, 0, &p0, &r0, &m, &e);
    CHECK_EQ(m, 0);
    CHECK_EQ(e, 0);
    /* the debounce alone is 8 / 6 frames (0.34 / 0.26 ms); the filters add to it: between 0.3 and 2.5 ms at 0.3 m/s */
    CHECK(p0 > 0.3 && p0 < 2.5);
    CHECK(r0 > 0.3 && r0 < 2.5);
    stroke_latency("F0", 50, 0, &p1, &r1, &m, &e);
    CHECK_EQ(m, 0);
    CHECK(p1 > -0.5 && p1 < 2.5);                                        /* slow: the noise-free engine presses about at the ideal frame */
    CHECK(r1 > 0.3 && r1 < 2.5);
    /* the single adaptive stage (F1B) is faster than the two fixed stages for a fast stroke - the whole point of the candidate */
    stroke_latency("F1B", 300, 0, &pb, &rb, &m, &e);
    CHECK_EQ(m, 0);
    CHECK_EQ(e, 0);
    CHECK(pb < p0 && rb < r0);
    stroke_latency("F1A", 300, 0, &pb, &rb, &m, &e);
    CHECK_EQ(m, 0);
    CHECK(pb <= p0 + 0.05 && rb <= r0 + 0.05);                            /* F1A changes only the slow stage: never slower */
    /* a FIXED key crosses 1.2 mm / 0.8 mm: only the main filter and the debounce */
    stroke_latency("F0", 300, 1, &p0, &r0, &m, &e);
    CHECK_EQ(m, 0);
    CHECK_EQ(e, 0);
    CHECK(p0 > 0.3 && p0 < 2.5);
    stroke_latency("F1B", 300, 1, &pb, &rb, &m, &e);
    CHECK(pb > p0 - 0.3 && pb < p0 + 0.3);   /* with the thresholds 12 / 32 an adaptive main filter is no faster on a 0.3 m/s ramp: the lag is below t1 */
}

/* ------------------------------------------------------------------ traces */

static int path_of(const char *name, char *out, size_t cap)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", __FILE__);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) slash = bslash;
    if (slash == NULL) return -1;
    *slash = '\0';
    return snprintf(out, cap, "%s/traces/%s", path, name) < (int)cap ? 0 : -1;
}

static void test_traces(void)
{
    char path[1100];
    CHECK_EQ(path_of("synthetic_idle_2col.kmtr", path, sizeof path), 0);
    lab_trace_t t;
    CHECK_EQ(lab_trace_load(&t, path), 0);
    CHECK_EQ(t.n_cols, 2);
    CHECK_EQ(t.slots[0], 0);
    CHECK_EQ(t.slots[1], 1);
    CHECK_EQ(t.n_frames, 6000);
    CHECK(lab_abs(t.fs - 23300.0) < 0.5);
    double n = 0, s = 0, ss = 0;
    for (unsigned long i = 0; i < t.n_frames; i++) {
        CHECK(t.v[i * 2] <= 0x0FFF);                                       /* the condition flag (bit 15) is stripped */
        const double v = t.v[i * 2];
        n++; s += v; ss += v * v;
    }
    CHECK(lab_abs(s / n - 3300.0) < 1.0);
    CHECK(lab_abs(lab_stat_sd(n, s, ss) - 15.0) < 1.0);
    /* replay through the engine: two idle keys, no events; the filter sd matches 15 counts of raw white noise (f sd about 3.3) */
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.trace = &t;
    r.n_frames = t.n_frames;
    r.n_keys = 2;
    r.slot[0] = t.slots[0];
    r.slot[1] = t.slots[1];
    rapid_cfg(&r.cfg[0], 20);
    km_engine_key_cfg_default(&r.cfg[1]);
    r.stat_from = 500;
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    CHECK_EQ(res.n_ev, 0);
    const double sd0 = lab_stat_sd(res.key[0].n, res.key[0].sum_f, res.key[0].sumsq_f);
    CHECK(sd0 > 2.7 && sd0 < 4.2);
    CHECK(res.key[1].n > 5000);
    /* the same trace through the real engine gives the same numbers */
    lab_result_t res2;
    r.use_real_engine = 1;
    CHECK_EQ(lab_run(&r, &res2), 0);
    CHECK(res2.key[0].sum_f == res.key[0].sum_f && res2.key[1].sumsq_f == res.key[1].sumsq_f);
    lab_result_free(&res);
    lab_result_free(&res2);
    lab_trace_free(&t);

    /* bad files are refused */
    CHECK(lab_trace_load(&t, "does/not/exist.kmtr") != 0);
    CHECK_EQ(path_of("synthetic_idle_2col.kmtr", path, sizeof path), 0);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        static uint8_t buf[30000];
        const size_t len = fread(buf, 1, sizeof buf, f);
        fclose(f);
        CHECK(len > 24000);
        const size_t cuts[] = { 10, 21, len - 1 };
        for (unsigned c = 0; c < 3; c++) {
            char tmp[L_tmpnam + 8];
            CHECK(tmpnam(tmp) != NULL);
            FILE *o = fopen(tmp, "wb");
            if (o != NULL) {
                fwrite(buf, 1, cuts[c], o);
                fclose(o);
                CHECK(lab_trace_load(&t, tmp) != 0);                       /* truncated */
                remove(tmp);
            }
        }
        buf[0] = 'X';
        char tmp[L_tmpnam + 8];
        CHECK(tmpnam(tmp) != NULL);
        FILE *o = fopen(tmp, "wb");
        if (o != NULL) {
            fwrite(buf, 1, len, o);
            fclose(o);
            CHECK(lab_trace_load(&t, tmp) != 0);                           /* bad magic */
            remove(tmp);
        }
    }
}

/* ------------------------------------------------------------------ the measured noise (profile "real") through the real engine */

#if KM_ENGINE_FILTER_MODES

/* false events of four still keys at 1.5 mm in the real engine, mode `mode`, noise x scale, rapid trigger rt (mm100) */
static unsigned real_false_events(unsigned mode, double scale, unsigned rt, unsigned seconds)
{
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.noise = lab_profile_by_name("real");
    r.noise_scale = scale;
    r.seed = 5;
    r.n_frames = (unsigned long)(seconds * LAB_FS);
    r.stat_from = 2000;
    first_key_slots(&r, 4);
    lab_path_t p[4];
    for (unsigned k = 0; k < 4; k++) {
        lab_path_init(&p[k], r.n_frames + 16);
        lab_path_ramp(&p[k], 150.0, 300.0);
        lab_path_hold(&p[k], (double)seconds);
        r.path[k] = &p[k];
        rapid_cfg(&r.cfg[k], rt);
    }
    r.use_real_engine = 1;
    r.real_filter_mode = mode;
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    unsigned ev = 0;
    for (unsigned i = 0; i < res.n_ev; i++) {
        if (res.ev[i].frame > 2000 + (unsigned long)(0.1 * LAB_FS)) {
            ev++;
        }
    }
    lab_result_free(&res);
    for (unsigned k = 0; k < 4; k++) lab_path_free(&p[k]);
    return ev;
}

/* frame of the first press of key 0 in a 300 mm/s stroke to 1.5 mm */
static unsigned long real_press_frame(unsigned mode, double scale)
{
    lab_run_t r;
    memset(&r, 0, sizeof r);
    r.noise = lab_profile_by_name("real");
    r.noise_scale = scale;
    r.seed = 9;
    r.n_frames = (unsigned long)(0.4 * LAB_FS);
    first_key_slots(&r, 1);
    lab_path_t p;
    lab_path_init(&p, r.n_frames + 16);
    lab_path_hold(&p, 0.05);
    lab_path_ramp(&p, 150.0, 300.0);
    lab_path_hold(&p, 1.0);
    r.path[0] = &p;
    rapid_cfg(&r.cfg[0], 25);
    r.use_real_engine = 1;
    r.real_filter_mode = mode;
    lab_result_t res;
    CHECK_EQ(lab_run(&r, &res), 0);
    unsigned long f = 0;
    for (unsigned i = 0; i < res.n_ev; i++) {
        if (res.ev[i].type == LAB_EV_PRESS) { f = res.ev[i].frame; break; }
    }
    lab_result_free(&res);
    lab_path_free(&p);
    return f;
}

static void test_real_noise(void)
{
    for (unsigned mode = 0; mode <= KM_ENGINE_FMODE_LAST; mode++) {
        CHECK_EQ(real_false_events(mode, 1.0, 20, 20), 0);            /* the measured noise: no false event at the 0.20 mm floor, any mode */
    }
    CHECK_EQ(real_false_events(KM_ENGINE_FMODE_FAST, 3.0, 30, 20), 0); /* the fast chain keeps a margin: three times the noise at 0.30 mm */
    const unsigned long f0 = real_press_frame(KM_ENGINE_FMODE_FIXED, 1.0);
    const unsigned long f3 = real_press_frame(KM_ENGINE_FMODE_FAST, 1.0);
    CHECK(f0 > 0 && f3 > 0);
    CHECK(f3 + 6 <= f0);                                              /* at least 6 frames (0.23 ms) earlier than the original chain */
}
#else
static void test_real_noise(void)
{
}
#endif

void test_filter_traces(void)
{
    test_maths_and_rng();
    test_sensor_truth();
    test_paths_and_ideal();
    test_matching();
    test_hooks();
    test_lab_equals_real();
    test_noise_models();
    test_candidates_on_their_noise();
    test_latency();
    test_traces();
    test_real_noise();
}
