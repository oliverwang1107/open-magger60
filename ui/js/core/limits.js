// THE one place that holds every number the configurator uses to decide what a trigger setting may be.
// (Checked against km_engine_cfg_valid of M5 task T-A, commit a441c13 on claude/m5-ta-rapid-trigger-988e21: floor KM_ENGINE_RT_MIN_MM100 = 20 for
// rt_down / rt_up / press (RAPID, not continuous); top and bottom dead zone each 0 or >= 20; rt_down <= 340 - bottom_dead;
// max(top_dead, 20) + bottom_dead + 20 <= 340; FIXED needs 10 <= release < press <= 340 and ignores every RAPID field; continuous
// RAPID ignores press / release. FIXED = "Normal", RAPID + continuous 0 = "Rapid Trigger", RAPID + continuous 1 = "Continuous RT".)
// Two layers, kept apart on purpose:
//
//   FW_*   what the keyboard's settings format accepts. A mirror of firmware/core/src/km_settings.c: km_settings_cfg_valid is
//          km_engine_cfg_valid (M5 task T-A), km_settings_usage_valid is km_keymap_code_valid (M5 task T-C). ui/tests/settings-codec.test.mjs checks it against the real C code with generated vectors
//          (firmware/tools/gen_settings_vector.py), so a firmware change that is not copied here fails a test.
//   UI_*   what the UI lets a person choose. Tighter than the firmware: the sensor noise floor (about +-0.15 mm at rest,
//          M5_PLAN.md section 1) means a threshold below ~0.20 mm would fire by itself. M5 task T-A (km_engine_cfg_valid) owns
//          the final rule; until its commit exists the documented floor is used. When it changes, change it HERE only.

export const TRAVEL_MM100 = 340;          // nominal travel, 3.40 mm (units of 0.01 mm everywhere in this file unless "Mm")

// ---- firmware settings format (km_settings.h; version 3 is the newest, 2 and 1 are still read; the writer uses the LOWEST version that holds the data) ----
export const FW_SETTINGS = Object.freeze({
  version: 3,                              // the newest version = the shape of the raw model; encodeSettings() writes 2 when nothing needs 3
  v2Version: 2,
  profiles: 4,
  layers: 4,
  slots: 76,
  keyCfgBytes: 14,
  optsBytes: 6,                            // per profile: tap_term u16, oneshot u16, tap_policy u8, reserved u8
  argBytes: 2,                             // per layer entry: two argument bytes after the code byte
  headerBytes: 12,
  size: 7944,                              // the version 2 blob, and the largest blob the writer produces: 12 + 4 * (76 * 14 + 6 + 4 * 76 + 4 * 76 * 2) + 4
  v1Size: 5488,                            // 12 + 4 * (76 * 14 + 4 * 76) + 4: the first format, read and migrated
  magic: [0x4b, 0x4d, 0x43, 0x53],         // "KMCS"
  minStep: 10,                             // FIXED: smallest release point (KM_ENGINE_CFG_MIN_RELEASE_MM100, 0.1 mm)
  rtMin: 20,                               // RAPID: smallest rapid-trigger distance / actuation / non-zero dead zone (KM_ENGINE_RT_MIN_MM100)
  // ---- version 3 (sections), km_settings.h ----
  keys: 61,                                // slots that carry a key, in slot order
  v3HeaderBytes: 16,
  v3SectionHeaderBytes: 4,                 // id u8, scope u8, length u16
  v3MaxSections: 64,
  v3MaxSize: 8176,                         // KM_STORE_MAX_PAYLOAD: one flash sector minus the store header
  v3MaxEncoded: 7308,                      // every section of every profile: the most the writer produces
  scopeGlobal: 0xff,
  optionalFrom: 0x80,                      // section ids from here on are skipped when unknown
});

// section ids and exact payload sizes (km_settings.h)
export const SECTION = Object.freeze({
  KEYCFG: 0x01, LAYERS: 0x02, DKS: 0x10, MORPH: 0x11, SOCD: 0x12, PAD: 0x20, DEVICE: 0x30,
});
export const SECTION_BYTES = Object.freeze({
  keycfg: 61 * 14, layers: 6 + 4 * 61 * 3, dksRec: 16, morphRec: 4, socdRec: 4, pad: 4 * 4 + 8, device: 16,
});

// the 15 slots without a key (km_keymap.c: key name ""); version 3 does not store them
export const FW_NO_KEY_SLOTS = Object.freeze([15, 17, 20, 24, 33, 35, 40, 41, 46, 50, 51, 53, 58, 69, 71]);
export const FW_KEY_SLOTS = Object.freeze(Array.from({ length: FW_SETTINGS.slots }, (_, i) => i).filter((i) => !FW_NO_KEY_SLOTS.includes(i)));
export const slotHasKey = (slot) => Number.isInteger(slot) && slot >= 0 && slot < FW_SETTINGS.slots && !FW_NO_KEY_SLOTS.includes(slot);

// M6 records (km_m6_types.h)
export const FW_M6 = Object.freeze({
  dksMax: 8, morphMax: 8, socdMax: 4, padCurves: 4,
  dksActMin: 40, dksGapMin: 40, dksBotDefault: 310, dksActDefault: 120, dksFlags: 0x01,
  morphFlags: 0x01, socdModeLast: 4, socdFlags: 0x01,
  padStartMin: 4, padEndMax: 68, padSpanMin: 4, padShapeLast: 3, padFlags: 0x01, padStartDefault: 4, padEndDefault: 64,
  padDeadzoneMax: 50, padSmoothMax: 3, padModeLast: 2, filterModeLast: 2, lightActLast: 7,
  padAxisCount: 6, padBtnLast: 20, padBtnStartMax: 68,
});
export const defaultPad = () => ({
  curves: Array.from({ length: FW_M6.padCurves }, () => ({ start: FW_M6.padStartDefault, end: FW_M6.padEndDefault, shape: 0, flags: 0 })),
  deadzone: 10, circular: 1, smooth: 0,
});
export const defaultDevice = () => ({ padMode: 0, filterMode: 0 });

// layer behaviour options of a profile (km_keymap.h km_layer_opts_t); ms
export const FW_LAYER_OPTS = Object.freeze({
  tapTermMin: 30, tapTermMax: 1000, tapTermDefault: 200,
  oneShotMin: 100, oneShotMax: 10000, oneShotDefault: 2000,   // 0 = the armed modifier waits for the next key
  policies: 3,                                                // 0 timeout, 1 permissive, 2 other key
});
export const TAP_POLICY = Object.freeze({ TIMEOUT: 0, PERMISSIVE: 1, OTHER_KEY: 2 });

export const MODE_FIXED = 0;
export const MODE_RAPID = 1;

/** Usage-byte rule of km_settings_usage_valid() = km_keymap_code_valid(): 0 (no key), 0x04..0xE7 (keyboard page), 0xF0..0xFF (function codes). */
export const FW_USAGE_RULE = Object.freeze({
  first: 0x04, last: 0xe7, funcFirst: 0xf0,
  // settings format version 2: 0xE8 toggle layer, 0xE9 tap-hold, 0xEA one-shot modifier (these need their argument bytes, see entryProblem)
  extFirst: 0xe8, extLast: 0xef,   // 0xEB..0xEF: M6 (format 3)
  // Integrated firmware: every code from 0xF0 up is accepted (0xFB/0xFC hold layer 2/3, 0xFD/0xFE reserved, 0xFF transparent).
  // Older builds (M5 task T-D alone) stopped at 0xFA; profile_codec.js still knows how to save for such a rule (it writes
  // "transparent" out as the base layer's code), see the tests that pass a narrower rule.
  funcLast: 0xff,
  // settings format version 3 (M6): the codes 0xEB..0xEF exist, 0xFD takes arguments, a tap-hold hold action may be any key
  v3: true,
});
// the rule of a firmware that writes settings format version 2 only (before M6)
export const FW_USAGE_RULE_V2 = Object.freeze({ ...FW_USAGE_RULE, extLast: 0xea, v3: false });
export const usageCodeValid = (c, rule = FW_USAGE_RULE) =>
  Number.isInteger(c) && (c === 0 || (c >= rule.first && c <= rule.last) || (c >= rule.funcFirst && c <= rule.funcLast)
    || (rule.extFirst !== undefined && c >= rule.extFirst && c <= rule.extLast));

// internal function codes of the key map (km_keymap.h / LAYERS_NOTES.md)
export const CODE = Object.freeze({
  NONE: 0x00, HOLD_L1: 0xf0, PROFILE_1: 0xf1, PROFILE_4: 0xf4, VOL_DOWN: 0xf5, VOL_UP: 0xf6, MUTE: 0xf7,
  PREV: 0xf8, PLAY_PAUSE: 0xf9, NEXT: 0xfa, HOLD_L2: 0xfb, HOLD_L3: 0xfc, RESERVED_1: 0xfd, RESERVED_2: 0xfe, TRANSPARENT: 0xff,
  TOGGLE: 0xe8, TAP_HOLD: 0xe9, ONESHOT: 0xea,   // extended codes: arg0 / arg1 follow (km_keymap.h)
  TOGGLE_KEY: 0xeb, MOD_MORPH: 0xec, DKS: 0xed, PAD_BTN: 0xee, PAD_AXIS: 0xef,   // M6 (format 3)
  LIGHT: 0xfd,                                   // = RESERVED_1: with arg0 0 it still does nothing, arg0 1..7 is a light action (format 3)
});
// consumer-page usage <-> function code (km_layers.c)
export const CONSUMER_CODES = Object.freeze({ 0xea: CODE.VOL_DOWN, 0xe9: CODE.VOL_UP, 0xe2: CODE.MUTE, 0xb6: CODE.PREV, 0xcd: CODE.PLAY_PAUSE, 0xb5: CODE.NEXT });

const isMediaCode = (a) => a >= CODE.VOL_DOWN && a <= CODE.NEXT;
const isModifierUsage = (a) => a >= 0xe0 && a <= 0xe7;
/** tap action of a tap-hold entry: any keyboard usage or a media code */
export const tapActionValid = (a) => Number.isInteger(a) && ((a >= 0x04 && a <= 0xe7) || isMediaCode(a));
/** hold action of a tap-hold entry: a layer hold or a modifier usage (format 2); format 3 also takes any other keyboard usage (see entryProblem) */
export const holdActionValid = (a) => a === CODE.HOLD_L1 || a === CODE.HOLD_L2 || a === CODE.HOLD_L3 || (Number.isInteger(a) && isModifierUsage(a));
const isUsage = (a) => Number.isInteger(a) && a >= 0x04 && a <= 0xe7;
const padAxisArgValid = (a) => {
  const axis = a & 7;
  return (a & ~0x1f) === 0 && axis < FW_M6.padAxisCount && !(axis >= 4 && (a & 0x08) !== 0);
};

/**
 * null when one table entry (code byte + its two argument bytes) is acceptable, else "code" (the byte itself is refused) or "args".
 * Same rule as km_keymap_entry_valid: arguments are 0 unless the code uses them.
 */
export function entryProblem(code, a0, a1, rule = FW_USAGE_RULE) {
  if (!usageCodeValid(code, rule)) return "code";
  if (!Number.isInteger(a0) || !Number.isInteger(a1)) return "args";
  switch (code) {
    case CODE.TOGGLE: return a0 >= 1 && a0 < FW_SETTINGS.layers && a1 === 0 ? null : "args";
    case CODE.TAP_HOLD:
      return tapActionValid(a0) && (holdActionValid(a1) || (rule.v3 && isUsage(a1) && a1 !== a0)) ? null : "args";
    case CODE.ONESHOT: return isModifierUsage(a0) && a1 === 0 ? null : "args";
    case CODE.TOGGLE_KEY: return isUsage(a0) && a1 === 0 ? null : "args";
    case CODE.MOD_MORPH: return isUsage(a0) && a1 >= 0 && a1 < FW_M6.morphMax ? null : "args";
    case CODE.DKS: return a0 >= 0 && a0 < FW_M6.dksMax && a1 === 0 ? null : "args";
    case CODE.PAD_BTN: return a0 >= 1 && a0 <= FW_M6.padBtnLast && a1 >= 0 && a1 <= FW_M6.padBtnStartMax ? null : "args";
    case CODE.PAD_AXIS: return a0 >= 0 && a0 <= 0xff && padAxisArgValid(a0) && a1 >= 0 && a1 < FW_M6.padCurves ? null : "args";
    case CODE.LIGHT: return rule.v3 ? (a0 >= 0 && a0 <= FW_M6.lightActLast && a1 === 0 ? null : "args") : (a0 === 0 && a1 === 0 ? null : "args");
    default: return a0 === 0 && a1 === 0 ? null : "args";
  }
}

/** True when the entry is acceptable now but a settings format version 2 blob could not hold it (so a blob that has one is written as version 3). */
export const entryNeedsV3 = (code, a0, a1) => entryProblem(code, a0, a1, FW_USAGE_RULE) === null && entryProblem(code, a0, a1, FW_USAGE_RULE_V2) !== null;

// ---- M6 record rules (km_settings.c km_settings_*_valid); each returns null or the name of the first rule broken ----
const u8 = (v) => Number.isInteger(v) && v >= 0 && v <= 0xff;
const u16 = (v) => Number.isInteger(v) && v >= 0 && v <= 0xffff;

/** DKS record { tAct, tBot, flags, acts: [[usage, events] x 4] } */
export function dksProblem(r) {
  if (!r || !u16(r.tAct) || !u16(r.tBot) || !u8(r.flags) || !Array.isArray(r.acts) || r.acts.length !== 4) return "shape";
  if ((r.flags & ~FW_M6.dksFlags) !== 0) return "flags";
  const bot = r.tBot === 0 ? FW_M6.dksBotDefault : r.tBot;
  if (r.tAct < FW_M6.dksActMin || bot > TRAVEL_MM100 || bot < r.tAct + FW_M6.dksGapMin) return "depth";
  for (const a of r.acts) {
    if (!Array.isArray(a) || a.length !== 2 || !u8(a[0]) || !u8(a[1])) return "shape";
    if (a[0] === 0 ? a[1] !== 0 : !isUsage(a[0])) return "action";
  }
  return null;
}
/** Mod-Morph record { mask, usage, flags } */
export const morphProblem = (r) => (!r || !u8(r.mask) || !u8(r.usage) || !u8(r.flags) ? "shape" : r.mask === 0 ? "mask" : !isUsage(r.usage) ? "usage" : (r.flags & ~FW_M6.morphFlags) !== 0 ? "flags" : null);
/** SOCD pair { a, b, mode, flags } */
export const socdProblem = (r) => (!r || !u8(r.a) || !u8(r.b) || !u8(r.mode) || !u8(r.flags) ? "shape"
  : !slotHasKey(r.a) || !slotHasKey(r.b) || r.a === r.b ? "slots" : r.mode > FW_M6.socdModeLast ? "mode" : (r.flags & ~FW_M6.socdFlags) !== 0 ? "flags" : null);
/** gamepad curve { start, end, shape, flags } (0.05 mm units) */
export const padCurveProblem = (c) => (!c || !u8(c.start) || !u8(c.end) || !u8(c.shape) || !u8(c.flags) ? "shape"
  : c.start < FW_M6.padStartMin || c.end > FW_M6.padEndMax || c.end < c.start + FW_M6.padSpanMin ? "range" : c.shape > FW_M6.padShapeLast ? "shape_id" : (c.flags & ~FW_M6.padFlags) !== 0 ? "flags" : null);
/** gamepad section { curves: [4], deadzone, circular, smooth } */
export function padProblem(p) {
  if (!p || !Array.isArray(p.curves) || p.curves.length !== FW_M6.padCurves) return "shape";
  for (const c of p.curves) { const why = padCurveProblem(c); if (why) return `curve_${why}`; }
  if (!u8(p.deadzone) || !u8(p.circular) || !u8(p.smooth)) return "shape";
  return p.deadzone > FW_M6.padDeadzoneMax || p.circular > 1 || p.smooth > FW_M6.padSmoothMax ? "options" : null;
}
/** device options { padMode, filterMode } */
export const deviceProblem = (d) => (!d || !u8(d.padMode) || !u8(d.filterMode) ? "shape" : d.padMode > FW_M6.padModeLast || d.filterMode > FW_M6.filterModeLast ? "mode" : null);

/** null, or the name of the first rule a layer options object {tapTerm, oneshot, policy} breaks (km_layer_opts_valid). */
export function optsProblem(o) {
  const L = FW_LAYER_OPTS;
  const int = (v) => Number.isInteger(v) && v >= 0 && v <= 0xffff;
  if (!o || !int(o.tapTerm) || !int(o.oneshot) || !Number.isInteger(o.policy)) return "type";
  if (o.tapTerm < L.tapTermMin || o.tapTerm > L.tapTermMax) return "tap_term";
  if (o.oneshot !== 0 && (o.oneshot < L.oneShotMin || o.oneshot > L.oneShotMax)) return "oneshot";
  if (o.policy < 0 || o.policy >= L.policies) return "policy";
  return null;
}

// ---- UI limits, in millimetres ----
export const NOISE_FLOOR_MM = 0.2;        // no threshold, rapid-trigger step or (non-zero) dead zone below this
export const HYSTERESIS_MIN_MM = 0.1;     // the release point sits at least this far BELOW the actuation point
export const TRAVEL_MM = TRAVEL_MM100 / 100;

export const UI_LIMITS = Object.freeze({
  actuation: { min: Math.round((NOISE_FLOOR_MM + HYSTERESIS_MIN_MM) * 100) / 100, max: TRAVEL_MM }, // the release point needs room below it
  release: { min: NOISE_FLOOR_MM, max: TRAVEL_MM - HYSTERESIS_MIN_MM },
  rt: { min: NOISE_FLOOR_MM, max: TRAVEL_MM },
  dead: { min: 0, max: 1.0 },             // 0 = off; a value between 0 and the floor is not allowed
});

/** Human-readable reasons, keyed by code; the UI turns them into text through i18n ("trig.reason.<code>"). */
export const REASONS = Object.freeze(["below_noise", "above_travel", "release_order", "dead_floor", "dead_sum", "rt_reach"]);
