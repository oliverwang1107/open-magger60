/*
 * km_m6_types.h - the record types and numbers every M6 work package codes against (M6 task P0). Original work.
 *
 * This header is the single place where the shapes of the M6 settings records are fixed: the DKS, Mod-Morph, SOCD and gamepad
 * (PAD) records of a profile and the global DEVICE options. They are what the settings blob (km_settings.h, format version 3)
 * stores, what km_keymap.h table entries refer to by INDEX (0xEC Mod-Morph, 0xED DKS) or by number (0xEE / 0xEF gamepad), and what
 * the modules km_socd (P1), km_dks (P3), km_pad (P4) and km_layers (P2) read. It includes nothing but <stdint.h>, so a module
 * can use it without pulling in the settings code.
 *
 * The structs are in-memory types (natural C layout, asserted below), NOT the wire format: the blob is written field by field
 * (see km_settings.h), so a compiler's padding never reaches flash. The reserved bytes are part of the type so that the blob
 * round trip is exact; they must be 0 (km_settings.c refuses anything else).
 *
 * Units: depth is in 0.01 mm ("mm100", 0..340, nominal travel 3.40 mm) unless a name says otherwise (the gamepad curves use
 * 0.05 mm steps).
 */
#ifndef KM_M6_TYPES_H
#define KM_M6_TYPES_H

#include <stdint.h>

/* ---- counts per profile ---- */
#define KM_DKS_MAX        8u    /* DKS records per profile */
#define KM_MORPH_MAX      8u    /* Mod-Morph records per profile */
#define KM_SOCD_MAX       4u    /* SOCD pairs per profile */
#define KM_PAD_CURVES     4u    /* gamepad response curves per profile */

#define KM_M6_TRAVEL_MM100 340u /* nominal key travel, same as the engine's */

/* ---- DKS: dynamic keystroke, one record = 4 actions that are triggered by 4 depth events (16 bytes) ---- */
typedef struct {
    uint16_t t_act_mm100;        /* start point (default 120): crossing it = event 1 (down) / event 4 (up, released) */
    uint16_t t_bot_mm100;        /* bottom point (0 = travel - 30): crossing it = event 2 (down) / event 3 (up) */
    uint8_t  flags;              /* bit0 = may re-trigger in mid-travel (default 0); the other bits must be 0 */
    uint8_t  rsvd[3];            /* must be 0 */
    struct {
        uint8_t usage;           /* keyboard usage 0x04..0xE7; 0 = this action is not used (then ev must be 0) */
        uint8_t ev;              /* 4 events x 2 bits: bits 0-1 = event 1 ... bits 6-7 = event 4; see KM_DKS_EV_* */
    } act[4];
} km_dks_rec_t;

#define KM_DKS_FLAG_RETRIGGER  0x01u
#define KM_DKS_FLAGS_MASK      0x01u
#define KM_DKS_EV_NONE         0u    /* this event does nothing for this action */
#define KM_DKS_EV_PRESS        1u    /* press the usage */
#define KM_DKS_EV_RELEASE      2u    /* release the usage */
#define KM_DKS_EV_TAP          3u    /* press, and release 10 ms later (KM_LAYERS_STEP_MS) */
#define KM_DKS_EVENTS          4u
#define KM_DKS_EV_GET(ev, n)   ((unsigned)(((ev) >> (2u * (n))) & 3u))             /* event n = 0..3 (events 1..4) */
#define KM_DKS_EV_SET(n, what) ((uint8_t)(((what) & 3u) << (2u * (n))))
#define KM_DKS_ACT_DEFAULT_MM100  120u   /* default start point */
#define KM_DKS_ACT_MIN_MM100       40u   /* below this the noise floor makes the start point unstable */
#define KM_DKS_GAP_MIN_MM100       40u   /* t_bot - t_act at least */
#define KM_DKS_BOT_DEFAULT_MM100  310u   /* what t_bot = 0 means: travel - 30 */

/* ---- Mod-Morph: a key that turns into another key while a modifier is held (4 bytes) ---- */
typedef struct {
    uint8_t mod_mask;            /* bit n = modifier usage 0xE0 + n (left and right count as separate bits); any one held = morph; must not be 0 */
    uint8_t usage;               /* the key sent instead, 0x04..0xE7 */
    uint8_t flags;               /* bit0 = suppress: the triggering modifier is not sent while the morph lasts; other bits must be 0 */
    uint8_t rsvd;                /* must be 0 */
} km_morph_rec_t;

#define KM_MORPH_FLAG_SUPPRESS 0x01u
#define KM_MORPH_FLAGS_MASK    0x01u

/* ---- SOCD: opposing-key handling for one pair of physical keys (4 bytes) ---- */
typedef struct {
    uint8_t slot_a, slot_b;      /* two different sensor slots that carry a key; a slot is in at most one pair of a profile */
    uint8_t mode;                /* KM_SOCD_MODE_* */
    uint8_t flags;               /* bit0 = only while layer 0 is active; other bits must be 0 */
} km_socd_pair_t;

#define KM_SOCD_MODE_OFF          0u
#define KM_SOCD_MODE_LAST_WINS    1u   /* the key pressed last wins; releasing it brings the other back */
#define KM_SOCD_MODE_NEUTRAL      2u   /* both down = neither is sent */
#define KM_SOCD_MODE_FIRST_WINS   3u   /* the key pressed first wins; the second is ignored */
#define KM_SOCD_MODE_DEEPER_WINS  4u   /* analog: the deeper key wins */
#define KM_SOCD_MODE_LAST         4u
#define KM_SOCD_FLAG_LAYER0_ONLY  0x01u
#define KM_SOCD_FLAGS_MASK        0x01u

/* ---- gamepad response curve (4 bytes) ---- */
typedef struct {
    uint8_t start_mm100_5;       /* where the output leaves 0, in 0.05 mm: 4..67 (4 = 0.20 mm, above the noise floor) */
    uint8_t end_mm100_5;         /* where the output reaches full, in 0.05 mm: start + 4 .. 68 (68 = 3.40 mm) */
    uint8_t shape;               /* KM_PAD_SHAPE_* */
    uint8_t flags;               /* bit0 = reversed; other bits must be 0 */
} km_pad_curve_t;

#define KM_PAD_SHAPE_LINEAR       0u
#define KM_PAD_SHAPE_SLOW_FAST    1u   /* slow at first, quick later */
#define KM_PAD_SHAPE_FAST_SLOW    2u   /* quick at first, slow later */
#define KM_PAD_SHAPE_S            3u
#define KM_PAD_SHAPE_LAST         3u
#define KM_PAD_FLAG_REVERSE       0x01u
#define KM_PAD_FLAGS_MASK         0x01u
#define KM_PAD_CURVE_START_MIN    4u
#define KM_PAD_CURVE_END_MAX      68u
#define KM_PAD_CURVE_SPAN_MIN     4u   /* end - start at least (0.20 mm) */
#define KM_PAD_CURVE_START_DEFAULT 4u  /* default curve: 0.20 mm .. 3.20 mm, linear */
#define KM_PAD_CURVE_END_DEFAULT  64u

/* gamepad options of a profile (8 bytes) */
typedef struct {
    uint8_t stick_deadzone;      /* centre dead zone of a stick, % of the full range, 0..50 */
    uint8_t circular;            /* 1 = limit each stick to the unit circle, 0 = square */
    uint8_t smooth;              /* analog path smoothing level 0..3 (0 = default), see M6_PLAN section 6.6 */
    uint8_t rsvd[5];             /* must be 0 */
} km_pad_opts_t;
#define KM_PAD_DEADZONE_MAX       50u
#define KM_PAD_SMOOTH_MAX         3u

/* ---- global device options (16 bytes) ---- */
typedef struct {
    uint8_t pad_mode;            /* KM_PAD_MODE_*; chosen at boot, so a change needs a re-plug */
    uint8_t filter_mode;         /* KM_FILTER_MODE_* (only for A/B comparisons, M6_PLAN section 6.6) */
    uint8_t rsvd[14];            /* must be 0 */
} km_device_opts_t;
#define KM_PAD_MODE_OFF           0u
#define KM_PAD_MODE_HID           1u
#define KM_PAD_MODE_XINPUT        2u   /* reserved for later */
#define KM_PAD_MODE_LAST          2u
#define KM_FILTER_MODE_DEFAULT    0u
#define KM_FILTER_MODE_LOW_LATENCY 1u
#define KM_FILTER_MODE_QUIET      2u
#define KM_FILTER_MODE_LAST       2u

/* ---- the numbers of the gamepad table entries (arguments of KM_CODE_PAD_BTN / KM_CODE_PAD_AXIS, km_keymap.h) ---- */
#define KM_PAD_BTN_FIRST          1u   /* arg0 of KM_CODE_PAD_BTN: buttons 1..16 */
#define KM_PAD_BTN_LAST           16u
#define KM_PAD_HAT_UP             17u  /* 17..20 = hat up / down / left / right */
#define KM_PAD_HAT_DOWN           18u
#define KM_PAD_HAT_LEFT           19u
#define KM_PAD_HAT_RIGHT          20u
#define KM_PAD_BTN_ARG_LAST       20u
#define KM_PAD_BTN_START_MAX      68u  /* arg1 of KM_CODE_PAD_BTN: 0 = the engine's pressed state, 1..68 = own start point in 0.05 mm */

#define KM_PAD_AXIS_LX            0u   /* arg0 of KM_CODE_PAD_AXIS: bits 0-2 = axis */
#define KM_PAD_AXIS_LY            1u
#define KM_PAD_AXIS_RX            2u
#define KM_PAD_AXIS_RY            3u
#define KM_PAD_AXIS_LT            4u   /* left trigger (only the positive direction) */
#define KM_PAD_AXIS_RT            5u   /* right trigger (only the positive direction) */
#define KM_PAD_AXIS_COUNT         6u
#define KM_PAD_AXIS_NEG           0x08u  /* bit3: this key pushes the axis to the negative side */
#define KM_PAD_AXIS_DIGITAL       0x10u  /* bit4: the key counts as fully pushed as soon as it is pressed */
#define KM_PAD_AXIS_MASK          0x1Fu
#define KM_PAD_AXIS_ARG(axis, neg, digital) \
    ((uint8_t)((axis) | ((neg) ? KM_PAD_AXIS_NEG : 0u) | ((digital) ? KM_PAD_AXIS_DIGITAL : 0u)))
/* arg1 of KM_CODE_PAD_AXIS = curve index 0..KM_PAD_CURVES-1 */

/* ---- other numbers of the new table codes ---- */
#define KM_LIGHT_ACT_NONE         0u   /* arg0 of KM_CODE_LIGHT: 0 does nothing (the old 'reserved' meaning) */
#define KM_LIGHT_ACT_NEXT_FX      1u
#define KM_LIGHT_ACT_PREV_FX      2u
#define KM_LIGHT_ACT_BRIGHT_UP    3u
#define KM_LIGHT_ACT_BRIGHT_DOWN  4u
#define KM_LIGHT_ACT_SPEED_UP     5u
#define KM_LIGHT_ACT_SPEED_DOWN   6u
#define KM_LIGHT_ACT_TOGGLE       7u
#define KM_LIGHT_ACT_LAST         7u

/* the in-memory structs are 4 / 8 / 16 bytes with no compiler padding, so a memcmp of two records is a fair comparison */
_Static_assert(sizeof(km_dks_rec_t) == 16u, "km_dks_rec_t must be 16 bytes");
_Static_assert(sizeof(km_morph_rec_t) == 4u, "km_morph_rec_t must be 4 bytes");
_Static_assert(sizeof(km_socd_pair_t) == 4u, "km_socd_pair_t must be 4 bytes");
_Static_assert(sizeof(km_pad_curve_t) == 4u, "km_pad_curve_t must be 4 bytes");
_Static_assert(sizeof(km_pad_opts_t) == 8u, "km_pad_opts_t must be 8 bytes");
_Static_assert(sizeof(km_device_opts_t) == 16u, "km_device_opts_t must be 16 bytes");

#endif /* KM_M6_TYPES_H */
