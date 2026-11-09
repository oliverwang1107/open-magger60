// Behaviours page (M6 P9a): the data layer (core/behaviors.js) against the P0 settings codec, the limits, the plain-language errors and the
// two locale files of the page. The DOM of the page itself is checked by eye in the browser (see the hand-over note); everything it
// decides is decided in core/behaviors.js, which has no DOM.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { loadLayout } from "../js/core/layout.js";
import { defaultWorkspace, workspaceToBlob, blobToWorkspace, bindingToCode, codeToBinding, normalizeBlob } from "../js/core/profile_codec.js";
import { decodeSettings, defaultRawSettings, rawSettingsProblem, SettingsError } from "../js/core/settings_codec.js";
import { CODE, FW_M6, FW_SETTINGS, FW_NO_KEY_SLOTS } from "../js/core/limits.js";
import * as B from "../js/core/behaviors.js";
import en6 from "../js/locales/en-m6.js";
import zh6 from "../js/locales/zh-TW-m6.js";
import zh from "../js/locales/zh-TW.js";
import en from "../js/locales/en.js";

const layout = loadLayout();
const V = JSON.parse(readFileSync(new URL("./vectors/settings_vectors.json", import.meta.url), "utf8"));
const bytes = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const hex = (b) => Buffer.from(b).toString("hex");
const slotOf = (id) => layout.byId.get(id).slot;
const fresh = () => defaultWorkspace(layout).profiles[0];
const save = (profile, others = null) => {
  const ws = defaultWorkspace(layout);
  ws.profiles[0] = profile;
  if (others) others.forEach((p, i) => { ws.profiles[i + 1] = p; });
  return workspaceToBlob(ws, layout);
};
/** save the profile, read the blob back, return the profile as the UI would show it after Save + reload */
const roundTrip = (profile) => {
  const r = save(profile);
  assert.deepEqual(r.problems, [], "the codec takes the profile");
  assert.ok(r.bytes);
  return { bytes: r.bytes, profile: blobToWorkspace(r.bytes, layout).profiles[0] };
};
const sameBehaviorData = (a, b) => {
  assert.deepEqual(a.m6 ?? {}, b.m6 ?? {});
  for (const id of Object.keys(a.keys)) assert.deepEqual(a.keys[id].bindings, b.keys[id].bindings, id);
};

// ------------------------------------------------------------------------------------------------ bindings

test("opaque bindings: build, take apart, and agree with the profile codec's own conversion", () => {
  assert.deepEqual(B.fwParts(B.dksBinding(5)), { code: CODE.DKS, a0: 5, a1: 0 });
  assert.deepEqual(B.fwParts(B.morphBinding(0x2a, 3)), { code: CODE.MOD_MORPH, a0: 0x2a, a1: 3 });
  assert.deepEqual(B.fwParts(B.toggleKeyBinding(0x16)), { code: CODE.TOGGLE_KEY, a0: 0x16, a1: 0 });
  assert.equal(B.fwParts({ type: "key", usage: 4 }), null);
  assert.equal(B.fwParts(null), null);
  assert.equal(B.fwParts({ type: "fw", index: -1 }), null);
  assert.equal(B.fwParts({ type: "fw", index: 0x1000000 }), null);
  assert.equal(B.fwParts({ type: "fw", index: 1.5 }), null);
  for (const b of [B.dksBinding(0), B.dksBinding(7), B.morphBinding(0xe7, 7), B.toggleKeyBinding(0x04)]) {
    const c = bindingToCode(b, 0);
    assert.ok(!c.problem);
    assert.deepEqual(codeToBinding(c.code, 0, c.args), b, "what the codec reads back from the code bytes is the same binding");
  }
});

test("behaviorOf tells the three kinds apart and ignores everything else (also the other M6 codes)", () => {
  assert.deepEqual(B.behaviorOf(B.dksBinding(2)), { kind: "dks", index: 2 });
  assert.deepEqual(B.behaviorOf(B.morphBinding(0x2a, 3)), { kind: "morph", usage: 0x2a, index: 3 });
  assert.deepEqual(B.behaviorOf(B.toggleKeyBinding(0x16)), { kind: "toggle", usage: 0x16 });
  assert.equal(B.behaviorOf(B.fwBinding(CODE.PAD_BTN, 1, 0)), null);
  assert.equal(B.behaviorOf(B.fwBinding(CODE.PAD_AXIS, 0, 0)), null);
  assert.equal(B.behaviorOf(B.fwBinding(CODE.LIGHT, 1, 0)), null);
  assert.equal(B.behaviorOf({ type: "key", usage: 4 }), null);
  assert.equal(B.behaviorOf({ type: "toggle", layer: 1 }), null, "toggle LAYER is another thing");
  assert.equal(B.behaviorOf(null), null);
});

test("key cap labels are short", () => {
  assert.equal(B.fwCapLabel(B.dksBinding(0)), "DKS1");
  assert.equal(B.fwCapLabel(B.morphBinding(42, 1)), "MM2");
  assert.equal(B.fwCapLabel(B.toggleKeyBinding(0x16)), "TG S");
  assert.equal(B.fwCapLabel(B.fwBinding(CODE.PAD_BTN, 1, 0)), "•");
  assert.equal(B.fwCapLabel({ type: "none" }), "");
  for (let i = 0; i < 8; i++) assert.ok(B.fwCapLabel(B.dksBinding(i)).length <= 6);
});

// ------------------------------------------------------------------------------------------------ DKS

test("DKS events: two bits per moment, every byte unpacks and packs back", () => {
  for (let v = 0; v < 256; v++) assert.equal(B.dksEventsPack(B.dksEventsUnpack(v)), v);
  assert.deepEqual(B.dksEventsUnpack(0b11_10_01_00), [0, 1, 2, 3]);
  assert.equal(B.dksEventsPack([1, 0, 0, 2]), 0b10_00_00_01);
});

test("a new DKS is valid, uses the plan's example (A on the way down and up, S at the bottom) and the codec takes it", () => {
  const p = fresh();
  const rec = B.newDksRecord();
  assert.equal(B.addRecord(p, "dks", rec).ok, true);
  assert.deepEqual(p.m6.dks[0], rec);
  assert.equal(rec.tAct, 120);
  assert.equal(B.dksBottom(rec), 310);
  const d = B.describeDks(rec);
  assert.deepEqual(d.map((m) => m.steps.map((s) => `${s.usage}:${s.op}`)), [["4:1"], ["22:1"], ["22:2"], ["4:2"]]);
  assert.deepEqual(d[3].closing, []);
  const rt = roundTrip(p);
  assert.deepEqual(rt.profile.m6.dks, [rec]);
  assert.equal(rt.bytes[4], 3, "uses format 3");
});

test("describeDks: depths of the four moments, and what is still held at the end", () => {
  const rec = { tAct: 100, tBot: 300, flags: 0, acts: [[0x04, B.dksEventsPack([1, 0, 0, 0])], [0x05, B.dksEventsPack([0, 3, 0, 0])], [0, 0], [0, 0]] };
  const d = B.describeDks(rec);
  assert.deepEqual(d.map((m) => m.depth), [100, 300, 280, 90], "bottom - 0.20 mm back up, start - 0.10 mm for the release");
  assert.deepEqual(d[3].closing, [0x04], "A was pressed and never released: event 4 lets go of it");
  assert.deepEqual(B.describeDks({ ...rec, tBot: 0 }).map((m) => m.depth), [100, 310, 290, 90], "tBot 0 = 3.10 mm");
});

test("dksSetDepth keeps every rule of the codec: start >= 0.40, gap >= 0.40, bottom <= 3.40", () => {
  const rec = B.newDksRecord();
  let r = B.dksSetDepth(rec, "tAct", 200);
  assert.equal(r.rec.tAct, 200); assert.equal(r.clamped, false);
  r = B.dksSetDepth(rec, "tAct", 0);
  assert.equal(r.rec.tAct, 40); assert.equal(r.clamped, true);
  r = B.dksSetDepth(rec, "tAct", 339);
  assert.equal(r.rec.tAct, 310 - 40); assert.equal(r.clamped, true);
  r = B.dksSetDepth(rec, "tBot", 100);
  assert.equal(r.rec.tBot, 160, "not closer than 0.40 mm to the start point");
  r = B.dksSetDepth(rec, "tBot", 999);
  assert.equal(r.rec.tBot, 340);
  r = B.dksSetDepth({ ...rec, tBot: 0 }, "tAct", 120);
  assert.equal(r.rec.tBot, 310, "a bottom point of 0 (default) becomes the explicit 3.10 mm");
  assert.equal(rec.tAct, 120, "the input record is not changed");
  assert.throws(() => B.dksSetDepth(rec, "flags", 1), RangeError);
  // every value anyone can ask for ends as a record the codec rule accepts
  for (let v = -50; v <= 400; v += 7) for (const f of ["tAct", "tBot"]) {
    const out = B.dksSetDepth(B.newDksRecord(), f, v).rec;
    assert.equal(B.behaviorIssues({ m6: { dks: [out] }, keys: {} }).length, 0, `${f}=${v}`);
  }
});

test("dksSetUsage / dksSetEvent: an unused action has no moments, and a moment needs a key", () => {
  let rec = B.newDksRecord();
  assert.equal(B.dksSetEvent(rec, 2, 0, B.DKS_EV.PRESS), null, "action 3 has no key yet");
  assert.deepEqual(B.dksSetEvent(rec, 2, 0, B.DKS_EV.NONE).acts[2], [0, 0]);
  rec = B.dksSetUsage(rec, 2, 0x1a);
  assert.deepEqual(rec.acts[2], [0x1a, 0]);
  rec = B.dksSetEvent(rec, 2, 1, B.DKS_EV.TAP);
  assert.deepEqual(B.dksEventsUnpack(rec.acts[2][1]), [0, 3, 0, 0]);
  rec = B.dksSetUsage(rec, 2, 0x1b);
  assert.deepEqual(B.dksEventsUnpack(rec.acts[2][1]), [0, 3, 0, 0], "changing the key keeps the moments");
  rec = B.dksSetUsage(rec, 2, 0);
  assert.deepEqual(rec.acts[2], [0, 0], "removing the key clears its moments (the codec refuses moments without a key)");
  assert.equal(B.behaviorIssues({ m6: { dks: [rec] }, keys: {} }).length, 0);
});

test("dksNotes: remarks about records that are valid but pointless", () => {
  assert.deepEqual(B.dksNotes(B.newDksRecord()), []);
  const empty = { tAct: 120, tBot: 310, flags: 0, acts: [[0, 0], [0, 0], [0, 0], [0, 0]] };
  assert.deepEqual(B.dksNotes(empty), [{ note: "no_actions" }]);
  const idle = B.dksSetUsage(B.newDksRecord(), 2, 0x06);
  assert.deepEqual(B.dksNotes(idle), [{ note: "idle", action: 2 }]);
});

test("DKS records: add up to 8, the 9th is refused, a bad record is refused and changes nothing", () => {
  const p = fresh();
  for (let i = 0; i < FW_M6.dksMax; i++) assert.deepEqual(B.addRecord(p, "dks", B.newDksRecord()), { ok: true, index: i });
  assert.deepEqual(B.addRecord(p, "dks", B.newDksRecord()), { ok: false, reason: "full" });
  assert.equal(p.m6.dks.length, 8);
  assert.equal(B.setRecord(p, "dks", 0, { ...B.newDksRecord(), tAct: 10 }).reason, "depth");
  assert.equal(B.setRecord(p, "dks", 0, { ...B.newDksRecord(), flags: 2 }).reason, "flags");
  assert.equal(B.setRecord(p, "dks", 0, { ...B.newDksRecord(), acts: [[3, 0], [0, 0], [0, 0], [0, 0]] }).reason, "action");
  assert.equal(B.setRecord(p, "dks", 0, { ...B.newDksRecord(), acts: [[0, 1], [0, 0], [0, 0], [0, 0]] }).reason, "action", "moments without a key");
  assert.equal(B.setRecord(p, "dks", 0, null).reason, "shape");
  assert.equal(B.setRecord(p, "dks", 8, B.newDksRecord()).reason, "index");
  assert.deepEqual(p.m6.dks[0], B.newDksRecord(), "nothing changed");
  assert.equal(roundTrip(p).profile.m6.dks.length, 8);
});

test("the records are copied: changing the record you passed in does not change the profile", () => {
  const p = fresh();
  const rec = B.newDksRecord();
  B.addRecord(p, "dks", rec);
  rec.acts[0][0] = 9;
  assert.equal(p.m6.dks[0].acts[0][0], 4);
});

test("assigning a DKS to keys, then removing a DKS in the middle renumbers the keys that use later ones and resets the ones that use it", () => {
  const p = fresh();
  for (let i = 0; i < 4; i++) B.addRecord(p, "dks", { ...B.newDksRecord(), tAct: 100 + i * 10 });
  B.assignRecord(p, layout, "dks", 0, [{ keyId: "KeyQ", layer: 0 }]);
  B.assignRecord(p, layout, "dks", 1, [{ keyId: "KeyW", layer: 0 }, { keyId: "KeyE", layer: 1 }]);
  B.assignRecord(p, layout, "dks", 2, [{ keyId: "KeyR", layer: 0 }]);
  B.assignRecord(p, layout, "dks", 3, [{ keyId: "KeyT", layer: 2 }]);
  assert.deepEqual(B.recordUsage(p, "dks"), [1, 2, 1, 1]);
  const r = B.removeRecord(p, layout, "dks", 1);
  assert.equal(r.ok, true);
  assert.deepEqual(r.reset, [{ keyId: "KeyW", layer: 0 }, { keyId: "KeyE", layer: 1 }]);
  assert.deepEqual(p.m6.dks.map((d) => d.tAct), [100, 120, 130]);
  assert.deepEqual(p.keys.KeyW.bindings[0], { type: "key", usage: 26 }, "back to the printed key on the base layer");
  assert.equal(p.keys.KeyE.bindings[1], null, "back to transparent on an overlay layer");
  assert.deepEqual(B.behaviorOf(p.keys.KeyQ.bindings[0]), { kind: "dks", index: 0 });
  assert.deepEqual(B.behaviorOf(p.keys.KeyR.bindings[0]), { kind: "dks", index: 1 });
  assert.deepEqual(B.behaviorOf(p.keys.KeyT.bindings[2]), { kind: "dks", index: 2 });
  assert.deepEqual(B.behaviorIssues(p), []);
  const rt = roundTrip(p);   // the strict "the key points at a record that exists" rule of the codec
  sameBehaviorData(p, rt.profile);
});

test("removing the last record leaves a profile without M6 data, which saves as format 2 again", () => {
  const p = fresh();
  B.addRecord(p, "dks", B.newDksRecord());
  B.assignRecord(p, layout, "dks", 0, [{ keyId: "KeyQ", layer: 0 }]);
  assert.equal(save(p).bytes[4], 3);
  B.removeRecord(p, layout, "dks", 0);
  assert.equal(p.m6, undefined);
  const r = save(p);
  assert.equal(r.bytes[4], 2, "nothing needs format 3 any more: an older firmware can still read it");
  assert.equal(hex(r.bytes), hex(workspaceToBlob(defaultWorkspace(layout), layout).bytes), "identical to the untouched default");
  assert.equal(B.removeRecord(p, layout, "dks", 0).reason, "index");
});

test("assignRecord: bad record index and unknown keys are skipped, not written", () => {
  const p = fresh();
  assert.deepEqual(B.assignRecord(p, layout, "dks", 0, [{ keyId: "KeyQ", layer: 0 }]).skipped, [{ keyId: "KeyQ", layer: 0, why: "record" }]);
  B.addRecord(p, "dks", B.newDksRecord());
  const r = B.assignRecord(p, layout, "dks", 0, [{ keyId: "Nope", layer: 0 }, { keyId: "KeyQ", layer: 3 }]);
  assert.deepEqual(r.skipped, [{ keyId: "Nope", layer: 0, why: "key" }]);
  assert.deepEqual(r.done, [{ keyId: "KeyQ", layer: 3 }]);
  assert.deepEqual(B.behaviorOf(p.keys.KeyQ.bindings[3]), { kind: "dks", index: 0 });
});

// ------------------------------------------------------------------------------------------------ Mod-Morph

test("Mod-Morph: modifier bits are the usage 0xE0 + n, left and right apart", () => {
  const rec = { mask: 0x22, usage: 76, flags: 0 };
  assert.equal(B.morphHasMod(rec, 0xe1), true);
  assert.equal(B.morphHasMod(rec, 0xe5), true);
  assert.equal(B.morphHasMod(rec, 0xe0), false);
  assert.equal(B.morphToggleMod(rec, 0xe5).mask, 0x02);
  assert.equal(B.morphToggleMod(rec, 0xe0).mask, 0x23);
  assert.equal(B.morphToggleMod(B.morphToggleMod(rec, 0xe3), 0xe3).mask, 0x22);
  assert.equal(B.newMorphRecord().mask, 0x22, "either Shift");
  for (const [name, mask] of Object.entries(B.MORPH_BOTH)) assert.equal(mask & 0x0f, mask >> 4, name);
});

test("Mod-Morph records: validity, the 9th is refused, mask 0 and a non-key output are refused", () => {
  const p = fresh();
  for (let i = 0; i < FW_M6.morphMax; i++) assert.equal(B.addRecord(p, "morph", B.newMorphRecord()).index, i);
  assert.equal(B.addRecord(p, "morph", B.newMorphRecord()).reason, "full");
  assert.equal(B.setRecord(p, "morph", 0, { mask: 0, usage: 76, flags: 0 }).reason, "mask");
  assert.equal(B.setRecord(p, "morph", 0, { mask: 2, usage: 3, flags: 0 }).reason, "usage");
  assert.equal(B.setRecord(p, "morph", 0, { mask: 2, usage: 0xe8, flags: 0 }).reason, "usage");
  assert.equal(B.setRecord(p, "morph", 0, { mask: 2, usage: 76, flags: 2 }).reason, "flags");
  assert.equal(B.setRecord(p, "morph", 0, { mask: 256, usage: 76, flags: 0 }).reason, "shape");
  assert.deepEqual(p.m6.morph[0], B.newMorphRecord());
  sameBehaviorData(p, roundTrip(p).profile);
});

test("morphNormalUsage: the key's own plain key, a morph it already is, else its printed key; nothing for a key that sends no usage", () => {
  const p = fresh();
  assert.equal(B.morphNormalUsage(p, layout, "Backspace", 0), 42);
  assert.equal(B.morphNormalUsage(p, layout, "Backspace", 1), 76, "layer 1 Backspace is Delete in the default table");
  assert.equal(B.morphNormalUsage(p, layout, "Fn", 0), null, "Fn is the layer key: no usage to send");
  p.keys.KeyQ.bindings[2] = { type: "media", usage: 0xcd };
  assert.equal(B.morphNormalUsage(p, layout, "KeyQ", 2), 20, "a media key is no 'ordinary key': falls back to the printed key");
  p.keys.KeyQ.bindings[0] = B.morphBinding(0x2a, 0);
  assert.equal(B.morphNormalUsage(p, layout, "KeyQ", 0), 0x2a);
  p.keys.KeyW.bindings[0] = { type: "key", usage: 0x04, mods: [225] };
  assert.equal(B.morphNormalUsage(p, layout, "KeyW", 0), 26, "a key with modifiers is not a plain key");
});

test("assigning Mod-Morph: arg0 = the normal key, arg1 = the record; keys without a normal key are skipped and reported", () => {
  const p = fresh();
  B.addRecord(p, "morph", B.newMorphRecord());
  const r = B.assignRecord(p, layout, "morph", 0, [{ keyId: "Backspace", layer: 0 }, { keyId: "Fn", layer: 0 }]);
  assert.deepEqual(r.done, [{ keyId: "Backspace", layer: 0 }]);
  assert.deepEqual(r.skipped, [{ keyId: "Fn", layer: 0, why: "normal" }]);
  assert.deepEqual(B.behaviorOf(p.keys.Backspace.bindings[0]), { kind: "morph", usage: 42, index: 0 });
  assert.deepEqual(p.keys.Fn.bindings[0], { type: "layer", layer: 1 }, "the skipped key is untouched");
  const rt = roundTrip(p);
  sameBehaviorData(p, rt.profile);
  const raw = decodeSettings(rt.bytes);
  assert.equal(raw.profiles[0].layers[0][slotOf("Backspace")], CODE.MOD_MORPH);
  assert.deepEqual(raw.profiles[0].args[0][slotOf("Backspace")], [42, 0]);
});

test("deleting a Mod-Morph renumbers the second argument of the keys that use later records", () => {
  const p = fresh();
  for (let i = 0; i < 3; i++) B.addRecord(p, "morph", { mask: 0x22, usage: 76 + i, flags: 0 });
  B.assignRecord(p, layout, "morph", 0, [{ keyId: "KeyQ", layer: 0 }]);
  B.assignRecord(p, layout, "morph", 1, [{ keyId: "KeyW", layer: 0 }]);
  B.assignRecord(p, layout, "morph", 2, [{ keyId: "KeyE", layer: 0 }]);
  const r = B.removeRecord(p, layout, "morph", 0);
  assert.deepEqual(r.reset, [{ keyId: "KeyQ", layer: 0 }]);
  assert.deepEqual(B.behaviorOf(p.keys.KeyW.bindings[0]), { kind: "morph", usage: 26, index: 0 }, "usage (arg0) stays, record (arg1) moves down");
  assert.deepEqual(B.behaviorOf(p.keys.KeyE.bindings[0]), { kind: "morph", usage: 8, index: 1 });
  assert.deepEqual(p.m6.morph.map((m) => m.usage), [77, 78]);
  sameBehaviorData(p, roundTrip(p).profile);
});

test("the Mod-Morph examples are valid records for keys that exist", () => {
  for (const ex of B.MORPH_PRESETS) {
    const p = fresh();
    assert.ok(layout.byId.has(ex.keyId), ex.id);
    assert.deepEqual(B.addRecord(p, "morph", ex.rec), { ok: true, index: 0 });
    assert.equal(B.assignRecord(p, layout, "morph", 0, [{ keyId: ex.keyId, layer: 0 }]).done.length, 1);
    sameBehaviorData(p, roundTrip(p).profile);
  }
  assert.deepEqual(B.MORPH_PRESETS.map((e) => [e.rec.usage, e.rec.mask]), [[76, 0x22], [53, 0x88]]);
});

// ------------------------------------------------------------------------------------------------ toggle keys

test("toggle keys: 'self' uses each key's own key, a chosen usage is used for all; entries are valid table entries", () => {
  const p = fresh();
  const r = B.assignToggle(p, layout, "self", [{ keyId: "ShiftLeft", layer: 0 }, { keyId: "KeyW", layer: 0 }, { keyId: "Fn", layer: 0 }]);
  assert.deepEqual(r.done.map((d) => d.keyId), ["ShiftLeft", "KeyW"]);
  assert.deepEqual(r.skipped, [{ keyId: "Fn", layer: 0, why: "normal" }]);
  assert.deepEqual(B.behaviorOf(p.keys.ShiftLeft.bindings[0]), { kind: "toggle", usage: 225 }, "a modifier can be toggled");
  assert.deepEqual(B.behaviorOf(p.keys.KeyW.bindings[0]), { kind: "toggle", usage: 26 });
  B.assignToggle(p, layout, 0x06, [{ keyId: "KeyE", layer: 2 }]);
  assert.deepEqual(B.behaviorOf(p.keys.KeyE.bindings[2]), { kind: "toggle", usage: 6 });
  assert.deepEqual(B.toggleKeysOn(p, 0), [{ keyId: "KeyW", usage: 26 }, { keyId: "ShiftLeft", usage: 225 }].sort((a, b) => layoutIndex(a.keyId) - layoutIndex(b.keyId)));
  assert.deepEqual(B.toggleKeysOn(p, 2), [{ keyId: "KeyE", usage: 6 }]);
  assert.deepEqual(B.toggleKeysOn(p, 1), []);
  sameBehaviorData(p, roundTrip(p).profile);
});
const layoutIndex = (id) => layout.keys.findIndex((k) => k.id === id);

test("toggle keys refuse what the codec refuses: no usage, 0xE8 and up, non-integers", () => {
  const p = fresh();
  for (const bad of [0, 3, 0xe8, 0xff, -1, 4.5, "x", null, undefined]) {
    const r = B.assignToggle(p, layout, bad, [{ keyId: "KeyQ", layer: 0 }]);
    assert.equal(r.done.length, 0, String(bad));
    assert.equal(r.skipped.length, 1, String(bad));
  }
  assert.deepEqual(p.keys.KeyQ.bindings[0], { type: "key", usage: 20 });
  assert.deepEqual(B.assignToggle(p, layout, 0xe7, [{ keyId: "KeyQ", layer: 0 }]).done.length, 1, "0xE7 (Right Win) is the last usage");
});

test("clearBehavior frees DKS, Mod-Morph and toggle keys, leaves every other entry alone", () => {
  const p = fresh();
  B.addRecord(p, "dks", B.newDksRecord());
  B.assignRecord(p, layout, "dks", 0, [{ keyId: "KeyQ", layer: 0 }]);
  B.assignToggle(p, layout, 0x04, [{ keyId: "KeyW", layer: 0 }]);
  p.keys.KeyE.bindings[0] = { type: "media", usage: 0xcd };
  const done = B.clearBehavior(p, layout, [{ keyId: "KeyQ", layer: 0 }, { keyId: "KeyW", layer: 0 }, { keyId: "KeyE", layer: 0 }, { keyId: "KeyR", layer: 0 }]);
  assert.deepEqual(done, [{ keyId: "KeyQ", layer: 0 }, { keyId: "KeyW", layer: 0 }]);
  assert.deepEqual(p.keys.KeyE.bindings[0], { type: "media", usage: 0xcd });
  assert.deepEqual(p.keys.KeyQ.bindings[0], { type: "key", usage: 20 });
  assert.equal(p.m6.dks.length, 1, "the record itself stays, only the keys are freed");
});

// ------------------------------------------------------------------------------------------------ SOCD

test("SOCD: a new pair is OFF; modes are the firmware's numbers 0..4; the notice mentions both limits", () => {
  assert.deepEqual(B.SOCD_MODES, ["off", "last", "neutral", "first", "deeper"]);
  const p = fresh();
  const r = B.addSocdPair(p, { a: slotOf("KeyA"), b: slotOf("KeyD") });
  assert.equal(r.ok, true);
  assert.equal(p.m6.socd[0].mode, 0, "default off");
  assert.equal(B.socdActive(p), 0);
  assert.equal(B.setSocdPair(p, 0, { mode: 1 }).ok, true);
  assert.equal(B.socdActive(p), 1);
  for (const [lang, min] of [[en6, 1], [zh6, 0.35]]) {   // Chinese needs far fewer characters for the same sentence
    assert.ok(lang["beh.socd.warn.title"].length > 10 * min && lang["beh.socd.warn.body"].length > 100 * min);
    assert.ok(lang["beh.socd.confirm.body"].length > 50 * min);
  }
  assert.match(en6["beh.socd.warn.title"], /forbid/);
  assert.match(en6["beh.socd.warn.body"], /off by default/);
  assert.match(zh6["beh.socd.warn.body"], /預設是關閉/);
});

test("SOCD pairs: 4 at most, a key in one pair only, two different keys that exist, valid mode and flags", () => {
  const p = fresh();
  const ids = [["KeyA", "KeyD"], ["KeyW", "KeyS"], ["ArrowLeft", "ArrowRight"], ["ArrowUp", "ArrowDown"]].map((pr) => pr.map((x) => (layout.byId.has(x) ? slotOf(x) : null)));
  // the layout is a 60% board: there are no arrow keys, use other letters for pairs 3 and 4
  ids[2] = [slotOf("KeyJ"), slotOf("KeyL")];
  ids[3] = [slotOf("KeyI"), slotOf("KeyK")];
  ids.forEach(([a, b], i) => assert.deepEqual(B.addSocdPair(p, { a, b, mode: 2 }), { ok: true, index: i }));
  assert.equal(B.addSocdPair(p, { a: slotOf("KeyQ"), b: slotOf("KeyE") }).reason, "full");
  assert.equal(p.m6.socd.length, FW_M6.socdMax);
  assert.equal(B.setSocdPair(p, 1, { a: slotOf("KeyA") }).reason, "overlap");
  assert.equal(B.setSocdPair(p, 1, { b: slotOf("KeyW") }).reason, "same");
  assert.equal(B.setSocdPair(p, 1, { mode: 5 }).reason, "mode");
  assert.equal(B.setSocdPair(p, 1, { flags: 2 }).reason, "flags");
  assert.equal(B.setSocdPair(p, 1, { a: FW_NO_KEY_SLOTS[0] }).reason, "slots", "a slot without a key");
  assert.equal(B.setSocdPair(p, 1, { b: 76 }).reason, "slots");
  assert.equal(B.setSocdPair(p, 9, { mode: 1 }).reason, "index");
  assert.deepEqual(p.m6.socd[1], { a: slotOf("KeyW"), b: slotOf("KeyS"), mode: 2, flags: 0 }, "refused changes change nothing");
  assert.equal(B.setSocdPair(p, 1, { a: slotOf("KeyS"), b: slotOf("KeyW") }).ok, true, "swapping the two keys of one pair is fine");
  assert.equal(B.setSocdPair(p, 0, { flags: B.SOCD_FLAG_LAYER0, mode: 4 }).ok, true);
  sameBehaviorData(p, roundTrip(p).profile);
  assert.equal(B.removeSocdPair(p, 2).ok, true);
  assert.equal(p.m6.socd.length, 3);
  assert.equal(B.removeSocdPair(p, 3).reason, "index");
  assert.equal(B.addSocdPair(p, { a: slotOf("KeyJ"), b: slotOf("KeyL") }).ok, true, "the keys of a removed pair are free again");
});

test("SOCD: a pair that is switched off still owns its keys (the codec counts every pair), and removing the last pair gives format 2 back", () => {
  const p = fresh();
  B.addSocdPair(p, { a: slotOf("KeyA"), b: slotOf("KeyD"), mode: 0 });
  assert.equal(B.addSocdPair(p, { a: slotOf("KeyD"), b: slotOf("KeyF") }).reason, "overlap");
  assert.equal(save(p).bytes[4], 3);
  B.removeSocdPair(p, 0);
  assert.equal(p.m6, undefined);
  assert.equal(save(p).bytes[4], 2);
});

test("nextFreePair offers A/D, then W/S, then other free keys, and never a key already in a pair", () => {
  const p = fresh();
  const seen = [];
  for (let i = 0; i < 4; i++) {
    const pair = B.nextFreePair(p, layout);
    assert.ok(pair);
    assert.equal(B.addSocdPair(p, { a: pair[0], b: pair[1] }).ok, true);
    seen.push(pair.map((s) => layout.bySlot.get(s).id));
  }
  assert.deepEqual(seen.slice(0, 2), [["KeyA", "KeyD"], ["KeyW", "KeyS"]]);
  assert.equal(new Set(seen.flat()).size, 8);
  // all keys but one taken: nothing to offer
  const q = fresh();
  const keys = layout.keys.map((k) => k.slot);
  q.m6 = { socd: [] };
  for (let i = 0; i + 1 < keys.length - 1 && q.m6.socd.length < 4; i += 2) q.m6.socd.push({ a: keys[i], b: keys[i + 1], mode: 0, flags: 0 });
  assert.ok(B.nextFreePair(q, layout), "keys left for a fifth pair; the caller checks the limit");
  const crowded = { keys: {}, m6: { socd: [] } };
  const free = layout.keys.map((k) => k.slot);
  for (let i = 0; i < free.length - 1; i += 2) crowded.m6.socd.push({ a: free[i], b: free[i + 1], mode: 0, flags: 0 });
  assert.equal(B.nextFreePair(crowded, layout), null, "one key left is not a pair");
});

test("SOCD presets name keys that exist on this layout", () => {
  for (const pr of B.SOCD_PRESETS) for (const pair of pr.pairs) for (const id of pair) {
    if (pr.id === "arrows") continue;   // a 60% board has no arrow keys: the page then adds nothing and says so
    assert.ok(layout.byId.has(id), `${pr.id} ${id}`);
  }
  const p = fresh();
  const wasd = B.SOCD_PRESETS.find((x) => x.id === "wasd");
  for (const [a, b] of wasd.pairs) assert.equal(B.addSocdPair(p, { a: slotOf(a), b: slotOf(b) }).ok, true);
  assert.equal(p.m6.socd.length, 2);
  assert.ok(B.SOCD_PRESETS.some((x) => x.id === "arrows"));
});

// ------------------------------------------------------------------------------------------------ the codec as the judge

test("everything at once: all kinds, all layers, saved in the 4-profile blob, read back identical, and format 3 is chosen", () => {
  const p = fresh();
  for (let i = 0; i < 8; i++) {
    B.addRecord(p, "dks", { tAct: 60 + i * 5, tBot: 200 + i * 10, flags: i & 1, acts: [[4 + i, 129], [0x10 + i, 0b00_10_01_00], [0, 0], [0x2c, B.dksEventsPack([3, 3, 3, 3])]] });
    B.addRecord(p, "morph", { mask: 1 << i, usage: 0x04 + i, flags: i & 1 });
  }
  const letters = ["KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI"];
  letters.forEach((id, i) => {
    B.assignRecord(p, layout, "dks", i, [{ keyId: id, layer: 0 }]);
    B.assignRecord(p, layout, "morph", i, [{ keyId: id, layer: 1 }]);
    B.assignToggle(p, layout, 0x04 + i, [{ keyId: id, layer: 2 }]);
  });
  [["KeyA", "KeyD", 1], ["KeyF", "KeyG", 2], ["KeyH", "KeyJ", 3], ["KeyK", "KeyL", 4]].forEach(([a, b, mode]) => B.addSocdPair(p, { a: slotOf(a), b: slotOf(b), mode, flags: mode & 1 }));
  assert.deepEqual(B.behaviorIssues(p), []);
  assert.equal(B.codecProblem(p, layout), null);
  const rt = roundTrip(p);
  assert.equal(rt.bytes[4], 3);
  sameBehaviorData(p, rt.profile);
  assert.ok(rt.bytes.length < FW_SETTINGS.v3MaxSize);
  // the same profile in all four slots is the largest case this page can produce
  const big = workspaceToBlob({ active: 0, profiles: [p, p, p, p] }, layout);
  assert.ok(big.bytes && big.bytes.length <= FW_SETTINGS.v3MaxEncoded, `${big.bytes?.length} bytes`);
  const ws = blobToWorkspace(big.bytes, layout);
  ws.profiles.forEach((q) => sameBehaviorData(p, q));
  // what Save then reload shows is what dirty checks compare: normalising twice changes nothing
  const once = normalizeBlob(big.bytes, layout);
  assert.equal(hex(normalizeBlob(once, layout)), hex(once));
});

test("the real firmware's biggest blob (v3_everything, written by the C code): the helpers read it, edit it, and it still saves", () => {
  const blob = bytes(V.v3.cases.find((c) => c.name === "v3_everything").encoded);
  const ws = blobToWorkspace(blob, layout);
  const p = ws.profiles[0];
  assert.equal(B.recordsOf(p, "dks").length, 8);
  assert.equal(B.recordsOf(p, "morph").length, 8);
  assert.equal(B.recordsOf(p, "socd").length, 4);
  assert.deepEqual(B.behaviorIssues(p), []);
  assert.equal(B.codecProblem(p, layout), null);
  const users = B.recordUsage(p, "dks");
  assert.equal(users[7], 1, "slot 0 / layer 0 points at DKS 8 (args 7,0)");
  assert.deepEqual(B.behaviorOf(p.keys[layout.bySlot.get(0).id].bindings[0]), { kind: "dks", index: 7 });
  // delete DKS 1: the key that used #8 now uses #7, and the profile still goes through the strict decoder
  B.removeRecord(p, layout, "dks", 0);
  assert.deepEqual(B.behaviorOf(p.keys[layout.bySlot.get(0).id].bindings[0]), { kind: "dks", index: 6 });
  const r = workspaceToBlob(ws, layout);
  assert.deepEqual(r.problems, []);
  sameBehaviorData(p, blobToWorkspace(r.bytes, layout).profiles[0]);
  // delete the Mod-Morph that layer 1 / slot 3 uses (record 8): that key goes back to transparent
  const k3 = layout.bySlot.get(3);
  assert.equal(B.behaviorOf(p.keys[k3.id].bindings[1])?.index, 7);
  B.removeRecord(p, layout, "morph", 7);
  assert.equal(p.keys[k3.id].bindings[1], null);
  assert.equal(B.codecProblem(p, layout), null);
});

/** Save, reload, save again: the same records, and the bytes settle after one pass (an overlay entry equal to the base layer's comes back as 'transparent', which does the same). */
const stable = (p) => {
  const first = save(p);
  assert.ok(first.bytes, JSON.stringify(first.problems));
  const once = normalizeBlob(first.bytes, layout);
  assert.equal(hex(normalizeBlob(once, layout)), hex(once), "reading and writing again changes nothing more");
  assert.deepEqual(blobToWorkspace(first.bytes, layout).profiles[0].m6 ?? {}, p.m6 ?? {});
};

test("a random walk over all operations never leaves the profile in a state the codec refuses, and always reads back the same", () => {
  let seed = 12345;
  const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return (seed >>> 8) % n; };
  const p = fresh();
  const ids = layout.keys.map((k) => k.id);
  let ops = 0;
  for (let step = 0; step < 400; step++) {
    const kind = ["dks", "morph"][rnd(2)];
    switch (rnd(9)) {
      case 0: B.addRecord(p, kind, kind === "dks" ? { ...B.newDksRecord(), tAct: 40 + rnd(100) } : { mask: 1 + rnd(255), usage: 4 + rnd(200), flags: rnd(2) }); break;
      case 1: B.removeRecord(p, layout, kind, rnd(9)); break;
      case 2: B.assignRecord(p, layout, kind, rnd(9), [{ keyId: ids[rnd(ids.length)], layer: rnd(4) }]); break;
      case 3: B.assignToggle(p, layout, rnd(2) ? "self" : 4 + rnd(200), [{ keyId: ids[rnd(ids.length)], layer: rnd(4) }]); break;
      case 4: B.clearBehavior(p, layout, [{ keyId: ids[rnd(ids.length)], layer: rnd(4) }]); break;
      case 5: { const s = B.nextFreePair(p, layout); if (s) B.addSocdPair(p, { a: s[0], b: s[1], mode: rnd(5) }); break; }
      case 6: B.setSocdPair(p, rnd(5), { mode: rnd(6), flags: rnd(3) }); break;
      case 7: B.removeSocdPair(p, rnd(5)); break;
      default: if (p.m6?.dks?.length) B.setRecord(p, "dks", rnd(8), B.dksSetDepth(p.m6.dks[0], "tBot", rnd(400)).rec);
    }
    ops++;
    assert.deepEqual(B.behaviorIssues(p), [], `step ${step}`);
    if (step % 20 === 0) {
      assert.equal(B.codecProblem(p, layout), null, `step ${step}`);
      stable(p);
    }
  }
  assert.equal(ops, 400);
  stable(p);
});

// ------------------------------------------------------------------------------------------------ errors in plain language

test("behaviorIssues finds every rule the codec has, for data that came from elsewhere (an import, a damaged keyboard copy)", () => {
  const bad = (m6, keys = {}) => B.behaviorIssues({ m6, keys });
  const dks = B.newDksRecord();
  assert.deepEqual(bad({ dks: [{ ...dks, tAct: 10 }] }), [{ area: "dks", index: 0, why: "depth" }]);
  assert.deepEqual(bad({ dks: [{ ...dks, tBot: 400 }] }), [{ area: "dks", index: 0, why: "depth" }]);
  assert.deepEqual(bad({ dks: [{ ...dks, acts: [[3, 0], [0, 0], [0, 0], [0, 0]] }] }), [{ area: "dks", index: 0, why: "action" }]);
  assert.deepEqual(bad({ dks: [{ ...dks, flags: 4 }] }), [{ area: "dks", index: 0, why: "flags" }]);
  assert.deepEqual(bad({ dks: [{ ...dks, acts: [] }] }), [{ area: "dks", index: 0, why: "shape" }]);
  assert.deepEqual(bad({ dks: Array.from({ length: 9 }, () => dks) }), [{ area: "dks", index: -1, why: "count" }]);
  assert.deepEqual(bad({ morph: [{ mask: 0, usage: 76, flags: 0 }] }), [{ area: "morph", index: 0, why: "mask" }]);
  assert.deepEqual(bad({ morph: [{ mask: 1, usage: 2, flags: 0 }] }), [{ area: "morph", index: 0, why: "usage" }]);
  assert.deepEqual(bad({ morph: [{ mask: 1, usage: 76, flags: 9 }] }), [{ area: "morph", index: 0, why: "flags" }]);
  assert.deepEqual(bad({ socd: [{ a: 1, b: 1, mode: 1, flags: 0 }] }), [{ area: "socd", index: 0, why: "slots" }]);
  assert.deepEqual(bad({ socd: [{ a: 1, b: 2, mode: 9, flags: 0 }] }), [{ area: "socd", index: 0, why: "mode" }]);
  assert.deepEqual(bad({ socd: [{ a: 1, b: 2, mode: 1, flags: 0 }, { a: 2, b: 3, mode: 1, flags: 0 }] }), [{ area: "socd", index: 1, why: "overlap" }]);
  assert.deepEqual(bad({ socd: Array.from({ length: 5 }, (_, i) => ({ a: i * 2, b: i * 2 + 1, mode: 0, flags: 0 })) })[0], { area: "socd", index: -1, why: "count" });
  const refs = bad({}, { KeyQ: { bindings: [B.dksBinding(0), B.morphBinding(4, 2), null, null] } });
  assert.deepEqual(refs.map((x) => [x.why, x.kind, x.keyId, x.layer]), [["reference", "dks", "KeyQ", 0], ["reference", "morph", "KeyQ", 1]]);
  assert.deepEqual(bad({}, { KeyQ: { bindings: [B.toggleKeyBinding(0xe8), null, null, null] } }).map((x) => x.why), ["args"]);
  assert.deepEqual(B.behaviorIssues({ keys: {} }), [], "a profile without M6 data has no issues");
});

test("codecProblem: a profile the codec refuses comes back as a SettingsError with the reason, a good one as null", () => {
  const p = fresh();
  assert.equal(B.codecProblem(p, layout), null);
  p.keys.KeyQ.bindings[0] = B.dksBinding(0);   // no DKS record 0
  let err = B.codecProblem(p, layout);
  assert.ok(err instanceof SettingsError);
  assert.equal(err.code, "usage");
  assert.equal(B.explainSettingsError(err).key, "beh.err.reference");
  const q = fresh();
  q.m6 = { dks: [{ ...B.newDksRecord(), tAct: 5 }] };
  err = B.codecProblem(q, layout);
  assert.equal(err.code, "record");
  assert.equal(B.explainSettingsError(err).key, "beh.err.dks.depth");
  assert.deepEqual(B.explainSettingsError(err).params, { profile: 1, n: 1 });
  const r = fresh();
  r.keys.KeyQ.bindings[0] = { type: "mouse", button: 1 };
  err = B.codecProblem(r, layout);
  assert.ok(err instanceof SettingsError, "a table problem of another page is wrapped the same way");
  assert.equal(B.explainSettingsError(err).key, "beh.err.entry");
  // the profile passed in is not changed by checking it
  assert.equal(q.m6.dks[0].tAct, 5);
});

test("every refusal of the settings codec for DKS / Mod-Morph / SOCD / table references has a plain-language text of its own", () => {
  const cases = [
    ["dks.depth", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), tAct: 39 }]; }],
    ["dks.depth", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), tBot: 341 }]; }],
    ["dks.depth", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), tAct: 100, tBot: 139 }]; }],
    ["dks.depth", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), tAct: 280, tBot: 0 }]; }],
    ["dks.action", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), acts: [[3, 0], [0, 0], [0, 0], [0, 0]] }]; }],
    ["dks.action", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), acts: [[0xe8, 0], [0, 0], [0, 0], [0, 0]] }]; }],
    ["dks.action", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), acts: [[0, 1], [0, 0], [0, 0], [0, 0]] }]; }],
    ["dks.flags", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), flags: 2 }]; }],
    ["dks.shape", (r) => { r.profiles[0].dks = [{ ...B.newDksRecord(), acts: [] }]; }],
    ["morph.mask", (r) => { r.profiles[1].morph = [{ mask: 0, usage: 76, flags: 0 }]; }],
    ["morph.usage", (r) => { r.profiles[1].morph = [{ mask: 1, usage: 3, flags: 0 }]; }],
    ["morph.usage", (r) => { r.profiles[1].morph = [{ mask: 1, usage: 0xe8, flags: 0 }]; }],
    ["morph.flags", (r) => { r.profiles[1].morph = [{ mask: 1, usage: 76, flags: 2 }]; }],
    ["morph.shape", (r) => { r.profiles[1].morph = [{ mask: 300, usage: 76, flags: 0 }]; }],
    ["socd.slots", (r) => { r.profiles[2].socd = [{ a: 3, b: 3, mode: 1, flags: 0 }]; }],
    ["socd.slots", (r) => { r.profiles[2].socd = [{ a: FW_NO_KEY_SLOTS[0], b: 3, mode: 1, flags: 0 }]; }],
    ["socd.slots", (r) => { r.profiles[2].socd = [{ a: 76, b: 3, mode: 1, flags: 0 }]; }],
    ["socd.shape", (r) => { r.profiles[2].socd = [{ a: 300, b: 3, mode: 1, flags: 0 }]; }],
    ["socd.mode", (r) => { r.profiles[2].socd = [{ a: 1, b: 3, mode: 5, flags: 0 }]; }],
    ["socd.flags", (r) => { r.profiles[2].socd = [{ a: 1, b: 3, mode: 1, flags: 2 }]; }],
    ["socd.overlap", (r) => { r.profiles[2].socd = [{ a: 1, b: 3, mode: 1, flags: 0 }, { a: 3, b: 4, mode: 1, flags: 0 }]; }],
    ["count", (r) => { r.profiles[3].dks = Array.from({ length: 9 }, () => B.newDksRecord()); }],
    ["count", (r) => { r.profiles[3].morph = Array.from({ length: 9 }, () => B.newMorphRecord()); }],
    ["count", (r) => { r.profiles[3].socd = Array.from({ length: 5 }, (_, i) => ({ a: 2 * i, b: 2 * i + 1, mode: 0, flags: 0 })); }],
    ["reference", (r) => { r.profiles[0].layers[0][0] = CODE.DKS; r.profiles[0].args[0][0] = [0, 0]; }],
    ["reference", (r) => { r.profiles[0].dks = [B.newDksRecord()]; r.profiles[0].layers[2][5] = CODE.DKS; r.profiles[0].args[2][5] = [1, 0]; }],
    ["reference", (r) => { r.profiles[0].layers[0][0] = CODE.MOD_MORPH; r.profiles[0].args[0][0] = [4, 0]; }],
    ["entry", (r) => { r.profiles[0].layers[0][0] = CODE.TOGGLE_KEY; r.profiles[0].args[0][0] = [0xe8, 0]; }],
    ["entry", (r) => { r.profiles[0].layers[0][0] = CODE.DKS; r.profiles[0].args[0][0] = [8, 0]; }],
  ];
  const seen = new Set();
  for (const [want, make] of cases) {
    const raw = defaultRawSettings();
    make(raw);
    const err = rawSettingsProblem(raw);
    assert.ok(err instanceof SettingsError, want);
    const { key, params } = B.explainSettingsError(err);
    assert.ok(B.ISSUE_KEYS.includes(key), `${want} -> ${key}`);
    assert.ok(key in en6 && key in zh6, key);
    const expected = { count: "beh.err.count", reference: "beh.err.reference", entry: "beh.err.entry" }[want] ?? `beh.err.${want}`;
    assert.equal(key, expected, `${want}: ${err.message}`);
    assert.ok(Number.isInteger(params.profile) && params.profile >= 1, `${want}: names the profile`);
    seen.add(key);
  }
  assert.ok(seen.size === 16, `${seen.size} different texts reached`);
  // and the page's own check says the same as the codec's for the record rules
  const p = fresh();
  p.m6 = { dks: [{ ...B.newDksRecord(), tAct: 39 }] };
  assert.equal(B.explainIssue(B.behaviorIssues(p)[0], 1).key, B.explainSettingsError(B.codecProblem(p, layout)).key);
});

test("explainSettingsError / explainIssue: every key they can return exists in both languages, with the same placeholders", () => {
  const ph = (s) => (s.match(/\{\w+\}/g) ?? []).sort().join();
  for (const k of B.ISSUE_KEYS) {
    assert.ok(k in en6, k); assert.ok(k in zh6, k);
    assert.ok(en6[k].length > 10 && zh6[k].length > 4);
    assert.equal(ph(en6[k]), ph(zh6[k]), k);
  }
  const probes = [
    new SettingsError("record", "x", { profile: 2, field: "count" }), new SettingsError("record", "x", { profile: 0, field: "pad.curve_range" }),
    new SettingsError("record", "x", { profile: 0, index: 3, field: "morph.mask" }), new SettingsError("record", "x", { field: "socd.overlap" }),
    new SettingsError("usage", "x", { field: "reference" }), new SettingsError("usage", "x", { field: "args" }), new SettingsError("length", "x"),
    new SettingsError("crc", "x"), new SettingsError("section", "x"), new Error("boom"), null,
  ];
  for (const e of probes) {
    const x = B.explainSettingsError(e);
    assert.ok(B.ISSUE_KEYS.includes(x.key), `${e?.code} ${e?.where?.field} -> ${x.key}`);
  }
  assert.equal(B.explainSettingsError(probes[0]).params.profile, 3);
  assert.equal(B.explainSettingsError(probes[2]).params.n, 4);
  const issues = [{ area: "dks", index: 2, why: "depth" }, { area: "morph", index: 0, why: "mask" }, { area: "socd", index: 1, why: "overlap" }, { area: "socd", index: -1, why: "count" },
    { area: "entry", index: 3, why: "reference" }, { area: "entry", index: -1, why: "args" }];
  for (const i of issues) assert.ok(B.ISSUE_KEYS.includes(B.explainIssue(i, 2).key), JSON.stringify(i));
  assert.deepEqual(B.explainIssue(issues[0], 2), { key: "beh.err.dks.depth", params: { profile: 2, n: 3 } });
});

// ------------------------------------------------------------------------------------------------ texts

test("the two locale files have the same keys and the same placeholders, and nothing is empty", () => {
  const ph = (s) => (s.match(/\{\w+\}/g) ?? []).sort().join();
  for (const k of Object.keys(en6)) assert.ok(k in zh6, `missing in zh-TW-m6: ${k}`);
  for (const k of Object.keys(zh6)) assert.ok(k in en6, `missing in en-m6: ${k}`);
  for (const k of Object.keys(en6)) {
    assert.ok(en6[k].trim() && zh6[k].trim(), k);
    assert.equal(ph(zh6[k]), ph(en6[k]), k);
  }
  assert.ok(Object.keys(en6).length > 150);
});

test("the Traditional Chinese file is Chinese (every sentence has CJK characters) and carries no simplified-only spelling of the page's key words", () => {
  // names and symbols that read the same in both languages
  const same = ["beh.tab.dks", "beh.legend.dks", "beh.dks.op.0", "beh.cur.line", "beh.cur.dks", "beh.usedBy.item", "beh.dks.name", "beh.morph.example.delete"];
  const latinOnly = Object.entries(zh6).filter(([k, v]) => !/[㐀-鿿]/.test(v) && !same.includes(k));
  assert.deepEqual(latinOnly.map(([k]) => k), [], "untranslated strings");
  const simplified = /[设删项选个钮层键组]/;
  for (const [k, v] of Object.entries(zh6)) assert.ok(!simplified.test(v), `simplified character in ${k}`);
});

test("the m6 keys do not collide with different text in the main dictionaries (merging them is safe)", () => {
  for (const [dict, main] of [[en6, en], [zh6, zh]]) for (const [k, v] of Object.entries(dict)) if (k in main) assert.equal(main[k], v, `${k} differs from the main dictionary`);
});

const pageSource = readFileSync(new URL("../js/pages/behaviors.js", import.meta.url), "utf8");

test("every literal tb('key') of the page exists in both languages", () => {
  const used = new Set([...pageSource.matchAll(/\btb\("([\w.]+)"/g)].map((m) => m[1]));
  assert.ok(used.size > 80, `${used.size} keys used`);
  for (const k of used) assert.ok(k in en6 && k in zh6, `untranslated: ${k}`);
});

test("every key the page builds from a name exists in both languages, and every key of the files is used", () => {
  const need = [];
  for (const t of ["dks", "morph", "toggle", "socd"]) need.push(`beh.tab.${t}`);
  for (const k of ["dks", "morph", "toggle", "socd", "plain"]) need.push(`beh.legend.${k}`);
  for (let e = 0; e < 4; e++) need.push(`beh.dks.ev.${e}`, `beh.dks.ev.${e}.tip`, `beh.dks.op.${e}`, `beh.dks.sum.${e}`);
  for (let s = 1; s <= 3; s++) need.push(`beh.dks.step.${s}`);
  for (const m of B.SOCD_MODES) need.push(`beh.socd.mode.${m}`, `beh.socd.mode.${m}.tip`);
  for (const kind of ["dks", "morph"]) need.push(`beh.${kind}.name`);
  for (const p of B.SOCD_PRESETS) need.push(`beh.socd.preset.${p.id}`);
  for (const p of B.MORPH_PRESETS) need.push(`beh.morph.example.${p.id}`);
  for (const side of ["left", "right"]) need.push(`beh.morph.${side}`);
  need.push("beh.dks.act", "beh.dks.bot");   // chosen with a condition in the page
  for (const r of ["full", "overlap", "same", "other"]) need.push(`beh.fail.${r}`);
  for (const k of need) {
    if (k === "beh.morph.name" || k === "beh.dks.name") { assert.ok(k in en6 && k in zh6, k); continue; }
    assert.ok(k in en6 && k in zh6, `missing: ${k}`);
  }
  // keys the files hold but nothing uses (typos, leftovers); the error texts are used through the explain functions
  const coreSource = readFileSync(new URL("../js/core/behaviors.js", import.meta.url), "utf8");
  const literal = new Set([...pageSource.matchAll(/\btb\("([\w.]+)"/g)].map((m) => m[1]));
  const unused = Object.keys(en6).filter((k) => !literal.has(k) && !need.includes(k) && !B.ISSUE_KEYS.includes(k) && k !== "nav.behaviors" && !coreSource.includes(`"${k}"`));
  assert.deepEqual(unused, [], "strings nothing uses");
});

test("the page uses the dictionaries of the main app that exist: layer names, tap-hold label, group names", () => {
  for (const i of [0, 1, 2, 3]) assert.ok(`remap.layer${i}` in zh && `remap.layer${i}` in en);
  assert.ok("remap.th.label" in zh && "remap.th.label" in en);
  for (const g of ["letters", "numbers", "punct", "func", "nav", "keypad", "modifiers", "oneshot", "media", "system", "mouse", "special"]) assert.ok(`group.${g}` in zh && `group.${g}` in en, g);
});

test("the page file does not touch what belongs to the integration step", () => {
  assert.ok(!/main\.js|index\.html|css\//.test(pageSource.replace(/\/\/.*$/gm, "")), "no imports of the shell");
  assert.match(pageSource, /export function mount\(root, app\)/);
  assert.match(pageSource, /destroy\(\)/);
});
