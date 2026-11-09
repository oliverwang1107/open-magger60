// Client of the configuration protocol (command 0xC6, CONFIG_PROTOCOL.md): the browser twin of ConfigClient in
// tools/magger60_config.py. Transport-agnostic: `transport.exchange(packet: Uint8Array) -> Promise<Uint8Array>` sends one
// 64-byte packet and resolves with the matching reply; it rejects with an error whose name is "TimeoutError" when
// nothing came back (the client then repeats the same request, which every op tolerates).
//
// The client proves the firmware is ours (cmd 3 build string with KMFW) before it builds a single 0xC6 request, and
// nothing outside config_protocol.js's whitelist can be built.
import {
  OP, ST, WORD, MAX_SETTINGS, MAX_SCENE, GET_PAGE, SET_PAGE, MAX_DEPTHS, PROTO_VERSION, MIN_PROTO_VERSION, FEAT, ConfigError, parseDeviceStatus, buildRequest, buildInfoQuery,
  decodeBuildReply, decodeReply, parseInfo, isKmfwBuild, statusName, crc32, le16, le32, rd16, rd32, ascii, concat,
} from "./config_protocol.js";
import { encodeLed, decodeLed } from "../core/led.js";
import { encodeLedEx, decodeLedEx } from "../core/led_ex.js";

const isTimeout = (err) => err?.name === "TimeoutError";

export class ConfigClient {
  #t;
  #retries;
  #token = 0;
  #verified = false;
  #log;
  build = null;
  info = null;

  constructor(transport, { retries = 4, log = () => {} } = {}) {
    this.#t = transport;
    this.#retries = retries;
    this.#log = log;
  }

  get verified() { return this.#verified; }

  /** cmd 3 must say KMFW, then GET_INFO must say protocol version 1 or 2 (2 only adds ops). Returns the parsed info. */
  async verify() {
    const { build, version } = decodeBuildReply(await this.#t.exchange(buildInfoQuery()));
    this.build = build;
    if (!isKmfwBuild(build)) {
      throw new ConfigError(`this keyboard runs '${build}', not our firmware (the build string must carry KMFW); no config command was sent`, null, "stock_firmware");
    }
    this.version = version;
    this.#verified = true;
    this.info = await this.getInfo();
    if (this.info.protoVersion < MIN_PROTO_VERSION || this.info.protoVersion > PROTO_VERSION) {
      this.#verified = false;
      throw new ConfigError(`protocol version ${this.info.protoVersion} is not supported (this client speaks ${MIN_PROTO_VERSION}..${PROTO_VERSION})`, null,
        this.info.protoVersion < MIN_PROTO_VERSION ? "proto_old" : "proto_new");
    }
    return this.info;
  }

  #nextToken() {
    this.#token = (this.#token + 1) & 0xff;
    return this.#token;
  }

  /** One request, its reply. Statuses other than those in `ok` throw ConfigError (with .status). */
  async call(op, args = new Uint8Array(0), ok = [ST.OK]) {
    if (!this.#verified && op !== OP.GET_INFO) throw new ConfigError("firmware not verified yet; call verify() first", null, "not_verified");
    const token = this.#nextToken();
    const packet = buildRequest(op, token, args);
    let last = null;
    for (let attempt = 0; attempt < this.#retries; attempt++) {
      let rep;
      try {
        rep = decodeReply(await this.#t.exchange(packet));
      } catch (err) {
        if (!isTimeout(err)) throw err;
        last = err;
        this.#log(`no reply to op ${op} (attempt ${attempt + 1})`);
        continue;
      }
      if (rep.op !== op || rep.token !== token) { this.#log("stale reply ignored"); continue; }
      if (!ok.includes(rep.status)) throw new ConfigError(`op ${op} refused: ${statusName(rep.status)}`, rep.status);
      return rep;
    }
    throw new ConfigError(`no answer to op ${op} after ${this.#retries} attempts${last ? `: ${last.message}` : ""}`, null, "timeout");
  }

  async getInfo() {
    this.info = parseInfo((await this.call(OP.GET_INFO)).payload);
    return this.info;
  }

  /** The live settings blob (a Uint8Array), read from one consistent snapshot, CRC-checked. */
  async dump(tries = 4) {
    return this.#dump(OP.GET_SETTINGS, Math.min(MAX_SETTINGS, this.info?.maxSettings ?? MAX_SETTINGS), tries);
  }

  async #dump(op, limit, tries) {
    for (let t = 0; t < tries; t++) {
      const blob = [];
      let sid = 0, total = null, crc = null, stale = false;
      while (total === null || blob.length < total) {
        const rep = await this.call(op, concat(le16(blob.length), Uint8Array.of(GET_PAGE, sid)), [ST.OK, ST.STALE]);
        if (rep.status === ST.STALE) { stale = true; break; }
        const p = rep.payload;
        if (p.length < 9) throw new ConfigError("GET_SETTINGS reply too short");
        const [psid, ptotal, pcrc, poff] = [p[0], rd16(p, 1), rd32(p, 3), rd16(p, 7)];
        if (total === null) {
          [sid, total, crc] = [psid, ptotal, pcrc];
          if (total < 1 || total > limit || sid === 0) throw new ConfigError(`nonsense GET_SETTINGS header (total ${total}, id ${sid})`);
        }
        if (psid !== sid || ptotal !== total || pcrc !== crc || poff !== blob.length) { stale = true; break; }
        const data = p.subarray(9);
        if (data.length === 0 || blob.length + data.length > total) throw new ConfigError(`GET_SETTINGS page has ${data.length} bytes at offset ${poff} (total ${total})`);
        blob.push(...data);
      }
      if (stale) continue;
      const out = Uint8Array.from(blob);
      if (crc32(out) !== crc) throw new ConfigError("the settings blob read back does not match its CRC-32", null, "crc");
      return out;
    }
    throw new ConfigError(`could not read a consistent settings snapshot in ${tries} tries`);
  }

  /** SET_BEGIN + SET_PAGEs: fills the keyboard's RAM staging buffer, nothing else. */
  async stage(blob) {
    const info = this.info ?? (await this.getInfo());
    await this.#stage(blob, OP.SET_BEGIN, OP.SET_PAGE, info.maxSettings);
    const inf = await this.getInfo();
    if (inf.stageState !== 2 || inf.stageTotal !== blob.length || inf.stageNext !== blob.length) {
      throw new ConfigError("staging is not complete after the last page");
    }
  }

  async #stage(blob, begin, pageOp, limit) {
    const info = this.info ?? (await this.getInfo());
    if (blob.length < 1 || blob.length > limit) throw new ConfigError(`the blob must be 1..${limit} bytes, not ${blob.length}`);
    await this.call(begin, le16(blob.length));
    const page = Math.min(SET_PAGE, info.setPage);
    let pos = 0, errors = 0;
    while (pos < blob.length) {
      const chunk = blob.subarray(pos, pos + page);
      const rep = await this.call(pageOp, concat(le16(pos), chunk), [ST.OK, ST.SEQUENCE]);
      const next = rep.payload.length >= 2 ? rd16(rep.payload) : null;
      if (rep.status === ST.OK) {
        if (next !== pos + chunk.length) throw new ConfigError(`the keyboard acknowledged the page but expects offset ${next}, not ${pos + chunk.length}`);
        pos = next;
      } else {
        if (++errors > 20 || next === null || next > blob.length) throw new ConfigError("staging keeps going out of sequence");
        this.#log(`sequence error: resuming at offset ${next}`);
        pos = next;
      }
    }
  }

  /**
   * COMMIT the staged `blob` (the one request that writes flash). `word` must be "SAVE". A lost reply is resolved through
   * GET_INFO: if the commit counter went up and nothing is staged, the commit happened and it is NOT sent again.
   * Returns the keyboard's commit counter.
   */
  async commit(blob, word) {
    if (word !== WORD.COMMIT) throw new ConfigError(`COMMIT needs the confirmation word "${WORD.COMMIT}"`);
    const before = (await this.getInfo()).commits;
    const token = this.#nextToken();
    const packet = buildRequest(OP.COMMIT, token, concat(ascii(WORD.COMMIT), le16(blob.length), le32(crc32(blob))));
    for (let attempt = 0; attempt < this.#retries; attempt++) {
      let rep;
      try {
        rep = decodeReply(await this.#t.exchange(packet));
      } catch (err) {
        if (!isTimeout(err)) throw err;
        const inf = await this.getInfo();
        if (inf.commits === before + 1 && inf.stageState === 0) return inf.commits;
        if (inf.commits !== before) throw new ConfigError(`commit counter moved by ${inf.commits - before}, expected 0 or 1`);
        continue; // the request itself was lost: send it again
      }
      if (rep.op !== OP.COMMIT || rep.token !== token) continue;
      if (rep.status !== ST.OK) throw new ConfigError(`COMMIT refused: ${statusName(rep.status)}`, rep.status);
      return rd32(rep.payload);
    }
    throw new ConfigError(`no answer to COMMIT after ${this.#retries} attempts`);
  }

  // ---- protocol 2 (CONFIG_PROTOCOL.md): device status and the light scene ----

  #need(feature, what) {
    if ((this.info?.protoVersion ?? 0) < 2 || !(this.info.features & feature)) {
      throw new ConfigError(`this firmware has no ${what} (protocol ${this.info?.protoVersion})`, null, "unsupported");
    }
  }

  /** DEVICE_STATUS (read-only): {hostLeds, caps, ..., padModeSetting, padModeActive, usbFellBack, profile, layer, streamState, ...}. */
  async deviceStatus() {
    this.#need(FEAT.DEVICE_STATUS, "device status");
    return parseDeviceStatus((await this.call(OP.DEVICE_STATUS)).payload);
  }

  /** The live light scene blob (KMLS, km_lightscene.h), read from one consistent snapshot. */
  async sceneDump(tries = 4) {
    this.#need(FEAT.SCENE, "light scene");
    return this.#dump(OP.SCENE_GET, MAX_SCENE, tries);
  }

  /** SCENE_BEGIN + SCENE_PAGEs: fills the scene staging buffer (RAM only, separate from the settings staging). */
  async sceneStage(blob) {
    this.#need(FEAT.SCENE, "light scene");
    await this.#stage(blob, OP.SCENE_BEGIN, OP.SCENE_PAGE, Math.min(MAX_SCENE, this.info.sceneMax || MAX_SCENE));
  }

  /**
   * SCENE_COMMIT the staged `blob` (writes the scene's flash sector; the word must be "LITE"). A lost reply is settled by reading the
   * scene back: when it equals `blob` the commit happened and is NOT sent again (the commit counter is then unknown: returns -1).
   */
  async sceneCommit(blob, word) {
    if (word !== WORD.SCENE_COMMIT) throw new ConfigError(`SCENE_COMMIT needs the confirmation word "${WORD.SCENE_COMMIT}"`);
    this.#need(FEAT.SCENE, "light scene");
    const token = this.#nextToken();
    const packet = buildRequest(OP.SCENE_COMMIT, token, concat(ascii(WORD.SCENE_COMMIT), le16(blob.length), le32(crc32(blob))));
    for (let attempt = 0; attempt < this.#retries; attempt++) {
      let rep;
      try {
        rep = decodeReply(await this.#t.exchange(packet));
      } catch (err) {
        if (!isTimeout(err)) throw err;
        const now = await this.#dump(OP.SCENE_GET, MAX_SCENE, 4);
        if (now.length === blob.length && now.every((b, i) => b === blob[i])) return -1;
        continue;
      }
      if (rep.op !== OP.SCENE_COMMIT || rep.token !== token) continue;
      if (rep.status !== ST.OK) throw new ConfigError(`SCENE_COMMIT refused: ${statusName(rep.status)}`, rep.status);
      return rd32(rep.payload);
    }
    throw new ConfigError(`no answer to SCENE_COMMIT after ${this.#retries} attempts`);
  }

  /** Make profile `index` the active one. `word` must be "PROF". Returns the active profile the keyboard reports. */
  async selectProfile(index, word) {
    if (word !== WORD.SELECT_PROFILE) throw new ConfigError(`SELECT_PROFILE needs the confirmation word "${WORD.SELECT_PROFILE}"`);
    const info = this.info ?? (await this.getInfo());
    if (!Number.isInteger(index) || index < 0 || index >= info.profileCount) {
      throw new ConfigError(`profile ${index}: the keyboard has ${info.profileCount} profiles`);
    }
    const rep = await this.call(OP.SELECT_PROFILE, concat(ascii(WORD.SELECT_PROFILE), Uint8Array.of(index)));
    return rep.payload[0];
  }

  /** Ask the keyboard to start a calibration (no key may be down). `word` must be "RCAL". BUSY is thrown as ConfigError code "busy". */
  async requestRecalibration(word) {
    if (word !== WORD.RECALIBRATE) throw new ConfigError(`RECALIBRATE needs the confirmation word "${WORD.RECALIBRATE}"`, null, "refused");
    await this.call(OP.RECALIBRATE, ascii(WORD.RECALIBRATE));
  }

  /** Ask the keyboard to store the calibration it has learned. `word` must be "SCAL". */
  async requestSaveCalibration(word) {
    if (word !== WORD.SAVE_CALIBRATION) throw new ConfigError(`SAVE_CALIBRATION needs the confirmation word "${WORD.SAVE_CALIBRATION}"`, null, "refused");
    await this.call(OP.SAVE_CALIBRATION, ascii(WORD.SAVE_CALIBRATION));
  }

  /** Live depths (0.01 mm) of `count` slots starting at slot 0. */
  async liveDepth(count = 76, out = new Uint16Array(count)) {
    for (let first = 0; first < count; first += MAX_DEPTHS) {
      const n = Math.min(MAX_DEPTHS, count - first);
      const p = (await this.call(OP.LIVE_DEPTH, Uint8Array.of(first, n))).payload;
      if (p.length !== 2 + 2 * n || p[0] !== first || p[1] !== n) throw new ConfigError("bad LIVE_DEPTH reply", null, "protocol");
      for (let i = 0; i < n; i++) out[first + i] = rd16(p, 2 + 2 * i);
    }
    return out;
  }

  /** The lights now in force: {effect, brightness, speed, r, g, b} (LED_GET, feature bit 6; RAM only). UNSUPPORTED on a build without lights. */
  async ledGet() {
    return decodeLed((await this.call(OP.LED_GET)).payload);
  }

  /** Extended LED_GET: the lights plus {idleS, status, lastResult, savesOk, savesFailed}. BAD_LENGTH on firmware without the extended form. */
  async ledGetEx(profile = null) {
    const args = profile === null || profile === undefined ? Uint8Array.of(1) : Uint8Array.of(1, profile);
    return decodeLedEx((await this.call(OP.LED_GET, args)).payload);
  }

  /** Extended LED_SET: lights + idle time, `save` also keeps them in the keyboard's flash. Returns the extended state. UNSUPPORTED when `save` and the build cannot keep them. */
  async ledSetEx(state, opts) {
    return decodeLedEx((await this.call(OP.LED_SET, encodeLedEx(state, opts))).payload);
  }

  /** Set the lights (LED_SET): nothing is staged or saved, no confirmation word. Returns what the keyboard now has. RANGE for a bad effect / speed 0. */
  async ledSet(state) {
    return decodeLed((await this.call(OP.LED_SET, encodeLed(state))).payload);
  }
}
