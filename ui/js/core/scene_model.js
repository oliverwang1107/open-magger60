// The working light scene of the lights page, without any DOM: the colour editor, the indicator rules, a gradient preview, presets and
// files, all feeding one SceneKeeper (which owns the talking to the keyboard). The page builds buttons on top of these methods; tests drive
// the methods directly. Every change ends in keeper.set(), so with live mode on it reaches the keyboard (coalesced) without extra code.
import { encodeScene, cloneScene } from "./lightscene_codec.js";
import {
  ColorEditor, withKeyColors, sceneKeyColors, roundedColors, shadeCount, linearGradient, rainbow, ledPoints, rowLeds, columnLeds, allLeds,
  copySelection, pasteSelection, PALETTE_SIZE,
} from "./scene_colors.js";
import { ledsOfSlots, slotOfLed } from "./led_map.js";
import { MAX_RULES, normalizeRule, newRule, rulesProblems, shadowedRules } from "./scene_rules.js";
import { buildPreset, sceneToText, sceneFromFile } from "./scene_presets.js";

export const DEFAULT_STOPS = Object.freeze([{ pos: 0, color: [255, 80, 80] }, { pos: 1, color: [80, 120, 255] }]);
export const MAX_STOPS = 5;

export class SceneModel {
  #listeners = new Set();
  #preview = null;

  /** keeper: a SceneKeeper; selection: () => iterable of selected sensor slots; layout: the page's layout. */
  constructor({ layout, keeper, selection = () => [], now }) {
    this.layout = layout;
    this.keeper = keeper;
    this.selection = selection;
    this.now = now;
    this.editor = new ColorEditor(sceneKeyColors(keeper.scene), { now });
    this.gradient = { stops: DEFAULT_STOPS.map((s) => ({ pos: s.pos, color: [...s.color] })), angle: 0, hueFrom: 0, hueSpan: 300 };
    this.paintColor = [255, 255, 255];
  }

  onChange(fn) { this.#listeners.add(fn); return () => this.#listeners.delete(fn); }
  #emit() { for (const fn of this.#listeners) fn(); }

  /** After keeper.load(): start from what the keyboard holds. */
  resetFromKeeper() {
    this.editor = new ColorEditor(sceneKeyColors(this.keeper.scene), { now: this.now });
    this.#preview = null;
    this.#emit();
  }

  // ---- selection -> lights ----
  get selectedLeds() { return ledsOfSlots([...this.selection()]); }

  /** Lights a scope names: "selection" | "all" | "rows" (every row that holds a selected light) | "cols". */
  scopeLeds(scope) {
    const sel = this.selectedLeds;
    if (scope === "all") return allLeds();
    if (scope === "selection") return sel;
    const keys = sel.map((l) => this.layout.bySlot.get(slotOfLed(l))).filter(Boolean);
    if (scope === "rows") return [...new Set([...new Set(keys.map((k) => Math.floor(k.y + k.h / 2)))].flatMap((r) => rowLeds(this.layout, r)))].sort((a, b) => a - b);
    if (scope === "cols") return [...new Set([...new Set(keys.map((k) => Math.floor(k.x + k.w / 2)))].flatMap((c) => columnLeds(this.layout, c)))].sort((a, b) => a - b);
    throw new RangeError(`unknown scope ${scope}`);
  }

  // ---- colours ----
  get colors() { return this.editor.colors; }
  /** What is drawn: the editor's colours with the gradient preview laid over them, rounded to the scene's 16 shades. */
  get shown() {
    const c = this.editor.colors.map((x) => [...x]);
    if (this.#preview) this.#preview.leds.forEach((l, i) => { c[l] = this.#preview.colors[i]; });
    return roundedColors(c);
  }
  get shades() { return shadeCount(this.#preview ? this.#withPreview() : this.editor.colors); }
  #withPreview() { const c = this.editor.colors.map((x) => [...x]); this.#preview.leds.forEach((l, i) => { c[l] = this.#preview.colors[i]; }); return c; }
  get overPalette() { return this.shades > PALETTE_SIZE; }
  get previewing() { return !!this.#preview; }

  #sync() {
    this.keeper.set(withKeyColors(this.keeper.scene, this.editor.colors));
    this.#emit();
  }

  /** Paint a scope (or an explicit list of lights) with one colour. `key` merges a stream of calls (a colour picker being dragged) into one undo step. */
  paint(color, scope = "selection", { key = null } = {}) {
    const leds = Array.isArray(scope) ? scope : this.scopeLeds(scope);
    this.paintColor = [...color];
    if (!leds.length || !this.editor.set(leds, color, { key })) { this.#emit(); return false; }
    this.#sync();
    return true;
  }

  undo() { if (this.editor.undo()) { this.#sync(); return true; } return false; }
  redo() { if (this.editor.redo()) { this.#sync(); return true; } return false; }
  copy() { const ok = copySelection(this.editor, this.layout, this.selectedLeds); this.#emit(); return ok; }
  paste() { const ok = pasteSelection(this.editor, this.layout, this.selectedLeds); if (ok) this.#sync(); return ok; }
  get canUndo() { return this.editor.canUndo; }
  get canRedo() { return this.editor.canRedo; }
  get hasClipboard() { return this.editor.hasClipboard; }

  // ---- gradients ----
  setGradient(patch) { Object.assign(this.gradient, patch); }
  /** Colours the current gradient gives to `leds` (linear with the stops and angle). */
  gradientFor(leds) { return linearGradient(ledPoints(this.layout, leds), this.gradient.stops, this.gradient.angle); }
  rainbowFor(leds) { return rainbow(ledPoints(this.layout, leds), { angle: this.gradient.angle, hueFrom: this.gradient.hueFrom, hueSpan: this.gradient.hueSpan }); }
  #fill(kind, leds) { return kind === "rainbow" ? this.rainbowFor(leds) : this.gradientFor(leds); }

  /** Show a gradient on `scope` without changing the scene (the drawn colours only). */
  previewGradient(kind, scope = "selection") {
    const leds = Array.isArray(scope) ? scope : this.scopeLeds(scope);
    this.#preview = leds.length ? { leds, colors: this.#fill(kind, leds) } : null;
    this.#emit();
  }
  clearPreview() { if (this.#preview) { this.#preview = null; this.#emit(); } }

  /** Write the gradient into the scene. kind: "linear" | "rainbow". */
  applyGradient(kind, scope = "selection", { key = null } = {}) {
    const leds = Array.isArray(scope) ? scope : this.scopeLeds(scope);
    this.#preview = null;
    if (!leds.length) { this.#emit(); return false; }
    const changed = this.editor.set(leds, this.#fill(kind, leds), { key });
    if (changed) this.#sync(); else this.#emit();
    return changed;
  }
  /** Gradient along one row (index from 0) or one column, whatever is selected. */
  applyLineGradient(axis, index, kind = "linear", { key = null } = {}) {
    const leds = axis === "row" ? rowLeds(this.layout, index) : columnLeds(this.layout, index);
    const saved = this.gradient.angle;
    this.gradient.angle = axis === "row" ? 0 : 90;
    try { return this.applyGradient(kind, leds, { key }); } finally { this.gradient.angle = saved; }
  }

  // ---- indicator rules ----
  get rules() { return this.keeper.scene.rules; }
  get ruleProblems() { return rulesProblems(this.rules); }
  get ruleShadows() { return shadowedRules(this.rules); }
  get canAddRule() { return this.rules.length < MAX_RULES; }
  #setRules(rules) {
    this.keeper.set({ ...cloneScene(this.keeper.scene), rules });
    this.#emit();
  }
  addRule(sourceId, led, color) {
    if (!this.canAddRule) return false;
    this.#setRules([...this.rules, newRule(sourceId, led, color)]);
    return true;
  }
  addRaw(rule) {
    if (!this.canAddRule) return false;
    this.#setRules([...this.rules, normalizeRule(rule)]);
    return true;
  }
  updateRule(i, patch) {
    if (!this.rules[i]) return false;
    const next = this.rules.map((r, k) => (k === i ? normalizeRule({ ...r, ...patch, color: patch.color ?? r.color }) : r));
    this.#setRules(next);
    return true;
  }
  removeRule(i) { if (!this.rules[i]) return false; this.#setRules(this.rules.filter((_, k) => k !== i)); return true; }
  moveRule(i, dir) {
    const j = i + dir;
    if (!this.rules[i] || j < 0 || j >= this.rules.length) return false;
    const next = [...this.rules];
    [next[i], next[j]] = [next[j], next[i]];
    this.#setRules(next);
    return true;
  }

  // ---- presets and files ----
  applyPreset(id) {
    const p = buildPreset(id, this.layout);
    this.editor.replaceAll(p.colors);
    if (p.rules) this.keeper.set({ ...withKeyColors(this.keeper.scene, this.editor.colors), rules: p.rules });
    else this.keeper.set(withKeyColors(this.keeper.scene, this.editor.colors));
    this.#preview = null;
    this.#emit();
  }
  exportText(name = "") { return sceneToText(this.keeper.scene, this.editor.colors, name); }
  /** Throws SceneFileError. The imported scene replaces colours, rules and animation settings of the working copy (not yet written). */
  importText(text) {
    const { scene, colors, name } = sceneFromFile(text);
    this.editor.replaceAll(colors);
    this.keeper.set({ ...scene, extra: this.keeper.scene.extra });
    this.#preview = null;
    this.#emit();
    return name;
  }

  /** Bytes of the scene as it would be written, or null when it is invalid. */
  get encodedSize() { try { return encodeScene(this.keeper.scene).length; } catch { return null; } }
}
