// LEGACY SHIM, safe to delete. The stock firmware answers a "version" request (command 3) on its configuration
// interface; this module sends that one read-only request and decodes the reply so the Device page can show a
// version before the custom protocol (scope command 0xC5, M3_PLAN.md) is wired in. When that exists, delete
// this file and the `legacyReadVersion` call in WebHidDevice.js.
//
// Packet (64 bytes, report id 0): [0]=0x55 header, [1]=cmd, [2]=0 (no XOR), [3]=checksum, [4]=len,
// [5..7]=offset LE24, rest 0. Checksum = (len + offset bytes + data bytes) & 0xFF. Reply: [0]=0xAA on success.
// Nothing but command 3 can be built or sent from here.

const REPORT_LEN = 64;
const HDR = 0x55;
const CMD = 0x03;
const REPLY_TIMEOUT_MS = 1500;

export class SafetyError extends Error {}

export function checksum(len, offset, data = []) {
  let sum = len + (offset & 0xff) + ((offset >> 8) & 0xff) + ((offset >> 16) & 0xff);
  for (const b of data) sum += b;
  return sum & 0xff;
}

/** Independent last check, run on the exact bytes right before they would be sent. */
export function assertSendable(p) {
  if (!(p instanceof Uint8Array) || p.length !== REPORT_LEN) throw new SafetyError("packet must be 64 bytes");
  if (p[0] !== HDR) throw new SafetyError("unexpected header byte");
  if (p[2] !== 0) throw new SafetyError("XOR-masked packets are not allowed");
  if (p[1] !== CMD) throw new SafetyError(`command 0x${p[1].toString(16)} is not allowed in this version`);
  if (p.subarray(8).some((b) => b !== 0)) throw new SafetyError("a read request carries no payload");
  if (p[3] !== checksum(p[4], p[5] | (p[6] << 8) | (p[7] << 16))) throw new SafetyError("checksum mismatch");
  return p;
}

/** The only request this module can build. Any other command number throws. */
export function buildReadRequest(cmd = CMD) {
  if (cmd !== CMD) throw new SafetyError(`command 0x${cmd.toString(16)} is not allowed in this version`);
  const p = new Uint8Array(REPORT_LEN);
  p[0] = HDR;
  p[1] = CMD;
  return assertSendable(p);
}

/** Decode a command 3 reply: {version: "104", build: "<text>"}. */
export function decodeInfoResponse(resp) {
  const r = resp instanceof Uint8Array ? resp : new Uint8Array(resp.buffer ?? resp);
  if (r.length < 8 || r[0] !== 0xaa || r[1] !== CMD) throw new Error("unexpected reply");
  const n = r[4];
  const data = r.subarray(8, 8 + n);
  if (((n + r[5] + r[6] + r[7] + data.reduce((a, b) => a + b, 0)) & 0xff) !== r[3]) throw new Error("reply checksum mismatch");
  if (n < 2) throw new Error("reply too short");
  const end = data.indexOf(0, 2);
  const text = Array.from(data.subarray(2, end < 0 ? data.length : end), (c) => String.fromCharCode(c)).join("");
  return { version: (data[0] | (data[1] << 8)).toString(16).toUpperCase(), build: text };
}

/** Send the version request on an open WebHID device and return {version, build}. */
export function legacyReadVersion(hid) {
  const packet = buildReadRequest();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => done(new Error("the keyboard did not answer in time")), REPLY_TIMEOUT_MS);
    const onReport = (ev) => {
      try {
        done(null, decodeInfoResponse(new Uint8Array(ev.data.buffer, ev.data.byteOffset, ev.data.byteLength)));
      } catch (err) {
        done(err);
      }
    };
    const done = (err, value) => {
      clearTimeout(timer);
      hid.removeEventListener("inputreport", onReport);
      err ? reject(err) : resolve(value);
    };
    hid.addEventListener("inputreport", onReport);
    hid.sendReport(0, assertSendable(packet)).catch(done);
  });
}
