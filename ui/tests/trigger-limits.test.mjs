// Trigger limits (one constants module), the rules that keep a trigger valid, version-1 profile migration, and the translated
// text families the UI builds keys for at run time.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { LIMITS, MODE, defaultTrigger, normalizeTrigger, triggerIssues, stepKey, newKeyState } from "../js/core/trigger.js";
import { UI_LIMITS, NOISE_FLOOR_MM, HYSTERESIS_MIN_MM, REASONS } from "../js/core/limits.js";
import { loadLayout } from "../js/core/layout.js";
import { defaultProfile, migrateProfile, validateProfile, importProfileJson, applyTriggerPatch, DEFAULT_FN_LAYER, LAYER_COUNT, PROFILE_VERSION } from "../js/core/profile.js";
import { mmToRaw } from "../js/core/units.js";
import { PROBLEM_KINDS } from "../js/device/errors.js";
import { FAULTS } from "../js/device/scope_client.js";
import zh from "../js/locales/zh-TW.js";
import en from "../js/locales/en.js";

const layout = loadLayout();
const jsRoot = join(dirname(fileURLToPath(import.meta.url)), "..", "js");

test("the UI limits come from ONE module: trigger.js re-exports them, nothing else defines its own", () => {
  assert.equal(LIMITS, UI_LIMITS);
  const offenders = [];
  const walk = (dir) => readdirSync(dir).forEach((f) => {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) return walk(p);
    if (!p.endsWith(".js") || p.endsWith("limits.js")) return;
    // a literal noise-floor style number used as a limit would mean a second source of truth
    if (/\b(min|max):\s*0?\.\d+/.test(readFileSync(p, "utf8"))) offenders.push(p);
  });
  walk(jsRoot);
  assert.deepEqual(offenders, [], "limits defined outside core/limits.js");
});

test("the factory trigger satisfies every limit", () => {
  assert.deepEqual(triggerIssues(defaultTrigger()), []);
});

test("nothing below the noise floor: thresholds are raised, dead zones snap to off or to the floor", () => {
  const t = { ...defaultTrigger(), rapidDown: 0.05, rapidUp: 0.1, topDead: 0.1, bottomDead: 0.04 };
  const notes = normalizeTrigger(t, ["rapidDown"]);
  assert.equal(t.rapidDown, NOISE_FLOOR_MM);
  assert.equal(t.rapidUp, NOISE_FLOOR_MM);
  assert.equal(t.topDead, NOISE_FLOOR_MM, "0.10 is nearer to the floor than to off");
  assert.equal(t.bottomDead, 0, "0.04 is nearer to off");
  assert.deepEqual(notes.map((n) => `${n.field}:${n.code}`).sort(), ["bottomDead:dead_floor", "rapidDown:below_noise", "rapidUp:below_noise", "topDead:dead_floor"]);
});

test("the release point always stays below the actuation point; the release point is the one that gives way", () => {
  const t = { ...defaultTrigger(), actuation: 0.5, release: 0.8 };
  const notes = normalizeTrigger(t, ["actuation"]);
  assert.equal(t.actuation, 0.5);
  assert.equal(t.release, 0.4);
  assert.deepEqual(notes.map((n) => n.code), ["release_order"]);
  const u = { ...defaultTrigger(), actuation: 1.0, release: 0.95 };
  normalizeTrigger(u, ["release"]);
  assert.equal(u.release, 0.9);
  assert.ok(u.actuation - u.release >= HYSTERESIS_MIN_MM - 1e-9);
  const low = { ...defaultTrigger(), actuation: 0.2 };
  normalizeTrigger(low, ["actuation"]);
  assert.equal(low.actuation, UI_LIMITS.actuation.min, "the actuation point leaves room for a release point below it");
  assert.ok(low.release >= UI_LIMITS.release.min);
});

test("values above the maximum are lowered with a reason", () => {
  const t = { ...defaultTrigger(), actuation: 9, rapidDown: 9 };
  const notes = normalizeTrigger(t, ["actuation"]);
  assert.equal(t.actuation, 3.4);
  assert.equal(t.rapidDown, 3.4);
  assert.ok(notes.every((n) => REASONS.includes(n.code)));
});

test("applyTriggerPatch returns the notes and applies to the listed keys only", () => {
  const p = defaultProfile(layout);
  const notes = applyTriggerPatch(p, ["KeyA", "KeyB"], { rapidUp: 0.01, topDead: 0.01 });
  assert.equal(p.keys.KeyA.trigger.rapidUp, NOISE_FLOOR_MM);
  assert.equal(p.keys.KeyB.trigger.topDead, 0);
  assert.equal(p.keys.KeyC.trigger.rapidUp, 0.3);
  assert.ok(notes.length >= 3);
  assert.deepEqual(applyTriggerPatch(p, ["KeyA"], { actuation: 1.3 }), []);
});

test("the live model releases a fixed-mode key at the release point, not at the actuation point", () => {
  const t = { ...defaultTrigger(), actuation: 1.2, release: 0.8 };
  const st = newKeyState();
  const ev = [];
  for (const mm of [0, 1.0, 1.3, 1.0, 0.85, 0.75]) { const e = stepKey(st, mmToRaw(mm), t); if (e) ev.push([mm, e]); }
  assert.deepEqual(ev, [[1.3, "press"], [0.75, "release"]]);
});

test("version 1 profile files still import: two layers become four, the release point is derived, old values are lifted to the floor", () => {
  const v1 = defaultProfile(layout);
  v1.version = 1;
  for (const k of Object.values(v1.keys)) { k.bindings = k.bindings.slice(0, 2); delete k.trigger.release; }
  v1.keys.KeyW.trigger = { mode: MODE.RT_CONTINUOUS, actuation: 0.5, rapidDown: 0.1, rapidUp: 0.1, topDead: 0.05, bottomDead: 0.15 }; // the old "speed" preset
  v1.keys.KeyQ.trigger = { mode: MODE.NORMAL, actuation: 2.0, rapidDown: 0.3, rapidUp: 0.3, topDead: 0, bottomDead: 0 };
  const r = importProfileJson(JSON.stringify(v1), layout);
  assert.ok(r.ok, JSON.stringify(r.errors));
  assert.equal(r.profile.version, PROFILE_VERSION);
  assert.equal(r.profile.keys.KeyW.bindings.length, LAYER_COUNT);
  assert.deepEqual(r.profile.keys.KeyW.bindings.slice(2), [null, null]);
  assert.equal(r.profile.keys.KeyQ.trigger.release, 1.6, "release = actuation - 0.4");
  assert.equal(r.profile.keys.KeyW.trigger.rapidDown, NOISE_FLOOR_MM);
  assert.ok(triggerIssues(r.profile.keys.KeyW.trigger).length === 0);
  assert.equal(migrateProfile(null), null);
  assert.equal(migrateProfile({ version: 2 }).version, 2, "only version 1 is touched");
});

test("a profile of version 2 with a missing layer is refused with a path", () => {
  const p = defaultProfile(layout);
  p.keys.KeyA.bindings = p.keys.KeyA.bindings.slice(0, 2);
  const r = validateProfile(p, layout);
  assert.equal(r.ok, false);
  assert.ok(r.errors.some((e) => e.path === "keys.KeyA.bindings" && e.code === "type"));
});

test("the default Fn layer is the key map's table (spot checks of LAYERS_NOTES.md)", () => {
  assert.equal(DEFAULT_FN_LAYER.Digit1.usage, 58);          // F1
  assert.equal(DEFAULT_FN_LAYER.Equal.usage, 69);           // F12
  assert.deepEqual(DEFAULT_FN_LAYER.KeyI, { type: "key", usage: 82 }); // up
  assert.deepEqual(DEFAULT_FN_LAYER.KeyZ, { type: "profile", index: 1 });
  assert.deepEqual(DEFAULT_FN_LAYER.KeyM, { type: "media", usage: 0xe2 });
  assert.equal(Object.keys(DEFAULT_FN_LAYER).length, 12 + 2 + 8 + 3 + 3 + 4);
});

test("every text key the UI composes at run time exists in both languages", () => {
  const keys = [];
  for (const k of PROBLEM_KINDS) keys.push(`problem.${k}.title`, `problem.${k}.body`);
  for (const r of REASONS) keys.push(`trig.reason.${r}`);
  for (let i = 0; i < LAYER_COUNT; i++) keys.push(`remap.layer${i}`, `remap.layer${i}.tip`);
  for (const s of ["saved", "unsaved", "saving", "local"]) keys.push(`pill.${s}`, `pill.${s}.tip`);
  for (const s of ["encode", "stage", "commit", "verify"]) keys.push(`save.step.${s}`, `save.step.${s}.short`);
  for (const [, f] of FAULTS) keys.push(`health.fault.${f}`);
  for (const c of ["none", "iwdg", "wwdg", "soft", "por", "pin", "lowpower"]) keys.push(`health.cause.${c}`);
  for (const w of ["too_few_frames", "keys_held", "unstable", "implausible"]) keys.push(`diag.cal.why.${w}`);
  for (const s of ["mock", "connected", "connecting", "disconnected", "error"]) keys.push(`status.${s}`);
  for (const k of keys) assert.ok(k in en && k in zh, `missing: ${k}`);
});

test("unreachable layers: a layer with entries that no key switches to is reported (informational)", async () => {
  const { unreachableLayers } = await import("../js/core/profile.js");
  const p = defaultProfile(layout);
  assert.deepEqual(unreachableLayers(p, layout), [], "the factory profile: Fn holds layer 1, layers 2 and 3 are empty");
  p.keys.KeyQ.bindings[2] = { type: "key", usage: 20 };
  assert.deepEqual(unreachableLayers(p, layout), [2]);
  p.keys.KeyA.bindings[0] = { type: "layer", layer: 2 };               // a hold-layer-2 key on the base layer
  assert.deepEqual(unreachableLayers(p, layout), []);
  p.keys.KeyW.bindings[3] = { type: "key", usage: 26 };
  p.keys.KeyS.bindings[2] = { type: "layer", layer: 3 };               // reachable only from layer 2, which is reachable
  assert.deepEqual(unreachableLayers(p, layout), []);
  p.keys.KeyA.bindings[0] = { type: "key", usage: 4 };                  // take the only way into layer 2 away
  assert.deepEqual(unreachableLayers(p, layout), [2, 3]);
  const fnKey = defaultProfile(layout);
  fnKey.keys.Fn.bindings[0] = { type: "key", usage: 4 };                // Fn is an ordinary key: reassigned, layer 1 is unreachable
  assert.deepEqual(unreachableLayers(fnKey, layout), [1]);
});

test("layer names are UI-only: optional, validated, kept on import and export", () => {
  const p = defaultProfile(layout);
  assert.deepEqual(p.layerNames, ["", "", "", ""]);
  p.layerNames = ["Base", "Media", "", "Nav"];
  const r = importProfileJson(JSON.stringify(p), layout);
  assert.ok(r.ok);
  assert.deepEqual(r.profile.layerNames, ["Base", "Media", "", "Nav"]);
  const old = defaultProfile(layout);
  delete old.layerNames;
  assert.deepEqual(importProfileJson(JSON.stringify(old), layout).profile.layerNames, ["", "", "", ""], "files without names still import");
  p.layerNames = ["x".repeat(25), "", "", ""];
  assert.ok(validateProfile(p, layout).errors.some((e) => e.path === "layerNames"));
  p.layerNames = ["a", "b"];
  assert.equal(validateProfile(p, layout).ok, false);
});

test("the Fn key is an ordinary key: its base-layer entry is only the factory default and can be anything the keyboard stores", async () => {
  const { workspaceToBlob, blobToWorkspace } = await import("../js/core/profile_codec.js");
  const { defaultWorkspace } = await import("../js/core/profile_codec.js");
  const ws = defaultWorkspace(layout);
  assert.deepEqual(ws.profiles[0].keys.Fn.bindings[0], { type: "layer", layer: 1 });
  ws.profiles[0].keys.Fn.bindings[0] = { type: "key", usage: 0xe4 };      // right Ctrl
  ws.profiles[0].keys.KeyM.bindings[0] = { type: "layer", layer: 1 };     // and the hold key moves to M
  const back = blobToWorkspace(workspaceToBlob(ws, layout).bytes, layout);
  assert.deepEqual(back.profiles[0].keys.Fn.bindings[0], { type: "key", usage: 0xe4 });
  assert.deepEqual(back.profiles[0].keys.KeyM.bindings[0], { type: "layer", layer: 1 });
  assert.deepEqual(back.profiles[1].keys.Fn.bindings[0], { type: "layer", layer: 1 }, "other profiles are untouched: layers are per profile");
});

test("the picker knows every function code of the key map with plain names", async () => {
  const { ALL_USAGES, bindingLabel } = await import("../js/core/hid-usages.js");
  const names = ALL_USAGES.filter((u) => u.group === "special").map((u) => u.name);
  for (const n of ["Hold layer 1", "Hold layer 2", "Hold layer 3", "Switch to profile 1", "Switch to profile 4", "Transparent", "No key"]) assert.ok(names.includes(n), n);
  const media = ALL_USAGES.filter((u) => u.group === "media").map((u) => u.name);
  for (const n of ["Volume up", "Volume down", "Mute", "Previous track", "Play / Pause", "Next track"]) assert.ok(media.includes(n), n);
  assert.equal(bindingLabel({ type: "profile", index: 3 }), "Switch to profile 3");
});
