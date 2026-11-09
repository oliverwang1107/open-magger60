// The scene half of the lights page: per-key colours (select, paint, fill, copy / paste, undo / redo, gradients), indicator rules, and the
// scene on the keyboard (state, write, read back, presets, files). All behaviour is in core/scene_model.js and core/scene_keeper.js; this file
// only builds controls and shows state. Every control is a real button / input / select, so the keyboard and screen readers work.
import { h, icon, download, toast } from "../dom.js";
import { t } from "../i18n.js";
import { rangeField } from "../ui/controls.js";
import { classifyProblem } from "../device/errors.js";
import { hexToRgb, rgbToHex } from "../core/lighting.js";
import { LED_COUNT, ledOfSlot, slotOfLed } from "../core/led_map.js";
import { PALETTE_SIZE, readingOrder, allLeds } from "../core/scene_colors.js";
import { MAX_STOPS } from "../core/scene_model.js";
import { LS, OFF } from "../core/lightscene_codec.js";
import { RULE_SOURCES, MAX_RULES, DIM_DEFAULT, makeFlags, ruleOff, ruleBlinks, capsLockRule, sourceInfo, NO_INPUTS } from "../core/scene_rules.js";
import { PRESET_IDS, SceneFileError } from "../core/scene_presets.js";
import { LED_EFFECT_CUSTOM } from "../core/led.js";

const SWATCHES = ["#ffffff", "#ff0000", "#ff8000", "#ffff00", "#00ff00", "#00ffd5", "#0080ff", "#4000ff", "#c000ff", "#ff0080", "#000000"];
const why = (err) => {
  const p = classifyProblem(err);
  return `${t(`problem.${p.kind}.title`)}${p.detail ? ` (${p.detail})` : ""}`;
};
const hex = (c) => rgbToHex(c);
const isTyping = (el) => !!el && (el.tagName === "INPUT" || el.tagName === "SELECT" || el.tagName === "TEXTAREA" || el.isContentEditable);

/** Shown instead of a panel when the keyboard cannot keep a light scene. */
export function sceneNotice(kind, goDevice) {
  return h("div.empty.small", {}, icon("lighting"), h("h3", {}, t(`scene.na.${kind}.title`)), h("p", {}, t(`scene.na.${kind}.body`)),
    goDevice ? h("button.btn.sm", { type: "button", on: { click: goDevice } }, t("led.goDevice")) : null);
}

/**
 * ctx = { app, model (SceneModel), keeper (SceneKeeper), getLink: () => LedKeeper | null, inputs (object the preview reads), goDevice }.
 * Returns { colors, rules, scene, help, paint(), destroy() }.
 */
export function buildScenePanels(ctx) {
  const { app, model, keeper } = ctx;
  const offs = [];
  const painters = [];
  const repaint = () => painters.forEach((fn) => fn());
  offs.push(model.onChange(repaint), app.on("selection", repaint));

  // ---------- shared pieces ----------
  const liveBox = () => {
    const input = h("input", { type: "checkbox", on: { change: () => setLive(input.checked) } });
    const el = h("label.toggle", {}, input, h("span.track"), h("span", {}, t("scene.live")));
    painters.push(() => { input.checked = keeper.live; });
    return h("div.field", {}, el, h("div.field-hint", {}, t("scene.live.hint")));
  };
  function setLive(on) {
    keeper.setLive(on);
    const link = ctx.getLink();
    if (on && link?.loaded && link.state.effect !== LED_EFFECT_CUSTOM) link.set({ effect: LED_EFFECT_CUSTOM });   // the colours only show with the custom effect
    repaint();
  }
  const flashNote = h("p.field-hint", {}, t("scene.flashnote"));

  // ================= per-key colours =================
  const selLine = h("p.field-hint.scene-sel", { role: "status", "aria-live": "polite" });
  const shadeLine = h("p.field-hint.scene-shades");
  const colorInput = h("input", { type: "color", "aria-label": t("scene.pick"), value: "#ff0000", on: { input: () => model.paint(hexToRgb(colorInput.value), "selection", { key: "pick" }) } });
  const swatches = SWATCHES.map((c) => h("button.swatch", {
    type: "button", style: { background: c }, title: c, "aria-label": c,
    on: { click: () => { colorInput.value = c; model.paint(hexToRgb(c), "selection"); model.editor.seal(); } },
  }));
  const needSel = () => { toast(t("scene.needSelection")); return false; };
  const fillBtn = (scope, label) => h("button.btn.sm", { type: "button", "data-fill": scope, on: { click: () => {
    if (scope !== "all" && !model.selectedLeds.length) { needSel(); return; }
    model.editor.seal();
    model.paint(model.paintColor, scope);
    model.editor.seal();
  } } }, label);
  const btnUndo = h("button.btn.sm", { type: "button", "data-act": "undo", title: "Ctrl+Z", on: { click: () => model.undo() } }, icon("reset"), t("scene.undo"));
  const btnRedo = h("button.btn.sm", { type: "button", "data-act": "redo", title: "Ctrl+Y", on: { click: () => model.redo() } }, t("scene.redo"));
  const btnCopy = h("button.btn.sm", { type: "button", "data-act": "copy", title: "Ctrl+C", on: { click: () => { if (!model.copy()) needSel(); else toast(t("scene.copied", { n: model.selectedLeds.length })); } } }, icon("copy"), t("scene.copy"));
  const btnPaste = h("button.btn.sm", { type: "button", "data-act": "paste", title: "Ctrl+V", on: { click: () => { if (!model.hasClipboard) toast(t("scene.nothingCopied")); else if (!model.paste()) needSel(); } } }, t("scene.paste"));
  const selAll = h("button.btn.sm", { type: "button", on: { click: () => app.select(app.layout.keys.map((k) => k.slot), "replace") } }, t("scene.selectAll"));
  const selNone = h("button.btn.sm", { type: "button", on: { click: () => app.select([], "replace") } }, t("scene.selectNone"));

  // ---- gradient ----
  const g = model.gradient;
  let gKind = "linear";
  let gTouched = false;      // the gradient is only previewed once the person has touched a gradient control (selecting keys alone paints nothing)
  const gMsg = h("p.field-hint", { role: "status", "aria-live": "polite" });
  const angle = rangeField({ id: "sc-angle", label: t("scene.g.angle"), hint: t("scene.g.angle.hint"), min: 0, max: 359, step: 1, unit: "°", onInput: (v) => { g.angle = v; gChanged(); } });
  const hueSpan = rangeField({ id: "sc-hue", label: t("scene.g.hueSpan"), hint: t("scene.g.hueSpan.hint"), min: 30, max: 360, step: 5, unit: "°", onInput: (v) => { g.hueSpan = v; gChanged(); } });
  const hueFrom = rangeField({ id: "sc-hue0", label: t("scene.g.hueFrom"), min: 0, max: 359, step: 1, unit: "°", onInput: (v) => { g.hueFrom = v; gChanged(); } });
  const stopsBox = h("div.g-stops", { role: "group", "aria-label": t("scene.g.stops") });
  const addStop = h("button.btn.sm", { type: "button", on: { click: () => {
    if (g.stops.length >= MAX_STOPS) return;
    const sorted = [...g.stops].sort((a, b) => a.pos - b.pos);
    const [a, b] = sorted.slice(-2);
    g.stops.push({ pos: (a.pos + b.pos) / 2, color: [...b.color] });
    buildStops(); gChanged();
  } } }, icon("plus"), t("scene.g.addStop"));
  function buildStops() {
    stopsBox.replaceChildren(...g.stops.map((s, i) => {
      const c = h("input", { type: "color", value: hex(s.color), "aria-label": t("scene.g.stopColor", { n: i + 1 }), on: { input: () => { s.color = hexToRgb(c.value); gChanged(); } } });
      const p = h("input", { type: "number", min: 0, max: 100, step: 1, value: String(Math.round(s.pos * 100)), inputmode: "numeric", "aria-label": t("scene.g.stopPos", { n: i + 1 }),
        on: { change: () => { s.pos = Math.max(0, Math.min(100, Number(p.value) || 0)) / 100; p.value = String(Math.round(s.pos * 100)); gChanged(); } } });
      const rm = h("button.btn.sm.icon", { type: "button", disabled: g.stops.length <= 2, "aria-label": t("scene.g.removeStop", { n: i + 1 }), title: t("scene.g.removeStop", { n: i + 1 }),
        on: { click: () => { g.stops.splice(i, 1); buildStops(); gChanged(); } } }, icon("x"));
      return h("div.g-stop", {}, c, h("span.num-input", {}, p, h("span", {}, "%")), rm);
    }));
    addStop.disabled = g.stops.length >= MAX_STOPS;
  }
  const gradientBar = h("div.g-bar", { role: "img", "aria-label": t("scene.g.bar") });
  function paintBar() {
    const st = [...g.stops].sort((a, b) => a.pos - b.pos);
    gradientBar.style.background = `linear-gradient(90deg, ${st.map((s) => `${hex(s.color)} ${Math.round(s.pos * 100)}%`).join(", ")})`;
    gradientBar.hidden = gKind !== "linear";
  }
  function gChanged() {
    gTouched = true;
    paintBar();
    if (keeper.live && model.selectedLeds.length > 1) model.applyGradient(gKind, "selection", { key: "grad" });   // live: it goes to the keyboard as it is edited
    else showPreview();
  }
  function showPreview() {
    if (model.selectedLeds.length > 1) { model.previewGradient(gKind); gMsg.textContent = t("scene.g.previewing"); }
    else { model.clearPreview(); gMsg.textContent = ""; }
  }
  const kindSeg = h("div.seg", { role: "radiogroup", "aria-label": t("scene.g.kind") }, ["linear", "rainbow"].map((k) => h("button", {
    type: "button", role: "radio", "data-kind": k, on: { click: () => { gKind = k; paintKind(); gChanged(); } },
  }, t(`scene.g.${k}`))));
  function paintKind() {
    kindSeg.querySelectorAll("button").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.kind === gKind)));
    angle.show(true);
    hueSpan.show(gKind === "rainbow");
    hueFrom.show(gKind === "rainbow");
    stopsBox.hidden = addStop.hidden = gKind !== "linear";
    paintBar();
  }
  const applyBtn = h("button.btn.primary.sm", { type: "button", "data-act": "apply-gradient", on: { click: () => {
    if (model.selectedLeds.length < 2) { gMsg.textContent = t("scene.g.needTwo"); return; }
    model.editor.seal();
    model.applyGradient(gKind, "selection");
    model.editor.seal();
    gTouched = false;
    gMsg.textContent = t("scene.g.applied");
  } } }, icon("check"), t("scene.g.apply"));
  const cancelBtn = h("button.btn.sm", { type: "button", on: { click: () => { gTouched = false; model.clearPreview(); gMsg.textContent = ""; } } }, t("scene.g.cancel"));
  const rowSel = h("select", { "aria-label": t("scene.g.rowN") }, [0, 1, 2, 3, 4].map((r) => h("option", { value: String(r) }, t("kb.row", { n: r + 1 }))));
  const colSel = h("select", { "aria-label": t("scene.g.colN") }, Array.from({ length: Math.ceil(app.layout.width) }, (_, c) => h("option", { value: String(c) }, t("kb.col", { n: c + 1 }))));
  const lineBtn = (axis, sel, label) => h("button.btn.sm", { type: "button", "data-act": `line-${axis}`, on: { click: () => {
    model.editor.seal();
    const ok = model.applyLineGradient(axis, Number(sel.value), gKind);
    model.editor.seal();
    gMsg.textContent = ok ? t("scene.g.applied") : t("scene.g.nothing");
  } } }, label);
  buildStops();
  paintKind();
  angle.set(g.angle); hueSpan.set(g.hueSpan); hueFrom.set(g.hueFrom);
  offs.push(app.on("selection", () => { if (gTouched && !keeper.live) showPreview(); }));

  const colors = h("div.scene-sec", {},
    h("p.field-hint", {}, t("scene.colors.intro")),
    selLine,
    h("div.btn-row.tight", {}, selAll, selNone),
    h("div.field", {}, h("div.field-label", {}, t("scene.pick")), h("div.palette", {}, swatches, h("label.swatch.custom", { title: t("led.custom") }, colorInput))),
    h("div.field", {}, h("div.field-label", {}, t("scene.fill")), h("div.btn-row.tight", {}, fillBtn("selection", t("scene.fill.selection")), fillBtn("rows", t("scene.fill.rows")), fillBtn("cols", t("scene.fill.cols")), fillBtn("all", t("scene.fill.all")))),
    h("div.btn-row.tight", {}, btnUndo, btnRedo, btnCopy, btnPaste),
    shadeLine,
    h("details.scene-gradient", { open: true },
      h("summary", {}, t("scene.g.title")),
      h("div.led-fields", {}, h("p.field-hint", {}, t("scene.g.intro")), kindSeg, gradientBar, stopsBox, addStop, angle.el, hueFrom.el, hueSpan.el,
        h("div.btn-row.tight", {}, applyBtn, cancelBtn), gMsg,
        h("div.field", {}, h("div.field-label", {}, t("scene.g.line")),
          h("div.btn-row.tight", {}, rowSel, lineBtn("row", rowSel, t("scene.g.applyRow")), colSel, lineBtn("column", colSel, t("scene.g.applyCol")))))),
    liveBox());
  painters.push(() => {
    const n = model.selectedLeds.length;
    selLine.textContent = n ? t("scene.selected", { n }) : t("scene.selected.none");
    btnUndo.disabled = !model.canUndo;
    btnRedo.disabled = !model.canRedo;
    btnCopy.disabled = n === 0;
    btnPaste.disabled = !model.hasClipboard || n === 0;
    const sh = model.shades;
    shadeLine.textContent = model.overPalette ? t("scene.shades.over", { n: sh, max: PALETTE_SIZE }) : t("scene.shades", { n: sh, max: PALETTE_SIZE });
    shadeLine.classList.toggle("warn", model.overPalette);
  });

  // ================= indicator rules =================
  const rulesList = h("div.rule-list");
  const rulesWarn = h("div.field-hint.rule-warn", { role: "status" });
  let rows = [];
  const keyChoices = readingOrder(app.layout, allLeds()).map((l) => ({ led: l, name: app.layout.bySlot.get(slotOfLed(l)).name }));
  const sourceLabel = (s) => t(`scene.src.${s.id}`);

  function buildRuleRow(i) {
    const rule = () => model.rules[i];
    const src = h("select", { id: `rule-${i}-src`, on: { change: () => model.updateRule(i, { source: RULE_SOURCES.find((s) => s.id === src.value).src, param: 0 }) } },
      RULE_SOURCES.map((s) => h("option", { value: s.id }, sourceLabel(s))));
    const param = h("select", { "aria-label": t("scene.rule.param"), on: { change: () => model.updateRule(i, { param: Number(param.value) }) } });
    const key = h("select", { "aria-label": t("scene.rule.key"), on: { change: () => model.updateRule(i, { led: Number(key.value) }) } },
      keyChoices.map((k) => h("option", { value: String(k.led) }, k.name)));
    const useSel = h("button.btn.sm", { type: "button", title: t("scene.rule.useSelected"), on: { click: () => {
      const sel = model.selectedLeds;
      if (!sel.length) { needSel(); return; }
      model.updateRule(i, { led: sel[0] });
    } } }, t("scene.rule.useSelected"));
    const color = h("input", { type: "color", "aria-label": t("scene.rule.color"), on: { input: () => model.updateRule(i, { color: hexToRgb(color.value) }) } });
    const off = h("select", { "aria-label": t("scene.rule.off"), on: { change: () => model.updateRule(i, { flags: makeFlags(Number(off.value), ruleBlinks(rule())), dim: Number(off.value) === OFF.DIM ? (rule().dim || Math.round((DIM_DEFAULT * 255) / 100)) : 0 }) } },
      [OFF.KEEP, OFF.BLACK, OFF.DIM].map((v) => h("option", { value: String(v) }, t(`scene.off.${["keep", "black", "dim"][v]}`))));
    const dim = rangeField({ id: `rule-${i}-dim`, label: t("scene.rule.dim"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => model.updateRule(i, { dim: Math.round((v * 255) / 100) }) });
    const blinkIn = h("input", { type: "checkbox", on: { change: () => model.updateRule(i, { flags: makeFlags(ruleOff(rule()), blinkIn.checked) }) } });
    const blink = h("label.toggle", {}, blinkIn, h("span.track"), h("span", {}, t("scene.rule.blink")));
    const up = h("button.btn.sm.icon", { type: "button", "aria-label": t("scene.rule.up"), title: t("scene.rule.up"), on: { click: () => model.moveRule(i, -1) } }, "↑");
    const down = h("button.btn.sm.icon", { type: "button", "aria-label": t("scene.rule.down"), title: t("scene.rule.down"), on: { click: () => model.moveRule(i, 1) } }, "↓");
    const del = h("button.btn.sm.icon.danger", { type: "button", "aria-label": t("scene.rule.remove"), title: t("scene.rule.remove"), on: { click: () => model.removeRule(i) } }, icon("trash"));
    const title = h("strong", {});
    const el = h("fieldset.rule", {},
      h("legend", {}, title),
      h("div.rule-grid", {},
        h("label.field", {}, h("span.field-label", {}, t("scene.rule.when")), src),
        h("label.field.rule-param", {}, h("span.field-label", {}, t("scene.rule.param")), param),
        h("div.field", {}, h("span.field-label", {}, t("scene.rule.key")), h("div.btn-row.tight", {}, key, useSel)),
        h("div.field", {}, h("span.field-label", {}, t("scene.rule.color")), color),
        h("label.field", {}, h("span.field-label", {}, t("scene.rule.off")), off),
        dim.el, blink),
      h("div.btn-row.tight", {}, up, down, del));
    const sync = () => {
      const r = rule();
      if (!r) return;
      const info = sourceInfo(r.source);
      src.value = info.id;
      title.textContent = t("scene.rule.n", { n: i + 1 });
      const pSig = String(info.params);
      if (param.dataset.sig !== pSig) {
        param.dataset.sig = pSig;
        param.replaceChildren(...Array.from({ length: info.params }, (_, v) => h("option", { value: String(v) }, info.id === "layer" ? t("scene.layerN", { n: v }) : t("scene.profileN", { n: v + 1 }))));
      }
      el.querySelector(".rule-param").hidden = info.params === 0;
      param.value = String(r.param);
      key.value = String(r.led);
      color.value = hex(r.color);
      off.value = String(ruleOff(r));
      dim.show(ruleOff(r) === OFF.DIM);
      dim.set(Math.round((r.dim * 100) / 255));
      blinkIn.checked = ruleBlinks(r);
      up.disabled = i === 0;
      down.disabled = i === model.rules.length - 1;
    };
    return { el, sync };
  }

  const addSel = h("select", { "aria-label": t("scene.rule.add") }, RULE_SOURCES.map((s) => h("option", { value: s.id }, sourceLabel(s))));
  const addBtn = h("button.btn.sm", { type: "button", "data-act": "add-rule", on: { click: () => {
    const sel = model.selectedLeds;
    model.addRule(addSel.value, sel.length ? sel[0] : 0, [255, 80, 60]);
  } } }, icon("plus"), t("scene.rule.add"));
  const capsBtn = h("button.btn.sm", { type: "button", "data-act": "add-caps", on: { click: () => model.addRaw(capsLockRule(app.layout)) } }, t("scene.rule.capsQuick"));
  const inputsBox = h("fieldset.rule-inputs", {},
    h("legend", {}, t("scene.sim.title")),
    h("p.field-hint", {}, t("scene.sim.hint")),
    h("div.rule-sim", {}, [["num", "scene.src.num"], ["caps", "scene.src.caps"], ["scroll", "scene.src.scroll"], ["pad", "scene.src.pad"], ["toggle", "scene.src.toggle"], ["external", "scene.src.external"]].map(([k, label]) => {
      const cb = h("input", { type: "checkbox", "data-sim": k, on: { change: () => { ctx.inputs[k] = cb.checked; } } });
      return h("label.toggle", {}, cb, h("span.track"), h("span", {}, t(label)));
    })),
    h("div.rule-sim", {},
      h("label.field", {}, h("span.field-label", {}, t("scene.sim.layer")), h("select", { on: { change: (e) => { ctx.inputs.layer = Number(e.target.value); } } }, [0, 1, 2, 3].map((v) => h("option", { value: String(v) }, t("scene.layerN", { n: v }))))),
      h("label.field", {}, h("span.field-label", {}, t("scene.sim.profile")), h("select", { on: { change: (e) => { ctx.inputs.profile = Number(e.target.value); } } }, [0, 1, 2, 3].map((v) => h("option", { value: String(v) }, t("scene.profileN", { n: v + 1 })))))));
  Object.assign(ctx.inputs, NO_INPUTS);

  const rules = h("div.scene-sec", {},
    h("p.field-hint", {}, t("scene.rules.intro", { max: MAX_RULES })),
    h("p.field-hint", {}, t("scene.rules.alwaysOn")),
    rulesList, rulesWarn,
    h("div.btn-row.tight", {}, addSel, addBtn, capsBtn),
    inputsBox);
  painters.push(() => {
    const n = model.rules.length;
    if (rows.length !== n) {
      rows = Array.from({ length: n }, (_, i) => buildRuleRow(i));
      rulesList.replaceChildren(...(n ? rows.map((r) => r.el) : [h("p.field-hint", {}, t("scene.rules.none"))]));
    }
    rows.forEach((r) => r.sync());
    addBtn.disabled = capsBtn.disabled = !model.canAddRule;
    addBtn.title = model.canAddRule ? "" : t("scene.rules.max", { max: MAX_RULES });
    const notes = [];
    for (const s of model.ruleShadows) notes.push(t("scene.rules.shadowed", { n: s.rule + 1, m: s.shadowedBy + 1 }));
    if (!model.canAddRule) notes.push(t("scene.rules.max", { max: MAX_RULES }));
    rulesWarn.textContent = notes.join(" ");
  });

  // ================= the scene on the keyboard =================
  const stateBox = h("div.banner.scene-state", { role: "status", "aria-live": "polite" });
  const loadBox = h("div.banner.warn", { hidden: true });
  const loadErr = h("div.banner.error", { hidden: true, role: "alert" });
  const effectBox = h("div.banner.info", { hidden: true });
  const cmpLine = h("p.field-hint", { role: "status", "aria-live": "polite" });
  const sizeLine = h("p.field-hint");
  const writeBtn = h("button.btn.primary.sm", { type: "button", "data-act": "write", on: { click: () => { cmpLine.textContent = ""; keeper.saveNow(); } } }, icon("check"), t("scene.write"));
  const reloadBtn = h("button.btn.sm", { type: "button", "data-act": "reload", on: { click: async () => {
    cmpLine.textContent = "";
    try { await keeper.load(); loadErr.hidden = true; model.resetFromKeeper(); toast(t("scene.reloaded")); } catch (err) { cmpLine.textContent = t("scene.readFail", { why: why(err) }); }
  } } }, icon("reset"), t("scene.reload"));
  const cmpBtn = h("button.btn.sm", { type: "button", "data-act": "compare", on: { click: async () => {
    try {
      const r = await keeper.compare();
      cmpLine.textContent = r.unreadable ? t("scene.cmp.unreadable") : r.same ? t("scene.cmp.same") : t("scene.cmp.differs");
    } catch (err) { cmpLine.textContent = t("scene.readFail", { why: why(err) }); }
  } } }, t("scene.compare"));
  const customBtn = h("button.btn.sm", { type: "button", on: { click: () => ctx.getLink()?.set({ effect: LED_EFFECT_CUSTOM }) } }, t("scene.useCustom"));
  effectBox.append(icon("info"), h("div", {}, h("p", {}, t("scene.effectHint")), customBtn));

  const presetBtns = PRESET_IDS.map((id) => h("button.fx", { type: "button", "data-preset": id, title: t(`scene.preset.${id}.d`), on: { click: () => { model.editor.seal(); model.applyPreset(id); model.editor.seal(); toast(t("scene.preset.applied", { name: t(`scene.preset.${id}`) })); } } }, t(`scene.preset.${id}`)));
  const fileMsg = h("p.field-hint", { role: "status", "aria-live": "polite" });
  const fileIn = h("input", { type: "file", accept: ".json,application/json", hidden: true, "aria-label": t("scene.import"), on: { change: async () => {
    const f = fileIn.files?.[0];
    fileIn.value = "";
    if (!f) return;
    try {
      const name = model.importText(await f.text());
      fileMsg.textContent = t("scene.import.ok", { name: name || f.name });
    } catch (err) {
      fileMsg.textContent = err instanceof SceneFileError ? t(`scene.file.err.${err.code}`) : t("scene.file.err.json");
    }
  } } });

  const scene = h("div.scene-sec", {},
    loadErr, stateBox, loadBox, effectBox,
    h("div.btn-row.tight", {}, writeBtn, reloadBtn, cmpBtn), cmpLine, sizeLine,
    liveBox(), flashNote,
    h("div.field", {}, h("div.field-label", {}, t("scene.presets")), h("div.fx-grid", {}, presetBtns), h("div.field-hint", {}, t("scene.presets.hint"))),
    h("div.field", {}, h("div.field-label", {}, t("scene.files")),
      h("div.btn-row.tight", {},
        h("button.btn.sm", { type: "button", "data-act": "export", on: { click: () => download("magger60-light-scene.json", model.exportText("Magger60"), "application/json") } }, icon("download"), t("scene.export")),
        h("button.btn.sm", { type: "button", "data-act": "import", on: { click: () => fileIn.click() } }, icon("upload"), t("scene.import")), fileIn),
      fileMsg, h("div.field-hint", {}, t("scene.files.hint"))));
  painters.push(() => {
    const sv = keeper.save;
    let kind = "info", ic = "info", text;
    if (sv.kind === "saved") { kind = "ok"; ic = "check"; text = t("scene.state.saved"); }
    else if (sv.kind === "saving") { text = t(`scene.state.saving.${sv.step === "busy" ? "busy" : sv.step === "wait" ? "wait" : "step"}`); }
    else if (sv.kind === "failed") { kind = "error"; ic = "warn"; text = t("scene.state.failed", { why: sv.error ? ` (${why(sv.error)})` : "" }); }
    else if (sv.kind === "pending") { kind = "warn"; ic = "warn"; text = t("scene.state.pending"); }
    else { kind = "warn"; ic = "warn"; text = t(`scene.na.${sv.reason}.body`); }
    stateBox.className = `banner scene-state ${kind}`;
    stateBox.dataset.save = sv.kind;
    stateBox.replaceChildren(icon(ic), h("div", {}, text));
    const busy = sv.kind === "saving" || keeper.busy;
    writeBtn.disabled = busy || sv.kind === "none" || !keeper.loaded;
    reloadBtn.disabled = busy || sv.kind === "none";
    cmpBtn.disabled = busy || sv.kind === "none";
    loadBox.hidden = keeper.onKeyboard.kind !== "unreadable";
    if (!loadBox.hidden) loadBox.replaceChildren(icon("warn"), h("div", {}, t("scene.unreadable")));
    const link = ctx.getLink();
    effectBox.hidden = !(link?.loaded && link.state.effect !== LED_EFFECT_CUSTOM);
    sizeLine.textContent = t("scene.size", { bytes: model.encodedSize ?? "?", max: LS.maxEncoded });
  });

  // ================= what each layer does =================
  const help = h("details.card.scene-help", {},
    h("summary", {}, t("scene.help.title")),
    h("ol.scene-layers", {}, ["base", "keys", "rules", "limits", "idle", "ext"].map((k) => h("li", {}, h("strong", {}, t(`scene.help.${k}`)), " ", t(`scene.help.${k}.d`)))),
    h("p.field-hint", {}, t("scene.help.note")));

  // keyboard shortcuts while the focus is not in a text field
  const onKey = (e) => {
    if (isTyping(e.target) || !(e.ctrlKey || e.metaKey)) return;
    const k = e.key.toLowerCase();
    if (k === "z") { e.preventDefault(); e.shiftKey ? model.redo() : model.undo(); }
    else if (k === "y") { e.preventDefault(); model.redo(); }
    else if (k === "c" && model.selectedLeds.length) { e.preventDefault(); model.copy(); }
    else if (k === "v" && model.hasClipboard && model.selectedLeds.length) { e.preventDefault(); model.paste(); }
  };
  ctx.keyTarget?.addEventListener("keydown", onKey);

  return {
    colors, rules, scene, help,
    paint: repaint,
    /** The first read of the scene failed: say so (the page keeps working with the default scene; Reload tries again). */
    setLoadError(msg) { loadErr.hidden = false; loadErr.replaceChildren(icon("warn"), h("div", {}, t("scene.loadFail", { why: msg }))); },
    destroy() { offs.forEach((off) => off()); ctx.keyTarget?.removeEventListener("keydown", onKey); },
  };
}

export { ledOfSlot, LED_COUNT };
