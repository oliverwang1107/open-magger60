// Client of the read-only scope channel (command 0xC5, firmware/hid/include/km/km_scope.h; host twin: tools/magger60_scope.py).
// The firmware publishes ONE snapshot at a time and rotates through its tags every few milliseconds, so asking for a given tag
// means asking again until it is the published one. A snapshot is read in pages of up to 25 values and kept only when tag and
// sequence number are the same in every page.
import { buildScopeRequest, decodeScopeReply, ConfigError, SCOPE_PAGE_VALUES } from "./config_protocol.js";

export const SCOPE_TAG = Object.freeze({ SUMS: 0x0003, ADC_DIAG: 0x0100, ENGINE: 0x0200, DEPTHS: 0x0201, HEALTH: 0x0300, KEYMASK: 0x0301 });

export class ScopeClient {
  #t;
  #verified;

  /** `isVerified()` tells whether the cmd 3 build string was checked (ConfigClient.verified): without it nothing is sent. */
  constructor(transport, isVerified) {
    this.#t = transport;
    this.#verified = isVerified;
  }

  async page(start, wantTag = 0) {
    if (!this.#verified()) throw new ConfigError("firmware not verified yet; call verify() first", null, "not_verified");
    return decodeScopeReply(await this.#t.exchange(buildScopeRequest(start, 56, wantTag)));
  }

  /**
   * The snapshot with tag `wantTag` as {tag, seq, values}. Throws ConfigError with code "scope_none" when the keyboard has not
   * published anything, "scope_tag" when it never publishes that tag (within `tries` asks), "scope_torn" when it keeps changing
   * under the read.
   */
  async snapshot(wantTag, { tries = 40 } = {}) {
    let torn = 0;
    for (let i = 0; i < tries; i++) {
      const p0 = await this.page(0, wantTag);
      if (p0.tag === 0 || p0.seq === 0) throw new ConfigError("the keyboard has not published anything on the scope channel yet", null, "scope_none");
      if (p0.tag !== wantTag) continue;
      const values = [...p0.values];
      let ok = true;
      while (values.length < p0.count) {
        const p = await this.page(values.length, wantTag);
        if (p.tag !== p0.tag || p.seq !== p0.seq || p.count !== p0.count || p.start !== values.length || !p.values.length) { ok = false; break; }
        values.push(...p.values);
      }
      if (ok) return { tag: p0.tag, seq: p0.seq, values: Uint16Array.from(values) };
      torn++;
    }
    const tag = `0x${wantTag.toString(16).padStart(4, "0")}`;
    throw new ConfigError(torn ? `scope tag ${tag} keeps changing while it is read` : `the keyboard does not publish scope tag ${tag}`, null, torn ? "scope_torn" : "scope_tag");
  }
}

export { SCOPE_PAGE_VALUES };

// ---- decoders of the tags the UI shows ----

const CAL_RESULT = ["ok", "too_few_frames", "keys_held", "unstable", "implausible"]; // km_engine_cal_result_t order (ENGINE_NOTES.md)

/** Tag 0x0200, 24 values (m4_main.c). */
export function parseEngineDiag(v) {
  const u32 = (lo, hi) => (v[lo] | (v[hi] << 16)) >>> 0;
  const pressed = [];
  for (let i = 12; i <= 17; i++) if (v[i] !== 0xff && v[i] !== undefined) pressed.push(v[i]);
  return {
    calResultRaw: v[0], calibrating: v[0] === 0xffff, calResult: v[0] === 0xffff ? null : CAL_RESULT[v[0]] ?? `code ${v[0]}`,
    calAttempts: v[1], keysDown: v[2], glitches: u32(3, 4), lastUpdateCycles: v[5], maxUpdateCycles: v[6], reports: u32(7, 8),
    bootProtocol: v[9] === 1, heldBack: u32(10, 11), pressedSlots: pressed, armed: v[18] === 1, frames: u32(19, 20),
  };
}

export const FAULTS = [[0x01, "scan_stall"], [0x02, "dma_error"], [0x04, "cal_lost"], [0x08, "usb_suspended"], [0x10, "engine_overrun"]];
const RESET_NAMES = [[0x80, "lowpower"], [0x40, "wwdg"], [0x20, "iwdg"], [0x10, "soft"], [0x08, "por"], [0x04, "pin"]];

/** Tag 0x0300, 24 values (km_health_pack_scope, M5 task T-E). */
export function parseHealth(v) {
  const u32 = (lo, hi) => (v[lo] | (v[hi] << 16)) >>> 0;
  const flags = v[0] & 0xff, cause = v[0] >> 8;
  return {
    faults: FAULTS.filter(([bit]) => flags & bit).map(([, n]) => n), faultFlags: flags,
    resetCause: RESET_NAMES.find(([bit]) => cause & bit)?.[1] ?? "none", resetCauseRaw: cause,
    frames: u32(1, 2), scanStalls: v[3], dmaErrors: v[4], scanRestarts: v[5], engineMaxCycles: v[6], engineOverBudget: v[7],
    engineOverruns: v[8], usbResets: v[9], usbSuspends: v[10], usbResumes: v[11], reportRetries: v[12], reportResyncs: v[13],
    glitches: v[14], calibrations: v[15], calibrationRefused: v[16],
    extra: { reportsSent: v[17], heartbeats: v[18], endpointRecoveries: v[19], previousFaults: v[20] === 0xffff ? null : v[20], bootCount: v[21], uptimeS: v[22], longestUpdateWithIrq: v[23] },
  };
}

/** Tag 0x0301, 8 values: bitmask of the slots the keyboard is reporting as pressed (+ sequence, faults, undelivered flag). */
export function parseKeyMask(v, slots = 76) {
  const down = [];
  for (let s = 0; s < slots; s++) if (v[s >> 4] & (1 << (s & 15))) down.push(s);
  return { down, seq: v[5], faultFlags: v[6] & 0xff, undelivered: v[7] === 1 };
}
