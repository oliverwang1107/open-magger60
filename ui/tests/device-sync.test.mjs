// The device layer and the load / save flow against the simulated keyboard, including every error path the UI explains.
import test from "node:test";
import assert from "node:assert/strict";
import { KmfwDevice, NotArmedError } from "../js/device/KmfwDevice.js";
import { MockDevice } from "../js/device/MockDevice.js";
import { SimKeyboard, FakeHid, FakeHidApi } from "../js/device/sim_keyboard.js";
import { buildRequest, OP, ST, WORD, checksum, concat, ascii, le16, le32, crc32 } from "../js/device/config_protocol.js";
import { classifyProblem } from "../js/device/errors.js";
import { loadLayout } from "../js/core/layout.js";
import { decodeSettings, encodeSettings, SettingsError } from "../js/core/settings_codec.js";
import { defaultWorkspace, workspaceToBlob, sameBytes } from "../js/core/profile_codec.js";
import { rawCodec } from "../js/core/raw_codec.js";
import { FW_SETTINGS } from "../js/core/limits.js";
import { MODE } from "../js/core/trigger.js";
import { loadFromKeyboard, pendingChanges, saveToKeyboard } from "../js/sync.js";

globalThis.window ??= new EventTarget();
const layout = loadLayout();
const factory = () => workspaceToBlob(defaultWorkspace(layout), layout).bytes;
const validate = (b) => { try { decodeSettings(b); return true; } catch { return false; } };

/** A keyboard the way the integrated firmware is meant to behave: 8 KB settings area, validates what it stores. */
const simulator = (o = {}) => new SimKeyboard({ blob: factory(), active: 0, validate, maxSettings: 8192, scope: true, health: true, ...o });
async function connect(simOpts = {}, devOpts = {}) {
  const sim = simulator(simOpts);
  const dev = new KmfwDevice({ hidApi: undefined, settingsSize: FW_SETTINGS.size, ...devOpts });
  dev.attachSettingsCodec(rawCodec);
  await dev.connectTransport(sim);
  return { sim, dev };
}
const tick = (ms) => new Promise((r) => setTimeout(r, ms));

test("connect: features and optional diagnostics are detected at run time", async () => {
  const full = await connect();
  assert.equal(full.dev.status, "connected");
  assert.deepEqual(full.dev.features, { settings: true, selectProfile: true, recalibrate: true, saveCalibration: true, live: true, lights: false, lightProfiles: false, deviceStatus: true, scene: true, stream: true, hostLeds: true, diag: true, health: true, keyMask: true });
  const plain = await connect({ health: false });
  assert.equal(plain.dev.features.diag, true);
  assert.equal(plain.dev.features.health, false, "no 0x0300 tag: no health panel");
  assert.equal(await plain.dev.readHealth(), null);
  const noScope = await connect({ scope: false });
  assert.equal(noScope.dev.features.diag, false);
  assert.equal(await noScope.dev.readEngineDiag(), null);
  assert.equal(noScope.dev.status, "connected", "a firmware without the scope channel is still usable");
});

test("scope data: engine diagnostics, health counters and the pressed-key mask are decoded", async () => {
  const depths = new Array(76).fill(0);
  depths[18] = 300; depths[5] = 340; // two keys held
  const { dev } = await connect({ depthFn: (s) => depths[s] });
  const d = await dev.readEngineDiag();
  assert.equal(d.calResult, "ok");
  assert.equal(d.armed, true);
  assert.equal(d.keysDown, 2);
  assert.deepEqual(d.pressedSlots.sort((a, b) => a - b), [5, 18]);
  const h = await dev.readHealth();
  assert.equal(h.resetCause, "iwdg");
  assert.deepEqual(h.faults, []);
  assert.deepEqual((await dev.readKeyMask()).down.sort((a, b) => a - b), [5, 18]);
});

test("connect problems are named: stock firmware, protocol too old / too new, settings area too small", async () => {
  const kinds = {};
  for (const [name, opts] of Object.entries({
    stock: { stock: true }, old: { protoVersion: 0 }, new: { protoVersion: 3 }, small: { maxSettings: 4096 },
  })) {
    const sim = simulator(opts);
    const dev = new KmfwDevice({ hidApi: undefined, settingsSize: FW_SETTINGS.size });
    await assert.rejects(dev.connectTransport(sim));
    assert.equal(dev.status, "error", name);
    kinds[name] = dev.problem.kind;
    assert.ok(!sim.seen.some(([cmd, op]) => cmd === 0xc6 && [3, 4, 5, 6, 7, 8].includes(op)), `${name}: nothing was written or staged`);
    if (name === "stock") assert.deepEqual(sim.seen, [[3, null]], "only the version query reached the stock keyboard");
  }
  assert.deepEqual(kinds, { stock: "stock_firmware", old: "proto_old", new: "proto_new", small: "settings_size" });
});

test("no device chosen / no WebHID are named too", async () => {
  const none = new KmfwDevice({ hidApi: new FakeHidApi([]) });
  await assert.rejects(none.connect());
  assert.equal(none.problem.kind, "cancelled");
  const unsupported = new KmfwDevice({ hidApi: undefined });
  await assert.rejects(unsupported.connect());
  assert.equal(unsupported.problem.kind, "unsupported");
});

test("load: the keyboard's settings become the editor's model, with nothing unsaved", async () => {
  const { dev } = await connect();
  const { workspace, blob } = await loadFromKeyboard(dev, layout);
  assert.equal(workspace.active, 0);
  const p = pendingChanges(workspace, blob, layout);
  assert.equal(p.dirty, false);
  assert.deepEqual(p.problems, []);
});

test("edit -> dirty -> Save writes, verifies and clears the difference; exactly one flash write", async () => {
  const { dev, sim } = await connect();
  const { workspace, blob } = await loadFromKeyboard(dev, layout);
  workspace.profiles[0].keys.KeyW.trigger = { mode: MODE.RT, actuation: 1.5, release: 1.0, rapidDown: 0.25, rapidUp: 0.3, topDead: 0.2, bottomDead: 0.2 };
  workspace.profiles[1].keys.KeyQ.bindings[0] = { type: "key", usage: 0xe0 };
  assert.equal(pendingChanges(workspace, blob, layout).dirty, true);
  assert.equal(sim.flashWrites, 0, "editing alone never writes");
  const steps = [];
  const res = await saveToKeyboard(dev, workspace, layout, { onStep: (s) => steps.push(s) });
  assert.deepEqual([...new Set(steps)], ["encode", "stage", "commit", "verify"]);
  assert.equal(sim.flashWrites, 1);
  assert.equal(res.commits, 1);
  assert.ok(sameBytes(sim.blob, pendingChanges(workspace, null, layout).bytes));
  assert.equal(pendingChanges(workspace, res.blob, layout).dirty, false);
  const again = (await loadFromKeyboard(dev, layout)).workspace;
  assert.equal(again.profiles[0].keys.KeyW.trigger.mode, MODE.RT);
  assert.equal(again.profiles[1].keys.KeyQ.bindings[0].usage, 0xe0);
  assert.equal(dev.armed, false, "the arming was used up");
});

test("settings the keyboard cannot hold block the save before anything is sent", async () => {
  const { dev, sim } = await connect();
  const { workspace } = await loadFromKeyboard(dev, layout);
  workspace.profiles[0].keys.KeyA.bindings[0] = { type: "mouse", button: 1 };
  const seen = sim.seen.length;
  await assert.rejects(saveToKeyboard(dev, workspace, layout), (e) => e.name === "UnsupportedSettingsError" && e.problems[0].keyId === "KeyA");
  assert.equal(sim.seen.length, seen);
  assert.equal(sim.flashWrites, 0);
});

test("corrupt blob on the keyboard: the load fails with a CRC / format explanation, the editor keeps working", async () => {
  const bad = factory();
  bad[200] ^= 0x55; // damaged after the keyboard computed its CRC
  const { dev } = await connect({ blob: bad });
  await assert.rejects(loadFromKeyboard(dev, layout), (e) => e instanceof SettingsError && e.code === "crc" && classifyProblem(e).kind === "crc");
  for (const [blob, kind] of [[new Uint8Array(100).fill(0xff), "magic"], [Uint8Array.from([1, 2, 3]), "short"]]) {
    const k = await connect({ blob });
    await assert.rejects(loadFromKeyboard(k.dev, layout), (e) => e.code === kind);
  }
});

test("a settings blob that is 'valid' to the transfer layer but refused by the validator is INVALID and changes nothing", async () => {
  const { dev, sim } = await connect();
  const before = Uint8Array.from(sim.blob);
  const evil = factory();
  evil[12 + 4] = 9; // mode 9 in profile 1 slot 0, CRC repaired so only the validator can notice
  evil.set(le32(crc32(evil.subarray(0, evil.length - 4))), evil.length - 4);
  dev.armWrites("SAVE");
  await assert.rejects(dev.writeSettings(evil), (e) => classifyProblem(e).kind === "invalid");
  assert.ok(sameBytes(sim.blob, before));
  assert.equal(sim.flashWrites, 0);
  assert.equal(dev.armed, false);
});

test("commit without the confirmation word is refused, by the client and by the keyboard", async () => {
  const { dev, sim } = await connect();
  await assert.rejects(dev.config.commit(factory(), ""), /confirmation word/);
  await assert.rejects(dev.writeSettings(factory()), NotArmedError);
  // a hand-made COMMIT packet with a wrong word, straight into the simulated firmware
  const staged = factory();
  await dev.config.stage(staged);
  const args = concat(ascii("NOPE"), le16(staged.length), le32(crc32(staged)));
  const data = concat(Uint8Array.of(OP.COMMIT, 7), args);
  const p = new Uint8Array(64);
  p.set([0x55, 0xc6, 0, checksum(data.length, 0, data), data.length, 0, 0, 0]);
  p.set(data, 8);
  const rep = sim.process(p);
  assert.equal(rep[8 + 2], ST.NEED_CONFIRM);
  assert.equal(sim.flashWrites, 0);
  assert.throws(() => buildRequest(OP.COMMIT, 1, args), /confirmation word/);
});

test("busy keyboard: the save waits, then succeeds when the key is let go; otherwise it ends in 'busy' and nothing changed", async () => {
  const { dev, sim } = await connect();
  const { workspace } = await loadFromKeyboard(dev, layout);
  workspace.active = 0;
  workspace.profiles[2].keys.KeyS.trigger.actuation = 2.0;
  sim.busy = true;
  setTimeout(() => { sim.busy = false; }, 120);
  const ok = await saveToKeyboard(dev, workspace, layout, { busyWaitMs: 3000 });
  assert.equal(ok.commits, 1);
  assert.equal(sim.flashWrites, 1);

  workspace.profiles[2].keys.KeyS.trigger.actuation = 2.2;
  sim.busy = true;
  const before = Uint8Array.from(sim.blob);
  await assert.rejects(saveToKeyboard(dev, workspace, layout, { busyWaitMs: 100 }), (e) => classifyProblem(e).kind === "busy");
  assert.ok(sameBytes(sim.blob, before));
  assert.equal(sim.flashWrites, 1);
});

test("keyboard removed in the middle of a transfer: clear 'lost' state, old settings stay, nothing is left armed", async () => {
  const { dev, sim } = await connect();
  const { workspace } = await loadFromKeyboard(dev, layout);
  workspace.profiles[3].keys.KeyD.trigger.actuation = 2.5;
  const before = Uint8Array.from(sim.blob);
  sim.unplugAfter = sim.seen.length + 30; // somewhere in the middle of the staging pages
  await assert.rejects(saveToKeyboard(dev, workspace, layout), (e) => classifyProblem(e).kind === "lost");
  assert.equal(dev.status, "error");
  assert.equal(dev.problem.kind, "lost");
  assert.equal(dev.armed, false);
  assert.ok(sameBytes(sim.blob, before));
  assert.equal(sim.flashWrites, 0);
  assert.throws(() => dev.armWrites("nope"));
  await assert.rejects(dev.readSettings(), /not connected/);
});

test("removal through the WebHID disconnect event is reported as lost as well", async () => {
  const sim = simulator();
  const hid = new FakeHid(sim);
  const api = new FakeHidApi([hid]);
  const dev = new KmfwDevice({ hidApi: api, settingsSize: FW_SETTINGS.size });
  await dev.connect();
  dev.armWrites("SAVE");
  const ev = new Event("disconnect");
  ev.device = hid;
  api.dispatchEvent(ev);
  assert.equal(dev.status, "disconnected");
  assert.equal(dev.problem.kind, "lost");
  assert.equal(dev.armed, false);
});

test("lost packets during a save are retried and the result is still exactly one commit", async () => {
  const { dev, sim } = await connect();
  const { workspace } = await loadFromKeyboard(dev, layout);
  workspace.profiles[0].keys.KeyE.trigger.actuation = 1.9;
  sim.dropReply.add(sim.seen.length + 5);
  sim.dropRequest.add(sim.seen.length + 9);
  const res = await saveToKeyboard(dev, workspace, layout);
  assert.equal(sim.flashWrites, 1);
  assert.equal(res.commits, 1);
}, { timeout: 15000 });

test("profile select, recalibrate and save-calibration need their own words and honour BUSY", async () => {
  const { dev, sim } = await connect();
  await assert.rejects(dev.recalibrate("SAVE"), /confirmation word/);
  await assert.rejects(dev.saveCalibration("RCAL"), /confirmation word/);
  assert.equal(sim.recal + sim.saveCal, 0);
  await dev.recalibrate("RCAL");
  assert.equal(sim.recal, 1);
  assert.equal((await dev.readEngineDiag()).calibrating, true, "the diagnostics show the calibration running");
  for (let i = 0; i < 30 && (await dev.readEngineDiag()).calibrating; i++);
  assert.equal((await dev.readEngineDiag()).calResult, "ok");
  assert.equal((await dev.readEngineDiag()).calAttempts, 2);
  await dev.saveCalibration("SCAL");
  assert.equal(sim.saveCal, 1);
  assert.equal(await dev.selectProfile(2, "PROF"), 2);
  assert.equal(dev.activeProfile, 2);
  assert.equal(decodeSettings(await dev.readSettings()).active, 2, "the blob says so too");
  sim.busy = true;
  await assert.rejects(dev.recalibrate("RCAL"), (e) => classifyProblem(e).kind === "busy");
  await assert.rejects(dev.selectProfile(1, "PROF"), (e) => classifyProblem(e).kind === "busy");
  assert.equal(sim.recal, 1);
  assert.equal(dev.activeProfile, 2);
});

test("everything that reaches the keyboard in a full session is on the whitelist", async () => {
  const { dev, sim } = await connect();
  const { workspace } = await loadFromKeyboard(dev, layout);
  workspace.profiles[0].keys.KeyR.trigger.actuation = 2.1;
  await saveToKeyboard(dev, workspace, layout);
  await dev.recalibrate("RCAL");
  await dev.saveCalibration("SCAL");
  await dev.selectProfile(1, "PROF");
  await dev.readEngineDiag(); await dev.readHealth(); await dev.readKeyMask();
  for (const [cmd, op] of sim.seen) {
    assert.ok(cmd === 3 || cmd === 0xc5 || (cmd === 0xc6 && op >= 1 && op <= 9), `cmd ${cmd} op ${op}`);
  }
});

test("the demo keyboard is the same machine: connect, features, save, persisted flash blob", async () => {
  const mock = new MockDevice(layout, { persist: false });
  assert.deepEqual(mock.capabilities, { live: true, readProfile: true, writeProfile: true });
  await mock.connect();
  assert.equal(mock.kind, "mock");
  assert.equal(mock.features.settings, true);
  assert.equal(mock.features.health, true);
  const { workspace } = await loadFromKeyboard(mock, layout);
  workspace.profiles[0].keys.KeyG.trigger.mode = MODE.RT_CONTINUOUS;
  const res = await saveToKeyboard(mock, workspace, layout);
  assert.equal(res.commits, 1);
  assert.equal(mock.sim.flashWrites, 1);
  assert.equal(decodeSettings(mock.sim.blob).profiles[0].keys[layout.byId.get("KeyG").slot].continuous, 1);
  // live depth comes through the protocol, paced
  const frames = [];
  const off = mock.subscribeLive((d) => frames.push(d.length));
  mock.press(layout.byId.get("KeyG").slot, 340, 20, 400, 20);
  await tick(250);
  off();
  assert.ok(frames.length >= 3 && frames.every((n) => n === 76), `${frames.length} frames`);
  await mock.disconnect();
});

test("encodeSettings output is what the simulator accepts (sanity: the validator is the real codec)", async () => {
  const { dev } = await connect();
  const s = decodeSettings(factory());
  s.profiles[2].keys[3].press = 200;
  dev.armWrites("SAVE");
  assert.equal(await dev.writeSettings(encodeSettings(s)), 1);
});

test("an explicit overlay entry equal to the base key is not an unsaved difference (it is the same as transparent)", async () => {
  const { dev } = await connect();
  const { workspace, blob } = await loadFromKeyboard(dev, layout);
  workspace.profiles[0].keys.KeyQ.bindings[2] = { type: "key", usage: 20 };   // Q on layer 2, over a base Q
  workspace.profiles[0].keys.KeyA.bindings[0] = { type: "layer", layer: 2 };  // hold layer 2 on A (valid code 0xFB)
  workspace.profiles[0].keys.KeyA.trigger.mode = MODE.RT;
  const before = pendingChanges(workspace, blob, layout);
  assert.equal(before.dirty, true);
  assert.deepEqual(before.problems, []);
  const res = await saveToKeyboard(dev, workspace, layout);
  assert.equal(pendingChanges(workspace, res.blob, layout).dirty, false, "nothing differs after the save");
  const again = (await loadFromKeyboard(dev, layout)).workspace;
  assert.deepEqual(again.profiles[0].keys.KeyA.bindings[0], { type: "layer", layer: 2 });
  assert.equal(pendingChanges(workspace, (await loadFromKeyboard(dev, layout)).blob, layout).dirty, false);
});
