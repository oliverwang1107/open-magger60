// Application state shared by every page: selection, profiles, device, language/theme, routing, and the sync between the editor's
// local model and the keyboard (see sync.js). Editing only ever changes the local model; the keyboard changes when the person
// presses Save (saveToKeyboard) or confirms switching the active profile, recalibrating or storing the calibration.
import { loadJson, saveJson } from "./storage.js";
import { setLang } from "./i18n.js";
import { LiveFeed } from "./live-feed.js";
import { cloneProfile, defaultProfile, importProfileJson, PROFILE_COUNT } from "./core/profile.js";
import { WebHidDevice } from "./device/WebHidDevice.js";
import { KmfwDevice } from "./device/KmfwDevice.js";
import { probeFirmware } from "./device/index.js";
import { classifyProblem } from "./device/errors.js";
import { WORD } from "./device/config_protocol.js";
import { VID, PID, USAGE_PAGE, USAGE } from "./device/ids.js";
import { rawCodec } from "./core/raw_codec.js";
import { FW_SETTINGS } from "./core/limits.js";
import { decodeSettings, PROFILE_BYTES } from "./core/settings_codec.js";
import { normalizeBlob } from "./core/profile_codec.js";
import { loadFromKeyboard, pendingChanges, saveToKeyboard } from "./sync.js";

export const PAGES = ["live", "performance", "remap", "behaviors", "pad", "lighting", "animation", "profiles", "device"];
const KEYBOARD_PAGES = new Set(["live", "performance", "remap", "behaviors", "pad", "lighting", "animation"]);
export const pageHasKeyboard = (p) => KEYBOARD_PAGES.has(p);

export async function createApp({ layout, mock }) {
  const prefs = loadJson("prefs", {});
  const listeners = new Map();
  const app = {
    layout,
    mock,
    device: mock,
    lang: prefs.lang === "en" ? "en" : "zh-TW",
    theme: prefs.theme === "light" ? "light" : "dark",
    page: PAGES.includes(location.hash.slice(2)) ? location.hash.slice(2) : "live",
    profiles: [],
    /** Index of the profile being edited (the top bar switch). Not the profile that is active on the keyboard, see deviceActive. */
    active: Number.isInteger(prefs.active) && prefs.active >= 0 && prefs.active < PROFILE_COUNT ? prefs.active : 0,
    /** Profile index the keyboard has active (from its settings), null when unknown. */
    deviceActive: null,
    /** The keyboard's settings as last read or saved, normalized through the UI model; null before the first read. */
    deviceBlob: null,
    /** Global device options and optional sections the UI keeps as they are ({ device, unknown }), as last read or edited; null = defaults (M6). */
    m6: null,
    sync: { status: "idle", step: null, dirty: false, profileDirty: [false, false, false, false], localOnly: false, problems: [], converted: 0, lastSave: null, error: null, loadError: null },
    selection: new Set(),
    primary: null,
    layer: 0,
    feed: null,
    deviceError: null,
    deviceProblem: null,
    /** Automatic connection (js/auto_connect.js), set by main.js; null in tests. */
    auto: null,
    /** What auto-connect is doing: { state, error } (states in auto_connect.js). */
    conn: { state: "idle", error: null },

    setConn(conn) {
      this.conn = conn;
      this.emit("conn");
    },

    on(evt, fn) {
      if (!listeners.has(evt)) listeners.set(evt, new Set());
      listeners.get(evt).add(fn);
      return () => listeners.get(evt).delete(fn);
    },
    emit(evt, detail) {
      listeners.get(evt)?.forEach((fn) => fn(detail));
    },

    get profile() {
      return this.profiles[this.active];
    },
    /** Key id (profile key) of a sensor slot. */
    idOf(slot) {
      return layout.bySlot.get(slot).id;
    },
    /** Settings of the key at `slot` in `profile` (default: active profile). */
    dataOf(slot, profile = this.profile) {
      return profile.keys[layout.bySlot.get(slot).id];
    },
    get selected() {
      return [...this.selection].sort((a, b) => a - b);
    },

    /** mode: replace | add | toggle | remove */
    select(slots, mode = "replace") {
      const list = Array.isArray(slots) ? slots : [slots];
      if (mode === "replace") this.selection = new Set(list);
      else for (const s of list) {
        if (mode === "add") this.selection.add(s);
        else if (mode === "remove") this.selection.delete(s);
        else this.selection.has(s) ? this.selection.delete(s) : this.selection.add(s);
      }
      if (list.length && mode !== "remove" && this.selection.has(list.at(-1))) this.primary = list.at(-1);
      if (!this.selection.has(this.primary)) this.primary = this.selected[0] ?? null;
      this.emit("selection");
    },

    setPage(page) {
      if (!PAGES.includes(page)) page = "live";
      if (location.hash !== `#/${page}`) history.replaceState(null, "", `#/${page}`);
      this.page = page;
      this.emit("page");
    },

    setLang(lang) {
      this.lang = lang;
      setLang(lang);
      savePrefs();
      this.emit("prefs");
    },
    setTheme(theme) {
      this.theme = theme;
      document.documentElement.dataset.theme = theme;
      savePrefs();
      this.emit("prefs");
    },

    /** Mutate a profile in the local model (default: the one being edited). Nothing is sent to the keyboard. */
    edit(fn, index = this.active) {
      const result = fn(this.profiles[index]);
      changed(index);
      return result;
    },
    replaceProfile(index, profile) {
      this.profiles[index] = profile;
      changed(index);
    },
    switchProfile(index) {
      this.active = index;
      savePrefs();
      this.emit("profile", { index, switched: true });
    },
    importProfile(index, text) {
      const r = importProfileJson(text, layout);
      if (r.ok) this.replaceProfile(index, r.profile);
      return r;
    },
    copyProfile(from, to) {
      this.replaceProfile(to, cloneProfile(this.profiles[from]));
    },

    // ---- sync with the keyboard ----

    /** Can this device store settings at all (demo keyboard or our firmware)? */
    get canWrite() {
      return this.device.status === "connected" && !!this.device.features?.settings && !!this.device.capabilities.writeProfile;
    },

    /** Replace the local model by what the keyboard holds. Unsaved edits are discarded. */
    async pull() {
      const dev = this.device;
      if (dev.status !== "connected" || !dev.capabilities.readProfile) { recompute(); return false; }
      this.sync.status = "loading";
      this.emit("sync");
      try {
        const { workspace, blob } = await loadFromKeyboard(dev, layout, { profiles: this.profiles });
        this.profiles = workspace.profiles;
        this.deviceActive = workspace.active;
        this.m6 = workspace.m6 ?? null;
        this.deviceBlob = blob;
        this.sync.loadError = null;
        this.sync.status = "idle";
        recompute();
        this.emit("profile", { index: this.active, reloaded: true });
        return true;
      } catch (err) {
        this.sync.loadError = problemOf(err);
        this.sync.status = "idle";
        recompute();
        return false;
      }
    },

    /** Write the local model to the keyboard (stage, commit with the word, read back, compare). The caller asked the person first. */
    async save() {
      if (!this.canWrite || this.sync.status === "saving") return null;
      this.sync.status = "saving";
      this.sync.step = "encode";
      this.sync.error = null;
      this.emit("sync");
      try {
        const res = await saveToKeyboard(this.device, { active: this.deviceActive ?? 0, profiles: this.profiles, m6: this.m6 ?? undefined }, layout, {
          onStep: (s) => { this.sync.step = s; this.emit("sync"); },
        });
        this.deviceBlob = res.blob;
        this.sync.lastSave = { at: Date.now(), commits: res.commits, converted: res.converted, bytes: res.bytes.length };
        this.sync.status = "idle";
        recompute();
        return res;
      } catch (err) {
        this.sync.error = { ...problemOf(err), problems: err.problems ?? null };
        this.sync.status = "idle";
        recompute();
        throw err;
      }
    },

    /** Make profile `index` the one the keyboard uses (persistent on the keyboard; the caller confirmed). */
    async activateOnKeyboard(index) {
      const dev = this.device;
      await dev.selectProfile(index, WORD.SELECT_PROFILE);
      const blob = await dev.readSettings();
      this.deviceActive = decodeSettings(blob).active;
      this.deviceBlob = normalizeBlob(blob, layout);
      recompute();
      this.emit("profile", { index: this.active, activated: true });
    },

    async recalibrate() { await this.device.recalibrate(WORD.RECALIBRATE); },
    async saveCalibration() { await this.device.saveCalibration(WORD.SAVE_CALIBRATION); },

    /** Switch back to the built-in simulator. */
    async useMock() {
      this.auto?.pause(); // the person chose the demo keyboard: do not grab the real one behind their back
      await swap(this.mock, () => this.mock.connect());
    },
    /** Close the real keyboard and stay disconnected (auto-connect waits until the person connects again or replugs). */
    async disconnectReal() {
      this.auto?.pause();
      await this.device.disconnect();
    },
    /** Pick the real keyboard. Must come from a user gesture. Our firmware -> full access; any other firmware -> read-only. */
    async connectReal() {
      this.deviceError = null;
      this.deviceProblem = null;
      if (!WebHidDevice.supported) {
        const err = new Error("WebHID is not available in this browser");
        err.name = "UnsupportedError";
        this.deviceError = err;
        this.deviceProblem = classifyProblem(err);
        this.emit("device");
        throw err;
      }
      const picked = await navigator.hid.requestDevice({ filters: [{ vendorId: VID, productId: PID, usagePage: USAGE_PAGE, usage: USAGE }] }).catch((err) => { throw err; });
      if (!picked.length) {
        const err = new Error("no device chosen");
        err.name = "NotFoundError";
        this.deviceProblem = classifyProblem(err);
        throw err;
      }
      this.auto?.resume();
      await this.connectHid(picked[0]);
    },
    /** Connect to a keyboard the browser already gave this page access to (a chosen or previously permitted HIDDevice). No user gesture needed. */
    async connectHid(hid) {
      this.deviceError = null;
      this.deviceProblem = null;
      let dev;
      try {
        const kind = await probeFirmware(hid);
        if (kind === "kmfw") {
          dev = new KmfwDevice({ settingsSize: FW_SETTINGS.size });
          dev.attachSettingsCodec(rawCodec);
        } else {
          dev = new WebHidDevice();
        }
      } catch (err) {
        this.deviceError = err;
        this.deviceProblem = classifyProblem(err);
        this.emit("device");
        throw err;
      }
      await swap(dev, () => dev.connectWith(hid));
    },
  };

  const problemOf = (err) => classifyProblem(err);

  // ---- UI-only data of the profiles (names, lighting, key colours) is kept in the browser; the keyboard has no place for it ----
  const readMeta = () => loadJson("meta", []);
  const persistMeta = () => saveJson("meta", app.profiles.map((p) => ({
    name: p.name, layerNames: p.layerNames, lighting: p.lighting, colors: Object.fromEntries(Object.entries(p.keys).map(([id, k]) => [id, k.color])),
  })));
  const applyMeta = (profile, m) => {
    if (!m) return profile;
    if (typeof m.name === "string") profile.name = m.name.slice(0, 32);
    if (Array.isArray(m.layerNames) && m.layerNames.length === profile.layerNames.length) profile.layerNames = m.layerNames.map((n) => (typeof n === "string" ? n.slice(0, 24) : ""));
    if (m.lighting && typeof m.lighting === "object") profile.lighting = { ...profile.lighting, ...m.lighting };
    for (const [id, c] of Object.entries(m.colors ?? {})) if (profile.keys[id] && /^#[0-9a-f]{6}$/i.test(c)) profile.keys[id].color = c;
    return profile;
  };

  const savePrefs = () => saveJson("prefs", { lang: app.lang, theme: app.theme, active: app.active });

  let recomputeTimer = 0;
  function recompute() {
    clearTimeout(recomputeTimer);
    const s = app.sync;
    const writable = app.canWrite;
    const p = pendingChanges({ active: app.deviceActive ?? 0, profiles: app.profiles, m6: app.m6 ?? undefined }, app.deviceBlob, layout);
    s.problems = p.problems;
    s.converted = p.converted;
    s.localOnly = !writable;
    s.dirty = writable && p.dirty;
    // which of the four profiles differ from the keyboard's copy (each profile is one slice of the blob)
    s.profileDirty = Array.from({ length: PROFILE_COUNT }, (_, i) => {
      if (!writable || !p.normalized || !app.deviceBlob) return false;
      const from = FW_SETTINGS.headerBytes + i * PROFILE_BYTES;
      for (let j = from; j < from + PROFILE_BYTES; j++) if (p.normalized[j] !== app.deviceBlob[j]) return true;
      return false;
    });
    app.emit("sync");
  }
  function changed(index) {
    clearTimeout(recomputeTimer);
    recomputeTimer = setTimeout(recompute, 60);
    persistMeta();
    app.emit("profile", { index });
  }

  // when the firmware publishes the pressed-key mask (tag 0x0301), the live view shows what the keyboard really reports as pressed
  let maskTimer = 0;
  function watchKeyMask(dev) {
    clearInterval(maskTimer);
    if (!dev.features?.keyMask) return;
    let busy = false;
    maskTimer = setInterval(async () => {
      if (busy || dev.status !== "connected") return;
      busy = true;
      try { app.feed.setPressedMask((await dev.readKeyMask())?.down ?? []); } catch { /* the device layer reports lost keyboards itself */ } finally { busy = false; }
    }, 80);
  }

  let unwatch = null;
  function watch(dev) {
    unwatch?.();
    const fn = () => { app.emit("device"); recompute(); };
    dev.addEventListener("status", fn);
    dev.addEventListener("info", () => app.emit("device"));
    unwatch = () => { dev.removeEventListener("status", fn); };
  }

  async function swap(dev, connect) {
    const prev = app.device;
    if (prev !== dev) await prev.disconnect().catch(() => {});
    app.device = dev;
    watch(dev);
    app.deviceBlob = null;
    app.deviceActive = null;
    app.m6 = null;
    app.sync.loadError = null;
    app.sync.error = null;
    try {
      await connect();
    } catch (err) {
      app.deviceError = err;
      app.deviceProblem = dev.problem ?? classifyProblem(err);
      app.device = mock;
      watch(mock);
      await mock.connect().catch(() => {});
      app.feed.attach(mock);
      watchKeyMask(mock);
      await app.pull();
      app.emit("device");
      throw err;
    }
    app.feed.attach(dev);
    watchKeyMask(dev);
    await app.pull();
    app.emit("device");
  }

  window.addEventListener("beforeunload", (e) => {
    if (app.sync.dirty) { e.preventDefault(); e.returnValue = ""; }
  });

  setLang(app.lang);
  document.documentElement.dataset.theme = app.theme;
  const meta = readMeta();
  app.profiles = Array.from({ length: PROFILE_COUNT }, (_, i) => applyMeta(defaultProfile(layout), meta[i]));
  app.feed = new LiveFeed(app);
  app.select([layout.byId.get("KeyW")?.slot ?? layout.keys[0].slot]);
  watch(mock);
  await mock.connect();
  app.device = mock;
  app.feed.attach(mock);
  watchKeyMask(mock);
  await app.pull();
  app.setPage(app.page);
  return app;
}
