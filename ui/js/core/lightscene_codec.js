// Binary codec of the keyboard's light scene blob "KMLS" version 1 (firmware/core/include/km/km_lightscene.h).
// Pure functions on a plain model; no device, no DOM. The scene is the part of the lights that is more than the five numbers of LED_SET:
// indicator rules (a light that shows Caps Lock, the active layer, ...), one colour per key (effect "custom"), and the animation settings.
//
// Layout (little endian): "KMLS" | version u16 = 1 | total length u16 | section count u8 (0..8) | 7 reserved bytes (0) |
//   sections, each [id u8][flags u8 = 0][len u16][payload] with STRICTLY INCREASING ids (id < 0x80 = required, >= 0x80 = optional) |
//   CRC-32 of everything before it.
//   0x01 rules  [count u8 <= 8] then count x 8 bytes [source, param, led, flags, r, g, b, dim]
//   0x02 keys   16 x [r g b] palette, then 61 palette indices (one per light, km_led order)
//   0x03 anim   [flags u8: bit0 start by itself, bit1 loop][clip u8][offset ms i32][reserved u16 = 0]
// The encoder always writes the three known sections, in this order. The decoder accepts missing sections (defaults) and keeps unknown
// OPTIONAL sections in model.extra, so a newer firmware's data survives an edit in this UI; it refuses everything else the firmware refuses.
// The vectors generated from the real C code (ui/tests/vectors/lightscene_vectors.json) prove byte-for-byte agreement.
import { crc32 } from "../device/config_protocol.js";

export class LightSceneError extends Error {
  /** code: short | magic | length | crc | version | reserved | section | range | shape | size */
  constructor(code, message, where = null) {
    super(message);
    this.name = "LightSceneError";
    this.code = code;
    this.where = where;
  }
}

export const LS = Object.freeze({
  magic: [0x4b, 0x4d, 0x4c, 0x53],       // "KMLS"
  version: 1,
  headerBytes: 16,
  maxRules: 8,
  ruleBytes: 8,
  palette: 16,
  leds: 61,
  maxEncoded: 214,
  maxDecode: 1024,
  maxSections: 8,
  animBytes: 8,
  keysBytes: 16 * 3 + 61,
  animOffsetMax: 3600000,
  secRules: 0x01,
  secKeys: 0x02,
  secAnim: 0x03,
});

/** Indicator sources (KM_LS_SRC_*). `param` is the layer / profile number for LAYER / PROFILE (0..3) and 0 for the others. */
export const SRC = Object.freeze({ NONE: 0, HOST_NUM: 1, HOST_CAPS: 2, HOST_SCROLL: 3, LAYER: 4, PROFILE: 5, PAD: 6, TOGGLE: 7, EXTERNAL: 8 });
export const SRC_LAST = 8;
/** What the light does while its condition is false (flags bits 0-1) and the blink flag (bit 2). */
export const OFF = Object.freeze({ KEEP: 0, BLACK: 1, DIM: 2 });
export const FLAG_OFF_MASK = 0x03;
export const FLAG_BLINK = 0x04;
export const FLAG_MASK = 0x07;
export const ANIM_AUTOSTART = 0x01;
export const ANIM_LOOP = 0x02;
export const ANIM_MASK = 0x03;

export const defaultScene = () => ({
  rules: [],
  palette: Array.from({ length: LS.palette }, (_, i) => (i === 0 ? [0, 150, 255] : [0, 0, 0])),   // = km_lightscene_default()
  keys: new Array(LS.leds).fill(0),
  anim: { flags: 0, clip: 0, offsetMs: 0 },
  extra: [],
});

export const cloneScene = (s) => ({
  rules: s.rules.map((r) => ({ ...r, color: [...r.color] })),
  palette: s.palette.map((c) => [...c]),
  keys: [...s.keys],
  anim: { ...s.anim },
  extra: (s.extra ?? []).map((e) => ({ id: e.id, data: Uint8Array.from(e.data) })),
});

const isByte = (v) => Number.isInteger(v) && v >= 0 && v <= 255;

/** null when the rule is acceptable, otherwise the name of the first rule it breaks (= km_ls_rule_valid). */
export function ruleProblem(r) {
  if (!r || !Array.isArray(r.color) || r.color.length !== 3) return "shape";
  for (const f of [r.source, r.param, r.led, r.flags, r.dim, ...r.color]) if (!isByte(f)) return "field_range";
  if (r.source < SRC.HOST_NUM || r.source > SRC_LAST) return "source";
  const paramMax = r.source === SRC.LAYER ? 4 : r.source === SRC.PROFILE ? 4 : 1;
  if (r.param >= paramMax) return "param";
  if (r.led >= LS.leds) return "led";
  if ((r.flags & ~FLAG_MASK) !== 0) return "flags";
  const off = r.flags & FLAG_OFF_MASK;
  if (off > OFF.DIM) return "off_mode";
  if (off !== OFF.DIM && r.dim !== 0) return "dim";
  return null;
}

/** First problem of a scene model as a LightSceneError, or null (= km_lightscene_valid, plus the rules about unknown optional sections). */
export function sceneProblem(s) {
  if (!s || !Array.isArray(s.rules) || !Array.isArray(s.palette) || !Array.isArray(s.keys) || !s.anim) return new LightSceneError("shape", "not a light scene model");
  if (s.rules.length > LS.maxRules) return new LightSceneError("range", `at most ${LS.maxRules} indicator rules`, { field: "rules" });
  for (let i = 0; i < s.rules.length; i++) {
    const why = ruleProblem(s.rules[i]);
    if (why) return new LightSceneError("range", `indicator rule ${i + 1} refused (${why})`, { rule: i, field: why });
  }
  if (s.palette.length !== LS.palette || s.palette.some((c) => !Array.isArray(c) || c.length !== 3 || !c.every(isByte))) {
    return new LightSceneError("shape", `the palette needs exactly ${LS.palette} colours of three bytes`, { field: "palette" });
  }
  if (s.keys.length !== LS.leds) return new LightSceneError("shape", `the key colours need exactly ${LS.leds} palette indices`, { field: "keys" });
  for (let i = 0; i < LS.leds; i++) {
    if (!Number.isInteger(s.keys[i]) || s.keys[i] < 0 || s.keys[i] >= LS.palette) return new LightSceneError("range", `light ${i}: palette index ${s.keys[i]} is not 0..${LS.palette - 1}`, { light: i, field: "keys" });
  }
  const a = s.anim;
  if (!isByte(a.flags) || (a.flags & ~ANIM_MASK) !== 0) return new LightSceneError("range", "animation flags refused", { field: "anim.flags" });
  if (!isByte(a.clip)) return new LightSceneError("range", "animation clip is one byte", { field: "anim.clip" });
  if (!Number.isInteger(a.offsetMs) || a.offsetMs < -LS.animOffsetMax || a.offsetMs > LS.animOffsetMax) {
    return new LightSceneError("range", `animation offset must be within +-${LS.animOffsetMax} ms`, { field: "anim.offsetMs" });
  }
  let prev = LS.secAnim;
  for (const e of s.extra ?? []) {
    if (!isByte(e.id) || e.id < 0x80 || e.id <= prev) return new LightSceneError("section", "extra sections need increasing ids from 0x80", { id: e.id });
    if (!(e.data instanceof Uint8Array) || e.data.length > 0xffff) return new LightSceneError("section", "extra section data must be bytes", { id: e.id });
    prev = e.id;
  }
  return null;
}

const w16 = (b, o, v) => { b[o] = v & 0xff; b[o + 1] = (v >> 8) & 0xff; };
const w32 = (b, o, v) => { b[o] = v & 0xff; b[o + 1] = (v >>> 8) & 0xff; b[o + 2] = (v >>> 16) & 0xff; b[o + 3] = (v >>> 24) & 0xff; };
const r16 = (b, o) => b[o] | (b[o + 1] << 8);
const r32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

/** Serialize. Throws LightSceneError for anything the firmware would refuse (an invalid scene is never written out). */
export function encodeScene(s) {
  const bad = sceneProblem(s);
  if (bad) throw bad;
  const extra = s.extra ?? [];
  const nSections = 3 + extra.length;
  if (nSections > LS.maxSections) throw new LightSceneError("section", `at most ${LS.maxSections} sections`);
  const rulesLen = 1 + s.rules.length * LS.ruleBytes;
  let total = LS.headerBytes + (4 + rulesLen) + (4 + LS.keysBytes) + (4 + LS.animBytes) + 4;
  for (const e of extra) total += 4 + e.data.length;
  if (total > LS.maxDecode) throw new LightSceneError("size", `the scene would be ${total} bytes; the firmware takes at most ${LS.maxDecode}`);
  const b = new Uint8Array(total);
  b.set(LS.magic, 0);
  w16(b, 4, LS.version);
  w16(b, 6, total);
  b[8] = nSections;
  let o = LS.headerBytes;
  const head = (id, len) => { b[o] = id; b[o + 1] = 0; w16(b, o + 2, len); o += 4; };

  head(LS.secRules, rulesLen);
  b[o++] = s.rules.length;
  for (const r of s.rules) {
    b[o] = r.source; b[o + 1] = r.param; b[o + 2] = r.led; b[o + 3] = r.flags;
    b[o + 4] = r.color[0]; b[o + 5] = r.color[1]; b[o + 6] = r.color[2]; b[o + 7] = r.dim;
    o += LS.ruleBytes;
  }
  head(LS.secKeys, LS.keysBytes);
  for (const c of s.palette) { b[o] = c[0]; b[o + 1] = c[1]; b[o + 2] = c[2]; o += 3; }
  b.set(s.keys, o);
  o += LS.leds;
  head(LS.secAnim, LS.animBytes);
  b[o] = s.anim.flags; b[o + 1] = s.anim.clip;
  w32(b, o + 2, s.anim.offsetMs >>> 0);                     // two's complement for a negative offset
  o += LS.animBytes;
  for (const e of extra) {
    head(e.id, e.data.length);
    b.set(e.data, o);
    o += e.data.length;
  }
  w32(b, o, crc32(b.subarray(0, o)));
  return b;
}

/**
 * Parse and validate exactly like km_lightscene_decode(). Throws LightSceneError (never returns a half-read model). Missing sections give
 * the defaults; unknown optional sections are kept in `extra` (the firmware drops them, so encodeScene({...s, extra: []}) is what the
 * firmware would write back).
 */
export function decodeScene(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes ?? []);
  if (b.length < LS.headerBytes + 4) throw new LightSceneError("short", `only ${b.length} bytes: too short to be a light scene`);
  if (b.length > LS.maxDecode) throw new LightSceneError("size", `${b.length} bytes: more than the ${LS.maxDecode} the firmware takes`);
  if (!LS.magic.every((v, i) => b[i] === v)) throw new LightSceneError("magic", "not a light scene of this firmware (the magic 'KMLS' is missing)");
  if (r16(b, 4) !== LS.version) throw new LightSceneError("version", `light scene version ${r16(b, 4)} is not understood`);
  if (r16(b, 6) !== b.length) throw new LightSceneError("length", `the blob says it is ${r16(b, 6)} bytes but ${b.length} arrived`);
  if (b[8] > LS.maxSections) throw new LightSceneError("section", `${b[8]} sections: at most ${LS.maxSections}`);
  for (let i = 9; i < LS.headerBytes; i++) if (b[i] !== 0) throw new LightSceneError("reserved", "reserved header bytes are not zero");
  if (r32(b, b.length - 4) !== crc32(b.subarray(0, b.length - 4))) throw new LightSceneError("crc", "the light scene is damaged (CRC-32 mismatch)");

  const s = defaultScene();
  const end = b.length - 4;
  let o = LS.headerBytes;
  let prev = 0;
  for (let n = 0; n < b[8]; n++) {
    if (end - o < 4) throw new LightSceneError("section", "a section header does not fit");
    const id = b[o], flags = b[o + 1], len = r16(b, o + 2);
    o += 4;
    if (flags !== 0) throw new LightSceneError("section", `section 0x${id.toString(16)}: flags must be 0`, { id });
    if (id <= prev) throw new LightSceneError("section", `section 0x${id.toString(16)}: ids must increase`, { id });
    if (len > end - o) throw new LightSceneError("section", `section 0x${id.toString(16)} runs past the end`, { id });
    prev = id;
    const p = b.subarray(o, o + len);
    if (id === LS.secRules) {
      if (len < 1 || p[0] > LS.maxRules || len !== 1 + p[0] * LS.ruleBytes) throw new LightSceneError("section", "rule section size does not match its count", { id });
      s.rules = [];
      for (let i = 0; i < p[0]; i++) {
        const q = p.subarray(1 + i * LS.ruleBytes, 1 + (i + 1) * LS.ruleBytes);
        const r = { source: q[0], param: q[1], led: q[2], flags: q[3], color: [q[4], q[5], q[6]], dim: q[7] };
        const why = ruleProblem(r);
        if (why) throw new LightSceneError("range", `indicator rule ${i + 1} refused (${why})`, { rule: i, field: why });
        s.rules.push(r);
      }
    } else if (id === LS.secKeys) {
      if (len !== LS.keysBytes) throw new LightSceneError("section", "key colour section has the wrong size", { id });
      s.palette = Array.from({ length: LS.palette }, (_, i) => [p[3 * i], p[3 * i + 1], p[3 * i + 2]]);
      s.keys = Array.from(p.subarray(3 * LS.palette, LS.keysBytes));
      for (let i = 0; i < LS.leds; i++) {
        if (s.keys[i] >= LS.palette) throw new LightSceneError("range", `light ${i}: palette index ${s.keys[i]} is not 0..${LS.palette - 1}`, { light: i });
      }
    } else if (id === LS.secAnim) {
      if (len !== LS.animBytes || (p[0] & ~ANIM_MASK) !== 0 || r16(p, 6) !== 0) throw new LightSceneError("section", "animation section refused", { id });
      const off = r32(p, 2) | 0;                              // signed
      if (off < -LS.animOffsetMax || off > LS.animOffsetMax) throw new LightSceneError("range", "animation offset out of range", { id });
      s.anim = { flags: p[0], clip: p[1], offsetMs: off };
    } else if (id >= 0x80) {
      s.extra.push({ id, data: Uint8Array.from(p) });
    } else {
      throw new LightSceneError("section", `unknown required section 0x${id.toString(16)}`, { id });
    }
    o += len;
  }
  if (o !== end) throw new LightSceneError("length", "bytes between the last section and the checksum");
  return s;
}
