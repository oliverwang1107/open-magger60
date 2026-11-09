// Keeping the editor's local model and the keyboard's settings in step, with no DOM: used by the app state and by the tests.
//
//   local model  = the four UI profiles being edited (state.js)
//   keyboard     = the settings blob on the device, read once on connect and after every save
//   dirty        = the blob the local model would be saved as differs from the keyboard's blob (both passed through the
//                  same UI model, so bytes the UI never shows cannot cause a difference)
//
// Nothing here writes to the keyboard unless saveToKeyboard() is called, which the UI does only after the person pressed Save.
import { blobToWorkspace, normalizeBlob, sameBytes, workspaceToBlob } from "./core/profile_codec.js";
import { classifyProblem } from "./device/errors.js";
import { WORD } from "./device/config_protocol.js";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Read the keyboard's settings. Returns { workspace, blob }; throws SettingsError / ConfigError (see classifyProblem). */
export async function loadFromKeyboard(device, layout, base = null) {
  const blob = await device.readSettings();
  return { workspace: blobToWorkspace(blob, layout, base), blob: normalizeBlob(blob, layout) };
}

/**
 * What a Save would do right now: { dirty, problems, converted, bytes }.
 * `problems` are things the keyboard cannot hold (the Save button stays disabled and the page lists them).
 */
export function pendingChanges(workspace, keyboardBlob, layout) {
  const r = workspaceToBlob(workspace, layout);
  // an overlay entry equal to the base layer's does what "transparent" does and is read back as such, so both sides are compared
  // in that normalized form (an explicit "Q on layer 2" over a base Q is not a difference)
  const normalized = r.bytes ? normalizeBlob(r.bytes, layout) : null;
  return {
    problems: r.problems,
    converted: r.converted,
    bytes: r.bytes,
    normalized,
    dirty: r.problems.length > 0 || !keyboardBlob || !sameBytes(normalized, keyboardBlob),
  };
}

/**
 * Write the workspace to the keyboard: stage, commit with the confirmation word, read back, compare.
 * `onStep(name)` is told "encode" | "stage" | "commit" | "verify" (the read-back comparison) for progress. A BUSY keyboard (a key is down) is retried for
 * `busyWaitMs`. Returns { commits, blob, converted, bytes }; on success `blob` is what the keyboard now holds (normalized).
 * Throws the underlying error (classify with classifyProblem); the keyboard keeps its old settings unless the COMMIT was accepted.
 */
export async function saveToKeyboard(device, workspace, layout, { onStep = () => {}, busyWaitMs = 4000 } = {}) {
  onStep("encode");
  const pending = pendingChanges(workspace, null, layout);
  if (pending.problems.length) {
    const e = new Error(`${pending.problems.length} setting(s) cannot be stored on the keyboard`);
    e.name = "UnsupportedSettingsError";
    e.problems = pending.problems;
    throw e;
  }
  const started = Date.now();
  for (;;) {
    device.armWrites(WORD.COMMIT); // one write; used up whether it works or not
    try {
      const commits = await device.writeSettings(pending.bytes, { onStep }); // stages, commits, reads back and compares
      return { commits, blob: normalizeBlob(pending.bytes, layout), converted: pending.converted, bytes: pending.bytes };
    } catch (err) {
      if (classifyProblem(err).kind === "busy" && Date.now() - started < busyWaitMs) { await sleep(150); continue; }
      throw err;
    }
  }
}
