// The bar under the top bar that says what is true about the settings right now: only in this browser, unsaved, being written,
// saved and verified, or failed (and why). Save is the ONE place the configurator writes settings to the keyboard.
import { h, icon, toast } from "../dom.js";
import { t } from "../i18n.js";
import { confirmDialog } from "./dialog.js";
import { classifyProblem } from "../device/errors.js";

const STEPS = ["encode", "stage", "commit", "verify"];

export function syncBar(app) {
  const el = h("div.sync-bar", { role: "status", "aria-live": "polite" });
  let resultUntil = 0;
  let resultTimer = 0;

  async function save() {
    const s = app.sync;
    const ok = await confirmDialog({
      title: t("save.confirm.title"),
      body: t("save.confirm.body"),
      list: [t("save.confirm.l1"), t("save.confirm.l2"), t("save.confirm.l3"), ...(s.converted ? [t("save.confirm.l4")] : [])],
      confirmLabel: t("save.confirm.ok"),
    });
    if (!ok) return;
    try {
      const res = await app.save();
      if (res) {
        resultUntil = Date.now() + 9000;
        clearTimeout(resultTimer);
        resultTimer = setTimeout(paint, 9100);
      }
    } catch (err) {
      if (err.name !== "UnsupportedSettingsError") toast(t(`problem.${classifyProblem(err).kind}.title`), "error");
    }
    paint();
  }

  async function discard() {
    const ok = await confirmDialog({ title: t("discard.title"), body: t("discard.body"), confirmLabel: t("discard.ok"), danger: true });
    if (ok) { await app.pull(); toast(t("discard.done")); }
  }

  function box(kind, ...children) {
    el.className = `sync-bar ${kind}`;
    el.hidden = false;
    el.replaceChildren(...children);
  }

  function paint() {
    const s = app.sync;
    const d = app.device;
    const down = d.status !== "connected";
    if (d.kind === "webhid" && !down) return box("info", icon("lock"), h("div.sb-text", {}, h("strong", {}, t("ro.title")), " ", t("ro.bar")));
    if (s.error) {
      const e = s.error;
      return box("error", icon("warn"), h("div.sb-text", {}, h("strong", {}, t(`problem.${e.kind}.title`)), " ", e.problems ? t("save.unsupported", { n: e.problems.length }) : t(`problem.${e.kind}.body`),
        e.detail ? h("code.detail", {}, e.detail) : null),
      h("button.btn.sm", { type: "button", on: { click: () => { s.error = null; paint(); } } }, t("common.dismiss")));
    }
    if (d.kind !== "mock" && down) return box("warn", icon("usb"), h("div.sb-text", {}, h("strong", {}, t("sync.notConnected")), " ", t("sync.notConnected.body")),
      h("button.btn.sm", { type: "button", on: { click: () => app.setPage("device") } }, t("sync.toDevice")));
    if (s.status === "saving") {
      const idx = Math.max(0, STEPS.indexOf(s.step));
      return box("busy", h("div.spinner", { "aria-hidden": "true" }), h("div.sb-text", {}, h("strong", {}, t("save.writing")), " ", t(`save.step.${s.step ?? "encode"}`)),
        h("ol.steps", {}, STEPS.map((x, i) => h("li", { class: i < idx ? "done" : i === idx ? "now" : "" }, t(`save.step.${x}.short`)))));
    }
    if (s.problems.length) {
      const names = [...new Set(s.problems.map((p) => (p.keyId ? app.layout.byId.get(p.keyId)?.name : null)).filter(Boolean))].slice(0, 6).join(", ");
      return box("warn", icon("warn"), h("div.sb-text", {}, h("strong", {}, t("save.cannot")), " ", t("save.cannot.body", { n: s.problems.length, keys: names })),
        h("button.btn.sm.primary", { type: "button", disabled: true }, icon("download"), t("save.btn")));
    }
    if (s.dirty) {
      return box("dirty", h("i.dot-unsaved", { "aria-hidden": "true" }), h("div.sb-text", {}, h("strong", {}, t("sync.unsaved")), " ", h("span.sb-more", {}, t("sync.unsaved.body"))),
      h("button.btn.sm", { type: "button", on: { click: discard } }, icon("reset"), t("discard.btn")),
      h("button.btn.sm.primary", { type: "button", on: { click: save } }, icon("download"), t("save.btn")));
    }
    if (s.lastSave && Date.now() < resultUntil) {
      return box("ok", icon("check"), h("div.sb-text", {}, h("strong", {}, t("save.done")), " ", h("span.sb-more", {}, t("save.done.body", { bytes: s.lastSave.bytes, n: s.lastSave.commits }))));
    }
    if (s.loadError) {
      return box("error", icon("warn"), h("div.sb-text", {}, h("strong", {}, t("sync.loadFail")), " ", t(`problem.${s.loadError.kind}.body`), s.loadError.detail ? h("code.detail", {}, s.loadError.detail) : null),
        h("button.btn.sm", { type: "button", on: { click: () => app.pull() } }, t("common.retry")));
    }
    el.hidden = true;
    el.className = "sync-bar";
  }

  const offs = [app.on("sync", paint), app.on("device", paint)];
  paint();
  return { el, paint, destroy() { offs.forEach((o) => o()); clearTimeout(resultTimer); } };
}
