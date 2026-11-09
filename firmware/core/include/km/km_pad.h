/*
 * km_pad.h - gamepad core (M6 package P4). Original work, pure logic, no hardware, no USB, no dependency on the key engine.
 *
 * Turns "which keys are held, bound to what, how deep" into one gamepad state: 16 buttons, one hat, two sticks (4 axes) and two
 * triggers, plus the byte layout of that state as it goes into the USB input report. The USB layer (P5b) only has to carry
 * KM_PAD_REPORT_LEN bytes; the report descriptor is built from the layout below. See core/PAD_NOTES.md for the design.
 *
 * ---- Input: the "pad keys" ----
 * The caller lists every key that is DOWN (as the key engine says) and whose current table item is KM_PAD_CODE_BTN (0xEE) or
 * KM_PAD_CODE_AXIS (0xEF): slot, code, arg0, arg1 (the table entry, latched like km_layers does). Keys that are not down are not
 * listed, so "no key down" is an empty list and the answer is the neutral report. The analog value is read through a callback
 * (km_engine_depth for now; km_engine_analog once P12c has it), 0.01 mm units, 0..340.
 *
 *   KM_PAD_CODE_BTN   arg0 = 1..16 button, 17..20 hat up / down / left / right
 *                     arg1 = 0: pressed while the key is down;  1..68: pressed from that depth (0.05 mm units, at least 0.20 mm)
 *                     on, released KM_PAD_BTN_HYST_MM100 above it
 *   KM_PAD_CODE_AXIS  arg0 = bit0-2 axis (0 LX, 1 LY, 2 RX, 3 RY, 4 left trigger, 5 right trigger), bit3 negative direction
 *                     (sticks only), bit4 digital (full deflection as soon as the key is down); bits 5-7 must be 0
 *                     arg1 = curve index 0..3
 *
 * ---- Output state and report ----
 * Report, KM_PAD_REPORT_LEN = 15 bytes, no report id, little endian, every byte defined:
 *   byte  0..1   buttons, bit n = button n+1
 *   byte  2      low nibble hat: 0 neutral, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW; high nibble always 0
 *   byte  3..4   LX  int16, -32767..32767 (never -32768), 0 = centre
 *   byte  5..6   LY  int16 (negative = up, bind W to LY negative)
 *   byte  7..8   RX  int16
 *   byte  9..10  RY  int16
 *   byte 11..12  left trigger  uint16 0..32767
 *   byte 13..14  right trigger uint16 0..32767
 * The all-zero report is the neutral state, so km_pad_report_released() is 15 zero bytes.
 *
 * ---- Rules (each one is tested) ----
 *  - released = exactly 0; a key at or beyond the curve end = exactly 32767.
 *  - stick axis = (deepest key bound to its + direction) - (deepest key bound to its - direction): opposing keys cancel
 *    when equally deep, the deeper one wins by the difference. Triggers: the deepest bound key.
 *  - hat: up+down cancel, left+right cancel; the remaining two give the diagonal.
 *  - buttons: any key bound to a button sets it.
 *  - fault (allowed == false), a changed epoch (table / profile / layer change), km_pad_reset() and km_pad_set_config(): the whole
 *    report is neutral at once, and a key that was already down then stays "held over" (ignored) until it has been released
 *    once, so nothing comes back by itself.
 *  - at most KM_PAD_MAX_KEYS entries are looked at, invalid entries are ignored.
 */
#ifndef KM_PAD_H
#define KM_PAD_H

#include <stdbool.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("km/km_m6_types.h")
#include "km/km_m6_types.h" /* P0 owns km_pad_curve_t / km_pad_opts_t; until it has merged, the copies below are used */
#define KM_PAD_HAVE_M6_TYPES 1
#endif
#endif

#define KM_PAD_SLOTS        76u     /* same as KM_NUM_SLOTS */
#define KM_PAD_BUTTONS      16u
#define KM_PAD_MAX_KEYS     20u     /* pad keys looked at per update (the rest is ignored) */
#define KM_PAD_REPORT_LEN   15u
#define KM_PAD_AXIS_MAX     32767   /* full deflection / full trigger */
#define KM_PAD_PERIOD_MS    1u      /* the state is recomputed at most once per millisecond */

/* table codes (P0 adds KM_CODE_PAD_BTN / KM_CODE_PAD_AXIS to km_keymap.h with the same values) */
#define KM_PAD_CODE_BTN     0xEEu
#define KM_PAD_CODE_AXIS    0xEFu

/* arg0 of KM_PAD_CODE_BTN */
#define KM_PAD_BTN_HAT_UP     17u
#define KM_PAD_BTN_HAT_DOWN   18u
#define KM_PAD_BTN_HAT_LEFT   19u
#define KM_PAD_BTN_HAT_RIGHT  20u

/* arg0 of KM_PAD_CODE_AXIS */
#define KM_PAD_AXIS_LX       0u
#define KM_PAD_AXIS_LY       1u
#define KM_PAD_AXIS_RX       2u
#define KM_PAD_AXIS_RY       3u
#define KM_PAD_AXIS_LT       4u
#define KM_PAD_AXIS_RT       5u
#define KM_PAD_AXES          6u
#define KM_PAD_AXIS_NEG      0x08u
#define KM_PAD_AXIS_DIGITAL  0x10u

/* depth numbers, 0.01 mm */
#define KM_PAD_MIN_START_MM100   20u    /* a curve starts at 0.20 mm or deeper: resting noise is about +-0.15 mm */
#define KM_PAD_BTN_HYST_MM100    20u    /* a depth-activated button releases this far above its activation point */
#define KM_PAD_BTN_MIN_ACT_MM100 20u
#define KM_PAD_OUT_HYST          64     /* an axis value follows its input only after moving this far (never at 0, full scale or a sign change) */
/* What the integrator should give a pad key in the key engine, so that "down" means "pressed a little" (FIXED mode, valid for
 * km_engine_cfg_valid). RAPID trigger on a stick key would drop the axis at every small lift. */
#define KM_PAD_ENGINE_PRESS_MM100   20u
#define KM_PAD_ENGINE_RELEASE_MM100 10u

#ifndef KM_PAD_HAVE_M6_TYPES
#define KM_PAD_CURVES   4u
/* shape: 0 linear, 1 slow then fast, 2 fast then slow, 3 S; flags bit0 = mirror (the curve turned by 180 degrees) */
typedef struct {
    uint8_t start_mm100_5;  /* depth where the value starts to rise, 0.05 mm units, 4..64 */
    uint8_t end_mm100_5;    /* depth of full value, start + 2 .. 68 */
    uint8_t shape;
    uint8_t flags;
} km_pad_curve_t;

typedef struct {
    uint8_t stick_deadzone; /* 0..50, percent of full deflection, radial, the rest is rescaled to full */
    uint8_t circular;       /* 1: each stick is kept inside the unit circle */
    uint8_t smooth;         /* analog-path smoothing level (P12c); km_pad does not use it */
    uint8_t rsvd[5];
} km_pad_opts_t;
#endif

#define KM_PAD_SHAPE_LINEAR   0u
#define KM_PAD_SHAPE_EASE_IN  1u
#define KM_PAD_SHAPE_EASE_OUT 2u
#define KM_PAD_SHAPE_S        3u
#define KM_PAD_CURVE_MIRROR   0x01u

/* One held pad key: the table entry of a key that is down. */
typedef struct {
    uint8_t slot;           /* sensor slot 0..75 */
    uint8_t code;           /* KM_PAD_CODE_BTN or KM_PAD_CODE_AXIS */
    uint8_t arg0;
    uint8_t arg1;
} km_pad_key_t;

/* The gamepad state. All zero = neutral. */
typedef struct {
    uint16_t buttons;       /* bit n = button n+1 */
    uint8_t  hat;           /* 0 neutral, 1..8 clockwise from north */
    int16_t  lx, ly, rx, ry;
    uint16_t lt, rt;
} km_pad_report_t;

typedef uint16_t (*km_pad_depth_fn)(void *ctx, unsigned slot);  /* depth of a slot in 0.01 mm */

typedef struct {
    const km_pad_key_t *keys;   /* held pad keys, any order; may be NULL when n == 0 */
    unsigned n;
    bool     allowed;           /* false = keys are not allowed (fault): neutral report */
    uint32_t epoch;             /* any change = table / profile / layer changed: neutral report, held keys held over */
    uint32_t now_ms;            /* free-running millisecond counter, only differences are used */
    km_pad_depth_fn depth;
    void    *ctx;
} km_pad_in_t;

typedef struct {
    km_pad_curve_t curve[KM_PAD_CURVES];
    km_pad_opts_t  opts;
    km_pad_report_t cur;        /* the state returned by the last update */
    int16_t  raw[4];            /* stick axes after the output band, before circle and dead zone */
    uint32_t blocked[(KM_PAD_SLOTS + 31u) / 32u];   /* keys held over a reset: ignored until released */
    uint32_t btn_on[(KM_PAD_SLOTS + 31u) / 32u];    /* depth-activated buttons that are on (hysteresis) */
    uint32_t last_ms;
    uint32_t epoch;
    bool     started;           /* an update has run (last_ms / epoch are valid) */
    bool     rearm;             /* the next update marks the keys it finds down as held over */
    /* counters */
    uint32_t computes, faults, resets, ignored_keys;
} km_pad_t;

/* ---- curves and options ---- */
void km_pad_default_curve(unsigned index, km_pad_curve_t *out);     /* index >= KM_PAD_CURVES gives curve 0 */
bool km_pad_curve_valid(const km_pad_curve_t *c);
bool km_pad_opts_valid(const km_pad_opts_t *o);
/* A table entry for the pad: true if (code, arg0, arg1) is a valid KM_PAD_CODE_BTN / KM_PAD_CODE_AXIS entry. P0's
 * km_keymap_entry_valid() can call this for 0xEE / 0xEF. */
bool km_pad_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1);
/* The value 0..32767 of curve c at a depth (0.01 mm). Monotonic, 0 up to the start, 32767 from the end. */
int32_t km_pad_curve_eval(const km_pad_curve_t *c, uint16_t depth_mm100);

/* ---- the state machine ---- */
void km_pad_init(km_pad_t *p);      /* default curves and options, neutral */
/* Load curves and options. NULL = defaults. Invalid curves become the default of their index, invalid options the defaults;
 * returns false if anything had to be replaced. Releases everything (like km_pad_reset). */
bool km_pad_set_config(km_pad_t *p, const km_pad_curve_t *curves /* KM_PAD_CURVES entries */, const km_pad_opts_t *opts);
/* Neutral at once; keys that are down now stay held over until released. */
void km_pad_reset(km_pad_t *p);
/* One pass of the main loop. *out always receives the current state (unchanged between recomputations). Returns true when the
 * state was recomputed (at most once per KM_PAD_PERIOD_MS, and at once after a fault, reset or epoch change). */
bool km_pad_update(km_pad_t *p, const km_pad_in_t *in, km_pad_report_t *out);

/* ---- report ---- */
void km_pad_report_released(uint8_t out[KM_PAD_REPORT_LEN]);                    /* the all-released report: 15 zero bytes */
void km_pad_report_pack(const km_pad_report_t *r, uint8_t out[KM_PAD_REPORT_LEN]);
void km_pad_report_unpack(const uint8_t in[KM_PAD_REPORT_LEN], km_pad_report_t *r);
bool km_pad_report_equal(const km_pad_report_t *a, const km_pad_report_t *b);   /* same bytes after packing */
bool km_pad_report_neutral(const km_pad_report_t *r);

#endif /* KM_PAD_H */
