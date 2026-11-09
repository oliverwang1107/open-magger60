// One place that turns whatever went wrong with a keyboard into a small, fixed set of situations the UI can explain
// (locale keys "problem.<kind>.title" / ".body"). The error objects themselves keep their technical text for the details line.
import { SettingsError } from "../core/settings_codec.js";

export const PROBLEM_KINDS = Object.freeze([
  "unsupported",      // this browser has no WebHID
  "cancelled",        // nobody picked a keyboard
  "stock_firmware",   // a keyboard that does not run our firmware: nothing is sent to it
  "proto_old",        // our firmware, but a protocol version older than this configurator speaks
  "proto_new",        // ... newer
  "settings_size",    // the keyboard's settings area is smaller than this configurator's settings blob
  "crc",              // a settings blob arrived damaged
  "settings",         // the keyboard holds settings this configurator cannot read (wrong version, shape, values)
  "busy",             // the keyboard refused for now (a key is down / it is writing)
  "invalid",          // the keyboard's validator refused what was sent
  "timeout",          // no answer
  "lost",             // the keyboard went away (unplugged, USB reset) during an exchange
  "refused",          // any other refusal by the keyboard
  "other",
]);

/** {kind, detail} for any error thrown by the device layer. `detail` is the technical message. */
export function classifyProblem(err) {
  if (!err) return { kind: "other", detail: "" };
  const detail = String(err.message ?? err);
  const name = err.name ?? "";
  if (name === "UnsupportedError") return { kind: "unsupported", detail };
  if (name === "NotFoundError" || /no device chosen/i.test(detail)) return { kind: "cancelled", detail };
  if (err instanceof SettingsError) return { kind: err.code === "crc" ? "crc" : "settings", detail, sub: err.code };
  if (name === "NetworkError" || name === "InvalidStateError" || /disconnected|not opened|device has been lost/i.test(detail)) return { kind: "lost", detail };
  switch (err.code) {
    case "stock_firmware": case "proto_old": case "proto_new": case "settings_size": case "crc": case "busy": case "timeout": case "lost":
      return { kind: err.code, detail };
    default:
  }
  if (err.status === 11) return { kind: "invalid", detail };
  if (err.status === 13) return { kind: "busy", detail };
  if (name === "TimeoutError") return { kind: "timeout", detail };
  if (err.status !== null && err.status !== undefined) return { kind: "refused", detail };
  return { kind: "other", detail };
}
