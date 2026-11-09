// Card that holds the keyboard canvas plus the quick-selection toolbar. Shared by every keyboard page.
import { h } from "../dom.js";
import { t } from "../i18n.js";
import { KeyboardView } from "./keyboard.js";
import { allSlots } from "../core/layout.js";

export function keyboardCard(app, opts = {}) {
  const { legend = null, extra = null, ...viewOpts } = opts;
  const view = new KeyboardView(app, viewOpts);
  const count = h("span.num.sel-count");
  const wasd = app.layout.keys.filter((k) => ["W", "A", "S", "D"].includes(k.name)).map((k) => k.slot);
  const btn = (label, fn, title) => h("button.chip", { type: "button", title, on: { click: () => { fn(); view.el.focus({ preventScroll: true }); } } }, label);

  const toolbar = h("div.kb-toolbar", {},
    h("div.kb-chips", { role: "toolbar", "aria-label": t("kb.toolbar") },
      btn(t("kb.all"), () => app.select(allSlots(app.layout)), "Ctrl+A"),
      btn(t("kb.none"), () => app.select([]), "Esc"),
      btn(t("kb.invert"), () => app.select(allSlots(app.layout).filter((s) => !app.selection.has(s)))),
      btn("WASD", () => app.select(wasd)),
    ),
    count,
  );
  const syncCount = () => { count.textContent = t("kb.selected", { n: app.selection.size }); };
  syncCount();
  const off = app.on("selection", syncCount);

  const hint = h("p.kb-hint", {}, t("kb.hint"));
  const el = h("section.card.kb-card", { "aria-label": t("kb.title") }, toolbar, view.el, legend, extra, hint);
  return { el, view, destroy() { off(); view.destroy(); } };
}
