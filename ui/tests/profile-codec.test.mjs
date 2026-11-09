// UI profile model <-> settings blob. The reference for "what the firmware's default key map is" is the REAL C table
// (km_keymap_default_table), through the generated vectors.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { loadLayout } from "../js/core/layout.js";
import { defaultWorkspace, workspaceToBlob, blobToWorkspace, normalizeBlob, sameBytes, bindingToCode, codeToBinding, triggerToRaw, triggerFromRaw } from "../js/core/profile_codec.js";
import { defaultProfile, cloneProfile } from "../js/core/profile.js";
import { decodeSettings, encodeSettings, PROFILE_BYTES } from "../js/core/settings_codec.js";
import { MODE, defaultTrigger } from "../js/core/trigger.js";
import { FW_USAGE_RULE } from "../js/core/limits.js";

const V = JSON.parse(readFileSync(new URL("./vectors/settings_vectors.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const bytes = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const layout = loadLayout();

test("the UI default workspace saves as exactly the key map's default table (Fn holds layer 1, 0xFF = transparent)", () => {
  const r = workspaceToBlob(defaultWorkspace(layout), layout);
  assert.deepEqual(r.problems, []);
  assert.equal(r.converted, 0, "the firmware stores transparent, nothing is converted");
  assert.equal(r.raw.profiles[0].layers[1][layout.byId.get("KeyQ").slot], 0xff);
  assert.equal(r.raw.profiles[0].layers[0][layout.byId.get("Fn").slot], 0xf0);
  assert.equal(hex(r.bytes), V.default, "UI default == the firmware default (km_keymap_default_table() through km_settings_encode)");
});

test("against a firmware whose settings stop at 0xFA, transparent is written out as the base code (same table as the flattened vector)", () => {
  const narrow = { ...FW_USAGE_RULE, funcLast: 0xfa };
  const r = workspaceToBlob(defaultWorkspace(layout), layout, narrow);
  assert.deepEqual(r.problems, []);
  assert.ok(r.converted > 4 * 3 * 40, "many transparent entries were converted");
  assert.equal(hex(r.bytes), V.default_flattened);
});

test("blob -> workspace -> blob is stable, and a loaded default has no unsaved difference", () => {
  const b = bytes(V.default_table);
  const ws = blobToWorkspace(b, layout);
  assert.equal(ws.active, 0);
  assert.equal(hex(workspaceToBlob(ws, layout).bytes), V.default_table);
  assert.ok(sameBytes(normalizeBlob(b, layout), b));
  assert.deepEqual(ws.profiles[0].keys.Fn.bindings[0], { type: "layer", layer: 1 });
  assert.deepEqual(ws.profiles[1].keys.KeyZ.bindings[1], { type: "profile", index: 1 });
  assert.deepEqual(ws.profiles[2].keys.Comma.bindings[1], { type: "media", usage: 0xb6 });
});

test("what the keyboard returns shows up as the UI's own default (transparent on layers 1-3), also from the flattened form", () => {
  assert.deepEqual(blobToWorkspace(bytes(V.default_table), layout), defaultWorkspace(layout));
  assert.deepEqual(blobToWorkspace(bytes(V.default_flattened), layout), defaultWorkspace(layout), "overlay entries equal to the base are shown as transparent");
});

test("the firmware's own default blob IS the key map table, and the UI default saves as exactly that", () => {
  assert.equal(V.default, V.default_table, "km_settings_default() copies km_keymap_default_table()");
  const dflt = decodeSettings(bytes(V.default));
  assert.equal(dflt.profiles[0].layers[0][14], 0xf0, "the Fn key holds layer 1");
  assert.equal(hex(workspaceToBlob(defaultWorkspace(layout), layout).bytes), V.default);
  assert.deepEqual(blobToWorkspace(bytes(V.default), layout), defaultWorkspace(layout), "loaded: nothing differs from the UI default");
});

test("a blob from an older firmware whose default read function codes as 0 still loads: the Fn key just reads as 'no key'", () => {
  const raw = decodeSettings(bytes(V.default_table));
  for (const p of raw.profiles) for (const l of p.layers) l.forEach((c, i) => { if (c >= 0xf0) l[i] = 0; });
  const ws = blobToWorkspace(encodeSettings(raw), layout);
  assert.deepEqual(ws.profiles[0].keys.Fn.bindings[0], { type: "none" });
  assert.deepEqual(ws.profiles[0].keys.KeyQ.bindings[0], { type: "key", usage: 20 });
});

test("an edit changes exactly the bytes of that key", () => {
  const ws = defaultWorkspace(layout);
  const before = workspaceToBlob(ws, layout).bytes;
  ws.profiles[2].keys.KeyW.trigger = { ...defaultTrigger(), mode: MODE.RT_CONTINUOUS, actuation: 1.5, release: 1.0, rapidDown: 0.25, rapidUp: 0.3, topDead: 0.2, bottomDead: 0.4 };
  ws.profiles[2].keys.KeyW.bindings[0] = { type: "key", usage: 0xe0 };
  const after = workspaceToBlob(ws, layout).bytes;
  const diff = [];
  before.forEach((v, i) => { if (v !== after[i]) diff.push(i); });
  const slot = layout.byId.get("KeyW").slot;
  const cfgAt = 12 + 2 * PROFILE_BYTES + slot * 14;
  const layerAt = 12 + 2 * PROFILE_BYTES + 76 * 14 + 6 + slot;     // 6 bytes of layer options come first
  assert.ok(diff.every((i) => (i >= cfgAt && i < cfgAt + 14) || i === layerAt || i >= before.length - 4), diff.join());
  // (the key's own trigger bytes, its layer-0 code byte, and the CRC; "transparent" entries on layers 1..3 stay 0xFF, the argument bytes stay 0)
  const back = decodeSettings(after).profiles[2];
  assert.deepEqual(back.keys[slot], { press: 150, release: 100, mode: 1, continuous: 1, rtDown: 25, rtUp: 30, topDead: 20, bottomDead: 40 });
  assert.equal(back.layers[0][slot], 0xe0);
});

test("things the keyboard cannot do are reported, not dropped silently", () => {
  const ws = defaultWorkspace(layout);
  ws.profiles[0].keys.KeyA.bindings[0] = { type: "mouse", button: 1 };
  ws.profiles[0].keys.KeyB.bindings[1] = { type: "key", usage: 6, mods: [224] };
  ws.profiles[1].keys.KeyC.bindings[2] = { type: "layer", layer: 2 };   // storable now: 0xFB is a valid code
  ws.profiles[1].keys.KeyD.bindings[0] = { type: "media", usage: 0x192 };
  const r = workspaceToBlob(ws, layout);
  assert.equal(r.bytes, null);
  const key = (p) => `${p.profile}:${p.keyId}:${p.layer}:${p.reason}`;
  assert.deepEqual(r.problems.map(key).sort(), ["0:KeyA:0:type", "0:KeyB:1:mods", "1:KeyD:0:media"].sort());
});

test("binding <-> code covers the documented function codes", () => {
  const cases = [[{ type: "layer", layer: 1 }, 0xf0], [{ type: "profile", index: 1 }, 0xf1], [{ type: "profile", index: 4 }, 0xf4],
    [{ type: "media", usage: 0xea }, 0xf5], [{ type: "media", usage: 0xe9 }, 0xf6], [{ type: "media", usage: 0xe2 }, 0xf7],
    [{ type: "media", usage: 0xb6 }, 0xf8], [{ type: "media", usage: 0xcd }, 0xf9], [{ type: "media", usage: 0xb5 }, 0xfa],
    [{ type: "key", usage: 4 }, 4], [{ type: "key", usage: 0xe7 }, 0xe7], [{ type: "none" }, 0]];
  for (const [b, c] of cases) {
    assert.equal(bindingToCode(b, 1).code, c, JSON.stringify(b));
    assert.deepEqual(codeToBinding(c, 1), b);
  }
  assert.equal(bindingToCode(null, 1).code, 0xff);
  assert.equal(bindingToCode(null, 0).code, 0);
  assert.deepEqual(codeToBinding(0xff, 2), null);
  assert.deepEqual(codeToBinding(0xfd, 0), { type: "none" });
});

test("trigger <-> raw: modes and millimetres", () => {
  for (const mode of [MODE.NORMAL, MODE.RT, MODE.RT_CONTINUOUS]) {
    const t = { mode, actuation: 1.35, release: 0.9, rapidDown: 0.21, rapidUp: 0.33, topDead: 0.2, bottomDead: 0 };
    assert.deepEqual(triggerFromRaw(triggerToRaw(t)), t);
  }
  assert.deepEqual(triggerToRaw({ ...defaultTrigger(), mode: MODE.RT }), { press: 120, release: 80, mode: 1, continuous: 0, rtDown: 30, rtUp: 30, topDead: 0, bottomDead: 0 });
});

test("every UI trigger that passes the UI limits is accepted by the firmware rules", () => {
  const p = defaultProfile(layout);
  for (const t of [{ actuation: 0.3, release: 0.2 }, { actuation: 3.4, release: 3.3 }, { rapidDown: 0.2, rapidUp: 3.4 }, { topDead: 1.0, bottomDead: 1.0 }]) {
    const q = cloneProfile(p);
    Object.assign(q.keys.KeyA.trigger, t);
    const ws = { active: 0, profiles: [q, p, p, p] };
    assert.ok(workspaceToBlob(ws, layout).bytes, JSON.stringify(t));
  }
});
