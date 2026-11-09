/*
 * test_pipeline_m6.c - the M6 key pipeline (km_pipeline.[hc], package P10): SOCD -> layers -> DKS / gamepad / light actions.
 *
 *   1. a profile without M6 entries behaves exactly like the layer logic alone (report equality)
 *   2. engine settings of DKS and gamepad slots (km_pipeline_slot_cfg)
 *   3. the order of the stages: SOCD hides a key from the layers; DKS events with a simulated engine; gamepad axis; light actions once per press
 *   4. hold-over: keys held through a profile change or a fault do not come back by themselves
 *   5. fault: toggle keys, DKS, gamepad and tap-hold all drop at once
 *   6. RANDOM sequences x random valid tables x random profile changes, faults and resets: structural invariants every pass, and
 *      "all keys up => nothing is output" at the end of every run
 *   7. bounded work per pass (counters)
 * The key engine is replaced by a model that only makes the list the engine would make (press order, depth for every key that is down);
 * the engine itself is tested in test_engine*.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_pipeline.h"
#include "km/km_settings.h"
#include "km_test.h"

void test_pipeline_m6(void);

#define CPM 240000u                 /* cycles per millisecond at 240 MHz, as KM_MS_CYCLES */

static uint32_t g_rng = 0x1F2E3D4Cu;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static unsigned rn(unsigned n) { return rnd() % n; }

/* ---- the engine model ---- */
typedef struct {
    uint8_t  down[KM_ENGINE_MAX_DOWN];
    unsigned n;
    uint16_t depth[KM_KEYMAP_SLOTS];
    uint32_t cycles;
} sim_t;

static uint16_t sim_depth(void *ctx, unsigned slot)
{
    const sim_t *s = ctx;
    return slot < KM_KEYMAP_SLOTS ? s->depth[slot] : 0;
}

static bool sim_is_down(const sim_t *s, unsigned slot)
{
    for (unsigned i = 0; i < s->n; i++) {
        if (s->down[i] == slot) {
            return true;
        }
    }
    return false;
}

static void sim_press(sim_t *s, unsigned slot, unsigned depth)
{
    s->depth[slot] = (uint16_t)depth;
    if (!sim_is_down(s, slot) && s->n < KM_ENGINE_MAX_DOWN) {
        s->down[s->n++] = (uint8_t)slot;
    }
}

static void sim_release(sim_t *s, unsigned slot)
{
    s->depth[slot] = 0;
    for (unsigned i = 0; i < s->n; i++) {
        if (s->down[i] == slot) {
            memmove(s->down + i, s->down + i + 1, s->n - i - 1u);
            s->n--;
            return;
        }
    }
}

static void sim_release_all(sim_t *s)
{
    s->n = 0;
    memset(s->depth, 0, sizeof s->depth);
}

static km_pipe_out_t step(km_pipeline_t *p, sim_t *s, bool allowed)
{
    km_pipe_out_t out;
    const km_pipe_in_t in = { s->down, s->n, allowed, s->cycles, CPM, sim_depth, s };
    s->cycles += CPM;                                     /* one millisecond per pass */
    km_pipeline_run(p, &in, &out);
    return out;
}

static void run_ms(km_pipeline_t *p, sim_t *s, unsigned ms, km_pipe_out_t *last)
{
    for (unsigned i = 0; i < ms; i++) {
        *last = step(p, s, true);
    }
}

static bool has_usage(const km_pipe_out_t *o, uint8_t u)
{
    for (unsigned i = 0; i < o->lo.kbd_n; i++) {
        if (o->lo.kbd[i] == u) {
            return true;
        }
    }
    return false;
}

/* ---- tables ---- */
static km_settings_t g_set;

static unsigned key_slot(unsigned i)
{
    unsigned seen = 0;
    for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            if (seen++ == i) {
                return s;
            }
        }
    }
    return 0;
}

static void set_entry(km_profile_t *pr, unsigned layer, unsigned slot, unsigned code, unsigned a0, unsigned a1)
{
    pr->layer_usage[layer][slot] = (uint8_t)code;
    pr->layer_arg[layer][slot][0] = (uint8_t)a0;
    pr->layer_arg[layer][slot][1] = (uint8_t)a1;
}

/* a profile whose tables are empty (no keys at all) except what the test sets */
static km_profile_t *fresh(void)
{
    km_settings_default(&g_set);
    km_profile_t *pr = &g_set.profile[0];
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            set_entry(pr, l, s, l == 0 ? 0u : KM_CODE_TRANSPARENT, 0, 0);
        }
    }
    pr->dks_n = pr->morph_n = pr->socd_n = 0;
    return pr;
}

static km_dks_rec_t make_dks(uint8_t a, uint8_t b)
{
    km_dks_rec_t r;
    memset(&r, 0, sizeof r);
    r.t_act_mm100 = 120;
    r.t_bot_mm100 = 0;                                          /* 310 */
    r.act[0].usage = a;                                         /* a: down at event 1, up at event 4 */
    r.act[0].ev = (uint8_t)(KM_DKS_EV_SET(0, KM_DKS_EV_PRESS) | KM_DKS_EV_SET(3, KM_DKS_EV_RELEASE));
    r.act[1].usage = b;                                         /* b: down at event 2, up at event 3 */
    r.act[1].ev = (uint8_t)(KM_DKS_EV_SET(1, KM_DKS_EV_PRESS) | KM_DKS_EV_SET(2, KM_DKS_EV_RELEASE));
    return r;
}

static km_pipeline_t g_pipe;

static void load(const km_profile_t *pr, const sim_t *s)
{
    km_layers_reset();
    km_pipeline_init(&g_pipe);
    km_pipeline_set_profile(&g_pipe, pr, s->down, s->n);
    CHECK(km_settings_valid(&g_set));
    (void)step(&g_pipe, (sim_t *)s, true);                  /* the first pass after a load sees no key: the gamepad's start-up pass */
}

/* ------------------------------------------------------------------------------------------------ 1. equivalence */
static void test_plain_equals_layers(void)
{
    km_profile_t *pr = fresh();
    const unsigned a = key_slot(0), b = key_slot(1), c = key_slot(2), fn = key_slot(3);
    set_entry(pr, 0, a, 0x04, 0, 0);
    set_entry(pr, 0, b, 0x05, 0, 0);
    set_entry(pr, 0, c, KM_CODE_TAP_HOLD, 0x06, 0xE1);
    set_entry(pr, 0, fn, KM_CODE_HOLD_L1, 0, 0);
    set_entry(pr, 1, a, 0x1D, 0, 0);
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    CHECK(!g_pipe.has_dks && !g_pipe.has_pad && !g_pipe.has_light);

    uint8_t rec[4000][2];
    unsigned nrec = 0;
    uint32_t seed = g_rng;
    for (unsigned pass = 0; pass < 2000; pass++) {                  /* the pipeline run */
        if (rn(6) == 0) {
            const unsigned k = rn(4);
            const unsigned slot = k == 0 ? a : k == 1 ? b : k == 2 ? c : fn;
            if (sim_is_down(&s, slot)) {
                sim_release(&s, slot);
            } else {
                sim_press(&s, slot, 200);
            }
        }
        const km_pipe_out_t o = step(&g_pipe, &s, true);
        CHECK(!o.pad_recomputed);
        rec[nrec][0] = o.lo.kbd_n;
        rec[nrec][1] = o.lo.kbd_n ? o.lo.kbd[0] : 0;
        nrec++;
        if (nrec == 4000u) {
            break;
        }
    }
    /* the same key sequence straight through the layer logic (SOCD has no pairs here) */
    g_rng = seed;
    sim_release_all(&s);
    s.cycles = 0;
    km_layers_reset();
    (void)km_layers_set_table(pr->layer_usage, pr->layer_arg, &pr->layer_opts);
    for (unsigned pass = 0; pass < nrec; pass++) {
        if (rn(6) == 0) {
            const unsigned k = rn(4);
            const unsigned slot = k == 0 ? a : k == 1 ? b : k == 2 ? c : fn;
            if (sim_is_down(&s, slot)) {
                sim_release(&s, slot);
            } else {
                sim_press(&s, slot, 200);
            }
        }
        km_layers_out_t lo;
        km_layers_update_cycles(s.down, s.n, s.cycles, CPM, &lo);
        s.cycles += CPM;
        CHECK_EQ(lo.kbd_n, rec[pass][0]);
        CHECK_EQ(lo.kbd_n ? lo.kbd[0] : 0, rec[pass][1]);
    }
}

/* ------------------------------------------------------------------------------------------------ 2. engine settings */
static void test_slot_cfg(void)
{
    km_profile_t *pr = fresh();
    const unsigned d = key_slot(0), p1 = key_slot(1), p2 = key_slot(2), n = key_slot(3), bad = key_slot(4);
    pr->dks[0] = make_dks(0x04, 0x16);
    pr->dks[0].t_act_mm100 = 150;
    pr->dks_n = 1;
    set_entry(pr, 0, d, 0x04, 0, 0);                              /* layer 0 is a plain key, layer 2 the DKS: the DKS still decides */
    set_entry(pr, 2, d, KM_CODE_DKS, 0, 0);
    set_entry(pr, 0, p1, KM_CODE_PAD_BTN, 1, 0);
    set_entry(pr, 1, p2, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(0, 0, 0), 0);
    set_entry(pr, 0, bad, KM_CODE_DKS, 5, 0);                     /* no such record: not overridden */
    pr->key[n].mode = KM_ENGINE_TRIG_RAPID;
    pr->key[n].rt_down_mm100 = 40;
    pr->key[n].rt_up_mm100 = 40;
    pr->key[n].press_mm100 = 0;
    km_engine_key_cfg_t c;
    CHECK(km_pipeline_slot_cfg(pr, d, &c));
    CHECK_EQ(c.mode, KM_ENGINE_TRIG_FIXED);
    CHECK_EQ(c.press_mm100, 150);
    CHECK_EQ(c.release_mm100, 140);
    CHECK(km_settings_cfg_valid(&c));
    CHECK(km_pipeline_slot_cfg(pr, p1, &c));
    CHECK_EQ(c.press_mm100, KM_PAD_ENGINE_PRESS_MM100);
    CHECK_EQ(c.release_mm100, KM_PAD_ENGINE_RELEASE_MM100);
    CHECK(km_settings_cfg_valid(&c));
    CHECK(km_pipeline_slot_cfg(pr, p2, &c));
    CHECK_EQ(c.press_mm100, KM_PAD_ENGINE_PRESS_MM100);
    CHECK(!km_pipeline_slot_cfg(pr, n, &c));
    CHECK_EQ(c.mode, KM_ENGINE_TRIG_RAPID);                       /* the user's own config is passed through untouched */
    CHECK_EQ(c.rt_down_mm100, 40);
    CHECK(!km_pipeline_slot_cfg(pr, bad, &c));
    CHECK(!km_pipeline_slot_cfg(pr, KM_KEYMAP_SLOTS + 3u, &c));
}

/* ------------------------------------------------------------------------------------------------ 3. the stages */
static void test_socd_before_layers(void)
{
    km_profile_t *pr = fresh();
    const unsigned a = key_slot(0), d = key_slot(1);
    set_entry(pr, 0, a, 0x04, 0, 0);
    set_entry(pr, 0, d, 0x07, 0, 0);
    pr->socd[0].slot_a = (uint8_t)a;
    pr->socd[0].slot_b = (uint8_t)d;
    pr->socd[0].mode = KM_SOCD_LAST;
    pr->socd_n = 1;
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    km_pipe_out_t o;
    sim_press(&s, a, 300);
    run_ms(&g_pipe, &s, 3, &o);
    CHECK(has_usage(&o, 0x04) && !has_usage(&o, 0x07));
    sim_press(&s, d, 300);
    run_ms(&g_pipe, &s, 3, &o);
    CHECK(!has_usage(&o, 0x04) && has_usage(&o, 0x07));          /* last input wins: the layer logic never saw A */
    CHECK_EQ(o.n_after_socd, 1);
    sim_release(&s, d);
    run_ms(&g_pipe, &s, 3, &o);
    CHECK(has_usage(&o, 0x04) && !has_usage(&o, 0x07));          /* A comes back, it was held all the time */
    sim_release(&s, a);
    run_ms(&g_pipe, &s, 3, &o);
    CHECK_EQ(o.lo.kbd_n, 0);
}

static void test_dks_events(void)
{
    km_profile_t *pr = fresh();
    const unsigned k = key_slot(5);
    pr->dks[0] = make_dks(0x04, 0x16);                            /* a at event 1/4, s at 2/3 */
    pr->dks_n = 1;
    set_entry(pr, 0, k, KM_CODE_DKS, 0, 0);
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    CHECK(g_pipe.has_dks);
    km_pipe_out_t o;
    run_ms(&g_pipe, &s, 5, &o);
    CHECK_EQ(o.lo.kbd_n, 0);
    /* the engine reports the key at the start point (120) and the depth keeps rising */
    sim_press(&s, k, 130);
    run_ms(&g_pipe, &s, 12, &o);
    CHECK(has_usage(&o, 0x04) && !has_usage(&o, 0x16));
    s.depth[k] = 335;
    run_ms(&g_pipe, &s, 12, &o);
    CHECK(has_usage(&o, 0x04) && has_usage(&o, 0x16));
    s.depth[k] = 260;                                              /* back past the bottom point minus hysteresis: event 3 */
    run_ms(&g_pipe, &s, 12, &o);
    CHECK(has_usage(&o, 0x04) && !has_usage(&o, 0x16));
    sim_release(&s, k);                                            /* the engine releases: event 4 */
    run_ms(&g_pipe, &s, 30, &o);
    CHECK_EQ(o.lo.kbd_n, 0);
    CHECK(!km_dks_busy(&g_pipe.dks));

    /* the DKS usages are merged with ordinary keys, once each */
    const unsigned other = key_slot(6);
    set_entry(pr, 0, other, 0x04, 0, 0);                           /* the same usage as DKS action 0 */
    load(pr, &s);
    sim_press(&s, other, 200);
    sim_press(&s, k, 200);
    run_ms(&g_pipe, &s, 12, &o);
    CHECK_EQ(o.lo.kbd_n, 1);
    CHECK(has_usage(&o, 0x04));
    sim_release(&s, other);
    run_ms(&g_pipe, &s, 12, &o);
    CHECK(has_usage(&o, 0x04));                                    /* still held by the DKS key */
    sim_release(&s, k);
    run_ms(&g_pipe, &s, 30, &o);
    CHECK_EQ(o.lo.kbd_n, 0);
}

static void test_pad_axis_and_buttons(void)
{
    km_profile_t *pr = fresh();
    const unsigned right = key_slot(0), left = key_slot(1), btn = key_slot(2);
    set_entry(pr, 0, right, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(0, 0, 1), 0);   /* LX +, digital */
    set_entry(pr, 0, left, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(0, 1, 1), 0);    /* LX -, digital */
    set_entry(pr, 0, btn, KM_CODE_PAD_BTN, 3, 0);
    pr->pad_opts.stick_deadzone = 0;
    pr->pad_opts.circular = 0;
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    km_pipe_out_t o;
    run_ms(&g_pipe, &s, 3, &o);
    CHECK(km_pad_report_neutral(&o.pad));
    sim_press(&s, right, 100);
    run_ms(&g_pipe, &s, 5, &o);
    CHECK_EQ(o.pad.lx, 32767);
    CHECK_EQ(o.lo.kbd_n, 0);                                       /* a gamepad key types nothing */
    sim_press(&s, btn, 100);
    run_ms(&g_pipe, &s, 5, &o);
    CHECK_EQ(o.pad.buttons, 1u << 2);
    sim_press(&s, left, 100);                                      /* both digital, equally deep: they cancel */
    run_ms(&g_pipe, &s, 5, &o);
    CHECK_EQ(o.pad.lx, 0);
    sim_release_all(&s);
    run_ms(&g_pipe, &s, 5, &o);
    CHECK(km_pad_report_neutral(&o.pad));
}

static void test_light_actions_once_per_press(void)
{
    km_profile_t *pr = fresh();
    const unsigned k1 = key_slot(0), k2 = key_slot(1);
    set_entry(pr, 0, k1, KM_CODE_LIGHT, KM_LIGHT_ACT_NEXT_FX, 0);
    set_entry(pr, 0, k2, KM_CODE_LIGHT, KM_LIGHT_ACT_BRIGHT_UP, 0);
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    CHECK(g_pipe.has_light);
    km_pipe_out_t o;
    sim_press(&s, k1, 200);
    o = step(&g_pipe, &s, true);
    CHECK_EQ(o.light_n, 1);
    CHECK_EQ(o.light[0], KM_LIGHT_ACT_NEXT_FX);
    unsigned later = 0;
    for (unsigned i = 0; i < 100; i++) {
        o = step(&g_pipe, &s, true);
        later += o.light_n;
    }
    CHECK_EQ(later, 0);                                            /* held: no repeat */
    sim_release(&s, k1);
    run_ms(&g_pipe, &s, 3, &o);
    sim_press(&s, k1, 200);
    sim_press(&s, k2, 200);
    o = step(&g_pipe, &s, true);
    CHECK_EQ(o.light_n, 2);                                        /* two keys in one pass: both, in press order */
    CHECK_EQ(o.light[0], KM_LIGHT_ACT_NEXT_FX);
    CHECK_EQ(o.light[1], KM_LIGHT_ACT_BRIGHT_UP);
    CHECK_EQ(o.lo.kbd_n, 0);
}

/* ------------------------------------------------------------------------------------------------ 4. hold-over */
static void test_holdover_after_profile_change(void)
{
    km_profile_t *pr = fresh();
    const unsigned k = key_slot(0), l = key_slot(1), g = key_slot(2);
    pr->dks[0] = make_dks(0x04, 0x16);
    pr->dks_n = 1;
    for (unsigned i = 0; i < 2; i++) {
        km_profile_t *q = &g_set.profile[i];
        set_entry(q, 0, k, KM_CODE_DKS, 0, 0);
        set_entry(q, 0, l, KM_CODE_LIGHT, KM_LIGHT_ACT_TOGGLE, 0);
        set_entry(q, 0, g, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(1, 1, 1), 0);
        q->dks[0] = pr->dks[0];
        q->dks_n = 1;
    }
    pr->pad_opts.stick_deadzone = 0;
    g_set.profile[1].pad_opts = pr->pad_opts;
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    km_pipe_out_t o;
    sim_press(&s, k, 200);
    sim_press(&s, l, 200);
    sim_press(&s, g, 200);
    run_ms(&g_pipe, &s, 15, &o);
    CHECK(has_usage(&o, 0x04));
    CHECK_EQ(o.pad.ly, -32767);
    /* profile switch with all three keys held */
    km_pipeline_set_profile(&g_pipe, &g_set.profile[1], s.down, s.n);
    unsigned light = 0;
    for (unsigned i = 0; i < 60; i++) {
        o = step(&g_pipe, &s, true);
        light += o.light_n;
        CHECK(!has_usage(&o, 0x04));                               /* the DKS key is ignored until it is released once */
        CHECK(km_pad_report_neutral(&o.pad));
    }
    CHECK_EQ(light, 0);
    /* released once: the keys work again */
    sim_release_all(&s);
    run_ms(&g_pipe, &s, 5, &o);
    sim_press(&s, k, 200);
    sim_press(&s, l, 200);
    sim_press(&s, g, 200);
    o = step(&g_pipe, &s, true);
    CHECK_EQ(o.light_n, 1);
    run_ms(&g_pipe, &s, 15, &o);
    CHECK(has_usage(&o, 0x04));
    CHECK_EQ(o.pad.ly, -32767);
}

/* ------------------------------------------------------------------------------------------------ 5. faults */
static void test_fault_drops_everything(void)
{
    km_profile_t *pr = fresh();
    const unsigned t = key_slot(0), k = key_slot(1), g = key_slot(2), x = key_slot(3), th = key_slot(4);
    set_entry(pr, 0, t, KM_CODE_TOGGLE_KEY, 0x2C, 0);              /* toggle Space */
    pr->dks[0] = make_dks(0x04, 0x16);
    pr->dks_n = 1;
    set_entry(pr, 0, k, KM_CODE_DKS, 0, 0);
    set_entry(pr, 0, g, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(2, 0, 1), 0);
    set_entry(pr, 0, x, 0x1B, 0, 0);
    set_entry(pr, 0, th, KM_CODE_TAP_HOLD, 0x1C, 0xE0);
    pr->pad_opts.stick_deadzone = 0;
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    km_pipe_out_t o;
    sim_press(&s, t, 200);
    run_ms(&g_pipe, &s, 15, &o);
    sim_release(&s, t);
    run_ms(&g_pipe, &s, 15, &o);
    CHECK(has_usage(&o, 0x2C));                                    /* the toggle key keeps Space on with every key up: by design */
    CHECK_EQ(o.lo.toggle_keys, 1);
    sim_press(&s, k, 200);
    sim_press(&s, g, 200);
    sim_press(&s, x, 200);
    sim_press(&s, th, 200);
    run_ms(&g_pipe, &s, 15, &o);
    CHECK(has_usage(&o, 0x04) && has_usage(&o, 0x1B));
    CHECK_EQ(o.pad.rx, 32767);

    /* fault: everything out at once, in the same pass */
    o = step(&g_pipe, &s, false);
    CHECK_EQ(o.lo.kbd_n, 0);
    CHECK_EQ(o.lo.consumer, 0);
    CHECK(km_pad_report_neutral(&o.pad));
    CHECK_EQ(o.light_n, 0);
    CHECK(g_pipe.blocked);
    CHECK(!km_dks_busy(&g_pipe.dks));
    for (unsigned i = 0; i < 20; i++) {
        o = step(&g_pipe, &s, false);
        CHECK_EQ(o.lo.kbd_n, 0);
    }
    /* the fault is over, the keys are still down: the plain key is a new press, DKS / gamepad / toggle state are gone */
    run_ms(&g_pipe, &s, 15, &o);
    CHECK(has_usage(&o, 0x1B));
    CHECK(!has_usage(&o, 0x04));
    CHECK(!has_usage(&o, 0x2C));
    CHECK_EQ(o.lo.toggle_keys, 0);
    CHECK(km_pad_report_neutral(&o.pad));
    sim_release_all(&s);
    run_ms(&g_pipe, &s, 800, &o);
    CHECK_EQ(o.lo.kbd_n, 0);
    CHECK(km_pad_report_neutral(&o.pad));
    /* and they work again */
    sim_press(&s, g, 200);
    run_ms(&g_pipe, &s, 5, &o);
    CHECK_EQ(o.pad.rx, 32767);
    /* km_pipeline_reset has the same effect as a fault */
    km_pipeline_reset(&g_pipe);
    sim_release_all(&s);
    o = step(&g_pipe, &s, true);
    CHECK(km_pad_report_neutral(&o.pad));
    CHECK_EQ(o.lo.kbd_n, 0);
}

/* ------------------------------------------------------------------------------------------------ 6. random */

static bool entry_ok(unsigned c, unsigned a0, unsigned a1)
{
    return km_keymap_entry_valid((uint8_t)c, (uint8_t)a0, (uint8_t)a1);
}

static void random_entry(km_profile_t *pr, unsigned layer, unsigned slot)
{
    for (unsigned tries = 0; tries < 20; tries++) {
        unsigned c, a0 = 0, a1 = 0;
        switch (rn(16)) {
        case 0: c = 0; break;
        case 1: c = layer ? KM_CODE_TRANSPARENT : 0x05; break;
        case 2: c = 0xE0 + rn(8); break;
        case 3: c = KM_CODE_HOLD_L1; break;
        case 4: c = KM_CODE_HOLD_L2; break;
        case 5: c = KM_CODE_TOGGLE; a0 = 1 + rn(3); break;
        case 6: c = KM_CODE_TAP_HOLD; a0 = 0x04 + rn(26); a1 = rn(3) == 0 ? KM_CODE_HOLD_L1 : (0xE0 + rn(8)); break;
        case 7: c = KM_CODE_ONESHOT_MOD; a0 = 0xE0 + rn(8); break;
        case 8: c = KM_CODE_TOGGLE_KEY; a0 = 0x04 + rn(26); break;
        case 9: c = KM_CODE_MOD_MORPH; a0 = 0x04 + rn(26); a1 = rn(2); break;
        case 10: c = KM_CODE_DKS; a0 = rn(3); break;
        case 11: c = KM_CODE_PAD_BTN; a0 = 1 + rn(20); a1 = rn(3) == 0 ? 4 + rn(30) : 0; break;
        case 12: {
            const unsigned axis = rn(6);
            c = KM_CODE_PAD_AXIS;
            a0 = KM_PAD_AXIS_ARG(axis, axis < 4 ? rn(2) : 0, rn(2));
            a1 = rn(4);
            break;
        }
        case 13: c = KM_CODE_LIGHT; a0 = 1 + rn(7); break;
        case 14: c = (KM_CODE_PROFILE_1 + 1u); break;
        default: c = 0x04 + rn(26); break;
        }
        if (entry_ok(c, a0, a1)) {
            set_entry(pr, layer, slot, c, a0, a1);
            return;
        }
    }
    set_entry(pr, layer, slot, 0x04, 0, 0);
}

#define NKEYS 12u

static void random_profile(km_profile_t *pr, unsigned slots[NKEYS])
{
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            set_entry(pr, l, s, l == 0 ? 0u : KM_CODE_TRANSPARENT, 0, 0);
        }
    }
    for (unsigned i = 0; i < NKEYS; i++) {
        for (unsigned l = 0; l < KM_LAYERS; l++) {
            random_entry(pr, l, slots[i]);
        }
        /* the engine settings the pipeline asks for must be acceptable to the engine */
        km_engine_key_cfg_t c;
        (void)km_pipeline_slot_cfg(pr, slots[i], &c);
        CHECK(km_settings_cfg_valid(&c));
    }
    pr->morph_n = 2;
    for (unsigned i = 0; i < 2; i++) {
        pr->morph[i].mod_mask = (uint8_t)(1u << rn(8));
        pr->morph[i].usage = (uint8_t)(0x04 + rn(26));
        pr->morph[i].flags = (uint8_t)rn(2);
        pr->morph[i].rsvd = 0;
    }
    pr->dks_n = 3;
    for (unsigned i = 0; i < 3; i++) {
        pr->dks[i] = make_dks((uint8_t)(0x04 + rn(26)), (uint8_t)(0x04 + rn(26)));
        pr->dks[i].t_act_mm100 = (uint16_t)(60 + rn(100));
        pr->dks[i].flags = (uint8_t)rn(2);
        if (rn(2)) {
            pr->dks[i].act[2].usage = (uint8_t)(0x04 + rn(26));
            pr->dks[i].act[2].ev = (uint8_t)(KM_DKS_EV_SET(1, KM_DKS_EV_TAP) | KM_DKS_EV_SET(2, KM_DKS_EV_PRESS));
        }
    }
    pr->socd_n = 0;
    for (unsigned i = 0; i < 2; i++) {
        km_socd_pair_t pair = { (uint8_t)slots[2 * i], (uint8_t)slots[2 * i + 1], (uint8_t)(1 + rn(4)), (uint8_t)rn(2) };
        pr->socd[pr->socd_n++] = pair;
    }
    pr->pad_opts.stick_deadzone = (uint8_t)rn(30);
    pr->pad_opts.circular = (uint8_t)rn(2);
}

static void check_structure(const km_pipe_out_t *o, const sim_t *s)
{
    CHECK(o->lo.kbd_n <= KM_KEYMAP_SLOTS);
    for (unsigned i = 0; i < o->lo.kbd_n; i++) {
        CHECK(o->lo.kbd[i] >= 0x04 && o->lo.kbd[i] <= 0xE7);
        for (unsigned j = 0; j < i; j++) {
            CHECK(o->lo.kbd[i] != o->lo.kbd[j]);
        }
    }
    CHECK(o->light_n <= KM_PIPE_MAX_LIGHT);
    for (unsigned i = 0; i < o->light_n; i++) {
        CHECK(o->light[i] >= 1 && o->light[i] <= KM_LIGHT_ACT_LAST);
    }
    CHECK(o->pad.hat <= 8);
    CHECK(o->pad.lx >= -KM_PAD_AXIS_MAX && o->pad.lx <= KM_PAD_AXIS_MAX);
    CHECK(o->pad.ly >= -KM_PAD_AXIS_MAX && o->pad.ly <= KM_PAD_AXIS_MAX);
    CHECK(o->pad.rx >= -KM_PAD_AXIS_MAX && o->pad.rx <= KM_PAD_AXIS_MAX);
    CHECK(o->pad.ry >= -KM_PAD_AXIS_MAX && o->pad.ry <= KM_PAD_AXIS_MAX);
    CHECK(o->pad.lt <= KM_PAD_AXIS_MAX && o->pad.rt <= KM_PAD_AXIS_MAX);
    uint8_t bytes[KM_PAD_REPORT_LEN];
    km_pad_report_t back;
    km_pad_report_pack(&o->pad, bytes);
    km_pad_report_unpack(bytes, &back);
    CHECK(km_pad_report_equal(&o->pad, &back));                    /* the report the sync layer compares is exactly the state */
    CHECK(o->lo.layer < KM_LAYERS);
    (void)s;
}

static void test_random_runs(void)
{
    unsigned runs_with_toggle_left = 0, total_light = 0, max_pad_keys = 0;
    for (unsigned run = 0; run < 400; run++) {
        unsigned slots[NKEYS];
        for (unsigned i = 0; i < NKEYS; i++) {
            slots[i] = key_slot(i * 3u % 50u + (run % 3u));
        }
        /* distinct slots */
        for (unsigned i = 0; i < NKEYS; i++) {
            for (unsigned j = 0; j < i; j++) {
                if (slots[i] == slots[j]) {
                    slots[i] = key_slot(50u + i);
                }
            }
        }
        km_settings_default(&g_set);
        random_profile(&g_set.profile[0], slots);
        random_profile(&g_set.profile[1], slots);
        CHECK(km_settings_valid(&g_set));
        sim_t s;
        memset(&s, 0, sizeof s);
        km_layers_reset();
        km_pipeline_init(&g_pipe);
        unsigned active = 0;
        km_pipeline_set_profile(&g_pipe, &g_set.profile[active], s.down, s.n);

        const unsigned passes = 1500;
        unsigned fault_left = 0;
        for (unsigned pass = 0; pass < passes; pass++) {
            const unsigned ev = rn(100);
            if (ev < 12) {                                         /* a key goes down or up */
                const unsigned slot = slots[rn(NKEYS)];
                if (sim_is_down(&s, slot)) {
                    sim_release(&s, slot);
                } else {
                    sim_press(&s, slot, 20 + rn(321));
                }
            } else if (ev < 30 && s.n != 0) {                      /* depth moves */
                const unsigned slot = s.down[rn(s.n)];
                int d = (int)s.depth[slot] + (int)rn(61) - 30;
                s.depth[slot] = (uint16_t)(d < 1 ? 1 : d > 340 ? 340 : d);
            } else if (ev == 99 && rn(8) == 0) {                   /* profile switch with keys down */
                active ^= 1u;
                km_pipeline_set_profile(&g_pipe, &g_set.profile[active], s.down, s.n);
            } else if (ev == 98 && rn(8) == 0) {
                fault_left = 1 + rn(40);
            } else if (ev == 97 && rn(10) == 0) {
                km_pipeline_reset(&g_pipe);
            } else if (ev == 96 && rn(30) == 0) {
                sim_release_all(&s);
            }
            const bool allowed = fault_left == 0;
            if (fault_left) {
                fault_left--;
            }
            const uint32_t lookups0 = g_pipe.lookups;
            const km_pipe_out_t o = step(&g_pipe, &s, allowed);
            CHECK(g_pipe.lookups - lookups0 <= KM_ENGINE_MAX_DOWN);
            check_structure(&o, &s);
            total_light += o.light_n;
            if (!allowed) {
                CHECK_EQ(o.lo.kbd_n, 0);
                CHECK(km_pad_report_neutral(&o.pad));
                CHECK_EQ(o.light_n, 0);
            }
        }
        if (g_pipe.pad_keys_max > max_pad_keys) {
            max_pad_keys = g_pipe.pad_keys_max;
        }
        CHECK(g_pipe.dks_keys_max <= KM_DKS_KEYS);
        CHECK(g_pipe.pad_keys_max <= KM_PAD_MAX_KEYS);
        /* every key up, and enough time for tap pulses, replay queues and DKS queues to run out */
        sim_release_all(&s);
        km_pipe_out_t o;
        for (unsigned i = 0; i < 1500; i++) {
            o = step(&g_pipe, &s, true);
        }
        /* What may legitimately be on with every key up: toggled keys (by design, until pressed again or a reset) and armed one-shot
         * modifiers (a tap arms them for the NEXT key; with the one-shot timeout 0 they wait for ever). Nothing else. */
        {
            unsigned armed_bits = 0, other = 0, armed_on = 0;
            for (unsigned b = 0; b < 8u; b++) {
                armed_bits += (o.lo.armed >> b) & 1u;
            }
            for (unsigned i = 0; i < o.lo.kbd_n; i++) {
                const uint8_t u = o.lo.kbd[i];
                if (u >= 0xE0 && u <= 0xE7 && ((o.lo.armed >> (u - 0xE0)) & 1u)) {
                    armed_on++;
                } else {
                    other++;
                }
            }
            CHECK(other <= o.lo.toggle_keys);
            CHECK(o.lo.kbd_n <= o.lo.toggle_keys + armed_bits);
            CHECK(armed_on <= armed_bits);
            CHECK_EQ(o.lo.consumer, 0);
            CHECK(km_pad_report_neutral(&o.pad));
            CHECK(!km_dks_busy(&g_pipe.dks));
            if (o.lo.toggle_keys != 0u || armed_bits != 0u) {
                runs_with_toggle_left++;
            }
        }
        /* and a reset brings even the toggled keys to rest */
        km_pipeline_reset(&g_pipe);
        o = step(&g_pipe, &s, true);
        CHECK_EQ(o.lo.kbd_n, 0);
        CHECK_EQ(o.lo.toggle_keys, 0);
    }
    CHECK(max_pad_keys > 0);
    CHECK(total_light > 0);
    CHECK(runs_with_toggle_left < 400);                            /* the random tables do exercise toggle keys, but not in every run */
    printf("  pipeline random: 400 runs x 1500 passes, %u runs ended with a toggle key on or a one-shot modifier armed, %u light actions, up to %u pad keys at once\n",
           runs_with_toggle_left, total_light, max_pad_keys);
}

/* ------------------------------------------------------------------------------------------------ 7. bounded work */
static void test_bounded_work(void)
{
    km_profile_t *pr = fresh();
    for (unsigned i = 0; i < 40; i++) {
        const unsigned slot = key_slot(i);
        switch (i % 4) {
        case 0: set_entry(pr, 0, slot, KM_CODE_DKS, 0, 0); break;
        case 1: set_entry(pr, 0, slot, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(i % 6, 0, 1), 0); break;
        case 2: set_entry(pr, 0, slot, KM_CODE_LIGHT, 1 + i % 7, 0); break;
        default: set_entry(pr, 0, slot, 0x04 + i, 0, 0); break;
        }
    }
    pr->dks[0] = make_dks(0x04, 0x05);
    pr->dks_n = 1;
    sim_t s;
    memset(&s, 0, sizeof s);
    load(pr, &s);
    for (unsigned i = 0; i < 40; i++) {
        sim_press(&s, key_slot(i), 200);
    }
    km_pipe_out_t o;
    for (unsigned i = 0; i < 50; i++) {
        const uint32_t l0 = g_pipe.lookups;
        o = step(&g_pipe, &s, true);
        CHECK(g_pipe.lookups - l0 <= 40u);
        CHECK(o.light_n <= KM_PIPE_MAX_LIGHT);
    }
    CHECK(g_pipe.dks_keys_max <= KM_DKS_KEYS);
    CHECK(g_pipe.pad_keys_max <= KM_PAD_MAX_KEYS);
    CHECK(g_pipe.light_dropped > 0 || g_pipe.light_actions >= 10);
    /* no M6 entries in the tables: no lookups at all */
    pr = fresh();
    set_entry(pr, 0, key_slot(0), 0x04, 0, 0);
    load(pr, &s);
    sim_release_all(&s);
    sim_press(&s, key_slot(0), 200);
    const uint32_t l0 = g_pipe.lookups;
    run_ms(&g_pipe, &s, 20, &o);
    CHECK_EQ(g_pipe.lookups, l0);
}


/* ------------------------------------------------------------------------------------------------ 8. wiring details */
static void test_wiring_details(void)
{
    km_pipe_out_t o;
    sim_t s;
    km_engine_key_cfg_t c;

    /* SOCD "only on layer 0": the pair sleeps while another layer is active (the layer of the previous pass decides) */
    {
        km_profile_t *pr = fresh();
        const unsigned a = key_slot(0), d = key_slot(1), fn = key_slot(2);
        set_entry(pr, 0, a, 0x04, 0, 0);
        set_entry(pr, 0, d, 0x07, 0, 0);
        set_entry(pr, 0, fn, KM_CODE_HOLD_L1, 0, 0);
        pr->socd[0].slot_a = (uint8_t)a;
        pr->socd[0].slot_b = (uint8_t)d;
        pr->socd[0].mode = KM_SOCD_LAST;
        pr->socd[0].flags = KM_SOCD_FLAG_LAYER0;
        pr->socd_n = 1;
        memset(&s, 0, sizeof s);
        load(pr, &s);
        sim_press(&s, a, 300);
        sim_press(&s, d, 300);
        run_ms(&g_pipe, &s, 3, &o);
        CHECK(!has_usage(&o, 0x04) && has_usage(&o, 0x07));       /* layer 0: last input wins */
        sim_release_all(&s);
        run_ms(&g_pipe, &s, 3, &o);
        sim_press(&s, fn, 300);
        run_ms(&g_pipe, &s, 3, &o);
        sim_press(&s, a, 300);
        sim_press(&s, d, 300);
        run_ms(&g_pipe, &s, 3, &o);
        CHECK_EQ(o.lo.layer, 1);
        CHECK(has_usage(&o, 0x04) && has_usage(&o, 0x07));         /* layer 1: the pair is asleep, both keys are reported (layer 1 is transparent) */
    }

    /* a DKS entry on layer 3 only is found; a LIGHT key with argument 0 does nothing */
    {
        km_profile_t *pr = fresh();
        const unsigned k = key_slot(0), l = key_slot(1);
        pr->dks[0] = make_dks(0x04, 0x05);
        pr->dks_n = 1;
        set_entry(pr, 3, k, KM_CODE_DKS, 0, 0);
        set_entry(pr, 0, l, KM_CODE_LIGHT, 0, 0);
        memset(&s, 0, sizeof s);
        load(pr, &s);
        CHECK(g_pipe.has_dks);
        CHECK(g_pipe.has_light);
        sim_press(&s, l, 200);
        o = step(&g_pipe, &s, true);
        CHECK_EQ(o.light_n, 0);
    }

    /* engine settings: a gamepad key drops every RAPID field of the user's config; a DKS record beyond the count is not used */
    {
        km_profile_t *pr = fresh();
        const unsigned p1 = key_slot(0), bad = key_slot(1);
        pr->key[p1].mode = KM_ENGINE_TRIG_RAPID;
        pr->key[p1].rt_down_mm100 = 40;
        pr->key[p1].rt_up_mm100 = 40;
        pr->key[p1].continuous = 1;
        pr->key[p1].top_dead_mm100 = 30;
        set_entry(pr, 0, p1, KM_CODE_PAD_BTN, 2, 0);
        CHECK(km_pipeline_slot_cfg(pr, p1, &c));
        CHECK_EQ(c.mode, KM_ENGINE_TRIG_FIXED);
        CHECK_EQ(c.rt_down_mm100, 0);
        CHECK_EQ(c.continuous, 0);
        CHECK_EQ(c.top_dead_mm100, 0);
        pr->dks[5] = make_dks(0x04, 0x05);                           /* a valid record, but beyond dks_n */
        pr->dks_n = 1;
        set_entry(pr, 0, bad, KM_CODE_DKS, 5, 0);
        CHECK(!km_pipeline_slot_cfg(pr, bad, &c));
    }

    /* the gamepad: a layer change is a new epoch (neutral, the held key is ignored until released); a reset with the key down too;
     * the curves of the profile are used; more pad keys than the module takes are cut off */
    {
        km_profile_t *pr = fresh();
        const unsigned x = key_slot(0), fn = key_slot(1);
        set_entry(pr, 0, x, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(0, 0, 1), 0);
        set_entry(pr, 0, fn, KM_CODE_HOLD_L1, 0, 0);
        pr->pad_opts.stick_deadzone = 0;
        memset(&s, 0, sizeof s);
        load(pr, &s);
        sim_press(&s, x, 200);
        run_ms(&g_pipe, &s, 5, &o);
        CHECK_EQ(o.pad.lx, 32767);
        sim_press(&s, fn, 200);                                      /* layer 0 -> 1 */
        run_ms(&g_pipe, &s, 5, &o);
        CHECK(km_pad_report_neutral(&o.pad));
        sim_release(&s, fn);
        run_ms(&g_pipe, &s, 5, &o);
        CHECK(km_pad_report_neutral(&o.pad));                        /* the key was held over: it stays quiet until released once */
        sim_release(&s, x);
        run_ms(&g_pipe, &s, 5, &o);
        sim_press(&s, x, 200);
        run_ms(&g_pipe, &s, 5, &o);
        CHECK_EQ(o.pad.lx, 32767);
        km_pipeline_reset(&g_pipe);                                  /* a reset (USB bus reset) with the key still down */
        o = step(&g_pipe, &s, true);
        CHECK(km_pad_report_neutral(&o.pad));
        run_ms(&g_pipe, &s, 5, &o);
        CHECK(km_pad_report_neutral(&o.pad));

        pr = fresh();
        set_entry(pr, 0, x, KM_CODE_PAD_AXIS, KM_PAD_AXIS_ARG(0, 0, 0), 0);   /* LX analog, curve 0 */
        pr->pad_curve[0].start_mm100_5 = 4;                          /* 0.20 mm */
        pr->pad_curve[0].end_mm100_5 = 8;                            /* 0.40 mm */
        pr->pad_curve[0].shape = 0;
        pr->pad_opts.stick_deadzone = 0;
        pr->pad_opts.circular = 0;
        memset(&s, 0, sizeof s);
        load(pr, &s);
        sim_press(&s, x, 60);                                        /* 0.60 mm: beyond the end of the short curve */
        run_ms(&g_pipe, &s, 5, &o);
        CHECK_EQ(o.pad.lx, 32767);                                   /* (the default curve would give about 2000 here) */

        pr = fresh();                                                /* 30 pad keys at once */
        for (unsigned i = 0; i < 30; i++) {
            set_entry(pr, 0, key_slot(i), KM_CODE_PAD_BTN, 1 + i % 16, 0);
        }
        memset(&s, 0, sizeof s);
        load(pr, &s);
        for (unsigned i = 0; i < 30; i++) {
            sim_press(&s, key_slot(i), 200);
        }
        run_ms(&g_pipe, &s, 5, &o);
        CHECK_EQ(g_pipe.pad_keys_max, KM_PAD_MAX_KEYS);
        CHECK(o.pad.buttons != 0);
    }

    /* Mod-Morph records of the profile reach the layer logic */
    {
        km_profile_t *pr = fresh();
        const unsigned sh = key_slot(0), m = key_slot(1);
        set_entry(pr, 0, sh, 0xE1, 0, 0);                            /* left shift */
        set_entry(pr, 0, m, KM_CODE_MOD_MORPH, 0x04, 0);
        pr->morph[0].mod_mask = 0x02;                                /* bit 1 = usage 0xE1 */
        pr->morph[0].usage = 0x05;
        pr->morph[0].flags = 0;
        pr->morph_n = 1;
        memset(&s, 0, sizeof s);
        load(pr, &s);
        sim_press(&s, m, 200);
        run_ms(&g_pipe, &s, 3, &o);
        CHECK(has_usage(&o, 0x04) && !has_usage(&o, 0x05));
        sim_release_all(&s);
        run_ms(&g_pipe, &s, 3, &o);
        sim_press(&s, sh, 200);
        run_ms(&g_pipe, &s, 3, &o);
        sim_press(&s, m, 200);
        run_ms(&g_pipe, &s, 3, &o);
        CHECK(has_usage(&o, 0x05) && !has_usage(&o, 0x04) && has_usage(&o, 0xE1));
    }
}

void test_pipeline_m6(void)
{
    test_plain_equals_layers();
    test_slot_cfg();
    test_socd_before_layers();
    test_dks_events();
    test_pad_axis_and_buttons();
    test_light_actions_once_per_press();
    test_holdover_after_profile_change();
    test_fault_drops_everything();
    test_random_runs();
    test_bounded_work();
    test_wiring_details();
}
