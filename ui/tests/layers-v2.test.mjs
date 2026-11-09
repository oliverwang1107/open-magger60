// Layers v2 (task L4): toggle layer, tap-hold and one-shot modifier bindings, the per-profile layer options, and their way
// through the settings blob (format version 2). The reference for the bytes is the REAL firmware, through the generated vectors.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { loadLayout } from "../js/core/layout.js";
import {
  defaultProfile, validateProfile, importProfileJson, exportProfileJson, defaultLayerOpts, layerOptsProblem, toggleTraps, unreachableLayers,
  TAP_POLICY_NAMES,
} from "../js/core/profile.js";
import { bindingToCode, codeToBinding, bindingStorable, defaultWorkspace, workspaceToBlob, blobToWorkspace, normalizeBlob, sameBytes } from "../js/core/profile_codec.js";
import { decodeSettings, encodeSettings } from "../js/core/settings_codec.js";
import { ALL_USAGES, bindingId, capLabel, bindingLabel, cleanBinding, isValidBinding, findUsage, GROUP_ORDER } from "../js/core/hid-usages.js";
import { CODE, FW_LAYER_OPTS, FW_USAGE_RULE, FW_USAGE_RULE_V2 } from "../js/core/limits.js";
import en from "../js/locales/en.js";
import zh from "../js/locales/zh-TW.js";

const V = JSON.parse(readFileSync(new URL("./vectors/settings_vectors.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const bytes = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const layout = loadLayout();
const key = (usage) => ({ type: "key", usage });
const th = (tap, hold) => ({ type: "taphold", tap, hold });

test("binding <-> key code + arguments for toggle, one-shot and tap-hold", () => {
  assert.deepEqual(bindingToCode({ type: "toggle", layer: 2 }, 0), { code: 0xe8, args: [2, 0], extended: true });
  assert.deepEqual(bindingToCode({ type: "oneshot", usage: 0xe1 }, 1), { code: 0xea, args: [0xe1, 0], extended: true });
  assert.deepEqual(bindingToCode(th(key(0x2c), { type: "layer", layer: 1 }), 0), { code: 0xe9, args: [0x2c, 0xf0], extended: true });
  assert.deepEqual(bindingToCode(th(key(0x29), key(0xe0)), 0), { code: 0xe9, args: [0x29, 0xe0], extended: true });
  assert.deepEqual(bindingToCode(th({ type: "media", usage: 0xcd }, { type: "layer", layer: 3 }), 2), { code: 0xe9, args: [0xf9, 0xfc], extended: true });
  for (const b of [{ type: "toggle", layer: 1 }, { type: "toggle", layer: 3 }, { type: "oneshot", usage: 0xe7 }, th(key(4), key(0xe7)),
    th({ type: "media", usage: 0xe2 }, { type: "layer", layer: 2 })]) {
    const r = bindingToCode(b, 0);
    assert.deepEqual(codeToBinding(r.code, 0, r.args), b, bindingId(b));
    assert.ok(bindingStorable(b, 0));
  }
});

test("things a tap-hold key cannot hold are refused, not written wrongly", () => {
  const bad = [
    { type: "toggle", layer: 0 }, { type: "toggle", layer: 4 }, { type: "oneshot", usage: 4 }, { type: "oneshot", usage: 0xe8 },
    th({ type: "layer", layer: 1 }, key(0xe0)),                                  // the tap action cannot be a layer
    th(key(4), key(4)),                                                          // holding the tap key itself is pointless
    th(key(4), { type: "media", usage: 0xe9 }),
    th({ type: "media", usage: 0x192 }, key(0xe0)),                              // a media key the keyboard cannot store
    th({ type: "key", usage: 4, mods: [224] }, key(0xe0)),
    th(key(4), { type: "profile", index: 1 }), th(null, key(0xe0)), th(key(4), null),
  ];
  for (const b of bad) assert.ok(bindingToCode(b, 0).problem, JSON.stringify(b));
  for (const b of bad) assert.equal(bindingStorable(b, 0), false);
  // a firmware whose rule has no extended codes (format 1) cannot store them
  const v1 = { ...FW_USAGE_RULE, extFirst: undefined, extLast: undefined };
  assert.equal(bindingStorable({ type: "toggle", layer: 1 }, 0, v1), false);
  assert.equal(bindingStorable(key(4), 0, v1), true);
});

test("settings format 3: a tap-hold key may hold an ordinary key; a firmware that writes format 2 cannot store that", () => {
  const b = th(key(0x2c), key(0x05));
  assert.deepEqual(bindingToCode(b, 0), { code: 0xe9, args: [0x2c, 0x05], extended: true });
  assert.equal(bindingStorable(b, 0), true);
  assert.equal(bindingStorable(b, 0, FW_USAGE_RULE_V2), false);
  assert.equal(bindingStorable(th(key(0x2c), key(0xe0)), 0, FW_USAGE_RULE_V2), true, "a modifier hold was always fine");
  assert.deepEqual(codeToBinding(0xe9, 0, [0x2c, 0x05]), b, "and it comes back as the same binding");
  // the workspace carrying it is written as format 3
  const ws = defaultWorkspace(layout);
  ws.profiles[0].keys.KeyA.bindings[0] = b;
  const r = workspaceToBlob(ws, layout);
  assert.deepEqual(r.problems, []);
  assert.equal(r.bytes[4], 3);
  assert.deepEqual(blobToWorkspace(r.bytes, layout).profiles[0].keys.KeyA.bindings[0], b);
  assert.equal(workspaceToBlob(ws, layout, FW_USAGE_RULE_V2).bytes, null, "refused against a format 2 firmware");
});

test("M6 entries the UI has no editor for survive a trip through the UI model unchanged", () => {
  const ws = defaultWorkspace(layout);
  const slotA = layout.byId.get("KeyA").slot;
  const raw = workspaceToBlob(ws, layout).raw;
  raw.profiles[2].dks = [{ tAct: 120, tBot: 0, flags: 0, acts: [[4, 1], [0, 0], [0, 0], [0, 0]] }];
  raw.profiles[2].layers[1][slotA] = 0xed; raw.profiles[2].args[1][slotA] = [0, 0];
  raw.profiles[0].layers[0][slotA] = 0xef; raw.profiles[0].args[0][slotA] = [0x0b, 2];
  raw.profiles[3].pad.curves[1].shape = 3;
  raw.profiles[1].layers[3][slotA] = 0xfd; raw.profiles[1].args[3][slotA] = [3, 0];
  raw.device = { padMode: 1, filterMode: 0 };
  raw.unknown = [{ id: 0x90, scope: 0xff, bytes: [1, 2, 3] }];
  const blob = encodeSettings(raw);
  assert.equal(blob[4], 3);
  const ws2 = blobToWorkspace(blob, layout);
  assert.deepEqual(ws2.profiles[0].keys.KeyA.bindings[0], { type: "fw", index: (0xef << 16) | (0x0b << 8) | 2 });
  assert.ok(ws2.profiles[2].m6.dks.length === 1 && ws2.profiles[3].m6.pad.curves[1].shape === 3 && !ws2.profiles[1].m6);
  assert.deepEqual(ws2.m6.device, { padMode: 1, filterMode: 0 });
  const back = workspaceToBlob(ws2, layout);
  assert.deepEqual(back.problems, []);
  assert.equal(hex(back.bytes), hex(blob), "nothing was lost or reordered");
  assert.equal(hex(normalizeBlob(blob, layout)), hex(blob));
  // the opaque binding carries the code: a reserved 0xFD with no action is still a plain 'none'
  assert.deepEqual(codeToBinding(0xfd, 0, [0, 0]), { type: "none" });
  assert.deepEqual(codeToBinding(0xfd, 0, [3, 0]), { type: "fw", index: (0xfd << 16) | (3 << 8) });
  assert.ok(bindingToCode({ type: "fw", index: -1 }, 0).problem && bindingToCode({ type: "fw", index: 0x1000000 }, 0).problem);
  // a workspace without any of it still saves as format 2
  assert.equal(workspaceToBlob(defaultWorkspace(layout), layout).bytes[4], 2);
});

test("shape check, cleaning, ids, labels of the new bindings", () => {
  const b = th({ type: "media", usage: 0xcd }, { type: "layer", layer: 2 });
  assert.ok(isValidBinding(b) && isValidBinding({ type: "toggle", layer: 3 }) && isValidBinding({ type: "oneshot", usage: 225 }));
  assert.ok(!isValidBinding({ type: "toggle", layer: 4 }) && !isValidBinding({ type: "oneshot", usage: 4 }) && !isValidBinding(th(key(4), key(5))));
  assert.ok(!isValidBinding({ type: "taphold", tap: key(4) }) && !isValidBinding(th({ type: "media", usage: 0x1234 }, key(0xe0))));
  assert.deepEqual(cleanBinding({ ...b, extra: 1, tap: { ...b.tap, junk: 2 } }), b);
  assert.equal(bindingId(th(key(0x2c), { type: "layer", layer: 1 })), "taphold:key:44>layer:1");
  assert.notEqual(bindingId(th(key(0x2c), key(0xe0))), bindingId(th(key(0x2c), key(0xe1))));
  assert.equal(bindingLabel(th(key(0x2c), key(0xe0))), "Tap Space / Hold Left Ctrl");
  assert.equal(capLabel(th(key(0x29), key(0xe0))), "Esc/LCtrl");
  assert.equal(capLabel(th(key(0x2c), { type: "layer", layer: 1 })), "Space/L1");
  assert.equal(capLabel({ type: "toggle", layer: 2 }), "TG2");
  assert.equal(capLabel({ type: "oneshot", usage: 0xe1 }), "OS LShift");
});

test("the picker offers toggle layers and every one-shot modifier, with names in both languages", () => {
  for (let l = 1; l <= 3; l++) assert.ok(findUsage({ type: "toggle", layer: l }), `toggle ${l}`);
  for (let u = 224; u <= 231; u++) assert.ok(findUsage({ type: "oneshot", usage: u }), `oneshot ${u}`);
  assert.ok(GROUP_ORDER.includes("oneshot"));
  for (const item of ALL_USAGES.filter((i) => i.binding?.type === "toggle" || i.binding?.type === "oneshot")) {
    assert.ok(`u.${item.name}` in en && `u.${item.name}` in zh, item.name);
    assert.ok(bindingStorable(item.binding, 0), item.name);
  }
  assert.ok("group.oneshot" in en && "group.oneshot" in zh);
});

test("layer options: defaults, limits, names", () => {
  const d = defaultLayerOpts();
  assert.deepEqual(d, { tapTermMs: 200, oneShotMs: 2000, tapPolicy: "timeout" });
  assert.equal(layerOptsProblem(d), null);
  assert.equal(layerOptsProblem({ ...d, tapTermMs: 29 }), "tapTermMs");
  assert.equal(layerOptsProblem({ ...d, tapTermMs: 1001 }), "tapTermMs");
  assert.equal(layerOptsProblem({ ...d, tapTermMs: 30.5 }), "tapTermMs");
  assert.equal(layerOptsProblem({ ...d, oneShotMs: 99 }), "oneShotMs");
  assert.equal(layerOptsProblem({ ...d, oneShotMs: 0 }), null);
  assert.equal(layerOptsProblem({ ...d, oneShotMs: FW_LAYER_OPTS.oneShotMax + 1 }), "oneShotMs");
  assert.equal(layerOptsProblem({ ...d, tapPolicy: "fast" }), "tapPolicy");
  assert.equal(layerOptsProblem(null), "type");
  assert.deepEqual([...TAP_POLICY_NAMES], ["timeout", "permissive", "other-key"]);
});

test("a profile without layer options (an older export) imports with the defaults; bad options are refused with a path", () => {
  const p = defaultProfile(layout);
  const old = JSON.parse(exportProfileJson(p));
  delete old.layerOpts;
  const r = validateProfile(old, layout);
  assert.ok(r.ok, JSON.stringify(r.errors));
  assert.deepEqual(r.profile.layerOpts, defaultLayerOpts());
  const bad = JSON.parse(exportProfileJson(p));
  bad.layerOpts.tapTermMs = 5;
  const r2 = validateProfile(bad, layout);
  assert.deepEqual(r2.errors.map((e) => `${e.path}:${e.code}`), ["layerOpts.tapTermMs:range"]);
  // new bindings travel through export / import
  p.keys.KeyA.bindings[0] = th(key(0x2c), { type: "layer", layer: 1 });
  p.keys.KeyS.bindings[1] = { type: "toggle", layer: 2 };
  p.keys.KeyD.bindings[2] = { type: "oneshot", usage: 0xe1 };
  p.layerOpts = { tapTermMs: 150, oneShotMs: 0, tapPolicy: "permissive" };
  const back = importProfileJson(exportProfileJson(p), layout);
  assert.ok(back.ok, JSON.stringify(back.errors));
  assert.deepEqual(back.profile, p);
  const broken = JSON.parse(exportProfileJson(p));
  broken.keys.KeyA.bindings[0].hold = key(5);
  assert.ok(!validateProfile(broken, layout).ok);
});

function workspaceWithNewKeys() {
  const ws = defaultWorkspace(layout);
  const p = ws.profiles[1];
  p.keys.Space.bindings[0] = th(key(0x2c), { type: "layer", layer: 1 });
  p.keys.Escape.bindings[0] = { type: "toggle", layer: 2 };      // layer 2 itself keeps it transparent: the same toggle key switches it off again
  p.keys.ControlLeft.bindings[0] = th(key(0x29), key(0xe0));
  p.keys.ShiftLeft.bindings[0] = { type: "oneshot", usage: 0xe1 };
  p.keys.KeyQ.bindings[3] = th({ type: "media", usage: 0xcd }, { type: "layer", layer: 2 });
  p.layerOpts = { tapTermMs: 180, oneShotMs: 1500, tapPolicy: "other-key" };
  return ws;
}

test("a workspace with the new key kinds goes into the blob and comes back identical", () => {
  const ws = workspaceWithNewKeys();
  const r = workspaceToBlob(ws, layout);
  assert.deepEqual(r.problems, []);
  assert.equal(r.bytes.length, 7944);
  const back = blobToWorkspace(r.bytes, layout);
  assert.deepEqual(back.profiles[1].keys.Space.bindings[0], ws.profiles[1].keys.Space.bindings[0]);
  assert.equal(back.profiles[1].keys.Escape.bindings[2], null);
  assert.deepEqual(back.profiles[1].keys.KeyQ.bindings[3], ws.profiles[1].keys.KeyQ.bindings[3]);
  assert.deepEqual(back.profiles[1].layerOpts, ws.profiles[1].layerOpts);
  assert.deepEqual(back.profiles[0].layerOpts, defaultLayerOpts());
  assert.deepEqual(back, ws);
  assert.ok(sameBytes(normalizeBlob(r.bytes, layout), r.bytes), "nothing differs after a round trip (the dirty check stays quiet)");
  const raw = decodeSettings(r.bytes);
  const slot = layout.byId.get("Space").slot;
  assert.equal(raw.profiles[1].layers[0][slot], CODE.TAP_HOLD);
  assert.deepEqual(raw.profiles[1].args[0][slot], [0x2c, 0xf0]);
  assert.deepEqual(raw.profiles[1].opts, { tapTerm: 180, oneshot: 1500, policy: 2 });
});

test("a transparent entry equal in code but different in arguments is not mistaken for transparent", () => {
  const ws = defaultWorkspace(layout);
  const p = ws.profiles[0];
  p.keys.KeyA.bindings[0] = { type: "toggle", layer: 1 };
  p.keys.KeyA.bindings[1] = { type: "toggle", layer: 2 };   // same code 0xE8, other argument: a real entry on layer 1
  const back = blobToWorkspace(workspaceToBlob(ws, layout).bytes, layout);
  assert.deepEqual(back.profiles[0].keys.KeyA.bindings[1], { type: "toggle", layer: 2 });
  p.keys.KeyA.bindings[1] = { type: "toggle", layer: 1 };   // identical to the base entry: that is what transparent does
  const back2 = blobToWorkspace(workspaceToBlob(ws, layout).bytes, layout);
  assert.equal(back2.profiles[0].keys.KeyA.bindings[1], null);
});

test("the firmware's own vectors survive the UI model unchanged (key slots only)", () => {
  const names = ["toggle_layers", "tap_hold_layer", "tap_hold_modifier", "tap_hold_media_tap", "one_shot_all_modifiers", "layer_opts_custom", "everything_v2"];
  for (const name of names) {
    const c = V.cases.find((x) => x.name === name);
    assert.ok(c && c.encoded, name);
    const blob = bytes(c.encoded);
    const ws = blobToWorkspace(blob, layout);
    assert.equal(hex(workspaceToBlob(ws, layout).bytes), c.encoded, `${name}: UI model -> blob gives the firmware's bytes back`);
  }
});

test("a version 1 blob opens in the UI and is written back as version 2 with the defaults for the new things", () => {
  const m = V.migrations.find((x) => x.name === "v1_default");
  const ws = blobToWorkspace(bytes(m.v1), layout);
  assert.deepEqual(ws, defaultWorkspace(layout));
  assert.equal(hex(workspaceToBlob(ws, layout).bytes), m.migrated);
  // the "reserved" codes of version 1 (0xFD / 0xFE) are 'no key'
  const r = V.migrations.find((x) => x.name === "v1_reserved_codes_FD_FE");
  const w2 = blobToWorkspace(bytes(r.v1), layout);
  assert.ok(w2.profiles.every((p) => Object.values(p.keys).every((k) => k.bindings.every((b) => b === null || b.type !== "taphold"))));
});

test("unreachable layers count toggle keys and the hold half of tap-hold keys; toggle traps are reported", () => {
  const p = defaultProfile(layout);
  p.keys.KeyA.bindings[2] = key(5);                          // layer 2 has an entry, nothing switches to it
  assert.deepEqual(unreachableLayers(p, layout), [2]);
  p.keys.KeyS.bindings[0] = { type: "toggle", layer: 2 };
  assert.deepEqual(unreachableLayers(p, layout), []);
  assert.deepEqual(toggleTraps(p, layout), [], "transparent on layer 2: the base layer's toggle key still works there");
  p.keys.KeyS.bindings[2] = key(7);                          // something else on that layer: the toggle key is gone there
  assert.deepEqual(toggleTraps(p, layout), [2], "layer 2 can be switched on but no key switches it off");
  p.keys.KeyS.bindings[2] = { type: "toggle", layer: 2 };    // the way back, written out
  assert.deepEqual(toggleTraps(p, layout), []);
  p.keys.KeyS.bindings[2] = key(7);
  p.keys.KeyD.bindings[2] = { type: "toggle", layer: 2 };    // or any other key on layer 2
  assert.deepEqual(toggleTraps(p, layout), []);
  const q = defaultProfile(layout);
  q.keys.KeyA.bindings[3] = key(5);
  q.keys.Space.bindings[0] = th(key(0x2c), { type: "layer", layer: 3 });
  assert.deepEqual(unreachableLayers(q, layout), [], "the hold half of a tap-hold key reaches its layer");
});
