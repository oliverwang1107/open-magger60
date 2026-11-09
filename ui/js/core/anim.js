// Animations for the 61 key lights: what a frame is, the built-in demos, turning pictures into frames (area average per light in linear
// light, same idea as tools/badapple_convert.py), the "look" (brightness / invert / one colour), the player clock and the save format.
// Pure functions and classes, no DOM; the browser-only readers (files, GIF, video) are in js/anim_import.js.
//
// A frame is a Uint8Array(183): r,g,b of light 0..60 (light order of km_led.h = core/led_map.js SLOT_OF_LED). An animation is
// { name, fps, frames: Uint8Array(183)[] }. Nothing here knows about USB: the page hands frames to core/stream_client.js.
import { LED_COUNT, SLOT_OF_LED } from "./led_map.js";

export const FRAME_BYTES = LED_COUNT * 3;
export const CANVAS_W = 15;               // the light area in key widths x key rows, a 3:1 strip
export const CANVAS_H = 5;
export const MAX_FRAMES = 7200;           // 4 minutes at 30 fps, about 1.3 MB (a full 3:39 video at 15 fps is 3300 frames)
export const FPS_MIN = 1;
export const FPS_MAX = 30;                // the lights refresh at most every 20 ms, 30 is what the stream recommends
export const SPEED_RANGE = Object.freeze([0.25, 3]);        // playback speed, times
export const FITS = Object.freeze(["crop", "stretch", "letterbox"]);

/** Centre of every light in key units ({x: 0..15, y: 0..5}), from the layout; light i is the one under sensor slot SLOT_OF_LED[i]. */
export function lightPositions(layout) {
  return SLOT_OF_LED.map((slot) => {
    const k = layout.bySlot.get(slot);
    return { x: k.x + k.w / 2, y: k.y + k.h / 2 };
  });
}

export const blankFrame = () => new Uint8Array(FRAME_BYTES);
const clamp8 = (v) => (v <= 0 ? 0 : v >= 255 ? 255 : Math.round(v));
const lerp = (a, b, f) => a + (b - a) * f;

export function hsv(h, s, v) {
  h = ((h % 1) + 1) % 1;
  const i = Math.floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  const [r, g, b] = [[v, t, p], [q, v, p], [p, v, t], [p, q, v], [t, p, v], [v, p, q]][i % 6];
  return [clamp8(r * 255), clamp8(g * 255), clamp8(b * 255)];
}

// ---- built-in demos (all original, generated; nothing from anybody's video) ----

export const DEMOS = Object.freeze([
  { id: "ripple", fps: 30, frames: 60 },
  { id: "badapple", fps: 15, frames: 120 },
  { id: "scanner", fps: 30, frames: 60 },
]);

/** Rainbow ripple: colour rings run outwards from the middle of the keyboard. Loops without a seam. */
function rippleFrame(pos, k, n) {
  const f = blankFrame();
  pos.forEach((p, i) => {
    const d = Math.hypot((p.x - 7.5) * 0.8, (p.y - 2.5) * 1.3);
    const hue = d / 7 - k / n;
    const v = 0.55 + 0.45 * Math.cos((d * 0.9 - (k / n) * 2 * Math.PI) * 1);
    f.set(hsv(hue, 1, Math.max(0.15, v)), i * 3);
  });
  return f;
}

/** A red light that sweeps left and right with a fading tail. */
function scannerFrame(pos, k, n) {
  const f = blankFrame();
  const x = 0.5 + (CANVAS_W - 1) * (0.5 - 0.5 * Math.cos((k / n) * 2 * Math.PI));
  pos.forEach((p, i) => {
    const v = Math.exp(-((p.x - x) ** 2) / 1.6);
    f.set([clamp8(255 * v), clamp8(40 * v * v), clamp8(10 * v * v * v)], i * 3);
  });
  return f;
}

/**
 * "Bad Apple style": a white silhouette that dances on black (2-bit grey, like the real clip would be on 61 lights), then the picture
 * is wiped away by bars. It is made of simple shapes: a head, a body, two swinging arms. Original artwork, generated here.
 */
function silhouetteFrame(pos, k, n) {
  const f = blankFrame();
  const t = (k / n) * 2 * Math.PI;
  const cx = 7.5 + 3.2 * Math.sin(t), sway = 0.35 * Math.sin(2 * t);
  const wipe = k / n > 0.8 ? ((k / n - 0.8) / 0.2) * (CANVAS_W + 2) : -1;         // bars close in during the last fifth
  pos.forEach((p, i) => {
    const dx = p.x - cx - sway * (p.y - 2.5) * 0.4, dy = p.y;
    let cover = 0;
    const blob = (x, y, rx, ry) => Math.max(0, 1 - Math.hypot((dx - x) / rx, (dy - y) / ry));
    cover = Math.max(cover, blob(0, 0.9, 0.9, 0.85) * 1.6);                              // head
    cover = Math.max(cover, blob(0, 3.0, 1.35, 1.9) * 1.6);                              // body
    const a = Math.sin(t * 2) * 0.9;
    cover = Math.max(cover, blob(-1.9, 2.0 + a, 1.1, 0.7) * 1.6, blob(1.9, 2.0 - a, 1.1, 0.7) * 1.6);   // arms
    let v = Math.min(1, cover);
    if (wipe >= 0 && Math.abs(((p.x * 1.0) % 2) - 1) < 1.2 && p.x < wipe) v = 0;
    const g = clamp8((Math.round(v * 3) / 3) * 255);                                    // 4 grey levels
    f.set([g, g, g], i * 3);
  });
  return f;
}

export function makeDemo(id, pos) {
  const d = DEMOS.find((x) => x.id === id);
  if (!d) throw new RangeError(`no demo called ${id}`);
  const make = { ripple: rippleFrame, badapple: silhouetteFrame, scanner: scannerFrame }[id];
  return { name: id, fps: d.fps, frames: Array.from({ length: d.frames }, (_, k) => make(pos, k, d.frames)), demo: true };
}

// ---- pictures -> frames ----

/**
 * Where the 15 x 5 light canvas looks into a w x h picture. Returns (u, v) => [sx, sy] in picture pixels, or null outside the picture
 * (black bars of "letterbox"). crop: fill the canvas, cut what sticks out; stretch: squash to 3:1; letterbox: whole picture, bars.
 */
export function fitMapper(w, h, fit = "crop") {
  if (fit === "stretch") return (u, v) => [(u / CANVAS_W) * w, (v / CANVAS_H) * h];
  const scale = fit === "letterbox" ? Math.min(CANVAS_W / w, CANVAS_H / h) : Math.max(CANVAS_W / w, CANVAS_H / h);   // key units per pixel
  const ox = (CANVAS_W - w * scale) / 2, oy = (CANVAS_H - h * scale) / 2;
  return (u, v) => {
    const sx = (u - ox) / scale, sy = (v - oy) / scale;
    return sx < 0 || sy < 0 || sx >= w || sy >= h ? null : [sx, sy];
  };
}

const TO_LIN = Float32Array.from({ length: 256 }, (_, i) => (i / 255) ** 2.2);
const toSrgb = (l) => clamp8(255 * Math.max(0, Math.min(1, l)) ** (1 / 2.2));

/**
 * RGBA pixels (Uint8ClampedArray, w x h) -> a frame: every light takes the area average of the 1 x 1 key square around it, averaged in
 * linear light (so a half-lit region is not too dark). Transparent pixels count as black.
 */
export function sampleRgba(rgba, w, h, pos, { fit = "crop" } = {}) {
  if (rgba.length < w * h * 4 || w < 1 || h < 1) throw new RangeError("the picture is smaller than its size says");
  const map = fitMapper(w, h, fit);
  const f = blankFrame();
  const N = 4;                                        // N x N sample points per light, plenty for a 15 x 5 target
  pos.forEach((p, i) => {
    let r = 0, g = 0, b = 0;
    for (let a = 0; a < N; a++) for (let c = 0; c < N; c++) {
      const m = map(p.x - 0.5 + (c + 0.5) / N, p.y - 0.5 + (a + 0.5) / N);
      if (!m) continue;
      const o = (Math.min(h - 1, Math.floor(m[1])) * w + Math.min(w - 1, Math.floor(m[0]))) * 4;
      const al = rgba[o + 3] / 255;
      r += TO_LIN[rgba[o]] * al; g += TO_LIN[rgba[o + 1]] * al; b += TO_LIN[rgba[o + 2]] * al;
    }
    f[i * 3] = toSrgb(r / (N * N)); f[i * 3 + 1] = toSrgb(g / (N * N)); f[i * 3 + 2] = toSrgb(b / (N * N));
  });
  return f;
}

/**
 * Frames with their own durations (animated GIF) -> frames at a constant rate. Returns { fps, frames } (frames are shared, not copied).
 * The rate is the one closest to the shortest delay, between FPS_MIN and FPS_MAX.
 */
export function resampleByDurations(frames, durationsMs, fps = null) {
  const durs = durationsMs.map((d) => (d > 0 ? d : 100));
  const shortest = Math.min(...durs);
  const rate = Math.max(FPS_MIN, Math.min(FPS_MAX, fps ?? Math.round(1000 / shortest)));
  const total = durs.reduce((a, b) => a + b, 0);
  const n = Math.min(MAX_FRAMES, Math.max(1, Math.round((total / 1000) * rate)));
  const out = [];
  let idx = 0, acc = durs[0];
  for (let k = 0; k < n; k++) {
    const t = (k * 1000) / rate;
    while (t >= acc && idx < frames.length - 1) { idx++; acc += durs[idx]; }
    out.push(frames[idx]);
  }
  return { fps: rate, frames: out };
}

// ---- the look: brightness / invert / one colour ----

/**
 * The frame as it is sent. brightness 0..1 (the keyboard still applies its own three limits on top), invert (white background pictures
 * are very dim after the keyboard's limits, so inverting is offered), tint "#rrggbb" or null: show the picture's brightness in that one colour.
 */
export function applyLook(frame, { brightness = 1, invert = false, tint = null, antiBleed = 0, contrast = 0, kernel = null } = {}) {
  const out = new Uint8Array(FRAME_BYTES);
  const tr = tint ? parseInt(tint.slice(1, 3), 16) : 255, tg = tint ? parseInt(tint.slice(3, 5), 16) : 255, tb = tint ? parseInt(tint.slice(5, 7), 16) : 255;
  const k = Math.max(0, Math.min(1, brightness));
  for (let i = 0; i < FRAME_BYTES; i += 3) {
    let r = frame[i], g = frame[i + 1], b = frame[i + 2];
    if (invert) { r = 255 - r; g = 255 - g; b = 255 - b; }
    if (tint) {
      const y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
      r = tr * y; g = tg * y; b = tb * y;
    }
    out[i] = clamp8(r * k); out[i + 1] = clamp8(g * k); out[i + 2] = clamp8(b * k);
  }
  return applyAntiBleed(applyContrast(out, contrast), kernel, antiBleed);   // after brightness, so the preview and the stream get the same pixels
}

// ---- light leak: the model, the simulation (preview) and the compensation (anti-bleed) ----

/** Reach of the leak in key widths: a light two keys away already receives almost nothing. */
export const LEAK_RADIUS = 1.0;
/** How much of a neighbour's brightness a light receives, scaled by the falloff (a model, not a measurement: tune it against the real keyboard). */
export const LEAK_GAIN = 0.18;
export const TRAIL_MAX_S = 0.8;            // afterglow time at 100 % on the slider (clip time, seconds)
const TRAIL_LOOKBACK = 24;                 // frames looked back at most; the rest is below 2 % anyway at 30 fps

/** For every light the lights around it and their weight exp(-(d / radius)^2), cut at 2 %: [[j, weight]...]. This is the leak model. */
export function leakKernel(pos, radius = LEAK_RADIUS) {
  return pos.map((p, i) => {
    const out = [];
    pos.forEach((q, j) => {
      if (j === i) return;
      const w = Math.exp(-(((p.x - q.x) ** 2 + (p.y - q.y) ** 2) / (radius * radius)));
      if (w >= 0.02) out.push([j, w]);
    });
    return out;
  });
}

const spreadOf = (frame, kernel, i, c) => { let a = 0; for (const [j, w] of kernel[i]) a += w * frame[j * 3 + c]; return a; };

/** What the keyboard really looks like: every light also shows LEAK_GAIN of its neighbours (blurry). Only for the preview, never sent. */
export function simulateLeak(frame, kernel, gain = LEAK_GAIN) {
  const out = new Uint8Array(FRAME_BYTES);
  for (let i = 0; i < kernel.length; i++) for (let c = 0; c < 3; c++) out[i * 3 + c] = clamp8(frame[i * 3 + c] + gain * spreadOf(frame, kernel, i, c));
  return out;
}

/**
 * Anti-bleed (lateral inhibition / a small deconvolution): each light gives up what its neighbours will leak into it, so that after the
 * leak the picture is closer to the one that was meant: x = clamp(in - k * LEAK_GAIN * sum(w * x_neighbours)), solved by two passes.
 * strength k 0..1; 0 returns the frame itself. Dark lights next to bright ones go darker, an isolated bright light stays as it is.
 */
export function applyAntiBleed(frame, kernel, strength, passes = 2) {
  if (!(strength > 0) || !kernel) return frame;
  const k = Math.min(1, strength) * LEAK_GAIN;
  let x = Float32Array.from(frame);
  for (let p = 0; p < passes; p++) {
    const next = new Float32Array(FRAME_BYTES);
    for (let i = 0; i < kernel.length; i++) for (let c = 0; c < 3; c++) {
      let a = 0;
      for (const [j, w] of kernel[i]) a += w * x[j * 3 + c];
      next[i * 3 + c] = Math.max(0, Math.min(255, frame[i * 3 + c] - k * a));
    }
    x = next;
  }
  return Uint8Array.from(x, (v) => clamp8(v));
}

/** "Blacks blacker": a gentle curve, amount 0..1 -> gamma 1..2 (0 = unchanged). Keeps 0 and 255 where they are. */
export function applyContrast(frame, amount) {
  if (!(amount > 0)) return frame;
  const g = 1 + Math.min(1, amount);
  const out = new Uint8Array(FRAME_BYTES);
  for (let i = 0; i < FRAME_BYTES; i++) out[i] = clamp8(255 * (frame[i] / 255) ** g);
  return out;
}

/**
 * Afterglow: a light that went dark fades out instead of cutting off. The result is a pure function of the position in the clip:
 * the brightest of the current frame and the earlier ones, each dimmed by exp(-age / tauSec). So scrubbing, looping and any speed give
 * the same picture for the same position, and nothing can exceed what the clip itself contained.
 */
export function trailFrame(frames, pos, fps, tauSec, loop, current) {
  if (!(tauSec > 0)) return current;
  const n = frames.length;
  const base = Math.min(n - 1, Math.floor(pos + 1e-9));
  const out = Uint8Array.from(current);
  for (let m = 1; m <= TRAIL_LOOKBACK; m++) {
    let idx = base - m;
    if (idx < 0) { if (!loop) break; idx = ((idx % n) + n) % n; }
    const w = Math.exp(-(m + (pos - base)) / (fps * tauSec));
    if (w < 0.02) break;
    const f = frames[idx];
    for (let q = 0; q < FRAME_BYTES; q++) { const v = f[q] * w; if (v > out[q]) out[q] = Math.round(v); }
  }
  return out;
}

export const isValidTint = (s) => typeof s === "string" && /^#[0-9a-fA-F]{6}$/.test(s);

// ---- the player clock ----

/**
 * Plays an animation in time: play / pause / stop / seek, speed, loop. tick(nowMs) advances the position by the real time that passed and
 * returns the frame to show now (blended between two neighbours when `blend` is on, which makes a 15 fps clip look smooth at 30 fps).
 * No timers inside: the page calls tick() from requestAnimationFrame, tests call it with made-up times.
 */
export class AnimPlayer {
  constructor({ onState = () => {} } = {}) {
    this.anim = null;
    this.state = "stopped";           // "stopped" | "playing" | "paused"
    this.pos = 0;                     // position in frames (fractional)
    this.speed = 1;
    this.loop = true;
    this.blend = true;
    this.trailS = 0;                  // afterglow time in seconds of the clip, 0 = off
    this.onState = onState;
    this.last = null;
  }

  load(anim) {
    this.anim = anim;
    this.pos = 0;
    this.state = "stopped";
    this.last = null;
    this.onState(this);
  }

  get count() { return this.anim?.frames.length ?? 0; }
  get duration() { return this.anim ? this.count / this.anim.fps : 0; }       // seconds of one pass at speed 1
  get time() { return this.anim ? this.pos / this.anim.fps : 0; }

  setSpeed(v) { this.speed = Math.max(SPEED_RANGE[0], Math.min(SPEED_RANGE[1], Number(v) || 1)); }
  setLoop(on) { this.loop = !!on; }

  play(nowMs) {
    if (!this.count) return;
    if (this.state === "stopped") this.pos = 0;
    this.state = "playing";
    this.last = nowMs;
    this.onState(this);
  }
  pause() { if (this.state === "playing") { this.state = "paused"; this.onState(this); } }
  stop() { this.state = "stopped"; this.pos = 0; this.onState(this); }
  seek(frac) { if (this.count) this.pos = Math.max(0, Math.min(this.count - 1e-6, frac * this.count)); }

  /** Advance to `nowMs` and return the frame (Uint8Array(183)) to show, or null when there is no animation. */
  tick(nowMs) {
    if (!this.count) return null;
    if (this.state === "playing") {
      const dt = this.last === null ? 0 : Math.max(0, Math.min(250, nowMs - this.last));   // a long stall (hidden tab) does not skip the clip
      this.last = nowMs;
      this.pos += (dt / 1000) * this.anim.fps * this.speed;
      if (this.pos >= this.count) {
        if (this.loop) this.pos %= this.count;
        else { this.pos = this.count - 1; this.state = "stopped"; this.onState(this); }
      }
    }
    return this.frameAt(this.pos);
  }

  /** The frame at `pos` with the afterglow (when on). Stateless: the same position always gives the same frame. */
  frameAt(pos) {
    const cur = this.#blended(pos);
    return this.trailS > 0 ? trailFrame(this.anim.frames, pos, this.anim.fps, this.trailS, this.loop, cur) : cur;
  }

  #blended(pos) {
    const { frames } = this.anim;
    const i = Math.min(frames.length - 1, Math.floor(pos + 1e-9));   // the epsilon: 0.3 s x 10 fps must be frame 3, not 2.9999
    if (!this.blend || frames.length < 2) return frames[i];
    const j = i + 1 < frames.length ? i + 1 : this.loop ? 0 : i;
    const f = Math.max(0, pos - i);
    if (j === i || f < 0.02) return frames[i];
    const out = new Uint8Array(FRAME_BYTES);
    for (let q = 0; q < FRAME_BYTES; q++) out[q] = Math.round(lerp(frames[i][q], frames[j][q], f));
    return out;
  }
}

// ---- file format (so a made animation can be kept and loaded again) ----

const b64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 0x4000) s += String.fromCharCode(...u8.subarray(i, i + 0x4000)); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

export function serializeAnim(anim) {
  return JSON.stringify({ format: "magger60-anim", version: 1, name: anim.name, fps: anim.fps, lights: LED_COUNT, frames: anim.frames.map(b64) });
}

/** Parse a saved animation; throws RangeError with a reason for anything that is not exactly what serializeAnim writes. */
export function parseAnim(text) {
  let o;
  try { o = JSON.parse(text); } catch { throw new RangeError("not a JSON file"); }
  if (!o || o.format !== "magger60-anim" || o.version !== 1) throw new RangeError("not an animation file");
  if (o.lights !== LED_COUNT) throw new RangeError("wrong number of lights");
  if (!Number.isInteger(o.fps) || o.fps < FPS_MIN || o.fps > FPS_MAX) throw new RangeError("fps out of range");
  if (!Array.isArray(o.frames) || !o.frames.length || o.frames.length > MAX_FRAMES) throw new RangeError("frame count out of range");
  const frames = o.frames.map((s) => {
    if (typeof s !== "string") throw new RangeError("bad frame");
    let u;
    try { u = unb64(s); } catch { throw new RangeError("bad frame"); }
    if (u.length !== FRAME_BYTES) throw new RangeError("bad frame size");
    return u;
  });
  return { name: String(o.name ?? "").slice(0, 60) || "animation", fps: o.fps, frames };
}

// ---- local clips: ./local/clips/index.json (never published, see tools/make_local_clip.py) ----

/** The entries of a local clip list that are safe to fetch: a plain file name ending in .json (and an optional plain .m4a / .mp3 / .ogg / .opus name). Anything else is dropped. */
export function parseClipIndex(text) {
  let o;
  try { o = JSON.parse(text); } catch { return []; }
  if (!Array.isArray(o)) return [];
  const plain = (v, ext) => typeof v === "string" && new RegExp(`^[\\w][\\w. -]{0,80}\\.(${ext})$`, "i").test(v) && !v.includes("..");
  return o.filter((e) => e && typeof e.name === "string" && e.name.trim() && plain(e.anim, "json") && (e.audio === undefined || plain(e.audio, "m4a|mp3|ogg|opus")))
    .map((e) => ({ name: e.name.trim().slice(0, 60), anim: e.anim, audio: e.audio ?? null }));
}
