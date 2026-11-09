import test from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import zh from "../js/locales/zh-TW.js";
import en from "../js/locales/en.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "js");

function sources(dir) {
  return readdirSync(dir).flatMap((f) => {
    const p = join(dir, f);
    return statSync(p).isDirectory() ? (f === "locales" ? [] : sources(p)) : p.endsWith(".js") ? [p] : [];
  });
}

test("every English key exists in zh-TW", () => {
  for (const k of Object.keys(en)) assert.ok(k in zh, `missing in zh-TW: ${k}`);
});

test("every zh-TW key exists in English, except data-driven display names (u.*)", () => {
  for (const k of Object.keys(zh)) if (!k.startsWith("u.")) assert.ok(k in en, `missing in en: ${k}`);
});

test("placeholders match between languages", () => {
  const ph = (s) => (s.match(/\{\w+\}/g) ?? []).sort().join();
  for (const k of Object.keys(en)) assert.equal(ph(zh[k]), ph(en[k]), k);
});

test("every literal t('key') used in the source has a translation", () => {
  const used = new Set();
  for (const file of sources(root)) {
    const src = readFileSync(file, "utf8");
    for (const m of src.matchAll(/\bt\("([\w.]+)"/g)) used.add(m[1]);
  }
  assert.ok(used.size > 100);
  for (const k of used) assert.ok(k in zh && k in en, `untranslated key: ${k}`);
});

test("dynamic key families are complete", () => {
  const groups = ["letters", "numbers", "punct", "func", "nav", "keypad", "modifiers", "oneshot", "media", "system", "mouse", "special"];
  for (const p of ["timeout", "permissive", "other-key"]) assert.ok(`remap.opts.policy.${p}` in zh && `remap.opts.policy.${p}.tip` in zh && `remap.opts.policy.${p}` in en, p);
  for (const g of groups) assert.ok(`group.${g}` in zh && `group.${g}` in en, g);
  for (const p of ["live", "performance", "remap", "lighting", "profiles", "device"]) assert.ok(`nav.${p}` in zh, p);
  for (const s of ["mock", "connected", "connecting", "disconnected", "error"]) assert.ok(`status.${s}` in zh, s);
  for (const e of ["type", "range", "key", "kind", "newer", "json", "name", "color", "binding"]) assert.ok(`err.${e}` in zh, e);
  for (const k of ["default", "typing", "gaming", "speed"]) assert.ok(`perf.preset.${k}` in zh, k);
});

test("effect names exist in both languages", async () => {
  const { EFFECTS } = await import("../js/core/lighting.js");
  for (const e of EFFECTS) assert.ok(`fx.${e.id}` in zh && `fx.${e.id}` in en, e.id);
});
