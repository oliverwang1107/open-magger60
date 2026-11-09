// Navigation / router registration: every page in PAGES has a module, an icon, a name in both languages and exports mount();
// every firmware feature that exists today has a page; fw key actions get a cap label and pass the binding check.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import zh from "../js/locales/zh-TW.js";
import en from "../js/locales/en.js";

globalThis.window ??= new EventTarget();
globalThis.location ??= { hash: "" };
const { PAGES, pageHasKeyboard } = await import("../js/state.js");
const { isValidBinding, capLabel, cleanBinding } = await import("../js/core/hid-usages.js");
await import("../js/core/behaviors.js");

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "js");
const read = (f) => readFileSync(join(root, f), "utf8");

test("the pages, in navigation order", () => {
  assert.deepEqual(PAGES, ["live", "performance", "remap", "behaviors", "pad", "lighting", "animation", "profiles", "device"]);
});

test("main.js has a lazy module for exactly the registered pages", () => {
  const src = read("main.js");
  const block = src.match(/const PAGE_MODULES = \{([\s\S]*?)\n\};/)[1];
  const keys = [...block.matchAll(/^\s+(\w+): \(\) => import\("\.\/pages\/(\w+)\.js"\)/gm)];
  assert.deepEqual(keys.map((m) => m[1]), PAGES, "same pages, same order");
  for (const [, key, file] of keys) assert.equal(file, key, `page ${key} loads pages/${file}.js`);
});

test("every page module can be loaded and exports mount()", async () => {
  for (const p of PAGES) {
    const mod = await import(`../js/pages/${p}.js`);
    assert.equal(typeof mod.mount, "function", p);
  }
});

test("every page has an icon, a name and a screen-reader friendly title in zh-TW and English", () => {
  const dom = read("dom.js");
  const icons = dom.match(/const ICONS = \{([\s\S]*?)\n\};/)[1];
  for (const p of PAGES) {
    assert.match(icons, new RegExp(`^\\s+${p}: "M`, "m"), `icon for ${p}`);
    assert.ok(zh[`nav.${p}`] && en[`nav.${p}`], `nav.${p} in both languages`);
  }
  const labels = new Set(PAGES.map((p) => zh[`nav.${p}`]));
  assert.equal(labels.size, PAGES.length, "no two pages share a name");
  const enLabels = new Set(PAGES.map((p) => en[`nav.${p}`]));
  assert.equal(enLabels.size, PAGES.length);
});

test("the pages that show the keyboard canvas", () => {
  for (const p of ["live", "performance", "remap", "behaviors", "pad", "lighting", "animation"]) assert.equal(pageHasKeyboard(p), true, p);
  for (const p of ["profiles", "device"]) assert.equal(pageHasKeyboard(p), false, p);
});

test("the navigation is a real list of links with aria-current (shell source)", () => {
  const src = read("main.js");
  assert.match(src, /h\("nav\.rail", \{ "aria-label": t\("nav\.aria"\)/);
  assert.match(src, /aria-current/);
  assert.match(src, /href: `#\/\$\{p\}`/);
});

test("firmware actions on keys: accepted by the binding check, labelled on the key caps", () => {
  assert.equal(isValidBinding({ type: "fw", index: 0 }), true);
  assert.equal(isValidBinding({ type: "fw", index: 0xffffff }), true);
  assert.equal(isValidBinding({ type: "fw", index: -1 }), false);
  assert.equal(isValidBinding({ type: "fw" }), false);
  assert.deepEqual(cleanBinding({ type: "fw", index: 7, junk: 1 }), { type: "fw", index: 7 });
  assert.notEqual(capLabel({ type: "fw", index: 0 }), undefined);
  assert.ok(String(capLabel({ type: "fw", index: 0x123456 })).length <= 6, "fits a key cap");
});

test("every firmware feature that exists today has a page (feature map)", () => {
  // keyboard feature -> the page that exposes it. When the firmware gains a feature, add it here with its page.
  const map = {
    "key remap / layers / Fn / tap-hold / one-shot": "remap",
    "actuation point, rapid trigger, per-key trigger": "performance",
    "DKS / Mod-Morph / toggle keys / SOCD": "behaviors",
    "gamepad mode": "pad",
    "lights (effect, brightness, speed, colour, idle fade-out, saved on the keyboard)": "lighting",
    "light frame stream (play an animation live on the lights)": "animation",
    "profile switching and storage": "profiles",
    "calibration (recalibrate, save), device info, status, connection": "device",
    "live key depth": "live",
  };
  for (const [feature, page] of Object.entries(map)) assert.ok(PAGES.includes(page), `${feature} -> ${page}`);
});
