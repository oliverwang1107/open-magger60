// Real keyboard running OUR firmware (build string "KMFW..."), over the configuration protocol (CONFIG_PROTOCOL.md).
// Device implementation next to MockDevice (simulator, a subclass of this one) and WebHidDevice (read-only version query for any firmware).
//
//   - Reading is always available: protocol info, the settings blob, live depths, diagnostics.
//   - Writing is two-step on purpose. `armWrites("SAVE")` arms exactly ONE write; writeSettings()/writeProfile() consume the
//     arming whether they succeed or fail, so a stray call can never write. `selectProfile(i, "PROF")`, `recalibrate("RCAL")`
//     and `saveCalibration("SCAL")` each carry their own word. The words are also sent inside the request, where the firmware
//     checks them again. The UI asks the person first, then calls these.
//   - The settings are an opaque blob here (the firmware's settings module owns the format; core/settings_codec.js reads it).
//     readProfile/writeProfile work only after a codec is attached: attachSettingsCodec({ decode(blob) -> [profile...],
//     encode(blob, index, profile) -> blob }).
//   - The lights (LED_GET / LED_SET, feature bit 6) are RAM-only on the keyboard and need no arming or word: readLights() / writeLights().
//   - Optional features are detected at run time and reported in `features`: scope diagnostics (tag 0x0200), the stability
//     counters (tag 0x0300) and the pressed-key mask (tag 0x0301) exist only in firmware images that publish them.
import { Device, UnsupportedError } from "./Device.js";
import { VID, PID, USAGE_PAGE, USAGE } from "./ids.js";
import { MODEL_NAME } from "../config.js";
import { ConfigClient } from "./ConfigClient.js";
import { ScopeClient, SCOPE_TAG, parseEngineDiag, parseHealth, parseKeyMask } from "./scope_client.js";
import { WebHidTransport } from "./hid_transport.js";
import { FEAT, WORD, ConfigError } from "./config_protocol.js";
import { classifyProblem } from "./errors.js";

export class NotArmedError extends Error {
  constructor(what = "writing") {
    super(`${what} to the keyboard is not armed: call armWrites("${WORD.COMMIT}") first (it arms one write)`);
    this.name = "NotArmedError";
  }
}

const NO_FEATURES = Object.freeze({ settings: false, selectProfile: false, recalibrate: false, saveCalibration: false, live: false, lights: false, lightProfiles: false, deviceStatus: false, scene: false, stream: false, hostLeds: false, diag: false, health: false, keyMask: false });
const SLOTS = 76;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now());

export class KmfwDevice extends Device {
  kind = "kmfw";
  readOnly = true; // false only while a write is armed
  capabilities = { live: false, readProfile: false, writeProfile: false };
  /** What this keyboard + firmware can do, filled in on connect. */
  features = { ...NO_FEATURES };
  /** {kind, detail} of the last failure, see errors.js; null while all is well. */
  problem = null;
  config = null; // ConfigClient once connected
  scope = null; // ScopeClient once connected
  /** Live polling rate limit in Hz (the real device is limited by USB round trips, the simulator by this). */
  liveHz = 50;
  #hid = null;
  #transport = null;
  #codec = null;
  #armed = false;
  #busy = false; // a settings write is running: live polling pauses
  #listeners = new Set();
  #liveRun = 0;
  #hidApi;
  #settingsSize;

  /**
   * `hidApi` defaults to navigator.hid; tests pass a fake with requestDevice/getDevices/addEventListener.
   * `settingsSize`: the size of the settings blob this configurator works with (FW_SETTINGS.size). When given, a keyboard whose
   * settings area is smaller is refused on connect (problem kind "settings_size"); null = no check.
   */
  constructor({ hidApi = typeof navigator !== "undefined" ? navigator.hid : undefined, settingsSize = null } = {}) {
    super();
    this.#hidApi = hidApi;
    this.#settingsSize = settingsSize;
  }

  static get supported() {
    return typeof navigator !== "undefined" && "hid" in navigator;
  }

  attachSettingsCodec(codec) {
    this.#codec = codec;
    this.#updateCaps();
  }

  async connect() {
    if (!this.#hidApi) {
      const err = new UnsupportedError("WebHID is not available in this browser");
      this.#fail(err);
      throw err;
    }
    this._setStatus("connecting");
    try {
      const granted = await this.#hidApi.requestDevice({ filters: [{ vendorId: VID, productId: PID, usagePage: USAGE_PAGE, usage: USAGE }] });
      if (!granted.length) {
        const e = new Error("no device chosen");
        e.name = "NotFoundError";
        throw e;
      }
      await this.connectWith(granted[0]);
    } catch (err) {
      this.#fail(err);
      throw err;
    }
  }

  /** Open an already chosen HIDDevice and verify the firmware (refuses anything that is not KMFW). */
  async connectWith(hid) {
    try {
      if (!hid.opened) await hid.open();
      this.#hid = hid;
      this.#hidApi?.addEventListener?.("disconnect", this.#onDisconnect);
      this._setInfo({ productName: hid.productName || MODEL_NAME, vid: hid.vendorId ?? VID, pid: hid.productId ?? PID });
      await this.connectTransport(new WebHidTransport(hid));
    } catch (err) {
      await this.#closeHid();
      this.#fail(err);
      throw err;
    }
  }

  /** Talk over any transport with exchange(packet) (WebHID, or the simulator): verify, check, publish. */
  async connectTransport(transport) {
    this.problem = null;
    this._setStatus("connecting");
    this.#transport = transport;
    this.config = new ConfigClient(transport);
    this.scope = new ScopeClient(transport, () => this.config.verified);
    try {
      await this.config.verify();
      const info = this.config.info;
      if (this.#settingsSize && (info.features & FEAT.READ) && info.maxSettings < this.#settingsSize) {
        throw new ConfigError(`the keyboard's settings area holds ${info.maxSettings} bytes, this configurator's settings are ${this.#settingsSize}`, null, "settings_size");
      }
      this.#publishInfo();
      this.#updateCaps();
      await this.#probeOptional();
      this._setStatus("connected");
    } catch (err) {
      this.config = null;
      this.scope = null;
      this.#fail(err);
      throw err;
    }
  }

  /**
   * The light frame stream's port: { send(packet), onReport(cb) -> off }. Needs feature bit 9 (protocol 2) and a verified KMFW
   * keyboard; every packet is checked again on the exact bytes by the transport (assertSendable). The stream is RAM only and ends by itself.
   */
  streamPort() {
    const tr = this.#transport;
    if (!this.features.stream || !tr?.send || !this.config?.verified) throw new UnsupportedError("this keyboard has no light stream");
    return { send: (p) => tr.send(p), onReport: (cb) => tr.onStreamReport(cb) };
  }

  #fail(err) {
    this.problem = classifyProblem(err);
    this.features = { ...NO_FEATURES };
    this._setStatus("error", err);
  }

  #publishInfo() {
    this._setInfo({ firmwareVersion: this.config.version, build: this.config.build, serial: null });
  }

  #updateCaps() {
    const f = this.config?.info?.features ?? 0;
    this.capabilities = {
      live: !!(f & FEAT.LIVE),
      readProfile: !!(f & FEAT.READ) && !!this.#codec,
      writeProfile: !!(f & FEAT.READ) && !!(f & FEAT.WRITE) && !!this.#codec,
    };
    this.features = {
      ...this.features,
      settings: !!(f & FEAT.READ) && !!(f & FEAT.WRITE), selectProfile: !!(f & FEAT.PROFILE), recalibrate: !!(f & FEAT.RECAL),
      saveCalibration: !!(f & FEAT.SAVE_CAL), live: !!(f & FEAT.LIVE), lights: !!(f & FEAT.LED), lightProfiles: !!(f & FEAT.LED) && !!(f & FEAT.LED_PROFILE) && this.config?.info?.protoVersion >= 2,
      // protocol 2 (M6 P8): only a firmware that speaks it can announce these
      deviceStatus: !!(f & FEAT.DEVICE_STATUS) && this.config?.info?.protoVersion >= 2, scene: !!(f & FEAT.SCENE) && this.config?.info?.protoVersion >= 2,
      stream: !!(f & FEAT.STREAM) && this.config?.info?.protoVersion >= 2, hostLeds: !!(f & FEAT.HOST_LEDS) && this.config?.info?.protoVersion >= 2,
    };
    this.dispatchEvent(new Event("info"));
  }

  /** Find out which optional scope tags this firmware publishes (each is a few requests; absent ones are simply not offered). */
  async #probeOptional() {
    const probe = async (tag) => { try { await this.scope.snapshot(tag, { tries: 30 }); return true; } catch (err) { if (classifyProblem(err).kind === "lost") throw err; return false; } };
    const diag = await probe(SCOPE_TAG.ENGINE);
    const health = diag ? await probe(SCOPE_TAG.HEALTH) : false;
    const keyMask = health ? await probe(SCOPE_TAG.KEYMASK) : false;
    this.features = { ...this.features, diag, health, keyMask };
    this.dispatchEvent(new Event("info"));
  }

  #onDisconnect = (ev) => {
    if (ev.device === this.#hid) {
      this.#hid = null;
      this.#stopLive();
      this.#disarm();
      this.problem = { kind: "lost", detail: "The keyboard was unplugged." };
      this._setStatus("disconnected");
    }
  };

  async #closeHid() {
    this.#hidApi?.removeEventListener?.("disconnect", this.#onDisconnect);
    const hid = this.#hid;
    this.#hid = null;
    try { await hid?.close(); } catch { /* already gone */ }
  }

  async disconnect() {
    this.#stopLive();
    this.#disarm();
    await this.#closeHid();
    this.config = null;
    this.scope = null;
    this.#transport = null;
    this._setStatus("disconnected");
  }

  async getInfo() {
    if (!this.config) return this.info;
    await this.config.getInfo();
    this.#publishInfo();
    this.#updateCaps();
    return this.info;
  }

  /** The profile the keyboard says is active (GET_INFO), or null. */
  get activeProfile() { return this.config?.info?.activeProfile ?? null; }

  // ---- reading ----

  /** The live settings blob (Uint8Array). */
  async readSettings() {
    return this.#guard(() => this.#client().dump());
  }

  async readProfile(index) {
    if (!this.#codec) throw new UnsupportedError("the settings codec is not attached yet, so profiles cannot be decoded");
    const profiles = this.#codec.decode(await this.readSettings());
    if (!Number.isInteger(index) || index < 0 || index >= profiles.length) throw new RangeError("profile slot out of range");
    return profiles[index];
  }

  /** Engine diagnostics (scope tag 0x0200) parsed, or null when this firmware does not publish them. */
  async readEngineDiag() {
    if (!this.features.diag) return null;
    return parseEngineDiag((await this.#guard(() => this.scope.snapshot(SCOPE_TAG.ENGINE))).values);
  }

  /** Stability counters (scope tag 0x0300) parsed, or null when this firmware does not publish them. */
  async readHealth() {
    if (!this.features.health) return null;
    return parseHealth((await this.#guard(() => this.scope.snapshot(SCOPE_TAG.HEALTH))).values);
  }

  /** The slots the keyboard is reporting as pressed (scope tag 0x0301), or null. */
  async readKeyMask() {
    if (!this.features.keyMask) return null;
    return parseKeyMask((await this.#guard(() => this.scope.snapshot(SCOPE_TAG.KEYMASK))).values);
  }

  // ---- writing (armed, one write at a time) ----

  /** Arm exactly one write. The word must be "SAVE". */
  armWrites(word) {
    if (word !== WORD.COMMIT) throw new ConfigError(`arming needs the word "${WORD.COMMIT}"`);
    this.#armed = true;
    this.readOnly = false;
  }

  #disarm() {
    this.#armed = false;
    this.readOnly = true;
  }

  get armed() { return this.#armed; }

  /**
   * Stage `blob`, commit it (flash), read it back and compare. Consumes the arming. Returns the commit counter.
   * Failures are ConfigErrors with a `code` (busy, crc, timeout, ...) or a status from the keyboard; the keyboard keeps its
   * old settings unless the COMMIT itself was accepted.
   */
  async writeSettings(blob, { onStep = () => {} } = {}) {
    if (!this.#armed) throw new NotArmedError("writing settings");
    this.#disarm();
    const c = this.#client();
    this.#busy = true;
    try {
      return await this.#guard(async () => {
        onStep("stage");
        await c.stage(blob);
        onStep("commit");
        const n = await c.commit(blob, WORD.COMMIT);
        onStep("verify");
        const back = await c.dump();
        if (back.length !== blob.length || back.some((b, i) => b !== blob[i])) throw new ConfigError("the settings read back after the commit differ from what was written", null, "readback");
        await this.getInfo();
        return n;
      });
    } finally {
      this.#busy = false;
    }
  }

  async writeProfile(index, profile) {
    if (!this.#codec) throw new UnsupportedError("the settings codec is not attached yet, so profiles cannot be encoded");
    if (!this.#armed) throw new NotArmedError("writing a profile");
    const blob = this.#codec.encode(await this.readSettings(), index, profile);
    return this.writeSettings(blob);
  }

  /** Switch the active profile (persistent on the keyboard). The word must be "PROF". */
  async selectProfile(index, word) {
    const c = this.#client();
    const active = await this.#guard(() => c.selectProfile(index, word));
    await this.getInfo();
    return active;
  }

  /** Ask the keyboard to start a calibration. The word must be "RCAL"; the keyboard answers BUSY while a key is down. */
  async recalibrate(word) {
    await this.#guard(() => this.#client().requestRecalibration(word));
  }

  /** Ask the keyboard to store the calibration it learned. The word must be "SCAL". */
  async saveCalibration(word) {
    await this.#guard(() => this.#client().requestSaveCalibration(word));
  }

  // ---- lights (LED_GET / LED_SET: RAM only on the keyboard, no confirmation word, nothing staged or saved) ----

  /** The lights now in force {effect, brightness, speed, r, g, b}. Needs features.lights. */
  async readLights() {
    this.#needLights();
    return this.#guard(() => this.#client().ledGet());
  }

  /** Change the lights; returns what the keyboard now has. They are gone at the next restart of the keyboard. */
  async writeLights(state) {
    this.#needLights();
    return this.#guard(() => this.#client().ledSet(state));
  }

  /** Extended lights (idle time + status, see core/led_ex.js). Rejects with a ConfigError whose .status is BAD_LENGTH on firmware without them. */
  async readLightsEx(profile = null) {
    this.#needLights();
    this.#needProfile(profile);
    return this.#guard(() => this.#client().ledGetEx(profile));
  }

  /** Change the lights and the idle time; with `save` the keyboard also keeps them in flash (coalesced by the firmware). */
  async writeLightsEx(state, opts) {
    this.#needLights();
    this.#needProfile(opts?.profile);
    return this.#guard(() => this.#client().ledSetEx(state, opts));
  }

  /** `profile` (0..3, or 255 = the one in force) addresses the lights of one key profile; only for firmware with features.lightProfiles. */
  #needProfile(profile) {
    if (profile !== null && profile !== undefined && !this.features.lightProfiles) throw new UnsupportedError("this firmware keeps one set of lights for all profiles");
  }

  #needLights() {
    if (!this.features.lights) throw new UnsupportedError("this keyboard has no lights (or its firmware is too old to control them)");
  }

  // ---- protocol 2: device status and the light scene (M6 P8) ----

  /** Read-only device state {hostLeds, caps, padModeSetting, padModeActive, usbFellBack, profile, layer, streamState, ...}. Needs features.deviceStatus. */
  async readDeviceStatus() {
    if (!this.features.deviceStatus) throw new UnsupportedError("this firmware does not report the device status");
    return this.#guard(() => this.#client().deviceStatus());
  }

  /** The live light scene blob (KMLS, km_lightscene.h). Needs features.scene. */
  async readScene() {
    if (!this.features.scene) throw new UnsupportedError("this firmware has no light scene storage");
    return this.#guard(() => this.#client().sceneDump());
  }

  /**
   * Stage `blob`, SCENE_COMMIT it (the keyboard applies it and writes its own flash sector), read it back and compare. Same rules as
   * writeSettings(): consumes the arming (armWrites("SAVE")), the scene commit word "LITE" travels inside the request.
   * Returns the scene commit counter (-1 when a lost reply was settled by the read-back).
   */
  async writeScene(blob, { onStep = () => {} } = {}) {
    if (!this.features.scene) throw new UnsupportedError("this firmware has no light scene storage");
    if (!this.#armed) throw new NotArmedError("writing the light scene");
    this.#disarm();
    const c = this.#client();
    this.#busy = true;
    try {
      return await this.#guard(async () => {
        onStep("stage");
        await c.sceneStage(blob);
        onStep("commit");
        const n = await c.sceneCommit(blob, WORD.SCENE_COMMIT);
        onStep("verify");
        const back = await c.sceneDump();
        if (back.length !== blob.length || back.some((b, i) => b !== blob[i])) throw new ConfigError("the scene read back after the commit differs from what was written", null, "readback");
        await this.getInfo();
        return n;
      });
    } finally {
      this.#busy = false;
    }
  }

  // ---- live depth (polling LIVE_DEPTH) ----

  subscribeLive(cb) {
    this.#listeners.add(cb);
    if (this.#listeners.size === 1 && this.capabilities.live) this.#startLive();
    return () => {
      this.#listeners.delete(cb);
      if (this.#listeners.size === 0) this.#stopLive();
    };
  }

  #startLive() {
    const run = ++this.#liveRun;
    const depth = new Uint16Array(SLOTS);
    (async () => {
      while (run === this.#liveRun && this.config) {
        const t0 = now();
        if (this.#busy) { await sleep(20); continue; }
        try {
          await this.config.liveDepth(SLOTS, depth);
        } catch (err) {
          if (run === this.#liveRun) { this.#stopLive(); this.#fail(err); }
          return;
        }
        if (run !== this.#liveRun) return;
        const t = now();
        for (const cb of this.#listeners) cb(depth, t);
        await sleep(Math.max(0, 1000 / this.liveHz - (now() - t0)));
      }
    })();
  }

  #stopLive() {
    this.#liveRun++;
  }

  #client() {
    if (!this.config || this.status !== "connected") throw new ConfigError("the keyboard is not connected", null, "lost");
    return this.config;
  }

  /** Run a device operation; when the keyboard vanished meanwhile, the device goes to "error" with problem.kind "lost". */
  async #guard(fn) {
    try {
      return await fn();
    } catch (err) {
      if (classifyProblem(err).kind === "lost") {
        this.#stopLive();
        this.#disarm();
        this.#fail(err);
      }
      throw err;
    }
  }
}
