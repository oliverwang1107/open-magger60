// Fixed-size history of depth samples per slot, for the Live graph. Samples are pushed at the rate the
// device delivers them; each push stamps the time so the graph can plot against real seconds.
import { SLOT_COUNT } from "./units.js";

export class LiveBuffer {
  constructor(capacity = 1024, slots = SLOT_COUNT) {
    this.capacity = capacity;
    this.slots = slots;
    this.depth = Array.from({ length: slots }, () => new Uint16Array(capacity));
    this.time = new Float64Array(capacity);
    this.head = 0; // next write position
    this.count = 0;
  }

  push(depths, tMs) {
    const h = this.head;
    for (let s = 0; s < this.slots; s++) this.depth[s][h] = depths[s] ?? 0;
    this.time[h] = tMs;
    this.head = (h + 1) % this.capacity;
    if (this.count < this.capacity) this.count++;
  }

  /** Samples of `slot` newer than `windowMs` before `nowMs`, oldest first, as {t[], v[]}. */
  window(slot, nowMs, windowMs) {
    const t = [], v = [];
    const start = (this.head - this.count + this.capacity) % this.capacity;
    for (let i = 0; i < this.count; i++) {
      const idx = (start + i) % this.capacity;
      if (nowMs - this.time[idx] <= windowMs) {
        t.push(this.time[idx]);
        v.push(this.depth[slot][idx]);
      }
    }
    return { t, v };
  }

  clear() {
    this.head = 0;
    this.count = 0;
  }
}
