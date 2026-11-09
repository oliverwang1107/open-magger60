/*
 * filter_sim.c - host-side noise / latency simulation of candidate depth filters for rapid trigger (FILTER_NOTES.md).
 *
 * A compact model of the key path, much faster than the engine lab (tests/filter_lab.c): raw 12-bit sample at the scan rate -> [pre filter]
 * -> sliding mean of N samples (km_scan: 16) -> filter stage(s) -> a rapid-trigger detector with the engine's rules (extremes, actuation
 * point, debounce 8 / 6 frames, top dead zone). One count = 0.01 mm: that is the STEEPEST part of the travel (near the top, span 1000 counts,
 * depth model a = 2); deeper down a count is worth less, so the tables are the worst case.
 * Floating point, no rounding of the engine's Q4 (the engine lab shows the same ranking with the real integer engine for F0 / F1B / F1C).
 *
 * Build and run through tools/filter_sim.py (it prints the tables); or by hand:
 *   gcc -O2 -o filter_sim firmware/tools/filter_sim.c -lm && ./filter_sim table [noise=mixed] [scale=1] [seconds=120]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_US 39.0          /* measured sensing frame, 9.36k cycles at 240 MHz */
#define MAXN 64

/* ---------------------------------------------------------------- random numbers */
typedef struct { uint64_t s; } rng_t;
static double rnd_u(rng_t *r)
{
    r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17;
    return (double)(r->s >> 11) / 9007199254740992.0;
}
static double rnd_g(rng_t *r)
{
    double a = -6.0;
    for (int i = 0; i < 12; i++) a += rnd_u(r);
    return a;
}

/* ---------------------------------------------------------------- noise model (same colours as tests/filter_lab.c profiles) */
typedef struct { const char *name; double white, lf, lf_tau, spike_hz, spike_amp; } noise_t;
static const noise_t noises[] = {
    { "white",  14.8, 0.0, 1.0,   0.0, 0.0 },
    { "mixed",  13.6, 1.4, 436.0, 0.0, 0.0 },       /* 17 ms time constant */
    { "lowfreq", 4.2, 3.2, 2200.0, 0.0, 0.0 },      /* 86 ms */
    { "spiky",  13.6, 1.4, 436.0, 40.0, 72.0 },     /* + isolated one-frame spikes */
    { "real",    3.5, 0.4, 436.0,  0.0,  0.0 },     /* MEASURED (km_noise scenario A, 61 keys): raw sd 3.16 / 3.51 / 4.18 (min / median / max), sd of the 16-point sum 0.96, p-p 20-28 */
};
typedef struct { const noise_t *n; double scale, lf; rng_t r; } nsrc_t;
static double noise_next(nsrc_t *s)
{
    const noise_t *n = s->n;
    double v = n->white * rnd_g(&s->r);
    if (n->lf > 0.0) {
        const double a = exp(-1.0 / n->lf_tau);
        s->lf = a * s->lf + sqrt(1.0 - a * a) * n->lf * rnd_g(&s->r);
        v += s->lf;
    }
    if (n->spike_hz > 0.0 && rnd_u(&s->r) < n->spike_hz * FRAME_US * 1e-6) {
        v += (rnd_u(&s->r) < 0.5 ? -1.0 : 1.0) * n->spike_amp * (0.6 + 0.6 * rnd_u(&s->r));
    }
    return v * s->scale;
}

/* ---------------------------------------------------------------- filter chain */
enum { S1_FIXED, S1_ADAPT };
enum { S2_FIXED, S2_ADAPT, S2_NONE };
typedef struct {
    const char *name;
    int ma;               /* sliding mean length (samples) */
    int median3;          /* 3-point median on the raw samples */
    int s1, s2;           /* stage types */
    int s1_shift;         /* fixed gain 1/2^k of stage 1 (engine: 2) */
    double deadband;      /* output holds still until it has moved more than this (counts); 0 = off */
    int fast_confirm;     /* debounce shortened to 3 frames when the stroke is >= 2 x the rapid-trigger distance */
    int press_frames, release_frames;
    double t1, t2;        /* adaptive gain thresholds (counts): |diff| < t1 -> 1/8, < t2 -> 1/4, else 1/2 */
} cand_t;

static const cand_t cands[] = {
    /* name                        ma med s1        s2        sh db  fc  pf rf */
    { "F0 now (MA16+1/4+1/8)",     16, 0, S1_FIXED, S2_FIXED, 2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "MA8",                        8, 0, S1_FIXED, S2_FIXED, 2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "MA4",                        4, 0, S1_FIXED, S2_FIXED, 2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "F1B adaptive, one stage",   16, 0, S1_ADAPT, S2_NONE,  2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "F1C adaptive, two stages",  16, 0, S1_ADAPT, S2_ADAPT, 2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "median3 + F0",              16, 1, S1_FIXED, S2_FIXED, 2, 0.0, 0, 8, 6, 12.0, 32.0 },
    { "F0 + dead-band 3 counts",   16, 0, S1_FIXED, S2_FIXED, 2, 3.0, 0, 8, 6, 12.0, 32.0 },
    { "F1C + dead-band 2 counts",  16, 0, S1_ADAPT, S2_ADAPT, 2, 2.0, 0, 8, 6, 12.0, 32.0 },
    { "F1C + fast confirm",        16, 0, S1_ADAPT, S2_ADAPT, 2, 0.0, 1, 8, 6, 12.0, 32.0 },
    { "F1B + fast confirm",        16, 0, S1_ADAPT, S2_NONE,  2, 0.0, 1, 8, 6, 12.0, 32.0 },
    { "F0 + debounce 4/3",         16, 0, S1_FIXED, S2_FIXED, 2, 0.0, 0, 4, 3, 12.0, 32.0 },
};
#define NCAND ((int)(sizeof cands / sizeof cands[0]))

typedef struct {
    const cand_t *c;
    double raw[MAXN]; int ri;                 /* raw ring for median3 */
    double ring[MAXN]; double sum; int idx, filled;
    double f, g, out;
    int started;
} chain_t;

static void chain_init(chain_t *x, const cand_t *c, double level)
{
    memset(x, 0, sizeof *x);
    x->c = c;
    for (int i = 0; i < MAXN; i++) { x->raw[i] = level; x->ring[i] = level; }
    x->sum = level * c->ma;
    x->f = x->g = x->out = level;
}

static double med3(double a, double b, double c)
{
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

static double ashift(double d, const cand_t *c)
{
    const double ad = fabs(d);
    return ad < c->t1 ? 0.125 : ad < c->t2 ? 0.25 : 0.5;
}

/* one raw sample in -> the depth proxy out (in counts: bigger = deeper, so the caller passes level - sample) */
static double chain_step(chain_t *x, double raw)
{
    const cand_t *c = x->c;
    double v = raw;
    if (c->median3) {
        x->raw[x->ri & 3] = raw;
        v = med3(x->raw[x->ri & 3], x->raw[(x->ri + 3) & 3], x->raw[(x->ri + 2) & 3]);
        x->ri++;
    }
    x->sum += v - x->ring[x->idx];
    x->ring[x->idx] = v;
    x->idx = (x->idx + 1) % c->ma;
    const double m = x->sum / c->ma;
    const double k1 = ldexp(1.0, -c->s1_shift);
    x->f += (m - x->f) * (c->s1 == S1_ADAPT ? ashift(m - x->f, c) : k1);
    if (c->s2 == S2_NONE) x->g = x->f;
    else x->g += (x->f - x->g) * (c->s2 == S2_ADAPT ? ashift(x->f - x->g, c) : 0.125);
    if (c->deadband > 0.0) {
        if (fabs(x->g - x->out) > c->deadband) x->out = x->g;
    } else {
        x->out = x->g;
    }
    return x->out;
}

/* ---------------------------------------------------------------- rapid trigger detector (the engine's rules, in counts of depth) */
typedef struct { int down; double lo, hi; int cnt; } det_t;
#define TOP_DEAD 20.0
#define TOP_REL  10.0

/* returns +1 press, -1 release, 0 nothing. d = depth proxy (counts, 0.01 mm), rt in counts, act = actuation depth */
static int det_step(det_t *t, double d, double rt, double act, const cand_t *c)
{
    if (!t->down) {
        if (d < t->lo || d < TOP_DEAD) t->lo = d;
        const double stroke = d - t->lo;
        const int ok = d >= act && stroke >= rt;
        const int need = (c->fast_confirm && stroke >= 2.0 * rt && c->press_frames > 3) ? 3 : c->press_frames;
        if (ok) {
            if (++t->cnt >= need) { t->down = 1; t->hi = d; t->cnt = 0; return 1; }
        } else t->cnt = 0;
    } else {
        if (d > t->hi) t->hi = d;
        const int ok = (t->hi - d >= rt) || d < TOP_REL;
        const int need = (c->fast_confirm && (t->hi - d >= 2.0 * rt) && c->release_frames > 3) ? 3 : c->release_frames;
        if (ok) {
            if (++t->cnt >= need) { t->down = 0; t->lo = d; t->cnt = 0; return -1; }
        } else t->cnt = 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- experiments */
static int cmp_d(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int n, double p)
{
    if (n == 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    return v[(int)((n - 1) * p / 100.0)];
}

typedef struct { const noise_t *noise; double scale; double seconds; uint64_t seed; } cfg_t;

static void nsrc_init(nsrc_t *s, const cfg_t *g, uint64_t seed)
{
    s->n = g->noise; s->scale = g->scale; s->lf = 0; s->r.s = 0x9E3779B97F4A7C15ull ^ (seed * 0xD1B54A32D192ED03ull) ^ g->seed;
    for (int i = 0; i < 10; i++) (void)rnd_u(&s->r);
}

/* a key held still at `depth` counts below rest: standard deviation and peak-to-peak of the filter output (counts), and false events */
static void still(const cand_t *c, const cfg_t *g, double depth, double *sd, double *pp, long fe[3], const double rt_set[3])
{
    const long n = (long)(g->seconds * 1e6 / FRAME_US);
    const double level = 3300.0;
    nsrc_t ns; chain_t ch;
    nsrc_init(&ns, g, 1);
    chain_init(&ch, c, 0.0);
    double s1 = 0, s2 = 0, mn = 1e9, mx = -1e9;
    long m = 0;
    det_t det[3];
    for (int i = 0; i < 3; i++) { fe[i] = 0; det[i].down = 1; det[i].lo = 1e9; det[i].hi = depth; det[i].cnt = 0; }
    for (long i = 0; i < n; i++) {
        const double d = chain_step(&ch, depth + noise_next(&ns));
        (void)level;
        if (i < 2000) continue;
        s1 += d; s2 += d * d; m++;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        for (int k = 0; k < 3; k++) if (det_step(&det[k], d, rt_set[k], 70.0, c) != 0) fe[k]++;
    }
    const double mean = s1 / (double)m;
    *sd = sqrt(s2 / (double)m - mean * mean);
    *pp = mx - mn;
}

/* strokes 0 -> 150 counts (1.5 mm) -> 0 at speed v mm/s; delay (frames) from the ideal moment to the event */
static void strokes(const cand_t *c, const cfg_t *g, double v_mm_s, double rt, int n_strokes, double press_ms[2], double rel_ms[2], int *miss)
{
    const double step = v_mm_s * 100.0 * FRAME_US * 1e-6;      /* counts per frame */
    double *dp = (double *)malloc(sizeof(double) * (size_t)n_strokes * 2);
    double *dr = dp + n_strokes;
    int np = 0, nr = 0;
    *miss = 0;
    nsrc_t ns; chain_t ch; det_t det;
    nsrc_init(&ns, g, 7);
    chain_init(&ch, c, 0.0);
    det.down = 0; det.lo = 1e9; det.hi = 0; det.cnt = 0;
    long fr = 0;
    const double act = 70.0, bottom = 150.0;
    for (int s = 0; s < n_strokes; s++) {
        double tru = 0.0;
        for (int i = 0; i < 600; i++) { chain_step(&ch, noise_next(&ns)); (void)det_step(&det, ch.out, rt, act, c); fr++; }   /* rest, settle */
        det.down = 0; det.lo = 1e9; det.cnt = 0;
        long ideal = -1, ev = -1;
        for (int guard = 0; guard < 20000 && tru < bottom; guard++) {      /* press stroke */
            tru += step; if (tru > bottom) tru = bottom;
            if (ideal < 0 && tru >= act) ideal = fr;
            const double d = chain_step(&ch, tru + noise_next(&ns));
            if (det_step(&det, d, rt, act, c) == 1 && ev < 0) ev = fr;
            fr++;
        }
        if (ev >= 0 && ideal >= 0) dp[np++] = (double)(ev - ideal); else (*miss)++;
        for (int i = 0; i < 1500; i++) {                                  /* hold at the bottom (1.5 mm: well inside the zone) */
            const double d = chain_step(&ch, tru + noise_next(&ns));
            (void)det_step(&det, d, rt, act, c); fr++;
        }
        if (!det.down) { det.down = 1; det.hi = tru; det.cnt = 0; }
        ideal = -1; ev = -1;
        for (int guard = 0; guard < 20000 && tru > 0.0; guard++) {         /* release stroke */
            tru -= step; if (tru < 0) tru = 0;
            if (ideal < 0 && tru <= bottom - rt) ideal = fr;
            const double d = chain_step(&ch, tru + noise_next(&ns));
            if (det_step(&det, d, rt, act, c) == -1 && ev < 0) ev = fr;
            fr++;
        }
        if (ev >= 0 && ideal >= 0) dr[nr++] = (double)(ev - ideal); else (*miss)++;
    }
    /* frames -> milliseconds; may be negative (a stroke far past the trigger distance is released at the next debounce) */
    press_ms[0] = pct(dp, np, 50) * FRAME_US / 1000.0; press_ms[1] = pct(dp, np, 95) * FRAME_US / 1000.0;
    rel_ms[0] = pct(dr, nr, 50) * FRAME_US / 1000.0;   rel_ms[1] = pct(dr, nr, 95) * FRAME_US / 1000.0;
    free(dp);
}

/* steady-state lag of the filter on a ramp without noise, in frames: how far behind the true position the output runs */
static double ramp_lag_frames(const cand_t *c, double v_mm_s)
{
    const double step = v_mm_s * 100.0 * FRAME_US * 1e-6;
    chain_t ch; chain_init(&ch, c, 0.0);
    double tru = 0, out = 0;
    for (int i = 0; i < 800; i++) { tru += step; out = chain_step(&ch, tru); }
    return (tru - out) / step;
}

/* frames until the output has covered 90 % of a step of `size` counts (no noise) */
static int step90(const cand_t *c, double size)
{
    chain_t ch; chain_init(&ch, c, 0.0);
    for (int i = 1; i < 400; i++) if (chain_step(&ch, size) >= 0.9 * size) return i;
    return 400;
}


/* smallest rapid-trigger distance (counts) with zero false events at it AND at every larger distance up to 60, key held at `depth` */
static int min_safe_rt(const cand_t *c, const cfg_t *g, double depth)
{
    enum { NRT = 60 };
    const long n = (long)(g->seconds * 1e6 / FRAME_US);
    nsrc_t ns; chain_t ch; det_t det[NRT]; long fe[NRT];
    nsrc_init(&ns, g, 3);
    chain_init(&ch, c, 0.0);
    for (int i = 0; i < NRT; i++) { fe[i] = 0; det[i].down = 1; det[i].lo = 1e9; det[i].hi = depth; det[i].cnt = 0; }
    for (long i = 0; i < n; i++) {
        const double d = chain_step(&ch, depth + noise_next(&ns));
        if (i < 2000) continue;
        for (int k = 0; k < NRT; k++) if (det_step(&det[k], d, (double)(k + 1), 70.0, c) != 0) fe[k]++;
    }
    int safe = NRT + 1;
    for (int k = NRT - 1; k >= 0; k--) { if (fe[k] != 0) break; safe = k + 1; }
    return safe;
}

static void median_delay(const cand_t *c, const cfg_t *g, double speed, double rt, double *press, double *rel)
{
    double p[2], r[2]; int miss;
    strokes(c, g, speed, rt, 120, p, r, &miss);
    *press = miss ? 99.0 : p[0];
    *rel = miss ? 99.0 : r[0];
}

static void minrt_table(const cfg_t *g0)
{
    printf("| candidate | smallest safe rt x1 noise (counts = 0.01 mm) | x2 | x3 | press / release delay ms @300 mm/s, rt 0.25 |\n|---|---|---|---|---|\n");
    for (int i = 0; i < NCAND; i++) {
        int rt[3];
        for (int m = 0; m < 3; m++) { cfg_t g = *g0; g.scale = g0->scale * (m + 1); rt[m] = min_safe_rt(&cands[i], &g, 150.0); }
        double p, r; median_delay(&cands[i], g0, 300.0, 25.0, &p, &r);
        printf("| %s | %d | %d | %d | %.2f / %.2f |\n", cands[i].name, rt[0], rt[1], rt[2], p, r);
    }
}

/* grid search for a low-latency chain that stays safe at rt 25 with 3x the noise */
static void search(const cfg_t *g0)
{
    static cand_t sc[1];
    static char nm[96];
    const int mas[] = { 16, 8, 4 };
    const int s1k[] = { 2, 1 };
    const double ad[][2] = { { 12, 32 }, { 3, 8 }, { 2, 5 } };
    const int debs[][2] = { { 8, 6 }, { 4, 3 }, { 3, 3 }, { 2, 2 } };
    printf("| chain | rt safe x1 / x3 (counts) | press / release ms @300 mm/s (rt 25) | @100 mm/s |\n|---|---|---|---|\n");
    for (int a = 0; a < 3; a++) for (int s = 0; s < 5; s++) for (int d = 0; d < 4; d++) for (int fc = 0; fc < 2; fc++) {
        cand_t *c = &sc[0];
        memset(c, 0, sizeof *c);
        c->ma = mas[a]; c->s2 = S2_NONE; c->press_frames = debs[d][0]; c->release_frames = debs[d][1]; c->fast_confirm = fc; c->s1_shift = 2;
        if (s < 2) { c->s1 = S1_FIXED; c->s1_shift = s1k[s]; } else { c->s1 = S1_ADAPT; c->t1 = ad[s - 2][0]; c->t2 = ad[s - 2][1]; }
        if (fc && debs[d][0] <= 3) continue;
        snprintf(nm, sizeof nm, "MA%d %s deb %d/%d%s", c->ma, s < 2 ? (s ? "1/2" : "1/4") : (s == 2 ? "adapt12/32" : s == 3 ? "adapt3/8" : "adapt2/5"), c->press_frames, c->release_frames, fc ? " +fc" : "");
        c->name = nm;
        cfg_t g1 = *g0, g3 = *g0; g3.scale = g0->scale * 3.0;
        const int r1 = min_safe_rt(c, &g1, 150.0), r3 = min_safe_rt(c, &g3, 150.0);
        if (r3 > 25) continue;                              /* must be safe at rt 0.25 mm with three times the noise */
        double p, r, p1, r1d; median_delay(c, g0, 300.0, 25.0, &p, &r); median_delay(c, g0, 100.0, 25.0, &p1, &r1d);
        printf("| %s | %d / %d | %.2f / %.2f | %.2f / %.2f |\n", nm, r1, r3, p, r, p1, r1d);
    }
}

static const noise_t *noise_by_name(const char *n)
{
    for (unsigned i = 0; i < sizeof noises / sizeof noises[0]; i++) if (strcmp(noises[i].name, n) == 0) return &noises[i];
    return NULL;
}

int main(int argc, char **argv)
{
    cfg_t g = { &noises[1], 1.0, 120.0, 1 };
    const char *mode = argc > 1 ? argv[1] : "table";
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "noise=", 6)) { g.noise = noise_by_name(argv[i] + 6); if (!g.noise) { fprintf(stderr, "unknown noise\n"); return 2; } }
        else if (!strncmp(argv[i], "scale=", 6)) g.scale = atof(argv[i] + 6);
        else if (!strncmp(argv[i], "seconds=", 8)) g.seconds = atof(argv[i] + 8);
        else if (!strncmp(argv[i], "seed=", 5)) g.seed = (uint64_t)atoll(argv[i] + 5);
    }
    printf("noise=%s scale=%.2f seconds=%.0f frame=%.0f us\n\n", g.noise->name, g.scale, g.seconds, FRAME_US);
    if (strcmp(mode, "table") == 0) {
        const double rts[3] = { 20.0, 25.0, 30.0 };
        printf("| candidate | sd out (counts) | p-p (counts) | false events rt 0.20 / 0.25 / 0.30 mm | step 90 %% (frames / us) | ramp lag @0.3 m/s (frames / us) |\n");
        printf("|---|---|---|---|---|---|\n");
        for (int i = 0; i < NCAND; i++) {
            double sd, pp; long fe[3];
            still(&cands[i], &g, 150.0, &sd, &pp, fe, rts);
            const int s90 = step90(&cands[i], 100.0);
            const double lag = ramp_lag_frames(&cands[i], 300.0);
            printf("| %s | %.2f | %.1f | %ld / %ld / %ld | %d / %.0f | %.1f / %.0f |\n", cands[i].name, sd, pp, fe[0], fe[1], fe[2], s90, s90 * FRAME_US, lag, lag * FRAME_US);
        }
    } else if (strcmp(mode, "latency") == 0) {
        const double rt = 25.0;
        const double speeds[3] = { 100.0, 300.0, 500.0 };
        printf("rapid trigger %.2f mm, actuation 0.70 mm, stroke 0 -> 1.5 mm -> 0. Delay from the ideal moment (ms, median / p95; includes the debounce)\n\n", rt / 100.0);
        printf("| candidate |");
        for (int s = 0; s < 3; s++) printf(" press %.0f mm/s | release %.0f mm/s |", speeds[s], speeds[s]);
        printf(" missed |\n|---|---|---|---|---|---|---|---|\n");
        for (int i = 0; i < NCAND; i++) {
            printf("| %s |", cands[i].name);
            int miss_all = 0;
            for (int s = 0; s < 3; s++) {
                double p[2], r[2]; int miss;
                strokes(&cands[i], &g, speeds[s], rt, 200, p, r, &miss);
                miss_all += miss;
                printf(" %.2f / %.2f | %.2f / %.2f |", p[0], p[1], r[0], r[1]);
            }
            printf(" %d |\n", miss_all);
        }
    } else if (strcmp(mode, "minrt") == 0) {
        minrt_table(&g);
    } else if (strcmp(mode, "search") == 0) {
        search(&g);
    } else {
        fprintf(stderr, "usage: filter_sim table|latency|minrt|search [noise=white|mixed|lowfreq|spiky|real] [scale=1] [seconds=120] [seed=1]\n");
        return 2;
    }
    return 0;
}
