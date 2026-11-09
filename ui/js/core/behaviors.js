// Behaviours (M6 package P9a): the data side of the Behaviours page. No DOM, no i18n; every function works on the UI profile model
// of profile_codec.js and keeps it in a state the settings codec accepts.
//
//   DKS        profile.m6.dks   [{tAct, tBot, flags, acts: [[usage, events] x 4]}]   0.01 mm; events = 4 x 2 bits (0 none, 1 press, 2 release, 3 tap)
//   Mod-Morph  profile.m6.morph [{mask, usage, flags}]                                mask bit n = modifier usage 0xE0 + n
//   SOCD       profile.m6.socd  [{a, b, mode, flags}]                                 a / b = sensor SLOTS (the settings format's own numbering)
//   key kinds  a table entry is the opaque binding {type: "fw", index: code << 16 | arg0 << 8 | arg1} (profile_codec.js):
//                DKS   0xED  arg0 = record index          Mod-Morph 0xEC  arg0 = normal usage, arg1 = record index
//                Toggle key 0xEB  arg0 = the usage that stays on
//
// The meaning of each field is the firmware's (firmware/core/DKS_NOTES.md, SOCD_NOTES.md on package P1/P3, SETTINGS_NOTES.md of P0);
// the rules are the P0 codec's (limits.js dksProblem / morphProblem / socdProblem, rawSettingsProblem). Nothing here writes bytes:
// the page edits the workspace and the codec checks it again when it is saved (codecProblem below runs that same check on demand).
import { CODE, FW_M6, FW_SETTINGS, TRAVEL_MM100, dksProblem, entryProblem, morphProblem, slotHasKey, socdProblem } from "./limits.js";
import { bindingFromUsage, keyName, setFwCapLabel } from "./hid-usages.js";
import { SettingsError } from "./settings_codec.js";
import { workspaceToBlob } from "./profile_codec.js";

// ---------------------------------------------------------------------------------------------------------------- opaque bindings

export const fwBinding = (code, a0 = 0, a1 = 0) => ({ type: "fw", index: (code << 16) | (a0 << 8) | a1 });
export const dksBinding = (index) => fwBinding(CODE.DKS, index, 0);
export const morphBinding = (usage, index) => fwBinding(CODE.MOD_MORPH, usage, index);
export const toggleKeyBinding = (usage) => fwBinding(CODE.TOGGLE_KEY, usage, 0);

/** {code, a0, a1} of an opaque binding, else null. */
export function fwParts(b) {
  if (!b || b.type !== "fw" || !Number.isInteger(b.index) || b.index < 0 || b.index > 0xffffff) return null;
  return { code: b.index >> 16, a0: (b.index >> 8) & 0xff, a1: b.index & 0xff };
}

/** What a table entry does among the four behaviours of this page: {kind: "dks"|"morph"|"toggle", ...} or null (any other binding). */
export function behaviorOf(b) {
  const p = fwParts(b);
  if (!p) return null;
  if (p.code === CODE.DKS) return { kind: "dks", index: p.a0 };
  if (p.code === CODE.MOD_MORPH) return { kind: "morph", usage: p.a0, index: p.a1 };
  if (p.code === CODE.TOGGLE_KEY) return { kind: "toggle", usage: p.a0 };
  return null;
}

/** Short text for a key cap (≤ 6 characters): "DKS1", "MM2", "TG A". Other opaque entries (gamepad, light) show a dot. */
export function fwCapLabel(b) {
  const x = behaviorOf(b);
  if (!x) return fwParts(b) ? "•" : "";
  if (x.kind === "dks") return `DKS${x.index + 1}`;
  if (x.kind === "morph") return `MM${x.index + 1}`;
  return `TG ${keyName(x.usage)}`;
}

setFwCapLabel(fwCapLabel); // key caps of the Layers page show "DKS1" / "MM2" / "TG A" instead of "fw:12"

// ---------------------------------------------------------------------------------------------------------------- the profile's records

export const M6_LIMITS = Object.freeze({ dks: FW_M6.dksMax, morph: FW_M6.morphMax, socd: FW_M6.socdMax });

const LIST = { dks: "dks", morph: "morph", socd: "socd" };
export const recordsOf = (profile, kind) => profile.m6?.[LIST[kind]] ?? [];

/** Replace one record list; an empty list is not kept (a profile without M6 data looks exactly like before, which is what profileFromRaw gives). */
function putRecords(profile, kind, list) {
  if (list.length) (profile.m6 ??= {})[LIST[kind]] = list;
  else if (profile.m6) {
    delete profile.m6[LIST[kind]];
    if (!Object.keys(profile.m6).length) delete profile.m6;
  }
}

const ok = (extra = {}) => ({ ok: true, ...extra });
const no = (reason, extra = {}) => ({ ok: false, reason, ...extra });

/** Every table entry of the profile: fn(keyId, layer, binding). */
function eachEntry(profile, fn) {
  for (const [keyId, data] of Object.entries(profile.keys)) data.bindings.forEach((b, layer) => fn(keyId, layer, b));
}

/** Keys (and the layers) whose entry points at record `index` of a record kind ("dks" | "morph"): [{keyId, layer}]. */
export function usersOf(profile, kind, index) {
  const out = [];
  eachEntry(profile, (keyId, layer, b) => {
    const x = behaviorOf(b);
    if (x?.kind === kind && x.index === index) out.push({ keyId, layer });
  });
  return out;
}

/** What a key falls back to when its special behaviour goes away: its printed key on the base layer, "transparent" above. */
export function defaultEntry(layout, keyId, layer) {
  const k = layout.byId.get(keyId);
  return layer === 0 ? (k ? bindingFromUsage(k.usage) : { type: "none" }) : null;
}

/** Put the key (all selected layers) back to its default entry. */
export function resetEntry(profile, layout, keyId, layer) {
  if (profile.keys[keyId]) profile.keys[keyId].bindings[layer] = defaultEntry(layout, keyId, layer);
}

// ---- DKS ----

export const DKS_EV = Object.freeze({ NONE: 0, PRESS: 1, RELEASE: 2, TAP: 3 });
export const DKS_EVENT_COUNT = 4;
/** the four moments, in the order of the bits (km_dks.h): crossing the start point downward, the bottom point downward, the bottom point upward, the start point upward */
export const DKS_EVENT_NAMES = Object.freeze(["act_down", "bot_down", "bot_up", "act_up"]);
export const DKS_ACTIONS = 4;
export const DKS_STEP_MM100 = 5;           // the editor moves the points in 0.05 mm steps
export const DKS_HYSTERESIS_MM100 = 20;    // the firmware lets go of the bottom point this far above it (noise is about +-15)

export const dksEventsUnpack = (byte) => [byte & 3, (byte >> 2) & 3, (byte >> 4) & 3, (byte >> 6) & 3];
export const dksEventsPack = (list) => list.reduce((byte, v, i) => byte | ((v & 3) << (2 * i)), 0);

/** The record a new "DKS" starts as: A on the way down and up at the start point, S while at the bottom (the plan's own example). */
export const newDksRecord = () => ({
  tAct: FW_M6.dksActDefault, tBot: FW_M6.dksBotDefault, flags: 0,
  acts: [
    [0x04, dksEventsPack([DKS_EV.PRESS, 0, 0, DKS_EV.RELEASE])],
    [0x16, dksEventsPack([0, DKS_EV.PRESS, DKS_EV.RELEASE, 0])],
    [0, 0], [0, 0],
  ],
});

export const dksBottom = (rec) => (rec.tBot === 0 ? FW_M6.dksBotDefault : rec.tBot);

/**
 * Move one of the two depth points ("tAct" or "tBot", 0.01 mm) and keep the record valid: the start point is at least 0.40 mm and at
 * least 0.40 mm above the bottom point, the bottom point is at most 3.40 mm. Returns {rec, clamped, from?} (a new record).
 */
export function dksSetDepth(rec, field, value) {
  const bot = dksBottom(rec);
  const v = Math.round(value);
  let next;
  if (field === "tAct") next = Math.min(Math.max(v, FW_M6.dksActMin), bot - FW_M6.dksGapMin);
  else if (field === "tBot") next = Math.min(Math.max(v, rec.tAct + FW_M6.dksGapMin), TRAVEL_MM100);
  else throw new RangeError(`unknown depth field ${field}`);
  return { rec: { ...structuredClone(rec), [field]: next, ...(field === "tAct" ? { tBot: bot } : {}) }, clamped: next !== v };
}

/** Change what action `i` is (a keyboard usage 0x04..0xE7, or 0 = unused; an unused action has no events). */
export function dksSetUsage(rec, i, usage) {
  const next = structuredClone(rec);
  next.acts[i] = [usage, usage === 0 ? 0 : next.acts[i][1]];
  return next;
}

/** Set one event (0..3) of action `i` to a DKS_EV value; an unused action cannot have events (returns null then). */
export function dksSetEvent(rec, i, ev, value) {
  if (rec.acts[i][0] === 0 && value !== DKS_EV.NONE) return null;
  const next = structuredClone(rec);
  const list = dksEventsUnpack(next.acts[i][1]);
  list[ev] = value;
  next.acts[i][1] = dksEventsPack(list);
  return next;
}

/**
 * The record as a list of the four moments: [{event: 0..3, name, depth (0.01 mm; null for the "released" moment, the key is up), steps: [{usage, op}]}].
 * The last moment (the key leaves the start point upward) releases everything still held, whatever the table says: `closing` lists those.
 */
export function describeDks(rec) {
  const bot = dksBottom(rec);
  const depth = [rec.tAct, bot, bot - DKS_HYSTERESIS_MM100, rec.tAct - 10];
  const out = DKS_EVENT_NAMES.map((name, event) => ({ event, name, depth: depth[event], steps: [] }));
  const held = new Set();
  rec.acts.forEach(([usage, bits]) => {
    if (usage === 0) return;
    dksEventsUnpack(bits).forEach((op, event) => {
      if (op === DKS_EV.NONE) return;
      out[event].steps.push({ usage, op });
      if (op === DKS_EV.PRESS) held.add(usage);
      if (op === DKS_EV.RELEASE) held.delete(usage);
    });
  });
  out[DKS_EVENT_COUNT - 1].closing = [...held];
  return out;
}

/** Soft remarks about a valid record (nothing the firmware refuses): [{note: "no_actions"} | {note: "idle", action}]. */
export function dksNotes(rec) {
  const notes = [];
  const used = rec.acts.map((a, i) => ({ a, i })).filter(({ a }) => a[0] !== 0);
  if (!used.length) notes.push({ note: "no_actions" });
  for (const { a, i } of used) if (a[1] === 0) notes.push({ note: "idle", action: i });
  return notes;
}

// ---- Mod-Morph ----

export const MORPH_MODIFIERS = Object.freeze([0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7]);
export const MORPH_BOTH = Object.freeze({ ctrl: 0x11, shift: 0x22, alt: 0x44, win: 0x88 });   // left and right of one modifier
export const newMorphRecord = () => ({ mask: MORPH_BOTH.shift, usage: 76, flags: 0 });         // Shift -> Delete
export const morphHasMod = (rec, usage) => (rec.mask & (1 << (usage - 0xe0))) !== 0;
export function morphToggleMod(rec, usage) {
  return { ...rec, mask: rec.mask ^ (1 << (usage - 0xe0)) };
}

/** Ready-made examples the page offers: the key it belongs on, plus the record. */
export const MORPH_PRESETS = Object.freeze([
  { id: "delete", keyId: "Backspace", rec: { mask: MORPH_BOTH.shift, usage: 76, flags: 0 } },       // Shift + Backspace = Delete
  { id: "grave", keyId: "Escape", rec: { mask: MORPH_BOTH.win, usage: 53, flags: 0 } },             // Win + Esc = `
]);

/** The usage a Mod-Morph key sends when no modifier is held: what the key does now (a plain key or a morph already), else its printed key. */
export function morphNormalUsage(profile, layout, keyId, layer) {
  const b = profile.keys[keyId]?.bindings[layer];
  const x = behaviorOf(b);
  if (x?.kind === "morph") return x.usage;
  if (b?.type === "key" && !b.mods?.length && b.usage >= 0x04 && b.usage <= 0xe7) return b.usage;
  const printed = layout.byId.get(keyId)?.usage;
  return Number.isInteger(printed) && printed >= 0x04 && printed <= 0xe7 ? printed : null;
}

// ---- generic record lists (DKS and Mod-Morph) ----

const KIND = {
  dks: { code: CODE.DKS, arg: 0, max: FW_M6.dksMax, problem: dksProblem },
  morph: { code: CODE.MOD_MORPH, arg: 1, max: FW_M6.morphMax, problem: morphProblem },
};

/** Add a record. {ok, index} or {ok: false, reason: "full" | <the codec's rule that the record breaks>}. */
export function addRecord(profile, kind, rec) {
  const k = KIND[kind];
  const list = recordsOf(profile, kind);
  if (list.length >= k.max) return no("full");
  const why = k.problem(rec);
  if (why) return no(why);
  putRecords(profile, kind, [...list, structuredClone(rec)]);
  return ok({ index: list.length });
}

/** Replace record `index`. Nothing changes when the record is refused. */
export function setRecord(profile, kind, index, rec) {
  const list = recordsOf(profile, kind);
  if (!(index >= 0 && index < list.length)) return no("index");
  const why = KIND[kind].problem(rec);
  if (why) return no(why);
  const next = list.slice();
  next[index] = structuredClone(rec);
  putRecords(profile, kind, next);
  return ok();
}

/**
 * Delete record `index`. The settings format wants every key to point at a record that exists, so keys using it go back to their default
 * entry (`layout` needed) and keys using a later record move down one number. Returns {ok, reset: [{keyId, layer}]}.
 */
export function removeRecord(profile, layout, kind, index) {
  const k = KIND[kind];
  const list = recordsOf(profile, kind);
  if (!(index >= 0 && index < list.length)) return no("index");
  const reset = usersOf(profile, kind, index);
  eachEntry(profile, (keyId, layer, b) => {
    const x = behaviorOf(b);
    if (x?.kind !== kind) return;
    const p = fwParts(b);
    if (x.index === index) resetEntry(profile, layout, keyId, layer);
    else if (x.index > index) {
      const a = [p.a0, p.a1];
      a[k.arg] -= 1;
      profile.keys[keyId].bindings[layer] = fwBinding(p.code, a[0], a[1]);
    }
  });
  putRecords(profile, kind, list.filter((_, i) => i !== index));
  return ok({ reset });
}

/** Number of table entries that use each record: [n0, n1, ...]. */
export const recordUsage = (profile, kind) => recordsOf(profile, kind).map((_, i) => usersOf(profile, kind, i).length);

/**
 * Point keys at a record. `keys` = [{keyId, layer}] (the page passes the selection on the layer being edited). Mod-Morph needs the usage
 * the key sends normally; a key that has none is skipped. Returns {done: [...], skipped: [{keyId, layer, why}]}.
 */
export function assignRecord(profile, layout, kind, index, keys) {
  const done = [];
  const skipped = [];
  const list = recordsOf(profile, kind);
  for (const { keyId, layer } of keys) {
    const data = profile.keys[keyId];
    if (!data) { skipped.push({ keyId, layer, why: "key" }); continue; }
    if (index < 0 || index >= list.length) { skipped.push({ keyId, layer, why: "record" }); continue; }
    let b;
    if (kind === "dks") b = dksBinding(index);
    else {
      const usage = morphNormalUsage(profile, layout, keyId, layer);
      if (usage === null) { skipped.push({ keyId, layer, why: "normal" }); continue; }
      b = morphBinding(usage, index);
    }
    data.bindings[layer] = b;
    done.push({ keyId, layer });
  }
  return { done, skipped };
}

/** Make keys toggle keys. usage = a keyboard usage 0x04..0xE7, or "self" = each key's own key. Same result shape as assignRecord. */
export function assignToggle(profile, layout, usage, keys) {
  const done = [];
  const skipped = [];
  for (const { keyId, layer } of keys) {
    const data = profile.keys[keyId];
    if (!data) { skipped.push({ keyId, layer, why: "key" }); continue; }
    const u = usage === "self" ? morphNormalUsage(profile, layout, keyId, layer) : usage;
    if (!Number.isInteger(u) || entryProblem(CODE.TOGGLE_KEY, u, 0) !== null) { skipped.push({ keyId, layer, why: "normal" }); continue; }
    data.bindings[layer] = toggleKeyBinding(u);
    done.push({ keyId, layer });
  }
  return { done, skipped };
}

/** Give keys their default entry again (a DKS, Mod-Morph or toggle key only; other entries are left alone). */
export function clearBehavior(profile, layout, keys) {
  const done = [];
  for (const { keyId, layer } of keys) {
    if (behaviorOf(profile.keys[keyId]?.bindings[layer])) { resetEntry(profile, layout, keyId, layer); done.push({ keyId, layer }); }
  }
  return done;
}

/** Keys that are a toggle key on `layer`: [{keyId, usage}]. */
export function toggleKeysOn(profile, layer) {
  const out = [];
  eachEntry(profile, (keyId, l, b) => {
    const x = behaviorOf(b);
    if (l === layer && x?.kind === "toggle") out.push({ keyId, usage: x.usage });
  });
  return out;
}

// ---- SOCD ----

/** modes in the order of the numbers the keyboard uses (km_socd.h) */
export const SOCD_MODES = Object.freeze(["off", "last", "neutral", "first", "deeper"]);
export const SOCD_FLAG_LAYER0 = 1;
export const SOCD_DEEPER_HYST_MM100 = 30;

export const SOCD_PRESETS = Object.freeze([
  { id: "wasd", pairs: [["KeyA", "KeyD"], ["KeyW", "KeyS"]] },
  { id: "arrows", pairs: [["ArrowLeft", "ArrowRight"], ["ArrowUp", "ArrowDown"]] },
]);

/** Slots that already belong to a pair (the codec allows a key in one pair only, switched off or not). */
export function socdTaken(profile, exceptIndex = -1) {
  const taken = new Set();
  recordsOf(profile, "socd").forEach((p, i) => { if (i !== exceptIndex) { taken.add(p.a); taken.add(p.b); } });
  return taken;
}

/** Why the pair {a, b, mode, flags} (slots) cannot be stored at position `index` (-1 = a new one); null when it can. */
export function socdPairProblem(profile, pair, index = -1) {
  const why = socdProblem(pair);
  if (why) return why === "slots" && pair.a === pair.b && slotHasKey(pair.a) ? "same" : why;
  const taken = socdTaken(profile, index);
  return taken.has(pair.a) || taken.has(pair.b) ? "overlap" : null;
}

/** Add a pair. New pairs start OFF unless `mode` says otherwise: nothing here switches SOCD on by itself. {ok, index} or {ok: false, reason}. */
export function addSocdPair(profile, { a, b, mode = 0, flags = 0 }) {
  const list = recordsOf(profile, "socd");
  if (list.length >= FW_M6.socdMax) return no("full");
  const pair = { a, b, mode, flags };
  const why = socdPairProblem(profile, pair);
  if (why) return no(why);
  putRecords(profile, "socd", [...list, pair]);
  return ok({ index: list.length });
}

/** Change fields of pair `index` ({a?, b?, mode?, flags?}). Nothing changes when the result is refused. */
export function setSocdPair(profile, index, patch) {
  const list = recordsOf(profile, "socd");
  if (!(index >= 0 && index < list.length)) return no("index");
  const pair = { ...list[index], ...patch };
  const why = socdPairProblem(profile, pair, index);
  if (why) return no(why);
  const next = list.slice();
  next[index] = pair;
  putRecords(profile, "socd", next);
  return ok();
}

export function removeSocdPair(profile, index) {
  const list = recordsOf(profile, "socd");
  if (!(index >= 0 && index < list.length)) return no("index");
  putRecords(profile, "socd", list.filter((_, i) => i !== index));
  return ok();
}

/** Number of pairs that are switched on. */
export const socdActive = (profile) => recordsOf(profile, "socd").filter((p) => p.mode !== 0).length;

// ---------------------------------------------------------------------------------------------------------------- checking

/**
 * Everything wrong with the behaviour data of one profile, found with the codec's own record rules:
 * [{area: "dks" | "morph" | "socd" | "entry", index, why, keyId?, layer?}]. Empty = the codec will take it.
 */
export function behaviorIssues(profile) {
  const out = [];
  for (const kind of ["dks", "morph"]) {
    const list = recordsOf(profile, kind);
    if (list.length > KIND[kind].max) out.push({ area: kind, index: -1, why: "count" });
    list.forEach((rec, index) => { const why = KIND[kind].problem(rec); if (why) out.push({ area: kind, index, why }); });
  }
  const socd = recordsOf(profile, "socd");
  if (socd.length > FW_M6.socdMax) out.push({ area: "socd", index: -1, why: "count" });
  const seen = new Set();
  socd.forEach((pair, index) => {
    const why = socdProblem(pair);
    if (why) out.push({ area: "socd", index, why });
    else if (seen.has(pair.a) || seen.has(pair.b)) out.push({ area: "socd", index, why: "overlap" });
    seen.add(pair.a); seen.add(pair.b);
  });
  const dks = recordsOf(profile, "dks").length;
  const morph = recordsOf(profile, "morph").length;
  eachEntry(profile, (keyId, layer, b) => {
    const x = behaviorOf(b);
    if (!x) return;
    if ((x.kind === "dks" && x.index >= dks) || (x.kind === "morph" && x.index >= morph)) out.push({ area: "entry", index: x.index, why: "reference", keyId, layer, kind: x.kind });
    else if (entryProblem(fwParts(b).code, fwParts(b).a0, fwParts(b).a1) !== null) out.push({ area: "entry", index: -1, why: "args", keyId, layer, kind: x.kind });
  });
  return out;
}

/** The settings codec's verdict on the profile saved as it is: null (it would be written), or the SettingsError it refuses it with. */
export function codecProblem(profile, layout) {
  const copy = JSON.parse(JSON.stringify(profile));
  let r;
  try {
    r = workspaceToBlob({ active: 0, profiles: [copy, copy, copy, copy] }, layout);
  } catch (err) {
    return err instanceof SettingsError ? err : new SettingsError("shape", String(err?.message ?? err));
  }
  if (r.bytes) return null;
  const first = r.problems[0];
  if (first?.error instanceof SettingsError) return first.error;
  return new SettingsError("usage", `key ${first?.keyId ?? "?"}, layer ${first?.layer ?? "?"}: the keyboard cannot hold this entry (${first?.reason ?? "unknown"})`, { layer: first?.layer, field: "args" });
}

/** SettingsError -> {key, params} for the page's plain-language text (keys "beh.err.*" in the m6 locale files). */
export function explainSettingsError(err) {
  const w = err?.where ?? {};
  const params = { profile: Number.isInteger(w.profile) ? w.profile + 1 : "", n: Number.isInteger(w.index) ? w.index + 1 : "" };
  if (!(err instanceof SettingsError)) return { key: "beh.err.other", params };
  if (err.code === "record") {
    const f = w.field ?? "";
    if (f === "count") return { key: "beh.err.count", params };
    const hit = /^(dks|morph|socd)\.(\w+)$/.exec(f);
    if (hit) return { key: `beh.err.${hit[1]}.${hit[2]}`, params };
    return { key: "beh.err.record", params };
  }
  if (err.code === "usage") return { key: w.field === "reference" ? "beh.err.reference" : "beh.err.entry", params };
  if (err.code === "length") return { key: "beh.err.length", params };
  return { key: "beh.err.other", params };
}

/** Issue of behaviorIssues -> {key, params}; it reads like the matching codec error ("beh.err.*"). `profile` = the profile number shown to the person. */
export function explainIssue(issue, profile = "") {
  const params = { profile, n: issue.index + 1 };
  if (issue.why === "count") return { key: "beh.err.count", params };
  if (issue.area === "entry") return { key: issue.why === "reference" ? "beh.err.reference" : "beh.err.entry", params };
  return { key: `beh.err.${issue.area}.${issue.why}`, params };
}

/** Every text key explainSettingsError / explainIssue can return (the locale test checks that each exists in both languages). */
export const ISSUE_KEYS = Object.freeze([
  "beh.err.other", "beh.err.count", "beh.err.record", "beh.err.reference", "beh.err.entry", "beh.err.length",
  "beh.err.dks.depth", "beh.err.dks.action", "beh.err.dks.shape", "beh.err.dks.flags",
  "beh.err.morph.mask", "beh.err.morph.usage", "beh.err.morph.flags", "beh.err.morph.shape",
  "beh.err.socd.slots", "beh.err.socd.mode", "beh.err.socd.flags", "beh.err.socd.shape", "beh.err.socd.overlap",
]);

/** The pairs to offer next: the first two free keys of A/D, W/S, ←/→, ↑/↓, else the first two free keys of the layout. [slotA, slotB] or null. */
export function nextFreePair(profile, layout) {
  const taken = socdTaken(profile);
  const free = (id) => { const k = layout.byId.get(id); return k && slotHasKey(k.slot) && !taken.has(k.slot) ? k.slot : null; };
  for (const pair of SOCD_PRESETS.flatMap((p) => p.pairs)) {
    const a = free(pair[0]), b = free(pair[1]);
    if (a !== null && b !== null) return [a, b];
  }
  const rest = layout.keys.map((k) => k.slot).filter((s) => slotHasKey(s) && !taken.has(s));
  return rest.length >= 2 ? [rest[0], rest[1]] : null;
}

export const BEHAVIOR_SLOTS = FW_SETTINGS.keys;
