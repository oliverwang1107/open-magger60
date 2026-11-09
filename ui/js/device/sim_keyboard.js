// A keyboard that follows CONFIG_PROTOCOL.md (and the scope channel of km_scope.h), written independently of the real firmware
// (firmware/hid/src/km_config.c) and of the Python tool: a third implementation of the same specification. It is the
// "hardware" of the demo mode (MockDevice talks to it through exactly the same client code as to a real keyboard) and the
// counterpart the node tests run against. Everything the real keyboard refuses, it refuses with the same status codes.
import {
  OP, ST, GET_PAGE, SET_PAGE, MAX_DEPTHS, MAX_SLOTS, MAX_SCENE, crc32, le16, le32, rd16, rd32, checksum, concat, assertSendable,
  CMD_SCOPE, SCOPE_PAGE_VALUES, LED_EFFECT_MAX,
} from "./config_protocol.js";
import { TimeoutError } from "./hid_transport.js";
import { FW_SETTINGS } from "../core/limits.js";
import { LED_DEFAULT } from "../core/led.js";
import { StreamSim } from "./stream_sim.js";

const frame = (cmd, data, offset = 0) => {
  const r = new Uint8Array(64);
  r[0] = 0xaa; r[1] = cmd; r[3] = checksum(data.length, offset, data); r[4] = data.length;
  r[5] = offset & 255; r[6] = (offset >> 8) & 255; r[7] = (offset >> 16) & 255;
  r.set(data, 8);
  return r;
};
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const word = (a, w) => w.split("").every((c, i) => a[i] === c.charCodeAt(0));
const SETTINGS_MAGIC = [0x4b, 0x4d, 0x43, 0x53];

export class SimKeyboard {
  /**
   * stock: answer like the stock firmware ("unknw" to everything but the version query).
   * blob/profiles/active/slots: the settings and shape the keyboard holds.
   * validate(blob) -> true | false: the firmware's validator (COMMIT answers INVALID when it says no).
   * depthFn(slot) -> 0.01 mm: live depths. onCommit(blob): called after every accepted COMMIT (the mock saves its "flash" there).
   * scope: publish scope snapshots (engine diagnostics, depths); health: also the T-E tags 0x0300/0x0301.
   * scene/devStatus: protocol 2 (CONFIG_PROTOCOL.md): the live light scene blob and the 16 DEVICE_STATUS bytes. Both are answered only when the
   * protocol version is 2 and the feature bit (0x80 device status, 0x100 scene) is set (UNSUPPORTED without the bit, UNKNOWN_OP before protocol 2).
   * protoVersion/features/maxSettings: what GET_INFO reports. Feature bit 6 (0x40) = the lights: LED_GET / LED_SET answer only then
   * (UNSUPPORTED otherwise, like a build without them); `led` is the state they start from (default: the firmware's own).
   */
  constructor({
    stock = false, blob = Uint8Array.from({ length: 100 }, (_, j) => (j * 3 + 1) & 255), profiles = 4, active = 2, slots = 76,
    validate = null, depthFn = null, onCommit = null, scope = false, health = false, protoVersion = 2, features = 0x7bf, maxSettings = 8192,
    build = null, led = LED_DEFAULT, scene = Uint8Array.from({ length: 50 }, (_, j) => (j * 5 + 2) & 255),
    devStatus = Uint8Array.of(2, 1, 0, 0x09, 2, 1, 2, 0, 0x34, 0x12, 3, 0, 7, 0, 0, 0), onSceneCommit = null, validateScene = null,
  } = {}) {
    this.stock = stock;
    this.build = build ?? (stock ? "Jan  1 2026,00:00:00STOCK-SIM" : "Jan  1 2026,00:00:00KMFW-SIM");
    this.blob = Uint8Array.from(blob);
    Object.assign(this, { profiles, active, slots, validate, depthFn, onCommit, scope, health, protoVersion, features, maxSettings });
    this.seen = []; // [cmd, op] of every packet that arrived
    this.flashWrites = 0;
    this.led = { ...led }; // RAM only, like the real thing: never touches blob / flashWrites / commits
    this.ledSets = 0;
    // lights per key profile (feature bits 6 + 11): the stored entries of the four profiles (null = follows the shared fallback, which is the built-in default here)
    this.ledIdle = 600;
    this.ledStored = [null, null, null, null];
    this.ledSaves = 0;
    this.recal = 0; this.saveCal = 0;
    this.commits = 0;
    this.state = 0; this.total = 0; this.next = 0; this.stage = new Uint8Array(this.maxSettings); this.last = null;
    this.snap = new Uint8Array(0); this.snapId = 0; this.snapValid = false;
    // protocol 2: the scene has its own staging and snapshot, like the settings
    this.scene = Uint8Array.from(scene); this.devStatus = Uint8Array.from(devStatus); this.onSceneCommit = onSceneCommit; this.validateScene = validateScene;
    this.sceneWrites = 0; this.sceneCommits = 0;
    this.scState = 0; this.scTotal = 0; this.scNext = 0; this.scStage = new Uint8Array(MAX_SCENE); this.scLast = null;
    this.scSnap = new Uint8Array(0); this.scSnapId = 0; this.scSnapValid = false;
    this.dropRequest = new Set(); // exchange numbers (1-based) whose request is lost
    this.dropReply = new Set();   // exchange numbers whose reply is lost (the request is processed)
    this.busy = false;            // true: COMMIT / SELECT_PROFILE / RECALIBRATE / SAVE_CALIBRATION answer BUSY (a key is down)
    this.unplugged = false;       // true: every exchange fails like a removed USB device
    this.unplugAfter = null;      // unplug when this many exchanges have been seen
    this.scopeRequests = 0;
    this.calibrating = 0;         // scope reads left until a started calibration "finishes"
    this.calResult = 0;           // km_engine_cal_result_t
    this.calAttempts = 1;
    this.calSaved = false;
    this.reports = 0;
    this.up = Date.now();
    // the light frame stream (command 0xC7, feature bit 9, protocol 2); `streamClock` lets a test drive the time-out
    this.streamClock = () => Date.now();
    this.stream = new StreamSim({ clock: () => this.streamClock() });
    this.streamListeners = new Set();
  }

  /** Stream transport interface (see device/hid_transport.js): fire and forget, replies go to the listeners. */
  async send(packet) {
    this.#checkPlug();
    assertSendable(packet);
    this.seen.push([packet[1], null]);
    const out = this.process(packet);
    if (out) queueMicrotask(() => this.streamListeners.forEach((cb) => cb(out)));
  }

  onStreamReport(cb) {
    this.streamListeners.add(cb);
    return () => this.streamListeners.delete(cb);
  }

  depth(slot) { return this.depthFn ? this.depthFn(slot) : 1000 + 7 * slot; }

  /** Transport interface for ConfigClient. */
  async exchange(packet) {
    this.#checkPlug();
    assertSendable(packet);
    this.seen.push([packet[1], packet[1] === 0xc6 ? packet[8] : null]);
    const n = this.seen.length;
    if (this.dropRequest.has(n)) throw new TimeoutError();
    const out = this.process(packet);
    if (this.dropReply.has(n)) throw new TimeoutError();
    return out;
  }

  #checkPlug() {
    if (this.unplugAfter !== null && this.seen.length >= this.unplugAfter) this.unplugged = true;
    if (this.unplugged) {
      const e = new Error("The device was disconnected.");
      e.name = "NetworkError";
      throw e;
    }
  }

  process(packet) {
    if (packet[1] === 3) return frame(3, concat(Uint8Array.of(0x04, 0x01), Uint8Array.from(this.build, (c) => c.charCodeAt(0))));
    if (this.stock) return frame(packet[1], Uint8Array.from("unknw", (c) => c.charCodeAt(0)));
    if (packet[1] === CMD_SCOPE) return this.#scope(packet);
    if (packet[1] === 0xc7) return this.protoVersion >= 2 && (this.features & 0x200) ? this.stream.process(packet) : frame(0xc7, Uint8Array.from("unknw", (c) => c.charCodeAt(0)));
    const d = packet.subarray(8, 8 + packet[4]);
    if (d.length < 2) return frame(0xc6, Uint8Array.of(d.length ? d[0] : 0, 0, ST.BAD_LENGTH));
    const [op, token] = d;
    const a = d.subarray(2);
    const done = (status, payload = new Uint8Array(0)) => frame(0xc6, concat(Uint8Array.of(op, token, status), payload));
    switch (op) {
      case OP.GET_INFO:
        if (a.length) return done(ST.BAD_LENGTH);
        return done(0, concat(Uint8Array.of(this.protoVersion, this.profiles, this.active), le16(this.maxSettings), Uint8Array.of(GET_PAGE, SET_PAGE),
          le16(this.features), Uint8Array.of(this.state), le16(this.total), le16(this.next), le32(this.commits),
          this.protoVersion >= 2 ? le16(this.features & 0x100 ? MAX_SCENE : 0) : new Uint8Array(0)));
      case OP.GET_SETTINGS: {
        if (a.length !== 4) return done(ST.BAD_LENGTH);
        const off = rd16(a), want = a[2], sid = a[3];
        if (want === 0 || want > GET_PAGE || (off === 0 && sid !== 0)) return done(ST.BAD_ARG);
        if (off === 0) { this.snap = Uint8Array.from(this.blob); this.snapValid = true; this.snapId = (this.snapId % 255) + 1; }
        else {
          if (!this.snapValid || sid !== this.snapId) return done(ST.STALE);
          if (off > this.snap.length) return done(ST.RANGE);
        }
        return done(0, concat(Uint8Array.of(this.snapId), le16(this.snap.length), le32(crc32(this.snap)), le16(off), this.snap.subarray(off, off + want)));
      }
      case OP.SET_BEGIN: {
        if (a.length !== 2) return done(ST.BAD_LENGTH);
        const total = rd16(a);
        if (total === 0) return done(ST.BAD_ARG);
        if (total > this.maxSettings) return done(ST.TOO_BIG);
        Object.assign(this, { state: 1, total, next: 0, last: null });
        this.stage = new Uint8Array(this.maxSettings);
        return done(0, le16(this.maxSettings));
      }
      case OP.SET_PAGE: {
        if (a.length < 3) return done(ST.BAD_LENGTH);
        const off = rd16(a), data = a.slice(2);
        if (this.state === 0) return done(ST.NOT_STAGED);
        if (this.last && this.last.off === off && eq(this.last.data, data)) return done(0, le16(this.next));
        if (off !== this.next) return done(ST.SEQUENCE, le16(this.next));
        if (off + data.length > this.total) return done(ST.RANGE, le16(this.next));
        this.stage.set(data, off);
        this.last = { off, data };
        this.next = off + data.length;
        if (this.next === this.total) this.state = 2;
        return done(0, le16(this.next));
      }
      case OP.COMMIT: {
        if (a.length !== 10) return done(ST.BAD_LENGTH);
        if (!word(a, "SAVE")) return done(ST.NEED_CONFIRM);
        if (this.state !== 2 || this.next !== this.total) return done(ST.NOT_STAGED);
        const staged = this.stage.slice(0, this.total);
        if (rd16(a, 4) !== this.total || rd32(a, 6) !== crc32(staged)) return done(ST.BAD_CRC);
        if (this.validate && !this.validate(staged)) return done(ST.INVALID);
        if (this.busy) return done(ST.BUSY);
        this.blob = staged;
        if (this.#isSettings(this.blob)) { this.active = this.blob[8]; this.#showStoredLights(); }
        this.flashWrites++; this.commits++;
        Object.assign(this, { state: 0, total: 0, next: 0, last: null, snapValid: false });
        this.onCommit?.(this.blob);
        return done(0, le32(this.commits));
      }
      case OP.SELECT_PROFILE:
        if (a.length !== 5) return done(ST.BAD_LENGTH);
        if (!word(a, "PROF")) return done(ST.NEED_CONFIRM);
        if (a[4] >= this.profiles) return done(ST.RANGE);
        if (this.busy) return done(ST.BUSY);
        this.active = a[4]; this.snapValid = false; this.#showStoredLights();
        if (this.#isSettings(this.blob)) {
          const b = Uint8Array.from(this.blob);
          b[8] = this.active;
          b.set(le32(crc32(b.subarray(0, b.length - 4))), b.length - 4);
          this.blob = b;
          this.onCommit?.(this.blob);
        }
        return done(0, Uint8Array.of(this.active));
      case 7: // RECALIBRATE_REQUEST and SAVE_CALIBRATION_REQUEST
      case 8:
        if (a.length !== 4) return done(ST.BAD_LENGTH);
        if (!word(a, op === 7 ? "RCAL" : "SCAL")) return done(ST.NEED_CONFIRM);
        if (this.busy) return done(ST.BUSY);
        if (op === 7) { this.recal++; this.calibrating = 12; this.calAttempts++; this.calSaved = false; } else { this.saveCal++; this.calSaved = true; }
        return done(0);
      case OP.LIVE_DEPTH: {
        if (a.length !== 2) return done(ST.BAD_LENGTH);
        const [first, count] = a;
        if (count === 0 || count > MAX_DEPTHS) return done(ST.BAD_ARG);
        if (first + count > MAX_SLOTS || first + count > this.slots) return done(ST.RANGE);
        return done(0, concat(Uint8Array.of(first, count), ...Array.from({ length: count }, (_, i) => le16(this.depth(first + i)))));
      }
      case OP.LED_GET:
      case OP.LED_SET: {
        if (!(this.features & 0x40)) return done(ST.UNSUPPORTED);
        const b = this.led;
        const now = () => Uint8Array.of(b.effect, b.brightness, b.speed, b.r, b.g, b.b);
        if ((this.features & 0x800) && ((op === OP.LED_GET && (a.length === 1 || a.length === 2)) || (op === OP.LED_SET && (a.length === 9 || a.length === 10)))) return this.#ledProfileForm(op, a, done);
        if (op === OP.LED_GET) return a.length ? done(ST.BAD_LENGTH) : done(0, now());
        if (a.length !== 6) return done(ST.BAD_LENGTH);
        if (a[0] > LED_EFFECT_MAX || a[2] === 0) return done(ST.RANGE);
        [b.effect, b.brightness, b.speed, b.r, b.g, b.b] = a;
        this.ledSets++;
        return done(0, now());
      }
      case OP.DEVICE_STATUS:
      case OP.SCENE_GET:
      case OP.SCENE_BEGIN:
      case OP.SCENE_PAGE:
      case OP.SCENE_COMMIT:
        return this.#protocol2(op, a, done);
      default:
        return done(ST.UNKNOWN_OP);
    }
  }

  // ---- lights per key profile: the extended forms of LED_GET / LED_SET (an independent model of m5_led.c) ----
  #lightsOf(p) {
    if (p === this.active) return { ...this.led, idleS: this.ledIdle, own: !!this.ledStored[p] };
    const s = this.ledStored[p];
    return s ? { ...s, own: true } : { ...LED_DEFAULT, idleS: 600, own: false };
  }

  #showStoredLights() {
    if (!(this.features & 0x800)) return;
    const s = this.ledStored[this.active] ?? { ...LED_DEFAULT, idleS: 600 };
    const { idleS, ...six } = s;
    this.led = { ...LED_DEFAULT, ...six };
    this.ledIdle = idleS ?? 600;
  }

  #ledProfileForm(op, a, done) {
    const prof = op === OP.LED_GET ? (a.length === 2 ? a[1] : 255) : (a.length === 10 ? a[9] : 255);
    if (op === OP.LED_GET && a[0] !== 1) return done(ST.BAD_ARG);
    if (prof !== 255 && prof >= this.profiles) return done(ST.BAD_ARG);
    const p = prof === 255 ? this.active : prof;
    const reply = () => {
      const l = this.#lightsOf(p);
      return done(0, Uint8Array.of(l.effect, l.brightness, l.speed, l.r, l.g, l.b, l.idleS & 255, l.idleS >> 8, 16 | (l.own ? 32 : 0), 0, this.ledSaves & 255, 0));
    };
    if (op === OP.LED_GET) return reply();
    if (a[8] & ~1) return done(ST.BAD_ARG);
    if (a[0] > LED_EFFECT_MAX || a[2] === 0) return done(ST.RANGE);
    const save = a[8] === 1;
    if (p !== this.active && !save) return done(ST.RANGE);
    const st = { effect: a[0], brightness: a[1], speed: a[2], r: a[3], g: a[4], b: a[5], idleS: a[6] | (a[7] << 8) };
    if (p === this.active) { const { idleS, ...six } = st; this.led = six; this.ledIdle = idleS; this.ledSets++; }
    if (save) { this.ledStored[p] = st; this.ledSaves++; }
    return reply();
  }

  // ---- protocol 2: DEVICE_STATUS and the light scene (an independent model of km_config.c) ----
  #protocol2(op, a, done) {
    if (this.protoVersion < 2) return done(ST.UNKNOWN_OP);
    if (op === OP.DEVICE_STATUS) {
      if (a.length) return done(ST.BAD_LENGTH);
      return this.features & 0x80 ? done(0, Uint8Array.from(this.devStatus)) : done(ST.UNSUPPORTED);
    }
    const has = !!(this.features & 0x100);
    switch (op) {
      case OP.SCENE_GET: {
        if (a.length !== 4) return done(ST.BAD_LENGTH);
        const off = rd16(a), want = a[2], sid = a[3];
        if (want === 0 || want > GET_PAGE || (off === 0 && sid !== 0)) return done(ST.BAD_ARG);
        if (!has) return done(ST.UNSUPPORTED);
        if (off === 0) { this.scSnap = Uint8Array.from(this.scene); this.scSnapValid = true; this.scSnapId = (this.scSnapId % 255) + 1; }
        else {
          if (!this.scSnapValid || sid !== this.scSnapId) return done(ST.STALE);
          if (off > this.scSnap.length) return done(ST.RANGE);
        }
        return done(0, concat(Uint8Array.of(this.scSnapId), le16(this.scSnap.length), le32(crc32(this.scSnap)), le16(off), this.scSnap.subarray(off, off + want)));
      }
      case OP.SCENE_BEGIN: {
        if (a.length !== 2) return done(ST.BAD_LENGTH);
        const total = rd16(a);
        if (total === 0) return done(ST.BAD_ARG);
        if (total > MAX_SCENE) return done(ST.TOO_BIG);
        if (!has) return done(ST.UNSUPPORTED);
        Object.assign(this, { scState: 1, scTotal: total, scNext: 0, scLast: null });
        this.scStage = new Uint8Array(MAX_SCENE);
        return done(0, le16(MAX_SCENE));
      }
      case OP.SCENE_PAGE: {
        if (a.length < 3) return done(ST.BAD_LENGTH);
        const off = rd16(a), data = a.slice(2);
        if (this.scState === 0) return done(ST.NOT_STAGED);
        if (this.scLast && this.scLast.off === off && eq(this.scLast.data, data)) return done(0, le16(this.scNext));
        if (off !== this.scNext) return done(ST.SEQUENCE, le16(this.scNext));
        if (off + data.length > this.scTotal) return done(ST.RANGE, le16(this.scNext));
        this.scStage.set(data, off);
        this.scLast = { off, data };
        this.scNext = off + data.length;
        if (this.scNext === this.scTotal) this.scState = 2;
        return done(0, le16(this.scNext));
      }
      default: { // SCENE_COMMIT
        if (a.length !== 10) return done(ST.BAD_LENGTH);
        if (!word(a, "LITE")) return done(ST.NEED_CONFIRM);
        if (!has) return done(ST.UNSUPPORTED);
        if (this.scState !== 2 || this.scNext !== this.scTotal) return done(ST.NOT_STAGED);
        const staged = this.scStage.slice(0, this.scTotal);
        if (rd16(a, 4) !== this.scTotal || rd32(a, 6) !== crc32(staged)) return done(ST.BAD_CRC);
        if (this.validateScene && !this.validateScene(staged)) return done(ST.INVALID);
        if (this.busy) return done(ST.BUSY);
        this.scene = staged;
        this.sceneWrites++; this.sceneCommits++;
        Object.assign(this, { scState: 0, scTotal: 0, scNext: 0, scLast: null, scSnapValid: false });
        this.onSceneCommit?.(this.scene);
        return done(0, le32(this.sceneCommits));
      }
    }
  }

  // a settings blob of format 2 (fixed size) or format 3 (sections: the length is in the header); both keep the active profile at byte 8 and the CRC last
  #isSettings(b) {
    if (b.length < 20 || !SETTINGS_MAGIC.every((v, i) => b[i] === v)) return false;
    const version = rd16(b, 4);
    return b.length === FW_SETTINGS.size || (version === 3 && rd16(b, 6) === b.length);
  }

  // ---- scope channel: one snapshot published at a time, rotating ----

  #tags() {
    const t = [0x0200, 0x0201];
    if (this.health) t.push(0x0300, 0x0301);
    return t;
  }

  #snapshot(tag) {
    const v = new Array(tag === 0x0201 ? this.slots : tag === 0x0301 ? 8 : 24).fill(0);
    const pressed = [];
    for (let s = 0; s < this.slots; s++) if (this.depth(s) > 120) pressed.push(s);
    if (tag === 0x0201) for (let s = 0; s < this.slots; s++) v[s] = this.depth(s);
    if (tag === 0x0200) {
      if (this.calibrating > 0) this.calibrating--;
      v[0] = this.calibrating > 0 ? 0xffff : this.calResult;
      v[1] = this.calAttempts; v[2] = pressed.length; v[5] = 3300; v[6] = 3900; v[7] = this.reports & 0xffff; v[8] = this.reports >>> 16;
      pressed.slice(0, 6).forEach((s, i) => { v[12 + i] = s; });
      for (let i = pressed.length; i < 6; i++) v[12 + i] = 0xff;
      v[18] = this.calibrating > 0 ? 0 : 1;
      const frames = Math.floor((Date.now() - this.up) * 2.3);
      v[19] = frames & 0xffff; v[20] = (frames >>> 16) & 0xffff;
    }
    if (tag === 0x0300) {
      const frames = Math.floor((Date.now() - this.up) * 2.3);
      v[0] = 0x20 << 8; v[1] = frames & 0xffff; v[2] = (frames >>> 16) & 0xffff; v[6] = 3300; v[15] = this.calAttempts;
      v[21] = 1; v[22] = Math.floor((Date.now() - this.up) / 1000) & 0xffff; v[20] = 0xffff;
    }
    if (tag === 0x0301) {
      for (const s of pressed) v[s >> 4] |= 1 << (s & 15);
      v[5] = this.scopeRequests & 0xffff;
    }
    return v;
  }

  #scope(p) {
    if (!this.scope) return frame(CMD_SCOPE, Uint8Array.from("unknw", (c) => c.charCodeAt(0)));
    this.scopeRequests++;
    const tags = this.#tags();
    const idx = Math.floor(this.scopeRequests / 6) % tags.length; // the published tag changes every few requests
    const tag = tags[idx];
    const start = p[5], want = p[6] | (p[7] << 8), room = p[4];
    const vals = this.#snapshot(tag);
    const seq = Math.floor(this.scopeRequests / 6) + 1;
    const header = concat(le16(tag), le16(seq & 0xffff || 1), le16(vals.length));
    if (room < 8 || (want !== 0 && want !== tag)) return frame(CMD_SCOPE, header, start);
    const n = Math.min(SCOPE_PAGE_VALUES, (room - 6) >> 1, Math.max(0, vals.length - start));
    return frame(CMD_SCOPE, concat(header, ...vals.slice(start, start + n).map(le16)), start);
  }
}

/** A fake WebHID device in front of a SimKeyboard: sendReport() answers with an input report event. */
export class FakeHid extends EventTarget {
  constructor(sim) {
    super();
    this.sim = sim;
    this.opened = false;
    this.productName = "Magger60 (fake)";
    this.vendorId = 0x19f5;
    this.productId = 0xfb49;
    this.sent = [];
  }
  async open() { this.opened = true; }
  async close() { this.opened = false; }
  async sendReport(id, data) {
    if (id !== 0) throw new Error("report id must be 0");
    const packet = Uint8Array.from(data);
    this.sent.push(packet);
    const n = this.sim.seen.length + 1;
    if (this.sim.unplugAfter !== null && this.sim.seen.length >= this.sim.unplugAfter) this.sim.unplugged = true;
    if (this.sim.unplugged) { const e = new Error("The device was disconnected."); e.name = "NetworkError"; throw e; }
    this.sim.seen.push([packet[1], packet[1] === 0xc6 ? packet[8] : null]);
    if (this.sim.dropRequest.has(n)) return;
    const out = this.sim.process(packet);
    if (this.sim.dropReply.has(n)) return;
    setTimeout(() => {
      const ev = new Event("inputreport");
      ev.data = new DataView(out.buffer, out.byteOffset, out.byteLength);
      this.dispatchEvent(ev);
    }, 0);
  }
}

/** A fake navigator.hid. */
export class FakeHidApi extends EventTarget {
  constructor(devices) { super(); this.devices = devices; }
  async requestDevice() { return this.devices; }
  async getDevices() { return this.devices; }
}
