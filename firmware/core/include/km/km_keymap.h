/*
 * km_keymap.h - key map: sensor slot -> key code, on KM_LAYERS layers (M4 task T-B, M5 task T-C).
 *
 * Original work. The slot -> key name table is our own measurement (core/data/slotmap_measured.json,
 * one press per key on the real keyboard); the usage numbers are the standard USB HID Usage Tables,
 * "Keyboard/Keypad Page (0x07)".
 *
 * A table entry ("key code", one byte per layer and slot, plus two argument bytes) is one of
 *   0x00         no key: the slot does nothing on this layer
 *   0x04..0xE7   USB HID keyboard-page usage (letters, digits, F-keys, arrows, modifiers 0xE0..0xE7, ...)
 *   0xE8..0xEA   extended function code (settings format version 2): TOGGLE layer, TAP-HOLD, ONE-SHOT modifier.
 *                These use the two argument bytes of the entry, see KM_CODE_TOGGLE .. KM_CODE_ONESHOT_MOD below
 *   0xEB..0xEF   extended function code (settings format version 3, M6): toggle key, Mod-Morph, DKS, gamepad button,
 *                gamepad axis; see KM_CODE_TOGGLE_KEY .. KM_CODE_PAD_AXIS below. km_layers.c produces no keyboard output
 *                for them (the modules that own them read the pressed keys through km_layers_locked_entry())
 *   0xF0..0xFF   internal function code, see KM_CODE_* below and km_layers.h; these are NOT keyboard usages
 * Every other code has both argument bytes 0. Everything else (0x01..0x03 are the HID error codes) is rejected when a
 * table is loaded, see km_keymap_entry_valid() and km_keymap_table_valid().
 *
 * Layer 0 is a plain ANSI 60% layout (the same keys as the M4 map); its keyboard usages did not change when the
 * layers were added. Slot numbers follow km_scan (0..75). Slots without a key (the 15 mirror / floating slots)
 * map to 0 on every layer. The Fn key (slot 14) carries the function code KM_CODE_HOLD_L1 on layer 0.
 * Layers 1..3: see km_layers.h. The default layer 1 is documented in core/LAYERS_NOTES.md.
 */
#ifndef KM_KEYMAP_H
#define KM_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_m6_types.h"   /* record counts and argument numbers of the M6 codes */

#define KM_KEYMAP_SLOTS   76u  /* same as KM_NUM_SLOTS */
#define KM_LAYERS         4u   /* layer 0 = base, 1..3 = overlays */
#define KM_KEYMAP_LAYERS  KM_LAYERS
#define KM_KEYMAP_KEYS    61u  /* physical keys (slots that carry a sensor and a key) */
#define KM_KEYMAP_ARGS    2u   /* argument bytes per table entry (settings format version 2) */

#define KM_USAGE_NONE     0x00u
#define KM_USAGE_MOD_FIRST 0xE0u  /* LeftControl */
#define KM_USAGE_MOD_LAST  0xE7u  /* RightGUI */

/* ---- internal function codes (table entries 0xF0..0xFF) ---- */
#define KM_CODE_FUNC_FIRST   0xF0u
#define KM_CODE_HOLD_L1      0xF0u  /* while held: layer 1 is active for keys pressed from now on */
#define KM_CODE_PROFILE_1    0xF1u  /* F1..F4: switch to profile 1..4 (requested when the key is RELEASED) */
#define KM_CODE_PROFILE_4    0xF4u
#define KM_CODE_VOL_DOWN     0xF5u  /* consumer page 0x0C, 0xEA Volume Decrement */
#define KM_CODE_VOL_UP       0xF6u  /* consumer page 0x0C, 0xE9 Volume Increment */
#define KM_CODE_MUTE         0xF7u  /* consumer page 0x0C, 0xE2 Mute */
#define KM_CODE_PREV_TRACK   0xF8u  /* consumer page 0x0C, 0xB6 Scan Previous Track */
#define KM_CODE_PLAY_PAUSE   0xF9u  /* consumer page 0x0C, 0xCD Play/Pause */
#define KM_CODE_NEXT_TRACK   0xFAu  /* consumer page 0x0C, 0xB5 Scan Next Track */
#define KM_CODE_HOLD_L2      0xFBu  /* while held: layer 2 (extension of the M5 plan, so layers 2 and 3 can be reached) */
#define KM_CODE_HOLD_L3      0xFCu  /* while held: layer 3 */
#define KM_CODE_LIGHT        0xFDu  /* arg0 = KM_LIGHT_ACT_* (km_m6_types.h), arg1 = 0; arg0 = 0 does nothing (the old 'reserved' meaning) */
/* 0xFE: reserved, accepted in a table, does nothing (arguments 0) */
#define KM_CODE_TRANSPARENT  0xFFu  /* layers 1..3: use the layer-0 entry of this slot; on layer 0 it means no key */

/* ---- extended function codes (settings format version 2); arg0, arg1 = the entry's argument bytes ---- */
#define KM_CODE_EXT_FIRST    0xE8u
#define KM_CODE_TOGGLE       0xE8u  /* arg0 = layer 1..3: a press switches that layer on or off until pressed again (or a profile switch) */
#define KM_CODE_TAP_HOLD     0xE9u  /* arg0 = tap action, arg1 = hold action: a tap sends the tap action, a hold does the hold action */
#define KM_CODE_ONESHOT_MOD  0xEAu  /* arg0 = modifier usage 0xE0..0xE7: a tap arms it for the next key, holding it is a normal modifier */
#define KM_CODE_EXT_LAST     0xEAu
/* tap action: any keyboard usage 0x04..0xE7 or a media code KM_CODE_VOL_DOWN..KM_CODE_NEXT_TRACK;
 * hold action: KM_CODE_HOLD_L1 / _L2 / _L3 (layer while held), a modifier usage 0xE0..0xE7 (modifier while held) or, since
 * settings format version 3, any other keyboard usage 0x04..0xDF (that key while held) as long as it differs from the tap action */

/* ---- M6 function codes (settings format version 3); the argument meanings are the rule of km_keymap_entry_valid() ---- */
#define KM_CODE_EXT3_FIRST   0xEBu
#define KM_CODE_TOGGLE_KEY   0xEBu  /* arg0 = keyboard usage 0x04..0xE7 (modifiers too), arg1 = 0: a press switches that key on or off (km_layers, P2) */
#define KM_CODE_MOD_MORPH    0xECu  /* arg0 = the usage sent normally 0x04..0xE7, arg1 = Mod-Morph record index 0..KM_MORPH_MAX-1 (km_layers, P2) */
#define KM_CODE_DKS          0xEDu  /* arg0 = DKS record index 0..KM_DKS_MAX-1, arg1 = 0 (km_dks, P3) */
#define KM_CODE_PAD_BTN      0xEEu  /* arg0 = gamepad button 1..16 or hat 17..20, arg1 = 0 (engine pressed state) or 1..68 own start point in 0.05 mm (km_pad, P4) */
#define KM_CODE_PAD_AXIS     0xEFu  /* arg0 = KM_PAD_AXIS_ARG(axis, neg, digital), arg1 = curve index 0..KM_PAD_CURVES-1 (km_pad, P4) */
#define KM_CODE_EXT3_LAST    0xEFu

/* Tap-hold decision policies (km_layer_opts_t.tap_policy), see km_layers.h */
#define KM_TAP_POLICY_TIMEOUT    0u  /* hold only when the key is held for tap_term_ms; other keys wait for the decision */
#define KM_TAP_POLICY_PERMISSIVE 1u  /* ... or when another key is pressed AND released while this one is still down */
#define KM_TAP_POLICY_OTHER_KEY  2u  /* ... or as soon as another key is pressed */
#define KM_TAP_POLICY_LAST       2u

/* Per-profile layer behaviour options (settings format version 2). */
typedef struct {
    uint16_t tap_term_ms;      /* a tap-hold key held this long (ms) is a hold: KM_LAYER_TAP_TERM_MIN..MAX */
    uint16_t oneshot_ms;       /* an armed one-shot modifier ends after this long (ms); 0 = it waits for the next key */
    uint8_t  tap_policy;       /* KM_TAP_POLICY_* */
} km_layer_opts_t;
#define KM_LAYER_TAP_TERM_MIN     30u
#define KM_LAYER_TAP_TERM_MAX     1000u
#define KM_LAYER_ONESHOT_MIN      100u    /* a non-zero one-shot timeout */
#define KM_LAYER_ONESHOT_MAX      10000u
#define KM_LAYER_TAP_TERM_DEFAULT 200u
#define KM_LAYER_ONESHOT_DEFAULT  2000u

/* Raw table entry (see above) for a slot on a layer; 0 when an index is out of range. */
uint8_t km_keymap_code(unsigned layer, unsigned slot);

/* Argument byte idx (0 or 1) of the entry; 0 when an index is out of range or the entry has no arguments. */
uint8_t km_keymap_arg(unsigned layer, unsigned slot, unsigned idx);

/* The layer behaviour options of the loaded table (the defaults while no table is loaded). */
const km_layer_opts_t *km_keymap_opts(void);
void km_layer_opts_default(km_layer_opts_t *o);
bool km_layer_opts_valid(const km_layer_opts_t *o);

/* Counts every table load / restore; km_layers uses it to drop toggled layers and armed one-shots when the table changes. */
uint32_t km_keymap_generation(void);

/* Keyboard-page usage for a slot on a layer: the table entry when it is a keyboard usage, otherwise 0. So it is 0
 * for slots without a key, for the Fn key, for every internal function code (including "transparent") and when an
 * index is out of range. Layer 0 gives the same values as the M4 map. */
uint8_t km_keymap_usage(unsigned layer, unsigned slot);

/* True when one table CODE byte is acceptable: 0x00, 0x04..0xE7, 0xE8..0xEF (extended, needs arguments) or 0xF0..0xFF. */
bool km_keymap_code_valid(uint8_t code);

/* True when a whole entry (code and its two arguments) is acceptable: both arguments must be 0 unless the code uses them
 * (TOGGLE: arg0 1..3, arg1 0; TAP_HOLD: arg0 a tap action, arg1 a hold action; ONE-SHOT: arg0 a modifier, arg1 0; and the
 * M6 codes, see KM_CODE_TOGGLE_KEY .. KM_CODE_PAD_AXIS and KM_CODE_LIGHT). This is the rule of the CURRENT firmware. Whether an
 * index (DKS, Mod-Morph) points at an existing record is not known here; km_settings_valid() checks that. */
bool km_keymap_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1);

/* The settings format version 2 rule for a code byte / a whole entry, exactly as it was before M6 (no codes 0xEB..0xEF, a hold
 * action only a layer hold or a modifier, 0xFD / 0xFE with arguments 0). A version 2 blob is read with these, and an
 * object whose every entry passes them can still be written as version 2. */
bool km_keymap_code_valid_v2(uint8_t code);
bool km_keymap_entry_valid_v2(uint8_t code, uint8_t arg0, uint8_t arg1);

/* True when the entry is valid but only settings format version 3 can hold it (a firmware that wrote version 2 would
 * refuse it). */
bool km_keymap_entry_needs_v3(uint8_t code, uint8_t arg0, uint8_t arg1);

/* The settings format version 1 rule for a code byte (no extended codes). */
bool km_keymap_code_valid_v1(uint8_t code);

/* True when every entry of a table is acceptable. A table of codes alone has all arguments 0, so extended codes fail. */
bool km_keymap_table_valid(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS]);

/* Copy the built-in table (layer 0 = ANSI 60%, layer 1 = the default FN layer, layers 2 and 3 transparent). */
void km_keymap_default_table(uint8_t out[KM_LAYERS][KM_KEYMAP_SLOTS]);

/* Load a replacement table (a copy is kept, the caller's buffer may go away). NULL = back to the built-in table.
 * A table that fails km_keymap_table_valid() is refused: the built-in table is restored and false is returned.
 * Version 1 form: all arguments 0, default options. */
bool km_layers_set_map(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS]);

/* Version 2 form: codes, argument bytes and the layer options. arg and opts may be NULL (= all arguments 0, default
 * options). Every entry (km_keymap_entry_valid) and the options (km_layer_opts_valid) must be acceptable, otherwise the
 * built-in table is restored and false is returned. */
bool km_layers_set_table(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS],
                         const uint8_t arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS], const km_layer_opts_t *opts);

/* A physical key sits on this slot (true for Fn, false for the mirror / floating slots and slot >= 76). */
bool km_keymap_slot_is_key(unsigned slot);

/* Our own key name for a slot ("ESC", "A", "LSHIFT", ...), "" when the slot has no key. */
const char *km_keymap_slot_name(unsigned slot);

/* True for the eight modifier usages 0xE0..0xE7. */
static inline bool km_usage_is_modifier(uint8_t usage)
{
    return usage >= KM_USAGE_MOD_FIRST && usage <= KM_USAGE_MOD_LAST;
}

#endif /* KM_KEYMAP_H */
