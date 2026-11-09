// Device interface. The app only talks to this; MockDevice and WebHidDevice implement it.
//
//   status        "disconnected" | "connecting" | "connected" | "error"
//   kind          "mock" | "webhid"
//   readOnly      true when nothing may be written to the hardware (always true for webhid in v1)
//   capabilities  { live, readProfile, writeProfile }  what this device can really do right now
//   info          { productName, firmwareVersion, build, serial, vid, pid } (fields may be null)
//
// Events (EventTarget): "status" (status/error changed), "info" (info changed).

export class ReadOnlyError extends Error {
  constructor(what = "write") {
    super(`${what} is disabled: this version of the configurator is read-only for real keyboards`);
    this.name = "ReadOnlyError";
  }
}

export class UnsupportedError extends Error {
  constructor(message) {
    super(message);
    this.name = "UnsupportedError";
  }
}

export class Device extends EventTarget {
  kind = "none";
  status = "disconnected";
  error = null;
  readOnly = false;
  capabilities = { live: false, readProfile: false, writeProfile: false };
  info = { productName: null, firmwareVersion: null, build: null, serial: null, vid: null, pid: null };

  async connect() { throw new Error("not implemented"); }
  async disconnect() { this._setStatus("disconnected"); }

  /** Refresh and return this.info. */
  async getInfo() { return this.info; }

  /** Profile slot 0..3 -> profile object (see core/profile.js). */
  async readProfile(_index) { throw new Error("not implemented"); }
  async writeProfile(_index, _profile) { throw new Error("not implemented"); }

  /**
   * Live depth stream. cb(depth, tMs): depth is a Uint16Array(76) indexed by sensor slot in 0.01 mm
   * (the array is reused between calls, copy it if you keep it); tMs is performance.now()-style time.
   * Returns an unsubscribe function. This is where the planned "scope" channel (M3_PLAN.md section 2)
   * attaches: a scope-backed device converts its frames to this shape and calls cb.
   */
  subscribeLive(_cb) { return () => {}; }

  _setStatus(status, error = null) {
    this.status = status;
    this.error = error;
    this.dispatchEvent(new Event("status"));
  }

  _setInfo(patch) {
    this.info = { ...this.info, ...patch };
    this.dispatchEvent(new Event("info"));
  }
}
