// The firmware's own C code compiled to WebAssembly (ui/wasm/km_core.wasm, built by tools/build_wasm.py) against
//   * the golden vectors the real firmware produced (ui/tests/vectors/settings_vectors.json, layer_vectors.json), and
//   * the hand-written JavaScript codec (js/core/settings_codec.js, js/core/limits.js).
// If P0 (or anyone) changes km_settings.c / km_keymap.c / km_layers.c: regenerate the vectors, run `python tools/build_wasm.py`,
// and this file must stay green without edits (the only coupled spots are listed at the top of firmware/wasm/km_wasm_api.c).
import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { encodeSettings, decodeSettings, SettingsError, rawCfgProblem } from "../js/core/settings_codec.js";
import { usageCodeValid, entryProblem, optsProblem, FW_SETTINGS, FW_KEY_SLOTS } from "../js/core/limits.js";
import { crc32 } from "../js/device/config_protocol.js";
import { loadCore } from "../wasm/km_wasm.js";

const REPO = new URL("../../", import.meta.url);
const V = JSON.parse(readFileSync(new URL("./vectors/settings_vectors.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const bytes = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const core = await loadCore();

test("the committed km_core.wasm was built from the C sources that are in the tree (else: python tools/build_wasm.py)", () => {
  const meta = JSON.parse(readFileSync(new URL("ui/wasm/km_core.meta.json", REPO), "utf8"));
  const lines = [];
  for (const [path, want] of Object.entries(meta.sources).sort(([a], [b]) => (a < b ? -1 : 1))) {
    const h = createHash("sha256").update(readFileSync(new URL(path, REPO)).toString("latin1").replaceAll("\r\n", "\n"), "latin1").digest("hex");
    assert.equal(h, want, `${path} changed after km_core.wasm was built: rebuild with python tools/build_wasm.py`);
    lines.push(`${path} ${h}\n`);
  }
  assert.equal(createHash("sha256").update(lines.join("")).digest("hex"), meta.source_hash);
  const headers = readdirSync(new URL("firmware/core/include/km/", REPO)).filter((n) => n.endsWith(".h")).map((n) => `firmware/core/include/km/${n}`);
  for (const h of headers) assert.ok(meta.sources[h], `${h} is new since km_core.wasm was built: rebuild with python tools/build_wasm.py`);
  assert.equal(createHash("sha256").update(readFileSync(new URL("ui/wasm/km_core.wasm", REPO))).digest("hex"), meta.wasm_sha256);
});

test("module shape: same dimensions, size and version as the JavaScript model", () => {
  assert.equal(core.size, V.size);
  assert.deepEqual(core.dims, { profiles: 4, layers: 4, slots: 76, args: 2 });
  assert.equal(core.version, FW_SETTINGS.version);
  assert.equal(core.version, 3, "settings format version 3 (M6 P0)");
});

test("CRC32 (check value and a long random string) equals the JS crc32", () => {
  assert.equal(core.crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
  const r = Uint8Array.from({ length: 5000 }, (_, i) => (i * 2654435761 >>> 7) & 255);
  assert.equal(core.crc32(r), crc32(r) >>> 0);
});

test("the factory default blob is byte-identical to the firmware vector and to the JS codec", () => {
  assert.equal(hex(core.defaultBlob()), V.default);
  assert.equal(hex(encodeSettings(decodeSettings(bytes(V.default)))), V.default);
});

for (const name of ["default_table", "default_flattened"]) {
  test(`${name}: decode then re-encode in wasm returns the same bytes`, () => {
    assert.equal(hex(core.decode(bytes(V[name]))), V[name]);
  });
}

for (const c of V.cases) {
  test(`case ${c.name}: wasm encode == firmware vector == JS codec (or all refuse)`, () => {
    core.decode(bytes(V.default));
    core.setActive(c.active);
    const js = decodeSettings(bytes(V.default));
    js.active = c.active;
    for (const [p, slot, cfg] of c.cfgs) { core.setCfg(p, slot, cfg); js.profiles[p].keys[slot] = { ...cfg }; }
    for (const [p, l, slot, v, a0, a1] of c.usages) { core.setEntry(p, l, slot, v, a0, a1); js.profiles[p].layers[l][slot] = v; js.profiles[p].args[l][slot] = [a0, a1]; }
    for (const [p, tapTerm, oneshot, policy] of c.opts) { core.setOpts(p, { tapTerm, oneshot, policy }); js.profiles[p].opts = { tapTerm, oneshot, policy }; }
    const w = core.encode();
    if (c.encoded === null) {
      assert.equal(w, null, "wasm refuses like the firmware");
      assert.equal(core.valid(), false);
      assert.throws(() => encodeSettings(js), SettingsError, "JS refuses too");
    } else {
      assert.equal(hex(w), c.encoded);
      assert.equal(core.valid(), true);
      assert.equal(hex(encodeSettings(js)), c.encoded);
    }
  });
}

test("usage-byte rule: all 256 values, wasm == firmware vector == JS", () => {
  for (let u = 0; u < 256; u++) {
    assert.equal(core.usageValid(u), V.usage_valid[u] === 1, `usage 0x${u.toString(16)}`);
    assert.equal(core.usageValid(u), usageCodeValid(u), `JS usage 0x${u.toString(16)}`);
  }
  assert.equal(core.usageValid(256), false);
});

test("table-entry rule: every probe (including all 256 bare codes), wasm == firmware == JS", () => {
  for (const p of V.entry_probes) {
    assert.equal(core.entryValid(p.code, p.arg0, p.arg1), p.valid, p.name);
    assert.equal(entryProblem(p.code, p.arg0, p.arg1) === null, p.valid, `JS ${p.name}`);
  }
});

test("layer-option rule: every probe, wasm == firmware == JS", () => {
  for (const p of V.opts_probes) {
    const o = { tapTerm: p.tapTerm, oneshot: p.oneshot, policy: p.policy };
    assert.equal(core.optsValid(o), p.valid, p.name);
    assert.equal(optsProblem(o) === null, p.valid, `JS ${p.name}`);
  }
});

test("key-config rule: every boundary probe, wasm == firmware == JS", () => {
  for (const p of V.cfg_probes) {
    assert.equal(core.cfgValid(p.cfg), p.valid, p.name);
    assert.equal(rawCfgProblem(p.cfg) === null, p.valid, `JS ${p.name}`);
  }
});

test("version 1 blobs: migrated or refused like the firmware (and like the JS codec)", () => {
  for (const m of V.migrations) {
    const w = core.decode(bytes(m.v1));
    if (m.migrated === null) {
      assert.equal(w, null, m.name);
      assert.throws(() => decodeSettings(bytes(m.v1)), SettingsError, `JS ${m.name}`);
    } else {
      assert.equal(hex(w), m.migrated, m.name);
      assert.equal(hex(encodeSettings(decodeSettings(bytes(m.v1)))), m.migrated, `JS ${m.name}`);
    }
  }
});

function mutate(m) {
  let b = bytes(V.default);
  if (m.fill) b = new Uint8Array(b.length).fill(parseInt(m.fill, 16));
  for (const [off, h] of m.patches) b.set(bytes(h), off);
  if (m.recrc) {
    const c = crc32(b.subarray(0, b.length - 4));
    b.set([c & 255, (c >>> 8) & 255, (c >>> 16) & 255, c >>> 24], b.length - 4);
  }
  if (m.truncate !== undefined) b = b.slice(0, m.truncate);
  if (m.extend) b = Uint8Array.from([...b, ...new Array(m.extend).fill(0)]);
  return b;
}

for (const m of V.mutations) {
  test(`corrupted blob ${m.name}: wasm ${m.accepted ? "accepts" : "refuses"} like the firmware and the JS codec`, () => {
    const b = mutate(m);
    const w = core.decode(b);
    if (m.accepted) {
      assert.equal(hex(w), hex(b));
      assert.equal(hex(encodeSettings(decodeSettings(b))), hex(b));
    } else {
      assert.equal(w, null);
      assert.throws(() => decodeSettings(b), SettingsError);
    }
  });
}

test("a refused decode leaves the held settings unchanged", () => {
  core.decode(bytes(V.default));
  core.setActive(2);
  assert.equal(core.decode(mutate(V.mutations.find((m) => m.name === "bad_magic"))), null);
  assert.equal(core.encode()[8], 2, "active profile still 2");
});

// ---- settings format version 3 (M6 task P0): the section format, DKS / mod-morph / SOCD / gamepad curves / device options ----
const V3 = V.v3;
const richBlob = bytes(V3.cases.find((c) => c.name === "v3_everything").encoded);

test("v3: the slots that carry a key are the ones the firmware says (wasm, vector and JS)", () => {
  const slots = core.keySlots();
  assert.equal(slots.length, V3.keys);
  for (let i = 0; i < 76; i++) assert.equal(slots.includes(i), V3.key_slots_flags[i] === "1", `slot ${i}`);
  assert.deepEqual(slots, [...FW_KEY_SLOTS]);
});

test("v3: the default settings written as sections decode to the default, which is then written as version 2 (nothing needs 3)", () => {
  assert.equal(hex(core.decode(bytes(V3.default_v3))), V.default);
  assert.equal(core.needsV3(), false);
});

for (const c of V3.cases) {
  test(`v3 case ${c.name}: wasm writer == firmware writer (same version byte), or both refuse`, () => {
    core.runCmds(c.cmds, c.active);
    const w = core.encode();
    if (c.encoded === null) {
      assert.equal(w, null);
      assert.equal(core.valid(), false);
    } else {
      assert.equal(hex(w), c.encoded);
      assert.equal(core.valid(), true);
      assert.equal(core.needsV3(), w[4] === 3, "needs_v3 says what the writer chose");
      assert.equal(hex(core.decode(w)), c.encoded, "and it reads back to the same bytes");
    }
  });
}

function patched(base, m) {
  let b = Uint8Array.from(base);
  if (m.fill) b = new Uint8Array(b.length).fill(parseInt(m.fill, 16));
  for (const [off, h] of m.patches) b.set(bytes(h), off);
  if (m.recrc) {
    const c = crc32(b.subarray(0, b.length - 4));
    b.set([c & 255, (c >>> 8) & 255, (c >>> 16) & 255, c >>> 24], b.length - 4);
  }
  if (m.truncate !== undefined) {
    b = b.slice(0, m.truncate);
    if (m.fixlen) {
      if (b.length >= 8) { b[6] = b.length & 255; b[7] = b.length >> 8; }
      if (b.length >= 4) { const c = crc32(b.subarray(0, b.length - 4)); b.set([c & 255, (c >>> 8) & 255, (c >>> 16) & 255, c >>> 24], b.length - 4); }
    }
  }
  if (m.extend) b = Uint8Array.from([...b, ...new Array(m.extend).fill(0)]);
  return b;
}

function sameAsFirmware(b, reencoded, name) {
  const w = core.decode(b);
  if (reencoded === null) assert.equal(w, null, `${name}: the firmware refuses this`);
  else assert.equal(hex(w), reencoded, `${name}: read and written again`);
}

for (const m of V3.mutations) {
  test(`v3 damaged blob ${m.name}: wasm ${m.reencoded === null ? "refuses" : "accepts"} like the firmware`, () => sameAsFirmware(patched(richBlob, m), m.reencoded, m.name));
}
for (const m of V3.blobs) {
  test(`v3 built blob ${m.name}: wasm ${m.reencoded === null ? "refuses" : "accepts"} like the firmware`, () => sameAsFirmware(bytes(m.blob), m.reencoded, m.name));
}

test("v3: entry rules for every (code, arg0, arg1) of the codes that matter, in both versions: wasm == firmware == JS", () => {
  const bit = (row, a0, a1) => (parseInt(row[a0 * 64 + (a1 >> 2)], 16) >> (a1 & 3)) & 1;
  let n = 0;
  for (const [code, row] of Object.entries(V3.entry_maps_v3)) {
    for (let a0 = 0; a0 < 256; a0++) for (let a1 = 0; a1 < 256; a1++) {
      const want = bit(row, a0, a1) === 1;
      if (core.entryValid(Number(code), a0, a1) !== want) assert.fail(`wasm: code ${code} args ${a0},${a1}: firmware says ${want}`);
      n++;
    }
  }
  for (const [code, row] of Object.entries(V3.entry_maps_v2)) {
    for (let a0 = 0; a0 < 256; a0++) for (let a1 = 0; a1 < 256; a1++) {
      if (core.entryValidV2(Number(code), a0, a1) !== (bit(row, a0, a1) === 1)) assert.fail(`wasm v2 rule: code ${code} args ${a0},${a1}`);
    }
  }
  assert.equal(n, Object.keys(V3.entry_maps_v3).length * 65536);
});

// ---- layers: the real km_layers.c natively (layer_vectors.json) against the same C code compiled to wasm ----
const LV = existsSync(new URL("./vectors/layer_vectors.json", import.meta.url))
  ? JSON.parse(readFileSync(new URL("./vectors/layer_vectors.json", import.meta.url), "utf8")) : null;

test("layer vectors exist (python firmware/tools/gen_layer_vectors.py)", () => assert.ok(LV && LV.scenarios.length >= 14));

for (const s of LV?.scenarios ?? []) {
  test(`layers ${s.name}: ${s.steps.length} steps, wasm output equals the native C output on every step`, () => {
    core.decode(bytes(V.default));
    for (const [slot, l, ...rest] of s.edits.map(([layer, slot, code, a0, a1]) => [slot, layer, code, a0, a1])) core.setEntry(0, l, slot, ...rest);
    if (s.opts) core.setOpts(0, { tapTerm: s.opts[0], oneshot: s.opts[1], policy: s.opts[2] });
    s.morph.forEach(([mask, usage, flags], i) => core.setMorph(0, i, { mask, usage, flags }));
    assert.equal(core.layersLoad(0), s.load === 1);
    s.steps.forEach(([now, down], i) => {
      const o = core.layersUpdate(down, now);
      const got = hex([o.kbd.length, o.layer, o.toggled, o.armed, o.profileRequest, o.pending ? 1 : 0, o.consumer & 255, o.consumer >> 8, o.toggleKeys, o.suppressed, ...o.kbd]);
      assert.equal(got, s.expect[i], `step ${i} (t=${now}, down=${down.join(",")})`);
      const lock = down.map((slot) => { const e = core.layersLocked(slot); return e ? [e.code, e.arg0, e.arg1].map((v) => v.toString(16).padStart(2, "0")).join("") : "-"; }).join(" ");
      assert.equal(lock, s.locked[i], `locked entries at step ${i}`);
    });
  });
}

test("layers: Fn held turns the default 'V' key into a profile key, releasing it asks for the switch", () => {
  core.decode(bytes(V.default));
  assert.ok(core.layersLoad(0));
  assert.deepEqual(core.layersUpdate([14], 0).layer, 1);
  const down = core.layersUpdate([14, 7], 20);
  assert.equal(down.profileRequest, 0, "nothing while down");
  const up = core.layersUpdate([14], 40);
  assert.equal(up.profileRequest, 4, "slot 7 is KM_CODE_PROFILE_4 on layer 1");
});
