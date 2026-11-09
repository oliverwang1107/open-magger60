import test from "node:test";
import assert from "node:assert/strict";
import { ConfigClient } from "../js/device/ConfigClient.js";
import { ConfigError, ST, OP, MAX_SETTINGS, le16, concat } from "../js/device/config_protocol.js";
import { SimKeyboard } from "./helpers/sim-keyboard.mjs";

const pattern = (n, seed = 5) => Uint8Array.from({ length: n }, (_, i) => (i * 11 + seed) & 255);
const sameBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
async function ready(opts) {
  const sim = new SimKeyboard(opts);
  const c = new ConfigClient(sim);
  await c.verify();
  return { sim, c };
}

test("verify reads the build and the info", async () => {
  const { c } = await ready();
  assert.match(c.build, /KMFW-SIM$/);
  assert.equal(c.info.profileCount, 4);
  assert.equal(c.info.activeProfile, 2);
});

test("stock firmware is refused and never sees a 0xC6 packet", async () => {
  const sim = new SimKeyboard({ stock: true });
  await assert.rejects(new ConfigClient(sim).verify(), /not our firmware/);
  assert.deepEqual(sim.seen, [[3, null]]);
});

test("no config request before verify", async () => {
  const c = new ConfigClient(new SimKeyboard());
  await assert.rejects(c.call(OP.LIVE_DEPTH, Uint8Array.of(0, 1)), /not verified/);
});

test("dump returns the live blob", async () => {
  const { sim, c } = await ready();
  assert.ok(sameBytes(await c.dump(), sim.blob));
});

test("stage + commit: nothing reaches flash before the commit; exactly one write after", async () => {
  const { sim, c } = await ready();
  const blob = pattern(1000);
  await c.stage(blob);
  assert.equal(sim.flashWrites, 0);
  assert.ok(!sameBytes(sim.blob, blob));
  assert.equal((await c.getInfo()).stageState, 2);
  assert.equal(await c.commit(blob, "SAVE"), 1);
  assert.equal(sim.flashWrites, 1);
  assert.ok(sameBytes(await c.dump(), blob));
});

test("commit refuses a wrong confirmation word on the client side, and the keyboard refuses a wrong CRC", async () => {
  const { sim, c } = await ready();
  const blob = pattern(60);
  await c.stage(blob);
  await assert.rejects(c.commit(blob, "SAVF"), /confirmation word/);
  await assert.rejects(c.commit(blob, ""), /confirmation word/);
  await assert.rejects(c.commit(pattern(60, 6), "SAVE"), (e) => e instanceof ConfigError && e.status === ST.BAD_CRC);
  assert.equal(sim.flashWrites, 0);
  assert.equal(await c.commit(blob, "SAVE"), 1);
});

test("8192 bytes round trip, 8193 and 0 are refused before anything is sent", async () => {
  const { sim, c } = await ready();
  const blob = pattern(8192, 9); // the simulated keyboard announces 8192 bytes, like the firmware
  await c.stage(blob);
  await c.commit(blob, "SAVE");
  assert.ok(sameBytes(await c.dump(), blob));
  const seen = sim.seen.length;
  await assert.rejects(c.stage(new Uint8Array(8193)), /1\.\.8192/);
  await assert.rejects(c.stage(new Uint8Array(0)), /1\.\.8192/);
  assert.equal(sim.seen.length, seen);
});

test("select profile needs the word and a valid index", async () => {
  const { sim, c } = await ready();
  await assert.rejects(c.selectProfile(1, "SAVE"), /confirmation word/);
  await assert.rejects(c.selectProfile(4, "PROF"), /profile 4/);
  await assert.rejects(c.selectProfile(-1, "PROF"), ConfigError);
  assert.equal(sim.active, 2);
  assert.equal(await c.selectProfile(3, "PROF"), 3);
  assert.equal(sim.active, 3);
});

test("live depth: 76 slots in four requests, and into a reused array", async () => {
  const { sim, c } = await ready();
  const before = sim.seen.length;
  const out = new Uint16Array(76);
  assert.equal(await c.liveDepth(76, out), out);
  assert.deepEqual([...out], Array.from({ length: 76 }, (_, s) => 1000 + 7 * s));
  assert.equal(sim.seen.length - before, 4);
});

test("a lost request or reply at any point of stage+commit still ends in exactly one write", async () => {
  const blob = pattern(300, 7);
  const probe = await ready();
  await probe.c.stage(blob);
  await probe.c.commit(blob, "SAVE");
  const total = probe.sim.seen.length;
  for (let k = 3; k <= total; k++) {
    for (const kind of ["dropRequest", "dropReply"]) {
      const { sim, c } = await ready();
      sim[kind].add(k);
      await c.stage(blob);
      await c.commit(blob, "SAVE");
      assert.ok(sameBytes(sim.blob, blob), `${kind} ${k}`);
      assert.equal(sim.flashWrites, 1, `${kind} ${k}`);
    }
  }
});

test("a lost reply to the COMMIT itself is resolved through GET_INFO, not by committing twice", async () => {
  const blob = pattern(120);
  const { sim, c } = await ready();
  await c.stage(blob);
  sim.dropReply.add(sim.seen.length + 2); // +1 is GET_INFO before the commit, +2 is the COMMIT
  assert.equal(await c.commit(blob, "SAVE"), 1);
  assert.equal(sim.flashWrites, 1);
  assert.equal(sim.seen.filter(([, op]) => op === OP.COMMIT).length, 1);
});

test("out-of-sequence pages are answered with the expected offset and change nothing", async () => {
  const { sim, c } = await ready();
  await c.call(OP.SET_BEGIN, le16(200));
  const rep = await c.call(OP.SET_PAGE, concat(le16(52), new Uint8Array(52)), [ST.OK, ST.SEQUENCE]);
  assert.equal(rep.status, ST.SEQUENCE);
  assert.equal(rep.payload[0] | (rep.payload[1] << 8), 0);
  assert.equal((await c.getInfo()).stageNext, 0);
  assert.equal(sim.flashWrites, 0);
});

test("stage resumes from the expected offset when the keyboard reports SEQUENCE", async () => {
  const { sim, c } = await ready();
  const blob = pattern(200);
  // the keyboard is made to forget the first page after it was acknowledged: restart behind the client's back
  let first = true;
  const real = sim.exchange.bind(sim);
  sim.exchange = async (p) => {
    const r = await real(p);
    if (first && p[8] === OP.SET_PAGE) { first = false; sim.state = 1; sim.next = 0; sim.last = null; }
    return r;
  };
  await c.stage(blob);
  await c.commit(blob, "SAVE");
  assert.ok(sameBytes(sim.blob, blob));
});

test("dump starts over after a STALE page", async () => {
  const { sim, c } = await ready({ blob: pattern(200, 2) });
  const real = sim.exchange.bind(sim);
  let done = false;
  sim.exchange = async (p) => {
    const r = await real(p);
    if (!done && p[8] === OP.GET_SETTINGS && sim.seen.length > 4) { done = true; sim.snapValid = false; }
    return r;
  };
  assert.ok(sameBytes(await c.dump(), pattern(200, 2)));
  assert.ok(done);
});

test("a corrupted page is caught by the CRC", async () => {
  const { sim, c } = await ready({ blob: pattern(100) });
  const real = sim.exchange.bind(sim);
  sim.exchange = async (p) => {
    const r = await real(p);
    if (p[8] === OP.GET_SETTINGS && p[10] === 0 && p[11] === 0) { r[20] ^= 1; r[3] = (r[3] ^ 1) & 255; } // flip a data bit, fix the frame checksum
    return r;
  };
  await assert.rejects(c.dump(), /CRC/);
});

test("only whitelisted ops ever reach the keyboard in a full session", async () => {
  const { sim, c } = await ready();
  const blob = pattern(500);
  await c.stage(blob);
  await c.commit(blob, "SAVE");
  await c.dump();
  await c.selectProfile(1, "PROF");
  await c.liveDepth(10);
  const ops = new Set(sim.seen.filter(([cmd]) => cmd === 0xc6).map(([, op]) => op));
  for (const op of ops) assert.ok([1, 2, 3, 4, 5, 6, 9].includes(op), `op ${op}`);
  assert.ok(sim.seen.every(([cmd]) => cmd === 3 || cmd === 0xc6));
});
