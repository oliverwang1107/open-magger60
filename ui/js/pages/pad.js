// Gamepad page (M6 package P9b): turn the keyboard into a gamepad as well. Pad mode on / off with a plain explanation, per-key bindings (stick
// directions, triggers, 16 buttons, d-pad) on any layer, the four response curves with a live preview, the stick options, a virtual gamepad
// driven by the live key depth, and the SDL mapping text for games and Steam.
// Everything is edited in the local model (app.edit); nothing reaches the keyboard until the person presses Save (see sync.js).
import { h, icon, toast } from "../dom.js";
import { t } from "../i18n.js";
import { pt } from "../locales/pad-i18n.js";
import { keyboardCard } from "../ui/kb-card.js";
import { rangeField, segmented, toggle } from "../ui/controls.js";
import { confirmDialog } from "../ui/dialog.js";
import { VirtualPad } from "../ui/pad-view.js";
import { CurvePlot } from "../ui/pad-curve.js";
import { defaultBinding } from "./remap.js";
import { injectPadStyle } from "./pad-style.js";
import { LAYER_COUNT } from "../core/profile.js";
import { sameBinding } from "../core/hid-usages.js";
import { defaultPad } from "../core/limits.js";
import { decodeSettings } from "../core/settings_codec.js";
import {
  BTN_ACT_MAX, BTN_ACT_MIN, CURVE_COUNT, CURVE_PRESETS, DEADZONE_MAX, MM_PER_UNIT, PAD_MODE, PLATFORMS, PadPreview, TARGETS, TARGET_GROUPS,
  applyBindings, changeCurve, checkPad, curvePreset, curveUse, isGuid, isPadBinding, newSpec, padKeys, padModeOf, padOf, presetEdits, sdlGuid, sdlMapping,
  setPad, setPadMode, specOfBinding, specProblem, targetById, unitsToMm,
} from "../core/pad.js";

const mmText = (units) => `${unitsToMm(units).toFixed(2)} mm`;
const TARGET_KEY = (id) => `pad.t.${id}`;
const GROUP_TARGETS = Object.fromEntries(TARGET_GROUPS.map((g) => [g, TARGETS.filter((x) => x.group === g)]));
const guessPlatform = () => {
  try {
    const p = `${navigator.userAgentData?.platform ?? navigator.platform ?? ""}`.toLowerCase();
    return p.includes("mac") ? "mac" : p.includes("linux") ? "linux" : "windows";
  } catch { return "windows"; }
};

export function mount(root, app) {
  injectPadStyle();
  const layerName = (i) => app.profile.layerNames?.[i] || t(`remap.layer${i}`);
  const preview = new PadPreview();
  let curveIdx = 0;
  let platform = guessPlatform();
  let cfgDirty = true;
  let curPad = null;        // the profile's pad section and the gamepad keys of the shown layer, refreshed when the model changes
  let layerKeys = new Map();
  const notes = new Map();   // field -> timer of the explanation shown under it

  // ============================================================================================ pad mode
  let lastType = padModeOf(app) === PAD_MODE.XINPUT ? PAD_MODE.XINPUT : PAD_MODE.HID;   // the type the toggle switches on
  const modeToggle = toggle({ label: pt("pad.mode.toggle"), checked: false, onChange: (v) => setPadMode(app, v ? lastType : PAD_MODE.OFF) });
  const typeSeg = segmented({
    label: pt("pad.type.label"), value: lastType,
    options: [
      { id: PAD_MODE.HID, label: pt("pad.type.hid"), title: pt("pad.type.hid.tip") },
      { id: PAD_MODE.XINPUT, label: pt("pad.type.xinput"), title: pt("pad.type.xinput.tip") },
    ],
    onChange: (m) => { lastType = m; if (padModeOf(app) !== PAD_MODE.OFF) setPadMode(app, m); else renderMode(); },
  });
  const typeNote = h("p.field-hint", { role: "note" });
  const modeChip = h("span.chip");
  const modeLine = h("p.pad-sub", { role: "status" });
  const checks = h("div.pad-checks");
  const modeCard = h("section.card", { "aria-labelledby": "pad-mode-h" },
    h("div.card-head", {}, h("h2#pad-mode-h.card-title", {}, pt("pad.mode.title"))),
    h("div.pad-lead", {}, modeToggle.el, h("div.pad-state", {}, modeChip, modeLine)),
    h("div.field", {}, h("span.field-label", {}, pt("pad.type.label")), typeSeg.el, typeNote),
    h("p.field-hint", {}, pt("pad.mode.intro")),
    h("ul.pad-explain", {}, [1, 2, 3, 4, 5].map((i) => h("li", {}, pt(`pad.mode.e${i}`)))),
    checks);

  /** What the keyboard holds right now for the pad mode (null = unknown: no keyboard data) */
  const savedMode = () => {
    if (!app.deviceBlob) return null;
    try { return decodeSettings(app.deviceBlob).device?.padMode ?? 0; } catch { return null; }
  };

  function renderMode() {
    const mode = padModeOf(app);
    if (mode !== PAD_MODE.OFF) lastType = mode;
    modeToggle.set(mode !== PAD_MODE.OFF);
    typeSeg.set(mode !== PAD_MODE.OFF ? mode : lastType);
    typeNote.textContent = pt(lastType === PAD_MODE.XINPUT ? "pad.type.xinput.note" : "pad.type.hid.note");
    modeChip.textContent = pt(mode === PAD_MODE.OFF ? "pad.state.off" : mode === PAD_MODE.HID ? "pad.state.on" : "pad.state.xinput");
    modeChip.className = `chip${mode !== PAD_MODE.OFF ? " on" : ""}`;
    const saved = savedMode();
    modeLine.textContent = saved === null ? pt("pad.line.local") : saved !== mode ? pt("pad.line.unsaved")
      : mode === PAD_MODE.HID ? pt("pad.line.saved.on") : mode === PAD_MODE.XINPUT ? pt("pad.line.saved.xinput") : pt("pad.line.saved.off");

    const items = checkPad(app.profile, app.layout, mode);
    checks.replaceChildren(...items.map((c) => {
      const layers = (c.layers ?? []).map(layerName).join(", ");
      const action = c.id === "off_but_bound" ? h("button.btn.sm", { type: "button", on: { click: () => setPadMode(app, lastType) } }, pt("pad.chk.off_but_bound.fix"))
        : null;
      return h("div.banner", { class: c.level === "warn" ? "warn" : "info", role: "status" }, icon(c.level === "warn" ? "warn" : "info"),
        h("div", {}, pt(`pad.chk.${c.id}`, { n: c.n ?? 0, layers }), action ? h("div", {}, action) : null));
    }));
  }

  // ============================================================================================ keyboard + bindings
  const card = keyboardCard(app, {
    mode: "depth", showTicks: false,
    legend: h("div.legend", {},
      h("span.lg", {}, h("i.sw.sw-depth"), pt("pad.lg.depth")),
      h("span.lg", {}, h("i.sw.sw-padkey"), pt("pad.lg.key"))),
  });

  function paintCaps() {
    const view = card.view;
    const keys = padKeys(app.profile, app.layout, app.layer);
    view.svg.classList.toggle("pad-has", keys.size > 0);
    for (const [slot, k] of view.keyEls) {
      const spec = keys.get(slot);
      k.g.classList.toggle("pad-key", !!spec);
      if (!spec) continue;
      const cap = targetById(spec.target).cap;
      k.alt.textContent = k.key.label.main;
      k.main.textContent = cap;
      k.main.setAttribute("class", `lbl ${cap.length > 4 ? "xs" : cap.length > 2 ? "sm" : ""}`);
    }
  }

  const layerTabs = h("div.seg.block", { role: "radiogroup", "aria-label": pt("pad.layer") });
  const paintTabs = () => {
    const counts = Array.from({ length: LAYER_COUNT }, (_, l) => padKeys(app.profile, app.layout, l).size);
    layerTabs.replaceChildren(...counts.map((n, l) => h("button", {
      type: "button", role: "radio", "aria-checked": String(app.layer === l), title: n ? pt("pad.layer.keys", { n }) : pt("pad.layer.none"),
      on: { click: () => { app.layer = l; card.view.refresh(); onModel(); } },
    }, layerName(l), n ? h("span.chip.on", { style: { marginLeft: "0.4rem", minHeight: "1.2rem", padding: "0 0.4rem" } }, String(n)) : null)));
  };

  const selLine = h("p.pad-sel");
  const chips = new Map();   // target id -> button
  const groupEls = TARGET_GROUPS.map((g) => h("div.pad-group", { class: g === "button" ? "wide" : "" },
    h("div.eyebrow", {}, pt(`pad.grp.${g}`)),
    h("div.pad-chips", { role: "group", "aria-label": pt(`pad.grp.${g}`) }, GROUP_TARGETS[g].map((tg) => {
      const b = h("button.chip", { type: "button", "aria-pressed": "false", title: pt(TARGET_KEY(tg.id)), "aria-label": pt(TARGET_KEY(tg.id)), on: { click: () => assign(tg.id) } }, tg.cap);
      chips.set(tg.id, b);
      return b;
    }))));
  const unbindBtn = h("button.btn.sm", { type: "button", title: pt("pad.unbind.tip"), on: { click: unbind } }, icon("reset"), pt("pad.unbind"));
  const clearLayerBtn = h("button.btn.sm.ghost", { type: "button", on: { click: clearLayer } }, icon("trash"), pt("pad.clearLayer"));
  const presetBtns = ["wasd", "ijkl", "triggers", "tfgh"].map((id) => h("button.chip", { type: "button", title: pt(`pad.preset.${id}.tip`), on: { click: () => preset(id) } }, pt(`pad.preset.${id}`)));

  // options of the selected gamepad keys
  const digital = toggle({ label: pt("pad.opt.digital"), onChange: (v) => patchSpec({ digital: v }) });
  const curveSeg = segmented({
    label: pt("pad.opt.curve"), value: null,
    options: Array.from({ length: CURVE_COUNT }, (_, i) => ({ id: i, label: String(i + 1), title: pt("pad.curve.n", { n: i + 1 }) })),
    onChange: (i) => { patchSpec({ curve: i }); },
  });
  const axisOpts = h("div.pad-opts", { "data-for": "axis" },
    h("div.field", {}, h("div.field-row", {}, h("span.field-label", {}, pt("pad.opt.curve")), h("button.btn.sm.ghost", { type: "button", on: { click: () => { curveIdx = selectedCurve() ?? curveIdx; renderCurve(); curveCard.scrollIntoView?.({ block: "nearest", behavior: "smooth" }); } } }, pt("pad.opt.editCurve"))), curveSeg.el,
      h("div.field-hint", {}, pt("pad.opt.curve.hint"))),
    h("div.field", {}, digital.el, h("div.field-hint", {}, pt("pad.opt.digital.hint"))));
  const useDepth = toggle({ label: pt("pad.opt.useDepth"), onChange: (v) => patchSpec({ act: v ? Math.max(BTN_ACT_MIN, actField.value ?? 40) : 0 }) });
  const actField = Object.assign(rangeField({
    id: "pad-act", label: pt("pad.opt.act"), hint: pt("pad.opt.act.hint"), min: unitsToMm(BTN_ACT_MIN), max: unitsToMm(BTN_ACT_MAX), step: MM_PER_UNIT,
    onInput: (mm) => { actField.value = Math.round(mm / MM_PER_UNIT); patchSpec({ act: actField.value }); },
    onClamp: (asked, v, side) => note("act", actField, pt(side === "below" ? "pad.note.act_low" : "pad.note.act_high", { v: v.toFixed(2) })),
  }), { value: null });
  const btnOpts = h("div.pad-opts", { "data-for": "button" }, useDepth.el, actField.el);
  const mixedOpts = h("p.field-hint.pad-opts", { "data-for": "mixed" }, pt("pad.opt.mixed"));
  const tip = h("p.field-hint.note.pad-tip", {}, icon("info"), pt("pad.tip.layer"));

  const bindCard = h("section.card", { "aria-labelledby": "pad-bind-h" },
    h("div.card-head", {}, h("h2#pad-bind-h.card-title", {}, pt("pad.bind.title")), h("div.row", {}, clearLayerBtn)),
    layerTabs,
    h("div", { style: { height: "var(--s3)" } }),
    selLine,
    h("div.pad-targets", {}, groupEls),
    h("div.btn-row", {}, unbindBtn),
    axisOpts, btnOpts, mixedOpts,
    h("div.pad-opts", {}, h("div.eyebrow", {}, pt("pad.preset.title")), h("div.pad-chips", {}, presetBtns)),
    tip);

  /** Specs of the selected keys on the shown layer: [{ slot, key, spec | null }] */
  const selection = () => app.selected.map((slot) => {
    const key = app.layout.bySlot.get(slot);
    return { slot, key, spec: specOfBinding(app.profile.keys[key.id].bindings[app.layer]), bound: app.profile.keys[key.id].bindings[app.layer] };
  });
  const unify = (list, get) => (list.length && list.every((x) => get(x) === get(list[0])) ? get(list[0]) : null);
  const kindOf = (spec) => (targetById(spec.target).kind === "axis" ? "axis" : "button");
  const selectedCurve = () => { const sel = selection().filter((x) => x.spec && kindOf(x.spec) === "axis" && !x.spec.digital); return unify(sel, (x) => x.spec.curve); };

  function note(field, fieldApi, text) {
    clearTimeout(notes.get(field));
    fieldApi.flag(text);
    notes.set(field, setTimeout(() => fieldApi.flag(""), 6000));
  }

  const restore = (key, layer) => defaultBinding(key, layer);
  function assign(targetId) {
    const sel = selection();
    if (!sel.length) return;
    const tg = targetById(targetId);
    const edits = sel.map(({ key, spec: old }) => {
      let spec = newSpec(targetId);
      if (old && kindOf(old) === (tg.kind === "axis" ? "axis" : "button")) spec = { ...spec, digital: old.digital, curve: old.curve, act: old.act };
      return { keyId: key.id, layer: app.layer, spec };
    });
    app.edit((p) => applyBindings(p, app.layout, edits, restore));
  }

  function patchSpec(patch) {
    const edits = selection().filter((x) => x.spec).map((x) => ({ keyId: x.key.id, layer: app.layer, spec: { ...x.spec, ...patch } }));
    const bad = edits.map((e) => specProblem(e.spec)).find(Boolean);
    if (bad) { toast(pt(`pad.bad.${bad}`), "error"); return; }
    if (edits.length) app.edit((p) => applyBindings(p, app.layout, edits, restore));
  }

  function unbind() {
    const edits = selection().filter((x) => x.spec).map((x) => ({ keyId: x.key.id, layer: app.layer, spec: null }));
    if (edits.length) app.edit((p) => applyBindings(p, app.layout, edits, restore));
  }

  async function clearLayer() {
    const keys = padKeys(app.profile, app.layout, app.layer);
    if (!keys.size) { toast(pt("pad.clearLayer.none")); return; }
    const ok = await confirmDialog({
      title: pt("pad.clearLayer.title", { layer: layerName(app.layer) }), body: pt("pad.clearLayer.body", { n: keys.size }), confirmLabel: pt("pad.clearLayer.ok"),
      cancelLabel: t("common.cancel"), danger: true,
    });
    if (!ok) return;
    const edits = [...keys.keys()].map((slot) => ({ keyId: app.layout.bySlot.get(slot).id, layer: app.layer, spec: null }));
    app.edit((p) => applyBindings(p, app.layout, edits, restore));
    toast(pt("pad.clearLayer.done", { n: edits.length }));
  }

  async function preset(id) {
    const edits = presetEdits(id, app.layout, app.layer);
    // keys the person has remapped on purpose are not overwritten without asking
    const custom = edits.filter((e) => {
      const b = app.profile.keys[e.keyId].bindings[app.layer];
      return !isPadBinding(b) && !sameBinding(b, restore(app.layout.byId.get(e.keyId), app.layer));
    });
    if (custom.length) {
      const ok = await confirmDialog({ title: pt("pad.preset.confirm.title"), body: pt("pad.preset.confirm.body", { n: custom.length }), confirmLabel: pt("pad.preset.confirm.ok"), cancelLabel: t("common.cancel") });
      if (!ok) return;
    }
    app.edit((p) => applyBindings(p, app.layout, edits, restore));
    toast(pt("pad.preset.done", { n: edits.length }));
  }

  function syncBindings() {
    paintTabs();
    const sel = selection();
    const bound = sel.filter((x) => x.spec);
    if (!sel.length) selLine.textContent = pt("pad.sel.none");
    else {
      const first = app.layout.bySlot.get(app.primary ?? sel[0].slot);
      const what = unify(bound.length === sel.length ? bound : [], (x) => x.spec.target);
      selLine.replaceChildren(
        sel.length === 1 ? pt("pad.sel.one", { key: first.name }) : pt("pad.sel.many", { n: sel.length }), " ",
        what ? h("strong", {}, pt(TARGET_KEY(what))) : bound.length === 0 ? pt("pad.sel.typing") : pt("pad.sel.mixed", { n: bound.length }));
    }
    const picked = unify(bound.length === sel.length ? bound : [], (x) => x.spec.target);
    for (const [id, b] of chips) {
      b.disabled = !sel.length;
      b.setAttribute("aria-pressed", String(id === picked));
    }
    unbindBtn.disabled = !bound.length;
    clearLayerBtn.disabled = padKeys(app.profile, app.layout, app.layer).size === 0;

    const kinds = new Set(bound.map((x) => kindOf(x.spec)));
    axisOpts.hidden = !(kinds.size === 1 && kinds.has("axis"));
    btnOpts.hidden = !(kinds.size === 1 && kinds.has("button"));
    mixedOpts.hidden = kinds.size < 2;
    if (!axisOpts.hidden) {
      digital.set(unify(bound, (x) => x.spec.digital) === true);
      const c = unify(bound, (x) => (x.spec.digital ? -1 : x.spec.curve));
      curveSeg.set(c === null || c < 0 ? "none" : c);
      curveSeg.el.querySelectorAll("button").forEach((b) => { b.disabled = unify(bound, (x) => x.spec.digital) === true; });
    }
    if (!btnOpts.hidden) {
      const act = unify(bound, (x) => x.spec.act);
      useDepth.set(act !== null && act > 0);
      actField.value = act;
      actField.setDisabled(act === null || act === 0);
      if (act !== null && act > 0) actField.set(unitsToMm(Math.max(act, BTN_ACT_MIN)));
      else if (act === 0) actField.set(unitsToMm(40));
      else actField.set(null);
    }
  }

  // ============================================================================================ curves + options
  const pad = () => padOf(app.profile);
  const curveTabs = h("div.seg.block.pad-curvetabs", { role: "radiogroup", "aria-label": pt("pad.curve.tabs") });
  const useLine = h("p.pad-sub");
  const plot = new CurvePlot({ onChange: (field, units) => editCurve(field, units) });
  const startField = rangeField({
    id: "pad-cs", label: pt("pad.curve.start"), hint: pt("pad.curve.start.hint"), min: unitsToMm(4), max: unitsToMm(64), step: MM_PER_UNIT,
    onInput: (mm) => editCurve("start", Math.round(mm / MM_PER_UNIT)),
    onClamp: (asked, v, side) => note("start", startField, pt(side === "below" ? "pad.curve.note.start_low" : "pad.curve.note.start_high", { min: "0.20", max: v.toFixed(2) })),
  });
  const endField = rangeField({
    id: "pad-ce", label: pt("pad.curve.end"), hint: pt("pad.curve.end.hint"), min: unitsToMm(8), max: unitsToMm(68), step: MM_PER_UNIT,
    onInput: (mm) => editCurve("end", Math.round(mm / MM_PER_UNIT)),
    onClamp: (asked, v, side) => note("end", endField, pt(side === "below" ? "pad.curve.note.end_low" : "pad.curve.note.end_high", { v: v.toFixed(2) })),
  });
  const shapeSeg = segmented({
    label: pt("pad.curve.shape"), block: true, value: null,
    options: [0, 1, 2, 3].map((i) => ({ id: i, label: pt(`pad.shape.${i}`), title: pt(`pad.shape.${i}.tip`) })),
    onChange: (i) => editCurve("shape", i),
  });
  const reverse = toggle({ label: pt("pad.curve.reverse"), onChange: (v) => editCurve("reverse", v) });
  const presetChips = CURVE_PRESETS.map((id) => h("button.chip", { type: "button", title: pt(`pad.cpreset.${id}.tip`), on: { click: () => setCurve(curvePreset(id)) } }, pt(`pad.cpreset.${id}`)));
  const resetCurveBtn = h("button.btn.sm.ghost", { type: "button", on: { click: () => setCurve(defaultPad().curves[curveIdx]) } }, icon("reset"), pt("pad.curve.reset"));
  const liveOff = h("p.pad-live-off", { hidden: true }, pt("pad.live.off"));

  const curveCard = h("section.card", { "aria-labelledby": "pad-curve-h" },
    h("div.card-head", {}, h("h2#pad-curve-h.card-title", {}, pt("pad.curve.title"))),
    h("p.field-hint", {}, pt("pad.curve.intro")),
    h("div", { style: { height: "var(--s3)" } }),
    curveTabs,
    h("div.pad-curvebody", {}, plot.el, liveOff, useLine,
      h("div.pad-twocol", {}, startField.el, endField.el),
      shapeSeg.el, reverse.el,
      h("div.pad-group", {}, h("div.eyebrow", {}, pt("pad.cpreset.title")), h("div.pad-chips", {}, presetChips, resetCurveBtn))));

  function setCurve(curve) {
    const p = pad();
    p.curves[curveIdx] = { start: curve.start, end: curve.end, shape: curve.shape, flags: curve.flags };
    app.edit((prof) => setPad(prof, p));
  }

  function editCurve(field, value) {
    const p = pad();
    const r = changeCurve(p.curves[curveIdx], field, value);
    p.curves[curveIdx] = r.curve;
    app.edit((prof) => setPad(prof, p));
    if (r.note) {
      const api = field === "start" ? startField : endField;
      note(field, api, pt(`pad.curve.note.${r.note}`, { min: "0.20", max: unitsToMm(p.curves[curveIdx].end - 4).toFixed(2), v: unitsToMm(field === "start" ? p.curves[curveIdx].start : p.curves[curveIdx].end).toFixed(2) }));
    }
  }

  function renderCurve() {
    const p = pad();
    const c = p.curves[curveIdx];
    const use = curveUse(app.profile, app.layout);
    curveTabs.replaceChildren(...p.curves.map((_, i) => h("button", {
      type: "button", role: "radio", "aria-checked": String(i === curveIdx), on: { click: () => { curveIdx = i; renderCurve(); } },
    }, pt("pad.curve.n", { n: i + 1 }), use[i] ? h("span.chip.on", { style: { marginLeft: "0.4rem", minHeight: "1.2rem", padding: "0 0.4rem" } }, String(use[i])) : null)));
    useLine.textContent = use[curveIdx] ? pt("pad.curve.used", { n: use[curveIdx] }) : pt("pad.curve.unused");
    plot.set(c);
    startField.set(unitsToMm(c.start));
    endField.set(unitsToMm(c.end));
    shapeSeg.set(c.shape);
    reverse.set((c.flags & 1) !== 0);
    renderOptions(p);
  }

  const deadField = rangeField({
    id: "pad-dz", label: pt("pad.opt.deadzone"), hint: pt("pad.opt.deadzone.hint"), min: 0, max: DEADZONE_MAX, step: 1, unit: "%",
    onInput: (v) => editOpts({ deadzone: v }),
    onClamp: (asked, v, side) => note("dz", deadField, pt(side === "below" ? "pad.note.dz_low" : "pad.note.dz_high", { v: String(v) })),
  });
  const circular = toggle({ label: pt("pad.opt.circular"), onChange: (v) => editOpts({ circular: v ? 1 : 0 }) });
  const optCard = h("section.card", { "aria-labelledby": "pad-opt-h" },
    h("div.card-head", {}, h("h2#pad-opt-h.card-title", {}, pt("pad.opts.title"))),
    h("div.fields", {}, deadField.el, h("div.field", {}, circular.el, h("div.field-hint", {}, pt("pad.opt.circular.hint")))));

  function editOpts(patch) {
    const p = pad();
    Object.assign(p, patch);
    app.edit((prof) => setPad(prof, p));
  }
  function renderOptions(p = pad()) {
    deadField.set(p.deadzone);
    circular.set(p.circular === 1);
  }

  // ============================================================================================ virtual gamepad
  const vpad = new VirtualPad();
  const tryBtn = h("button.btn.sm", { type: "button", title: pt("pad.try.tip"), on: { click: tryIt } }, pt("pad.try"));
  const padCard = h("section.card", { "aria-labelledby": "pad-vp-h" },
    h("div.card-head", {}, h("h2#pad-vp-h.card-title", {}, pt("pad.vp.title")), h("div.row", {}, tryBtn)),
    vpad.el,
    h("p.field-hint", { style: { marginTop: "var(--s3)" } }, pt("pad.vp.hint")));

  /** Demo keyboard only: press the selected gamepad keys (or all of this layer's) slowly to the bottom and back, so the curves can be watched. */
  function tryIt() {
    const bound = selection().filter((x) => x.spec).map((x) => x.slot);
    const slots = bound.length ? bound : [...padKeys(app.profile, app.layout, app.layer).keys()];
    if (!slots.length) { toast(pt("pad.try.none")); return; }
    slots.slice(0, 6).forEach((slot) => app.mock.press(slot, 340, 1600, 400, 1400));
  }

  // ============================================================================================ SDL text
  const platSeg = segmented({
    label: pt("pad.sdl.platform"), value: platform,
    options: PLATFORMS.map((p) => ({ id: p, label: pt(`pad.sdl.${p}`) })),
    onChange: (p) => { platform = p; renderSdl(); },
  });
  const guessInput = h("input", { type: "text", placeholder: pt("pad.sdl.guid.ph"), "aria-label": pt("pad.sdl.guid"), spellcheck: "false", autocomplete: "off", on: { input: renderSdl } });
  const guidFlag = h("div.field-hint");
  const sdlText = h("textarea.pad-sdl", { readonly: true, "aria-label": pt("pad.sdl.text"), rows: 5, on: { focus: () => sdlText.select() } });
  const copyBtn = h("button.btn.sm", { type: "button", on: { click: copySdl } }, icon("copy"), pt("pad.sdl.copy"));
  const where = h("details.pad-where", {}, h("summary", {}, pt("pad.where.title")), h("ul", {}, [1, 2, 3, 4].map((i) => h("li", {}, pt(`pad.where.${i}`)))));
  const sdlCard = h("section.card", { "aria-labelledby": "pad-sdl-h" },
    h("div.card-head", {}, h("h2#pad-sdl-h.card-title", {}, pt("pad.sdl.title")), platSeg.el),
    h("p.field-hint", {}, pt("pad.sdl.intro")),
    h("div", { style: { height: "var(--s3)" } }),
    sdlText,
    h("div.btn-row", {}, copyBtn),
    h("div.field", { style: { marginTop: "var(--s4)" } }, h("label.field-label", {}, pt("pad.sdl.guid")), guessInput, guidFlag, h("div.field-hint", {}, pt("pad.sdl.guid.hint", { guid: sdlGuid(platform) }))),
    where);

  function renderSdl() {
    const raw = guessInput.value.trim();
    const custom = raw && isGuid(raw);
    guidFlag.textContent = raw && !custom ? pt("pad.sdl.guid.bad") : "";
    guidFlag.style.color = raw && !custom ? "var(--warn)" : "";
    sdlText.value = sdlMapping({ platform, guid: custom ? raw : null });
  }
  async function copySdl() {
    try {
      await navigator.clipboard.writeText(sdlText.value);
      toast(pt("pad.sdl.copied"));
    } catch {
      sdlText.focus();
      sdlText.select();
      toast(pt("pad.sdl.copyfail"), "error");
    }
  }

  // ============================================================================================ glue
  function onModel() {
    cfgDirty = true;
    curPad = pad();
    layerKeys = padKeys(app.profile, app.layout, app.layer);
    paintCaps();
    renderMode();
    syncBindings();
    renderCurve();
  }
  const offs = [
    app.on("profile", onModel),
    app.on("sync", renderMode),
    app.on("selection", () => { syncBindings(); }),
    app.on("device", () => { liveOff.hidden = app.feed.hasData || app.device.capabilities.live; tryBtn.hidden = app.device.kind !== "mock"; }),
  ];

  let raf = 0;
  const tick = (now) => {
    if (cfgDirty) {
      preview.configure(curPad, layerKeys);
      cfgDirty = false;
    }
    const report = preview.step(app.feed.depth, now);
    vpad.update(report, { circular: curPad.circular === 1, deadzone: curPad.deadzone });
    // live marker on the curve: the deepest key that uses the shown curve
    let best = null;
    for (const [slot, spec] of layerKeys) {
      if (kindOf(spec) === "axis" && !spec.digital && spec.curve === curveIdx) best = Math.max(best ?? 0, app.feed.depth[slot]);
    }
    const live = app.feed.hasData || app.device.capabilities.live;
    plot.setLive(live ? best : null);
    liveOff.hidden = live;
    raf = requestAnimationFrame(tick);
  };

  root.append(h("div.page-stack.pad", {},
    modeCard,
    h("div.pad-grid", {},
      h("div.pad-col", {}, card.el, bindCard, sdlCard),
      h("div.pad-col", {}, padCard, curveCard, optCard))));
  tryBtn.hidden = app.device.kind !== "mock";
  renderSdl();
  onModel();
  raf = requestAnimationFrame(tick);
  return {
    destroy() {
      cancelAnimationFrame(raf);
      offs.forEach((o) => o());
      notes.forEach(clearTimeout);
      card.destroy();
    },
  };
}
