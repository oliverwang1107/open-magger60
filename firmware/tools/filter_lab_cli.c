/*
 * filter_lab_cli.c - command line front end of the filter lab (tests/filter_lab.[ch]); driven by tools/filter_eval.py, which also
 * builds it. Every command prints ONE line "RESULT cmd=... key=value ...".
 *
 *   quiet   false events on a key at rest, keys held at three depths and two keys with a finger wobble (rapid trigger, distance rt)
 *   latency reversal / crossing latency on strokes of one speed (rapid or fixed)
 *   step    90 % rise time of the filters and the analog paths on a depth step
 *   trace   run a real capture (KMTR file) through the engine
 *   bench   host time per frame of the variants
 *
 * Arguments are key=value: filter= profile= rt= seconds= scale= seed= seeds= speed= mode= cycles= depth= wig= floor= file= tN= k= nref=
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <km/km_keymap.h>

#include "../tests/filter_lab.h"

typedef struct {
    const char *filter, *profile, *mode, *file;
    unsigned rt, depth, seeds, cycles, nref, floor_;
    double seconds, scale, speed, wig;
    uint64_t seed;
    int t1, t2, k;
} args_t;

static double num(const char *v) { return strtod(v, NULL); }

static void parse(int argc, char **argv, args_t *a)
{
    memset(a, 0, sizeof *a);
    a->filter = "F0"; a->profile = "mixed"; a->mode = "rapid"; a->file = NULL;
    a->rt = 20; a->depth = 150; a->seeds = 6; a->cycles = 30; a->nref = 0;
    a->seconds = 30; a->scale = 1; a->speed = 300; a->wig = 0.15; a->seed = 1; a->t1 = -1; a->t2 = -1; a->k = -1;
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            fprintf(stderr, "argument without '=': %s\n", argv[i]);
            exit(2);
        }
        *eq = 0;
        const char *k = argv[i], *v = eq + 1;
        if (!strcmp(k, "filter")) a->filter = v;
        else if (!strcmp(k, "profile")) a->profile = v;
        else if (!strcmp(k, "mode")) a->mode = v;
        else if (!strcmp(k, "file")) a->file = v;
        else if (!strcmp(k, "rt")) a->rt = (unsigned)num(v);
        else if (!strcmp(k, "depth")) a->depth = (unsigned)num(v);
        else if (!strcmp(k, "seeds")) a->seeds = (unsigned)num(v);
        else if (!strcmp(k, "cycles")) a->cycles = (unsigned)num(v);
        else if (!strcmp(k, "nref")) a->nref = (unsigned)num(v);
        else if (!strcmp(k, "floor")) a->floor_ = (unsigned)num(v);
        else if (!strcmp(k, "seconds")) a->seconds = num(v);
        else if (!strcmp(k, "scale")) a->scale = num(v);
        else if (!strcmp(k, "speed")) a->speed = num(v);
        else if (!strcmp(k, "wig")) a->wig = num(v);
        else if (!strcmp(k, "seed")) a->seed = (uint64_t)num(v);
        else if (!strcmp(k, "t1")) a->t1 = (int)num(v);
        else if (!strcmp(k, "t2")) a->t2 = (int)num(v);
        else if (!strcmp(k, "k")) a->k = (int)num(v);
        else { fprintf(stderr, "unknown argument: %s\n", k); exit(2); }
    }
}

static lab_variant_t g_var_copy;

static const lab_variant_t *pick_variant(const args_t *a)
{
    const lab_variant_t *v = lab_variant_by_name(a->filter);
    if (v == NULL) {
        fprintf(stderr, "unknown filter %s\n", a->filter);
        exit(2);
    }
    g_var_copy = *v;
    if (a->t1 >= 0) g_var_copy.t1 = a->t1;
    if (a->t2 >= 0) g_var_copy.t2 = a->t2;
    if (a->k >= 0) g_var_copy.k = a->k;
    return &g_var_copy;
}

static const lab_noise_t *pick_noise(const args_t *a)
{
    if (!strcmp(a->profile, "none")) {
        return NULL;
    }
    const lab_noise_t *n = lab_profile_by_name(a->profile);
    if (n == NULL) {
        fprintf(stderr, "unknown profile %s\n", a->profile);
        exit(2);
    }
    return n;
}

static void cfg_rapid(km_engine_key_cfg_t *c, unsigned rt)
{
    km_engine_key_cfg_default(c);
    c->mode = KM_ENGINE_TRIG_RAPID;
    c->rt_down_mm100 = (uint16_t)rt;
    c->rt_up_mm100 = (uint16_t)rt;
    c->top_dead_mm100 = LAB_TD;
    c->bottom_dead_mm100 = 0;
    c->continuous = 1;
    c->press_mm100 = 0;
}

static void pick_slots(lab_run_t *r, unsigned n)
{
    unsigned k = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && k < n; s++) {
        if (km_keymap_slot_is_key(s)) {
            r->slot[k++] = s;
        }
    }
    r->n_keys = n;
}

static void base_run(lab_run_t *r, const args_t *a, unsigned long frames, uint64_t seed)
{
    memset(r, 0, sizeof *r);
    r->variant = pick_variant(a);
    r->noise = pick_noise(a);
    r->noise_scale = a->scale;
    r->seed = seed;
    r->n_frames = frames;
    r->n_ref = a->nref;
    r->rt_floor = (uint16_t)(a->floor_ != 0 ? a->floor_ : (a->rt < KM_ENGINE_RT_MIN_MM100 ? a->rt : KM_ENGINE_RT_MIN_MM100));
}

/* ---------------------------------------------------------------- quiet */

static int cmd_quiet(const args_t *a)
{
    const unsigned long frames = (unsigned long)(a->seconds * LAB_FS);
    lab_path_t p[6];
    lab_run_t r;
    base_run(&r, a, frames, a->seed);
    pick_slots(&r, 6);
    static const unsigned depths[6] = { 0, 30, 150, 300, 30, 150 };
    for (unsigned k = 0; k < 6; k++) {
        lab_path_init(&p[k], frames + 8);
        if (depths[k] != 0) {
            lab_path_ramp(&p[k], depths[k], 100.0);
            lab_path_hold(&p[k], 0.5);
            if (k >= 4) {
                lab_path_wiggle(&p[k], a->seconds, a->wig * a->rt, 8.0);
            }
        }
        lab_path_hold(&p[k], a->seconds);
        r.path[k] = &p[k];
        cfg_rapid(&r.cfg[k], a->rt);
    }
    r.stat_from = (unsigned long)(1.5 * LAB_FS);
    lab_result_t res;
    const int rc = lab_run(&r, &res);
    if (rc != 0) {
        printf("RESULT cmd=quiet error=%d\n", rc);
        return 1;
    }
    unsigned ev[6] = { 0 };
    for (unsigned i = 0; i < res.n_ev; i++) {
        const lab_event_t *e = &res.ev[i];
        if (e->key == 0 || e->frame >= (unsigned long)(1.0 * LAB_FS)) {
            ev[e->key]++;
        }
    }
    printf("RESULT cmd=quiet filter=%s profile=%s rt=%u scale=%g seconds=%g false_rest=%u false_h30=%u false_h150=%u false_h300=%u false_w30=%u false_w150=%u false_total=%u",
           a->filter, a->profile, a->rt, a->scale, a->seconds, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[0] + ev[1] + ev[2] + ev[3] + ev[4] + ev[5]);
    for (unsigned k = 0; k < 4; k++) {
        const lab_key_stats_t *s = &res.key[k];
        printf(" sd_f_%u=%.3f sd_g_%u=%.3f", k, lab_stat_sd(s->n, s->sum_f, s->sumsq_f), k, lab_stat_sd(s->n, s->sum_g, s->sumsq_g));
    }
    for (unsigned k = 1; k <= 3; k++) {
        const lab_key_stats_t *s = &res.key[k];
        for (int i = 0; i < LAB_NUM_A; i++) {
            printf(" a%d_sd_%u=%.3f a%d_jps_%u=%.2f a%d_pp_%u=%d", i, k, lab_stat_sd(s->a_n[i], s->a_sum[i], s->a_sumsq[i]), i, k,
                   (double)s->a_jumps[i] / (a->seconds - 1.5), i, k, s->a_max[i] - s->a_min[i]);
        }
    }
    printf("\n");
    for (unsigned k = 0; k < 6; k++) lab_path_free(&p[k]);
    lab_result_free(&res);
    return 0;
}

/* ---------------------------------------------------------------- latency */

static int cmd_latency(const args_t *a)
{
    const int fixed = !strcmp(a->mode, "fixed");
    double press_lat[4096], rel_lat[4096];
    unsigned n_p = 0, n_r = 0, missed = 0, extra = 0, matched = 0;
    for (unsigned sd = 0; sd < a->seeds; sd++) {
        lab_path_t p;
        lab_path_init(&p, (unsigned long)((a->cycles * (2.0 * a->depth / (a->speed * 100.0) + 0.3) + 1.0) * LAB_FS));
        lab_path_hold(&p, 0.15);
        for (unsigned c = 0; c < a->cycles; c++) {
            lab_path_ramp(&p, a->depth, a->speed);
            lab_path_ramp(&p, 0, a->speed);
            lab_path_hold(&p, 0.15);
        }
        lab_run_t r;
        base_run(&r, a, p.n, a->seed + sd);
        pick_slots(&r, 1);
        r.path[0] = &p;
        if (fixed) {
            km_engine_key_cfg_default(&r.cfg[0]);
        } else {
            cfg_rapid(&r.cfg[0], a->rt);
        }
        lab_result_t res;
        if (lab_run(&r, &res) != 0) {
            printf("RESULT cmd=latency error=1\n");
            return 1;
        }
        lab_event_t ideal[1024];
        const unsigned ni = fixed ? lab_ideal_fixed(&p, 120, 80, ideal, 1024) : lab_ideal_rapid(&p, a->rt, a->rt, ideal, 1024);
        lab_match_t *m = (lab_match_t *)malloc(sizeof *m);
        lab_match(ideal, ni, res.ev, res.n_ev, 0, (unsigned)(0.010 * LAB_FS), (unsigned)(0.2 * LAB_FS), m);
        matched += m->matched; missed += m->missed; extra += m->extra;
        for (unsigned i = 0; i < m->n_lat[1] && n_p < 4096; i++) press_lat[n_p++] = m->lat_ms[1][i];
        for (unsigned i = 0; i < m->n_lat[0] && n_r < 4096; i++) rel_lat[n_r++] = m->lat_ms[0][i];
        free(m);
        lab_path_free(&p);
        lab_result_free(&res);
    }
    double pm = 0, pp95 = 0, rm = 0, rp95 = 0;
    if (n_p) { double t[4096]; memcpy(t, press_lat, n_p * sizeof(double)); pm = lab_median(t, n_p); memcpy(t, press_lat, n_p * sizeof(double)); pp95 = lab_percentile(t, n_p, 95); }
    if (n_r) { double t[4096]; memcpy(t, rel_lat, n_r * sizeof(double)); rm = lab_median(t, n_r); memcpy(t, rel_lat, n_r * sizeof(double)); rp95 = lab_percentile(t, n_r, 95); }
    printf("RESULT cmd=latency mode=%s filter=%s profile=%s rt=%u speed=%g depth=%u scale=%g matched=%u missed=%u extra=%u press_med_ms=%.3f press_p95_ms=%.3f rel_med_ms=%.3f rel_p95_ms=%.3f press_med_mm100=%.2f rel_med_mm100=%.2f\n",
           a->mode, a->filter, a->profile, a->rt, a->speed, a->depth, a->scale, matched, missed, extra, pm, pp95, rm, rp95,
           pm * a->speed / 10.0, rm * a->speed / 10.0);
    return 0;
}

/* ---------------------------------------------------------------- step */

static int cmd_step(const args_t *a)
{
    const unsigned long pre = (unsigned long)(0.15 * LAB_FS), post = (unsigned long)(0.15 * LAB_FS);
    const unsigned long n = pre + post;
    double rise[LAB_NUM_A] = { 0 };
    double rise_f = 0;
    for (unsigned sd = 0; sd < a->seeds; sd++) {
        lab_path_t p;
        lab_path_init(&p, n + 8);
        lab_path_step(&p, 100);
        lab_path_hold(&p, 0.15);
        lab_path_step(&p, 200);
        lab_path_hold(&p, 0.15);
        lab_run_t r;
        base_run(&r, a, p.n, a->seed + sd);
        pick_slots(&r, 1);
        r.path[0] = &p;
        km_engine_key_cfg_default(&r.cfg[0]);
        r.cfg[0].press_mm100 = 340;
        r.cfg[0].release_mm100 = 330;
        float *ser[LAB_NUM_A];
        for (int i = 0; i < LAB_NUM_A; i++) { ser[i] = (float *)calloc(p.n + 8, sizeof(float)); r.series[i] = ser[i]; }
        lab_result_t res;
        if (lab_run(&r, &res) != 0) { printf("RESULT cmd=step error=1\n"); return 1; }
        for (int i = 0; i < LAB_NUM_A; i++) {
            double a0 = 0, a1 = 0;
            for (unsigned long f = pre / 2; f < pre; f++) a0 += ser[i][f];
            a0 /= (double)(pre - pre / 2);
            for (unsigned long f = n - post / 4; f < n; f++) a1 += ser[i][f];
            a1 /= (double)(post / 4);
            const double thr = a0 + 0.9 * (a1 - a0);
            unsigned long f = pre;
            while (f < n && ser[i][f] < thr) f++;
            rise[i] += (double)(f - pre) * 1000.0 / LAB_FS;
            free(ser[i]);
        }
        rise_f += 0;
        lab_path_free(&p);
        lab_result_free(&res);
    }
    printf("RESULT cmd=step filter=%s profile=%s scale=%g", a->filter, a->profile, a->scale);
    for (int i = 0; i < LAB_NUM_A; i++) printf(" a%d_rise90_ms=%.3f", i, rise[i] / a->seeds);
    printf("\n");
    (void)rise_f;
    return 0;
}

/* ---------------------------------------------------------------- trace */

static int cmd_trace(const args_t *a)
{
    lab_trace_t t;
    if (a->file == NULL || lab_trace_load(&t, a->file) != 0) {
        printf("RESULT cmd=trace error=load\n");
        return 1;
    }
    lab_run_t r;
    base_run(&r, a, t.n_frames, a->seed);
    r.noise = NULL;
    r.trace = &t;
    r.n_keys = t.n_cols < LAB_MAX_KEYS ? t.n_cols : LAB_MAX_KEYS;
    for (unsigned k = 0; k < r.n_keys; k++) {
        r.slot[k] = t.slots[k];
        if (!km_keymap_slot_is_key(r.slot[k])) {
            printf("RESULT cmd=trace error=slot%u_is_not_a_key\n", r.slot[k]);
            return 1;
        }
        if (!strcmp(a->mode, "fixed")) km_engine_key_cfg_default(&r.cfg[k]); else cfg_rapid(&r.cfg[k], a->rt);
        r.span[k] = 0;
    }
    r.stat_from = (unsigned long)(0.05 * t.n_frames);
    lab_result_t res;
    const int rc = lab_run(&r, &res);
    if (rc != 0) {
        printf("RESULT cmd=trace error=run%d\n", rc);
        return 1;
    }
    printf("RESULT cmd=trace file=%s filter=%s mode=%s rt=%u frames=%lu seconds=%.3f events=%u", a->file, a->filter, a->mode, a->rt,
           t.n_frames, (double)t.n_frames / (t.fs > 0 ? t.fs : LAB_FS), res.n_ev);
    for (unsigned k = 0; k < r.n_keys; k++) {
        const lab_key_stats_t *s = &res.key[k];
        printf(" slot%u_sd_f=%.3f slot%u_sd_g=%.3f", r.slot[k], lab_stat_sd(s->n, s->sum_f, s->sumsq_f), r.slot[k], lab_stat_sd(s->n, s->sum_g, s->sumsq_g));
        for (int i = 0; i < LAB_NUM_A; i++) {
            printf(" slot%u_a%d_sd=%.3f slot%u_a%d_jumps=%lu", r.slot[k], i, lab_stat_sd(s->a_n[i], s->a_sum[i], s->a_sumsq[i]), r.slot[k], i, s->a_jumps[i]);
        }
    }
    printf("\n");
    lab_result_free(&res);
    lab_trace_free(&t);
    return 0;
}

/* ---------------------------------------------------------------- bench */

static int cmd_bench(const args_t *a)
{
    printf("RESULT cmd=bench");
    for (int real = 1; real >= 0; real--) {
        for (unsigned v = 0; v < lab_n_variants; v++) {
            if (real && v != 0) continue;
            lab_path_t p[6];
            lab_run_t r;
            args_t b = *a;
            b.filter = lab_variants[v].name;
            const unsigned long frames = (unsigned long)(a->seconds * LAB_FS);
            base_run(&r, &b, frames, 3);
            pick_slots(&r, 6);
            r.use_real_engine = real;
            for (unsigned k = 0; k < 6; k++) {
                lab_path_init(&p[k], frames + 8);
                lab_path_ramp(&p[k], 100, 50);
                lab_path_hold(&p[k], a->seconds);
                r.path[k] = &p[k];
                cfg_rapid(&r.cfg[k], 25);
            }
            lab_result_t res;
            const clock_t t0 = clock();
            if (lab_run(&r, &res) != 0) { printf(" error=1\n"); return 1; }
            const double ns = (double)(clock() - t0) * 1e9 / CLOCKS_PER_SEC / (double)(frames + 504);
            printf(" %s%s_ns_per_frame=%.0f", real ? "real_" : "", lab_variants[v].name, ns);
            for (unsigned k = 0; k < 6; k++) lab_path_free(&p[k]);
            lab_result_free(&res);
        }
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: filter_lab_cli quiet|latency|step|trace|bench [key=value ...]\n");
        return 2;
    }
    args_t a;
    parse(argc, argv, &a);
    if (!strcmp(argv[1], "quiet")) return cmd_quiet(&a);
    if (!strcmp(argv[1], "latency")) return cmd_latency(&a);
    if (!strcmp(argv[1], "step")) return cmd_step(&a);
    if (!strcmp(argv[1], "trace")) return cmd_trace(&a);
    if (!strcmp(argv[1], "bench")) return cmd_bench(&a);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
