// Automatic connection: the state machine against a fake navigator.hid (getDevices + connect / disconnect events).
import test from "node:test";
import assert from "node:assert/strict";
import { createAutoConnect, isConfigInterface, CONN_STATES } from "../js/auto_connect.js";
import { VID, PID } from "../js/device/ids.js";

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const cfgIf = () => ({ vendorId: VID, productId: PID, collections: [{ usagePage: 1, usage: 0 }], name: "config" });
const kbIf = () => ({ vendorId: VID, productId: PID, collections: [{ usagePage: 1, usage: 6 }], name: "keyboard" });

class FakeHidApi extends EventTarget {
  constructor(devices = []) { super(); this.devices = devices; this.calls = 0; }
  async getDevices() { this.calls++; return [...this.devices]; }
  plug(d) { this.devices.push(d); const e = new Event("connect"); e.device = d; this.dispatchEvent(e); }
  unplug(d) { this.devices = this.devices.filter((x) => x !== d); const e = new Event("disconnect"); e.device = d; this.dispatchEvent(e); }
}

function harness({ devices = [], connect, hid = new FakeHidApi(devices), isConnected, retryDelaysMs = [1, 1] } = {}) {
  const states = [];
  const calls = [];
  let connected = false;
  const auto = createAutoConnect({
    hid: hid === null ? undefined : hid,
    connect: async (d) => { calls.push(d); if (connect) await connect(d, calls.length); connected = true; },
    onState: (c) => { states.push(c.state); },
    isConnected: isConnected ?? (() => connected),
    retryDelaysMs, sleepFn: (ms) => wait(Math.min(ms, 2)),
  });
  return { auto, hid, calls, states, setConnected: (v) => { connected = v; } };
}

test("a keyboard the browser already trusts is connected without any click", async () => {
  const kb = cfgIf();
  const h = harness({ devices: [kbIf(), kb] });
  await h.auto.start();
  assert.deepEqual(h.states, ["searching", "connecting", "connected"]);
  assert.deepEqual(h.calls, [kb], "only the configuration interface is opened, not the typing interface");
  assert.ok(CONN_STATES.includes(h.auto.state));
});

test("first visit: nothing trusted yet -> needs_permission (the person presses connect once), no connect attempt", async () => {
  const h = harness({ devices: [] });
  await h.auto.start();
  assert.equal(h.auto.state, "needs_permission");
  assert.equal(h.calls.length, 0);
  // an unrelated device is not our keyboard
  const other = harness({ devices: [{ vendorId: 0x1234, productId: PID, collections: [] }, { vendorId: VID, productId: 1, collections: [] }, kbIf()] });
  await other.auto.start();
  assert.equal(other.auto.state, "needs_permission");
  assert.equal(other.calls.length, 0);
});

test("no WebHID in this browser: state unsupported, nothing else happens", async () => {
  const h = harness({ hid: null });
  await h.auto.start();
  assert.equal(h.auto.state, "unsupported");
  assert.equal(h.calls.length, 0);
});

test("unplug -> waiting; plug in again -> connects by itself (a new HIDDevice object)", async () => {
  const kb = cfgIf();
  const h = harness({ devices: [kb] });
  await h.auto.start();
  assert.equal(h.auto.state, "connected");
  h.setConnected(false);
  h.hid.unplug(kb);
  assert.equal(h.auto.state, "waiting");
  const again = cfgIf();
  h.hid.plug(again);
  await wait(10);
  assert.equal(h.auto.state, "connected");
  assert.equal(h.calls.at(-1), again);
  assert.equal(h.calls.length, 2);
});

test("first visit, then the person connects by hand: markConnected; later unplug / replug is automatic", async () => {
  const h = harness({ devices: [] });
  await h.auto.start();
  assert.equal(h.auto.state, "needs_permission");
  h.auto.markConnected();
  assert.equal(h.auto.state, "connected");
  const kb = cfgIf();
  h.hid.devices.push(kb);
  h.hid.unplug(kb);
  assert.equal(h.auto.state, "waiting");
  h.hid.plug(kb);
  await wait(10);
  assert.equal(h.auto.state, "connected");
});

test("a plug-in event on the first visit (permission arrived some other way) connects too", async () => {
  const h = harness({ devices: [] });
  await h.auto.start();
  h.hid.plug(cfgIf());
  await wait(10);
  assert.equal(h.auto.state, "connected");
});

test("a failing keyboard is tried three times and then left alone: no loop, no spam", async () => {
  let n = 0;
  const boom = Object.assign(new Error("Failed to open the device"), { name: "NetworkError" });
  const h = harness({ devices: [cfgIf()], connect: () => { n++; throw boom; } });
  await h.auto.start();
  assert.equal(h.auto.state, "failed");
  assert.equal(h.auto.error, boom);
  assert.equal(n, 3, "now, +1 retry, +1 retry");
  await wait(40);
  assert.equal(n, 3, "nothing happens by itself afterwards");
  assert.equal(h.states.filter((s) => s === "failed").length, 1, "one failed notification");
  // plugging it in again is a new chance
  n = 0;
  h.hid.plug(cfgIf());
  await wait(40);
  assert.equal(n, 3);
});

test("a hiccup on the first try is retried and succeeds", async () => {
  const h = harness({ devices: [cfgIf()], connect: (d, i) => { if (i < 2) throw new Error("busy"); } });
  await h.auto.start();
  assert.equal(h.auto.state, "connected");
  assert.equal(h.calls.length, 2);
});

test("a refusal by the browser (no permission) is not retried", async () => {
  const h = harness({ devices: [cfgIf()], connect: () => { throw Object.assign(new Error("no"), { name: "SecurityError" }); } });
  await h.auto.start();
  assert.equal(h.auto.state, "failed");
  assert.equal(h.calls.length, 1);
});

test("two plug events at once open the keyboard once", async () => {
  const h = harness({ devices: [], connect: () => wait(15) });
  await h.auto.start();
  const kb = cfgIf();
  h.hid.plug(kb);
  h.hid.plug(kb);
  await wait(60);
  assert.equal(h.calls.length, 1);
  assert.equal(h.auto.state, "connected");
});

test("events for a keyboard that is already connected, or for the typing interface, do nothing", async () => {
  const h = harness({ devices: [cfgIf()] });
  await h.auto.start();
  const before = h.calls.length;
  h.hid.plug(cfgIf());
  h.hid.plug(kbIf());
  await wait(10);
  assert.equal(h.calls.length, before);
  h.hid.unplug(kbIf());
  assert.equal(h.auto.state, "connected", "unplugging some other interface does not disconnect");
});

test("pause (the person chose the demo keyboard): nothing connects until they connect again", async () => {
  const kb = cfgIf();
  const h = harness({ devices: [kb] });
  await h.auto.start();
  h.setConnected(false);
  h.auto.pause();
  assert.equal(h.auto.state, "paused");
  h.hid.unplug(kb);
  assert.equal(h.auto.state, "paused");
  h.hid.plug(cfgIf());
  await wait(10);
  assert.equal(h.calls.length, 1, "no reconnect while paused");
  h.auto.resume();
  h.hid.plug(cfgIf());
  await wait(10);
  assert.equal(h.auto.state, "connected");
  assert.equal(h.calls.length, 2);
});

test("stop removes the listeners", async () => {
  const h = harness({ devices: [] });
  await h.auto.start();
  h.auto.stop();
  h.hid.plug(cfgIf());
  await wait(10);
  assert.equal(h.calls.length, 0);
});

test("getDevices failing counts as nothing found, not a crash", async () => {
  const hid = new FakeHidApi();
  hid.getDevices = async () => { throw new Error("denied"); };
  const h = harness({ hid });
  await h.auto.start();
  assert.equal(h.auto.state, "needs_permission");
});

test("isConfigInterface: right ids and the configuration usage", () => {
  assert.equal(isConfigInterface(cfgIf()), true);
  assert.equal(isConfigInterface({ vendorId: VID, productId: PID, collections: [] }), true, "a fake without collection info is accepted");
  assert.equal(isConfigInterface(kbIf()), false);
  assert.equal(isConfigInterface({ vendorId: VID, productId: PID + 1, collections: [] }), false);
  assert.equal(isConfigInterface(null), false);
});
