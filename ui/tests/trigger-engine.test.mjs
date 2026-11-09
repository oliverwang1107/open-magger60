import test from "node:test";
import assert from "node:assert/strict";
import { stepKey, newKeyState, defaultTrigger, MODE } from "../js/core/trigger.js";
import { mmToRaw } from "../js/core/units.js";

const run = (trig, mmPath) => {
  const st = newKeyState();
  const ev = [];
  for (const mm of mmPath) {
    const e = stepKey(st, mmToRaw(mm), trig);
    if (e) ev.push(e);
  }
  return ev;
};

test("normal mode presses past the actuation point and releases at the same point", () => {
  const t = { ...defaultTrigger(), actuation: 1.5 };
  assert.deepEqual(run(t, [0, 1, 1.4, 1.6, 2, 1.6, 1.4, 0]), ["press", "release"]);
});

test("top dead zone delays the press", () => {
  const t = { ...defaultTrigger(), actuation: 0.2, topDead: 0.5 };
  assert.deepEqual(run(t, [0, 0.3, 0.45, 0.6]), ["press"]);
  assert.equal(stepKey(newKeyState(), mmToRaw(0.4), t), null);
});

test("rapid trigger releases after the release distance and presses again after the press distance", () => {
  const t = { ...defaultTrigger(), mode: MODE.RT, actuation: 1.0, rapidDown: 0.3, rapidUp: 0.2 };
  assert.deepEqual(run(t, [0, 1.2, 2.0, 1.75, 2.1, 2.2, 1.9, 0.05]), ["press", "release", "press", "release"]);
});

test("rapid trigger (mode 1) does not re-press above the actuation point, continuous mode does", () => {
  const base = { ...defaultTrigger(), actuation: 1.0, rapidDown: 0.2, rapidUp: 0.2 };
  const path = [0, 1.5, 1.2, 0.6, 0.9, 0.3, 0.6];
  assert.deepEqual(run({ ...base, mode: MODE.RT }, path), ["press", "release"]);
  assert.deepEqual(run({ ...base, mode: MODE.RT_CONTINUOUS }, path), ["press", "release", "press", "release", "press"]);
});

test("bottom dead zone keeps the key pressed near the bottom", () => {
  const t = { ...defaultTrigger(), mode: MODE.RT, actuation: 1, rapidUp: 0.1, bottomDead: 0.3 };
  assert.deepEqual(run(t, [0, 1.5, 3.3, 3.2, 3.15]), ["press"]);
  assert.deepEqual(run(t, [0, 1.5, 3.3, 3.2, 3.0]), ["press", "release"]);
});

test("returning to the top always releases", () => {
  const t = { ...defaultTrigger(), mode: MODE.RT, actuation: 1, rapidUp: 3 };
  assert.deepEqual(run(t, [0, 1.5, 0.05]), ["press", "release"]);
});
