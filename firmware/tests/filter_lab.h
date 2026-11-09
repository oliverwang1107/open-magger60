/*
 * filter_lab.h - host-side evaluation of candidate sensing filters on the REAL key engine logic (M6 task P12b, core/FILTER_NOTES.md).
 *
 * A run feeds the engine frame by frame exactly like the keyboard does: raw 12-bit samples per slot -> km_scan sliding sums (the real
 * km_scan.c) -> sums / 16 -> engine. The samples come from a synthetic scene (clean sensor reading from a depth path, plus a noise
 * model) or from a real capture file (KMTR, tools/magger60_noise.py). The engine is either the real core/src/km_engine.c or its
 * generated lab copy (tests/filter_lab_engine.c) whose two filter statements call the hooks of the selected candidate.
 *
 * Everything here is deterministic (fixed seeds), needs no libm (the unit tests link without it) and allocates only through
 * the caller's calls to lab_path_* / lab_trace_load.
 */
#ifndef FILTER_LAB_H
#define FILTER_LAB_H

#include <stddef.h>
#include <stdint.h>

#include <km/km_depth_model.h>
#include <km/km_engine.h>

#define LAB_MAX_KEYS   8u
#define LAB_FS         23300.0       /* frames per second (frame about 43 us) */
#define LAB_REST       3300u         /* released reading of every synthetic key */
#define LAB_SPAN       1000u         /* released - bottom, the steepest (worst) of the measured keys: 998..1365 */
#define LAB_TD         20u           /* the rapid-trigger top dead zone the ideal state machine and the configs use (mm100) */

/* ---------------------------------------------------------------- candidates */

enum { LAB_S1_BASE = 0, LAB_S1_ADAPT = 1 };                      /* main filter: gain 1/4, or adaptive gain */
enum { LAB_S2_BASE = 0, LAB_S2_ADAPT = 1, LAB_S2_PASS = 2 };     /* rapid-trigger slow stage: gain 1/8, adaptive, or none (g = f) */
enum { LAB_PRE_NONE = 0, LAB_PRE_MEDIAN3 = 1, LAB_PRE_CLAMP = 2, LAB_PRE_CM = 3 };   /* before the sliding sum / before the engine */

typedef struct {
    const char *name;
    uint8_t stage1, stage2, pre;
    int t1, t2;          /* adaptive gain: |difference| < t1 counts -> 1/8, < t2 -> 1/4, else 1/2 */
    int k;               /* PRE_CLAMP: samples are limited to +-k counts around their running mean */
} lab_variant_t;

extern const lab_variant_t lab_variants[];
extern const unsigned lab_n_variants;
const lab_variant_t *lab_variant_by_name(const char *name);       /* "F0", "F1A", "F1B", "F4M", "F4C", "F5"; NULL if unknown */
/* The variant the lab engine's hooks use. lab_run() sets it for the duration of a run; tests may set it directly. */
void lab_set_variant(const lab_variant_t *v);

/* ---------------------------------------------------------------- noise */

typedef struct {
    const char *name;
    double white;                 /* white noise, standard deviation in counts, on every raw sample */
    double lf, lf_tau;            /* slow component: AR(1), standard deviation and time constant in frames */
    double tone_amp, tone_hz;     /* sine, peak amplitude in counts */
    double tone2_amp, tone2_hz;
    double spike_hz, spike_amp;   /* isolated one-frame spikes of +-spike_amp (x0.6..1.2), per second and key */
    double cm, cm_tau;            /* common-mode component shared by all slots: AR(1) standard deviation and time constant */
    double uniform_pp;            /* > 0: legacy model of ENGINE_NOTES.md: uniform +-uniform_pp/2 added at the ENGINE INPUT (no raw samples) */
} lab_noise_t;

extern const lab_noise_t lab_profiles[];
extern const unsigned lab_n_profiles;
const lab_noise_t *lab_profile_by_name(const char *name);

typedef struct { uint64_t s; } lab_rng_t;
void lab_rng_seed(lab_rng_t *r, uint64_t seed);
double lab_rng_uniform(lab_rng_t *r);       /* [0, 1) */
double lab_rng_gauss(lab_rng_t *r);         /* mean 0, sd 1 (sum of 12 uniforms: no tail beyond 6 sd) */
double lab_sqrt(double x);
double lab_abs(double x);

/* ---------------------------------------------------------------- sensor truth */

/* reading as a function of true depth (inverse of km_depth_model, both ends exact) */
typedef struct { unsigned released, bottom; double counts_at[KM_TRAVEL_MM100 + 2u]; } lab_sensor_t;
void lab_sensor_init(lab_sensor_t *s, unsigned released, unsigned span);
double lab_sensor_counts(const lab_sensor_t *s, double depth_mm100);

/* ---------------------------------------------------------------- depth paths */

typedef struct { float *d; unsigned long n, cap; } lab_path_t;          /* true depth in mm100, one value per frame */
int lab_path_init(lab_path_t *p, unsigned long cap);
void lab_path_free(lab_path_t *p);
void lab_path_hold(lab_path_t *p, double seconds);                      /* stay where the path ends */
void lab_path_ramp(lab_path_t *p, double to_mm100, double speed_mm_s);  /* straight to `to` at the speed */
void lab_path_step(lab_path_t *p, double to_mm100);                     /* jump */
void lab_path_wiggle(lab_path_t *p, double seconds, double amp_mm100, double hz);   /* sinusoid around the end point (starts and ends there) */

/* ---------------------------------------------------------------- real traces (KMTR, tools/magger60_noise.py) */

typedef struct { unsigned n_cols; uint8_t slots[16]; unsigned long n_frames; float fs; uint16_t *v; } lab_trace_t;   /* v[frame * n_cols + col], 12-bit */
int lab_trace_load(lab_trace_t *t, const char *path);                   /* 0 = ok */
void lab_trace_free(lab_trace_t *t);

/* ---------------------------------------------------------------- the run */

typedef enum { LAB_EV_RELEASE = 0, LAB_EV_PRESS = 1 } lab_ev_type_t;
typedef struct { unsigned long frame; unsigned key; int type; } lab_event_t;

enum { LAB_A0 = 0, LAB_A1, LAB_A2, LAB_A3, LAB_NUM_A };
extern const char *const lab_analog_name[LAB_NUM_A];   /* analog path candidates, evaluated at 1 kHz */

typedef struct {
    unsigned n_keys;
    unsigned slot[LAB_MAX_KEYS];                       /* engine slots of the evaluated keys (km_keymap_slot_is_key) */
    km_engine_key_cfg_t cfg[LAB_MAX_KEYS];
    unsigned span[LAB_MAX_KEYS];                       /* released - bottom of each key (0 = LAB_SPAN) */
    const lab_variant_t *variant;                      /* NULL = F0 */
    const lab_noise_t *noise;                          /* NULL = no noise */
    double noise_scale;                                /* multiplies every noise component (0 = 1) */
    uint64_t seed;
    unsigned n_ref;                                    /* extra idle key slots that carry noise (for the common-mode candidate) */
    int use_real_engine;                               /* 1 = core/src/km_engine.c, 0 = the lab copy */
    unsigned real_filter_mode;                         /* real engine only: KM_ENGINE_FMODE_* (needs KM_ENGINE_FILTER_MODES = 1 to differ from 0) */
    uint16_t rt_floor;                                 /* lab engine only: smallest accepted rt distance (0 = 20, the real value) */
    unsigned settle_frames;                            /* idle frames after the configs are set, before frame 0 of the scene (0 = 400) */
    unsigned long n_frames;                            /* length of the scene */
    unsigned long stat_from;                           /* standstill statistics are taken from this frame on */
    const lab_path_t *path[LAB_MAX_KEYS];              /* true depth of each key; NULL = at rest */
    const lab_trace_t *trace;                          /* real data: column c is key c, raw samples (wraps around); path/noise are ignored */
    float *series[LAB_NUM_A];                          /* optional, n_frames entries: analog output of key 0, in 8-bit ticks */
} lab_run_t;

typedef struct {
    double n, sum_f, sumsq_f, sum_g, sumsq_g;          /* main filter and rapid-trigger stage, counts, over the standstill window */
    double a_n[LAB_NUM_A], a_sum[LAB_NUM_A], a_sumsq[LAB_NUM_A];   /* analog outputs, 8-bit ticks */
    unsigned long a_jumps[LAB_NUM_A];                  /* frames in which the output tick changed */
    int a_min[LAB_NUM_A], a_max[LAB_NUM_A];
} lab_key_stats_t;

typedef struct {
    lab_event_t *ev;                                   /* press / release events in frame order (frame counted from the scene start) */
    unsigned n_ev, cap_ev;
    lab_key_stats_t key[LAB_MAX_KEYS];
    unsigned long glitches;
    int calibrated;
} lab_result_t;

/* Run a scene. 0 = ok, negative = setup failed (calibration refused, config invalid, out of memory). Free with lab_result_free. */
int lab_run(const lab_run_t *r, lab_result_t *out);
void lab_result_free(lab_result_t *r);

double lab_stat_sd(double n, double sum, double sumsq);

/* ---------------------------------------------------------------- the reference rapid-trigger machine and event matching */

/* What a perfect detector with these distances would report on the true path (no filter, no noise): continuous rapid trigger
 * (press at rt_down below the shallowest point, release at rt_up above the deepest point or in the top dead zone). Events of key 0. */
unsigned lab_ideal_rapid(const lab_path_t *p, unsigned rt_down, unsigned rt_up, lab_event_t *out, unsigned cap);
/* The same for a FIXED key: press at `press`, release at `release` (mm100). */
unsigned lab_ideal_fixed(const lab_path_t *p, unsigned press, unsigned release, lab_event_t *out, unsigned cap);

typedef struct {
    unsigned matched, missed, extra;                   /* ideal events found / not found; real events with no ideal partner */
    double lat_ms[2][4096];                            /* signed delay real - ideal in ms, [type][i]; not more than 4096 kept */
    unsigned n_lat[2];
} lab_match_t;
/* Pair every ideal event with the first not yet used real event of the same type of key `key` within [-early, +late] frames. */
void lab_match(const lab_event_t *ideal, unsigned n_ideal, const lab_event_t *real, unsigned n_real, unsigned key,
               unsigned early, unsigned late, lab_match_t *m);
double lab_median(double *x, unsigned n);              /* sorts x */
double lab_percentile(double *x, unsigned n, double p); /* sorts x; p in 0..100 */

#endif
