import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
  OP, ST, crc32, buildRequest, assertSendable, decodeReply, parseInfo, isKmfwBuild, buildInfoQuery, decodeBuildReply,
  SafetyError, le16, le32, ascii, concat, checksum,
} from "../js/device/config_protocol.js";
import { SimKeyboard } from "./helpers/sim-keyboard.mjs";

const hexBytes = (h) => Uint8Array.from(h.match(/../g), (x) => parseInt(x, 16));
const vectors = [...readFileSync(new URL("../../firmware/tests/golden/config_vectors.h", import.meta.url), "utf8")
  .matchAll(/\{ "([^"]+)", (\d+), "([0-9a-f]{128})", "([0-9a-f]{128})" \}/g)]
  .map((m) => ({ name: m[1], group: Number(m[2]), req: hexBytes(m[3]), rep: hexBytes(m[4]) }));

test("crc32 check value", () => {
  assert.equal(crc32(ascii("123456789")), 0xcbf43926);
  assert.equal(crc32(new Uint8Array(0)), 0);
  assert.equal(crc32(Uint8Array.of(0, 1, 2, 3, 4, 5, 6, 7, 8, 9)), 0x456cd746);
});

test("the golden vectors are there", () => {
  assert.ok(vectors.length >= 20);
});

test("golden packets: the JavaScript model reproduces every reply, decodeReply reads it, buildRequest reproduces the request", () => {
  vectors.forEach((v, i) => {
    const sim = new SimKeyboard();
    vectors.slice(0, i).filter((p) => p.group === v.group).forEach((p) => sim.process(p.req));
    assert.deepEqual(sim.process(v.req), v.rep, v.name);
    const rep = decodeReply(v.rep);
    assert.equal(rep.op, v.req[8], v.name);
    assert.equal(rep.token, v.req[9], v.name);
    const n = v.req[4];
    if (n >= 2) {
      try {
        assert.deepEqual(buildRequest(v.req[8], v.req[9], v.req.slice(10, 8 + n)), v.req, v.name);
      } catch (err) {
        if (!(err instanceof SafetyError)) throw err; // requests the client must never send (bad fields, recalibrate)
      }
    }
  });
});

test("info reply parses", () => {
  const rep = decodeReply(vectors.find((v) => v.name === "get_info").rep);
  const info = parseInfo(rep.payload);
  assert.equal(info.protoVersion, 2);
  assert.equal(info.profileCount, 4);
  assert.equal(info.activeProfile, 2);
  assert.equal(info.maxSettings, 8192);
  assert.equal(info.getPage, 44);
  assert.equal(info.setPage, 52);
  assert.equal(info.features, 0x7bf);
  assert.equal(info.sceneMax, 1024);
  assert.deepEqual(info.featureNames.slice(0, 2), ["read settings", "write settings"]);
  assert.deepEqual(info.featureNames.slice(6), ["device status", "light scene", "stream", "host lock lights"]);
  assert.equal(info.commits, 0);
});

test("the whitelist: only the documented ops can be built", () => {
  for (const op of [0, 0x0b, 0x2f, 0x34, 0x80, 0xff]) assert.throws(() => buildRequest(op, 1, ascii("RCAL")), SafetyError, `op ${op}`);
  // ops 7 and 8 (recalibrate, save the calibration) are sendable, but only with their own confirmation word
  assert.doesNotThrow(() => buildRequest(7, 1, ascii("RCAL")));
  assert.doesNotThrow(() => buildRequest(8, 1, ascii("SCAL")));
  assert.throws(() => buildRequest(7, 1, ascii("SCAL")), SafetyError);
  assert.throws(() => buildRequest(8, 1, ascii("RCAL")), SafetyError);
  assert.throws(() => buildRequest(7, 1, ascii("SAVE")), SafetyError);
  assert.throws(() => buildRequest(7, 1), SafetyError);
  assert.throws(() => buildRequest(OP.GET_INFO, 256), SafetyError);
});

test("confirmation words are inside the request and cannot be left out or changed", () => {
  const crc = le32(crc32(Uint8Array.of(1)));
  for (const w of ["SAVX", "save", "PROF", "\0\0\0\0", "RCAL"]) {
    assert.throws(() => buildRequest(OP.COMMIT, 1, concat(ascii(w), le16(1), crc)), SafetyError, w);
  }
  assert.throws(() => buildRequest(OP.SELECT_PROFILE, 1, concat(ascii("SAVE"), Uint8Array.of(1))), SafetyError);
  assert.doesNotThrow(() => buildRequest(OP.COMMIT, 1, concat(ascii("SAVE"), le16(1), crc)));
  assert.doesNotThrow(() => buildRequest(OP.SELECT_PROFILE, 1, concat(ascii("PROF"), Uint8Array.of(1))));
});

test("argument shapes are enforced", () => {
  const bad = [
    [OP.SET_PAGE, le16(0)], [OP.SET_PAGE, concat(le16(0), new Uint8Array(53))],
    [OP.SET_BEGIN, le16(0)], [OP.SET_BEGIN, le16(8193)], [OP.SET_BEGIN, Uint8Array.of(1)],
    [OP.GET_SETTINGS, concat(le16(0), Uint8Array.of(45, 0))], [OP.GET_SETTINGS, concat(le16(0), Uint8Array.of(0, 0))],
    [OP.GET_SETTINGS, concat(le16(0), Uint8Array.of(44, 3))], [OP.GET_SETTINGS, Uint8Array.of(0, 0, 44)],
    [OP.LIVE_DEPTH, Uint8Array.of(120, 9)], [OP.LIVE_DEPTH, Uint8Array.of(0, 0)], [OP.LIVE_DEPTH, Uint8Array.of(0, 26)],
    [OP.GET_INFO, Uint8Array.of(0)], [OP.SELECT_PROFILE, concat(ascii("PROF"), Uint8Array.of(16))],
  ];
  for (const [op, args] of bad) assert.throws(() => buildRequest(op, 1, args), SafetyError, `op ${op} args ${args}`);
  assert.doesNotThrow(() => buildRequest(OP.SET_BEGIN, 1, le16(4096)));
  assert.doesNotThrow(() => buildRequest(OP.SET_PAGE, 1, concat(le16(4044), new Uint8Array(52))));
  assert.doesNotThrow(() => buildRequest(OP.LIVE_DEPTH, 1, Uint8Array.of(103, 25)));
});

test("assertSendable refuses tampered packets", () => {
  const good = buildRequest(OP.COMMIT, 5, concat(ascii("SAVE"), le16(10), le32(1)));
  const mutate = (fn) => { const p = good.slice(); fn(p); return p; };
  const cases = {
    "raw header": (p) => (p[0] = 0x5f), "cmd 6": (p) => (p[1] = 6), "cmd 0xe1": (p) => (p[1] = 0xe1), "scope cmd": (p) => (p[1] = 0xc5),
    "xor key": (p) => (p[2] = 1), "offset field": (p) => (p[5] = 1), "checksum": (p) => (p[3] ^= 1), "trailing byte": (p) => (p[40] = 1),
    "word": (p) => (p[13] = 0x46), "length": (p) => (p[4] = 11), "recalibrate op": (p) => (p[8] = 7),
  };
  for (const [name, fn] of Object.entries(cases)) assert.throws(() => assertSendable(mutate(fn)), SafetyError, name);
  assert.throws(() => assertSendable(good.subarray(0, 63)), SafetyError);
  assert.throws(() => assertSendable(Array.from(good)), SafetyError);
});

test("the version query is the only non-0xC6 packet", () => {
  assertSendable(buildInfoQuery());
  const q = buildInfoQuery();
  q[10] = 1;
  assert.throws(() => assertSendable(q), SafetyError);
});

test("decodeReply rejects bad frames", () => {
  const ok = vectors.find((v) => v.name === "get_info").rep;
  const mut = (fn) => { const r = ok.slice(); fn(r); return r; };
  assert.throws(() => decodeReply(mut((r) => (r[3] ^= 1))), /checksum/);
  assert.throws(() => decodeReply(mut((r) => (r[0] = 0xab))), /0xAB/);
  assert.throws(() => decodeReply(mut((r) => (r[1] = 0xc5))), /unexpected/);
  assert.throws(() => decodeReply(mut((r) => (r[5] = 1))), /zero/);
  assert.throws(() => decodeReply(mut((r) => (r[60] = 1))), /beyond/);
  assert.throws(() => decodeReply(mut((r) => { r[4] = 2; r[3] = checksum(2, 0, r.subarray(8, 10)); })), /length/);
  assert.throws(() => decodeReply(ok.subarray(0, 63)), /64/);
});

test("isKmfwBuild", () => {
  assert.ok(isKmfwBuild("Jan  1 2026,00:00:00KMFW-K3"));
  assert.ok(isKmfwBuild("KMFW-X"));
  assert.ok(!isKmfwBuild("Jan  1 2026,00:00:00STOCK-SIM"));
  assert.ok(!isKmfwBuild("xKMFW"));
  assert.ok(!isKmfwBuild(""));
});

test("cmd 3 reply decodes", () => {
  const sim = new SimKeyboard();
  assert.deepEqual(decodeBuildReply(sim.process(buildInfoQuery())), { version: "104", build: "Jan  1 2026,00:00:00KMFW-SIM" });
  assert.equal(ST.OK, 0);
});
