// Tiny DOM helpers: hyperscript-style element builder, inline icons, toasts.
const SVG_NS = "http://www.w3.org/2000/svg";

const injectedCss = new Set();

/** Add a page's CSS text to the document once, under a key. Uses a constructed stylesheet (adoptedStyleSheets): unlike a <style> element
 *  it is allowed by a strict Content-Security-Policy (style-src 'self'), which the hosted site sets. Falls back to a <style> element. */
export function injectCss(key, css, doc = document) {
  if (injectedCss.has(key)) return;
  try {
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(css);
    doc.adoptedStyleSheets = [...doc.adoptedStyleSheets, sheet];
  } catch {
    if (doc.getElementById(key)) { injectedCss.add(key); return; }
    const el = doc.createElement("style");
    el.id = key;
    el.textContent = css;
    doc.head.append(el);
  }
  injectedCss.add(key);
}

/** h("div.card#id", {class, on: {click}, aria-*, data-*}, ...children). Strings/numbers become text nodes. */
export function h(tag, props = {}, ...children) {
  const [name, ...classes] = tag.split(".");
  const [tagName, id] = name.split("#");
  const el = document.createElement(tagName || "div");
  if (id) el.id = id;
  if (classes.length) el.className = classes.join(" ");
  applyProps(el, props);
  append(el, children);
  return el;
}

export function s(tag, props = {}, ...children) {
  const el = document.createElementNS(SVG_NS, tag);
  applyProps(el, props);
  append(el, children);
  return el;
}

function applyProps(el, props) {
  for (const [k, v] of Object.entries(props ?? {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "on") for (const [ev, fn] of Object.entries(v)) el.addEventListener(ev, fn);
    else if (k === "class") el.setAttribute("class", [el.getAttribute("class"), v].filter(Boolean).join(" "));
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else if (k === "value" || k === "checked" || k === "disabled" || k === "selected") el[k] = v;
    else el.setAttribute(k, v === true ? "" : v);
  }
}

function append(el, children) {
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };

// Original 24x24 line icons (stroke 1.75, round caps).
const ICONS = {
  live: "M3 12h3.5l2.5-7 4 14 2.5-7H21",
  performance: "M4 7h9M17 7h3M4 17h3M11 17h9M13 4v6M7 14v6",
  remap: "M5 6h14a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1zM8 19h8M9 10.5h.01M12 10.5h.01M15 10.5h.01",
  behaviors: "M4 18h4V6H4zM10 18h4V10h-4zM16 18h4V3h-4z",
  pad: "M7 8h10a5 5 0 0 1 4.9 4l.9 4.6a2.4 2.4 0 0 1-4.2 1.9L17 16H7l-1.6 2.5a2.4 2.4 0 0 1-4.2-1.9l.9-4.6A5 5 0 0 1 7 8zM8 10.5v3M6.5 12h3M16 11.6h.01M18 13.4h.01",
  lighting: "M12 3a6 6 0 0 0-3.5 10.9V16h7v-2.1A6 6 0 0 0 12 3zM9.5 19.5h5M10.5 22h3",
  animation: "M4 5h16v11H4zM8 20h8M10 8.5v4.5l4-2.25z",
  play: "M7 4.5v15l12-7.5z",
  pause: "M8 5v14M16 5v14",
  stop: "M6 6h12v12H6z",
  profiles: "M12 4 3 8.5 12 13l9-4.5L12 4zM3 12.5 12 17l9-4.5M3 16.5 12 21l9-4.5",
  device: "M8 3v4M16 3v4M6 7h12v5a6 6 0 0 1-12 0V7zM12 18v3",
  sun: "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2.5v2M12 19.5v2M4.6 4.6 6 6M18 18l1.4 1.4M2.5 12h2M19.5 12h2M4.6 19.4 6 18M18 6l1.4-1.4",
  moon: "M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z",
  check: "m5 12.5 4.5 4.5L19 7.5",
  plus: "M12 5v14M5 12h14",
  copy: "M9 9h10v10H9zM5 15V5h10",
  download: "M12 4v11m0 0 4-4m-4 4-4-4M5 19h14",
  upload: "M12 16V5m0 0-4 4m4-4 4 4M5 19h14",
  trash: "M5 7h14M10 7V4h4v3M7 7l1 13h8l1-13",
  search: "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-4-4",
  lock: "M7 11V8a5 5 0 0 1 10 0v3M6 11h12v9H6z",
  info: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v5M12 8h.01",
  warn: "M12 3 2.5 20h19L12 3zM12 10v5M12 17.5h.01",
  x: "M6 6l12 12M18 6 6 18",
  usb: "M12 3v14M12 17a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM12 9l4-2.5V5M12 12 8 9.5V8M18 5h-4v2h4zM8 6.5a1.5 1.5 0 1 0 0 3",
  edit: "M4 20h4L19 9l-4-4L4 16v4zM13 7l4 4",
  link: "M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3A4 4 0 0 0 11 18.7l1-1",
  reset: "M4 12a8 8 0 1 0 2.5-5.8M4 4v5h5",
  globe: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18",
};

export function icon(name, extra = {}) {
  return s("svg", { viewBox: "0 0 24 24", fill: "none", stroke: "currentColor", "stroke-width": "1.75", "stroke-linecap": "round", "stroke-linejoin": "round", "aria-hidden": "true", ...extra }, s("path", { d: ICONS[name] ?? "" }));
}

export function toast(message, kind = "info") {
  let host = document.querySelector(".toasts");
  if (!host) document.body.append((host = h("div.toasts", { role: "status", "aria-live": "polite" })));
  const el = h("div.toast", { class: kind === "error" ? "error" : "" }, message);
  host.append(el);
  setTimeout(() => { el.classList.add("out"); setTimeout(() => el.remove(), 260); }, kind === "error" ? 6000 : 2800);
}

export function download(filename, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = h("a", { href: url, download: filename });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
