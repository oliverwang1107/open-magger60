/*
 * bench_engine_budget.c - worst-case per-frame work of the REAL key engine (km_engine.c + km_depth_model.c + km_keymap.c), M5 task T-E.
 * Not part of any build; tools/bench_engine_budget.py compiles and runs it.
 *
 *   bench_engine_budget <ranges.txt> [frames_scale]
 *
 * ranges.txt: 76 lines "released bottom" (ADC counts per slot; the endpoints come from core/data/slot_ranges_s2.json).
 *
 * Every frame is timed with the CPU time-stamp counter around one km_engine_update(). For each frame the harness also knows
 *   calls  how many times the depth model was called (a threshold recompute costs about 24, none otherwise), counted with the linker's
 *          --wrap of km_depth_model_mm100;
 *   iter   how many slots refresh_one() has to look at before it finds a dirty one (computed from the public engine state BEFORE the call;
 *          the loop in refresh_one() divides by n_active at each step, see STABILITY_NOTES.md);
 *   new    presses accepted in that frame.
 * The output is one line per scenario plus the five most expensive frames of each, machine readable (tab separated), see the python tool.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <km/km_depth_model.h>
#include <km/km_engine.h>
#include <km/km_keymap.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t ticks(void)
{
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
#else
static inline uint64_t ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* ---- depth model call counter ---- */
static unsigned g_calls;
uint16_t __real_km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value);
uint16_t __wrap_km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value)
{
    g_calls++;
    return __real_km_depth_model_mm100(cal, value);
}

/* private flag values of km_engine.c that the harness peeks at (read only) */
#define F_ACTIVE 0x02u
#define F_DIRTY  0x20u

static uint32_t g_rng = 20240601u;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
static int noise(void)               /* +-15 counts, flat */
{
    return (int)(rnd() % 31u) - 15;
}

static uint16_t g_rel[KM_ENGINE_SLOTS], g_bot[KM_ENGINE_SLOTS];
static km_engine_t g_e;

typedef struct {
    uint64_t ticks;
    unsigned calls, iter, news;
} frame_t;

#define TOPN 5
#define REPS 5                       /* every scenario is replayed REPS times with the same random numbers; per frame the fastest run counts */
typedef struct {
    const char *name;
    uint64_t n;                       /* frames of the current replay */
    uint64_t total;                   /* frames of the scenario (after the first replay) */
    uint64_t cap;
    uint32_t *min_ticks;
    uint8_t *calls, *iter, *news, *drift;   /* drift: how many keys look at their baseline in this frame (about 4 since the drift look is spread; was all 61 on every 16th) */
} stat_t;

static unsigned dirty_distance(const km_engine_t *e)
{
    if (!e->dirty) {
        return 0;
    }
    for (unsigned n = 0; n < e->n_active; n++) {
        const unsigned i = (e->cursor + n) % e->n_active;
        if (e->slot[e->active[i]].flags & F_DIRTY) {
            return n + 1u;
        }
    }
    return e->n_active;
}

static int cmp32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t g_overhead;

static void frame(stat_t *st, const uint16_t counts[KM_ENGINE_SLOTS])
{
    const unsigned it = dirty_distance(&g_e);
    const unsigned down0 = g_e.n_down;
    unsigned drift = 0;
    for (unsigned a = 0; a < g_e.n_active; a++) {
        drift += ((g_e.frame + g_e.active[a]) & (KM_ENGINE_DRIFT_PERIOD - 1u)) == 0u;
    }
    g_calls = 0;
    const uint64_t t0 = ticks();
    km_engine_update(&g_e, counts);
    const uint64_t t1 = ticks();
    uint64_t dt = (t1 - t0) > g_overhead ? (t1 - t0) - g_overhead : 0;
    if (dt > 0xFFFFFFFFull) {
        dt = 0xFFFFFFFFull;
    }
    if (st->n < st->cap) {
        const uint64_t i = st->n;
        if (st->min_ticks[i] == 0 || (uint32_t)dt < st->min_ticks[i]) {
            st->min_ticks[i] = (uint32_t)dt;
        }
        st->calls[i] = (uint8_t)(g_calls > 255u ? 255u : g_calls);
        st->iter[i] = (uint8_t)(it > 255u ? 255u : it);
        st->news[i] = (uint8_t)(g_e.n_down > down0 ? g_e.n_down - down0 : 0u);
        st->drift[i] = (uint8_t)drift;
    }
    st->n++;
}

static void report(stat_t *st)
{
    const uint64_t n = st->total < st->cap ? st->total : st->cap;
    uint32_t *sorted = malloc((size_t)n * sizeof sorted[0]);
    memcpy(sorted, st->min_ticks, (size_t)n * sizeof sorted[0]);
    qsort(sorted, (size_t)n, sizeof sorted[0], cmp32);
    uint64_t sum = 0, n_rec = 0, sum0 = 0, n0 = 0;
    uint64_t cs[4] = { 0, 0, 0, 0 }, cn[4] = { 0, 0, 0, 0 };   /* class = (recompute ? 2 : 0) + (drift look ? 1 : 0) */
    for (uint64_t i = 0; i < n; i++) {
        sum += st->min_ticks[i];
        {
            const unsigned c = (st->calls[i] ? 2u : 0u) + (st->drift[i] > 8u ? 1u : 0u);   /* a "drift look" frame = one where more than 8 keys looked (none since the spread) */
            cs[c] += st->min_ticks[i];
            cn[c]++;
        }
        if (st->calls[i]) {
            n_rec++;
        } else {
            n0++;
            sum0 += st->min_ticks[i];
        }
    }
    printf("S\t%s\t%llu\t%llu\t%u\t%u\t%u\t%u\t%llu\t%llu\n", st->name, (unsigned long long)n, (unsigned long long)(sum / (n ? n : 1)),
           sorted[n / 2], sorted[n * 99 / 100], sorted[n * 999 / 1000], sorted[n - 1], (unsigned long long)n_rec,
           (unsigned long long)(n0 ? sum0 / n0 : 0));
    printf("C\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n", st->name, (unsigned long long)cn[0], (unsigned long long)(cn[0] ? cs[0] / cn[0] : 0),
           (unsigned long long)cn[1], (unsigned long long)(cn[1] ? cs[1] / cn[1] : 0), (unsigned long long)cn[2],
           (unsigned long long)(cn[2] ? cs[2] / cn[2] : 0), (unsigned long long)cn[3], (unsigned long long)(cn[3] ? cs[3] / cn[3] : 0));
    /* the TOPN most expensive frames */
    uint64_t used[TOPN];
    for (unsigned t = 0; t < TOPN; t++) {
        uint64_t best = 0;
        uint32_t bt = 0;
        for (uint64_t i = 0; i < n; i++) {
            int taken = 0;
            for (unsigned u = 0; u < t; u++) {
                taken |= used[u] == i;
            }
            if (!taken && st->min_ticks[i] > bt) {
                bt = st->min_ticks[i];
                best = i;
            }
        }
        used[t] = best;
        printf("T\t%s\t%u\t%u\t%u\t%u\t%u\n", st->name, bt, st->calls[best], st->iter[best], st->news[best], st->drift[best]);
    }
    free(sorted);
}

static void stat_init(stat_t *st, const char *name, uint64_t cap)
{
    memset(st, 0, sizeof *st);
    st->name = name;
    st->cap = cap;
    st->min_ticks = calloc((size_t)cap, sizeof st->min_ticks[0]);
    st->calls = calloc((size_t)cap, 1);
    st->iter = calloc((size_t)cap, 1);
    st->news = calloc((size_t)cap, 1);
    st->drift = calloc((size_t)cap, 1);
}

static void stat_free(stat_t *st)
{
    free(st->min_ticks);
    free(st->calls);
    free(st->iter);
    free(st->news);
    free(st->drift);
}

/* ---- signal model: per slot a depth fraction n in 0..1 (0 = released, 1 = bottomed), reading = released - n * span + noise ---- */
static float g_n[KM_ENGINE_SLOTS];
static int g_off[KM_ENGINE_SLOTS];              /* baseline shift (thermal drift), counts */

static void make_counts(uint16_t out[KM_ENGINE_SLOTS])
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        float v = (float)g_rel[s] + (float)g_off[s] - g_n[s] * (float)(g_rel[s] - g_bot[s]) + (float)noise();
        if (v < 0) {
            v = 0;
        }
        if (v > 4095) {
            v = 4095;
        }
        out[s] = (uint16_t)v;
    }
}

static void fresh_engine(void)
{
    uint16_t c[KM_ENGINE_SLOTS];
    memset(g_n, 0, sizeof g_n);
    memset(g_off, 0, sizeof g_off);
    km_engine_init(&g_e);
    km_engine_calibrate_begin(&g_e);
    for (unsigned i = 0; i < 64; i++) {
        make_counts(c);
        km_engine_calibrate_idle(&g_e, c);
    }
    if (km_engine_calibrate_finish(&g_e) != KM_ENGINE_CAL_OK) {
        fprintf(stderr, "calibration refused\n");
        exit(2);
    }
}

static void settle(void)                           /* run until every pending recompute is done */
{
    uint16_t c[KM_ENGINE_SLOTS];
    stat_t scratch;
    stat_init(&scratch, "settle", 256);
    for (unsigned i = 0; i < 200 && g_e.dirty; i++) {
        make_counts(c);
        frame(&scratch, c);
    }
    stat_free(&scratch);
}

static void run_idle(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        make_counts(c);
        frame(st, c);
    }
}

/* keys pressed one after the other like typing: a 230-frame (10 ms) stroke down, hold, stroke up */
static void run_typing(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    int phase[KM_ENGINE_SLOTS];                    /* 0 idle, 1..230 going down, 231.. hold, then up */
    int hold[KM_ENGINE_SLOTS];
    memset(phase, 0, sizeof phase);
    for (uint64_t i = 0; i < frames; i++) {
        if (rnd() % 2500u == 0) {
            const unsigned s = rnd() % KM_ENGINE_SLOTS;
            if (km_keymap_slot_is_key(s) && phase[s] == 0) {
                phase[s] = 1;
                hold[s] = 1000 + (int)(rnd() % 3000u);
            }
        }
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (phase[s] == 0) {
                continue;
            }
            phase[s]++;
            const int p = phase[s];
            if (p <= 230) {
                g_n[s] = (float)p / 230.0f;
            } else if (p <= 230 + hold[s]) {
                g_n[s] = 1.0f;
            } else if (p <= 460 + hold[s]) {
                g_n[s] = 1.0f - (float)(p - 230 - hold[s]) / 230.0f;
            } else {
                g_n[s] = 0;
                phase[s] = 0;
            }
        }
        make_counts(c);
        frame(st, c);
    }
}

/* all 61 keys pressed at the same moment (hand on the keyboard / a fall), held, released */
static void run_all_keys(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        const uint64_t m = i % 6000u;
        float n = 0;
        if (m >= 500u && m < 730u) {
            n = (float)(m - 500u) / 230.0f;
        } else if (m >= 730u && m < 3000u) {
            n = 1.0f;
        } else if (m >= 3000u && m < 3230u) {
            n = 1.0f - (float)(m - 3000u) / 230.0f;
        }
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            g_n[s] = km_keymap_slot_is_key(s) ? n : 0;
        }
        make_counts(c);
        frame(st, c);
    }
}

/* thermal drift: every baseline moves together by +-60 counts over 6000 frames; every key's threshold becomes stale at about the same time */
static void run_drift(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        const double ph = (double)(i % 60000u) / 60000.0;
        const int off = (int)(60.0 * (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0));
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            g_off[s] = off;
        }
        make_counts(c);
        frame(st, c);
    }
}

/* a settings change touches every key in one frame (km_engine_set_key_cfg on all slots): 61 recomputes queue up, one per frame */
static void run_cfg_storm(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        if (i % 2000u == 0) {
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                km_engine_key_cfg_t cfg;
                if (km_engine_get_key_cfg(&g_e, s, &cfg) && km_keymap_slot_is_key(s)) {
                    cfg.press_mm100 = (uint16_t)(100u + rnd() % 60u);
                    cfg.release_mm100 = (uint16_t)(60u + rnd() % 30u);
                    (void)km_engine_set_key_cfg(&g_e, s, &cfg);
                }
            }
        }
        make_counts(c);
        frame(st, c);
    }
}

/* the worst case of refresh_one(): exactly one dirty slot, the farthest away from the round-robin cursor */
static void run_far_dirty(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        if (g_e.dirty == 0) {
            const unsigned idx = (g_e.cursor + g_e.n_active - 1u) % g_e.n_active;
            km_engine_key_cfg_t cfg;
            const unsigned s = g_e.active[idx];
            (void)km_engine_get_key_cfg(&g_e, s, &cfg);
            cfg.press_mm100 = (uint16_t)(cfg.press_mm100 == 120u ? 121u : 120u);
            (void)km_engine_set_key_cfg(&g_e, s, &cfg);
        }
        make_counts(c);
        frame(st, c);
    }
}

/* common-mode glitches (supply dips): 10 slots drop by 700 counts for 3 frames now and then, plus a floating input flickering */
static void run_glitch(stat_t *st, uint64_t frames)
{
    uint16_t c[KM_ENGINE_SLOTS];
    fresh_engine();
    settle();
    for (uint64_t i = 0; i < frames; i++) {
        make_counts(c);
        if ((i % 5000u) < 12u) {
            for (unsigned s = 0, k = 0; s < KM_ENGINE_SLOTS && k < 10u; s++) {
                if (km_keymap_slot_is_key(s)) {
                    c[s] = (uint16_t)(c[s] - 700);
                    k++;
                }
            }
        }
        if ((i % 777u) < 5u) {
            c[10] = 1000;
        }
        frame(st, c);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s ranges.txt [scale]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror("ranges");
        return 2;
    }
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        unsigned r, b;
        if (fscanf(f, "%u %u", &r, &b) != 2) {
            fprintf(stderr, "bad ranges file\n");
            return 2;
        }
        g_rel[s] = (uint16_t)r;
        g_bot[s] = (uint16_t)b;
    }
    fclose(f);
    const uint64_t scale = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;

    /* timer overhead and tick rate */
    uint64_t best = ~0ull;
    for (unsigned i = 0; i < 1000; i++) {
        const uint64_t a = ticks();
        const uint64_t b = ticks();
        if (b - a < best) {
            best = b - a;
        }
    }
    g_overhead = best;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const uint64_t k0 = ticks();
    do {
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec) < 100000000ll);
    const uint64_t k1 = ticks();
    printf("H\ttick_overhead\t%llu\ttick_hz\t%.0f\n", (unsigned long long)g_overhead,
           (double)(k1 - k0) / (((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 1e9));

    static const struct {
        const char *name;
        void (*fn)(stat_t *, uint64_t);
        uint64_t frames;
    } sc[] = {
        { "idle", run_idle, 300000 },          { "typing", run_typing, 1000000 },    { "all keys", run_all_keys, 120000 },
        { "drift storm", run_drift, 600000 },  { "cfg storm", run_cfg_storm, 60000 }, { "one far dirty slot", run_far_dirty, 100000 },
        { "glitch + fault slot", run_glitch, 200000 },
    };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        stat_t st;
        const uint64_t frames = sc[i].frames * scale;
        stat_init(&st, sc[i].name, frames);
        for (unsigned rep = 0; rep < REPS; rep++) {
            g_rng = 20240601u;
            st.n = 0;
            sc[i].fn(&st, frames);
        }
        st.total = st.n;
        report(&st);
        stat_free(&st);
    }
    return 0;
}
