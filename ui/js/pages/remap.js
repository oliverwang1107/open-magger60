// Layers page: four layers on the keyboard; pick keys on the canvas, then pick what they do on that layer from a searchable list.
// Only actions the keyboard can really hold are offered (letters, numbers, modifiers, media keys, hold-FN-layer, toggle layer, one-shot
// modifiers, switch profile N, transparent, no key); anything else would be refused by the firmware's settings format.
// Below the list: the dual-role (tap / hold) key composer, which needs two choices and so is not a list entry, and the layer behaviour
// options of the profile (tap term, tap policy, one-shot timeout).
import { h, icon, toast } from "../dom.js";
import { t, tOr } from "../i18n.js";
import { keyboardCard } from "../ui/kb-card.js";
import { confirmDialog } from "../ui/dialog.js";
import { ALL_USAGES, GROUP_ORDER, TAP_MEDIA_USAGES, bindingId, bindingLabel, bindingFromUsage, cleanBinding, findUsage, searchUsages } from "../core/hid-usages.js";
import { DEFAULT_FN_LAYER, LAYER_COUNT, LAYER_NAME_MAX, TAP_POLICY_NAMES, defaultLayerOpts, layerOptsProblem, toggleTraps, unreachableLayers } from "../core/profile.js";
import { FW_LAYER_OPTS } from "../core/limits.js";
import { segmented } from "../ui/controls.js";
import { bindingStorable } from "../core/profile_codec.js";

const usageName = (u) => tOr(`u.${u.name}`, u.name);
/** Text of one binding in the current language; a tap-hold key is spelled out ("Tap Space, hold Left Ctrl"). */
const bindingText = (b) => {
  if (b?.type === "taphold") return t("remap.th.label", { tap: bindingText(b.tap), hold: bindingText(b.hold) });
  const u = findUsage(b);
  return u ? usageName(u) : bindingLabel(b);
};
const defaultLayerName = (i) => t(`remap.layer${i}`);
/** A layer's name: the one the person gave it (kept in this browser), else the default. */
const layerName = (profile, i) => profile.layerNames?.[i] || defaultLayerName(i);

/** What a key does on `layer` out of the box (the key map's own table). null = transparent. */
export function defaultBinding(key, layer) {
  if (layer === 0) return bindingFromUsage(key.usage);
  if (layer === 1) return DEFAULT_FN_LAYER[key.id] ? { ...DEFAULT_FN_LAYER[key.id] } : null;
  return null;
}

export function mount(root, app) {
  const card = keyboardCard(app, { mode: "remap" });
  let group = "all";
  let query = "";

  // layer tabs: built from the profile so names and the "unreachable" marker follow the settings
  const layerTabs = h("div.seg.block", { role: "radiogroup", "aria-label": t("remap.layer") });
  const paintTabs = () => {
    const un = unreachableLayers(app.profile, app.layout);
    layerTabs.replaceChildren(...Array.from({ length: LAYER_COUNT }, (_, i) => h("button", {
      type: "button", role: "radio", "aria-checked": String(app.layer === i), class: un.includes(i) ? "unreachable" : "",
      title: un.includes(i) ? t("remap.unreachable.tab") : t(`remap.layer${i}.tip`),
      on: { click: () => { app.layer = i; card.view.refresh(); sync(); } },
    }, layerName(app.profile, i), un.includes(i) ? h("i.warn-dot", { "aria-hidden": "true" }) : null)));
    return un;
  };
  const layerSeg = { el: layerTabs };
  const nameInput = h("input", { type: "text", maxlength: LAYER_NAME_MAX, "aria-label": t("remap.layerName"), placeholder: t("remap.layerName"),
    on: { change: () => app.edit((p) => { p.layerNames[app.layer] = nameInput.value.trim(); }) } });
  const forProfile = h("p.field-hint.note", {}, icon("info"), h("span.fp"));
  const unreachableNote = h("div.banner.info.unreach", { hidden: true, role: "status" }, icon("info"), h("div"));

  const title = h("h2.card-title");
  const current = h("div.cur-binding");
  const search = h("input", { type: "search", placeholder: t("remap.search"), "aria-label": t("remap.search"), autocomplete: "off", on: { input: () => { query = search.value; renderList(); } } });
  const chips = h("div.group-chips", { role: "group", "aria-label": t("remap.groups") });
  const list = h("div.usage-list", { role: "listbox", "aria-label": t("remap.list") });
  const clearBtn = h("button.btn.sm", { type: "button", title: t("remap.transparent.tip"), on: { click: () => assign(null) } }, t("remap.transparent"));
  const resetBtn = h("button.btn.sm.ghost", { type: "button", on: { click: resetKeys } }, icon("reset"), t("remap.reset"));
  const resetLayerBtn = h("button.btn.sm.ghost", { type: "button", on: { click: resetLayer } }, icon("reset"), t("remap.resetLayer"));
  const noKey = h("div.empty", {}, icon("remap"), h("h3", {}, t("remap.empty.title")), h("p", {}, t("remap.empty.body")));
  const hiddenNote = h("p.field-hint.note", {}, icon("info"), t("remap.hiddenNote"));
  const trapNote = h("div.banner.info.unreach", { hidden: true, role: "status" }, icon("info"), h("div"));

  // ---- dual-role (tap / hold) key: two choices, assigned together
  const tapChoices = ALL_USAGES.filter((u) => (u.binding?.type === "key" && !u.binding.mods) || (u.binding?.type === "media" && TAP_MEDIA_USAGES.includes(u.binding.usage)));
  const holdChoices = ALL_USAGES.filter((u) => (u.binding?.type === "layer") || (u.binding?.type === "key" && u.group === "modifiers"));
  const optionGroups = (items) => {
    const out = [];
    for (const g of GROUP_ORDER) {
      const part = items.filter((u) => u.group === g);
      if (part.length) out.push(h("optgroup", { label: t(`group.${g}`) }, part.map((u) => h("option", { value: u.id }, usageName(u)))));
    }
    return out;
  };
  const tapSel = h("select", { "aria-label": t("remap.th.tap") }, optionGroups(tapChoices));
  const holdSel = h("select", { "aria-label": t("remap.th.hold") }, optionGroups(holdChoices));
  tapSel.value = findUsage({ type: "key", usage: 44 })?.id ?? tapChoices[0].id;      // Space
  holdSel.value = findUsage({ type: "layer", layer: 1 })?.id ?? holdChoices[0].id;
  const thBtn = h("button.btn.sm.primary", { type: "button", on: { click: () => {
    const tap = tapChoices.find((u) => u.id === tapSel.value)?.binding;
    const hold = holdChoices.find((u) => u.id === holdSel.value)?.binding;
    if (tap && hold) assign({ type: "taphold", tap, hold });
  } } }, t("remap.th.assign"));
  const thBox = h("div.adv-box", {},
    h("div.eyebrow", {}, t("remap.th.title")),
    h("p.field-hint", {}, t("remap.th.hint")),
    h("div.fields.compact", {},
      h("label.field", {}, h("span.field-label", {}, t("remap.th.tap")), tapSel),
      h("label.field", {}, h("span.field-label", {}, t("remap.th.hold")), holdSel)),
    h("div.row", {}, thBtn));

  // ---- layer behaviour options of this profile (kept on the keyboard)
  const L = FW_LAYER_OPTS;
  const numberInput = (key, min, max, step, ariaKey) => {
    const input = h("input", { type: "number", min, max, step, inputmode: "numeric", "aria-label": t(ariaKey), on: { change: () => setOpt(key, input) } });
    return input;
  };
  function setOpt(key, input) {
    const v = Math.round(Number(input.value));
    const next = { ...(app.profile.layerOpts ?? defaultLayerOpts()), [key]: v };
    if (!Number.isFinite(v) || layerOptsProblem(next)) {
      input.value = (app.profile.layerOpts ?? defaultLayerOpts())[key];                  // out of range: keep what was there
      toast(t("remap.opts.range"), "warn");
      return;
    }
    app.edit((p) => { p.layerOpts = next; });
  }
  const termInput = numberInput("tapTermMs", L.tapTermMin, L.tapTermMax, 10, "remap.opts.term");
  const oneShotInput = numberInput("oneShotMs", 0, L.oneShotMax, 100, "remap.opts.oneshot");
  const policySeg = segmented({
    label: t("remap.opts.policy"), block: true,
    options: TAP_POLICY_NAMES.map((id) => ({ id, label: t(`remap.opts.policy.${id}`), title: t(`remap.opts.policy.${id}.tip`) })),
    value: "timeout",
    onChange: (id) => app.edit((p) => { p.layerOpts = { ...(p.layerOpts ?? defaultLayerOpts()), tapPolicy: id }; }),
  });
  const policyTip = h("p.field-hint", {}, "");
  const optsBox = h("div.adv-box", {},
    h("div.eyebrow", {}, t("remap.opts.title")),
    h("p.field-hint", {}, t("remap.opts.hint", { n: 0 })),
    h("div.fields.compact", {},
      h("label.field", {}, h("span.field-label", {}, t("remap.opts.term")), termInput, h("span.field-hint", {}, t("remap.opts.term.hint", { min: L.tapTermMin, max: L.tapTermMax }))),
      h("label.field", {}, h("span.field-label", {}, t("remap.opts.oneshot")), oneShotInput, h("span.field-hint", {}, t("remap.opts.oneshot.hint", { min: L.oneShotMin, max: L.oneShotMax })))),
    h("div.field", {}, h("span.field-label", {}, t("remap.opts.policy")), policySeg.el, policyTip));
  const optsHint = optsBox.querySelector(".field-hint");

  const body = h("div.remap-body", {},
    forProfile,
    h("div.remap-top", {}, layerSeg.el, h("div.row", {}, clearBtn, resetBtn, resetLayerBtn)),
    h("div.layer-meta", {}, h("p.field-hint", { class: "layer-tip" }, ""), nameInput),
    unreachableNote,
    trapNote,
    current,
    h("div.search", {}, icon("search"), search),
    chips, list, hiddenNote,
    thBox);
  const panel = h("section.card.remap-panel", {}, h("div.card-head", {}, title), body, noKey, optsBox);
  const layerTip = body.querySelector(".layer-tip");

  function assign(binding) {
    const slots = app.selected;
    if (!slots.length) return;
    app.edit((p) => slots.forEach((sl) => (p.keys[app.idOf(sl)].bindings[app.layer] = binding ? cleanBinding(binding) : null)));
  }

  function resetKeys() {
    const slots = app.selected;
    if (!slots.length) return;
    app.edit((p) => slots.forEach((sl) => {
      p.keys[app.idOf(sl)].bindings[app.layer] = defaultBinding(app.layout.bySlot.get(sl), app.layer);
    }));
    toast(t("remap.resetDone", { n: slots.length }));
  }

  async function resetLayer() {
    const ok = await confirmDialog({ title: t("remap.resetLayer.title", { layer: layerName(app.profile, app.layer) }), body: t("remap.resetLayer.body"), confirmLabel: t("remap.resetLayer.ok"), danger: true });
    if (!ok) return;
    app.edit((p) => app.layout.keys.forEach((k) => { p.keys[k.id].bindings[app.layer] = defaultBinding(k, app.layer); }));
    toast(t("remap.resetLayer.done", { layer: layerName(app.profile, app.layer) }));
  }

  function renderChips() {
    chips.replaceChildren(...["all", ...GROUP_ORDER].map((g) =>
      h("button.chip", { type: "button", class: group === g ? "on" : "", "aria-pressed": String(group === g), on: { click: () => { group = g; renderChips(); renderList(); } } }, g === "all" ? t("remap.group.all") : t(`group.${g}`))));
  }

  function currentIds() {
    return new Set(app.selected.map((sl) => bindingId(app.dataOf(sl).bindings[app.layer])));
  }

  function renderList() {
    const ids = currentIds();
    const sameId = ids.size === 1 ? [...ids][0] : null;
    let items = searchUsages(query, (g) => t(`group.${g}`));
    // search also matches the translated name
    if (query.trim()) {
      const q = query.trim().toLowerCase();
      items = [...new Set([...items, ...ALL_USAGES.filter((u) => usageName(u).toLowerCase().includes(q))])];
    }
    // offered: everything the keyboard can store, plus every layer / profile function code of the key map (a hold-layer-2/3 key is
    // marked when this firmware cannot store it yet); "transparent" only exists on overlay layers
    const offered = (u) => (u.binding === null ? app.layer > 0 : bindingStorable(u.binding, app.layer) || u.binding?.type === "layer");
    const storable = items.filter(offered);
    hiddenNote.hidden = !ALL_USAGES.some((u) => !offered(u));
    items = storable;
    if (group !== "all") items = items.filter((u) => u.group === group);
    chips.querySelectorAll(".chip").forEach((c, i) => {
      const g = ["all", ...GROUP_ORDER][i];
      c.hidden = g !== "all" && !ALL_USAGES.some((u) => u.group === g && offered(u));
    });
    if (!items.length) {
      list.replaceChildren(h("div.empty.small", {}, h("p", {}, t("remap.noresult", { q: query }))));
      return;
    }
    const nodes = [];
    for (const g of GROUP_ORDER) {
      const part = items.filter((u) => u.group === g);
      if (!part.length) continue;
      if (group === "all") nodes.push(h("div.eyebrow.grp", {}, t(`group.${g}`)));
      nodes.push(h("div.usage-grid", {}, part.map((u) => h("button.usage", {
        type: "button", role: "option", "aria-selected": String(sameId === u.id),
        class: `${sameId === u.id ? "active" : ""} ${u.binding && !bindingStorable(u.binding, app.layer) ? "unavailable" : ""}`,
        title: u.binding && !bindingStorable(u.binding, app.layer) ? `${usageName(u)} — ${t("remap.unavailable")}` : usageName(u),
        on: { click: () => assign(u.binding) },
      }, usageName(u)))));
    }
    list.replaceChildren(...nodes);
  }

  function sync() {
    const slots = app.selected;
    const has = slots.length > 0;
    body.hidden = !has;
    noKey.hidden = has;
    title.textContent = has ? t("remap.title", { n: slots.length }) : t("remap.title0");
    const un = paintTabs();
    layerTip.textContent = t(`remap.layer${app.layer}.tip`);
    forProfile.querySelector(".fp").textContent = t("remap.forProfile", { n: app.active + 1 });
    if (document.activeElement !== nameInput) nameInput.value = app.profile.layerNames?.[app.layer] ?? "";
    nameInput.placeholder = defaultLayerName(app.layer);
    unreachableNote.hidden = !un.length;
    if (un.length) unreachableNote.lastChild.textContent = un.map((l) => t("remap.unreachable", { layer: layerName(app.profile, l), n: l })).join(" ");
    const traps = toggleTraps(app.profile, app.layout);
    trapNote.hidden = !traps.length;
    if (traps.length) trapNote.lastChild.textContent = traps.map((l) => t("remap.trap", { layer: layerName(app.profile, l), n: l })).join(" ");
    const opts = app.profile.layerOpts ?? defaultLayerOpts();
    if (document.activeElement !== termInput) termInput.value = opts.tapTermMs;
    if (document.activeElement !== oneShotInput) oneShotInput.value = opts.oneShotMs;
    policySeg.set(opts.tapPolicy);
    policyTip.textContent = t(`remap.opts.policy.${opts.tapPolicy}.tip`);
    optsHint.textContent = t("remap.opts.hint", { n: app.active + 1 });
    clearBtn.textContent = app.layer === 0 ? t("remap.nokey") : t("remap.transparent");
    clearBtn.title = app.layer === 0 ? t("remap.nokey.tip") : t("remap.transparent.tip");
    if (!has) return;
    const ids = currentIds();
    const k = app.layout.bySlot.get(app.primary);
    const b = app.dataOf(app.primary)?.bindings[app.layer];
    current.replaceChildren(
      h("span.eyebrow", {}, slots.length > 1 ? t("remap.curMulti", { n: slots.length }) : t("remap.cur")),
      h("div.cur-row", {},
        h("kbd", {}, k?.name ?? "—"), icon("link"),
        ids.size > 1 ? h("span.chip.warn", {}, t("common.mixed")) : h("strong", {}, b ? bindingText(b) : (app.layer === 0 ? t("remap.nokey") : t("remap.transparent")))));
    // a selected dual-role key fills the composer with its own choices
    if (ids.size === 1 && b?.type === "taphold") {
      const tid = findUsage(b.tap)?.id;
      const hid = findUsage(b.hold)?.id;
      if (tid && tapChoices.some((u) => u.id === tid)) tapSel.value = tid;
      if (hid && holdChoices.some((u) => u.id === hid)) holdSel.value = hid;
    }
    renderList();
  }

  const offs = [app.on("selection", sync), app.on("profile", sync)];
  renderChips();
  sync();
  root.append(h("div.page-grid.remap", {}, card.el, panel));
  return { destroy() { offs.forEach((o) => o()); card.destroy(); } };
}
