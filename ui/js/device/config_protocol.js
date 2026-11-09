// Packet layer of the custom firmware's configuration protocol (command 0xC6), the browser twin of
// tools/magger60_config.py. Specification: CONFIG_PROTOCOL.md at the repository root (the byte-level format, the status
// codes and the worked examples); firmware: firmware/hid/src/km_config.c. Pure functions, no I/O.
//
// Nothing here can build a packet outside the whitelist below, and every packet is checked a second time by
// assertSendable() on the exact bytes right before they go out. The whitelist: the cmd 3 version query, the 0xC6 ops
// below (each op that changes persistent state carries its own confirmation word: SAVE, PROF, RCAL, SCAL, LITE) and the
// read-only scope command 0xC5 (km_scope.h). The lights (ops 0x20 LED_GET / 0x21 LED_SET) change RAM only and carry no word.

import { assertStreamPacket, CMD_STREAM } from "../core/stream_protocol.js";

export const REPORT_LEN = 64;
export const HDR = 0x55;
export const CMD_GET_INFO = 0x03; // the stock-protocol version query, used to prove the firmware is ours
export const CMD_CONFIG = 0xc6;
export const CMD_SCOPE = 0xc5;           // read-only debug channel (km_scope.h)
export const FIRMWARE_LABEL = "KMFW";
export const PROTO_VERSION = 2;       // the newest protocol this library speaks (2 = M6 P8: DEVICE_STATUS, SCENE_*, feature bits 7..10)
export const MIN_PROTO_VERSION = 1;   // protocol 2 only ADDS: a version-1 keyboard is served as before, without the new ops
// Sanity cap of this library (a u16 length, far above any blob). The keyboard announces its real limit in GET_INFO (max_settings);
// the client honours that, so a firmware that raises its 8192-byte staging buffer needs no change here.
export const MAX_SETTINGS = 8192;
export const GET_PAGE = 44;
export const SET_PAGE = 52;
export const MAX_DEPTHS = 25;
export const MAX_SLOTS = 128;
export const MAX_SCENE = 1024;       // the light scene blob (KMLS, at most 214 bytes) has its own small staging buffer
export const DEVICE_STATUS_BYTES = 16;
export const LED_EFFECT_MAX = 9; // the firmware accepts effects 0..9 (7 custom = per-key colours of the light scene, 8 external = stream, 9 animation); UI list: ui/js/core/led.js (M6 P9c)
const MAX_DATA = 56;

export const OP = Object.freeze({
  GET_INFO: 1, GET_SETTINGS: 2, SET_BEGIN: 3, SET_PAGE: 4, COMMIT: 5, SELECT_PROFILE: 6, RECALIBRATE: 7, SAVE_CALIBRATION: 8, LIVE_DEPTH: 9,
  LED_GET: 0x20, LED_SET: 0x21, // the lights (feature bit 6): RAM only, no confirmation word, outside the settings machinery
  DEVICE_STATUS: 0x0a,          // protocol 2 (feature bit 7): read-only device state
  SCENE_GET: 0x30, SCENE_BEGIN: 0x31, SCENE_PAGE: 0x32, SCENE_COMMIT: 0x33, // protocol 2 (feature bit 8): the light scene blob
});
const ALLOWED_OPS = new Set(Object.values(OP));
// confirmation words inside the request, for the ops that change persistent state
export const WORD = Object.freeze({ COMMIT: "SAVE", SELECT_PROFILE: "PROF", RECALIBRATE: "RCAL", SAVE_CALIBRATION: "SCAL", SCENE_COMMIT: "LITE" });
const WORDS = {
  [OP.COMMIT]: WORD.COMMIT, [OP.SELECT_PROFILE]: WORD.SELECT_PROFILE, [OP.RECALIBRATE]: WORD.RECALIBRATE, [OP.SAVE_CALIBRATION]: WORD.SAVE_CALIBRATION,
  [OP.SCENE_COMMIT]: WORD.SCENE_COMMIT,
};
// exact length of the arguments (after op and token); SET_PAGE is 2 + 1..52
const ARG_LEN = { [OP.GET_INFO]: 0, [OP.GET_SETTINGS]: 4, [OP.SET_BEGIN]: 2, [OP.COMMIT]: 10, [OP.SELECT_PROFILE]: 5, [OP.RECALIBRATE]: 4, [OP.SAVE_CALIBRATION]: 4, [OP.LIVE_DEPTH]: 2,
  [OP.LED_GET]: [0, 1, 2], [OP.LED_SET]: [6, 9, 10], // LED ops: plain, extended, or extended + profile (core/led_ex.js)
  [OP.DEVICE_STATUS]: 0, [OP.SCENE_GET]: 4, [OP.SCENE_BEGIN]: 2, [OP.SCENE_COMMIT]: 10 };

export const ST = Object.freeze({
  OK: 0, UNKNOWN_OP: 1, BAD_LENGTH: 2, BAD_ARG: 3, RANGE: 4, NEED_CONFIRM: 5, SEQUENCE: 6, STALE: 7, TOO_BIG: 8,
  NOT_STAGED: 9, BAD_CRC: 10, INVALID: 11, UNSUPPORTED: 12, BUSY: 13, FAILED: 14,
});
const ST_NAMES = Object.fromEntries(Object.entries(ST).map(([k, v]) => [v, k]));
export const statusName = (s) => ST_NAMES[s] ?? `status ${s}`;

export class SafetyError extends Error {
  constructor(message) { super(message); this.name = "SafetyError"; }
}

/**
 * `status`: the keyboard's status code when it refused a request (ST.*). `code`: why the connection or transfer failed, for the
 * UI to explain (stock_firmware, proto_old, proto_new, settings_size, crc, busy, timeout, lost, not_verified, refused).
 */
export class ConfigError extends Error {
  constructor(message, status = null, code = null) {
    super(message);
    this.name = "ConfigError";
    this.status = status;
    this.code = code ?? (status === ST.BUSY ? "busy" : status === ST.BAD_CRC ? "crc" : status !== null ? "refused" : null);
  }
}

// ---- helpers ----

export const le16 = (v) => Uint8Array.of(v & 0xff, (v >> 8) & 0xff);
export const le32 = (v) => Uint8Array.of(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
export const rd16 = (b, i = 0) => b[i] | (b[i + 1] << 8);
export const rd32 = (b, i = 0) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0;
export const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
export function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32 (IEEE 802.3, as zlib): crc32("123456789") === 0xCBF43926. */
export function crc32(bytes) {
  let c = 0xffffffff;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** (len + offset bytes + data bytes) & 0xFF, the checksum of every frame. */
export function checksum(len, offset, data = []) {
  let sum = len + (offset & 0xff) + ((offset >> 8) & 0xff) + ((offset >> 16) & 0xff);
  for (const b of data) sum += b;
  return sum & 0xff;
}

/** True for a cmd 3 build string of our firmware: optional "Mon dd yyyy,hh:mm:ss" stamp, then "KMFW...". */
export function isKmfwBuild(build) {
  const m = /^[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4},[0-9]{2}:[0-9]{2}:[0-9]{2}/.exec(build);
  return (m ? build.slice(m[0].length) : build).startsWith(FIRMWARE_LABEL);
}

// ---- cmd 3 (version query) ----

export function buildInfoQuery() {
  const p = new Uint8Array(REPORT_LEN);
  p[0] = HDR;
  p[1] = CMD_GET_INFO;
  return p; // checksum of len 0 / offset 0 is 0
}

/** Decode a cmd 3 reply to {version, build}. */
export function decodeBuildReply(resp) {
  if (resp.length !== REPORT_LEN || resp[0] !== 0xaa || resp[1] !== CMD_GET_INFO) throw new ConfigError("unexpected version reply");
  const n = resp[4];
  if (n < 2 || n > MAX_DATA) throw new ConfigError("bad version reply length");
  const data = resp.subarray(8, 8 + n);
  if (checksum(n, resp[5] | (resp[6] << 8) | (resp[7] << 16), data) !== resp[3]) throw new ConfigError("version reply checksum mismatch");
  const end = data.indexOf(0, 2);
  const text = Array.from(data.subarray(2, end < 0 ? data.length : end), (c) => String.fromCharCode(c)).join("");
  return { version: (data[0] | (data[1] << 8)).toString(16).toUpperCase(), build: text };
}

// ---- 0xC6 requests ----

/** A whitelisted request as a 64-byte packet; throws SafetyError for anything else. */
export function buildRequest(op, token, args = new Uint8Array(0)) {
  if (!ALLOWED_OPS.has(op)) throw new SafetyError(`config op 0x${op.toString(16)} is not allowed in this version`);
  if (!Number.isInteger(token) || token < 0 || token > 0xff) throw new SafetyError("token out of range");
  const data = concat(Uint8Array.of(op, token), args);
  if (data.length > MAX_DATA) throw new SafetyError("request data above 56 bytes");
  const p = new Uint8Array(REPORT_LEN);
  p[0] = HDR;
  p[1] = CMD_CONFIG;
  p[3] = checksum(data.length, 0, data);
  p[4] = data.length;
  p.set(data, 8);
  return assertSendable(p);
}

/** Independent last check on the exact bytes about to be sent (cmd 3 query or a whitelisted 0xC6 request). */
export function assertSendable(p) {
  if (!(p instanceof Uint8Array) || p.length !== REPORT_LEN) throw new SafetyError("packet must be 64 bytes");
  if (p[0] !== HDR) throw new SafetyError("only 'U' packets may be sent (raw '_' packets can reboot the keyboard)");
  if (p[2] !== 0) throw new SafetyError("XOR-masked packets are not allowed");
  if (p[1] === CMD_GET_INFO) {
    if (p.subarray(3).some((b) => b !== 0)) throw new SafetyError("the version query is all zero after the command");
    return p;
  }
  if (p[1] === CMD_SCOPE) return assertScopeRequest(p);
  if (p[1] === CMD_STREAM) return assertStreamPacket(p); // the light frame stream (STREAM_PROTOCOL.md), RAM only, ends by itself
  if (p[1] !== CMD_CONFIG) throw new SafetyError(`command 0x${p[1].toString(16)} is not allowed in this version`);
  if (p[5] || p[6] || p[7]) throw new SafetyError("the offset field must be 0");
  const n = p[4];
  if (n < 2 || n > MAX_DATA) throw new SafetyError(`bad data length ${n}`);
  if (p.subarray(8 + n).some((b) => b !== 0)) throw new SafetyError("bytes after the data must be zero");
  if (p[3] !== checksum(n, 0, p.subarray(8, 8 + n))) throw new SafetyError("checksum does not match the data");
  const op = p[8];
  if (!ALLOWED_OPS.has(op)) throw new SafetyError(`config op 0x${op.toString(16)} is not allowed in this version`);
  const args = p.subarray(10, 8 + n);
  if (op === OP.SET_PAGE || op === OP.SCENE_PAGE) {
    if (args.length < 3 || args.length > 2 + SET_PAGE) throw new SafetyError(`SET_PAGE carries 1..${SET_PAGE} data bytes`);
  } else if (![ARG_LEN[op]].flat().includes(args.length)) {
    throw new SafetyError(`op ${op} needs ${ARG_LEN[op]} argument bytes, not ${args.length}`);
  }
  if (op in WORDS && String.fromCharCode(...args.subarray(0, 4)) !== WORDS[op]) {
    throw new SafetyError(`op ${op} must carry the confirmation word "${WORDS[op]}"`);
  }
  if (op === OP.GET_SETTINGS || op === OP.SCENE_GET) {
    const off = rd16(args), want = args[2], sid = args[3];
    if (want < 1 || want > GET_PAGE || (off === 0 && sid !== 0)) throw new SafetyError("bad GET_SETTINGS arguments");
  }
  if (op === OP.SET_BEGIN || op === OP.SCENE_BEGIN) {
    const total = rd16(args);
    if (total < 1 || total > (op === OP.SET_BEGIN ? MAX_SETTINGS : MAX_SCENE)) throw new SafetyError("SET_BEGIN length out of range");
  }
  if (op === OP.SELECT_PROFILE && args[4] > 15) throw new SafetyError("profile index out of range");
  if (op === OP.LIVE_DEPTH && !(args[1] >= 1 && args[1] <= MAX_DEPTHS && args[0] + args[1] <= MAX_SLOTS)) {
    throw new SafetyError("bad LIVE_DEPTH arguments");
  }
  if (op === OP.LED_GET && args.length >= 1 && args[0] !== 1) throw new SafetyError("extended LED_GET starts with the argument 1");
  if (op === OP.LED_GET && args.length === 2 && !(args[1] <= 3 || args[1] === 0xff)) throw new SafetyError("LED_GET profile must be 0..3 or 255");
  if (op === OP.LED_SET && args.length >= 9 && (args[8] & ~1)) throw new SafetyError("LED_SET flags: only bit 0 (SAVE) is defined");
  if (op === OP.LED_SET && args.length === 10 && !(args[9] <= 3 || args[9] === 0xff)) throw new SafetyError("LED_SET profile must be 0..3 or 255");
  if (op === OP.LED_SET && (args[0] > LED_EFFECT_MAX || args[2] === 0)) throw new SafetyError(`LED_SET needs effect 0..${LED_EFFECT_MAX} and speed 1..255`);
  return p;
}

// ---- 0xC5 scope requests (read-only) ----
export const SCOPE_MAX_VALUES = 128;
export const SCOPE_PAGE_VALUES = 25;
const SCOPE_HEADER = 6;

/** Scope request: len = reply data bytes accepted, offset = start | wantTag << 8, no payload. */
export function buildScopeRequest(start = 0, length = MAX_DATA, wantTag = 0) {
  if (!(Number.isInteger(start) && start >= 0 && start <= 0xff)) throw new SafetyError("scope start out of range");
  if (!(Number.isInteger(length) && length >= 0 && length <= MAX_DATA)) throw new SafetyError("scope length out of range");
  if (!(Number.isInteger(wantTag) && wantTag >= 0 && wantTag <= 0xffff)) throw new SafetyError("scope tag out of range");
  const p = new Uint8Array(REPORT_LEN);
  p[0] = HDR;
  p[1] = CMD_SCOPE;
  p[3] = checksum(length, start | (wantTag << 8));
  p[4] = length;
  p[5] = start;
  p[6] = wantTag & 0xff;
  p[7] = wantTag >> 8;
  return assertSendable(p);
}

function assertScopeRequest(p) {
  if (p[4] > MAX_DATA) throw new SafetyError(`scope length above ${MAX_DATA}`);
  if (p.subarray(8).some((b) => b !== 0)) throw new SafetyError("a scope request must not carry payload bytes");
  if (p[3] !== checksum(p[4], p[5] | (p[6] << 8) | (p[7] << 16))) throw new SafetyError("scope checksum does not match the header");
  return p;
}

/** Decode a scope reply to {tag, seq, count, start, values}. */
export function decodeScopeReply(resp) {
  if (!(resp instanceof Uint8Array) || resp.length !== REPORT_LEN) throw new ConfigError("scope reply is not 64 bytes");
  if (resp[0] === 0xab) throw new ConfigError("the keyboard reports a checksum error (0xAB)");
  if (resp[0] !== 0xaa || resp[1] !== CMD_SCOPE) throw new ConfigError("unexpected scope reply");
  const n = resp[4];
  if (n < SCOPE_HEADER || n > MAX_DATA || n % 2) throw new ConfigError(`bad scope reply length ${n}`);
  const data = resp.subarray(8, 8 + n);
  if (checksum(n, resp[5] | (resp[6] << 8) | (resp[7] << 16), data) !== resp[3]) throw new ConfigError("scope reply checksum does not match");
  if (resp[6] || resp[7]) throw new ConfigError("scope reply offset above 255");
  const count = rd16(data, 4);
  if (count > SCOPE_MAX_VALUES) throw new ConfigError(`scope count ${count} above ${SCOPE_MAX_VALUES}`);
  const values = [];
  for (let i = SCOPE_HEADER; i < n; i += 2) values.push(rd16(data, i));
  if (values.length > SCOPE_PAGE_VALUES || (values.length && resp[5] + values.length > count)) throw new ConfigError("scope reply carries more values than the snapshot has");
  if (resp.subarray(8 + n).some((b) => b !== 0)) throw new ConfigError("scope reply has data beyond its length");
  return { tag: rd16(data, 0), seq: rd16(data, 2), count, start: resp[5], values };
}

/** Decode a reply frame to {op, token, status, payload}. */
export function decodeReply(resp) {
  if (!(resp instanceof Uint8Array) || resp.length !== REPORT_LEN) throw new ConfigError("reply is not 64 bytes");
  if (resp[0] === 0xab) throw new ConfigError("the keyboard reports a checksum error (0xAB)");
  if (resp[0] !== 0xaa || resp[1] !== CMD_CONFIG) throw new ConfigError("unexpected reply");
  const n = resp[4];
  if (n < 3 || n > MAX_DATA) throw new ConfigError(`bad reply length ${n} (is this really our firmware?)`);
  if (resp[2] || resp[5] || resp[6] || resp[7]) throw new ConfigError("reply header fields must be zero");
  const data = resp.subarray(8, 8 + n);
  if (checksum(n, 0, data) !== resp[3]) throw new ConfigError("reply checksum does not match");
  if (resp.subarray(8 + n).some((b) => b !== 0)) throw new ConfigError("reply has data beyond its length");
  return { op: data[0], token: data[1], status: data[2], payload: data.slice(3) };
}

export const FEATURES = ["read settings", "write settings", "select profile", "recalibrate", "save calibration", "live depth", "lights",
  "device status", "light scene", "stream", "host lock lights", "lights per profile"];
export const FEAT = Object.freeze({ READ: 1, WRITE: 2, PROFILE: 4, RECAL: 8, SAVE_CAL: 16, LIVE: 32, LED: 64, DEVICE_STATUS: 128, SCENE: 256, STREAM: 512, HOST_LEDS: 1024, LED_PROFILE: 2048 });

/** Parse the GET_INFO payload (18 bytes; protocol 2 adds two: the scene size limit, 0 = no scene). */
export function parseInfo(p) {
  if (p.length < 18) throw new ConfigError(`GET_INFO reply too short (${p.length} bytes)`);
  const features = rd16(p, 7);
  return {
    protoVersion: p[0], profileCount: p[1], activeProfile: p[2] === 0xff ? null : p[2],
    maxSettings: rd16(p, 3), getPage: p[5], setPage: p[6], features,
    featureNames: FEATURES.filter((_, i) => (features >> i) & 1),
    stageState: p[9], stageTotal: rd16(p, 10), stageNext: rd16(p, 12), commits: rd32(p, 14),
    sceneMax: p.length >= 20 ? rd16(p, 18) : 0,
  };
}

/** Parse the DEVICE_STATUS payload (16 bytes, protocol 2): host lock lights, gamepad mode, USB fall-back, profile, layer, stream. */
export function parseDeviceStatus(p) {
  if (p.length !== DEVICE_STATUS_BYTES) throw new ConfigError(`DEVICE_STATUS reply has ${p.length} bytes, not ${DEVICE_STATUS_BYTES}`, null, "protocol");
  return {
    hostLeds: p[0], num: !!(p[0] & 1), caps: !!(p[0] & 2), scroll: !!(p[0] & 4),
    padModeSetting: p[1], padModeActive: p[2],
    usbFellBack: !!(p[3] & 1), streamActive: !!(p[3] & 2), animPlaying: !!(p[3] & 4), hostLedsKnown: !!(p[3] & 8),
    profile: p[4], layer: p[5], streamState: p[6],
    streamFrames: rd16(p, 8), streamLost: rd16(p, 10), ledReports: rd16(p, 12),
  };
}
