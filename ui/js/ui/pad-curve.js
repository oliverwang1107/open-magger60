// Response curve plot: depth (mm) across, output (%) up. The start and the end of the curve are two handles that can be dragged with the
// pointer or moved with the arrow keys; a marker shows the live depth of a key and what the curve makes of it.
import { h, s } from "../dom.js";
import { pt } from "../locales/pad-i18n.js";
import { MM_PER_UNIT, curvePoints, unitsToMm } from "../core/pad.js";
import { curveEval } from "../core/pad_sim.js";

const W = 340, H = 236, L = 42, R = 14, T = 14, B = 36;
const PW = W - L - R, PH = H - T - B;
const MAX_MM = 3.4;
const NOISE_MM = 0.2;
const x = (mm) => L + (mm / MAX_MM) * PW;
const y = (v) => T + (1 - v) * PH;

export class CurvePlot {
  /** onChange(field, units): field "start" | "end", units in 0.05 mm */
  constructor({ onChange }) {
    this.onChange = onChange;
    this.curve = null;
    this.#build();
  }

  #handle(field, label) {
    const g = s("g", { class: "cp-handle", tabindex: "0", role: "slider", "aria-label": label, "aria-orientation": "horizontal" },
      s("circle", { class: "cp-hit", r: 16 }), s("circle", { class: "cp-knob", r: 7 }));
    g.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      g.setPointerCapture(e.pointerId);
      g.classList.add("drag");
      g.focus({ preventScroll: true });
      const move = (ev) => {
        const pt0 = this.svg.createSVGPoint();
        pt0.x = ev.clientX;
        pt0.y = ev.clientY;
        const p = pt0.matrixTransform(this.svg.getScreenCTM().inverse());
        this.onChange(field, Math.round(((p.x - L) / PW) * MAX_MM / MM_PER_UNIT));
      };
      const up = () => { g.classList.remove("drag"); g.removeEventListener("pointermove", move); g.removeEventListener("pointerup", up); g.removeEventListener("pointercancel", up); };
      g.addEventListener("pointermove", move);
      g.addEventListener("pointerup", up);
      g.addEventListener("pointercancel", up);
    });
    g.addEventListener("keydown", (e) => {
      const step = e.shiftKey ? 5 : 1;
      const cur = this.curve?.[field];
      if (cur === undefined) return;
      const move = { ArrowLeft: -step, ArrowDown: -step, ArrowRight: step, ArrowUp: step, PageDown: -10, PageUp: 10 }[e.key];
      if (move === undefined) return;
      e.preventDefault();
      this.onChange(field, cur + move);
    });
    return g;
  }

  #build() {
    const grid = [];
    for (const v of [0, 0.25, 0.5, 0.75, 1]) {
      grid.push(s("line", { class: "cp-grid", x1: L, x2: L + PW, y1: y(v), y2: y(v) }),
        s("text", { class: "cp-tick", x: L - 6, y: y(v), "text-anchor": "end", "dominant-baseline": "central" }, `${Math.round(v * 100)}%`));
    }
    for (const mm of [0, 0.5, 1, 1.5, 2, 2.5, 3, 3.4]) {
      grid.push(s("line", { class: "cp-grid v", x1: x(mm), x2: x(mm), y1: T, y2: T + PH }),
        s("text", { class: "cp-tick", x: x(mm), y: T + PH + 14, "text-anchor": "middle" }, mm === 3.4 ? "3.4" : String(mm)));
    }
    this.noise = s("rect", { class: "cp-noise", x: x(0), y: T, width: x(NOISE_MM) - x(0), height: PH });
    this.area = s("path", { class: "cp-area" });
    this.path = s("path", { class: "cp-line" });
    this.startLine = s("line", { class: "cp-limit", y1: T, y2: T + PH });
    this.endLine = s("line", { class: "cp-limit", y1: T, y2: T + PH });
    this.hStart = this.#handle("start", pt("pad.curve.start"));
    this.hEnd = this.#handle("end", pt("pad.curve.end"));
    this.liveLine = s("line", { class: "cp-live", y1: T, y2: T + PH, style: { display: "none" } });
    this.liveDot = s("circle", { class: "cp-livedot", r: 5, style: { display: "none" } });
    this.liveText = s("text", { class: "cp-livetext", "dominant-baseline": "central", style: { display: "none" } });
    this.svg = s("svg", { class: "cp-svg", viewBox: `0 0 ${W} ${H}`, role: "group", "aria-label": pt("pad.curve.aria") },
      this.noise, ...grid, s("text", { class: "cp-axis", x: L + PW / 2, y: H - 3, "text-anchor": "middle" }, pt("pad.curve.xaxis")),
      this.area, this.startLine, this.endLine, this.path, this.liveLine, this.liveDot, this.liveText, this.hStart, this.hEnd);
    this.el = h("div.cp", {}, this.svg);
  }

  /** curve = { start, end, shape, flags } (0.05 mm units); null hides the plot's content */
  set(curve) {
    this.curve = curve;
    const pts = curvePoints(curve, 85);
    const d = pts.map((p, i) => `${i ? "L" : "M"}${x(p.mm).toFixed(1)} ${y(p.v).toFixed(1)}`).join("");
    this.path.setAttribute("d", d);
    this.area.setAttribute("d", `${d}L${x(MAX_MM).toFixed(1)} ${y(0).toFixed(1)}L${x(0).toFixed(1)} ${y(0).toFixed(1)}Z`);
    const sx = x(curve.start * MM_PER_UNIT);
    const ex = x(curve.end * MM_PER_UNIT);
    this.startLine.setAttribute("x1", sx); this.startLine.setAttribute("x2", sx);
    this.endLine.setAttribute("x1", ex); this.endLine.setAttribute("x2", ex);
    // value 0 starts at `start` and full value is reached at `end`, also for a reversed curve
    this.hStart.setAttribute("transform", `translate(${sx} ${y(0)})`);
    this.hEnd.setAttribute("transform", `translate(${ex} ${y(1)})`);
    for (const [g, field, lo, hi] of [[this.hStart, "start", 4, curve.end - 4], [this.hEnd, "end", curve.start + 4, 68]]) {
      g.setAttribute("aria-valuemin", unitsToMm(lo).toFixed(2));
      g.setAttribute("aria-valuemax", unitsToMm(hi).toFixed(2));
      g.setAttribute("aria-valuenow", unitsToMm(curve[field]).toFixed(2));
      g.setAttribute("aria-valuetext", `${unitsToMm(curve[field]).toFixed(2)} mm`);
    }
  }

  /** depthMm100 = live depth of a key that uses this curve (0.01 mm), or null for no marker */
  setLive(depthMm100) {
    const show = depthMm100 !== null && depthMm100 >= 5 && this.curve !== null;   // nothing to mark while the key rests
    for (const el of [this.liveLine, this.liveDot, this.liveText]) el.style.display = show ? "" : "none";
    if (!show) return;
    const v = curveEval(this.curve, depthMm100) / 32767;
    const px = x(Math.min(MAX_MM, depthMm100 / 100));
    this.liveLine.setAttribute("x1", px); this.liveLine.setAttribute("x2", px);
    this.liveDot.setAttribute("cx", px); this.liveDot.setAttribute("cy", y(v));
    const right = px > L + PW * 0.62;
    this.liveText.setAttribute("x", px + (right ? -9 : 9));
    this.liveText.setAttribute("y", Math.min(T + PH - 10, Math.max(T + 10, y(v))));
    this.liveText.setAttribute("text-anchor", right ? "end" : "start");
    this.liveText.textContent = `${(depthMm100 / 100).toFixed(2)} mm → ${Math.round(v * 100)}%`;
  }
}
