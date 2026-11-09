/*
 * km_depth_model.h - our own reading -> key depth model (M4 task T-A).
 *
 * Maps one Hall-sensor reading to the key depth in 0.01 mm, given the two per-key calibration
 * endpoints (released and bottomed-out reading). The model, its derivation and the cost estimate
 * are documented at the top of core/src/km_depth_model.c; the file format and tool for validating
 * the curve shape with a feeler gauge are in tools/magger60_depth_check.py.
 *
 * Original work: derived from basic magnet physics and our own measurements
 * (core/data/slot_ranges_s2.json), every number here is our own measurement or the public switch spec.
 */
#ifndef KM_DEPTH_MODEL_H
#define KM_DEPTH_MODEL_H

#include <stdint.h>

/* Per-key calibration endpoints in ADC counts (the km_scan_sums() value / 16, 12 bit).
 * Smaller reading = deeper press, so a usable calibration has released > bottom. */
typedef struct {
    uint16_t released;   /* reading with the key at rest */
    uint16_t bottom;     /* reading with the key pressed all the way down */
} km_depth_cal_t;

/* Nominal switch travel in 0.01 mm (public spec: 3.4 mm). */
#define KM_TRAVEL_MM100 340u

/*
 * Curvature of the model, a_q6 / 64 (see km_depth_model.c). Default 2.0, a prior from the magnet
 * physics that still has to be confirmed with the feeler-gauge measurement. 0 = linear.
 * To change the shape for all keys: set the number here, or build km_depth_model.c with
 * -DKM_DEPTH_CURVE_A_Q6=<n>. Valid range 0..KM_DEPTH_CURVE_A_Q6_MAX (larger values are clamped).
 */
#ifndef KM_DEPTH_CURVE_A_Q6
#define KM_DEPTH_CURVE_A_Q6 128u
#endif
#define KM_DEPTH_CURVE_A_Q6_MAX 1024u   /* a = 16: more than any plausible magnet; bounds the 32-bit math */

/* Calibration sanity limits: span = released - bottom must be inside [MIN, MAX] counts. */
#define KM_DEPTH_MIN_SPAN 64u     /* real keys measure 998..1365 */
#define KM_DEPTH_MAX_SPAN 4095u   /* the 12-bit ADC range */

/*
 * Reading -> depth in 0.01 mm, clamped to 0..KM_TRAVEL_MM100; never increases when the reading
 * rises (smaller reading = deeper). value >= released gives 0, value <= bottom gives
 * KM_TRAVEL_MM100 exactly. An unusable calibration (cal == NULL, released <= bottom, span outside
 * KM_DEPTH_MIN_SPAN..KM_DEPTH_MAX_SPAN) gives 0 for every reading: the key can never look pressed.
 */
uint16_t km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value);

/* The same with an explicit curvature a_q6 / 64 (tests, tools). km_depth_model_mm100() calls this
 * with KM_DEPTH_CURVE_A_Q6. */
uint16_t km_depth_model_curve_mm100(const km_depth_cal_t *cal, uint16_t value, uint16_t a_q6);

#endif /* KM_DEPTH_MODEL_H */
