// Live page: per-key analog depth on the keyboard, and a graph + readout for the selected key.
import { h, icon } from "../dom.js";
import { t } from "../i18n.js";
import { keyboardCard } from "../ui/kb-card.js";
import { DepthGraph } from "../ui/graph.js";
import { toggle } from "../ui/controls.js";
import { rawToMm, MAX_TRAVEL_RAW } from "../core/units.js";

export function mount(root, app) {
  const card = keyboardCard(app, {
    mode: "depth",
    legend: h("div.legend", {},
      h("span.lg", {}, h("i.sw.sw-depth"), t("live.legend.depth")),
      h("span.lg", {}, h("i.sw.sw-press"), t("live.legend.pressed")),
      h("span.lg", {}, h("i.sw.sw-tick"), t("live.legend.act")),
    ),
  });

  // ---- selected key panel
  const graph = new DepthGraph(app);
  const name = h("h2.card-title");
  const extra = h("span.chip");
  const big = h("div.big-num.num");
  const state = h("span.chip");
  const stat = (label) => { const v = h("div.stat-v.num"); return { v, el: h("div.stat", {}, h("div.stat-l", {}, label), v) }; };
  const sPct = stat(t("live.stat.travel"));
  const sPeak = stat(t("live.stat.peak"));
  const sRate = stat(t("live.stat.rate"));
  const sSrc = stat(t("live.stat.source"));

  const pauseBtn = h("button.btn.sm", { type: "button", on: { click: () => { app.feed.paused = !app.feed.paused; syncControls(); } } });
  const peakBtn = h("button.btn.sm.ghost", { type: "button", on: { click: () => app.feed.resetPeaks() } }, icon("reset"), t("live.resetPeak"));
  const demo = toggle({ label: t("live.demo"), checked: app.mock.demo, onChange: (v) => (app.mock.demo = v) });

  const empty = h("div.empty", {}, icon("live"), h("h3", {}, t("live.nodata.title")), h("p", {}, t("live.nodata.body")));
  const body = h("div.live-body", {},
    h("div.live-side", {},
      h("div.live-readout", {}, h("div", {}, big, h("div.stat-l", {}, t("live.stat.depth"))), state),
      h("div.stats", {}, sPct.el, sPeak.el, sRate.el, sSrc.el)),
    h("div.graph-wrap", {}, graph.canvas),
  );
  const foot = h("div.live-foot", {}, demo.el);
  const panel = h("section.card.live-panel", {},
    h("div.card-head", {}, h("div.row", {}, name, extra), h("div.row", {}, peakBtn, pauseBtn)),
    body, empty,
    foot,
  );

  function syncControls() {
    pauseBtn.textContent = app.feed.paused ? t("live.resume") : t("live.pause");
    demo.el.hidden = app.device.kind !== "mock";
    foot.hidden = demo.el.hidden;
  }

  function syncSelection() {
    const slot = app.primary;
    graph.setSlot(slot);
    const k = slot === null ? null : app.layout.bySlot.get(slot);
    name.textContent = k ? `${t("live.key")} ${k.name}` : t("live.nokey");
    extra.hidden = app.selection.size < 2;
    extra.textContent = `+${app.selection.size - 1}`;
  }

  let raf = 0;
  const tickReadout = () => {
    const feed = app.feed;
    const slot = app.primary;
    const has = app.device.capabilities.live || feed.hasData;
    body.hidden = !has;
    empty.hidden = has;
    if (has && slot !== null) {
      const d = feed.depth[slot];
      big.textContent = `${rawToMm(d).toFixed(2)} mm`;
      const pressed = feed.pressed[slot] === 1;
      state.textContent = pressed ? t("live.state.pressed") : t("live.state.idle");
      state.className = "chip" + (pressed ? " on" : "");
      sPct.v.textContent = `${Math.round((d / MAX_TRAVEL_RAW) * 100)}%`;
      sPeak.v.textContent = `${rawToMm(feed.peak[slot]).toFixed(2)} mm`;
      sRate.v.textContent = feed.fps ? `${feed.fps} Hz` : "—";
    }
    sSrc.v.textContent = app.device.kind === "mock" ? t("live.src.mock") : t("live.src.none");
    raf = requestAnimationFrame(tickReadout);
  };

  const offs = [app.on("selection", syncSelection), app.on("device", syncControls)];
  syncControls();
  syncSelection();
  tickReadout();

  root.append(h("div.page-grid.live", {}, card.el, panel));
  return {
    destroy() {
      cancelAnimationFrame(raf);
      offs.forEach((o) => o());
      graph.destroy();
      card.destroy();
    },
  };
}
