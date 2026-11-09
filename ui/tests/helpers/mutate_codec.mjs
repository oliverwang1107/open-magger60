// Mutation pass for the settings codec (M6 task P0): change one comparison, bound, offset or flag at a time in a scratch copy of ui/,
// run the codec tests against the firmware vectors, and require the tests to FAIL for every mutation.
//
//   node ui/tests/helpers/mutate_codec.mjs            # all mutations
//   node ui/tests/helpers/mutate_codec.mjs 3 7        # only mutations 3 and 7
//
// A mutation that the tests do not notice ("SURVIVED") means a missing test or a missing vector: add one. The real tree is never modified.
// (The firmware side has the same kind of pass: firmware/tools/mutate_settings.py.)
import { cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const UI = fileURLToPath(new URL("../../", import.meta.url));
const SET = "js/core/settings_codec.js";
const LIM = "js/core/limits.js";
const PRO = "js/core/profile_codec.js";

// Mutations of a check that a second check repeats (a section that reaches one byte into the CRC is also refused by its exact-size rule, scope 4
// and an unknown required id are also refused when the model is validated) change nothing observable and are not listed.
const M = [
  ["decode: 65 sections accepted", SET, "if (n > F.v3MaxSections)", "if (n > F.v3MaxSections + 1)"],
  ["decode: reserved header byte 13 not checked", SET, "if (b[13] !== 0 || b[14] !== 0 || b[15] !== 0)", "if (b[14] !== 0 || b[15] !== 0)"],
  ["decode: key count 76 accepted", SET, "b[11] !== F.keys) {", "b[11] !== F.keys && b[11] !== F.slots) {"],
  ["decode: duplicate sections accepted", SET, "if (seen.has(key)) fail(", "if (false) fail("],
  ["decode: a per-profile section with the global scope", SET, "if (scope >= F.profiles) fail(\"section\", `section 0x${id.toString(16)} needs a profile`);", ""],
  ["decode: device section with a profile scope", SET, "if (scope !== F.scopeGlobal) fail(\"section\", \"the device section is global\");", ""],
  ["decode: bytes before the CRC accepted", SET, "if (p !== end) fail(", "if (p > end) fail("],
  ["decode: DKS count not compared with the length", SET, "d.length !== 1 + d[0] * SECTION_BYTES.dksRec", "d.length < 1 + d[0] * SECTION_BYTES.dksRec"],
  ["decode: DKS reserved bytes ignored", SET, "if (d[t + 5] || d[t + 6] || d[t + 7])", "if (d[t + 5] || d[t + 6])"],
  ["decode: Mod-Morph reserved byte ignored", SET, "if (m[4 * i + 3]) fail(", "if (false) fail("],
  ["decode: gamepad reserved bytes ignored", SET, "if (a.subarray(19, 24).some((v) => v !== 0))", "if (a.subarray(19, 23).some((v) => v !== 0))"],
  ["decode: device reserved bytes ignored", SET, "if (dev.subarray(2).some((v) => v !== 0))", "if (dev.subarray(3).some((v) => v !== 0))"],
  ["decode: layer option reserved byte ignored (v3)", SET, "if (ly[5] !== 0) fail(", "if (false) fail("],
  ["decode: a blob over 8176 bytes accepted", SET, "b.length > F.v3MaxSize) fail(", "b.length > F.v3MaxSize + 1) fail("],
  ["decode: key configs read by position, not by key slot", SET, "q.keys[slot] = getCfg(x.keycfg, i * F.keyCfgBytes);", "q.keys[i] = getCfg(x.keycfg, i * F.keyCfgBytes);"],
  ["decode: version 2 blob may hold format 3 entries", SET, "if (entryNeedsV3(q.layers[l][slot], q.args[l][slot][0], q.args[l][slot][1])) {", "if (false) {"],
  ["encode: key configs by position", SET, "FW_KEY_SLOTS.forEach((slot, i) => putCfg(kc, i * F.keyCfgBytes, pr.keys[slot]));", "FW_KEY_SLOTS.forEach((slot, i) => putCfg(kc, i * F.keyCfgBytes, pr.keys[i]));"],
  ["encode: DKS usage and events swapped", SET, "d[q + 8 + 2 * k] = a[0]; d[q + 9 + 2 * k] = a[1];", "d[q + 8 + 2 * k] = a[1]; d[q + 9 + 2 * k] = a[0];"],
  ["encode: SOCD mode and flags swapped", SET, "c[4 * i + 2] = r.mode; c[4 * i + 3] = r.flags;", "c[4 * i + 2] = r.flags; c[4 * i + 3] = r.mode;"],
  ["encode: gamepad option bytes shifted", SET, "a[16] = pad.deadzone; a[17] = pad.circular; a[18] = pad.smooth;", "a[16] = pad.circular; a[17] = pad.deadzone; a[18] = pad.smooth;"],
  ["encode: device bytes swapped", SET, "d[0] = dev.padMode; d[1] = dev.filterMode;", "d[0] = dev.filterMode; d[1] = dev.padMode;"],
  ["encode: always version 3", SET, "return needsV3(s) ? encodeSettingsV3(s, rule) : encodeSettingsV2(s, rule);", "return encodeSettingsV3(s, rule);"],
  ["encode: always version 2 when only entries need 3", SET, "for (let l = 0; l < F.layers; l++) {\n      for (let slot = 0; slot < F.slots; slot++) if (entryNeedsV3(", "for (let l = 0; l < 0; l++) {\n      for (let slot = 0; slot < F.slots; slot++) if (entryNeedsV3("],
  ["needsV3: SOCD pairs ignored", SET, "|| socdOf(pr).length ||", "||"],
  ["needsV3: device ignored", SET, "if (!deviceIsDefault(deviceOf(s)) || unknownOf(s).length > 0) return true;", "if (unknownOf(s).length > 0) return true;"],
  ["needsV3: unknown sections ignored", SET, "if (!deviceIsDefault(deviceOf(s)) || unknownOf(s).length > 0) return true;", "if (!deviceIsDefault(deviceOf(s))) return true;"],
  ["model: a DKS key may point past the records", SET, "(code === CODE.DKS && a0 >= dks.length)", "(code === CODE.DKS && a0 > dks.length)"],
  ["model: a Mod-Morph key may point past the records", SET, "(code === CODE.MOD_MORPH && a1 >= morph.length)", "(code === CODE.MOD_MORPH && a1 > morph.length)"],
  ["model: a key in two SOCD pairs", SET, "if (used.has(pair.a) || used.has(pair.b))", "if (used.has(pair.a))"],
  ["model: 5 SOCD pairs", SET, "socd.length > FW_M6.socdMax", "socd.length > FW_M6.socdMax + 1"],
  ["limits: DKS start point floor 39", LIM, "r.tAct < FW_M6.dksActMin ||", "r.tAct < FW_M6.dksActMin - 1 ||"],
  ["limits: DKS gap 39", LIM, "bot < r.tAct + FW_M6.dksGapMin", "bot < r.tAct + FW_M6.dksGapMin - 1"],
  ["limits: DKS default bottom 311", LIM, "dksBotDefault: 310", "dksBotDefault: 311"],
  ["limits: DKS unused action may have events", LIM, "if (a[0] === 0 ? a[1] !== 0 : !isUsage(a[0])) return \"action\";", "if (a[0] === 0 ? false : !isUsage(a[0])) return \"action\";"],
  ["limits: Mod-Morph empty mask", LIM, "r.mask === 0 ? \"mask\" :", ""],
  ["limits: SOCD same slot twice", LIM, "|| r.a === r.b ? \"slots\"", "? \"slots\""],
  ["limits: SOCD slot without a key", LIM, "!slotHasKey(r.a) || !slotHasKey(r.b)", "r.a > 75 || r.b > 75"],
  ["limits: pad curve span 5", LIM, "c.end < c.start + FW_M6.padSpanMin", "c.end < c.start + FW_M6.padSpanMin + 1"],
  ["limits: pad dead zone 49", LIM, "p.deadzone > FW_M6.padDeadzoneMax", "p.deadzone >= FW_M6.padDeadzoneMax"],
  ["limits: default pad end 63", LIM, "padEndDefault: 64", "padEndDefault: 63"],
  ["limits: default dead zone 11", LIM, "deadzone: 10, circular: 1", "deadzone: 11, circular: 1"],
  ["limits: trigger axis may go negative", LIM, "!(axis >= 4 && (a & 0x08) !== 0)", "!(axis > 4 && (a & 0x08) !== 0)"],
  ["limits: axis argument bit 5 accepted", LIM, "(a & ~0x1f) === 0 &&", ""],
  ["limits: Mod-Morph index 8", LIM, "a1 < FW_M6.morphMax ? null", "a1 <= FW_M6.morphMax ? null"],
  ["limits: DKS entry index 8", LIM, "a0 < FW_M6.dksMax && a1 === 0", "a0 <= FW_M6.dksMax && a1 === 0"],
  ["limits: gamepad start point 69", LIM, "a1 <= FW_M6.padBtnStartMax", "a1 <= FW_M6.padBtnStartMax + 1"],
  ["limits: light action 8", LIM, "a0 <= FW_M6.lightActLast && a1 === 0", "a0 <= FW_M6.lightActLast + 1 && a1 === 0"],
  ["limits: tap-hold may hold the tap key", LIM, "isUsage(a1) && a1 !== a0", "isUsage(a1)"],
  ["limits: the format 2 rule allows plain-key holds", LIM, "rule.v3 && isUsage(a1)", "isUsage(a1)"],
  ["limits: no-key slot list loses a slot", LIM, "[15, 17, 20, 24, 33,", "[17, 20, 24, 33,"],
  ["profile codec: opaque entries lose the second argument", PRO, "args: [(i >> 8) & 0xff, i & 0xff]", "args: [(i >> 8) & 0xff, 0]"],
  ["profile codec: device options not carried", PRO, "device: structuredClone(ws.m6?.device ?? defaultDevice())", "device: defaultDevice()"],
  ["profile codec: DKS records not carried", PRO, "dks: structuredClone(profile.m6?.dks ?? []),", "dks: [],"],
];

function runTests(dir) {
  const r = spawnSync(process.execPath, ["--test", "tests/settings-codec.test.mjs", "tests/layers-v2.test.mjs", "tests/profile-codec.test.mjs"], { cwd: dir, encoding: "utf8" });
  return r.status === 0;
}

const only = new Set(process.argv.slice(2).map(Number));
const tmp = mkdtempSync(join(tmpdir(), "km_mut_ui_"));
let survived = 0;
try {
  cpSync(UI, tmp, { recursive: true, filter: (src) => !src.includes("node_modules") });
  // the tests read the firmware vectors from ui/tests/vectors, which came along
  if (!runTests(tmp)) { console.log("baseline: FAIL (cannot judge mutations)"); process.exit(2); }
  console.log("baseline: pass");
  M.forEach(([name, rel, from, to], i) => {
    const n = i + 1;
    if (only.size && !only.has(n)) return;
    const path = join(tmp, rel);
    const src = readFileSync(path, "utf8");
    const text = src.replace(/\r\n/g, "\n");
    const count = text.split(from).length - 1;
    if (count !== 1) { console.log(`${String(n).padStart(2)} ${name.padEnd(62)} NOT APPLICABLE (${count} matches)`); survived++; return; }
    writeFileSync(path, text.replace(from, () => to));
    const killed = !runTests(tmp);
    writeFileSync(path, src);
    console.log(`${String(n).padStart(2)} ${name.padEnd(62)} ${killed ? "killed" : "SURVIVED"}`);
    if (!killed) survived++;
  });
  console.log("survivors:", survived);
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
process.exit(survived ? 1 : 0);
