// Demo keyboard so the whole configurator works without hardware. It is a KmfwDevice (the same client code, the same safety
// rules, the same error paths) connected to a SimKeyboard (sim_keyboard.js) instead of a USB device: settings are staged,
// committed with the confirmation word, read back and compared exactly as with a real keyboard. The simulated "flash"
// (the settings blob) is kept in localStorage so the demo remembers what was saved. The depth stream comes from a small
// finger model (auto typing and/or the PC keyboard).
//
// The simulated firmware reports a settings area of 8192 bytes (the real one answers 8192 since settings format version 2) and stores whatever km_settings would accept.
import { encodeScene, defaultScene } from "../core/lightscene_codec.js";
import { KmfwDevice } from "./KmfwDevice.js";
import { SimKeyboard } from "./sim_keyboard.js";
import { VID, PID } from "./ids.js";
import { MODEL_NAME } from "../config.js";
import { usageFromEventCode } from "../core/keycodes.js";
import { MAX_TRAVEL_RAW, SLOT_COUNT } from "../core/units.js";
import { FW_SETTINGS } from "../core/limits.js";
import { decodeSettings, encodeSettings } from "../core/settings_codec.js";
import { defaultWorkspace, workspaceToBlob } from "../core/profile_codec.js";
import { rawCodec } from "../core/raw_codec.js";
import { loadJson, saveJson } from "../storage.js";

const TICK_MS = 1000 / 120;
const ease = (p) => 1 - Math.pow(1 - p, 3);
const toHex = (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
const fromHex = (h) => Uint8Array.from(h.match(/../g) ?? [], (x) => parseInt(x, 16));

export class MockDevice extends KmfwDevice {
  kind = "mock";
  readOnly = false;
  capabilities = { live: true, readProfile: true, writeProfile: true };
  /** Auto "typing" so the Live page looks alive. Toggle from the UI. */
  demo = !(typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches);
  /** The simulated keyboard (exposed so tests and the demo controls can reach its knobs: busy, unplugged, ...). */
  sim;
  #layout;
  #listeners = new Set();
  #timer = 0;
  #fingers = new Map(); // slot -> {phase, t0, dur, from, to, hold}
  #pos = new Float32Array(SLOT_COUNT);
  #out = new Uint16Array(SLOT_COUNT);
  #nextAuto = 0;
  #onKeyDown = (e) => this.#hostKey(e, true);
  #onKeyUp = (e) => this.#hostKey(e, false);

  /** `flash`: the settings blob the simulated keyboard starts with (default: what was saved before, else the factory settings). `lights`: the simulated keyboard has the lights (feature bit 6); false = a keyboard with older firmware. */
  constructor(layout, { flash = null, persist = true, health = true, lights = true } = {}) {
    super({ hidApi: undefined, settingsSize: FW_SETTINGS.size });
    this.#layout = layout;
    this.attachSettingsCodec(rawCodec);
    this.capabilities = { live: true, readProfile: true, writeProfile: true }; // the simulated keyboard can do all of it (before and after connect)
    const saved = persist ? loadJson("mock-flash") : null;
    let blob = flash;
    if (!blob && typeof saved === "string") {
      try { blob = fromHex(saved); blob = encodeSettings(decodeSettings(blob)); } catch { blob = null; } // a blob saved by an older version (format 1) is migrated
    }
    blob ??= workspaceToBlob(defaultWorkspace(layout), layout).bytes;
    const sceneSavedHex = persist ? loadJson("mock-scene") : null;
    const sceneSaved = typeof sceneSavedHex === "string" && sceneSavedHex.length >= 2 ? fromHex(sceneSavedHex) : null; // the light scene the demo "flash" kept
    this.sim = new SimKeyboard({
      blob, active: blob[8], validate: (b) => { try { decodeSettings(b); return true; } catch { return false; } },
      depthFn: (slot) => this.#out[slot] ?? 0, onCommit: persist ? (b) => saveJson("mock-flash", toHex(b)) : null,
      scope: true, health, maxSettings: 8192, build: "Jan  1 2026,00:00:00KMFW-DEMO",
      // the demo keyboard speaks protocol 2 (device status, light scene, stream, host lock lights: bits 0..10); "older firmware" = protocol 1, no lights
      protoVersion: lights ? 2 : 1, features: lights ? 0xfff : 0x3f,
      scene: sceneSaved ?? encodeScene(defaultScene()), onSceneCommit: persist ? (b) => saveJson("mock-scene", toHex(b)) : null,
    });
    this.info = {
      productName: `${MODEL_NAME} (demo)`, firmwareVersion: "demo", build: "Simulated keyboard, no hardware attached",
      serial: "DEMO-0001", vid: VID, pid: PID,
    };
  }

  async connect() {
    if (this.status === "connected") return;
    this.sim.unplugged = false;
    await this.connectTransport(this.sim);
    this._setInfo({ productName: `${MODEL_NAME} (demo)`, serial: "DEMO-0001", vid: VID, pid: PID });
    this.#timer = setInterval(() => this.#tick(), TICK_MS);
    window.addEventListener("keydown", this.#onKeyDown);
    window.addEventListener("keyup", this.#onKeyUp);
  }

  async disconnect() {
    clearInterval(this.#timer);
    window.removeEventListener("keydown", this.#onKeyDown);
    window.removeEventListener("keyup", this.#onKeyUp);
    this.#fingers.clear();
    this.#pos.fill(0);
    await super.disconnect();
  }

  /** Forget the saved settings: the demo keyboard goes back to factory settings. */
  resetFlash() {
    this.sim.blob = workspaceToBlob(defaultWorkspace(this.#layout), this.#layout).bytes;
    this.sim.active = 0;
    this.sim.snapValid = false;
    saveJson("mock-flash", toHex(this.sim.blob));
  }

  /** Programmatic key press, used by the demo and available to the UI (depth in raw 0.01 mm). */
  press(slot, depth = MAX_TRAVEL_RAW, downMs = 70, holdMs = 90, upMs = 80) {
    const now = performance.now();
    this.#fingers.set(slot, { phase: "down", t0: now, dur: downMs, from: this.#pos[slot], to: depth, hold: holdMs, up: upMs });
  }

  #hostKey(e, down) {
    if (e.repeat || e.metaKey || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target?.tagName)) return;
    const usage = usageFromEventCode(e.code);
    const key = usage === null ? null : this.#layout.byUsage.get(usage);
    if (!key) return;
    const now = performance.now();
    if (down) {
      this.#fingers.set(key.slot, { phase: "down", t0: now, dur: 75, from: this.#pos[key.slot], to: MAX_TRAVEL_RAW, hold: Infinity, up: 85 });
    } else {
      const f = this.#fingers.get(key.slot);
      if (f) this.#fingers.set(key.slot, { ...f, phase: "up", t0: now, from: this.#pos[key.slot], to: 0, dur: f.up });
    }
  }

  #autoType(now) {
    if (!this.demo || now < this.#nextAuto) return;
    const letters = this.#layout.keys.filter((k) => (k.usage >= 4 && k.usage <= 29) || k.usage === 44);
    const k = letters[Math.floor(Math.random() * letters.length)];
    const partial = Math.random() < 0.45; // sometimes a light touch, to show analog travel
    const depth = partial ? 60 + Math.random() * 200 : MAX_TRAVEL_RAW;
    this.press(k.slot, depth, 60 + Math.random() * 60, 40 + Math.random() * 140, 50 + Math.random() * 60);
    this.#nextAuto = now + 110 + Math.random() * 420;
  }

  #tick() {
    const now = performance.now();
    this.#autoType(now);
    for (const [slot, f] of this.#fingers) {
      const p = Math.min(1, (now - f.t0) / f.dur);
      this.#pos[slot] = f.from + (f.to - f.from) * ease(p);
      if (p < 1) continue;
      if (f.phase === "down") {
        this.#fingers.set(slot, { ...f, phase: "hold", t0: now, dur: f.hold });
      } else if (f.phase === "hold") {
        if (f.hold !== Infinity) this.#fingers.set(slot, { ...f, phase: "up", t0: now, from: this.#pos[slot], to: 0, dur: f.up });
      } else {
        this.#pos[slot] = 0;
        this.#fingers.delete(slot);
      }
    }
    for (let s = 0; s < SLOT_COUNT; s++) {
      const v = this.#pos[s];
      this.#out[s] = v <= 0 ? 0 : Math.max(0, Math.min(MAX_TRAVEL_RAW, Math.round(v + (Math.random() - 0.5) * 1.6)));
    }
    // like the real keyboard, the simulated one refuses to write flash while a key is held down (here: a key held on the PC keyboard)
    this.sim.busy = [...this.#fingers.values()].some((f) => f.hold === Infinity && f.phase !== "up");
  }
}
