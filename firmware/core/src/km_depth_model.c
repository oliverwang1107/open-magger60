/*
 * km_depth_model.c - our own reading -> key depth model (M4 task T-A, original work).
 *
 * ---- What it does -------------------------------------------------------------------------
 * A Hall-effect key has a magnet in the stem and a linear Hall sensor under it. We know two
 * readings per key (own measurement, core/data/slot_ranges_s2.json): `released` (key at rest) and
 * `bottom` (key pressed all the way down, nominal travel 3.40 mm), and nothing in between. The
 * model turns any reading into a depth, so that the engine can use millimetre thresholds that
 * mean the same on every key although the readings differ per key (released 3102..3451,
 * bottom 1794..2398, span 998..1365 counts).
 *
 * ---- Physics ------------------------------------------------------------------------------
 * Pressing the key moves the magnet by x towards the sensor, so their distance is r = r0 - x
 * (r0: distance at rest). Far from the magnet the field falls off as a power of the distance:
 *
 *      B(x) = B1 / (r0 - x)^p        p = 3 for a point dipole on its axis, about 2..3 for a
 *                                    real cylinder magnet at a few millimetres
 *
 * The reading is linear in B: v = v0 - g * B(x) (a smaller reading means a stronger field here;
 * v0, g and the field already present at rest are unknown). That is why only differences matter,
 * and why the normalised press
 *
 *      n = (released - v) / (released - bottom)  =  (B(x) - B(0)) / (B(T) - B(0))   in 0..1
 *
 * needs no knowledge of v0 or g. With s = x / r0 and sT = T / r0 (T = 3.40 mm, the fraction of
 * the gap that the press closes) the model inverts to
 *
 *      d(n) / T = (1 - (1 + n K)^(-1/p)) / (1 - (1 + K)^(-1/p)),    K = (1 - sT)^(-p) - 1
 *
 * which is concave: the field grows faster than the travel, so n lags behind the depth, and the
 * first millimetre of travel moves the reading less than the last one. Free parameters are p
 * and sT (two), plus the unknown sensor offset that we ignore here (an off-axis sensor changes
 * the effective p, which the fit absorbs).
 *
 * ---- The curve used (one parameter) -------------------------------------------------------
 * For p = 1 the inverse is a rational function, d/T = n (1 + a) / (1 + a n), with a = K. For
 * p = 2..3 the exact curve above needs a root and a power per call, but the same rational form
 * with a refitted a follows it closely (maximum error over all n, T = 3.4 mm, computed by
 * `magger60_depth_check.py study`):
 *
 *      p = 1.5..3 and sT = 0.25..0.6 (rest gap 5.7..13.6 mm): at most 0.08 mm (typically 0.02)
 *
 * which is smaller than a feeler-gauge measurement can resolve. So the code uses
 *
 *      depth = T * n (1 + a) / (1 + a n),     n = clamp((released - value) / span, 0, 1)
 *
 * with ONE shared shape parameter a (the field growth from rest to bottom, a = B(T)/B(0) - 1 for
 * p = 1; larger a = more concave; a = 0 is a straight line). Properties:
 *   - depth(n = 0) = 0 and depth(n = 1) = T exactly, for any a >= 0 and any span;
 *   - strictly increasing in n (slope T (1 + a) / (1 + a n)^2 > 0), so flat-safe at both ends;
 *   - the slope is largest at rest (T (1 + a) per unit n) and smallest at the bottom (T / (1 + a)).
 *     In counts: with a = 2 and span 1000, one count is 0.010 mm at the top and 0.0011 mm at
 *     the bottom. Consequence for the engine: noise of +-15 counts is +-0.15 mm near rest but only
 *     +-0.02 mm near the bottom, so actuation points belong in the lower half of the travel or
 *     need a hysteresis of at least 0.2 mm;
 *   - values outside [bottom, released] are clamped (n = 0 or 1), so noise cannot overshoot.
 *
 * ---- The default a = 2.0 is a prior, not a measurement -------------------------------------
 * a = 2 is the rational fit of p = 2, sT = 0.52 (rest gap 6.5 mm) or p = 3, sT = 0.42 (8 mm); it
 * puts mid-reading (n = 0.5) at 2.55 mm of 3.40 mm. Real key shapes (magnet geometry, sensor
 * offset, ADC front end) can differ, and we have no ground truth between the endpoints. The
 * feeler-gauge procedure in tools/magger60_depth_check.py fits a from a few readings; set the
 * result in KM_DEPTH_CURVE_A_Q6 (km_depth_model.h). Because every key has its own endpoints, a
 * single shared a is enough as long as all keys use the same switch.
 *
 * ---- Fixed point, cost ----------------------------------------------------------------------
 * a is stored as a_q6 = a * 64. With num = released - clamp(value) and span = released - bottom:
 *
 *      depth = round( T * (64 + a_q6) * num / (64 * span + a_q6 * num) )
 *
 * One multiply chain and ONE 32-bit division, no float, no table, no branch on the hot path
 * other than the two clamps. Overflow: T (64 + a_q6) num <= 340 * 1088 * 4095 = 1.52e9 < 2^32
 * for span <= 4095 and a_q6 <= 1024; both bounds are enforced (spans outside
 * KM_DEPTH_MIN_SPAN..KM_DEPTH_MAX_SPAN are refused, a_q6 is clamped). At num = span the numerator
 * and denominator are equal multiples of span, so the result is exactly T; at num = 0 it is 0.
 * Rounding a strictly increasing function keeps the result non-decreasing in num, so the
 * integer result is monotonic (also proven exhaustively in tests/test_depth_model.c).
 * Cost on the Cortex-M4F (arm-none-eabi-gcc 16, -Os, Cortex-M4F: 90 bytes of code): the main
 * path is 30 instructions with 3 single-cycle multiplies and one UDIV (2..12 cycles), so about
 * 40 cycles per call (35..50 with the pipeline refills of the taken branches; an estimate from
 * the instruction count, not a measurement on the chip). 61 keys x 23,000 frames/s = 1.4 M
 * calls/s would then take about 56 M cycles/s, roughly a quarter of the 240 MHz core. The engine
 * should therefore skip the call for keys that are clearly at rest (value >= released needs no
 * model: the depth is 0) which is nearly every key nearly all the time.
 *
 * ---- Unusable calibration -------------------------------------------------------------------
 * released <= bottom or a span below KM_DEPTH_MIN_SPAN (the reading noise of +-15 counts would
 * be a large part of the travel) or above KM_DEPTH_MAX_SPAN (not a 12-bit reading): every reading
 * gives depth 0, so such a key can never look pressed. Deciding what to do with the key
 * (disable, report) is the engine's job (km_engine_calibrate_idle).
 */
#include "km/km_depth_model.h"

#include <stddef.h>

uint16_t km_depth_model_curve_mm100(const km_depth_cal_t *cal, uint16_t value, uint16_t a_q6)
{
    if (cal == NULL) {
        return 0;
    }
    const uint32_t released = cal->released;
    const uint32_t bottom = cal->bottom;
    if (released <= bottom) {
        return 0;
    }
    const uint32_t span = released - bottom;
    if (span < KM_DEPTH_MIN_SPAN || span > KM_DEPTH_MAX_SPAN) {
        return 0;
    }
    const uint32_t a = (a_q6 > KM_DEPTH_CURVE_A_Q6_MAX) ? KM_DEPTH_CURVE_A_Q6_MAX : a_q6;

    uint32_t v = value;
    if (v > released) {
        v = released;
    }
    if (v < bottom) {
        v = bottom;
    }
    const uint32_t num = released - v;                 /* 0..span */
    const uint32_t den = (span << 6) + a * num;         /* 64 * span + a_q6 * num, > 0 */
    const uint32_t top = KM_TRAVEL_MM100 * (64u + a) * num;
    return (uint16_t)((top + (den >> 1)) / den);
}

uint16_t km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value)
{
    return km_depth_model_curve_mm100(cal, value, KM_DEPTH_CURVE_A_Q6);
}
