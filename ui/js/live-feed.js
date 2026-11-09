// Turns the device's depth stream into what the UI needs: latest depths, pressed flags (via the trigger
// model and the active profile), a short history for the graph, and press/release events for markers.
import { LiveBuffer } from "./core/live-buffer.js";
import { newKeyState, stepKey } from "./core/trigger.js";
import { SLOT_COUNT } from "./core/units.js";

const MIN_STEP_MS = 6;       // history decimation, keeps ~8 s even if a device streams at 1 kHz
const EVENT_KEEP_MS = 8000;

export class LiveFeed {
  depth = new Uint16Array(SLOT_COUNT);
  pressed = new Uint8Array(SLOT_COUNT);
  peak = new Uint16Array(SLOT_COUNT);
  buffer = new LiveBuffer(1400);
  fps = 0;
  lastFrameAt = 0;
  paused = false;
  /** Slots the keyboard itself says are pressed (scope tag 0x0301), when it publishes them; replaces the reference model below. */
  #mask = null;
  #maskAt = 0;
  #states = Array.from({ length: SLOT_COUNT }, newKeyState);
  #events = Array.from({ length: SLOT_COUNT }, () => []);
  #unsub = null;
  #lastPush = 0;
  #frames = 0;
  #fpsAt = 0;

  constructor(app) {
    this.app = app;
  }

  attach(device) {
    this.#unsub?.();
    this.reset();
    this.#mask = null;
    this.#unsub = device.subscribeLive(this.#onFrame);
  }

  reset() {
    this.depth.fill(0);
    this.pressed.fill(0);
    this.peak.fill(0);
    this.buffer.clear();
    this.fps = 0;
    this.lastFrameAt = 0;
    this.#states.forEach((s, i) => (this.#states[i] = newKeyState()));
    this.#events.forEach((e) => (e.length = 0));
  }

  get hasData() {
    return performance.now() - this.lastFrameAt < 1500;
  }

  /** Press/release events of `slot` in the last `windowMs`. */
  eventsFor(slot, nowMs, windowMs) {
    return this.#events[slot].filter((e) => nowMs - e.t <= windowMs);
  }

  setPressedMask(slots) {
    this.#mask = new Set(slots);
    this.#maskAt = performance.now();
  }

  get usesKeyboardMask() {
    return this.#mask !== null && performance.now() - this.#maskAt < 500;
  }

  resetPeaks() {
    this.peak.fill(0);
  }

  #onFrame = (depths, t) => {
    if (this.paused) return;
    this.lastFrameAt = performance.now();
    this.depth.set(depths);
    if (t - this.#lastPush >= MIN_STEP_MS) {
      this.buffer.push(depths, t);
      this.#lastPush = t;
    }
    const keys = this.app.profile.keys;
    for (const k of this.app.layout.keys) {
      const s = k.slot;
      const d = depths[s];
      if (d > this.peak[s]) this.peak[s] = d;
      const ev = stepKey(this.#states[s], d, keys[k.id].trigger);
      this.pressed[s] = this.usesKeyboardMask ? (this.#mask.has(s) ? 1 : 0) : this.#states[s].pressed ? 1 : 0;
      if (ev) {
        const list = this.#events[s];
        list.push({ t, type: ev, depth: d });
        while (list.length && t - list[0].t > EVENT_KEEP_MS) list.shift();
      }
    }
    this.#frames++;
    if (t - this.#fpsAt >= 500) {
      this.fps = Math.round((this.#frames * 1000) / (t - this.#fpsAt));
      this.#frames = 0;
      this.#fpsAt = t;
    }
  };
}
