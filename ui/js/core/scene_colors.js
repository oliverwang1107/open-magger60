// Per-key colour editing for the light scene's "custom" effect: gradient maths, quantising to the scene's 16-colour palette, a colour
// editor with undo / redo / copy / paste, and row / column helpers on the real layout. Pure functions and one plain class, no DOM, no device.
//
// The editor works on full 8-bit colours per light (`colors[led]`, km_led order, 61 entries). The keyboard stores a PALETTE of 16 colours
// plus one palette index per light, so before a scene is written the colours are rounded to at most 16 shades (quantize()). The page
// previews the rounded colours, so what is drawn is what the keyboard will show.
import { LED_COUNT, ledOfSlot, ledsOfSlots, slotOfLed } from "./led_map.js";
import { LS } from "./lightscene_codec.js";

export const PALETTE_SIZE = LS.palette;
const byte = (v) => Math.max(0, Math.min(255, Math.round(v)));
const sameColor = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];

// ---- colour maths ----

export const lerpColor = (a, b, t) => [0, 1, 2].map((i) => byte(a[i] + (b[i] - a[i]) * t));

/** h in degrees (any value), s and v 0..1 -> [r, g, b] 0..255. */
export function hsvToRgb(h, s, v) {
  const hh = ((h % 360) + 360) % 360;
  const f = (n) => { const k = (n + hh / 60) % 6; return v - v * s * Math.max(0, Math.min(k, 4 - k, 1)); };
  return [byte(f(5) * 255), byte(f(3) * 255), byte(f(1) * 255)];
}

/** [r, g, b] -> [h degrees 0..360, s 0..1, v 0..1]. */
export function rgbToHsv([r, g, b]) {
  const R = r / 255, G = g / 255, B = b / 255;
  const max = Math.max(R, G, B), min = Math.min(R, G, B), d = max - min;
  let h = 0;
  if (d > 0) {
    if (max === R) h = ((G - B) / d) % 6;
    else if (max === G) h = (B - R) / d + 2;
    else h = (R - G) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  return [h, max === 0 ? 0 : d / max, max];
}

/** Colour of a gradient at t (0..1). `stops` = [{pos 0..1, color:[r,g,b]}], at least one; positions are sorted here, t outside 0..1 is clamped. */
export function gradientAt(stops, t) {
  if (!Array.isArray(stops) || stops.length === 0) throw new RangeError("a gradient needs at least one colour stop");
  const s = [...stops].sort((a, b) => a.pos - b.pos);
  const x = Math.max(0, Math.min(1, Number.isFinite(t) ? t : 0));
  if (x <= s[0].pos) return [...s[0].color];
  if (x >= s[s.length - 1].pos) return [...s[s.length - 1].color];
  for (let i = 1; i < s.length; i++) {
    if (x <= s[i].pos) {
      const span = s[i].pos - s[i - 1].pos;
      return span <= 0 ? [...s[i].color] : lerpColor(s[i - 1].color, s[i].color, (x - s[i - 1].pos) / span);
    }
  }
  return [...s[s.length - 1].color];
}

/** Position 0..1 of every point along a direction (angle in degrees: 0 = left to right, 90 = top to bottom, screen coordinates). */
export function projectT(points, angleDeg) {
  const a = (angleDeg * Math.PI) / 180, dx = Math.cos(a), dy = Math.sin(a);
  const p = points.map((q) => q.x * dx + q.y * dy);
  const lo = Math.min(...p), hi = Math.max(...p);
  return p.map((v) => (hi - lo < 1e-9 ? 0 : (v - lo) / (hi - lo)));
}

/** Linear gradient over `points` ([{x, y}] key centres): the first stop sits at the start of the points' extent along `angleDeg`, the last at its end. */
export function linearGradient(points, stops, angleDeg = 0) {
  if (!points.length) return [];
  return projectT(points, angleDeg).map((t) => gradientAt(stops, t));
}

/** Rainbow over the points: hue runs from `hueFrom` through `hueSpan` degrees along the direction. */
export function rainbow(points, { angle = 0, hueFrom = 0, hueSpan = 300, sat = 1, val = 1 } = {}) {
  if (!points.length) return [];
  return projectT(points, angle).map((t) => hsvToRgb(hueFrom + hueSpan * t, sat, val));
}

// ---- 16-colour palette ----

/**
 * Round `colors` to at most `max` shades. Up to `max` distinct colours are kept exactly; more are merged with median cut (weighted by how
 * many lights use each colour). Deterministic. Returns { palette: [[r,g,b] x max] (unused entries black), index: [palette index per colour] }.
 * The most used colour gets index 0.
 */
export function quantize(colors, max = PALETTE_SIZE) {
  const distinct = new Map();
  for (const c of colors) {
    const k = c.join(",");
    const e = distinct.get(k);
    if (e) e.n++; else distinct.set(k, { c: [...c], n: 1 });
  }
  const boxes = [[...distinct.values()]];
  while (boxes.length < max) {
    // split the box with the widest channel range (a box of one colour can not be split)
    let bi = -1, bestRange = 0, bestCh = 0;
    boxes.forEach((box, i) => {
      if (box.length < 2) return;
      for (let ch = 0; ch < 3; ch++) {
        const vs = box.map((e) => e.c[ch]);
        const r = Math.max(...vs) - Math.min(...vs);
        if (r > bestRange) { bestRange = r; bi = i; bestCh = ch; }
      }
    });
    if (bi < 0) break;
    const box = boxes[bi].slice().sort((a, b) => a.c[bestCh] - b.c[bestCh] || a.c[0] - b.c[0] || a.c[1] - b.c[1] || a.c[2] - b.c[2]);
    const total = box.reduce((s, e) => s + e.n, 0);
    let acc = 0, cut = 1;
    for (let i = 0; i < box.length - 1; i++) { acc += box[i].n; cut = i + 1; if (acc * 2 >= total) break; }
    boxes.splice(bi, 1, box.slice(0, cut), box.slice(cut));
  }
  const entries = boxes.map((box) => {
    const n = box.reduce((s, e) => s + e.n, 0);
    return { c: [0, 1, 2].map((ch) => byte(box.reduce((s, e) => s + e.c[ch] * e.n, 0) / n)), n };
  }).sort((a, b) => b.n - a.n || a.c[0] - b.c[0] || a.c[1] - b.c[1] || a.c[2] - b.c[2]);
  const palette = Array.from({ length: max }, (_, i) => (entries[i] ? [...entries[i].c] : [0, 0, 0]));
  const nearest = (c) => {
    let best = 0, bd = Infinity;
    for (let i = 0; i < entries.length; i++) {
      const d = (c[0] - palette[i][0]) ** 2 + (c[1] - palette[i][1]) ** 2 + (c[2] - palette[i][2]) ** 2;
      if (d < bd) { bd = d; best = i; }
    }
    return best;
  };
  return { palette, index: colors.map(nearest) };
}

/** What the keyboard will show for these colours: each rounded to the palette. */
export function roundedColors(colors) {
  const q = quantize(colors);
  return q.index.map((i) => [...q.palette[i]]);
}

/** Number of distinct shades in `colors`. */
export const shadeCount = (colors) => new Set(colors.map((c) => c.join(","))).size;

/** The scene model with its key colours replaced by `colors` (rounded to 16 shades). Everything else of the scene is kept. */
export function withKeyColors(scene, colors) {
  if (colors.length !== LED_COUNT) throw new RangeError(`need ${LED_COUNT} colours`);
  const q = quantize(colors);
  return { ...scene, palette: q.palette, keys: q.index };
}

/** Per-light full colours of a scene: palette[keys[led]]. */
export const sceneKeyColors = (scene) => scene.keys.map((i) => [...scene.palette[i]]);

// ---- layout helpers (light indices, km_led order; the layout is the page's) ----

const keyOfLed = (layout, led) => layout.bySlot.get(slotOfLed(led));
export const ledCenter = (layout, led) => { const k = keyOfLed(layout, led); return k ? { x: k.x + k.w / 2, y: k.y + k.h / 2 } : { x: 0, y: 0 }; };
export const ledPoints = (layout, leds) => leds.map((l) => ledCenter(layout, l));
/** Lights in reading order (top row first, left to right). */
export const readingOrder = (layout, leds) => [...leds].sort((a, b) => { const p = ledCenter(layout, a), q = ledCenter(layout, b); return p.y - q.y || p.x - q.x || a - b; });
export const rowLeds = (layout, row) => ledsOfSlots(layout.keys.filter((k) => Math.floor(k.y + k.h / 2) === row).map((k) => k.slot));
export const columnLeds = (layout, col) => ledsOfSlots(layout.keys.filter((k) => Math.floor(k.x + k.w / 2) === col).map((k) => k.slot));
export const allLeds = () => Array.from({ length: LED_COUNT }, (_, i) => i);

// ---- the editor ----

/**
 * The colours of the 61 lights plus history. Every change goes through set(leds, colorOrColors, { key }): one undo step per call; calls with
 * the same `key` within `mergeMs` of each other (a colour picker being dragged) share one step. Nothing here talks to a keyboard.
 */
export class ColorEditor {
  #hist;
  #pos = 0;
  #clip = null;
  #lastKey = null;
  #lastAt = 0;
  #now;
  #mergeMs;
  #max;

  constructor(colors = null, { now = () => Date.now(), mergeMs = 600, maxSteps = 100 } = {}) {
    const start = colors ? colors.map((c) => [...c]) : Array.from({ length: LED_COUNT }, () => [0, 150, 255]);
    if (start.length !== LED_COUNT) throw new RangeError(`need ${LED_COUNT} colours`);
    this.#hist = [start];
    this.#now = now;
    this.#mergeMs = mergeMs;
    this.#max = maxSteps;
  }

  get colors() { return this.#hist[this.#pos]; }
  get canUndo() { return this.#pos > 0; }
  get canRedo() { return this.#pos < this.#hist.length - 1; }
  get hasClipboard() { return !!this.#clip; }
  get clipboard() { return this.#clip ? this.#clip.map((c) => [...c]) : null; }

  /** Replace all colours (loading a scene, a preset): one undo step, never merged. */
  replaceAll(colors) {
    if (colors.length !== LED_COUNT) throw new RangeError(`need ${LED_COUNT} colours`);
    return this.#push(colors.map((c) => [...c]), null);
  }

  /** Paint `leds` with one colour, or with `color[i]` for leds[i] when `color` is a list of colours. Returns true when something changed. */
  set(leds, color, { key = null } = {}) {
    const next = this.colors.map((c) => [...c]);
    const many = Array.isArray(color[0]);
    leds.forEach((l, i) => {
      if (!Number.isInteger(l) || l < 0 || l >= LED_COUNT) return;
      const c = many ? color[i] : color;
      if (c) next[l] = [byte(c[0]), byte(c[1]), byte(c[2])];
    });
    return this.#push(next, key);
  }

  #push(next, key) {
    const cur = this.colors;
    if (next.every((c, i) => sameColor(c, cur[i]))) return false;
    const t = this.#now();
    const merge = key !== null && key === this.#lastKey && t - this.#lastAt <= this.#mergeMs && this.#pos > 0;
    this.#lastKey = key;
    this.#lastAt = t;
    if (merge) {
      this.#hist[this.#pos] = next;
      return true;
    }
    this.#hist.length = this.#pos + 1;
    this.#hist.push(next);
    if (this.#hist.length > this.#max) this.#hist.shift();
    this.#pos = this.#hist.length - 1;
    return true;
  }

  undo() { if (!this.canUndo) return false; this.#pos--; this.#lastKey = null; return true; }
  redo() { if (!this.canRedo) return false; this.#pos++; this.#lastKey = null; return true; }
  /** Stop merging: the next set() starts a new undo step. */
  seal() { this.#lastKey = null; }

  /** Remember the colours of `leds` (in the order given; use copySelection for reading order). */
  copy(leds) {
    if (!leds.length) return false;
    this.#clip = leds.map((l) => [...this.colors[l]]);
    return true;
  }

  /** Paste onto `leds` (in the order given): one copied colour fills them all; otherwise colour i goes to light i and the pattern repeats. */
  paste(leds) {
    if (!this.#clip || !leds.length) return false;
    return this.set(leds, leds.map((_, i) => this.#clip[i % this.#clip.length]));
  }
}

/** Copy / paste in reading order, so the selected lights can be passed in any order. */
export const copySelection = (ed, layout, leds) => ed.copy(readingOrder(layout, leds));
export const pasteSelection = (ed, layout, leds) => ed.paste(readingOrder(layout, leds));

export { ledsOfSlots, ledOfSlot };
