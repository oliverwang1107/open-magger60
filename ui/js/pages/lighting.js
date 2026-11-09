// Lights page: the keyboard's own lights (61 key lights). Effect, brightness, speed, colour and the idle fade-out time are sent live
// with LED_SET while the person changes them; once they stop, ONE LED_SET with the SAVE flag keeps them in the keyboard's flash and
// the status bits say pending / saved / failed / restored at boot (core/led_keep.js, CONFIG_PROTOCOL.md 0x20 / 0x21). On firmware
// that cannot keep them the page says so. Without feature bit 6 (no lights, old firmware) or without a keyboard it explains why and
// offers nothing to click.
//
// UI round 2 adds the light scene on the same page (tabs next to the effect controls): per-key colours with gradients, indicator rules, and
// the scene on the keyboard (write / read back / presets / files). core/scene_model.js and core/scene_keeper.js do the work, pages/lighting_scene.js
// builds the controls. A keyboard without the scene operations keeps the effect tab and says why the other tabs are empty.
import { h, icon } from "../dom.js";
import { t } from "../i18n.js";
import { KeyboardView } from "../ui/keyboard.js";
import { rangeField } from "../ui/controls.js";
import { classifyProblem } from "../device/errors.js";
import { MAX_TRAVEL_RAW } from "../core/units.js";
import {
  LED_EFFECTS, LED_EFFECTS_PICKABLE, LED_EFFECT_CUSTOM, ledEffectById, ledAvailability, previewLed, ledColorHex, withLedColor,
  brightnessToPct, pctToBrightness, speedToStep, stepToSpeed, SPEED_STEPS, ledEffectIndex,
} from "../core/led.js";
import { SceneKeeper, sceneAvailability } from "../core/scene_keeper.js";
import { SceneModel } from "../core/scene_model.js";
import { applyRules } from "../core/scene_rules.js";
import { ledOfSlot } from "../core/led_map.js";
import { buildScenePanels, sceneNotice } from "./lighting_scene.js";
import { LedKeeper } from "../core/led_keep.js";
import { LED_IDLE_DEFAULT_S } from "../core/led_ex.js";
import { profileName } from "./profiles.js";

// the first-screen "looks": effect looks set the keyboard's own effect; key looks fill the light scene (shared) and switch to the per-key effect
const LOOKS = [
  { id: "single", fx: "static" }, { id: "rainbow", fx: "rainbow" }, { id: "breath", fx: "breath" }, { id: "reactive", fx: "reactive" },
  { id: "wasd", scene: "wasd" }, { id: "zones", scene: "zones" }, { id: "ocean", scene: "ocean" }, { id: "sunset", scene: "sunset" },
];
const SWATCHES = ["#ffffff", "#ff0000", "#ff8000", "#ffff00", "#00ff00", "#00ffd5", "#0080ff", "#4000ff", "#c000ff", "#ff0080"];
// idle fade-out choices in seconds (0 = never); the keyboard's own value is added when it is something else
const IDLE_CHOICES_S = [0, 60, 300, LED_IDLE_DEFAULT_S, 1800, 3600];
const idleLabel = (s) => (s === 0 ? t("led.idle.never") : s % 3600 === 0 ? t("led.idle.hours", { n: s / 3600 }) : s % 60 === 0 ? t("led.idle.min", { n: s / 60 }) : t("led.idle.sec", { n: s }));
const why = (err) => {
  const p = classifyProblem(err);
  return `${t(`problem.${p.kind}.title`)}${p.detail ? ` (${p.detail})` : ""}`;
};

export function mount(root, app) {
  let view = null;       // the keyboard preview, only while the lights are usable
  let link = null;       // LedKeeper of the current device
  let shownDev = null;   // the device and availability the page was last built for
  let shownAvail = null;
  let profMode = { profile: null, live: true };   // which key profile's lights the page edits (null = the firmware has one set for all)
  let token = 0;
  let tab = "effect";    // the tab survives a reconnect
  let onLinkChange = () => {};
  let simplePaint = () => {};   // repaints the first-screen panel
  const body = h("div.page-stack");
  root.append(body);

  // ---- the page for a keyboard with lights ----
  function buildControls(dev, my) {
    const stateLine = h("p.field-hint.led-state", { role: "status", "aria-live": "polite" });
    const desc = h("p.field-hint.led-desc");
    const fxButtons = LED_EFFECTS_PICKABLE.map((e) => h("button.fx", {
      type: "button", role: "radio", "data-id": e.id, title: t(`led.fxd.${e.id}`),
      disabled: e.id === "custom" && !dev.features?.scene,
      on: { click: () => link.set({ effect: LED_EFFECTS.indexOf(e) }) },
    }, t(`led.fx.${e.id}`)));
    const grid = h("div.fx-grid", { role: "radiogroup", "aria-label": t("led.effect") }, fxButtons);

    const brightness = rangeField({ id: "led-bri", label: t("led.brightness"), hint: t("led.brightness.hint"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => link.set({ brightness: pctToBrightness(v) }) });
    const speed = rangeField({ id: "led-spd", label: t("led.speed"), hint: t("led.speed.hint"), min: SPEED_STEPS.min, max: SPEED_STEPS.max, step: 1, unit: "", onInput: (v) => link.set({ speed: stepToSpeed(v) }) });

    const setColor = (hex) => link.set(withLedColor(link.state, hex));
    const colorInput = h("input", { type: "color", "aria-label": t("led.color"), on: { input: () => setColor(colorInput.value) } });
    const swatches = SWATCHES.map((c) => h("button.swatch", { type: "button", style: { background: c }, title: c, "aria-label": c, on: { click: () => setColor(c) } }));
    const colorBlock = h("div.field", {}, h("div.field-label", {}, t("led.color")), h("div.palette", {}, swatches, h("label.swatch.custom", { title: t("led.custom") }, colorInput)));

    const retry = h("button.btn.sm", { type: "button", hidden: true, on: { click: () => (link.error ? link.retry() : link.retrySave()) } }, icon("reset"), t("led.retry"));

    // idle fade-out: presets, plus the keyboard's own value when it is not one of them (0 = never)
    const idleSel = h("select", { id: "led-idle", "aria-label": t("led.idle"), on: { change: () => link.set({ idleS: Number(idleSel.value) }) } });
    const idleNote = h("p.field-hint.led-idle-note", { hidden: true });
    const idleBlock = h("div.field", {}, h("label.field-label", { for: "led-idle" }, t("led.idle")), idleSel, h("div.field-hint", {}, t("led.idle.hint")), idleNote);
    const paintIdle = (s) => {
      if (!link.hasIdle) { idleBlock.hidden = true; return; }
      idleBlock.hidden = false;
      const values = [...new Set([...IDLE_CHOICES_S, s.idleS])].sort((a, b) => (a === 0 ? 1 : b === 0 ? -1 : a - b));
      const sig = values.join();
      if (idleSel.dataset.sig !== sig) { idleSel.dataset.sig = sig; idleSel.replaceChildren(...values.map((v) => h("option", { value: String(v) }, idleLabel(v)))); }
      idleSel.value = String(s.idleS);
      idleNote.hidden = !link.flags.idleOff;
      idleNote.textContent = t("led.idle.off");
    };
    // what is kept on the keyboard: the honest answer, from the status bits
    const saveBox = h("div.banner.led-save", { role: "status", "aria-live": "polite" });
    function paintSave() {
      const sv = link.save;
      let kind = "info", ic = "info", text;
      if (sv.kind === "saved") { kind = "ok"; ic = "check"; text = profMode.profile !== null && !sv.restored ? t("led.save.savedProf", { n: profMode.profile + 1 }) : t(sv.restored ? "led.save.restored" : "led.save.saved"); }
      else if (sv.kind === "saving") { text = t("led.save.saving"); }
      else if (sv.kind === "failed") { kind = "error"; ic = "warn"; text = t("led.save.failed", { why: sv.error ? ` (${why(sv.error)})` : "" }); }
      else { kind = "warn"; ic = "warn"; text = t(`led.save.none.${sv.reason}`); }
      saveBox.className = `banner led-save ${kind}`;
      saveBox.dataset.save = sv.kind;
      saveBox.replaceChildren(icon(ic), h("div", {}, text));
    }

    function paint(s) {
      paintProf();
      simplePaint(s);
      paintIdle(s);
      const fx = ledEffectById(LED_EFFECTS[s.effect].id);
      fxButtons.forEach((b, i) => b.setAttribute("aria-checked", String(i === s.effect)));
      desc.textContent = t(`led.fxd.${fx.id}`) + (fx.id === "custom" && !dev.features?.scene ? ` ${t("led.fxd.custom.noscene")}` : "");
      brightness.set(brightnessToPct(s.brightness));
      brightness.show(fx.id !== "off");
      speed.set(speedToStep(s.speed));
      speed.show(fx.speed);
      const hex = ledColorHex(s);
      colorInput.value = hex;
      swatches.forEach((el) => el.classList.toggle("on", el.title.toLowerCase() === hex.toLowerCase()));
      colorBlock.hidden = !fx.color;
    }
    function paintState() {
      if (link.error) {
        stateLine.textContent = t("led.state.error", { why: why(link.error) });
        stateLine.dataset.state = "error";
      } else {
        stateLine.textContent = link.pending ? t("led.state.sending") : t("led.state.sent");
        stateLine.dataset.state = link.pending ? "sending" : "sent";
      }
      paintSave();
      retry.hidden = !(link.error || link.save.kind === "failed");
    }
    link = new LedKeeper(dev, {
      profile: profMode.profile, live: profMode.live,
      onChange: () => { if (my === token && link.loaded) { paint(link.state); paintState(); onLinkChange(); } },
    });

    // which profile the lights belong to, and whether the keyboard is showing them now
    const profBox = h("div.banner.info.led-prof", { role: "status", "aria-live": "polite" });
    function paintProf() {
      const n = (profMode.profile ?? 0) + 1;
      const name = profileName(app.profiles[profMode.profile ?? 0], profMode.profile ?? 0);
      let kind = "info", text;
      if (profMode.profile === null) { kind = "warn"; text = t("led.prof.old"); }
      else if (!profMode.live && !link.canSave) { kind = "warn"; text = t("led.prof.other.nosave", { n }); }
      else if (!profMode.live) text = t("led.prof.other", { n, name });
      else if (!link.ownLights) text = t("led.prof.shared", { n });
      else text = t("led.prof.live", { n, name });
      profBox.className = `banner ${kind} led-prof`;
      profBox.dataset.mode = profMode.profile === null ? "shared" : profMode.live ? "live" : "other";
      profBox.replaceChildren(icon(kind === "warn" ? "warn" : "info"), h("div", {}, text));
    }

    const panel = h("section.card.light-panel", {},
      h("div.card-head", {}, h("h2.card-title", {}, t("led.title")), h("span.card-sub", {}, t("led.sub"))),
      h("div.led-fields", {},
        h("div.field", {}, h("div.field-label", {}, t("led.effect")), grid, desc),
        brightness.el, speed.el, colorBlock, idleBlock,
        h("div.led-foot", {}, stateLine, retry)));
    return { panel, paint, paintState, profBox, saveBox };
  }

  // ---- the first screen: pick a look, one colour, brightness, and where it is saved ----
  function buildSimple(dev, ui) {
    const hint = h("p.field-hint.easy-keys", { hidden: true }, t("easy.look.keys.hint"));
    const lookBtns = LOOKS.map((l) => h("button.fx.look", {
      type: "button", role: "radio", "data-look": l.id, title: l.scene ? t(`scene.preset.${l.scene}.d`) : t(`led.fxd.${l.fx}`),
      disabled: !!l.scene && !sceneUi,
      on: { click: () => pickLook(l) },
    }, t(`easy.look.${l.id}`)));
    const looks = h("div.field", {}, h("div.field-label", {}, t("easy.looks")), h("div.fx-grid.looks", { role: "radiogroup", "aria-label": t("easy.looks") }, lookBtns), h("div.field-hint", {}, t("easy.looks.hint")), hint);
    function pickLook(l) {
      if (l.fx) { link.set({ effect: ledEffectIndex(l.fx) }); return; }
      if (!sceneUi) return;
      sceneUi.model.editor.seal();
      sceneUi.model.applyPreset(l.scene);
      sceneUi.model.editor.seal();
      sceneUi.keeper.saveNow();                       // the scene is written once, right now
      link.set({ effect: LED_EFFECT_CUSTOM });
    }
    const setColor = (hex) => link.set(withLedColor(link.state, hex));
    const colorInput = h("input", { type: "color", "aria-label": t("led.color"), on: { input: () => setColor(colorInput.value) } });
    const swatches = SWATCHES.map((c) => h("button.swatch.big", { type: "button", style: { background: c }, title: c, "aria-label": c, on: { click: () => setColor(c) } }));
    const colorBlock = h("div.field", {}, h("div.field-label", {}, t("easy.color")), h("div.palette", {}, swatches, h("label.swatch.big.custom", { title: t("led.custom") }, colorInput)), h("div.field-hint", {}, t("easy.color.hint")));
    const bright = rangeField({ id: "easy-bri", label: t("easy.bright"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => link.set({ brightness: pctToBrightness(v) }) });
    const retry = h("button.btn.sm", { type: "button", hidden: true, on: { click: () => (link.error ? link.retry() : link.retrySave()) } }, icon("reset"), t("easy.retry"));
    const el = h("section.card.easy-panel", {}, ui.profBox, ui.saveBox, retry, looks, colorBlock, bright.el);
    simplePaint = (s) => {
      const fx = LED_EFFECTS[s.effect];
      lookBtns.forEach((b, i) => {
        const l = LOOKS[i];
        b.setAttribute("aria-checked", String(l.fx ? fx.id === l.fx : fx.id === "custom"));
      });
      hint.hidden = fx.id !== "custom";
      colorBlock.hidden = !fx.color;
      bright.set(brightnessToPct(s.brightness));
      bright.show(fx.id !== "off");
      const hex = ledColorHex(s);
      colorInput.value = hex;
      swatches.forEach((x) => x.classList.toggle("on", x.title.toLowerCase() === hex.toLowerCase()));
      retry.hidden = !(link.error || link.save.kind === "failed");
    };
    return { el };
  }

  // what the keyboard preview draws: the effect (tab 1), or the light scene (tabs 2-4), see colorFnFor()
  let sceneUi = null;    // { panels, model, keeper } while the scene is usable
  let memo = { t: -1, colors: null };
  const sceneColors = (tSec) => {
    if (memo.t === tSec) return memo.colors;
    let c = sceneUi.model.shown;
    if (tab === "rules") c = applyRules(c, sceneUi.model.rules, sceneUi.inputs, Math.floor(tSec * 2) % 2 === 0);
    memo = { t: tSec, colors: c };
    return c;
  };
  function colorFn(key, tSec) {
    const s = link?.state;
    if (!s) return [0, 0, 0];
    const led = ledOfSlot(key.slot);
    if (sceneUi && led >= 0) {
      if (tab !== "effect") return sceneColors(tSec)[led];
      if (s.effect === LED_EFFECT_CUSTOM) return sceneColors(tSec)[led].map((v) => (v * s.brightness) / 255);
    }
    if (s.effect > 6) return [0, 0, 0];
    return previewLed(s, { x: key.x + key.w / 2, tSec, depth: Math.min(1, app.feed.depth[key.slot] / MAX_TRAVEL_RAW) });
  }

  let previewCard = null;
  function buildPreview() {
    view = new KeyboardView(app, { mode: "lighting", colorFn });
    previewCard = h("section.card.kb-card.led-preview", { "aria-label": t("led.title") }, view.el, h("p.kb-hint", {}, t("led.preview")));
    return previewCard;
  }

  function notice(kind) {
    return h("section.card.led-notice", {},
      h("div.empty", {}, icon("lighting"), h("h3", {}, t(`led.na.${kind}.title`)), h("p", {}, t(`led.na.${kind}.body`)),
        h("button.btn.sm", { type: "button", on: { click: () => app.setPage("device") } }, t("led.goDevice"))));
  }

  // ---- the tabs: effect | key colours | indicators | scene ----
  const TABS = ["effect", "colors", "rules", "scene"];
  function buildTabs(effectPanel, sceneSlots) {
    const panels = { effect: effectPanel, ...sceneSlots };
    const buttons = {};
    const wrap = h("div.led-tabs");
    const list = h("div.seg.block.led-tablist", { role: "tablist", "aria-label": t("scene.tabs") }, TABS.map((id) => {
      const b = h("button", { type: "button", role: "tab", id: `led-tab-${id}`, "aria-controls": `led-panel-${id}`, "data-tab": id,
        on: { click: () => select(id), keydown: (e) => {
          const i = TABS.indexOf(id);
          const to = e.key === "ArrowRight" ? TABS[(i + 1) % TABS.length] : e.key === "ArrowLeft" ? TABS[(i + TABS.length - 1) % TABS.length] : e.key === "Home" ? TABS[0] : e.key === "End" ? TABS.at(-1) : null;
          if (to) { e.preventDefault(); select(to); buttons[to].focus(); }
        } } }, t(`scene.tab.${id}`));
      buttons[id] = b;
      return b;
    }));
    const holders = Object.fromEntries(TABS.map((id) => [id, h("div.led-tabpanel", { role: "tabpanel", id: `led-panel-${id}`, "aria-labelledby": `led-tab-${id}`, tabindex: "-1" }, panels[id])]));
    function select(id) {
      tab = id;
      memo = { t: -1, colors: null };
      TABS.forEach((x) => {
        buttons[x].setAttribute("aria-selected", String(x === id));
        buttons[x].tabIndex = x === id ? 0 : -1;
        holders[x].hidden = x !== id;
      });
      previewCard?.classList.toggle("editing", id !== "effect");
      if (id !== "colors") sceneUi?.model.clearPreview();
    }
    wrap.append(list, ...TABS.map((id) => holders[id]));
    select(tab);
    return wrap;
  }

  const sceneCard = (key, body) => h("section.card.scene-card", {}, h("div.card-head", {}, h("h2.card-title", {}, t(`scene.tab.${key}`)), h("span.card-sub", {}, t(`scene.sub.${key}`))), body);

  // ---- (re)build whenever the device or its abilities change ----
  async function refresh() {
    const dev = app.device;
    const avail = ledAvailability(dev);
    const sceneAvail = sceneAvailability(dev);
    // lights per key profile: edit the profile selected in the top bar; it is "live" when the keyboard is in that profile
    const perProfile = !!dev?.features?.lightProfiles;
    const next = perProfile ? { profile: app.active, live: app.deviceActive == null || app.deviceActive === app.active } : { profile: null, live: true };
    const sig = `${avail}/${sceneAvail}/${next.profile}/${next.live}`;
    if (dev === shownDev && sig === shownAvail) return;
    [shownDev, shownAvail] = [dev, sig];
    profMode = next;
    const my = ++token;
    link?.dispose();
    link = null;
    sceneUi?.panels.destroy();
    sceneUi?.keeper.dispose();
    sceneUi = null;
    onLinkChange = () => {};
    view?.destroy();
    view = null;

    if (avail !== "ok") { body.replaceChildren(notice(avail)); return; }

    const ui = buildControls(dev, my);
    simplePaint = () => {};
    const slot = h("div.led-slot");
    const side = h("div.led-side", {}, slot);
    const layout = h("div.page-grid.light", {}, buildPreview(), side);
    body.replaceChildren(layout);

    // the scene half: only when the keyboard has the scene operations; otherwise the tabs say why they are empty
    const sceneSlots = {};
    let helpEl = null;
    if (sceneAvail === "ok") {
      const keeper = new SceneKeeper(dev, { onChange: () => { if (my === token) sceneUi?.panels.paint(); } });
      const model = new SceneModel({ layout: app.layout, keeper, selection: () => app.selection });
      const inputs = {};
      const panels = buildScenePanels({ app, model, keeper, inputs, getLink: () => link, keyTarget: root, goDevice: () => app.setPage("device") });
      sceneUi = { panels, model, keeper, inputs };
      sceneSlots.colors = sceneCard("colors", panels.colors);
      sceneSlots.rules = sceneCard("rules", panels.rules);
      sceneSlots.scene = sceneCard("scene", panels.scene);
      helpEl = panels.help;
      onLinkChange = () => panels.paint();
    } else {
      for (const id of ["colors", "rules", "scene"]) sceneSlots[id] = sceneCard(id, sceneNotice(sceneAvail, () => app.setPage("device")));
    }
    const tabs = buildTabs(ui.panel, sceneSlots);
    const simple = buildSimple(dev, ui);
    const more = h("details.led-more", {}, h("summary", {}, t("easy.more"), h("span.field-hint", {}, ` ${t("easy.more.hint")}`)), tabs, helpEl);

    const load = async () => {
      slot.replaceChildren(h("p.field-hint", { role: "status" }, t("led.loading")));
      try {
        await link.load();
        if (my !== token) return;
        slot.replaceChildren(simple.el, more);
        simplePaint(link.state);
        ui.paintState();
        if (sceneUi) {
          try { await sceneUi.keeper.load(); sceneUi.model.resetFromKeeper(); } catch (err) {
            if (my !== token) return;
            sceneUi.panels.setLoadError(why(err));
          }
          sceneUi.panels.paint();
        }
      } catch (err) {
        if (my !== token) return;
        slot.replaceChildren(h("div.banner.error", {}, icon("warn"), h("div", {}, t("led.loadFail", { why: why(err) }))),
          h("button.btn.sm", { type: "button", on: { click: load } }, icon("reset"), t("led.retry")));
      }
    };
    await load();
  }

  const off = app.on("device", () => { refresh(); });
  const offProfile = app.on("profile", () => { refresh(); });     // another profile selected / made active: show that profile's lights
  refresh();
  return {
    destroy() {
      off();
      offProfile();
      token++;
      link?.dispose();
      sceneUi?.panels.destroy();
      sceneUi?.keeper.dispose();     // sends a live write that was only waiting for the person to stop
      view?.destroy();
    },
  };
}
