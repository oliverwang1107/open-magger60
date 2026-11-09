// Canvas graph of one key's depth over the last few seconds, with the actuation point and the
// press/release events the trigger model produced. Depth grows downward, like the real key travel.
import { h } from "../dom.js";
import { t } from "../i18n.js";
import { MAX_TRAVEL_MM, rawToMm } from "../core/units.js";
import { MODE } from "../core/trigger.js";

export class DepthGraph {
  constructor(app, { windowMs = 5000 } = {}) {
    this.app = app;
    this.windowMs = windowMs;
    this.slot = null;
    this.canvas = h("canvas.graph", { role: "img", "aria-label": t("live.graph.aria") });
    this.ctx = this.canvas.getContext("2d");
    this.running = true;
    this.colors = null;
    this.colorsAt = 0;
    this.ro = new ResizeObserver(() => this.#resize());
    this.ro.observe(this.canvas);
    const loop = (now) => {
      if (!this.running) return;
      this.draw(now);
      this.raf = requestAnimationFrame(loop);
    };
    this.raf = requestAnimationFrame(loop);
  }

  setSlot(slot) {
    this.slot = slot;
  }

  #resize() {
    const dpr = window.devicePixelRatio || 1;
    const r = this.canvas.getBoundingClientRect();
    this.canvas.width = Math.max(1, Math.round(r.width * dpr));
    this.canvas.height = Math.max(1, Math.round(r.height * dpr));
    this.dpr = dpr;
  }

  #palette() {
    const now = performance.now();
    if (this.colors && now - this.colorsAt < 1000) return this.colors;
    const cs = getComputedStyle(document.documentElement);
    const g = (n) => cs.getPropertyValue(n).trim();
    this.colors = { accent: g("--accent"), line: g("--line"), lineStrong: g("--line-strong"), text: g("--text-dim"), faint: g("--text-faint"), surface: g("--surface"), soft: g("--accent-soft"), warn: g("--warn"), mono: g("--mono") };
    this.colorsAt = now;
    return this.colors;
  }

  draw() {
    const { ctx, canvas } = this;
    if (!canvas.width || !canvas.isConnected) return;
    const dpr = this.dpr || 1;
    const W = canvas.width / dpr, H = canvas.height / dpr;
    const c = this.#palette();
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    const L = 38, R = 10, T = 10, B = 20;
    const pw = W - L - R, ph = H - T - B;
    const yOf = (mm) => T + (mm / MAX_TRAVEL_MM) * ph;
    ctx.font = `11px ${c.mono}`;
    ctx.textBaseline = "middle";

    // grid + y labels (mm)
    ctx.lineWidth = 1;
    for (let mm = 0; mm <= 3; mm += 1) {
      const y = Math.round(yOf(mm)) + 0.5;
      ctx.strokeStyle = c.line;
      ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(W - R, y); ctx.stroke();
      ctx.fillStyle = c.faint;
      ctx.textAlign = "right";
      ctx.fillText(String(mm), L - 8, y);
    }
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let s = 0; s <= this.windowMs / 1000; s++) {
      const x = L + pw - (s / (this.windowMs / 1000)) * pw;
      ctx.fillStyle = c.faint;
      ctx.fillText(s === 0 ? t("live.graph.now") : `−${s}s`, Math.min(W - R - 12, Math.max(L + 8, x)), H - B + 5);
    }

    if (this.slot === null) return;
    const trig = this.app.dataOf(this.slot)?.trigger;
    const now = performance.now();
    const xOf = (tMs) => L + pw - ((now - tMs) / this.windowMs) * pw;

    // dead zones and actuation / release lines
    if (trig) {
      ctx.fillStyle = c.soft;
      if (trig.topDead > 0) ctx.fillRect(L, T, pw, yOf(trig.topDead) - T);
      if (trig.bottomDead > 0) ctx.fillRect(L, yOf(MAX_TRAVEL_MM - trig.bottomDead), pw, yOf(MAX_TRAVEL_MM) - yOf(MAX_TRAVEL_MM - trig.bottomDead));
      this.#hline(ctx, L, W - R, yOf(trig.actuation), c.accent, [6, 4], t("live.graph.act", { v: trig.actuation.toFixed(2) }), c, trig.mode === MODE.NORMAL ? -9 : -9);
    }

    // depth curve
    const win = this.app.feed.buffer.window(this.slot, now, this.windowMs + 200);
    if (win.t.length > 1) {
      const grad = ctx.createLinearGradient(0, T, 0, T + ph);
      grad.addColorStop(0, "transparent");
      grad.addColorStop(1, c.soft);
      ctx.beginPath();
      ctx.moveTo(xOf(win.t[0]), yOf(0));
      for (let i = 0; i < win.t.length; i++) ctx.lineTo(xOf(win.t[i]), yOf(rawToMm(win.v[i])));
      ctx.lineTo(xOf(win.t.at(-1)), yOf(0));
      ctx.closePath();
      ctx.fillStyle = grad;
      ctx.fill();
      ctx.beginPath();
      for (let i = 0; i < win.t.length; i++) {
        const x = xOf(win.t[i]), y = yOf(rawToMm(win.v[i]));
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      }
      ctx.strokeStyle = c.accent;
      ctx.lineWidth = 2;
      ctx.lineJoin = "round";
      ctx.stroke();
    }

    // trigger events: filled dot = press, ring = release
    for (const ev of this.app.feed.eventsFor(this.slot, now, this.windowMs)) {
      const x = xOf(ev.t), y = yOf(rawToMm(ev.depth));
      ctx.beginPath();
      ctx.arc(x, y, 5, 0, Math.PI * 2);
      if (ev.type === "press") { ctx.fillStyle = c.accent; ctx.fill(); }
      else { ctx.fillStyle = c.surface; ctx.fill(); ctx.strokeStyle = c.accent; ctx.lineWidth = 2; ctx.stroke(); }
    }
  }

  #hline(ctx, x0, x1, y, color, dash, label, c, dy) {
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.25;
    ctx.setLineDash(dash);
    ctx.beginPath(); ctx.moveTo(x0, Math.round(y) + 0.5); ctx.lineTo(x1, Math.round(y) + 0.5); ctx.stroke();
    ctx.restore();
    ctx.fillStyle = color;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    ctx.fillText(label, x1 - 4, y + dy);
  }

  destroy() {
    this.running = false;
    cancelAnimationFrame(this.raf);
    this.ro.disconnect();
  }
}
