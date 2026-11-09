// The keyboard's side of the light frame stream (STREAM_PROTOCOL.md), written from the specification for the demo keyboard and the tests:
// a third implementation next to firmware/hid/src/km_stream.c and the model inside tools/magger60_stream.py. It validates like the firmware
// (a refused packet changes nothing), assembles frames in a separate buffer, ends the session by itself after the time-out and answers
// START / STOP / KEEPALIVE and every ACK_REQ. `visible()` is the frame the lights would show (null = the keyboard's own effect).
import { OP, FMT, FLAG_ACK, STATUS, STATE, VERSION, LIGHTS, WINDOW, CMD_STREAM, checksum, seqNewer } from "../core/stream_protocol.js";

const sat = (v) => Math.min(0xffff, v);
const ACK_MIN_MS = 2, BAD_RUN_MAX = 32;

export class StreamSim {
  /** `clock()` -> ms. `enabled`: false = the light core vetoes streams (lights off): START answers UNAVAILABLE. */
  constructor({ clock = () => Date.now(), enabled = true } = {}) {
    this.clock = clock;
    this.enabled = enabled;
    this.session = 0;
    this.state = STATE.IDLE;
    this.last = 0;               // newest accepted sequence number
    this.fmt = FMT.RGB;
    this.timeoutMs = 500;
    this.deadline = 0;
    this.shown = null;           // Uint8Array(183) of the frame on show
    this.shownNo = 0;
    this.asm = null;             // { no, covered: Set, buf }
    this.stats = { committed: 0, dropped: 0, lost: 0, stale: 0, rejected: 0 };
    this.starts = 0;
    this.stops = 0;
    this.lastReply = null;
    this.badRun = 0;
  }

  tick() {
    if (this.state !== STATE.IDLE && this.clock() >= this.deadline) this.#end();
  }
  #end() { this.state = STATE.IDLE; this.session = 0; this.shown = null; this.asm = null; }

  /** The 183-byte frame the lights show, or null when the keyboard shows its own effect. */
  visible() { this.tick(); return this.state === STATE.LIVE ? this.shown : null; }

  setEnabled(on) { this.enabled = on; if (!on) this.#end(); }

  // `limited`: answers to frame chunks (ACK_REQ, errors) are given at most once per 2 ms; answers to control packets never wait
  #reply(seq, op, status, limited = false) {
    const now = this.clock();
    if (limited) {
      if (this.lastReply !== null && now - this.lastReply < ACK_MIN_MS) return null;
      this.lastReply = now;
    }
    const r = new Uint8Array(64);
    const d = new Uint8Array(22);
    const u16 = (i, v) => { d[i] = v & 255; d[i + 1] = v >> 8; };
    d[0] = op; d[1] = status; d[2] = this.state === STATE.IDLE ? 0 : this.session; d[3] = this.state; u16(4, this.last); d[6] = this.shownNo; d[7] = WINDOW;
    u16(8, this.stats.committed); u16(10, this.stats.dropped); u16(12, this.stats.lost); u16(14, this.stats.stale); u16(16, this.stats.rejected);
    d[18] = VERSION; d[19] = 3; u16(20, this.timeoutMs);
    r[0] = 0xaa; r[1] = CMD_STREAM; r[3] = checksum(d.length, seq, d); r[4] = d.length; r[5] = seq & 255; r[6] = seq >> 8;
    r.set(d, 8);
    return r;
  }

  /** One request packet -> the reply report, or null when the keyboard stays silent. */
  process(p) {
    this.tick();
    const n = p[4], seq = p[5] | (p[6] << 8);
    const refuse = (status, control, answer = true) => {
      this.stats.rejected = sat(this.stats.rejected + 1);
      this.badRun = Math.min(255, this.badRun + 1);
      if (this.state !== STATE.IDLE && this.badRun >= BAD_RUN_MAX) this.#end();             // 32 malformed packets in a row end the session
      return answer ? this.#reply(seq, p[8], status, !control) : null;
    };
    if (n < 3 || n > 56) { this.stats.rejected = sat(this.stats.rejected + 1); return null; }
    const [op, session, flags] = p.subarray(8, 11);
    const control = op !== OP.FRAME;
    const answer = control || !!(flags & FLAG_ACK);
    if (op < OP.START || op > OP.KEEPALIVE) return refuse(STATUS.UNKNOWN_OP, true);
    if (p[2] !== 0 || p[7] !== 0 || flags & ~FLAG_ACK) return refuse(STATUS.BAD_ARG, control, answer);
    const body = p.subarray(11, 8 + n);
    switch (op) {
      case OP.START: {
        if (n !== 8) return refuse(STATUS.BAD_LENGTH, true);
        const to = body[2] | (body[3] << 8);
        if (session === 0 || body[0] !== VERSION || body[1] > FMT.GRAY || body[4] !== 0 || (to !== 0 && (to < 100 || to > 5000))) return refuse(STATUS.BAD_ARG, true);
        if (!this.enabled) return this.#reply(seq, op, STATUS.UNAVAILABLE);
        this.stats = { committed: 0, dropped: 0, lost: 0, stale: 0, rejected: 0 };
        this.#end();
        Object.assign(this, { session, state: STATE.OPEN, last: seq, fmt: body[1], timeoutMs: to || 500, shown: null, asm: null, badRun: 0 });
        this.deadline = this.clock() + this.timeoutMs;
        this.starts++;
        return this.#reply(seq, op, STATUS.OK);
      }
      case OP.STOP: case OP.KEEPALIVE: {
        if (this.state === STATE.IDLE || session !== this.session) return refuse(STATUS.SESSION, true);
        if (n !== 3) return refuse(STATUS.BAD_LENGTH, true);
        if (op === OP.STOP) { this.stops++; this.#end(); return this.#reply(seq, op, STATUS.OK); }
        if (!this.#accept(seq)) return this.#reply(seq, op, STATUS.STALE);
        return this.#reply(seq, op, STATUS.OK);
      }
      default: {                                                                              // OP.FRAME
        if (n < 9) return refuse(STATUS.BAD_LENGTH, false, answer);
        if (this.state === STATE.IDLE || session !== this.session) return refuse(STATUS.SESSION, false, answer);
        const bpp = this.fmt === FMT.GRAY ? 1 : 3;
        const [no, first, count] = body;
        if (count === 0 || first >= LIGHTS || first + count > LIGHTS) return refuse(STATUS.BAD_ARG, false, answer);
        if (n !== 6 + count * bpp) return refuse(STATUS.BAD_LENGTH, false, answer);
        if (!this.#accept(seq)) return answer ? this.#reply(seq, op, STATUS.STALE, true) : null;
        if (!this.#chunk(no, first, count, body.subarray(3), bpp)) return answer ? this.#reply(seq, op, STATUS.STALE, true) : null;
        return answer ? this.#reply(seq, op, STATUS.OK, true) : null;
      }
    }
  }

  #accept(seq) {
    if (!seqNewer(seq, this.last)) { this.stats.stale = sat(this.stats.stale + 1); return false; }
    this.stats.lost = sat(this.stats.lost + ((seq - this.last) & 0xffff) - 1);
    this.last = seq;
    this.badRun = 0;
    this.deadline = this.clock() + this.timeoutMs;
    return true;
  }

  #chunk(no, first, count, px, bpp) {
    if (this.asm && this.asm.no !== no) {
      const ahead = (no - this.asm.no) & 255;
      if (ahead >= 1 && ahead <= 127) { this.stats.dropped = sat(this.stats.dropped + 1); this.asm = null; } else { this.stats.stale = sat(this.stats.stale + 1); return false; }
    }
    if (!this.asm) {
      if (this.state === STATE.LIVE) {
        const ahead = (no - this.shownNo) & 255;
        if (ahead === 0 || ahead > 127) { this.stats.stale = sat(this.stats.stale + 1); return false; }   // the frame on show, or an older one
      }
      this.asm = { no, covered: new Set(), buf: new Uint8Array(LIGHTS * 3) };
    }
    for (let i = 0; i < count; i++) {
      const o = (first + i) * 3;
      if (bpp === 3) this.asm.buf.set(px.subarray(i * 3, i * 3 + 3), o); else this.asm.buf.fill(px[i], o, o + 3);
      this.asm.covered.add(first + i);
    }
    if (this.asm.covered.size === LIGHTS) {
      this.shown = this.asm.buf; this.shownNo = no; this.asm = null; this.state = STATE.LIVE; this.stats.committed = sat(this.stats.committed + 1);
    }
    return true;
  }
}
