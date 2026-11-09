// Trigger page: mode, actuation / release point, Rapid Trigger steps, dead zones for the selected keys, with a travel diagram that
// redraws as you edit and shows the sensor noise floor. Every limit comes from core/limits.js; a value outside a limit is not
// accepted silently: the field says why it moved.
import { h, s, icon, toast } from "../dom.js";
import { t } from "../i18n.js";
import { keyboardCard } from "../ui/kb-card.js";
import { rangeField, segmented, toggle } from "../ui/controls.js";
import { applyTriggerPatch } from "../core/profile.js";
import { MODE, LIMITS } from "../core/trigger.js";
import { NOISE_FLOOR_MM, HYSTERESIS_MIN_MM } from "../core/limits.js";
import { MAX_TRAVEL_MM, rawToMm } from "../core/units.js";

const PRESETS = [
  { id: "default", patch: { mode: MODE.NORMAL, actuation: 1.2, release: 0.8, topDead: 0, bottomDead: 0 } },
  { id: "typing", patch: { mode: MODE.NORMAL, actuation: 1.5, release: 1.1, topDead: 0, bottomDead: 0 } },
  { id: "gaming", patch: { mode: MODE.RT, actuation: 1.0, rapidDown: 0.25, rapidUp: 0.25, topDead: 0.2, bottomDead: 0.2 } },
  { id: "speed", patch: { mode: MODE.RT_CONTINUOUS, actuation: 0.5, rapidDown: 0.2, rapidUp: 0.2, topDead: 0, bottomDead: 0.2 } },
];
const FIELDS = ["actuation", "release", "rapidDown", "rapidUp", "topDead", "bottomDead"];

/** Same value for every key? -> that value, otherwise null (shown as "mixed"). */
function unify(slots, getter) {
  const first = getter(slots[0]);
  return slots.every((sl) => getter(sl) === first) ? first : null;
}

export function mount(root, app) {
  const card = keyboardCard(app, { mode: "depth" });
  let linked = true;
  const trig = (slot) => app.dataOf(slot).trigger;
  const mm = (v) => `${v.toFixed(2)} mm`;

  const fields = {};
  /** Show why a value moved (or was refused) under its field; fades after a few seconds. */
  const flagTimers = new Map();
  const explain = (field, code, value, lim) => {
    const f = fields[field];
    if (!f) return;
    f.flag(t(`trig.reason.${code}`, { v: mm(value), min: mm(lim?.min ?? NOISE_FLOOR_MM), max: mm(lim?.max ?? MAX_TRAVEL_MM), floor: mm(NOISE_FLOOR_MM), gap: mm(HYSTERESIS_MIN_MM) }));
    clearTimeout(flagTimers.get(field));
    flagTimers.set(field, setTimeout(() => f.flag(""), 6000));
  };
  const apply = (patch) => {
    const ids = app.selected.map((sl) => app.idOf(sl));
    if (!ids.length) return;
    const notes = app.edit((p) => applyTriggerPatch(p, ids, patch));
    const seen = new Set();
    for (const n of notes) {
      if (seen.has(n.field)) continue;
      seen.add(n.field);
      explain(n.field, n.code, n.value, n.field === "release" ? LIMITS.release : n.field.startsWith("rapid") ? LIMITS.rt : n.field.endsWith("Dead") ? LIMITS.dead : LIMITS.actuation);
    }
  };
  const clampHandler = (field, lim) => (asked, v, side) => explain(field, side === "below" ? "below_noise" : "above_travel", v, lim);

  const mode = segmented({
    label: t("perf.mode"), accent: true, block: true, value: null,
    options: [
      { id: MODE.NORMAL, label: t("perf.mode.normal"), title: t("perf.mode.normal.tip") },
      { id: MODE.RT, label: t("perf.mode.rt"), title: t("perf.mode.rt.tip") },
      { id: MODE.RT_CONTINUOUS, label: t("perf.mode.rtc"), title: t("perf.mode.rtc.tip") },
    ],
    onChange: (m) => apply({ mode: m }),
  });
  fields.actuation = rangeField({ id: "act", label: t("perf.act"), hint: t("perf.act.hint"), ...LIMITS.actuation, onInput: (v) => apply({ actuation: v }), onClamp: clampHandler("actuation", LIMITS.actuation) });
  fields.release = rangeField({ id: "rel", label: t("perf.release"), hint: t("perf.release.hint"), ...LIMITS.release, onInput: (v) => apply({ release: v }), onClamp: clampHandler("release", LIMITS.release) });
  fields.rapidDown = rangeField({ id: "rtp", label: t("perf.rapidDown"), hint: t("perf.rapidDown.hint"), ...LIMITS.rt, onInput: (v) => apply(linked ? { rapidDown: v, rapidUp: v } : { rapidDown: v }), onClamp: clampHandler("rapidDown", LIMITS.rt) });
  fields.rapidUp = rangeField({ id: "rtr", label: t("perf.rapidUp"), hint: t("perf.rapidUp.hint"), ...LIMITS.rt, onInput: (v) => apply(linked ? { rapidDown: v, rapidUp: v } : { rapidUp: v }), onClamp: clampHandler("rapidUp", LIMITS.rt) });
  fields.topDead = rangeField({ id: "top", label: t("perf.top"), hint: t("perf.top.hint"), ...LIMITS.dead, onInput: (v) => apply({ topDead: v }), onClamp: clampHandler("topDead", LIMITS.dead) });
  fields.bottomDead = rangeField({ id: "bot", label: t("perf.bottom"), hint: t("perf.bottom.hint"), ...LIMITS.dead, onInput: (v) => apply({ bottomDead: v }), onClamp: clampHandler("bottomDead", LIMITS.dead) });
  const link = toggle({ label: t("perf.link"), checked: true, onChange: (v) => { linked = v; if (v) apply({ rapidUp: trig(app.primary).rapidDown }); } });
  const modeNote = h("p.field-hint");
  const rules = h("p.field-hint.note", {}, icon("info"), t("perf.rules", { floor: mm(NOISE_FLOOR_MM), gap: mm(HYSTERESIS_MIN_MM) }));

  const presets = h("div.presets", { role: "group", "aria-label": t("perf.presets") },
    PRESETS.map((p) => h("button.chip", { type: "button", on: { click: () => apply(p.patch) } }, t(`perf.preset.${p.id}`))));
  const applyAll = h("button.btn.sm", { type: "button", title: t("perf.applyAll.tip"), on: { click: copyToAll } }, icon("copy"), t("perf.applyAll"));
  function copyToAll() {
    if (app.primary === null) return;
    const src = { ...trig(app.primary) };
    const ids = app.layout.keys.map((k) => k.id);
    app.edit((p) => applyTriggerPatch(p, ids, src));
    toast(t("perf.applyAll.done", { n: ids.length }));
  }

  // ---- travel diagram (vertical): 0 mm at the top (key released), 3.4 mm at the bottom (fully pressed)
  const TH = 280, TW = 64, TOP = 14, LX = 84; // track height / width / top margin / label column
  const toY = (v) => TOP + (v / MAX_TRAVEL_MM) * TH;
  const mk = (cls, attrs = {}) => s("rect", { class: cls, x: 0, width: TW, ...attrs });
  const noise = mk("noise", { rx: 3 });
  const dz1 = mk("dz", { rx: 3 });
  const dz2 = mk("dz", { rx: 3 });
  const rtBand = mk("rtband");
  const depthBar = mk("dbar", { rx: 3 });
  const line = (cls) => s("line", { class: cls, x1: -8, x2: TW + 8 });
  const actLine = line("aline"), relLine = line("rline"), floorLine = line("fline");
  const lbl = (cls) => s("text", { class: `dlbl ${cls}`, x: cls === "a" || cls === "r" ? TW + 14 : TW / 2, "text-anchor": cls === "a" || cls === "r" ? "start" : "middle", "dominant-baseline": "central" });
  const actText = lbl("a"), relText = lbl("r"), floorText = lbl("f"), topText = lbl("z"), botText = lbl("z");
  const ticks = [];
  for (let v = 0; v <= MAX_TRAVEL_MM + 1e-9; v += 0.5) {
    ticks.push(s("line", { class: "tick", x1: -4, x2: 0, y1: toY(v), y2: toY(v) }), s("text", { class: "ticklbl", x: -8, y: toY(v), "text-anchor": "end", "dominant-baseline": "central" }, v.toFixed(1)));
  }
  const endTick = s("g", {}, s("line", { class: "tick", x1: -4, x2: 0, y1: toY(MAX_TRAVEL_MM), y2: toY(MAX_TRAVEL_MM) }), s("text", { class: "ticklbl", x: -8, y: toY(MAX_TRAVEL_MM), "text-anchor": "end", "dominant-baseline": "central" }, "3.4"));
  const diagram = s("svg", { class: "travel", viewBox: `-30 0 ${TW + LX + 110} ${TH + TOP * 2}`, role: "img", "aria-label": t("perf.diagram") },
    s("rect", { class: "track", x: 0, y: TOP, width: TW, height: TH, rx: 4 }),
    noise, depthBar, dz1, dz2, rtBand, floorLine, relLine, actLine, ...ticks, endTick, actText, relText, floorText, topText, botText);
  const legend = h("ul.dlegend", {},
    h("li", {}, h("i.sw.sw-act"), t("perf.lg.act")), h("li", {}, h("i.sw.sw-rel"), t("perf.lg.rel")),
    h("li", {}, h("i.sw.sw-dz"), t("perf.lg.dz")), h("li", {}, h("i.sw.sw-noise"), t("perf.lg.noise", { floor: mm(NOISE_FLOOR_MM) })));

  const fieldsEl = h("div.fields", {}, modeNote, fields.actuation.el, fields.release.el, fields.rapidDown.el, fields.rapidUp.el, link.el, fields.topDead.el, fields.bottomDead.el, rules);
  const empty = h("div.empty", {}, icon("performance"), h("h3", {}, t("perf.empty.title")), h("p", {}, t("perf.empty.body")));
  const title = h("h2.card-title");
  const panel = h("section.card.perf-panel", {},
    h("div.card-head", {}, title, h("div.row", {}, presets, applyAll)),
    h("div.perf-grid", {}, h("div.perf-main", {}, mode.el, fieldsEl), h("div.perf-diagram", {}, diagram, legend)),
    empty);

  function sync() {
    const slots = app.selected;
    const has = slots.length > 0;
    panel.classList.toggle("has-sel", has);
    empty.hidden = has;
    applyAll.hidden = !has;
    panel.querySelector(".perf-grid").hidden = !has;
    title.textContent = has ? t("perf.title", { n: slots.length }) : t("perf.title0");
    if (!has) return;
    const m = unify(slots, (sl) => trig(sl).mode);
    mode.set(m);
    for (const f of FIELDS) fields[f].set(unify(slots, (sl) => trig(sl)[f]));
    // what the keyboard's engine actually uses per mode (ENGINE_NOTES.md): Normal = actuation + release; Rapid Trigger = actuation as the
    // lowest re-trigger depth, press / release steps, dead zones; Continuous = the same without the actuation point. Mixed shows everything.
    const normal = m === MODE.NORMAL, cont = m === MODE.RT_CONTINUOUS;
    fields.actuation.show(!cont);
    fields.release.show(normal || m === null);
    fields.rapidDown.show(!normal); fields.rapidUp.show(!normal); link.el.hidden = normal;
    fields.topDead.show(!normal); fields.bottomDead.show(!normal);
    modeNote.textContent = m === null ? t("perf.modeNote.mixed") : t(`perf.modeNote.${m}`);
    paintDiagram();
  }

  function paintDiagram() {
    const slot = app.primary;
    if (slot === null) return;
    const tr = trig(slot);
    const rt = tr.mode !== MODE.NORMAL, cont = tr.mode === MODE.RT_CONTINUOUS;
    noise.setAttribute("y", toY(0)); noise.setAttribute("height", toY(NOISE_FLOOR_MM) - toY(0));
    floorLine.setAttribute("y1", toY(NOISE_FLOOR_MM)); floorLine.setAttribute("y2", toY(NOISE_FLOOR_MM));
    floorText.setAttribute("y", (toY(0) + toY(NOISE_FLOOR_MM)) / 2); floorText.textContent = t("perf.lbl.noise", { v: NOISE_FLOOR_MM.toFixed(2) });
    // dead zones belong to Rapid Trigger only (the engine's Normal mode ignores them)
    const top = rt ? tr.topDead : 0, bottom = rt ? tr.bottomDead : 0;
    dz1.setAttribute("y", toY(0)); dz1.setAttribute("height", Math.max(0, toY(top) - toY(0)));
    dz2.setAttribute("y", toY(MAX_TRAVEL_MM - bottom)); dz2.setAttribute("height", Math.max(0, toY(MAX_TRAVEL_MM) - toY(MAX_TRAVEL_MM - bottom)));
    // labels sit inside their band, and only when the band is tall enough to hold a line of text
    topText.textContent = top >= 0.3 ? t("perf.lbl.top", { v: top.toFixed(2) }) : "";
    topText.setAttribute("y", toY(top / 2));
    botText.textContent = bottom >= 0.3 ? t("perf.lbl.bottom", { v: bottom.toFixed(2) }) : "";
    botText.setAttribute("y", toY(MAX_TRAVEL_MM - bottom / 2));
    actLine.style.display = actText.style.display = cont ? "none" : "";
    actLine.setAttribute("y1", toY(tr.actuation)); actLine.setAttribute("y2", toY(tr.actuation));
    actText.setAttribute("y", toY(tr.actuation) - 1);
    actText.textContent = t("perf.lbl.act", { v: tr.actuation.toFixed(2) });
    // where the release happens: Normal at the release point; Rapid Trigger a step above the deepest point (drawn from the actuation point)
    const rel = !rt ? tr.release : Math.max(0, (cont ? 1.0 : tr.actuation) - tr.rapidUp);
    relLine.setAttribute("y1", toY(rel)); relLine.setAttribute("y2", toY(rel));
    relText.setAttribute("y", toY(rel) + 1);
    relText.textContent = rt ? t("perf.lbl.rtUp", { v: tr.rapidUp.toFixed(2) }) : t("perf.lbl.rel", { v: tr.release.toFixed(2) });
    relLine.style.display = relText.style.display = cont ? "none" : "";
    // the stretch where Rapid Trigger works: from the actuation point (Continuous: from the top zone) down to the bottom dead zone
    const from = cont ? Math.max(NOISE_FLOOR_MM, tr.topDead) : tr.actuation;
    rtBand.setAttribute("y", toY(from));
    rtBand.setAttribute("height", rt ? Math.max(0, toY(MAX_TRAVEL_MM - bottom) - toY(from)) : 0);
  }

  let raf = 0;
  const loop = () => {
    const slot = app.primary;
    if (slot !== null) {
      const d = rawToMm(app.feed.depth[slot]);
      depthBar.setAttribute("y", toY(0));
      depthBar.setAttribute("height", Math.max(0, toY(Math.min(d, MAX_TRAVEL_MM)) - toY(0)));
      diagram.classList.toggle("pressed", app.feed.pressed[slot] === 1);
    }
    raf = requestAnimationFrame(loop);
  };
  loop();

  const offs = [app.on("selection", sync), app.on("profile", sync)];
  sync();
  root.append(h("div.page-grid.perf", {}, card.el, panel));
  return {
    destroy() { cancelAnimationFrame(raf); flagTimers.forEach(clearTimeout); offs.forEach((o) => o()); card.destroy(); },
  };
}
