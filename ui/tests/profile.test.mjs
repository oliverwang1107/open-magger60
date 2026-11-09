import test from "node:test";
import assert from "node:assert/strict";
import { loadLayout } from "../js/core/layout.js";
import { defaultProfile, validateProfile, importProfileJson, exportProfileJson, cloneProfile, applyTriggerPatch, PROFILE_VERSION } from "../js/core/profile.js";
import { hexToRgb, rgbToHex, isHexColor, EFFECTS } from "../js/core/lighting.js";
import { capLabel, bindingLabel, bindingFromUsage, bindingId, searchUsages, findUsage, isValidBinding, cleanBinding } from "../js/core/hid-usages.js";
import { usageFromEventCode } from "../js/core/keycodes.js";
import { LiveBuffer } from "../js/core/live-buffer.js";
import { MODE } from "../js/core/trigger.js";

const layout = loadLayout();
const codes = (r) => r.errors.map((e) => `${e.path}:${e.code}`);

test("default profile is valid and covers all 61 keys by id", () => {
  const p = defaultProfile(layout);
  assert.equal(Object.keys(p.keys).length, 61);
  assert.ok("KeyW" in p.keys && "Fn" in p.keys);
  const r = validateProfile(p, layout);
  assert.ok(r.ok, JSON.stringify(r.errors));
});

test("export -> import round trip keeps everything", () => {
  const p = defaultProfile(layout);
  p.name = "FPS";
  p.keys.KeyW.trigger.mode = MODE.RT;
  p.keys.KeyQ.bindings[0] = { type: "key", usage: 6, mods: [224] };
  const r = importProfileJson(exportProfileJson(p), layout);
  assert.ok(r.ok, JSON.stringify(r.errors));
  assert.deepEqual(r.profile, p);
});

test("rejects bad json, wrong kind, newer version", () => {
  assert.deepEqual(codes(importProfileJson("{nope", layout)), ["$:json"]);
  const p = defaultProfile(layout);
  assert.deepEqual(codes(validateProfile({ ...p, kind: "other" }, layout)), ["kind:kind"]);
  assert.deepEqual(codes(validateProfile({ ...p, version: PROFILE_VERSION + 1 }, layout)), ["version:newer"]);
  assert.deepEqual(codes(validateProfile(null, layout)), ["$:type"]);
});

test("rejects out of range values with paths", () => {
  const p = defaultProfile(layout);
  p.keys.Escape.trigger.actuation = 9;
  p.keys.Digit1.trigger.mode = "turbo";
  p.keys.Digit2.color = "red";
  p.keys.Digit3.bindings[0] = { type: "key", usage: 999 };
  p.keys.Digit4.bindings[1] = { type: "mouse", button: 0 };
  p.lighting.brightness = 120;
  p.lighting.effect = "nope";
  p.keys.NotAKey = p.keys.Escape;
  const c = codes(validateProfile(p, layout));
  for (const e of ["keys.Escape.trigger.actuation:range", "keys.Digit1.trigger.mode:range", "keys.Digit2.color:color",
    "keys.Digit3.bindings[0]:binding", "keys.Digit4.bindings[1]:binding", "lighting.brightness:range",
    "lighting.effect:range", "keys.NotAKey:key"]) assert.ok(c.includes(e), e);
});

test("missing keys are filled from defaults on import", () => {
  const p = defaultProfile(layout);
  delete p.keys.Escape;
  const r = validateProfile(p, layout);
  assert.ok(r.ok);
  assert.equal(r.profile.keys.Escape.trigger.actuation, 1.2); // the firmware default (km_engine_key_cfg_default)
});

test("stray fields on bindings are dropped on import", () => {
  const p = defaultProfile(layout);
  p.keys.Escape.bindings[0] = { type: "media", usage: 0xe9, name: "Volume up", id: "x" };
  const r = validateProfile(p, layout);
  assert.deepEqual(r.profile.keys.Escape.bindings[0], { type: "media", usage: 0xe9 });
});

test("applyTriggerPatch clamps and applies to the given keys only", () => {
  const p = defaultProfile(layout);
  applyTriggerPatch(p, ["Escape", "Digit1"], { actuation: 99, rapidDown: 0, topDead: 0.123 });
  assert.equal(p.keys.Escape.trigger.actuation, 3.4);
  assert.equal(p.keys.Digit1.trigger.rapidDown, 0.2); // the noise floor (core/limits.js)
  assert.equal(p.keys.Digit1.trigger.topDead, 0.2);   // between 0 and the floor snaps to the nearer of off / floor
  assert.equal(p.keys.Digit2.trigger.actuation, 1.2);
});

test("bindings: ids, validation, labels, search, defaults", () => {
  assert.equal(bindingLabel(bindingFromUsage(4)), "A");
  assert.equal(bindingLabel(bindingFromUsage(224)), "Left Ctrl");
  assert.equal(bindingLabel(bindingFromUsage(null)), "Hold layer 1");
  assert.equal(bindingLabel({ type: "media", usage: 0xe9 }), "Volume up");
  assert.equal(bindingLabel({ type: "key", usage: 6, mods: [224] }), "LCtrl+C");
  assert.equal(bindingLabel(null), "");
  assert.equal(bindingId(null), "none");
  assert.equal(findUsage({ type: "media", usage: 0xe2 }).name, "Mute");
  assert.ok(isValidBinding(null) && isValidBinding({ type: "wheel", dir: "up" }) && isValidBinding({ type: "profile", index: 4 }));
  assert.ok(!isValidBinding({ type: "wheel", dir: "left" }) && !isValidBinding({ type: "key" }) && !isValidBinding("x"));
  assert.deepEqual(cleanBinding({ type: "mouse", button: 2, name: "x" }), { type: "mouse", button: 2 });
  assert.ok(searchUsages("vol").length >= 2);
  assert.ok(searchUsages("media").some((u) => u.name === "Mute"));
  assert.equal(searchUsages("zzzz").length, 0);
  assert.ok(searchUsages("").length > 100);
  const p = defaultProfile(layout);
  assert.equal(p.keys.Digit1.bindings[1].usage, 58); // Fn + 1 = F1
  assert.deepEqual(p.keys.Fn.bindings[0], { type: "layer", layer: 1 });
});

test("key cap labels stay short", () => {
  assert.equal(capLabel(bindingFromUsage(224)), "LCtrl");
  assert.equal(capLabel(bindingFromUsage(229)), "RShift");
  assert.equal(capLabel(bindingFromUsage(null)), "Hold L1");
  assert.equal(capLabel({ type: "media", usage: 0xe9 }), "Vol+");
  assert.equal(capLabel({ type: "profile", index: 2 }), "P2");
  assert.equal(capLabel(bindingFromUsage(4)), "A");
});

test("lighting helpers", () => {
  assert.deepEqual(hexToRgb("#ff8000"), [255, 128, 0]);
  assert.equal(rgbToHex([255, 128, 0]), "#ff8000");
  assert.equal(hexToRgb("zzz"), null);
  assert.ok(isHexColor("#AbCdEf") && !isHexColor("#abc"));
  assert.equal(new Set(EFFECTS.map((e) => e.id)).size, EFFECTS.length);
});

test("browser key codes map to HID usages that exist in the layout", () => {
  assert.equal(usageFromEventCode("KeyA"), 4);
  assert.equal(usageFromEventCode("Digit0"), 39);
  assert.equal(usageFromEventCode("Space"), 44);
  assert.equal(usageFromEventCode("ShiftRight"), 229);
  assert.equal(usageFromEventCode("F13"), null);
  for (const k of layout.keys) if (k.usage !== null) assert.equal(layout.byUsage.get(k.usage).id, k.id);
});

test("live buffer keeps a rolling window", () => {
  const b = new LiveBuffer(4, 2);
  for (let i = 0; i < 6; i++) b.push([i, 10 * i], i * 10);
  assert.equal(b.count, 4);
  const w = b.window(1, 50, 25);
  assert.deepEqual(w.v, [30, 40, 50]);
  assert.deepEqual(w.t, [30, 40, 50]);
  b.clear();
  assert.equal(b.window(0, 0, 1000).v.length, 0);
});

test("cloneProfile is deep", () => {
  const p = defaultProfile(layout);
  const c = cloneProfile(p);
  c.keys.Escape.trigger.actuation = 1;
  assert.equal(p.keys.Escape.trigger.actuation, 1.2);
});
