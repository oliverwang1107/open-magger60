// Trigger settings and a small model of when a key counts as pressed, so the UI can show "triggered" for any
// depth stream (mock now, the device's scope channel later). The model is a reference implementation of the
// usual behaviour, not a copy of any firmware: verify it against the custom firmware once that exists.
import { mmToRaw, MAX_TRAVEL_RAW } from "./units.js";
import { UI_LIMITS, NOISE_FLOOR_MM, HYSTERESIS_MIN_MM, TRAVEL_MM } from "./limits.js";

export const MODE = { NORMAL: "normal", RT: "rapid", RT_CONTINUOUS: "continuous" };
export const MODES = Object.values(MODE);

// UI limits in mm: all of them come from core/limits.js (the one constants module).
export const LIMITS = UI_LIMITS;

// Factory trigger = the firmware's own default (km_engine_key_cfg_default): press 1.20 mm, release 0.80 mm, rapid steps 0.30 mm.
export const defaultTrigger = () => ({
  mode: MODE.NORMAL, actuation: 1.2, release: 0.8, rapidDown: 0.3, rapidUp: 0.3, topDead: 0, bottomDead: 0,
});

const r2 = (v) => Math.round(v * 100) / 100;
const clampTo = (v, lo, hi) => r2(Math.min(hi, Math.max(lo, Number.isFinite(v) ? v : lo)));
// a non-zero dead zone below the noise floor is not allowed: it snaps to 0 (off) or to the floor, whichever is nearer
const snapDead = (v) => (v <= 0 ? 0 : v < NOISE_FLOOR_MM ? (v < NOISE_FLOOR_MM / 2 ? 0 : NOISE_FLOOR_MM) : v);

/**
 * Bring a trigger inside every limit, in place. `changed` (field names the user just set) decides which side gives way when two
 * dead-zone fields contradict: the field that was NOT edited moves. Returns notes [{field, code, value}] for the UI to explain
 * (codes: below_noise, above_travel, release_order, dead_floor, dead_sum); empty when nothing had to be adjusted.
 */
export function normalizeTrigger(t, changed = []) {
  const notes = [];
  const note = (field, code, value) => notes.push({ field, code, value });
  const L = LIMITS;
  const fix = (field, lim, snap = (v) => v) => {
    const before = t[field];
    const snapped = snap(before);
    t[field] = clampTo(snapped, lim.min, lim.max);
    if (t[field] !== r2(before)) note(field, snapped !== before ? "dead_floor" : before < lim.min ? "below_noise" : "above_travel", t[field]);
  };
  fix("actuation", L.actuation); fix("release", L.release); fix("rapidDown", L.rt); fix("rapidUp", L.rt);
  fix("topDead", L.dead, snapDead); fix("bottomDead", L.dead, snapDead);
  if (t.release > r2(t.actuation - HYSTERESIS_MIN_MM)) { // the release point always sits below the actuation point; it is the one that moves
    t.release = r2(t.actuation - HYSTERESIS_MIN_MM);
    note("release", "release_order", t.release);
  }
  if (t.rapidDown > r2(TRAVEL_MM - t.bottomDead)) { // a press must be reachable: rt_down <= travel - bottom dead zone (km_engine_cfg_valid)
    if (changed.includes("bottomDead") && !changed.includes("rapidDown")) {
      t.bottomDead = clampTo(TRAVEL_MM - t.rapidDown, 0, L.dead.max);
      note("bottomDead", "rt_reach", t.bottomDead);
    } else {
      t.rapidDown = clampTo(TRAVEL_MM - t.bottomDead, L.rt.min, L.rt.max);
      note("rapidDown", "rt_reach", t.rapidDown);
    }
  }
  if (t.topDead + t.bottomDead > TRAVEL_MM - NOISE_FLOOR_MM) {
    const f = changed.includes("topDead") ? "bottomDead" : "topDead";
    t[f] = clampTo(TRAVEL_MM - NOISE_FLOOR_MM - t[f === "topDead" ? "bottomDead" : "topDead"], 0, L.dead.max);
    note(f, "dead_sum", t[f]);
  }
  return notes;
}

/** Problems of a trigger exactly as stored (no changes made); [] when it satisfies every limit. */
export function triggerIssues(t) {
  const copy = { ...t };
  return normalizeTrigger(copy, []);
}

export const newKeyState = () => ({ pressed: false, low: 0, high: 0, armed: false });

/**
 * Advance one key by one depth sample (raw 0.01 mm units). Returns "press" | "release" | null.
 * normal: fixed point, released at the same point. rapid: released after `rapidUp` of upward travel from the
 * deepest point, pressed again after `rapidDown` of downward travel from the shallowest point, but only below
 * the actuation point. continuous: like rapid, but after crossing the actuation point once it re-triggers at
 * any depth below the top dead zone. The bottom dead zone keeps a key pressed near the bottom.
 */
export function stepKey(st, depth, trig, maxRaw = MAX_TRAVEL_RAW) {
  const act = mmToRaw(trig.actuation);
  const top = mmToRaw(trig.topDead);
  const bottom = mmToRaw(trig.bottomDead);
  if (trig.mode === MODE.NORMAL) {
    const rel = Math.min(act, mmToRaw(trig.release ?? trig.actuation)); // the release point sits at or below the actuation point
    if (!st.pressed && depth > Math.max(act, top)) return press(st, depth);
    if (st.pressed && depth < rel) return release(st, depth);
    return null;
  }
  const down = mmToRaw(trig.rapidDown);
  const up = mmToRaw(trig.rapidUp);
  if (depth > act) st.armed = true;
  if (depth < 10) { // back at the top: forget everything
    st.armed = false;
    return st.pressed ? release(st, depth) : (st.low = depth, null);
  }
  if (st.pressed) {
    if (depth > st.high) st.high = depth;
    const inBottomZone = depth > maxRaw - bottom;
    if (!inBottomZone && st.high - depth >= up) return release(st, depth);
    return null;
  }
  if (depth < st.low) st.low = depth;
  const canRetrigger = trig.mode === MODE.RT_CONTINUOUS ? st.armed && depth > top : depth > act;
  if (canRetrigger && depth - st.low >= (st.armed ? down : 0) && depth > top) return press(st, depth);
  return null;
}

function press(st, depth) {
  st.pressed = true;
  st.high = depth;
  return "press";
}
function release(st, depth) {
  st.pressed = false;
  st.low = depth;
  return "release";
}
