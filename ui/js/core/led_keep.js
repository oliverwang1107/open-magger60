// Keeps the keyboard's lights in step with the page AND stores them in the keyboard's flash. Built on LedLink (live changes while a
// slider is dragged, one LED_SET on the wire at a time) and the extended LED_GET / LED_SET (core/led_ex.js):
//
//   * every change is sent live without SAVE (never touches flash);
//   * `saveDelayMs` after the LAST change (the person stopped dragging) ONE LED_SET with SAVE goes out, then the extended LED_GET is
//     polled until the keyboard says the save is no longer pending; the firmware coalesces and rate-limits the real flash write;
//   * leaving the page with a change not saved yet sends the SAVE at once (dispose()).
//
// keeper.save is what the page shows:
//   { kind: "saved",  restored }   the keyboard holds exactly these values (restored: they were also read from flash at boot)
//   { kind: "saving" }             a change is on its way to flash (waiting for the person to stop, sent, or waiting for the keyboard)
//   { kind: "failed", error }      the keyboard could not write (status bit) or did not answer; retrySave() tries again
//   { kind: "none", reason }       nothing can be kept: "old" (firmware without the extended form), "nopersist" (build without flash storage)
// A SAVE is never sent to a build that does not report the PERSISTENT status bit (it would answer UNSUPPORTED and apply nothing).
//
// Per key profile (options `profile`, `live`; the keyboard must have features.lightProfiles): the keeper edits the lights of ONE profile.
//   live = true   that profile is the one the keyboard is in: changes go out live (the keyboard shows them) and are saved to that profile;
//   live = false  another profile: nothing goes out live (the keyboard shows its own profile); the change is only saved, to that profile.
// keeper.ownLights tells whether the profile has lights of its own or still follows the shared ones.
// No DOM. All waiting goes through the injected sleep / timer so tests run fast.
import { LedLink } from "./led_link.js";
import { ledStatusFlags, LED_PROFILE_ACTIVE } from "./led_ex.js";
import { ST } from "../device/config_protocol.js";

const defaultSleep = (ms) => new Promise((r) => setTimeout(r, ms));
// answers that mean "this firmware does not know the extended form" (anything else is a real failure and is shown as one)
const isRefusal = (err) => !!err && (err.status === ST.BAD_LENGTH || err.status === ST.UNSUPPORTED || err.status === ST.BAD_ARG || err.status === ST.UNKNOWN_OP);
const SIX = ["effect", "brightness", "speed", "r", "g", "b"];
const pickSix = (s) => Object.fromEntries(SIX.map((k) => [k, s[k]]));

export class LedKeeper {
  #dev;          // { readLights, writeLights, readLightsEx?, writeLightsEx? } (a KmfwDevice)
  #cfg;
  #sleep;
  #onChange;
  #link;
  #mode = null;          // "ext" | "plain" (decided by the first read)
  #flags = ledStatusFlags(0);
  #dirty = false;        // a change the keyboard has not confirmed as saved
  #seq = 0;              // counts changes; a save only clears #dirty when nothing newer arrived meanwhile
  #timer = null;
  #saving = false;       // a save cycle (send + poll) is running
  #saveError = null;
  #refused = false;      // the keyboard refused SAVE although it said it could keep the values
  #closed = false;

  #profile;      // key profile whose lights are edited (0..3), null = the keyboard has one set of lights
  #live;         // that profile is the keyboard's active one

  constructor(dev, { saveDelayMs = 700, pollMs = 400, giveUpMs = 30000, gapMs = 25, sleepFn = defaultSleep, onChange = () => {}, onError = () => {}, profile = null, live = true } = {}) {
    this.#dev = dev;
    this.#profile = profile;
    this.#live = live;
    this.#cfg = { saveDelayMs, pollMs, giveUpMs };
    this.#sleep = sleepFn;
    this.#onChange = onChange;
    this.#link = new LedLink({
      read: () => this.#read(),
      write: (s) => this.#liveWrite(s),
      onChange: () => this.#onChange(),
      onError: (err) => { onError(err); this.#onChange(); },
      gapMs, sleepFn,
    });
  }

  get mode() { return this.#mode; }
  get profile() { return this.#profile; }
  get live() { return this.#live; }
  /** Per-profile mode: does the profile have lights of its own (true), or does it still follow the shared ones? */
  get ownLights() { return this.#profile === null ? true : this.#flags.profileOwn; }
  /** What the page shows: the six light fields (+ idleS when the keyboard has the extended form). */
  get state() { return this.#link.state; }
  get loaded() { return this.#link.loaded; }
  get pending() { return this.#link.pending; }
  /** The live-link error (a LED_SET that did not go through), null if none. */
  get error() { return this.#link.error; }
  /** Status bits of the keyboard as of its last reply: { pending, failed, restored, idleOff, persistent }. */
  get flags() { return this.#flags; }
  get hasIdle() { return this.#mode === "ext"; }
  get canSave() { return this.#mode === "ext" && this.#flags.persistent && !this.#refused; }

  get save() {
    if (this.#mode !== "ext") return { kind: "none", reason: "old" };
    if (!this.canSave) return { kind: "none", reason: "nopersist" };
    if (this.#saveError) return { kind: "failed", error: this.#saveError };
    if (this.#dirty || this.#saving || this.#flags.pending) return { kind: "saving" };
    if (this.#flags.failed) return { kind: "failed", error: null };
    return { kind: "saved", restored: this.#flags.restored };
  }

  async load() {
    await this.#link.load();
    if (this.canSave && this.#flags.pending) { this.#dirty = true; this.#cycle(false); } // a save from before is still on its way
    return this.state;
  }

  /** After a live-change error: send the wanted state again. */
  retry() { this.#link.retry(); }

  /** Change something (a partial state). `idleS` is ignored on firmware without the extended form. */
  set(patch) {
    if (this.#closed) return;
    if (!this.#live && !this.canSave) return;        // another profile can only be edited by saving: nothing to do when that is not possible
    if (this.#mode !== "ext" && "idleS" in patch) { const { idleS: _drop, ...rest } = patch; patch = rest; }
    this.#link.set(patch);
    if (this.canSave) {
      this.#dirty = true;
      this.#seq++;
      this.#saveError = null;
      this.#arm(this.#cfg.saveDelayMs);
    }
  }

  /** Send the SAVE again after a failure. */
  retrySave() {
    if (this.#closed || !this.canSave) return;
    this.#saveError = null;
    this.#dirty = true;
    this.#seq++;
    this.#arm(0);
  }

  /** The page goes away. A change that was not saved yet is sent now (fire and forget); nothing else happens afterwards. */
  dispose() {
    if (this.#closed) return;
    clearTimeout(this.#timer);
    this.#timer = null;
    const want = this.#link.state;
    const owed = this.#dirty && this.canSave && !!want && !this.#saving;
    this.#closed = true;
    this.#link.close();
    if (owed) Promise.resolve().then(() => this.#dev.writeLightsEx(want, { idleS: want.idleS, save: true, profile: this.#profile })).catch(() => {});
  }

  // ---- the wire ----

  async #read() {
    if (this.#dev.readLightsEx) {
      try {
        const ex = await this.#dev.readLightsEx(this.#profile);
        this.#mode = "ext";
        return this.#absorb(ex);
      } catch (err) {
        if (!isRefusal(err)) throw err;
      }
    }
    this.#mode = "plain";
    return this.#dev.readLights();
  }

  async #liveWrite(s) {
    if (!this.#live) return { ...s };                 // another profile: the keyboard is not showing it, there is nothing to send now
    if (this.#mode !== "ext") return this.#dev.writeLights(pickSix(s));
    // the live change goes to "the profile in force" (255): it can not miss if the page's idea of the active profile is a moment old
    return this.#absorb(await this.#dev.writeLightsEx(pickSix(s), { idleS: s.idleS, save: false, profile: this.#profile === null ? null : LED_PROFILE_ACTIVE }));
  }

  // keep the status bits, hand the page the light fields + idle time
  #absorb(ex) {
    this.#flags = ledStatusFlags(ex.status);
    return { ...pickSix(ex), idleS: ex.idleS };
  }

  // ---- saving ----

  #arm(delay) {
    clearTimeout(this.#timer);
    this.#timer = setTimeout(() => { this.#timer = null; this.#fire(); }, delay);
    this.#onChange();
  }

  #fire() {
    if (this.#closed) return;
    if (this.#link.pending || this.#saving) { this.#arm(40); return; } // a live change is still on the wire / a cycle is running: wait
    this.#cycle(true);
  }

  async #cycle(send) {
    this.#saving = true;
    const seq = this.#seq;
    this.#onChange();
    try {
      if (send) {
        const want = this.#link.state;
        this.#absorb(await this.#dev.writeLightsEx(want, { idleS: want.idleS, save: true, profile: this.#profile }));
      }
      let waited = 0;
      while (!this.#closed && this.#flags.pending) {
        if (waited >= this.#cfg.giveUpMs) throw Object.assign(new Error("the keyboard did not finish saving in time"), { code: "save_timeout" });
        await this.#sleep(this.#cfg.pollMs);
        waited += this.#cfg.pollMs;
        if (this.#link.pending) continue; // a live change is on the wire; look again afterwards
        this.#absorb(await this.#dev.readLightsEx(this.#profile));
      }
      if (seq === this.#seq) this.#dirty = false; // nothing newer was asked for meanwhile (otherwise the re-armed timer sends again)
    } catch (err) {
      if (err && err.status === ST.UNSUPPORTED) { this.#refused = true; this.#dirty = false; } else this.#saveError = err;
    } finally {
      this.#saving = false;
      if (!this.#closed) this.#onChange();
    }
  }
}
