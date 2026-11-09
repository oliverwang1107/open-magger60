// Profile model, defaults, validation and JSON import/export. The schema is our own; it is documented in
// ui/README.md and is independent of whatever byte format a device uses on the wire (see profile_codec.js for that).
//
// Version 2 (this file): four layers of bindings per key (was two) and a separate release point in the trigger.
// Version 1 files (two layers, no release point) still import: the missing parts are filled in, see migrateProfile().
import { MODES, LIMITS, defaultTrigger, normalizeTrigger } from "./trigger.js";
import { bindingFromUsage, isValidBinding, cleanBinding } from "./hid-usages.js";
import { isHexColor, SPEED_RANGE, EFFECTS } from "./lighting.js";
import { FW_LAYER_OPTS } from "./limits.js";

export const PROFILE_KIND = "fluxline-profile";
export const PROFILE_VERSION = 2;
export const PROFILE_COUNT = 4;
export const LAYER_NAME_MAX = 24;
export const LAYER_COUNT = 4; // base layer, the Fn layer (1) and two spare layers (2, 3)

// Layer behaviour options of a profile (they live on the keyboard, in the settings blob of format version 2):
//   tapTermMs   a tap-hold key held this long is a hold (ms)
//   oneShotMs   an armed one-shot modifier ends after this long (ms); 0 = it waits for the next key
//   tapPolicy   "timeout" (hold only by time), "permissive" (also when another key is pressed and released inside the hold key),
//               "other-key" (also as soon as another key is pressed)
export const TAP_POLICY_NAMES = Object.freeze(["timeout", "permissive", "other-key"]);
export const defaultLayerOpts = () => ({ tapTermMs: FW_LAYER_OPTS.tapTermDefault, oneShotMs: FW_LAYER_OPTS.oneShotDefault, tapPolicy: "timeout" });
/** null when the options object is acceptable, otherwise the name of the first field that is not. */
export function layerOptsProblem(o) {
  const L = FW_LAYER_OPTS;
  if (!o || typeof o !== "object") return "type";
  if (!Number.isInteger(o.tapTermMs) || o.tapTermMs < L.tapTermMin || o.tapTermMs > L.tapTermMax) return "tapTermMs";
  if (!Number.isInteger(o.oneShotMs) || (o.oneShotMs !== 0 && (o.oneShotMs < L.oneShotMin || o.oneShotMs > L.oneShotMax))) return "oneShotMs";
  if (!TAP_POLICY_NAMES.includes(o.tapPolicy)) return "tapPolicy";
  return null;
}

// Default layer 1 (Fn held), the key map's own table (firmware/core/LAYERS_NOTES.md). Keys not listed are transparent.
const media = (usage) => ({ type: "media", usage });
const key = (usage) => ({ type: "key", usage });
const prof = (index) => ({ type: "profile", index });
export const DEFAULT_FN_LAYER = Object.freeze({
  Digit1: key(58), Digit2: key(59), Digit3: key(60), Digit4: key(61), Digit5: key(62), Digit6: key(63), Digit7: key(64),
  Digit8: key(65), Digit9: key(66), Digit0: key(67), Minus: key(68), Equal: key(69),
  Backspace: key(76), Tab: key(53),
  KeyI: key(82), KeyJ: key(80), KeyK: key(81), KeyL: key(79), KeyU: key(74), KeyO: key(77), KeyP: key(75), Semicolon: key(78),
  BracketLeft: media(0xea), BracketRight: media(0xe9), KeyM: media(0xe2),
  Comma: media(0xb6), Period: media(0xcd), Slash: media(0xb5),
  KeyZ: prof(1), KeyX: prof(2), KeyC: prof(3), KeyV: prof(4),
});

export function defaultKey(layoutKey) {
  const fn = DEFAULT_FN_LAYER[layoutKey.id];
  return {
    trigger: defaultTrigger(),
    bindings: [bindingFromUsage(layoutKey.usage), fn ? { ...fn } : null, null, null],
    color: "#ffffff",
  };
}

export function defaultProfile(layout) {
  const keys = {};
  for (const k of layout.keys) keys[k.id] = defaultKey(k);
  return {
    kind: PROFILE_KIND,
    version: PROFILE_VERSION,
    name: "",
    layerNames: Array(LAYER_COUNT).fill(""),   // UI-only, like the profile name (the settings blob has no place for names)
    layerOpts: defaultLayerOpts(),
    lighting: { effect: "static", brightness: 80, speed: 2, color: "#00ffd5" },
    keys,
  };
}

export const defaultProfiles = (layout) => Array.from({ length: PROFILE_COUNT }, () => defaultProfile(layout));
export const cloneProfile = (p) => JSON.parse(JSON.stringify(p));

const isNum = (v) => typeof v === "number" && Number.isFinite(v);
const inRange = (v, lo, hi) => isNum(v) && v >= lo && v <= hi;

/**
 * Bring a version 1 profile (two layers, no release point, older limits) to version 2. Returns a copy; anything that is not
 * a profile is returned untouched so validateProfile can name what is wrong. Values below the new noise floor are raised to it.
 */
export function migrateProfile(data) {
  if (!data || typeof data !== "object" || Array.isArray(data) || data.version !== 1) return data;
  const out = cloneProfile(data);
  out.version = PROFILE_VERSION;
  for (const k of Object.values(out.keys && typeof out.keys === "object" ? out.keys : {})) {
    if (!k || typeof k !== "object") continue;
    if (Array.isArray(k.bindings) && k.bindings.length === 2) k.bindings.push(null, null);
    const t = k.trigger;
    if (t && typeof t === "object" && isNum(t.actuation)) {
      if (!isNum(t.release)) t.release = Math.max(LIMITS.release.min, Math.round((t.actuation - 0.4) * 100) / 100);
      normalizeTrigger(t, []);
    }
  }
  return out;
}

function validateTrigger(t, path, errors) {
  if (!t || typeof t !== "object") return errors.push({ path, code: "type" });
  if (!MODES.includes(t.mode)) errors.push({ path: `${path}.mode`, code: "range" });
  const fields = [["actuation", LIMITS.actuation], ["release", LIMITS.release], ["rapidDown", LIMITS.rt], ["rapidUp", LIMITS.rt], ["topDead", LIMITS.dead], ["bottomDead", LIMITS.dead]];
  for (const [f, lim] of fields) if (!inRange(t[f], lim.min, lim.max)) errors.push({ path: `${path}.${f}`, code: "range" });
}

/**
 * Check an unknown value (parsed JSON) against the schema. Missing keys are filled from defaults so a file
 * made for another layout still loads. Returns {ok, errors, profile}; `profile` is set only when ok.
 */
export function validateProfile(input, layout) {
  const errors = [];
  const data = migrateProfile(input);
  if (!data || typeof data !== "object" || Array.isArray(data)) return { ok: false, errors: [{ path: "$", code: "type" }] };
  if (data.kind !== PROFILE_KIND) errors.push({ path: "kind", code: "kind" });
  if (!Number.isInteger(data.version)) errors.push({ path: "version", code: "type" });
  else if (data.version > PROFILE_VERSION) errors.push({ path: "version", code: "newer" });
  if (typeof data.name !== "string" || data.name.length > 32) errors.push({ path: "name", code: "name" });
  if (data.layerNames !== undefined && (!Array.isArray(data.layerNames) || data.layerNames.length !== LAYER_COUNT
    || data.layerNames.some((n) => typeof n !== "string" || n.length > LAYER_NAME_MAX))) errors.push({ path: "layerNames", code: "name" });

  if (data.layerOpts !== undefined && layerOptsProblem(data.layerOpts)) errors.push({ path: `layerOpts.${layerOptsProblem(data.layerOpts)}`, code: "range" });

  const l = data.lighting;
  if (!l || typeof l !== "object") errors.push({ path: "lighting", code: "type" });
  else {
    if (!EFFECTS.some((e) => e.id === l.effect)) errors.push({ path: "lighting.effect", code: "range" });
    if (!inRange(l.brightness, 0, 100)) errors.push({ path: "lighting.brightness", code: "range" });
    if (!inRange(l.speed, SPEED_RANGE.min, SPEED_RANGE.max)) errors.push({ path: "lighting.speed", code: "range" });
    if (!isHexColor(l.color)) errors.push({ path: "lighting.color", code: "color" });
  }

  if (!data.keys || typeof data.keys !== "object" || Array.isArray(data.keys)) errors.push({ path: "keys", code: "type" });
  else {
    for (const [id, k] of Object.entries(data.keys)) {
      const p = `keys.${id}`;
      if (!layout.byId.has(id)) { errors.push({ path: p, code: "key" }); continue; }
      if (!k || typeof k !== "object") { errors.push({ path: p, code: "type" }); continue; }
      validateTrigger(k.trigger, `${p}.trigger`, errors);
      if (!Array.isArray(k.bindings) || k.bindings.length !== LAYER_COUNT) errors.push({ path: `${p}.bindings`, code: "type" });
      else k.bindings.forEach((b, i) => { if (!isValidBinding(b)) errors.push({ path: `${p}.bindings[${i}]`, code: "binding" }); });
      if (!isHexColor(k.color)) errors.push({ path: `${p}.color`, code: "color" });
    }
  }
  if (errors.length) return { ok: false, errors };

  const profile = cloneProfile(data);
  profile.layerNames ??= Array(LAYER_COUNT).fill("");
  profile.layerOpts = { ...defaultLayerOpts(), ...(profile.layerOpts ?? {}) };
  const defaults = defaultProfile(layout);
  for (const k of layout.keys) profile.keys[k.id] ??= defaults.keys[k.id];
  for (const k of Object.values(profile.keys)) k.bindings = k.bindings.map(cleanBinding); // drop stray fields
  return { ok: true, errors: [], profile };
}

export function exportProfileJson(profile) {
  return JSON.stringify(profile, null, 2) + "\n";
}

/** Parse + validate text from an imported file. */
export function importProfileJson(text, layout) {
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    return { ok: false, errors: [{ path: "$", code: "json" }] };
  }
  return validateProfile(data, layout);
}

/**
 * Layers 1..3 that hold entries (a key with something other than "transparent") but that no key can reach: nothing in the
 * profile switches to them. Layer 0 is always reachable; a hold-layer key counts when it sits on a layer that is reachable
 * itself. Informational only: the settings can always be rewritten, so there is no lock-out to prevent.
 */
export function unreachableLayers(profile, layout) {
  const reach = new Set([0]);
  for (let changed = true; changed;) {
    changed = false;
    for (const k of layout.keys) {
      for (const l of [...reach]) {
        const b = profile.keys[k.id]?.bindings[l];
        // a hold key, a toggle key and the hold half of a tap-hold key all switch to a layer
        const target = b?.type === "layer" || b?.type === "toggle" ? b.layer : b?.type === "taphold" && b.hold.type === "layer" ? b.hold.layer : null;
        if (target !== null && !reach.has(target)) { reach.add(target); changed = true; }
      }
    }
  }
  const out = [];
  for (let l = 1; l < LAYER_COUNT; l++) {
    if (!reach.has(l) && layout.keys.some((k) => profile.keys[k.id]?.bindings[l] != null)) out.push(l);
  }
  return out;
}

/**
 * Layers that a toggle key can switch ON but that no key can switch OFF again: while layer L is on, a key whose entry on L (or, when
 * that is transparent, on the base layer) is "toggle layer L" is needed. Informational: a profile switch and a replug always end a
 * toggled layer, so nothing is locked in for good. Returns the layer numbers, ascending.
 */
export function toggleTraps(profile, layout) {
  const entry = (k, l) => profile.keys[k.id]?.bindings[l] ?? (l > 0 ? profile.keys[k.id]?.bindings[0] : null);
  const out = [];
  for (let l = 1; l < LAYER_COUNT; l++) {
    const canOn = layout.keys.some((k) => profile.keys[k.id]?.bindings.some((b) => b?.type === "toggle" && b.layer === l));
    const canOff = layout.keys.some((k) => { const b = entry(k, l); return b?.type === "toggle" && b.layer === l; });
    if (canOn && !canOff) out.push(l);
  }
  return out;
}

/**
 * Apply `patch` to the trigger of every key id in `ids`, keeping every limit. Returns the notes of the adjustments made
 * (see normalizeTrigger) so the UI can explain them; [] when the patch went through unchanged.
 */
export function applyTriggerPatch(profile, ids, patch) {
  const notes = [];
  for (const id of ids) {
    const t = profile.keys[id]?.trigger;
    if (!t) continue;
    Object.assign(t, patch);
    notes.push(...normalizeTrigger(t, Object.keys(patch)));
  }
  return notes;
}
