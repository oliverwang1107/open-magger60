// The gamepad page's model (M6 package P9b), no DOM: what a key can be bound to on the gamepad, how bindings are stored in the key map
// (opaque `fw` bindings of the P0 codec), the per-profile response curves and options, the device option "pad mode", checks that explain
// problems in plain terms, the live preview state, and the SDL mapping string.
//
// How it is stored (settings format 3, see profile_codec.js / settings_codec.js):
//   - a binding is a key-map entry: code 0xEE (button / hat, arg0 = 1..16 button, 17..20 d-pad up / down / left / right, arg1 = 0 follow the
//     key or 1..68 = own start depth in 0.05 mm) or 0xEF (axis, arg0 = axis | 0x08 negative | 0x10 digital, arg1 = curve 0..3). The UI model
//     keeps it as { type: "fw", index: code << 16 | arg0 << 8 | arg1 }, which `bindingToCode` writes back byte for byte.
//   - curves and options live in `profile.m6.pad = { curves: [{ start, end, shape, flags } x 4], deadzone, circular, smooth }` (0.05 mm units)
//   - pad mode lives in the global device options, `app.m6.device.padMode` (0 off, 1 HID gamepad, 2 XInput-style gamepad, M6 P11)
import { CODE, FW_M6, defaultDevice, defaultPad, deviceProblem } from "./limits.js";
import { deviceIsDefault, padIsDefault } from "./settings_codec.js";
import { LAYER_COUNT, unreachableLayers } from "./profile.js";
import { VID, PID } from "../device/ids.js";
import {
  CURVE_MIRROR, PAD_AXIS, PAD_AXIS_DIGITAL, PAD_AXIS_NEG, PAD_BTN_MIN_ACT_MM100, PAD_CODE_AXIS, PAD_CODE_BTN, PAD_ENGINE_PRESS_MM100,
  PAD_ENGINE_RELEASE_MM100, PAD_MIN_START_MM100, PadCore, SHAPE, curveEval, defaultCurve, entryValid,
} from "./pad_sim.js";

export { CURVE_MIRROR, SHAPE, curveEval, defaultCurve, entryValid };

export const PAD_MODE = Object.freeze({ OFF: 0, HID: 1, XINPUT: 2 });
/** An XInput pad has 11 buttons (A B X Y LB RB Back Start L3 R3 Guide = buttons 1..11 here); 12..16 exist only in the HID gamepad. */
export const XINPUT_BUTTONS = 11;
export const CURVE_COUNT = FW_M6.padCurves;
export const DEADZONE_MAX = FW_M6.padDeadzoneMax;
/** Smallest depth difference between the start and the end of a curve, in 0.05 mm units (0.20 mm; the firmware's rule, FW_M6.padSpanMin). */
export const CURVE_SPAN_MIN = FW_M6.padSpanMin;
export const CURVE_START_MIN = FW_M6.padStartMin;
export const CURVE_END_MAX = FW_M6.padEndMax;
export const MM_PER_UNIT = 0.05;
/** Smallest own start depth of a depth button, in 0.05 mm (0.20 mm; the firmware raises a smaller value to this) */
export const BTN_ACT_MIN = PAD_BTN_MIN_ACT_MM100 / 5;
export const BTN_ACT_MAX = FW_M6.padBtnStartMax;

export const unitsToMm = (u) => Math.round(u * 5) / 100;
export const mmToUnits = (mm) => Math.round(mm * 20);

// ---- what a key can be bound to ---------------------------------------------------------------------------------------------------

/** The names the buttons get in SDL / Steam (tools/magger60_pad_map.py BUTTON_ROLES); the other buttons are plain numbers. */
export const BUTTON_ROLES = Object.freeze({ a: 1, b: 2, x: 3, y: 4, leftshoulder: 5, rightshoulder: 6, back: 7, start: 8, leftstick: 9, rightstick: 10, guide: 11, misc1: 12 });
/** Face text of the gamepad drawing and the key caps */
export const BUTTON_CAPS = Object.freeze({ 1: "A", 2: "B", 3: "X", 4: "Y", 5: "LB", 6: "RB", 7: "Back", 8: "Start", 9: "L3", 10: "R3", 11: "Guide" });

const STICK_AXES = [["lx", PAD_AXIS.LX, "L", "←", "→"], ["ly", PAD_AXIS.LY, "L", "↑", "↓"], ["rx", PAD_AXIS.RX, "R", "←", "→"], ["ry", PAD_AXIS.RY, "R", "↑", "↓"]];
const HAT = [["up", 17, "↑"], ["down", 18, "↓"], ["left", 19, "←"], ["right", 20, "→"]];

/**
 * Every gamepad function a key can have. { id, kind: "axis" | "button" | "hat", group, cap (short key-cap text), axis?, neg?, button? }
 * Stick targets: a stick axis goes negative (left / up) or positive (right / down); LY negative is up, as the report says.
 */
export const TARGETS = Object.freeze([
  ...STICK_AXES.flatMap(([n, axis, side, neg, pos]) => [
    { id: `${n}-`, kind: "axis", group: side === "L" ? "lstick" : "rstick", axis, neg: true, cap: `${side}${neg}` },
    { id: `${n}+`, kind: "axis", group: side === "L" ? "lstick" : "rstick", axis, neg: false, cap: `${side}${pos}` },
  ]),
  { id: "lt", kind: "axis", group: "trigger", axis: PAD_AXIS.LT, neg: false, cap: "LT" },
  { id: "rt", kind: "axis", group: "trigger", axis: PAD_AXIS.RT, neg: false, cap: "RT" },
  ...Array.from({ length: 16 }, (_, i) => ({ id: `b${i + 1}`, kind: "button", group: "button", button: i + 1, cap: BUTTON_CAPS[i + 1] ?? `B${i + 1}` })),
  ...HAT.map(([n, button, arrow]) => ({ id: `hat-${n}`, kind: "hat", group: "dpad", button, cap: `D${arrow}` })),
]);
export const TARGET_GROUPS = Object.freeze(["lstick", "rstick", "trigger", "button", "dpad"]);
const TARGET_BY_ID = new Map(TARGETS.map((x) => [x.id, x]));
export const targetById = (id) => TARGET_BY_ID.get(id) ?? null;

/** A binding "spec": { target: id, digital: bool, curve: 0..3, act: 0 | 4..68 } (digital and curve are for axes, act for buttons and d-pad) */
export const newSpec = (target) => {
  const tg = targetById(target);
  return tg?.kind === "axis" ? { target, digital: false, curve: tg.group === "trigger" ? 1 : 0, act: 0 } : { target, digital: false, curve: 0, act: 0 };
};

/** Reason code if `spec` cannot be stored, else null. The page turns the code into a sentence ("pad.bad.<code>"). */
export function specProblem(spec) {
  const tg = spec && targetById(spec.target);
  if (!tg) return "target";
  if (tg.kind === "axis") {
    if (!Number.isInteger(spec.curve) || spec.curve < 0 || spec.curve >= CURVE_COUNT) return "curve";
    return null;
  }
  if (!Number.isInteger(spec.act) || spec.act < 0 || spec.act > BTN_ACT_MAX) return "act_range";
  if (spec.act !== 0 && spec.act < BTN_ACT_MIN) return "act_low";
  return null;
}

/** The key-map entry of a spec as the three bytes { code, arg0, arg1 }. Throws when the spec has a problem. */
export function specToEntry(spec) {
  const why = specProblem(spec);
  if (why) throw new RangeError(`gamepad binding refused: ${why}`);
  const tg = targetById(spec.target);
  if (tg.kind === "axis") return { code: CODE.PAD_AXIS, arg0: tg.axis | (tg.neg ? PAD_AXIS_NEG : 0) | (spec.digital ? PAD_AXIS_DIGITAL : 0), arg1: spec.curve };
  return { code: CODE.PAD_BTN, arg0: tg.button, arg1: spec.act };
}

/** The UI binding of a spec (what goes into profile.keys[id].bindings[layer]) */
export const specToBinding = (spec) => {
  const e = specToEntry(spec);
  return { type: "fw", index: (e.code << 16) | (e.arg0 << 8) | e.arg1 };
};

/** The key-map entry { code, arg0, arg1 } of a binding, or null when it is not an opaque `fw` binding. */
export function entryOfBinding(b) {
  if (!b || b.type !== "fw" || !Number.isInteger(b.index) || b.index < 0 || b.index > 0xffffff) return null;
  return { code: b.index >> 16, arg0: (b.index >> 8) & 0xff, arg1: b.index & 0xff };
}

/** The spec of a binding if it is a valid gamepad entry, else null (not a gamepad binding, or an entry the firmware would ignore). */
export function specOfBinding(b) {
  const e = entryOfBinding(b);
  if (!e || (e.code !== CODE.PAD_AXIS && e.code !== CODE.PAD_BTN) || !entryValid(e.code, e.arg0, e.arg1)) return null;
  if (e.code === CODE.PAD_AXIS) {
    const axis = e.arg0 & 7;
    const neg = (e.arg0 & PAD_AXIS_NEG) !== 0;
    const tg = TARGETS.find((x) => x.kind === "axis" && x.axis === axis && x.neg === neg);
    return tg ? { target: tg.id, digital: (e.arg0 & PAD_AXIS_DIGITAL) !== 0, curve: e.arg1, act: 0 } : null;
  }
  const tg = TARGETS.find((x) => (x.kind === "button" || x.kind === "hat") && x.button === e.arg0);
  return tg ? { target: tg.id, digital: false, curve: 0, act: e.arg1 } : null;
}

export const sameSpec = (a, b) => !!a && !!b && a.target === b.target && a.digital === b.digital && a.act === b.act && (targetById(a.target).kind !== "axis" || a.curve === b.curve);

/** Does a binding take over a key for the gamepad (any gamepad table entry, valid or not)? */
export const isPadBinding = (b) => {
  const e = entryOfBinding(b);
  return !!e && (e.code === CODE.PAD_AXIS || e.code === CODE.PAD_BTN);
};

// ---- bindings of a profile ----------------------------------------------------------------------------------------------------------

/** Map slot -> spec of the keys of `profile` that are gamepad keys on `layer`. */
export function padKeys(profile, layout, layer) {
  const out = new Map();
  for (const k of layout.keys) {
    const spec = specOfBinding(profile.keys[k.id]?.bindings[layer]);
    if (spec) out.set(k.slot, spec);
  }
  return out;
}

/** How many keys are gamepad keys on each layer: [n0, n1, n2, n3] */
export function padKeyCounts(profile, layout) {
  return Array.from({ length: LAYER_COUNT }, (_, l) => padKeys(profile, layout, l).size);
}

/**
 * Give keys a gamepad function (or take it away). `edits` = [{ keyId, layer, spec | null }]; null removes it: the key gets
 * `restore(key, layer)` back (the page passes the key map's own default for that key and layer, null = transparent). Returns the number of keys changed.
 */
export function applyBindings(profile, layout, edits, restore = () => null) {
  let n = 0;
  for (const e of edits) {
    const data = profile.keys[e.keyId];
    if (!data) continue;
    const key = layout.byId.get(e.keyId);
    if (e.spec) data.bindings[e.layer] = specToBinding(e.spec);
    else if (isPadBinding(data.bindings[e.layer])) data.bindings[e.layer] = restore(key, e.layer);
    else continue;
    n++;
  }
  return n;
}

/** Quick starts: key id per target (this keyboard has no arrow keys: WASD, IJKL, QE and TFGH are the natural groups). */
export const PRESETS = Object.freeze({
  wasd: { "ly-": "KeyW", "lx-": "KeyA", "ly+": "KeyS", "lx+": "KeyD" },
  ijkl: { "ry-": "KeyI", "rx-": "KeyJ", "ry+": "KeyK", "rx+": "KeyL" },
  triggers: { lt: "KeyQ", rt: "KeyE" },
  tfgh: { "hat-up": "KeyT", "hat-left": "KeyF", "hat-down": "KeyG", "hat-right": "KeyH" },
});

/** Edits for a preset on a layout (keys that the layout does not have are skipped). */
export function presetEdits(name, layout, layer) {
  const map = PRESETS[name];
  if (!map) return [];
  return Object.entries(map).filter(([, id]) => layout.byId.has(id)).map(([target, keyId]) => ({ keyId, layer, spec: newSpec(target) }));
}

// ---- curves and options ---------------------------------------------------------------------------------------------------------

export const padOf = (profile) => structuredClone(profile.m6?.pad ?? defaultPad());

/** Store the pad section of a profile; a default pad leaves no `m6.pad` behind (so a profile that does not use it still saves as format 2). */
export function setPad(profile, pad) {
  const m6 = { ...(profile.m6 ?? {}) };
  if (padIsDefault(pad)) delete m6.pad; else m6.pad = structuredClone(pad);
  if (Object.keys(m6).length) profile.m6 = m6; else delete profile.m6;
}

/**
 * Change one field of a curve. `field`: start | end (0.05 mm units) | shape | reverse. A value outside the rules is moved to the nearest
 * allowed one and the reason is returned: { curve, note } with note = null | "start_low" | "end_high" | "start_high" | "end_low".
 * Start and end never push each other: the person drags one point, the other stays where it is.
 */
export function changeCurve(curve, field, value) {
  const c = { start: curve.start, end: curve.end, shape: curve.shape, flags: curve.flags };
  let note = null;
  if (field === "start") {
    const v = Math.round(value);
    const max = c.end - CURVE_SPAN_MIN;
    c.start = Math.min(max, Math.max(CURVE_START_MIN, v));
    if (v < CURVE_START_MIN) note = "start_low"; else if (v > max) note = "start_high";
  } else if (field === "end") {
    const v = Math.round(value);
    const min = c.start + CURVE_SPAN_MIN;
    c.end = Math.min(CURVE_END_MAX, Math.max(min, v));
    if (v > CURVE_END_MAX) note = "end_high"; else if (v < min) note = "end_low";
  } else if (field === "shape") {
    c.shape = Math.min(SHAPE.S, Math.max(SHAPE.LINEAR, Math.round(value)));
  } else if (field === "reverse") {
    c.flags = value ? (c.flags | CURVE_MIRROR) : (c.flags & ~CURVE_MIRROR);
  }
  return { curve: c, note };
}

/** Points to draw a curve: depth in mm (0..3.4) and value in 0..1, `n` + 1 points */
export function curvePoints(curve, n = 68) {
  const pts = [];
  for (let i = 0; i <= n; i++) {
    const depth = Math.round((340 * i) / n);
    pts.push({ mm: depth / 100, v: curveEval(curve, depth) / 32767 });
  }
  return pts;
}

export const CURVE_PRESETS = Object.freeze(["stick", "trigger", "fine", "s"]);
export const curvePreset = (id) => defaultCurve(Math.max(0, CURVE_PRESETS.indexOf(id)));

/** How many keys use each of the four curves, across all layers: [n0..n3] (digital axes do not use a curve) */
export function curveUse(profile, layout) {
  const use = new Array(CURVE_COUNT).fill(0);
  for (let l = 0; l < LAYER_COUNT; l++) {
    for (const spec of padKeys(profile, layout, l).values()) {
      if (targetById(spec.target).kind === "axis" && !spec.digital) use[spec.curve]++;
    }
  }
  return use;
}

// ---- device option -------------------------------------------------------------------------------------------------------------------

/** The global device options of an app (`app.m6.device`); the default when the app has none. */
export const deviceOf = (app) => ({ ...defaultDevice(), ...(app.m6?.device ?? {}) });

export function padModeOf(app) {
  return deviceOf(app).padMode;
}

/** New device options with another pad mode; null when the mode is refused. Other options and unknown sections are kept. */
export function withPadMode(m6, mode) {
  const next = { device: { ...defaultDevice(), ...(m6?.device ?? {}), padMode: mode }, unknown: m6?.unknown ?? [] };
  return deviceProblem(next.device) ? null : next;
}

/** Set the pad mode in the local model of the app. `edit` triggers the usual "changed" bookkeeping (dirty check, save button). */
export function setPadMode(app, mode) {
  const next = withPadMode(app.m6, mode);
  if (!next) return false;
  app.edit(() => { app.m6 = next; });
  return true;
}

export { deviceIsDefault };

// ---- checks that explain a situation ---------------------------------------------------------------------------------------------------

/**
 * What is worth telling the person about the profile's gamepad keys. Each item: { id, level: "warn" | "info", n? }; the page has a sentence
 * for every id (pad.chk.<id>).
 *   off_but_bound     keys are bound to the gamepad but pad mode is off: those keys do nothing at all (the keyboard no longer types with them)
 *   xinput_dropped    pad mode is XInput and keys are bound to buttons 12..16, which an XInput pad does not have: they send nothing
 *   unreachable       a layer with gamepad keys cannot be reached by any key
 *   half_axis         a stick direction is bound but the opposite one is not: the stick can only move one way
 */
export function checkPad(profile, layout, mode) {
  const out = [];
  const counts = padKeyCounts(profile, layout);
  const total = counts.reduce((a, b) => a + b, 0);
  if (total > 0 && mode === PAD_MODE.OFF) out.push({ id: "off_but_bound", level: "warn", n: total });
  const unreachable = unreachableLayers(profile, layout).filter((l) => counts[l] > 0);
  if (unreachable.length) out.push({ id: "unreachable", level: "warn", n: unreachable.length, layers: unreachable });
  const one = [];
  for (let l = 0; l < LAYER_COUNT; l++) {
    const have = new Set([...padKeys(profile, layout, l).values()].filter((s) => targetById(s.target).kind === "axis").map((s) => s.target));
    for (const [n] of STICK_AXES) {
      if (have.has(`${n}-`) !== have.has(`${n}+`)) one.push({ layer: l, axis: n });
    }
  }
  if (one.length) out.push({ id: "half_axis", level: "info", n: one.length, axes: one });
  if (mode === PAD_MODE.XINPUT) {
    let dropped = 0;
    for (let l = 0; l < LAYER_COUNT; l++) {
      for (const s of padKeys(profile, layout, l).values()) {
        const tg = targetById(s.target);
        if (tg.kind === "button" && tg.button > XINPUT_BUTTONS) dropped++;
      }
    }
    if (dropped) out.push({ id: "xinput_dropped", level: "info", n: dropped });
  }
  return out;
}

// ---- live preview -------------------------------------------------------------------------------------------------------------------------

/**
 * Drives a PadCore from the live depth feed: which keys are "down" is decided the way the keyboard does for a gamepad key (key engine,
 * fixed trigger, down from 0.20 mm, up below 0.10 mm), then the same gamepad core as in the firmware turns depths into a report.
 * Not a promise of what the keyboard sends (rapid trigger and calibration are not modelled), a faithful picture of the curves.
 */
export class PadPreview {
  constructor() {
    this.core = new PadCore();
    this.bound = new Map();   // slot -> { code, arg0, arg1 }
    this.down = new Set();
    this.sig = "";
    this.report = this.core.cur;
  }

  /** Load curves, options and the key bindings of a layer: { pad, keys: Map slot -> spec }. A change resets the preview like a profile switch does. */
  configure(pad, keys) {
    const sig = JSON.stringify([pad.curves, pad.deadzone, pad.circular, pad.smooth, [...keys].map(([s, sp]) => [s, sp])]);
    if (sig === this.sig) return;
    this.sig = sig;
    this.core.setConfig(pad.curves, { deadzone: pad.deadzone, circular: pad.circular, smooth: pad.smooth });
    this.bound = new Map([...keys].map(([slot, spec]) => [slot, { slot, ...specToEntry(spec) }]));
    this.down = new Set();
  }

  /** One step: depths = 0.01 mm per slot (the live feed's array). Returns the report. */
  step(depths, nowMs) {
    const keys = [];
    for (const [slot, e] of this.bound) {
      const d = depths[slot] ?? 0;
      const was = this.down.has(slot);
      const on = was ? d > PAD_ENGINE_RELEASE_MM100 : d >= PAD_ENGINE_PRESS_MM100;
      if (on) { this.down.add(slot); keys.push({ slot, code: e.code, arg0: e.arg0, arg1: e.arg1 }); } else this.down.delete(slot);
    }
    this.report = this.core.update({ keys, allowed: true, epoch: 0, nowMs: Math.floor(nowMs), depth: (slot) => depths[slot] ?? 0 }).report;
    return this.report;
  }
}

/** Fraction -1..1 (sticks) or 0..1 (triggers) of a report axis, for drawing */
export const axisFraction = (v) => v / 32767;

// ---- SDL mapping string ---------------------------------------------------------------------------------------------------------------

export const PAD_NAME = "Magger60 Gamepad";
/** bcdDevice of the gamepad descriptor variant (M6 plan 3.1.5); only the Linux GUID carries it */
export const PAD_BCD = 0x0002;
export const PLATFORMS = Object.freeze(["windows", "linux", "mac"]);

const hex2 = (n) => (n & 0xff).toString(16).padStart(2, "0");
const le16 = (n) => hex2(n) + hex2(n >> 8);

/** The GUID SDL builds for a USB device (bus 3, ids, no name hash; Windows carries no version, Linux the bcdDevice). 32 hex digits. */
export function sdlGuid(platform = "windows", { vid = VID, pid = PID, bcd = PAD_BCD } = {}) {
  const version = platform === "linux" ? bcd : 0;
  return `${le16(3)}${le16(0)}${le16(vid)}${le16(0)}${le16(pid)}${le16(0)}${le16(version)}${le16(0)}`;
}

// SDL axis numbers follow the usage order X Y Z Rx Ry Rz = a0..a5: the left trigger (Z) is a2 and the right stick a3 / a4
const SDL_AXES = [["leftx", 0], ["lefty", 1], ["rightx", 3], ["righty", 4], ["lefttrigger", 2], ["righttrigger", 5]];
const SDL_DPAD = [["dpup", 1], ["dpright", 2], ["dpdown", 4], ["dpleft", 8]];
const SDL_PLATFORM = { windows: "Windows", linux: "Linux", mac: "Mac OS X" };

/** One line for gamecontrollerdb.txt / SDL_GAMECONTROLLERCONFIG / Steam (same text as tools/magger60_pad_map.py sdl). */
export function sdlMapping({ platform = "windows", guid = null, name = PAD_NAME } = {}) {
  const g = (guid ?? sdlGuid(platform)).toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(g)) throw new RangeError("an SDL GUID is 32 hex digits");
  const parts = [g, name];
  for (const [role, n] of Object.entries(BUTTON_ROLES)) parts.push(`${role}:b${n - 1}`);
  for (const [role, a] of SDL_AXES) parts.push(`${role}:a${a}`);
  for (const [role, mask] of SDL_DPAD) parts.push(`${role}:h0.${mask}`);
  parts.push(`platform:${SDL_PLATFORM[platform]}`);
  return `${parts.join(",")},`;
}

export const isGuid = (s) => /^[0-9a-fA-F]{32}$/.test(String(s).trim());
export { PAD_CODE_AXIS, PAD_CODE_BTN, PAD_MIN_START_MM100 };
