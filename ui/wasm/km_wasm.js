// The keyboard's own settings / key-map / layer code, compiled to WebAssembly (firmware/wasm/km_wasm_api.c, built by
// tools/build_wasm.py into km_core.wasm). OPTIONAL: nothing in the app needs it; it is here so the page can ask the very
// code the keyboard runs (is this blob acceptable? what does this key press turn into?) instead of a hand-written twin.
//
//   import { loadCore } from "./wasm/km_wasm.js";
//   const core = await loadCore();                // browser: fetches km_core.wasm next to this file
//   const blob = core.defaultBlob();              // Uint8Array, 7944 bytes (the lowest format version that holds the settings is written)
//   const again = core.decode(blob);              // Uint8Array in the current format, or null when the firmware would refuse it
//
// The module has no imports and no JavaScript glue is needed beyond this file: every call copies bytes into the module's
// I/O buffer, calls one export and copies the answer out.

const ABI = 3;

export class KmCore {
  constructor(instance) {
    this.x = instance.exports;
    if (this.x.km_abi() !== ABI) throw new Error(`km_core.wasm has ABI ${this.x.km_abi()}, this loader expects ${ABI}: rebuild with tools/build_wasm.py`);
    this.size = this.x.km_settings_size();
    this.version = this.x.km_settings_version();
    this.ioSize = this.x.km_io_size();
    this.dims = { profiles: this.x.km_dims(0), layers: this.x.km_dims(1), slots: this.x.km_dims(2), args: this.x.km_dims(3) };
  }

  get #io() { return new Uint8Array(this.x.memory.buffer, this.x.km_io(), this.ioSize); }
  #out(n) { return Uint8Array.from(this.#io.subarray(0, n)); }

  /** Factory-default settings blob. */
  defaultBlob() { return this.#out(this.x.km_w_default()); }

  /** Decode a blob with the firmware's decoder. Returns the blob re-encoded in the current format (an old format is migrated),
   *  or null when the firmware refuses it. A successful call also makes it the settings the edit / valid / layer calls use. */
  decode(bytes) {
    if (bytes.length > this.ioSize) return null;
    this.#io.set(bytes);
    const n = this.x.km_w_decode(bytes.length);
    return n ? this.#out(n) : null;
  }

  /** Encode the held settings; null when they hold a value the firmware would refuse. */
  encode() {
    const n = this.x.km_w_encode();
    return n ? this.#out(n) : null;
  }

  /** CRC32 (the firmware's) of a byte string. */
  crc32(bytes) {
    this.#io.set(bytes);
    return this.x.km_w_crc32(bytes.length) >>> 0;
  }

  // ---- edits of the held settings; encode() / valid() then say whether the firmware accepts the result ----
  setActive(a) { return !!this.x.km_w_set_active(a); }
  setCfg(p, slot, c) {
    return !!this.x.km_w_set_cfg(p, slot, c.press, c.release, c.mode, c.continuous, c.rtDown, c.rtUp, c.topDead, c.bottomDead);
  }
  setEntry(p, layer, slot, code, a0 = 0, a1 = 0) { return !!this.x.km_w_set_entry(p, layer, slot, code, a0, a1); }
  setOpts(p, o) { return !!this.x.km_w_set_opts(p, o.tapTerm, o.oneshot, o.policy); }
  valid() { return !!this.x.km_w_valid(); }
  needsV3() { return !!this.x.km_w_needs_v3(); }

  // ---- settings format version 3 (M6): DKS, mod-morph, SOCD, gamepad curves, device options ----
  setDks(p, i, d) { const a = d.acts; return !!this.x.km_w_set_dks(p, i, d.tAct, d.tBot, d.flags, a[0][0], a[0][1], a[1][0], a[1][1], a[2][0], a[2][1], a[3][0], a[3][1], d.rsvd ?? 0); }
  setDksCount(p, n) { return !!this.x.km_w_set_dks_n(p, n); }
  setMorph(p, i, m) { return !!this.x.km_w_set_morph(p, i, m.mask, m.usage, m.flags, m.rsvd ?? 0); }
  setMorphCount(p, n) { return !!this.x.km_w_set_morph_n(p, n); }
  setSocd(p, i, d) { return !!this.x.km_w_set_socd(p, i, d.a, d.b, d.mode, d.flags); }
  setSocdCount(p, n) { return !!this.x.km_w_set_socd_n(p, n); }
  setPadCurve(p, i, c) { return !!this.x.km_w_set_pad(p, i, c.start, c.end, c.shape, c.flags); }
  setPadOpts(p, o, rsvd = 0) { return !!this.x.km_w_set_padopt(p, o.deadzone, o.circular, o.smooth, rsvd); }
  setDevice(d, rsvd = 0) { return !!this.x.km_w_set_device(d.padMode, d.filterMode, rsvd); }

  /** The sensor slots that carry a key (61 of 76), ascending. */
  keySlots() { return Array.from(this.#io.subarray(0, this.x.km_w_key_slots())); }

  /** Replay a list of edit commands, the form used by ui/tests/vectors/settings_vectors.json (made by firmware/tools/gen_settings_vector.py):
   *  ["cfg", p, slot, press, release, mode, continuous, rtDown, rtUp, topDead, bottomDead], ["ent", p, layer, slot, code, a0, a1],
   *  ["opt", p, tapTerm, oneshot, policy], ["dks", p, i, tAct, tBot, flags, u0, e0, u1, e1, u2, e2, u3, e3, rsvd], ["dksn", p, n],
   *  ["morph", p, i, mask, usage, flags, rsvd], ["morphn", p, n], ["socd", p, i, a, b, mode, flags], ["socdn", p, n],
   *  ["pad", p, i, start, end, shape, flags], ["padopt", p, deadzone, circular, smooth, rsvd], ["dev", padMode, filterMode, rsvd].
   *  Starts from the factory default; then encode() / valid() say what the firmware makes of the result. */
  runCmds(cmds, active = 0) {
    this.defaultBlob();
    this.setActive(active);
    const x = this.x;
    for (const [op, ...a] of cmds) {
      switch (op) {
        case "cfg": x.km_w_set_cfg(...a); break;
        case "ent": x.km_w_set_entry(...a); break;
        case "opt": x.km_w_set_opts(...a); break;
        case "dks": x.km_w_set_dks(...a); break;
        case "dksn": x.km_w_set_dks_n(...a); break;
        case "morph": x.km_w_set_morph(...a); break;
        case "morphn": x.km_w_set_morph_n(...a); break;
        case "socd": x.km_w_set_socd(...a); break;
        case "socdn": x.km_w_set_socd_n(...a); break;
        case "pad": x.km_w_set_pad(...a); break;
        case "padopt": x.km_w_set_padopt(...a); break;
        case "dev": x.km_w_set_device(...a); break;
        default: throw new Error(`unknown command ${op}`);
      }
    }
  }

  // ---- the single rules ----
  usageValid(u) { return !!this.x.km_w_usage_valid(u); }
  entryValid(code, a0, a1) { return !!this.x.km_w_entry_valid(code, a0, a1); }
  /** the rule a version 2 blob is read with (the M6 codes 0xEB..0xEF are not valid there) */
  entryValidV2(code, a0, a1) { return !!this.x.km_w_entry_valid_v2(code, a0, a1); }
  optsValid(o) { return !!this.x.km_w_opts_valid(o.tapTerm, o.oneshot, o.policy); }
  cfgValid(c) { return !!this.x.km_w_cfg_valid(c.press, c.release, c.mode, c.continuous, c.rtDown, c.rtUp, c.topDead, c.bottomDead); }

  // ---- layers ----
  /** Make profile p of the held settings the loaded key map and reset the layer state. */
  layersLoad(p) { return !!this.x.km_w_layers_load(p); }
  layersReset() { this.x.km_w_layers_reset(); }
  /** The table entry the key on this slot is locked to after the last layersUpdate ({ code, arg0, arg1 }), or null (see km_layers.h). */
  layersLocked(slot) {
    const v = this.x.km_w_layers_locked(slot) >>> 0;
    return v >>> 24 ? { code: v & 255, arg0: (v >>> 8) & 255, arg1: (v >>> 16) & 255 } : null;
  }
  /** downSlots: slot numbers currently pressed, oldest first; nowMs: free-running millisecond clock. */
  layersUpdate(downSlots, nowMs) {
    const io = this.#io;
    io.set(downSlots);
    const n = this.x.km_w_layers_update(downSlots.length, nowMs >>> 0);
    return {
      kbd: Array.from(this.#io.subarray(10, n)), layer: this.#io[1], toggled: this.#io[2], armed: this.#io[3],
      profileRequest: this.#io[4], pending: this.#io[5] === 1, consumer: this.#io[6] | (this.#io[7] << 8),
      toggleKeys: this.#io[8], suppressed: this.#io[9],
    };
  }
}

/** Load the module. `source` may be bytes (Uint8Array / ArrayBuffer), a Response or a URL; the default is km_core.wasm next to
 *  this file. Throws when the module is missing or does not match this loader (the caller decides whether that matters). */
export async function loadCore(source = new URL("./km_core.wasm", import.meta.url)) {
  let bytes = source;
  if (source instanceof URL || typeof source === "string") {
    if (typeof process !== "undefined" && process.versions?.node && (source.protocol ?? "file:") === "file:") {
      const { readFile } = await import("node:fs/promises");
      bytes = await readFile(source);
    } else {
      const r = await fetch(source);
      if (!r.ok) throw new Error(`km_core.wasm: HTTP ${r.status}`);
      bytes = new Uint8Array(await r.arrayBuffer());
    }
  } else if (typeof Response !== "undefined" && source instanceof Response) {
    bytes = new Uint8Array(await source.arrayBuffer());
  }
  const { instance } = await WebAssembly.instantiate(bytes, {});
  return new KmCore(instance);
}
