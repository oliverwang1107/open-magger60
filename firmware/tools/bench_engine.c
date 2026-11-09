/*
 * bench_engine.c - host timing of km_engine_update() (M4 task T-B, rapid-trigger cases added in M5 task T-A).
 * Not part of any build.
 *
 *   gcc -O2 -Ifirmware/core/include -Ifirmware/tests firmware/tools/bench_engine.c firmware/core/src/km_engine.c \
 *       firmware/core/src/km_keymap.c firmware/core/src/km_depth_model.c -o bench_engine
 *
 * Add  -Wl,--wrap=km_depth_model_mm100 -DBENCH_COUNT  to also print how many depth-model calls a frame makes
 * (that is what the rapid-trigger path adds; one call is about 40-50 cycles on the Cortex-M4F).
 *
 * Prints nanoseconds per frame for 76 slots (61 keys) in these cases, each with every key on FIXED and on RAPID:
 *   all keys up   - noise only
 *   10 keys held  - ten keys pressed deep and still (noise only)
 *   10 keys moving- ten keys slowly moving between 0.3 mm and 1.5 mm, i.e. off the rest position all the time
 *   all 61 moving - the same on every key (not a real use, the worst case of the model path)
 * Run it on the old engine for a before / after comparison (the FIXED lines are the ones that must not grow much).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <km/km_depth_model.h>
#include <km/km_engine.h>

#ifdef BENCH_COUNT
unsigned long bench_model_calls;
uint16_t __real_km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value);
uint16_t __wrap_km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value)
{
    bench_model_calls++;
    return __real_km_depth_model_mm100(cal, value);
}
#endif

#define NF 256u

static km_engine_t eng;

static double run(const uint16_t frames[][KM_ENGINE_SLOTS], unsigned reps)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned r = 0; r < reps; r++) {
        for (unsigned i = 0; i < NF; i++) {
            km_engine_update(&eng, frames[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    return ns / ((double)NF * reps);
}

static unsigned rng = 12345;
static int noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (int)((rng >> 24) % 31u) - 15;
}

/* mode 0 idle, 1 ten keys held, 2 ten keys moving, 3 all keys moving */
static void build(uint16_t frames[][KM_ENGINE_SLOTS], int mode)
{
    for (unsigned i = 0; i < NF; i++) {
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            int v = 3300;
            const int sel = (mode == 3) || (s % 6u == 1u);
            if (mode == 1 && sel) {
                v = 2100;
            } else if (mode >= 2 && sel) {
                /* triangle wave 3300 .. 3050 (about 0.25 .. 2.5 mm), a different phase per key */
                const unsigned ph = (i * 4u + s * 7u) % 256u;
                v = 3300 - (int)(ph < 128u ? ph : 256u - ph) * 2;
            }
            frames[i][s] = (uint16_t)(v + noise());
        }
    }
}

#ifndef RAPID_NOT_AVAILABLE /* -DRAPID_NOT_AVAILABLE: build against an engine without rapid trigger (before / after) */
static void set_mode(int rapid)
{
    km_engine_key_cfg_t c;
    km_engine_key_cfg_default(&c);
    if (rapid) {
        c.mode = KM_ENGINE_TRIG_RAPID;
        c.continuous = 1;
    }
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        km_engine_set_key_cfg(&eng, s, &c); /* slots without a key: ignored by the engine */
    }
}
#endif

/* the keys of case 1 go down one after the other (from rest to the held depth) */
static void engage_staggered(void)
{
    for (unsigned t = 0; t < 400u; t++) {
        uint16_t fr[KM_ENGINE_SLOTS];
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            const unsigned rank = s / 6u; /* the keys of the case are s % 6 == 1 */
            const int down = (s % 6u == 1u) && t >= 24u + 12u * rank;
            fr[s] = (uint16_t)((down ? 2100 : 3300) + noise());
        }
        km_engine_update(&eng, fr);
    }
}

int main(void)
{
    static uint16_t cases[4][NF][KM_ENGINE_SLOTS];
    static const char *names[4] = { "all keys up    ", "10 keys held   ", "10 keys moving ", "all 61 moving  " };
    for (int m = 0; m < 4; m++) {
        build(cases[m], m);
    }
    printf("(%.0f ns per frame is 23 kHz)\n", 1e9 / 23000.0);
    for (int rapid = 0; rapid <= 1; rapid++) {
        for (int m = 0; m < 4; m++) {
            km_engine_init(&eng);
            km_engine_calibrate_begin(&eng);
            for (unsigned i = 0; i < NF; i++) {
                uint16_t idle[KM_ENGINE_SLOTS];
                for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                    idle[s] = (uint16_t)(3300 + noise());
                }
                km_engine_calibrate_idle(&eng, idle);
            }
            km_engine_calibrate_finish(&eng);
#ifdef RAPID_NOT_AVAILABLE
            if (rapid) {
                continue;
            }
#else
            set_mode(rapid);
#endif
            if (m == 1) {
                engage_staggered(); /* one key every 12 frames, 8 pressed in one frame would be a glitch and be dropped */
            }
            run(cases[m], 40); /* warm up, settles the filters and the threshold recomputes */
#ifdef BENCH_COUNT
            bench_model_calls = 0;
#endif
            const double ns = run(cases[m], 400);
            printf("%-5s %s: %6.0f ns/frame", rapid ? "RAPID" : "FIXED", names[m], ns);
            if (m == 1) {
                printf("  (%u keys down)", (unsigned)eng.n_down);
            }
#ifdef BENCH_COUNT
            printf("   %.2f depth-model calls/frame", (double)bench_model_calls / ((double)NF * 400.0));
#endif
            printf("\n");
        }
    }
    return 0;
}
