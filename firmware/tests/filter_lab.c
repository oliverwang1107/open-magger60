/* filter_lab.c - see filter_lab.h. */
#include "filter_lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <km/km_depth_model.h>
#include <km/km_keymap.h>
#include <km/km_scan.h>

#include "filter_lab_engine.h"
#include "filter_lab_hooks.h"

#define LAB_PI 3.14159265358979323846

/* ---------------------------------------------------------------- small maths without libm */

double lab_abs(double x)
{
    return x < 0 ? -x : x;
}

double lab_sqrt(double x)
{
    if (x <= 0) {
        return 0;
    }
    double g = x > 1 ? x : 1;
    for (int i = 0; i < 60; i++) {
        const double n = 0.5 * (g + x / g);
        if (lab_abs(n - g) < 1e-15 * g) {
            return n;
        }
        g = n;
    }
    return g;
}

static double lab_exp_neg(double x)               /* e^-x for x >= 0, x not large */
{
    double term = 1, sum = 1;
    for (int i = 1; i < 40; i++) {
        term *= -x / i;
        sum += term;
    }
    return sum;
}

static void lab_sincos(double x, double *s, double *c)
{
    const double two_pi = 2 * LAB_PI;
    x -= two_pi * (double)(long)(x / two_pi);
    if (x > LAB_PI) {
        x -= two_pi;
    } else if (x < -LAB_PI) {
        x += two_pi;
    }
    double ts = x, tc = 1, ss = x, cc = 1;
    for (int i = 1; i < 24; i++) {
        tc *= -x * x / ((2 * i - 1) * (2 * i));
        ts *= -x * x / ((2 * i) * (2 * i + 1));
        cc += tc;
        ss += ts;
    }
    *s = ss;
    *c = cc;
}

void lab_rng_seed(lab_rng_t *r, uint64_t seed)
{
    uint64_t z = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;          /* one scramble, so that nearby seeds give unrelated streams */
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    r->s = z ^ (z >> 31);
}

static uint64_t rng_next(lab_rng_t *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double lab_rng_uniform(lab_rng_t *r)
{
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

double lab_rng_gauss(lab_rng_t *r)
{
    double u = 0;
    for (int i = 0; i < 3; i++) {
        const uint64_t x = rng_next(r);
        u += (double)((x & 0xFFFFu) + ((x >> 16) & 0xFFFFu) + ((x >> 32) & 0xFFFFu) + (x >> 48));
    }
    return u / 65536.0 - 6.0;
}

double lab_stat_sd(double n, double sum, double sumsq)
{
    if (n < 2) {
        return 0;
    }
    const double v = (sumsq - sum * sum / n) / (n - 1);
    return v > 0 ? lab_sqrt(v) : 0;
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

double lab_percentile(double *x, unsigned n, double p)
{
    if (n == 0) {
        return 0;
    }
    qsort(x, n, sizeof x[0], cmp_double);
    const double k = (double)(n - 1) * p / 100.0;
    const unsigned lo = (unsigned)k;
    const unsigned hi = lo + 1 < n ? lo + 1 : lo;
    return x[lo] + (x[hi] - x[lo]) * (k - (double)lo);
}

double lab_median(double *x, unsigned n)
{
    return lab_percentile(x, n, 50);
}

/* ---------------------------------------------------------------- candidates and their hooks */

/* The hooks. F0 reproduces the engine's own statements exactly. */
const lab_variant_t lab_variants[] = {
    /* name   stage1          stage2          pre              t1  t2  k */
    { "F0",  LAB_S1_BASE,  LAB_S2_BASE,  LAB_PRE_NONE,    12, 32, 0 },    /* the engine as it is */
    { "F1A", LAB_S1_BASE,  LAB_S2_ADAPT, LAB_PRE_NONE,    12, 32, 0 },    /* adaptive gain in the rapid-trigger slow stage */
    { "F1B", LAB_S1_ADAPT, LAB_S2_PASS,  LAB_PRE_NONE,    12, 32, 0 },    /* one adaptive stage instead of the two */
    { "F1C", LAB_S1_ADAPT, LAB_S2_ADAPT, LAB_PRE_NONE,    12, 32, 0 },    /* adaptive gain in both stages */
    { "F4M", LAB_S1_BASE,  LAB_S2_BASE,  LAB_PRE_MEDIAN3, 12, 32, 0 },    /* 3-point median on every raw sample */
    { "F4C", LAB_S1_BASE,  LAB_S2_BASE,  LAB_PRE_CLAMP,   12, 32, 45 },   /* raw samples limited to +-45 counts (3 sd) around their mean */
    { "F5",  LAB_S1_BASE,  LAB_S2_BASE,  LAB_PRE_CM,      12, 32, 0 },    /* common-mode subtraction before the engine */
};
const unsigned lab_n_variants = sizeof lab_variants / sizeof lab_variants[0];

static const lab_variant_t *g_var = &lab_variants[0];
uint16_t lab_rt_floor = KM_ENGINE_RT_MIN_MM100;

const lab_variant_t *lab_variant_by_name(const char *name)
{
    for (unsigned i = 0; i < lab_n_variants; i++) {
        if (strcmp(lab_variants[i].name, name) == 0) {
            return &lab_variants[i];
        }
    }
    return NULL;
}

void lab_set_variant(const lab_variant_t *v)
{
    g_var = v != NULL ? v : &lab_variants[0];
}

static unsigned adaptive_shift(int32_t diff_q4, const lab_variant_t *v)
{
    const int32_t ad = diff_q4 < 0 ? -diff_q4 : diff_q4;
    return ad < (v->t1 << 4) ? 3u : ad < (v->t2 << 4) ? 2u : 1u;
}

int32_t lab_hook_stage1(int32_t f, int32_t x)
{
    if (g_var->stage1 == LAB_S1_ADAPT) {
        return f + ((x - f) >> adaptive_shift(x - f, g_var));
    }
    return f + ((x - f) >> 2);
}

int32_t lab_hook_stage2(int32_t g, int32_t f)
{
    if (g_var->stage2 == LAB_S2_PASS) {
        return f;
    }
    if (g_var->stage2 == LAB_S2_ADAPT) {
        const unsigned sh = adaptive_shift(f - g, g_var);
        return g + (((f - g) + (1 << (sh - 1u))) >> sh);
    }
    return g + (((f - g) + (1 << 2)) >> 3);
}

/* ---------------------------------------------------------------- noise profiles */

/* Every profile is scaled so that the main filter f of the engine, at rest, has a standard deviation of about 3.3 counts, the
 * operating point of ENGINE_NOTES.md ("the engine's main filter leaves sigma 3.3 counts", from white +-15 counts at the engine
 * input, sd 8.7). Only the COLOUR differs. The sixteen-sample mean of the sliding sum is itself a low-pass, so the same f sd needs a
 * much smaller white raw sample than the documented 8.7 at the input of the engine (about 15 counts, not 35), and a slow component
 * passes the mean unchanged. How white the real sensor is, and how large, is what P12a measures; `scale` multiplies all components. */
const lab_noise_t lab_profiles[] = {
    /* name        white lf  lf_tau tone tone_hz tone2 tone2_hz spike_hz spike_amp cm cm_tau uniform_pp */
    { "white",      14.8, 0,  1,     0,   0,      0,    0,       0,       0,        0,  1,    0 },
    { "mixed",      13.6, 1.4, 400,  0,   0,      0,    0,       0,       0,        0,  1,    0 },
    { "lowfreq",    4.2,  3.2, 2000, 0,   0,      0,    0,       0,       0,        0,  1,    0 },
    { "legacy",     0,    0,  1,     0,   0,      0,    0,       0,       0,        0,  1,    30 },
    { "commonmode", 13.6, 1.4, 400,  0,   0,      0,    0,       0,       0,        2.5, 300, 0 },
    { "spiky",      13.6, 1.4, 400,  0,   0,      0,    0,       40,      80,       0,  1,    0 },
    { "spikeonly",  0,    0,  1,     0,   0,      0,    0,       100,     80,       0,  1,    0 },   /* diagnostic: isolated spikes and nothing else */
    { "tonal",      13.6, 1.4, 400,  3,   1000,   2,    50,      0,       0,        0,  1,    0 },
    { "real",       3.5,  0.4, 400,  0,   0,      0,    0,       0,       0,        0,  1,    0 },   /* MEASURED (km_noise, scenario A, 61 keys): raw sd 3.16 / 3.51 / 4.18, sd of the 16-point sum 0.96 */
};
const unsigned lab_n_profiles = sizeof lab_profiles / sizeof lab_profiles[0];

const lab_noise_t *lab_profile_by_name(const char *name)
{
    for (unsigned i = 0; i < lab_n_profiles; i++) {
        if (strcmp(lab_profiles[i].name, name) == 0) {
            return &lab_profiles[i];
        }
    }
    return NULL;
}

typedef struct {
    lab_rng_t rng;
    double lf, a, b;
    double c1, s1, c2, s2, cw1, sw1, cw2, sw2;
    int spike_left;
    double spike_val;
} noise_state_t;

static void noise_init(noise_state_t *n, const lab_noise_t *p, uint64_t seed)
{
    memset(n, 0, sizeof *n);
    lab_rng_seed(&n->rng, seed);
    n->a = lab_exp_neg(1.0 / (p->lf_tau > 1 ? p->lf_tau : 1));
    n->b = lab_sqrt(1 - n->a * n->a);
    n->lf = lab_rng_gauss(&n->rng) * p->lf;
    lab_sincos(2 * LAB_PI * lab_rng_uniform(&n->rng), &n->s1, &n->c1);
    lab_sincos(2 * LAB_PI * lab_rng_uniform(&n->rng), &n->s2, &n->c2);
    lab_sincos(2 * LAB_PI * p->tone_hz / LAB_FS, &n->sw1, &n->cw1);
    lab_sincos(2 * LAB_PI * p->tone2_hz / LAB_FS, &n->sw2, &n->cw2);
}

/* one frame of noise in counts */
static double noise_next(noise_state_t *n, const lab_noise_t *p, double scale)
{
    double v = p->white * lab_rng_gauss(&n->rng);
    if (p->lf > 0) {
        n->lf = n->a * n->lf + n->b * p->lf * lab_rng_gauss(&n->rng);
        v += n->lf;
    }
    if (p->tone_amp > 0) {
        const double c = n->c1 * n->cw1 - n->s1 * n->sw1;
        n->s1 = n->s1 * n->cw1 + n->c1 * n->sw1;
        n->c1 = c;
        v += p->tone_amp * n->s1;
    }
    if (p->tone2_amp > 0) {
        const double c = n->c2 * n->cw2 - n->s2 * n->sw2;
        n->s2 = n->s2 * n->cw2 + n->c2 * n->sw2;
        n->c2 = c;
        v += p->tone2_amp * n->s2;
    }
    if (p->spike_hz > 0) {
        if (n->spike_left > 0) {
            v += n->spike_val;
            n->spike_left--;
        } else if (lab_rng_uniform(&n->rng) < p->spike_hz / LAB_FS) {
            n->spike_val = (lab_rng_uniform(&n->rng) < 0.5 ? -1.0 : 1.0) * p->spike_amp * (0.6 + 0.6 * lab_rng_uniform(&n->rng));
            n->spike_left = 0;                          /* one frame long (a 3-point median removes exactly these) */
            v += n->spike_val;
        }
    }
    return v * scale;
}

/* ---------------------------------------------------------------- sensor truth */

void lab_sensor_init(lab_sensor_t *s, unsigned released, unsigned span)
{
    s->released = released;
    s->bottom = released - span;
    const km_depth_cal_t cal = { (uint16_t)released, (uint16_t)s->bottom };
    for (unsigned d = 0; d <= KM_TRAVEL_MM100; d++) {
        unsigned lo = s->bottom, hi = released;          /* largest reading whose depth is >= d */
        while (lo < hi) {
            const unsigned mid = (lo + hi + 1u) / 2u;
            if (km_depth_model_mm100(&cal, (uint16_t)mid) >= d) {
                lo = mid;
            } else {
                hi = mid - 1u;
            }
        }
        s->counts_at[d] = (double)lo;
    }
    s->counts_at[0] = (double)released;
    s->counts_at[KM_TRAVEL_MM100] = (double)s->bottom;
    s->counts_at[KM_TRAVEL_MM100 + 1u] = (double)s->bottom;
}

double lab_sensor_counts(const lab_sensor_t *s, double depth)
{
    if (depth <= 0) {
        return s->counts_at[0];
    }
    if (depth >= KM_TRAVEL_MM100) {
        return s->counts_at[KM_TRAVEL_MM100];
    }
    const unsigned i = (unsigned)depth;
    const double fr = depth - (double)i;
    return s->counts_at[i] + (s->counts_at[i + 1u] - s->counts_at[i]) * fr;
}

/* ---------------------------------------------------------------- paths */

int lab_path_init(lab_path_t *p, unsigned long cap)
{
    p->d = (float *)malloc(cap * sizeof(float));
    p->n = 0;
    p->cap = p->d != NULL ? cap : 0;
    return p->d != NULL ? 0 : -1;
}

void lab_path_free(lab_path_t *p)
{
    free(p->d);
    p->d = NULL;
    p->n = p->cap = 0;
}

static double path_end(const lab_path_t *p)
{
    return p->n > 0 ? (double)p->d[p->n - 1] : 0.0;
}

static void path_push(lab_path_t *p, double v)
{
    if (p->n < p->cap) {
        p->d[p->n++] = (float)v;
    }
}

void lab_path_hold(lab_path_t *p, double seconds)
{
    const double v = path_end(p);
    const unsigned long n = (unsigned long)(seconds * LAB_FS);
    for (unsigned long i = 0; i < n; i++) {
        path_push(p, v);
    }
}

void lab_path_ramp(lab_path_t *p, double to, double speed_mm_s)
{
    const double from = path_end(p);
    const double per_frame = speed_mm_s * 100.0 / LAB_FS;          /* mm100 per frame */
    const double dist = lab_abs(to - from);
    const unsigned long n = (unsigned long)(dist / per_frame);
    for (unsigned long i = 1; i <= n; i++) {
        path_push(p, from + (to > from ? 1.0 : -1.0) * per_frame * (double)i);
    }
    path_push(p, to);
}

void lab_path_step(lab_path_t *p, double to)
{
    path_push(p, to);
}

void lab_path_wiggle(lab_path_t *p, double seconds, double amp, double hz)
{
    const double c0 = path_end(p);
    const unsigned long n = (unsigned long)(seconds * LAB_FS);
    double s = 0, c = 1, sw, cw;
    lab_sincos(2 * LAB_PI * hz / LAB_FS, &sw, &cw);
    for (unsigned long i = 0; i < n; i++) {
        path_push(p, c0 + amp * s);
        const double nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    path_push(p, c0);
}

/* ---------------------------------------------------------------- real traces */

int lab_trace_load(lab_trace_t *t, const char *path)
{
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    uint8_t h[20];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "KMTR", 4) != 0 || (h[4] | (h[5] << 8)) != 1) {
        fclose(f);
        return -2;
    }
    const unsigned cols = (unsigned)(h[6] | (h[7] << 8));
    const unsigned long n = (unsigned long)h[8] | ((unsigned long)h[9] << 8) | ((unsigned long)h[10] << 16) | ((unsigned long)h[11] << 24);
    if (cols == 0 || cols > sizeof t->slots || n == 0 || n > 50000000ul) {
        fclose(f);
        return -3;
    }
    memcpy(&t->fs, &h[12], 4);
    uint8_t slots[16 + 4];
    const size_t pad = (4u - cols % 4u) % 4u;
    if (fread(slots, 1, cols + pad, f) != cols + pad) {
        fclose(f);
        return -4;
    }
    memcpy(t->slots, slots, cols);
    t->n_cols = cols;
    t->n_frames = n;
    uint8_t *raw = (uint8_t *)malloc(n * cols * 2u);
    t->v = (uint16_t *)malloc(n * cols * sizeof(uint16_t));
    if (raw == NULL || t->v == NULL || fread(raw, 2u * cols, n, f) != n) {
        free(raw);
        free(t->v);
        t->v = NULL;
        fclose(f);
        return -5;
    }
    for (unsigned long i = 0; i < n * cols; i++) {
        t->v[i] = (uint16_t)((raw[2 * i] | (raw[2 * i + 1] << 8)) & 0x0FFF);
    }
    free(raw);
    fclose(f);
    return 0;
}

void lab_trace_free(lab_trace_t *t)
{
    free(t->v);
    memset(t, 0, sizeof *t);
}

/* ---------------------------------------------------------------- engine dispatch (real or lab copy) */

typedef struct { int real; km_engine_t e; } eng_t;

static void E_init(eng_t *x) { if (x->real) km_engine_init(&x->e); else lab_engine_init(&x->e); }
static void E_cal_begin(eng_t *x) { if (x->real) km_engine_calibrate_begin(&x->e); else lab_engine_calibrate_begin(&x->e); }
static void E_cal_idle(eng_t *x, const uint16_t *c) { if (x->real) km_engine_calibrate_idle(&x->e, c); else lab_engine_calibrate_idle(&x->e, c); }
static km_engine_cal_result_t E_cal_finish(eng_t *x) { return x->real ? km_engine_calibrate_finish(&x->e) : lab_engine_calibrate_finish(&x->e); }
static void E_update(eng_t *x, const uint16_t *c) { if (x->real) km_engine_update(&x->e, c); else lab_engine_update(&x->e, c); }
static int E_down(const eng_t *x, unsigned s) { return x->real ? km_engine_slot_down(&x->e, s) : lab_engine_slot_down(&x->e, s); }
static int E_apply(eng_t *x, unsigned s, const km_engine_key_cfg_t *c) { return x->real ? km_engine_apply_key_cfg(&x->e, s, c) : lab_engine_apply_key_cfg(&x->e, s, c); }
static int E_valid(const eng_t *x, const km_engine_key_cfg_t *c) { return x->real ? km_engine_cfg_valid(c) : lab_engine_cfg_valid(c); }
static int E_set_bottom(eng_t *x, unsigned s, uint16_t b) { return x->real ? km_engine_set_bottom(&x->e, s, b) : lab_engine_set_bottom(&x->e, s, b); }
static int E_enable(eng_t *x, unsigned s, int on) { return x->real ? km_engine_set_slot_enabled(&x->e, s, on != 0) : lab_engine_set_slot_enabled(&x->e, s, on != 0); }
static uint16_t E_depth(const eng_t *x, unsigned s) { return x->real ? km_engine_depth(&x->e, s) : lab_engine_depth(&x->e, s); }
static uint16_t E_base(const eng_t *x, unsigned s) { return x->real ? km_engine_baseline(&x->e, s) : lab_engine_baseline(&x->e, s); }
static uint16_t E_bottom(const eng_t *x, unsigned s) { return x->real ? km_engine_bottom(&x->e, s) : lab_engine_bottom(&x->e, s); }
static uint32_t E_glitches(const eng_t *x) { return x->real ? km_engine_glitch_count(&x->e) : lab_engine_glitch_count(&x->e); }

/* ---------------------------------------------------------------- the run */

const char *const lab_analog_name[LAB_NUM_A] = { "engine depth", "IIR 1/2 + hysteresis", "IIR 1/4 + hysteresis", "IIR 1/4" };

/* depth in mm100 with sub-count interpolation between two neighbouring integer readings */
static double depth_interp(const km_depth_cal_t *cal, double counts)
{
    if (counts < 0) {
        counts = 0;
    }
    const unsigned c0 = (unsigned)counts;
    const double fr = counts - (double)c0;
    const double d0 = km_depth_model_mm100(cal, (uint16_t)c0);
    const double d1 = km_depth_model_mm100(cal, (uint16_t)(c0 + 1u));
    return d0 + (d1 - d0) * fr;
}

static int ticks_round(double mm100)
{
    const double t = mm100 * 255.0 / KM_TRAVEL_MM100;
    return (int)(t + 0.5);
}

typedef struct {
    double s16, s32;                 /* analog filter states, counts */
    int out[LAB_NUM_A];
    int valid;
} analog_t;

static void ev_push(lab_result_t *res, unsigned long frame, unsigned key, int type)
{
    if (res->n_ev == res->cap_ev) {
        const unsigned nc = res->cap_ev == 0 ? 256u : res->cap_ev * 2u;
        lab_event_t *n = (lab_event_t *)realloc(res->ev, nc * sizeof *n);
        if (n == NULL) {
            return;
        }
        res->ev = n;
        res->cap_ev = nc;
    }
    res->ev[res->n_ev].frame = frame;
    res->ev[res->n_ev].key = key;
    res->ev[res->n_ev].type = type;
    res->n_ev++;
}

void lab_result_free(lab_result_t *r)
{
    free(r->ev);
    memset(r, 0, sizeof *r);
}

#define WARM_FRAMES 40u
#define CAL_FRAMES  64u

typedef struct {
    int32_t prev1[KM_ENGINE_SLOTS], prev2[KM_ENGINE_SLOTS];     /* median of 3 */
    double mean[KM_ENGINE_SLOTS];                                 /* clamp: running mean of the limited samples */
    double base[KM_ENGINE_SLOTS];                                 /* common mode: slow baseline of every slot */
    int init[KM_ENGINE_SLOTS];
} pre_t;

static int32_t median3(int32_t a, int32_t b, int32_t c)
{
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

int lab_run(const lab_run_t *r, lab_result_t *out)
{
    memset(out, 0, sizeof *out);
    const lab_variant_t *var = r->variant != NULL ? r->variant : &lab_variants[0];
    const lab_noise_t *np = r->noise;
    const double scale = r->noise_scale > 0 ? r->noise_scale : 1.0;
    const unsigned settle = r->settle_frames != 0 ? r->settle_frames : 400u;
    const int legacy = np != NULL && np->uniform_pp > 0;
    const int use_noise = np != NULL && r->trace == NULL;

    eng_t *x = (eng_t *)calloc(1, sizeof *x);
    km_scan_t *scan = (km_scan_t *)calloc(1, sizeof *scan);
    pre_t *pre = (pre_t *)calloc(1, sizeof *pre);
    noise_state_t *ns = (noise_state_t *)calloc(KM_ENGINE_SLOTS, sizeof *ns);
    analog_t an[LAB_MAX_KEYS];
    lab_sensor_t sens[LAB_MAX_KEYS];
    if (x == NULL || scan == NULL || pre == NULL || ns == NULL) {
        free(x); free(scan); free(pre); free(ns);
        return -1;
    }
    memset(an, 0, sizeof an);
    x->real = r->use_real_engine != 0;
    lab_set_variant(var);
    lab_rt_floor = r->rt_floor != 0 ? r->rt_floor : (uint16_t)KM_ENGINE_RT_MIN_MM100;

    int key_of[KM_ENGINE_SLOTS];
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        key_of[s] = -1;
    }
    for (unsigned k = 0; k < r->n_keys; k++) {
        key_of[r->slot[k]] = (int)k;
        lab_sensor_init(&sens[k], LAB_REST, r->span[k] != 0 ? r->span[k] : LAB_SPAN);
    }
    /* the reference slots: the first n_ref other key slots carry noise too (common mode needs company) */
    int ref_of[KM_ENGINE_SLOTS];
    unsigned n_ref = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        ref_of[s] = 0;
        if (key_of[s] < 0 && km_keymap_slot_is_key(s) && n_ref < r->n_ref) {
            ref_of[s] = 1;
            n_ref++;
        }
    }
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (np != NULL) {
            noise_init(&ns[s], np, r->seed * 1000003ull + s * 7919ull + 1ull);
        }
    }
    noise_state_t cmn;
    if (np != NULL) {
        noise_init(&cmn, np, r->seed * 31ull + 977ull);
        /* the common mode has its own time constant */
        cmn.a = lab_exp_neg(1.0 / (np->cm_tau > 1 ? np->cm_tau : 1));
        cmn.b = lab_sqrt(1 - cmn.a * cmn.a);
        cmn.lf = 0;
    }

    km_scan_init(scan);
    E_init(x);
    if (x->real) {
        (void)km_engine_set_filter_mode(&x->e, r->real_filter_mode);     /* the real engine's own adaptive filters, if compiled in */
    }
    uint16_t counts[KM_ENGINE_SLOTS];
    int rc = 0;
    lab_result_t *res = out;
    unsigned long pushed = 0;
    const unsigned long total = (unsigned long)WARM_FRAMES + CAL_FRAMES + settle + r->n_frames;
    int phase_cfg_done = 0;
    int prev_down[LAB_MAX_KEYS] = { 0 };
    double cm_state = 0;

    for (unsigned long fr = 0; fr < total; fr++) {
        const long scene = (long)fr - (long)(WARM_FRAMES + CAL_FRAMES + settle);   /* < 0 before the scene starts */
        uint16_t raw[KM_ENGINE_SLOTS];
        double clean[KM_ENGINE_SLOTS];

        double cm = 0;
        if (use_noise && np->cm > 0) {
            cm_state = cmn.a * cm_state + cmn.b * np->cm * lab_rng_gauss(&cmn.rng);
            cm = cm_state * scale;
        }
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            double c = LAB_REST;
            const int k = key_of[s];
            if (k >= 0) {
                if (r->trace != NULL) {
                    const unsigned col = (unsigned)k < r->trace->n_cols ? (unsigned)k : 0u;
                    c = r->trace->v[(fr % r->trace->n_frames) * r->trace->n_cols + col];
                } else if (scene >= 0 && r->path[k] != NULL) {
                    const lab_path_t *p = r->path[k];
                    c = lab_sensor_counts(&sens[k], p->n == 0 ? 0.0 : (double)p->d[(unsigned long)scene < p->n ? (unsigned long)scene : p->n - 1u]);
                } else {
                    c = (double)sens[k].released;
                }
            }
            clean[s] = c;
            double v = c;
            if (use_noise && !legacy && (k >= 0 || ref_of[s])) {
                v += noise_next(&ns[s], np, scale) + cm;
            }
            int iv = (int)(v + (v >= 0 ? 0.5 : -0.5));
            if (iv < 0) iv = 0;
            if (iv > 4095) iv = 4095;
            raw[s] = (uint16_t)iv;
        }

        /* candidate pre-filters on the raw samples */
        if (var->pre == LAB_PRE_MEDIAN3 || var->pre == LAB_PRE_CLAMP) {
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (key_of[s] < 0 && !ref_of[s]) {
                    continue;
                }
                int32_t v = raw[s];
                if (var->pre == LAB_PRE_MEDIAN3) {
                    const int32_t m = pre->init[s] ? median3(pre->prev2[s], pre->prev1[s], v) : v;
                    pre->prev2[s] = pre->prev1[s];
                    pre->prev1[s] = v;
                    pre->init[s] = 1;
                    v = m;
                } else {
                    if (!pre->init[s]) {
                        pre->mean[s] = v;
                        pre->init[s] = 1;
                    }
                    const double m = pre->mean[s];
                    if (v > m + var->k) v = (int32_t)(m + var->k);
                    if (v < m - var->k) v = (int32_t)(m - var->k);
                    pre->mean[s] = m + ((double)v - m) / 16.0;
                }
                raw[s] = (uint16_t)v;
            }
        }

        if (legacy) {
            /* the model of ENGINE_NOTES.md: noise at the engine input, no raw samples and no sliding sum */
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                double v = clean[s];
                if (use_noise && (key_of[s] >= 0 || ref_of[s])) {
                    v += (lab_rng_uniform(&ns[s].rng) - 0.5) * np->uniform_pp * scale;
                }
                int iv = (int)(v + (v >= 0 ? 0.5 : -0.5));
                counts[s] = (uint16_t)(iv < 0 ? 0 : iv > 4095 ? 4095 : iv);
            }
        } else {
            uint32_t words[KM_SCAN_WORDS];
            memset(words, 0, sizeof words);
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                words[s >> 1] |= (uint32_t)raw[s] << (16u * (s & 1u));
            }
            for (unsigned p = 0; p < KM_SCAN_PHASES; p++) {
                km_scan_push_phase(scan, p, &words[p * KM_SCAN_PHASE_WORDS]);
            }
            km_scan_push_injected(scan, words[36], words[37]);
            pushed++;
            const uint16_t *sums = km_scan_sums(scan);
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                counts[s] = (uint16_t)(sums[s] >> 4);
            }
        }

        if (pushed <= WARM_FRAMES && !legacy) {
            continue;                                    /* the sums are full after 16 frames */
        }

        if (var->pre == LAB_PRE_CM) {
            /* common mode: the mean deviation of the slots that look at rest from their own slow baseline, taken off all of them */
            double sum = 0;
            unsigned n = 0;
            double dev[KM_ENGINE_SLOTS];
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (key_of[s] < 0 && !ref_of[s]) {
                    continue;
                }
                if (!pre->init[s]) {
                    pre->base[s] = counts[s];
                    pre->init[s] = 1;
                }
                dev[s] = (double)counts[s] - pre->base[s];
                if (lab_abs(dev[s]) < 24.0) {
                    sum += dev[s];
                    n++;
                }
            }
            const double cmv = n > 0 ? sum / (double)n : 0.0;
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (key_of[s] < 0 && !ref_of[s]) {
                    continue;
                }
                if (lab_abs(dev[s]) < 24.0) {
                    pre->base[s] += (dev[s]) / 4096.0;          /* the baseline follows the raw value slowly, noise and all */
                }
                const double cv = (double)counts[s] - cmv;
                counts[s] = (uint16_t)(cv < 0 ? 0 : cv > 4095 ? 4095 : (int)(cv + 0.5));
            }
        }

        if (!x->e.calibrated) {
            const unsigned long cal_i = pushed - (legacy ? 0u : WARM_FRAMES);
            const unsigned long done = legacy ? fr : cal_i - 1u;
            if (done == 0) {
                E_cal_begin(x);
            }
            E_cal_idle(x, counts);
            if (done + 1u >= CAL_FRAMES) {
                if (E_cal_finish(x) != KM_ENGINE_CAL_OK) {
                    rc = -2;
                    break;
                }
            }
            continue;
        }

        if (!phase_cfg_done) {
            phase_cfg_done = 1;
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (km_keymap_slot_is_key(s) && key_of[s] < 0) {
                    (void)E_enable(x, s, 0);
                }
            }
            for (unsigned k = 0; k < r->n_keys; k++) {
                km_engine_key_cfg_t c = r->cfg[k];
                if (!E_valid(x, &c) || !E_apply(x, r->slot[k], &c)) {
                    rc = -3;
                    break;
                }
                (void)E_set_bottom(x, r->slot[k], sens[k].bottom);
            }
            if (rc != 0) {
                break;
            }
        }

        E_update(x, counts);

        if (scene < 0) {
            continue;
        }
        const unsigned long sf = (unsigned long)scene;
        for (unsigned k = 0; k < r->n_keys; k++) {
            const unsigned slot = r->slot[k];
            const int dn = E_down(x, slot);
            if (dn != prev_down[k]) {
                ev_push(res, sf, k, dn ? LAB_EV_PRESS : LAB_EV_RELEASE);
                prev_down[k] = dn;
            }
            const km_engine_slot_t *sl = &x->e.slot[slot];
            const double fc = sl->filt_q4 / 16.0;
            /* analog outputs */
            const km_depth_cal_t cal = { E_base(x, slot), E_bottom(x, slot) };
            analog_t *a = &an[k];
            lab_key_stats_t *st = &res->key[k];
            if (sf >= r->stat_from) {                    /* the filters: every frame */
                st->n += 1;
                st->sum_f += fc;
                st->sumsq_f += fc * fc;
                const double gc = sl->g_q4 / 16.0;
                st->sum_g += gc;
                st->sumsq_g += gc * gc;
            }
            /* the analog paths run at 1 kHz, like the gamepad report (one evaluation every 23 frames); between two the output holds */
            if (a->valid && (sf % 23u) != 0u) {
                if (k == 0) {
                    for (int i = 0; i < LAB_NUM_A; i++) {
                        if (r->series[i] != NULL && sf < r->n_frames) {
                            r->series[i][sf] = (float)a->out[i];
                        }
                    }
                }
                continue;
            }
            if (!a->valid) {
                a->s16 = a->s32 = fc;
            } else {
                a->s16 += (fc - a->s16) / 2.0;
                a->s32 += (fc - a->s32) / 4.0;
            }
            const double t0 = (double)E_depth(x, slot) * 255.0 / KM_TRAVEL_MM100;
            const double t1 = depth_interp(&cal, a->s16) * 255.0 / KM_TRAVEL_MM100;
            const double t2 = depth_interp(&cal, a->s32) * 255.0 / KM_TRAVEL_MM100;
            int o[LAB_NUM_A];
            o[LAB_A0] = ticks_round(t0 * KM_TRAVEL_MM100 / 255.0);
            o[LAB_A3] = (int)(t2 + 0.5);
            o[LAB_A1] = a->valid ? a->out[LAB_A1] : (int)(t1 + 0.5);
            o[LAB_A2] = a->valid ? a->out[LAB_A2] : (int)(t2 + 0.5);
            if (lab_abs(t1 - o[LAB_A1]) > 0.75) o[LAB_A1] = (int)(t1 + 0.5);
            if (lab_abs(t2 - o[LAB_A2]) > 0.75) o[LAB_A2] = (int)(t2 + 0.5);
            a->valid = 1;
            if (sf >= r->stat_from) {
                for (int i = 0; i < LAB_NUM_A; i++) {
                    if (st->a_n[i] == 0) {
                        st->a_min[i] = st->a_max[i] = o[i];
                    } else if (o[i] != a->out[i]) {
                        st->a_jumps[i]++;
                    }
                    st->a_n[i] += 1;
                    st->a_sum[i] += o[i];
                    st->a_sumsq[i] += (double)o[i] * o[i];
                    if (o[i] < st->a_min[i]) st->a_min[i] = o[i];
                    if (o[i] > st->a_max[i]) st->a_max[i] = o[i];
                }
            }
            for (int i = 0; i < LAB_NUM_A; i++) {
                a->out[i] = o[i];
            }
            if (k == 0) {
                for (int i = 0; i < LAB_NUM_A; i++) {
                    if (r->series[i] != NULL && sf < r->n_frames) {
                        r->series[i][sf] = (float)o[i];
                    }
                }
            }
        }
    }
    res->glitches = E_glitches(x);
    res->calibrated = x->e.calibrated;
    free(x); free(scan); free(pre); free(ns);
    lab_set_variant(NULL);
    lab_rt_floor = KM_ENGINE_RT_MIN_MM100;
    return rc;
}

/* ---------------------------------------------------------------- ideal machines and matching */

unsigned lab_ideal_rapid(const lab_path_t *p, unsigned rt_down, unsigned rt_up, lab_event_t *out, unsigned cap)
{
    unsigned n = 0;
    int down = 0;
    double ext = 0;
    for (unsigned long f = 0; f < p->n; f++) {
        const double d = p->d[f];
        if (!down) {
            if (d < 5.0) {
                ext = 0;                    /* at rest (depth < KM_ENGINE_RT_REST_MM100): the top of the travel */
            } else if (d < ext) {
                ext = d;
            }
            if (d > LAB_TD && d >= ext + rt_down) {
                down = 1;
                ext = d;
                if (n < cap) { out[n].frame = f; out[n].key = 0; out[n].type = LAB_EV_PRESS; }
                n++;
            }
        } else {
            if (d > ext) {
                ext = d;
            }
            if (ext - d >= rt_up || d <= (double)LAB_TD - 10.0) {
                down = 0;
                ext = d;
                if (n < cap) { out[n].frame = f; out[n].key = 0; out[n].type = LAB_EV_RELEASE; }
                n++;
            }
        }
    }
    return n < cap ? n : cap;
}

unsigned lab_ideal_fixed(const lab_path_t *p, unsigned press, unsigned release, lab_event_t *out, unsigned cap)
{
    unsigned n = 0;
    int down = 0;
    for (unsigned long f = 0; f < p->n; f++) {
        const double d = p->d[f];
        if (!down && d >= press) {
            down = 1;
            if (n < cap) { out[n].frame = f; out[n].key = 0; out[n].type = LAB_EV_PRESS; }
            n++;
        } else if (down && d <= release) {
            down = 0;
            if (n < cap) { out[n].frame = f; out[n].key = 0; out[n].type = LAB_EV_RELEASE; }
            n++;
        }
    }
    return n < cap ? n : cap;
}

void lab_match(const lab_event_t *ideal, unsigned n_ideal, const lab_event_t *real, unsigned n_real, unsigned key,
               unsigned early, unsigned late, lab_match_t *m)
{
    memset(m, 0, sizeof *m);
    unsigned char *used = (unsigned char *)calloc(n_real != 0 ? n_real : 1u, 1);
    if (used == NULL) {
        return;
    }
    for (unsigned i = 0; i < n_ideal; i++) {
        int found = 0;
        for (unsigned j = 0; j < n_real; j++) {
            if (used[j] || real[j].key != key || real[j].type != ideal[i].type) {
                continue;
            }
            if (real[j].frame + early < ideal[i].frame) {
                continue;
            }
            if (real[j].frame <= ideal[i].frame + late) {
                used[j] = 1;
                found = 1;
                const int t = ideal[i].type;
                if (m->n_lat[t] < 4096u) {
                    m->lat_ms[t][m->n_lat[t]++] = ((double)real[j].frame - (double)ideal[i].frame) * 1000.0 / LAB_FS;
                }
            }
            break;
        }
        if (found) {
            m->matched++;
        } else {
            m->missed++;
        }
    }
    for (unsigned j = 0; j < n_real; j++) {
        if (real[j].key == key && !used[j]) {
            m->extra++;
        }
    }
    free(used);
}
