// Lights per key profile (firmware: km_ledprof.h, CONFIG_PROTOCOL.md "per-profile form"): the codec and wire rules of LED_GET with two
// argument bytes / LED_SET with ten, the device layer against the simulated keyboard (profile switch shows that profile's lights), and the
// LedKeeper rules (live profile: live writes + save to that profile; other profile: nothing live, only a save).
import test from "node:test";
import assert from "node:assert/strict";
import { encodeLedEx, ledStatusFlags, LED_STATUS, LED_PROFILE_ACTIVE, isLedProfile } from "../js/core/led_ex.js";
import { LedKeeper } from "../js/core/led_keep.js";
import { LED_DEFAULT } from "../js/core/led.js";
import { buildRequest, OP, ST, SafetyError, ConfigError, FEAT } from "../js/device/config_protocol.js";
import { KmfwDevice } from "../js/device/KmfwDevice.js";
import { MockDevice } from "../js/device/MockDevice.js";
import { SimKeyboard } from "../js/device/sim_keyboard.js";
import { loadLayout } from "../js/core/layout.js";
import { decodeSettings } from "../js/core/settings_codec.js";
import { defaultWorkspace, workspaceToBlob } from "../js/core/profile_codec.js";
import { rawCodec } from "../js/core/raw_codec.js";
import { FW_SETTINGS } from "../js/core/limits.js";
import { WORD } from "../js/device/config_protocol.js";

globalThis.window ??= new EventTarget();
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(cond, what, ms = 2000) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) assert.fail(`timed out waiting for: ${what}`);
    await wait(2);
  }
}

const layout = loadLayout();
const factory = () => workspaceToBlob(defaultWorkspace(layout), layout).bytes;
const validate = (b) => { try { decodeSettings(b); return true; } catch { return false; } };
async function deviceOn(sim) {
  const dev = new KmfwDevice({ hidApi: undefined, settingsSize: FW_SETTINGS.size });
  dev.attachSettingsCodec(rawCodec);
  await dev.connectTransport(sim);
  return dev;
}
const simWith = (features) => new SimKeyboard({ blob: factory(), active: 0, validate, maxSettings: 8192, features });

// ---- codec and wire rules ----

test("per-profile LED_SET: ten bytes, the profile last; 255 = the profile in force", () => {
  const s = { ...LED_DEFAULT, effect: 3, brightness: 200, speed: 90, r: 1, g: 2, b: 3 };
  assert.deepEqual([...encodeLedEx(s, { idleS: 600, save: true, profile: 2 })], [3, 200, 90, 1, 2, 3, 0x58, 0x02, 1, 2]);
  assert.equal(encodeLedEx(s, { idleS: 600, profile: LED_PROFILE_ACTIVE })[9], 255);
  assert.equal(encodeLedEx(s, { idleS: 600 }).length, 9, "without a profile it stays the nine-byte form");
  assert.equal(encodeLedEx(s, { idleS: 600, profile: null }).length, 9);
  for (const bad of [4, -1, 254, 1.5, "1"]) assert.throws(() => encodeLedEx(s, { idleS: 0, profile: bad }), RangeError, String(bad));
  assert.equal(isLedProfile(0) && isLedProfile(3) && isLedProfile(255), true);
  assert.equal(isLedProfile(4), false);
});

test("status bit 5 = this profile has lights of its own", () => {
  assert.equal(LED_STATUS.PROFILE_OWN, 32);
  assert.equal(ledStatusFlags(0x30).profileOwn, true);
  assert.equal(ledStatusFlags(0x10).profileOwn, false);
  assert.equal(FEAT.LED_PROFILE, 0x800);
});

test("wire rules: the profile forms are allowed, bad profiles and near misses are refused before sending", () => {
  const ok = (op, args) => assert.doesNotThrow(() => buildRequest(op, 1, Uint8Array.from(args)));
  const bad = (op, args) => assert.throws(() => buildRequest(op, 1, Uint8Array.from(args)), SafetyError);
  ok(OP.LED_GET, [1, 0]); ok(OP.LED_GET, [1, 3]); ok(OP.LED_GET, [1, 255]);
  bad(OP.LED_GET, [1, 4]); bad(OP.LED_GET, [1, 254]); bad(OP.LED_GET, [0, 1]); bad(OP.LED_GET, [1, 0, 0]);
  const nine = [1, 2, 3, 4, 5, 6, 0, 0, 1];
  ok(OP.LED_SET, [...nine, 0]); ok(OP.LED_SET, [...nine, 3]); ok(OP.LED_SET, [...nine, 255]);
  bad(OP.LED_SET, [...nine, 4]); bad(OP.LED_SET, [...nine, 128]);
  bad(OP.LED_SET, [1, 2, 3, 4, 5, 6, 0, 0, 2, 0]);          // unknown flag bit, also in the long form
  bad(OP.LED_SET, [...nine, 0, 0]);                          // eleven bytes
});

// ---- the device layer against the simulated keyboard ----

test("device layer: lights of a profile are stored per profile and shown when the profile is selected", async () => {
  const sim = simWith(0xfff);
  const dev = await deviceOn(sim);
  assert.equal(dev.features.lightProfiles, true);

  // profile 0 is active: a saved change applies now and is profile 0's own
  const mine = { ...LED_DEFAULT, effect: 2, brightness: 100, r: 255, g: 0, b: 0 };
  const a = await dev.writeLightsEx(mine, { idleS: 60, save: true, profile: 0 });
  assert.equal(a.effect, 2);
  assert.equal(ledStatusFlags(a.status).profileOwn, true);
  assert.equal(sim.led.effect, 2, "applied live to the active profile");

  // profile 1 is not active: a save stores it WITHOUT touching the lights now; without save it is refused
  const other = { ...LED_DEFAULT, effect: 3, r: 0, g: 0, b: 255 };
  await assert.rejects(dev.writeLightsEx(other, { idleS: 0, save: false, profile: 1 }), (e) => e instanceof ConfigError && e.status === ST.RANGE);
  const b = await dev.writeLightsEx(other, { idleS: 0, save: true, profile: 1 });
  assert.equal(b.effect, 3);
  assert.equal(b.b, 255);
  assert.equal(sim.led.effect, 2, "the keyboard still shows profile 0");

  // reading each profile
  assert.equal((await dev.readLightsEx(0)).r, 255);
  assert.equal((await dev.readLightsEx(1)).b, 255);
  const p2 = await dev.readLightsEx(2);
  assert.equal(p2.effect, LED_DEFAULT.effect, "no entry: the shared lights (the defaults here)");
  assert.equal(ledStatusFlags(p2.status).profileOwn, false);
  assert.equal((await dev.readLightsEx(LED_PROFILE_ACTIVE)).r, 255);

  // switching the keyboard's profile switches the lights
  await dev.selectProfile(1, WORD.SELECT_PROFILE);
  assert.equal(sim.led.effect, 3);
  assert.equal((await dev.readLightsEx(LED_PROFILE_ACTIVE)).b, 255);
  await dev.selectProfile(2, WORD.SELECT_PROFILE);
  assert.equal(sim.led.effect, LED_DEFAULT.effect);
  await dev.selectProfile(0, WORD.SELECT_PROFILE);
  assert.equal(sim.led.r, 255);

  // a profile that does not exist
  await assert.rejects(dev.readLightsEx(4), (e) => e instanceof SafetyError || e instanceof RangeError || (e instanceof ConfigError));
});

test("device layer: firmware without per-profile lights refuses a profile argument before sending", async () => {
  const sim = simWith(0x7ff);
  const dev = await deviceOn(sim);
  assert.equal(dev.features.lightProfiles, false);
  await assert.rejects(dev.readLightsEx(1), /one set of lights/);
  await assert.rejects(dev.writeLightsEx(LED_DEFAULT, { idleS: 0, save: true, profile: 1 }), /one set of lights/);
  assert.equal(sim.seen.filter(([, op]) => op === OP.LED_GET || op === OP.LED_SET).length, 0);
});

test("the demo keyboard has the per-profile lights", async () => {
  const dev = new MockDevice(layout, { persist: false, health: false });
  await dev.connect();
  assert.equal(dev.features.lightProfiles, true);
  const r = await dev.writeLightsEx({ ...LED_DEFAULT, effect: 1 }, { idleS: 0, save: true, profile: 3 });
  assert.equal(r.effect, 1);
  assert.equal((await dev.readLightsEx(3)).effect, 1);
  dev.disconnect?.();
});

// ---- the keeper ----

function fakeDev() {
  const log = [];
  const state = { ...LED_DEFAULT, idleS: 600 };
  const own = new Set();
  const reply = (p) => ({ ...state, status: 0x10 | (own.has(p) ? 0x20 : 0), lastResult: 0, savesOk: 0, savesFailed: 0 });
  return {
    log,
    features: { lightProfiles: true },
    async readLights() { throw new Error("plain read must not be used"); },
    async writeLights() { throw new Error("plain write must not be used"); },
    async readLightsEx(profile) { log.push(["read", profile]); return reply(profile); },
    async writeLightsEx(s, { idleS, save, profile }) {
      log.push(["write", profile, save, s.effect]);
      Object.assign(state, s, { idleS });
      if (save) own.add(profile === LED_PROFILE_ACTIVE ? 0 : profile);
      return reply(profile);
    },
  };
}
const keeperFor = (dev, o) => new LedKeeper(dev, { saveDelayMs: 15, pollMs: 3, gapMs: 1, giveUpMs: 400, ...o });

test("keeper, live profile: live writes go to 'the profile in force', the save names the profile", async () => {
  const dev = fakeDev();
  const k = keeperFor(dev, { profile: 2, live: true });
  await k.load();
  assert.deepEqual(dev.log[0], ["read", 2]);
  assert.equal(k.ownLights, false, "no entry yet: it follows the shared lights");
  k.set({ effect: 4 });
  await until(() => dev.log.some((e) => e[0] === "write" && e[2] === true), "the save");
  const writes = dev.log.filter((e) => e[0] === "write");
  assert.deepEqual(writes[0], ["write", 255, false, 4], "live: profile 255");
  assert.deepEqual(writes.at(-1), ["write", 2, true, 4], "save: the explicit profile");
  await until(() => k.save.kind === "saved", "saved");
  assert.equal(k.ownLights, true);
  k.dispose();
});

test("keeper, other profile: nothing goes out live, one save to that profile", async () => {
  const dev = fakeDev();
  const k = keeperFor(dev, { profile: 1, live: false });
  await k.load();
  k.set({ effect: 5 });
  k.set({ brightness: 99 });
  assert.equal(k.state.effect, 5, "the page shows the change at once");
  await until(() => k.save.kind === "saved" && k.ownLights, "saved");
  const writes = dev.log.filter((e) => e[0] === "write");
  assert.equal(writes.length, 1, "no live write, a single save");
  assert.deepEqual(writes[0], ["write", 1, true, 5]);
  k.dispose();
});

test("keeper, other profile on a build that cannot store: edits are ignored", async () => {
  const dev = fakeDev();
  dev.readLightsEx = async (p) => ({ ...LED_DEFAULT, idleS: 600, status: 0x00, lastResult: 0, savesOk: 0, savesFailed: 0 });   // not PERSISTENT
  const k = keeperFor(dev, { profile: 1, live: false });
  await k.load();
  assert.equal(k.canSave, false);
  k.set({ effect: 5 });
  assert.notEqual(k.state.effect, 5);
  assert.equal(dev.log.filter((e) => e[0] === "write").length, 0);
  k.dispose();
});

test("keeper, dispose with an unsaved change saves it to the bound profile", async () => {
  const dev = fakeDev();
  const k = keeperFor(dev, { profile: 3, live: true, saveDelayMs: 5000 });
  await k.load();
  k.set({ speed: 40 });
  k.dispose();
  await until(() => dev.log.some((e) => e[0] === "write" && e[2] === true), "the save at dispose");
  assert.deepEqual(dev.log.filter((e) => e[2] === true).at(-1).slice(0, 3), ["write", 3, true]);
});

test("keeper without a profile keeps the older behaviour (nine bytes, no profile argument)", async () => {
  const dev = fakeDev();
  const k = keeperFor(dev);
  await k.load();
  assert.deepEqual(dev.log[0], ["read", null]);
  k.set({ effect: 1 });
  await until(() => k.save.kind === "saved", "saved");
  assert.ok(dev.log.filter((e) => e[0] === "write").every((e) => e[1] === null));
  assert.equal(k.ownLights, true);
  k.dispose();
});
