// Light scene editor (UI round 2): light <-> slot map, gradient maths, 16-colour palette, colour editor (undo / redo / copy / paste),
// indicator rules, presets and scene files, and the whole model on top of them. No DOM, no keyboard.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { loadLayout } from "../js/core/layout.js";
import { LED_COUNT, SLOT_OF_LED, ledOfSlot, slotOfLed, ledsOfSlots } from "../js/core/led_map.js";
import {
  lerpColor, hsvToRgb, rgbToHsv, gradientAt, projectT, linearGradient, rainbow, quantize, roundedColors, shadeCount, withKeyColors, sceneKeyColors,
  ColorEditor, readingOrder, rowLeds, columnLeds, allLeds, ledCenter, PALETTE_SIZE, copySelection, pasteSelection,
} from "../js/core/scene_colors.js";
import {
  RULE_SOURCES, normalizeRule, newRule, applyRules, conditionOn, NO_INPUTS, shadowedRules, rulesProblems, capsLockRule, ledOfKeyId, makeFlags, MAX_RULES, DIM_DEFAULT,
} from "../js/core/scene_rules.js";
import { buildPreset, PRESET_IDS, sceneToFile, sceneToText, sceneFromFile, SceneFileError, FILE_FORMAT } from "../js/core/scene_presets.js";
import { defaultScene, encodeScene, decodeScene, SRC, OFF, ruleProblem, LS } from "../js/core/lightscene_codec.js";
import { SceneModel } from "../js/core/scene_model.js";
import { SceneKeeper } from "../js/core/scene_keeper.js";

const layout = loadLayout();
const RED = [255, 0, 0], GREEN = [0, 255, 0], BLUE = [0, 0, 255], BLACK = [0, 0, 0];

// ---------------------------------------------------------------- light <-> slot

test("light map: 61 lights, a bijection onto the 61 key slots of the layout", () => {
  assert.equal(SLOT_OF_LED.length, LED_COUNT);
  assert.equal(new Set(SLOT_OF_LED).size, LED_COUNT);
  assert.deepEqual([...SLOT_OF_LED].sort((a, b) => a - b), layout.keys.map((k) => k.slot).sort((a, b) => a - b));
  for (let l = 0; l < LED_COUNT; l++) assert.equal(ledOfSlot(slotOfLed(l)), l);
  assert.equal(ledOfSlot(15), -1, "a slot without a key has no light");
  assert.equal(slotOfLed(61), -1);
  assert.equal(slotOfLed(-1), -1);
  assert.deepEqual(ledsOfSlots([SLOT_OF_LED[5], SLOT_OF_LED[2], SLOT_OF_LED[5], 15]), [2, 5]);
});

test("light map equals the table in the firmware source (km_led.c k_slot_of_led)", (t) => {
  let src;
  try { src = readFileSync(fileURLToPath(new URL("../../firmware/core/src/km_led.c", import.meta.url)), "utf8"); } catch { t.skip("firmware sources not next to the UI"); return; }
  const body = /k_slot_of_led\[KM_LED_COUNT\]\s*=\s*\{([^}]*)\}/.exec(src)[1];
  assert.deepEqual(body.split(",").map((s) => s.trim()).filter(Boolean).map(Number), [...SLOT_OF_LED]);
});

test("light order is serpentine over the rows (Esc first, Backspace 13th, Tab 14th)", () => {
  const key = (l) => layout.bySlot.get(slotOfLed(l));
  assert.equal(key(0).id, "Escape");
  assert.equal(key(13).id, "Backspace");
  assert.equal(key(14).id, "Backslash", "row 2 runs right to left");
  assert.equal(key(60).id, "ControlRight");
});

// ---------------------------------------------------------------- colour maths

test("hsv <-> rgb", () => {
  assert.deepEqual(hsvToRgb(0, 1, 1), RED);
  assert.deepEqual(hsvToRgb(120, 1, 1), GREEN);
  assert.deepEqual(hsvToRgb(240, 1, 1), BLUE);
  assert.deepEqual(hsvToRgb(360, 1, 1), RED, "360 wraps");
  assert.deepEqual(hsvToRgb(-120, 1, 1), BLUE);
  assert.deepEqual(hsvToRgb(10, 0, 0.5), [128, 128, 128]);
  for (const c of [RED, [12, 200, 99], [255, 255, 255], BLACK, [1, 2, 3]]) {
    const [h, s, v] = rgbToHsv(c);
    assert.deepEqual(hsvToRgb(h, s, v), c);
  }
  assert.deepEqual(lerpColor(BLACK, [255, 100, 50], 0.5), [128, 50, 25]);
});

test("gradientAt: ends, middle, several stops, unsorted stops, clamping, one stop", () => {
  const two = [{ pos: 0, color: RED }, { pos: 1, color: BLUE }];
  assert.deepEqual(gradientAt(two, 0), RED);
  assert.deepEqual(gradientAt(two, 1), BLUE);
  assert.deepEqual(gradientAt(two, 0.5), [128, 0, 128]);
  assert.deepEqual(gradientAt(two, -3), RED);
  assert.deepEqual(gradientAt(two, 7), BLUE);
  assert.deepEqual(gradientAt(two, NaN), RED);
  const three = [{ pos: 1, color: BLUE }, { pos: 0, color: RED }, { pos: 0.5, color: GREEN }];
  assert.deepEqual(gradientAt(three, 0.5), GREEN);
  assert.deepEqual(gradientAt(three, 0.25), [128, 128, 0]);
  assert.deepEqual(gradientAt(three, 0.75), [0, 128, 128]);
  const inner = [{ pos: 0.25, color: RED }, { pos: 0.75, color: BLUE }];
  assert.deepEqual(gradientAt(inner, 0.1), RED, "before the first stop = first colour");
  assert.deepEqual(gradientAt(inner, 0.9), BLUE);
  assert.deepEqual(gradientAt([{ pos: 0.3, color: GREEN }], 0.9), GREEN);
  assert.deepEqual(gradientAt([{ pos: 0.5, color: RED }, { pos: 0.5, color: BLUE }], 0.5), RED, "two stops at one place do not divide by zero");
  assert.throws(() => gradientAt([], 0.5), RangeError);
});

test("projectT: angle 0 runs left to right, 90 top to bottom, 180 reversed, one point or a line across the direction = 0", () => {
  const pts = [{ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 10, y: 0 }];
  assert.deepEqual(projectT(pts, 0), [0, 0.5, 1]);
  assert.deepEqual(projectT(pts, 180).map((v) => Math.round(v * 1000) / 1000), [1, 0.5, 0]);
  assert.deepEqual(projectT(pts, 90), [0, 0, 0], "all points have the same y");
  const col = [{ x: 1, y: 0 }, { x: 1, y: 2 }, { x: 1, y: 4 }];
  assert.deepEqual(projectT(col, 90).map((v) => Math.round(v * 1000) / 1000), [0, 0.5, 1]);
  const diag = projectT([{ x: 0, y: 0 }, { x: 4, y: 4 }], 45);
  assert.deepEqual(diag.map((v) => Math.round(v * 1000) / 1000), [0, 1]);
  assert.deepEqual(projectT([{ x: 3, y: 3 }], 33), [0]);
});

test("linearGradient across the whole keyboard: the left key gets the first colour, the right key the last, in between is in order", () => {
  const leds = allLeds();
  const cols = linearGradient(ledPoints(leds), [{ pos: 0, color: RED }, { pos: 1, color: BLUE }], 0);
  const byX = [...leds].sort((a, b) => ledCenter(layout, a).x - ledCenter(layout, b).x);
  assert.deepEqual(cols[byX[0]], RED);
  assert.deepEqual(cols[byX.at(-1)], BLUE);
  for (let i = 1; i < byX.length; i++) assert.ok(cols[byX[i]][2] >= cols[byX[i - 1]][2], "blue never decreases to the right");
  assert.deepEqual(linearGradient([], [{ pos: 0, color: RED }], 0), []);
  function ledPoints(ls) { return ls.map((l) => ledCenter(layout, l)); }
});

test("rainbow: hue grows along the direction, the first key is hueFrom, the last hueFrom + span", () => {
  const pts = [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 2, y: 0 }];
  assert.deepEqual(rainbow(pts, { hueFrom: 0, hueSpan: 240 }), [RED, GREEN, BLUE]);
  assert.deepEqual(rainbow(pts, { hueFrom: 120, hueSpan: 0 }), [GREEN, GREEN, GREEN]);
  assert.deepEqual(rainbow(pts, { angle: 180, hueFrom: 0, hueSpan: 240 }), [BLUE, GREEN, RED]);
  assert.deepEqual(rainbow(pts, { hueSpan: 240, sat: 0, val: 1 }), [[255, 255, 255], [255, 255, 255], [255, 255, 255]]);
  assert.deepEqual(rainbow([]), []);
});

// ---------------------------------------------------------------- 16-colour palette

test("quantize: up to 16 distinct colours are kept exactly, most used first, unused entries black", () => {
  const cols = [RED, RED, RED, GREEN, GREEN, BLUE];
  const q = quantize(cols);
  assert.equal(q.palette.length, PALETTE_SIZE);
  assert.deepEqual(q.palette[0], RED);
  assert.deepEqual(q.palette[1], GREEN);
  assert.deepEqual(q.palette[2], BLUE);
  assert.deepEqual(q.palette[3], BLACK);
  assert.deepEqual(q.index.map((i) => q.palette[i]), cols);
  assert.deepEqual(roundedColors(cols), cols);
  assert.deepEqual(quantize([]).index, []);
});

test("quantize: 61 different colours become at most 16, close to the originals, deterministic, and rounding twice changes nothing", () => {
  const cols = Array.from({ length: 61 }, (_, i) => hsvToRgb(i * 5.9, 1, 1));
  assert.equal(shadeCount(cols), 61);
  const q = quantize(cols);
  assert.ok(new Set(q.index).size <= 16);
  assert.ok(q.index.every((i) => i >= 0 && i < 16));
  const worst = Math.max(...cols.map((c, i) => Math.max(...c.map((v, k) => Math.abs(v - q.palette[q.index[i]][k])))));
  assert.ok(worst <= 110, `worst channel error ${worst}`);
  assert.deepEqual(quantize(cols), q);
  const r1 = roundedColors(cols);
  assert.ok(shadeCount(r1) <= 16);
  assert.deepEqual(roundedColors(r1), r1);
  // a 17th colour far away from the rest is merged, never dropped: black and white stay far apart
  const mix = [...Array.from({ length: 30 }, () => BLACK), ...Array.from({ length: 30 }, () => [255, 255, 255]), ...Array.from({ length: 16 }, (_, i) => [i * 3, i * 3, 1])];
  const rm = roundedColors(mix);
  assert.deepEqual(rm[0], BLACK);
  assert.deepEqual(rm[30], [255, 255, 255]);
});

test("withKeyColors: a scene with these colours goes through the codec and comes back as the rounded colours", () => {
  const cols = Array.from({ length: LED_COUNT }, (_, i) => hsvToRgb(i * 6, 1, 1));
  const scene = withKeyColors(defaultScene(), cols);
  const back = decodeScene(encodeScene(scene));
  assert.deepEqual(sceneKeyColors(back), roundedColors(cols));
  assert.deepEqual(sceneKeyColors(defaultScene()), Array.from({ length: LED_COUNT }, () => [0, 150, 255]));
  assert.throws(() => withKeyColors(defaultScene(), cols.slice(1)), RangeError);
});

// ---------------------------------------------------------------- the colour editor

test("ColorEditor: set, undo, redo, a new edit drops the redo branch, no-ops are not steps", () => {
  const ed = new ColorEditor();
  assert.deepEqual(ed.colors[7], [0, 150, 255]);
  assert.equal(ed.canUndo, false);
  assert.equal(ed.set([1, 2], RED), true);
  assert.deepEqual(ed.colors[1], RED);
  assert.equal(ed.set([1], RED), false, "same colour: nothing to undo");
  assert.equal(ed.set([3], GREEN), true);
  assert.equal(ed.undo(), true);
  assert.deepEqual(ed.colors[3], [0, 150, 255]);
  assert.deepEqual(ed.colors[1], RED);
  assert.equal(ed.redo(), true);
  assert.deepEqual(ed.colors[3], GREEN);
  ed.undo();
  ed.set([4], BLUE);
  assert.equal(ed.canRedo, false, "redo branch dropped");
  assert.equal(ed.undo() && ed.undo(), true);
  assert.equal(ed.undo(), false);
  assert.deepEqual(ed.colors, new ColorEditor().colors);
});

test("ColorEditor: out-of-range lights are ignored, colours are rounded and clamped, per-light colours are matched by position", () => {
  const ed = new ColorEditor();
  ed.set([0, 99, -1, 1.5], [300, -4, 10.6]);
  assert.deepEqual(ed.colors[0], [255, 0, 11]);
  assert.equal(ed.colors.length, LED_COUNT);
  ed.set([5, 6, 7], [RED, GREEN]);
  assert.deepEqual([ed.colors[5], ed.colors[6]], [RED, GREEN]);
  assert.deepEqual(ed.colors[7], [0, 150, 255], "no colour for the third light: it keeps its colour");
  assert.throws(() => new ColorEditor([[0, 0, 0]]), RangeError);
});

test("ColorEditor: calls with one key within the merge time are ONE undo step (a picker being dragged); a pause or another key starts a new one", () => {
  let t = 1000;
  const ed = new ColorEditor(null, { now: () => t, mergeMs: 500 });
  ed.set([0], [10, 0, 0], { key: "pick" });
  t += 100; ed.set([0], [20, 0, 0], { key: "pick" });
  t += 100; ed.set([0], [30, 0, 0], { key: "pick" });
  assert.equal(ed.undo(), true);
  assert.deepEqual(ed.colors[0], [0, 150, 255], "all three came back at once");
  ed.redo();
  t += 900; ed.set([0], [40, 0, 0], { key: "pick" });
  ed.undo();
  assert.deepEqual(ed.colors[0], [30, 0, 0], "after a pause: its own step");
  ed.redo();
  t += 10; ed.set([0], [50, 0, 0], { key: "other" });
  ed.undo();
  assert.deepEqual(ed.colors[0], [40, 0, 0], "another key: its own step");
  ed.seal();
  ed.redo();
  ed.set([0], [60, 0, 0], { key: "pick" });
  ed.seal();
  ed.set([0], [70, 0, 0], { key: "pick" });
  ed.undo();
  assert.deepEqual(ed.colors[0], [60, 0, 0], "seal() ends the merge");
});

test("ColorEditor: history is bounded and undo still works at the limit", () => {
  const ed = new ColorEditor(null, { maxSteps: 5 });
  for (let i = 1; i <= 10; i++) ed.set([0], [i, 0, 0]);
  let n = 0;
  while (ed.undo()) n++;
  assert.equal(n, 4);
  assert.deepEqual(ed.colors[0], [6, 0, 0]);
});

test("ColorEditor: copy and paste — one colour fills, equal counts map in order, a different count repeats the pattern, nothing copied = nothing pasted", () => {
  const ed = new ColorEditor();
  assert.equal(ed.paste([0]), false);
  assert.equal(ed.copy([]), false);
  ed.set([0, 1, 2], [RED, GREEN, BLUE]);
  ed.copy([0]);
  assert.equal(ed.paste([10, 11, 12]), true);
  assert.deepEqual([10, 11, 12].map((l) => ed.colors[l]), [RED, RED, RED]);
  ed.copy([0, 1, 2]);
  ed.paste([20, 21, 22]);
  assert.deepEqual([20, 21, 22].map((l) => ed.colors[l]), [RED, GREEN, BLUE]);
  ed.paste([30, 31, 32, 33, 34]);
  assert.deepEqual([30, 31, 32, 33, 34].map((l) => ed.colors[l]), [RED, GREEN, BLUE, RED, GREEN]);
  ed.paste([40]);
  assert.deepEqual(ed.colors[40], RED);
  assert.equal(ed.paste([40]), false, "pasting what is already there is not a step");
  ed.set([0], [1, 1, 1]);
  assert.deepEqual(ed.clipboard[0], RED, "the clipboard holds a copy, not a reference");
});

test("copy / paste in reading order: the selection can be given in any order", () => {
  const ed = new ColorEditor();
  const esc = 0, k1 = 1, k2 = 2;                       // lights 0..2 = Esc, 1, 2 (top row, left to right)
  ed.set([esc, k1, k2], [RED, GREEN, BLUE]);
  copySelection(ed, layout, [k2, esc, k1]);
  const row2 = rowLeds(layout, 1);                      // second key row
  pasteSelection(ed, layout, [...row2].reverse().slice(0, 3));
  const first3 = readingOrder(layout, [...row2].reverse().slice(0, 3));
  assert.deepEqual(first3.map((l) => ed.colors[l]), [RED, GREEN, BLUE]);
});

test("rows and columns of the real layout", () => {
  assert.equal(rowLeds(layout, 0).length, 14);
  assert.equal(rowLeds(layout, 1).length, 14);
  assert.equal(rowLeds(layout, 2).length, 13);
  assert.equal(rowLeds(layout, 3).length, 12);
  assert.equal(rowLeds(layout, 4).length, 8);
  assert.equal(rowLeds(layout, 9).length, 0);
  const total = [0, 1, 2, 3, 4].flatMap((r) => rowLeds(layout, r));
  assert.equal(new Set(total).size, LED_COUNT, "every light is in exactly one row");
  assert.ok(columnLeds(layout, 0).length >= 4);
  assert.deepEqual(readingOrder(layout, [5, 0, 3]), [0, 3, 5]);
  assert.equal(allLeds().length, 61);
});

// ---------------------------------------------------------------- indicator rules

test("rules: sources list matches the codec's sources and only layer / profile carry a parameter", () => {
  assert.deepEqual(RULE_SOURCES.map((s) => s.src).sort((a, b) => a - b), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.deepEqual(RULE_SOURCES.filter((s) => s.params > 0).map((s) => s.id).sort(), ["layer", "profile"]);
});

test("normalizeRule makes a rule the firmware accepts, from sloppy form values", () => {
  const r = normalizeRule({ source: SRC.HOST_CAPS, param: 3, led: 99, flags: 0xfc, color: [300, -1, 9.4], dim: 77 });
  assert.equal(ruleProblem(r), null);
  assert.deepEqual([r.param, r.led, r.dim, r.color, r.flags], [0, 60, 0, [255, 0, 9], 4], "stray flag bits dropped, the blink bit kept");
  assert.equal(normalizeRule({ source: SRC.HOST_CAPS, param: 0, led: 0, flags: 3, color: RED, dim: 0 }).flags & 3, OFF.DIM, "the off mode is capped at DIM");
  assert.equal(normalizeRule({ source: SRC.LAYER, param: 9, led: 0, flags: 0, color: RED, dim: 0 }).param, 3);
  const d = normalizeRule({ source: SRC.LAYER, param: 1, led: 0, flags: makeFlags(OFF.DIM), color: RED, dim: 300 });
  assert.equal(d.dim, 255);
  assert.equal(normalizeRule({ ...d, flags: makeFlags(OFF.BLACK) }).dim, 0, "dim only goes with DIM");
  for (const s of RULE_SOURCES) assert.equal(ruleProblem(newRule(s.id, 4)), null, s.id);
  assert.equal(DIM_DEFAULT > 0 && DIM_DEFAULT < 255, true);
});

test("capsLockRule lights the Caps Lock key's own light", () => {
  const r = capsLockRule(layout);
  assert.equal(r.source, SRC.HOST_CAPS);
  assert.equal(slotOfLed(r.led), layout.byId.get("CapsLock").slot);
  assert.equal(ledOfKeyId(layout, "NoSuchKey"), -1);
});

test("conditionOn: every source", () => {
  const on = { num: true, caps: true, scroll: true, layer: 2, profile: 3, pad: true, toggle: true, external: true };
  for (const s of RULE_SOURCES) assert.equal(conditionOn(newRule(s.id, 0), on), s.id === "layer" || s.id === "profile" ? false : true, s.id);
  assert.equal(conditionOn(normalizeRule({ source: SRC.LAYER, param: 2, led: 0, flags: 0, color: RED, dim: 0 }), on), true);
  assert.equal(conditionOn(normalizeRule({ source: SRC.PROFILE, param: 3, led: 0, flags: 0, color: RED, dim: 0 }), on), true);
  assert.equal(conditionOn(normalizeRule({ source: SRC.PROFILE, param: 3, led: 0, flags: 0, color: RED, dim: 0 }), { ...on, profile: 1 }), false);
  for (const s of RULE_SOURCES) assert.equal(conditionOn(newRule(s.id, 0), NO_INPUTS), s.id === "layer" || s.id === "profile", `${s.id}: layer 0 and profile 0 are active when nothing else is said`);
  assert.equal(conditionOn({ source: 99, param: 0 }, on), false);
});

test("applyRules: showing replaces the colour; keep / black / dim when not showing; blink; first showing rule wins; first off-rule decides", () => {
  const base = Array.from({ length: LED_COUNT }, () => [100, 100, 100]);
  const caps = (extra = {}) => normalizeRule({ source: SRC.HOST_CAPS, param: 0, led: 4, flags: makeFlags(OFF.KEEP), color: RED, dim: 0, ...extra });
  assert.deepEqual(applyRules(base, [caps()], { ...NO_INPUTS, caps: true })[4], RED);
  assert.deepEqual(applyRules(base, [caps()], NO_INPUTS)[4], [100, 100, 100], "keep");
  assert.deepEqual(applyRules(base, [caps({ flags: makeFlags(OFF.BLACK) })], NO_INPUTS)[4], BLACK);
  assert.deepEqual(applyRules(base, [caps({ flags: makeFlags(OFF.DIM), dim: 51 })], NO_INPUTS)[4], [20, 20, 20]);
  assert.deepEqual(applyRules(base, [caps()], { ...NO_INPUTS, caps: true })[5], [100, 100, 100], "other lights untouched");
  const blink = caps({ flags: makeFlags(OFF.BLACK, true) });
  assert.deepEqual(applyRules(base, [blink], { ...NO_INPUTS, caps: true }, true)[4], RED);
  assert.deepEqual(applyRules(base, [blink], { ...NO_INPUTS, caps: true }, false)[4], BLACK, "dark half of the blink applies the off mode");
  assert.deepEqual(applyRules(base, [blink], NO_INPUTS, true)[4], BLACK, "not true: off mode, no blinking");
  // two rules on one light
  const a = caps({ color: RED }), b = normalizeRule({ source: SRC.HOST_NUM, param: 0, led: 4, flags: makeFlags(OFF.KEEP), color: GREEN, dim: 0 });
  assert.deepEqual(applyRules(base, [a, b], { ...NO_INPUTS, caps: true, num: true })[4], RED, "first showing rule wins");
  assert.deepEqual(applyRules(base, [b, a], { ...NO_INPUTS, caps: true, num: true })[4], GREEN);
  assert.deepEqual(applyRules(base, [a, b], { ...NO_INPUTS, num: true })[4], GREEN);
  const black = caps({ flags: makeFlags(OFF.BLACK) }), dim = normalizeRule({ source: SRC.HOST_NUM, param: 0, led: 4, flags: makeFlags(OFF.DIM), color: GREEN, dim: 51 });
  assert.deepEqual(applyRules(base, [black, dim], NO_INPUTS)[4], BLACK, "the first off-rule decides");
  assert.deepEqual(applyRules(base, [dim, black], NO_INPUTS)[4], [20, 20, 20]);
  assert.deepEqual(applyRules(base, [black, a], { ...NO_INPUTS, caps: true })[4], RED, "a showing colour beats any off-rule");
  assert.deepEqual(base[4], [100, 100, 100], "the base is not modified");
});

test("shadowed rules and rule problems", () => {
  const a = newRule("caps", 3), b = newRule("caps", 3, GREEN), c = newRule("caps", 4), d = newRule("layer", 3);
  assert.deepEqual(shadowedRules([a, b, c, d]), [{ rule: 1, shadowedBy: 0 }]);
  assert.deepEqual(shadowedRules([a, c, d]), []);
  assert.deepEqual(rulesProblems([a, b]), []);
  assert.deepEqual(rulesProblems([{ ...a, led: 61 }]), [{ rule: 0, why: "led" }]);
  assert.deepEqual(rulesProblems(Array.from({ length: MAX_RULES + 1 }, () => a)), [{ rule: -1, why: "too_many" }]);
  assert.equal(MAX_RULES, LS.maxRules);
});

// ---------------------------------------------------------------- presets and files

test("every built-in preset builds, is valid for the codec and fits the palette", () => {
  for (const id of PRESET_IDS) {
    const p = buildPreset(id, layout);
    assert.equal(p.colors.length, LED_COUNT, id);
    const scene = { ...withKeyColors(defaultScene(), p.colors), rules: p.rules ?? [] };
    const back = decodeScene(encodeScene(scene));
    assert.deepEqual(back.rules, scene.rules, id);
    assert.ok(shadeCount(sceneKeyColors(back)) <= 16, id);
  }
  assert.throws(() => buildPreset("nope", layout), RangeError);
  const caps = buildPreset("caps", layout);
  assert.deepEqual(caps.rules.map((r) => r.source), [SRC.HOST_CAPS, SRC.LAYER]);
  assert.equal(buildPreset("solid", layout).rules, null, "colour presets leave the rules alone");
  const wasd = buildPreset("wasd", layout).colors;
  assert.deepEqual(wasd[ledOfKeyId(layout, "KeyW")], [255, 255, 255]);
  assert.notDeepEqual(wasd[ledOfKeyId(layout, "KeyQ")], [255, 255, 255]);
});

test("scene file: round trip keeps colours, rules and animation settings; the file says what it is", () => {
  const colors = Array.from({ length: LED_COUNT }, (_, i) => hsvToRgb(i * 6, 1, 0.8));
  const scene = {
    ...withKeyColors(defaultScene(), colors),
    rules: [newRule("caps", 28, RED), normalizeRule({ source: SRC.LAYER, param: 1, led: 59, flags: makeFlags(OFF.DIM, true), color: GREEN, dim: 30 })],
    anim: { flags: 3, clip: 2, offsetMs: -1500 },
  };
  const text = sceneToText(scene, colors, "mine");
  const o = JSON.parse(text);
  assert.equal(o.format, FILE_FORMAT);
  assert.equal(o.rules[0].source, "caps");
  assert.deepEqual(o.rules[1], { source: "layer", param: 1, led: 59, color: "#00ff00", off: "dim", dim: 30, blink: true });
  const back = sceneFromFile(text);
  assert.deepEqual(back.colors, colors);
  assert.deepEqual(back.scene.rules, scene.rules);
  assert.deepEqual(back.scene.anim, scene.anim);
  assert.equal(back.name, "mine");
  assert.deepEqual(back.scene, { ...scene, palette: back.scene.palette, keys: back.scene.keys, extra: [] });
  assert.doesNotThrow(() => encodeScene(back.scene));
  assert.deepEqual(sceneFromFile(sceneToFile(scene, colors)).colors, colors, "an object works as well as text");
});

test("scene file: strict — each kind of bad file is refused with a code", () => {
  const good = sceneToFile(defaultScene(), Array.from({ length: LED_COUNT }, () => [1, 2, 3]));
  const code = (mut) => { try { sceneFromFile(typeof mut === "string" ? mut : mut(structuredClone(good))); return "accepted"; } catch (e) { assert.ok(e instanceof SceneFileError); return e.code; } };
  assert.equal(code("{nope"), "json");
  assert.equal(code("[]"), "format");
  assert.equal(code("null"), "format");
  assert.equal(code((o) => ({ ...o, format: "other" })), "format");
  assert.equal(code((o) => ({ ...o, version: 2 })), "version");
  assert.equal(code((o) => { o.colors.pop(); return o; }), "colors");
  assert.equal(code((o) => { o.colors[3] = "red"; return o; }), "colors");
  assert.equal(code((o) => { o.rules = [{ source: "caps", param: 0, led: 61, color: "#ff0000", off: "keep", dim: 0, blink: false }]; return o; }), "rules");
  assert.equal(code((o) => { o.rules = [{ source: "weather", param: 0, led: 1, color: "#ff0000" }]; return o; }), "rules");
  assert.equal(code((o) => { o.rules = [{ source: "caps", led: 1, color: "#ff0000", off: "maybe" }]; return o; }), "rules");
  assert.equal(code((o) => { o.rules = Array.from({ length: 9 }, () => ({ source: "caps", led: 1, color: "#ff0000" })); return o; }), "rules");
  assert.equal(code((o) => { o.anim = { clip: 300 }; return o; }), "anim");
  assert.equal(code((o) => { o.anim = { offsetMs: 4e6 }; return o; }), "anim");
  assert.equal(code((o) => { delete o.rules; delete o.anim; return o; }), "accepted", "rules and animation are optional");
});

// ---------------------------------------------------------------- the model

function fakeKeeper(scene = defaultScene()) {
  const sets = [];
  return { scene, sets, set(s) { encodeScene(s); this.scene = s; sets.push(s); } };
}
function model(opts = {}) {
  const keeper = fakeKeeper(opts.scene);
  const selected = new Set(opts.slots ?? []);
  const m = new SceneModel({ layout, keeper, selection: () => selected });
  return { m, keeper, selected };
}
const slotsOf = (ids) => ids.map((id) => layout.byId.get(id).slot);

test("model: painting the selection changes those lights, reaches the keeper as a valid scene, and undo takes it back", () => {
  const { m, keeper } = model({ slots: slotsOf(["KeyA", "KeyS"]) });
  assert.equal(m.paint(RED), true);
  assert.deepEqual([m.colors[ledOfKeyId(layout, "KeyA")], m.colors[ledOfKeyId(layout, "KeyS")], m.colors[ledOfKeyId(layout, "KeyD")]], [RED, RED, [0, 150, 255]]);
  assert.deepEqual(sceneKeyColors(keeper.scene)[ledOfKeyId(layout, "KeyA")], RED);
  assert.equal(m.paint(RED), false, "same colour again");
  assert.equal(m.undo(), true);
  assert.deepEqual(sceneKeyColors(keeper.scene)[ledOfKeyId(layout, "KeyA")], [0, 150, 255]);
  assert.equal(m.canUndo, false);
  assert.equal(m.redo(), true);
  assert.equal(m.canRedo, false);
});

test("model: no selection = nothing painted; scopes: all, rows, columns", () => {
  const { m, selected } = model();
  assert.equal(m.paint(RED, "selection"), false);
  assert.equal(m.paint(GREEN, "all"), true);
  assert.ok(m.colors.every((c) => c.join() === GREEN.join()));
  selected.add(layout.byId.get("KeyG").slot);
  const rowOfG = m.scopeLeds("rows");
  assert.deepEqual(rowOfG, rowLeds(layout, 2));
  const colOfG = m.scopeLeds("cols");
  assert.ok(colOfG.includes(ledOfKeyId(layout, "KeyG")) && colOfG.length >= 4);
  m.paint(BLUE, "rows");
  assert.ok(rowLeds(layout, 2).every((l) => m.colors[l].join() === BLUE.join()));
  assert.equal(m.colors[ledOfKeyId(layout, "KeyQ")].join(), GREEN.join());
  assert.throws(() => m.scopeLeds("diagonal"), RangeError);
});

test("model: a dragged colour picker is ONE undo step", () => {
  const { m } = model({ slots: slotsOf(["KeyA"]) });
  for (const v of [10, 20, 30, 40]) m.paint([v, 0, 0], "selection", { key: "pick" });
  m.undo();
  assert.equal(m.canUndo, false);
  assert.deepEqual(m.colors[ledOfKeyId(layout, "KeyA")], [0, 150, 255]);
});

test("model: gradient preview shows without changing the scene; apply writes it; row and column gradients ignore the selection", () => {
  const { m, keeper, selected } = model();
  for (const k of rowLeds(layout, 0)) selected.add(slotOfLed(k));
  m.setGradient({ stops: [{ pos: 0, color: RED }, { pos: 1, color: BLUE }], angle: 0 });
  const before = keeper.sets.length;
  m.previewGradient("linear");
  assert.equal(m.previewing, true);
  assert.equal(keeper.sets.length, before, "a preview never reaches the keeper");
  const top = readingOrder(layout, rowLeds(layout, 0));
  assert.deepEqual(m.shown[top[0]], RED);
  assert.deepEqual(m.shown[top.at(-1)], BLUE);
  assert.deepEqual(m.colors[top[0]], [0, 150, 255], "the editor is untouched");
  m.clearPreview();
  assert.equal(m.previewing, false);
  assert.deepEqual(m.shown[top[0]], [0, 150, 255]);
  assert.equal(m.applyGradient("linear"), true);
  assert.equal(m.previewing, false);
  assert.deepEqual(m.colors[top[0]], RED);
  assert.deepEqual(m.colors[top.at(-1)], BLUE);
  assert.equal(keeper.sets.length, before + 1);
  // line gradients
  const keep = m.gradient.angle;
  assert.equal(m.applyLineGradient("column", 3), true);
  assert.equal(m.gradient.angle, keep, "the angle setting is not disturbed");
  const col = columnLeds(layout, 3);
  const ys = [...col].sort((a, b) => ledCenter(layout, a).y - ledCenter(layout, b).y);
  assert.deepEqual(m.colors[ys[0]], RED);
  assert.deepEqual(m.colors[ys.at(-1)], BLUE);
  assert.equal(m.applyLineGradient("row", 4), true);
  const bottom = readingOrder(layout, rowLeds(layout, 4));
  assert.deepEqual(m.colors[bottom[0]], RED);
  assert.deepEqual(m.colors[bottom.at(-1)], BLUE);
  assert.equal(m.applyLineGradient("row", 12), false, "a row that does not exist is empty");
});

test("model: rainbow over everything uses many shades, the drawn colours are rounded to 16 and the page can tell", () => {
  const { m, keeper } = model();
  m.setGradient({ angle: 0, hueFrom: 0, hueSpan: 300 });
  assert.equal(m.applyGradient("rainbow", "all"), true);
  assert.ok(m.shades > 16, `${m.shades} shades before rounding`);
  assert.equal(m.overPalette, true);
  assert.ok(shadeCount(m.shown) <= 16);
  assert.ok(shadeCount(sceneKeyColors(keeper.scene)) <= 16);
  m.paint(RED, "all");
  assert.equal(m.overPalette, false);
});

test("model: copy and paste go through the keeper", () => {
  const { m, selected, keeper } = model({ slots: slotsOf(["KeyA"]) });
  m.paint(RED);
  assert.equal(m.copy(), true);
  assert.equal(m.hasClipboard, true);
  selected.clear();
  slotsOf(["KeyS", "KeyD"]).forEach((s) => selected.add(s));
  assert.equal(m.paste(), true);
  assert.deepEqual(sceneKeyColors(keeper.scene)[ledOfKeyId(layout, "KeyD")], RED);
  selected.clear();
  assert.equal(m.copy(), false, "nothing selected, nothing copied");
  assert.equal(m.paste(), false);
});

test("model: rules — add, change, move, remove; limits; invalid input is repaired, not sent", () => {
  const { m, keeper } = model();
  assert.equal(m.addRule("caps", 28, RED), true);
  assert.equal(keeper.scene.rules.length, 1);
  assert.equal(m.addRule("layer", 59), true);
  assert.equal(m.updateRule(1, { param: 2 }), true);
  assert.equal(keeper.scene.rules[1].param, 2);
  assert.equal(m.updateRule(1, { source: SRC.HOST_NUM }), true);
  assert.equal(keeper.scene.rules[1].param, 0, "a source without a parameter resets it");
  assert.equal(m.updateRule(1, { flags: makeFlags(OFF.DIM), dim: 90 }), true);
  assert.equal(keeper.scene.rules[1].dim, 90);
  assert.equal(m.updateRule(1, { flags: makeFlags(OFF.KEEP) }), true);
  assert.equal(keeper.scene.rules[1].dim, 0);
  assert.equal(m.updateRule(1, { led: 500 }), true);
  assert.equal(keeper.scene.rules[1].led, 60);
  assert.equal(m.updateRule(7, {}), false);
  assert.equal(m.moveRule(1, -1), true);
  assert.equal(keeper.scene.rules[0].source, SRC.HOST_NUM);
  assert.equal(m.moveRule(0, -1), false);
  assert.equal(m.moveRule(1, 1), false);
  assert.equal(m.removeRule(0), true);
  assert.equal(m.removeRule(5), false);
  assert.equal(m.rules.length, 1);
  for (let i = 0; i < 20; i++) m.addRule("num", i);
  assert.equal(m.rules.length, MAX_RULES);
  assert.equal(m.canAddRule, false);
  assert.equal(m.addRule("num", 1), false);
  assert.deepEqual(m.ruleProblems, []);
  assert.ok(m.encodedSize <= LS.maxEncoded);
});

test("model: editing colours keeps the rules and the other scene parts; editing rules keeps the colours", () => {
  const { m, keeper } = model({ slots: slotsOf(["KeyA"]), scene: { ...defaultScene(), anim: { flags: 1, clip: 4, offsetMs: 10 }, extra: [{ id: 0x90, data: Uint8Array.of(1, 2) }] } });
  m.addRule("caps", 28, RED);
  m.paint(GREEN);
  assert.equal(keeper.scene.rules.length, 1);
  assert.deepEqual(keeper.scene.anim, { flags: 1, clip: 4, offsetMs: 10 });
  assert.equal(keeper.scene.extra.length, 1, "an optional section of a newer firmware survives");
  m.removeRule(0);
  assert.deepEqual(sceneKeyColors(keeper.scene)[ledOfKeyId(layout, "KeyA")], GREEN);
});

test("model: presets replace the colours (undoable) and the rules only when the preset has rules; files import and export", () => {
  const { m, keeper } = model();
  m.addRule("num", 1);
  m.applyPreset("rainbow");
  assert.equal(keeper.scene.rules.length, 1, "a colour preset keeps the rules");
  assert.ok(shadeCount(sceneKeyColors(keeper.scene)) <= 16);
  assert.equal(m.undo(), true);
  assert.deepEqual(m.colors[0], [0, 150, 255]);
  m.applyPreset("caps");
  assert.deepEqual(keeper.scene.rules.map((r) => r.source), [SRC.HOST_CAPS, SRC.LAYER]);
  const text = m.exportText("x");
  const other = model();
  assert.equal(other.m.importText(text), "x");
  assert.deepEqual(other.keeper.scene.rules, keeper.scene.rules);
  assert.deepEqual(other.m.colors, m.colors);
  assert.equal(other.m.canUndo, true, "an import can be undone");
  assert.throws(() => other.m.importText("{}"), SceneFileError);
});

test("model: change notifications", () => {
  const { m } = model({ slots: slotsOf(["KeyA"]) });
  let n = 0;
  const off = m.onChange(() => n++);
  m.paint(RED);
  m.previewGradient("rainbow");
  m.clearPreview();
  assert.ok(n >= 3);
  off();
  const k = n;
  m.paint(GREEN);
  assert.equal(n, k);
});

test("keeper is only needed for what the page shows: SceneKeeper is exported", () => {
  assert.equal(typeof SceneKeeper, "function");
});

// ---------------------------------------------------------------- texts

test("every dynamic text key of the scene part of the lights page exists in both languages", async () => {
  const zh = (await import("../js/locales/zh-TW.js")).default;
  const en = (await import("../js/locales/en.js")).default;
  const keys = [];
  for (const id of ["effect", "colors", "rules", "scene"]) keys.push(`scene.tab.${id}`);
  for (const id of ["colors", "rules", "scene"]) keys.push(`scene.sub.${id}`);
  for (const k of ["old", "stock", "offline"]) keys.push(`scene.na.${k}.title`, `scene.na.${k}.body`);
  for (const s of RULE_SOURCES) keys.push(`scene.src.${s.id}`);
  for (const k of ["keep", "black", "dim"]) keys.push(`scene.off.${k}`);
  for (const k of ["linear", "rainbow"]) keys.push(`scene.g.${k}`);
  for (const id of PRESET_IDS) keys.push(`scene.preset.${id}`, `scene.preset.${id}.d`);
  for (const c of ["json", "format", "version", "colors", "rules", "anim"]) keys.push(`scene.file.err.${c}`);
  for (const k of ["busy", "wait", "step"]) keys.push(`scene.state.saving.${k}`);
  for (const k of ["base", "keys", "rules", "limits", "idle", "ext"]) keys.push(`scene.help.${k}`, `scene.help.${k}.d`);
  for (const k of ["custom", "external", "anim"]) keys.push(`led.fx.${k}`, `led.fxd.${k}`);
  for (const k of keys) assert.ok(typeof zh[k] === "string" && zh[k] && typeof en[k] === "string" && en[k], k);
  // every file error code the preset module can raise has a text
  assert.equal(new Set(keys).size, keys.length);
});
