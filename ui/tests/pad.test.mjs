// Gamepad page (M6 package P9b): the page's model and maths against the REAL gamepad core (km_pad.c, through generated vectors), the way
// bindings / curves / options / pad mode travel through the settings codec (P0), limits, plain-language checks and the strings.
import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loadLayout } from "../js/core/layout.js";
import { defaultProfile } from "../js/core/profile.js";
import { bindingToCode, blobToWorkspace, codeToBinding, defaultWorkspace, normalizeBlob, workspaceToBlob } from "../js/core/profile_codec.js";
import { decodeSettings, encodeSettings, needsV3, padIsDefault } from "../js/core/settings_codec.js";
import { CODE, FW_M6, FW_SETTINGS, defaultDevice, defaultPad, entryProblem, padCurveProblem, padProblem } from "../js/core/limits.js";
import {
  PAD_AXIS, PAD_CODE_AXIS, PAD_CODE_BTN, PadCore, SHAPE, curveEval, curveValid, defaultCurve, entryValid, hatOf, neutralReport, optsValid, packReport, reportHex,
  reportNeutral, stickShape, unpackReport,
} from "../js/core/pad_sim.js";
import * as pad from "../js/core/pad.js";
import zh from "../js/locales/zh-TW-pad.js";
import en from "../js/locales/en-pad.js";
import { PAD_CSS } from "../js/pages/pad-style.js";

const V = JSON.parse(readFileSync(new URL("./vectors/pad_vectors.json", import.meta.url), "utf8"));
const layout = loadLayout();
const cv = ([start, end, shape, flags]) => ({ start, end, shape, flags });
const slotOf = (id) => layout.byId.get(id).slot;

// ================================================================================================ the real km_pad.c, replayed

test("curves: validity and every value at every depth equal the real km_pad_curve_eval", () => {
  assert.ok(V.curves.length >= 150);
  for (const c of V.curves) {
    const curve = cv(c.curve);
    assert.equal(curveValid(curve), c.valid, `valid ${c.curve}`);
    if (!c.valid) continue;
    for (let d = 0; d <= 340; d++) {
      if (curveEval(curve, d) !== c.values[d]) assert.fail(`curve ${c.curve} at ${d}: ${curveEval(curve, d)} != ${c.values[d]}`);
    }
  }
  assert.ok(V.curves.some((c) => !c.valid) && V.curves.some((c) => c.valid));
});

test("default curves (km_pad_default_curve), also for an index out of range", () => {
  V.defaults.forEach((d, i) => assert.deepEqual(defaultCurve(i), cv(d), `default ${i}`));
  assert.deepEqual(defaultCurve(7), defaultCurve(0));
  assert.deepEqual(defaultCurve(-1), defaultCurve(0));
});

test("table entries: the complete argument rules of 0xEE / 0xEF equal km_pad_entry_valid", () => {
  for (const code of [0xee, 0xef]) {
    const maxOf = V.entry_max_arg1[String(code)];
    for (let a0 = 0; a0 < 256; a0++) {
      const max = maxOf[String(a0)] ?? -1;
      for (let a1 = 0; a1 < 256; a1++) {
        if (entryValid(code, a0, a1) !== (a1 <= max)) assert.fail(`entry ${code.toString(16)} ${a0} ${a1}`);
      }
    }
  }
  for (const code of [0x00, 0x04, 0xe8, 0xed, 0xf0, 0xff]) assert.equal(entryValid(code, 1, 0), false);
});

test("pad options validity equals km_pad_opts_valid", () => {
  assert.ok(V.opts_valid.length > 50);
  for (const [[dz, circ, sm, rs], ok] of V.opts_valid) {
    assert.equal(optsValid({ deadzone: dz, circular: circ, smooth: sm, rsvd: [rs, 0, 0, 0, 0] }), ok, `${dz} ${circ} ${sm} ${rs}`);
  }
});

test("key sequences: the port gives byte for byte the reports of the real km_pad_update (sticks, circle, dead zone, hat, buttons, cadence, faults, epochs)", () => {
  let steps = 0;
  let recomputes = 0;
  let nonNeutral = 0;
  V.scenarios.forEach((sc, si) => {
    const core = new PadCore();
    const ok = core.setConfig(sc.curves.map(cv), { deadzone: sc.opts[0], circular: sc.opts[1], smooth: sc.opts[2] });
    assert.equal(ok, sc.config_ok, `config ${si}`);
    sc.steps.forEach((st, i) => {
      if (sc.reset_at.includes(i)) core.reset();
      const depth = new Map(st.keys.map((k) => [k[0], k[4]]));
      const r = core.update({
        keys: st.keys.map((k) => ({ slot: k[0], code: k[1], arg0: k[2], arg1: k[3] })), allowed: st.allowed === 1, epoch: st.epoch, nowMs: st.now, depth: (s) => depth.get(s) ?? 0,
      });
      const exp = sc.out[i];
      if (reportHex(r.report) !== exp.report || r.recomputed !== exp.recomputed) assert.fail(`scenario ${si} step ${i}: ${reportHex(r.report)} (${r.recomputed}) != ${exp.report} (${exp.recomputed})`);
      steps++;
      if (r.recomputed) recomputes++;
      if (!reportNeutral(r.report)) nonNeutral++;
    });
  });
  assert.ok(steps > 7000 && recomputes > 5000 && nonNeutral > 4000, `${steps} ${recomputes} ${nonNeutral}`);
});

test("the SDL mapping lines equal the ones of tools/magger60_pad_map.py", () => {
  for (const platform of pad.PLATFORMS) assert.equal(pad.sdlMapping({ platform }), V.sdl[platform], platform);
});

test("the vectors are not stale (only when the gamepad core and a C compiler are in this checkout)", (t) => {
  const core = fileURLToPath(new URL("../../firmware/core/src/km_pad.c", import.meta.url));
  const padmap = fileURLToPath(new URL("../../tools/magger60_pad_map.py", import.meta.url));
  if (!existsSync(core) || !existsSync(padmap)) return t.skip("km_pad.c or magger60_pad_map.py is not in this checkout yet");
  const gen = fileURLToPath(new URL("./vectors/gen_pad_vectors.py", import.meta.url));
  const r = spawnSync("python", ["-I", gen, "--check"], { encoding: "utf8" });
  if (r.error || /no C compiler/.test(r.stdout + r.stderr)) return t.skip("python or a C compiler is not available");
  assert.equal(r.status, 0, r.stdout + r.stderr);
});

// ================================================================================================ curve maths on its own

test("a curve is 0 up to its start, full from its end, never goes backwards, and the reversed linear / S curves are the same", () => {
  for (const shape of [0, 1, 2, 3]) for (const flags of [0, 1]) for (const [start, end] of [[4, 64], [4, 8], [20, 68], [64, 68], [10, 40]]) {
    const c = { start, end, shape, flags };
    assert.equal(curveEval(c, 0), 0);
    assert.equal(curveEval(c, start * 5), 0);
    assert.equal(curveEval(c, end * 5), 32767);
    assert.equal(curveEval(c, 340), 32767);
    let prev = 0;
    for (let d = 0; d <= 340; d++) { const v = curveEval(c, d); assert.ok(v >= prev && v >= 0 && v <= 32767, `${shape}/${flags} ${d}`); prev = v; }
  }
  for (const shape of [SHAPE.LINEAR, SHAPE.S]) {
    for (let d = 0; d <= 340; d++) {
      const a = curveEval({ start: 4, end: 64, shape, flags: 0 }, d);
      const b = curveEval({ start: 4, end: 64, shape, flags: 1 }, d);
      assert.ok(Math.abs(a - b) <= 2, `shape ${shape} depth ${d}: ${a} vs ${b}`);
    }
  }
  for (let d = 0; d <= 340; d++) {   // reversing slow-then-fast gives fast-then-slow (up to rounding)
    const a = curveEval({ start: 4, end: 64, shape: SHAPE.EASE_IN, flags: 1 }, d);
    const b = curveEval({ start: 4, end: 64, shape: SHAPE.EASE_OUT, flags: 0 }, d);
    assert.ok(Math.abs(a - b) <= 2, `depth ${d}`);
  }
});

test("the output band: an axis follows its input only after 64 units; 0, full scale and a change of sign pass at once; faults are counted once", () => {
  const core = new PadCore();
  const curve = { start: 4, end: 68, shape: SHAPE.EASE_IN, flags: 0 };   // flat near its start: small steps between neighbouring depths
  core.setConfig([curve, curve, curve, curve], { deadzone: 0, circular: 0, smooth: 0 });
  const key = { slot: 3, code: PAD_CODE_AXIS, arg0: PAD_AXIS.RX, arg1: 0 };
  let now = 0;
  const at = (depth) => core.update({ keys: depth === null ? [] : [key], nowMs: (now += 2), depth: () => depth }).report.rx;
  // two depths whose outputs differ by exactly 63 / exactly 64 units, found on this curve
  const pair = (units) => {
    for (let a = 30; a < 330; a++) for (let b = a + 1; b < 340; b++) {
      const diff = curveEval(curve, b) - curveEval(curve, a);
      if (diff === units && curveEval(curve, a) > 0) return [a, b];
      if (diff > units) break;
    }
    return null;
  };
  const [a63, b63] = pair(63) ?? assert.fail("no 63-unit step on the curve");
  const [a64, b64] = pair(64) ?? assert.fail("no 64-unit step on the curve");
  const moved = (from, to) => { at(null); at(null); at(from); return at(to); };
  assert.equal(moved(a63, b63), curveEval(curve, a63), "63 units are not enough");
  assert.equal(moved(a64, b64), curveEval(curve, b64), "64 units are enough");
  at(null);
  assert.equal(at(340), 32767);
  at(null);
  assert.equal(core.update({ keys: [], nowMs: (now += 2), depth: () => 0 }).report.rx, 0);
  const f0 = core.counters.faults;
  core.update({ keys: [], allowed: false, nowMs: (now += 2) });
  core.update({ keys: [], allowed: false, nowMs: (now += 2) });
  assert.equal(core.counters.faults - f0, 1, "a continuing fault is counted once");
  assert.ok(reportNeutral(core.update({ keys: [key], allowed: false, nowMs: (now += 2), depth: () => 340 }).report));
});

test("known values of the default curve", () => {
  const c = defaultCurve(0);
  assert.equal(curveEval(c, 20), 0);
  assert.equal(curveEval(c, 170), 16384);          // the middle of 0.20..3.20 mm
  assert.equal(curveEval(c, 320), 32767);
  assert.equal(curveEval(defaultCurve(1), 110), 16384);   // the trigger curve is 0.20..2.00 mm: its middle is 1.10 mm
});

test("curve points for drawing run from the left edge to the right edge and follow curveEval", () => {
  const pts = pad.curvePoints(defaultCurve(3), 68);
  assert.equal(pts.length, 69);
  assert.equal(pts[0].mm, 0);
  assert.equal(pts.at(-1).mm, 3.4);
  assert.equal(pts[0].v, 0);
  assert.equal(pts.at(-1).v, 1);
  for (const p of pts) assert.equal(p.v, curveEval(defaultCurve(3), Math.round(p.mm * 100)) / 32767);
});

test("hat table: opposite directions cancel, the rest is the diagonal", () => {
  const up = 1, down = 2, left = 4, right = 8;
  assert.equal(hatOf(0), 0);
  assert.deepEqual([up, up | right, right, down | right, down, down | left, left, up | left].map(hatOf), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.equal(hatOf(up | down), 0);
  assert.equal(hatOf(left | right), 0);
  assert.equal(hatOf(up | down | right), 3);
  assert.equal(hatOf(up | down | left | right), 0);
});

test("stick: full deflection, the circle, the dead zone", () => {
  assert.deepEqual(stickShape(32767, 32767, 0, false), [32767, 32767]);        // a square without the circle option
  const [cx, cy] = stickShape(32767, 32767, 0, true);
  assert.ok(cx * cx + cy * cy <= 32767 * 32767 && cx > 23000 && cx === cy);
  assert.deepEqual(stickShape(1000, 1000, 10, false), [0, 0]);                  // inside a 10 % dead zone
  assert.deepEqual(stickShape(32767, 0, 10, false), [32767, 0]);                // full stays full
  assert.deepEqual(stickShape(0, -32767, 50, true), [0, -32767]);
  const [dx] = stickShape(16383, 0, 10, false);
  assert.ok(dx > 0 && dx < 16383);                                              // rescaled, smaller than what was put in
});

test("report bytes: layout, round trip, neutral", () => {
  const r = { buttons: 0x8001, hat: 5, lx: -32767, ly: 12345, rx: 1, ry: -1, lt: 32767, rt: 7 };
  const bytes = packReport(r);
  assert.equal(bytes.length, 15);
  assert.equal(reportHex(r).slice(0, 6), "018005");
  assert.deepEqual([...bytes.slice(0, 3)], [0x01, 0x80, 5]);
  assert.deepEqual(unpackReport(bytes), r);
  assert.ok(reportNeutral(neutralReport()));
  assert.equal(reportHex(neutralReport()), "00".repeat(15));
  assert.equal(packReport({ ...neutralReport(), hat: 9 })[2], 0);   // an out-of-range hat packs as neutral
});

// ================================================================================================ bindings

test("every gamepad function has a distinct key-map entry the firmware accepts, and comes back as the same spec", () => {
  const seen = new Set();
  for (const tg of pad.TARGETS) {
    for (const digital of tg.kind === "axis" ? [false, true] : [false]) {
      for (const curve of tg.kind === "axis" ? [0, 1, 2, 3] : [0]) {
        for (const act of tg.kind === "axis" ? [0] : [0, 4, 40, 68]) {
          const spec = { target: tg.id, digital, curve, act };
          assert.equal(pad.specProblem(spec), null, JSON.stringify(spec));
          const e = pad.specToEntry(spec);
          assert.equal(entryValid(e.code, e.arg0, e.arg1), true, `${tg.id} ${e.code} ${e.arg0} ${e.arg1}`);
          assert.equal(entryProblem(e.code, e.arg0, e.arg1), null);
          const key = `${e.code}/${e.arg0}/${e.arg1}`;
          assert.ok(!seen.has(key), `entry used twice: ${key}`);
          seen.add(key);
          const b = pad.specToBinding(spec);
          assert.deepEqual(pad.specOfBinding(b), spec, `round trip ${JSON.stringify(spec)}`);
          assert.ok(pad.isPadBinding(b));
        }
      }
    }
  }
  assert.equal(pad.TARGETS.length, 8 + 2 + 16 + 4);
});

test("entry bytes are exactly the ones km_pad_report documents", () => {
  const e = (target, extra = {}) => pad.specToEntry({ ...pad.newSpec(target), ...extra });
  assert.deepEqual(e("lx+"), { code: 0xef, arg0: 0, arg1: 0 });
  assert.deepEqual(e("lx-"), { code: 0xef, arg0: 0x08, arg1: 0 });
  assert.deepEqual(e("ly-", { digital: true, curve: 3 }), { code: 0xef, arg0: 0x19, arg1: 3 });
  assert.deepEqual(e("ry+"), { code: 0xef, arg0: 3, arg1: 0 });
  assert.deepEqual(e("rx-"), { code: 0xef, arg0: 0x0a, arg1: 0 });
  assert.deepEqual(e("lt"), { code: 0xef, arg0: 4, arg1: 1 });   // a trigger starts on curve 2
  assert.deepEqual(e("rt", { curve: 2 }), { code: 0xef, arg0: 5, arg1: 2 });
  assert.deepEqual(e("b1"), { code: 0xee, arg0: 1, arg1: 0 });
  assert.deepEqual(e("b16", { act: 40 }), { code: 0xee, arg0: 16, arg1: 40 });
  assert.deepEqual(e("hat-up"), { code: 0xee, arg0: 17, arg1: 0 });
  assert.deepEqual(e("hat-right"), { code: 0xee, arg0: 20, arg1: 0 });
  assert.equal(pad.targetById("ly-").cap, "L↑");
  assert.equal(pad.targetById("hat-left").cap, "D←");
});

test("specs the page must refuse, in plain reason codes", () => {
  assert.equal(pad.specProblem(null), "target");
  assert.equal(pad.specProblem({ target: "nope", digital: false, curve: 0, act: 0 }), "target");
  assert.equal(pad.specProblem({ target: "lx+", digital: false, curve: 4, act: 0 }), "curve");
  assert.equal(pad.specProblem({ target: "lx+", digital: false, curve: -1, act: 0 }), "curve");
  assert.equal(pad.specProblem({ target: "b1", digital: false, curve: 0, act: 69 }), "act_range");
  assert.equal(pad.specProblem({ target: "b1", digital: false, curve: 0, act: 1.5 }), "act_range");
  assert.equal(pad.specProblem({ target: "b1", digital: false, curve: 0, act: 3 }), "act_low");   // the firmware takes it, but would raise it to 0.20 mm
  assert.equal(pad.specProblem({ target: "b1", digital: false, curve: 0, act: 4 }), null);
  assert.throws(() => pad.specToEntry({ target: "b1", digital: false, curve: 0, act: 99 }), RangeError);
  // the firmware itself accepts act 1..3, so the UI rule is the tighter one
  for (let act = 0; act <= 68; act++) assert.equal(entryProblem(0xee, 1, act), null);
  assert.notEqual(entryProblem(0xee, 1, 69), null);
});

test("bindings that are not valid gamepad entries are not gamepad keys", () => {
  assert.equal(pad.specOfBinding(null), null);
  assert.equal(pad.specOfBinding({ type: "key", usage: 4 }), null);
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xed << 16) | 0x0000 }), null);                 // DKS
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xef << 16) | (0x0c << 8) }), null);            // right trigger with the negative bit
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xef << 16) | (0x20 << 8) }), null);            // a reserved bit
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xef << 16) | (0x06 << 8) }), null);            // axis 6 does not exist
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xee << 16) | (21 << 8) }), null);
  assert.equal(pad.specOfBinding({ type: "fw", index: (0xee << 16) | (0 << 8) }), null);
  assert.equal(pad.specOfBinding({ type: "fw", index: -1 }), null);
  assert.equal(pad.specOfBinding({ type: "fw", index: 0x1000000 }), null);
  assert.equal(pad.isPadBinding({ type: "fw", index: (0xee << 16) | (21 << 8) }), true);   // still a gamepad entry: the page knows it must not leave it alone
  assert.equal(pad.isPadBinding({ type: "fw", index: 0xed0000 }), false);
});

test("the P0 codec writes a gamepad binding as the firmware's entry and reads it back", () => {
  for (const tg of pad.TARGETS) {
    const spec = { ...pad.newSpec(tg.id), act: tg.kind === "axis" ? 0 : 10 };
    const b = pad.specToBinding(spec);
    for (const layer of [0, 1, 3]) {
      const r = bindingToCode(b, layer);
      const e = pad.specToEntry(spec);
      assert.deepEqual([r.code, r.args[0], r.args[1]], [e.code, e.arg0, e.arg1]);
      assert.equal(r.problem, undefined);
      assert.deepEqual(codeToBinding(r.code, layer, r.args), b);
    }
  }
});

// ================================================================================================ the whole way through the settings blob

const workspaceWith = (fn) => {
  const ws = defaultWorkspace(layout);
  fn(ws);
  return ws;
};
const bind = (profile, id, layer, spec) => { profile.keys[id].bindings[layer] = pad.specToBinding(spec); };

test("a profile without gamepad settings still saves as format 2 (a firmware without the gamepad can read it)", () => {
  const ws = defaultWorkspace(layout);
  const r = workspaceToBlob(ws, layout);
  assert.equal(r.problems.length, 0);
  assert.equal(r.bytes.length, FW_SETTINGS.size);
  assert.equal(r.bytes[4], 2);
  // going through the page's setter with the default pad leaves nothing behind
  const p = ws.profiles[0];
  pad.setPad(p, defaultPad());
  assert.equal(p.m6, undefined);
  assert.equal(workspaceToBlob(ws, layout).bytes[4], 2);
});

test("bindings, curves, options and pad mode round-trip through the blob (format 3) and come out the same", () => {
  const ws = workspaceWith((w) => {
    const p = w.profiles[1];
    bind(p, "KeyW", 0, pad.newSpec("ly-"));
    bind(p, "KeyS", 0, pad.newSpec("ly+"));
    bind(p, "KeyA", 0, pad.newSpec("lx-"));
    bind(p, "KeyD", 0, { ...pad.newSpec("lx+"), digital: true });
    bind(p, "KeyQ", 3, pad.newSpec("lt"));
    bind(p, "KeyE", 3, { ...pad.newSpec("rt"), curve: 2 });
    bind(p, "Space", 3, { ...pad.newSpec("b1"), act: 40 });
    bind(p, "KeyT", 3, pad.newSpec("hat-up"));
    const set = pad.padOf(p);
    set.curves[2] = { start: 8, end: 50, shape: 1, flags: 1 };
    set.deadzone = 20;
    set.circular = 0;
    pad.setPad(p, set);
    w.m6 = { device: { padMode: 1, filterMode: 0 }, unknown: [] };
  });
  const r = workspaceToBlob(ws, layout);
  assert.deepEqual(r.problems, []);
  assert.equal(r.bytes[4], 3);
  assert.ok(needsV3(decodeSettings(r.bytes)));
  const back = blobToWorkspace(r.bytes, layout);
  assert.equal(back.m6.device.padMode, 1);
  const p = back.profiles[1];
  assert.deepEqual(pad.padOf(p).curves[2], { start: 8, end: 50, shape: 1, flags: 1 });
  assert.equal(p.m6.pad.deadzone, 20);
  assert.equal(p.m6.pad.circular, 0);
  const keys = pad.padKeys(p, layout, 0);
  assert.deepEqual([...keys].map(([s, sp]) => [s, sp.target, sp.digital]).sort((a, b) => a[0] - b[0]),
    [["KeyA", "lx-", false], ["KeyD", "lx+", true], ["KeyS", "ly+", false], ["KeyW", "ly-", false]].map(([id, t, d]) => [slotOf(id), t, d]).sort((a, b) => a[0] - b[0]));
  const l3 = pad.padKeys(p, layout, 3);
  assert.deepEqual(l3.get(slotOf("KeyE")), { target: "rt", digital: false, curve: 2, act: 0 });
  assert.deepEqual(l3.get(slotOf("Space")), { target: "b1", digital: false, curve: 0, act: 40 });
  assert.equal(l3.get(slotOf("KeyQ")).target, "lt");
  assert.equal(l3.get(slotOf("KeyT")).target, "hat-up");
  assert.equal(pad.padKeys(back.profiles[0], layout, 0).size, 0);   // the other profiles are untouched
  // writing it again gives the same bytes, and the dirty check's normal form is stable
  assert.deepEqual(workspaceToBlob(back, layout).bytes, r.bytes);
  assert.deepEqual(normalizeBlob(r.bytes, layout), normalizeBlob(normalizeBlob(r.bytes, layout), layout));
  // what the firmware's own decoder reads: the key map entries and the pad section
  const raw = decodeSettings(r.bytes);
  assert.equal(raw.profiles[1].layers[0][slotOf("KeyW")], CODE.PAD_AXIS);
  assert.deepEqual(raw.profiles[1].args[0][slotOf("KeyW")], [0x08 | PAD_AXIS.LY, 0]);
  assert.deepEqual(raw.profiles[1].args[0][slotOf("KeyD")], [0x10 | PAD_AXIS.LX, 0]);
  assert.equal(raw.profiles[1].layers[3][slotOf("Space")], CODE.PAD_BTN);
  assert.deepEqual(raw.profiles[1].args[3][slotOf("Space")], [1, 40]);
  assert.deepEqual(raw.profiles[1].pad.curves[2], { start: 8, end: 50, shape: 1, flags: 1 });
  assert.equal(raw.device.padMode, 1);
});

test("pad mode alone is enough for format 3, and turning it off again returns to format 2", () => {
  const ws = defaultWorkspace(layout);
  ws.m6 = pad.withPadMode(null, pad.PAD_MODE.HID);
  assert.equal(workspaceToBlob(ws, layout).bytes[4], 3);
  assert.equal(blobToWorkspace(workspaceToBlob(ws, layout).bytes, layout).m6.device.padMode, 1);
  ws.m6 = pad.withPadMode(ws.m6, pad.PAD_MODE.OFF);
  const r = workspaceToBlob(ws, layout);
  assert.equal(r.bytes[4], 2);
  assert.equal(blobToWorkspace(r.bytes, layout).m6, undefined);
});

test("limits: every key on every layer as a gamepad key fits the blob, and the sections stay within the format's size", () => {
  const ws = workspaceWith((w) => {
    for (const p of w.profiles) {
      layout.keys.forEach((k, i) => {
        for (let l = 0; l < 4; l++) {
          const tg = pad.TARGETS[(i + l * 7) % pad.TARGETS.length];
          bind(p, k.id, l, { ...pad.newSpec(tg.id), curve: (i + l) % 4, act: tg.kind === "axis" ? 0 : (i % 3) * 20 });
        }
      });
      const s = pad.padOf(p);
      s.deadzone = 50;
      s.curves = [{ start: 67, end: 68, shape: 3, flags: 0 }, { start: 4, end: 68, shape: 2, flags: 1 }, { start: 30, end: 34, shape: 1, flags: 0 }, { start: 4, end: 8, shape: 0, flags: 1 }]
        .map((c) => (c.end - c.start < 4 ? { ...c, start: 64 } : c));
      pad.setPad(p, s);
    }
    w.m6 = pad.withPadMode(null, pad.PAD_MODE.HID);
  });
  const r = workspaceToBlob(ws, layout);
  assert.deepEqual(r.problems, []);
  assert.ok(r.bytes.length <= FW_SETTINGS.v3MaxEncoded && r.bytes.length <= FW_SETTINGS.v3MaxSize, `${r.bytes.length} bytes`);
  const back = blobToWorkspace(r.bytes, layout);
  for (let i = 0; i < 4; i++) for (let l = 0; l < 4; l++) assert.equal(pad.padKeys(back.profiles[i], layout, l).size, 61, `profile ${i} layer ${l}`);
  assert.deepEqual(workspaceToBlob(back, layout).bytes, r.bytes);
});

test("what the keyboard would refuse is refused before anything is written, with a reason", () => {
  const refused = (fn) => {
    const ws = workspaceWith((w) => fn(w.profiles[0], w));
    const r = workspaceToBlob(ws, layout);
    assert.equal(r.bytes, null);
    assert.equal(r.problems[0].reason, "settings");
  };
  const badCurve = (c) => refused((p) => { const s = pad.padOf(p); s.curves[0] = c; p.m6 = { pad: s }; });
  badCurve({ start: 3, end: 64, shape: 0, flags: 0 });       // starts in the sensor noise
  badCurve({ start: 4, end: 7, shape: 0, flags: 0 });        // closer than 0.20 mm
  badCurve({ start: 4, end: 69, shape: 0, flags: 0 });
  badCurve({ start: 4, end: 64, shape: 4, flags: 0 });
  badCurve({ start: 4, end: 64, shape: 0, flags: 2 });
  refused((p) => { const s = pad.padOf(p); s.deadzone = 51; p.m6 = { pad: s }; });
  refused((p) => { const s = pad.padOf(p); s.circular = 2; p.m6 = { pad: s }; });
  refused((p, w) => { w.m6 = { device: { padMode: 3, filterMode: 0 }, unknown: [] }; });
  assert.equal(pad.withPadMode(null, 3), null);
  assert.equal(pad.withPadMode(null, 255), null);
  // an entry the firmware refuses (the page never makes one, but a stored profile could hold it)
  const ws = workspaceWith((w) => { w.profiles[0].keys.KeyW.bindings[0] = { type: "fw", index: (0xef << 16) | (0x0c << 8) }; });
  const r = workspaceToBlob(ws, layout);
  assert.equal(r.bytes, null);
  assert.equal(r.problems[0].reason, "firmware_rule");
});

test("padProblem / padCurveProblem accept exactly what the page's curve rules produce", () => {
  assert.equal(padProblem(defaultPad()), null);
  assert.equal(padCurveProblem({ start: 4, end: 8, shape: 0, flags: 0 }), null);
  assert.notEqual(padCurveProblem({ start: 4, end: 7, shape: 0, flags: 0 }), null);
  assert.equal(FW_M6.padSpanMin, pad.CURVE_SPAN_MIN);
  assert.equal(FW_M6.padStartMin * 5, 20);
  assert.equal(pad.BTN_ACT_MIN * 5, 20);
});

// ================================================================================================ editing a curve

test("changeCurve keeps start and end legal and says why a value moved", () => {
  const c = { start: 4, end: 64, shape: 0, flags: 0 };
  assert.deepEqual(pad.changeCurve(c, "start", 10), { curve: { ...c, start: 10 }, note: null });
  assert.deepEqual(pad.changeCurve(c, "start", 1), { curve: { ...c, start: 4 }, note: "start_low" });
  assert.deepEqual(pad.changeCurve(c, "start", 63), { curve: { ...c, start: 60 }, note: "start_high" });
  assert.deepEqual(pad.changeCurve(c, "start", 60), { curve: { ...c, start: 60 }, note: null });
  assert.deepEqual(pad.changeCurve(c, "end", 40), { curve: { ...c, end: 40 }, note: null });
  assert.deepEqual(pad.changeCurve(c, "end", 99), { curve: { ...c, end: 68 }, note: "end_high" });
  assert.deepEqual(pad.changeCurve(c, "end", 5), { curve: { ...c, end: 8 }, note: "end_low" });
  assert.deepEqual(pad.changeCurve(c, "shape", 2).curve.shape, 2);
  assert.deepEqual(pad.changeCurve(c, "shape", 9).curve.shape, 3);
  assert.deepEqual(pad.changeCurve(c, "shape", -3).curve.shape, 0);
  assert.equal(pad.changeCurve(c, "reverse", true).curve.flags, 1);
  assert.equal(pad.changeCurve({ ...c, flags: 1 }, "reverse", false).curve.flags, 0);
  assert.equal(pad.changeCurve(c, "start", 10.4).curve.start, 10);
  assert.equal(c.start, 4);   // the input is not changed
});

test("changeCurve never produces something the keyboard would refuse, whatever is dragged to", () => {
  let seed = 12345;
  const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  let curve = { start: 4, end: 64, shape: 0, flags: 0 };
  for (let i = 0; i < 20000; i++) {
    const field = ["start", "end", "shape", "reverse"][rnd(4)];
    const value = field === "reverse" ? rnd(2) === 1 : rnd(160) - 40;
    curve = pad.changeCurve(curve, field, value).curve;
    if (!curveValid(curve) || padCurveProblem(curve) !== null) assert.fail(`${field} ${value} gave ${JSON.stringify(curve)}`);
  }
});

test("curve units: 0.05 mm steps", () => {
  assert.equal(pad.unitsToMm(4), 0.2);
  assert.equal(pad.unitsToMm(68), 3.4);
  assert.equal(pad.mmToUnits(1.25), 25);
  for (let u = 0; u <= 68; u++) assert.equal(pad.mmToUnits(pad.unitsToMm(u)), u);
});

test("curve templates are the firmware's default curves and are valid", () => {
  assert.deepEqual(pad.CURVE_PRESETS.map((id) => pad.curvePreset(id)), [0, 1, 2, 3].map(defaultCurve));
  for (const id of pad.CURVE_PRESETS) assert.equal(padCurveProblem(pad.curvePreset(id)), null);
});

test("setPad / padOf: a changed pad is kept, a default one leaves no trace, copies are independent", () => {
  const p = defaultProfile(layout);
  const a = pad.padOf(p);
  a.curves[0].end = 40;
  assert.equal(p.m6, undefined);                 // padOf returns a copy
  pad.setPad(p, a);
  assert.equal(p.m6.pad.curves[0].end, 40);
  a.curves[0].end = 12;
  assert.equal(p.m6.pad.curves[0].end, 40);      // setPad stored a copy
  p.m6.dks = [{ keep: 1 }];
  pad.setPad(p, defaultPad());
  assert.deepEqual(p.m6, { dks: [{ keep: 1 }] }); // other M6 data is kept
  delete p.m6.dks;
  pad.setPad(p, { ...defaultPad(), deadzone: 0 });
  assert.equal(padIsDefault(p.m6.pad), false);
});

// ================================================================================================ bindings of a profile

test("applyBindings gives keys a function and restores them; keys that were not gamepad keys are left alone", () => {
  const p = defaultProfile(layout);
  const restore = (key, layer) => (layer === 0 ? { type: "key", usage: key.usage } : null);
  const w0 = JSON.stringify(p.keys.KeyW.bindings);
  assert.equal(pad.applyBindings(p, layout, pad.presetEdits("wasd", layout, 0), restore), 4);
  assert.equal(pad.padKeys(p, layout, 0).size, 4);
  assert.equal(pad.padKeys(p, layout, 1).size, 0);
  assert.equal(pad.specOfBinding(p.keys.KeyW.bindings[0]).target, "ly-");
  // removing: only gamepad keys change; layer 0 gets the key's own key back, an overlay layer gets "transparent"
  p.keys.KeyA.bindings[2] = { type: "key", usage: 0x2c };
  const n = pad.applyBindings(p, layout, [
    { keyId: "KeyW", layer: 0, spec: null }, { keyId: "KeyA", layer: 2, spec: null }, { keyId: "KeyB", layer: 0, spec: null },
  ], restore);
  assert.equal(n, 1);
  assert.equal(JSON.stringify(p.keys.KeyW.bindings), w0);
  assert.deepEqual(p.keys.KeyA.bindings[2], { type: "key", usage: 0x2c });
  bind(p, "KeyE", 2, pad.newSpec("rt"));
  pad.applyBindings(p, layout, [{ keyId: "KeyE", layer: 2, spec: null }], restore);
  assert.equal(p.keys.KeyE.bindings[2], null);
  assert.equal(pad.applyBindings(p, layout, [{ keyId: "Nope", layer: 0, spec: pad.newSpec("b1") }], restore), 0);
});

test("quick setups name real keys of this keyboard", () => {
  for (const name of Object.keys(pad.PRESETS)) {
    const edits = pad.presetEdits(name, layout, 2);
    assert.equal(edits.length, Object.keys(pad.PRESETS[name]).length, name);
    for (const e of edits) { assert.ok(layout.byId.has(e.keyId)); assert.equal(e.layer, 2); assert.equal(pad.specProblem(e.spec), null); }
    assert.equal(new Set(edits.map((e) => e.keyId)).size, edits.length);
  }
  assert.deepEqual(pad.presetEdits("nothing", layout, 0), []);
  const wasd = Object.fromEntries(pad.presetEdits("wasd", layout, 0).map((e) => [e.keyId, e.spec.target]));
  assert.deepEqual(wasd, { KeyW: "ly-", KeyA: "lx-", KeyS: "ly+", KeyD: "lx+" });
});

test("curve use counts the keys of all layers that use each curve, digital keys do not", () => {
  const p = defaultProfile(layout);
  bind(p, "KeyW", 0, { ...pad.newSpec("ly-"), curve: 2 });
  bind(p, "KeyS", 0, { ...pad.newSpec("ly+"), curve: 2 });
  bind(p, "KeyA", 1, { ...pad.newSpec("lx-"), curve: 0 });
  bind(p, "KeyD", 1, { ...pad.newSpec("lx+"), curve: 3, digital: true });
  bind(p, "KeyQ", 3, pad.newSpec("lt"));
  bind(p, "Space", 3, pad.newSpec("b1"));
  assert.deepEqual(pad.curveUse(p, layout), [1, 1, 2, 0]);
  assert.deepEqual(pad.padKeyCounts(p, layout), [2, 2, 0, 2]);
});

// ================================================================================================ checks in plain terms

test("checks: keys bound while pad mode is off, reserved mode, a layer nobody can reach, a stick that can only go one way", () => {
  const ids = (p, mode) => pad.checkPad(p, layout, mode).map((c) => c.id);
  const p = defaultProfile(layout);
  assert.deepEqual(ids(p, 0), []);
  bind(p, "KeyW", 0, pad.newSpec("ly-"));
  bind(p, "KeyS", 0, pad.newSpec("ly+"));
  assert.deepEqual(ids(p, 0), ["off_but_bound"]);
  assert.equal(pad.checkPad(p, layout, 0)[0].n, 2);
  assert.equal(pad.checkPad(p, layout, 0)[0].level, "warn");
  assert.deepEqual(ids(p, 1), []);
  assert.deepEqual(ids(p, 2), []);                  // XInput mode: nothing to explain for buttons 1..11 and sticks
  bind(p, "KeyZ", 0, pad.newSpec("b12"));
  bind(p, "KeyX", 0, pad.newSpec("b16"));
  bind(p, "KeyC", 0, pad.newSpec("b11"));
  assert.deepEqual(ids(p, 1), []);                  // the standard gamepad has all 16
  assert.deepEqual(ids(p, 2), ["xinput_dropped"]);  // an Xbox-compatible pad has 11: buttons 12..16 send nothing
  assert.equal(pad.checkPad(p, layout, 2)[0].n, 2);
  assert.equal(pad.XINPUT_BUTTONS, 11);
  p.keys.KeyZ.bindings[0] = null; p.keys.KeyX.bindings[0] = null; p.keys.KeyC.bindings[0] = null;
  bind(p, "KeyD", 0, pad.newSpec("lx+"));          // right without left: a stick that only goes one way
  const half = pad.checkPad(p, layout, 1);
  assert.deepEqual(half.map((c) => c.id), ["half_axis"]);
  assert.equal(half[0].level, "info");
  assert.deepEqual(half[0].axes, [{ layer: 0, axis: "lx" }]);
  bind(p, "KeyA", 0, pad.newSpec("lx-"));
  assert.deepEqual(ids(p, 1), []);
  // triggers have one direction by nature
  bind(p, "KeyQ", 0, pad.newSpec("lt"));
  assert.deepEqual(ids(p, 1), []);
  // layer 3 with gamepad keys but nothing that switches there
  bind(p, "KeyE", 3, pad.newSpec("rt"));
  const un = pad.checkPad(p, layout, 1);
  assert.deepEqual(un.map((c) => c.id), ["unreachable"]);
  assert.deepEqual(un[0].layers, [3]);
  p.keys.Space.bindings[0] = { type: "toggle", layer: 3 };
  assert.deepEqual(ids(p, 1), []);
});

test("pad mode lives in the global device options and an edit goes through app.edit", () => {
  const app = { m6: null, calls: 0, edit(fn) { this.calls++; return fn({}); } };
  assert.equal(pad.padModeOf(app), 0);
  assert.equal(pad.setPadMode(app, 1), true);
  assert.equal(app.calls, 1);
  assert.deepEqual(app.m6, { device: { padMode: 1, filterMode: 0 }, unknown: [] });
  assert.equal(pad.padModeOf(app), 1);
  app.m6 = { device: { padMode: 1, filterMode: 2 }, unknown: [{ id: 0x90, scope: 255, bytes: [1] }] };
  assert.equal(pad.setPadMode(app, 0), true);
  assert.deepEqual(app.m6, { device: { padMode: 0, filterMode: 2 }, unknown: [{ id: 0x90, scope: 255, bytes: [1] }] });   // the rest is kept as it was
  assert.equal(pad.setPadMode(app, 7), false);
  assert.equal(app.calls, 2);
  assert.deepEqual(pad.deviceOf({}), defaultDevice());
});

// ================================================================================================ live preview

test("the live preview follows key depths the way the keyboard does", () => {
  const p = defaultProfile(layout);
  const set = pad.padOf(p);
  set.deadzone = 0; set.circular = 0;
  pad.setPad(p, set);
  bind(p, "KeyW", 0, pad.newSpec("ly-"));
  bind(p, "KeyS", 0, pad.newSpec("ly+"));
  bind(p, "KeyD", 0, pad.newSpec("lx+"));
  bind(p, "KeyE", 0, pad.newSpec("rt"));
  bind(p, "Space", 0, pad.newSpec("b1"));
  bind(p, "KeyT", 0, { ...pad.newSpec("b2"), act: 40 });    // from 2.00 mm
  const preview = new pad.PadPreview();
  const keys = pad.padKeys(p, layout, 0);
  preview.configure(pad.padOf(p), keys);
  const depths = new Uint16Array(76);
  let now = 100;
  const step = () => preview.step(depths, (now += 2));
  assert.ok(reportNeutral(step()));
  depths[slotOf("KeyW")] = 340;
  const full = step();
  assert.equal(full.ly, -32767);
  depths[slotOf("KeyW")] = 0;
  depths[slotOf("KeyS")] = 170;
  assert.equal(step().ly, 16384);                           // half way of 0.20..3.20 mm
  depths[slotOf("KeyS")] = 10;                              // shallower than the curve start (and the engine's release point)
  assert.equal(step().ly, 0);
  depths[slotOf("KeyD")] = 340; depths[slotOf("KeyW")] = 340;
  const diag = step();
  assert.deepEqual([diag.lx, diag.ly], [32767, -32767]);   // a square, because the circle option is off
  depths.fill(0);
  step();
  depths[slotOf("KeyE")] = 120;
  assert.equal(step().rt, curveEval(pad.padOf(p).curves[1], 120));   // a trigger uses its own curve
  depths[slotOf("Space")] = 25;
  assert.equal(step().buttons & 1, 1);                      // follows the key's press, from 0.20 mm
  depths[slotOf("Space")] = 12;
  assert.equal(step().buttons & 1, 1);                      // still down: it only goes up below 0.10 mm
  depths[slotOf("Space")] = 5;
  assert.equal(step().buttons & 1, 0);
  depths[slotOf("KeyT")] = 150;
  assert.equal(step().buttons & 2, 0);                      // the depth button waits for 2.00 mm
  depths[slotOf("KeyT")] = 210;
  assert.equal(step().buttons & 2, 2);
  depths[slotOf("KeyT")] = 190;
  assert.equal(step().buttons & 2, 2);                      // hysteresis of 0.20 mm
  depths[slotOf("KeyT")] = 170;
  assert.equal(step().buttons & 2, 0);
});

test("a key that is already down when the preview starts is held over until it has been released (the firmware's rule)", () => {
  const p = defaultProfile(layout);
  bind(p, "KeyW", 0, pad.newSpec("ly-"));
  const preview = new pad.PadPreview();
  preview.configure(pad.padOf(p), pad.padKeys(p, layout, 0));
  const depths = new Uint16Array(76);
  depths[slotOf("KeyW")] = 340;
  assert.ok(reportNeutral(preview.step(depths, 10)));
  assert.ok(reportNeutral(preview.step(depths, 12)));
  depths[slotOf("KeyW")] = 0;
  preview.step(depths, 14);
  depths[slotOf("KeyW")] = 340;
  assert.notEqual(preview.step(depths, 16).ly, 0);
});

test("the circle and dead-zone options change the preview, a changed configuration resets it", () => {
  const p = defaultProfile(layout);
  bind(p, "KeyW", 0, pad.newSpec("ly-"));
  bind(p, "KeyD", 0, pad.newSpec("lx+"));
  const preview = new pad.PadPreview();
  const depths = new Uint16Array(76);
  depths[slotOf("KeyW")] = 340;
  depths[slotOf("KeyD")] = 340;
  const run = (opts) => {
    const set = { ...pad.padOf(p), ...opts };
    preview.configure(set, pad.padKeys(p, layout, 0));
    depths.fill(0);
    preview.step(depths, 1000 + (run.n = (run.n ?? 0) + 10));
    depths[slotOf("KeyW")] = 340;
    depths[slotOf("KeyD")] = 340;
    return preview.step(depths, 1000 + (run.n += 10));
  };
  const square = run({ circular: 0, deadzone: 0 });
  assert.deepEqual([square.lx, square.ly], [32767, -32767]);
  const circle = run({ circular: 1, deadzone: 0 });
  assert.ok(circle.lx * circle.lx + circle.ly * circle.ly <= 32767 * 32767 && circle.lx < 32767);
  const dead = run({ circular: 0, deadzone: 30 });
  assert.deepEqual([dead.lx, dead.ly], [32767, -32767]);
});

// ================================================================================================ SDL text

test("SDL GUID and mapping text", () => {
  assert.equal(pad.sdlGuid("windows"), "03000000f519000049fb000000000000");
  assert.equal(pad.sdlGuid("linux"), "03000000f519000049fb000002000000");
  assert.equal(pad.sdlGuid("mac"), pad.sdlGuid("windows"));
  assert.equal(pad.sdlGuid("windows", { vid: 0x1234, pid: 0xabcd }), "030000003412000" + "0cdab000000000000");
  for (const platform of pad.PLATFORMS) {
    const line = pad.sdlMapping({ platform });
    const items = line.split(",");
    assert.equal(items.at(-1), "");
    assert.match(items[0], /^[0-9a-f]{32}$/);
    const m = Object.fromEntries(items.slice(2, -1).map((x) => x.split(/:(.*)/s).slice(0, 2)));
    assert.deepEqual([m.leftx, m.lefty, m.lefttrigger, m.rightx, m.righty, m.righttrigger], ["a0", "a1", "a2", "a3", "a4", "a5"]);
    assert.deepEqual([m.a, m.b, m.x, m.y, m.start, m.guide, m.misc1], ["b0", "b1", "b2", "b3", "b7", "b10", "b11"]);
    assert.deepEqual([m.dpup, m.dpright, m.dpdown, m.dpleft], ["h0.1", "h0.2", "h0.4", "h0.8"]);
    const values = Object.entries(m).filter(([k]) => k !== "platform").map(([, v]) => v);
    assert.equal(new Set(values).size, values.length, "an input is used twice");
  }
  const custom = "AABBCCDDAABBCCDDAABBCCDDAABBCCDD";
  assert.ok(pad.sdlMapping({ guid: custom }).startsWith(custom.toLowerCase() + ","));
  assert.throws(() => pad.sdlMapping({ guid: "xyz" }), RangeError);
  assert.equal(pad.isGuid(custom), true);
  assert.equal(pad.isGuid(`${custom}0`), false);
  assert.equal(pad.isGuid("g".repeat(32)), false);
  assert.equal(pad.isGuid(`  ${custom}  `), true);
});

// ================================================================================================ strings and styles

const placeholders = (s) => (s.match(/\{\w+\}/g) ?? []).sort().join();

test("both languages have the same keys and placeholders", () => {
  for (const k of Object.keys(en)) assert.ok(k in zh, `missing in zh-TW: ${k}`);
  for (const k of Object.keys(zh)) assert.ok(k in en, `missing in en: ${k}`);
  for (const k of Object.keys(en)) assert.equal(placeholders(zh[k]), placeholders(en[k]), k);
  for (const dict of [zh, en]) for (const [k, v] of Object.entries(dict)) assert.ok(typeof v === "string" && v.trim(), `empty: ${k}`);
});

test("every string the page asks for exists", async () => {
  const { readdirSync } = await import("node:fs");
  const used = new Set();
  const files = ["../js/pages/pad.js", "../js/ui/pad-view.js", "../js/ui/pad-curve.js"].map((f) => readFileSync(new URL(f, import.meta.url), "utf8"));
  for (const src of files) {
    for (const m of src.matchAll(/\bpt\("([\w.\-+]+)"/g)) used.add(m[1]);
    for (const m of src.matchAll(/\bpt\(`([^`]*)`/g)) used.add(m[1]);   // families, expanded below
  }
  assert.ok(used.size > 60);
  const families = [...used].filter((k) => k.includes("${"));
  const literal = [...used].filter((k) => !k.includes("${"));
  for (const k of literal) assert.ok(k in zh && k in en, `missing string: ${k}`);
  const expand = {
    "pad.bad.${bad}": ["target", "curve", "act_range", "act_low"].map((c) => `pad.bad.${c}`),
    "pad.chk.${c.id}": ["off_but_bound", "xinput_dropped", "unreachable", "half_axis"].map((c) => `pad.chk.${c}`),
    "pad.cpreset.${id}": pad.CURVE_PRESETS.map((c) => `pad.cpreset.${c}`),
    "pad.cpreset.${id}.tip": pad.CURVE_PRESETS.map((c) => `pad.cpreset.${c}.tip`),
    "pad.curve.note.${r.note}": ["start_low", "start_high", "end_low", "end_high"].map((c) => `pad.curve.note.${c}`),
    "pad.grp.${g}": pad.TARGET_GROUPS.map((g) => `pad.grp.${g}`),
    "pad.mode.e${i}": [1, 2, 3, 4, 5].map((i) => `pad.mode.e${i}`),
    "pad.preset.${id}": Object.keys(pad.PRESETS).map((c) => `pad.preset.${c}`),
    "pad.preset.${id}.tip": Object.keys(pad.PRESETS).map((c) => `pad.preset.${c}.tip`),
    "pad.sdl.${p}": pad.PLATFORMS.map((c) => `pad.sdl.${c}`),
    "pad.shape.${i}": [0, 1, 2, 3].map((i) => `pad.shape.${i}`),
    "pad.shape.${i}.tip": [0, 1, 2, 3].map((i) => `pad.shape.${i}.tip`),
    "pad.where.${i}": [1, 2, 3, 4].map((i) => `pad.where.${i}`),
  };
  for (const f of families) {
    assert.ok(expand[f], `a string family the test does not know: ${f}`);
    for (const k of expand[f]) assert.ok(k in zh && k in en, `missing string: ${k}`);
  }
  // keys made in other ways
  for (const tg of pad.TARGETS) assert.ok(`pad.t.${tg.id}` in zh && `pad.t.${tg.id}` in en, tg.id);
  for (const k of ["pad.state.off", "pad.state.on", "pad.state.xinput", "pad.type.label", "pad.type.hid", "pad.type.xinput", "pad.type.hid.tip", "pad.type.xinput.tip", "pad.type.hid.note", "pad.type.xinput.note", "pad.line.saved.xinput", "pad.note.act_low", "pad.note.act_high", "pad.note.dz_low", "pad.note.dz_high",
    "pad.curve.n", "nav.pad", "pad.preset.title", "pad.opt.editCurve"]) assert.ok(k in zh && k in en, k);
  void readdirSync;
});

test("pt() answers in the current language and falls back to English", async () => {
  const { setLang } = await import("../js/i18n.js");
  const { pt } = await import("../js/locales/pad-i18n.js");
  globalThis.document ??= { documentElement: {} };
  setLang("zh-TW");
  assert.equal(pt("pad.mode.title"), zh["pad.mode.title"]);
  assert.equal(pt("pad.sel.many", { n: 3 }), zh["pad.sel.many"].replace("{n}", "3"));
  setLang("en");
  assert.equal(pt("pad.mode.title"), en["pad.mode.title"]);
  assert.equal(pt("pad.curve.note.start_high", { max: "1.00" }), en["pad.curve.note.start_high"].replace("{max}", "1.00"));
  assert.equal(pt("pad.nonexistent.key"), "pad.nonexistent.key");
  setLang("zh-TW");
});

test("the page's styles are scoped to the page", () => {
  const rules = PAD_CSS.split("\n").filter((l) => /\{/.test(l) && !l.startsWith("@") && !l.startsWith("/*"));
  assert.ok(rules.length > 60);
  for (const r of rules) assert.ok(r.startsWith('[data-page="pad"]'), `unscoped rule: ${r.slice(0, 60)}`);
  assert.ok(!/@import|url\(/.test(PAD_CSS));
});

test("the page module loads and offers mount()", async () => {
  globalThis.document ??= { documentElement: {} };
  const mod = await import("../js/pages/pad.js");
  assert.equal(typeof mod.mount, "function");
});
