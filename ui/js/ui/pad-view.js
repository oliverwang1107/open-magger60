// The virtual gamepad: a drawing of a controller that shows a gamepad report (sticks, triggers, 16 buttons, d-pad) as it changes.
// update(report, { circular }) only touches the parts of the drawing whose value changed.
import { h, s } from "../dom.js";
import { pt } from "../locales/pad-i18n.js";
import { reportHex } from "../core/pad_sim.js";

const STICK_R = 34;      // drawing radius that full deflection reaches
const FACE = [[3, "X", -26, 0], [4, "Y", 0, -26], [2, "B", 26, 0], [1, "A", 0, 26]];   // [button, cap, dx, dy] around the face cluster centre

const pct = (v) => `${Math.round((v / 32767) * 100)}`;

export class VirtualPad {
  constructor() {
    this.last = "";
    this.stick = {};
    this.btn = new Map();
    this.hat = [];
    this.trig = {};
    this.readout = {};
    this.#build();
  }

  #build() {
    const stickWell = (id, cx, cy) => {
      const square = s("rect", { class: "vp-square", x: cx - STICK_R, y: cy - STICK_R, width: STICK_R * 2, height: STICK_R * 2, rx: 3 });
      const well = s("circle", { class: "vp-well", cx, cy, r: STICK_R });
      const dead = s("circle", { class: "vp-dead", cx, cy, r: 0 });
      const dot = s("circle", { class: "vp-dot", cx, cy, r: 8 });
      this.stick[id] = { cx, cy, square, dead, dot };
      return s("g", {}, square, well, dead, dot);
    };
    const button = (n, cx, cy, text, r = 11) => {
      const c = s("circle", { class: "vp-btn", cx, cy, r });
      const t = s("text", { class: "vp-cap", x: cx, y: cy, "text-anchor": "middle", "dominant-baseline": "central" }, text);
      this.btn.set(n, c);
      return s("g", {}, c, t);
    };
    const bar = (id, x, y, w, label) => {
      const back = s("rect", { class: "vp-trig-back", x, y, width: w, height: 12, rx: 4 });
      const fill = s("rect", { class: "vp-trig-fill", x, y, width: 0, height: 12, rx: 4 });
      this.trig[id] = { fill, w };
      return s("g", {}, back, fill, s("text", { class: "vp-cap sm", x: x + w / 2, y: y + 6, "text-anchor": "middle", "dominant-baseline": "central" }, label));
    };
    const bumper = (n, x, label) => {
      const r = s("rect", { class: "vp-btn", x, y: 36, width: 80, height: 12, rx: 6 });
      this.btn.set(n, r);
      return s("g", {}, r, s("text", { class: "vp-cap sm", x: x + 40, y: 42, "text-anchor": "middle", "dominant-baseline": "central" }, label));
    };

    const dpad = s("g", { class: "vp-dpad" });
    const dcx = 150, dcy = 152;
    const arm = (rot, hats) => {
      const p = s("path", { class: "vp-hat", d: "M-9 -9 L-9 -30 L9 -30 L9 -9 Z", transform: `translate(${dcx} ${dcy}) rotate(${rot})` });
      this.hat.push({ p, hats });
      return p;
    };
    dpad.append(arm(0, [8, 1, 2]), arm(90, [2, 3, 4]), arm(180, [4, 5, 6]), arm(270, [6, 7, 8]),
      s("rect", { class: "vp-hat-mid", x: dcx - 9, y: dcy - 9, width: 18, height: 18 }));

    const extra = [12, 13, 14, 15, 16].map((n, i) => button(n, 164 + i * 18, 224, String(n), 8));
    this.svg = s("svg", { class: "vp-svg", viewBox: "0 0 400 240", role: "img", "aria-label": pt("pad.vp.aria") },
      s("path", { class: "vp-body", d: "M82 56 Q200 28 318 56 Q372 74 378 140 Q382 196 338 200 Q300 204 278 178 L122 178 Q100 204 62 200 Q18 196 22 140 Q28 74 82 56 Z" }),
      bar("lt", 70, 14, 80, "LT"), bar("rt", 250, 14, 80, "RT"),
      bumper(5, 70, "LB"), bumper(6, 250, "RB"),
      stickWell("left", 105, 100), dpad,
      stickWell("right", 250, 152),
      ...FACE.map(([n, text, dx, dy]) => button(n, 312 + dx, 100 + dy, text)),
      button(7, 175, 84, "‹", 8), button(11, 200, 68, "◎", 9), button(8, 225, 84, "›", 8),
      button(9, 105, 148, "L3", 10), button(10, 250, 100, "R3", 10),
      ...extra);

    const cell = (key, label) => {
      const v = h("span.num");
      this.readout[key] = v;
      return h("div.vp-val", {}, h("span.vp-val-l", {}, label), v);
    };
    this.el = h("div.vp", {}, this.svg,
      h("div.vp-vals", { role: "group", "aria-label": pt("pad.vp.values") }, cell("lx", "LX"), cell("ly", "LY"), cell("rx", "RX"), cell("ry", "RY"), cell("lt", "LT"), cell("rt", "RT")));
  }

  /** report = { buttons, hat, lx, ly, rx, ry, lt, rt }; opts = { circular, deadzone } (the drawn limits of a stick) */
  update(report, { circular = true, deadzone = 0 } = {}) {
    const key = `${reportHex(report)}|${circular}|${deadzone}`;
    if (key === this.last) return;
    this.last = key;
    for (const [id, [x, y]] of Object.entries({ left: [report.lx, report.ly], right: [report.rx, report.ry] })) {
      const st = this.stick[id];
      st.dot.setAttribute("cx", st.cx + (x / 32767) * STICK_R);
      st.dot.setAttribute("cy", st.cy + (y / 32767) * STICK_R);
      st.dot.classList.toggle("on", x !== 0 || y !== 0);
      st.square.style.display = circular ? "none" : "";
      st.dead.setAttribute("r", (deadzone / 100) * STICK_R);
    }
    for (const [n, el] of this.btn) el.classList.toggle("on", (report.buttons & (1 << (n - 1))) !== 0);
    for (const { p, hats } of this.hat) p.classList.toggle("on", hats.includes(report.hat));
    this.trig.lt.fill.setAttribute("width", (report.lt / 32767) * this.trig.lt.w);
    this.trig.rt.fill.setAttribute("width", (report.rt / 32767) * this.trig.rt.w);
    this.readout.lx.textContent = `${pct(report.lx)}%`;
    this.readout.ly.textContent = `${pct(report.ly)}%`;
    this.readout.rx.textContent = `${pct(report.rx)}%`;
    this.readout.ry.textContent = `${pct(report.ry)}%`;
    this.readout.lt.textContent = `${pct(report.lt)}%`;
    this.readout.rt.textContent = `${pct(report.rt)}%`;
  }
}
