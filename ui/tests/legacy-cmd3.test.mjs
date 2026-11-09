import test from "node:test";
import assert from "node:assert/strict";
import { buildReadRequest, assertSendable, decodeInfoResponse, checksum, legacyReadVersion, SafetyError } from "../js/device/legacy_cmd3.js";
import { VID, PID } from "../js/device/ids.js";

// A well-formed reply made here: version bytes 04 01 (= "104") then a text, zero padded to 64 bytes.
function makeReply(text) {
  const data = [0x04, 0x01, ...[...text].map((c) => c.charCodeAt(0))];
  const r = new Uint8Array(64);
  r[0] = 0xaa; r[1] = 3; r[4] = data.length;
  r.set(data, 8);
  r[3] = checksum(data.length, 0, data);
  return r;
}

test("device ids", () => {
  assert.equal(VID, 0x19f5);
  assert.equal(PID, 0xfb49);
});

test("request: header 0x55, cmd 3, no xor, checksum 0, len 0, rest zero", () => {
  const p = buildReadRequest();
  assert.equal(p.length, 64);
  assert.deepEqual([...p.subarray(0, 8)], [0x55, 3, 0, 0, 0, 0, 0, 0]);
  assert.ok(p.subarray(8).every((b) => b === 0));
});

test("only command 3 can be built", () => {
  for (const cmd of [0x01, 0x04, 0x05, 0x06, 0x09, 0x0b, 0x0e, 0x80, 0xa1, 0xb1, 0xc5, 0xe0, 0xe1, 0xee]) {
    assert.throws(() => buildReadRequest(cmd), SafetyError, `cmd ${cmd}`);
  }
});

test("assertSendable refuses tampered packets", () => {
  const good = buildReadRequest();
  const mutate = (fn) => { const p = good.slice(); fn(p); return p; };
  assert.throws(() => assertSendable(mutate((p) => (p[0] = 0x5f))), SafetyError);
  assert.throws(() => assertSendable(mutate((p) => (p[1] = 0x06))), SafetyError);
  assert.throws(() => assertSendable(mutate((p) => (p[2] = 1))), SafetyError);
  assert.throws(() => assertSendable(mutate((p) => (p[9] = 1))), SafetyError);
  assert.throws(() => assertSendable(mutate((p) => (p[3] = 9))), SafetyError);
  assert.throws(() => assertSendable(good.subarray(0, 63)), SafetyError);
});

test("decodes a reply", () => {
  assert.deepEqual(decodeInfoResponse(makeReply("Test build 1")), { version: "104", build: "Test build 1" });
});

test("decode rejects checksum error, error reply and wrong cmd", () => {
  const ok = makeReply("x");
  const bad = ok.slice(); bad[3] ^= 1;
  assert.throws(() => decodeInfoResponse(bad), /checksum/);
  const ab = ok.slice(); ab[0] = 0xab;
  assert.throws(() => decodeInfoResponse(ab), /unexpected/);
  const other = ok.slice(); other[1] = 4;
  assert.throws(() => decodeInfoResponse(other), /unexpected/);
});

test("legacyReadVersion sends exactly one packet to a fake device and decodes the answer", async () => {
  const listeners = new Set();
  const sent = [];
  const fake = {
    addEventListener: (_t, f) => listeners.add(f),
    removeEventListener: (_t, f) => listeners.delete(f),
    sendReport: async (id, data) => {
      sent.push([id, [...data.subarray(0, 8)]]);
      const reply = makeReply("Fake");
      setTimeout(() => [...listeners].forEach((f) => f({ data: new DataView(reply.buffer) })), 1);
    },
  };
  const info = await legacyReadVersion(fake);
  assert.deepEqual(info, { version: "104", build: "Fake" });
  assert.deepEqual(sent, [[0, [0x55, 3, 0, 0, 0, 0, 0, 0]]]);
  assert.equal(listeners.size, 0);
});
