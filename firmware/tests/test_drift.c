/*
 * test_drift.c - km_drift.c against a simulated Hall sensor (hours of time-lapse). Original work.
 *
 * The simulator: every key has a true rest reading and a true bottom reading (counts); the reading for a physical depth x (0.01 mm) is the
 * inverse of the engine's own depth model for the TRUE endpoints, so a correctly calibrated engine actuates exactly at the configured depth.
 * Temperature drift moves the true rest by `drift` counts and the true bottom by drift * beta %. A "probe" runs a slow press on a COPY of the
 * engine (the real one is not disturbed) and reports the depth in 0.01 mm at which the key was reported down: the actuation point.
 * Time-lapse: one simulated second = SIM_FPS frames (the real frame rate is 23 kHz); the engine's own baseline tracker (time constant ~1.4 s real) is
 * therefore ~29x slower here (~40 s); the simulated drift rates below are kept under 0.03 counts a second so that its lag stays below the 6-count band, still far faster than the drift being simulated.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "km/km_depth_model.h"
#include "km/km_drift.h"
#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km_test.h"

#define SIM_FPS 800u
#define NKEYS_MAX 8u
#define WANT_MM100 KM_ENGINE_DEFAULT_PRESS_MM100

typedef struct {
    km_engine_t e;
    km_drift_t d;
    uint16_t rest0[KM_ENGINE_SLOTS];
    uint16_t bot0[KM_ENGINE_SLOTS];
    int drift;                       /* counts added to the true rest */
    int beta_pct;                    /* the true bottom moves by drift * beta_pct / 100 */
    int x[KM_ENGINE_SLOTS];          /* physical depth, 0.01 mm; 0 = key up */
    int noise;                       /* uniform +-noise counts */
    uint32_t rng;
    uint32_t sec;
    unsigned keys[NKEYS_MAX];        /* the key slots that are evaluated */
    unsigned nkeys;
    uint32_t presses;                /* press events seen on any key */
    int stuck_slot;                  /* >= 0: this slot always reads stuck_value */
    int stuck_value;
} sim_t;

static uint32_t rnd(sim_t *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->rng >> 8;
}

static int noise_of(sim_t *s)
{
    return s->noise == 0 ? 0 : (int)(rnd(s) % (unsigned)(2 * s->noise + 1)) - s->noise;
}

static km_depth_cal_t true_cal(const sim_t *s, unsigned slot)
{
    km_depth_cal_t c;
    c.released = (uint16_t)((int)s->rest0[slot] + s->drift);
    c.bottom = (uint16_t)((int)s->bot0[slot] + s->drift * s->beta_pct / 100);
    return c;
}

/* the reading at which the key is x (0.01 mm) deep: the largest v whose depth is >= x */
static uint16_t reading_at(const km_depth_cal_t *c, int x)
{
    if (x <= 0) {
        return c->released;
    }
    int lo = c->bottom, hi = c->released;
    if (km_depth_model_mm100(c, (uint16_t)lo) < (unsigned)x) {
        return c->bottom;
    }
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (km_depth_model_mm100(c, (uint16_t)mid) >= (unsigned)x) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return (uint16_t)lo;
}

static void make_frame(sim_t *s, uint16_t f[KM_ENGINE_SLOTS])
{
    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
        const km_depth_cal_t c = true_cal(s, i);
        int v = (int)reading_at(&c, s->x[i]) + noise_of(s);
        if ((int)i == s->stuck_slot) {
            v = s->stuck_value + noise_of(s);
        }
        f[i] = (uint16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
    }
}

static void frame(sim_t *s)
{
    uint16_t f[KM_ENGINE_SLOTS];
    make_frame(s, f);
    const unsigned before = s->e.n_down;
    km_engine_update(&s->e, f);
    if (s->e.n_down > before) {
        s->presses += s->e.n_down - before;
    }
    km_drift_pass(&s->d, &s->e, s->sec);
}

static void frames(sim_t *s, unsigned n)
{
    while (n--) {
        frame(s);
    }
}

/* nkeys keys evaluated (the others disabled), calibrated with 64 quiet frames; learn each key's bottom with one full press */
static void sim_boot(sim_t *s, uint32_t seed, unsigned nkeys, int noise, bool drift_on)
{
    memset(s, 0, sizeof *s);
    s->rng = seed * 2654435761u + 12345u;
    s->noise = noise;
    s->beta_pct = 100;
    s->stuck_slot = -1;
    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
        s->rest0[i] = (uint16_t)(3100u + (i * 37u) % 340u);
        s->bot0[i] = (uint16_t)(s->rest0[i] - (1000u + (i * 53u) % 300u));
    }
    km_engine_init(&s->e);
    km_drift_init(&s->d);
    unsigned n = 0;
    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
        if (km_keymap_slot_is_key(i)) {
            if (n < nkeys) {
                s->keys[n++] = i;
            } else {
                (void)km_engine_set_slot_enabled(&s->e, i, false);
            }
        }
    }
    s->nkeys = n;
    km_drift_set_enabled(&s->d, &s->e, drift_on);
}

static km_engine_cal_result_t sim_calibrate(sim_t *s)
{
    uint16_t f[KM_ENGINE_SLOTS];
    km_engine_calibrate_begin(&s->e);
    for (unsigned i = 0; i < 64; i++) {
        make_frame(s, f);
        km_engine_calibrate_idle(&s->e, f);
    }
    return km_engine_calibrate_finish(&s->e);
}

static void press_full(sim_t *s, unsigned slot)
{
    for (int x = 0; x <= 340; x += 10) {
        s->x[slot] = x;
        frames(s, 20);
    }
    frames(s, 300);
    s->x[slot] = 0;
    frames(s, 300);
}

static void learn_all(sim_t *s)
{
    for (unsigned i = 0; i < s->nkeys; i++) {
        press_full(s, s->keys[i]);
    }
}

/* run n simulated seconds of idle */
static void seconds(sim_t *s, unsigned n)
{
    while (n--) {
        s->sec++;
        frames(s, SIM_FPS);
    }
}

/* the depth (0.01 mm) at which a slow press on a copy of the engine makes the key go down; -1 = never */
static int probe(const sim_t *s, unsigned slot)
{
    km_engine_t c = s->e;
    const km_depth_cal_t cal = true_cal(s, slot);
    uint16_t f[KM_ENGINE_SLOTS];
    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
        const km_depth_cal_t ci = true_cal(s, i);
        f[i] = ci.released;
    }
    for (int x = 0; x <= 340; x++) {
        f[slot] = reading_at(&cal, x);
        for (unsigned n = 0; n < 16; n++) {
            km_engine_update(&c, f);
        }
        if (km_engine_slot_down(&c, slot)) {
            return x;
        }
    }
    return -1;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* worst deviation from the intended actuation depth over the evaluated keys */
static int worst_err(const sim_t *s)
{
    int w = 0;
    for (unsigned i = 0; i < s->nkeys; i++) {
        const int p = probe(s, s->keys[i]);
        const int e = p < 0 ? 999 : iabs(p - (int)WANT_MM100);
        if (e > w) {
            w = e;
        }
    }
    return w;
}

static void ready(sim_t *s, uint32_t seed, unsigned nkeys, int noise, bool drift_on)
{
    sim_boot(s, seed, nkeys, noise, drift_on);
    CHECK_EQ(sim_calibrate(s), KM_ENGINE_CAL_OK);
    learn_all(s);
    seconds(s, 60);
}

/* ---- scenarios ---- */

static void t_no_drift_no_change(void)
{
    static sim_t s;
    ready(&s, 1, 6, 15, true);
    CHECK(worst_err(&s) <= 3);
    seconds(&s, 3600);
    CHECK(s.d.total_updates <= 6);                      /* noise alone does not keep moving things */
    CHECK(worst_err(&s) <= 3);
    for (unsigned i = 0; i < s.nkeys; i++) {
        CHECK(iabs(s.d.k[s.keys[i]].off) <= 2);
        CHECK(s.d.k[s.keys[i]].armed);
    }
}

typedef struct {
    int final_drift;       /* counts after hours */
    unsigned hours;
    int beta;
    int noise;
    bool on;
} ramp_t;

/* linear ramp plus a slow wobble; returns the worst actuation error seen at the probes (every 20 minutes) */
static int run_ramp(sim_t *s, const ramp_t *r, int *max_off, int *max_step_per_5s)
{
    ready(s, 7, 6, r->noise, r->on);
    s->beta_pct = r->beta;
    int worst = 0;
    const unsigned total = r->hours * 3600u;
    int last_off[KM_ENGINE_SLOTS] = { 0 };
    *max_off = 0;
    *max_step_per_5s = 0;
    unsigned changes = 0;
    for (unsigned t = 0; t < total; t++) {
        s->drift = (int)((long long)r->final_drift * t / total) + (5 * (((t / 300u) % 2u) ? 1 : -1) * (int)(t % 300u) / 300);
        seconds(s, 1);
        for (unsigned i = 0; i < s->nkeys; i++) {
            const int o = s->d.k[s->keys[i]].off;
            if (iabs(o) > *max_off) {
                *max_off = iabs(o);
            }
            if (o != last_off[i]) {
                changes++;
                if (iabs(o - last_off[i]) > 1) {
                    *max_step_per_5s = 99;
                }
                last_off[i] = o;
            }
        }
        if (t % 1200u == 1199u) {
            const int w = worst_err(s);
            if (w > worst) {
                worst = w;
            }
        }
    }
    *max_step_per_5s = *max_step_per_5s == 99 ? 99 : (int)(changes <= s->nkeys * (total / KM_DRIFT_STEP_S + 1u) ? 1 : 98);
    return worst;
}

static void t_thermal(void)
{
    static sim_t off, on;
    int mo, ms;
    /* drift up 90 counts in 3 h (readings rise: the old bottom is too deep); with the module off the actuation point creeps */
    ramp_t r = { 90, 3, 100, 12, false };
    const int e_off = run_ramp(&off, &r, &mo, &ms);
    r.on = true;
    const int e_on = run_ramp(&on, &r, &mo, &ms);
    printf("    thermal +90/3h: actuation error OFF %d, ON %d (0.01 mm); max correction %d counts\n", e_off, e_on, mo);
    CHECK(e_off >= 6);                       /* the problem exists */
    CHECK(e_on <= 4);                        /* and the module removes it: within 0.04 mm of the intended 1.20 mm */
    CHECK(mo >= 80 && mo <= 92);
    CHECK_EQ(ms, 1);                         /* one count at a time, never faster than the rate limit */
    for (unsigned i = 0; i < on.nkeys; i++) {
        CHECK(on.d.k[on.keys[i]].updates >= 80 && on.d.k[on.keys[i]].updates < 400);
    }
    /* drift down 90 counts (the module on only: the module off is the mirror image of the first run) */
    r.final_drift = -90;
    r.hours = 2;
    const int e_on2 = run_ramp(&on, &r, &mo, &ms);
    printf("    thermal -90/2h: actuation error ON %d\n", e_on2);
    CHECK(e_on2 <= 4);
    CHECK(mo >= 80 && mo <= 92);
}

static void t_partial_beta(void)
{
    static sim_t on;
    int mo, ms;
    ramp_t r = { 80, 2, 80, 12, true };            /* the true bottom moves only 80 % as much: the model is 20 % off */
    const int e = run_ramp(&on, &r, &mo, &ms);
    printf("    beta 80%%: error ON %d\n", e);
    CHECK(e <= 6);
}

static void t_held_key(void)
{
    static sim_t s;
    ready(&s, 3, 6, 12, true);
    const unsigned held = s.keys[0];
    int base_raw = s.d.k[held].raw;
    /* the key is held at 1.5 mm for 2 hours while the sensor drifts 60 counts */
    s.x[held] = 150;
    seconds(&s, 20);
    const uint32_t upd0 = s.d.total_updates;
    for (unsigned t = 0; t < 2u * 3600u; t++) {
        s.drift = (int)(60ll * t / (2u * 3600u));  /* beyond the engine's own tracking window of 24 counts: the raise logic has to find the rest level again */
        seconds(&s, 1);
    }
    CHECK(km_engine_slot_down(&s.e, held));
    CHECK_EQ(s.d.total_updates, upd0);                  /* frozen for the whole time: nothing moved, nothing learned */
    CHECK_EQ(s.d.k[held].raw, base_raw);
    CHECK_EQ(s.d.k[held].off, 0);
    s.x[held] = 0;
    seconds(&s, 25 * 60);
    const int w = worst_err(&s);
    printf("    held 2 h, 25 min after release: error %d, correction %d\n", w, s.d.k[s.keys[1]].off);
    CHECK(w <= 4);
    CHECK(s.d.total_updates > upd0);
}

static void t_light_touch(void)
{
    /* a key held at 0.05 mm (up, but not at rest) for an hour: no correction, and the others are unaffected */
    static sim_t s;
    ready(&s, 4, 6, 12, true);
    const unsigned k = s.keys[2];
    s.x[k] = 20;
    seconds(&s, 3600);
    /* the engine's own tracker absorbs a light touch (<= 24 counts) into the baseline; the correction follows it, at 12 counts a minute at
     * most, and goes back when the key is let go. Never beyond what the baseline did. */
    CHECK(s.d.k[k].off >= -24 && s.d.k[k].off <= 0);
    CHECK(!km_engine_slot_down(&s.e, k));
    s.x[k] = 0;
    seconds(&s, 900);
    CHECK(iabs(s.d.k[k].off) <= 5);
    CHECK(worst_err(&s) <= 4);
}

static void t_stuck_key(void)
{
    static sim_t s;
    /* key 1 reads 1200 forever (a dead sensor): the engine disables it at calibration; it is never armed */
    sim_boot(&s, 5, 6, 12, true);
    s.stuck_slot = (int)s.keys[1];
    s.stuck_value = 1200;
    CHECK_EQ(sim_calibrate(&s), KM_ENGINE_CAL_OK);
    s.x[s.keys[0]] = 0;
    learn_all(&s);
    seconds(&s, 60);
    s.drift = 40;
    seconds(&s, 20 * 60);
    CHECK(!s.d.k[s.keys[1]].armed);
    CHECK_EQ(s.d.k[s.keys[1]].updates, 0);
    CHECK(s.d.k[s.keys[3]].off >= 30);                   /* the others follow */
    /* mid-run a key sticks half way down for good: the module stays frozen, never corrupts anything */
    static sim_t m;
    ready(&m, 6, 6, 12, true);
    m.x[m.keys[4]] = 200;
    seconds(&m, 30);
    const uint32_t u = m.d.total_updates;
    m.drift = 50;
    seconds(&m, 3600);
    CHECK_EQ(m.d.total_updates, u);
    for (unsigned i = 0; i < m.nkeys; i++) {
        CHECK(iabs(m.d.k[m.keys[i]].off) <= 1);
    }
    m.x[m.keys[4]] = 0;
    seconds(&m, 25 * 60);
    CHECK(worst_err(&m) <= 4);
}

static void t_typing(void)
{
    static sim_t s;
    ready(&s, 8, 6, 12, true);
    s.drift = 0;
    for (unsigned t = 0; t < 120; t++) {
        s.drift = (int)t / 4;
        seconds(&s, 1);
    }
    /* ten minutes of typing: 5 taps a second on random keys (0.1 s down each); nothing may be moved during the bursts */
    const uint32_t u0 = s.d.total_updates;
    unsigned taps = 0;
    for (unsigned t = 0; t < 600; t++) {
        s.sec++;
        for (unsigned tap = 0; tap < 5; tap++) {
            const unsigned k = s.keys[rnd(&s) % s.nkeys];
            s.x[k] = 220;
            frames(&s, SIM_FPS / 10u);
            s.x[k] = 0;
            frames(&s, SIM_FPS / 10u);
            taps++;
        }
    }
    CHECK(s.presses >= taps - 5);
    CHECK_EQ(s.d.total_updates, u0);
    CHECK(s.d.freezes >= 590);
    seconds(&s, 60);
    CHECK(s.d.total_updates > u0);               /* and it goes on afterwards */
}

static void t_glitches(void)
{
    static sim_t s;
    /* all 61 keys: a common-mode dip of 600 counts for 6 frames every 7 s, and single-slot spikes of +-900 for 2 frames */
    ready(&s, 9, KM_KEYMAP_KEYS, 12, true);
    const uint32_t g0 = km_engine_glitch_count(&s.e);
    s.drift = 0;
    unsigned spikes = 0;
    for (unsigned t = 0; t < 20u * 60u; t++) {
        s.drift = (int)(t / 40u);               /* +30 counts in 20 minutes */
        s.sec++;
        for (unsigned f = 0; f < 200; f++) {    /* a time-lapse of 200 frames a second is enough for 61 keys */
            const bool dip = (t % 7u) == 0u && f < 12u;
            if (dip) {
                for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                    s.x[i] = km_keymap_slot_is_key(i) ? 150 : 0;
                }
            } else if (f == 12u) {
                memset(s.x, 0, sizeof s.x);
            }
            frame(&s);
            if (f == 100u && (t % 3u) == 0u) {
                const unsigned k = s.keys[rnd(&s) % s.nkeys];
                uint16_t v[KM_ENGINE_SLOTS];
                make_frame(&s, v);
                v[k] = (uint16_t)(v[k] - 900);
                km_engine_update(&s.e, v);
                km_engine_update(&s.e, v);
                spikes++;
            }
        }
    }
    CHECK(km_engine_glitch_count(&s.e) > g0);
    CHECK(spikes > 100);
    unsigned armed = 0;
    for (unsigned i = 0; i < s.nkeys; i++) {
        const km_drift_key_t *k = &s.d.k[s.keys[i]];
        if (k->armed) {
            armed++;
            CHECK(iabs(k->off) <= 31);
        }
    }
    CHECK(armed > 40);
}

static void t_power_up_half_pressed(void)
{
    static sim_t s;
    sim_boot(&s, 10, 6, 12, true);
    const unsigned k = s.keys[2];
    s.x[k] = 200;                                  /* a key is pressed 2 mm while the keyboard is plugged in */
    CHECK_EQ(sim_calibrate(&s), KM_ENGINE_CAL_OK);
    CHECK_EQ(km_engine_slot_status(&s.e, k), KM_ENGINE_SLOT_ASSUMED);
    for (unsigned i = 0; i < s.nkeys; i++) {
        if (s.keys[i] != k) {
            press_full(&s, s.keys[i]);
        }
    }
    seconds(&s, 600);
    CHECK(!s.d.k[k].armed);                        /* not armed from a pressed reading */
    CHECK_EQ(s.d.k[k].updates, 0);
    s.drift = 30;
    s.x[k] = 0;                                    /* released after 10 minutes */
    seconds(&s, 15 * 60);
    CHECK_EQ(km_engine_slot_status(&s.e, k), KM_ENGINE_SLOT_OK);
    CHECK(s.d.k[k].armed);
    /* the learned rest is the true one (within noise), not the pressed reading */
    const int true_rest = (int)s.rest0[k] + s.drift;
    CHECK(iabs((int)km_engine_baseline(&s.e, k) - true_rest) <= 3);
    CHECK(iabs((int)s.d.k[k].ref_base - (int)s.rest0[k]) <= 45);
    press_full(&s, k);                             /* it has not learned its stroke yet: one full press teaches it */
    seconds(&s, 20 * 60);
    CHECK(probe(&s, k) >= 0 && iabs(probe(&s, k) - (int)WANT_MM100) <= 4);
}

static void t_recalibrate_and_toggle(void)
{
    static sim_t s;
    ready(&s, 11, 6, 12, true);
    for (unsigned t = 0; t < 4500; t++) {
        s.drift = (int)t / 50;                         /* +90 in 75 minutes */
        seconds(&s, 1);
    }
    CHECK(s.d.k[s.keys[0]].off >= 60);
    /* re-calibration the way m6_main does it: calibrated = false, passes keep running, then the new calibration */
    s.e.calibrated = false;
    frames(&s, 100);
    CHECK_EQ(sim_calibrate(&s), KM_ENGINE_CAL_OK);
    frames(&s, 10);
    seconds(&s, 2);
    for (unsigned i = 0; i < s.nkeys; i++) {
        CHECK(s.d.k[s.keys[i]].armed);
        CHECK_EQ(s.d.k[s.keys[i]].off, 0);            /* reset by the re-calibration */
    }
    learn_all(&s);
    seconds(&s, 120);
    CHECK(worst_err(&s) <= 4);
    /* switching off gives the learned bottoms back */
    for (unsigned t = 0; t < 900; t++) {
        s.drift = 90 + (int)t / 50;
        seconds(&s, 1);
    }
    const unsigned k = s.keys[1];
    CHECK(s.d.k[k].off != 0);
    const uint16_t raw = s.d.k[k].raw;
    km_drift_set_enabled(&s.d, &s.e, false);
    CHECK_EQ(km_engine_bottom(&s.e, k), raw);
    const uint32_t u = s.d.total_updates;
    seconds(&s, 600);
    CHECK_EQ(s.d.total_updates, u);                   /* off means off */
    km_drift_set_enabled(&s.d, &s.e, true);
    seconds(&s, 600);
    CHECK(s.d.total_updates > u || s.d.k[k].armed);
}

static void t_learner_interplay(void)
{
    static sim_t s;
    ready(&s, 12, 6, 0, true);
    for (unsigned t = 0; t < 3000; t++) {
        s.drift = (int)t / 50;                           /* +60 */
        seconds(&s, 1);
    }
    const unsigned k = s.keys[0];
    CHECK(s.d.k[k].off >= 55);
    /* a harder press: the true bottom is 25 counts deeper than before (a worn switch, or just pressing hard) */
    s.bot0[k] = (uint16_t)(s.bot0[k] - 25);
    press_full(&s, k);
    seconds(&s, 120);
    const uint16_t eb = km_engine_bottom(&s.e, k);
    CHECK(iabs((int)eb - ((int)s.bot0[k] + s.drift)) <= 16);    /* the engine learned it (a deeper bottom is learned only beyond a margin of 8 counts, and the model reads depth 340 a few counts above the bottom) and the module kept its offset on top */
    CHECK_EQ(eb, s.d.k[k].last);
    CHECK(iabs(((int)s.d.k[k].raw + s.d.k[k].off) - (int)eb) <= 1);
    CHECK(probe(&s, k) >= 0 && iabs(probe(&s, k) - (int)WANT_MM100) <= 4);
}

static void t_clamp(void)
{
    static sim_t s;
    ready(&s, 13, 4, 12, true);
    for (unsigned t = 0; t < 4u * 3600u; t++) {            /* +300 counts: twice the engine's own drift limit */
        s.drift = (int)(300ll * t / (4u * 3600u));
        seconds(&s, 1);
        if (t % 600u == 0u) {
            for (unsigned i = 0; i < s.nkeys; i++) {
                const unsigned k = s.keys[i];
                CHECK(iabs(s.d.k[k].off) <= KM_DRIFT_MAX_OFF);
                const int span = (int)km_engine_baseline(&s.e, k) - (int)km_engine_bottom(&s.e, k);
                CHECK(span >= (int)KM_ENGINE_MIN_SPAN && span <= (int)KM_ENGINE_MAX_SPAN);
            }
        }
    }
}

/* randomised: whatever happens, the invariants hold */
static void t_fuzz(void)
{
    static sim_t s;
    for (uint32_t seed = 1; seed <= 6; seed++) {
        ready(&s, 100 + seed, 6, 20, true);
        int last[NKEYS_MAX] = { 0 };
        for (unsigned t = 0; t < 40u * 60u; t++) {
            s.drift += (int)(rnd(&s) % 3u) - 1 + (t % 97u == 0u ? 1 : 0);
            if (s.drift > 200) {
                s.drift = 200;
            } else if (s.drift < -200) {
                s.drift = -200;
            }
            s.sec++;
            const unsigned burst = rnd(&s) % 5u == 0u;
            for (unsigned f = 0; f < 200; f++) {
                if (burst && f % 50u == 0u) {
                    const unsigned k = s.keys[rnd(&s) % s.nkeys];
                    s.x[k] = (int)(rnd(&s) % 340u);
                }
                if (f % 50u == 25u) {
                    memset(s.x, 0, sizeof s.x);
                }
                frame(&s);
            }
            for (unsigned i = 0; i < s.nkeys; i++) {
                const km_drift_key_t *k = &s.d.k[s.keys[i]];
                CHECK(iabs(k->off) <= KM_DRIFT_MAX_OFF);
                CHECK(iabs(k->off - last[i]) <= 1);
                last[i] = k->off;
            }
        }
        memset(s.x, 0, sizeof s.x);
    }
}

static void t_pack_and_misc(void)
{
    static sim_t s;
    ready(&s, 14, 6, 12, true);
    s.drift = 40;
    seconds(&s, 1200);
    uint16_t v[KM_DRIFT_SCOPE_VALUES];
    km_drift_pack(&s.d, &s.e, 1, v);
    CHECK_EQ(v[0] & 1u, 1u);
    CHECK_EQ(v[0] >> 8, s.nkeys);
    CHECK_EQ(v[1], s.keys[1]);
    CHECK_EQ((int16_t)v[2], s.d.k[s.keys[1]].off);
    CHECK_EQ(v[3], s.d.k[s.keys[1]].updates);
    CHECK(v[5] >= 37 && v[5] <= 40);
    CHECK_EQ(v[6], (uint16_t)s.d.total_updates);
    /* disabled: nothing happens at all, whatever the sensor does */
    static sim_t o;
    ready(&o, 14, 6, 12, false);
    o.drift = 100;
    seconds(&o, 1800);
    CHECK_EQ(o.d.total_updates, 0);
    km_drift_pack(&o.d, &o.e, 0, v);
    CHECK_EQ(v[0] & 1u, 0u);
    /* the drift_pass on an uncalibrated engine does nothing */
    km_engine_t e;
    km_drift_t d;
    km_engine_init(&e);
    km_drift_init(&d);
    km_drift_set_enabled(&d, &e, true);
    km_drift_pass(&d, &e, 5);
    km_drift_pass(&d, &e, 6);
    CHECK_EQ(d.total_updates, 0);
}

void test_drift(void)
{
    t_no_drift_no_change();
    t_thermal();
    t_partial_beta();
    t_held_key();
    t_light_touch();
    t_stuck_key();
    t_typing();
    t_glitches();
    t_power_up_half_pressed();
    t_recalibrate_and_toggle();
    t_learner_interplay();
    t_clamp();
    t_fuzz();
    t_pack_and_misc();
}
