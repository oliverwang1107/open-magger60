// SceneKeeper: reading the scene from a (simulated) keyboard, the write flow (stage, commit, read back), live mode with coalescing,
// BUSY retries, failures, and keyboards that cannot keep a scene.
import test from "node:test";
import assert from "node:assert/strict";
import { KmfwDevice } from "../js/device/KmfwDevice.js";
import { SimKeyboard } from "./helpers/sim-keyboard.mjs";
import { SceneKeeper, sceneAvailability } from "../js/core/scene_keeper.js";
import { defaultScene, encodeScene, decodeScene, LightSceneError } from "../js/core/lightscene_codec.js";
import { withKeyColors, sceneKeyColors } from "../js/core/scene_colors.js";
import { newRule } from "../js/core/scene_rules.js";
import { ST } from "../js/device/config_protocol.js";

globalThis.window ??= new EventTarget();
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(cond, what, ms = 3000) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) assert.fail(`timed out waiting for: ${what}`);
    await wait(2);
  }
}
async function setup(opts = {}, keeperOpts = {}) {
  const sim = new SimKeyboard({ scene: encodeScene(defaultScene()), ...opts });
  const dev = new KmfwDevice({ hidApi: undefined });
  await dev.connectTransport(sim);
  const changes = [];
  const keeper = new SceneKeeper(dev, { liveDelayMs: 15, busyRetryMs: 5, sleepFn: wait, onChange: () => changes.push(keeper.save.kind), ...keeperOpts });
  return { sim, dev, keeper, changes };
}
const solid = (c) => withKeyColors(defaultScene(), Array.from({ length: 61 }, () => c));
const red = () => solid([255, 0, 0]);

test("availability: connected firmware with the scene, firmware without it, other firmware, nothing", async () => {
  const { dev } = await setup();
  assert.equal(sceneAvailability(dev), "ok");
  const old = new KmfwDevice({ hidApi: undefined });
  await old.connectTransport(new SimKeyboard({ protoVersion: 1, features: 0x3f }));
  assert.equal(sceneAvailability(old), "old");
  assert.equal(sceneAvailability({ status: "connected", kind: "webhid" }), "stock");
  assert.equal(sceneAvailability({ status: "disconnected", kind: "kmfw" }), "offline");
  assert.equal(sceneAvailability(null), "offline");
});

test("load: the scene on the keyboard becomes the working scene and the state is 'saved'", async () => {
  const s = { ...red(), rules: [newRule("caps", 28)] };
  const { keeper } = await setup({ scene: encodeScene(s) });
  assert.equal(keeper.save.kind, "pending", "before loading nothing is known");
  await keeper.load();
  assert.deepEqual(keeper.scene.rules, s.rules);
  assert.deepEqual(sceneKeyColors(keeper.scene)[3], [255, 0, 0]);
  assert.equal(keeper.onKeyboard.kind, "ok");
  assert.deepEqual(keeper.save, { kind: "saved" });
  assert.equal(keeper.loaded, true);
});

test("load: bytes that are not a light scene are reported, the default scene is shown, and it counts as pending", async () => {
  const { keeper } = await setup({ scene: Uint8Array.from({ length: 50 }, (_, j) => (j * 5 + 2) & 255) });
  await keeper.load();
  assert.equal(keeper.onKeyboard.kind, "unreadable");
  assert.ok(keeper.onKeyboard.error instanceof LightSceneError);
  assert.deepEqual(keeper.scene, defaultScene());
  assert.equal(keeper.save.kind, "pending");
});

test("a keyboard without the scene operations: refused without talking to it", async () => {
  const sim = new SimKeyboard({ protoVersion: 1, features: 0x3f });
  const dev = new KmfwDevice({ hidApi: undefined });
  await dev.connectTransport(sim);
  const keeper = new SceneKeeper(dev);
  assert.deepEqual(keeper.save, { kind: "none", reason: "old" });
  await assert.rejects(keeper.load(), /not available: old/);
  await keeper.saveNow();
  assert.equal(sim.sceneWrites, 0);
  assert.deepEqual(keeper.save, { kind: "none", reason: "old" });
});

test("saveNow: stage, commit and read back; the keyboard holds exactly the scene; the page is told about the write", async () => {
  const { sim, keeper, changes } = await setup();
  await keeper.load();
  keeper.set(red());
  assert.equal(keeper.save.kind, "pending");
  await keeper.saveNow();
  assert.deepEqual(keeper.save, { kind: "saved" });
  assert.equal(sim.sceneWrites, 1);
  assert.deepEqual([...sim.scene], [...encodeScene(red())]);
  assert.ok(changes.includes("saving"));
});

test("the steps of a write are visible while it runs", async () => {
  const { keeper } = await setup();
  await keeper.load();
  keeper.set(red());
  const seen = new Set();
  const p = keeper.saveNow();
  while (keeper.busy) { const st = keeper.save; if (st.kind === "saving") seen.add(st.step); await wait(0); }
  await p;
  assert.ok([...seen].every((s) => ["stage", "commit", "verify", "wait", "busy"].includes(s)), [...seen].join());
});

test("nothing pending = no write; writing the same scene twice sends one write", async () => {
  const { sim, keeper } = await setup();
  await keeper.load();
  await keeper.saveNow();
  assert.equal(sim.sceneWrites, 0);
  keeper.set(red());
  await keeper.saveNow();
  await keeper.saveNow();
  assert.equal(sim.sceneWrites, 1);
});

test("changing back to what the keyboard holds is 'saved' again, without any write", async () => {
  const { sim, keeper } = await setup();
  await keeper.load();
  keeper.set(red());
  assert.equal(keeper.save.kind, "pending");
  keeper.set(defaultScene());
  assert.equal(keeper.save.kind, "saved");
  assert.equal(sim.sceneWrites, 0);
});

test("an invalid scene is refused before it becomes the working copy", async () => {
  const { keeper } = await setup();
  await keeper.load();
  assert.throws(() => keeper.set({ ...defaultScene(), keys: new Array(61).fill(16) }), LightSceneError);
  assert.deepEqual(keeper.scene, defaultScene());
});

test("live mode: many quick changes are ONE write of the last state, sent after the person stopped", async () => {
  const { sim, keeper } = await setup();
  await keeper.load();
  keeper.setLive(true);
  for (let i = 0; i < 20; i++) keeper.set(solid([i * 10, 0, 0]));
  assert.equal(sim.sceneWrites, 0, "nothing is written while changes keep coming");
  assert.equal(keeper.save.kind, "saving");
  await until(() => keeper.save.kind === "saved", "the live write");
  assert.equal(sim.sceneWrites, 1);
  assert.deepEqual(sceneKeyColors(decodeScene(sim.scene))[0], [190, 0, 0]);
});

test("live mode: changes that arrive while a write is running cost at most one more write, and the keyboard ends with the last state", async () => {
  const { sim, dev, keeper } = await setup({}, { liveDelayMs: 5 });
  await keeper.load();
  const real = dev.writeScene.bind(dev);
  let release;
  const gate = new Promise((r) => { release = r; });
  let first = true;
  dev.writeScene = async (b, o) => { if (first) { first = false; await gate; } return real(b, o); };   // hold the first write open
  keeper.setLive(true);
  keeper.set(red());
  await until(() => keeper.busy, "the write to start");
  keeper.set(solid([0, 255, 0]));
  keeper.set(solid([0, 0, 255]));
  release();
  await until(() => keeper.save.kind === "saved" && !keeper.busy, "all writes");
  assert.equal(sim.sceneWrites, 2, "the red state went out, then ONE more write for the two newer changes");
  assert.deepEqual(sceneKeyColors(decodeScene(sim.scene))[0], [0, 0, 255]);
});

test("live mode off: changes wait for the button; turning it on writes what is pending; turning it off cancels a waiting write", async () => {
  const { sim, keeper } = await setup({}, { liveDelayMs: 40 });
  await keeper.load();
  keeper.set(red());
  await wait(80);
  assert.equal(sim.sceneWrites, 0);
  keeper.setLive(true);
  assert.equal(keeper.save.kind, "saving");
  keeper.setLive(false);
  await wait(80);
  assert.equal(sim.sceneWrites, 0, "cancelled");
  assert.equal(keeper.save.kind, "pending");
  keeper.setLive(true);
  await until(() => keeper.save.kind === "saved", "write after turning live on");
  assert.equal(sim.sceneWrites, 1);
});

test("BUSY (a key is down): tried again until it works", async () => {
  const { sim, keeper } = await setup();
  await keeper.load();
  keeper.set(red());
  sim.busy = true;
  const p = keeper.saveNow();
  await until(() => keeper.save.kind === "saving" && keeper.save.step === "busy", "the busy step");
  assert.equal(sim.sceneWrites, 0);
  sim.busy = false;
  await p;
  assert.deepEqual(keeper.save, { kind: "saved" });
  assert.equal(sim.sceneWrites, 1);
});

test("BUSY for too long: gives up with the keyboard's answer and says 'failed'; retry works later", async () => {
  const { sim, keeper } = await setup({}, { busyRetries: 2 });
  await keeper.load();
  keeper.set(red());
  sim.busy = true;
  await keeper.saveNow();
  assert.equal(keeper.save.kind, "failed");
  assert.equal(keeper.save.error.status, ST.BUSY);
  sim.busy = false;
  await keeper.retry();
  assert.deepEqual(keeper.save, { kind: "saved" });
});

test("a refusal by the keyboard (INVALID) is a failure; the working scene stays; retry works once the keyboard accepts", async () => {
  let accept = false;
  const { sim, keeper } = await setup({ validateScene: () => accept });
  await keeper.load();
  keeper.set(red());
  await keeper.saveNow();
  assert.equal(keeper.save.kind, "failed");
  assert.equal(keeper.save.error.status, ST.INVALID);
  assert.deepEqual(sceneKeyColors(keeper.scene)[0], [255, 0, 0]);
  assert.equal(sim.sceneWrites, 0);
  accept = true;
  await keeper.retry();
  assert.deepEqual(keeper.save, { kind: "saved" });
  assert.equal(sim.sceneWrites, 1);
});

test("a failed live write is not repeated by itself; the next change tries again", async () => {
  let accept = false;
  const { sim, keeper } = await setup({ validateScene: () => accept }, { liveDelayMs: 5 });
  await keeper.load();
  keeper.setLive(true);
  keeper.set(red());
  await until(() => keeper.save.kind === "failed", "the failure");
  await wait(60);
  assert.equal(keeper.save.kind, "failed");
  accept = true;
  keeper.set(solid([1, 2, 3]));
  await until(() => keeper.save.kind === "saved", "the next try");
  assert.equal(sim.sceneWrites, 1);
});

test("compare: reads the keyboard back and says whether it is the working scene", async () => {
  const { sim, keeper } = await setup();
  await keeper.load();
  assert.deepEqual(await keeper.compare(), { same: true, unreadable: false });
  keeper.set(red());
  assert.deepEqual(await keeper.compare(), { same: false, unreadable: false });
  await keeper.saveNow();
  assert.deepEqual(await keeper.compare(), { same: true, unreadable: false });
  sim.scene = Uint8Array.of(1, 2, 3);
  assert.deepEqual(await keeper.compare(), { same: false, unreadable: true });
  assert.equal(keeper.save.kind, "pending");
});

test("dispose sends a live write that was still waiting for the person to stop", async () => {
  const { sim, keeper } = await setup({}, { liveDelayMs: 5000 });
  await keeper.load();
  keeper.setLive(true);
  keeper.set(red());
  keeper.dispose();
  await until(() => sim.sceneWrites === 1, "the flush on dispose");
});

test("the keeper arms exactly one write per write and leaves nothing armed", async () => {
  const { dev, keeper } = await setup();
  await keeper.load();
  keeper.set(red());
  await keeper.saveNow();
  assert.equal(dev.armed, false);
});
