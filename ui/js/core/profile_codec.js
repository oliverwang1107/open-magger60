// Mapping between the UI's profile objects (key ids, millimetres, bindings) and the keyboard's settings blob (slots, 0.01 mm,
// key codes). The byte format itself is settings_codec.js. A "workspace" is { active, profiles: [4 UI profiles] }.
//
// What the keyboard keeps: per key the trigger (mode, actuation, release, rapid-trigger steps, dead zones) and 4 layers of
// key codes; plus which profile is active. Everything else a UI profile holds (name, key colours) stays in the browser.
// The keyboard's lights are NOT in this blob: they are stored per key profile through the light ops (LED_GET / LED_SET with a profile
// argument, core/led_keep.js, LIGHT_PROFILE_NOTES.md); `profile.lighting` below is only the browser's own note.
//
// "Transparent": the key map has a code for it (0xFF) but a firmware whose settings rule stops earlier does not store it (limits.js,
// FW_USAGE_RULE). So when the rule says no, a transparent entry on layers 1..3 is saved as the layer-0 entry of the same slot (code and
// arguments), which does exactly what transparent does ("use the base layer's key"); the notes returned by workspaceToBlob say how many
// were converted.
//
// Settings format version 3 (M6) adds key kinds the UI has no editor for yet (toggle key, Mod-Morph, DKS, gamepad button / axis, light
// action) and per-profile records (DKS, Mod-Morph, SOCD, gamepad curves). Nothing of that is lost on the way through the UI model:
// an entry with such a code becomes the opaque binding { type: "fw", index: code << 16 | arg0 << 8 | arg1 } (bindingToCode writes it
// back unchanged), the records of a profile ride along in `profile.m6` ({ dks, morph, socd, pad }, only the parts that are in use) and
// the global device options plus any unknown optional sections in `workspace.m6` ({ device, unknown }). The behaviours pages (M6 P9a / P9b)
// give them real editors and labels.
//
// Settings format version 2 adds three key kinds with arguments (toggle layer, tap-hold, one-shot modifier) and per-profile layer
// options (tap term, one-shot timeout, tap policy); a UI profile keeps those options in `profile.layerOpts`.
import { MODE } from "./trigger.js";
import { mmToRaw, rawToMm } from "./units.js";
import { CODE, CONSUMER_CODES, defaultDevice, defaultPad, FW_LAYER_OPTS, FW_SETTINGS, FW_USAGE_RULE, MODE_FIXED, MODE_RAPID, TAP_POLICY, entryProblem } from "./limits.js";
import { cloneProfile, defaultProfile, PROFILE_COUNT, TAP_POLICY_NAMES } from "./profile.js";
import { bindingFromUsage } from "./hid-usages.js";
import { decodeSettings, deviceIsDefault, encodeSettings, padIsDefault, rawSettingsProblem, SettingsError, zeroArgs } from "./settings_codec.js";

const CODE_TO_CONSUMER = Object.fromEntries(Object.entries(CONSUMER_CODES).map(([u, c]) => [c, Number(u)]));
const HOLD_CODE = { 1: CODE.HOLD_L1, 2: CODE.HOLD_L2, 3: CODE.HOLD_L3 };

const NO_ARGS = Object.freeze([0, 0]);

/** The code byte a tap or hold action of a tap-hold key is written as; null when the keyboard cannot hold it. */
function actionCode(b) {
  if (!b) return null;
  if (b.type === "key") return !b.mods?.length && b.usage >= 0x04 && b.usage <= 0xe7 ? b.usage : null;
  if (b.type === "media") return b.usage in CONSUMER_CODES ? CONSUMER_CODES[b.usage] : null;
  if (b.type === "layer") return HOLD_CODE[b.layer] ?? null;
  return null;
}

/** Key code (and its two argument bytes) of a binding on layer `layer`, or {problem} when the keyboard cannot hold it. null = transparent. */
export function bindingToCode(b, layer) {
  if (b === null) return { code: layer === 0 ? CODE.NONE : CODE.TRANSPARENT, args: NO_ARGS, transparent: layer !== 0 };
  switch (b.type) {
    case "none": return { code: CODE.NONE, args: NO_ARGS };
    case "key":
      if (b.mods?.length) return { problem: "mods" };
      return b.usage >= 0x04 && b.usage <= 0xe7 ? { code: b.usage, args: NO_ARGS } : { problem: "usage" };
    case "media": return b.usage in CONSUMER_CODES ? { code: CONSUMER_CODES[b.usage], args: NO_ARGS } : { problem: "media" };
    case "layer": return HOLD_CODE[b.layer] ? { code: HOLD_CODE[b.layer], args: NO_ARGS } : { problem: "layer" };
    case "profile": return b.index >= 1 && b.index <= 4 ? { code: CODE.PROFILE_1 + b.index - 1, args: NO_ARGS } : { problem: "profile" };
    case "toggle": return b.layer >= 1 && b.layer <= 3 ? { code: CODE.TOGGLE, args: [b.layer, 0], extended: true } : { problem: "layer" };
    case "oneshot": return b.usage >= 0xe0 && b.usage <= 0xe7 ? { code: CODE.ONESHOT, args: [b.usage, 0], extended: true } : { problem: "usage" };
    case "taphold": {
      const tap = actionCode(b.tap);
      const hold = actionCode(b.hold);
      if (tap === null || b.tap.type === "layer" || hold === null || (b.hold.type !== "layer" && b.hold.type !== "key")) return { problem: "taphold" };
      return entryProblem(CODE.TAP_HOLD, tap, hold) ? { problem: "taphold" } : { code: CODE.TAP_HOLD, args: [tap, hold], extended: true };
    }
    case "fw": {   // an M6 entry the UI has no editor for: kept exactly as the keyboard has it
      const i = b.index;
      if (!Number.isInteger(i) || i < 0 || i > 0xffffff) return { problem: "type" };
      return { code: i >> 16, args: [(i >> 8) & 0xff, i & 0xff], extended: true, v3: true };
    }
    default: return { problem: "type" }; // mouse, wheel, system: the keyboard has no such output
  }
}

/** Can the keyboard hold this binding on this layer (now, with this firmware's rule)? Used to offer only what works. */
export function bindingStorable(b, layer, rule = FW_USAGE_RULE) {
  const r = bindingToCode(b, layer);
  if (r.problem) return false;
  return r.code === CODE.TRANSPARENT || entryProblem(r.code, r.args[0], r.args[1], rule) === null;
}

const KEY_OF = (usage) => ({ type: "key", usage });
const ACTION_OF = (code) => {
  if (code >= 0x04 && code <= 0xe7) return KEY_OF(code);
  if (code in CODE_TO_CONSUMER) return { type: "media", usage: CODE_TO_CONSUMER[code] };
  if (code === CODE.HOLD_L1) return { type: "layer", layer: 1 };
  if (code === CODE.HOLD_L2) return { type: "layer", layer: 2 };
  if (code === CODE.HOLD_L3) return { type: "layer", layer: 3 };
  return null;
};

/** Binding for a key code (and argument bytes) read from the keyboard. */
export function codeToBinding(code, layer = 0, args = NO_ARGS) {
  if (code >= CODE.TOGGLE_KEY && code <= CODE.PAD_AXIS) return { type: "fw", index: (code << 16) | (args[0] << 8) | args[1] };
  if (code === CODE.LIGHT && args[0] !== 0) return { type: "fw", index: (code << 16) | (args[0] << 8) | args[1] };
  if (code === CODE.NONE || code === CODE.RESERVED_1 || code === CODE.RESERVED_2) return { type: "none" };
  if (code === CODE.TRANSPARENT) return layer === 0 ? { type: "none" } : null;
  if (code >= 0x04 && code <= 0xe7) return { type: "key", usage: code };
  if (code === CODE.HOLD_L1) return { type: "layer", layer: 1 };
  if (code === CODE.HOLD_L2) return { type: "layer", layer: 2 };
  if (code === CODE.HOLD_L3) return { type: "layer", layer: 3 };
  if (code >= CODE.PROFILE_1 && code <= CODE.PROFILE_4) return { type: "profile", index: code - CODE.PROFILE_1 + 1 };
  if (code in CODE_TO_CONSUMER) return { type: "media", usage: CODE_TO_CONSUMER[code] };
  if (code === CODE.TOGGLE) return { type: "toggle", layer: args[0] };
  if (code === CODE.ONESHOT) return { type: "oneshot", usage: args[0] };
  if (code === CODE.TAP_HOLD) {
    const tap = ACTION_OF(args[0]);
    const hold = ACTION_OF(args[1]);
    return tap && hold ? { type: "taphold", tap, hold } : { type: "none" };
  }
  return { type: "none" };
}

const MODE_TO_RAW = { [MODE.NORMAL]: [MODE_FIXED, 0], [MODE.RT]: [MODE_RAPID, 0], [MODE.RT_CONTINUOUS]: [MODE_RAPID, 1] };

/** The layer options of a UI profile ({tapTermMs, oneShotMs, tapPolicy}) <-> the raw {tapTerm, oneshot, policy}. */
export function optsToRaw(o) {
  const d = { tapTermMs: FW_LAYER_OPTS.tapTermDefault, oneShotMs: FW_LAYER_OPTS.oneShotDefault, tapPolicy: "timeout", ...(o ?? {}) };
  const policy = TAP_POLICY_NAMES.indexOf(d.tapPolicy);
  return { tapTerm: d.tapTermMs, oneshot: d.oneShotMs, policy: policy < 0 ? TAP_POLICY.TIMEOUT : policy };
}

export function optsFromRaw(o) {
  return { tapTermMs: o.tapTerm, oneShotMs: o.oneshot, tapPolicy: TAP_POLICY_NAMES[o.policy] ?? "timeout" };
}

export function triggerToRaw(t) {
  const [mode, continuous] = MODE_TO_RAW[t.mode] ?? MODE_TO_RAW[MODE.NORMAL];
  return {
    press: mmToRaw(t.actuation), release: mmToRaw(t.release ?? t.actuation - 0.4), mode, continuous,
    rtDown: mmToRaw(t.rapidDown), rtUp: mmToRaw(t.rapidUp), topDead: mmToRaw(t.topDead), bottomDead: mmToRaw(t.bottomDead),
  };
}

export function triggerFromRaw(c) {
  return {
    mode: c.mode === MODE_RAPID ? (c.continuous ? MODE.RT_CONTINUOUS : MODE.RT) : MODE.NORMAL,
    actuation: rawToMm(c.press), release: rawToMm(c.release), rapidDown: rawToMm(c.rtDown), rapidUp: rawToMm(c.rtUp),
    topDead: rawToMm(c.topDead), bottomDead: rawToMm(c.bottomDead),
  };
}

/**
 * One UI profile -> the raw profile of the blob. Slots without a key get the firmware default.
 * Returns { raw, problems: [{keyId, layer, reason}], converted } - problems must be empty before anything is written.
 */
export function profileToRaw(profile, layout, rule = FW_USAGE_RULE) {
  const defaults = defaultProfile(layout);
  const raw = {
    keys: Array.from({ length: FW_SETTINGS.slots }, () => triggerToRaw(defaults.keys[layout.keys[0].id].trigger)),
    layers: Array.from({ length: FW_SETTINGS.layers }, () => new Array(FW_SETTINGS.slots).fill(0)),
    args: zeroArgs(),
    opts: optsToRaw(profile.layerOpts),
    dks: structuredClone(profile.m6?.dks ?? []),
    morph: structuredClone(profile.m6?.morph ?? []),
    socd: structuredClone(profile.m6?.socd ?? []),
    pad: structuredClone(profile.m6?.pad ?? defaultPad()),
  };
  const problems = [];
  let converted = 0;
  for (const k of layout.keys) {
    const data = profile.keys[k.id];
    raw.keys[k.slot] = triggerToRaw(data.trigger);
    const entries = [];
    for (let l = 0; l < FW_SETTINGS.layers; l++) {
      const r = bindingToCode(data.bindings[l] ?? null, l);
      if (r.problem) { problems.push({ keyId: k.id, layer: l, reason: r.problem }); entries.push({ code: CODE.NONE, args: NO_ARGS }); } else entries.push(r);
    }
    for (let l = 0; l < FW_SETTINGS.layers; l++) {
      let e = entries[l];
      if (e.code === CODE.TRANSPARENT && entryProblem(e.code, 0, 0, rule) !== null) {
        e = entries[0].code === CODE.TRANSPARENT ? { code: CODE.NONE, args: NO_ARGS } : entries[0]; // what "transparent" does, written out
        converted++;
      }
      const why = entryProblem(e.code, e.args[0], e.args[1], rule);
      if (why) { problems.push({ keyId: k.id, layer: l, reason: "firmware_rule", code: e.code }); e = { code: CODE.NONE, args: NO_ARGS }; }
      raw.layers[l][k.slot] = e.code;
      raw.args[l][k.slot] = [e.args[0], e.args[1]];
    }
  }
  return { raw, problems, converted };
}

/** The raw profile of the blob -> a UI profile; name, lighting and key colours come from `base` (a UI profile) when given. */
export function profileFromRaw(raw, layout, base = null) {
  const out = base ? cloneProfile(base) : defaultProfile(layout);
  out.layerOpts = optsFromRaw(raw.opts);
  const m6 = {};   // only what is in use, so a profile without M6 data looks exactly as before
  if (raw.dks?.length) m6.dks = structuredClone(raw.dks);
  if (raw.morph?.length) m6.morph = structuredClone(raw.morph);
  if (raw.socd?.length) m6.socd = structuredClone(raw.socd);
  if (raw.pad && !padIsDefault(raw.pad)) m6.pad = structuredClone(raw.pad);
  if (Object.keys(m6).length) out.m6 = m6; else delete out.m6;
  for (const k of layout.keys) {
    const data = out.keys[k.id];
    data.trigger = triggerFromRaw(raw.keys[k.slot]);
    // an overlay entry equal to the base layer's (code and arguments) is shown as "transparent": that is what it does, and it is what saving writes out again
    const same = (l) => raw.layers[l][k.slot] === raw.layers[0][k.slot] && raw.args[l][k.slot][0] === raw.args[0][k.slot][0] && raw.args[l][k.slot][1] === raw.args[0][k.slot][1];
    data.bindings = raw.layers.map((layer, l) => (l > 0 && same(l) ? null : codeToBinding(layer[k.slot], l, raw.args[l][k.slot])));
  }
  return out;
}

/** The workspace (4 UI profiles + active index) -> blob bytes. Returns { bytes, problems, converted } (bytes null when problems). */
export function workspaceToBlob(ws, layout, rule = FW_USAGE_RULE) {
  const profiles = [];
  const problems = [];
  let converted = 0;
  ws.profiles.forEach((p, i) => {
    const r = profileToRaw(p, layout, rule);
    profiles.push(r.raw);
    converted += r.converted;
    problems.push(...r.problems.map((x) => ({ ...x, profile: i })));
  });
  const raw = { version: FW_SETTINGS.version, active: ws.active, profiles, device: structuredClone(ws.m6?.device ?? defaultDevice()), unknown: structuredClone(ws.m6?.unknown ?? []) };
  if (problems.length) return { bytes: null, problems, converted, raw };
  const bad = rawSettingsProblem(raw, rule);
  if (bad) return { bytes: null, problems: [{ reason: "settings", error: bad }], converted, raw };
  return { bytes: encodeSettings(raw, rule), problems: [], converted, raw };
}

/** Blob bytes -> workspace. `base` (a workspace) supplies the browser-only parts of each profile. Throws SettingsError. */
export function blobToWorkspace(bytes, layout, base = null, rule = FW_USAGE_RULE) {
  const raw = decodeSettings(bytes, rule);
  const ws = {
    active: raw.active,
    profiles: raw.profiles.map((p, i) => profileFromRaw(p, layout, base?.profiles?.[i] ?? null)),
  };
  if (!deviceIsDefault(raw.device) || raw.unknown.length) ws.m6 = { device: raw.device, unknown: raw.unknown };
  return ws;
}

/** The blob a workspace would be saved as, after passing through the UI model: what "the same settings" means for dirty checks. */
export function normalizeBlob(bytes, layout, rule = FW_USAGE_RULE) {
  return workspaceToBlob(blobToWorkspace(bytes, layout, null, rule), layout, rule).bytes;
}

export const sameBytes = (a, b) => !!a && !!b && a.length === b.length && a.every((v, i) => v === b[i]);

/** Start workspace when no keyboard data is known: the UI default profile, four times. */
export function defaultWorkspace(layout) {
  return { active: 0, profiles: Array.from({ length: PROFILE_COUNT }, () => defaultProfile(layout)) };
}

export { SettingsError, bindingFromUsage };
