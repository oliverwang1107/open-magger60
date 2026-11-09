// Automatic connection to the keyboard. When the page opens it looks for a keyboard the browser already gave this site access to
// (navigator.hid.getDevices(): no click, no chooser) and connects; it reconnects when the keyboard is plugged in again (the "connect" /
// "disconnect" events of navigator.hid). The very first visit still needs a click, because the browser insists on a chooser then.
//
// States (app.conn.state):
//   unsupported       this browser has no WebHID: only the demo keyboard
//   searching         looking at the keyboards this site may already use
//   needs_permission  none yet (first visit, or access was revoked): the person has to press "connect" once
//   connecting        a keyboard was found, opening it
//   connected         talking to it
//   waiting           it was unplugged (or not plugged in): the page connects by itself when it comes back
//   failed            it was found but could not be opened (error says why); NOT retried in a loop: three tries, then it stops
//                     until the keyboard is plugged in again or the person presses connect
//   paused            the person chose the demo keyboard / disconnected: nothing happens by itself until they connect again
//
// No DOM. `connect(hidDevice)` does the real work (open, verify the firmware, swap the app's device) and rejects on failure.
import { VID, PID, USAGE_PAGE, USAGE } from "./device/ids.js";

export const CONN_STATES = Object.freeze(["unsupported", "searching", "needs_permission", "connecting", "connected", "waiting", "failed", "paused"]);

/** Is this HIDDevice the keyboard's configuration interface (vendor/product id and the usage the config interface shows)? */
export function isConfigInterface(d) {
  if (!d || d.vendorId !== VID || d.productId !== PID) return false;
  const cols = d.collections;
  return !Array.isArray(cols) || cols.length === 0 || cols.some((c) => c.usagePage === USAGE_PAGE && c.usage === USAGE);
}

const defaultSleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * @param {object} o
 * @param {object|undefined} o.hid          navigator.hid (or a fake): getDevices(), addEventListener
 * @param {(hidDevice) => Promise} o.connect
 * @param {(conn: {state: string, error: any}) => void} o.onState
 * @param {() => boolean} [o.isConnected]   true while a real keyboard is connected (so a stray event does not reconnect)
 * @param {number[]} [o.retryDelaysMs]      waits between the tries of one round (default 3 tries: now, +500 ms, +1500 ms)
 */
export function createAutoConnect({ hid, connect, onState, isConnected = () => false, retryDelaysMs = [500, 1500], sleepFn = defaultSleep }) {
  let state = "idle";
  let error = null;
  let paused = false;
  let running = null;    // the round in progress (a promise), so two triggers never open the keyboard twice
  let current = null;    // the HIDDevice we are connected to
  let started = false;
  let stopped = false;

  const set = (next, err = null) => {
    state = next;
    error = err;
    onState({ state, error });
  };
  const known = () => (hid?.getDevices ? hid.getDevices().then((l) => l.filter(isConfigInterface)) : Promise.resolve([]));

  async function round(device) {
    set("connecting");
    let lastErr = null;
    for (let i = 0; i <= retryDelaysMs.length; i++) {
      if (stopped || paused) return;
      try {
        await connect(device);
        current = device;
        set("connected");
        return;
      } catch (err) {
        lastErr = err;
        // the person closed a chooser / the browser refused for lack of permission: retrying cannot help
        if (err?.name === "SecurityError" || err?.name === "NotAllowedError") break;
        if (i < retryDelaysMs.length) await sleepFn(retryDelaysMs[i]);
      }
    }
    if (!stopped && !paused) set("failed", lastErr);
  }

  function run(fn) {
    if (running) return running;
    running = fn().finally(() => { running = null; });
    return running;
  }

  async function look() {
    if (stopped || paused || isConnected()) return;
    set("searching");
    let found = [];
    try { found = await known(); } catch { found = []; }
    if (stopped || paused) return;
    if (!found.length) { set(current ? "waiting" : "needs_permission"); return; }
    await round(found[0]);
  }

  function onPlug(ev) {
    if (stopped || paused || !isConfigInterface(ev.device) || isConnected() || state === "connecting") return;
    run(() => round(ev.device));
  }
  function onUnplug(ev) {
    if (stopped || !isConfigInterface(ev.device)) return;
    if (current && ev.device !== current && state === "connected") return; // another interface of the same keyboard
    if (state === "connected" || state === "connecting" || state === "failed") set(paused ? "paused" : "waiting");
  }

  return {
    get state() { return state; },
    get error() { return error; },
    /** Call once when the page is ready. Resolves when the first round is over. */
    start() {
      if (started) return running ?? Promise.resolve();
      started = true;
      if (!hid) { set("unsupported"); return Promise.resolve(); }
      hid.addEventListener("connect", onPlug);
      hid.addEventListener("disconnect", onUnplug);
      return run(look);
    },
    /** Look again right now (e.g. after the person pressed connect and chose a keyboard elsewhere). */
    rescan() { return run(look); },
    /** The person chose the demo keyboard / disconnected: stay out of the way. */
    pause() { paused = true; set("paused"); },
    /** The person connects by hand again: automatic behaviour is back on. */
    resume() { paused = false; },
    /** The manual connect worked: record it (the person's own click went through app.connectReal). */
    markConnected(device) { current = device ?? current; paused = false; set("connected"); },
    stop() {
      stopped = true;
      hid?.removeEventListener?.("connect", onPlug);
      hid?.removeEventListener?.("disconnect", onUnplug);
    },
  };
}
