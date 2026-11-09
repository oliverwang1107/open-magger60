// Makes the two files for the M6 hardware test (M6_TEST_GUIDE.md): a settings blob and a light scene, with the real codecs of the configurator.
//
//   node ui/tools/make_m6_test_files.mjs --out DIR [--from settings_dump.bin] [--pad-mode 1]
//
//   --from FILE      the settings the keyboard has now (python tools/magger60_config.py dump FILE --run). Profiles 1..3 are kept EXACTLY as they are;
//                    only profile 4 is replaced by the M6 test profile, so the daily profiles are untouched. Without it the factory settings are used.
//   --pad-mode 0|1   the gamepad mode in the settings (default 0 = off). It takes effect at the next plug-in, and only in the image km_m6.
//   --out DIR        writes m6_test_settings.bin (commit it with: python tools/magger60_config.py commit m6_test_settings.bin --run --confirm SAVE)
//                    and m6_test_scene.bin (python tools/magger60_config.py scene-commit m6_test_scene.bin --run --confirm LITE)
//
// The test profile (profile 4, base layer) turns these keys into M6 behaviours; every other key keeps the factory typing layer:
//   Q  DKS 1: past 1.2 mm types a, at the bottom also s (released again when the key comes back up), full release lets go of a
//   E  DKS 2: taps d at 0.8 mm, taps f at the bottom (retrigger on)
//   T  toggle key: switches the letter x on / off (like a Caps Lock for x)
//   Y  Mod-Morph: Backspace normally, Delete while a Shift is held
//   J + L  SOCD pair, last input wins        N + M  SOCD pair, the deeper key wins
//   Z / X / C / V  light keys: next effect / brighter / dimmer / lights on-off
//   W A S D  left stick (analog)             1 2  gamepad buttons 1 and 2      3 4  left / right trigger (analog)
// The scene has four indicator rules: Caps Lock (Caps key light red), a toggle key is on (T light green), layer 1 active (Fn light amber), gamepad on (Esc light cyan).
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";
import { decodeSettings, encodeSettings, defaultRawSettings } from "../js/core/settings_codec.js";
import { encodeScene, defaultScene, SRC, OFF } from "../js/core/lightscene_codec.js";
import { loadLayout } from "../js/core/layout.js";
import { CODE } from "../js/core/limits.js";

const args = process.argv.slice(2);
const opt = (name, def = null) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const out = opt("--out");
if (!out || args.includes("--help")) {
  console.log("usage: node ui/tools/make_m6_test_files.mjs --out DIR [--from settings_dump.bin] [--pad-mode 0|1]");
  process.exit(out ? 0 : 2);
}
const padMode = Number(opt("--pad-mode", "0"));
if (![0, 1].includes(padMode)) { console.error("--pad-mode must be 0 or 1"); process.exit(2); }

const layout = loadLayout();
const slotOf = (usage) => {
  const k = layout.byUsage.get(usage);
  if (!k) throw new Error(`no key with usage 0x${usage.toString(16)}`);
  return k.slot;
};
const U = { A: 0x04, B: 0x05, C: 0x06, D: 0x07, E: 0x08, F: 0x09, G: 0x0a, J: 0x0d, L: 0x0f, M: 0x10, N: 0x11, Q: 0x14, S: 0x16, T: 0x17, V: 0x19, W: 0x1a, X: 0x1b, Y: 0x1c, Z: 0x1d,
  N1: 0x1e, N2: 0x1f, N3: 0x20, N4: 0x21, BACKSPACE: 0x2a, DELETE: 0x4c, LSHIFT: 0xe1, RSHIFT: 0xe5 };

let raw;
const from = opt("--from");
if (from) {
  raw = decodeSettings(Uint8Array.from(readFileSync(from)));
} else {
  raw = defaultRawSettings(0);
  // factory typing layer for the first layer of the test profile: every key types its own letter
  for (const p of raw.profiles) for (const k of layout.keys) if (k.usage >= 0x04 && k.usage <= 0xe7) p.layers[0][k.slot] = k.usage;
}
const P = raw.profiles[3];
const set = (usage, code, a0 = 0, a1 = 0) => { const s = slotOf(usage); P.layers[0][s] = code; P.args[0][s] = [a0, a1]; return s; };
const axis = (n, neg, digital) => n | (neg << 3) | (digital << 4);

// every key of profile 4 types its own letter first (so the test profile is complete even if profile 4 was empty)
for (const k of layout.keys) if (k.usage >= 0x04 && k.usage <= 0xe7 && P.layers[0][k.slot] === 0) P.layers[0][k.slot] = k.usage;

P.dks = [
  { tAct: 120, tBot: 0, flags: 0, acts: [[U.A, 0b10_00_00_01], [U.S, 0b00_10_01_00], [0, 0], [0, 0]] },   // a: press@1 release@4, s: press@2 release@3
  { tAct: 80, tBot: 0, flags: 1, acts: [[U.D, 0b00_00_00_11], [U.F, 0b00_00_11_00], [0, 0], [0, 0]] },    // d: tap@1, f: tap@2, retrigger on
];
P.morph = [{ mask: 0x22, usage: U.DELETE, flags: 0 }];
const sJ = slotOf(U.J), sL = slotOf(U.L), sN = slotOf(U.N), sM = slotOf(U.M);
P.socd = [{ a: sJ, b: sL, mode: 1, flags: 0 }, { a: sN, b: sM, mode: 4, flags: 0 }];
set(U.Q, CODE.DKS, 0, 0);
set(U.E, CODE.DKS, 1, 0);
set(U.T, CODE.TOGGLE_KEY, U.X, 0);
set(U.Y, CODE.MOD_MORPH, U.BACKSPACE, 0);
set(U.Z, CODE.LIGHT, 1, 0);
set(U.X, CODE.LIGHT, 3, 0);
set(U.C, CODE.LIGHT, 4, 0);
set(U.V, CODE.LIGHT, 7, 0);
set(U.W, CODE.PAD_AXIS, axis(1, 1, 0), 0);   // LY negative (up)
set(U.S, CODE.PAD_AXIS, axis(1, 0, 0), 0);   // LY positive
set(U.A, CODE.PAD_AXIS, axis(0, 1, 0), 0);   // LX negative
set(U.D, CODE.PAD_AXIS, axis(0, 0, 0), 0);   // LX positive
set(U.N1, CODE.PAD_BTN, 1, 0);
set(U.N2, CODE.PAD_BTN, 2, 0);
set(U.N3, CODE.PAD_AXIS, axis(4, 0, 0), 0);  // left trigger
set(U.N4, CODE.PAD_AXIS, axis(5, 0, 0), 0);  // right trigger
raw.device = { ...(raw.device ?? { padMode: 0, filterMode: 0 }), padMode };

const blob = encodeSettings(raw);
const back = decodeSettings(blob);
if (back.profiles.length !== 4) throw new Error("self-check failed");

// the scene: indicator rules. Light numbers are the keyboard's own order (km_led.h): the light behind the Caps Lock key is 28, behind T 22, behind Fn 59, behind Esc 0.
const scene = defaultScene();
scene.rules = [
  { source: SRC.HOST_CAPS, param: 0, led: 28, flags: OFF.KEEP, color: [255, 0, 0], dim: 0 },
  { source: SRC.TOGGLE, param: 0, led: 22, flags: OFF.KEEP, color: [0, 255, 0], dim: 0 },
  { source: SRC.LAYER, param: 1, led: 59, flags: OFF.KEEP, color: [255, 160, 0], dim: 0 },
  { source: SRC.PAD, param: 0, led: 0, flags: OFF.KEEP, color: [0, 255, 255], dim: 0 },
];
const sceneBytes = encodeScene(scene);

mkdirSync(out, { recursive: true });
writeFileSync(join(out, "m6_test_settings.bin"), blob);
writeFileSync(join(out, "m6_test_scene.bin"), sceneBytes);
console.log(`m6_test_settings.bin  ${blob.length} bytes (format version ${blob[4]}), pad mode ${padMode}, profile 4 = the M6 test profile${from ? ", profiles 1..3 from " + from : ""}`);
console.log(`m6_test_scene.bin     ${sceneBytes.length} bytes, 4 indicator rules`);
