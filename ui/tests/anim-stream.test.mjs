// The light frame stream in the browser (STREAM_PROTOCOL.md): packet layer, the keyboard model, the client with flow control, the
// controller the animation page uses, and the device plumbing (feature bit, transport, safety check).
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  OP, FMT, STATUS, STATE, FLAG_ACK, LIGHTS, buildPacket, buildStart, buildStop, buildKeepalive, buildFrameChunk, frameChunks, parseReply,
  assertStreamPacket, seqNewer, checksum, StreamError,
} from "../js/core/stream_protocol.js";
import { StreamClient } from "../js/core/stream_client.js";
import { StreamController, streamAvailability } from "../js/core/anim_stream.js";
import { StreamSim } from "../js/device/stream_sim.js";
import { SimKeyboard, FakeHid, FakeHidApi } from "./helpers/sim-keyboard.mjs";
import { KmfwDevice } from "../js/device/KmfwDevice.js";
import { assertSendable, SafetyError } from "../js/device/config_protocol.js";

const hex = (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
const unhex = (s) => Uint8Array.from(s.match(/../g) ?? [], (x) => parseInt(x, 16));
const frameOf = (n) => Uint8Array.from({ length: 183 }, (_, i) => (i * 7 + n) & 255);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const portOf = (sim) => ({ send: (p) => sim.send(p), onReport: (cb) => sim.onStreamReport(cb) });
const simKb = (o = {}) => new SimKeyboard({ features: 0xfff, ...o });

// ---- packets ----

test("the worked example of STREAM_PROTOCOL.md section 8, byte for byte", () => {
  assert.equal(hex(buildStart(7, 65530, FMT.RGB, 600)).slice(0, 32), "55c7006408faff00" + "0107000100580200");
  const frame = buildFrameChunk(7, 65531, 1, 0, 16, new Uint8Array(48));
  assert.equal(hex(frame).slice(0, 16), `55c700${hex([frame[3]])}36fbff00`);
  assert.deepEqual([...frame.subarray(0, 3)], [0x55, 0xc7, 0]);
  assert.deepEqual([...frame.subarray(8, 14)], [2, 7, 0, 1, 0, 16]);
  assert.equal(frame[4], 54);
});

test("checksum and wrap-around of the sequence number", () => {
  const p = buildKeepalive(9, 0x1234);
  assert.equal(p[3], checksum(3, 0x1234, p.subarray(8, 11)));
  assert.equal(seqNewer(1, 0), true);
  assert.equal(seqNewer(0, 65535), true);
  assert.equal(seqNewer(5, 5), false);
  assert.equal(seqNewer(65535, 0), false);
  assert.throws(() => buildPacket(1, 1, 70000), RangeError);
});

test("a frame is 4 RGB packets (16,16,16,13) or 2 gray packets (50,11)", () => {
  assert.deepEqual(frameChunks(new Uint8Array(183)).map((c) => c.count), [16, 16, 16, 13]);
  assert.deepEqual(frameChunks(new Uint8Array(61), FMT.GRAY).map((c) => c.count), [50, 11]);
  assert.throws(() => frameChunks(new Uint8Array(60)), RangeError);
});

test("assertStreamPacket accepts the four ops and refuses everything else", () => {
  for (const p of [buildStart(1, 0), buildStart(255, 65535, FMT.GRAY, 100), buildStart(3, 5, 0, 5000), buildStop(2, 3), buildKeepalive(2, 4), buildFrameChunk(2, 1, 0, 0, 16, new Uint8Array(48)), buildFrameChunk(2, 1, 0, 60, 1, new Uint8Array(3), FLAG_ACK)]) assert.equal(assertStreamPacket(p), p);
  const bad = (p, why) => assert.throws(() => assertStreamPacket(p), (e) => e instanceof StreamError && e.code === "unsafe", why);
  bad(buildStart(0, 0), "session 0");
  bad(buildStart(1, 0, FMT.RGB, 99), "time-out under 100");
  bad(buildStart(1, 0, FMT.RGB, 5001), "time-out over 5000");
  bad(buildStart(1, 0, 2), "format 2");
  bad(buildPacket(9, 1, 0), "unknown op");
  bad(buildPacket(0x10, 1, 0), "the reserved animation-player ops are not sent from here");
  bad(buildPacket(OP.KEEPALIVE, 1, 0, 2), "unknown flag bit");
  bad(buildPacket(OP.KEEPALIVE, 1, 0, 0, [1]), "KEEPALIVE with extra data");
  bad(buildFrameChunk(1, 0, 0, 55, 10, new Uint8Array(30)), "light range past 60");
  bad(buildFrameChunk(1, 0, 0, 0, 0, new Uint8Array(0)), "no lights");
  bad(buildFrameChunk(1, 0, 0, 0, 4, new Uint8Array(5)), "pixel bytes that match no format");
  const t1 = buildKeepalive(1, 1); t1[2] = 0x5a; bad(t1, "masked");
  const t2 = buildKeepalive(1, 1); t2[3] ^= 1; bad(t2, "checksum");
  const t3 = buildKeepalive(1, 1); t3[7] = 1; bad(t3, "byte 7");
  const t4 = buildKeepalive(1, 1); t4[40] = 1; bad(t4, "dirt after the data");
  bad(new Uint8Array(63), "length");
});

test("the transport check lets a good 0xC7 packet through and still refuses the rest", () => {
  assert.doesNotThrow(() => assertSendable(buildKeepalive(1, 1)));
  const raw = buildKeepalive(1, 1); raw[3] ^= 1;
  assert.throws(() => assertSendable(raw), StreamError);
  const other = buildKeepalive(1, 1); other[1] = 0xc8;
  assert.throws(() => assertSendable(other), SafetyError);
});

test("parseReply reads the 22 reply bytes and recognises the answer of a firmware without the command", () => {
  const sim = new StreamSim();
  const r = parseReply(sim.process(buildKeepalive(1, 0)));
  assert.equal(r.status, STATUS.SESSION);
  assert.equal(r.version, 1);
  assert.equal(r.window, 16);
  assert.equal(r.formats, 3);
  assert.equal(r.state, STATE.IDLE);
  const unknw = new Uint8Array(64); unknw[0] = 0xaa; unknw[1] = 0xc7; unknw[4] = 5; unknw.set([117, 110, 107, 110, 119], 8);
  assert.throws(() => parseReply(unknw), (e) => e.code === "nostream");
  assert.throws(() => parseReply(new Uint8Array(64)), (e) => e.code === "bad_reply");
});

// ---- the keyboard model: replay the independently generated golden vectors ----

test("the keyboard model answers every request of firmware/tests/golden/stream_vectors.h identically (reply, state, frame)", () => {
  const src = readFileSync(join(dirname(fileURLToPath(import.meta.url)), "..", "..", "firmware", "tests", "golden", "stream_vectors.h"), "utf8");
  const rows = [...src.matchAll(/\{ "([^"]*)", (\d+), "([0-9a-f]*)", "([0-9a-f]*)", (\d+), (\d+), 0x([0-9A-Fa-f]+)u \}/g)];
  assert.ok(rows.length > 20, `found ${rows.length} vectors`);
  let now = 0;
  const sim = new StreamSim({ clock: () => now });
  const fnv = (b) => { let h = 0x811c9dc5; for (const x of b) h = Math.imul(h ^ x, 0x01000193) >>> 0; return h; };
  for (const [, name, ms, req, reply, state, , hash] of rows) {
    now = Number(ms);
    const out = req ? sim.process(unhex(req)) : (sim.tick(), null);
    assert.equal(out ? hex(out) : "", reply, name);
    assert.equal(sim.state, Number(state), `${name}: state`);
    const frame = sim.visible();
    assert.equal(frame ? fnv(frame) : 0, parseInt(hash, 16), `${name}: frame`);
  }
});

test("the model ends the stream by itself after the time-out and shows its own lights again", () => {
  let now = 0;
  const sim = new StreamSim({ clock: () => now });
  sim.process(buildStart(5, 100, FMT.RGB, 300));
  frameChunks(frameOf(1)).forEach((c, i) => sim.process(buildFrameChunk(5, 101 + i, 1, c.first, c.count, c.raw)));
  assert.ok(sim.visible());
  now = 299; assert.ok(sim.visible());
  now = 300; assert.equal(sim.visible(), null);
  assert.equal(sim.state, STATE.IDLE);
  const bad = parseReply(sim.process(buildKeepalive(5, 110)));
  assert.equal(bad.status, STATUS.SESSION);
});

test("the model refuses a START while the lights are off, and a refused packet changes nothing", () => {
  const sim = new StreamSim({ enabled: false });
  assert.equal(parseReply(sim.process(buildStart(1, 0))).status, STATUS.UNAVAILABLE);
  assert.equal(sim.state, STATE.IDLE);
  const on = new StreamSim();
  on.process(buildStart(1, 0));
  const before = JSON.stringify([on.last, on.stats.committed]);
  const bad = buildFrameChunk(1, 1, 0, 0, 16, new Uint8Array(48), 2);                // flag bit 1 does not exist
  on.process(bad);
  assert.equal(JSON.stringify([on.last, on.stats.committed]), before);
});

// ---- the client ----

test("client: start, frames reach the lights complete, stop returns to the keyboard's own lights", async () => {
  const sim = simKb();
  const c = new StreamClient(portOf(sim), { session: 9, seq0: 65530, replyWaitMs: 50 });
  await c.start();
  assert.equal(c.state, "live");
  for (let i = 1; i <= 10; i++) { assert.equal(await c.sendFrame(frameOf(i)), "sent"); await sleep(1); }
  await sleep(5);
  assert.deepEqual([...sim.stream.visible()], [...frameOf(10)]);
  assert.equal(sim.stream.stats.committed, 10);
  assert.equal(sim.stream.stats.lost, 0);
  assert.ok(c.stats.acks >= 4, "every 8th packet asks for an answer");
  assert.ok(c.unanswered() <= 16);
  await c.stop();
  assert.equal(sim.stream.visible(), null);
  assert.equal(c.state, "idle");
  c.dispose();
});

test("client: a silent keyboard is not flooded: frames are skipped at the window, never queued", async () => {
  const sim = simKb();
  const sent = [];
  const mute = { send: async (p) => { sent.push(p); if (p[8] === OP.START) return sim.send(p); }, onReport: (cb) => sim.onStreamReport(cb) };   // frames go nowhere, no ACK ever comes back
  const c = new StreamClient(mute, { session: 3, seq0: 10, replyWaitMs: 50 });
  await c.start();
  const results = [];
  for (let i = 0; i < 12; i++) results.push(await c.sendFrame(frameOf(i)));
  assert.equal(results.filter((r) => r === "sent").length, 4, "16 packets = 4 frames, then the window is full");
  assert.ok(results.slice(4).every((r) => r === "skipped"));
  assert.ok(sent.filter((p) => p[8] === OP.FRAME).length <= 16);
  c.dispose();
});

test("client: when the keyboard dropped the stream (time-out) the next frame starts a new session", async () => {
  const sim = simKb();
  const c = new StreamClient(portOf(sim), { session: 4, seq0: 1, replyWaitMs: 50, ackEvery: 1 });
  await c.start();
  await c.sendFrame(frameOf(1));
  await sleep(5);
  sim.stream.state = STATE.IDLE; sim.stream.session = 0;             // the keyboard ended it
  await c.sendFrame(frameOf(2));                                       // its ACK reports IDLE
  await sleep(5);
  await c.sendFrame(frameOf(3));
  await sleep(5);
  assert.equal(c.stats.restarts, 1);
  assert.equal(sim.stream.starts, 2);
  assert.ok(sim.stream.visible());
  c.dispose();
});

test("client: START refused with the lights off is reported as 'unavailable'; no answer is 'silent'", async () => {
  const sim = simKb();
  sim.stream.enabled = false;
  const c = new StreamClient(portOf(sim), { replyWaitMs: 30 });
  await assert.rejects(c.start(), (e) => e.code === "unavailable");
  assert.equal(c.state, "error");
  c.dispose();
  const dead = new StreamClient({ send: async () => {}, onReport: () => () => {} }, { replyWaitMs: 20 });
  await assert.rejects(dead.start(), (e) => e.code === "silent");
  await assert.rejects(dead.sendFrame(frameOf(1)), (e) => e.code === "not_live");
});

// ---- the controller (what the page uses) ----

test("controller: phases off -> starting -> live -> off, rate limit, errors", async () => {
  const sim = simKb();
  const dev = { status: "connected", kind: "kmfw", features: { stream: true }, streamPort: () => portOf(sim) };
  assert.equal(streamAvailability(dev), "ok");
  const phases = [];
  const sc = new StreamController(dev, { fps: 30, clientOptions: { replyWaitMs: 50 }, onChange: (c) => { if (phases.at(-1) !== c.phase) phases.push(c.phase); } });
  await sc.start();
  assert.equal(sc.phase, "live");
  assert.equal(sc.push(frameOf(1), 1000), true);
  assert.equal(sc.push(frameOf(2), 1010), false, "30 fps = at most one frame per 33 ms");
  await sleep(10);
  assert.equal(sc.push(frameOf(3), 1040), true);
  await sleep(10);
  assert.equal(sc.sent, 2);
  await sc.stop();
  assert.deepEqual(phases, ["starting", "live", "off"]);
  assert.equal(sc.phase, "off");
  assert.equal(sim.stream.visible(), null);
  assert.equal(sc.push(frameOf(4), 5000), false, "nothing is sent when off");

  sim.stream.enabled = false;
  await sc.start();
  assert.equal(sc.phase, "error");
  assert.equal(sc.reason, "unavailable");
  await sc.stop();

  const unplugged = { ...dev, streamPort: () => ({ send: async () => { throw new Error("gone"); }, onReport: () => () => {} }) };
  const sc2 = new StreamController(unplugged, { clientOptions: { replyWaitMs: 20 } });
  await sc2.start();
  assert.equal(sc2.phase, "error");
  assert.equal(sc2.reason, "lost");
  assert.equal(new StreamController({ streamPort() { throw new Error("no"); } }).phase, "off");
});

test("availability: offline, stock firmware, old firmware, ok", () => {
  assert.equal(streamAvailability(null), "offline");
  assert.equal(streamAvailability({ status: "disconnected" }), "offline");
  assert.equal(streamAvailability({ status: "connected", kind: "webhid" }), "stock");
  assert.equal(streamAvailability({ status: "connected", kind: "kmfw", features: { stream: false }, streamPort() {} }), "old");
  assert.equal(streamAvailability({ status: "connected", kind: "kmfw", features: { stream: true }, streamPort() {} }), "ok");
});

// ---- the device ----

test("KmfwDevice over WebHID: the stream goes through the same transport; a keyboard without the feature bit has no port", async () => {
  const sim = simKb({ build: "Jan  1 2026,00:00:00KMFW-SIM" });
  const hid = new FakeHid(sim);
  const dev = new KmfwDevice({ hidApi: new FakeHidApi([hid]) });
  await dev.connect();
  assert.equal(dev.features.stream, true);
  const port = dev.streamPort();
  const got = [];
  const off = port.onReport((r) => got.push(r));
  await port.send(buildStart(2, 40, FMT.RGB, 0));
  await sleep(10);
  assert.equal(parseReply(got[0]).status, STATUS.OK);
  assert.equal(sim.stream.state, STATE.OPEN);
  await assert.rejects(port.send(buildPacket(0x10, 2, 41)), StreamError, "a packet outside the four ops never leaves");
  off();
  await dev.disconnect();
  assert.throws(() => dev.streamPort());

  const old = new KmfwDevice({ hidApi: new FakeHidApi([new FakeHid(simKb({ features: 0x1ff }))]) });
  await old.connect();
  assert.equal(old.features.stream, false);
  assert.throws(() => old.streamPort());
});

test("the simulated keyboard of a firmware without the stream answers 'unknw' to 0xC7", () => {
  const sim = simKb({ features: 0x1ff });
  const r = sim.process(buildKeepalive(1, 0));
  assert.throws(() => parseReply(r), (e) => e.code === "nostream");
});
