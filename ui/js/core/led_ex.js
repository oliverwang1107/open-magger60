// The extended form of LED_GET / LED_SET (CONFIG_PROTOCOL.md, 0x20 / 0x21): the same six light bytes plus the idle fade-out time,
// a SAVE flag that keeps the configuration in the keyboard's flash, and a status block. Pure functions, no I/O.
//   LED_GET with one argument byte 1  -> 12 bytes: effect brightness speed r g b | idle_s u16 LE | status | last_result | saves_ok | saves_failed
//   LED_SET with nine argument bytes  -> the six bytes, idle_s u16 LE, flags (bit0 = SAVE); the reply is the 12 bytes above
// Per key profile (feature bit 11, LIGHT_PROFILE_NOTES.md): LED_GET with two argument bytes (1, profile) and LED_SET with ten (the nine +
// profile) address one of the four profiles (0..3) or the profile in force (255); the reply is the same 12 bytes about that profile.
// An older firmware answers both forms BAD_LENGTH; a build that cannot keep the values answers SAVE with UNSUPPORTED (and applies nothing).
import { decodeLed, encodeLed } from "./led.js";

export const LED_STATUS = Object.freeze({ SAVE_PENDING: 1, SAVE_FAILED: 2, LOADED: 4, IDLE_OFF: 8, PERSISTENT: 16, PROFILE_OWN: 32 });
export const LED_PROFILE_ACTIVE = 255; // profile argument: the profile the keyboard is in
export const LED_FLAG_SAVE = 1;
export const LED_IDLE_DEFAULT_S = 600; // the firmware's default: lights fade out after 10 minutes without a key press
export const LED_IDLE_MAX_S = 0xffff;

const isIdle = (v) => Number.isInteger(v) && v >= 0 && v <= LED_IDLE_MAX_S;

/** A profile argument: an index 0..3 or LED_PROFILE_ACTIVE. */
export const isLedProfile = (p) => Number.isInteger(p) && ((p >= 0 && p <= 3) || p === LED_PROFILE_ACTIVE);

/** The argument bytes of the extended LED_SET (nine; ten with `profile`). Throws RangeError for anything the firmware would refuse. */
export function encodeLedEx(s, { idleS, save = false, profile = null } = {}) {
  if (!isIdle(idleS)) throw new RangeError("idle time must be a whole number of seconds, 0..65535");
  if (profile !== null && profile !== undefined && !isLedProfile(profile)) throw new RangeError("profile must be 0..3 or 255 (the profile in force)");
  const six = encodeLed(s);
  const withProfile = profile !== null && profile !== undefined;
  const out = new Uint8Array(withProfile ? 10 : 9);
  out.set(six, 0);
  out[6] = idleS & 0xff;
  out[7] = idleS >> 8;
  out[8] = save ? LED_FLAG_SAVE : 0;
  if (withProfile) out[9] = profile;
  return out;
}

/** The 12 reply bytes -> {effect, brightness, speed, r, g, b, idleS, status, lastResult, savesOk, savesFailed}. */
export function decodeLedEx(p) {
  if (!p || p.length !== 12) throw new RangeError(`extended light reply must be 12 bytes, not ${p?.length ?? 0}`);
  return {
    ...decodeLed(p.subarray(0, 6)),
    idleS: p[6] | (p[7] << 8),
    status: p[8], lastResult: p[9], savesOk: p[10], savesFailed: p[11],
  };
}

/** The status byte as booleans. */
export function ledStatusFlags(status) {
  return {
    pending: !!(status & LED_STATUS.SAVE_PENDING),
    failed: !!(status & LED_STATUS.SAVE_FAILED),
    restored: !!(status & LED_STATUS.LOADED),
    idleOff: !!(status & LED_STATUS.IDLE_OFF),
    persistent: !!(status & LED_STATUS.PERSISTENT),
    profileOwn: !!(status & LED_STATUS.PROFILE_OWN), // per-profile reply: this profile has lights of its own (clear = it follows the shared fallback)
  };
}
