// Built-in light presets, and the scene file (JSON) the page exports and imports. Pure functions.
//
// The file keeps the FULL colours of the editor (hex per light, km_led order), the indicator rules in readable form, and the animation
// settings; the 16-colour palette is derived when the scene is built (withKeyColors), exactly as before a write. Reading is strict: a file
// that is not a Magger60 light scene, or has a value the keyboard would refuse, is rejected with a code the page turns into a message.
import { defaultScene, ruleProblem, SRC, OFF, ANIM_MASK, LS } from "./lightscene_codec.js";
import { LED_COUNT } from "./led_map.js";
import { hexToRgb, rgbToHex } from "./lighting.js";
import { withKeyColors, sceneKeyColors, linearGradient, rainbow, ledPoints, allLeds, ledsOfSlots } from "./scene_colors.js";
import { RULE_SOURCES, newRule, makeFlags, ruleOff, ruleBlinks, ledOfKeyId, normalizeRule, MAX_RULES } from "./scene_rules.js";

export const FILE_FORMAT = "magger60-light-scene";
export const FILE_VERSION = 1;

export class SceneFileError extends Error {
  /** code: json | format | version | colors | rules | anim */
  constructor(code, message) { super(message); this.name = "SceneFileError"; this.code = code; }
}

const OFF_NAMES = ["keep", "black", "dim"];

/** Scene + full colours -> a plain object ready for JSON.stringify. */
export function sceneToFile(scene, colors, name = "") {
  return {
    format: FILE_FORMAT,
    version: FILE_VERSION,
    name,
    colors: colors.map(rgbToHex),
    rules: scene.rules.map((r) => ({
      source: RULE_SOURCES.find((s) => s.src === r.source)?.id ?? r.source,
      param: r.param,
      led: r.led,
      color: rgbToHex(r.color),
      off: OFF_NAMES[ruleOff(r)],
      dim: r.dim,
      blink: ruleBlinks(r),
    })),
    anim: { autostart: !!(scene.anim.flags & 1), loop: !!(scene.anim.flags & 2), clip: scene.anim.clip, offsetMs: scene.anim.offsetMs },
  };
}

/** Text of an exported file. */
export const sceneToText = (scene, colors, name) => `${JSON.stringify(sceneToFile(scene, colors, name), null, 2)}\n`;

/** A parsed file object (or its text) -> { scene, colors, name }. Throws SceneFileError. The scene is complete and valid (palette built from the colours). */
export function sceneFromFile(input) {
  let o = input;
  if (typeof input === "string") {
    try { o = JSON.parse(input); } catch { throw new SceneFileError("json", "the file is not JSON"); }
  }
  if (!o || typeof o !== "object" || o.format !== FILE_FORMAT) throw new SceneFileError("format", "not a Magger60 light scene file");
  if (o.version !== FILE_VERSION) throw new SceneFileError("version", `file version ${o.version} is not understood`);
  if (!Array.isArray(o.colors) || o.colors.length !== LED_COUNT) throw new SceneFileError("colors", `the file needs exactly ${LED_COUNT} colours`);
  const colors = o.colors.map((c) => hexToRgb(c));
  if (colors.some((c) => !c)) throw new SceneFileError("colors", "a colour in the file is not #rrggbb");

  const rulesIn = o.rules ?? [];
  if (!Array.isArray(rulesIn) || rulesIn.length > MAX_RULES) throw new SceneFileError("rules", `at most ${MAX_RULES} indicator rules`);
  const rules = rulesIn.map((q, i) => {
    const src = RULE_SOURCES.find((s) => s.id === q?.source)?.src ?? (Number.isInteger(q?.source) ? q.source : -1);
    const off = OFF_NAMES.indexOf(q?.off ?? "keep");
    const color = hexToRgb(q?.color);
    if (src < SRC.HOST_NUM || off < 0 || !color) throw new SceneFileError("rules", `indicator rule ${i + 1} is not understood`);
    const r = { source: src, param: q.param ?? 0, led: q.led, flags: makeFlags(off, !!q.blink), color, dim: off === OFF.DIM ? (q.dim ?? 0) : 0 };
    if (ruleProblem(r)) throw new SceneFileError("rules", `indicator rule ${i + 1} would be refused by the keyboard (${ruleProblem(r)})`);
    return r;
  });

  const a = o.anim ?? {};
  const anim = { flags: (a.autostart ? 1 : 0) | (a.loop ? 2 : 0), clip: a.clip ?? 0, offsetMs: a.offsetMs ?? 0 };
  if (!Number.isInteger(anim.clip) || anim.clip < 0 || anim.clip > 255 || (anim.flags & ~ANIM_MASK) !== 0
    || !Number.isInteger(anim.offsetMs) || Math.abs(anim.offsetMs) > LS.animOffsetMax) throw new SceneFileError("anim", "the animation settings are out of range");

  const scene = { ...withKeyColors(defaultScene(), colors), rules, anim, extra: [] };
  return { scene, colors, name: typeof o.name === "string" ? o.name.slice(0, 60) : "" };
}

// ---- presets ----

const BLUE = [0, 150, 255];
const DIM_BLUE = [0, 40, 70];
const hex = (h) => hexToRgb(h);
const ids = (layout, keyIds) => ledsOfSlots(keyIds.map((id) => layout.byId.get(id)?.slot).filter((s) => s !== undefined));

/** Preset ids in the order the page lists them; each gives { colors, rules } for a layout (rules: null = leave the rules as they are). */
export const PRESET_IDS = Object.freeze(["solid", "ocean", "sunset", "rainbow", "wasd", "zones", "modifiers", "caps"]);

export function buildPreset(id, layout) {
  const leds = allLeds();
  const pts = ledPoints(layout, leds);
  switch (id) {
    case "solid": return { colors: leds.map(() => [...BLUE]), rules: null };
    case "ocean": return { colors: linearGradient(pts, [{ pos: 0, color: hex("#0a2a6b") }, { pos: 0.55, color: hex("#00b7ff") }, { pos: 1, color: hex("#5affc8") }], 20), rules: null };
    case "sunset": return { colors: linearGradient(pts, [{ pos: 0, color: hex("#ff7a00") }, { pos: 0.5, color: hex("#ff2d6f") }, { pos: 1, color: hex("#6a1bff") }], 0), rules: null };
    case "rainbow": return { colors: rainbow(pts, { angle: 0, hueFrom: 0, hueSpan: 300 }), rules: null };
    case "wasd": {
      const hot = new Set(ids(layout, ["KeyW", "KeyA", "KeyS", "KeyD"]));
      return { colors: leds.map((l) => (hot.has(l) ? [255, 255, 255] : [...DIM_BLUE])), rules: null };
    }
    case "zones": {   // gaming zones: WASD, arrows and the number row each get their own colour, the rest stays dim
      const move = new Set(ids(layout, ["KeyW", "KeyA", "KeyS", "KeyD"]));
      const arrows = new Set(ids(layout, ["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"]));
      const nums = new Set(ids(layout, ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6"]));
      const skills = new Set(ids(layout, ["KeyQ", "KeyE", "KeyR", "KeyF", "Space", "ShiftLeft", "ControlLeft"]));
      return { colors: leds.map((l) => (move.has(l) ? [255, 40, 40] : arrows.has(l) ? [0, 255, 213] : nums.has(l) ? [255, 200, 0] : skills.has(l) ? [180, 60, 255] : [...DIM_BLUE])), rules: null };
    }
    case "modifiers": {
      const mods = new Set(ids(layout, ["ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight", "Fn", "Tab", "CapsLock", "Escape", "Enter", "Backspace"]));
      return { colors: leds.map((l) => (mods.has(l) ? [255, 140, 20] : [0, 120, 160])), rules: null };
    }
    case "caps": {
      const rules = [];
      const caps = ledOfKeyId(layout, "CapsLock");
      if (caps >= 0) rules.push(newRule("caps", caps, [255, 60, 40]));
      const fn = ledOfKeyId(layout, "Fn");
      if (fn >= 0) rules.push(normalizeRule({ source: SRC.LAYER, param: 1, led: fn, flags: makeFlags(OFF.KEEP), color: [60, 255, 120], dim: 0 }));
      return { colors: leds.map(() => [...DIM_BLUE]), rules };
    }
    default: throw new RangeError(`unknown preset ${id}`);
  }
}

export { sceneKeyColors };
