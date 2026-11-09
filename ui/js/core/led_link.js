// Keeps the keyboard's lights in step with what the page shows, while a slider is being dragged: changes are merged, only one
// LED_SET is on the wire at a time, and the newest wanted state always goes out last. No DOM, no timers beyond the gap below.
//
//   const link = new LedLink({ read, write, onChange, onError });   read() -> state, write(state) -> state the keyboard now has
//   await link.load();        // LED_GET
//   link.set({ speed: 40 });  // merged into the wanted state and sent soon; never throws
//   link.state                // what the page should show: the wanted state while changes are pending, else the keyboard's
//
// A failed write stops the sending, keeps the wanted state, and reports once through onError; the next set() or retry() tries again.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class LedLink {
  #read;
  #write;
  #onChange;
  #onError;
  #gap;
  #sleep;
  #wanted = null;   // newest state the page asked for and the keyboard has not confirmed yet
  #confirmed = null; // last state the keyboard reported
  #running = false;
  #closed = false;
  error = null;

  constructor({ read, write, onChange = () => {}, onError = () => {}, gapMs = 25, sleepFn = sleep }) {
    this.#read = read;
    this.#write = write;
    this.#onChange = onChange;
    this.#onError = onError;
    this.#gap = gapMs;
    this.#sleep = sleepFn;
  }

  get state() { return this.#wanted ?? this.#confirmed; }
  get pending() { return this.#wanted !== null; }
  get loaded() { return this.#confirmed !== null; }

  /** LED_GET. Rejects when the keyboard does not answer; the page shows the error and offers a retry. */
  async load() {
    const s = await this.#read();
    if (this.#closed) return s;
    this.#confirmed = s;
    this.#wanted = null;
    this.error = null;
    this.#onChange(this.state);
    return s;
  }

  /** Ask for a change (a partial state). The page updates at once; the keyboard follows as fast as the USB round trips allow. */
  set(patch) {
    if (this.#closed || !this.state) return;
    this.#wanted = { ...this.state, ...patch };
    this.#onChange(this.state);
    this.#pump();
  }

  /** After an error: send the wanted state again. */
  retry() {
    this.error = null;
    this.#pump();
  }

  /** The page went away: nothing is sent any more (a request already on the wire still finishes). */
  close() { this.#closed = true; }

  async #pump() {
    if (this.#running) return;
    this.#running = true;
    try {
      while (this.#wanted && !this.#closed) {
        const sending = this.#wanted;
        let got;
        try {
          got = await this.#write(sending);
        } catch (err) {
          this.error = err;
          this.#onError(err);
          return;
        }
        this.error = null;
        this.#confirmed = got;
        // nothing newer was asked for meanwhile: the keyboard's answer is the truth (it may have clamped something)
        if (this.#wanted === sending) this.#wanted = null;
        if (!this.#closed) this.#onChange(this.state);
        if (this.#wanted) await this.#sleep(this.#gap);
      }
    } finally {
      this.#running = false;
    }
  }
}
