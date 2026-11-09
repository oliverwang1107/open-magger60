// What the animation page needs on top of StreamClient: can this device take a stream, and one object that owns the session
// (start / push frames at a steady rate / stop) and reports a plain phase for the page to show. No DOM, no timers of its own:
// the page calls push(frame, nowMs) from its clock.
import { StreamClient } from "./stream_client.js";
import { FMT } from "./stream_protocol.js";

/** "ok" | "offline" (nothing connected) | "stock" (other firmware: its version was read, nothing else) | "old" (our firmware without the stream). */
export function streamAvailability(device) {
  if (!device || device.status !== "connected") return "offline";
  if (device.kind === "webhid") return "stock";
  return device.features?.stream && typeof device.streamPort === "function" ? "ok" : "old";
}

export class StreamController {
  /** `device`: anything with streamPort() (KmfwDevice, MockDevice). fps: how often a frame is sent at most (the lights show 50 at most; 30 is recommended). */
  constructor(device, { fps = 30, timeoutMs = 800, onChange = () => {}, clientOptions = {} } = {}) {
    this.device = device;
    this.minGapMs = 1000 / Math.max(1, Math.min(50, fps));
    this.timeoutMs = timeoutMs;
    this.onChange = onChange;
    this.clientOptions = clientOptions;
    this.client = null;
    /** "off" (the keyboard shows its own lights) | "starting" | "live" | "error" */
    this.phase = "off";
    /** why the phase is "error": "unavailable" (lights off), "silent", "lost" (unplugged / write failed), "unsupported", other status names */
    this.reason = null;
    this.sentAt = -Infinity;
    this.sent = 0;
  }

  #set(phase, reason = null) {
    this.phase = phase;
    this.reason = reason;
    this.onChange(this);
  }

  get stats() { return this.client?.stats ?? null; }
  /** The keyboard's own report of itself (STATE.IDLE / OPEN / LIVE and counters), from the newest reply; null before the first. */
  get keyboard() { return this.client?.last ?? null; }

  async start() {
    if (this.phase === "starting" || this.phase === "live") return;
    this.#set("starting");
    let port;
    try { port = this.device.streamPort(); } catch { this.#set("error", "unsupported"); return; }
    this.client?.dispose();
    this.client = new StreamClient(port, { fmt: FMT.RGB, timeoutMs: this.timeoutMs, onChange: () => this.onChange(this), ...this.clientOptions });
    try {
      await this.client.start();
      this.sentAt = -Infinity;
      if (this.phase === "starting") this.#set("live");
    } catch (err) {
      if (this.phase === "off") return;                            // stopped while starting
      this.#set("error", err.code && err.code !== "bad_reply" ? err.code : "lost");
    }
  }

  /** End the stream: STOP is sent, the keyboard shows its own lights again at once (and by itself after the time-out if the STOP is lost). */
  async stop() {
    const c = this.client;
    this.#set("off");
    this.client = null;
    if (!c) return;
    try { await c.stop(); } finally { c.dispose(); }
  }

  /** Send `frame` (Uint8Array(183)) if the stream is live and the last frame went out long enough ago. Never waits; returns true when a frame went out. */
  push(frame, nowMs) {
    const c = this.client;
    if (this.phase !== "live" || !c || nowMs - this.sentAt < this.minGapMs) return false;
    this.sentAt = nowMs;
    c.sendFrame(frame).then((r) => { if (r === "sent") this.sent++; }).catch((err) => {
      if (this.phase === "live") this.#set("error", err.code === "silent" ? "silent" : "lost");
    });
    return true;
  }

  dispose() {
    const c = this.client;
    this.client = null;
    this.phase = "off";
    if (c) c.stop().finally(() => c.dispose());
  }
}
