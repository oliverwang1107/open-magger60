/*
 * km_engine.h - our own key engine (M4 task T-B): sensor counts in, pressed keys out.
 * Original work, designed from the measured data (core/data/slot_ranges_s2.json, slotmap_measured.json).
 *
 * Input per frame: the 76 sixteen-point sums of km_scan divided by 16 (12-bit ADC counts, a smaller
 * reading = a deeper press; released 3100..3450, bottomed 1800..2400, noise about +-15 counts).
 *
 * Per slot and frame (cheap, fixed point, no division):
 *   1. low-pass   y += (x - y) / 4 in Q4 (time constant ~4 frames, ~0.17 ms; the +-15 count noise drops to ~6)
 *   2. compare the filtered value with two thresholds (in counts) -> a state machine with hysteresis:
 *        up   -> down once the value stayed at/below the press threshold for KM_ENGINE_PRESS_FRAMES frames
 *        down -> up   once it stayed at/above the release threshold for KM_ENGINE_RELEASE_FRAMES frames
 *      The two thresholds are NOT computed per frame: they are the counts where the depth model
 *      (km_depth_model_mm100) reaches the configured press / release depth, found once by bisection
 *      whenever the baseline or the bottom reading changes (slow, one slot per frame at most).
 *   3. slow baseline drift tracking, only while the key is up and the value is within
 *      KM_ENGINE_DRIFT_WINDOW counts of the baseline (a key that is even slightly pressed is not tracked).
 *   4. bottom learning: the "bottomed-out" reading starts at (baseline - KM_ENGINE_DEFAULT_SPAN) and is
 *      lowered whenever a pressed key reads deeper than that, so every key learns its own stroke on the
 *      first full press. km_engine_set_bottom() loads a stored value.
 *
 * Which slots are keys is taken from km_keymap (61 slots); the 8 ADC4 mirror slots (15 17 33 35 51 53
 * 69 71) and the 7 floating inputs (20 24 40 41 46 50 58) are never evaluated, never reported.
 *
 * Crosstalk / glitch guards (the data shows a press moving exactly one slot by 1000..1300 counts):
 *   - non-key slots are masked (see above), so the mirrors can never press anything;
 *   - a reading below KM_ENGINE_FAULT_LOW (floating/broken sensor, lower than any real bottom) forces
 *     the slot up and keeps it out until it reads plausibly again;
 *   - if KM_ENGINE_GLITCH_ONSET or more slots would become "pressed" in the very same frame (a supply
 *     dip or a common-mode shift, not fingers) none of them is accepted in that frame and they must
 *     qualify again; km_engine_glitch_count() counts these events.
 *
 * Trigger algorithms: km_engine_key_cfg_t.mode selects, per key, between
 *   FIXED  trigger_fixed(): press at a fixed depth, release at a lower one (see above);
 *   RAPID  trigger_rapid(): rapid trigger. The key is pressed when its depth has grown by rt_down since
 *          the shallowest point of the stroke, released when it has shrunk by rt_up since the deepest
 *          point; dead zones at the top and the bottom; optional "continuous" mode. The precise rules,
 *          the noise-floor derivation and the cost are in core/ENGINE_NOTES.md ("Rapid trigger").
 * Both share the debounce, glitch, fault and drift protections of this file. An invalid or unknown
 * configuration is never used: the key falls back to the FIXED defaults (km_engine_apply_key_cfg()).
 */
#ifndef KM_ENGINE_H
#define KM_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#define KM_ENGINE_SLOTS      76u
#define KM_ENGINE_MAX_DOWN   64u   /* capacity of the pressed-slot list (there are only 61 keys) */

/*
 * Optional adaptive filter gain (FILTER_NOTES.md, M6 P12c). KM_ENGINE_FILTER_MODES = 0 (the default) compiles none of it: the engine is the
 * one of every earlier image, bit for bit. = 1 compiles the two extra filters and lets km_engine_set_filter_mode() choose between them at
 * run time (mode 0 = the fixed-gain filters, the same as with the flag off). The state layout does not depend on the flag.
 */
#ifndef KM_ENGINE_FILTER_MODES
#define KM_ENGINE_FILTER_MODES 0
#endif
#define KM_ENGINE_FMODE_FIXED       0u   /* main filter gain 1/4, rapid-trigger stage gain 1/8 (the original) */
#define KM_ENGINE_FMODE_LOW_LATENCY 1u   /* main filter with adaptive gain 1/8 .. 1/2, no second stage */
#define KM_ENGINE_FMODE_QUIET       2u   /* adaptive gain 1/8 .. 1/2 in both stages */
#define KM_ENGINE_FMODE_FAST        3u   /* measured-noise low-latency chain: adaptive gain 1/8 .. 1/2 with T1 = 3 / T2 = 8 counts, no second stage, rapid-trigger debounce 4 / 3 frames
                                          * (image-level only: settings.device.filter_mode stays 0..2, the stored format does not change) */
#define KM_ENGINE_FMODE_LAST        3u
#define KM_ENGINE_FAST_T1           3u   /* counts (the real filtered noise has sd 0.8) */
#define KM_ENGINE_FAST_T2           8u
#define KM_ENGINE_FAST_PRESS_FRAMES   4u
#define KM_ENGINE_FAST_RELEASE_FRAMES 3u
#define KM_ENGINE_ADAPT_T1          12u  /* counts: a stage whose input is closer than this to its output keeps gain 1/8 (noise) ... */
#define KM_ENGINE_ADAPT_T2          32u  /* ... closer than this: 1/4; farther (a real stroke): 1/2 */
/*
 * Fast confirm (only with KM_ENGINE_FILTER_MODES, off until km_engine_set_fast_confirm(e, true)): a rapid-trigger press / release whose stroke
 * is already at least KM_ENGINE_FAST_FACTOR times its trigger distance needs KM_ENGINE_FAST_FRAMES frames instead of the 8 / 6 of the
 * debounce. The debounce exists for spikes; a spike cannot fake a stroke of twice the trigger distance through the 16-sample mean.
 */
#define KM_ENGINE_FAST_FRAMES       3u
#define KM_ENGINE_FAST_FACTOR       2u

/* ---- tunables (frames are ~43 us, 23 kHz) ---- */
#define KM_ENGINE_FILTER_SHIFT   2u    /* low-pass gain 1/4 */
#define KM_ENGINE_PRESS_FRAMES   8u    /* consecutive frames past the press threshold (0.35 ms): the low-pass tail of a 3-frame spike is 7 */
#define KM_ENGINE_RELEASE_FRAMES 6u    /* consecutive frames past the release threshold (longer: bounce on release) */
#define KM_ENGINE_GLITCH_ONSET   8u    /* this many simultaneous presses in one frame = glitch */
#define KM_ENGINE_FAULT_LOW      1500u /* counts; floating inputs read ~1360, the deepest real bottom is ~1800 */
#define KM_ENGINE_VALUE_MAX      4000u /* calibration: above this the input is railed */
#define KM_ENGINE_DEFAULT_SPAN   1000u /* counts; start value of released - bottom (smallest real span: 998) */
#define KM_ENGINE_MIN_SPAN       400u  /* the bottom is never closer than this to the baseline */
#define KM_ENGINE_MAX_SPAN       1800u
#define KM_ENGINE_LEARN_MARGIN   8u    /* counts deeper than the known bottom before it is lowered */
#define KM_ENGINE_DRIFT_WINDOW   24u   /* counts around the baseline in which drift is tracked */
#define KM_ENGINE_DRIFT_LIMIT    150u  /* the baseline never wanders further than this from its start */
#define KM_ENGINE_DRIFT_PERIOD   16u   /* drift is looked at every 16th frame */
#define KM_ENGINE_DRIFT_SHIFT    11u   /* gain 1/2048 per look: time constant ~1.4 s */

/* ---- idle calibration ---- */
#define KM_ENGINE_CAL_MIN_FRAMES  16u
#define KM_ENGINE_CAL_IDLE_MIN    2500u /* median released level must lie in [MIN, KM_ENGINE_VALUE_MAX] */
#define KM_ENGINE_CAL_HELD_DROP   350u  /* counts below the median = "looks pressed" (~1 mm) */
#define KM_ENGINE_CAL_NOISE_MAX   120u  /* max-min over the stretch above this = key was moving */
#define KM_ENGINE_CAL_MAX_BAD     6u    /* more held / moving slots than this refuses the calibration */
#define KM_ENGINE_CAL_MIN_SLOTS   30u   /* fewer plausible key slots than this refuses the calibration */

typedef enum {
    KM_ENGINE_CAL_OK = 0,
    KM_ENGINE_CAL_TOO_FEW_FRAMES,
    KM_ENGINE_CAL_KEYS_HELD,    /* too many slots look pressed, or most of them: try again when keys are up */
    KM_ENGINE_CAL_UNSTABLE,     /* too many slots were moving during the stretch */
    KM_ENGINE_CAL_IMPLAUSIBLE   /* too few usable key slots, or the level is railed */
} km_engine_cal_result_t;

typedef enum {
    KM_ENGINE_SLOT_NO_KEY = 0,  /* no key on this slot (mirror / floating) */
    KM_ENGINE_SLOT_UNCALIBRATED,
    KM_ENGINE_SLOT_DISABLED,    /* implausible idle level at calibration, or disabled by the caller */
    KM_ENGINE_SLOT_FAULT,       /* reads below KM_ENGINE_FAULT_LOW right now */
    KM_ENGINE_SLOT_ASSUMED,     /* was held / moving at calibration: baseline guessed, converging */
    KM_ENGINE_SLOT_OK
} km_engine_slot_status_t;

typedef enum {
    KM_ENGINE_TRIG_FIXED = 0,   /* fixed actuation point with hysteresis */
    KM_ENGINE_TRIG_RAPID = 1    /* rapid trigger (depth reversal), see ENGINE_NOTES.md */
} km_engine_trig_mode_t;

/* Per-key trigger settings, depth in 0.01 mm of travel (nominal travel 3.40 mm = 340). */
typedef struct {
    uint16_t press_mm100;       /* FIXED: default 120 (1.20 mm). RAPID, continuous = 0: the actuation point */
    uint16_t release_mm100;     /* FIXED: default 80 (0.80 mm); must be below press */
    uint8_t  mode;              /* km_engine_trig_mode_t */
    uint16_t rt_down_mm100;     /* RAPID: press after this much downward travel from the shallowest point */
    uint16_t rt_up_mm100;       /* RAPID: release after this much upward travel from the deepest point */
    uint16_t top_dead_mm100;    /* RAPID: no press above this depth (0 = the minimum zone, KM_ENGINE_RT_MIN_MM100) */
    uint16_t bottom_dead_mm100; /* RAPID: the last this much of travel counts as fully pressed (0 = none) */
    uint8_t  continuous;        /* RAPID: 1 = may be pressed again anywhere until back in the top dead zone */
} km_engine_key_cfg_t;

#define KM_ENGINE_DEFAULT_PRESS_MM100   120u
#define KM_ENGINE_DEFAULT_RELEASE_MM100 80u
#define KM_ENGINE_CFG_MIN_RELEASE_MM100 10u
#define KM_ENGINE_DEFAULT_RT_MM100      30u   /* default rt_down / rt_up (0.30 mm) */
/*
 * FIXED valid: KM_ENGINE_CFG_MIN_RELEASE_MM100 <= release < press <= 340 (the RAPID fields are ignored).
 *
 * RAPID valid: rt_down, rt_up in KM_ENGINE_RT_MIN_MM100..340; top_dead and bottom_dead each 0 or in
 * KM_ENGINE_RT_MIN_MM100..340; continuous 0 or 1; with continuous = 0 also press in
 * KM_ENGINE_RT_MIN_MM100..340; rt_down <= 340 - bottom_dead (a press must be reachable);
 * max(top_dead, KM_ENGINE_RT_MIN_MM100) + bottom_dead + KM_ENGINE_RT_MIN_MM100 <= 340 (the zones leave room).
 * Anything else, any other mode value: invalid.
 */
#define KM_ENGINE_RT_MIN_MM100 20u   /* noise floor of every rapid-trigger distance, see ENGINE_NOTES.md */
#define KM_ENGINE_RT_REST_MM100 5u   /* below this depth a key is "at rest": no depth is computed for it */
#define KM_ENGINE_RT_EDGE_HYST_MM100 10u /* a pressed key is released by the top dead zone this far inside it */
#define KM_ENGINE_RT_FILTER_SHIFT 3u /* rapid trigger works on a second low-pass stage, gain 1/8 */

/* Private per-slot state (public only so that the owner can allocate it statically). */
typedef struct {
    uint32_t base_q16;          /* released baseline, counts << 16 */
    uint16_t filt_q4;           /* filtered reading, counts << 4 */
    uint16_t press_q4;          /* press when filt_q4 <= this (RAPID: deeper than the top dead zone) */
    uint16_t release_q4;        /* release when filt_q4 >= this (RAPID: well inside the top dead zone) */
    uint16_t learn_q4;          /* below this a pressed key lowers the bottom */
    uint16_t bottom;            /* counts */
    uint16_t thr_base;          /* baseline (counts) the thresholds were computed for */
    uint16_t base0;             /* baseline at calibration, centre of the drift range */
    uint16_t rest_q4;           /* RAPID: above this the key is at rest (depth < KM_ENGINE_RT_REST_MM100) */
    uint16_t bz_q4;             /* RAPID: at or below this the key is inside the bottom dead zone */
    uint16_t g_q4;              /* RAPID: second low-pass stage, counts << 4 */
    uint16_t ext;               /* RAPID: shallowest (key up: highest) / deepest (key down: lowest) reading of the stroke, counts */
    uint16_t mc_dn;             /* RAPID: fewest counts a rt_down press can take (cheap test before the depth model) */
    uint16_t mc_up;             /* RAPID: the same for rt_up */
    km_engine_key_cfg_t cfg;
    uint8_t  flags;
    uint8_t  cnt;               /* frames the pending transition has been seen */
    uint8_t  settle;            /* drift ticks an assumed baseline has been matched */
} km_engine_slot_t;

typedef struct {
    km_engine_slot_t slot[KM_ENGINE_SLOTS];
    uint8_t  active[KM_ENGINE_SLOTS];   /* slots evaluated per frame */
    uint8_t  down[KM_ENGINE_MAX_DOWN];  /* pressed slots, oldest press first */
    uint8_t  n_active;
    uint8_t  n_down;
    uint8_t  dirty;                     /* slots waiting for a threshold recompute */
    uint8_t  cursor;                    /* round-robin position of the recompute */
    bool     calibrated;
    uint8_t  filter_mode;               /* KM_ENGINE_FMODE_*; only read when KM_ENGINE_FILTER_MODES is compiled in */
    uint8_t  fast_confirm;              /* 0 / 1, the same */
    uint32_t frame;
    uint32_t glitches;
    struct {
        uint32_t acc[KM_ENGINE_SLOTS];
        uint16_t lo[KM_ENGINE_SLOTS];
        uint16_t hi[KM_ENGINE_SLOTS];
        uint16_t frames;
    } cal;
} km_engine_t;

/* All slots uncalibrated, nothing pressed, default trigger settings; key slots come from km_keymap. */
void km_engine_init(km_engine_t *e);

/* ---- boot calibration: begin, feed a quiet stretch of frames, finish ---- */
void km_engine_calibrate_begin(km_engine_t *e);
/* Feed one frame (76 values = sums / 16) of the quiet stretch. */
void km_engine_calibrate_idle(km_engine_t *e, const uint16_t sums_div16[KM_ENGINE_SLOTS]);
/*
 * Evaluate the stretch (at least KM_ENGINE_CAL_MIN_FRAMES frames). On KM_ENGINE_CAL_OK the engine is
 * armed: slots with an implausible level (floating, railed) are disabled, slots that look pressed or were
 * moving get the median level as a guessed baseline (status ASSUMED). Any other result leaves the engine
 * unarmed (km_engine_update does nothing); call begin and try again.
 */
km_engine_cal_result_t km_engine_calibrate_finish(km_engine_t *e);

/* One frame; call once per frame with the km_scan sums / 16. No-op until calibrated. */
void km_engine_update(km_engine_t *e, const uint16_t sums_div16[KM_ENGINE_SLOTS]);

/* ---- results ---- */
bool km_engine_slot_down(const km_engine_t *e, unsigned slot);
/* Pressed slots, oldest press first (same-frame presses in ascending slot order); *n = count. */
const uint8_t *km_engine_down_slots(const km_engine_t *e, unsigned *n);

/* ---- diagnostics ---- */
uint16_t km_engine_depth(const km_engine_t *e, unsigned slot);      /* 0.01 mm, 0 for unusable slots */
uint16_t km_engine_baseline(const km_engine_t *e, unsigned slot);   /* counts */
uint16_t km_engine_bottom(const km_engine_t *e, unsigned slot);     /* counts */
uint16_t km_engine_filtered(const km_engine_t *e, unsigned slot);   /* counts */
km_engine_slot_status_t km_engine_slot_status(const km_engine_t *e, unsigned slot);
uint32_t km_engine_glitch_count(const km_engine_t *e);

/* ---- configuration ---- */
/* Choose the filter (KM_ENGINE_FMODE_*). false for an unknown mode (nothing changed). Without KM_ENGINE_FILTER_MODES the value is stored and ignored. */
bool km_engine_set_filter_mode(km_engine_t *e, unsigned mode);
unsigned km_engine_get_filter_mode(const km_engine_t *e);
/* Fast confirm of long rapid-trigger strokes (see KM_ENGINE_FAST_FRAMES); ignored without KM_ENGINE_FILTER_MODES. */
void km_engine_set_fast_confirm(km_engine_t *e, bool on);
bool km_engine_get_fast_confirm(const km_engine_t *e);
void km_engine_key_cfg_default(km_engine_key_cfg_t *cfg);
bool km_engine_cfg_valid(const km_engine_key_cfg_t *cfg);
/* false (and nothing changed) for an invalid config or slot. */
bool km_engine_set_key_cfg(km_engine_t *e, unsigned slot, const km_engine_key_cfg_t *cfg);
/*
 * For configs that come from storage or from the host: true = cfg applied; false = it was NULL or invalid
 * and the key now runs the FIXED defaults (a bad config never leaves a key with stale or undefined
 * settings). Still false, and nothing changed, for a slot that is out of range.
 */
bool km_engine_apply_key_cfg(km_engine_t *e, unsigned slot, const km_engine_key_cfg_t *cfg);
bool km_engine_get_key_cfg(const km_engine_t *e, unsigned slot, km_engine_key_cfg_t *cfg);
/* Disable / re-enable a key slot (a disabled slot is never evaluated and is forced up). Only slots with a key. */
bool km_engine_set_slot_enabled(km_engine_t *e, unsigned slot, bool enabled);
/* Raise (never lower) a key's baseline to `counts`; false = refused (down, not evaluated, assumed, fault, not higher, beyond the drift limit). */
bool km_engine_raise_baseline(km_engine_t *e, unsigned slot, uint16_t counts);
/* Load a stored bottom reading (counts); clamped to KM_ENGINE_MIN_SPAN..KM_ENGINE_MAX_SPAN below the baseline. */
bool km_engine_set_bottom(km_engine_t *e, unsigned slot, uint16_t bottom);

#endif /* KM_ENGINE_H */
