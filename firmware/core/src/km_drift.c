/* km_drift.c - see km_drift.h and DRIFT_NOTES.md. Original work. */
#include "km/km_drift.h"

#include <string.h>

static int32_t shift_of(int32_t off)
{
    return (off * (int32_t)KM_DRIFT_GAIN_PCT) / 100;
}

static uint16_t clamp_u16(int32_t v)
{
    return v < 0 ? (uint16_t)0 : (v > 0xFFFF ? (uint16_t)0xFFFFu : (uint16_t)v);
}

void km_drift_init(km_drift_t *d)
{
    memset(d, 0, sizeof *d);
    d->sec = 0xFFFFFFFFu;
}

/* put every armed key's learned bottom back, forget the keys */
static void release_all(km_drift_t *d, km_engine_t *e)
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        km_drift_key_t *k = &d->k[s];
        if (k->armed && k->off != 0 && e->calibrated) {
            (void)km_engine_set_bottom(e, s, k->raw);
        }
    }
    memset(d->k, 0, sizeof d->k);
}

void km_drift_set_enabled(km_drift_t *d, km_engine_t *e, bool on)
{
    if (on == d->enabled) {
        return;
    }
    if (!on) {
        release_all(d, e);
    }
    d->enabled = on;
}

static void tick(km_drift_t *d, km_engine_t *e, uint32_t now_s)
{
    const bool frozen = e->n_down != 0u || (int32_t)(now_s - d->busy_until) < 0;
    if (frozen) {
        d->freezes++;
    }
    for (unsigned i = 0; i < e->n_active; i++) {
        const unsigned s = e->active[i];
        km_drift_key_t *k = &d->k[s];
        if (km_engine_slot_status(e, s) != KM_ENGINE_SLOT_OK) {
            k->armed = 0;
            k->still = 0;
            continue;
        }
        const int32_t base = km_engine_baseline(e, s);
        const uint16_t bot = km_engine_bottom(e, s);
        if (!k->armed) {
            k->ref_base = (uint16_t)base;
            k->raw = bot;
            k->last = bot;
            k->off = 0;
            k->still = 0;
            k->armed = 1;
            continue;
        }
        if (bot != k->last) {   /* the engine learned a deeper bottom, or a stored stroke was applied: adopt it, do not fight it */
            k->raw = clamp_u16((int32_t)k->raw + (int32_t)bot - (int32_t)k->last);
            k->last = bot;
        }
        const uint16_t filt = km_engine_filtered(e, s);
        const int32_t dev = (int32_t)filt - base;
        const int32_t moved = (int32_t)filt - (int32_t)k->prev;
        k->prev = filt;
        if (km_engine_slot_down(e, s) || dev > KM_DRIFT_STILL_BAND || dev < -KM_DRIFT_STILL_BAND) {
            k->still = 0;
        } else if (k->still < 255u) {
            k->still++;
        }
        if (km_engine_slot_down(e, s) || dev <= KM_DRIFT_STILL_BAND || moved > KM_DRIFT_STILL_BAND || moved < -KM_DRIFT_STILL_BAND) {
            k->high = 0;
        } else if (k->high < 255u) {
            k->high++;
        }
        if (!frozen && k->high >= KM_DRIFT_RAISE_S && ((now_s + s) % KM_DRIFT_STEP_S) == 0u) {
            (void)km_engine_raise_baseline(e, s, (uint16_t)(base + 1));
            continue;
        }
        if (frozen || k->still < KM_DRIFT_STILL_S || ((now_s + s) % KM_DRIFT_STEP_S) != 0u) {
            continue;
        }
        int32_t want = base - (int32_t)k->ref_base;
        want = want > KM_DRIFT_MAX_OFF ? KM_DRIFT_MAX_OFF : (want < -KM_DRIFT_MAX_OFF ? -KM_DRIFT_MAX_OFF : want);
        if (want - k->off < KM_DRIFT_DEADBAND && k->off - want < KM_DRIFT_DEADBAND) {
            continue;           /* a count or two of noise in the baseline is not drift */
        }
        const int32_t off = (int32_t)k->off + (want > k->off ? 1 : -1);
        const int32_t nb = (int32_t)k->raw + shift_of(off);
        if (nb < 0 || nb > base - (int32_t)KM_ENGINE_MIN_SPAN || nb < base - (int32_t)KM_ENGINE_MAX_SPAN) {
            continue;           /* would leave the engine's allowed stroke: leave the key as it is */
        }
        if (km_engine_set_bottom(e, s, (uint16_t)nb)) {
            k->off = (int16_t)off;
            k->last = km_engine_bottom(e, s);
            k->updates++;
            d->total_updates++;
        }
    }
}

void km_drift_pass(km_drift_t *d, km_engine_t *e, uint32_t now_s)
{
    if (!d->enabled) {
        return;
    }
    if (!e->calibrated) {               /* re-calibration under way: forget every key, they are armed again from the new calibration */
        d->seen_cal = false;
        return;
    }
    if (!d->seen_cal) {
        memset(d->k, 0, sizeof d->k);
        d->seen_cal = true;
    }
    if (e->n_down != d->prev_down || e->glitches != d->prev_glitch) {    /* a press / release / glitch: typing is going on */
        d->prev_down = e->n_down;
        d->prev_glitch = e->glitches;
        d->busy_until = now_s + KM_DRIFT_BUSY_S;
    }
    if (now_s == d->sec) {
        return;
    }
    d->sec = now_s;
    tick(d, e, now_s);
}

void km_drift_pack(const km_drift_t *d, const km_engine_t *e, unsigned rotation, uint16_t out[KM_DRIFT_SCOPE_VALUES])
{
    unsigned armed = 0, big = 0;
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (d->k[s].armed) {
            armed++;
            const unsigned a = (unsigned)(d->k[s].off < 0 ? -d->k[s].off : d->k[s].off);
            if (a > big) {
                big = a;
            }
        }
    }
    unsigned slot = 0;
    if (e->n_active != 0u) {
        slot = e->active[rotation % e->n_active];
    }
    const km_drift_key_t *k = &d->k[slot];
    out[0] = (uint16_t)((d->enabled ? 1u : 0u) | (((int32_t)(d->sec - d->busy_until) < 0) ? 2u : 0u) | (armed << 8));
    out[1] = (uint16_t)slot;
    out[2] = (uint16_t)k->off;
    out[3] = k->updates;
    out[4] = k->still;
    out[5] = (uint16_t)big;
    out[6] = (uint16_t)d->total_updates;
    out[7] = (uint16_t)d->freezes;
}
