// App shell: navigation rail, top bar (device status, profile switcher, language, theme) and page routing.
import { h, s, icon, toast } from "./dom.js";
import { t, LANGS } from "./i18n.js";
import { loadLayout } from "./core/layout.js";
import { MockDevice } from "./device/MockDevice.js";
import { syncBar } from "./ui/sync-bar.js";
import { createApp, PAGES } from "./state.js";
import { profileName } from "./pages/profiles.js";
import "./core/behaviors.js"; // registers the key-cap text of DKS / Mod-Morph / toggle keys
import { APP_NAME, MODEL_NAME } from "./config.js";
import { WebHidDevice } from "./device/WebHidDevice.js";
import { classifyProblem } from "./device/errors.js";
import { createAutoConnect } from "./auto_connect.js";

const PAGE_MODULES = {
  live: () => import("./pages/live.js"),
  performance: () => import("./pages/performance.js"),
  remap: () => import("./pages/remap.js"),
  behaviors: () => import("./pages/behaviors.js"),
  pad: () => import("./pages/pad.js"),
  lighting: () => import("./pages/lighting.js"),
  animation: () => import("./pages/animation.js"),
  profiles: () => import("./pages/profiles.js"),
  device: () => import("./pages/device.js"),
};

const logo = () => s("svg", { class: "logo", viewBox: "0 0 32 32", "aria-hidden": "true" },
  s("rect", { x: 3, y: 3, width: 26, height: 26, rx: 8, fill: "none", stroke: "currentColor", "stroke-width": 2 }),
  s("rect", { x: 9, y: 15, width: 14, height: 8, rx: 3, fill: "var(--accent)" }),
  s("path", { d: "M9 11h14", stroke: "currentColor", "stroke-width": 2, "stroke-linecap": "round" }));

async function start() {
  const root = document.getElementById("app");
  const layout = loadLayout();
  const mock = new MockDevice(layout);
  const app = await createApp({ layout, mock });
  window.app = app; // handy for debugging in the console

  let current = null;
  let token = 0;
  let sync = null;

  // ---- static shell pieces that get rebuilt on language change
  function buildShell() {
    current?.destroy?.();
    current = null;

    const nav = h("nav.rail", { "aria-label": t("nav.aria") },
      h("div.brand", {}, logo(), h("div.brand-text", {}, h("strong", {}, APP_NAME), h("span", {}, MODEL_NAME))),
      h("ul.nav-list", {}, PAGES.map((p) => h("li", {}, h("a.nav-item", { href: `#/${p}`, "data-page": p, on: { click: (e) => { e.preventDefault(); app.setPage(p); } } }, icon(p), h("span", {}, t(`nav.${p}`)))))),
      h("div.rail-foot", {}, h("span", {}, `v0.1 · ${t("nav.preview")}`)));

    const title = h("h1.page-title");
    const statusDot = h("i.dot");
    const statusText = h("span.status-text");
    const connectBtn = h("button.btn.sm.primary.top-connect", { type: "button", hidden: true, title: t("conn.first"), on: { click: connectByHand } }, icon("usb"), t("dev.connectShort"));
    const status = h("button.status", { type: "button", title: t("status.open"), on: { click: () => app.setPage("device") } }, statusDot, statusText, h("span.status-ro", {}, icon("lock"), t("status.ro")));
    const profBtns = app.profiles.map((_, i) => h("button", { type: "button", role: "radio", on: { click: () => app.switchProfile(i) } }, String(i + 1)));
    const profName = h("span.prof-name");
    const profile = h("div.profile-switch", {}, h("div.seg", { role: "radiogroup", "aria-label": t("top.profile") }, profBtns), profName);
    const langSeg = h("div.seg", { role: "radiogroup", "aria-label": t("top.lang") }, LANGS.map((l) => h("button", { type: "button", role: "radio", lang: l.id, title: l.name, "aria-checked": String(app.lang === l.id), on: { click: () => app.setLang(l.id) } }, l.short)));
    const themeBtn = h("button.btn.icon.ghost", { type: "button", title: t("top.theme"), "aria-label": t("top.theme"), on: { click: () => app.setTheme(app.theme === "dark" ? "light" : "dark") } });

    const pill = h("span.sync-pill");
    const topbar = h("header.topbar", {}, title, h("div.top-actions", {}, pill, connectBtn, status, profile, langSeg, themeBtn));
    sync?.destroy();
    sync = syncBar(app);
    const main = h("main#main.main", { tabindex: "-1" });
    root.replaceChildren(
      h("a.skip", { href: "#main", on: { click: (e) => { e.preventDefault(); main.focus(); } } }, t("nav.skip")),
      h("div.app", {}, nav, h("div.content", {}, h("div.topwrap", {}, topbar, sync.el), main)));

    const syncNav = () => {
      nav.querySelectorAll(".nav-item").forEach((a) => {
        if (a.dataset.page === app.page) { a.setAttribute("aria-current", "page"); a.scrollIntoView?.({ inline: "nearest", block: "nearest" }); } else a.removeAttribute("aria-current");
      });
      title.textContent = t(`nav.${app.page}`);
      document.title = `${t(`nav.${app.page}`)} · ${APP_NAME}`;
    };
    const syncStatus = () => {
      const d = app.device;
      let key = d.kind === "mock" ? "mock" : d.status;
      const c = app.conn.state;
      // the demo keyboard is in use while auto-connect is busy or waiting: say what is going on instead of just "demo"
      if (d.kind === "mock" && (c === "connecting" || c === "searching")) key = "connecting";
      statusText.textContent = d.kind === "mock" && c === "waiting" ? t("conn.waiting.short") : t(`status.${key}`);
      status.dataset.state = key;
      status.classList.toggle("ro", d.kind === "webhid");
      syncConnect();
      syncPill();
    };
    const syncConnect = () => {
      const c = app.conn.state;
      connectBtn.hidden = !(app.device.kind === "mock" && WebHidDevice.supported && ["needs_permission", "failed", "waiting", "paused"].includes(c));
    };
    const syncPill = () => {
      const s = app.sync;
      const state = !app.canWrite ? "local" : s.status === "saving" ? "saving" : s.dirty || s.problems.length ? "unsaved" : "saved";
      pill.dataset.state = state;
      pill.textContent = t(`pill.${state}`);
      pill.title = t(`pill.${state}.tip`);
    };
    const syncProfile = () => {
      profBtns.forEach((b, i) => {
        b.setAttribute("aria-checked", String(i === app.active));
        b.classList.toggle("kb-active", i === app.deviceActive);
        b.classList.toggle("unsaved", !!app.sync.profileDirty?.[i]);
        b.title = profileName(app.profiles[i], i) + (i === app.deviceActive ? ` · ${t("prof.active")}` : "");
      });
      profName.textContent = profileName(app.profile, app.active);
    };
    const syncTheme = () => {
      themeBtn.replaceChildren(icon(app.theme === "dark" ? "sun" : "moon"));
    };

    syncNav(); syncStatus(); syncProfile(); syncTheme();
    shell.syncNav = syncNav;
    shell.syncStatus = syncStatus;
    shell.syncConnect = syncConnect;
    shell.syncPill = syncPill;
    shell.syncProfile = syncProfile;
    shell.syncTheme = syncTheme;
    shell.main = main;
    mountPage();
  }

  const shell = {};

  async function mountPage() {
    const my = ++token;
    current?.destroy?.();
    current = null;
    shell.main.replaceChildren();
    shell.syncNav();
    let mod;
    try {
      mod = await PAGE_MODULES[app.page]();
    } catch (err) {
      console.error(err);
      shell.main.append(h("div.banner.error", {}, icon("warn"), h("div", {}, t("err.load"), ` (${err.message})`)));
      return;
    }
    if (my !== token) return;
    const page = h("div.page", { "data-page": app.page });
    shell.main.append(page);
    current = mod.mount(page, app);
  }

  app.on("page", mountPage);
  app.on("device", () => shell.syncStatus());
  app.on("conn", () => shell.syncStatus());

  // The person's own click: the browser shows its chooser (needed once per browser); afterwards the page connects by itself.
  async function connectByHand() {
    try {
      await app.connectReal();
      toast(app.device.kind === "kmfw" ? t("dev.connected") : t("dev.connected.ro"));
    } catch (err) {
      const p = app.deviceProblem ?? classifyProblem(err);
      if (p.kind !== "cancelled") toast(t(`problem.${p.kind}.title`), "error");
    }
  }
  app.on("sync", () => { shell.syncPill(); shell.syncProfile(); });
  app.on("profile", () => shell.syncProfile());
  let shownLang = app.lang;
  app.on("prefs", () => {
    shell.syncTheme();
    if (shownLang !== app.lang) { shownLang = app.lang; buildShell(); }
  });
  window.addEventListener("hashchange", () => { const p = location.hash.slice(2); if (PAGES.includes(p) && p !== app.page) app.setPage(p); });

  buildShell();

  // ---- automatic connection (js/auto_connect.js) ----
  const realConnected = () => app.device.kind !== "mock" && app.device.status === "connected";
  let lastConn = "idle";
  const auto = createAutoConnect({
    hid: WebHidDevice.supported ? navigator.hid : undefined,
    connect: (d) => app.connectHid(d),
    isConnected: realConnected,
    onState: (c) => {
      const prev = lastConn;
      lastConn = c.state;
      app.setConn(c);
      if (c.state === "failed" && prev !== "failed") toast(t("conn.failed"), "error");
      if (c.state === "connected" && prev === "connecting") toast(t("conn.auto"));
    },
  });
  app.auto = auto;
  // a keyboard connected by hand (through the chooser) counts as connected for the state machine
  app.on("device", () => { if (realConnected() && auto.state !== "connected") auto.markConnected(); });
  auto.start();

  // Optional: the firmware's own C core compiled to WebAssembly (ui/wasm, built by tools/build_wasm.py). Nothing depends on it;
  // when it loads it is available as window.kmCore (decode / encode / validate a settings blob, resolve key presses on layers).
  import("../wasm/km_wasm.js").then((m) => m.loadCore()).then((core) => { window.kmCore = core; }).catch(() => {});
}

start().catch((err) => {
  console.error(err);
  document.getElementById("app").textContent = `Failed to start: ${err.message}`;
});
