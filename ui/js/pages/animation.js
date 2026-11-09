// Animation page: pick or load an animation, watch it on the keyboard preview, and play it live on the keyboard's 61 lights through the
// light frame stream (command 0xC7, STREAM_PROTOCOL.md). Same easy style as the lights page: big buttons, plain words, live preview.
//
//   * The preview always works (also without a keyboard); what the preview draws is exactly the frame that is sent.
//   * "Show on the keyboard" starts a stream session. The keyboard shows the frames instead of its own lights for as long as they keep
//     coming, and goes back to its own lights by itself about a second after they stop (closing this page, unplugging, a sleeping PC).
//   * Saving an animation INTO the keyboard is not possible with the current firmware (the clip is compiled into the image, M6_PLAN.md 5.4 B);
//     the page says so and offers a file of the animation instead.
import { h, icon, toast } from "../dom.js";
import { t } from "../i18n.js";
import { KeyboardView } from "../ui/keyboard.js";
import { rangeField } from "../ui/controls.js";
import { ledOfSlot } from "../core/led_map.js";
import { AnimPlayer, DEMOS, TRAIL_MAX_S, leakKernel, simulateLeak, SPEED_RANGE, FITS, FRAME_BYTES, applyLook, blankFrame, isValidTint, lightPositions, makeDemo, serializeAnim, parseAnim, parseClipIndex } from "../core/anim.js";
import { StreamController, streamAvailability } from "../core/anim_stream.js";
import { importFiles, ImportError, MAX_VIDEO_S } from "../anim_import.js";
import { loadJson, saveJson } from "../storage.js";

const TINTS = ["#ffffff", "#ff3b30", "#ff9500", "#ffd60a", "#34c759", "#00d5c0", "#0a84ff", "#bf5af2"];
const RATES = [10, 15, 24, 30];
const fmtTime = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

export function mount(root, app) {
  const prefs = { brightness: 70, speed: 1, loop: true, invert: false, tint: null, antiBleed: 0, contrast: 0, showLeak: false, trail: 0, fit: "crop", rate: 15, sync: true, ...loadJson("anim-prefs", {}) };
  const pos = lightPositions(app.layout);
  const player = new AnimPlayer({ onState: () => paint() });
  player.setLoop(prefs.loop);
  player.setSpeed(prefs.speed);
  player.trailS = (prefs.trail / 100) * TRAIL_MAX_S;
  const kernel = leakKernel(pos);
  // sound of a local clip (./local/clips): follows the player (play / pause / stop / seek / speed / loop)
  const audio = typeof Audio !== "undefined" ? new Audio() : null;
  let hasAudio = false;
  let soundBlocked = false;
  const setBlocked = (v) => { if (soundBlocked !== v) { soundBlocked = v; paint(); } };
  const audioPlay = () => { if (!hasAudio) return; audio.play().then(() => setBlocked(false), () => setBlocked(true)); };
  const audioSeek = () => { if (hasAudio && Number.isFinite(audio.duration || NaN) !== false) { try { audio.currentTime = Math.min(player.time, audio.duration || player.time); } catch { /* not loaded yet */ } } };
  function dropAudio() { if (!audio) return; audio.pause(); audio.removeAttribute("src"); audio.load(); hasAudio = false; soundBlocked = false; }
  let leakView = blankFrame();       // what the preview draws: shown, or shown + the leak simulation
  let shown = blankFrame();         // the frame on the preview and (when streaming) on the keyboard, after the look
  let source = null;                // { files, label } the animation was made from, so a changed fit / rate can read it again
  let loading = null;               // AbortController while a file is being read
  let stream = null;
  let streamDev = null;
  let alive = true;
  let timer = null;
  let loadMsg = "";

  // ---- stream session for the current device ----
  const availability = () => streamAvailability(app.device);
  function attachStream() {
    if (streamDev === app.device && stream) return;
    stream?.dispose();
    stream = null;
    streamDev = app.device;
    if (availability() === "ok") stream = new StreamController(app.device, { onChange: () => paint() });
  }
  const wantStream = () => prefs.sync && !!stream && player.state !== "stopped";
  async function syncStream() {
    if (!stream) return;
    if (wantStream() && stream.phase === "off") await stream.start();
    else if (!wantStream() && (stream.phase === "live" || stream.phase === "starting" || stream.phase === "error")) await stream.stop();
  }

  // ---- the clock: advances the player, draws the preview, feeds the stream ----
  function frameTick() {
    if (!alive) return;
    const now = performance.now();
    const f = player.tick(now);
    if (f) {
      shown = applyLook(f, { brightness: prefs.brightness / 100, invert: prefs.invert, tint: prefs.tint, antiBleed: prefs.antiBleed / 100, contrast: prefs.contrast / 100, kernel });
      leakView = prefs.showLeak ? simulateLeak(shown, kernel) : shown;
      stream?.push(shown, now);
    }
    if (hasAudio) {
      audio.loop = player.loop; audio.playbackRate = player.speed;
      if (player.state === "playing") {
        if (audio.paused && !soundBlocked) audioPlay();
        else if (!audio.paused && Math.abs(audio.currentTime - player.time) > 0.25 && !audio.seeking) audioSeek();
      } else if (!audio.paused) audio.pause();
      if (player.state === "stopped" && audio.currentTime > 0 && player.pos === 0) audio.currentTime = 0;
    }
    if (stream && stream.phase !== "off" && !wantStream()) syncStream();
    timeline.set(player.count ? (player.pos / player.count) * 1000 : 0);
    timeText.textContent = `${fmtTime(player.time)} / ${fmtTime(player.duration)}`;
  }

  // ---- the keyboard preview ----
  const view = new KeyboardView(app, {
    mode: "lighting",
    colorFn: (key) => { const i = ledOfSlot(key.slot); return i < 0 ? [0, 0, 0] : [leakView[i * 3], leakView[i * 3 + 1], leakView[i * 3 + 2]]; },
  });
  const nameEl = h("strong.anim-name");
  const timeText = h("span.anim-time");
  const timeline = rangeField({ id: "anim-pos", label: t("anim.position"), min: 0, max: 1000, step: 1, unit: "", onInput: (v) => { player.seek(v / 1000); audioSeek(); frameTick(); } });
  timeline.el.classList.add("anim-timeline");
  const btnPlay = h("button.btn.primary.big", { type: "button", on: { click: () => { player.play(performance.now()); audioSeek(); audioPlay(); (stream?.phase === "error" ? stream.stop() : Promise.resolve()).then(syncStream); } } }, icon("play"), t("anim.play"));
  const btnPause = h("button.btn.big", { type: "button", on: { click: () => { player.pause(); if (hasAudio) audio.pause(); } } }, icon("pause"), t("anim.pause"));
  const btnStop = h("button.btn.big", { type: "button", on: { click: () => { player.stop(); if (hasAudio) { audio.pause(); audio.currentTime = 0; } frameTick(); syncStream(); } } }, icon("stop"), t("anim.stop"));
  const preview = h("section.card.kb-card.led-preview", { "aria-label": t("anim.title") },
    view.el,
    h("div.anim-now", {}, nameEl, timeText),
    h("div.anim-transport", { role: "group", "aria-label": t("anim.transport") }, btnPlay, btnPause, btnStop),
    timeline.el,
    h("p.kb-hint", {}, t("anim.preview.hint")));

  // ---- where the animation comes from ----
  const demoBtns = DEMOS.map((d) => h("button.fx.look", { type: "button", "data-demo": d.id, title: t(`anim.demo.${d.id}.d`), on: { click: () => useAnim(makeDemo(d.id, pos), { demo: d.id }) } }, t(`anim.demo.${d.id}`)));
  const fileInput = h("input", { type: "file", multiple: true, accept: "image/*,video/*,.gif,.json", hidden: true, on: { change: () => { if (fileInput.files.length) loadFiles([...fileInput.files]); fileInput.value = ""; } } });
  const pick = h("button.btn.primary", { type: "button", on: { click: () => fileInput.click() } }, icon("upload"), t("anim.pick"));
  const cancel = h("button.btn.sm", { type: "button", hidden: true, on: { click: () => loading?.abort() } }, icon("x"), t("anim.cancel"));
  const loadBar = h("progress.anim-bar", { max: 100, value: 0, hidden: true, "aria-label": t("anim.loading", { p: "" }).replace(/\s*%$/, "") });
  const loadState = h("p.field-hint.anim-load", { role: "status", "aria-live": "polite" });
  const fitSeg = h("div.seg", { role: "radiogroup", "aria-label": t("anim.fit") }, FITS.map((f) => h("button", { type: "button", role: "radio", "data-fit": f, title: t(`anim.fit.${f}.d`), on: { click: () => { prefs.fit = f; savePrefs(); paint(); rereadSource(); } } }, t(`anim.fit.${f}`))));
  const rateSeg = h("div.seg", { role: "radiogroup", "aria-label": t("anim.rate") }, RATES.map((r) => h("button", { type: "button", role: "radio", "data-rate": String(r), on: { click: () => { prefs.rate = r; savePrefs(); paint(); rereadSource(); } } }, String(r))));
  const sourceCard = h("section.card.easy-panel", {},
    h("div.card-head", {}, h("h2.card-title", {}, t("anim.source")), h("span.card-sub", {}, t("anim.source.sub"))),
    h("div.field", {}, h("div.field-label", {}, t("anim.demos")), h("div.fx-grid.looks", {}, demoBtns)),
    h("div.field", {}, h("div.field-label", {}, t("anim.yours")), h("div.anim-pick", {}, pick, cancel, fileInput), loadBar, loadState, h("p.field-hint", {}, t("anim.yours.hint", { s: MAX_VIDEO_S }))),
    h("div.field", {}, h("div.field-label", {}, t("anim.fit")), fitSeg, h("div.field-hint", {}, t("anim.fit.hint"))),
    h("div.field", {}, h("div.field-label", {}, t("anim.rate")), rateSeg, h("div.field-hint", {}, t("anim.rate.hint"))));

  // ---- local clips: only when ./local/clips/index.json exists (never in a public deployment) ----
  const localBtns = h("div.fx-grid.looks");
  const localCard = h("section.card.easy-panel.anim-local", { hidden: true },
    h("div.card-head", {}, h("h2.card-title", {}, t("anim.local")), h("span.card-sub", {}, t("anim.local.sub"))),
    localBtns);
  async function loadClip(entry) {
    loading?.abort();
    const ctl = new AbortController();
    loading = ctl;
    cancel.hidden = false;
    loadBar.hidden = false; loadBar.value = 0;
    loadState.textContent = t("anim.loading", { p: 0 });
    try {
      const res = await fetch(`./local/clips/${entry.anim}`, { signal: ctl.signal, cache: "no-store" });
      if (!res.ok) throw new Error(String(res.status));
      const total = Number(res.headers.get("content-length")) || 0;
      const chunks = []; let got = 0;
      const rdr = res.body.getReader();
      for (;;) {
        const { done, value } = await rdr.read();
        if (done) break;
        chunks.push(value); got += value.length;
        if (total) { loadBar.value = Math.round((got / total) * 90); loadState.textContent = t("anim.loading", { p: Math.round((got / total) * 90) }); }
      }
      const text = new TextDecoder().decode(await new Blob(chunks).arrayBuffer());
      loadBar.value = 95;
      await new Promise((r) => setTimeout(r, 0));
      const anim = parseAnim(text);
      if (loading !== ctl || !alive) return;
      dropAudio();
      if (entry.audio) { audio.src = `./local/clips/${entry.audio}`; audio.preload = "auto"; hasAudio = true; }
      loadMsg = t("anim.loaded", { n: anim.frames.length, fps: anim.fps }) + (entry.audio ? ` ${t("anim.local.sound")}` : "");
      useAnim({ ...anim, name: entry.name }, { local: entry.name });
    } catch (err) {
      if (loading !== ctl || err?.name === "AbortError") return;
      loadMsg = t("anim.err.bad_file");
      toast(loadMsg, "error");
    } finally {
      if (loading === ctl) { loading = null; cancel.hidden = true; loadBar.hidden = true; }
      paint();
    }
  }
  fetch("./local/clips/index.json", { cache: "no-store" }).then((r) => (r.ok ? r.text() : "")).then((txt) => {
    const list = parseClipIndex(txt);
    if (!alive || !list.length) return;
    localBtns.replaceChildren(...list.map((e) => h("button.fx.look", { type: "button", "data-clip": e.name, title: e.audio ? t("anim.local.sound") : "", on: { click: () => loadClip(e) } }, e.name)));
    localCard.hidden = false;
  }).catch(() => {});

  // ---- how it looks ----
  const speed = rangeField({ id: "anim-speed", label: t("anim.speed"), min: SPEED_RANGE[0], max: SPEED_RANGE[1], step: 0.05, unit: "×", onInput: (v) => { prefs.speed = v; player.setSpeed(v); savePrefs(); } });
  const bright = rangeField({ id: "anim-bri", label: t("anim.bright"), hint: t("anim.bright.hint"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => { prefs.brightness = v; savePrefs(); } });
  const anti = rangeField({ id: "anim-anti", label: t("anim.anti"), hint: t("anim.anti.hint"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => { prefs.antiBleed = v; savePrefs(); } });
  const contrast = rangeField({ id: "anim-contrast", label: t("anim.contrast"), hint: t("anim.contrast.hint"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => { prefs.contrast = v; savePrefs(); } });
  const leakBox = h("input", { type: "checkbox", id: "anim-leak", on: { change: () => { prefs.showLeak = leakBox.checked; savePrefs(); } } });
  const trail = rangeField({ id: "anim-trail", label: t("anim.trail"), hint: t("anim.trail.hint"), min: 0, max: 100, step: 1, unit: "%", onInput: (v) => { prefs.trail = v; player.trailS = (v / 100) * TRAIL_MAX_S; savePrefs(); } });
  const loopBox = h("input", { type: "checkbox", id: "anim-loop", on: { change: () => { prefs.loop = loopBox.checked; player.setLoop(prefs.loop); savePrefs(); } } });
  const invertBox = h("input", { type: "checkbox", id: "anim-inv", on: { change: () => { prefs.invert = invertBox.checked; savePrefs(); } } });
  const tintInput = h("input", { type: "color", "aria-label": t("anim.tint"), on: { input: () => setTint(tintInput.value) } });
  const tintSw = [h("button.swatch.big.none", { type: "button", title: t("anim.tint.none"), "aria-label": t("anim.tint.none"), on: { click: () => setTint(null) } }, icon("x")),
    ...TINTS.map((c) => h("button.swatch.big", { type: "button", style: { background: c }, title: c, "aria-label": c, on: { click: () => setTint(c) } })),
    h("label.swatch.big.custom", { title: t("led.custom") }, tintInput)];
  const setTint = (c) => { prefs.tint = c; savePrefs(); paint(); };
  const lookCard = h("section.card.easy-panel", {},
    h("div.card-head", {}, h("h2.card-title", {}, t("anim.look")), h("span.card-sub", {}, t("anim.look.sub"))),
    speed.el, bright.el, anti.el, contrast.el, trail.el,
    h("label.check", { for: "anim-leak" }, leakBox, h("span", {}, t("anim.leak")), h("span.field-hint", {}, ` ${t("anim.leak.hint")}`)),
    h("label.check", { for: "anim-loop" }, loopBox, h("span", {}, t("anim.loop"))),
    h("label.check", { for: "anim-inv" }, invertBox, h("span", {}, t("anim.invert")), h("span.field-hint", {}, ` ${t("anim.invert.hint")}`)),
    h("div.field", {}, h("div.field-label", {}, t("anim.tint")), h("div.palette", {}, tintSw), h("div.field-hint", {}, t("anim.tint.hint"))));

  // ---- the keyboard: streaming status ----
  const syncBox = h("input", { type: "checkbox", id: "anim-sync", on: { change: () => { prefs.sync = syncBox.checked; savePrefs(); syncStream(); paint(); } } });
  const backBtn = h("button.btn.primary", { type: "button", hidden: true, on: { click: () => { prefs.sync = false; savePrefs(); syncStream(); paint(); } } }, icon("reset"), t("anim.back"));
  const retryBtn = h("button.btn.sm", { type: "button", hidden: true, on: { click: () => { stream?.stop().then(() => syncStream()); } } }, icon("reset"), t("led.retry"));
  const soundBtn = h("button.btn.primary", { type: "button", on: { click: () => audioPlay() } }, icon("play"), t("anim.sound.allow"));
  const soundBanner = h("div.banner.warn.anim-sound", { role: "status", hidden: true }, icon("warn"), h("div", {}, t("anim.sound.blocked")), soundBtn);
  const streamBanner = h("div.banner.anim-stream", { role: "status", "aria-live": "polite" });
  const kbCard = h("section.card.easy-panel.anim-kb", {},
    h("div.card-head", {}, h("h2.card-title", {}, t("anim.kb")), h("span.card-sub", {}, t("anim.kb.sub"))),
    h("label.check.big", { for: "anim-sync" }, syncBox, h("span", {}, t("anim.sync"))),
    soundBanner, streamBanner, h("div.anim-kbrow", {}, backBtn, retryBtn),
    h("p.field-hint", {}, t("anim.kb.how")));

  // ---- save into the keyboard / keep as a file ----
  const saveBtn = h("button.btn", { type: "button", disabled: true, title: t("anim.keep.nofw") }, icon("download"), t("anim.keep.kb"));
  const fileBtn = h("button.btn", { type: "button", on: { click: downloadAnim } }, icon("download"), t("anim.keep.file"));
  const keepCard = h("section.card.easy-panel", {},
    h("div.card-head", {}, h("h2.card-title", {}, t("anim.keep")), h("span.card-sub", {}, t("anim.keep.sub"))),
    h("div.banner.warn", {}, icon("info"), h("div", {}, t("anim.keep.nofw"))),
    h("div.anim-kbrow", {}, saveBtn, fileBtn));

  function downloadAnim() {
    if (!player.anim) return;
    const blob = new Blob([serializeAnim(player.anim)], { type: "application/json" });
    const a = h("a", { href: URL.createObjectURL(blob), download: `${player.anim.name.replace(/[^\w.-]+/g, "_") || "animation"}.json` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }

  function savePrefs() { saveJson("anim-prefs", prefs); }

  // ---- loading ----
  function useAnim(anim, src = null) {
    if (!src?.local) dropAudio();
    player.load(anim);
    source = src?.files ? src : null;
    if (src?.demo) loadMsg = "";
    nameEl.textContent = anim.name;
    syncStream();
    frameTick();
    paint();
  }
  async function loadFiles(files) {
    loading?.abort();
    const ctl = new AbortController();
    loading = ctl;
    cancel.hidden = false;
    loadBar.hidden = false; loadBar.value = 0;
    loadState.textContent = t("anim.loading", { p: 0 });
    try {
      const anim = await importFiles(files, { pos, fit: prefs.fit, fps: prefs.rate, signal: ctl.signal, onProgress: (p) => { if (loading === ctl) { loadBar.value = Math.round(p * 100); loadState.textContent = t("anim.loading", { p: Math.round(p * 100) }); } } });
      if (loading !== ctl || !alive) return;
      loadMsg = t("anim.loaded", { n: anim.frames.length, fps: anim.fps });
      useAnim(anim, { files });
    } catch (err) {
      if (loading !== ctl) return;
      loadMsg = err instanceof ImportError ? (err.code === "aborted" ? "" : t(`anim.err.${err.code}`, { s: MAX_VIDEO_S })) : t("anim.err.bad_file");
      if (err instanceof ImportError && err.code !== "aborted") toast(loadMsg, "error");
    } finally {
      if (loading === ctl) { loading = null; cancel.hidden = true; loadBar.hidden = true; }
      paint();
    }
  }
  function rereadSource() { if (source?.files && !loading) loadFiles(source.files); }

  // ---- painting ----
  const REASONS = ["unavailable", "silent", "lost", "unsupported"];
  function paint() {
    if (!alive) return;
    const a = availability();
    const phase = stream?.phase ?? "off";
    nameEl.textContent = player.anim?.name ?? t("anim.none");
    [btnPlay, btnPause, btnStop].forEach((b) => { b.disabled = !player.count; });
    btnPlay.hidden = player.state === "playing";
    btnPause.hidden = player.state !== "playing";
    btnStop.disabled = !player.count || player.state === "stopped";
    timeline.el.hidden = !player.count;
    demoBtns.forEach((b) => b.setAttribute("aria-checked", String(player.anim?.demo && player.anim.name === b.dataset.demo)));
    fitSeg.querySelectorAll("button").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.fit === prefs.fit)));
    rateSeg.querySelectorAll("button").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.rate === String(prefs.rate))));
    speed.set(prefs.speed); bright.set(prefs.brightness); anti.set(prefs.antiBleed); contrast.set(prefs.contrast); leakBox.checked = prefs.showLeak; trail.set(prefs.trail);
    loopBox.checked = prefs.loop; invertBox.checked = prefs.invert;
    tintSw.forEach((el) => el.classList?.toggle("on", (el.title ?? "").toLowerCase() === (prefs.tint ?? "none").toLowerCase() || (prefs.tint === null && el.classList.contains("none"))));
    if (isValidTint(prefs.tint)) tintInput.value = prefs.tint;
    if (!loading) loadState.textContent = loadMsg;
    syncBox.checked = prefs.sync;
    syncBox.disabled = a !== "ok";

    let kind = "info", ic = "info", text, mode;
    if (a !== "ok") { kind = "warn"; ic = "warn"; text = t(`anim.na.${a}`); mode = `na-${a}`; }
    else if (phase === "starting") { text = t("anim.st.starting"); mode = "starting"; }
    else if (phase === "live") { kind = "ok"; ic = "check"; text = t(app.device.kind === "mock" ? "anim.st.live.demo" : "anim.st.live"); mode = "live"; }
    else if (phase === "error") { kind = "error"; ic = "warn"; text = t(`anim.st.err.${REASONS.includes(stream.reason) ? stream.reason : "other"}`, { why: stream.reason ?? "" }); mode = "error"; }
    else if (!prefs.sync) { text = t("anim.st.off.sync"); mode = "normal-off"; }
    else if (!player.count) { text = t("anim.st.off.pick"); mode = "normal-pick"; }
    else if (player.state === "stopped") { text = t("anim.st.off.play"); mode = "normal-play"; }
    else { text = t("anim.st.off"); mode = "normal"; }
    streamBanner.className = `banner anim-stream ${kind}`;
    streamBanner.dataset.mode = mode;
    streamBanner.replaceChildren(icon(ic), h("div", {}, text));
    soundBanner.hidden = !soundBlocked;
    backBtn.hidden = !(phase === "live" || phase === "starting");
    retryBtn.hidden = phase !== "error" || stream?.reason === "unsupported";
    saveBtn.disabled = true;
    fileBtn.disabled = !player.count;
  }

  attachStream();
  const offDevice = app.on("device", () => { attachStream(); syncStream(); paint(); });
  // the page opens with a demo so there is always something to see
  useAnim(makeDemo("ripple", pos), { demo: "ripple" });
  root.append(h("div.page-grid.light", {}, preview, h("div.led-side", {}, h("div.led-slot", {}, kbCard, sourceCard, localCard, lookCard, keepCard))));
  timer = setInterval(frameTick, 20);
  paint();

  return {
    destroy() {
      alive = false;
      clearInterval(timer);
      offDevice();
      loading?.abort();
      view.destroy();
      dropAudio();
      stream?.dispose();             // sends STOP: the keyboard shows its own lights again at once
    },
  };
}
