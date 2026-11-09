// The keyboard's own lights (LED_GET / LED_SET of the configuration protocol, CONFIG_PROTOCOL.md): effect list, which
// controls an effect uses, the six-byte wire format, and the person-friendly scales (brightness in percent, speed 1..100).
// Pure functions, no I/O. The old browser-only preview model is core/lighting.js.
import { LED_EFFECT_MAX } from "../device/config_protocol.js";
import { hexToRgb, rgbToHex } from "./lighting.js";

/**
 * The firmware's effects, in wire order (km_led_fx_t). `speed` / `color`: does the effect use that field (km_led.c).
 * Brightness scales every effect except "off".
 */
export const LED_EFFECTS = Object.freeze([
  { id: "off", speed: false, color: false },
  { id: "static", speed: false, color: true },
  { id: "breath", speed: true, color: true },
  { id: "rainbow", speed: true, color: false },
  { id: "reactive", speed: true, color: true },
  { id: "depth", speed: false, color: false },
  { id: "demo", speed: true, color: true },
  { id: "custom", speed: false, color: false },     // 7: one colour per key, from the light scene (the person picks it)
  { id: "external", speed: false, color: false },   // 8: the stream (host tools); not chosen on the lights page
  { id: "anim", speed: false, color: false },       // 9: the animation player; not chosen on the lights page
]);
/** Effects the lights page offers as a choice (8 and 9 are set by the stream / animation tools; the page only shows them when the keyboard is in one). */
export const LED_EFFECTS_PICKABLE = Object.freeze(LED_EFFECTS.filter((e) => e.id !== "external" && e.id !== "anim"));
export const LED_EFFECT_CUSTOM = 7;
if (LED_EFFECTS.length !== LED_EFFECT_MAX + 1) throw new Error("LED_EFFECTS out of step with the protocol");

export const ledEffectById = (id) => LED_EFFECTS.find((e) => e.id === id) ?? LED_EFFECTS[0];
export const ledEffectIndex = (id) => Math.max(0, LED_EFFECTS.findIndex((e) => e.id === id));

/**
 * Can the lights page work with this device right now? "ok" | "offline" (nothing connected, or the connection failed) |
 * "stock" (a keyboard with other firmware: only its version was read) | "old" (our firmware, but GET_INFO has no feature bit 6).
 */
export function ledAvailability(device) {
  if (!device || device.status !== "connected") return "offline";
  if (device.kind === "webhid") return "stock";
  return device.features?.lights ? "ok" : "old";
}

/** What the firmware starts with (km_led_cfg_default); also the starting point of a simulated keyboard. */
export const LED_DEFAULT = Object.freeze({ effect: 6, brightness: 255, speed: 128, r: 0, g: 150, b: 255 });

const byte = (v) => (Number.isFinite(Number(v)) ? Math.max(0, Math.min(255, Math.round(Number(v)))) : 0);
const isByte = (v) => Number.isInteger(v) && v >= 0 && v <= 255;

/** True when `s` is something LED_SET accepts: effect 0..9, speed 1..255, every field a byte. */
export function isValidLed(s) {
  return !!s && Number.isInteger(s.effect) && s.effect >= 0 && s.effect <= LED_EFFECT_MAX
    && isByte(s.brightness) && isByte(s.speed) && s.speed >= 1 && isByte(s.r) && isByte(s.g) && isByte(s.b);
}

/** {effect, brightness, speed, r, g, b} -> the six argument bytes of LED_SET. Throws RangeError for anything the firmware would refuse. */
export function encodeLed(s) {
  if (!isValidLed(s)) throw new RangeError("light settings out of range (effect 0..9, speed 1..255, the rest 0..255)");
  return Uint8Array.of(s.effect, s.brightness, s.speed, s.r, s.g, s.b);
}

/** The six payload bytes of LED_GET / LED_SET -> {effect, brightness, speed, r, g, b}. */
export function decodeLed(p) {
  if (!p || p.length !== 6) throw new RangeError(`light reply must be 6 bytes, not ${p?.length ?? 0}`);
  const s = { effect: p[0], brightness: p[1], speed: p[2], r: p[3], g: p[4], b: p[5] };
  if (!isValidLed(s)) throw new RangeError("the keyboard reported light settings outside the protocol's ranges");
  return s;
}

/** Make a state the firmware accepts out of whatever a UI produced (clamps, rounds, effect names are not accepted here). */
export function clampLed(s) {
  return {
    effect: Math.max(0, Math.min(LED_EFFECT_MAX, Math.round(Number(s.effect) || 0))),
    brightness: byte(s.brightness),
    speed: Math.max(1, byte(s.speed)),
    r: byte(s.r), g: byte(s.g), b: byte(s.b),
  };
}

// ---- scales the page shows: brightness 0..100 %, speed 1..100 (both map linearly onto the byte; a value the keyboard reports
// ---- round-trips: fromPct(toPct(x)) is stable for every x the UI can produce) ----
export const SPEED_STEPS = Object.freeze({ min: 1, max: 100 });
export const brightnessToPct = (b) => Math.round((b * 100) / 255);
export const pctToBrightness = (p) => byte((Math.max(0, Math.min(100, p)) * 255) / 100);
export const speedToStep = (s) => Math.max(1, Math.min(100, Math.round(1 + ((s - 1) * 99) / 254)));
export const stepToSpeed = (n) => byte(1 + ((Math.max(1, Math.min(100, n)) - 1) * 254) / 99);

export const ledColorHex = (s) => rgbToHex([s.r, s.g, s.b]);
export function withLedColor(s, hex) {
  const rgb = hexToRgb(hex);
  return rgb ? { ...s, r: rgb[0], g: rgb[1], b: rgb[2] } : s;
}

// ---- browser preview (an approximation of km_led.c's look, not its exact maths; the keyboard is the truth) ----

const hsv = (h, s, v) => {
  const f = (n) => { const k = (n + h / 60) % 6; return v - v * s * Math.max(0, Math.min(k, 4 - k, 1)); };
  return [f(5) * 255, f(3) * 255, f(1) * 255];
};
const lerp = (slow, fast, speed) => slow - ((speed - 1) * (slow - fast)) / 254;
const DEMO_ORDER = [3, 4, 5, 2, 1]; // km_led.h: the demo cycles through these every 8 s
const DEMO_PHASE_S = 8;

/**
 * Colour of a key in the preview. `x` is the key centre in key units, `tSec` time, `depth` 0..1 (how far the key is down, only the
 * reactive and depth effects look at it). Returns [r, g, b] 0..255, brightness already applied.
 */
export function previewLed(s, { x, tSec, depth = 0 }) {
  let fx = s.effect;
  if (fx === 6) fx = DEMO_ORDER[Math.floor(tSec / DEMO_PHASE_S) % DEMO_ORDER.length];
  const base = [s.r, s.g, s.b];
  let rgb;
  switch (fx) {
    case 1: rgb = base; break;
    case 2: {
      const period = lerp(6, 1.2, s.speed);
      const k = 0.5 - 0.5 * Math.cos((tSec / period) * 2 * Math.PI);
      rgb = base.map((c) => c * k);
      break;
    }
    case 3: rgb = hsv((((x * 24 - (tSec / lerp(12, 1.5, s.speed)) * 360) % 360) + 360) % 360, 1, 1); break;
    case 4: rgb = base.map((c) => c * Math.min(1, depth * 3)); break; // lit while down; the fade after release is not drawn
    case 5: rgb = depth <= 0.02 ? [0, 0, 0] : hsv(240 - Math.min(1, depth) * 240, 1, 1); break; // blue (shallow) to red (bottomed out)
    default: rgb = [0, 0, 0];
  }
  const k = s.brightness / 255;
  return rgb.map((v) => v * k);
}
