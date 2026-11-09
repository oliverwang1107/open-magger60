import test from "node:test";
import assert from "node:assert/strict";
import { mmToRaw, rawToMm, clampMm, MAX_TRAVEL_MM, MAX_TRAVEL_RAW } from "../js/core/units.js";

test("mm <-> raw round trip at 0.01 mm resolution", () => {
  assert.equal(mmToRaw(2), 200);
  assert.equal(mmToRaw(0.5), 50);
  assert.equal(rawToMm(340), 3.4);
  for (let raw = 0; raw <= MAX_TRAVEL_RAW; raw++) assert.equal(mmToRaw(rawToMm(raw)), raw);
});

test("clampMm limits to travel and rounds to 0.01", () => {
  assert.equal(clampMm(9), MAX_TRAVEL_MM);
  assert.equal(clampMm(-1), 0);
  assert.equal(clampMm(1.2349), 1.23);
});
