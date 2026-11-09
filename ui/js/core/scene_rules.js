// Indicator rules of the light scene (km_lightscene.h, LED_NOTES.md 18.4): helpers to build, check and PREVIEW them. Pure functions.
// A rule = "while <condition> is true, light <led> shows <color>"; while it is false the light keeps / goes dark / dims (flags bits 0-1),
// and while true it may blink (bit 2). The first rule that is showing wins on a light. The page uses applyRules() to draw what the keyboard
// would show for a chosen set of inputs; it is a model of km_led.c apply_rules, not the firmware.
import { SRC, OFF, FLAG_BLINK, FLAG_OFF_MASK, LS, ruleProblem } from "./lightscene_codec.js";
import { LED_COUNT, ledOfSlot } from "./led_map.js";

/**
 * Conditions the person can pick, in the order the page lists them. `params`: how many values the parameter has (0 = none).
 * "toggle" has no parameter: it is true while ANY Toggle key is on.
 */
export const RULE_SOURCES = Object.freeze([
  { id: "caps", src: SRC.HOST_CAPS, params: 0 },
  { id: "num", src: SRC.HOST_NUM, params: 0 },
  { id: "scroll", src: SRC.HOST_SCROLL, params: 0 },
  { id: "layer", src: SRC.LAYER, params: 4 },
  { id: "profile", src: SRC.PROFILE, params: 4 },
  { id: "toggle", src: SRC.TOGGLE, params: 0 },
  { id: "pad", src: SRC.PAD, params: 0 },
  { id: "external", src: SRC.EXTERNAL, params: 0 },
]);
export const sourceInfo = (src) => RULE_SOURCES.find((s) => s.src === src) ?? null;

export const MAX_RULES = LS.maxRules;
export const DIM_DEFAULT = 40;

export const makeFlags = (off, blink = false) => (off & FLAG_OFF_MASK) | (blink ? FLAG_BLINK : 0);
export const ruleOff = (r) => r.flags & FLAG_OFF_MASK;
export const ruleBlinks = (r) => (r.flags & FLAG_BLINK) !== 0;

/** A rule the firmware accepts, from whatever the form holds: param forced into range or to 0, dim only with DIM, flags masked. */
export function normalizeRule(r) {
  const info = sourceInfo(r.source);
  const off = Math.min(OFF.DIM, ruleOff(r));
  const out = {
    source: r.source,
    param: info && info.params > 0 ? Math.max(0, Math.min(info.params - 1, r.param | 0)) : 0,
    led: Math.max(0, Math.min(LED_COUNT - 1, r.led | 0)),
    flags: makeFlags(off, ruleBlinks(r)),
    color: r.color.map((v) => Math.max(0, Math.min(255, Math.round(v)))),
    dim: off === OFF.DIM ? Math.max(0, Math.min(255, r.dim | 0)) : 0,
  };
  return out;
}

export const newRule = (sourceId, led, color = [255, 255, 255]) => {
  const info = RULE_SOURCES.find((s) => s.id === sourceId) ?? RULE_SOURCES[0];
  return normalizeRule({ source: info.src, param: 0, led, flags: makeFlags(OFF.KEEP), color, dim: 0 });
};

/** Light under the key with this KeyboardEvent.code-style id (Caps Lock key: "CapsLock"), or -1. */
export function ledOfKeyId(layout, id) {
  const k = layout.byId.get(id);
  return k ? ledOfSlot(k.slot) : -1;
}

/** The ready-made "Caps Lock lights its own key" rule. */
export const capsLockRule = (layout) => newRule("caps", Math.max(0, ledOfKeyId(layout, "CapsLock")), [255, 80, 60]);

/** Problems of a rule list for the page: [{ rule: index, why }] where `why` is a ruleProblem() name or "too_many" (index -1). */
export function rulesProblems(rules) {
  const out = [];
  if (rules.length > MAX_RULES) out.push({ rule: -1, why: "too_many" });
  rules.forEach((r, i) => { const why = ruleProblem(r); if (why) out.push({ rule: i, why }); });
  return out;
}

/** Rules that can never show because an earlier rule on the same light has the very same condition: [{ rule, shadowedBy }]. */
export function shadowedRules(rules) {
  const out = [];
  rules.forEach((r, i) => {
    const j = rules.findIndex((q, k) => k < i && q.led === r.led && q.source === r.source && q.param === r.param);
    if (j >= 0) out.push({ rule: i, shadowedBy: j });
  });
  return out;
}

/** Is the condition of the rule true for these inputs? inputs = { num, caps, scroll, layer, profile, pad, toggle, external }. */
export function conditionOn(r, inputs) {
  switch (r.source) {
    case SRC.HOST_NUM: return !!inputs.num;
    case SRC.HOST_CAPS: return !!inputs.caps;
    case SRC.HOST_SCROLL: return !!inputs.scroll;
    case SRC.LAYER: return (inputs.layer | 0) === r.param;
    case SRC.PROFILE: return (inputs.profile | 0) === r.param;
    case SRC.PAD: return !!inputs.pad;
    case SRC.TOGGLE: return !!inputs.toggle;
    case SRC.EXTERNAL: return !!inputs.external;
    default: return false;
  }
}

export const NO_INPUTS = Object.freeze({ num: false, caps: false, scroll: false, layer: 0, profile: 0, pad: false, toggle: false, external: false });

/**
 * What the lights show once the rules are applied on top of `base` (61 colours, before brightness). `blinkOn` = the lit half of a 500 ms blink.
 * Per light: the first rule that is showing wins; otherwise the first rule asking for BLACK / DIM decides; KEEP rules do nothing.
 * (A blinking rule in its dark half counts as "not showing": it applies its own off mode.)
 */
export function applyRules(base, rules, inputs, blinkOn = true) {
  const out = base.map((c) => [...c]);
  const decided = new Set();
  const shown = new Set();
  for (const r of rules) {
    if (r.led >= LED_COUNT) continue;
    const showing = conditionOn(r, inputs) && (!ruleBlinks(r) || blinkOn);
    if (showing && !shown.has(r.led)) {
      shown.add(r.led);
      out[r.led] = [...r.color];
    }
  }
  for (const r of rules) {
    if (r.led >= LED_COUNT || shown.has(r.led) || decided.has(r.led)) continue;
    const off = ruleOff(r);
    if (off === OFF.BLACK) { out[r.led] = [0, 0, 0]; decided.add(r.led); }
    else if (off === OFF.DIM) { out[r.led] = base[r.led].map((v) => Math.round((v * r.dim) / 255)); decided.add(r.led); }
  }
  return out;
}
