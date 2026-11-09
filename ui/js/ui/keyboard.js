// The keyboard canvas: SVG keys, multi-select (click, shift/ctrl, drag box, row/column tabs, arrow keys),
// and per-mode key content (live depth fill, trigger tick, bound action, lighting colour).
import { h, s } from "../dom.js";
import { t } from "../i18n.js";
import { neighbour, slotsInRect, rowSlots, columnSlots, allSlots } from "../core/layout.js";
import { MAX_TRAVEL_RAW, rawToMm, mmToRaw } from "../core/units.js";
import { capLabel } from "../core/hid-usages.js";
import { hexToRgb } from "../core/lighting.js";

const U = 60;          // one key unit in SVG px
const GAP = 3;         // visual gap between keys
const ML = 26, MT = 24, PAD = 6;

/** mode: "depth" (live + performance) | "remap" | "lighting" */
export class KeyboardView {
  constructor(app, { mode = "depth", showTicks = true, colorFn = null } = {}) {
    this.app = app;
    this.mode = mode;
    this.showTicks = showTicks;
    this.colorFn = colorFn; // (key, tSeconds) -> [r,g,b], lighting preview
    this.keyEls = new Map();
    this.focusSlot = app.primary ?? app.layout.keys[0].slot;
    this.lastDepth = new Int16Array(76).fill(-1);
    this.lastPressed = new Int8Array(76).fill(-1);
    this.hover = null;
    this.running = false;
    this.#build();
    this.offs = [
      app.on("selection", () => this.#syncSelection()),
      app.on("profile", () => this.refresh()),
    ];
    this.refresh();
    this.start();
  }

  #build() {
    const { layout } = this.app;
    const W = ML + layout.width * U + PAD, H = MT + layout.height * U + PAD;
    this.svg = s("svg", { class: "kb-svg", viewBox: `0 0 ${W} ${H}`, role: "presentation", focusable: "false" });
    const defs = s("defs");
    this.svg.append(defs);

    // row and column tabs
    const tabs = s("g", { class: "kb-tabs" });
    for (let c = 0; c < Math.ceil(layout.width); c++) {
      tabs.append(this.#tab(ML + c * U + GAP, 4, U - GAP * 2, 14, String(c + 1), () => columnSlots(layout, c), t("kb.col", { n: c + 1 })));
    }
    for (let r = 0; r < layout.rows; r++) {
      tabs.append(this.#tab(2, MT + r * U + GAP, 16, U - GAP * 2, String(r + 1), () => rowSlots(layout, r), t("kb.row", { n: r + 1 }), true));
    }
    this.svg.append(tabs);

    const keysG = s("g", { class: "kb-keys" });
    for (const key of layout.keys) keysG.append(this.#key(key, defs));
    this.svg.append(keysG);
    this.marquee = s("rect", { class: "kb-marquee", hidden: true });
    this.svg.append(this.marquee);

    this.tip = h("div.kb-tip", { hidden: true, "aria-hidden": "true" });
    this.live = h("div.sr-only", { "aria-live": "polite" });
    this.el = h("div.kb", {
      tabindex: "0", role: "group", "aria-label": t("kb.aria"),
      on: { keydown: (e) => this.#onKey(e), focus: () => this.el.classList.add("focused"), blur: () => this.el.classList.remove("focused") },
    }, h("div.kb-scroll", {}, this.svg), this.tip, this.live);

    this.svg.addEventListener("pointerdown", (e) => this.#down(e));
    this.svg.addEventListener("pointermove", (e) => this.#move(e));
    this.svg.addEventListener("pointerup", (e) => this.#up(e));
    this.svg.addEventListener("pointercancel", () => this.#cancel());
    this.svg.addEventListener("pointerleave", () => { this.hover = null; this.tip.hidden = true; });
  }

  #tab(x, y, w, hgt, text, getSlots, label, vertical = false) {
    const g = s("g", { class: "kb-tab", "data-tab": "1" },
      s("title", {}, label),
      s("rect", { x, y, width: w, height: hgt, rx: 4 }),
      s("text", { x: x + w / 2, y: y + hgt / 2, "text-anchor": "middle", "dominant-baseline": "central" }, text));
    g.addEventListener("click", (e) => {
      this.app.select(getSlots(), e.shiftKey || e.ctrlKey || e.metaKey ? "add" : "replace");
      this.el.focus({ preventScroll: true });
    });
    return g;
  }

  #key(key, defs) {
    const w = key.w * U - GAP * 2, hh = key.h * U - GAP * 2;
    const g = s("g", { class: "key", "data-slot": key.slot, transform: `translate(${ML + key.x * U + GAP} ${MT + key.y * U + GAP})` });
    const id = `kc${key.slot}`;
    defs.append(s("clipPath", { id }, s("rect", { width: w, height: hh, rx: 9 })));
    const cap = s("rect", { class: "cap", width: w, height: hh, rx: 9 });
    const fill = s("rect", { class: "fill", width: w, height: 0, y: hh, "clip-path": `url(#${id})` });
    const tick = s("path", { class: "tick", d: "" });
    const main = s("text", { class: "lbl", x: w / 2, y: hh / 2 + 1, "text-anchor": "middle", "dominant-baseline": "central" });
    const alt = s("text", { class: "alt", x: 8, y: 14 });
    const ring = s("rect", { class: "ring", x: 1, y: 1, width: w - 2, height: hh - 2, rx: 8 });
    g.append(cap, fill, tick, alt, main, ring);
    this.keyEls.set(key.slot, { g, cap, fill, tick, main, alt, w, hh, key });
    return g;
  }

  /** Re-read labels, colours, trigger ticks and selection from the profile. */
  refresh() {
    const p = this.app.profile;
    for (const [slot, k] of this.keyEls) {
      const { key } = k;
      const data = p.keys[key.id];
      let main = key.label.main;
      const alt = key.label.alt;
      if (this.mode === "remap") {
        const b = data.bindings[this.app.layer];
        main = b ? capLabel(b) : "";
        k.g.classList.toggle("transparent", !b && this.app.layer > 0);
        if (!b) main = this.app.layer > 0 ? "▽" : "–"; // transparent on an overlay layer; on the base layer null means no key
      }
      k.main.textContent = main;
      k.alt.textContent = this.mode === "remap" ? "" : alt;
      k.main.setAttribute("class", "lbl " + (main.length > 6 ? "xs" : main.length > 3 ? "sm" : ""));
      if (this.mode === "lighting") {
        const rgb = hexToRgb(data.color) ?? [0, 0, 0];
        this.#paintCap(k, rgb);
      } else {
        k.cap.style.fill = "";
        k.main.style.fill = "";
        k.alt.style.fill = "";
      }
      const showTick = this.showTicks && this.mode === "depth";
      k.tick.style.display = showTick ? "" : "none";
      if (showTick) {
        const y = (mmToRaw(data.trigger.actuation) / MAX_TRAVEL_RAW) * k.hh;
        k.tick.setAttribute("d", `M3 ${y}h9M${k.w - 12} ${y}h9`);
      }
      k.fill.style.display = this.mode === "depth" ? "" : "none";
    }
    this.svg.classList.toggle("mode-depth", this.mode === "depth");
    this.svg.classList.toggle("mode-lighting", this.mode === "lighting");
    this.lastDepth.fill(-1);
    this.#syncSelection();
  }

  #paintCap(k, rgb) {
    const [r, g, b] = rgb.map((v) => Math.round(v));
    k.cap.style.fill = `rgb(${r} ${g} ${b})`;
    const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
    const ink = lum > 0.55 ? "rgb(12 16 22 / 0.85)" : "rgb(255 255 255 / 0.88)";
    k.main.style.fill = ink;
    k.alt.style.fill = ink;
  }

  #syncSelection() {
    const sel = this.app.selection;
    if (this.app.primary !== null && sel.has(this.app.primary)) this.focusSlot = this.app.primary;
    for (const [slot, k] of this.keyEls) {
      k.g.classList.toggle("sel", sel.has(slot));
      k.g.classList.toggle("primary", slot === this.app.primary && sel.has(slot));
    }
    this.svg.classList.toggle("multi", sel.size > 1);
    this.live.textContent = t("kb.selected", { n: sel.size });
  }

  // ---- pointer interaction -------------------------------------------------

  #toUnits(e) {
    const pt = this.svg.createSVGPoint();
    pt.x = e.clientX;
    pt.y = e.clientY;
    const p = pt.matrixTransform(this.svg.getScreenCTM().inverse());
    return { x: (p.x - ML) / U, y: (p.y - MT) / U };
  }

  #slotAt(e) {
    const g = e.target.closest?.(".key");
    return g ? Number(g.dataset.slot) : null;
  }

  #down(e) {
    if (e.button !== 0 || e.target.closest?.("[data-tab]")) return;
    const additive = e.shiftKey || e.ctrlKey || e.metaKey;
    this.gesture = { id: e.pointerId, x: e.clientX, y: e.clientY, start: this.#toUnits(e), hit: this.#slotAt(e), additive, base: new Set(this.app.selection), moved: false, touch: e.pointerType === "touch" };
    this.el.focus({ preventScroll: true });
  }

  #move(e) {
    const g = this.gesture;
    if (g && g.id === e.pointerId && !g.touch) {
      if (!g.moved && Math.hypot(e.clientX - g.x, e.clientY - g.y) > 5) {
        g.moved = true;
        try { this.svg.setPointerCapture(e.pointerId); } catch { /* synthetic or already released pointer */ }
        this.marquee.hidden = false;
      }
      if (g.moved) {
        const cur = this.#toUnits(e);
        const rect = { x0: g.start.x, y0: g.start.y, x1: cur.x, y1: cur.y };
        this.marquee.setAttribute("x", ML + Math.min(rect.x0, rect.x1) * U);
        this.marquee.setAttribute("y", MT + Math.min(rect.y0, rect.y1) * U);
        this.marquee.setAttribute("width", Math.abs(rect.x1 - rect.x0) * U);
        this.marquee.setAttribute("height", Math.abs(rect.y1 - rect.y0) * U);
        const slots = slotsInRect(this.app.layout, rect);
        this.app.select(g.additive ? [...g.base, ...slots] : slots, "replace");
        return;
      }
    }
    const slot = this.#slotAt(e);
    if (slot !== this.hover) {
      if (this.hover !== null) this.keyEls.get(this.hover)?.g.classList.remove("hover");
      this.hover = slot;
      if (slot !== null) this.keyEls.get(slot).g.classList.add("hover");
    }
    this.tipPos = { x: e.clientX, y: e.clientY };
    this.#updateTip();
  }

  #up(e) {
    const g = this.gesture;
    this.gesture = null;
    if (!g || g.id !== e.pointerId) return;
    this.marquee.hidden = true;
    if (g.moved) return;
    if (g.hit === null) {
      if (!g.additive) this.app.select([], "replace");
    } else if (g.additive || g.touch) {
      this.app.select(g.hit, "toggle");
    } else {
      this.app.select(g.hit, "replace");
    }
    this.focusSlot = g.hit ?? this.focusSlot;
  }

  #cancel() {
    this.gesture = null;
    this.marquee.hidden = true;
  }

  #updateTip() {
    if (this.hover === null || !this.tipPos) { this.tip.hidden = true; return; }
    const k = this.keyEls.get(this.hover);
    const rect = this.el.getBoundingClientRect();
    const mm = rawToMm(this.app.feed.depth[this.hover]).toFixed(2);
    const live = this.mode === "depth" && this.app.feed.hasData ? ` · ${mm} mm` : "";
    this.tip.textContent = `${k.key.name}${live}`;
    this.tip.hidden = false;
    this.tip.style.left = `${Math.min(rect.width - 140, Math.max(0, this.tipPos.x - rect.left + 12))}px`;
    this.tip.style.top = `${this.tipPos.y - rect.top - 34}px`;
  }

  // ---- keyboard interaction ------------------------------------------------

  #onKey(e) {
    const dirs = { ArrowLeft: "left", ArrowRight: "right", ArrowUp: "up", ArrowDown: "down" };
    const { app } = this;
    if (dirs[e.key]) {
      e.preventDefault();
      this.focusSlot = neighbour(app.layout, this.focusSlot, dirs[e.key]);
      if (e.shiftKey) app.select(this.focusSlot, "add");
      else if (!(e.ctrlKey || e.metaKey)) app.select(this.focusSlot, "replace");
      this.#paintFocus();
    } else if (e.key === " " || e.key === "Enter") {
      e.preventDefault();
      app.select(this.focusSlot, e.ctrlKey || e.metaKey || e.shiftKey ? "toggle" : "replace");
    } else if ((e.key === "a" || e.key === "A") && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      app.select(allSlots(app.layout));
    } else if (e.key === "Escape") {
      app.select([]);
    }
  }

  #paintFocus() {
    for (const [slot, k] of this.keyEls) k.g.classList.toggle("focus", slot === this.focusSlot);
  }

  // ---- animation -------------------------------------------------------------

  start() {
    if (this.running) return;
    this.running = true;
    const loop = (now) => {
      if (!this.running) return;
      this.#frame(now);
      this.raf = requestAnimationFrame(loop);
    };
    this.raf = requestAnimationFrame(loop);
  }

  #frame(now) {
    const feed = this.app.feed;
    if (this.mode === "depth") {
      for (const [slot, k] of this.keyEls) {
        const d = feed.depth[slot];
        const pr = feed.pressed[slot];
        if (d !== this.lastDepth[slot]) {
          this.lastDepth[slot] = d;
          const hgt = Math.min(1, d / MAX_TRAVEL_RAW) * k.hh;
          k.fill.setAttribute("y", k.hh - hgt);
          k.fill.setAttribute("height", hgt);
          k.g.style.setProperty("--heat", (hgt / k.hh).toFixed(3)); // deeper = stronger colour
        }
        if (pr !== this.lastPressed[slot]) {
          this.lastPressed[slot] = pr;
          k.g.classList.toggle("pressed", !!pr);
        }
      }
      if (this.hover !== null) this.#updateTip();
    } else if (this.mode === "lighting" && this.colorFn) {
      const tSec = now / 1000;
      for (const [, k] of this.keyEls) this.#paintCap(k, this.colorFn(k.key, tSec));
    }
  }

  destroy() {
    this.running = false;
    cancelAnimationFrame(this.raf);
    this.offs.forEach((off) => off());
  }
}
