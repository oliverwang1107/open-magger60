/*
 * Frame-cost regression check (M5 T-F part 5, STABILITY_NOTES.md section 5): the engine once had a frame that cost twice a normal one
 * (every key's baseline look on every 16th frame) and met a threshold recompute now and then, which came close to the whole frame
 * budget. Host timing is too noisy for a test, so this counts the WORK of each frame, which is exact:
 *   - how many keys look at their baseline in one frame (their base_q16 moves),
 *   - how many keys get their thresholds recomputed in one frame (the dirty counter),
 * and prices a frame with the model of tools/bench_engine_budget.py (M4 cycle estimates: 3000 idle frame, 40 per baseline look,
 * 22 depth-model calls of 45 + 120 per recompute, 8 per step of the 61-step dirty search). The worst possible frame has to stay
 * well under the 10.4 k cycles budget; the old engine modelled 7.0 k and measured up to 10.7 k on the keyboard.
 */
#define BUDGET_MODEL_BASE        3000u
#define BUDGET_MODEL_PER_LOOK      40u
#define BUDGET_MODEL_RECOMPUTE   (22u * 45u + 120u)
#define BUDGET_MODEL_SEARCH      (61u * 8u)
#define BUDGET_MODEL_LIMIT       6000u          /* the old worst case was 7038 on this model; the budget is 10400 */

static void test_engine_frame_work(void)
{
    km_engine_t e;
    uint16_t idle[KM_ENGINE_SLOTS], frame[KM_ENGINE_SLOTS];
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        idle[s] = (uint16_t)(3200u + (s * 7u) % 120u);
    }
    km_engine_init(&e);
    km_engine_calibrate_begin(&e);
    for (unsigned i = 0; i < 64; i++) {
        km_engine_calibrate_idle(&e, idle);
    }
    CHECK(km_engine_calibrate_finish(&e) == KM_ENGINE_CAL_OK);

    /* every key reads 6 counts off its baseline (inside the drift window), so every look moves the baseline */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        frame[s] = (uint16_t)(idle[s] + 6u);
    }
    unsigned looks_total[KM_ENGINE_SLOTS] = { 0 };
    unsigned max_looks = 0;
    unsigned active = e.n_active;
    for (unsigned f = 0; f < 320; f++) {
        uint32_t before[KM_ENGINE_SLOTS];
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            before[s] = e.slot[s].base_q16;
        }
        km_engine_update(&e, frame);
        unsigned looks = 0;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            if (e.slot[s].base_q16 != before[s]) {
                looks++;
                looks_total[s]++;
            }
        }
        if (looks > max_looks) {
            max_looks = looks;
        }
    }
    CHECK(max_looks <= (active + KM_ENGINE_DRIFT_PERIOD - 1u) / KM_ENGINE_DRIFT_PERIOD + 1u);    /* ~4 keys, never all of them */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (e.slot[s].flags & 0x02u) {                                                           /* active: still once per period */
            CHECK_EQ(looks_total[s], 320u / KM_ENGINE_DRIFT_PERIOD);
        }
    }

    /* a settings change that dirties every key: one recompute per frame, in every frame, until all are done */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_slot_is_key(s)) {
            km_engine_set_bottom(&e, s, (uint16_t)(km_engine_bottom(&e, s) - 30u));
        }
    }
    unsigned max_recompute = 0, frames_to_clean = 0, worst_model = 0;
    while (e.dirty != 0u && frames_to_clean < 200u) {
        const unsigned d0 = e.dirty;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {

            frame[s] = (uint16_t)(idle[s] + 6u);
        }
        unsigned looks = 0;
        uint32_t before[KM_ENGINE_SLOTS];
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            before[s] = e.slot[s].base_q16;
        }
        km_engine_update(&e, frame);
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            looks += e.slot[s].base_q16 != before[s];
        }
        const unsigned recomputed = d0 - e.dirty;
        if (recomputed > max_recompute) {
            max_recompute = recomputed;
        }
        const unsigned model = BUDGET_MODEL_BASE + looks * BUDGET_MODEL_PER_LOOK + (recomputed ? BUDGET_MODEL_RECOMPUTE + BUDGET_MODEL_SEARCH : 0u);
        if (model > worst_model) {
            worst_model = model;
        }
        frames_to_clean++;
    }
    CHECK_EQ(max_recompute, 1u);                                   /* at most one slot per frame ... */
    CHECK(frames_to_clean >= 55u && frames_to_clean <= 70u);       /* ... and it takes ~61 frames to do them all */
    CHECK(worst_model < BUDGET_MODEL_LIMIT);                       /* the worst modelled frame: recompute + full search + the looks of that frame */
    printf("  engine frame work: <= %u baseline looks and %u threshold recompute per frame, worst modelled frame %u of 10400 cycles\n",
           max_looks, max_recompute, worst_model);
}
