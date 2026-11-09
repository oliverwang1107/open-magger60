// WebHID transport for ConfigClient: sends one 64-byte output report (report id 0) and waits for the input report that
// answers it. Requests are strictly one at a time (a queue), every packet goes through assertSendable() first, and
// replies that belong to an older request (other command, or op/token of a different request) are skipped.
import { assertSendable, CMD_CONFIG, REPORT_LEN } from "./config_protocol.js";

export class TimeoutError extends Error {
  constructor(message = "the keyboard did not answer in time") { super(message); this.name = "TimeoutError"; }
}

export class WebHidTransport {
  #hid;
  #timeoutMs;
  #queue = Promise.resolve();

  constructor(hid, { timeoutMs = 600 } = {}) {
    this.#hid = hid;
    this.#timeoutMs = timeoutMs;
  }

  exchange(packet) {
    const run = () => this.#exchange(packet);
    const result = this.#queue.then(run, run);
    this.#queue = result.catch(() => {});
    return result;
  }

  /** Fire-and-forget write (the light stream, command 0xC7): checked like any packet, no reply awaited, not queued behind exchanges. */
  async send(packet) {
    assertSendable(packet);
    await this.#hid.sendReport(0, packet);
  }

  /** cb(report) for every stream reply (0xAA 0xC7) the keyboard sends; returns the function that stops listening. */
  onStreamReport(cb) {
    const hid = this.#hid;
    const on = (ev) => {
      const r = new Uint8Array(ev.data.buffer, ev.data.byteOffset, ev.data.byteLength).slice();
      if (r.length === REPORT_LEN && r[0] === 0xaa && r[1] === 0xc7) cb(r);
    };
    hid.addEventListener("inputreport", on);
    return () => hid.removeEventListener("inputreport", on);
  }

  #exchange(packet) {
    assertSendable(packet); // last line of defence, on the exact bytes
    const hid = this.#hid;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => finish(new TimeoutError()), this.#timeoutMs);
      const matches = (r) => r.length === REPORT_LEN && r[1] === packet[1]
        && (packet[1] !== CMD_CONFIG || r[0] !== 0xaa || (r[8] === packet[8] && r[9] === packet[9]));
      const onReport = (ev) => {
        const r = new Uint8Array(ev.data.buffer, ev.data.byteOffset, ev.data.byteLength).slice();
        if (matches(r)) finish(null, r);
      };
      const finish = (err, value) => {
        clearTimeout(timer);
        hid.removeEventListener("inputreport", onReport);
        err ? reject(err) : resolve(value);
      };
      hid.addEventListener("inputreport", onReport);
      hid.sendReport(0, packet).catch(finish);
    });
  }
}
