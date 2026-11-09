// A faithful port of the keyboard's gamepad core (firmware/core/src/km_pad.c, M6 package P4): response curves, sticks (circle, dead zone),
// hat, buttons, the output band, the 1 ms cadence and the fault / epoch / hold-over rules. The settings page uses it to draw the
// virtual gamepad from the live key depths; tests/pad.test.mjs makes it give the same bytes as the real C code
// (tests/vectors/pad_vectors.json, made by gen_pad_vectors.py from km_pad.c), so a change in km_pad.c that is not copied here fails a test.
//
// Units: depth is in 0.01 mm (the live feed's unit); a curve is { start, end, shape, flags } with start / end in 0.05 mm steps (the
// settings model's names for start_mm100_5 / end_mm100_5). All arithmetic is integer, in the same order as the C code.

export const PAD_SLOTS = 76;
export const PAD_BUTTONS = 16;
export const PAD_MAX_KEYS = 20;
export const PAD_REPORT_LEN = 15;
export const PAD_AXIS_MAX = 32767;
export const PAD_PERIOD_MS = 1;
export const PAD_CODE_BTN = 0xee;
export const PAD_CODE_AXIS = 0xef;
export const PAD_BTN_HAT_UP = 17;
export const PAD_BTN_HAT_RIGHT = 20;
export const PAD_AXIS = Object.freeze({ LX: 0, LY: 1, RX: 2, RY: 3, LT: 4, RT: 5 });
export const PAD_AXIS_NEG = 0x08;
export const PAD_AXIS_DIGITAL = 0x10;
export const PAD_MIN_START_MM100 = 20;
export const PAD_BTN_HYST_MM100 = 20;
export const PAD_BTN_MIN_ACT_MM100 = 20;
export const PAD_OUT_HYST = 64;
/** What the key engine does with a gamepad key (KM_PAD_ENGINE_PRESS_MM100 / RELEASE): "down" from 0.20 mm, "up" again below 0.10 mm. */
export const PAD_ENGINE_PRESS_MM100 = 20;
export const PAD_ENGINE_RELEASE_MM100 = 10;
export const SHAPE = Object.freeze({ LINEAR: 0, EASE_IN: 1, EASE_OUT: 2, S: 3 });
export const CURVE_MIRROR = 0x01;

const SCALE15 = 32768;
const MAXV = PAD_AXIS_MAX;

/** km_pad_default_curve(index): 0 stick, 1 trigger, 2 fine control near the centre, 3 S shape. An index out of range gives curve 0. */
export function defaultCurve(index) {
  const defs = [
    { start: 4, end: 64, shape: SHAPE.LINEAR, flags: 0 },
    { start: 4, end: 40, shape: SHAPE.LINEAR, flags: 0 },
    { start: 4, end: 64, shape: SHAPE.EASE_IN, flags: 0 },
    { start: 4, end: 64, shape: SHAPE.S, flags: 0 },
  ];
  return { ...defs[index >= 0 && index < defs.length ? index : 0] };
}

const u8 = (v) => Number.isInteger(v) && v >= 0 && v <= 255;

/** km_pad_curve_valid */
export function curveValid(c) {
  return !!c && u8(c.start) && u8(c.end) && u8(c.shape) && u8(c.flags)
    && c.start >= PAD_MIN_START_MM100 / 5 && c.end >= c.start + 2 && c.end <= 68 && c.shape <= SHAPE.S && (c.flags & ~CURVE_MIRROR) === 0;
}

/** km_pad_opts_valid; opts = { deadzone, circular, smooth } plus an optional `rsvd` array that must be zeros */
export function optsValid(o) {
  if (!o || !u8(o.deadzone) || !u8(o.circular) || !u8(o.smooth)) return false;
  if (o.deadzone > 50 || o.circular > 1 || o.smooth > 3) return false;
  return !(o.rsvd ?? []).some((v) => v !== 0);
}

/** km_pad_entry_valid: is (code, arg0, arg1) a valid gamepad table entry? */
export function entryValid(code, arg0, arg1) {
  if (code === PAD_CODE_BTN) return arg0 >= 1 && arg0 <= PAD_BTN_HAT_RIGHT && arg1 >= 0 && arg1 <= 68;
  if (code === PAD_CODE_AXIS) {
    const axis = arg0 & 7;
    if ((arg0 & 0xe0) !== 0 || axis >= 6 || arg1 < 0 || arg1 >= 4) return false;
    return axis <= PAD_AXIS.RY || (arg0 & PAD_AXIS_NEG) === 0;
  }
  return false;
}

// Shape on Q15: t in 0..32768 -> 0..32768
function shapeQ15(shape, t) {
  switch (shape) {
    case SHAPE.EASE_IN: return (t * t) >> 15;
    case SHAPE.EASE_OUT: { const u = SCALE15 - t; return SCALE15 - ((u * u + (SCALE15 - 1)) >> 15); }
    case SHAPE.S: return Math.floor((t * t * (3 * SCALE15 - 2 * t)) / 2 ** 30);
    default: return t;
  }
}

/** km_pad_curve_eval: 0..32767 for a depth in 0.01 mm. Monotonic, 0 up to the start, 32767 from the end. */
export function curveEval(c, depth) {
  const s = c.start * 5;
  const e = c.end * 5;
  const d = depth;
  if (d <= s) return 0;
  if (d >= e) return MAXV;
  const t = Math.floor(((d - s) << 15) / (e - s));
  const y = (c.flags & CURVE_MIRROR) !== 0 ? SCALE15 - shapeQ15(c.shape, SCALE15 - t) : shapeQ15(c.shape, t);
  return (y * MAXV + (SCALE15 >> 1)) >> 15;
}

// ---- report -------------------------------------------------------------------------------------------------------------------

export const neutralReport = () => ({ buttons: 0, hat: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0 });

/** km_pad_report_pack: the 15 bytes of the USB input report */
export function packReport(r) {
  const out = new Uint8Array(PAD_REPORT_LEN);
  const v = new DataView(out.buffer);
  v.setUint16(0, r.buttons, true);
  out[2] = r.hat >= 0 && r.hat <= 8 ? r.hat : 0;
  v.setInt16(3, r.lx, true);
  v.setInt16(5, r.ly, true);
  v.setInt16(7, r.rx, true);
  v.setInt16(9, r.ry, true);
  v.setUint16(11, r.lt, true);
  v.setUint16(13, r.rt, true);
  return out;
}

/** km_pad_report_unpack */
export function unpackReport(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, PAD_REPORT_LEN);
  const hat = bytes[2] & 0x0f;
  return {
    buttons: v.getUint16(0, true), hat: hat > 8 ? 0 : hat,
    lx: v.getInt16(3, true), ly: v.getInt16(5, true), rx: v.getInt16(7, true), ry: v.getInt16(9, true),
    lt: v.getUint16(11, true), rt: v.getUint16(13, true),
  };
}

export const reportHex = (r) => Array.from(packReport(r), (b) => b.toString(16).padStart(2, "0")).join("");
export const reportNeutral = (r) => packReport(r).every((b) => b === 0);

// ---- stick maths ----------------------------------------------------------------------------------------------------------------

function isqrtFloor(n) {
  let r = 0;
  let b = 2 ** 30;
  while (b > n) b = Math.floor(b / 4);
  while (b !== 0) {
    if (n >= r + b) { n -= r + b; r = Math.floor(r / 2) + b; } else r = Math.floor(r / 2);
    b = Math.floor(b / 4);
  }
  return r;
}
const isqrtCeil = (n) => { const r = isqrtFloor(n); return r * r < n ? r + 1 : r; };
const clampAxis = (v) => (v > MAXV ? MAXV : v < -MAXV ? -MAXV : v);
const idiv = (a, b) => Math.trunc(a / b);   // C integer division (truncates toward zero); every operand here is small enough to be exact

/** Circle limit, then centre dead zone (radial, rescaled to full), then clamp: returns [x, y]. */
export function stickShape(x, y, deadzonePct, circular) {
  let r2 = x * x + y * y;
  if (r2 === 0) return [x, y];
  const max2 = MAXV * MAXV;
  if (circular && r2 > max2) {
    const r = isqrtCeil(r2);
    x = idiv(x * MAXV, r);
    y = idiv(y * MAXV, r);
    r2 = x * x + y * y;
  }
  if (deadzonePct !== 0) {
    const dz = Math.floor((deadzonePct * MAXV) / 100);
    if (r2 <= dz * dz) return [0, 0];
    const r = isqrtCeil(r2);
    const rn = Math.floor(((r - dz) * MAXV) / (MAXV - dz));
    x = clampAxis(idiv(x * rn, r));
    y = clampAxis(idiv(y * rn, r));
  }
  return [x, y];
}

/** A value follows its input only after moving PAD_OUT_HYST; 0, full scale and a change of sign are taken at once. */
function outHyst(prev, v) {
  if (v === 0 || v === MAXV || v === -MAXV || (v < 0) !== (prev < 0) || prev === 0) return v;
  const d = v - prev;
  return d >= PAD_OUT_HYST || d <= -PAD_OUT_HYST ? v : prev;
}

const HAT_TABLE = [[8, 1, 2], [7, 0, 3], [6, 5, 4]];   // [vertical + 1][horizontal + 1]; vertical -1 = up, horizontal -1 = left
/** bits: 1 up, 2 down, 4 left, 8 right -> 0 neutral, 1..8 clockwise from north */
export function hatOf(bits) {
  const v = ((bits & 2) !== 0 ? 1 : 0) - ((bits & 1) !== 0 ? 1 : 0);
  const h = ((bits & 8) !== 0 ? 1 : 0) - ((bits & 4) !== 0 ? 1 : 0);
  return HAT_TABLE[v + 1][h + 1];
}

// ---- the state machine (km_pad_t) -------------------------------------------------------------------------------------------------

const cloneCurve = (c) => ({ start: c.start, end: c.end, shape: c.shape, flags: c.flags });

/**
 * One gamepad core. update() follows km_pad_update():
 *   keys   = the keys that are DOWN and bound to the gamepad: [{ slot, code, arg0, arg1 }] (in any order)
 *   depth  = (slot) => depth in 0.01 mm
 * It returns { report, recomputed }.
 */
export class PadCore {
  constructor() {
    this.init();
  }

  init() {
    this.curves = [0, 1, 2, 3].map(defaultCurve);
    this.opts = { deadzone: 0, circular: 0, smooth: 0 };
    this.cur = neutralReport();
    this.raw = [0, 0, 0, 0];
    this.blocked = new Set();
    this.btnOn = new Set();
    this.lastMs = 0;
    this.epoch = 0;
    this.started = false;
    this.rearm = true;
    this.counters = { computes: 0, faults: 0, resets: 0, ignoredKeys: 0 };
  }

  reset() {
    this.cur = neutralReport();
    this.raw = [0, 0, 0, 0];
    this.blocked = new Set();
    this.btnOn = new Set();
    this.rearm = true;
    this.counters.resets++;
  }

  /** km_pad_set_config: invalid curves become the default of their index, invalid options the zero options; true if nothing had to be replaced. */
  setConfig(curves, opts) {
    let ok = true;
    for (let i = 0; i < 4; i++) {
      if (curves && curveValid(curves[i])) this.curves[i] = cloneCurve(curves[i]);
      else { this.curves[i] = defaultCurve(i); ok = ok && !curves; }
    }
    if (opts && optsValid(opts)) this.opts = { deadzone: opts.deadzone, circular: opts.circular, smooth: opts.smooth };
    else { this.opts = { deadzone: 0, circular: 0, smooth: 0 }; ok = ok && !opts; }
    this.reset();
    return ok;
  }

  update({ keys = [], allowed = true, epoch = 0, nowMs = 0, depth = () => 0 }) {
    if (!allowed) {   // fault: neutral now and hold every key over until it has been released
      if (!this.rearm) this.counters.faults++;
      this.cur = neutralReport();
      this.raw = [0, 0, 0, 0];
      this.btnOn = new Set();
      this.rearm = true;
      this.epoch = epoch >>> 0;
      this.started = true;
      return { report: { ...this.cur }, recomputed: true };
    }
    if (this.started && (epoch >>> 0) !== this.epoch) this.reset();
    this.epoch = epoch >>> 0;
    if (this.started && !this.rearm && ((nowMs - this.lastMs) >>> 0) < PAD_PERIOD_MS) return { report: { ...this.cur }, recomputed: false };
    this.started = true;
    this.lastMs = nowMs >>> 0;
    this.counters.computes++;

    const n = Math.min(keys.length, PAD_MAX_KEYS);
    if (keys.length > n) this.counters.ignoredKeys += keys.length - n;
    const use = new Array(n).fill(false);
    const listed = new Set();
    for (let i = 0; i < n; i++) {
      const k = keys[i];
      use[i] = k.slot >= 0 && k.slot < PAD_SLOTS && entryValid(k.code, k.arg0, k.arg1);
      if (use[i]) listed.add(k.slot); else this.counters.ignoredKeys++;
    }
    if (this.rearm) {   // keys found down now are held over; nothing is output for them
      this.blocked = new Set(listed);
      this.btnOn = new Set();
      this.rearm = false;
      return { report: { ...this.cur }, recomputed: true };
    }
    for (let i = 0; i < n; i++) if (use[i] && this.blocked.has(keys[i].slot)) use[i] = false;
    this.blocked = new Set([...this.blocked].filter((s) => listed.has(s)));
    this.btnOn = new Set([...this.btnOn].filter((s) => listed.has(s)));
    this.#compute(keys, n, use, depth);
    return { report: { ...this.cur }, recomputed: true };
  }

  #compute(keys, n, use, depth) {
    let buttons = 0;
    let hatbits = 0;
    const pos = [0, 0, 0, 0, 0, 0];
    const neg = [0, 0, 0, 0];
    for (let i = 0; i < n; i++) {
      if (!use[i]) continue;
      const k = keys[i];
      if (k.code === PAD_CODE_BTN) {
        let on = true;
        if (k.arg1 === 0) this.btnOn.delete(k.slot);
        else {
          let act = k.arg1 * 5;
          if (act < PAD_BTN_MIN_ACT_MM100) act = PAD_BTN_MIN_ACT_MM100;
          const hyst = Math.min(Math.floor(act / 2), PAD_BTN_HYST_MM100);
          const d = depth(k.slot);
          on = this.btnOn.has(k.slot) ? d + hyst >= act : d >= act;
          if (on) this.btnOn.add(k.slot); else this.btnOn.delete(k.slot);
        }
        if (on) {
          if (k.arg0 <= PAD_BUTTONS) buttons |= 1 << (k.arg0 - 1);
          else hatbits |= 1 << (k.arg0 - PAD_BTN_HAT_UP);
        }
      } else {
        const axis = k.arg0 & 7;
        const v = (k.arg0 & PAD_AXIS_DIGITAL) !== 0 ? MAXV : curveEval(this.curves[k.arg1], depth(k.slot));
        if ((k.arg0 & PAD_AXIS_NEG) !== 0) { if (v > neg[axis]) neg[axis] = v; } else if (v > pos[axis]) pos[axis] = v;
      }
    }
    // the band acts on the raw axis values, before the circle and the dead zone, so that those two rules always hold exactly
    const v = [0, 0, 0, 0];
    for (let a = 0; a < 4; a++) {
      this.raw[a] = outHyst(this.raw[a], pos[a] - neg[a]);
      v[a] = this.raw[a];
    }
    const left = stickShape(v[0], v[1], this.opts.deadzone, this.opts.circular !== 0);
    const right = stickShape(v[2], v[3], this.opts.deadzone, this.opts.circular !== 0);
    this.cur = {
      buttons, hat: hatOf(hatbits), lx: left[0], ly: left[1], rx: right[0], ry: right[1],
      lt: outHyst(this.cur.lt, pos[PAD_AXIS.LT]), rt: outHyst(this.cur.rt, pos[PAD_AXIS.RT]),
    };
  }
}
