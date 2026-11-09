// Behaviours page (M6 P9a): Dynamic Keystroke (DKS), Mod-Morph, toggle keys and SOCD pairs of the profile being edited.
// Like the Layers page it only edits the in-memory workspace (app.edit); the keyboard changes when the person presses Save.
// All rules (what a record may hold, how deleting renumbers the keys) live in core/behaviors.js and the settings codec; this file
// is the screen. Strings come from locales/{en,zh-TW}-m6.js through tb() (falls back to t(), so merging them into the main
// dictionaries later changes nothing).
import { h, icon, injectCss, toast } from "../dom.js";
import { getLang, t, tOr } from "../i18n.js";
import { keyboardCard } from "../ui/kb-card.js";
import { confirmDialog } from "../ui/dialog.js";
import { segmented, toggle } from "../ui/controls.js";
import { ALL_USAGES, GROUP_ORDER, findUsage, keyName } from "../core/hid-usages.js";
import { LAYER_COUNT } from "../core/profile.js";
import { MODE } from "../core/trigger.js";
import { FW_M6 } from "../core/limits.js";
import * as B from "../core/behaviors.js";
import en6 from "../locales/en-m6.js";
import zh6 from "../locales/zh-TW-m6.js";

const DICTS6 = { "zh-TW": zh6, en: en6 };
/** Text of this page in the current language (the page's own dictionaries first, then the app's). */
export function tb(key, params) {
  let str = DICTS6[getLang()]?.[key] ?? en6[key] ?? t(key);
  if (params) for (const [k, v] of Object.entries(params)) str = str.replaceAll(`{${k}}`, v);
  return str;
}

const TABS = ["dks", "morph", "toggle", "socd"];
const mm = (v) => (v / 100).toFixed(2);

// the person confirmed the SOCD notice once in this browser tab; asked again after a reload
let socdAcknowledged = false;

// colours of the keys on the canvas, per theme: [dark, light]
const KIND_COLORS = {
  dks: [[77, 227, 192], [11, 159, 132]],
  morph: [[244, 182, 74], [183, 121, 31]],
  toggle: [[150, 140, 255], [98, 84, 214]],
  socd: [[255, 134, 120], [207, 70, 56]],
  plain: [[27, 32, 40], [247, 249, 251]],
};
const rgbCss = (c) => `rgb(${c[0]} ${c[1]} ${c[2]})`;

const STYLE = `
.beh-cur { display: grid; gap: var(--s1); padding: var(--s3) var(--s4); background: var(--surface-2); border: 1px solid var(--line); border-radius: var(--r-md); font-size: 0.88rem; }
.beh-cur strong { color: var(--text); font-weight: 600; }
.beh-cur .what { color: var(--text-dim); }
.beh-legend { display: flex; flex-wrap: wrap; gap: var(--s2) var(--s4); list-style: none; margin: var(--s3) 0 0; padding: 0; font-size: 0.78rem; color: var(--text-dim); }
.beh-legend li { display: inline-flex; align-items: center; gap: 0.4rem; }
.beh-legend i { width: 0.8rem; height: 0.8rem; border-radius: 3px; border: 1px solid var(--line-strong); flex: none; }
.beh-body { display: grid; gap: var(--s4); }
.beh-recs { display: flex; flex-wrap: wrap; gap: var(--s2); align-items: center; }
.beh-recs .seg { flex: 1 1 auto; flex-wrap: wrap; }
.beh-recs .seg button small { margin-left: 0.4rem; color: var(--text-faint); font-weight: 400; }
.beh-recs .seg button[aria-checked="true"] small { color: var(--text-dim); }
.beh-edit { display: grid; gap: var(--s4); padding: var(--s4); background: var(--surface-2); border: 1px solid var(--line); border-radius: var(--r-md); }
.beh-edit h3 { font-size: 0.98rem; }
.beh-edit h4 { font-size: 0.84rem; font-weight: 600; color: var(--text); }
.beh-actions { display: flex; flex-wrap: wrap; gap: var(--s2); align-items: center; }
.beh-chips { display: flex; flex-wrap: wrap; gap: var(--s2); }
.beh-bar { padding: 1.9rem 0.7rem 1.6rem; }
.beh-track { position: relative; height: 0.5rem; border-radius: 999px; background: var(--surface-3); }
.beh-zone { position: absolute; top: 0; bottom: 0; background: var(--accent-soft); border-left: 2px solid var(--accent); border-right: 2px solid var(--warn); }
.beh-tick { position: absolute; top: 0.7rem; width: 1px; height: 0.4rem; background: var(--line-strong); }
.beh-tick span { position: absolute; top: 0.45rem; left: 0; transform: translateX(-50%); font: 500 0.68rem var(--mono); color: var(--text-faint); }
.beh-handle { position: absolute; top: 50%; width: 1.5rem; height: 1.5rem; margin: -0.75rem 0 0 -0.75rem; border-radius: 50%; border: 3px solid var(--bg); padding: 0; cursor: grab; touch-action: none; box-shadow: 0 0 0 1px var(--line-strong); }
.beh-handle:active { cursor: grabbing; }
.beh-handle.act { background: var(--accent); }
.beh-handle.bot { background: var(--warn); }
.beh-handle:focus-visible { outline: 2px solid var(--text); outline-offset: 2px; }
.beh-handle .tag { position: absolute; bottom: 1.45rem; left: 50%; transform: translateX(-50%); white-space: nowrap; font: 600 0.7rem var(--mono); color: var(--text-dim); pointer-events: none; }
.beh-depths { display: flex; flex-wrap: wrap; gap: var(--s4); }
.beh-depths .num-input { max-width: 8rem; }
.beh-table-wrap { overflow-x: auto; }
.beh-table { border-collapse: collapse; width: 100%; font-size: 0.82rem; min-width: 34rem; }
.beh-table th { text-align: left; font-weight: 600; color: var(--text-dim); padding: 0 var(--s2) var(--s2) 0; vertical-align: bottom; }
.beh-table th small { display: block; font: 500 0.7rem var(--mono); color: var(--text-faint); }
.beh-table td { padding: 0 var(--s2) var(--s2) 0; }
.beh-table select { min-height: 2rem; font-size: 0.82rem; width: 100%; }
.beh-table .rowhead { color: var(--text-dim); white-space: nowrap; font-weight: 500; }
.beh-sum { margin: 0; padding-left: 1.1rem; display: grid; gap: var(--s1); color: var(--text-dim); font-size: 0.84rem; }
.beh-sum .closing { color: var(--text-faint); }
.beh-notes { display: grid; gap: var(--s2); }
.beh-mods { display: grid; grid-template-columns: auto repeat(4, minmax(0, 1fr)); gap: var(--s2) var(--s3); align-items: center; font-size: 0.86rem; }
.beh-mods .rowhead { color: var(--text-dim); }
.beh-check { display: inline-flex; align-items: center; gap: 0.45rem; cursor: pointer; min-height: 1.9rem; }
.beh-check input { width: 1.05rem; height: 1.05rem; accent-color: var(--accent); }
.beh-pairs { display: grid; gap: var(--s3); }
.beh-pair { display: grid; gap: var(--s3); padding: var(--s3) var(--s4); background: var(--surface-2); border: 1px solid var(--line); border-radius: var(--r-md); }
.beh-pair.on { border-color: color-mix(in srgb, var(--warn) 55%, transparent); }
.beh-pair-keys { display: grid; grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr)); gap: var(--s3); }
.beh-pair select { min-height: 2rem; font-size: 0.85rem; }
.beh-pair .seg { flex-wrap: wrap; }
.beh-socd-warn { border: 1px solid color-mix(in srgb, var(--warn) 55%, transparent); }
.beh-socd-warn p { margin-top: var(--s1); }
.beh-list { list-style: none; margin: 0; padding: 0; display: grid; gap: var(--s2); font-size: 0.86rem; }
.beh-list li { display: flex; align-items: center; justify-content: space-between; gap: var(--s3); padding: 0.3rem 0.6rem; background: var(--surface-2); border: 1px solid var(--line); border-radius: var(--r-sm); }
.beh-issues ul { margin: var(--s2) 0 0; padding-left: 1.1rem; display: grid; gap: var(--s1); }
details.beh-modes { font-size: 0.84rem; color: var(--text-dim); }
details.beh-modes summary { cursor: pointer; color: var(--text); font-weight: 500; }
details.beh-modes dl { margin: var(--s2) 0 0; display: grid; gap: var(--s2); }
details.beh-modes dt { color: var(--text); font-weight: 600; }
details.beh-modes dd { margin: 0; }
@media (max-width: 719px) { .beh-mods { grid-template-columns: auto repeat(2, minmax(0, 1fr)); } .beh-edit { padding: var(--s3); } }
`;

function ensureStyle() {
  injectCss("beh-style", STYLE);
}

const clampIndex = (i, n) => Math.min(Math.max(i, 0), Math.max(n - 1, 0));

/** The name of a keyboard usage in the current language ("A", "Left Shift" …). */
const usageName = (usage) => {
  const u = findUsage({ type: "key", usage });
  return u ? tOr(`u.${u.name}`, u.name) : keyName(usage);
};

export function mount(root, app) {
  ensureStyle();
  let tab = "dks";
  const pick = { dks: 0, morph: 0 };
  let dragging = false;
  let depthParts = null;   // the live pieces of the depth bar, repainted while a dot is dragged

  const layerName = (i) => app.profile.layerNames?.[i] || t(`remap.layer${i}`);
  const targets = () => app.selected.map((slot) => ({ keyId: app.idOf(slot), layer: app.layer }));
  const keyLabel = (keyId) => app.layout.byId.get(keyId)?.name ?? keyId;
  const list = (kind) => B.recordsOf(app.profile, kind);

  /** Mark a control so focus can come back to it after the panel is rebuilt. */
  const fid = (el, id) => { el.dataset.fid = id; return el; };

  function failText(reason) {
    if (["full", "overlap", "same"].includes(reason)) return tb(`beh.fail.${reason}`);
    return tb("beh.fail.other", { why: reason });
  }
  const fail = (reason) => toast(failText(reason), "warn");

  // ------------------------------------------------------------------------------------------------ keyboard canvas
  const keyColors = new Map();
  const legendItems = ["dks", "morph", "toggle", "socd", "plain"];
  const swatch = (kind) => h("i", { style: { background: rgbCss(KIND_COLORS[kind][app.theme === "light" ? 1 : 0]) } });
  const legend = h("ul.beh-legend", { "aria-label": tb("beh.legend.aria") });
  const paintLegend = () => legend.replaceChildren(...legendItems.map((k) => h("li", {}, swatch(k), tb(`beh.legend.${k}`))));
  const card = keyboardCard(app, { mode: "lighting", legend, colorFn: (key) => keyColors.get(key.id) ?? KIND_COLORS.plain[0] });

  function paintKeys() {
    const dark = app.theme !== "light";
    const pick2 = (kind) => KIND_COLORS[kind][dark ? 0 : 1];
    const socdKeys = new Set();
    for (const pair of list("socd")) {
      for (const slot of [pair.a, pair.b]) { const k = app.layout.bySlot.get(slot); if (k) socdKeys.add(k.id); }
    }
    keyColors.clear();
    for (const k of app.layout.keys) {
      const kind = B.behaviorOf(app.profile.keys[k.id]?.bindings[app.layer])?.kind ?? (socdKeys.has(k.id) ? "socd" : "plain");
      keyColors.set(k.id, pick2(kind));
    }
    paintLegend();
    card.view.refresh();
  }

  // ------------------------------------------------------------------------------------------------ pieces shared by the sections
  const layerSeg = segmented({
    label: tb("beh.layer"), block: true,
    options: Array.from({ length: LAYER_COUNT }, (_, i) => ({ id: String(i), label: layerName(i) })),
    value: String(app.layer),
    onChange: (id) => { app.layer = Number(id); render(); },
  });
  const tabSeg = segmented({
    label: tb("beh.tabs"), block: true, accent: true,
    options: TABS.map((id) => ({ id, label: tb(`beh.tab.${id}`) })),
    value: tab,
    onChange: (id) => { tab = id; render(); },
  });

  const forProfile = h("p.field-hint.note", {}, icon("info"), h("span"));
  const cur = h("div.beh-cur", { "aria-live": "polite" });
  const issuesBox = h("div.banner.error.beh-issues", { hidden: true, role: "alert" }, icon("warn"), h("div"));
  const content = h("div.beh-body");
  const layerRow = h("div.field", {}, h("span.field-label", {}, tb("beh.layer")), layerSeg.el, h("span.field-hint", {}, tb("beh.layerNote")));
  const panel = h("section.card.beh-panel", {}, h("div.card-head", {}, h("h2.card-title", {}, tb("nav.behaviors"))),
    h("div.beh-body", {}, forProfile, tabSeg.el, layerRow, cur, issuesBox, content));

  /** Select box of keyboard usages (optgroups as on the Layers page). value 0 = "not used" when allowNone. */
  function usageSelect({ value, onChange, label, id, allowNone = false, extra = [] }) {
    const sel = h("select", { "aria-label": label, on: { change: () => onChange(sel.value === "self" ? "self" : Number(sel.value)) } },
      extra.map((e) => h("option", { value: e.value }, e.label)),
      allowNone ? h("option", { value: "0" }, tb("beh.dks.nokey")) : null,
      GROUP_ORDER.map((g) => {
        const part = ALL_USAGES.filter((u) => u.group === g && u.binding?.type === "key" && !u.binding.mods);
        return part.length ? h("optgroup", { label: t(`group.${g}`) }, part.map((u) => h("option", { value: String(u.binding.usage) }, tOr(`u.${u.name}`, u.name)))) : null;
      }));
    sel.value = String(value);
    return fid(sel, id);
  }

  function recordBar(kind, recs, usage, makeNew) {
    const full = recs.length >= B.M6_LIMITS[kind];
    const seg = segmented({
      label: tb("beh.rec.list"),
      options: recs.map((_, i) => ({ id: String(i), label: h("span", {}, tb(`beh.${kind}.name`, { n: i + 1 }), h("small", {}, usage[i] ? tb("beh.rec.keys", { n: usage[i] }) : tb("beh.rec.unused"))) })),
      value: String(pick[kind]),
      onChange: (id) => { pick[kind] = Number(id); render(); },
    });
    const add = fid(h("button.btn.sm.primary", { type: "button", disabled: full, title: full ? tb("beh.rec.full", { max: B.M6_LIMITS[kind] }) : undefined, on: { click: makeNew } }, icon("plus"), tb("beh.rec.new")), `${kind}-new`);
    return h("div.beh-recs", {}, recs.length ? seg.el : null, add);
  }

  function usedBy(kind, index) {
    const users = B.usersOf(app.profile, kind, index);
    return h("div.field", {}, h("span.field-label", {}, tb("beh.usedBy")),
      users.length ? h("div.beh-chips", {}, users.map((u) => h("span.chip", {}, tb("beh.usedBy.item", { key: keyLabel(u.keyId), layer: layerName(u.layer) })))) : h("span.field-hint", {}, tb("beh.usedBy.none")));
  }

  function assignRow(kind, index, name) {
    const n = app.selection.size;
    const assign = fid(h("button.btn.sm.primary", { type: "button", disabled: !n, on: { click: () => assign_(kind, index, name) } }, tb("beh.assign.sel", { n })), `${kind}-assign`);
    const clear = fid(h("button.btn.sm", { type: "button", disabled: !n, on: { click: clearSelected } }, tb("beh.assign.clear")), `${kind}-clear`);
    const del = fid(h("button.btn.sm.ghost.danger", { type: "button", on: { click: () => remove(kind, index, name) } }, icon("trash"), tb("beh.rec.delete")), `${kind}-delete`);
    return h("div.beh-actions", {}, assign, clear, del, n ? null : h("span.field-hint", {}, tb("beh.assign.none")));
  }

  function skippedText(skipped) {
    return skipped.map((s) => keyLabel(s.keyId)).join(", ");
  }

  function assign_(kind, index, name) {
    const keys = targets();
    const r = app.edit((p) => B.assignRecord(p, app.layout, kind, index, keys));
    if (r.done.length) toast(tb("beh.assign.done", { n: r.done.length, name, layer: layerName(app.layer) }));
    if (r.skipped.length) toast(tb("beh.assign.skipped", { n: r.skipped.length, keys: skippedText(r.skipped) }), "warn");
  }

  function clearSelected() {
    const keys = targets();
    const done = app.edit((p) => B.clearBehavior(p, app.layout, keys));
    toast(done.length ? tb("beh.assign.cleared", { n: done.length }) : tb("beh.assign.nothing"), done.length ? "info" : "warn");
  }

  async function remove(kind, index, name) {
    const users = B.usersOf(app.profile, kind, index);
    if (users.length) {
      const ok = await confirmDialog({
        title: tb("beh.rec.delete.title", { name }), body: tb("beh.rec.delete.body"),
        list: users.map((u) => tb("beh.usedBy.item", { key: keyLabel(u.keyId), layer: layerName(u.layer) })),
        confirmLabel: tb("beh.rec.delete.ok"), danger: true,
      });
      if (!ok) return;
    }
    const r = app.edit((p) => B.removeRecord(p, app.layout, kind, index));
    if (r.ok) toast(tb("beh.rec.deleted", { name }));
  }

  function setRecord(kind, index, rec) {
    const r = app.edit((p) => B.setRecord(p, kind, index, rec));
    if (!r.ok) { fail(r.reason); render(); }
    return r.ok;
  }

  // ------------------------------------------------------------------------------------------------ DKS
  const opLabels = [0, 1, 2, 3].map((v) => tb(`beh.dks.op.${v}`));

  function dksSection() {
    const recs = list("dks");
    const usage = B.recordUsage(app.profile, "dks");
    pick.dks = clampIndex(pick.dks, recs.length);
    const nodes = [h("p.field-hint", {}, tb("beh.dks.hint")), recordBar("dks", recs, usage, newDks)];
    if (!recs.length) {
      nodes.push(h("div.empty.small", {}, icon("remap"), h("h3", {}, tb("beh.dks.empty.title")), h("p", {}, tb("beh.dks.empty.body"))));
      return nodes;
    }
    nodes.push(dksEditor(recs[pick.dks], pick.dks));
    return nodes;
  }

  function newDks() {
    const r = app.edit((p) => B.addRecord(p, "dks", B.newDksRecord()));
    if (r.ok) pick.dks = r.index; else fail(r.reason);
    render();
  }

  function applyDepth(index, field, value) {
    const rec = list("dks")[index];
    if (!rec) return;
    const r = B.dksSetDepth(rec, field, value);
    if (r.clamped && !dragging) toast(tb("beh.dks.clamped", { mm: mm(r.rec[field]) }), "warn");
    if (r.rec.tAct === rec.tAct && B.dksBottom(r.rec) === B.dksBottom(rec)) { if (!dragging) render(); return; }
    setRecord("dks", index, r.rec);
  }

  function depthBar(rec, index) {
    const bar = h("div.beh-bar", { role: "group", "aria-label": tb("beh.dks.bar") });
    const track = h("div.beh-track");
    const zone = h("div.beh-zone");
    track.append(zone);
    for (let m = 0; m <= 3; m++) track.append(h("div.beh-tick", { style: { left: `${(m * 100 / 340) * 100}%` } }, h("span", {}, `${m}`)));
    const handle = (field) => {
      const tag = h("span.tag");
      const el = fid(h("button.beh-handle", { type: "button", class: field === "tAct" ? "act" : "bot", role: "slider", "aria-label": tb(field === "tAct" ? "beh.dks.act" : "beh.dks.bot"), "aria-valuemin": "0", "aria-valuemax": "340" }, tag), `dks-handle-${field}`);
      const valueFromPointer = (e) => {
        const r = track.getBoundingClientRect();
        const raw = ((e.clientX - r.left) / r.width) * 340;
        return Math.round(raw / B.DKS_STEP_MM100) * B.DKS_STEP_MM100;
      };
      el.addEventListener("pointerdown", (e) => { e.preventDefault(); dragging = true; el.setPointerCapture(e.pointerId); el.focus(); });
      el.addEventListener("pointermove", (e) => { if (dragging && el.hasPointerCapture(e.pointerId)) applyDepth(index, field, valueFromPointer(e)); });
      const stop = (e) => { if (!dragging) return; dragging = false; try { el.releasePointerCapture(e.pointerId); } catch { /* already released */ } render(); };
      el.addEventListener("pointerup", stop);
      el.addEventListener("pointercancel", stop);
      el.addEventListener("keydown", (e) => {
        const cur2 = list("dks")[index];
        const now = field === "tAct" ? cur2.tAct : B.dksBottom(cur2);
        const step = e.shiftKey ? 20 : B.DKS_STEP_MM100;
        const to = { ArrowLeft: now - step, ArrowDown: now - step, ArrowRight: now + step, ArrowUp: now + step, PageDown: now - 50, PageUp: now + 50, Home: 0, End: 340 }[e.key];
        if (to === undefined) return;
        e.preventDefault();
        applyDepth(index, field, to);
      });
      return { el, tag };
    };
    const act = handle("tAct");
    const bot = handle("tBot");
    track.append(act.el, bot.el);
    bar.append(track);
    const paint = (r) => {
      const b = B.dksBottom(r);
      const pos = (v) => `${(v / 340) * 100}%`;
      zone.style.left = pos(r.tAct);
      zone.style.width = `${((b - r.tAct) / 340) * 100}%`;
      act.el.style.left = pos(r.tAct);
      bot.el.style.left = pos(b);
      act.el.setAttribute("aria-valuenow", r.tAct); act.el.setAttribute("aria-valuetext", `${mm(r.tAct)} mm`);
      bot.el.setAttribute("aria-valuenow", b); bot.el.setAttribute("aria-valuetext", `${mm(b)} mm`);
      act.el.setAttribute("aria-valuemax", String(b - FW_M6.dksGapMin)); bot.el.setAttribute("aria-valuemin", String(r.tAct + FW_M6.dksGapMin));
      act.tag.textContent = `${mm(r.tAct)}`;
      bot.tag.textContent = `${mm(b)}`;
    };
    paint(rec);
    depthParts = { paint, index };
    return bar;
  }

  function depthInputs(rec, index) {
    const field = (name, key, value, id) => {
      const input = fid(h("input", { type: "number", min: "0.40", max: "3.40", step: "0.05", inputmode: "decimal", "aria-label": `${tb(name)} (mm)`,
        on: { change: () => { if (input.value !== "") applyDepth(index, key, Math.round(Number(input.value) * 100)); else render(); } } }), id);
      input.value = mm(value);
      return h("label.field", {}, h("span.field-label", {}, tb(name)), h("div.num-input", {}, input, h("span", {}, "mm")));
    };
    return h("div.beh-depths", {}, field("beh.dks.act", "tAct", rec.tAct, "dks-num-tAct"), field("beh.dks.bot", "tBot", B.dksBottom(rec), "dks-num-tBot"));
  }

  function dksTable(rec, index) {
    const depth = [rec.tAct, B.dksBottom(rec), B.dksBottom(rec) - B.DKS_HYSTERESIS_MM100, rec.tAct - 10];
    const head = h("tr", {}, h("th", { scope: "col" }, tb("beh.dks.key")),
      [0, 1, 2, 3].map((e) => h("th", { scope: "col", title: tb(`beh.dks.ev.${e}.tip`) }, tb(`beh.dks.ev.${e}`), h("small", {}, `${mm(depth[e])} mm`))));
    const rows = rec.acts.map(([usageCode, bits], i) => {
      const events = B.dksEventsUnpack(bits);
      const keyCell = h("td", {}, usageSelect({
        value: usageCode, allowNone: true, label: `${tb("beh.dks.actionN", { n: i + 1 })}: ${tb("beh.dks.key")}`, id: `dks-key-${i}`,
        onChange: (u) => setRecord("dks", index, B.dksSetUsage(list("dks")[index], i, u)),
      }));
      const cells = events.map((v, e) => {
        const sel = fid(h("select", { disabled: usageCode === 0, "aria-label": `${tb("beh.dks.actionN", { n: i + 1 })}: ${tb(`beh.dks.ev.${e}`)}`,
          on: { change: () => { const next = B.dksSetEvent(list("dks")[index], i, e, Number(sel.value)); if (next) setRecord("dks", index, next); } } },
        opLabels.map((label, op) => h("option", { value: String(op) }, label))), `dks-ev-${i}-${e}`);
        sel.value = String(v);
        return h("td", {}, sel);
      });
      return h("tr", {}, h("th.rowhead", { scope: "row" }, tb("beh.dks.actionN", { n: i + 1 })), keyCell, cells);
    });
    // the first body cell is the row header, the key select the second: keep the header row aligned with the data columns
    head.prepend(h("th", { scope: "col" }, ""));
    return h("div.beh-table-wrap", {}, h("table.beh-table", { "aria-label": tb("beh.dks.table") }, h("thead", {}, head), h("tbody", {}, rows)));
  }

  function dksSummary(rec) {
    const stepText = (s) => tb(`beh.dks.step.${s.op}`, { key: usageName(s.usage) });
    const lines = B.describeDks(rec).map((m) => {
      const steps = m.steps.length ? m.steps.map(stepText).join(", ") : tb("beh.dks.sum.nothing");
      return h("li", {}, tb(`beh.dks.sum.${m.event}`, { mm: mm(m.depth), steps }));
    });
    const closing = B.describeDks(rec).at(-1).closing;
    if (closing.length) lines.push(h("li.closing", {}, tb("beh.dks.sum.closing", { keys: closing.map(usageName).join(", ") })));
    return h("ul.beh-sum", {}, lines);
  }

  function dksEditor(rec, index) {
    const name = tb("beh.dks.name", { n: index + 1 });
    const retrig = toggle({ label: tb("beh.dks.retrig"), checked: (rec.flags & 1) === 1, onChange: (on) => setRecord("dks", index, { ...structuredClone(list("dks")[index]), flags: on ? 1 : 0 }) });
    const notes = B.dksNotes(rec).map((n) => h("div.banner.info", {}, icon("info"), h("div", {}, n.note === "idle" ? tb("beh.dks.note.idle", { n: n.action + 1 }) : tb("beh.dks.note.no_actions"))));
    const rtKeys = app.selected.map((s) => app.dataOf(s)).filter(Boolean);
    const rtNames = app.selected.filter((s) => { const d = app.dataOf(s); return d && (d.trigger.mode !== MODE.NORMAL || d.trigger.topDead > 0 || d.trigger.bottomDead > 0); }).map((s) => keyLabel(app.idOf(s)));
    return h("div.beh-edit", {},
      h("h3", {}, name),
      h("div.field", {}, h("span.field-label", {}, tb("beh.dks.depths")), depthBar(rec, index), depthInputs(rec, index), h("span.field-hint", {}, tb("beh.dks.depths.hint"))),
      h("div.field", {}, h("span.field-label", {}, tb("beh.dks.table")), dksTable(rec, index)),
      h("div.field", {}, retrig.el, h("span.field-hint", {}, tb("beh.dks.retrig.hint"))),
      h("div.field", {}, h("span.field-label", {}, tb("beh.dks.summary")), dksSummary(rec)),
      notes.length ? h("div.beh-notes", {}, notes) : null,
      h("p.field-hint.note", {}, icon("info"), tb("beh.dks.fixed")),
      usedBy("dks", index),
      rtKeys.length && rtNames.length ? h("div.banner.warn", {}, icon("warn"), h("div", {}, tb("beh.assign.rt", { keys: rtNames.join(", ") }))) : null,
      assignRow("dks", index, name));
  }

  // ------------------------------------------------------------------------------------------------ Mod-Morph
  const MOD_ROWS = ["Ctrl", "Shift", "Alt", "Win"];

  function morphSection() {
    const recs = list("morph");
    const usage = B.recordUsage(app.profile, "morph");
    pick.morph = clampIndex(pick.morph, recs.length);
    const nodes = [h("p.field-hint", {}, tb("beh.morph.hint")), recordBar("morph", recs, usage, newMorph)];
    if (!recs.length) nodes.push(h("div.empty.small", {}, icon("remap"), h("h3", {}, tb("beh.morph.empty.title")), h("p", {}, tb("beh.morph.empty.body"))));
    else nodes.push(morphEditor(recs[pick.morph], pick.morph));
    nodes.push(h("div.field", {}, h("span.field-label", {}, tb("beh.morph.examples")),
      h("div.beh-actions", {}, B.MORPH_PRESETS.map((p) => fid(h("button.btn.sm", { type: "button", on: { click: () => addPreset(p) } }, tb(`beh.morph.example.${p.id}`)), `morph-example-${p.id}`))),
      h("span.field-hint", {}, tb("beh.morph.examples.hint"))));
    return nodes;
  }

  function newMorph() {
    const r = app.edit((p) => B.addRecord(p, "morph", B.newMorphRecord()));
    if (r.ok) pick.morph = r.index; else fail(r.reason);
    render();
  }

  function addPreset(preset) {
    const done = app.edit((p) => {
      const r = B.addRecord(p, "morph", preset.rec);
      if (!r.ok) return r;
      const a = B.assignRecord(p, app.layout, "morph", r.index, [{ keyId: preset.keyId, layer: 0 }]);
      return { ...r, assigned: a.done.length };
    });
    if (!done.ok) { fail(done.reason); return; }
    pick.morph = done.index;
    toast(tb("beh.assign.done", { n: done.assigned, name: tb("beh.morph.name", { n: done.index + 1 }), layer: layerName(0) }));
    render();
  }

  function morphEditor(rec, index) {
    const name = tb("beh.morph.name", { n: index + 1 });
    const modGrid = [h("span"), ...MOD_ROWS.map((m) => h("span.rowhead", {}, m))];
    for (const [side, offset] of [["left", 0xe0], ["right", 0xe4]]) {
      modGrid.push(h("span.rowhead", {}, tb(`beh.morph.${side}`)));
      MOD_ROWS.forEach((_, i) => {
        const usageCode = offset + i;
        const box = fid(h("input", { type: "checkbox", checked: B.morphHasMod(rec, usageCode), "aria-label": usageName(usageCode),
          on: { change: () => {
            const now = list("morph")[index];
            const next = B.morphToggleMod(now, usageCode);
            if (next.mask === 0) { toast(tb("beh.morph.lastMod"), "warn"); render(); return; }
            setRecord("morph", index, next);
          } } }), `morph-mod-${usageCode}`);
        modGrid.push(h("label.beh-check", {}, box));
      });
    }
    const out = usageSelect({ value: rec.usage, label: tb("beh.morph.becomes"), id: "morph-usage", onChange: (u) => setRecord("morph", index, { ...structuredClone(list("morph")[index]), usage: u }) });
    const suppress = toggle({ label: tb("beh.morph.suppress"), checked: (rec.flags & 1) === 1, onChange: (on) => setRecord("morph", index, { ...structuredClone(list("morph")[index]), flags: on ? 1 : 0 }) });
    const users = B.usersOf(app.profile, "morph", index);
    const usersBox = h("div.field", {}, h("span.field-label", {}, tb("beh.usedBy")),
      users.length ? h("div.beh-chips", {}, users.map((u) => {
        const x = B.behaviorOf(app.profile.keys[u.keyId].bindings[u.layer]);
        return h("span.chip", { title: tb("beh.morph.normal", { key: keyLabel(u.keyId), normal: usageName(x.usage) }) }, tb("beh.usedBy.item", { key: keyLabel(u.keyId), layer: layerName(u.layer) }));
      })) : h("span.field-hint", {}, tb("beh.usedBy.none")));
    return h("div.beh-edit", {},
      h("h3", {}, name),
      h("div.field", {}, h("span.field-label", {}, tb("beh.morph.when")), h("div.beh-mods", {}, modGrid), h("span.field-hint", {}, tb("beh.morph.mods.hint"))),
      h("div.field", {}, h("span.field-label", {}, tb("beh.morph.becomes")), out),
      h("div.field", {}, suppress.el, h("span.field-hint", {}, tb("beh.morph.suppress.hint"))),
      usersBox,
      assignRow("morph", index, name));
  }

  // ------------------------------------------------------------------------------------------------ toggle keys
  let toggleUsage = "self";

  function toggleSection() {
    const n = app.selection.size;
    const pickSel = usageSelect({
      value: toggleUsage, label: tb("beh.toggle.what"), id: "toggle-usage", extra: [{ value: "self", label: tb("beh.toggle.self") }],
      onChange: (u) => { toggleUsage = u; },
    });
    const assign = fid(h("button.btn.sm.primary", { type: "button", disabled: !n, on: { click: () => {
      const keys = targets();
      const r = app.edit((p) => B.assignToggle(p, app.layout, toggleUsage, keys));
      if (r.done.length) toast(tb("beh.toggle.done", { n: r.done.length, layer: layerName(app.layer) }));
      if (r.skipped.length) toast(tb("beh.toggle.skipped", { n: r.skipped.length, keys: skippedText(r.skipped) }), "warn");
    } } }, tb("beh.toggle.assign", { n })), "toggle-assign");
    const keys = B.toggleKeysOn(app.profile, app.layer);
    const rows = keys.length ? h("ul.beh-list", {}, keys.map((k) => h("li", {}, h("span", {}, tb("beh.toggle.item", { key: keyLabel(k.keyId), name: usageName(k.usage) })),
      h("button.btn.sm.ghost", { type: "button", on: { click: () => app.edit((p) => B.resetEntry(p, app.layout, k.keyId, app.layer)) } }, tb("beh.toggle.remove"))))) : h("p.field-hint", {}, tb("beh.toggle.none"));
    return [
      h("p.field-hint", {}, tb("beh.toggle.hint")),
      h("div.beh-edit", {},
        h("div.field", {}, h("span.field-label", {}, tb("beh.toggle.what")), pickSel),
        h("div.beh-actions", {}, assign, n ? null : h("span.field-hint", {}, tb("beh.assign.none")))),
      h("div.field", {}, h("span.field-label", {}, tb("beh.toggle.list")), rows),
      h("div.banner.info", {}, icon("info"), h("div", {}, h("strong", {}, tb("beh.toggle.safety.title")), h("p", {}, tb("beh.toggle.safety")))),
    ];
  }

  // ------------------------------------------------------------------------------------------------ SOCD
  function socdSection() {
    const pairs = list("socd");
    const max = B.M6_LIMITS.socd;
    const nodes = [
      h("div.banner.warn.beh-socd-warn", { role: "note" }, icon("warn"), h("div", {}, h("strong", {}, tb("beh.socd.warn.title")), h("p", {}, tb("beh.socd.warn.body")))),
      h("p.field-hint", {}, tb("beh.socd.hint")),
      h("p.field-hint", {}, tb("beh.socd.count", { n: pairs.length, max, on: B.socdActive(app.profile) })),
    ];
    if (!pairs.length) nodes.push(h("p.field-hint", {}, tb("beh.socd.empty")));
    else nodes.push(h("div.beh-pairs", {}, pairs.map((p, i) => pairCard(p, i))));
    const full = pairs.length >= max;
    nodes.push(h("div.beh-actions", {},
      fid(h("button.btn.sm.primary", { type: "button", disabled: full, title: full ? tb("beh.socd.full", { max }) : undefined, on: { click: addPair } }, icon("plus"), tb("beh.socd.add")), "socd-add"),
      B.SOCD_PRESETS.map((pr) => fid(h("button.btn.sm", { type: "button", disabled: full, on: { click: () => addPreset2(pr) } }, tb(`beh.socd.preset.${pr.id}`)), `socd-preset-${pr.id}`))));
    nodes.push(h("details.beh-modes", {}, h("summary", {}, tb("beh.socd.modes.title")),
      h("dl", {}, B.SOCD_MODES.flatMap((m) => [h("dt", {}, tb(`beh.socd.mode.${m}`)), h("dd", {}, tb(`beh.socd.mode.${m}.tip`))]))));
    return nodes;
  }

  function addPair() {
    const slots = B.nextFreePair(app.profile, app.layout);
    if (!slots) { fail("overlap"); return; }
    const r = app.edit((p) => B.addSocdPair(p, { a: slots[0], b: slots[1] }));
    if (!r.ok) fail(r.reason);
  }

  function addPreset2(preset) {
    let added = 0;
    app.edit((p) => {
      for (const [ka, kb] of preset.pairs) {
        const a = app.layout.byId.get(ka)?.slot, b = app.layout.byId.get(kb)?.slot;
        if (a !== undefined && b !== undefined && B.addSocdPair(p, { a, b }).ok) added++;
      }
    });
    toast(added ? tb("beh.socd.added", { n: added }) : tb("beh.socd.addedNone"), added ? "info" : "warn");
  }

  async function changeMode(index, mode) {
    const now = list("socd")[index];
    if (!now || now.mode === mode) { render(); return; }
    if (mode !== 0 && !socdAcknowledged) {
      const ok = await confirmDialog({ title: tb("beh.socd.confirm.title"), body: tb("beh.socd.confirm.body"), confirmLabel: tb("beh.socd.confirm.ok"), danger: true });   // danger: Cancel has the focus
      if (!ok) { render(); return; }
      socdAcknowledged = true;
    }
    const r = app.edit((p) => B.setSocdPair(p, index, { mode }));
    if (!r.ok) fail(r.reason);
  }

  function setPair(index, patch) {
    const r = app.edit((p) => B.setSocdPair(p, index, patch));
    if (!r.ok) { fail(r.reason); render(); }
  }

  function pairCard(pair, index) {
    const taken = B.socdTaken(app.profile, index);
    const keySelect = (field, label) => {
      const sel = fid(h("select", { "aria-label": `${tb("beh.socd.pair", { n: index + 1 })}: ${label}`, on: { change: () => setPair(index, { [field]: Number(sel.value) }) } },
        app.layout.keys.map((k) => h("option", { value: String(k.slot), disabled: taken.has(k.slot) || k.slot === pair[field === "a" ? "b" : "a"] }, k.name))), `socd-${field}-${index}`);
      sel.value = String(pair[field]);
      return h("label.field", {}, h("span.field-label", {}, label), sel);
    };
    const modeSeg = segmented({
      label: `${tb("beh.socd.pair", { n: index + 1 })}: ${tb("beh.socd.mode")}`,
      options: B.SOCD_MODES.map((m, i) => ({ id: String(i), label: tb(`beh.socd.mode.${m}`), title: tb(`beh.socd.mode.${m}.tip`) })),
      value: String(pair.mode),
      onChange: (id) => changeMode(index, Number(id)),
    });
    const layer0 = toggle({ label: tb("beh.socd.layer0"), checked: (pair.flags & B.SOCD_FLAG_LAYER0) !== 0, onChange: (on) => setPair(index, { flags: on ? B.SOCD_FLAG_LAYER0 : 0 }) });
    return h("div.beh-pair", { class: pair.mode !== 0 ? "on" : "" },
      h("div.card-head", {}, h("h3.card-title", {}, tb("beh.socd.pair", { n: index + 1 })),
        fid(h("button.btn.sm.ghost.danger", { type: "button", on: { click: () => app.edit((p) => B.removeSocdPair(p, index)) } }, icon("trash"), tb("beh.socd.remove")), `socd-remove-${index}`)),
      h("div.beh-pair-keys", {}, keySelect("a", tb("beh.socd.a")), keySelect("b", tb("beh.socd.b"))),
      h("div.field", {}, h("span.field-label", {}, tb("beh.socd.mode")), modeSeg.el, h("span.field-hint", {}, tb(`beh.socd.mode.${B.SOCD_MODES[pair.mode]}.tip`))),
      layer0.el);
  }

  // ------------------------------------------------------------------------------------------------ what the selected key does now
  function entryText(b) {
    if (b === null || b === undefined) return tb("beh.cur.transparent");
    if (b.type === "none") return tb("beh.cur.nokey");
    const x = B.behaviorOf(b);
    if (x?.kind === "dks") return list("dks")[x.index] ? tb("beh.cur.dks", { n: x.index + 1 }) : tb("beh.cur.dks.missing", { n: x.index + 1 });
    if (x?.kind === "morph") {
      const rec = list("morph")[x.index];
      if (!rec) return tb("beh.cur.morph.missing", { n: x.index + 1 });
      const mods = B.MORPH_MODIFIERS.filter((u) => B.morphHasMod(rec, u)).map(usageName).join(" / ");
      return tb("beh.cur.morph", { n: x.index + 1, normal: usageName(x.usage), morph: usageName(rec.usage), mods });
    }
    if (x?.kind === "toggle") return tb("beh.cur.toggle", { name: usageName(x.usage) });
    if (b.type === "fw") return tb("beh.cur.other");
    const u = findUsage(b);
    if (u) return tOr(`u.${u.name}`, u.name);
    if (b.type === "taphold") return t("remap.th.label", { tap: entryText(b.tap), hold: entryText(b.hold) });
    return b.type;
  }

  function paintCurrent() {
    const sel = app.selected;
    if (!sel.length) { cur.replaceChildren(h("span.eyebrow", {}, tb("beh.cur.title")), h("span.what", {}, tb("beh.cur.none"))); return; }
    const slot = app.primary ?? sel[0];
    const keyId = app.idOf(slot);
    const lines = [h("span.eyebrow", {}, tb("beh.cur.title")),
      h("div", {}, h("strong", {}, tb("beh.cur.line", { key: keyLabel(keyId), layer: layerName(app.layer) })), sel.length > 1 ? h("span.field-hint", {}, ` ${tb("beh.cur.more", { n: sel.length - 1 })}`) : null),
      h("div.what", {}, entryText(app.profile.keys[keyId].bindings[app.layer]))];
    const idx = list("socd").findIndex((p) => p.a === slot || p.b === slot);
    if (idx >= 0) lines.push(h("div.what", {}, tb("beh.cur.socd", { n: idx + 1, mode: tb(`beh.socd.mode.${B.SOCD_MODES[list("socd")[idx].mode]}`) })));
    cur.replaceChildren(...lines);
  }

  function paintIssues() {
    const lines = B.behaviorIssues(app.profile).map((i) => B.explainIssue(i, app.active + 1));
    let codec = null;
    if (!lines.length) {
      const err = B.codecProblem(app.profile, app.layout);
      if (err) codec = B.explainSettingsError(err);
    }
    issuesBox.hidden = !lines.length && !codec;
    if (issuesBox.hidden) return;
    const texts = lines.map((l) => tb(l.key, l.params));
    issuesBox.lastChild.replaceChildren(h("strong", {}, tb("beh.issues.title")), codec ? h("p", {}, tb("beh.issues.codec")) : null,
      h("ul", {}, (codec ? [codec] : []).map((c) => h("li", {}, tb(c.key, c.params))), texts.map((x) => h("li", {}, x))));
  }

  // ------------------------------------------------------------------------------------------------ render
  const SECTIONS = { dks: dksSection, morph: morphSection, toggle: toggleSection, socd: socdSection };

  function render() {
    const active = document.activeElement;
    const focusId = active && panel.contains(active) ? active.dataset?.fid ?? null : null;
    depthParts = null;
    forProfile.querySelector("span").textContent = tb("beh.forProfile", { n: app.active + 1 });
    tabSeg.set(tab);
    layerSeg.set(String(app.layer));
    layerRow.hidden = tab === "socd";
    paintCurrent();
    paintIssues();
    content.replaceChildren(...SECTIONS[tab]());
    paintKeys();
    if (focusId) content.querySelector(`[data-fid="${focusId}"]`)?.focus({ preventScroll: true });
  }

  /** A change from elsewhere (the edit just made, another page, a switch of profile). While a dot is dragged only the bar moves. */
  function sync() {
    if (dragging && depthParts) {
      const rec = list("dks")[depthParts.index];
      if (rec) { depthParts.paint(rec); paintCurrent(); paintIssues(); return; }
    }
    render();
  }

  const offs = [app.on("selection", render), app.on("profile", sync), app.on("prefs", () => { paintKeys(); })];
  render();
  root.append(h("div.page-grid.remap", {}, card.el, panel));
  return { destroy() { offs.forEach((o) => o()); card.destroy(); } };
}
