// Configuration protocol 2 (M6 P8, CONFIG_PROTOCOL.md): DEVICE_STATUS, the SCENE_* family, the feature bits, and the guarantee that
// protocol 1 keyboards and every protocol-1 op are unaffected.
import test from "node:test";
import assert from "node:assert/strict";
import { ConfigClient } from "../js/device/ConfigClient.js";
import { KmfwDevice, NotArmedError } from "../js/device/KmfwDevice.js";
import { MockDevice } from "../js/device/MockDevice.js";
import { UnsupportedError } from "../js/device/Device.js";
import { ConfigError, ST, OP, FEAT, WORD, MAX_SCENE, buildRequest, SafetyError, ascii, le16, le32, crc32, parseDeviceStatus } from "../js/device/config_protocol.js";
import { SimKeyboard } from "./helpers/sim-keyboard.mjs";
import { loadLayout } from "../js/core/layout.js";

globalThis.window ??= new EventTarget();
const pattern = (n, seed = 5) => Uint8Array.from({ length: n }, (_, i) => (i * 11 + seed) & 255);
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
async function ready(opts) {
  const sim = new SimKeyboard(opts);
  const c = new ConfigClient(sim);
  await c.verify();
  return { sim, c };
}
async function device(opts) {
  const sim = new SimKeyboard(opts);
  const dev = new KmfwDevice({ hidApi: undefined });
  await dev.connectTransport(sim);
  return { sim, dev };
}

test("info: protocol 2, the new feature bits, the scene limit", async () => {
  const { c } = await ready();
  assert.equal(c.info.protoVersion, 2);
  assert.ok(c.info.features & FEAT.DEVICE_STATUS && c.info.features & FEAT.SCENE && c.info.features & FEAT.STREAM && c.info.features & FEAT.HOST_LEDS);
  assert.equal(c.info.sceneMax, MAX_SCENE);
  const { c: old } = await ready({ protoVersion: 1, features: 0x3f });
  assert.equal(old.info.protoVersion, 1);
  assert.equal(old.info.sceneMax, 0, "a protocol 1 reply is 18 bytes: no scene limit");
  assert.equal(old.info.features, 0x3f);
});

test("DEVICE_STATUS decodes the 16 bytes", async () => {
  const { c } = await ready();
  const s = await c.deviceStatus();
  assert.deepEqual(s, {
    hostLeds: 2, num: false, caps: true, scroll: false, padModeSetting: 1, padModeActive: 0, usbFellBack: true, streamActive: false, animPlaying: false,
    hostLedsKnown: true, profile: 2, layer: 1, streamState: 2, streamFrames: 0x1234, streamLost: 3, ledReports: 7,
  });
  assert.throws(() => parseDeviceStatus(new Uint8Array(15)), ConfigError);
  assert.equal(buildRequest(OP.DEVICE_STATUS, 1).length, 64);
  assert.throws(() => buildRequest(OP.DEVICE_STATUS, 1, Uint8Array.of(0)), SafetyError);
});

test("DEVICE_STATUS and SCENE_* are not sent to a protocol 1 keyboard or one without the feature bit", async () => {
  const { sim, c } = await ready({ protoVersion: 1, features: 0x3f });
  const seen = sim.seen.length;
  await assert.rejects(c.deviceStatus(), /no device status/);
  await assert.rejects(c.sceneDump(), /no light scene/);
  await assert.rejects(c.sceneStage(pattern(10)), /no light scene/);
  assert.equal(sim.seen.length, seen, "nothing was sent");
  // a keyboard that speaks protocol 2 but has no scene storage answers UNSUPPORTED
  const noScene = await ready({ features: 0x7bf & ~0x100 });
  await assert.rejects(noScene.c.sceneDump(), /no light scene/);
  const raw = noScene.sim.process(buildRequest(OP.SCENE_GET, 9, Uint8Array.of(0, 0, 44, 0)));
  assert.equal(raw[10], ST.UNSUPPORTED);
  // and a protocol 1 simulator answers the new ops UNKNOWN_OP, as the old firmware did
  const rawOld = sim.process(buildRequest(OP.DEVICE_STATUS, 9));
  assert.equal(rawOld[10], ST.UNKNOWN_OP);
});

test("scene: read, stage (nothing written), commit (one write), read back", async () => {
  const { sim, c } = await ready();
  assert.ok(same(await c.sceneDump(), sim.scene));
  const next = pattern(214, 9);
  await c.sceneStage(next);
  assert.equal(sim.sceneWrites, 0);
  assert.equal(sim.flashWrites, 0, "the settings flash is not touched by a scene");
  assert.ok(!same(sim.scene, next));
  assert.equal((await c.getInfo()).stageState, 0, "the settings staging is separate");
  assert.equal(await c.sceneCommit(next, WORD.SCENE_COMMIT), 1);
  assert.equal(sim.sceneWrites, 1);
  assert.ok(same(await c.sceneDump(), next));
});

test("scene: the confirmation word, size limits, and a settings staging that survives", async () => {
  const { sim, c } = await ready();
  await assert.rejects(c.sceneCommit(pattern(10), "SAVE"), /needs the confirmation word "LITE"/);
  assert.throws(() => buildRequest(OP.SCENE_COMMIT, 1, concat2(ascii("SAVE"), le16(1), le32(1))), SafetyError);
  assert.throws(() => buildRequest(OP.COMMIT, 1, concat2(ascii("LITE"), le16(1), le32(1))), SafetyError);
  assert.throws(() => buildRequest(OP.SCENE_BEGIN, 1, le16(MAX_SCENE + 1)), SafetyError);
  assert.throws(() => buildRequest(OP.SCENE_BEGIN, 1, le16(0)), SafetyError);
  await assert.rejects(c.sceneStage(new Uint8Array(MAX_SCENE + 1)), /1\.\.1024/);
  await assert.rejects(c.sceneStage(new Uint8Array(0)), ConfigError);
  // a staged settings blob is not disturbed by a scene transfer
  const settings = pattern(300);
  await c.stage(settings);
  await c.sceneStage(pattern(100));
  assert.equal((await c.getInfo()).stageState, 2);
  assert.equal(await c.commit(settings, WORD.COMMIT), 1);
  assert.equal(sim.sceneWrites, 0);
  // a scene commit whose CRC is not that of the staged bytes is refused and writes nothing; with nothing staged it is NOT_STAGED
  const raw = sim.process(buildRequest(OP.SCENE_COMMIT, 5, concat2(ascii("LITE"), le16(100), le32(crc32(pattern(100, 7))))));
  assert.equal(raw[10], ST.BAD_CRC);
  assert.equal(sim.sceneWrites, 0);
  const fresh = new SimKeyboard();
  assert.equal(fresh.process(buildRequest(OP.SCENE_COMMIT, 5, concat2(ascii("LITE"), le16(100), le32(1))))[10], ST.NOT_STAGED);
});

test("scene: a lost COMMIT reply is settled by reading the scene back (one write, not two)", async () => {
  const { sim, c } = await ready();
  const next = pattern(120, 4);
  await c.sceneStage(next);
  const n = sim.seen.length + 1;           // the next exchange is the SCENE_COMMIT
  sim.dropReply.add(n);
  assert.equal(await c.sceneCommit(next, WORD.SCENE_COMMIT), -1);
  assert.equal(sim.sceneWrites, 1);
  assert.ok(same(sim.scene, next));
  // a lost REQUEST is sent again
  const { sim: s2, c: c2 } = await ready();
  await c2.sceneStage(next);
  s2.dropRequest.add(s2.seen.length + 1);
  assert.equal(await c2.sceneCommit(next, WORD.SCENE_COMMIT), 1);
  assert.equal(s2.sceneWrites, 1);
});

test("scene: a validator refusal is INVALID and keeps the old scene", async () => {
  const { sim, c } = await ready({ validateScene: () => false });
  const old = Uint8Array.from(sim.scene);
  await c.sceneStage(pattern(60));
  await assert.rejects(c.sceneCommit(pattern(60), WORD.SCENE_COMMIT), (e) => e.status === ST.INVALID);
  assert.ok(same(sim.scene, old));
  assert.equal(sim.sceneWrites, 0);
});

test("KmfwDevice: status and scene need their features; the scene write needs the arming", async () => {
  const { sim, dev } = await device();
  assert.equal(dev.features.deviceStatus && dev.features.scene && dev.features.stream && dev.features.hostLeds, true);
  assert.equal((await dev.readDeviceStatus()).caps, true);
  assert.ok(same(await dev.readScene(), sim.scene));
  const blob = pattern(214, 2);
  await assert.rejects(dev.writeScene(blob), NotArmedError);
  assert.equal(sim.sceneWrites, 0);
  dev.armWrites("SAVE");
  const steps = [];
  assert.equal(await dev.writeScene(blob, { onStep: (s) => steps.push(s) }), 1);
  assert.deepEqual(steps, ["stage", "commit", "verify"]);
  assert.equal(dev.readOnly, true, "the arming is consumed");
  assert.ok(same(sim.scene, blob));
  await assert.rejects(dev.writeScene(blob), NotArmedError);
  // an older keyboard: the features are false and the calls say so without sending anything
  const old = await device({ protoVersion: 1, features: 0x3f });
  assert.deepEqual([old.dev.features.deviceStatus, old.dev.features.scene, old.dev.features.stream, old.dev.features.hostLeds], [false, false, false, false]);
  await assert.rejects(old.dev.readDeviceStatus(), UnsupportedError);
  await assert.rejects(old.dev.readScene(), UnsupportedError);
  old.dev.armWrites("SAVE");
  await assert.rejects(old.dev.writeScene(blob), UnsupportedError);
});

test("protocol 1 ops are unchanged on a protocol 2 keyboard (settings round trip, profile switch, depths)", async () => {
  const { sim, c } = await ready();
  const blob = pattern(1000);
  await c.stage(blob);
  assert.equal(await c.commit(blob, WORD.COMMIT), 1);
  assert.ok(same(await c.dump(), blob));
  assert.equal(await c.selectProfile(3, WORD.SELECT_PROFILE), 3);
  assert.equal((await c.liveDepth(76))[10], 1070);
  assert.equal(sim.flashWrites, 1);
});

test("the demo keyboard (MockDevice) offers protocol 2", async () => {
  const dev = new MockDevice(loadLayout(), { persist: false });
  await dev.connect();
  try {
    assert.equal(dev.config.info.protoVersion, 2);
    assert.equal(dev.features.scene, true);
    assert.equal(typeof (await dev.readDeviceStatus()).profile, "number");
    const before = await dev.readScene();
    assert.ok(before.length > 0);
    dev.armWrites("SAVE");
    await dev.writeScene(pattern(80, 1));
    assert.ok(same(await dev.readScene(), pattern(80, 1)));
  } finally {
    await dev.disconnect();
  }
});

function concat2(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
