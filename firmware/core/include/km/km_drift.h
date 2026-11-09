/*
 * km_drift.h - slow calibration drift compensation (own module, default OFF). Original work.
 *
 * What the engine already does (km_engine.c drift_step): the RELEASED level of every key (its baseline) follows the sensor with a time
 * constant of ~1.4 s while the key is up and the reading is within KM_ENGINE_DRIFT_WINDOW counts, at most KM_ENGINE_DRIFT_LIMIT from the value
 * at calibration. What nothing follows is the PRESSED end of the stroke (the bottom): it is only ever lowered (learn_step), never raised, and
 * it does not move with the baseline. When the whole sensor level slides (temperature, supply), the span "baseline - bottom" the depth model
 * works with becomes wrong, and so does every actuation point in mm (DRIFT_NOTES.md quantifies it).
 *
 * This module keeps the span right. Once per second it looks at every key and, only while the key is up, the engine is calibrated and the
 * keyboard is quiet, moves the bottom by (gain % of) the baseline's displacement since the key was armed:
 *      engine bottom = learned bottom + gain/100 * clamp(baseline - baseline at arming, -KM_DRIFT_MAX_OFF, +KM_DRIFT_MAX_OFF)
 * with these protections (each one is a test):
 *   - the baseline is written in ONE case only, and only upwards (km_engine_raise_baseline): a key that is up reads steadily above it for
 *     KM_DRIFT_RAISE_S seconds (the engine's own tracker ignores readings more than KM_ENGINE_DRIFT_WINDOW away: a step of the supply level,
 *     a key that was held for hours while the sensor warmed up). A reading above the rest level can never be a press, so a rest level can
 *     not be moved towards a pressed value; the raise is 1 count per KM_DRIFT_STEP_S and stops at the engine's drift limit;
 *   - a key is followed only after it sat within KM_DRIFT_STILL_BAND counts of its baseline for KM_DRIFT_STILL_S seconds, was up the whole
 *     time, and its status is OK (a key that was held / moving at power-up is ASSUMED and is armed only when the engine has settled it);
 *   - nothing moves while any key is down, within KM_DRIFT_BUSY_S seconds of a press / release (typing bursts) or of a glitch event;
 *   - the correction moves by at most ONE count per key per KM_DRIFT_STEP_S seconds (12 counts / minute), total at most KM_DRIFT_MAX_OFF;
 *   - the bottom the engine learns by itself (a deeper press) is adopted, not fought: the learned part is tracked separately;
 *   - re-calibration (engine not calibrated) disarms everything; the keys are armed again from the new calibration;
 *   - nothing is ever written to flash by this module. km_calmgr saves the bottoms it already saves (at most once per 600 s, only when
 *     the bottom of some key is >= 16 counts deeper than at the last save); a record always holds a consistent (baseline, bottom) pair.
 * Cost: nothing in the 23 kHz frame path. The caller invokes km_drift_pass() once per main-loop pass; a pass costs a few loads, the work
 * (61 keys, no division except one by 100 per applied step) happens once per second.
 */
#ifndef KM_DRIFT_H
#define KM_DRIFT_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_engine.h"

#define KM_DRIFT_STEP_S      5u    /* a key's correction moves by at most 1 count per this many seconds */
#define KM_DRIFT_MAX_OFF     120   /* counts: the baseline displacement that is followed at most (the engine's own limit is 150) */
#define KM_DRIFT_STILL_S     30u   /* seconds a key must have sat at rest before it is followed */
#define KM_DRIFT_STILL_BAND  6     /* counts around the baseline that count as "at rest" (noise after the filter is about +-6) */
#define KM_DRIFT_DEADBAND    3     /* the correction moves only when the baseline is this far from it (noise in the baseline is 1 count)  */
#define KM_DRIFT_RAISE_S     20u   /* seconds a key must read steadily above its baseline before the baseline is raised (1 count per KM_DRIFT_STEP_S) */
#define KM_DRIFT_BUSY_S      3u    /* seconds of quiet needed after any press / release / glitch */
#ifndef KM_DRIFT_GAIN_PCT
#define KM_DRIFT_GAIN_PCT    100   /* % of the baseline's displacement that the bottom follows (100 = the whole span stays constant) */
#endif

typedef struct {
    int16_t  off;        /* the displacement being followed (baseline - baseline at arming, rate limited), counts */
    uint16_t ref_base;   /* baseline at arming */
    uint16_t raw;        /* the learned bottom without the correction */
    uint16_t last;       /* the bottom the engine reported after our last write / adoption */
    uint16_t updates;    /* how many times this key's bottom was moved */
    uint8_t  still;      /* seconds at rest, saturating */
    uint8_t  high;       /* seconds the key has read steadily above its baseline, saturating */
    uint16_t prev;       /* filtered reading one second ago */
    uint8_t  armed;
} km_drift_key_t;

typedef struct {
    km_drift_key_t k[KM_ENGINE_SLOTS];
    bool     enabled;
    bool     seen_cal;       /* the engine was calibrated at the last pass */
    uint8_t  prev_down;
    uint32_t prev_glitch;
    uint32_t sec;            /* the last second handled */
    uint32_t busy_until;     /* quiet again from this second on */
    uint32_t total_updates;
    uint32_t freezes;        /* seconds skipped because of activity */
} km_drift_t;

void km_drift_init(km_drift_t *d);
/* Off -> removes every correction (the bottoms go back to the learned values) and forgets the keys. */
void km_drift_set_enabled(km_drift_t *d, km_engine_t *e, bool on);
/* Once per main-loop pass; now_s = uptime in seconds. */
void km_drift_pass(km_drift_t *d, km_engine_t *e, uint32_t now_s);

/* ---- diagnostics (scope tag 0x0210) ---- */
#define KM_DRIFT_SCOPE_TAG 0x0210u
#define KM_DRIFT_SCOPE_VALUES 8u
/* [0] bit0 enabled, bit1 frozen now, bits 8-15 armed keys  [1] slot reported this time (rotates through the keys)  [2] its correction
 * (int16 counts)  [3] its update count  [4] its seconds at rest  [5] largest |correction| over all keys  [6] total updates (low 16 bits)
 * [7] seconds skipped because of activity (low 16 bits) */
void km_drift_pack(const km_drift_t *d, const km_engine_t *e, unsigned rotation, uint16_t out[KM_DRIFT_SCOPE_VALUES]);

#endif /* KM_DRIFT_H */
