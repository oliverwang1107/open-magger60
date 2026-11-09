// Lights kept on the keyboard: the extended LED_GET / LED_SET codec and wire rules, the device layer against a simulated keyboard
// that implements the extended form, and the LedKeeper flow (live changes, one coalesced SAVE, status bits, failure, old firmware).
import test from "node:test";
import assert from "node:assert/strict";
import { encodeLedEx, decodeLedEx, ledStatusFlags, LED_STATUS, LED_IDLE_DEFAULT_S } from "../js/core/led_ex.js";
import { LedKeeper } from "../js/core/led_keep.js";
import { LED_DEFAULT } from "../js/core/led.js";
import { buildRequest, OP, ST, SafetyError, ConfigError, concat, checksum } from "../js/device/config_protocol.js";
import { KmfwDevice } from "../js/device/KmfwDevice.js";
import { SimKeyboard } from "../js/device/sim_keyboard.js";
import { loadLayout } from "../js/core/layout.js";
import { decodeSettings } from "../js/core/settings_codec.js";
import { defaultWorkspace, workspaceToBlob } from "../js/core/profile_codec.js";
import { rawCodec } from "../js/core/raw_codec.js";
import { FW_SETTINGS } from "../js/core/limits.js";

globalThis.window ??= new EventTarget();
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(cond, what, ms = 2000) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) assert.fail(`timed out waiting for: ${what}`);
    await wait(2);
  }
}

// ---- codec ----

test("extended LED_SET: nine bytes, idle time little endian, SAVE is bit 0", () => {
  const b = encodeLedEx({ ...LED_DEFAULT, effect: 3, brightness: 200, speed: 90, r: 1, g: 2, b: 3 }, { idleS: 600, save: true });
  assert.deepEqual([...b], [3, 200, 90, 1, 2, 3, 0x58, 0x02, 1]);
  assert.equal(encodeLedEx(LED_DEFAULT, { idleS: 0 })[8], 0);
  assert.deepEqual([...encodeLedEx(LED_DEFAULT, { idleS: 0xffff }).subarray(6, 8)], [255, 255]);
  assert.throws(() => encodeLedEx(LED_DEFAULT, { idleS: 65536 }), RangeError);
  assert.throws(() => encodeLedEx(LED_DEFAULT, { idleS: -1 }), RangeError);
  assert.throws(() => encodeLedEx(LED_DEFAULT, {}), RangeError, "the idle time is required");
  assert.throws(() => encodeLedEx({ ...LED_DEFAULT, speed: 0 }, { idleS: 5 }), RangeError, "speed 0 is refused like in the plain form");
});

test("extended LED_GET reply: lights, idle time, status block", () => {
  const r = decodeLedEx(Uint8Array.of(6, 255, 128, 0, 150, 255, 0x58, 0x02, 0x14, 0, 3, 1));
  assert.deepEqual(r, { effect: 6, brightness: 255, speed: 128, r: 0, g: 150, b: 255, idleS: 600, status: 0x14, lastResult: 0, savesOk: 3, savesFailed: 1 });
  assert.deepEqual(ledStatusFlags(0x14), { pending: false, failed: false, restored: true, idleOff: false, persistent: true, profileOwn: false });
  assert.deepEqual(ledStatusFlags(LED_STATUS.SAVE_PENDING | LED_STATUS.SAVE_FAILED | LED_STATUS.IDLE_OFF), { pending: true, failed: true, restored: false, idleOff: true, persistent: false, profileOwn: false });
  assert.throws(() => decodeLedEx(new Uint8Array(6)), RangeError);
  assert.throws(() => decodeLedEx(null), RangeError);
});

test("wire rules: the extended forms are allowed, near misses are refused before sending", () => {
  const ok = (op, args) => assert.doesNotThrow(() => buildRequest(op, 1, Uint8Array.from(args)));
  const bad = (op, args) => assert.throws(() => buildRequest(op, 1, Uint8Array.from(args)), SafetyError);
  ok(OP.LED_GET, []); ok(OP.LED_GET, [1]);
  bad(OP.LED_GET, [0]); bad(OP.LED_GET, [2]); bad(OP.LED_GET, [1, 4]); bad(OP.LED_GET, [2, 1]); bad(OP.LED_GET, [1, 1, 1]);
  ok(OP.LED_SET, [1, 2, 3, 4, 5, 6]);
  ok(OP.LED_SET, [1, 2, 3, 4, 5, 6, 0, 0, 0]); ok(OP.LED_SET, [1, 2, 3, 4, 5, 6, 0x58, 2, 1]);
  bad(OP.LED_SET, [1, 2, 3, 4, 5, 6, 0, 0]); bad(OP.LED_SET, [1, 2, 3, 4, 5, 6, 0, 0, 2]);
  bad(OP.LED_SET, [10, 2, 3, 4, 5, 6, 0, 0, 0]); bad(OP.LED_SET, [1, 2, 0, 4, 5, 6, 0, 0, 1]);
});

// ---- a simulated keyboard that implements the extended form the way the firmware documents it ----

const layout = loadLayout();
const factory = () => workspaceToBlob(defaultWorkspace(layout), layout).bytes;
const validate = (b) => { try { decodeSettings(b); return true; } catch { return false; } };
const frame = (data) => {
  const r = new Uint8Array(64);
  r[0] = 0xaa; r[1] = 0xc6; r[3] = checksum(data.length, 0, data); r[4] = data.length;
  r.set(data, 8);
  return r;
};

/** `persistent`: the build can keep the values (status bit 4). `pollsToWrite`: extended LED_GETs until a requested save has reached flash. */
class ExtSim extends SimKeyboard {
  constructor({ persistent = true, pollsToWrite = 2, failSave = false, ext = true, ...o } = {}) {
    super({ blob: factory(), active: 0, validate, maxSettings: 8192, features: 0x7f, ...o });
    Object.assign(this, { persistent, pollsToWrite, failSave, ext });
    this.idleS = LED_LIGHT_IDLE;
    this.pending = 0;
    this.failed = false;
    this.restored = false;
    this.flash = null;       // what a save stored
    this.saveRequests = 0;
    this.liveSets = 0;
  }

  status() { return (this.pending ? 1 : 0) | (this.failed ? 2 : 0) | (this.restored ? 4 : 0) | (this.persistent ? 16 : 0); }
  extReply() {
    const b = this.led;
    return Uint8Array.of(b.effect, b.brightness, b.speed, b.r, b.g, b.b, this.idleS & 255, this.idleS >> 8, this.status(), 0, 0, 0);
  }

  process(packet) {
    const d = packet.subarray(8, 8 + packet[4]);
    const [op, token] = d;
    const a = d.subarray(2);
    const done = (st, payload = new Uint8Array(0)) => frame(concat(Uint8Array.of(op, token, st), payload));
    if (op === OP.LED_GET && a.length === 1) {
      if (!this.ext) return done(ST.BAD_LENGTH);
      if (a[0] !== 1) return done(ST.BAD_ARG);
      if (this.pending && --this.pollsLeft <= 0) { this.pending = 0; this.failed = this.failSave; if (!this.failSave) this.flash = { ...this.led, idleS: this.idleS }; }
      return done(0, this.extReply());
    }
    if (op === OP.LED_SET && a.length === 9) {
      if (!this.ext) return done(ST.BAD_LENGTH);
      if (a[8] & ~1) return done(ST.BAD_ARG);
      if (a[8] === 1 && !this.persistent) return done(ST.UNSUPPORTED);
      if (a[0] > 6 || a[2] === 0) return done(ST.RANGE);
      [this.led.effect, this.led.brightness, this.led.speed, this.led.r, this.led.g, this.led.b] = a;
      this.idleS = a[6] | (a[7] << 8);
      if (a[8] === 1) { this.saveRequests++; this.pending = 1; this.failed = false; this.pollsLeft = this.pollsToWrite; } else this.liveSets++;
      return done(0, this.extReply());
    }
    return super.process(packet);
  }
}
const LED_LIGHT_IDLE = LED_IDLE_DEFAULT_S;

async function deviceOn(sim) {
  const dev = new KmfwDevice({ hidApi: undefined, settingsSize: FW_SETTINGS.size });
  dev.attachSettingsCodec(rawCodec);
  await dev.connectTransport(sim);
  return dev;
}

test("device layer: extended read / write / save against a keyboard with the extended form", async () => {
  const sim = new ExtSim();
  const dev = await deviceOn(sim);
  const first = await dev.readLightsEx();
  assert.equal(first.idleS, 600);
  assert.equal(ledStatusFlags(first.status).persistent, true);
  const live = await dev.writeLightsEx({ ...LED_DEFAULT, effect: 2, brightness: 100 }, { idleS: 0, save: false });
  assert.equal(live.effect, 2);
  assert.equal(live.idleS, 0, "0 = never fade out");
  assert.equal(sim.saveRequests, 0, "a live change never asks for a save");
  const saved = await dev.writeLightsEx({ ...LED_DEFAULT, effect: 2, brightness: 100 }, { idleS: 0, save: true });
  assert.equal(ledStatusFlags(saved.status).pending, true);
  await dev.readLightsEx(); await dev.readLightsEx();
  assert.equal(ledStatusFlags((await dev.readLightsEx()).status).pending, false);
  assert.equal(sim.flash.effect, 2);
});

test("device layer: firmware without the extended form answers BAD_LENGTH, the plain form keeps working", async () => {
  const sim = new ExtSim({ ext: false });
  const dev = await deviceOn(sim);
  await assert.rejects(dev.readLightsEx(), (e) => e instanceof ConfigError && e.status === ST.BAD_LENGTH);
  assert.equal((await dev.readLights()).effect, LED_DEFAULT.effect);
  assert.equal((await dev.writeLights({ ...LED_DEFAULT, effect: 1 })).effect, 1);
});

test("device layer: a build that cannot keep the values refuses SAVE and applies nothing", async () => {
  const sim = new ExtSim({ persistent: false });
  const dev = await deviceOn(sim);
  await assert.rejects(dev.writeLightsEx({ ...LED_DEFAULT, effect: 1 }, { idleS: 5, save: true }), (e) => e.status === ST.UNSUPPORTED);
  assert.equal(sim.led.effect, LED_DEFAULT.effect);
});

// ---- the keeper against a fake device (fast, scriptable) ----

/** A device object with the same methods as KmfwDevice; `log` records every call. */
function fakeDev({ persistent = true, ext = true, restored = false, pollsToWrite = 2, failSave = false, failWriteOnSave = null, startStatus = 0 } = {}) {
  const state = { ...LED_DEFAULT, idleS: 600 };
  const k = { pending: !!(startStatus & 1), failed: false, polls: 0, log: [], saves: 0, live: 0 };
  const status = () => (k.pending ? 1 : 0) | (k.failed ? 2 : 0) | (restored ? 4 : 0) | (persistent ? 16 : 0);
  const reply = () => ({ ...state, status: status(), lastResult: 0, savesOk: 0, savesFailed: 0 });
  const refuse = () => Object.assign(new ConfigError("op refused: BAD_LENGTH", ST.BAD_LENGTH), {});
  const plain = (s) => ({ effect: s.effect, brightness: s.brightness, speed: s.speed, r: s.r, g: s.g, b: s.b });
  const dev = {
    k, state,
    async readLights() { k.log.push("plain-read"); return plain(state); },
    async writeLights(s) { k.log.push(["plain-write", s]); Object.assign(state, s); return plain(state); },
    async readLightsEx() {
      if (!ext) throw refuse();
      k.log.push("ext-read");
      if (k.pending && --k.polls <= 0) { k.pending = false; k.failed = failSave; }
      return reply();
    },
    async writeLightsEx(s, { idleS, save }) {
      if (!ext) throw refuse();
      if (save && failWriteOnSave) throw failWriteOnSave;
      if (save && !persistent) throw Object.assign(new ConfigError("op refused: UNSUPPORTED", ST.UNSUPPORTED));
      k.log.push(["ext-write", { ...s, idleS }, save]);
      Object.assign(state, s, { idleS });
      if (save) { k.saves++; k.pending = true; k.failed = false; k.polls = pollsToWrite; } else k.live++;
      return reply();
    },
  };
  if (startStatus & 1) k.polls = 2;
  return dev;
}
const keeperFor = (dev, o = {}) => new LedKeeper(dev, { saveDelayMs: 15, pollMs: 3, gapMs: 1, giveUpMs: 400, ...o });

test("keeper: loads the extended state; a keyboard that restored from flash says so", async () => {
  const dev = fakeDev({ restored: true });
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.equal(keeper.mode, "ext");
  assert.equal(keeper.hasIdle, true);
  assert.equal(keeper.state.idleS, 600);
  assert.deepEqual(keeper.save, { kind: "saved", restored: true });
  const plainSaved = keeperFor(fakeDev());
  await plainSaved.load();
  assert.deepEqual(plainSaved.save, { kind: "saved", restored: false });
});

test("keeper: dragging a slider sends live changes without SAVE, then exactly ONE SAVE after the person stops", async () => {
  const dev = fakeDev();
  const keeper = keeperFor(dev);
  await keeper.load();
  for (let v = 10; v <= 200; v += 10) { keeper.set({ brightness: v }); await wait(1); } // 20 steps, 1 ms apart: far below the save delay
  assert.equal(keeper.save.kind, "saving", "waiting for the person to stop counts as saving");
  assert.equal(dev.k.saves, 0, "no SAVE while still dragging");
  await until(() => keeper.save.kind === "saved", "saved");
  assert.equal(dev.k.saves, 1, "one SAVE for the whole drag");
  assert.ok(dev.k.live >= 1, "the changes went out live");
  assert.equal(dev.state.brightness, 200, "the SAVE carried the final value");
  const saveWrite = dev.k.log.find((e) => Array.isArray(e) && e[2] === true);
  assert.equal(saveWrite[1].brightness, 200);
  assert.ok(dev.k.log.filter((e) => e === "ext-read").length >= 2, "polled the extended LED_GET until the save was no longer pending");
});

test("keeper: the idle fade-out time travels with every change and is saved", async () => {
  const dev = fakeDev();
  const keeper = keeperFor(dev);
  await keeper.load();
  keeper.set({ idleS: 0 });
  await until(() => keeper.save.kind === "saved", "saved");
  assert.equal(dev.state.idleS, 0);
  keeper.set({ effect: 1 });
  await until(() => dev.k.saves === 2 && keeper.save.kind === "saved", "second save");
  assert.equal(dev.state.idleS, 0, "a colour change does not bring the idle time back to the default");
});

test("keeper: a change made while a save is being written gets its own SAVE afterwards", async () => {
  const dev = fakeDev({ pollsToWrite: 6 });
  const keeper = keeperFor(dev);
  await keeper.load();
  keeper.set({ effect: 2 });
  await until(() => dev.k.saves === 1, "first save sent");
  keeper.set({ speed: 77 });
  assert.equal(keeper.save.kind, "saving");
  await until(() => dev.k.saves === 2 && keeper.save.kind === "saved", "second save and saved");
  assert.equal(dev.state.speed, 77);
  assert.equal(dev.state.effect, 2);
});

test("keeper: the keyboard reports a failed write -> failed state, retry saves again", async () => {
  const dev = fakeDev({ failSave: true });
  const keeper = keeperFor(dev);
  await keeper.load();
  keeper.set({ effect: 4 });
  await until(() => keeper.save.kind === "failed", "failed");
  assert.equal(keeper.save.error, null, "the status bit, not an exception");
  assert.equal(dev.k.saves, 1, "no automatic retry loop");
  await wait(60);
  assert.equal(dev.k.saves, 1);
  // the flash works again
  const ok = fakeDev();
  const k2 = keeperFor(ok);
  await k2.load();
  k2.set({ effect: 4 });
  await until(() => k2.save.kind === "saved", "saved");
});

test("keeper: retrySave after a failure sends the SAVE once more", async () => {
  const dev = fakeDev({ failSave: true });
  const keeper = keeperFor(dev);
  await keeper.load();
  keeper.set({ effect: 4 });
  await until(() => keeper.save.kind === "failed", "failed");
  keeper.retrySave();
  assert.equal(keeper.save.kind, "saving");
  await until(() => dev.k.saves === 2 && keeper.save.kind === "failed", "second attempt, failed again");
  assert.equal(dev.k.saves, 2);
});

test("keeper: a SAVE that cannot be sent (error) is a failed state with the error, once", async () => {
  const boom = Object.assign(new Error("no reply"), { name: "TimeoutError" });
  const dev = fakeDev({ failWriteOnSave: boom });
  const keeper = keeperFor(dev);
  await keeper.load();
  keeper.set({ effect: 4 });
  await until(() => keeper.save.kind === "failed", "failed");
  assert.equal(keeper.save.error, boom);
  const writes = dev.k.log.length;
  await wait(60);
  assert.equal(dev.k.log.length, writes, "nothing is sent again by itself");
  // a new change tries again
  keeper.set({ effect: 5 });
  assert.equal(keeper.save.kind, "saving");
});

test("keeper: a save the keyboard never finishes gives up with a timeout, not an endless poll", async () => {
  const dev = fakeDev({ pollsToWrite: 1e9 });
  const keeper = keeperFor(dev, { giveUpMs: 30 });
  await keeper.load();
  keeper.set({ effect: 4 });
  await until(() => keeper.save.kind === "failed", "timeout");
  assert.equal(keeper.save.error.code, "save_timeout");
});

test("keeper: firmware without the extended form -> plain protocol, no idle time, no SAVE ever, honest state", async () => {
  const dev = fakeDev({ ext: false });
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.equal(keeper.mode, "plain");
  assert.equal(keeper.hasIdle, false);
  assert.deepEqual(keeper.save, { kind: "none", reason: "old" });
  keeper.set({ idleS: 5, brightness: 33 });
  await until(() => dev.k.log.some((e) => Array.isArray(e) && e[0] === "plain-write"), "plain write");
  assert.equal(dev.state.brightness, 33);
  assert.equal("idleS" in dev.k.log.find((e) => Array.isArray(e))[1], false);
  await wait(60);
  assert.equal(dev.k.saves, 0);
  assert.deepEqual(keeper.save, { kind: "none", reason: "old" });
});

test("keeper: a build without flash storage (status bit 4 clear) never gets a SAVE", async () => {
  const dev = fakeDev({ persistent: false });
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.deepEqual(keeper.save, { kind: "none", reason: "nopersist" });
  keeper.set({ effect: 3 });
  await until(() => dev.k.live === 1, "live write");
  await wait(60);
  assert.equal(dev.k.saves, 0);
  assert.equal(dev.state.effect, 3, "the live change still works");
});

test("keeper: a refusal of SAVE (UNSUPPORTED) although the bit said persistent ends as 'cannot keep'", async () => {
  const dev = fakeDev({ persistent: true });
  const keeper = keeperFor(dev);
  await keeper.load();
  dev.writeLightsEx = ((orig) => async (s, o) => { if (o.save) throw Object.assign(new ConfigError("refused", ST.UNSUPPORTED)); return orig(s, o); })(dev.writeLightsEx);
  keeper.set({ effect: 3 });
  await until(() => keeper.save.kind === "none", "none");
  assert.equal(keeper.save.reason, "nopersist");
});

test("keeper: a save that was already pending when the page opened is followed to its end", async () => {
  const dev = fakeDev({ startStatus: 1 });
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.equal(keeper.save.kind, "saving");
  await until(() => keeper.save.kind === "saved", "saved");
  assert.equal(dev.k.saves, 0, "it only watched, it did not send another SAVE");
});

test("keeper: leaving the page with an unsaved change sends the SAVE at once; with nothing to save sends nothing", async () => {
  const dev = fakeDev();
  const keeper = keeperFor(dev, { saveDelayMs: 5000 });
  await keeper.load();
  keeper.set({ effect: 1, speed: 20 });
  await until(() => dev.k.live >= 1, "live write");
  keeper.dispose();
  await until(() => dev.k.saves === 1, "save on leaving");
  assert.equal(dev.state.speed, 20);

  const clean = fakeDev();
  const k2 = keeperFor(clean);
  await k2.load();
  k2.dispose();
  await wait(20);
  assert.equal(clean.k.saves, 0);
  k2.set({ effect: 2 });
  await wait(30);
  assert.equal(clean.k.live + clean.k.saves, 0, "a closed keeper sends nothing");
});

test("keeper: onChange fires for the page to repaint (set, live reply, save progress)", async () => {
  const dev = fakeDev();
  let n = 0;
  const keeper = keeperFor(dev, { onChange: () => { n++; } });
  await keeper.load();
  const before = n;
  keeper.set({ effect: 2 });
  await until(() => keeper.save.kind === "saved", "saved");
  assert.ok(n - before >= 3);
});

test("keeper over the real device layer and an extended simulated keyboard: drag, one save, flash holds the final lights", async () => {
  const sim = new ExtSim({ pollsToWrite: 2 });
  const dev = await deviceOn(sim);
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.equal(keeper.mode, "ext");
  for (let i = 1; i <= 15; i++) { keeper.set({ brightness: i * 10 }); await wait(1); }
  keeper.set({ idleS: 300 });
  await until(() => keeper.save.kind === "saved", "saved", 4000);
  assert.equal(sim.saveRequests, 1);
  assert.equal(sim.flash.brightness, 150);
  assert.equal(sim.flash.idleS, 300);
  assert.ok(sim.liveSets >= 1);
});

test("keeper over the real device layer against a keyboard with old firmware (plain forms only)", async () => {
  const sim = new ExtSim({ ext: false });
  const dev = await deviceOn(sim);
  const keeper = keeperFor(dev);
  await keeper.load();
  assert.equal(keeper.mode, "plain");
  keeper.set({ effect: 2 });
  await until(() => !keeper.pending && sim.led.effect === 2, "live change");
  assert.deepEqual(keeper.save, { kind: "none", reason: "old" });
});
