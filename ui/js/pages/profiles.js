// Profiles page: the keyboard's four profiles. Names are kept in this browser (the settings format has no place for them);
// everything else is what the keyboard stores. "Edit" chooses which profile the other pages show; "Make active" switches which
// one the keyboard uses (stored on the keyboard, asks first).
import { h, icon, toast, download } from "../dom.js";
import { t } from "../i18n.js";
import { confirmDialog } from "../ui/dialog.js";
import { defaultProfile, exportProfileJson, PROFILE_COUNT, PROFILE_VERSION } from "../core/profile.js";
import { MODE } from "../core/trigger.js";
import { effectById } from "../core/lighting.js";
import { bindingFromUsage, sameBinding } from "../core/hid-usages.js";
import { classifyProblem } from "../device/errors.js";

export const profileName = (p, i) => p.name || t("prof.default", { n: i + 1 });

function summary(app, p) {
  const trig = app.layout.keys.map((k) => p.keys[k.id].trigger);
  const rt = trig.filter((x) => x.mode !== MODE.NORMAL).length;
  const avg = trig.reduce((a, x) => a + x.actuation, 0) / trig.length;
  const remapped = app.layout.keys.filter((k) => !sameBinding(p.keys[k.id].bindings[0], bindingFromUsage(k.usage))).length;
  const fn = app.layout.keys.filter((k) => p.keys[k.id].bindings.slice(1).some((b) => b !== null)).length;
  return [
    [t("prof.sum.act"), `${avg.toFixed(2)} mm`],
    [t("prof.sum.rt"), `${rt} / ${trig.length}`],
    [t("prof.sum.remap"), String(remapped)],
    [t("prof.sum.layers"), String(fn)],
  ];
}

export function mount(root, app) {
  const fileInput = h("input", { type: "file", accept: ".json,application/json", hidden: true });
  let importTarget = 0;
  const errorBox = h("div", { hidden: true });
  const armed = new Map(); // two-step confirm for destructive buttons

  fileInput.addEventListener("change", async () => {
    const file = fileInput.files[0];
    fileInput.value = "";
    if (!file) return;
    const r = app.importProfile(importTarget, await file.text());
    if (r.ok) {
      errorBox.hidden = true;
      toast(t("prof.imported", { n: importTarget + 1 }));
    } else showErrors(r.errors);
  });

  function showErrors(errors) {
    const rows = errors.slice(0, 6).map((e) => h("li", {}, h("code", {}, e.path), " — ", t(`err.${e.code}`)));
    if (errors.length > 6) rows.push(h("li", {}, t("prof.more", { n: errors.length - 6 })));
    errorBox.replaceChildren(h("div.banner.error", {}, icon("warn"), h("div", {}, h("strong", {}, t("prof.importFail")), h("ul.errlist", {}, rows))));
    errorBox.hidden = false;
  }

  const grid = h("div.prof-grid");

  function confirmBtn(key, label, onConfirm, cls = "") {
    const b = h("button.btn.sm.ghost", { type: "button", class: cls });
    const paint = () => { b.replaceChildren(...(armed.has(key) ? [t("prof.sure")] : [icon("reset"), label])); b.classList.toggle("danger", armed.has(key)); };
    b.addEventListener("click", () => {
      if (armed.has(key)) { clearTimeout(armed.get(key)); armed.delete(key); paint(); onConfirm(); return; }
      armed.set(key, setTimeout(() => { armed.delete(key); paint(); }, 3000));
      paint();
    });
    paint();
    return b;
  }

  async function makeActive(i) {
    const ok = await confirmDialog({
      title: t("prof.activate.title", { n: i + 1 }),
      body: t("prof.activate.body"),
      confirmLabel: t("prof.activate.ok"),
    });
    if (!ok) return;
    try {
      await app.activateOnKeyboard(i);
      toast(t("prof.activate.done", { n: i + 1 }));
    } catch (err) {
      const p = classifyProblem(err);
      toast(`${t(`problem.${p.kind}.title`)}: ${p.detail}`, "error");
    }
  }

  function render() {
    const canWrite = app.canWrite;
    grid.replaceChildren(...app.profiles.map((p, i) => {
      const editing = i === app.active;
      const live = app.deviceActive === i;
      const unsaved = app.sync.profileDirty?.[i];
      const name = h("input", { type: "text", value: p.name, placeholder: t("prof.default", { n: i + 1 }), maxlength: 32, "aria-label": t("prof.name"),
        on: { change: () => app.edit((pp) => (pp.name = name.value.trim()), i) } });
      const copySel = h("select", { "aria-label": t("prof.copyFrom") },
        h("option", { value: "" }, t("prof.copyFrom")),
        app.profiles.map((q, j) => (j === i ? null : h("option", { value: j }, profileName(q, j)))));
      copySel.addEventListener("change", () => {
        if (copySel.value === "") return;
        app.copyProfile(Number(copySel.value), i);
        toast(t("prof.copied", { a: Number(copySel.value) + 1, b: i + 1 }));
      });
      return h("article.card.prof", { class: `${editing ? "editing" : ""} ${live ? "active" : ""}`, "aria-current": editing ? "true" : undefined },
        h("div.prof-head", {},
          h("span.prof-idx.num", {}, i + 1),
          name),
        h("div.prof-badges", {},
          editing ? h("span.chip.on", {}, icon("edit"), t("prof.editing")) : null,
          live ? h("span.chip.live", {}, icon("check"), t("prof.active")) : null,
          unsaved ? h("span.chip.warn", {}, t("prof.unsaved")) : null),
        h("dl.prof-sum", {}, summary(app, p).map(([k, v]) => [h("dt", {}, k), h("dd.num", {}, v)])),
        h("div.prof-actions", {},
          editing ? null : h("button.btn.sm", { type: "button", on: { click: () => app.switchProfile(i) } }, icon("edit"), t("prof.edit")),
          live ? null : h("button.btn.sm", { type: "button", disabled: !canWrite, title: canWrite ? t("prof.activate.tip") : t("prof.activate.na"), on: { click: () => makeActive(i) } }, icon("usb"), t("prof.activate")),
          h("button.btn.sm", { type: "button", on: { click: () => download(`magger60-${(p.name || "profile-" + (i + 1)).replace(/[^\w一-鿿-]+/g, "_")}.json`, exportProfileJson(p)) } }, icon("download"), t("prof.export")),
          h("button.btn.sm", { type: "button", on: { click: () => { importTarget = i; fileInput.click(); } } }, icon("upload"), t("prof.import")),
          copySel,
          confirmBtn(`reset${i}`, t("prof.reset"), () => { app.replaceProfile(i, defaultProfile(app.layout)); toast(t("prof.resetDone", { n: i + 1 })); })));
    }));
  }

  const note = h("div.banner.info", {}, icon("info"), h("div", {}, t("prof.note", { v: PROFILE_VERSION }), " ", h("code", {}, "ui/README.md")));
  const names = h("div.banner.info", {}, icon("info"), h("div", {}, t("prof.namesNote")));
  const ro = h("div.banner.warn", { hidden: true }, icon("lock"), h("div", {}, h("strong", {}, t("ro.title")), " ", t("prof.ro")));

  const sync = () => { ro.hidden = app.canWrite; render(); };
  const offs = [app.on("profile", render), app.on("device", sync), app.on("sync", render)];
  sync();
  root.append(h("div.page-stack", {},
    h("div.page-intro", {}, h("h2.card-title", {}, t("prof.title")), h("p.card-sub", {}, t("prof.sub", { n: PROFILE_COUNT }))),
    ro, errorBox, grid, names, note, fileInput));
  return { destroy() { offs.forEach((o) => o()); armed.forEach(clearTimeout); } };
}
