// Device page: connection and its problems in plain words, firmware info, what works, engine diagnostics with calibration
// controls, and (when the firmware publishes it) the stability counters.
import { h, icon, toast } from "../dom.js";
import { t } from "../i18n.js";
import { confirmDialog } from "../ui/dialog.js";
import { WebHidDevice } from "../device/WebHidDevice.js";
import { classifyProblem } from "../device/errors.js";
import { VID, PID } from "../device/ids.js";

const hex = (n) => (n === null || n === undefined ? "—" : "0x" + n.toString(16).toUpperCase().padStart(4, "0"));
const POLL_MS = 600;

export function mount(root, app) {
  const body = h("div.page-stack");
  let diag = null, health = null, diagError = null;
  let timer = 0, polling = false;

  function row(label, value, mono = false) {
    return [h("dt", {}, label), h("dd", { class: mono ? "num" : "" }, value ?? "—")];
  }
  function cap(label, ok, note) {
    return h("li.cap", { class: ok ? "ok" : "no" }, icon(ok ? "check" : "x"), h("span", {}, label), note ? h("span.cap-note", {}, note) : null);
  }

  async function connectReal() {
    try {
      await app.connectReal();
      toast(app.device.kind === "kmfw" ? t("dev.connected") : t("dev.connected.ro"));
    } catch (err) {
      if (classifyProblem(err).kind !== "cancelled") toast(`${t(`problem.${(app.deviceProblem ?? classifyProblem(err)).kind}.title`)}`, "error");
      render();
    }
  }

  async function resetDemo() {
    const ok = await confirmDialog({ title: t("dev.resetDemo.title"), body: t("dev.resetDemo.body"), confirmLabel: t("dev.resetDemo.ok"), danger: true });
    if (!ok) return;
    app.device.resetFlash();
    await app.pull();
    toast(t("dev.resetDemo.done"));
  }

  async function requery() {
    try {
      await app.device.getInfo();
      toast(t("dev.queried"));
    } catch (err) {
      toast(`${t("dev.connectFail")}: ${err.message}`, "error");
    }
  }

  async function recalibrate() {
    const ok = await confirmDialog({ title: t("cal.recal.title"), body: t("cal.recal.body"), confirmLabel: t("cal.recal.ok") });
    if (!ok) return;
    try { await app.recalibrate(); toast(t("cal.recal.started")); poll(); } catch (err) { calError(err); }
  }
  async function saveCal() {
    const ok = await confirmDialog({ title: t("cal.save.title"), body: t("cal.save.body"), confirmLabel: t("cal.save.ok") });
    if (!ok) return;
    try { await app.saveCalibration(); toast(t("cal.save.done")); } catch (err) { calError(err); }
  }
  function calError(err) {
    const p = classifyProblem(err);
    toast(p.kind === "busy" ? t("cal.busy") : `${t(`problem.${p.kind}.title`)}: ${p.detail}`, "error");
  }

  function problemCard() {
    const p = app.deviceProblem ?? (app.device.status === "error" || app.device.status === "disconnected" ? app.device.problem : null);
    if (!p) return null;
    return h("div.banner.error.problem", { role: "alert" }, icon(p.kind === "cancelled" ? "info" : "warn"),
      h("div", {}, h("strong", {}, t(`problem.${p.kind}.title`)), h("p", {}, t(`problem.${p.kind}.body`)), p.detail ? h("p.detail", {}, h("code", {}, p.detail)) : null));
  }

  // what automatic connection is doing, in one sentence (plus the one-time explanation on the first visit)
  function connLine() {
    const c = app.conn.state;
    const real = app.device.kind !== "mock";
    if (real && app.device.status === "connected") return t("conn.state.connected");
    if (c === "needs_permission") return t("conn.first");
    return t(`conn.state.${c}`, { why: app.conn.error ? classifyProblem(app.conn.error).detail || app.conn.error.message : "" });
  }

  function connectionCard() {
    const d = app.device;
    const real = d.kind !== "mock";
    const supported = WebHidDevice.supported;
    const stateKey = d.kind === "mock" ? "mock" : d.status;
    const stock = d.kind === "webhid";
    return h("section.card", {},
      h("div.card-head", {}, h("h2.card-title", {}, t("dev.connection")), h("span.chip", { class: d.status === "connected" ? "on" : "" }, t(`status.${stateKey}`))),
      h("p.card-sub", {}, !real ? t("dev.mock.desc") : stock ? t("dev.stock.desc") : t("dev.real.desc")),
      problemCard(),
      stock ? h("div.banner.info", {}, icon("lock"), h("div", {}, h("strong", {}, t("problem.stock_firmware.title")), h("p", {}, t("problem.stock_firmware.body")))) : null,
      supported ? h("p.field-hint.conn-line", { role: "status", "data-state": app.conn.state }, connLine()) : null,
      !supported ? h("div.banner.warn", {}, icon("warn"), h("div", {}, t("dev.nowebhid"))) : null,
      h("div.btn-row", {},
        !real ? h("button.btn.primary", { type: "button", disabled: !supported, on: { click: connectReal } }, icon("usb"), t("dev.connectReal")) : null,
        real ? h("button.btn", { type: "button", on: { click: () => app.useMock() } }, t("dev.useMock")) : null,
        !real ? h("button.btn.ghost", { type: "button", on: { click: resetDemo } }, icon("reset"), t("dev.resetDemo")) : null,
        real && d.status === "connected" ? h("button.btn", { type: "button", on: { click: requery } }, t("dev.requery")) : null,
        real && d.status !== "connected" ? h("button.btn.primary", { type: "button", disabled: !supported, on: { click: connectReal } }, icon("usb"), t("dev.reconnect")) : null,
        real && d.status === "connected" ? h("button.btn.ghost", { type: "button", on: { click: () => app.disconnectReal() } }, t("dev.disconnect")) : null),
      h("p.field-hint", {}, t("dev.hint", { vid: hex(VID), pid: hex(PID) })));
  }

  function infoCard() {
    const d = app.device;
    const i = d.info;
    const c = d.config?.info;
    return h("section.card", {},
      h("div.card-head", {}, h("h2.card-title", {}, t("dev.info"))),
      h("dl.kv", {},
        row(t("dev.product"), i.productName),
        row(t("dev.vidpid"), i.vid ? `${hex(i.vid)} : ${hex(i.pid)}` : `${hex(VID)} : ${hex(PID)}`, true),
        row(t("dev.fw"), i.firmwareVersion, true),
        row(t("dev.build"), i.build, true),
        c ? row(t("dev.proto"), `${c.protoVersion}`, true) : null,
        c ? row(t("dev.settingsArea"), t("dev.bytes", { n: c.maxSettings }), true) : null,
        c ? row(t("dev.commits"), String(c.commits), true) : null,
        row(t("dev.interface"), d.kind === "mock" ? t("dev.if.mock") : t("dev.if.config"))));
  }

  function capsCard() {
    const d = app.device;
    const f = d.features ?? {};
    const real = d.kind !== "mock";
    const stock = d.kind === "webhid";
    return h("section.card", {},
      h("div.card-head", {}, h("h2.card-title", {}, t("dev.caps"))),
      h("ul.caps", {},
        cap(t("dev.cap.info"), d.status === "connected"),
        cap(t("dev.cap.live"), !!d.capabilities.live, stock ? t("dev.cap.needs") : null),
        cap(t("dev.cap.read"), !!d.capabilities.readProfile, stock ? t("dev.cap.needs") : null),
        cap(t("dev.cap.write"), !!d.capabilities.writeProfile, stock ? t("dev.cap.ro") : t("dev.cap.confirm")),
        cap(t("dev.cap.profile"), !!f.selectProfile, null),
        cap(t("dev.cap.cal"), !!f.recalibrate && !!f.saveCalibration, null),
        cap(t("dev.cap.diag"), !!f.diag, real && d.status === "connected" && !f.diag && !stock ? t("dev.cap.optional") : null),
        cap(t("dev.cap.health"), !!f.health, real && d.status === "connected" && !f.health && !stock ? t("dev.cap.optional") : null)));
  }

  function stat(label, value, tone = "") {
    return h("div.stat", { class: tone }, h("div.stat-l", {}, label), h("div.stat-v.num", {}, value));
  }

  // The diagnostics and health cards are built once per connection; the poll only replaces the numbers (so buttons keep focus).
  function diagCard() {
    const d = app.device;
    if (!d.features?.diag) return null;
    const chip = h("span.chip");
    const errBox = h("div", { hidden: true });
    const stats = h("div.stats.stats-wide", {}, h("div.skeleton", { style: { height: "5rem", gridColumn: "1 / -1" } }));
    const el = h("section.card.diag-card", {},
      h("div.card-head", {}, h("h2.card-title", {}, t("diag.title")), chip),
      errBox, stats,
      h("div.btn-row", {},
        h("button.btn", { type: "button", disabled: !d.features.recalibrate, on: { click: recalibrate } }, icon("reset"), t("cal.recal")),
        h("button.btn", { type: "button", disabled: !d.features.saveCalibration, on: { click: saveCal } }, icon("download"), t("cal.save"))),
      h("p.field-hint", {}, t("cal.hint")));
    chip.hidden = true;
    const update = () => {
      errBox.hidden = !diagError;
      if (diagError) errBox.replaceChildren(h("div.banner.error", {}, icon("warn"), h("div", {}, diagError)));
      const dg = diag;
      if (!dg) return;
      const state = dg.calibrating ? "running" : dg.calResult === "ok" ? "ok" : "fail";
      chip.hidden = false;
      chip.className = `chip ${state === "ok" ? "on" : state === "fail" ? "warn" : ""}`;
      chip.textContent = state === "running" ? t("diag.cal.running") : state === "ok" ? t("diag.cal.ok") : t("diag.cal.fail", { why: t(`diag.cal.why.${dg.calResult}`) });
      stats.replaceChildren(
        stat(t("diag.armed"), dg.armed ? t("common.yes") : t("common.no"), dg.armed ? "" : "warn"),
        stat(t("diag.attempts"), String(dg.calAttempts)),
        stat(t("diag.keysDown"), String(dg.keysDown)),
        stat(t("diag.update"), `${dg.lastUpdateCycles} / ${dg.maxUpdateCycles}`),
        stat(t("diag.frames"), dg.frames.toLocaleString()),
        stat(t("diag.glitches"), String(dg.glitches), dg.glitches ? "warn" : ""),
        stat(t("diag.reports"), dg.reports.toLocaleString()),
        stat(t("diag.heldBack"), String(dg.heldBack)));
    };
    return { el, update };
  }

  function healthCard() {
    const d = app.device;
    if (!d.features?.health) return null;
    const chip = h("span.chip");
    const alert = h("div", { hidden: true });
    const stats = h("div.stats.stats-wide", {}, h("div.skeleton", { style: { height: "5rem", gridColumn: "1 / -1" } }));
    const el = h("section.card.health-card", {}, h("div.card-head", {}, h("h2.card-title", {}, t("health.title")), chip), alert, stats);
    chip.hidden = true;
    const update = () => {
      const hh = health;
      if (!hh) return;
      const bad = hh.faults.length > 0;
      chip.hidden = false;
      chip.className = `chip ${bad ? "warn" : "on"}`;
      chip.textContent = bad ? t("health.faults", { n: hh.faults.length }) : t("health.ok");
      alert.hidden = !bad;
      if (bad) alert.replaceChildren(h("div.banner.warn", {}, icon("warn"), h("div", {}, hh.faults.map((f) => t(`health.fault.${f}`)).join(" · "))));
      stats.replaceChildren(
        stat(t("health.reset"), t(`health.cause.${hh.resetCause}`)),
        stat(t("health.uptime"), `${hh.extra.uptimeS} s`),
        stat(t("health.boots"), String(hh.extra.bootCount)),
        stat(t("health.scanStalls"), String(hh.scanStalls), hh.scanStalls ? "warn" : ""),
        stat(t("health.dma"), String(hh.dmaErrors), hh.dmaErrors ? "warn" : ""),
        stat(t("health.restarts"), String(hh.scanRestarts), hh.scanRestarts ? "warn" : ""),
        stat(t("health.overBudget"), String(hh.engineOverBudget), hh.engineOverBudget ? "warn" : ""),
        stat(t("health.usbResets"), String(hh.usbResets)),
        stat(t("health.usbSuspends"), String(hh.usbSuspends)),
        stat(t("health.retries"), String(hh.reportRetries), hh.reportRetries ? "warn" : ""),
        stat(t("health.resyncs"), String(hh.reportResyncs), hh.reportResyncs ? "warn" : ""),
        stat(t("health.calRefused"), String(hh.calibrationRefused)));
    };
    return { el, update };
  }

  let cards = { diag: null, health: null };
  function render() {
    cards = { diag: diagCard(), health: healthCard() };
    body.replaceChildren(...[
      h("div.dev-grid", {}, connectionCard(), infoCard()),
      capsCard(), cards.diag?.el, cards.health?.el].filter(Boolean));
  }

  async function poll() {
    if (polling) return;
    const d = app.device;
    if (d.status !== "connected" || !(d.features?.diag || d.features?.health)) return;
    polling = true;
    try {
      diag = await d.readEngineDiag();
      health = await d.readHealth();
      diagError = null;
    } catch (err) {
      diagError = err.message;
    } finally {
      polling = false;
    }
    cards.diag?.update();
    cards.health?.update();
  }

  const offs = [app.on("device", () => { diag = health = null; render(); poll(); }), app.on("conn", render)];
  render();
  poll();
  timer = setInterval(poll, POLL_MS);
  root.append(body);
  return { destroy() { clearInterval(timer); offs.forEach((o) => o()); } };
}
