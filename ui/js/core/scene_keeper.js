// Keeps the page's light scene and the keyboard's in step: reads the scene from the keyboard, remembers what the keyboard holds, and writes
// the working scene back with the P8 scene operations (KmfwDevice.writeScene: SCENE_BEGIN, SCENE_PAGE x n, SCENE_COMMIT "LITE", read back and
// compare). Every write is a flash write on the keyboard (a sector erase), so "live" mode waits until the person has stopped changing things
// (`liveDelayMs`), never has two writes in flight, and sends ONE write for any number of changes made meanwhile (coalescing). A write the
// keyboard answers with BUSY (a key is down) is tried again a little later.
//
//   keeper.save (what the page shows):
//     { kind: "saved" }                         the keyboard holds exactly the working scene (compared byte for byte)
//     { kind: "pending" }                       the working scene differs from the keyboard's (nothing is on its way)
//     { kind: "saving", step }                  a write is running / waiting for the person to stop (live); step: "wait" | "stage" | "commit" | "verify" | "busy"
//     { kind: "failed", error }                 the last write did not work; the keyboard still has its previous scene (or an unknown one after a lost reply)
//     { kind: "none", reason }                  nothing can be written: "offline" | "stock" | "old" (firmware without light scene operations)
// keeper.onKeyboard: { kind: "ok" } | { kind: "unreadable", error } (the keyboard answered but the blob is not a valid KMLS) | { kind: "unknown" } (not read yet).
// No DOM. All waiting goes through the injected sleep.
import { encodeScene, decodeScene, defaultScene, cloneScene, LightSceneError } from "./lightscene_codec.js";
import { WORD, ST } from "../device/config_protocol.js";

const defaultSleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sameBytes = (a, b) => !!a && !!b && a.length === b.length && a.every((v, i) => v === b[i]);

/** Can the scene be used with this device? "ok" | "offline" | "stock" (other firmware) | "old" (our firmware, no light scene operations). */
export function sceneAvailability(device) {
  if (!device || device.status !== "connected") return "offline";
  if (device.kind === "webhid") return "stock";
  return device.features?.scene ? "ok" : "old";
}

export class SceneKeeper {
  #dev;
  #cfg;
  #sleep;
  #onChange;
  #scene = defaultScene();
  #deviceBytes = null;     // what the keyboard held when we last read / wrote it
  #onKeyboard = { kind: "unknown" };
  #live = false;
  #seq = 0;                // counts changes
  #timer = null;
  #running = false;
  #step = null;
  #error = null;
  #closed = false;
  #loaded = false;

  constructor(dev, { liveDelayMs = 900, busyRetryMs = 1500, busyRetries = 6, sleepFn = defaultSleep, onChange = () => {} } = {}) {
    this.#dev = dev;
    this.#cfg = { liveDelayMs, busyRetryMs, busyRetries };
    this.#sleep = sleepFn;
    this.#onChange = onChange;
  }

  get availability() { return sceneAvailability(this.#dev); }
  get loaded() { return this.#loaded; }
  /** The working scene (do not mutate; use set()). */
  get scene() { return this.#scene; }
  get onKeyboard() { return this.#onKeyboard; }
  get live() { return this.#live; }
  get busy() { return this.#running; }

  /** True when the working scene is not what the keyboard holds. */
  get pending() {
    if (!this.#deviceBytes) return true;
    try { return !sameBytes(encodeScene(this.#scene), this.#deviceBytes); } catch { return true; }
  }

  get save() {
    const a = this.availability;
    if (a !== "ok") return { kind: "none", reason: a };
    if (this.#running || this.#timer !== null) return { kind: "saving", step: this.#step ?? "wait" };
    if (this.#error) return { kind: "failed", error: this.#error };
    return this.pending ? { kind: "pending" } : { kind: "saved" };
  }

  /** Read the scene from the keyboard. A blob that does not decode leaves the default scene in place and says so in onKeyboard. */
  async load() {
    if (this.availability !== "ok") throw new Error(`light scene not available: ${this.availability}`);
    const bytes = await this.#dev.readScene();
    this.#deviceBytes = Uint8Array.from(bytes);
    try {
      this.#scene = decodeScene(bytes);
      this.#onKeyboard = { kind: "ok" };
    } catch (err) {
      if (!(err instanceof LightSceneError)) throw err;
      this.#scene = defaultScene();
      this.#onKeyboard = { kind: "unreadable", error: err };
      this.#deviceBytes = null;                   // the keyboard's content is not a scene we know: the default one is "pending"
    }
    this.#error = null;
    this.#loaded = true;
    this.#changed();
    return this.#scene;
  }

  /** Replace the working scene (an edit, a preset, an import). Throws LightSceneError when the scene is not one the keyboard would take. */
  set(scene) {
    encodeScene(scene);                           // refuse an invalid scene before it becomes the working copy
    this.#scene = cloneScene(scene);
    this.#seq++;
    this.#error = null;
    if (this.#live && this.availability === "ok") this.#schedule();
    this.#changed();
  }

  /** Live mode: every change is written to the keyboard after the person has stopped for `liveDelayMs`. */
  setLive(on) {
    this.#live = !!on;
    if (!this.#live) this.#cancelTimer();
    else if (this.pending && this.availability === "ok" && this.#loaded) this.#schedule();
    this.#changed();
  }

  #cancelTimer() {
    if (this.#timer !== null) { clearTimeout(this.#timer); this.#timer = null; }
  }

  #schedule() {
    this.#cancelTimer();
    this.#step = "wait";
    this.#timer = setTimeout(() => { this.#timer = null; this.#pump(); }, this.#cfg.liveDelayMs);
  }

  /** Write now (the "write to keyboard" button, or retry). Resolves when the write cycle has ended; check .save. */
  async saveNow() {
    this.#cancelTimer();
    this.#error = null;
    await this.#pump();
  }

  retry() { return this.saveNow(); }

  // One writer at a time; changes that arrive during a write are picked up by the loop, so any number of them cost one more write.
  async #pump() {
    if (this.#running || this.#closed || this.availability !== "ok") return;
    this.#running = true;
    try {
      for (;;) {
        const seq = this.#seq;
        let blob;
        try { blob = encodeScene(this.#scene); } catch (err) { this.#error = err; break; }
        if (sameBytes(blob, this.#deviceBytes)) { if (seq === this.#seq) break; else continue; }
        try {
          await this.#writeWithBusyRetry(blob);
        } catch (err) {
          this.#error = err;
          this.#deviceBytes = null;               // after a failed write the keyboard's content is not known for sure
          break;
        }
        this.#deviceBytes = blob;
        this.#error = null;
        if (seq === this.#seq) break;             // nothing changed while writing; otherwise go round again with the newer scene
      }
    } finally {
      this.#running = false;
      this.#step = null;
      this.#changed();
    }
  }

  async #writeWithBusyRetry(blob) {
    for (let attempt = 0; ; attempt++) {
      try {
        this.#dev.armWrites(WORD.COMMIT);         // arms exactly one write
        await this.#dev.writeScene(blob, { onStep: (s) => { this.#step = s; this.#changed(); } });
        return;
      } catch (err) {
        if (err?.status === ST.BUSY && attempt < this.#cfg.busyRetries && !this.#closed) {
          this.#step = "busy";
          this.#changed();
          await this.#sleep(this.#cfg.busyRetryMs);
          continue;
        }
        throw err;
      }
    }
  }

  /** Read the scene back from the keyboard and compare with the working copy: { same, unreadable } (updates what the keeper knows). */
  async compare() {
    const bytes = await this.#dev.readScene();
    const same = (() => { try { return sameBytes(encodeScene(this.#scene), Uint8Array.from(bytes)); } catch { return false; } })();
    let unreadable = false;
    try { decodeScene(bytes); } catch { unreadable = true; }
    this.#deviceBytes = unreadable ? null : Uint8Array.from(bytes);
    this.#changed();
    return { same, unreadable };
  }

  /** Stop. A live write that was only waiting for the person to stop is sent now rather than lost. */
  dispose() {
    const flush = this.#timer !== null;
    this.#cancelTimer();
    if (flush) this.#pump().finally(() => { this.#closed = true; });
    else this.#closed = true;
  }

  #changed() { if (!this.#closed) this.#onChange(); }
}
