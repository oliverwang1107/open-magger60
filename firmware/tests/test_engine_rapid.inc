/*
 * test_engine_rapid.inc - part of test_engine.c: rapid trigger (KM_ENGINE_TRIG_RAPID) of the key engine.
 *
 * Signals are depth trajectories of one key (linear in depth, converted to a reading with the true endpoints
 * of core/data/slot_ranges_s2.json) plus +-15 counts of uniform noise; the checks are written against the true
 * depth at the event (rig_t.press_depth / release_depth), never against fixed counts. The rules being tested
 * are the ones in core/ENGINE_NOTES.md ("Rapid trigger").
 */

typedef struct {
    rig_t   *r;
    unsigned s;
    int      depth;     /* current true depth, 0.01 mm */
} traj_t;

/* move the key linearly in depth to `to` in `frames` frames */
static void tj_go(traj_t *t, int to, unsigned frames)
{
    const int from = t->depth;
    for (unsigned i = 1; i <= frames; i++) {
        const int dd = from + (to - from) * (int)i / (int)frames;
        t->r->level[t->s] = level_at(t->s, (uint16_t)dd);
        rig_step(t->r);
    }
    t->depth = to;
}

static km_engine_key_cfg_t rt_cfg(unsigned down, unsigned up, unsigned top, unsigned bot, unsigned cont)
{
    km_engine_key_cfg_t c;
    km_engine_key_cfg_default(&c);
    c.mode = KM_ENGINE_TRIG_RAPID;
    c.rt_down_mm100 = (uint16_t)down;
    c.rt_up_mm100 = (uint16_t)up;
    c.top_dead_mm100 = (uint16_t)top;
    c.bottom_dead_mm100 = (uint16_t)bot;
    c.continuous = (uint8_t)cont;
    return c;
}

/* a keyboard with all keys up, the true bottom of `slot` known and `slot` set to `c` */
static void rt_setup(rig_t *r, uint32_t seed, int noise, unsigned slot, const km_engine_key_cfg_t *c)
{
    rig_boot(r, seed, noise);
    CHECK(km_engine_set_bottom(&r->e, slot, data()->bottom[slot]));
    CHECK(km_engine_set_key_cfg(&r->e, slot, c));
    rig_run(r, 200);
}

#define IN(v, lo, hi) ((int)(v) >= (lo) && (int)(v) <= (hi))

/* ---------------------------------------------------------------- configuration */

static void test_rapid_config(void)
{
    km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 1);
    CHECK(km_engine_cfg_valid(&c));
    CHECK_EQ(KM_ENGINE_RT_MIN_MM100, 20);

    /* the noise floor: 19 is refused, 20 is accepted, for each of the four distances */
    c = rt_cfg(19, 30, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(20, 30, 0, 0, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 19, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 20, 0, 0, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 19, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 1, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 20, 0, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 0, 19, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 0, 20, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(0, 30, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 0, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));

    /* upper limits: the travel */
    c = rt_cfg(30, 340, 0, 0, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 341, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(341, 30, 0, 0, 1);
    CHECK(!km_engine_cfg_valid(&c));
    /* a press has to be reachable: rt_down <= 340 - bottom_dead */
    c = rt_cfg(300, 30, 0, 40, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(301, 30, 0, 40, 1);
    CHECK(!km_engine_cfg_valid(&c));
    /* the zones leave at least the floor between them: top(eff) + bottom + 20 <= 340 */
    c = rt_cfg(30, 30, 200, 120, 1);
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 200, 121, 1);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 0, 300, 1);          /* top 0 counts as 20 */
    CHECK(km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 0, 301, 1);
    CHECK(!km_engine_cfg_valid(&c));

    /* continuous: 0 or 1 only (erased flash reads 0xFF) */
    c = rt_cfg(30, 30, 0, 0, 2);
    CHECK(!km_engine_cfg_valid(&c));
    c = rt_cfg(30, 30, 0, 0, 255);
    CHECK(!km_engine_cfg_valid(&c));
    /* without continuous the actuation point (press_mm100) is used: it must be sane; with it, it is ignored */
    c = rt_cfg(30, 30, 0, 0, 0);
    CHECK(km_engine_cfg_valid(&c));
    c.press_mm100 = 19;
    CHECK(!km_engine_cfg_valid(&c));
    c.press_mm100 = 20;
    CHECK(km_engine_cfg_valid(&c));
    c.press_mm100 = 340;
    CHECK(km_engine_cfg_valid(&c));
    c.press_mm100 = 341;
    CHECK(!km_engine_cfg_valid(&c));
    c.continuous = 1;
    CHECK(km_engine_cfg_valid(&c));
    c.press_mm100 = 0xFFFF;                 /* garbage in an unused field of a continuous config */
    CHECK(km_engine_cfg_valid(&c));

    /* unknown modes */
    c = rt_cfg(30, 30, 0, 0, 1);
    c.mode = 2;
    CHECK(!km_engine_cfg_valid(&c));
    c.mode = 255;
    CHECK(!km_engine_cfg_valid(&c));

    /* FIXED does not look at the rapid-trigger fields (all zero, all 0xFFFF) */
    km_engine_key_cfg_default(&c);
    c.rt_down_mm100 = 0;
    c.rt_up_mm100 = 0;
    c.top_dead_mm100 = 0xFFFF;
    c.bottom_dead_mm100 = 0xFFFF;
    c.continuous = 77;
    CHECK(km_engine_cfg_valid(&c));
    /* ... and RAPID does not look at the FIXED ones when continuous */
    c = rt_cfg(30, 30, 0, 0, 1);
    c.press_mm100 = 0;
    c.release_mm100 = 500;
    CHECK(km_engine_cfg_valid(&c));

    /* the default is FIXED with sane rapid values, so switching the mode alone gives a valid config */
    km_engine_key_cfg_default(&c);
    CHECK_EQ(c.mode, KM_ENGINE_TRIG_FIXED);
    c.mode = KM_ENGINE_TRIG_RAPID;
    CHECK(km_engine_cfg_valid(&c));
    CHECK_EQ(c.rt_down_mm100, KM_ENGINE_DEFAULT_RT_MM100);
    CHECK_EQ(c.rt_up_mm100, KM_ENGINE_DEFAULT_RT_MM100);

    /* set / get / apply */
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 200, NOISE);
    km_engine_key_cfg_t g;
    c = rt_cfg(25, 35, 40, 50, 1);
    CHECK(km_engine_set_key_cfg(&r->e, 18, &c));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &g));
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_RAPID);
    CHECK_EQ(g.rt_down_mm100, 25);
    CHECK_EQ(g.rt_up_mm100, 35);
    CHECK_EQ(g.top_dead_mm100, 40);
    CHECK_EQ(g.bottom_dead_mm100, 50);
    CHECK_EQ(g.continuous, 1);
    c.rt_down_mm100 = 10;                                   /* below the floor: refused, nothing changed */
    CHECK(!km_engine_set_key_cfg(&r->e, 18, &c));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &g));
    CHECK_EQ(g.rt_down_mm100, 25);
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_RAPID);
    /* apply: an invalid config means the FIXED defaults for that key, and says so */
    CHECK(!km_engine_apply_key_cfg(&r->e, 18, &c));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &g));
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_FIXED);
    CHECK_EQ(g.press_mm100, KM_ENGINE_DEFAULT_PRESS_MM100);
    CHECK_EQ(g.release_mm100, KM_ENGINE_DEFAULT_RELEASE_MM100);
    c = rt_cfg(25, 35, 40, 50, 1);
    CHECK(km_engine_apply_key_cfg(&r->e, 18, &c));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &g));
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_RAPID);
    CHECK(!km_engine_apply_key_cfg(&r->e, 18, NULL));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &g));
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_FIXED);
    CHECK(!km_engine_apply_key_cfg(&r->e, 76, &c));         /* slot out of range */
    CHECK(!km_engine_set_key_cfg(&r->e, 76, &c));
    free(r);
}

/* ---------------------------------------------------------------- the rules, noise-free */

static void test_rapid_rules_exact(void)
{
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    traj_t t = { r, s, 0 };

    /* continuous, 0.30 / 0.30, minimum top zone (0.20), no bottom zone */
    km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 1);
    rt_setup(r, 210, 0, s, &c);
    tj_go(&t, 200, 4000);
    CHECK_EQ(r->press_ev[s], 1);                                /* first press: rt_down from the top (30) */
    CHECK(IN(r->press_depth[s], 29, 34));
    CHECK_EQ(r->release_ev[s], 0);
    tj_go(&t, 120, 1600);                                       /* up from the deepest point (200): release at 170 */
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 165, 173));
    tj_go(&t, 250, 2600);                                       /* down from the shallowest point (120): press at 150 */
    CHECK_EQ(r->press_ev[s], 2);
    CHECK(IN(r->press_depth[s], 147, 155));
    tj_go(&t, 300, 1000);
    tj_go(&t, 285, 300);                                        /* a 15 reversal is below rt_up: nothing */
    tj_go(&t, 300, 300);
    CHECK_EQ(r->release_ev[s], 1);
    tj_go(&t, 255, 900);                                        /* up from 300: release at 270 */
    CHECK_EQ(r->release_ev[s], 2);
    CHECK(IN(r->release_depth[s], 265, 273));
    tj_go(&t, 340, 1700);                                       /* down from 255: press at 285 */
    CHECK_EQ(r->press_ev[s], 3);
    CHECK(IN(r->press_depth[s], 282, 290));
    rig_run(r, 300);
    tj_go(&t, 0, 7000);                                         /* back up: release at 310, then nothing more */
    CHECK_EQ(r->release_ev[s], 3);
    CHECK(IN(r->release_depth[s], 305, 313));
    rig_run(r, 300);
    CHECK_EQ(r->press_ev[s], 3);
    CHECK_EQ(r->release_ev[s], 3);
    CHECK(!km_engine_slot_down(&r->e, s));

    /* the distances are independent: down 0.50, up 0.25 */
    c = rt_cfg(50, 25, 0, 0, 1);
    rt_setup(r, 211, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 200, 4000);
    CHECK(IN(r->press_depth[s], 49, 54));
    tj_go(&t, 100, 2000);
    CHECK(IN(r->release_depth[s], 172, 177));                   /* 200 - 25 */
    tj_go(&t, 160, 1200);                                       /* down from the shallowest point (100): press at 150 */
    CHECK_EQ(r->press_ev[s], 2);
    CHECK(IN(r->press_depth[s], 149, 154));

    /* a press right after the key left the top zone, then straight back: released 0.10 inside the zone edge (20 - 10) */
    c = rt_cfg(30, 30, 0, 0, 1);
    rt_setup(r, 212, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 40, 800);
    CHECK_EQ(r->press_ev[s], 1);
    tj_go(&t, 0, 800);
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 7, 11));
    free(r);
}

/* ---------------------------------------------------------------- continuous / non-continuous */

static void test_rapid_continuous(void)
{
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    traj_t t = { r, s, 0 };

    /* the same path with both modes: down to 200, up to 40 (released), down again. The actuation point is 1.00 mm */
    for (unsigned cont = 0; cont <= 1; cont++) {
        km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, cont);
        c.press_mm100 = 100;
        rt_setup(r, 220 + cont, 0, s, &c);
        t.depth = 0;
        tj_go(&t, 200, 4000);
        CHECK_EQ(r->press_ev[s], 1);
        CHECK(IN(r->press_depth[s], cont ? 29 : 99, cont ? 34 : 105));     /* first press: rt_down / actuation point */
        tj_go(&t, 40, 3200);
        CHECK_EQ(r->release_ev[s], 1);
        CHECK(IN(r->release_depth[s], 165, 173));
        tj_go(&t, 140, 2000);
        CHECK_EQ(r->press_ev[s], 2);
        /* continuous: 30 below the shallowest point (40) -> 70; not continuous: not before the actuation point */
        CHECK(IN(r->press_depth[s], cont ? 69 : 99, cont ? 75 : 105));
        /* back to the top: both reset in the top zone */
        tj_go(&t, 0, 3000);
        CHECK_EQ(r->release_ev[s], 2);
        CHECK(!km_engine_slot_down(&r->e, s));
    }

    /* not continuous: a reversal above the actuation point never presses, below it does */
    km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 0);
    c.press_mm100 = 100;
    rt_setup(r, 224, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 90, 1800);
    tj_go(&t, 40, 1000);
    tj_go(&t, 95, 1100);
    tj_go(&t, 60, 700);
    tj_go(&t, 99, 800);
    CHECK_EQ(total_events(r), 0);
    tj_go(&t, 120, 400);
    CHECK_EQ(r->press_ev[s], 1);
    free(r);
}

/* ---------------------------------------------------------------- dead zones */

static void test_rapid_dead_zones(void)
{
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    traj_t t = { r, s, 0 };

    /* top zone 1.00 mm: shallow wiggling, even with big reversals, never presses */
    km_engine_key_cfg_t c = rt_cfg(20, 20, 100, 0, 1);
    rt_setup(r, 230, NOISE, s, &c);
    for (unsigned i = 0; i < 20; i++) {
        tj_go(&t, 92, 300);
        tj_go(&t, 40, 300);
        tj_go(&t, 97, 300);
        tj_go(&t, 0, 300);
    }
    tj_go(&t, 98, 300);
    CHECK_EQ(total_events(r), 0);
    tj_go(&t, 140, 300);                                        /* beyond the zone, and 20+ up from the shallowest */
    rig_run(r, 100);
    CHECK_EQ(r->press_ev[s], 1);
    CHECK(IN(r->press_depth[s], 100, 112));

    /* released by falling into the zone: rt_up 2.00 is far away, the zone edge (100 - 0.10 hysteresis) comes first */
    c = rt_cfg(30, 200, 100, 0, 1);
    rt_setup(r, 231, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 160, 3000);
    CHECK_EQ(r->press_ev[s], 1);
    tj_go(&t, 0, 3200);
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 86, 91));

    /* after that release the key has to come down rt_down (0.40) from its shallowest point again */
    c = rt_cfg(40, 200, 100, 0, 1);
    rt_setup(r, 238, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 160, 3000);
    CHECK_EQ(r->press_ev[s], 1);
    tj_go(&t, 80, 1600);                                        /* released at 90, shallowest point 80 */
    CHECK_EQ(r->release_ev[s], 1);
    tj_go(&t, 112, 700);                                        /* beyond the zone, but only 32 below the shallowest */
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[s], 1);
    tj_go(&t, 130, 400);                                        /* 50 below it: pressed (at 120) */
    CHECK_EQ(r->press_ev[s], 2);
    CHECK(IN(r->press_depth[s], 119, 126));

    /* not hysteresis-free at the edge: a finger resting on the zone edge presses at most once */
    c = rt_cfg(20, 20, 100, 0, 1);
    rt_setup(r, 232, NOISE, s, &c);
    t.depth = 0;
    tj_go(&t, 100, 2000);
    rig_run(r, 100000);
    CHECK(r->press_ev[s] <= 1);
    CHECK(r->release_ev[s] <= 1);

    /* top zone 0 means the minimum (20): a finger resting at 0.10 does not press */
    c = rt_cfg(20, 20, 0, 0, 1);
    rt_setup(r, 233, NOISE, s, &c);
    t.depth = 0;
    tj_go(&t, 10, 500);
    rig_run(r, 30000);
    CHECK_EQ(total_events(r), 0);

    /* bottom zone 0.60: the last 0.60 counts as fully pressed; jitter inside it does not release */
    c = rt_cfg(30, 30, 0, 60, 1);
    rt_setup(r, 234, NOISE, s, &c);
    t.depth = 0;
    tj_go(&t, 340, 3000);
    CHECK_EQ(r->press_ev[s], 1);
    for (unsigned i = 0; i < 6000; i++) {                       /* triangle wave 290 .. 340, in the zone (starts at 280) */
        const int phase = (int)(i % 200u);
        r->level[s] = level_at(s, (uint16_t)(290 + (phase < 100 ? phase : 200 - phase) / 2));
        rig_step(r);
    }
    CHECK_EQ(r->press_ev[s], 1);
    CHECK_EQ(r->release_ev[s], 0);
    t.depth = 315;
    tj_go(&t, 200, 1800);                                       /* leaves the zone at 280: released there */
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 272, 281));
    tj_go(&t, 0, 2000);

    /* the same jitter without a bottom zone: it is a real reversal (60 > rt_up) and releases */
    c = rt_cfg(30, 30, 0, 0, 1);
    rt_setup(r, 235, NOISE, s, &c);
    t.depth = 0;
    tj_go(&t, 340, 3000);
    for (unsigned i = 0; i < 4; i++) {
        tj_go(&t, 280, 300);
        tj_go(&t, 340, 300);
    }
    CHECK_EQ(r->release_ev[s], 4);
    CHECK_EQ(r->press_ev[s], 5);

    /* rt_up larger than the zone: rt_up decides (340 - 90 = 250); zone larger than rt_up: the zone decides (240) */
    c = rt_cfg(30, 90, 0, 40, 1);
    rt_setup(r, 236, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 340, 3000);
    tj_go(&t, 0, 3400);
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 242, 251));
    c = rt_cfg(30, 30, 0, 100, 1);
    rt_setup(r, 237, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 340, 3000);
    tj_go(&t, 0, 3400);
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(IN(r->release_depth[s], 232, 241));
    free(r);
}

/* ---------------------------------------------------------------- noise, holds, speed */

static void test_rapid_noise_and_holds(void)
{
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    traj_t t = { r, s, 0 };

    /* every key rapid at the floor values, idle for 10 s with +-15 counts of noise: nothing happens */
    rig_boot(r, 240, NOISE);
    const km_engine_key_cfg_t floor = rt_cfg(KM_ENGINE_RT_MIN_MM100, KM_ENGINE_RT_MIN_MM100, 0, 0, 1);
    for (unsigned k = 0; k < KM_ENGINE_SLOTS; k++) {
        if (data()->is_key[k]) {
            CHECK(km_engine_set_key_cfg(&r->e, k, &floor));
        }
    }
    rig_run(r, 230000);
    CHECK_EQ(total_events(r), 0);

    /* a key held at various depths for 8 s, floor values: pressed once, never released by noise. The top is the
       worst place: the model is steepest there (about 1 depth unit per count) */
    static const int hold[] = { 40, 60, 100, 200, 330 };
    for (unsigned i = 0; i < sizeof hold / sizeof hold[0]; i++) {
        rt_setup(r, 241 + i, NOISE, s, &floor);
        t.depth = 0;
        tj_go(&t, hold[i], 1500);
        CHECK_EQ(r->press_ev[s], 1);
        rig_run(r, 184000);
        CHECK_EQ(r->press_ev[s], 1);
        CHECK_EQ(r->release_ev[s], 0);
        CHECK(km_engine_slot_down(&r->e, s));
        tj_go(&t, 0, 1500);
        rig_run(r, 100);
        CHECK_EQ(r->release_ev[s], 1);
    }

    /* held up, 0.10 below the top (inside the zone): no presses */
    rt_setup(r, 250, NOISE, s, &floor);
    t.depth = 0;
    tj_go(&t, 10, 500);
    rig_run(r, 120000);
    CHECK_EQ(total_events(r), 0);

    /* reversals of several sizes and speeds, noisy, at mid travel and near the top. Sizes below the floor never
       trigger anything, sizes from 40 up always release and press again. Speeds in depth per frame: 4.5 (a hard
       tap), 0.8, 0.1 */
    static const unsigned frames_per_100[] = { 22, 125, 1000 };
    static const int sizes[] = { 10, 19, 40, 60, 100 };
    static const int start[] = { 60, 220 };
    for (unsigned sp = 0; sp < 3; sp++) {
        for (unsigned z = 0; z < 5; z++) {
            for (unsigned st = 0; st < 2; st++) {
                const km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 1);
                rt_setup(r, 260 + sp * 10 + z, NOISE, s, &c);
                t.depth = 0;
                const unsigned f100 = frames_per_100[sp];
                tj_go(&t, start[st] + 40, (unsigned)(start[st] + 40) * f100 / 100u);
                rig_run(r, 400);
                CHECK_EQ(r->press_ev[s], 1);
                const unsigned ff = (unsigned)sizes[z] * f100 / 100u;
                tj_go(&t, start[st] + 40 - sizes[z], ff);
                rig_run(r, 100);
                tj_go(&t, start[st] + 40, ff);
                rig_run(r, 100);
                if (sizes[z] < 20) {
                    CHECK_EQ(r->release_ev[s], 0);
                    CHECK_EQ(r->press_ev[s], 1);
                } else {
                    CHECK_EQ(r->release_ev[s], 1);
                    CHECK_EQ(r->press_ev[s], 2);
                }
            }
        }
    }
    free(r);
}

static void test_rapid_exact_latency(void)
{
    const data_t *d = data();
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);

    /* No noise. From rest straight to the bottom in one frame: the first filter step already puts both filters
       beyond the press line with more than rt_down, so the key is down after exactly KM_ENGINE_PRESS_FRAMES. */
    km_engine_key_cfg_t c = rt_cfg(20, 20, 0, 0, 1);
    rt_setup(r, 320, 0, s, &c);
    r->level[s] = d->bottom[s];
    unsigned n = 0;
    while (!km_engine_slot_down(&r->e, s) && n < 100) {
        rig_step(r);
        n++;
    }
    CHECK_EQ(n, KM_ENGINE_PRESS_FRAMES);

    /* The slow stage starts from the rest reading, not from wherever it was left: the same jump to 0.40 takes the
       same number of frames on a fresh key and after a full stroke and a long rest. */
    c = rt_cfg(20, 20, 0, 0, 1);
    unsigned fresh = 0, again = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        rt_setup(r, 324, 0, s, &c);
        if (pass == 1) {
            r->level[s] = d->bottom[s];
            rig_run(r, 100);
            r->level[s] = d->released[s];
            rig_run(r, 1000);
            CHECK(!km_engine_slot_down(&r->e, s));
        }
        r->level[s] = level_at(s, 40);
        n = 0;
        while (!km_engine_slot_down(&r->e, s) && n < 400) {
            rig_step(r);
            n++;
        }
        if (pass == 0) {
            fresh = n;
        } else {
            again = n;
        }
    }
    CHECK(fresh > KM_ENGINE_PRESS_FRAMES && fresh < 100);
    CHECK_EQ(again, fresh);

    /* Release by the top zone: the key rests at depth 95 (zone 100, so the release line is 90) and jumps to rest.
       The slow filter crosses the line on the second frame, the key is up RELEASE_FRAMES later. */
    c = rt_cfg(20, 340, 100, 0, 1);
    rt_setup(r, 321, 0, s, &c);
    traj_t t = { r, s, 0 };
    tj_go(&t, 160, 1600);
    tj_go(&t, 95, 1600);
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, s));
    r->level[s] = d->released[s];
    n = 0;
    while (km_engine_slot_down(&r->e, s) && n < 100) {
        rig_step(r);
        n++;
    }
    CHECK_EQ(n, KM_ENGINE_RELEASE_FRAMES + 1u);

    /* The slow filter follows the fast one at once when the key is back near the rest position: a key pressed
       0.60 deep that jumps to rest is up after 15 frames (the fast filter is in the rest region from the 9th). */
    c = rt_cfg(20, 340, 0, 0, 1);
    rt_setup(r, 322, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 60, 1000);
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, s));
    r->level[s] = d->released[s];
    n = 0;
    while (km_engine_slot_down(&r->e, s) && n < 100) {
        rig_step(r);
        n++;
    }
    CHECK_EQ(n, 15);

    /* Release by reversal: held at 1.50 (rt_up 0.20), the key jumps to 1.00. The slow filter is 0.20 above its
       deepest point on the 8th frame; the key is up RELEASE_FRAMES - 1 frames after that. */
    c = rt_cfg(20, 20, 0, 0, 1);
    rt_setup(r, 323, 0, s, &c);
    t.depth = 0;
    tj_go(&t, 150, 1500);
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, s));
    r->level[s] = level_at(s, 100);
    n = 0;
    while (km_engine_slot_down(&r->e, s) && n < 100) {
        rig_step(r);
        n++;
    }
    CHECK_EQ(n, 8 + KM_ENGINE_RELEASE_FRAMES - 1u);
    free(r);
}

static void test_rapid_fast_and_spikes(void)
{
    const data_t *d = data();
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    const km_engine_key_cfg_t c = rt_cfg(30, 30, 40, 50, 1);
    rt_setup(r, 270, NOISE, s, &c);

    /* straight from rest to the bottom in one frame, held, straight back: one press, one release, a few frames late */
    r->level[s] = d->bottom[s];
    unsigned lat = 0;
    while (!km_engine_slot_down(&r->e, s) && lat < 200) {
        rig_step(r);
        lat++;
    }
    CHECK(lat >= KM_ENGINE_PRESS_FRAMES && lat <= 40);
    rig_run(r, 300);
    CHECK_EQ(r->press_ev[s], 1);
    r->level[s] = d->released[s];
    lat = 0;
    while (km_engine_slot_down(&r->e, s) && lat < 200) {
        rig_step(r);
        lat++;
    }
    CHECK(lat >= KM_ENGINE_RELEASE_FRAMES && lat <= 40);
    rig_run(r, 300);
    CHECK_EQ(r->press_ev[s], 1);
    CHECK_EQ(r->release_ev[s], 1);

    /* one- and two-frame spikes to the bottom are not keypresses */
    for (unsigned len = 1; len <= 2; len++) {
        for (unsigned rep = 0; rep < 20; rep++) {
            r->level[s] = d->bottom[s];
            rig_run(r, len);
            r->level[s] = d->released[s];
            rig_run(r, 100);
        }
    }
    CHECK_EQ(r->press_ev[s], 1);
    /* a one-frame dip of a pressed key to the top does not release it */
    r->level[s] = d->bottom[s];
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[s], 2);
    for (unsigned rep = 0; rep < 20; rep++) {
        r->level[s] = d->released[s];
        rig_run(r, 2);
        r->level[s] = d->bottom[s];
        rig_run(r, 50);
    }
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->release_ev[s], 1);
    r->level[s] = d->released[s];
    rig_run(r, 100);
    CHECK_EQ(r->release_ev[s], 2);

    /* the same from the other side: a pressed key at mid travel (no bottom zone) and a one- or two-frame jump of the
       reading far above the rest level (a glitch): not released */
    {
        const km_engine_key_cfg_t mid = rt_cfg(30, 30, 0, 0, 1);
        rt_setup(r, 272, NOISE, s, &mid);
        traj_t tm = { r, s, 0 };
        tj_go(&tm, 200, 3000);
        rig_run(r, 300);
        CHECK_EQ(r->press_ev[s], 1);
        for (unsigned len = 1; len <= 2; len++) {
            for (unsigned rep = 0; rep < 20; rep++) {
                r->level[s] = 4095;
                rig_run(r, len);
                r->level[s] = level_at(s, 200);
                rig_run(r, 100);
            }
        }
        CHECK_EQ(r->press_ev[s], 1);
        CHECK_EQ(r->release_ev[s], 0);
    }

    /* a slow press under noise (40 noise sequences): each press is close to where the rules put it, and on
       average slightly early (the line is 0.21; noise lets the slow filter cross a little before). A direction
       guard that is too strict delays the press and moves the mean to 0.20 and beyond. */
    {
        unsigned sum = 0;
        for (unsigned seed = 0; seed < 40; seed++) {
            const km_engine_key_cfg_t slow = rt_cfg(20, 20, 0, 0, 1);
            rt_setup(r, 500 + seed, NOISE, s, &slow);
            traj_t ts = { r, s, 0 };
            tj_go(&ts, 100, 10000);
            CHECK_EQ(r->press_ev[s], 1);
            CHECK(IN(r->press_depth[s], 14, 25));
            sum += r->press_depth[s];
        }
        CHECK(sum < 784u);                              /* mean below 19.6 */
    }

    /* a fast tap: 3.4 mm down in 25 frames (about 1 ms), held 20 ms, back in 25 frames */
    rt_setup(r, 271, NOISE, s, &c);
    traj_t t = { r, s, 0 };
    for (unsigned rep = 1; rep <= 10; rep++) {
        tj_go(&t, 340, 25);
        rig_run(r, 460);
        tj_go(&t, 0, 25);
        rig_run(r, 100);
        CHECK_EQ(r->press_ev[s], rep);
        CHECK_EQ(r->release_ev[s], rep);
    }
    free(r);
}

/* ---------------------------------------------------------------- the whole keyboard, real endpoints */

static void test_rapid_real_endpoints(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 280, NOISE);
    const km_engine_key_cfg_t c = rt_cfg(30, 30, 40, 50, 1);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            CHECK(km_engine_set_bottom(&r->e, s, d->bottom[s]));
            CHECK(km_engine_set_key_cfg(&r->e, s, &c));
        }
    }
    rig_run(r, 200);

    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            continue;
        }
        const unsigned before = total_events(r);
        traj_t t = { r, s, 0 };
        /* full stroke at about 0.5 depth units per frame */
        tj_go(&t, 340, 700);
        rig_run(r, 100);
        CHECK(km_engine_slot_down(&r->e, s));
        CHECK_EQ(r->press_ev[s], 1);
        CHECK(IN(r->press_depth[s], 41, 58));                   /* max(top zone 40 + 1, rt_down 30) */
        tj_go(&t, 0, 700);
        rig_run(r, 100);
        CHECK(!km_engine_slot_down(&r->e, s));
        CHECK_EQ(r->release_ev[s], 1);
        CHECK(IN(r->release_depth[s], 262, 292));               /* leaves the bottom zone (starts at 290) */
        /* a stroke with a reversal: down to 250, up to 190 (release at 220), down to 240 (press at 220), up */
        tj_go(&t, 250, 500);
        tj_go(&t, 190, 120);
        CHECK_EQ(r->release_ev[s], 2);
        CHECK(IN(r->release_depth[s], 205, 222));
        tj_go(&t, 240, 100);
        CHECK_EQ(r->press_ev[s], 3);
        CHECK(IN(r->press_depth[s], 218, 235));
        tj_go(&t, 0, 500);
        rig_run(r, 100);
        CHECK(!km_engine_slot_down(&r->e, s));
        CHECK_EQ(r->release_ev[s], 3);
        CHECK_EQ(total_events(r), before + 6u);                 /* only this key did anything */
    }
    CHECK_EQ(km_engine_glitch_count(&r->e), 0);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            CHECK_EQ(r->press_ev[s], 0);
        }
    }

    /* unknown bottom: the first stroke teaches it; the key is never left pressed, and the second stroke is right */
    rig_boot(r, 281, NOISE);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            CHECK(km_engine_set_key_cfg(&r->e, s, &c));
        }
    }
    rig_run(r, 200);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            continue;
        }
        traj_t t = { r, s, 0 };
        for (unsigned pass = 1; pass <= 2; pass++) {
            tj_go(&t, 340, 700);
            rig_run(r, 100);
            CHECK(km_engine_slot_down(&r->e, s));
            tj_go(&t, 0, 700);
            rig_run(r, 100);
            CHECK(!km_engine_slot_down(&r->e, s));
            CHECK_EQ(r->press_ev[s], pass);
            CHECK_EQ(r->release_ev[s], pass);
            if (pass == 2) {
                CHECK(IN(r->press_depth[s], 41, 58));
            }
        }
    }
    free(r);
}

/* ---------------------------------------------------------------- baseline drift */

static void test_rapid_drift(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 290, NOISE);
    const km_engine_key_cfg_t c = rt_cfg(20, 20, 0, 0, 1);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            CHECK(km_engine_set_key_cfg(&r->e, s, &c));
        }
    }
    /* warming by 60 counts over ~11 s: the baseline follows, nothing is triggered */
    for (unsigned i = 1; i <= 60; i++) {
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            r->level[s] = (uint16_t)(d->idle[s] + i);
        }
        rig_run(r, 4000);
    }
    rig_run(r, 100000);
    CHECK_EQ(total_events(r), 0);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            const int diff = (int)km_engine_baseline(&r->e, s) - ((int)d->released[s] + 60);
            CHECK(diff >= -3 && diff <= 3);
        }
    }
    /* cooling by 40 counts */
    for (unsigned i = 1; i <= 100; i++) {
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            r->level[s] = (uint16_t)(d->idle[s] + 60 - (int)i);
        }
        rig_run(r, 4000);
    }
    rig_run(r, 100000);
    CHECK_EQ(total_events(r), 0);
    /* and a press from the drifted rest still works at the right depth (reading against the shifted rest level) */
    {
        const unsigned s = 18;
        const int shift = -40;
        const km_depth_cal_t cal = { (uint16_t)(d->released[s] + shift), d->bottom[s] };
        CHECK(km_engine_set_bottom(&r->e, s, d->bottom[s]));
        rig_run(r, 200);
        int pressed = 0;
        for (int i = 1; i <= 1600 && !pressed; i++) {
            r->level[s] = (uint16_t)(cal.released + ((int)cal.bottom - (int)cal.released) * i / 1600);
            rig_step(r);
            if (km_engine_slot_down(&r->e, s)) {
                const uint16_t depth = km_depth_model_mm100(&cal, r->level[s]);
                CHECK(IN(depth, 28, 50));
                pressed = 1;
            }
        }
        CHECK(pressed);
    }
    free(r);
}

/* ---------------------------------------------------------------- protections and safe states */

static void test_rapid_protections(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    const km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 1);
    unsigned keys[12], nk = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && nk < 12; s++) {
        if (d->is_key[s]) {
            keys[nk++] = s;
        }
    }

    /* glitch: 8 rapid keys onset in one frame -> none accepted, and no events while the glitch lasts or after */
    rig_boot(r, 300, 0);
    for (unsigned i = 0; i < nk; i++) {
        CHECK(km_engine_set_bottom(&r->e, keys[i], d->bottom[keys[i]]));
        CHECK(km_engine_set_key_cfg(&r->e, keys[i], &c));
    }
    rig_run(r, 200);
    for (unsigned i = 0; i < 8; i++) {
        r->level[keys[i]] = d->bottom[keys[i]];
    }
    rig_run(r, 200);
    CHECK(km_engine_glitch_count(&r->e) > 0);
    CHECK_EQ(total_events(r), 0);
    for (unsigned i = 0; i < 8; i++) {
        r->level[keys[i]] = d->released[keys[i]];
    }
    rig_run(r, 200);
    CHECK_EQ(total_events(r), 0);
    /* seven in one frame are fingers (accepted) */
    for (unsigned i = 0; i < 7; i++) {
        r->level[keys[i]] = d->bottom[keys[i]];
    }
    rig_run(r, 200);
    unsigned n;
    km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 7);
    for (unsigned i = 0; i < 7; i++) {
        r->level[keys[i]] = d->released[keys[i]];
    }
    rig_run(r, 200);
    km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 0);

    /* a rapid key that floats (fault level) while pressed is forced up, and does not press on its way back */
    rig_boot(r, 301, NOISE);
    CHECK(km_engine_set_bottom(&r->e, keys[0], d->bottom[keys[0]]));
    CHECK(km_engine_set_key_cfg(&r->e, keys[0], &c));
    rig_run(r, 200);
    r->level[keys[0]] = d->bottom[keys[0]];
    rig_run(r, 200);
    CHECK(km_engine_slot_down(&r->e, keys[0]));
    r->level[keys[0]] = 1360;
    rig_run(r, 100);
    CHECK(!km_engine_slot_down(&r->e, keys[0]));
    CHECK_EQ(km_engine_slot_status(&r->e, keys[0]), KM_ENGINE_SLOT_FAULT);
    const unsigned presses = r->press_ev[keys[0]];
    for (int v = 1360; v <= (int)d->released[keys[0]]; v += 2) {   /* the slow way back through the whole range */
        r->level[keys[0]] = (uint16_t)v;
        rig_step(r);
    }
    rig_run(r, 300);
    CHECK_EQ(r->press_ev[keys[0]], presses);
    CHECK_EQ(km_engine_slot_status(&r->e, keys[0]), KM_ENGINE_SLOT_OK);
    CHECK(!km_engine_slot_down(&r->e, keys[0]));

    /* disabling a pressed rapid key forces it up at once; enabling it again starts clean */
    r->level[keys[1]] = d->bottom[keys[1]];
    CHECK(km_engine_set_key_cfg(&r->e, keys[1], &c));
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, keys[1]));
    CHECK(km_engine_set_slot_enabled(&r->e, keys[1], false));
    CHECK(!km_engine_slot_down(&r->e, keys[1]));
    rig_run(r, 300);
    CHECK(!km_engine_slot_down(&r->e, keys[1]));
    r->level[keys[1]] = d->released[keys[1]];
    CHECK(km_engine_set_slot_enabled(&r->e, keys[1], true));
    rig_run(r, 300);
    CHECK(!km_engine_slot_down(&r->e, keys[1]));
    r->level[keys[1]] = d->bottom[keys[1]];
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, keys[1]));
    r->level[keys[1]] = d->released[keys[1]];
    rig_run(r, 300);
    CHECK(!km_engine_slot_down(&r->e, keys[1]));

    /* changing the mode while the key is down never leaves it stuck: FIXED -> RAPID -> FIXED, key released after */
    rig_boot(r, 302, NOISE);
    CHECK(km_engine_set_bottom(&r->e, keys[2], d->bottom[keys[2]]));
    rig_run(r, 200);
    for (unsigned pass = 0; pass < 4; pass++) {
        km_engine_key_cfg_t cc;
        km_engine_key_cfg_default(&cc);
        if (pass & 1u) {
            cc = rt_cfg(30, 30, 0, 0, pass >> 1);
        }
        r->level[keys[2]] = d->bottom[keys[2]];
        rig_run(r, 200);
        CHECK(km_engine_slot_down(&r->e, keys[2]));
        CHECK(km_engine_set_key_cfg(&r->e, keys[2], &cc));
        rig_run(r, 200);
        CHECK(km_engine_slot_down(&r->e, keys[2]));       /* still held, whatever the mode */
        r->level[keys[2]] = d->released[keys[2]];
        rig_run(r, 300);
        CHECK(!km_engine_slot_down(&r->e, keys[2]));
    }

    /* switching to RAPID while the key is held mid-travel does not use what an earlier stroke left behind: held
       at the bottom (deepest point 3.40), then in FIXED mode brought up to 1.50, then back to RAPID */
    rig_boot(r, 303, NOISE);
    CHECK(km_engine_set_bottom(&r->e, keys[4], d->bottom[keys[4]]));
    CHECK(km_engine_set_key_cfg(&r->e, keys[4], &c));
    rig_run(r, 200);
    traj_t tj = { r, keys[4], 0 };
    tj_go(&tj, 340, 600);
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, keys[4]));
    km_engine_key_cfg_t fx;
    km_engine_key_cfg_default(&fx);
    CHECK(km_engine_set_key_cfg(&r->e, keys[4], &fx));
    tj_go(&tj, 150, 600);
    rig_run(r, 300);
    CHECK(km_engine_slot_down(&r->e, keys[4]));
    CHECK(km_engine_set_key_cfg(&r->e, keys[4], &c));
    rig_run(r, 1000);
    CHECK(km_engine_slot_down(&r->e, keys[4]));
    CHECK_EQ(r->release_ev[keys[4]], 0);

    /* glitch, then only one of the keys stays deep: it qualifies again like a FIXED key would */
    rig_boot(r, 304, 0);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(km_engine_set_bottom(&r->e, keys[i], d->bottom[keys[i]]));
        CHECK(km_engine_set_key_cfg(&r->e, keys[i], &c));
    }
    rig_run(r, 200);
    for (unsigned i = 0; i < 8; i++) {
        r->level[keys[i]] = d->bottom[keys[i]];
    }
    rig_run(r, 100);
    CHECK_EQ(total_events(r), 0);
    for (unsigned i = 1; i < 8; i++) {
        r->level[keys[i]] = d->released[keys[i]];
    }
    rig_run(r, 100);
    CHECK(km_engine_slot_down(&r->e, keys[0]));
    CHECK_EQ(r->press_ev[keys[0]], 1);

    /* a corrupt config in memory is not used: the next threshold recompute puts the FIXED defaults back */
    r->e.slot[keys[3]].cfg.mode = 9;
    r->e.slot[keys[3]].cfg.rt_down_mm100 = 0;
    CHECK(km_engine_set_bottom(&r->e, keys[3], d->bottom[keys[3]]));
    rig_run(r, 300);
    km_engine_key_cfg_t g;
    CHECK(km_engine_get_key_cfg(&r->e, keys[3], &g));
    CHECK_EQ(g.mode, KM_ENGINE_TRIG_FIXED);
    r->level[keys[3]] = d->bottom[keys[3]];
    rig_run(r, 200);
    CHECK(km_engine_slot_down(&r->e, keys[3]));
    r->level[keys[3]] = d->released[keys[3]];
    rig_run(r, 200);
    CHECK(!km_engine_slot_down(&r->e, keys[3]));
    free(r);
}

/* ---------------------------------------------------------------- random walks: never stuck, events consistent */

static uint32_t rt_rnd(uint32_t *seed)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

static void test_rapid_random_walks(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    uint32_t seed = 12345;
    unsigned keys[8], nk = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && nk < 8; s += 5) {
        if (d->is_key[s]) {
            keys[nk++] = s;
        }
    }
    for (unsigned trial = 0; trial < 60; trial++) {
        const unsigned s = keys[trial % nk];
        km_engine_key_cfg_t c;
        km_engine_key_cfg_default(&c);
        if (trial % 4u != 0u) {
            for (unsigned tries = 0; tries < 50; tries++) {
                c = rt_cfg(20 + rt_rnd(&seed) % 120, 20 + rt_rnd(&seed) % 120, rt_rnd(&seed) % 3 ? 20 + rt_rnd(&seed) % 100 : 0,
                           rt_rnd(&seed) % 3 ? 20 + rt_rnd(&seed) % 100 : 0, rt_rnd(&seed) & 1u);
                c.press_mm100 = (uint16_t)(20 + rt_rnd(&seed) % 300);
                if (km_engine_cfg_valid(&c)) {
                    break;
                }
            }
        }
        rig_boot(r, 400 + trial, NOISE);
        if (trial & 1u) {
            CHECK(km_engine_set_bottom(&r->e, s, d->bottom[s]));
        }
        CHECK(km_engine_apply_key_cfg(&r->e, s, &c) || !km_engine_cfg_valid(&c));
        rig_run(r, 200);
        traj_t t = { r, s, 0 };
        for (unsigned seg = 0; seg < 25; seg++) {
            const int to = (int)(rt_rnd(&seed) % 341u);
            const unsigned fr = 2u + rt_rnd(&seed) % 700u;
            tj_go(&t, to, fr);
            const long diff = (long)r->press_ev[s] - (long)r->release_ev[s];
            CHECK(diff == 0 || diff == 1);
            CHECK_EQ(diff, km_engine_slot_down(&r->e, s) ? 1 : 0);
            unsigned n;
            km_engine_down_slots(&r->e, &n);
            CHECK(n <= 1u);
            if (rt_rnd(&seed) % 4u == 0u) {
                rig_run(r, rt_rnd(&seed) % 3000u);
            }
        }
        tj_go(&t, 0, 1000);
        rig_run(r, 400);
        CHECK(!km_engine_slot_down(&r->e, s));
        CHECK_EQ(r->press_ev[s], r->release_ev[s]);
        unsigned n;
        km_engine_down_slots(&r->e, &n);
        CHECK_EQ(n, 0);
    }
    free(r);
}

/* ---------------------------------------------------------------- the stroke test in front of the depth model */

/*
 * The engine calls the depth model only after a stroke of mc_dn / mc_up counts away from the remembered extreme
 * (km_engine_slot_t). That is only allowed to skip frames in which the trigger distance cannot have been reached:
 * for every starting reading the fewest counts to a depth difference of rt must not be below mc, and (so that it
 * pays) not much above it. Checked exhaustively on a sample of the measured keys and on asymmetric distances.
 */
static void test_rapid_stroke_test(void)
{
    static const unsigned pairs[][2] = { { 20, 300 }, { 300, 20 }, { 21, 150 }, { 150, 21 }, { 30, 30 },
                                         { 77, 120 }, { 340, 60 },  { 99, 100 }, { 33, 250 } };
    rig_t *r = (rig_t *)malloc(sizeof *r);
    unsigned n = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!data()->is_key[s] || (n++ % 5u) != 0u) {
            continue;
        }
        for (unsigned p = 0; p < sizeof pairs / sizeof pairs[0]; p++) {
            const km_engine_key_cfg_t c = rt_cfg(pairs[p][0], pairs[p][1], 0, 0, 1);
            rt_setup(r, 400u + s, 0, s, &c);
            const km_engine_slot_t *k = &r->e.slot[s];
            const km_depth_cal_t cal = { k->thr_base, k->bottom };
            for (unsigned which = 0; which < 2; which++) {
                const unsigned rt = pairs[p][which];
                const unsigned mc = which ? k->mc_up : k->mc_dn;
                unsigned fewest = 0xFFFFu;
                for (int c0 = (int)cal.released + 3; c0 >= (int)cal.bottom; c0--) {
                    const unsigned d0 = km_depth_model_mm100(&cal, (uint16_t)c0);
                    for (int cc = c0; cc >= (int)cal.bottom && (unsigned)(c0 - cc) < fewest; cc--) {
                        if (km_depth_model_mm100(&cal, (uint16_t)cc) >= d0 + rt) {
                            fewest = (unsigned)(c0 - cc);
                            break;
                        }
                    }
                }
                CHECK(mc >= 1u);
                CHECK(mc <= fewest);            /* never rules out a stroke that reaches the distance */
                CHECK(mc + 2u + rt / 20u >= fewest); /* and is not much shorter than the fewest counts there are (the flatter the
                                                   curve at depth rt, the more counts one unit of it is worth) */
            }
        }
    }
    free(r);
}

/* ---------------------------------------------------------------- a key that is up and off the top when RAPID starts */

static void test_rapid_start_mid_travel(void)
{
    const unsigned s = 73;
    rig_t *r = (rig_t *)malloc(sizeof *r);
    traj_t t = { r, s, 0 };
    km_engine_key_cfg_t fx;
    km_engine_key_cfg_default(&fx);
    fx.press_mm100 = 300;
    fx.release_mm100 = 100;
    rig_boot(r, 220, 0);
    CHECK(km_engine_set_bottom(&r->e, s, data()->bottom[s]));
    CHECK(km_engine_set_key_cfg(&r->e, s, &fx));
    rig_run(r, 200);
    tj_go(&t, 150, 1500);                                       /* up (FIXED press point 3.00 mm), 1.50 mm deep */
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[s], 0);
    const km_engine_key_cfg_t c = rt_cfg(30, 30, 0, 0, 1);
    CHECK(km_engine_set_key_cfg(&r->e, s, &c));
    rig_run(r, 400);
    CHECK_EQ(r->press_ev[s], 0);                                /* the stroke starts here, not at the top: no travel yet */
    tj_go(&t, 190, 800);                                        /* 0.30 below the starting point */
    CHECK_EQ(r->press_ev[s], 1);
    CHECK(IN(r->press_depth[s], 178, 188));
    free(r);
}

static void test_engine_rapid(void)
{
    test_rapid_config();
    test_rapid_rules_exact();
    test_rapid_stroke_test();
    test_rapid_start_mid_travel();
    test_rapid_continuous();
    test_rapid_dead_zones();
    test_rapid_noise_and_holds();
    test_rapid_exact_latency();
    test_rapid_fast_and_spikes();
    test_rapid_real_endpoints();
    test_rapid_drift();
    test_rapid_protections();
    test_rapid_random_walks();
}
