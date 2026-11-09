/* test_engine_calib.inc - part of test_engine.c: rig helpers, calibration, idle noise, press / release. */

static void rig_step(rig_t *r)
{
    uint16_t f[KM_ENGINE_SLOTS];
    rig_frame(r, f);
    km_engine_update(&r->e, f);
    r->frames++;
    rig_events(r);
}

static void rig_run(rig_t *r, unsigned n)
{
    while (n--) {
        rig_step(r);
    }
}

static void rig_init(rig_t *r, uint32_t seed, int noise)
{
    const data_t *d = data();
    memset(r, 0, sizeof *r);
    km_engine_init(&r->e);
    memcpy(r->level, d->idle, sizeof r->level);
    r->noise = noise;
    r->rng = seed;
}

static km_engine_cal_result_t rig_calibrate(rig_t *r, unsigned frames)
{
    uint16_t f[KM_ENGINE_SLOTS];
    km_engine_calibrate_begin(&r->e);
    for (unsigned i = 0; i < frames; i++) {
        rig_frame(r, f);
        km_engine_calibrate_idle(&r->e, f);
    }
    return km_engine_calibrate_finish(&r->e);
}

/* a keyboard that booted with all keys up */
static void rig_boot(rig_t *r, uint32_t seed, int noise)
{
    rig_init(r, seed, noise);
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);
}

static void rig_ramp(rig_t *r, unsigned slot, uint16_t to, unsigned frames)
{
    const int from = r->level[slot];
    for (unsigned i = 1; i <= frames; i++) {
        r->level[slot] = (uint16_t)(from + ((int)to - from) * (int)i / (int)frames);
        rig_step(r);
    }
}

static unsigned total_events(const rig_t *r)
{
    unsigned n = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        n += r->press_ev[s] + r->release_ev[s];
    }
    return n;
}

/* ---------------------------------------------------------------- engine: calibration */

static void test_engine_calibration(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 1, NOISE);

    /* every key slot armed with a baseline within noise of the truth, every other slot is "no key" */
    unsigned ok = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            CHECK_EQ(km_engine_slot_status(&r->e, s), KM_ENGINE_SLOT_OK);
            const int diff = (int)km_engine_baseline(&r->e, s) - (int)d->released[s];
            CHECK(diff >= -4 && diff <= 4);
            ok++;
        } else {
            CHECK_EQ(km_engine_slot_status(&r->e, s), KM_ENGINE_SLOT_NO_KEY);
        }
    }
    CHECK_EQ(ok, 61);
    CHECK_EQ(km_engine_slot_status(&r->e, 76), KM_ENGINE_SLOT_NO_KEY);

    /* too few frames */
    rig_init(r, 2, NOISE);
    CHECK_EQ(rig_calibrate(r, KM_ENGINE_CAL_MIN_FRAMES - 1), KM_ENGINE_CAL_TOO_FEW_FRAMES);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_UNCALIBRATED);
    CHECK_EQ(rig_calibrate(r, KM_ENGINE_CAL_MIN_FRAMES), KM_ENGINE_CAL_OK); /* exactly the minimum is enough */

    /* not calibrated: update is a no-op even with a key pressed */
    rig_init(r, 3, 0);
    r->level[18] = d->bottom[18];
    rig_run(r, 200);
    CHECK_EQ(total_events(r), 0);
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK_EQ(km_engine_depth(&r->e, 18), 0);

    /* a key slot that is floating (about 1360) at boot is disabled and stays dead; the others work */
    rig_init(r, 4, NOISE);
    r->level[18] = 1360;
    r->level[63] = 4095; /* railed */
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_DISABLED);
    CHECK_EQ(km_engine_slot_status(&r->e, 63), KM_ENGINE_SLOT_DISABLED);
    CHECK_EQ(km_engine_slot_status(&r->e, 73), KM_ENGINE_SLOT_OK);
    r->level[18] = d->bottom[18]; /* the sensor "comes back" deep: still ignored */
    r->level[63] = d->bottom[63];
    r->level[73] = d->bottom[73];
    rig_run(r, 300);
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK(!km_engine_slot_down(&r->e, 63));
    CHECK(km_engine_slot_down(&r->e, 73));
    CHECK_EQ(r->press_ev[18] + r->press_ev[63], 0);

    /* 3 keys held at boot: calibrates, those slots are guessed (ASSUMED) and read as pressed, then
       follow the key up and learn their real baseline */
    rig_init(r, 5, NOISE);
    r->level[18] = d->bottom[18];
    r->level[5] = d->bottom[5];
    r->level[73] = (uint16_t)(d->released[73] - 700); /* held 2 mm in */
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_ASSUMED);
    CHECK_EQ(km_engine_slot_status(&r->e, 5), KM_ENGINE_SLOT_ASSUMED);
    CHECK_EQ(km_engine_slot_status(&r->e, 73), KM_ENGINE_SLOT_ASSUMED);
    CHECK_EQ(km_engine_slot_status(&r->e, 6), KM_ENGINE_SLOT_OK);
    rig_run(r, 100);
    CHECK(km_engine_slot_down(&r->e, 18));
    CHECK(km_engine_slot_down(&r->e, 5));
    CHECK(km_engine_slot_down(&r->e, 73));
    r->level[18] = d->released[18];
    r->level[5] = d->released[5];
    r->level[73] = d->released[73];
    rig_run(r, 300000); /* ~13 s at 23 kHz: well over the 1.4 s drift time constant */
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK(!km_engine_slot_down(&r->e, 5));
    CHECK(!km_engine_slot_down(&r->e, 73));
    CHECK_EQ(r->press_ev[18], 1);
    CHECK_EQ(r->release_ev[18], 1);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_OK);
    static const unsigned was_held[3] = { 18, 5, 73 };
    for (unsigned i = 0; i < 3; i++) {
        const int diff = (int)km_engine_baseline(&r->e, was_held[i]) - (int)d->released[was_held[i]];
        if (diff < -4 || diff > 4) {
            printf("  slot %u baseline diff %d\n", was_held[i], diff);
        }
        CHECK(diff >= -4 && diff <= 4); /* the guess (median) was replaced by the real level */
    }
    /* after that the key presses normally at the right depth */
    rig_ramp(r, 18, d->bottom[18], 300);
    CHECK_EQ(r->press_ev[18], 2);
    CHECK(r->press_depth[18] >= 60 && r->press_depth[18] <= 220);

    /* many keys held (8): refused, engine stays unarmed */
    rig_init(r, 6, NOISE);
    unsigned held = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s] && (s % 8u) == 1u) {
            r->level[s] = d->bottom[s];
            held++;
        }
    }
    CHECK(held > KM_ENGINE_CAL_MAX_BAD);
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_KEYS_HELD);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_UNCALIBRATED);
    rig_run(r, 100);
    CHECK_EQ(total_events(r), 0);
    /* exactly the limit is still accepted, one more is not */
    for (unsigned limit = KM_ENGINE_CAL_MAX_BAD; limit <= KM_ENGINE_CAL_MAX_BAD + 1u; limit++) {
        rig_init(r, 7, NOISE);
        unsigned put = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS && put < limit; s++) {
            if (d->is_key[s]) {
                r->level[s] = d->bottom[s];
                put++;
            }
        }
        CHECK_EQ(rig_calibrate(r, 64), limit == KM_ENGINE_CAL_MAX_BAD ? KM_ENGINE_CAL_OK : KM_ENGINE_CAL_KEYS_HELD);
    }
    /* retry on the same engine once the keys are up */
    memcpy(r->level, d->idle, sizeof r->level);
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);

    /* most keys pressed (median far down) */
    rig_init(r, 9, NOISE);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            r->level[s] = d->bottom[s];
        }
    }
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_KEYS_HELD);

    /* keys moving during the stretch */
    rig_init(r, 10, NOISE);
    {
        uint16_t f[KM_ENGINE_SLOTS];
        km_engine_calibrate_begin(&r->e);
        for (unsigned i = 0; i < 64; i++) {
            unsigned moved = 0;
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (d->is_key[s] && moved < KM_ENGINE_CAL_MAX_BAD + 1u) {
                    r->level[s] = (uint16_t)(d->released[s] - ((i % 2u) ? 150u : 0u)); /* +-150 swing */
                    moved++;
                }
            }
            rig_frame(r, f);
            km_engine_calibrate_idle(&r->e, f);
        }
        CHECK_EQ(km_engine_calibrate_finish(&r->e), KM_ENGINE_CAL_UNSTABLE);
    }

    /* a board where almost every input floats: nothing usable */
    rig_init(r, 11, NOISE);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        r->level[s] = 1360;
    }
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_IMPLAUSIBLE);

    /* begin discards an earlier stretch */
    rig_init(r, 12, 0);
    {
        uint16_t f[KM_ENGINE_SLOTS];
        km_engine_calibrate_begin(&r->e);
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            f[s] = 1000;
        }
        for (unsigned i = 0; i < 30; i++) {
            km_engine_calibrate_idle(&r->e, f);
        }
    }
    CHECK_EQ(rig_calibrate(r, 20), KM_ENGINE_CAL_OK); /* rig_calibrate begins again */
    CHECK_EQ(km_engine_baseline(&r->e, 18), d->released[18]);
    free(r);
}

/* ---------------------------------------------------------------- engine: idle, press, release */

static void test_engine_idle_noise(void)
{
    rig_t *r = (rig_t *)malloc(sizeof *r);
    for (int noise = 15; noise <= 45; noise += 15) {
        rig_boot(r, 100u + (uint32_t)noise, noise);
        rig_run(r, 60000);
        CHECK_EQ(total_events(r), 0);
        CHECK_EQ(km_engine_glitch_count(&r->e), 0);
    }
    /* worst case: the noise sits on the extremes, flipping every frame, every second frame, ... */
    rig_boot(r, 7, 0);
    for (unsigned period = 1; period <= 4; period++) {
        for (unsigned i = 0; i < 5000; i++) {
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                r->level[s] = (uint16_t)(data()->idle[s] + (((i / period) & 1u) ? NOISE : -NOISE));
            }
            rig_step(r);
        }
    }
    CHECK_EQ(total_events(r), 0);
    free(r);
}

static void test_engine_press_release(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    const unsigned s = 18; /* Esc */
    rig_boot(r, 20, NOISE);

    /* one slow press and release: exactly one press and one release */
    rig_ramp(r, s, d->bottom[s], 400);
    CHECK_EQ(r->press_ev[s], 1);
    CHECK_EQ(r->release_ev[s], 0);
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK(r->press_depth[s] >= 60 && r->press_depth[s] <= 220); /* before the bottom is learned */
    rig_run(r, 500);
    CHECK(km_engine_depth(&r->e, s) >= KM_TRAVEL_MM100 - 12u);
    rig_ramp(r, s, d->released[s], 400);
    rig_run(r, 50);
    CHECK_EQ(r->press_ev[s], 1);
    CHECK_EQ(r->release_ev[s], 1);
    CHECK(r->release_depth[s] >= 30 && r->release_depth[s] <= 140);
    CHECK_EQ(total_events(r), 2);

    /* the bottom was learned: the second press happens at the configured 1.20 mm */
    {
        const int e = (int)km_engine_bottom(&r->e, s) - (int)d->bottom[s];
        CHECK(e >= -20 && e <= 20);
    }
    rig_ramp(r, s, d->bottom[s], 400);
    CHECK_EQ(r->press_ev[s], 2);
    CHECK(r->press_depth[s] >= 100 && r->press_depth[s] <= 145);
    rig_ramp(r, s, d->released[s], 400);
    rig_run(r, 50);
    CHECK_EQ(r->release_ev[s], 2);
    CHECK(r->release_depth[s] >= 55 && r->release_depth[s] <= 100);

    /* hysteresis: pressed, then back up to 1.0 mm (between release 0.8 and press 1.2): stays pressed;
       then oscillate around 1.0 mm: no events at all */
    rig_ramp(r, s, d->bottom[s], 200);
    CHECK(km_engine_slot_down(&r->e, s));
    rig_ramp(r, s, level_at(s, 100), 200);
    rig_run(r, 200);
    CHECK(km_engine_slot_down(&r->e, s));
    for (unsigned i = 0; i < 100; i++) {
        r->level[s] = level_at(s, i % 2u ? 90 : 110);
        rig_run(r, 20);
    }
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->press_ev[s], 3);
    CHECK_EQ(r->release_ev[s], 2);
    /* up to 0.5 mm: released exactly once */
    rig_ramp(r, s, level_at(s, 50), 100);
    rig_run(r, 100);
    CHECK(!km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->release_ev[s], 3);
    /* and it takes the full 1.20 mm to press again: 1.0 mm is not enough */
    rig_ramp(r, s, level_at(s, 100), 100);
    rig_run(r, 500);
    CHECK(!km_engine_slot_down(&r->e, s));
    rig_ramp(r, s, level_at(s, 140), 100);
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->press_ev[s], 4);

    /* nothing else moved */
    for (unsigned o = 0; o < KM_ENGINE_SLOTS; o++) {
        if (o != s) {
            CHECK_EQ(r->press_ev[o] + r->release_ev[o], 0);
        }
    }
    free(r);
}
