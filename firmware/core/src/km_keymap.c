/*
 * km_keymap.c - key map with layers (M4 task T-B, M5 task T-C). Original work, see km_keymap.h.
 *
 * Usage numbers: USB HID Usage Tables, Keyboard/Keypad Page (0x07): A..Z = 0x04..0x1D, 1..9 = 0x1E..0x26,
 * 0 = 0x27, Enter 0x28, Escape 0x29, Backspace 0x2A, Tab 0x2B, Space 0x2C, - 0x2D, = 0x2E, [ 0x2F, ] 0x30,
 * \ 0x31, ; 0x33, ' 0x34, , 0x36, . 0x37, / 0x38, Caps Lock 0x39, LeftControl..RightGUI = 0xE0..0xE7.
 * Key names and slot numbers: core/data/slotmap_measured.json (measured on the real keyboard).
 * The grave/tilde key does not exist on this board (Esc sits in its place), so layer 0 has no 0x35; the default
 * layer 1 provides it (Tab).
 * Layer 1 usages: F1..F12 = 0x3A..0x45, Insert 0x49, Home 0x4A, PageUp 0x4B, Delete 0x4C, End 0x4D, PageDown 0x4E,
 * Right 0x4F, Left 0x50, Down 0x51, Up 0x52, Grave 0x35 (HID Usage Tables, page 0x07).
 */
#include "km/km_keymap.h"

#include <string.h>

typedef struct {
    uint8_t     usage;  /* layer 0 */
    const char *name;   /* "" = no key on this slot */
} key_entry_t;

static const key_entry_t k_keys[KM_KEYMAP_SLOTS] = {
    /*  0 */ { 0xE1, "LSHIFT" },
    /*  1 */ { 0x0C, "I" },
    /*  2 */ { 0x14, "Q" },
    /*  3 */ { 0x2D, "-" },
    /*  4 */ { 0x20, "3" },
    /*  5 */ { 0xE0, "LCTRL" },
    /*  6 */ { 0x08, "E" },
    /*  7 */ { 0x19, "V" },
    /*  8 */ { 0x2A, "BACKSPACE" },
    /*  9 */ { 0x0D, "J" },
    /* 10 */ { 0x28, "ENTER" },
    /* 11 */ { 0x37, "." },
    /* 12 */ { 0x0F, "L" },
    /* 13 */ { 0x17, "T" },
    /* 14 */ { 0x00, "FN" },
    /* 15 */ { 0x00, "" },
    /* 16 */ { 0x1C, "Y" },
    /* 17 */ { 0x00, "" },
    /* 18 */ { 0x29, "ESC" },
    /* 19 */ { 0x25, "8" },
    /* 20 */ { 0x00, "" },
    /* 21 */ { 0x12, "O" },
    /* 22 */ { 0x1F, "2" },
    /* 23 */ { 0xE2, "LALT" },
    /* 24 */ { 0x00, "" },
    /* 25 */ { 0x06, "C" },
    /* 26 */ { 0x2E, "=" },
    /* 27 */ { 0x10, "M" },
    /* 28 */ { 0xE5, "RSHIFT" },
    /* 29 */ { 0x36, "," },
    /* 30 */ { 0x38, "/" },
    /* 31 */ { 0x22, "5" },
    /* 32 */ { 0x34, "'" },
    /* 33 */ { 0x00, "" },
    /* 34 */ { 0x0A, "G" },
    /* 35 */ { 0x00, "" },
    /* 36 */ { 0x39, "CAPS" },
    /* 37 */ { 0x18, "U" },
    /* 38 */ { 0x1D, "Z" },
    /* 39 */ { 0x13, "P" },
    /* 40 */ { 0x00, "" },
    /* 41 */ { 0x00, "" },
    /* 42 */ { 0x15, "R" },
    /* 43 */ { 0x1B, "X" },
    /* 44 */ { 0x30, "]" },
    /* 45 */ { 0x11, "N" },
    /* 46 */ { 0x00, "" },
    /* 47 */ { 0x0E, "K" },
    /* 48 */ { 0x33, ";" },
    /* 49 */ { 0x09, "F" },
    /* 50 */ { 0x00, "" },
    /* 51 */ { 0x00, "" },
    /* 52 */ { 0x0B, "H" },
    /* 53 */ { 0x00, "" },
    /* 54 */ { 0x2B, "TAB" },
    /* 55 */ { 0x26, "9" },
    /* 56 */ { 0x1E, "1" },
    /* 57 */ { 0x27, "0" },
    /* 58 */ { 0x00, "" },
    /* 59 */ { 0xE3, "LWIN" },
    /* 60 */ { 0x21, "4" },
    /* 61 */ { 0x05, "B" },
    /* 62 */ { 0x2F, "[" },
    /* 63 */ { 0x2C, "SPACE" },
    /* 64 */ { 0x31, "\\" },
    /* 65 */ { 0xE6, "RALT" },
    /* 66 */ { 0xE7, "RWIN" },
    /* 67 */ { 0x23, "6" },
    /* 68 */ { 0xE4, "RCTRL" },
    /* 69 */ { 0x00, "" },
    /* 70 */ { 0x24, "7" },
    /* 71 */ { 0x00, "" },
    /* 72 */ { 0x16, "S" },
    /* 73 */ { 0x04, "A" },
    /* 74 */ { 0x07, "D" },
    /* 75 */ { 0x1A, "W" },
};

/* ---- layers ---- */

typedef struct {
    uint8_t slot;
    uint8_t code;
} override_t;

/* Layer 0 is the table above, except that the Fn key (slot 14) holds layer 1. */
static const override_t k_layer0[] = {
    { 14, KM_CODE_HOLD_L1 },  /* FN */
};

/*
 * Default layer 1 (FN held). Every key that is not listed is transparent (works as on layer 0), every slot without
 * a key is 0. The key names are those of slotmap_measured.json; tests/test_layers.c checks them by name.
 *   number row   1..9 0 - =   F1..F12
 *   BACKSPACE    Delete            TAB   Grave (the board has no ` key)
 *   I J K L      Up Left Down Right (inverted T)
 *   U O P ;      Home End PageUp PageDown
 *   [ ]          Volume down / up       M  Mute      , . /  Previous / Play-Pause / Next
 *   Z X C V      switch to profile 1 / 2 / 3 / 4 (on release)
 */
static const override_t k_layer1[] = {
    { 56, 0x3A }, /* 1 -> F1 */   { 22, 0x3B }, /* 2 -> F2 */   {  4, 0x3C }, /* 3 -> F3 */
    { 60, 0x3D }, /* 4 -> F4 */   { 31, 0x3E }, /* 5 -> F5 */   { 67, 0x3F }, /* 6 -> F6 */
    { 70, 0x40 }, /* 7 -> F7 */   { 19, 0x41 }, /* 8 -> F8 */   { 55, 0x42 }, /* 9 -> F9 */
    { 57, 0x43 }, /* 0 -> F10 */  {  3, 0x44 }, /* - -> F11 */  { 26, 0x45 }, /* = -> F12 */
    {  8, 0x4C }, /* BACKSPACE -> Delete */
    { 54, 0x35 }, /* TAB -> Grave */
    {  1, 0x52 }, /* I -> Up */    {  9, 0x50 }, /* J -> Left */   { 47, 0x51 }, /* K -> Down */
    { 12, 0x4F }, /* L -> Right */
    { 37, 0x4A }, /* U -> Home */  { 21, 0x4D }, /* O -> End */    { 39, 0x4B }, /* P -> PageUp */
    { 48, 0x4E }, /* ; -> PageDown */
    { 62, KM_CODE_VOL_DOWN },   /* [ */  { 44, KM_CODE_VOL_UP },     /* ] */  { 27, KM_CODE_MUTE },  /* M */
    { 29, KM_CODE_PREV_TRACK }, /* , */  { 11, KM_CODE_PLAY_PAUSE }, /* . */  { 30, KM_CODE_NEXT_TRACK }, /* / */
    { 38, KM_CODE_PROFILE_1 },  /* Z */  { 43, KM_CODE_PROFILE_1 + 1u }, /* X */
    { 25, KM_CODE_PROFILE_1 + 2u }, /* C */  {  7, KM_CODE_PROFILE_4 }, /* V */
};

/* The built-in table entry. */
static uint8_t default_code(unsigned layer, unsigned slot)
{
    if (layer == 0) {
        for (unsigned i = 0; i < sizeof k_layer0 / sizeof k_layer0[0]; i++) {
            if (k_layer0[i].slot == slot) {
                return k_layer0[i].code;
            }
        }
        return k_keys[slot].usage;
    }
    if (k_keys[slot].name[0] == '\0') {
        return KM_USAGE_NONE;
    }
    if (layer == 1) {
        for (unsigned i = 0; i < sizeof k_layer1 / sizeof k_layer1[0]; i++) {
            if (k_layer1[i].slot == slot) {
                return k_layer1[i].code;
            }
        }
    }
    return KM_CODE_TRANSPARENT;
}

static uint8_t g_map[KM_LAYERS][KM_KEYMAP_SLOTS];
static uint8_t g_arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS];
static km_layer_opts_t g_opts = { KM_LAYER_TAP_TERM_DEFAULT, KM_LAYER_ONESHOT_DEFAULT, KM_TAP_POLICY_TIMEOUT };
static bool g_custom;
static uint32_t g_generation;

uint8_t km_keymap_code(unsigned layer, unsigned slot)
{
    if (layer >= KM_KEYMAP_LAYERS || slot >= KM_KEYMAP_SLOTS) {
        return KM_USAGE_NONE;
    }
    return g_custom ? g_map[layer][slot] : default_code(layer, slot);
}

uint8_t km_keymap_arg(unsigned layer, unsigned slot, unsigned idx)
{
    if (!g_custom || layer >= KM_KEYMAP_LAYERS || slot >= KM_KEYMAP_SLOTS || idx >= KM_KEYMAP_ARGS) {
        return 0;
    }
    return g_arg[layer][slot][idx];
}

const km_layer_opts_t *km_keymap_opts(void)
{
    return &g_opts;
}

void km_layer_opts_default(km_layer_opts_t *o)
{
    o->tap_term_ms = KM_LAYER_TAP_TERM_DEFAULT;
    o->oneshot_ms = KM_LAYER_ONESHOT_DEFAULT;
    o->tap_policy = KM_TAP_POLICY_TIMEOUT;
}

bool km_layer_opts_valid(const km_layer_opts_t *o)
{
    return o != NULL && o->tap_term_ms >= KM_LAYER_TAP_TERM_MIN && o->tap_term_ms <= KM_LAYER_TAP_TERM_MAX
        && (o->oneshot_ms == 0u || (o->oneshot_ms >= KM_LAYER_ONESHOT_MIN && o->oneshot_ms <= KM_LAYER_ONESHOT_MAX))
        && o->tap_policy <= KM_TAP_POLICY_LAST;
}

uint32_t km_keymap_generation(void)
{
    return g_generation;
}

uint8_t km_keymap_usage(unsigned layer, unsigned slot)
{
    const uint8_t code = km_keymap_code(layer, slot);
    return code < KM_CODE_EXT_FIRST ? code : (uint8_t)KM_USAGE_NONE;
}

bool km_keymap_code_valid_v1(uint8_t code)
{
    return code == KM_USAGE_NONE || (code >= 0x04u && code <= 0xE7u) || code >= KM_CODE_FUNC_FIRST;
}

bool km_keymap_code_valid_v2(uint8_t code)
{
    return km_keymap_code_valid_v1(code) || (code >= KM_CODE_EXT_FIRST && code <= KM_CODE_EXT_LAST);
}

bool km_keymap_code_valid(uint8_t code)
{
    return km_keymap_code_valid_v1(code) || (code >= KM_CODE_EXT_FIRST && code <= KM_CODE_EXT3_LAST);
}

static bool tap_action_valid(uint8_t a)
{
    return (a >= 0x04u && a <= 0xE7u) || (a >= KM_CODE_VOL_DOWN && a <= KM_CODE_NEXT_TRACK);
}

static bool layer_or_modifier_hold(uint8_t a)
{
    return a == KM_CODE_HOLD_L1 || a == KM_CODE_HOLD_L2 || a == KM_CODE_HOLD_L3 || km_usage_is_modifier(a);
}

static bool usage_valid(uint8_t a)
{
    return a >= 0x04u && a <= 0xE7u;
}

/* The settings format version 2 rule, exactly as it was before M6. */
bool km_keymap_entry_valid_v2(uint8_t code, uint8_t arg0, uint8_t arg1)
{
    switch (code) {
    case KM_CODE_TOGGLE:
        return arg0 >= 1u && arg0 < KM_LAYERS && arg1 == 0u;
    case KM_CODE_TAP_HOLD:
        return tap_action_valid(arg0) && layer_or_modifier_hold(arg1);
    case KM_CODE_ONESHOT_MOD:
        return km_usage_is_modifier(arg0) && arg1 == 0u;
    default:
        return km_keymap_code_valid_v2(code) && arg0 == 0u && arg1 == 0u;
    }
}

/* A gamepad axis argument: bits 0-2 axis (0..5), bit 3 negative side, bit 4 digital, bits 5-7 zero; a trigger has no negative side. */
static bool pad_axis_arg_valid(uint8_t a)
{
    const unsigned axis = a & 7u;
    return (a & ~KM_PAD_AXIS_MASK) == 0u && axis < KM_PAD_AXIS_COUNT && !(axis >= KM_PAD_AXIS_LT && (a & KM_PAD_AXIS_NEG) != 0u);
}

bool km_keymap_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1)
{
    switch (code) {
    case KM_CODE_TAP_HOLD:
        /* since format 3 the hold action may be any keyboard usage, but holding the tap action itself is pointless */
        return tap_action_valid(arg0) && (layer_or_modifier_hold(arg1) || (usage_valid(arg1) && arg1 != arg0));
    case KM_CODE_TOGGLE_KEY:
        return usage_valid(arg0) && arg1 == 0u;
    case KM_CODE_MOD_MORPH:
        return usage_valid(arg0) && arg1 < KM_MORPH_MAX;
    case KM_CODE_DKS:
        return arg0 < KM_DKS_MAX && arg1 == 0u;
    case KM_CODE_PAD_BTN:
        return arg0 >= KM_PAD_BTN_FIRST && arg0 <= KM_PAD_BTN_ARG_LAST && arg1 <= KM_PAD_BTN_START_MAX;
    case KM_CODE_PAD_AXIS:
        return pad_axis_arg_valid(arg0) && arg1 < KM_PAD_CURVES;
    case KM_CODE_LIGHT:
        return arg0 <= KM_LIGHT_ACT_LAST && arg1 == 0u;
    default:
        return km_keymap_entry_valid_v2(code, arg0, arg1);   /* TOGGLE, ONE-SHOT, 0xFE and every plain code are unchanged */
    }
}

bool km_keymap_entry_needs_v3(uint8_t code, uint8_t arg0, uint8_t arg1)
{
    return km_keymap_entry_valid(code, arg0, arg1) && !km_keymap_entry_valid_v2(code, arg0, arg1);
}

bool km_keymap_table_valid(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS])
{
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            if (!km_keymap_entry_valid(map[l][s], 0, 0)) {
                return false;
            }
        }
    }
    return true;
}

void km_keymap_default_table(uint8_t out[KM_LAYERS][KM_KEYMAP_SLOTS])
{
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            out[l][s] = default_code(l, s);
        }
    }
}

bool km_layers_set_table(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS],
                         const uint8_t arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS], const km_layer_opts_t *opts)
{
    g_generation++;
    km_layer_opts_default(&g_opts);
    if (map == NULL) {
        g_custom = false;
        return true;
    }
    bool ok = opts == NULL || km_layer_opts_valid(opts);
    for (unsigned l = 0; ok && l < KM_LAYERS; l++) {
        for (unsigned s = 0; ok && s < KM_KEYMAP_SLOTS; s++) {
            ok = km_keymap_entry_valid(map[l][s], arg ? arg[l][s][0] : 0u, arg ? arg[l][s][1] : 0u);
        }
    }
    if (!ok) {
        g_custom = false;  /* a damaged table never gets used: back to the built-in one */
        return false;
    }
    memcpy(g_map, map, sizeof g_map);
    if (arg != NULL) {
        memcpy(g_arg, arg, sizeof g_arg);
    } else {
        memset(g_arg, 0, sizeof g_arg);
    }
    if (opts != NULL) {
        g_opts = *opts;
    }
    g_custom = true;
    return true;
}

bool km_layers_set_map(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS])
{
    return km_layers_set_table(map, NULL, NULL);
}

bool km_keymap_slot_is_key(unsigned slot)
{
    return slot < KM_KEYMAP_SLOTS && k_keys[slot].name[0] != '\0';
}

const char *km_keymap_slot_name(unsigned slot)
{
    return slot < KM_KEYMAP_SLOTS ? k_keys[slot].name : "";
}
