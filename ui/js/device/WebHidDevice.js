// Real keyboard over WebHID. READ-ONLY in v1: the only thing sent is the version query in legacy_cmd3.js.
// Profile reads/writes and everything else are rejected before any byte leaves the browser.
import { Device, ReadOnlyError, UnsupportedError } from "./Device.js";
import { VID, PID, USAGE_PAGE, USAGE } from "./ids.js";
import { legacyReadVersion } from "./legacy_cmd3.js";
import { MODEL_NAME } from "../config.js";

export class WebHidDevice extends Device {
  kind = "webhid";
  readOnly = true;
  capabilities = { live: false, readProfile: false, writeProfile: false };
  #hid = null;
  #scope = null; // optional live-channel adapter, see attachScope()

  static get supported() {
    return typeof navigator !== "undefined" && "hid" in navigator;
  }

  /**
   * Slot for the custom "scope" live channel (command 0xC5, M3_PLAN.md section 2, firmware/hid/include/km/km_scope.h).
   * An adapter is { start(onFrame), stop() } where onFrame(depthUint16Array76, tMs) delivers depths in
   * 0.01 mm. Once attached, subscribeLive() forwards its frames and capabilities.live becomes true.
   */
  attachScope(adapter) {
    this.#scope = adapter;
    this.capabilities = { ...this.capabilities, live: !!adapter };
  }

  /** Ask the user to pick the keyboard (must be called from a click). */
  async connect() {
    if (!WebHidDevice.supported) throw new UnsupportedError("WebHID is not available in this browser");
    this._setStatus("connecting");
    try {
      const granted = await navigator.hid.requestDevice({
        filters: [{ vendorId: VID, productId: PID, usagePage: USAGE_PAGE, usage: USAGE }],
      });
      if (!granted.length) throw new Error("no device chosen");
      await this.#open(granted[0]);
    } catch (err) {
      this._setStatus("error", err);
      throw err;
    }
  }

  /** Open a keyboard the caller already picked (the app first asks which firmware it runs). */
  async connectWith(hid) {
    this._setStatus("connecting");
    try {
      await this.#open(hid);
    } catch (err) {
      this._setStatus("error", err);
      throw err;
    }
  }

  /** Reconnect to a keyboard the user already allowed earlier, without a prompt. */
  async connectGranted() {
    if (!WebHidDevice.supported) return false;
    const list = await navigator.hid.getDevices();
    const dev = list.find((d) => d.vendorId === VID && d.productId === PID && d.collections.some((c) => c.usagePage === USAGE_PAGE && c.usage === USAGE));
    if (!dev) return false;
    this._setStatus("connecting");
    try {
      await this.#open(dev);
      return true;
    } catch (err) {
      this._setStatus("error", err);
      return false;
    }
  }

  async #open(hid) {
    if (!hid.opened) await hid.open();
    this.#hid = hid;
    navigator.hid.addEventListener("disconnect", this.#onDisconnect);
    this._setInfo({ productName: hid.productName || MODEL_NAME, vid: hid.vendorId, pid: hid.productId });
    await this.getInfo();
    this._setStatus("connected");
  }

  #onDisconnect = (ev) => {
    if (ev.device === this.#hid) {
      this.#hid = null;
      this._setStatus("disconnected");
    }
  };

  async disconnect() {
    navigator.hid?.removeEventListener("disconnect", this.#onDisconnect);
    const hid = this.#hid;
    this.#hid = null;
    try { await hid?.close(); } catch { /* already gone */ }
    this._setStatus("disconnected");
  }

  /** Version query through the legacy shim (the only request ever sent). */
  async getInfo() {
    if (!this.#hid) return this.info;
    const { version, build } = await legacyReadVersion(this.#hid);
    this._setInfo({ firmwareVersion: version, build });
    return this.info;
  }

  async readProfile() { throw new ReadOnlyError("reading profiles"); }
  async writeProfile() { throw new ReadOnlyError("writing profiles"); }

  subscribeLive(cb) {
    if (!this.#scope) return () => {};
    this.#scope.start(cb);
    return () => this.#scope.stop();
  }
}
