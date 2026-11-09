import test from "node:test";
import assert from "node:assert/strict";
import { KmfwDevice, NotArmedError } from "../js/device/KmfwDevice.js";
import { WebHidDevice } from "../js/device/WebHidDevice.js";
import { MockDevice } from "../js/device/MockDevice.js";
import { Device, UnsupportedError } from "../js/device/Device.js";
import { createDevice, probeFirmware, DEVICE_KINDS } from "../js/device/index.js";
import { loadLayout } from "../js/core/layout.js";
import { ConfigError } from "../js/device/config_protocol.js";
import { SimKeyboard, FakeHid, FakeHidApi } from "./helpers/sim-keyboard.mjs";

const pattern = (n, seed = 5) => Uint8Array.from({ length: n }, (_, i) => (i * 11 + seed) & 255);
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

async function connected(simOpts) {
  const sim = new SimKeyboard(simOpts);
  const hid = new FakeHid(sim);
  const api = new FakeHidApi([hid]);
  const dev = new KmfwDevice({ hidApi: api });
  await dev.connect();
  return { sim, hid, api, dev };
}

test("connect: verifies the firmware, fills info and capabilities", async () => {
  const { dev, hid } = await connected();
  assert.ok(dev instanceof Device);
  assert.equal(dev.kind, "kmfw");
  assert.equal(dev.status, "connected");
  assert.match(dev.info.build, /KMFW-SIM$/);
  assert.equal(dev.info.firmwareVersion, "104");
  assert.equal(dev.info.vid, 0x19f5);
  assert.equal(dev.capabilities.live, true);
  assert.equal(dev.capabilities.readProfile, false); // no settings codec yet
  assert.equal(dev.capabilities.writeProfile, false);
  assert.equal(dev.readOnly, true);
  assert.ok(hid.opened);
  await dev.disconnect();
  assert.equal(dev.status, "disconnected");
  assert.equal(hid.opened, false);
});

test("a stock-firmware keyboard is refused: only the version query is ever sent, the device ends in error", async () => {
  const sim = new SimKeyboard({ stock: true });
  const hid = new FakeHid(sim);
  const dev = new KmfwDevice({ hidApi: new FakeHidApi([hid]) });
  await assert.rejects(dev.connect(), /not our firmware/);
  assert.equal(dev.status, "error");
  assert.deepEqual(sim.seen, [[3, null]]);
  assert.equal(hid.opened, false);
});

test("no WebHID: connect says so", async () => {
  await assert.rejects(new KmfwDevice({ hidApi: undefined }).connect(), UnsupportedError);
});

test("readSettings returns the live blob", async () => {
  const { dev, sim } = await connected();
  assert.ok(same(await dev.readSettings(), sim.blob));
});

test("writing is refused unless armed; arming needs the word and covers exactly one write", async () => {
  const { dev, sim, hid } = await connected();
  const blob = pattern(400);
  const sent = hid.sent.length;
  await assert.rejects(dev.writeSettings(blob), NotArmedError);
  assert.equal(hid.sent.length, sent, "nothing was sent");
  assert.throws(() => dev.armWrites("save"), ConfigError);
  assert.throws(() => dev.armWrites(""), ConfigError);
  assert.equal(dev.armed, false);
  dev.armWrites("SAVE");
  assert.equal(dev.armed, true);
  assert.equal(dev.readOnly, false);
  assert.equal(await dev.writeSettings(blob), 1);
  assert.equal(sim.flashWrites, 1);
  assert.ok(same(sim.blob, blob));
  assert.equal(dev.armed, false);
  assert.equal(dev.readOnly, true);
  await assert.rejects(dev.writeSettings(blob), NotArmedError, "the arming was used up");
  assert.equal(sim.flashWrites, 1);
});

test("a failed write also uses up the arming", async () => {
  const { dev, sim } = await connected();
  dev.armWrites("SAVE");
  await assert.rejects(dev.writeSettings(new Uint8Array(8300)), /1\.\.8192/);
  assert.equal(dev.armed, false);
  assert.equal(sim.flashWrites, 0);
});

test("profiles need a codec; with one, read and write go through the blob", async () => {
  const { dev, sim } = await connected();
  await assert.rejects(dev.readProfile(0), UnsupportedError);
  await assert.rejects(dev.writeProfile(0, {}), UnsupportedError);
  // a toy codec: the blob is four profiles of one byte each
  dev.attachSettingsCodec({
    decode: (blob) => Array.from(blob.subarray(0, 4), (v) => ({ v })),
    encode: (blob, i, p) => { const b = Uint8Array.from(blob); b[i] = p.v; return b; },
  });
  assert.equal(dev.capabilities.readProfile, true);
  assert.equal(dev.capabilities.writeProfile, true);
  assert.deepEqual(await dev.readProfile(1), { v: sim.blob[1] });
  await assert.rejects(dev.readProfile(4), RangeError);
  await assert.rejects(dev.writeProfile(1, { v: 99 }), NotArmedError);
  dev.armWrites("SAVE");
  await dev.writeProfile(1, { v: 99 });
  assert.equal(sim.blob[1], 99);
  assert.equal(sim.blob.length, 100);
  assert.deepEqual(await dev.readProfile(1), { v: 99 });
});

test("selectProfile needs its own word", async () => {
  const { dev, sim } = await connected();
  await assert.rejects(dev.selectProfile(1, "SAVE"), /confirmation word/);
  await assert.rejects(dev.selectProfile(1, undefined), ConfigError);
  assert.equal(sim.active, 2);
  assert.equal(await dev.selectProfile(1, "PROF"), 1);
  assert.equal(sim.active, 1);
  assert.equal(dev.config.info.activeProfile, 1);
});

test("live depth: subscribers get 76 depths; unsubscribing stops the polling", async () => {
  const { dev, sim } = await connected();
  const frames = [];
  const off = dev.subscribeLive((depth, t) => frames.push([Array.from(depth.subarray(0, 3)), depth.length, typeof t]));
  for (let i = 0; i < 100 && frames.length < 2; i++) await new Promise((r) => setTimeout(r, 20)); // timers tick at ~15 ms on Windows
  off();
  assert.ok(frames.length >= 2, `got ${frames.length} frames`);
  assert.deepEqual(frames[0], [[1000, 1007, 1014], 76, "number"]);
  await new Promise((r) => setTimeout(r, 150)); // let a request that was already in flight finish
  const n = sim.seen.length;
  await new Promise((r) => setTimeout(r, 200));
  assert.equal(sim.seen.length, n, "polling stopped");
});

test("everything the device sent is cmd 3 or a whitelisted 0xC6 op", async () => {
  const { dev, hid } = await connected();
  dev.armWrites("SAVE");
  await dev.writeSettings(pattern(300));
  await dev.selectProfile(0, "PROF");
  await dev.readSettings();
  for (const p of hid.sent) {
    assert.equal(p[0], 0x55);
    // cmd 3, whitelisted 0xC6 ops, and the read-only scope command 0xC5 (used once on connect to find out which diagnostics exist)
    assert.ok(p[1] === 3 || p[1] === 0xc5 || (p[1] === 0xc6 && [1, 2, 3, 4, 5, 6, 9].includes(p[8])), `cmd ${p[1]} op ${p[8]}`);
  }
});

test("a lost report is retried by the client (nothing breaks, one flash write)", async () => {
  const sim = new SimKeyboard();
  const hid = new FakeHid(sim);
  const dev = new KmfwDevice({ hidApi: new FakeHidApi([hid]) });
  await dev.connect();
  sim.dropReply.add(sim.seen.length + 3);
  sim.dropRequest.add(sim.seen.length + 6);
  dev.armWrites("SAVE");
  await dev.writeSettings(pattern(200));
  assert.equal(sim.flashWrites, 1);
}, { timeout: 15000 });

test("disconnect clears the arming", async () => {
  const { dev } = await connected();
  dev.armWrites("SAVE");
  await dev.disconnect();
  assert.equal(dev.armed, false);
  assert.equal(dev.readOnly, true);
});

test("device factory: three kinds side by side; the mock and the read-only shim are unchanged", async () => {
  assert.deepEqual(DEVICE_KINDS, ["mock", "webhid", "kmfw"]);
  const mock = createDevice("mock", { layout: loadLayout() });
  assert.ok(mock instanceof MockDevice);
  assert.equal(mock.kind, "mock");
  assert.equal(mock.readOnly, false);
  assert.deepEqual(mock.capabilities, { live: true, readProfile: true, writeProfile: true });
  const ro = createDevice("webhid");
  assert.ok(ro instanceof WebHidDevice);
  assert.equal(ro.readOnly, true);
  assert.deepEqual(ro.capabilities, { live: false, readProfile: false, writeProfile: false });
  assert.ok(createDevice("kmfw", { hidApi: undefined }) instanceof KmfwDevice);
  assert.throws(() => createDevice("nope"), RangeError);
});

test("probeFirmware tells our firmware from the stock one using only the version query", async () => {
  const ours = new FakeHid(new SimKeyboard());
  const stock = new FakeHid(new SimKeyboard({ stock: true }));
  assert.equal(await probeFirmware(ours), "kmfw");
  assert.equal(await probeFirmware(stock), "webhid");
  for (const h of [ours, stock]) assert.ok(h.sent.every((p) => p[1] === 3));
});
