/*
 * km_engine.c - our own key engine (M4 task T-B). Design and tunables: see km_engine.h.
 *
 * Hot path (km_engine_update): per active key slot one load, a Q4 low-pass, two compares and the
 * debounce counter; no division, no call (a rapid-trigger key calls the depth model only on a stroke that is
 * long enough to reach its trigger distance, see rapid_up()). Everything that needs the depth model (bisection for the
 * threshold counts) runs off the hot path: at calibration, and afterwards for at most one slot per frame
 * when its baseline / bottom / trigger settings changed.
 *
 * Shifts of negative values rely on arithmetic right shift (gcc and clang for ARM and the host do).
 */
#include "km/km_engine.h"

#include <string.h>

#include "km/km_depth_model.h"
#include "km/km_keymap.h"

#define F_PRESENT  0x01u  /* a key sits on this slot (km_keymap) */
#define F_ACTIVE   0x02u  /* evaluated every frame */
#define F_DOWN     0x04u
#define F_ASSUMED  0x08u  /* baseline guessed at calibration */
#define F_FAULT    0x10u  /* reads below KM_ENGINE_FAULT_LOW */
#define F_DIRTY    0x20u  /* thresholds need a recompute */
#define F_BADIDLE  0x40u  /* implausible level at calibration */
#define F_USEROFF  0x80u  /* disabled by the caller */

#define FAULT_Q4        (KM_ENGINE_FAULT_LOW << 4)
#define DRIFT_WINDOW_Q4 (KM_ENGINE_DRIFT_WINDOW << 4)
#define ASSUMED_WIN_Q4  (260u << 4)     /* assumed baselines may be this far off and still converge */
#define ASSUMED_LIMIT   400u            /* ... and may wander this far from the guess */
#define ASSUMED_SETTLED_Q4 (12u << 4)   /* converged: the value sits this close to the baseline (noise included) ... */
#define ASSUMED_SETTLE_TICKS 32u        /* ... net of misses, for this many drift ticks (512 frames) */
#define RT_DIR_TOL_Q4   (8 << 4)        /* rapid trigger: the fast filter may disagree with the slow one by this much (noise) */
#define EXT_NONE        0xFFFFu         /* rapid trigger: no extreme known yet, the next evaluation takes the current depth */

#if KM_ENGINE_FILTER_MODES
/*
 * Adaptive gain (FILTER_NOTES.md): a stage whose input is within noise of its output (|diff| < T1 counts) keeps the slow gain 1/8, a stage that
 * is being pulled farther than T2 counts (a stroke, not noise) takes 1/2. So the noise is averaged as before while a real movement is followed
 * about as fast as the first stage allows. The same statements as the lab candidates F1B / F1C of tests/filter_lab.c (proved equal by
 * tests/test_filter_traces.c). Not compiled in unless KM_ENGINE_FILTER_MODES = 1.
 */
static inline unsigned adapt_shift(int32_t diff_q4, unsigned mode)
{
    const int32_t ad = diff_q4 < 0 ? -diff_q4 : diff_q4;
    const int32_t t1 = (int32_t)((mode == KM_ENGINE_FMODE_FAST ? KM_ENGINE_FAST_T1 : KM_ENGINE_ADAPT_T1) << 4);
    const int32_t t2 = (int32_t)((mode == KM_ENGINE_FMODE_FAST ? KM_ENGINE_FAST_T2 : KM_ENGINE_ADAPT_T2) << 4);
    return ad < t1 ? 3u : ad < t2 ? 2u : 1u;
}

static inline int32_t adapt_stage1(int32_t f, int32_t x_q4, unsigned mode)
{
    return f + ((x_q4 - f) >> adapt_shift(x_q4 - f, mode));
}

/* the rapid-trigger stage: mode 1 has none (g follows f), mode 2 adapts its gain */
static inline int32_t adapt_stage2(unsigned mode, int32_t g, int32_t f)
{
    if (mode == KM_ENGINE_FMODE_LOW_LATENCY || mode == KM_ENGINE_FMODE_FAST) {
        return f;
    }
    const unsigned sh = adapt_shift(f - g, mode);
    return g + (((f - g) + (1 << (sh - 1u))) >> sh);
}
#endif

/* ---------- threshold computation (cold) ---------- */

/*
 * Largest reading v in [bottom, released] whose depth is >= depth_mm100 (the model never increases
 * with the reading). bottom - 1 when even the bottom reading is shallower than that.
 */
static int32_t edge_value(const km_depth_cal_t *cal, uint16_t depth_mm100)
{
    int32_t lo = cal->bottom;
    int32_t hi = cal->released;
    if (km_depth_model_mm100(cal, (uint16_t)lo) < depth_mm100) {
        return lo - 1;
    }
    while (lo < hi) {
        const int32_t mid = (lo + hi + 1) >> 1;
        if (km_depth_model_mm100(cal, (uint16_t)mid) >= depth_mm100) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static uint16_t baseline_counts(const km_engine_slot_t *k)
{
    return (uint16_t)((k->base_q16 + 0x8000u) >> 16);
}

/* Calibration endpoints with the bottom forced into the allowed span. */
static km_depth_cal_t make_cal(const km_engine_slot_t *k)
{
    km_depth_cal_t cal;
    const uint16_t rel = baseline_counts(k);
    uint16_t bot = k->bottom;
    if (rel < KM_ENGINE_MIN_SPAN) {
        bot = 0;
    } else if (bot > rel - KM_ENGINE_MIN_SPAN) {
        bot = (uint16_t)(rel - KM_ENGINE_MIN_SPAN);
    }
    if (rel > KM_ENGINE_MAX_SPAN && bot < rel - KM_ENGINE_MAX_SPAN) {
        bot = (uint16_t)(rel - KM_ENGINE_MAX_SPAN);
    }
    cal.released = rel;
    cal.bottom = bot;
    return cal;
}

/* a pressed key reading below this (Q4) has gone deeper than the known bottom */
static uint16_t learn_threshold_q4(uint16_t bottom)
{
    if (bottom <= KM_ENGINE_LEARN_MARGIN) {
        return 0;
    }
    return (uint16_t)((bottom - KM_ENGINE_LEARN_MARGIN) << 4);
}

/* the top dead zone as the engine uses it: never thinner than the noise floor */
static uint16_t top_dead_eff(const km_engine_key_cfg_t *c)
{
    return c->top_dead_mm100 != 0u ? c->top_dead_mm100 : (uint16_t)KM_ENGINE_RT_MIN_MM100;
}

/* "reading <= edge" as a threshold on the Q4 filter value: filt_q4 <= result */
static uint16_t le_q4(int32_t edge)
{
    const int32_t q = (edge + 1) * 16 - 1;
    return q < 0 ? (uint16_t)0 : (uint16_t)q;
}

/*
 * Fewest counts a stroke of `dist_mm100` can take, whatever its starting point: the depth curve is concave and
 * rises most at rest, so a depth difference of `dist` needs at least as many counts as the depth `dist - 1`
 * (rounding) takes from rest. Rapid trigger only calls the depth model once a stroke is this long.
 */
static uint16_t stroke_counts(const km_depth_cal_t *cal, uint16_t dist_mm100)
{
    const int32_t m = (int32_t)cal->released - edge_value(cal, (uint16_t)(dist_mm100 - 1u));
    return m < 1 ? (uint16_t)1 : (uint16_t)(m > 0xFFFF ? 0xFFFF : m);
}

static void recompute(km_engine_slot_t *k)
{
    const km_depth_cal_t cal = make_cal(k);
    if (!km_engine_cfg_valid(&k->cfg)) {
        km_engine_key_cfg_default(&k->cfg);     /* never run on a corrupt config */
    }
    if (k->cfg.mode == KM_ENGINE_TRIG_RAPID) {
        /*
         * press_q4: reading deeper than the top dead zone (depth > top_dead). release_q4: inside it by
         * KM_ENGINE_RT_EDGE_HYST_MM100 (depth <= top_dead - hysteresis), so that a finger resting on the
         * edge can neither press and release by noise nor chatter. rest_q4: depth below
         * KM_ENGINE_RT_REST_MM100. bz_q4: depth >= travel - bottom_dead.
         */
        const uint16_t td = top_dead_eff(&k->cfg);
        k->press_q4 = le_q4(edge_value(&cal, (uint16_t)(td + 1u)));
        k->release_q4 = (uint16_t)(le_q4(edge_value(&cal, (uint16_t)(td - KM_ENGINE_RT_EDGE_HYST_MM100 + 1u))) + 1u);
        k->rest_q4 = le_q4(edge_value(&cal, KM_ENGINE_RT_REST_MM100));
        k->bz_q4 = le_q4(edge_value(&cal, (uint16_t)(KM_TRAVEL_MM100 - k->cfg.bottom_dead_mm100)));
        k->mc_dn = stroke_counts(&cal, k->cfg.rt_down_mm100);
        k->mc_up = stroke_counts(&cal, k->cfg.rt_up_mm100);
    } else {
        const int32_t ep = edge_value(&cal, k->cfg.press_mm100);
        const int32_t er = edge_value(&cal, (uint16_t)(k->cfg.release_mm100 + 1u));
        int32_t press = (ep + 1) * 16 - 1;      /* filt_q4 <= press  <=>  reading <= ep */
        int32_t release = (er + 1) * 16;        /* filt_q4 >= release <=> depth <= release_mm100 */
        if (press < 0) {
            press = 0;
        }
        if (release < press + 64) {             /* degenerate model: keep at least 4 counts of hysteresis */
            release = press + 64;
        }
        if (release > 0xFFFF) {
            release = 0xFFFF;
        }
        k->press_q4 = (uint16_t)press;
        k->release_q4 = (uint16_t)release;
    }
    k->bottom = cal.bottom;
    k->learn_q4 = learn_threshold_q4(cal.bottom);
    k->thr_base = cal.released;
}

static void mark_dirty(km_engine_t *e, km_engine_slot_t *k)
{
    if (!(k->flags & F_DIRTY)) {
        k->flags |= F_DIRTY;
        e->dirty++;
    }
}

/*
 * Recompute the thresholds of the next waiting slot (round robin), at most one per frame. The search for the slot is a plain
 * increment with wrap (no 32-bit modulo per step: the farthest dirty slot costs 61 cheap steps, not 61 divisions).
 */
static void refresh_one(km_engine_t *e)
{
    unsigned i = e->cursor < e->n_active ? e->cursor : 0u;
    for (unsigned n = 0; n < e->n_active; n++, i = (i + 1u < e->n_active) ? i + 1u : 0u) {
        km_engine_slot_t *k = &e->slot[e->active[i]];
        if (k->flags & F_DIRTY) {
            recompute(k);
            k->flags = (uint8_t)(k->flags & ~F_DIRTY);
            e->dirty--;
            e->cursor = (uint8_t)((i + 1u < e->n_active) ? i + 1u : 0u);
            return;
        }
    }
    e->dirty = 0;  /* nothing found: the counter was stale (slot deactivated while dirty) */
}

static void rebuild_active(km_engine_t *e)
{
    e->n_active = 0;
    e->dirty = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        km_engine_slot_t *k = &e->slot[s];
        if (k->flags & F_ACTIVE) {
            e->active[e->n_active++] = (uint8_t)s;
            if (k->flags & F_DIRTY) {
                e->dirty++;
            }
        } else {
            k->flags = (uint8_t)(k->flags & ~F_DIRTY);
        }
    }
    e->cursor = 0;
}

/* ---------- pressed list ---------- */

static void down_remove(km_engine_t *e, unsigned slot)
{
    for (unsigned i = 0; i < e->n_down; i++) {
        if (e->down[i] == slot) {
            memmove(&e->down[i], &e->down[i + 1], (size_t)(e->n_down - i - 1u));
            e->n_down--;
            return;
        }
    }
}

static void force_up(km_engine_t *e, unsigned slot)
{
    km_engine_slot_t *k = &e->slot[slot];
    if (k->flags & F_DOWN) {
        k->flags = (uint8_t)(k->flags & ~F_DOWN);
        down_remove(e, slot);
    }
    k->cnt = 0;
    k->ext = EXT_NONE;
}

/* ---------- init / config ---------- */

void km_engine_key_cfg_default(km_engine_key_cfg_t *cfg)
{
    cfg->press_mm100 = KM_ENGINE_DEFAULT_PRESS_MM100;
    cfg->release_mm100 = KM_ENGINE_DEFAULT_RELEASE_MM100;
    cfg->mode = KM_ENGINE_TRIG_FIXED;
    cfg->rt_down_mm100 = KM_ENGINE_DEFAULT_RT_MM100;
    cfg->rt_up_mm100 = KM_ENGINE_DEFAULT_RT_MM100;
    cfg->top_dead_mm100 = 0;
    cfg->bottom_dead_mm100 = 0;
    cfg->continuous = 0;
}

/* a rapid-trigger distance: not below the noise floor, not more than the whole travel */
static bool rt_dist_ok(uint16_t v)
{
    return v >= KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;
}

/* a dead zone: 0 = none / minimum, otherwise a real distance */
static bool dead_ok(uint16_t v)
{
    return v == 0u || rt_dist_ok(v);
}

bool km_engine_cfg_valid(const km_engine_key_cfg_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->mode == KM_ENGINE_TRIG_FIXED) {
        return cfg->release_mm100 >= KM_ENGINE_CFG_MIN_RELEASE_MM100 && cfg->release_mm100 < cfg->press_mm100 &&
               cfg->press_mm100 <= KM_TRAVEL_MM100;
    }
    if (cfg->mode != KM_ENGINE_TRIG_RAPID) {
        return false;
    }
    if (!rt_dist_ok(cfg->rt_down_mm100) || !rt_dist_ok(cfg->rt_up_mm100) || !dead_ok(cfg->top_dead_mm100) ||
        !dead_ok(cfg->bottom_dead_mm100) || cfg->continuous > 1u) {
        return false;
    }
    if (cfg->continuous == 0u && !rt_dist_ok(cfg->press_mm100)) {
        return false;                           /* the actuation point is used */
    }
    if (cfg->rt_down_mm100 > KM_TRAVEL_MM100 - cfg->bottom_dead_mm100) {
        return false;                           /* a press would be out of reach */
    }
    return (uint32_t)top_dead_eff(cfg) + cfg->bottom_dead_mm100 + KM_ENGINE_RT_MIN_MM100 <= KM_TRAVEL_MM100;
}

bool km_engine_set_filter_mode(km_engine_t *e, unsigned mode)
{
    if (mode > KM_ENGINE_FMODE_LAST) {
        return false;
    }
    if (e->filter_mode != (uint8_t)mode) {
        e->filter_mode = (uint8_t)mode;
        for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
            e->slot[s].g_q4 = e->slot[s].filt_q4;       /* the second stage restarts from the first: no stale value from the other mode */
        }
    }
    return true;
}

unsigned km_engine_get_filter_mode(const km_engine_t *e)
{
    return e->filter_mode;
}

void km_engine_set_fast_confirm(km_engine_t *e, bool on)
{
    e->fast_confirm = on ? 1u : 0u;
}

bool km_engine_get_fast_confirm(const km_engine_t *e)
{
    return e->fast_confirm != 0u;
}

void km_engine_init(km_engine_t *e)
{
    memset(e, 0, sizeof *e);
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        km_engine_key_cfg_default(&e->slot[s].cfg);
        e->slot[s].ext = EXT_NONE;
        if (km_keymap_slot_is_key(s)) {
            e->slot[s].flags = F_PRESENT;
        }
    }
}

/* A trigger change takes effect at once: thresholds recomputed now, rapid-trigger memory cleared. */
static void apply_cfg(km_engine_t *e, km_engine_slot_t *k, const km_engine_key_cfg_t *cfg)
{
    k->cfg = *cfg;
    k->ext = EXT_NONE;
    k->cnt = 0;
    if (k->flags & F_ACTIVE) {
        recompute(k);
        k->g_q4 = k->filt_q4;
        if (k->flags & F_DIRTY) {
            k->flags = (uint8_t)(k->flags & ~F_DIRTY);
            e->dirty--;
        }
    }
}

bool km_engine_set_key_cfg(km_engine_t *e, unsigned slot, const km_engine_key_cfg_t *cfg)
{
    if (slot >= KM_ENGINE_SLOTS || !km_engine_cfg_valid(cfg)) {
        return false;
    }
    apply_cfg(e, &e->slot[slot], cfg);
    return true;
}

bool km_engine_apply_key_cfg(km_engine_t *e, unsigned slot, const km_engine_key_cfg_t *cfg)
{
    if (slot >= KM_ENGINE_SLOTS) {
        return false;
    }
    if (km_engine_cfg_valid(cfg)) {
        apply_cfg(e, &e->slot[slot], cfg);
        return true;
    }
    km_engine_key_cfg_t def;
    km_engine_key_cfg_default(&def);
    apply_cfg(e, &e->slot[slot], &def);
    return false;
}

bool km_engine_get_key_cfg(const km_engine_t *e, unsigned slot, km_engine_key_cfg_t *cfg)
{
    if (slot >= KM_ENGINE_SLOTS) {
        return false;
    }
    *cfg = e->slot[slot].cfg;
    return true;
}

bool km_engine_set_slot_enabled(km_engine_t *e, unsigned slot, bool enabled)
{
    if (slot >= KM_ENGINE_SLOTS || !(e->slot[slot].flags & F_PRESENT)) {
        return false;
    }
    km_engine_slot_t *k = &e->slot[slot];
    if (!enabled) {
        force_up(e, slot);
        k->flags = (uint8_t)((k->flags | F_USEROFF) & ~(F_ACTIVE | F_DIRTY));
    } else {
        k->flags = (uint8_t)(k->flags & ~F_USEROFF);
        if (e->calibrated && !(k->flags & F_BADIDLE)) {
            k->filt_q4 = (uint16_t)(k->base_q16 >> 12);
            k->g_q4 = k->filt_q4;
            k->ext = EXT_NONE;
            k->cnt = 0;
            k->flags = (uint8_t)(k->flags | F_ACTIVE | F_DIRTY);
        }
    }
    rebuild_active(e);
    return true;
}

bool km_engine_set_bottom(km_engine_t *e, unsigned slot, uint16_t bottom)
{
    if (slot >= KM_ENGINE_SLOTS || !(e->slot[slot].flags & F_PRESENT)) {
        return false;
    }
    km_engine_slot_t *k = &e->slot[slot];
    k->bottom = bottom;
    if (k->flags & F_ACTIVE) {
        mark_dirty(e, k);
    }
    return true;
}

/* ---------- calibration ---------- */

void km_engine_calibrate_begin(km_engine_t *e)
{
    memset(&e->cal, 0, sizeof e->cal);
}

void km_engine_calibrate_idle(km_engine_t *e, const uint16_t sums_div16[KM_ENGINE_SLOTS])
{
    if (e->cal.frames == 0xFFFFu) {
        return;
    }
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        const uint16_t v = sums_div16[s] > 4095u ? 4095u : sums_div16[s];
        if (e->cal.frames == 0 || v < e->cal.lo[s]) {
            e->cal.lo[s] = v;
        }
        if (e->cal.frames == 0 || v > e->cal.hi[s]) {
            e->cal.hi[s] = v;
        }
        e->cal.acc[s] += v;
    }
    e->cal.frames++;
}

km_engine_cal_result_t km_engine_calibrate_finish(km_engine_t *e)
{
    const uint32_t n = e->cal.frames;
    uint16_t mean[KM_ENGINE_SLOTS];
    uint16_t sorted[KM_ENGINE_SLOTS];
    unsigned n_ok = 0;

    e->calibrated = false;
    if (n < KM_ENGINE_CAL_MIN_FRAMES) {
        return KM_ENGINE_CAL_TOO_FEW_FRAMES;
    }

    /* plausible key slots and their median level (insertion sort, cold path) */
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        mean[s] = (uint16_t)((e->cal.acc[s] + n / 2u) / n);
        if ((e->slot[s].flags & F_PRESENT) && mean[s] >= KM_ENGINE_FAULT_LOW && mean[s] <= KM_ENGINE_VALUE_MAX) {
            unsigned j = n_ok++;
            while (j > 0 && sorted[j - 1] > mean[s]) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = mean[s];
        }
    }
    if (n_ok < KM_ENGINE_CAL_MIN_SLOTS) {
        return KM_ENGINE_CAL_IMPLAUSIBLE;
    }
    const uint16_t med = sorted[n_ok / 2u];
    if (med < KM_ENGINE_CAL_IDLE_MIN) {
        return KM_ENGINE_CAL_KEYS_HELD;  /* most keys read as pressed */
    }

    unsigned held = 0;
    unsigned moving = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        const bool usable = (e->slot[s].flags & F_PRESENT) && mean[s] >= KM_ENGINE_FAULT_LOW && mean[s] <= KM_ENGINE_VALUE_MAX;
        if (!usable) {
            continue;
        }
        if (mean[s] + KM_ENGINE_CAL_HELD_DROP < med) {
            held++;
        } else if ((unsigned)(e->cal.hi[s] - e->cal.lo[s]) > KM_ENGINE_CAL_NOISE_MAX) {
            moving++;
        }
    }
    if (held > KM_ENGINE_CAL_MAX_BAD) {
        return KM_ENGINE_CAL_KEYS_HELD;
    }
    if (moving > KM_ENGINE_CAL_MAX_BAD) {
        return KM_ENGINE_CAL_UNSTABLE;
    }

    e->n_down = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        km_engine_slot_t *k = &e->slot[s];
        k->flags = (uint8_t)(k->flags & (F_PRESENT | F_USEROFF));
        k->cnt = 0;
        if (!(k->flags & F_PRESENT)) {
            continue;
        }
        const bool usable = mean[s] >= KM_ENGINE_FAULT_LOW && mean[s] <= KM_ENGINE_VALUE_MAX;
        if (!usable) {
            k->flags |= F_BADIDLE;
            continue;
        }
        const bool bad = mean[s] + KM_ENGINE_CAL_HELD_DROP < med || (unsigned)(e->cal.hi[s] - e->cal.lo[s]) > KM_ENGINE_CAL_NOISE_MAX;
        const uint16_t base = bad ? med : mean[s];
        k->base_q16 = (uint32_t)base << 16;
        k->base0 = base;
        k->filt_q4 = (uint16_t)(base << 4);
        k->g_q4 = k->filt_q4;
        k->ext = EXT_NONE;
        k->bottom = (uint16_t)(base - KM_ENGINE_DEFAULT_SPAN);
        recompute(k);
        if (bad) {
            k->flags |= F_ASSUMED;
        }
        if (!(k->flags & F_USEROFF)) {
            k->flags |= F_ACTIVE;
        }
    }
    rebuild_active(e);
    e->calibrated = true;
    e->frame = 0;
    km_engine_calibrate_begin(e);
    return KM_ENGINE_CAL_OK;
}

/* ---------- per-frame update ---------- */

/*
 * Rare: a reading below any real bottom, or a faulted slot reading again. true = skip the slot this frame.
 * A faulted slot is only re-armed once it reads like a released key (above the release threshold): on its
 * way back from a floating level it passes through the pressed range and must not press anything.
 */
static bool fault_step(km_engine_t *e, unsigned slot, km_engine_slot_t *k, int32_t f)
{
    if (f < (int32_t)FAULT_Q4) {
        k->flags |= F_FAULT;
        force_up(e, slot);
        return true;
    }
    if (f < (int32_t)k->release_q4) {
        return true;
    }
    k->flags = (uint8_t)(k->flags & ~F_FAULT);
    k->cnt = 0;
    k->g_q4 = (uint16_t)f;
    return false;
}

/* Slow baseline tracking; only called every KM_ENGINE_DRIFT_PERIOD frames for a key that is up. */
static void drift_step(km_engine_t *e, km_engine_slot_t *k, int32_t f)
{
    const bool assumed = (k->flags & F_ASSUMED) != 0;
    const int32_t win = assumed ? (int32_t)ASSUMED_WIN_Q4 : (int32_t)DRIFT_WINDOW_Q4;
    const int32_t base = (int32_t)k->base_q16;
    const int32_t d = f - (base >> 12);
    if (d < -win || d > win) {
        k->settle = 0;
        return;
    }
    const int32_t limit = (int32_t)(assumed ? ASSUMED_LIMIT : KM_ENGINE_DRIFT_LIMIT) << 16;
    const int32_t centre = (int32_t)k->base0 << 16;
    int32_t b = base + (((f << 12) - base) >> KM_ENGINE_DRIFT_SHIFT);
    if (b > centre + limit) {
        b = centre + limit;
    } else if (b < centre - limit) {
        b = centre - limit;
    }
    k->base_q16 = (uint32_t)b;
    if (assumed) {
        /* converged only after sitting on the baseline for a while: the key passes through it on its way up */
        if (d >= -(int32_t)ASSUMED_SETTLED_Q4 && d <= (int32_t)ASSUMED_SETTLED_Q4) {
            if (++k->settle >= ASSUMED_SETTLE_TICKS) {
                k->flags = (uint8_t)(k->flags & ~F_ASSUMED);
                k->base0 = baseline_counts(k);
            }
        } else if (k->settle > 0) {
            k->settle--;
        }
    }
    const int32_t moved = (int32_t)baseline_counts(k) - (int32_t)k->thr_base;
    if (moved >= 2 || moved <= -2) {
        mark_dirty(e, k);
    }
}

/* A pressed key reads deeper than the known bottom: lower the bottom (the first full press teaches the stroke). */
static void learn_step(km_engine_t *e, km_engine_slot_t *k, int32_t f)
{
    int32_t b = f >> 4;
    const int32_t floor_b = (int32_t)baseline_counts(k) - (int32_t)KM_ENGINE_MAX_SPAN;
    if (b < floor_b) {
        b = floor_b;
    }
    if (b < 0) {
        b = 0;
    }
    k->bottom = (uint16_t)b;
    k->learn_q4 = learn_threshold_q4((uint16_t)b);
    mark_dirty(e, k);
}

/*
 * Fixed actuation point with hysteresis (km_engine_trig_mode_t KM_ENGINE_TRIG_FIXED). This is the one
 * place a different trigger (rapid trigger) replaces: it gets the filtered reading and decides press /
 * release. New presses are only queued in newly[]; the caller applies the glitch guard to the batch.
 */
static inline void trigger_fixed(km_engine_t *e, unsigned s, km_engine_slot_t *k, int32_t f, bool drift_tick,
                                 uint8_t *newly, unsigned *nn)
{
    if (!(k->flags & F_DOWN)) {
        if (f <= (int32_t)k->press_q4) {
            if (++k->cnt >= KM_ENGINE_PRESS_FRAMES) {
                k->cnt = 0;
                k->flags |= F_DOWN;
                newly[(*nn)++] = (uint8_t)s;
            }
        } else {
            k->cnt = 0;
            if (drift_tick) {
                drift_step(e, k, f);
            }
        }
    } else if (f >= (int32_t)k->release_q4) {
        if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {
            k->cnt = 0;
            k->flags = (uint8_t)(k->flags & ~F_DOWN);
            down_remove(e, s);
        }
    } else {
        k->cnt = 0;
        if (f < (int32_t)k->learn_q4) {
            learn_step(e, k, f);
        }
    }
}

/*
 * Rapid trigger (KM_ENGINE_TRIG_RAPID). Works on g, a second low-pass stage behind the filter of the main loop:
 * the extremes it remembers would otherwise pick up the noise peaks (see ENGINE_NOTES.md for the derivation).
 * A key that reads clearly at rest (filt_q4 above rest_q4) costs one compare, one store of g and the reset of
 * its memory. The memory of the stroke (`ext`) is a reading, not a depth: the depth model never increases with
 * the reading, so the shallowest depth is the depth of the highest reading and the deepest of the lowest. The model
 * runs only when the stroke from that extreme is long enough to reach rt_down / rt_up at all (mc_dn / mc_up);
 * a key that is held, or wiggles by noise, costs a compare.
 */
static inline uint16_t rapid_depth(const km_engine_slot_t *k, unsigned counts)
{
    const km_depth_cal_t cal = { k->thr_base, k->bottom };
    return km_depth_model_mm100(&cal, (uint16_t)counts);
}

static void rapid_up(km_engine_t *e, unsigned s, km_engine_slot_t *k, int32_t f, int32_t g, bool drift_tick,
                     uint8_t *newly, unsigned *nn)
{
    const unsigned c = (unsigned)((g + 8) >> 4);
    unsigned top = k->ext;                      /* highest reading so far = shallowest point */
    if (top == EXT_NONE || c > top) {
        top = c;
        k->ext = (uint16_t)c;
    }
    bool want = false;
#if KM_ENGINE_FILTER_MODES
    unsigned need = e->filter_mode == KM_ENGINE_FMODE_FAST ? KM_ENGINE_FAST_PRESS_FRAMES : KM_ENGINE_PRESS_FRAMES;
#endif
    /* f <= g + tol: the key is still going down or holding, not coming back from a spike */
    if (g <= (int32_t)k->press_q4 && f <= g + RT_DIR_TOL_Q4 && top - c >= k->mc_dn) {
        const unsigned d = rapid_depth(k, c);
        want = d >= (unsigned)rapid_depth(k, top) + k->cfg.rt_down_mm100 &&
               (k->cfg.continuous || d >= k->cfg.press_mm100);
#if KM_ENGINE_FILTER_MODES
        if (want && e->fast_confirm && d >= (unsigned)rapid_depth(k, top) + KM_ENGINE_FAST_FACTOR * k->cfg.rt_down_mm100) {
            need = KM_ENGINE_FAST_FRAMES;
        }
#endif
    }
    if (want) {
#if KM_ENGINE_FILTER_MODES
        if (++k->cnt >= need) {
#else
        if (++k->cnt >= KM_ENGINE_PRESS_FRAMES) {
#endif
            k->cnt = 0;
            k->flags |= F_DOWN;
            k->ext = (uint16_t)c;               /* from here on: the lowest reading = the deepest point */
            newly[(*nn)++] = (uint8_t)s;
        }
    } else {
        k->cnt = 0;
        if (g > (int32_t)k->press_q4 && drift_tick) {
            drift_step(e, k, g);
        }
    }
}

static void rapid_down(km_engine_t *e, unsigned s, km_engine_slot_t *k, int32_t f, int32_t g)
{
    bool release = false;
    const bool rising = f >= g - RT_DIR_TOL_Q4; /* going up or holding, not falling back from a dip */
    if (g >= (int32_t)k->release_q4) {          /* back in the top dead zone */
        if (!rising) {
            k->cnt = 0;
        } else if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {
            k->ext = (uint16_t)((k->release_q4 + 8u) >> 4);  /* the edge of the zone, KM_ENGINE_RT_EDGE_HYST_MM100 inside */
            release = true;
        }
    } else if (g <= (int32_t)k->bz_q4) {        /* bottom dead zone: counts as fully pressed */
        k->cnt = 0;
        if (f < (int32_t)k->learn_q4) {
            learn_step(e, k, f);
        }
        k->ext = k->bottom;                     /* the deepest point there is */
    } else {
        const unsigned c = (unsigned)((g + 8) >> 4);
        unsigned low = k->ext;                  /* lowest reading so far = deepest point (EXT_NONE: none yet) */
        if (c < low) {
            low = c;
            k->ext = (uint16_t)c;
        }
        if (rising && c - low >= k->mc_up &&
            (unsigned)rapid_depth(k, low) - rapid_depth(k, c) >= k->cfg.rt_up_mm100) {
#if KM_ENGINE_FILTER_MODES
            unsigned need = e->filter_mode == KM_ENGINE_FMODE_FAST ? KM_ENGINE_FAST_RELEASE_FRAMES : KM_ENGINE_RELEASE_FRAMES;
            if (e->fast_confirm && (unsigned)rapid_depth(k, low) - rapid_depth(k, c) >= KM_ENGINE_FAST_FACTOR * k->cfg.rt_up_mm100) {
                need = KM_ENGINE_FAST_FRAMES;
            }
            if (++k->cnt >= need) {
#else
            if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {
#endif
                k->ext = (uint16_t)c;           /* from here on: the highest reading = the shallowest point */
                release = true;
            }
        } else {
            k->cnt = 0;
            if (f < (int32_t)k->learn_q4) {
                learn_step(e, k, f);
            }
        }
    }
    if (release) {
        k->cnt = 0;
        k->flags = (uint8_t)(k->flags & ~F_DOWN);
        down_remove(e, s);
    }
}

/* everything except the common case "up and clearly at rest" (kept out of the hot loop) */
static void rapid_general(km_engine_t *e, unsigned s, km_engine_slot_t *k, int32_t f, bool drift_tick, uint8_t *newly,
                          unsigned *nn)
{
    int32_t g = f;
    if (f <= (int32_t)k->rest_q4) {
        g = k->g_q4;
#if KM_ENGINE_FILTER_MODES
        if (e->filter_mode != KM_ENGINE_FMODE_FIXED) {
            g = adapt_stage2(e->filter_mode, g, f);
        } else
#endif
        g += ((f - g) + (1 << (KM_ENGINE_RT_FILTER_SHIFT - 1u))) >> KM_ENGINE_RT_FILTER_SHIFT;
    }
    k->g_q4 = (uint16_t)g;
    if (k->flags & F_DOWN) {
        rapid_down(e, s, k, f, g);
    } else if (g > (int32_t)k->rest_q4) {
        k->cnt = 0;
        k->ext = k->thr_base;                   /* at rest: the top of the travel */
        if (drift_tick) {
            drift_step(e, k, g);
        }
    } else {
        rapid_up(e, s, k, f, g, drift_tick, newly, nn);
    }
}

static inline void trigger_rapid(km_engine_t *e, unsigned s, km_engine_slot_t *k, int32_t f, bool drift_tick,
                                 uint8_t *newly, unsigned *nn)
{
    if (f > (int32_t)k->rest_q4 && !(k->flags & F_DOWN)) {
        k->g_q4 = (uint16_t)f;                  /* the slow stage follows the fast one at rest */
        k->cnt = 0;
        k->ext = k->thr_base;
        if (drift_tick) {
            drift_step(e, k, f);
        }
        return;
    }
    rapid_general(e, s, k, f, drift_tick, newly, nn);
}

void km_engine_update(km_engine_t *e, const uint16_t sums_div16[KM_ENGINE_SLOTS])
{
    if (!e->calibrated) {
        return;
    }
    /*
     * Every key still looks at its baseline once per KM_ENGINE_DRIFT_PERIOD frames, but not all in the same frame: slot s is looked at when
     * (frame + s) % period == 0, about 4 keys per frame instead of all 61 on every 16th (that frame cost twice a normal one and, when
     * it met a threshold recompute, came close to the whole frame budget; core/STABILITY_NOTES.md section 5).
     */
    const uint32_t frame_no = e->frame++;
    uint8_t newly[KM_ENGINE_SLOTS];
    unsigned nn = 0;
#if KM_ENGINE_FILTER_MODES
    const unsigned fmode = e->filter_mode;
#endif

    for (unsigned i = 0; i < e->n_active; i++) {
        const unsigned s = e->active[i];
        km_engine_slot_t *k = &e->slot[s];
        const bool drift_tick = ((frame_no + s) & (KM_ENGINE_DRIFT_PERIOD - 1u)) == 0u;
        uint32_t v = sums_div16[s];
        if (v > 4095u) {
            v = 4095u;
        }
        int32_t f = k->filt_q4;
#if KM_ENGINE_FILTER_MODES
        if (fmode != KM_ENGINE_FMODE_FIXED) {
            f = adapt_stage1(f, (int32_t)(v << 4), fmode);
        } else
#endif
        f += ((int32_t)(v << 4) - f) >> KM_ENGINE_FILTER_SHIFT;
        k->filt_q4 = (uint16_t)f;
        if (f < (int32_t)FAULT_Q4 || (k->flags & F_FAULT)) {
            if (fault_step(e, s, k, f)) {
                continue;
            }
        }
        if (k->cfg.mode == KM_ENGINE_TRIG_RAPID) {
            trigger_rapid(e, s, k, f, drift_tick, newly, &nn);
        } else {
            trigger_fixed(e, s, k, f, drift_tick, newly, &nn);
        }
    }

    if (nn >= KM_ENGINE_GLITCH_ONSET) {
        /* not fingers: undo the whole batch, the slots have to qualify again */
        for (unsigned i = 0; i < nn; i++) {
            e->slot[newly[i]].flags = (uint8_t)(e->slot[newly[i]].flags & ~F_DOWN);
            e->slot[newly[i]].ext = e->slot[newly[i]].thr_base; /* rapid trigger: qualify again from the top */
        }
        e->glitches++;
    } else {
        for (unsigned i = 0; i < nn; i++) {
            e->down[e->n_down++] = newly[i];
        }
    }

    if (e->dirty) {
        refresh_one(e);
    }
}

/* ---------- results ---------- */

bool km_engine_slot_down(const km_engine_t *e, unsigned slot)
{
    return slot < KM_ENGINE_SLOTS && (e->slot[slot].flags & F_DOWN) != 0;
}

const uint8_t *km_engine_down_slots(const km_engine_t *e, unsigned *n)
{
    *n = e->n_down;
    return e->down;
}

uint16_t km_engine_depth(const km_engine_t *e, unsigned slot)
{
    if (slot >= KM_ENGINE_SLOTS || !(e->slot[slot].flags & F_ACTIVE)) {
        return 0;
    }
    const km_engine_slot_t *k = &e->slot[slot];
    const km_depth_cal_t cal = make_cal(k);
    return km_depth_model_mm100(&cal, (uint16_t)((k->filt_q4 + 8u) >> 4));
}

uint16_t km_engine_baseline(const km_engine_t *e, unsigned slot)
{
    return slot < KM_ENGINE_SLOTS ? baseline_counts(&e->slot[slot]) : (uint16_t)0;
}

uint16_t km_engine_bottom(const km_engine_t *e, unsigned slot)
{
    return slot < KM_ENGINE_SLOTS ? make_cal(&e->slot[slot]).bottom : (uint16_t)0;
}

uint16_t km_engine_filtered(const km_engine_t *e, unsigned slot)
{
    return slot < KM_ENGINE_SLOTS ? (uint16_t)((e->slot[slot].filt_q4 + 8u) >> 4) : (uint16_t)0;
}

km_engine_slot_status_t km_engine_slot_status(const km_engine_t *e, unsigned slot)
{
    if (slot >= KM_ENGINE_SLOTS || !(e->slot[slot].flags & F_PRESENT)) {
        return KM_ENGINE_SLOT_NO_KEY;
    }
    const uint8_t fl = e->slot[slot].flags;
    if (fl & (F_BADIDLE | F_USEROFF)) {
        return KM_ENGINE_SLOT_DISABLED;
    }
    if (!e->calibrated) {
        return KM_ENGINE_SLOT_UNCALIBRATED;
    }
    if (fl & F_FAULT) {
        return KM_ENGINE_SLOT_FAULT;
    }
    return (fl & F_ASSUMED) ? KM_ENGINE_SLOT_ASSUMED : KM_ENGINE_SLOT_OK;
}

uint32_t km_engine_glitch_count(const km_engine_t *e)
{
    return e->glitches;
}

/* Raise a key's released baseline (km_drift.c: a key that is up reads steadily above it, beyond what drift_step follows). Only upwards: a
 * reading above the baseline can never be a press, so this cannot learn a pressed position as rest. Refused for a key that is down, not
 * evaluated, still being settled (ASSUMED), or when the result would be more than KM_ENGINE_DRIFT_LIMIT above the calibration value. */
bool km_engine_raise_baseline(km_engine_t *e, unsigned slot, uint16_t counts)
{
    if (slot >= KM_ENGINE_SLOTS) {
        return false;
    }
    km_engine_slot_t *k = &e->slot[slot];
    if (!e->calibrated || !(k->flags & F_ACTIVE) || (k->flags & (F_DOWN | F_ASSUMED | F_FAULT)) || counts <= baseline_counts(k) ||
        counts > (uint32_t)k->base0 + KM_ENGINE_DRIFT_LIMIT || counts > KM_ENGINE_VALUE_MAX) {
        return false;
    }
    k->base_q16 = (uint32_t)counts << 16;
    if (!(k->flags & F_DIRTY)) {            /* = mark_dirty(), spelled out so that the compiler's inlining in the rest of this file stays as it was */
        k->flags |= F_DIRTY;
        e->dirty++;
    }
    return true;
}
