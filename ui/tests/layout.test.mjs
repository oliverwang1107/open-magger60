import test from "node:test";
import assert from "node:assert/strict";
import { loadLayout, buildLayout, slotsInRect, rowSlots, columnSlots, neighbour } from "../js/core/layout.js";
import { ANSI60_ROWS } from "../js/core/ansi60.js";
import { SLOT_BY_KEY_ID } from "../js/core/wiring.js";

test("ANSI 60% layout has 61 keys, unique ids and unique slots below 76", () => {
  const l = loadLayout();
  assert.equal(l.keys.length, 61);
  assert.equal(new Set(l.keys.map((k) => k.id)).size, 61);
  assert.equal(new Set(l.keys.map((k) => k.slot)).size, 61);
  assert.ok(l.keys.every((k) => k.slot >= 0 && k.slot < 76));
  assert.equal(l.width, 15);
  assert.equal(l.rows, 5);
});

test("every row is exactly 15 units wide except the standard ANSI total", () => {
  for (const row of ANSI60_ROWS) assert.equal(row.reduce((a, k) => a + k.w, 0), 15);
  assert.deepEqual(ANSI60_ROWS.map((r) => r.length), [14, 14, 13, 12, 8]);
});

test("key lookups", () => {
  const l = loadLayout();
  assert.equal(l.byId.get("Escape").name, "Esc");
  assert.equal(l.byId.get("Space").w, 6.25);
  assert.equal(l.byId.get("KeyW").usage, 26);
  assert.equal(l.byUsage.get(4).id, "KeyA");
  assert.equal(l.byId.get("Fn").isFn, true);
  assert.equal(l.byId.get("Fn").usage, null);
  assert.equal(l.bySlot.get(l.byId.get("KeyW").slot).id, "KeyW");
});

test("a key without a sensor slot or a duplicate slot is rejected", () => {
  assert.throws(() => buildLayout(ANSI60_ROWS, { ...SLOT_BY_KEY_ID, KeyA: undefined }), /no sensor slot/);
  assert.throws(() => buildLayout(ANSI60_ROWS, { ...SLOT_BY_KEY_ID, KeyA: SLOT_BY_KEY_ID.KeyB }), /duplicate/);
});

test("row, column and rectangle selection", () => {
  const l = loadLayout();
  assert.equal(rowSlots(l, 0).length, 14);
  assert.equal(rowSlots(l, 4).length, 8);
  assert.ok(columnSlots(l, 0).length >= 4);
  const r = slotsInRect(l, { x0: 0.1, y0: 0.1, x1: 1.9, y1: 0.9 });
  assert.deepEqual(r.sort((a, b) => a - b), [l.byId.get("Escape").slot, l.byId.get("Digit1").slot].sort((a, b) => a - b));
  assert.equal(slotsInRect(l, { x0: 20, y0: 20, x1: 21, y1: 21 }).length, 0);
});

test("arrow navigation moves to the geometric neighbour", () => {
  const l = loadLayout();
  const esc = l.byId.get("Escape").slot;
  assert.equal(l.bySlot.get(neighbour(l, esc, "right")).id, "Digit1");
  assert.equal(l.bySlot.get(neighbour(l, esc, "down")).id, "Tab");
  assert.equal(neighbour(l, esc, "up"), esc);
});
