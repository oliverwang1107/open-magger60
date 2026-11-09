/*
 * km_calib.h - persistent per-key calibration: record format, engine glue, save policy (M5 task T-F).
 * Original work. Hardware-free; the flash side is km_calstore.h (append-only log in sector 0x08034000).
 *
 * What is stored. For every key slot the engine knows two numbers (km_engine.h): the released level (baseline,
 * ADC counts) and the bottomed-out level (bottom, counts). The baseline is re-measured at every boot (it moves
 * with temperature) and tracked while the keyboard runs, so it is NOT restored. The bottom is LEARNED: it starts
 * at baseline - 1000 and is lowered by each key's deepest press, so after power-up every key starts with the
 * default stroke again and only gets accurate once it has been pressed all the way down. The record keeps what
 * was learned, so the depth and the rapid-trigger thresholds are right from the first press after the boot.
 * The baseline is stored as well, but only as a witness: it tells whether the live sensor still looks like the
 * one the record was made for.
 *
 * Record payload (little endian, KM_CAL_PAYLOAD_BYTES = 308 bytes):
 *   0   1   version (1)
 *   1   1   number of slots (76)
 *   2   2   reserved, 0
 *   4   76 x { base u16, bottom u16 }, indexed by engine slot. 0/0 = nothing known about this slot (no key, or the slot
 *           was not healthy when the record was made). Otherwise base in [KM_ENGINE_FAULT_LOW, KM_ENGINE_VALUE_MAX]
 *           and base - bottom in [KM_ENGINE_MIN_SPAN, KM_ENGINE_MAX_SPAN] (the engine's own limits).
 * The log record around it (magic, CRC, sequence) is km_calstore.h. A record that decodes is trusted for its
 * shape, not for its truth: km_cal_restore() compares it with the live sensor values before using it.
 *
 * Restoring at boot (km_cal_restore): instead of the 64-frame quiet stretch of the normal calibration, ONE live frame
 * (the 16-point sums are already an average) seeds the engine's calibration and the stored bottoms are applied, as a
 * span below the live baseline (bottom = live baseline - (stored base - stored bottom)). It is refused (the caller then
 * calibrates the normal way) when the live values disagree with the record: more than KM_ENGINE_CAL_MAX_BAD key slots
 * differ from the stored baseline by more than KM_CAL_AGREE_COUNTS, or fewer than KM_ENGINE_CAL_MIN_SLOTS slots can be
 * compared, or the engine's own plausibility checks (keys held, railed inputs) refuse the seeded calibration.
 * Slots that differ individually get no stored bottom. The honest gain of restoring is NOT the saved few milliseconds
 * (the normal stretch is 64 frames = 2.7 ms) but that the learned strokes are known from the first press.
 *
 * When to save (km_cal_policy_*), evaluated once per second by the caller:
 *   - something changed noticeably: some key's bottom is at least KM_CAL_DELTA_COUNTS deeper than in the reference (the
 *     reference = the bottoms right after the last calibration / restore / save);
 *   - the bottoms have been still for KM_CAL_QUIET_S seconds (learning finished, not one save per press);
 *   - at least KM_CAL_MIN_INTERVAL_S seconds since the last save (the first save after a boot is exempt).
 * Wear: a bottom only ever goes deeper while a key is learning (bounded by the stroke), so after the first days almost
 * nothing changes any more; the worst case is one record per KM_CAL_MIN_INTERVAL_S = 144 a day, 24 records per
 * erase = 6 erases a day, far below the flash endurance of the part for years.
 */
#ifndef KM_CALIB_H
#define KM_CALIB_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_calstore.h"
#include "km/km_engine.h"

#define KM_CAL_VERSION          1u
#define KM_CAL_SLOTS            KM_ENGINE_SLOTS
#define KM_CAL_PAYLOAD_BYTES    (4u + 4u * KM_CAL_SLOTS)

#define KM_CAL_AGREE_COUNTS     150u    /* = KM_ENGINE_DRIFT_LIMIT: the engine's own bound for how far a baseline may wander */
#define KM_CAL_DELTA_COUNTS     16u
#define KM_CAL_QUIET_S          20u
#define KM_CAL_MIN_INTERVAL_S   600u
#define KM_CAL_REQUEST_GAP_S    10u     /* the host's save_calibration command: at most one write attempt per this many seconds */

typedef struct {
    uint16_t base[KM_CAL_SLOTS];
    uint16_t bottom[KM_CAL_SLOTS];
} km_cal_record_t;

/* ---- record format ---- */
bool     km_cal_record_valid(const km_cal_record_t *r);                                 /* the shape rules above */
unsigned km_cal_encode(const km_cal_record_t *r, uint8_t *buf, unsigned cap);           /* KM_CAL_PAYLOAD_BYTES, 0 = invalid record / small buffer */
bool     km_cal_decode(km_cal_record_t *r, const uint8_t *buf, unsigned len);           /* false = not exactly a record we wrote */

/* ---- flash ---- */
/* Newest valid record of the log. false = none (erased, foreign, damaged, or it does not decode). */
bool km_cal_load(const km_clog_t *log, km_cal_record_t *r);
/* Plan / do a save (km_calstore.h semantics). */
km_clog_result_t km_cal_job_begin(km_clog_job_t *job, const km_clog_t *log, const km_cal_record_t *r);
km_clog_result_t km_cal_save(const km_clog_t *log, const km_cal_record_t *r);

/* ---- engine glue ---- */
/* The engine's current state as a record. Slots that are not healthy (or all of them when the engine is not calibrated) keep the
 * entry of prev (NULL = 0/0). */
void km_cal_capture(const km_engine_t *e, const km_cal_record_t *prev, km_cal_record_t *out);
/* The engine's current bottoms (counts), the reference for km_cal_changed(). */
void km_cal_snapshot(const km_engine_t *e, uint16_t ref[KM_CAL_SLOTS]);
/* Some healthy key's bottom is at least KM_CAL_DELTA_COUNTS deeper than ref. */
bool km_cal_changed(const km_engine_t *e, const uint16_t ref[KM_CAL_SLOTS]);
/* Sum of all healthy key bottoms: changes while a key is still learning. */
uint32_t km_cal_bottom_sum(const km_engine_t *e);

/* Apply the stored strokes to a freshly calibrated engine, slot by slot, only where the live baseline is within
 * KM_CAL_AGREE_COUNTS of the stored one. Returns the number of slots changed. */
unsigned km_cal_apply(km_engine_t *e, const km_cal_record_t *r);

typedef enum {
    KM_CAL_RESTORE_OK = 0,      /* the engine is calibrated from the live frame, stored strokes applied */
    KM_CAL_RESTORE_DISAGREE,    /* the live sensor does not look like the record: untouched, calibrate normally, distrust the record */
    KM_CAL_RESTORE_REFUSED      /* the engine refused the live frame (keys held, railed...): untouched, calibrate normally */
} km_cal_restore_t;

/* live = the 76 sixteen-point sums / 16 of one frame, as for km_engine_update(). *applied = slots that got a stored bottom. */
km_cal_restore_t km_cal_restore(km_engine_t *e, const km_cal_record_t *r, const uint16_t live[KM_CAL_SLOTS], unsigned *applied);

/* ---- save policy ---- */
typedef struct {
    uint32_t last_sum;
    uint32_t last_change_s;     /* when the bottom sum last changed */
    uint32_t last_save_s;
    uint32_t last_request_s;
    bool     have_sum;
    bool     saved_once;
    bool     requested_once;
} km_cal_policy_t;

void km_cal_policy_init(km_cal_policy_t *p);
/* Call about once per second with the uptime in seconds: true = a save is due now (caller still checks keys / USB idle). */
bool km_cal_policy_due(km_cal_policy_t *p, const km_engine_t *e, const uint16_t ref[KM_CAL_SLOTS], uint32_t now_s);
void km_cal_policy_saved(km_cal_policy_t *p, uint32_t now_s);
/* The host's save_calibration command: true = allowed now (not within KM_CAL_REQUEST_GAP_S of the previous attempt); records the attempt. */
bool km_cal_policy_request(km_cal_policy_t *p, uint32_t now_s);

#endif /* KM_CALIB_H */
