// Modal confirmation built on <dialog>: focus is trapped and returned, Esc cancels. Used before anything that changes the keyboard.
import { h } from "../dom.js";
import { t } from "../i18n.js";

/**
 * Ask before an action. Resolves true (confirmed) or false (cancelled / Esc / click outside).
 * body: string or Node(s). list: optional array of strings shown as bullets. danger: confirm button in the warning colour.
 */
export function confirmDialog({ title, body, list = null, confirmLabel, cancelLabel = t("common.cancel"), danger = false }) {
  return new Promise((resolve) => {
    const previous = document.activeElement;
    const ok = h("button.btn", { type: "button", class: danger ? "danger-solid" : "primary", value: "ok" }, confirmLabel ?? t("common.ok"));
    const cancel = h("button.btn", { type: "button", value: "cancel" }, cancelLabel);
    const dlg = h("dialog.modal", { "aria-labelledby": "dlg-title" },
      h("form", { method: "dialog", class: "modal-body" },
        h("h2#dlg-title", {}, title),
        typeof body === "string" ? h("p", {}, body) : body,
        list ? h("ul", {}, list.map((x) => h("li", {}, x))) : null,
        h("div.modal-actions", {}, cancel, ok)));
    let answer = false;
    const done = () => { dlg.close(); dlg.remove(); previous?.focus?.(); resolve(answer); };
    ok.addEventListener("click", () => { answer = true; done(); });
    cancel.addEventListener("click", done);
    dlg.addEventListener("cancel", (e) => { e.preventDefault(); done(); });          // Esc
    dlg.addEventListener("click", (e) => { if (e.target === dlg) done(); });          // the backdrop
    document.body.append(dlg);
    dlg.showModal();
    (danger ? cancel : ok).focus();                                                   // never default to the dangerous button
  });
}
