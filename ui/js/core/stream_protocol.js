// The light frame stream (command 0xC7), browser side. Byte-level specification: STREAM_PROTOCOL.md at the repository root; the
// device side is firmware/hid/src/km_stream.c, the other host tool is tools/magger60_stream.py (same packets, byte for byte;
// tests/stream.test.mjs replays the worked example of section 8). Pure functions, no I/O.
//
// Safety: assertStreamPacket() checks the exact bytes of every 0xC7 packet right before it leaves the browser (header, masked
// requests, checksum, op, session, flags and the per-op lengths). It is called from the transports (device/hid_transport.js and the
// simulator) via device/config_protocol.js assertSendable(), so no page can send a 0xC7 packet that is not one of the four ops below.

export const REPORT_LEN = 64;
export const HDR = 0x55;
export const CMD_STREAM = 0xc7;
export const VERSION = 1;
export const LIGHTS = 61;
export const MAX_DATA = 56;
export const WINDOW = 16;
export const ACK_EVERY = 8;
export const TIMEOUT_MIN_MS = 100;
export const TIMEOUT_MAX_MS = 5000;

export const OP = Object.freeze({ START: 1, FRAME: 2, STOP: 3, KEEPALIVE: 4 });
export const FMT = Object.freeze({ RGB: 0, GRAY: 1 });
export const FLAG_ACK = 1;
export const STATUS = Object.freeze({ OK: 0, UNKNOWN_OP: 1, BAD_LENGTH: 2, BAD_ARG: 3, SESSION: 4, STALE: 5, UNAVAILABLE: 6 });
export const STATE = Object.freeze({ IDLE: 0, OPEN: 1, LIVE: 2 });
const STATUS_NAMES = Object.fromEntries(Object.entries(STATUS).map(([k, v]) => [v, k]));
export const statusName = (s) => STATUS_NAMES[s] ?? `status ${s}`;
/** Lights one packet can carry. */
export const CHUNK_LIGHTS = Object.freeze({ [FMT.RGB]: 16, [FMT.GRAY]: 50 });

export class StreamError extends Error {
  constructor(message, code = null) { super(message); this.name = "StreamError"; this.code = code; }
}

export function checksum(len, seq, data) {
  let s = len + (seq & 0xff) + (seq >> 8);
  for (const b of data) s += b;
  return s & 0xff;
}

/** A 64-byte 'U' report: command 0xC7, the sequence number in the offset field; op / session / flags / body as data. */
export function buildPacket(op, session, seq, flags = 0, body = []) {
  const data = Uint8Array.of(op, session, flags, ...body);
  if (data.length > MAX_DATA) throw new RangeError(`data longer than ${MAX_DATA} bytes`);
  if (!Number.isInteger(seq) || seq < 0 || seq > 0xffff) throw new RangeError("the sequence number is 16 bit");
  const p = new Uint8Array(REPORT_LEN);
  p[0] = HDR; p[1] = CMD_STREAM; p[2] = 0;
  p[3] = checksum(data.length, seq, data);
  p[4] = data.length;
  p[5] = seq & 0xff; p[6] = seq >> 8; p[7] = 0;
  p.set(data, 8);
  return p;
}

export const buildStart = (session, seq, fmt = FMT.RGB, timeoutMs = 0, flags = 0) =>
  buildPacket(OP.START, session, seq, flags, [VERSION, fmt, timeoutMs & 0xff, timeoutMs >> 8, 0]);
export const buildStop = (session, seq, flags = 0) => buildPacket(OP.STOP, session, seq, flags);
export const buildKeepalive = (session, seq, flags = 0) => buildPacket(OP.KEEPALIVE, session, seq, flags);
export const buildFrameChunk = (session, seq, frameNo, first, count, raw, flags = 0) =>
  buildPacket(OP.FRAME, session, seq, flags, [frameNo & 0xff, first, count, ...raw]);

/** 61 lights as a flat Uint8Array(183) r,g,b (or 61 bytes of gray) -> [{first, count, raw}] in the largest chunks one packet carries. */
export function frameChunks(rgb, fmt = FMT.RGB) {
  const bpp = fmt === FMT.GRAY ? 1 : 3;
  if (rgb.length !== LIGHTS * bpp) throw new RangeError(`a frame is ${LIGHTS * bpp} bytes in this format`);
  const per = CHUNK_LIGHTS[fmt];
  const out = [];
  for (let first = 0; first < LIGHTS; first += per) {
    const count = Math.min(per, LIGHTS - first);
    out.push({ first, count, raw: rgb.subarray(first * bpp, (first + count) * bpp) });
  }
  return out;
}

/** Parse a reply report. Throws StreamError("nostream") for the "unknw" answer of a firmware without the command. */
export function parseReply(r) {
  if (!(r instanceof Uint8Array) || r.length !== REPORT_LEN || r[0] !== 0xaa || r[1] !== CMD_STREAM) throw new StreamError("not a stream reply", "bad_reply");
  if (r[4] < 22) throw new StreamError("this keyboard does not know the stream command", "nostream");
  const d = r.subarray(8, 8 + r[4]);
  const u16 = (i) => d[i] | (d[i + 1] << 8);
  return {
    seq: r[5] | (r[6] << 8), op: d[0], status: d[1], session: d[2], state: d[3], lastSeq: u16(4), frameNo: d[6], window: d[7],
    committed: u16(8), dropped: u16(10), lost: u16(12), stale: u16(14), rejected: u16(16), version: d[18], formats: d[19], timeoutMs: u16(20),
  };
}

/** Is sequence number `a` newer than `b` (16 bit, wrapping)? */
export function seqNewer(a, b) {
  const diff = (a - b) & 0xffff;
  return diff !== 0 && diff < 0x8000;
}

/** Independent last check on the exact bytes of a 0xC7 packet. Returns the packet. */
export function assertStreamPacket(p) {
  const bad = (m) => { throw new StreamError(`refused stream packet: ${m}`, "unsafe"); };
  if (!(p instanceof Uint8Array) || p.length !== REPORT_LEN) bad("must be 64 bytes");
  if (p[0] !== HDR || p[1] !== CMD_STREAM) bad("not a 'U' 0xC7 packet");
  if (p[2] !== 0) bad("masked packets are not allowed");
  if (p[7] !== 0) bad("byte 7 must be 0");
  const n = p[4];
  if (n < 3 || n > MAX_DATA) bad(`bad data length ${n}`);
  if (p.subarray(8 + n).some((b) => b !== 0)) bad("bytes after the data must be zero");
  const seq = p[5] | (p[6] << 8);
  if (p[3] !== checksum(n, seq, p.subarray(8, 8 + n))) bad("checksum does not match");
  const [op, session, flags] = p.subarray(8, 11);
  if (session < 1) bad("session id is 1..255");
  if (flags & ~FLAG_ACK) bad("unknown flag bits");
  const body = p.subarray(11, 8 + n);
  switch (op) {
    case OP.START: {
      if (n !== 8) bad("START carries 8 data bytes");
      const timeout = body[2] | (body[3] << 8);
      if (body[0] !== VERSION || body[1] > FMT.GRAY || body[4] !== 0) bad("START version / format / reserved");
      if (timeout !== 0 && (timeout < TIMEOUT_MIN_MS || timeout > TIMEOUT_MAX_MS)) bad("time-out must be 0 or 100..5000 ms");
      break;
    }
    case OP.FRAME: {
      const [, first, count] = body;
      if (count < 1 || first > LIGHTS - 1 || count > LIGHTS - first) bad("light range outside 0..60");
      const bytes = n - 6;
      if (bytes !== count * 3 && bytes !== count) bad("pixel bytes do not match the light count");
      break;
    }
    case OP.STOP: case OP.KEEPALIVE:
      if (n !== 3) bad("STOP / KEEPALIVE carry 3 data bytes");
      break;
    default: bad(`op ${op} is not allowed`);
  }
  return p;
}
