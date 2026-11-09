// Reusable form controls. Each returns {el, set(...)} so pages can build once and sync values later.
import { h } from "../dom.js";
import { t } from "../i18n.js";

const snap = (v, step) => Math.round(v / step) * step;
const decimals = (step) => (String(step).split(".")[1] ?? "").length;

/** Slider with a linked number box. set(null) shows a "mixed" state (selection has different values). */
export function rangeField({ label, hint, min, max, step = 0.01, unit = "mm", onInput, onClamp, id }) {
  const dec = decimals(step);
  const range = h("input", { type: "range", min, max, step, id: id ? `${id}-r` : undefined, "aria-label": label });
  const num = h("input", { type: "number", min, max, step, inputmode: "decimal", id: id ? `${id}-n` : undefined, "aria-label": `${label} (${unit})` });
  const wrap = h("div.slider", {}, range, h("div.num-input", {}, num, unit ? h("span", {}, unit) : null));
  const hintEl = hint ? h("div.field-hint", {}, hint) : null;
  const labelEl = h("label.field-label", { for: id ? `${id}-r` : undefined }, label);
  const el = h("div.field", {}, h("div.field-row", {}, labelEl), wrap, hintEl);
  let value = null;

  const paint = () => {
    const pct = value === null ? 0 : ((value - min) / (max - min)) * 100;
    range.style.setProperty("--pct", `${Math.max(0, Math.min(100, pct))}%`);
  };
  const commit = (raw) => {
    const asked = snap(Number(raw), step);
    const v = Math.min(max, Math.max(min, asked));
    if (!Number.isFinite(v)) return;
    if (asked !== v) onClamp?.(asked, v, asked < min ? "below" : "above"); // typed (or snapped) outside the allowed range: tell the caller why it moved
    value = Number(v.toFixed(dec));
    range.value = value;
    paint();
    onInput?.(value);
  };
  range.addEventListener("input", () => { commit(range.value); num.value = value.toFixed(dec); wrap.classList.remove("mixed"); });
  num.addEventListener("change", () => { if (num.value !== "") { commit(num.value); num.value = value.toFixed(dec); wrap.classList.remove("mixed"); } });
  num.addEventListener("keydown", (e) => { if (e.key === "Enter") num.blur(); });

  return {
    el,
    set(v) {
      if (v === null) {
        wrap.classList.add("mixed");
        value = null;
        num.value = "";
        num.placeholder = "—";
        paint();
        return;
      }
      wrap.classList.remove("mixed");
      value = v;
      range.value = v;
      if (document.activeElement !== num) num.value = v.toFixed(dec);
      paint();
    },
    setDisabled(d) { range.disabled = num.disabled = d; el.classList.toggle("disabled", d); },
    show(visible) { el.hidden = !visible; },
    /** Mark the field as invalid / explain it: text under the field, red outline. Empty text clears it. */
    flag(text) {
      let box = el.querySelector(":scope > .field-flag");
      if (!text) { box?.remove(); el.classList.remove("flagged"); return; }
      if (!box) { box = h("div.field-flag", { role: "status" }); el.append(box); }
      box.textContent = text;
      el.classList.add("flagged");
    },
  };
}

/** options: [{id, label, title?}]. onChange(id). */
export function segmented({ options, value, onChange, label, accent = false, block = false }) {
  const el = h("div.seg", { role: "radiogroup", "aria-label": label, class: `${accent ? "accent" : ""} ${block ? "block" : ""}` });
  const buttons = options.map((o) => h("button", {
    type: "button", role: "radio", title: o.title, "data-id": o.id,
    on: { click: () => onChange(o.id) },
  }, o.label));
  el.append(...buttons);
  const set = (v) => buttons.forEach((b) => b.setAttribute("aria-checked", String(b.dataset.id === String(v))));
  set(value);
  return { el, set };
}

export function toggle({ label, checked = false, onChange }) {
  const input = h("input", { type: "checkbox", checked, on: { change: () => onChange(input.checked) } });
  const el = h("label.toggle", {}, input, h("span.track"), h("span", {}, label));
  return { el, set(v) { input.checked = v; }, setDisabled(d) { input.disabled = d; } };
}

export function mixedNote() {
  return h("span.chip.warn", {}, t("common.mixed"));
}
