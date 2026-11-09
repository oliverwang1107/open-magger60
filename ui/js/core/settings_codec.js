// Binary codec of the keyboard's settings blob, km_settings formats version 1, 2 and 3 (firmware/core/include/km/km_settings.h).
// Pure functions on the "raw" settings model; the mapping to the UI's profile objects is in profile_codec.js.
//
// Version 2 (fixed layout, little endian): "KMCS" | version u16 | total length u16 | active u8 | profiles u8 | layers u8 | slots u8 |
//   4 profiles x { 76 x [press u16, release u16, mode u8, continuous u8, rtDown u16, rtUp u16, topDead u16, bottomDead u16]
//                  layer options [tap term u16 ms, one-shot timeout u16 ms, tap policy u8, reserved 0]
//                  4 layers x 76 code bytes
//                  4 layers x 76 x 2 argument bytes } | CRC-32 of everything before it.
// Version 3 (M6): the same magic and CRC, a 16 byte header (... | keys u8 = 61 | sections n u8 | 3 reserved zero bytes) and then n
//   sections [id u8][scope u8][length u16][payload]: KEYCFG 0x01 and LAYERS 0x02 for every profile (only the 61 slots that carry a
//   key), optional DKS 0x10, MORPH 0x11, SOCD 0x12, PAD 0x20 per profile and DEVICE 0x30 global. Ids from 0x80 up are optional: an
//   unknown one is skipped when reading and kept in `settings.unknown` so it can be written back. See km_settings.h for the bytes.
// Version 1 (no layer options, no argument bytes, 5488 bytes) is still READ and migrated (arguments 0, default options).
//
// encodeSettings() writes the LOWEST version that holds the data: version 2 unless the model uses something only version 3 has
// (needsV3), so somebody who never uses a new feature can go back to an older firmware. decodeSettings() reads all three.
// The raw model: { version, active, profiles: [{ keys[76], layers[4][76] (code bytes), args[4][76] = [arg0, arg1], opts,
//   dks: [{tAct, tBot, flags, acts: [[usage, events] x 4]}], morph: [{mask, usage, flags}], socd: [{a, b, mode, flags}],
//   pad: {curves: [{start, end, shape, flags} x 4], deadzone, circular, smooth} }], device: {padMode, filterMode},
//   unknown: [{id, scope, bytes}] }. A model without the M6 members (dks, morph, socd, pad, device, unknown) means "none / default".
// Every rule below is the firmware's own (km_settings_decode / km_settings_*_valid); the vectors generated from the real C
// code (ui/tests/vectors/settings_vectors.json) prove byte-for-byte agreement. Anything that is not exactly a blob the
// firmware writes is refused with a SettingsError that says why; no input makes the decoder throw anything else.
import { crc32 } from "../device/config_protocol.js";
import {
  FW_KEY_SLOTS, FW_LAYER_OPTS, FW_M6, FW_SETTINGS as F, FW_USAGE_RULE, MODE_FIXED, MODE_RAPID, SECTION, SECTION_BYTES, TRAVEL_MM100,
  CODE, defaultDevice, defaultPad, deviceProblem, dksProblem, entryNeedsV3, entryProblem, morphProblem, optsProblem, padProblem, socdProblem,
} from "./limits.js";

export class SettingsError extends Error {
  /** code: short | magic | length | crc | version | shape | range | cfg | usage | opts | section | record; where: {profile, slot, layer, field} */
  constructor(code, message, where = null) {
    super(message);
    this.name = "SettingsError";
    this.code = code;
    this.where = where;
  }
}

export const PROFILE_BYTES = F.slots * F.keyCfgBytes + F.optsBytes + F.layers * F.slots + F.layers * F.slots * F.argBytes;   // one profile of a VERSION 2 blob

// = km_layer_opts_default()
export const defaultRawOpts = () => ({ tapTerm: FW_LAYER_OPTS.tapTermDefault, oneshot: FW_LAYER_OPTS.oneShotDefault, policy: 0 });
export const zeroArgs = () => Array.from({ length: F.layers }, () => Array.from({ length: F.slots }, () => [0, 0]));

// = km_engine_key_cfg_default(): press 1.20 mm, release 0.80 mm, rapid-trigger steps 0.30 mm
export const defaultRawCfg = () => ({ press: 120, release: 80, mode: MODE_FIXED, continuous: 0, rtDown: 30, rtUp: 30, topDead: 0, bottomDead: 0 });

/**
 * null when the key config is acceptable, otherwise the name of the first rule it breaks. Same rules as km_settings_cfg_valid, which is
 * km_engine_cfg_valid: FIXED looks only at press / release (every other field is ignored, whatever it holds); RAPID checks the
 * rapid-trigger distances, dead zones, `continuous`, the actuation point (only when not continuous) and that the zones leave room.
 */
export function rawCfgProblem(c) {
  const int = (v) => Number.isInteger(v) && v >= 0 && v <= 0xffff;
  for (const f of ["press", "release", "rtDown", "rtUp", "topDead", "bottomDead"]) if (!int(c[f])) return "field_range";
  if (c.mode === MODE_FIXED) {
    return c.release >= F.minStep && c.release < c.press && c.press <= TRAVEL_MM100 ? null : "press_release";
  }
  if (c.mode !== MODE_RAPID) return "mode";
  const dist = (v) => v >= F.rtMin && v <= TRAVEL_MM100;
  const dead = (v) => v === 0 || dist(v);
  if (!dist(c.rtDown) || !dist(c.rtUp)) return "rapid_step";
  if (!dead(c.topDead) || !dead(c.bottomDead)) return "dead_zone";
  if (c.continuous > 1) return "continuous";
  if (c.continuous === 0 && !dist(c.press)) return "press_release";   // the actuation point is used
  if (c.rtDown > TRAVEL_MM100 - c.bottomDead) return "rt_reach";       // a press would be out of reach
  if ((c.topDead || F.rtMin) + c.bottomDead + F.rtMin > TRAVEL_MM100) return "dead_zone"; // the zones must leave room
  return null;
}

const dksOf = (pr) => pr.dks ?? [];
const morphOf = (pr) => pr.morph ?? [];
const socdOf = (pr) => pr.socd ?? [];
const padOf = (pr) => pr.pad ?? defaultPad();
const deviceOf = (s) => s.device ?? defaultDevice();
const unknownOf = (s) => s.unknown ?? [];

/** First problem of a raw settings model as a SettingsError, or null. */
export function rawSettingsProblem(s, rule = FW_USAGE_RULE) {
  if (!s || !Array.isArray(s.profiles) || s.profiles.length !== F.profiles) return new SettingsError("shape", `settings need exactly ${F.profiles} profiles`);
  if (!Number.isInteger(s.active) || s.active < 0 || s.active >= F.profiles) return new SettingsError("range", `active profile ${s.active} is not 0..${F.profiles - 1}`);
  const dev = deviceProblem(deviceOf(s));
  if (dev) return new SettingsError("record", `device options refused (${dev})`, { field: dev });
  if (!Array.isArray(unknownOf(s))) return new SettingsError("shape", "unknown sections must be a list");
  const seen = new Set();
  for (const u of unknownOf(s)) {
    const ok = u && Number.isInteger(u.id) && u.id >= F.optionalFrom && u.id <= 0xff && (u.scope === F.scopeGlobal || (Number.isInteger(u.scope) && u.scope >= 0 && u.scope < F.profiles))
      && (u.bytes instanceof Uint8Array || Array.isArray(u.bytes)) && u.bytes.length <= 0xffff
      && Array.from(u.bytes).every((v) => Number.isInteger(v) && v >= 0 && v <= 0xff);
    if (!ok || seen.has(`${u.id}/${u.scope}`)) return new SettingsError("section", "an unknown optional section is malformed or repeated", { field: "unknown" });
    seen.add(`${u.id}/${u.scope}`);
  }
  for (let p = 0; p < F.profiles; p++) {
    const pr = s.profiles[p];
    if (!pr || pr.keys?.length !== F.slots || pr.layers?.length !== F.layers || pr.layers.some((l) => l?.length !== F.slots)
      || pr.args?.length !== F.layers || pr.args.some((l) => l?.length !== F.slots || l.some((a) => a?.length !== F.argBytes))) {
      return new SettingsError("shape", `profile ${p + 1} must have ${F.slots} key configs and ${F.layers} layers of ${F.slots} entries with ${F.argBytes} arguments each`, { profile: p });
    }
    const opts = optsProblem(pr.opts);
    if (opts) return new SettingsError("opts", `profile ${p + 1}: layer options refused (${opts})`, { profile: p, field: opts });
    const dks = dksOf(pr), morph = morphOf(pr), socd = socdOf(pr);
    if (!Array.isArray(pr.dks ?? []) || !Array.isArray(pr.morph ?? []) || !Array.isArray(pr.socd ?? [])
      || dks.length > FW_M6.dksMax || morph.length > FW_M6.morphMax || socd.length > FW_M6.socdMax) {
      return new SettingsError("record", `profile ${p + 1}: too many DKS, Mod-Morph or SOCD records`, { profile: p, field: "count" });
    }
    for (const [list, check, name] of [[dks, dksProblem, "dks"], [morph, morphProblem, "morph"], [socd, socdProblem, "socd"]]) {
      for (let i = 0; i < list.length; i++) {
        const why = check(list[i]);
        if (why) return new SettingsError("record", `profile ${p + 1}: ${name} record ${i + 1} refused (${why})`, { profile: p, field: `${name}.${why}`, index: i });
      }
    }
    const used = new Set();
    for (const pair of socd) {
      if (used.has(pair.a) || used.has(pair.b)) return new SettingsError("record", `profile ${p + 1}: a key is in two SOCD pairs`, { profile: p, field: "socd.overlap" });
      used.add(pair.a); used.add(pair.b);
    }
    const pad = padProblem(padOf(pr));
    if (pad) return new SettingsError("record", `profile ${p + 1}: gamepad settings refused (${pad})`, { profile: p, field: `pad.${pad}` });
    for (let slot = 0; slot < F.slots; slot++) {
      const why = rawCfgProblem(pr.keys[slot]);
      if (why) return new SettingsError("cfg", `profile ${p + 1}, slot ${slot}: trigger settings refused (${why})`, { profile: p, slot, field: why });
      for (let l = 0; l < F.layers; l++) {
        const code = pr.layers[l][slot], a0 = pr.args[l][slot][0], a1 = pr.args[l][slot][1];
        const why = entryProblem(code, a0, a1, rule);
        if (why) {
          return new SettingsError("usage", `profile ${p + 1}, layer ${l}, slot ${slot}: key code 0x${Number(code).toString(16)} is not accepted (${why})`, { profile: p, layer: l, slot, field: why });
        }
        if ((code === CODE.DKS && a0 >= dks.length) || (code === CODE.MOD_MORPH && a1 >= morph.length)) {
          return new SettingsError("usage", `profile ${p + 1}, layer ${l}, slot ${slot}: the key points at a record that does not exist`, { profile: p, layer: l, slot, field: "reference" });
        }
      }
    }
  }
  return null;
}

/** The model of the factory defaults; `layerFill` is the code byte every layer entry gets (0 = no key). */
export function defaultRawSettings(layerFill = 0) {
  return {
    version: F.version,
    active: 0,
    profiles: Array.from({ length: F.profiles }, () => ({
      keys: Array.from({ length: F.slots }, defaultRawCfg),
      layers: Array.from({ length: F.layers }, () => new Array(F.slots).fill(layerFill)),
      args: zeroArgs(),
      opts: defaultRawOpts(),
      dks: [], morph: [], socd: [],
      pad: defaultPad(),
    })),
    device: defaultDevice(),
    unknown: [],
  };
}

export const cloneRaw = (s) => JSON.parse(JSON.stringify(s, (k, v) => (v instanceof Uint8Array ? Array.from(v) : v)));

const w16 = (b, o, v) => { b[o] = v & 0xff; b[o + 1] = (v >> 8) & 0xff; };
const r16 = (b, o) => b[o] | (b[o + 1] << 8);
const r32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const putCrc = (b) => {
  const crc = crc32(b.subarray(0, b.length - 4));
  const o = b.length - 4;
  b[o] = crc & 0xff; b[o + 1] = (crc >>> 8) & 0xff; b[o + 2] = (crc >>> 16) & 0xff; b[o + 3] = (crc >>> 24) & 0xff;
};
const putCfg = (b, o, c) => {
  w16(b, o, c.press); w16(b, o + 2, c.release); b[o + 4] = c.mode; b[o + 5] = c.continuous;
  w16(b, o + 6, c.rtDown); w16(b, o + 8, c.rtUp); w16(b, o + 10, c.topDead); w16(b, o + 12, c.bottomDead);
};
const getCfg = (b, o) => ({
  press: r16(b, o), release: r16(b, o + 2), mode: b[o + 4], continuous: b[o + 5],
  rtDown: r16(b, o + 6), rtUp: r16(b, o + 8), topDead: r16(b, o + 10), bottomDead: r16(b, o + 12),
});

export const padIsDefault = (p) => {
  const d = defaultPad();
  return p.deadzone === d.deadzone && p.circular === d.circular && p.smooth === d.smooth
    && p.curves.every((c, i) => c.start === d.curves[i].start && c.end === d.curves[i].end && c.shape === d.curves[i].shape && c.flags === d.curves[i].flags);
};
export const deviceIsDefault = (d) => d.padMode === 0 && d.filterMode === 0;

/** True when the model holds something a version 2 blob cannot: a version 3 only table entry, a DKS / Mod-Morph / SOCD record, gamepad
 * settings or device options that differ from the default, or an unknown optional section (kept only by version 3). Same as km_settings_needs_v3. */
export function needsV3(s) {
  if (!deviceIsDefault(deviceOf(s)) || unknownOf(s).length > 0) return true;
  for (const pr of s.profiles) {
    if (dksOf(pr).length || morphOf(pr).length || socdOf(pr).length || !padIsDefault(padOf(pr))) return true;
    for (let l = 0; l < F.layers; l++) {
      for (let slot = 0; slot < F.slots; slot++) if (entryNeedsV3(pr.layers[l][slot], pr.args[l][slot][0], pr.args[l][slot][1])) return true;
    }
  }
  return false;
}

/** Version 2 bytes (throws SettingsError for invalid settings and when the settings need version 3). */
export function encodeSettingsV2(s, rule = FW_USAGE_RULE) {
  const bad = rawSettingsProblem(s, rule);
  if (bad) throw bad;
  if (needsV3(s)) throw new SettingsError("version", "these settings use features only settings format version 3 can hold");
  const b = new Uint8Array(F.size);
  b.set(F.magic, 0);
  w16(b, 4, F.v2Version);
  w16(b, 6, F.size);
  b[8] = s.active; b[9] = F.profiles; b[10] = F.layers; b[11] = F.slots;
  let o = F.headerBytes;
  for (const pr of s.profiles) {
    for (const c of pr.keys) { putCfg(b, o, c); o += F.keyCfgBytes; }
    w16(b, o, pr.opts.tapTerm); w16(b, o + 2, pr.opts.oneshot); b[o + 4] = pr.opts.policy; b[o + 5] = 0;
    o += F.optsBytes;
    for (const layer of pr.layers) { b.set(layer, o); o += F.slots; }
    for (const layer of pr.args) for (const a of layer) { b[o] = a[0]; b[o + 1] = a[1]; o += F.argBytes; }
  }
  putCrc(b);
  return b;
}

/** Version 3 bytes: sections in the order the firmware writes them (per profile KEYCFG, LAYERS, DKS, MORPH, SOCD, PAD; then DEVICE; then the
 * unknown optional sections). A slot without a key is not stored. */
export function encodeSettingsV3(s, rule = FW_USAGE_RULE) {
  const bad = rawSettingsProblem(s, rule);
  if (bad) throw bad;
  const parts = [];   // [id, scope, Uint8Array]
  s.profiles.forEach((pr, p) => {
    const kc = new Uint8Array(SECTION_BYTES.keycfg);
    FW_KEY_SLOTS.forEach((slot, i) => putCfg(kc, i * F.keyCfgBytes, pr.keys[slot]));
    parts.push([SECTION.KEYCFG, p, kc]);
    const ly = new Uint8Array(SECTION_BYTES.layers);
    w16(ly, 0, pr.opts.tapTerm); w16(ly, 2, pr.opts.oneshot); ly[4] = pr.opts.policy;
    let o = F.optsBytes;
    for (let l = 0; l < F.layers; l++) for (const slot of FW_KEY_SLOTS) ly[o++] = pr.layers[l][slot];
    for (let l = 0; l < F.layers; l++) for (const slot of FW_KEY_SLOTS) { ly[o++] = pr.args[l][slot][0]; ly[o++] = pr.args[l][slot][1]; }
    parts.push([SECTION.LAYERS, p, ly]);
    const dks = dksOf(pr);
    if (dks.length) {
      const d = new Uint8Array(1 + dks.length * SECTION_BYTES.dksRec);
      d[0] = dks.length;
      dks.forEach((r, i) => {
        const q = 1 + i * SECTION_BYTES.dksRec;
        w16(d, q, r.tAct); w16(d, q + 2, r.tBot); d[q + 4] = r.flags;
        r.acts.forEach((a, k) => { d[q + 8 + 2 * k] = a[0]; d[q + 9 + 2 * k] = a[1]; });
      });
      parts.push([SECTION.DKS, p, d]);
    }
    const morph = morphOf(pr);
    if (morph.length) {
      const m = new Uint8Array(morph.length * SECTION_BYTES.morphRec);
      morph.forEach((r, i) => { m[4 * i] = r.mask; m[4 * i + 1] = r.usage; m[4 * i + 2] = r.flags; });
      parts.push([SECTION.MORPH, p, m]);
    }
    const socd = socdOf(pr);
    if (socd.length) {
      const c = new Uint8Array(socd.length * SECTION_BYTES.socdRec);
      socd.forEach((r, i) => { c[4 * i] = r.a; c[4 * i + 1] = r.b; c[4 * i + 2] = r.mode; c[4 * i + 3] = r.flags; });
      parts.push([SECTION.SOCD, p, c]);
    }
    const pad = padOf(pr);
    if (!padIsDefault(pad)) {
      const a = new Uint8Array(SECTION_BYTES.pad);
      pad.curves.forEach((c, i) => { a[4 * i] = c.start; a[4 * i + 1] = c.end; a[4 * i + 2] = c.shape; a[4 * i + 3] = c.flags; });
      a[16] = pad.deadzone; a[17] = pad.circular; a[18] = pad.smooth;
      parts.push([SECTION.PAD, p, a]);
    }
  });
  const dev = deviceOf(s);
  if (!deviceIsDefault(dev)) {
    const d = new Uint8Array(SECTION_BYTES.device);
    d[0] = dev.padMode; d[1] = dev.filterMode;
    parts.push([SECTION.DEVICE, F.scopeGlobal, d]);
  }
  for (const u of unknownOf(s)) parts.push([u.id, u.scope, Uint8Array.from(u.bytes)]);
  const size = F.v3HeaderBytes + parts.reduce((n, p) => n + F.v3SectionHeaderBytes + p[2].length, 0) + 4;
  if (parts.length > F.v3MaxSections || size > F.v3MaxSize) throw new SettingsError("length", `the settings need ${size} bytes in ${parts.length} sections; the most that fits is ${F.v3MaxSize} in ${F.v3MaxSections}`);
  const b = new Uint8Array(size);
  b.set(F.magic, 0);
  w16(b, 4, F.version);
  w16(b, 6, size);
  b[8] = s.active; b[9] = F.profiles; b[10] = F.layers; b[11] = F.keys; b[12] = parts.length;
  let o = F.v3HeaderBytes;
  for (const [id, scope, payload] of parts) {
    b[o] = id; b[o + 1] = scope; w16(b, o + 2, payload.length);
    b.set(payload, o + F.v3SectionHeaderBytes);
    o += F.v3SectionHeaderBytes + payload.length;
  }
  putCrc(b);
  return b;
}

/** Serialize with the lowest version that holds the model: version 2, or version 3 when needsV3(). Throws SettingsError for anything the firmware would refuse. */
export function encodeSettings(s, rule = FW_USAGE_RULE) {
  const bad = rawSettingsProblem(s, rule);
  if (bad) throw bad;
  return needsV3(s) ? encodeSettingsV3(s, rule) : encodeSettingsV2(s, rule);
}

const fail = (code, msg, where) => { throw new SettingsError(code, msg, where); };

function decodeV3(b) {
  if (b.length < F.v3HeaderBytes + 4 || b.length > F.v3MaxSize) fail("length", `a version 3 blob is at most ${F.v3MaxSize} bytes, this one has ${b.length}`);
  if (b[9] !== F.profiles || b[10] !== F.layers || b[11] !== F.keys) {
    fail("shape", `the blob holds ${b[9]} profiles x ${b[10]} layers x ${b[11]} keys, expected ${F.profiles} x ${F.layers} x ${F.keys}`);
  }
  const n = b[12];
  if (n > F.v3MaxSections) fail("section", `${n} sections: the most is ${F.v3MaxSections}`);
  if (b[13] !== 0 || b[14] !== 0 || b[15] !== 0) fail("section", "the reserved header bytes are not 0");
  const sec = Array.from({ length: F.profiles }, () => ({}));
  let dev = null;
  const unknown = [];
  const seen = new Set();
  const end = b.length - 4;
  let p = F.v3HeaderBytes;
  for (let i = 0; i < n; i++) {
    if (end - p < F.v3SectionHeaderBytes) fail("section", `section ${i + 1} is cut off`);
    const id = b[p], scope = b[p + 1], len = r16(b, p + 2);
    p += F.v3SectionHeaderBytes;
    if (end - p < len) fail("section", `section ${i + 1} (0x${id.toString(16)}) claims ${len} bytes but the blob ends first`);
    if (scope !== F.scopeGlobal && scope >= F.profiles) fail("section", `section 0x${id.toString(16)} has the scope ${scope}`);
    const key = `${id}/${scope}`;
    if (seen.has(key)) fail("section", `section 0x${id.toString(16)} appears twice for the same scope`);
    seen.add(key);
    const payload = b.subarray(p, p + len);
    const name = { [SECTION.KEYCFG]: "keycfg", [SECTION.LAYERS]: "layers", [SECTION.DKS]: "dks", [SECTION.MORPH]: "morph", [SECTION.SOCD]: "socd", [SECTION.PAD]: "pad" }[id];
    if (name) {
      if (scope >= F.profiles) fail("section", `section 0x${id.toString(16)} needs a profile`);
      sec[scope][name] = payload;
    } else if (id === SECTION.DEVICE) {
      if (scope !== F.scopeGlobal) fail("section", "the device section is global");
      dev = payload;
    } else if (id < F.optionalFrom) {
      fail("section", `section 0x${id.toString(16)} is not known to this configurator and is required`);
    } else {
      unknown.push({ id, scope, bytes: Array.from(payload) });
    }
    p += len;
  }
  if (p !== end) fail("section", "there are bytes between the last section and the CRC");

  const s = defaultRawSettings(0);
  s.active = b[8];
  s.unknown = unknown;
  for (let pr = 0; pr < F.profiles; pr++) {
    const q = s.profiles[pr], x = sec[pr];
    if (!x.keycfg || x.keycfg.length !== SECTION_BYTES.keycfg || !x.layers || x.layers.length !== SECTION_BYTES.layers) {
      fail("section", `profile ${pr + 1} needs a key section of ${SECTION_BYTES.keycfg} bytes and a layer section of ${SECTION_BYTES.layers} bytes`, { profile: pr });
    }
    FW_KEY_SLOTS.forEach((slot, i) => { q.keys[slot] = getCfg(x.keycfg, i * F.keyCfgBytes); });
    const ly = x.layers;
    if (ly[5] !== 0) fail("opts", `profile ${pr + 1}: the reserved byte of the layer options is not 0`, { profile: pr, field: "reserved" });
    q.opts = { tapTerm: r16(ly, 0), oneshot: r16(ly, 2), policy: ly[4] };
    let o = F.optsBytes;
    for (let l = 0; l < F.layers; l++) for (const slot of FW_KEY_SLOTS) q.layers[l][slot] = ly[o++];
    for (let l = 0; l < F.layers; l++) for (const slot of FW_KEY_SLOTS) { q.args[l][slot] = [ly[o], ly[o + 1]]; o += 2; }

    if (x.dks) {
      const d = x.dks;
      if (d.length < 1 || d[0] > FW_M6.dksMax || d.length !== 1 + d[0] * SECTION_BYTES.dksRec) fail("record", `profile ${pr + 1}: the DKS section does not hold what its count says`, { profile: pr, field: "dks.count" });
      for (let i = 0; i < d[0]; i++) {
        const t = 1 + i * SECTION_BYTES.dksRec;
        if (d[t + 5] || d[t + 6] || d[t + 7]) fail("record", `profile ${pr + 1}: DKS record ${i + 1} has a reserved byte that is not 0`, { profile: pr, field: "dks.reserved", index: i });
        q.dks.push({ tAct: r16(d, t), tBot: r16(d, t + 2), flags: d[t + 4], acts: [0, 1, 2, 3].map((k) => [d[t + 8 + 2 * k], d[t + 9 + 2 * k]]) });
      }
    }
    if (x.morph) {
      const m = x.morph;
      if (m.length % SECTION_BYTES.morphRec !== 0 || m.length / SECTION_BYTES.morphRec > FW_M6.morphMax) fail("record", `profile ${pr + 1}: the Mod-Morph section has a wrong length`, { profile: pr, field: "morph.count" });
      for (let i = 0; i < m.length / 4; i++) {
        if (m[4 * i + 3]) fail("record", `profile ${pr + 1}: Mod-Morph record ${i + 1} has a reserved byte that is not 0`, { profile: pr, field: "morph.reserved", index: i });
        q.morph.push({ mask: m[4 * i], usage: m[4 * i + 1], flags: m[4 * i + 2] });
      }
    }
    if (x.socd) {
      const c = x.socd;
      if (c.length % SECTION_BYTES.socdRec !== 0 || c.length / SECTION_BYTES.socdRec > FW_M6.socdMax) fail("record", `profile ${pr + 1}: the SOCD section has a wrong length`, { profile: pr, field: "socd.count" });
      for (let i = 0; i < c.length / 4; i++) q.socd.push({ a: c[4 * i], b: c[4 * i + 1], mode: c[4 * i + 2], flags: c[4 * i + 3] });
    }
    if (x.pad) {
      const a = x.pad;
      if (a.length !== SECTION_BYTES.pad) fail("record", `profile ${pr + 1}: the gamepad section must be ${SECTION_BYTES.pad} bytes`, { profile: pr, field: "pad.length" });
      if (a.subarray(19, 24).some((v) => v !== 0)) fail("record", `profile ${pr + 1}: a reserved byte of the gamepad options is not 0`, { profile: pr, field: "pad.reserved" });
      q.pad = {
        curves: [0, 1, 2, 3].map((i) => ({ start: a[4 * i], end: a[4 * i + 1], shape: a[4 * i + 2], flags: a[4 * i + 3] })),
        deadzone: a[16], circular: a[17], smooth: a[18],
      };
    }
  }
  if (dev) {
    if (dev.length !== SECTION_BYTES.device) fail("record", `the device section must be ${SECTION_BYTES.device} bytes`, { field: "device.length" });
    if (dev.subarray(2).some((v) => v !== 0)) fail("record", "a reserved byte of the device options is not 0", { field: "device.reserved" });
    s.device = { padMode: dev[0], filterMode: dev[1] };
  }
  return s;
}

function decodeV12(b, version) {
  const want = version === 1 ? F.v1Size : F.size;
  if (b.length !== want) fail("length", `a version ${version} blob must be ${want} bytes, not ${b.length}`);
  if (b[9] !== F.profiles || b[10] !== F.layers || b[11] !== F.slots) {
    fail("shape", `the blob holds ${b[9]} profiles x ${b[10]} layers x ${b[11]} slots, expected ${F.profiles} x ${F.layers} x ${F.slots}`);
  }
  const s = defaultRawSettings(0);
  s.active = b[8];
  let o = F.headerBytes;
  for (let p = 0; p < F.profiles; p++) {
    const q = s.profiles[p];
    for (let slot = 0; slot < F.slots; slot++) { q.keys[slot] = getCfg(b, o); o += F.keyCfgBytes; }
    if (version === 2) {
      if (b[o + 5] !== 0) fail("opts", `profile ${p + 1}: the reserved byte of the layer options is not 0`, { profile: p, field: "reserved" });
      q.opts = { tapTerm: r16(b, o), oneshot: r16(b, o + 2), policy: b[o + 4] };
      o += F.optsBytes;
    }
    for (let l = 0; l < F.layers; l++) { q.layers[l] = Array.from(b.subarray(o, o + F.slots)); o += F.slots; }
    if (version === 2) {
      for (let l = 0; l < F.layers; l++) for (let slot = 0; slot < F.slots; slot++) { q.args[l][slot] = [b[o], b[o + 1]]; o += F.argBytes; }
    } else {
      // version 1: the codes 0xE8..0xEF did not exist; the other codes mean the same in both versions
      for (let l = 0; l < F.layers; l++) {
        for (let slot = 0; slot < F.slots; slot++) {
          const c = q.layers[l][slot];
          if (c >= 0xe8 && c <= 0xef) fail("usage", `profile ${p + 1}, layer ${l}, slot ${slot}: key code 0x${c.toString(16)} did not exist in settings format version 1`, { profile: p, layer: l, slot });
        }
      }
    }
    if (version === 2) {
      // a version 2 blob follows the version 2 rule of an entry: no M6 codes, no wider tap-hold, no light action
      for (let l = 0; l < F.layers; l++) {
        for (let slot = 0; slot < F.slots; slot++) {
          if (entryNeedsV3(q.layers[l][slot], q.args[l][slot][0], q.args[l][slot][1])) {
            fail("usage", `profile ${p + 1}, layer ${l}, slot ${slot}: key code 0x${q.layers[l][slot].toString(16)} did not exist in settings format version 2`, { profile: p, layer: l, slot });
          }
        }
      }
    }
  }
  return s;
}

/**
 * Parse and validate exactly like km_settings_decode(): version 3, 2, or 1 (migrated: arguments 0, default layer options, and the
 * version 1 rule for code bytes). Throws SettingsError (never returns a half-read model, never throws anything else). The result is always
 * a model of the newest version (see FW_SETTINGS.version).
 */
export function decodeSettings(bytes, rule = FW_USAGE_RULE) {
  const b = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes ?? []);
  if (b.length < F.headerBytes + 4) throw new SettingsError("short", `only ${b.length} bytes: too short to be a settings blob`);
  if (!F.magic.every((v, i) => b[i] === v)) throw new SettingsError("magic", "not a settings blob of this firmware (the magic 'KMCS' is missing)");
  if (r16(b, 6) !== b.length) throw new SettingsError("length", `the blob says it is ${r16(b, 6)} bytes but ${b.length} arrived`);
  if (r32(b, b.length - 4) !== crc32(b.subarray(0, b.length - 4))) throw new SettingsError("crc", "the settings blob is damaged (CRC-32 mismatch)");
  const version = r16(b, 4);
  if (version !== 1 && version !== 2 && version !== 3) throw new SettingsError("version", `settings format version ${version} is not supported (this configurator reads versions 1 to ${F.version})`);
  const s = version === 3 ? decodeV3(b) : decodeV12(b, version);
  const bad = rawSettingsProblem(s, rule);
  if (bad) throw bad;
  return s;
}

/** Explain a SettingsError (or any error) for the person in front of the screen; keys live in the locales as "err.settings.<code>". */
export const settingsErrorCode = (err) => (err instanceof SettingsError ? err.code : "unknown");
