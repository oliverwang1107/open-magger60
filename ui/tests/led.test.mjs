// The keyboard's lights (LED_GET 0x20 / LED_SET 0x21): wire format, client, device, the coalescing sender and the page's decisions.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { OP, ST, FEAT, buildRequest, decodeReply, parseInfo, SafetyError, ConfigError } from "../js/device/config_protocol.js";
import { ConfigClient } from "../js/device/ConfigClient.js";
import { KmfwDevice } from "../js/device/KmfwDevice.js";
import { MockDevice } from "../js/device/MockDevice.js";
import { UnsupportedError } from "../js/device/Device.js";
import { classifyProblem } from "../js/device/errors.js";
import { loadLayout } from "../js/core/layout.js";
import {
  LED_EFFECTS, LED_DEFAULT, ledEffectById, ledAvailability, isValidLed, encodeLed, decodeLed, clampLed, previewLed,
  brightnessToPct, pctToBrightness, speedToStep, stepToSpeed, withLedColor, ledColorHex,
} from "../js/core/led.js";
import { LedLink } from "../js/core/led_link.js";
import { SimKeyboard } from "./helpers/sim-keyboard.mjs";
import zh from "../js/locales/zh-TW.js";
import en from "../js/locales/en.js";

globalThis.window ??= new EventTarget();
const hex = (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
const unhex = (h) => Uint8Array.from(h.match(/../g), (x) => parseInt(x, 16));
const padded = (h) => unhex(h.padEnd(128, "0"));
const SIX = { effect: 3, brightness: 200, speed: 90, r: 1, g: 2, b: 3 };
const withLights = (o = {}) => new SimKeyboard({ features: 0x7f, ...o });
async function client(o) {
  const sim = withLights(o);
  const c = new ConfigClient(sim);
  await c.verify();
  return { sim, c };
}

// ---- wire format ----

test("LED_SET / LED_GET packets are the worked example of CONFIG_PROTOCOL.md, byte for byte", () => {
  assert.deepEqual(buildRequest(OP.LED_SET, 0x7e, encodeLed(SIX)), padded("55c600d208000000" + "217e03c85a010203"));
  const sim = withLights();
  assert.deepEqual(sim.process(padded("55c600d208000000" + "217e03c85a010203")), padded("aac600d309000000" + "217e0003c85a010203"));
  assert.deepEqual(decodeReply(sim.process(buildRequest(OP.LED_GET, 5))).payload, encodeLed(SIX));
  // the example is also in the specification text
  const spec = readFileSync(fileURLToPath(new URL("../../CONFIG_PROTOCOL.md", import.meta.url)), "utf8");
  assert.ok(spec.includes("req  55 c6 00 d2 08 00 00 00 | 21 7e 03 c8 5a 01 02 03"));
});

test("LED_GET has no arguments; LED_SET needs exactly six and a valid effect and speed, checked before anything is sent", () => {
  assert.equal(buildRequest(OP.LED_GET, 1)[4], 2);
  assert.throws(() => buildRequest(OP.LED_GET, 1, Uint8Array.of(0)), SafetyError);
  assert.throws(() => buildRequest(OP.LED_SET, 1, Uint8Array.of(1, 2, 3)), SafetyError);
  assert.throws(() => buildRequest(OP.LED_SET, 1, Uint8Array.of(10, 255, 10, 0, 0, 0)), /effect 0\.\.9/);
  assert.throws(() => buildRequest(OP.LED_SET, 1, Uint8Array.of(1, 255, 0, 0, 0, 0)), /speed 1\.\.255/);
  assert.ok(buildRequest(OP.LED_SET, 1, Uint8Array.of(6, 0, 1, 255, 255, 255)));
});

test("GET_INFO feature bit 6 is the lights", () => {
  assert.equal(FEAT.LED, 0x40);
  const info = (features) => parseInfo(decodeReply(new SimKeyboard({ features }).process(buildRequest(OP.GET_INFO, 1))).payload);
  assert.ok(info(0x7f).featureNames.includes("lights"));
  assert.ok(!info(0x3f).featureNames.includes("lights"));
});

test("encode / decode / validate", () => {
  assert.deepEqual(encodeLed(SIX), Uint8Array.of(3, 200, 90, 1, 2, 3));
  assert.deepEqual(decodeLed(encodeLed(SIX)), SIX);
  assert.ok(isValidLed(LED_DEFAULT));
  for (const bad of [{ ...SIX, effect: 10 }, { ...SIX, effect: -1 }, { ...SIX, speed: 0 }, { ...SIX, brightness: 256 }, { ...SIX, r: 1.5 }, { ...SIX, g: "9" }, null]) {
    assert.ok(!isValidLed(bad), JSON.stringify(bad));
    assert.throws(() => encodeLed(bad), RangeError);
  }
  assert.throws(() => decodeLed(new Uint8Array(5)), /6 bytes/);
  assert.throws(() => decodeLed(Uint8Array.of(10, 0, 1, 0, 0, 0)), /outside/);
  assert.deepEqual(clampLed({ effect: 99, brightness: -4, speed: 0, r: 300, g: 2.6, b: "x" }), { effect: 9, brightness: 0, speed: 1, r: 255, g: 3, b: 0 });
});

test("effect table: ten effects in wire order, each with what it uses", () => {
  assert.deepEqual(LED_EFFECTS.map((e) => e.id), ["off", "static", "breath", "rainbow", "reactive", "depth", "demo", "custom", "external", "anim"]);
  const uses = (id) => [ledEffectById(id).color, ledEffectById(id).speed];
  assert.deepEqual(uses("static"), [true, false]);
  assert.deepEqual(uses("rainbow"), [false, true]);
  assert.deepEqual(uses("depth"), [false, false]);
  assert.deepEqual(uses("off"), [false, false]);
  assert.deepEqual(uses("demo"), [true, true]);
});

test("friendly scales round-trip and stay inside the protocol's ranges", () => {
  for (let p = 0; p <= 100; p++) assert.equal(brightnessToPct(pctToBrightness(p)), p, `brightness ${p}`);
  for (let n = 1; n <= 100; n++) {
    const b = stepToSpeed(n);
    assert.ok(b >= 1 && b <= 255);
    assert.equal(speedToStep(b), n, `speed ${n}`);
  }
  assert.equal(pctToBrightness(100), 255);
  assert.equal(stepToSpeed(1), 1);
  assert.equal(stepToSpeed(100), 255);
  assert.equal(speedToStep(1), 1);
  assert.equal(speedToStep(255), 100);
  assert.equal(speedToStep(LED_DEFAULT.speed), 51);
});

test("colour helpers", () => {
  const s = withLedColor(SIX, "#ff8000");
  assert.deepEqual([s.r, s.g, s.b], [255, 128, 0]);
  assert.equal(ledColorHex(s), "#ff8000");
  assert.equal(withLedColor(SIX, "nonsense"), SIX);
});

test("preview: off is dark, static is the colour scaled by brightness, reactive lights only a pressed key", () => {
  const at = { x: 3, tSec: 1, depth: 0 };
  assert.deepEqual(previewLed({ ...SIX, effect: 0 }, at), [0, 0, 0]);
  assert.deepEqual(previewLed({ effect: 1, brightness: 255, speed: 1, r: 10, g: 20, b: 30 }, at), [10, 20, 30]);
  assert.deepEqual(previewLed({ effect: 1, brightness: 0, speed: 1, r: 10, g: 20, b: 30 }, at), [0, 0, 0]);
  assert.deepEqual(previewLed({ effect: 4, brightness: 255, speed: 100, r: 9, g: 9, b: 9 }, at), [0, 0, 0]);
  assert.ok(previewLed({ effect: 4, brightness: 255, speed: 100, r: 90, g: 90, b: 90 }, { ...at, depth: 1 })[0] > 0);
  const hot = previewLed({ ...SIX, effect: 5, brightness: 255 }, { ...at, depth: 1 });
  assert.ok(hot[0] > hot[2], "bottomed out is red");
  const fresh = previewLed({ ...SIX, effect: 5, brightness: 255 }, { ...at, depth: 0.1 });
  assert.ok(fresh[2] > fresh[0], "shallow is blue");
  const rainbowA = previewLed({ ...SIX, effect: 3, brightness: 255 }, { x: 0, tSec: 0 });
  const rainbowB = previewLed({ ...SIX, effect: 3, brightness: 255 }, { x: 5, tSec: 0 });
  assert.notDeepEqual(rainbowA, rainbowB);
  // demo cycles through the other effects: second 0 is the rainbow, second 9 reactive (dark without a press)
  assert.deepEqual(previewLed({ ...SIX, effect: 6, brightness: 255 }, { x: 5, tSec: 0 }), rainbowB);
  assert.deepEqual(previewLed({ ...SIX, effect: 6, brightness: 255 }, { x: 5, tSec: 9, depth: 0 }), [0, 0, 0]);
});

// ---- client against the simulated keyboard ----

test("client: LED_GET returns the firmware's defaults; LED_SET returns the values now in force", async () => {
  const { sim, c } = await client();
  assert.deepEqual(await c.ledGet(), LED_DEFAULT);
  assert.deepEqual(await c.ledSet(SIX), SIX);
  assert.deepEqual(await c.ledGet(), SIX);
  assert.deepEqual(sim.led, SIX);
});

test("client: the lights never touch settings, flash or the commit counter", async () => {
  const { sim, c } = await client();
  const blob = Uint8Array.from(sim.blob);
  await c.ledSet(SIX);
  await c.ledSet({ ...SIX, effect: 1 });
  await c.ledGet();
  assert.equal(sim.flashWrites, 0);
  assert.equal(sim.commits, 0);
  assert.equal(sim.state, 0);
  assert.deepEqual(sim.blob, blob);
  assert.equal(sim.ledSets, 2);
  // only the version query, GET_INFO and the two light ops went over the wire
  assert.deepEqual([...new Set(sim.seen.map(([cmd, op]) => `${cmd}/${op}`))].sort(), ["198/1", "198/32", "198/33", "3/null"].sort());
});

test("client: a keyboard without feature bit 6 answers UNSUPPORTED, and an invalid state is refused before it is sent", async () => {
  const { sim, c } = await client({ features: 0x3f });
  await assert.rejects(c.ledGet(), (e) => e instanceof ConfigError && e.status === ST.UNSUPPORTED);
  await assert.rejects(c.ledSet(SIX), (e) => e.status === ST.UNSUPPORTED);
  const lights = await client();
  const seen = lights.sim.seen.length;
  await assert.rejects(lights.c.ledSet({ ...SIX, speed: 0 }), RangeError);
  await assert.rejects(lights.c.ledSet({ ...SIX, effect: 10 }), RangeError);
  assert.equal(lights.sim.seen.length, seen, "nothing was sent");
  void sim;
});

test("simulated keyboard: RANGE for effect > 9 or speed 0 changes nothing; BAD_LENGTH for wrong sizes", async () => {
  const sim = withLights();
  const send = (op, args) => decodeReply(sim.process(buildRequest(op, 1, args)));
  // buildRequest refuses these, so go around it the way a broken host would: raw packets
  const raw = (op, args) => {
    const p = new Uint8Array(64);
    const data = Uint8Array.of(op, 1, ...args);
    p[0] = 0x55; p[1] = 0xc6; p[4] = data.length; p.set(data, 8);
    p[3] = data.reduce((a, b) => a + b, data.length) & 255;
    return decodeReply(sim.process(p));
  };
  assert.equal(raw(OP.LED_SET, [10, 1, 1, 1, 1, 1]).status, ST.RANGE);
  assert.equal(raw(OP.LED_SET, [1, 1, 0, 1, 1, 1]).status, ST.RANGE);
  assert.equal(raw(OP.LED_SET, [1, 1, 1]).status, ST.BAD_LENGTH);
  assert.equal(raw(OP.LED_GET, [1]).status, ST.BAD_LENGTH);
  assert.deepEqual(sim.led, LED_DEFAULT);
  assert.equal(send(OP.LED_GET, new Uint8Array(0)).status, ST.OK);
});

// ---- device ----

async function device(simOpts = {}) {
  const sim = withLights(simOpts);
  const dev = new KmfwDevice({ hidApi: undefined });
  await dev.connectTransport(sim);
  return { sim, dev };
}

test("device: features.lights follows feature bit 6; read / write work without arming", async () => {
  const { dev, sim } = await device();
  assert.equal(dev.features.lights, true);
  assert.equal(dev.armed, false);
  assert.deepEqual(await dev.readLights(), LED_DEFAULT);
  assert.deepEqual(await dev.writeLights(SIX), SIX);
  assert.equal(dev.readOnly, true, "writing the lights does not arm settings writes");
  assert.deepEqual(sim.led, SIX);
});

test("device: without the feature bit the calls throw UnsupportedError and send nothing", async () => {
  const { dev, sim } = await device({ features: 0x3f });
  assert.equal(dev.features.lights, false);
  const seen = sim.seen.length;
  await assert.rejects(dev.readLights(), UnsupportedError);
  await assert.rejects(dev.writeLights(SIX), UnsupportedError);
  assert.equal(sim.seen.length, seen);
  assert.equal(ledAvailability(dev), "old");
});

test("device: an unplugged keyboard turns into 'lost' like every other operation", async () => {
  const { dev, sim } = await device();
  sim.unplugged = true;
  await assert.rejects(dev.writeLights(SIX), (e) => classifyProblem(e).kind === "lost");
  assert.equal(dev.status, "error");
});

test("demo keyboard: has lights by default, `lights: false` models older firmware", async () => {
  const layout = loadLayout();
  const withIt = new MockDevice(layout, { persist: false, health: false });
  const without = new MockDevice(layout, { persist: false, health: false, lights: false });
  for (const m of [withIt, without]) { await m.connectTransport(m.sim); }
  assert.equal(withIt.features.lights, true);
  assert.deepEqual(await withIt.readLights(), LED_DEFAULT);
  assert.equal(without.features.lights, false);
  assert.equal(ledAvailability(without), "old");
  assert.equal(ledAvailability(withIt), "ok");
});

test("availability: offline, stock firmware, old firmware, ok", () => {
  assert.equal(ledAvailability(null), "offline");
  assert.equal(ledAvailability({ status: "disconnected", kind: "kmfw", features: { lights: true } }), "offline");
  assert.equal(ledAvailability({ status: "error", kind: "kmfw", features: { lights: true } }), "offline");
  assert.equal(ledAvailability({ status: "connected", kind: "webhid" }), "stock");
  assert.equal(ledAvailability({ status: "connected", kind: "kmfw", features: { lights: false } }), "old");
  assert.equal(ledAvailability({ status: "connected", kind: "mock", features: {} }), "old");
  assert.equal(ledAvailability({ status: "connected", kind: "kmfw", features: { lights: true } }), "ok");
});

// ---- the sender behind the sliders ----

const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; };
const flush = () => new Promise((r) => setImmediate(r));

test("LedLink: load shows the keyboard's values; set updates the page at once", async () => {
  const seen = [];
  const link = new LedLink({ read: async () => SIX, write: async (s) => s, onChange: (s) => seen.push(s), sleepFn: async () => {} });
  assert.equal(link.loaded, false);
  assert.equal(link.state, null);
  link.set({ speed: 5 }); // before load: ignored
  assert.equal(seen.length, 0);
  await link.load();
  assert.deepEqual(link.state, SIX);
  link.set({ speed: 5 });
  assert.equal(link.state.speed, 5);
  assert.equal(link.pending, true);
  await flush();
  assert.equal(link.pending, false);
  assert.deepEqual(link.state, { ...SIX, speed: 5 });
});

test("LedLink: only one request is in flight; changes made meanwhile are merged and the newest goes out last", async () => {
  const writes = [];
  const gates = [];
  const link = new LedLink({
    read: async () => SIX,
    write: (s) => { writes.push(s); const d = deferred(); gates.push(d); return d.promise; },
    sleepFn: async () => {},
  });
  await link.load();
  link.set({ brightness: 10 });
  await flush();
  assert.equal(writes.length, 1);
  link.set({ brightness: 20 });
  link.set({ brightness: 30 });
  link.set({ r: 9 });
  await flush();
  assert.equal(writes.length, 1, "no second request while the first is on the wire");
  gates[0].resolve(writes[0]);
  await flush();
  assert.equal(writes.length, 2);
  assert.deepEqual(writes[1], { ...SIX, brightness: 30, r: 9 }, "the intermediate values were skipped");
  assert.deepEqual(link.state, { ...SIX, brightness: 30, r: 9 }, "the page keeps showing what was asked for while it is in flight");
  gates[1].resolve(writes[1]);
  await flush();
  assert.equal(link.pending, false);
});

test("LedLink: what the keyboard answers wins when nothing newer was asked for", async () => {
  const link = new LedLink({ read: async () => SIX, write: async (s) => ({ ...s, brightness: 140 }), sleepFn: async () => {} });
  await link.load();
  link.set({ brightness: 255 });
  await flush();
  assert.equal(link.state.brightness, 140);
});

test("LedLink: a failed write is reported once, the wanted state is kept, retry sends it again", async () => {
  let fail = true;
  const errors = [];
  const writes = [];
  const link = new LedLink({
    read: async () => SIX,
    write: async (s) => { writes.push(s); if (fail) throw new ConfigError("op 33 refused: BUSY", ST.BUSY); return s; },
    onError: (e) => errors.push(e), sleepFn: async () => {},
  });
  await link.load();
  link.set({ effect: 1 });
  await flush();
  assert.equal(errors.length, 1);
  assert.equal(link.error, errors[0]);
  assert.equal(link.state.effect, 1, "the page still shows what was asked for");
  assert.equal(link.pending, true);
  fail = false;
  link.retry();
  await flush();
  assert.equal(writes.length, 2);
  assert.equal(link.error, null);
  assert.equal(link.pending, false);
  // a new change after an error also tries again
  fail = true;
  link.set({ effect: 2 });
  await flush();
  fail = false;
  link.set({ effect: 3 });
  await flush();
  assert.equal(link.error, null);
  assert.equal(link.state.effect, 3);
});

test("LedLink: after close nothing more is sent; a load error rejects", async () => {
  const writes = [];
  const link = new LedLink({ read: async () => SIX, write: async (s) => { writes.push(s); return s; }, sleepFn: async () => {} });
  await link.load();
  link.close();
  link.set({ speed: 9 });
  await flush();
  assert.equal(writes.length, 0);
  const bad = new LedLink({ read: async () => { throw new ConfigError("no answer", null, "timeout"); }, write: async (s) => s });
  await assert.rejects(bad.load(), /no answer/);
  assert.equal(bad.loaded, false);
});

test("LedLink against the simulated keyboard: a burst of slider moves ends with the last value on the keyboard", async () => {
  const { sim, dev } = await device();
  const link = new LedLink({ read: () => dev.readLights(), write: (s) => dev.writeLights(s), gapMs: 1 });
  await link.load();
  for (let v = 0; v <= 255; v += 5) link.set({ brightness: v });
  link.set({ brightness: 77 });
  while (link.pending) await new Promise((r) => setTimeout(r, 5));
  assert.equal(sim.led.brightness, 77);
  assert.ok(sim.ledSets < 20, `${sim.ledSets} requests for 53 moves`);
  assert.equal(sim.flashWrites, 0);
});

// ---- texts ----

test("every light text exists in both languages, for every effect", () => {
  for (const e of LED_EFFECTS) for (const k of [`led.fx.${e.id}`, `led.fxd.${e.id}`]) assert.ok(k in zh && k in en, k);
  for (const k of ["offline", "stock", "old"]) for (const part of ["title", "body"]) assert.ok(`led.na.${k}.${part}` in zh && `led.na.${k}.${part}` in en, k);
  assert.match(zh["led.save.none.old"], /重新啟動/);
  assert.match(en["led.save.none.old"], /restart/);
});
