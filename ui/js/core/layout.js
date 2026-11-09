// Keyboard layout: our own ANSI 60% key list (ansi60.js) joined with the board wiring (wiring.js), plus
// selection geometry helpers.
import { ANSI60_ROWS } from "./ansi60.js";
import { SLOT_BY_KEY_ID } from "./wiring.js";

/** Lay rows out left to right; every key becomes {id, slot, row, x, y, w, h, label, alt, usage, name, isFn}. */
export function buildLayout(rows = ANSI60_ROWS, slotById = SLOT_BY_KEY_ID) {
  const keys = [];
  rows.forEach((row, r) => {
    let x = 0;
    for (const k of row) {
      const slot = slotById[k.id];
      if (!Number.isInteger(slot)) throw new Error(`key ${k.id} has no sensor slot`);
      keys.push({ id: k.id, slot, row: r, x, y: r, w: k.w, h: 1, label: { main: k.label, alt: k.alt }, usage: k.usage, name: k.name, isFn: k.id === "Fn" });
      x += k.w;
    }
  });
  const bySlot = new Map(keys.map((k) => [k.slot, k]));
  const byId = new Map(keys.map((k) => [k.id, k]));
  if (bySlot.size !== keys.length || byId.size !== keys.length) throw new Error("duplicate key id or slot in layout");
  const byUsage = new Map();
  for (const k of keys) if (k.usage !== null && !byUsage.has(k.usage)) byUsage.set(k.usage, k);
  const width = Math.max(...keys.map((k) => k.x + k.w));
  const height = rows.length;
  return { keys, bySlot, byId, byUsage, width, height, rows: height };
}

export const loadLayout = () => buildLayout();

const cx = (k) => k.x + k.w / 2;
const cy = (k) => k.y + k.h / 2;

export const rowSlots = (layout, row) => layout.keys.filter((k) => Math.floor(cy(k)) === row).map((k) => k.slot);
export const columnSlots = (layout, col) => layout.keys.filter((k) => Math.floor(cx(k)) === col).map((k) => k.slot);
export const allSlots = (layout) => layout.keys.map((k) => k.slot);

/** Keys whose rectangle overlaps r = {x0,y0,x1,y1} (key units). */
export function slotsInRect(layout, r) {
  const x0 = Math.min(r.x0, r.x1), x1 = Math.max(r.x0, r.x1);
  const y0 = Math.min(r.y0, r.y1), y1 = Math.max(r.y0, r.y1);
  return layout.keys.filter((k) => k.x < x1 && k.x + k.w > x0 && k.y < y1 && k.y + k.h > y0).map((k) => k.slot);
}

/** Nearest key from `slot` in direction dir ("left"|"right"|"up"|"down"), for arrow-key navigation. */
export function neighbour(layout, slot, dir) {
  const from = layout.bySlot.get(slot);
  if (!from) return layout.keys[0]?.slot;
  const v = { left: [-1, 0], right: [1, 0], up: [0, -1], down: [0, 1] }[dir];
  let best = null, bestScore = Infinity;
  for (const k of layout.keys) {
    if (k === from) continue;
    const dx = cx(k) - cx(from), dy = cy(k) - cy(from);
    const along = dx * v[0] + dy * v[1];
    if (along <= 0.2) continue;
    const across = Math.abs(dx * v[1]) + Math.abs(dy * v[0]);
    const score = along + across * 2.5;
    if (score < bestScore) { bestScore = score; best = k; }
  }
  return best ? best.slot : slot;
}
