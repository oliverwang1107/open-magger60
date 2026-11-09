/* test_engine_run.inc - part of test_engine.c: bounce, config, disable, multi-key, drift, real endpoints. */

static void test_engine_fast_slow_bounce(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    const unsigned s = 73; /* A */

    /* fast: straight from released to bottom in one frame, held, straight back */
    rig_boot(r, 30, NOISE);
    r->level[s] = d->bottom[s];
    unsigned lat = 0;
    while (!km_engine_slot_down(&r->e, s) && lat < 100) {
        rig_step(r);
        lat++;
    }
    CHECK(lat >= KM_ENGINE_PRESS_FRAMES && lat <= KM_ENGINE_PRESS_FRAMES + 3u); /* a few frames, not instant */
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[s], 1);
    r->level[s] = d->released[s];
    lat = 0;
    while (km_engine_slot_down(&r->e, s) && lat < 100) {
        rig_step(r);
        lat++;
    }
    CHECK(lat >= KM_ENGINE_RELEASE_FRAMES && lat <= KM_ENGINE_RELEASE_FRAMES + 8u);
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[s], 1);
    CHECK_EQ(r->release_ev[s], 1);

    /* a one- or two-frame spike to the bottom is no keypress */
    for (unsigned len = 1; len <= 2; len++) {
        for (unsigned rep = 0; rep < 20; rep++) {
            r->level[s] = d->bottom[s];
            rig_run(r, len);
            r->level[s] = d->released[s];
            rig_run(r, 100);
        }
    }
    CHECK_EQ(r->press_ev[s], 1);
    /* ... but a real 40-frame press (1.7 ms) is */
    r->level[s] = d->bottom[s];
    rig_run(r, 40);
    r->level[s] = d->released[s];
    rig_run(r, 100);
    CHECK_EQ(r->press_ev[s], 2);
    CHECK_EQ(r->release_ev[s], 2);

    /* very slow press (about 1 s) and very slow release */
    rig_ramp(r, s, d->bottom[s], 23000);
    CHECK_EQ(r->press_ev[s], 3);
    rig_ramp(r, s, d->released[s], 23000);
    rig_run(r, 100);
    CHECK_EQ(r->release_ev[s], 3);

    /* release bounce: while pressed the key flickers up past the release point for fewer frames than the
       release debounce: stays pressed, no extra events */
    r->level[s] = d->bottom[s];
    rig_run(r, 100);
    CHECK_EQ(r->press_ev[s], 4);
    for (unsigned rep = 0; rep < 30; rep++) {
        r->level[s] = d->released[s];
        rig_run(r, 7);
        r->level[s] = d->bottom[s];
        rig_run(r, 30);
    }
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->press_ev[s], 4);
    CHECK_EQ(r->release_ev[s], 3);
    /* a longer lift (the real release) lets go once; a bounce back down right after it presses once more */
    r->level[s] = d->released[s];
    rig_run(r, 60);
    CHECK(!km_engine_slot_down(&r->e, s));
    r->level[s] = d->bottom[s];
    rig_run(r, 60);
    CHECK(km_engine_slot_down(&r->e, s));
    CHECK_EQ(r->release_ev[s], 4);
    CHECK_EQ(r->press_ev[s], 5);
    free(r);
}

static void test_engine_config(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    km_engine_key_cfg_t c;
    rig_boot(r, 40, NOISE);

    /* defaults */
    CHECK(km_engine_get_key_cfg(&r->e, 18, &c));
    CHECK_EQ(c.press_mm100, 120);
    CHECK_EQ(c.release_mm100, 80);
    CHECK_EQ(c.mode, KM_ENGINE_TRIG_FIXED);
    CHECK(!km_engine_get_key_cfg(&r->e, 76, &c));

    /* validation: nothing changes on a bad config */
    km_engine_key_cfg_default(&c);
    CHECK(km_engine_cfg_valid(&c));
    c.release_mm100 = 120;
    CHECK(!km_engine_cfg_valid(&c));
    CHECK(!km_engine_set_key_cfg(&r->e, 18, &c));
    c.release_mm100 = 130;
    CHECK(!km_engine_cfg_valid(&c));
    km_engine_key_cfg_default(&c);
    c.release_mm100 = 9;
    CHECK(!km_engine_cfg_valid(&c));
    c.release_mm100 = 10;
    CHECK(km_engine_cfg_valid(&c));
    km_engine_key_cfg_default(&c);
    c.press_mm100 = 341;
    CHECK(!km_engine_cfg_valid(&c));
    c.press_mm100 = 340;
    CHECK(km_engine_cfg_valid(&c));
    km_engine_key_cfg_default(&c);
    c.mode = 2;
    CHECK(!km_engine_cfg_valid(&c));
    CHECK(!km_engine_cfg_valid(NULL));
    CHECK(!km_engine_set_key_cfg(&r->e, 18, &c));
    km_engine_key_cfg_default(&c);
    CHECK(!km_engine_set_key_cfg(&r->e, 76, &c));
    CHECK(km_engine_get_key_cfg(&r->e, 18, &c));
    CHECK_EQ(c.press_mm100, 120);
    CHECK_EQ(c.release_mm100, 80);

    /* a deeper actuation point for one key (2.5 mm / 2.0 mm) leaves the others alone */
    CHECK(km_engine_get_key_cfg(&r->e, 73, &c));
    c.press_mm100 = 250;
    c.release_mm100 = 200;
    CHECK(km_engine_set_key_cfg(&r->e, 73, &c));
    CHECK(km_engine_set_bottom(&r->e, 73, d->bottom[73]));
    rig_run(r, 200); /* the thresholds are recomputed in the background */
    rig_ramp(r, 73, d->bottom[73], 300);
    rig_ramp(r, 18, d->bottom[18], 300);
    CHECK_EQ(r->press_ev[73], 1);
    CHECK_EQ(r->press_ev[18], 1);
    CHECK(r->press_depth[73] >= 245 && r->press_depth[73] <= 285);
    CHECK(r->press_depth[18] >= 60 && r->press_depth[18] <= 220);
    rig_ramp(r, 73, d->released[73], 300);
    rig_run(r, 50);
    CHECK(r->release_depth[73] >= 150 && r->release_depth[73] <= 205);

    /* a shallow one: 0.5 mm / 0.2 mm */
    c.press_mm100 = 50;
    c.release_mm100 = 20;
    CHECK(km_engine_set_key_cfg(&r->e, 73, &c));
    rig_run(r, 200);
    rig_ramp(r, 73, d->bottom[73], 300);
    CHECK_EQ(r->press_ev[73], 2);
    CHECK(r->press_depth[73] >= 48 && r->press_depth[73] <= 85);
    rig_ramp(r, 73, d->released[73], 300);
    rig_run(r, 50);
    CHECK_EQ(r->release_ev[73], 2);
    free(r);
}

static void test_engine_disable_and_mirrors(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 50, NOISE);

    /* The ADC4 mirror slots follow slots 13 (15, 17), 31 (33, 35), 49 (51, 53) and 67 (69, 71). Press those
       four keys: the mirrors move with them, but they never become keys. */
    static const unsigned src[4] = { 13, 31, 49, 67 };
    static const unsigned mir[4][2] = { { 15, 17 }, { 33, 35 }, { 51, 53 }, { 69, 71 } };
    for (unsigned i = 0; i < 4; i++) {
        const unsigned drop = (unsigned)d->released[src[i]] - (unsigned)d->bottom[src[i]];
        for (unsigned k = 0; k < 2; k++) {
            r->level[mir[i][k]] = (uint16_t)(d->idle[mir[i][k]] - drop);
        }
        rig_ramp(r, src[i], d->bottom[src[i]], 200);
        rig_run(r, 100);
        CHECK(km_engine_slot_down(&r->e, src[i]));
        for (unsigned k = 0; k < 2; k++) {
            CHECK(!km_engine_slot_down(&r->e, mir[i][k]));
            CHECK_EQ(km_engine_depth(&r->e, mir[i][k]), 0);
            CHECK_EQ(km_engine_slot_status(&r->e, mir[i][k]), KM_ENGINE_SLOT_NO_KEY);
        }
    }
    unsigned n;
    const uint8_t *list = km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 4);
    for (unsigned i = 0; i < n; i++) {
        CHECK(km_keymap_slot_is_key(list[i]));
    }
    /* floating inputs wandering to a "pressed" level are ignored too */
    static const unsigned flt[7] = { 20, 24, 40, 41, 46, 50, 58 };
    for (unsigned i = 0; i < 7; i++) {
        r->level[flt[i]] = 1800;
    }
    rig_run(r, 200);
    for (unsigned i = 0; i < 7; i++) {
        CHECK_EQ(r->press_ev[flt[i]], 0);
    }
    CHECK_EQ(total_events(r), 4);

    /* disable / enable a key at run time */
    rig_boot(r, 51, NOISE);
    r->level[73] = d->bottom[73];
    rig_run(r, 100);
    CHECK(km_engine_slot_down(&r->e, 73));
    CHECK(km_engine_set_slot_enabled(&r->e, 73, false)); /* forced up at once */
    CHECK(!km_engine_slot_down(&r->e, 73));
    CHECK_EQ(km_engine_slot_status(&r->e, 73), KM_ENGINE_SLOT_DISABLED);
    unsigned nd;
    km_engine_down_slots(&r->e, &nd);
    CHECK_EQ(nd, 0);
    rig_run(r, 200);
    CHECK(!km_engine_slot_down(&r->e, 73));
    CHECK_EQ(r->press_ev[73], 1);
    CHECK_EQ(r->release_ev[73], 1);
    CHECK(km_engine_set_slot_enabled(&r->e, 73, true)); /* still held: presses again */
    rig_run(r, 100);
    CHECK(km_engine_slot_down(&r->e, 73));
    CHECK_EQ(r->press_ev[73], 2);
    CHECK_EQ(km_engine_slot_status(&r->e, 73), KM_ENGINE_SLOT_OK);
    CHECK(!km_engine_set_slot_enabled(&r->e, 15, true)); /* a mirror cannot be enabled */
    CHECK(!km_engine_set_slot_enabled(&r->e, 76, true));
    /* a slot disabled for implausible idle is not revived by enabling it */
    rig_init(r, 52, NOISE);
    r->level[18] = 1360;
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);
    CHECK(km_engine_set_slot_enabled(&r->e, 18, true));
    r->level[18] = d->bottom[18];
    rig_run(r, 200);
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_DISABLED);

    /* a slot that is disabled by the user before calibration stays off after it */
    rig_init(r, 53, NOISE);
    CHECK(km_engine_set_slot_enabled(&r->e, 63, false));
    CHECK_EQ(rig_calibrate(r, 64), KM_ENGINE_CAL_OK);
    r->level[63] = d->bottom[63];
    rig_run(r, 200);
    CHECK(!km_engine_slot_down(&r->e, 63));
    CHECK(km_engine_set_slot_enabled(&r->e, 63, true));
    rig_run(r, 200);
    CHECK(km_engine_slot_down(&r->e, 63));

    /* sensor fault at run time: a pressed key whose input suddenly floats is forced up and stays out */
    rig_boot(r, 54, NOISE);
    r->level[18] = d->bottom[18];
    rig_run(r, 100);
    CHECK(km_engine_slot_down(&r->e, 18));
    r->level[18] = 1360;
    rig_run(r, 100);
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_FAULT);
    CHECK_EQ(r->release_ev[18], 1);
    r->level[18] = 1520; /* above the fault level but nowhere near a released key: still out */
    rig_run(r, 100);
    CHECK(!km_engine_slot_down(&r->e, 18));
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_FAULT);
    /* back to normal: on its way through the pressed range it must not press */
    r->level[18] = d->released[18];
    rig_run(r, 100);
    CHECK_EQ(km_engine_slot_status(&r->e, 18), KM_ENGINE_SLOT_OK);
    CHECK_EQ(r->press_ev[18], 1);
    rig_ramp(r, 18, d->bottom[18], 200);
    CHECK(km_engine_slot_down(&r->e, 18));
    CHECK_EQ(r->press_ev[18], 2);
    free(r);
}

static void test_engine_multikey(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 60, NOISE);

    /* press A S D F G H J K one after the other: the pressed list is in press order */
    static const unsigned order[8] = { 73, 72, 74, 49, 34, 52, 9, 47 };
    for (unsigned i = 0; i < 8; i++) {
        r->level[order[i]] = d->bottom[order[i]];
        rig_run(r, 60);
        unsigned n;
        const uint8_t *list = km_engine_down_slots(&r->e, &n);
        CHECK_EQ(n, i + 1u);
        CHECK_EQ(list[i], order[i]);
        uint8_t b[8];
        km_report_build_boot(list, n, b);
        if (i < 6) {
            CHECK_EQ(b[2 + i], km_keymap_usage(0, order[i]));
            CHECK_EQ(b[2], 0x04); /* A stays first */
        } else {
            for (unsigned j = 2; j < 8; j++) {
                CHECK_EQ(b[j], 0x01); /* rollover */
            }
        }
    }
    /* let go of S (second) and K (last): the rest keep their order. A D F G H J */
    r->level[72] = d->released[72];
    r->level[47] = d->released[47];
    rig_run(r, 60);
    unsigned n;
    const uint8_t *list = km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 6);
    CHECK_EQ(list[0], 73);
    CHECK_EQ(list[1], 74);
    CHECK_EQ(list[5], 9);
    uint8_t b[8];
    const uint8_t want[8] = { 0x00, 0x00, 0x04, 0x07, 0x09, 0x0A, 0x0B, 0x0D };
    km_report_build_boot(list, n, b);
    CHECK(memcmp(b, want, 8) == 0);
    /* release the others */
    for (unsigned i = 0; i < 8; i++) {
        r->level[order[i]] = d->released[order[i]];
    }
    rig_run(r, 60);
    km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 0);
    CHECK_EQ(km_engine_glitch_count(&r->e), 0);

    /* Shift + A comes out in the modifier byte */
    r->level[0] = d->bottom[0];
    r->level[73] = d->bottom[73];
    rig_run(r, 60);
    list = km_engine_down_slots(&r->e, &n);
    km_report_build_boot(list, n, b);
    CHECK_EQ(b[0], 0x02);
    CHECK_EQ(b[2], 0x04);
    r->level[0] = d->released[0];
    r->level[73] = d->released[73];
    rig_run(r, 60);

    /* all 61 keys held one after another, then released in reverse: the list holds 61, nothing gets lost */
    unsigned held = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            r->level[s] = d->bottom[s];
            rig_run(r, 40);
            held++;
        }
    }
    list = km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, held);
    CHECK_EQ(r->max_down, 61);
    uint8_t nk[29];
    km_report_build_nkro(list, n, nk);
    CHECK_EQ(nk[28], 0xFF);
    for (int s = (int)KM_ENGINE_SLOTS - 1; s >= 0; s--) {
        if (d->is_key[s]) {
            r->level[s] = d->released[s];
            rig_run(r, 40);
        }
    }
    km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 0);
    CHECK_EQ(km_engine_glitch_count(&r->e), 0);

    /* simultaneous onsets: 7 in one frame are accepted (ascending slot order), 8 in one frame are a glitch */
    rig_boot(r, 61, 0);
    unsigned cnt = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && cnt < KM_ENGINE_GLITCH_ONSET - 1u; s++) {
        if (d->is_key[s]) {
            r->level[s] = d->bottom[s];
            cnt++;
        }
    }
    rig_run(r, 100);
    list = km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, KM_ENGINE_GLITCH_ONSET - 1u);
    for (unsigned i = 1; i < n; i++) {
        CHECK(list[i - 1] < list[i]);
    }
    CHECK_EQ(km_engine_glitch_count(&r->e), 0);

    rig_boot(r, 62, 0);
    cnt = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS && cnt < KM_ENGINE_GLITCH_ONSET; s++) {
        if (d->is_key[s]) {
            r->level[s] = d->bottom[s];
            cnt++;
        }
    }
    rig_run(r, 100);
    km_engine_down_slots(&r->e, &n);
    CHECK_EQ(n, 0);
    CHECK_EQ(total_events(r), 0);
    CHECK(km_engine_glitch_count(&r->e) > 0);
    /* the disturbance goes away: back to normal, and a real press afterwards works */
    memcpy(r->level, d->idle, sizeof r->level);
    rig_run(r, 100);
    rig_ramp(r, 18, d->bottom[18], 100);
    CHECK_EQ(r->press_ev[18], 1);
    /* a common-mode shift of every key (supply dip) does not press anything and does not stick */
    rig_boot(r, 63, NOISE);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            r->level[s] = (uint16_t)(d->released[s] - 700);
        }
    }
    rig_run(r, 1000);
    CHECK_EQ(total_events(r), 0);
    memcpy(r->level, d->idle, sizeof r->level);
    rig_run(r, 1000);
    CHECK_EQ(total_events(r), 0);
    free(r);
}

static void test_engine_drift(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);

    /* slow warming: every released level rises 60 counts over ~250k frames (11 s); the baseline follows,
       nothing is triggered, and the actuation depth is still right afterwards */
    rig_boot(r, 70, NOISE);
    CHECK(km_engine_set_bottom(&r->e, 18, d->bottom[18]));
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
            /* at rest the model is steepest: about 1 (0.01 mm) per count, so +-15 counts of noise read as up to 0.15 mm */
            CHECK(km_engine_depth(&r->e, s) <= 15);
        }
    }
    {
        /* press depth after the drift, against the drifted released level */
        const unsigned s = 18;
        const km_depth_cal_t c = { (uint16_t)(d->released[s] + 60u), d->bottom[s] };
        const int from = c.released;
        int pressed = 0;
        for (int i = 1; i <= 800 && !pressed; i++) {
            r->level[s] = (uint16_t)(from + ((int)c.bottom - from) * i / 800);
            rig_step(r);
            if (km_engine_slot_down(&r->e, s)) {
                const uint16_t depth = km_depth_model_mm100(&c, r->level[s]);
                CHECK(depth >= 100 && depth <= 145);
                pressed = 1;
            }
        }
        CHECK(pressed);
        CHECK_EQ(r->press_ev[s], 1);
    }

    /* cooling: the same downwards */
    rig_boot(r, 71, NOISE);
    for (unsigned i = 1; i <= 40; i++) {
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            r->level[s] = (uint16_t)(d->idle[s] - i);
        }
        rig_run(r, 4000);
    }
    rig_run(r, 100000);
    CHECK_EQ(total_events(r), 0);
    CHECK(km_engine_baseline(&r->e, 18) <= d->released[18] - 37u);
    CHECK(km_engine_baseline(&r->e, 18) >= d->released[18] - 43u);

    /* a key held pressed does not pull the baseline down; neither does one held half way (0.5 mm) */
    rig_boot(r, 72, NOISE);
    r->level[18] = d->bottom[18];
    r->level[73] = level_at(73, 50);
    rig_run(r, 300000);
    CHECK(km_engine_slot_down(&r->e, 18));
    CHECK(!km_engine_slot_down(&r->e, 73));
    CHECK(abs((int)km_engine_baseline(&r->e, 18) - (int)d->released[18]) <= 4);
    CHECK(abs((int)km_engine_baseline(&r->e, 73) - (int)d->released[73]) <= 4);
    r->level[18] = d->released[18];
    r->level[73] = d->released[73];
    rig_run(r, 200);
    CHECK_EQ(r->press_ev[18], 1);
    CHECK_EQ(r->release_ev[18], 1);
    CHECK_EQ(r->press_ev[73], 0);

    /* a jump of the released level (not drift) is not tracked, and does no harm upwards */
    rig_boot(r, 73, NOISE);
    r->level[18] = (uint16_t)(d->released[18] + 300);
    rig_run(r, 200000);
    CHECK(abs((int)km_engine_baseline(&r->e, 18) - (int)d->released[18]) <= 4);
    CHECK_EQ(total_events(r), 0);

    /* the baseline never wanders further than KM_ENGINE_DRIFT_LIMIT from its calibrated value */
    rig_boot(r, 74, NOISE);
    for (unsigned i = 1; i <= 400; i++) {
        r->level[18] = (uint16_t)(d->released[18] + i);
        rig_run(r, 3000);
        CHECK(km_engine_baseline(&r->e, 18) <= d->released[18] + KM_ENGINE_DRIFT_LIMIT + 1u);
    }
    CHECK(km_engine_baseline(&r->e, 18) >= d->released[18] + KM_ENGINE_DRIFT_LIMIT - 4u);
    free(r);
}

static void test_engine_real_endpoints(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);
    rig_boot(r, 80, NOISE);

    /* every key in turn: released -> bottom -> released, twice (the first press teaches the stroke) */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            continue;
        }
        const unsigned before = total_events(r);
        for (unsigned pass = 1; pass <= 2; pass++) {
            rig_ramp(r, s, d->bottom[s], 300);
            rig_run(r, 100);
            CHECK(km_engine_slot_down(&r->e, s));
            CHECK_EQ(r->press_ev[s], pass);
            CHECK(r->press_depth[s] >= 55 && r->press_depth[s] <= 230);
            if (pass == 2) {
                CHECK(r->press_depth[s] >= 100 && r->press_depth[s] <= 145);
            }
            CHECK(km_engine_depth(&r->e, s) >= KM_TRAVEL_MM100 - 20u);
            rig_ramp(r, s, d->released[s], 300);
            rig_run(r, 100);
            CHECK(!km_engine_slot_down(&r->e, s));
            CHECK_EQ(r->release_ev[s], pass);
            CHECK(r->release_depth[s] >= 30 && r->release_depth[s] <= 140);
            CHECK(km_engine_depth(&r->e, s) <= 8);
        }
        CHECK_EQ(total_events(r), before + 4u); /* only this key did anything */
        const int e = (int)km_engine_bottom(&r->e, s) - (int)d->bottom[s];
        CHECK(e >= -20 && e <= 20);
        CHECK(km_engine_slot_status(&r->e, s) == KM_ENGINE_SLOT_OK);
    }
    CHECK_EQ(km_engine_glitch_count(&r->e), 0);

    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            CHECK_EQ(r->press_ev[s], 0);
        }
    }

    /* a keystroke at typing speed (about 6 ms down and 6 ms up) on every key, noisy */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            rig_ramp(r, s, d->bottom[s], 140);
            rig_run(r, 300);
            rig_ramp(r, s, d->released[s], 140);
            rig_run(r, 60);
            CHECK_EQ(r->press_ev[s], 3);
            CHECK_EQ(r->release_ev[s], 3);
        }
    }
    free(r);
}

static void test_engine_misc(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);

    /* readings above the 12-bit range are clamped, nothing breaks */
    rig_boot(r, 90, 0);
    {
        uint16_t f[KM_ENGINE_SLOTS];
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            f[s] = 65535;
        }
        for (unsigned i = 0; i < 100; i++) {
            km_engine_update(&r->e, f);
        }
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            CHECK(!km_engine_slot_down(&r->e, s));
        }
    }
    CHECK(!km_engine_slot_down(&r->e, 76));
    CHECK(!km_engine_slot_down(&r->e, 1000));
    CHECK_EQ(km_engine_depth(&r->e, 1000), 0);
    CHECK_EQ(km_engine_baseline(&r->e, 1000), 0);
    CHECK_EQ(km_engine_bottom(&r->e, 1000), 0);
    CHECK_EQ(km_engine_filtered(&r->e, 1000), 0);

    /* a stored bottom reading is used from the start */
    rig_boot(r, 91, NOISE);
    CHECK(km_engine_set_bottom(&r->e, 18, d->bottom[18]));
    CHECK(!km_engine_set_bottom(&r->e, 15, 2000));
    rig_run(r, 200);
    CHECK_EQ(km_engine_bottom(&r->e, 18), d->bottom[18]);
    rig_ramp(r, 18, d->bottom[18], 400);
    CHECK_EQ(r->press_ev[18], 1);
    CHECK(r->press_depth[18] >= 100 && r->press_depth[18] <= 145);
    /* an absurd bottom (above the baseline) is pulled into the allowed span */
    CHECK(km_engine_set_bottom(&r->e, 73, 4000));
    rig_run(r, 200);
    CHECK(km_engine_bottom(&r->e, 73) + KM_ENGINE_MIN_SPAN <= km_engine_baseline(&r->e, 73));

    /* filtered reading and depth diagnostics follow the key */
    rig_boot(r, 92, 0);
    CHECK(km_engine_set_bottom(&r->e, 18, d->bottom[18]));
    r->level[18] = level_at(18, 200);
    rig_run(r, 200);
    CHECK(abs((int)km_engine_filtered(&r->e, 18) - (int)r->level[18]) <= 1);
    CHECK(km_engine_depth(&r->e, 18) >= 195 && km_engine_depth(&r->e, 18) <= 205);
    free(r);
}

static void test_engine_exact(void)
{
    const data_t *d = data();
    rig_t *r = (rig_t *)malloc(sizeof *r);

    /* With the bottom known and no noise the actuation point is exact for every key: the reading that sits
       at 1.20 mm (largest reading with depth >= 120) presses, one count shallower does not. */
    rig_boot(r, 110, 0);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->is_key[s]) {
            CHECK(km_engine_set_bottom(&r->e, s, d->bottom[s]));
        }
    }
    rig_run(r, 200);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (!d->is_key[s]) {
            continue;
        }
        const uint16_t edge = level_at(s, KM_ENGINE_DEFAULT_PRESS_MM100);
        r->level[s] = (uint16_t)(edge + 1u);
        rig_run(r, 100);
        CHECK(!km_engine_slot_down(&r->e, s));
        r->level[s] = edge;
        rig_run(r, 100);
        CHECK(km_engine_slot_down(&r->e, s));
        r->level[s] = d->released[s];
        rig_run(r, 100);
        CHECK(!km_engine_slot_down(&r->e, s));
    }
    CHECK_EQ(total_events(r), 2u * 61u);

    /* Exact debounce: the key rests one count short of the press point (filter settled), then drops 60
       counts deeper at once. The filtered value is past the threshold from the first frame on, so the key
       is down after exactly KM_ENGINE_PRESS_FRAMES frames. */
    {
        const unsigned s = 73;
        const uint16_t edge = level_at(s, KM_ENGINE_DEFAULT_PRESS_MM100);
        r->level[s] = (uint16_t)(edge + 1u);
        rig_run(r, 100);
        r->level[s] = (uint16_t)(edge - 60u);
        unsigned n = 0;
        while (!km_engine_slot_down(&r->e, s) && n < 50) {
            rig_step(r);
            n++;
        }
        CHECK_EQ(n, KM_ENGINE_PRESS_FRAMES);
        /* the same for the release: one count deeper than the release point, then 60 counts up */
        const uint16_t rel = level_at(s, (uint16_t)(KM_ENGINE_DEFAULT_RELEASE_MM100 + 1u));
        r->level[s] = rel;
        rig_run(r, 100);
        CHECK(km_engine_slot_down(&r->e, s));
        r->level[s] = (uint16_t)(rel + 60u);
        n = 0;
        while (km_engine_slot_down(&r->e, s) && n < 50) {
            rig_step(r);
            n++;
        }
        CHECK_EQ(n, KM_ENGINE_RELEASE_FRAMES);
    }

    /* readings above 12 bit are clamped to 4095 before they reach the filter */
    rig_boot(r, 111, 0);
    r->level[18] = 4095;
    rig_run(r, 200);
    CHECK_EQ(km_engine_filtered(&r->e, 18), 4095);
    {
        uint16_t f[KM_ENGINE_SLOTS];
        memcpy(f, r->level, sizeof f);
        f[18] = 5000;
        for (unsigned i = 0; i < 200; i++) {
            km_engine_update(&r->e, f);
        }
        CHECK_EQ(km_engine_filtered(&r->e, 18), 4095);
    }

    /* moving keys during calibration: the limit itself is accepted */
    for (unsigned count = KM_ENGINE_CAL_MAX_BAD; count <= KM_ENGINE_CAL_MAX_BAD + 1u; count++) {
        uint16_t f[KM_ENGINE_SLOTS];
        rig_init(r, 112, NOISE);
        km_engine_calibrate_begin(&r->e);
        for (unsigned i = 0; i < 64; i++) {
            unsigned moved = 0;
            for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
                if (d->is_key[s] && moved < count) {
                    r->level[s] = (uint16_t)(d->released[s] - ((i % 2u) ? 150u : 0u));
                    moved++;
                }
            }
            rig_frame(r, f);
            km_engine_calibrate_idle(&r->e, f);
        }
        CHECK_EQ(km_engine_calibrate_finish(&r->e), count == KM_ENGINE_CAL_MAX_BAD ? KM_ENGINE_CAL_OK : KM_ENGINE_CAL_UNSTABLE);
    }
    free(r);
}

void test_engine(void)
{
    test_keymap();
    test_report_boot();
    test_report_nkro();
    test_engine_calibration();
    test_engine_idle_noise();
    test_engine_press_release();
    test_engine_fast_slow_bounce();
    test_engine_config();
    test_engine_disable_and_mirrors();
    test_engine_multikey();
    test_engine_drift();
    test_engine_real_endpoints();
    test_engine_misc();
    test_engine_exact();
    test_engine_rapid();
    test_engine_frame_work();
}
