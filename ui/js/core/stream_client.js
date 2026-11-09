// One light-stream session from the browser (STREAM_PROTOCOL.md sections 4, 5 and 9): START, frames with flow control, keep-alives, STOP.
// The same procedure as tools/magger60_stream.py, event driven: replies arrive through port.onReport(cb), requests go out through
// port.send(packet) (device.streamPort() on a real keyboard). Nothing here waits for a frame to be answered, and a frame that does not fit
// the window is skipped, never queued, so a slow or silent keyboard can never make the page pile packets up.
import {
  OP, FMT, STATUS, STATE, FLAG_ACK, WINDOW, ACK_EVERY, StreamError, buildStart, buildStop, buildKeepalive, buildFrameChunk, frameChunks,
  parseReply, seqNewer, statusName,
} from "./stream_protocol.js";

const rnd = (n) => (typeof crypto !== "undefined" && crypto.getRandomValues ? crypto.getRandomValues(new Uint32Array(1))[0] % n : Math.floor(Math.random() * n));

export class StreamClient {
  /**
   * port: { send(packet) -> Promise, onReport(cb) -> off }. timeoutMs: how long the keyboard waits in silence before it goes back to its
   * own effect (100..5000; the keyboard's default is 500). Every field is injectable for tests.
   */
  constructor(port, { fmt = FMT.RGB, timeoutMs = 800, session = null, seq0 = null, ackEvery = ACK_EVERY, replyWaitMs = 150, onChange = () => {} } = {}) {
    this.port = port;
    this.fmt = fmt;
    this.timeoutMs = timeoutMs;
    this.session = session ?? 1 + rnd(255);
    this.seq = (seq0 ?? rnd(0x10000)) & 0xffff;
    this.acked = this.seq;
    this.window = WINDOW;
    this.ackEvery = ackEvery;
    this.replyWaitMs = replyWaitMs;
    this.sinceAck = 0;
    this.frameNo = 0;
    this.onChange = onChange;
    /** "idle" (no session) | "starting" | "live" (session open on the keyboard) | "error" */
    this.state = "idle";
    this.error = null;
    /** The newest reply (parseReply) and the keyboard's own state (STATE.*) from it. */
    this.last = null;
    this.stats = { frames: 0, skipped: 0, packets: 0, acks: 0, restarts: 0, keepalives: 0, errors: 0 };
    this.needRestart = false;
    this.probing = false;
    this.sending = false;
    this.waiters = new Map();
    this.off = port.onReport((r) => this.#onReport(r));
  }

  #set(state, error = null) {
    this.state = state;
    this.error = error;
    this.onChange(this);
  }

  #nextSeq() { this.seq = (this.seq + 1) & 0xffff; return this.seq; }

  async #write(packet) {
    this.stats.packets++;
    try { await this.port.send(packet); } catch (err) { this.#set("error", err); throw err; }
  }

  #onReport(raw) {
    let rep;
    try { rep = parseReply(raw); } catch { this.stats.errors++; return; }
    this.last = rep;
    this.stats.acks++;
    if (rep.session === this.session && seqNewer(rep.lastSeq, this.acked)) this.acked = rep.lastSeq;
    if (rep.window) this.window = rep.window;
    if (this.state === "live" && (rep.state === STATE.IDLE || (rep.session !== this.session && rep.state !== STATE.IDLE))) this.needRestart = true;
    const w = this.waiters.get(`${rep.op}:${rep.seq}`);
    if (w) { this.waiters.delete(`${rep.op}:${rep.seq}`); w(rep); }
    this.onChange(this);
  }

  /** Send a control packet and wait for its reply (same op and sequence number); resend the same packet when the keyboard stays silent. */
  async #request(packet, op, tries = 3) {
    const seq = packet[5] | (packet[6] << 8);
    for (let i = 0; i < tries; i++) {
      const key = `${op}:${seq}`;
      const answer = new Promise((resolve) => {
        this.waiters.set(key, resolve);
        setTimeout(() => { if (this.waiters.get(key) === resolve) { this.waiters.delete(key); resolve(null); } }, this.replyWaitMs);
      });
      await this.#write(packet);
      const rep = await answer;
      if (rep) return rep;
    }
    throw new StreamError(`the keyboard did not answer (op ${op})`, "silent");
  }

  /** Open the session. Throws StreamError: "unavailable" (lights off), "silent", or the status name in lower case. */
  async start() {
    this.#set("starting");
    this.acked = this.seq;                                          // START carries the base sequence number
    try {
      const rep = await this.#request(buildStart(this.session, this.seq, this.fmt, this.timeoutMs), OP.START);
      if (rep.status !== STATUS.OK) throw new StreamError(`START refused: ${statusName(rep.status)}`, rep.status === STATUS.UNAVAILABLE ? "unavailable" : statusName(rep.status).toLowerCase());
    } catch (err) {
      this.#set("error", err);
      throw err;
    }
    this.needRestart = false;
    this.sinceAck = 0;
    this.#set("live");
    return this.last;
  }

  async restart() {
    this.stats.restarts++;
    this.#nextSeq();
    return this.start();
  }

  /** Close the session politely. The keyboard would drop the stream by itself after the time-out anyway. */
  async stop() {
    if (this.state !== "live" && this.state !== "starting") { this.#set("idle"); return; }
    this.#set("idle");
    try { await this.#request(buildStop(this.session, this.#nextSeq()), OP.STOP, 2); } catch { /* the time-out ends it */ }
  }

  /** The keyboard answers a KEEPALIVE always: it keeps a still picture alive and is the "status" query. */
  async keepalive() {
    this.stats.keepalives++;
    return this.#request(buildKeepalive(this.session, this.#nextSeq()), OP.KEEPALIVE);
  }

  unanswered() { return (this.seq - this.acked) & 0xffff; }

  /**
   * Send one frame (Uint8Array(183) r,g,b per light, or 61 gray bytes in the gray format).
   * Resolves "sent" | "skipped" (the keyboard is behind; nothing was queued) | "busy" (the previous call is still writing).
   */
  async sendFrame(rgb) {
    if (this.state !== "live") throw new StreamError("no stream session", "not_live");
    if (this.sending) return "busy";
    this.sending = true;
    try {
      if (this.needRestart) await this.restart();
      const chunks = frameChunks(rgb, this.fmt);
      if (this.unanswered() + chunks.length > this.window) {
        this.stats.skipped++;
        if (this.unanswered() >= this.window && !this.probing) {  // silent keyboard: ask with a control packet that is always answered
          this.probing = true;
          this.keepalive().catch(() => {}).finally(() => { this.probing = false; });
        }
        return "skipped";
      }
      this.frameNo = (this.frameNo + 1) & 0xff;
      for (const c of chunks) {
        this.sinceAck++;
        const ack = this.sinceAck >= this.ackEvery;
        if (ack) this.sinceAck = 0;
        await this.#write(buildFrameChunk(this.session, this.#nextSeq(), this.frameNo, c.first, c.count, c.raw, ack ? FLAG_ACK : 0));
      }
      this.stats.frames++;
      return "sent";
    } finally {
      this.sending = false;
    }
  }

  /** Stop listening (does not send STOP: call stop() first). */
  dispose() {
    this.off?.();
    this.off = null;
    this.waiters.clear();
  }
}
