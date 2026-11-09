# Dynamic Keystroke, DKS (M6 package P3)

Original work: "Dynamic Keystroke" is a feature name from the public product literature of Hall-effect keyboards; what a
record means, the event rules, the data format (plan section 2.2) and the code are our own. Nothing here comes from the
stock firmware.

Files: `core/include/km/km_dks.h`, `core/src/km_dks.c`, `tests/test_dks.c`, `tools/mutate_dks.py`.

**白話**：一顆鍵，依按壓深度觸發最多 4 個動作。例：淺按（越過起始點）= 按下 `A`；按到底 = 再按下 `S`；從底部放開一點 =
放開 `S`；完全放開 = 放開 `A`。每個動作在 4 個事件各可選「無／按下／放開／點一下」。判定按下／放開仍然由引擎負責（去抖動、
故障保護都照舊）；這個模組只「讀」引擎說按下中的 DKS 鍵的深度。

## The four events

| event | when | who decides |
|---|---|---|
| 1 | the key goes down past `t_act` (default 1.20 mm) | the key engine (FIXED trigger, press = `t_act`) |
| 2 | the key goes down past `t_bot` (0 in the record = travel - 0.30 mm = 3.10 mm) | this module, from `km_engine_depth()` |
| 3 | the key comes back up to `t_bot - 0.20 mm` | this module (hysteresis 0.20 mm, the noise is +-0.15 mm) |
| 4 | the key is released | the key engine (FIXED trigger, release = `t_act - 0.10 mm`) |

Per action and event: 0 nothing, 1 press, 2 release, 3 tap (press now, release `KM_DKS_TAP_MS` = 10 ms later). A tap on an
action that is already held down changes nothing; a tap on a running pulse restarts the pulse.

Rules (each is a test):

* **Fast press.** A key that is already past `t_bot` when the engine reports it gets event 1 and event 2, in that order.
* **Fast release.** A key released while still "bottomed" gets event 3 and then event 4.
* **Event 4 releases everything** of the key that is still down (except a tap pulse that is still running: it ends by itself
  after 10 ms). A record that presses at event 1 and never releases cannot leave a key stuck.
* **Retrigger flag** (`flags` bit0). Plan section 2.2 only says "allow retriggering in the middle of the stroke (default 0)";
  our definition: bit0 = 0: events 2 and 3 happen at most once per press (between event 1 and event 4); bit0 = 1: every
  crossing of the bottom point fires them again without a full release. (The lead may want to name this in the UI.)
* **Visibility.** Two events of one key are applied at least `KM_DKS_STEP_MS` = 4 ms apart (they wait in a queue of 6), so an
  action that one event presses and the next one releases is on the wire for at least that long: a quick slam cannot make a
  press disappear. The first event of an idle key is applied at once, no added latency. If a queue ever fills up, everything
  in it is applied at once, in order (nothing is dropped; `queue_flushes` counts it).
* **Union.** The output is the union of all down actions of all keys; the same usage on two actions or two keys is listed
  once and stays down until the last holder lets go.
* **Ignored keys.** A key whose record index is out of range or unusable is ignored (nothing remembered, nothing output);
  more than `KM_DKS_KEYS` = 8 DKS keys down at once: the extra ones are ignored (`overflow_presses`), never stuck.
* **Reset.** `km_dks_reset()` drops every action at once and forgets all keys. If a key is still down afterwards it starts a
  new press (events from 1 on, with the depth it has now). For a fault pass an empty list at the same time (the caller does
  that anyway when keys are not allowed).
* **Settle time.** After the last DKS key is released the output is empty within `KM_DKS_MAX_SETTLE_MS` = 42 ms, provided the
  module is called at least every 2 ms (it is called on every main-loop pass, tens of microseconds apart).

## Validity (`km_dks_rec_valid`)

`t_act >= 40` (0.40 mm); effective `t_bot` (0 = 310) with `t_bot - t_act >= 40` and `t_bot <= 340`; `flags` 0 or 1;
`rsvd[0..2] = 0`; every action usage 0 (unused) or `0x04..0xE7`. The `ev` bits of an action with usage 0 are ignored (the
plan does not say; if P0 wants a canonical form it can refuse them in its decoder, this module accepts both).
`km_dks_set_records(n <= 8)` accepts a set only when every record is valid; otherwise **no** record is used (DKS off) and
false is returned, the same "a bad configuration never half works" rule as the engine and the SOCD filter. It always drops
the running state.

## Public API (`km_dks.h`)

```c
typedef struct { uint8_t slot, rec; } km_dks_press_t;                  /* a pressed slot whose latched entry is KM_CODE_DKS */
typedef uint16_t (*km_dks_depth_fn)(void *ctx, unsigned slot);          /* km_engine_depth(&engine, slot) */
typedef struct { uint8_t usage[KM_DKS_OUT_MAX]; uint8_t n; } km_dks_out_t;

void     km_dks_init(km_dks_t *d);
void     km_dks_reset(km_dks_t *d);                                     /* fault, USB reset, profile / table change */
bool     km_dks_rec_valid(const km_dks_rec_t *r);
bool     km_dks_set_records(km_dks_t *d, const km_dks_rec_t *recs, unsigned n);
bool     km_dks_engine_points(const km_dks_rec_t *r, uint16_t *press_mm100, uint16_t *release_mm100);
uint16_t km_dks_t_bot(const km_dks_rec_t *r);
unsigned km_dks_update_ms(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now_ms,
                          km_dks_depth_fn depth, void *ctx, km_dks_out_t *out);
unsigned km_dks_update_cycles(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now_cycles,
                              uint32_t cycles_per_ms, km_dks_depth_fn depth, void *ctx, km_dks_out_t *out);
unsigned km_dks_merge(uint8_t *kbd, unsigned n_kbd, unsigned cap, const km_dks_out_t *dks);   /* append, no duplicates */
bool     km_dks_busy(const km_dks_t *d);
```

`km_dks_t` is about 1 KB of RAM (8 records x 16 bytes, 8 key entries x 40 bytes, the 76-byte slot map, the cached output);
`km_dks.c` is about 1.7 KB of code (`-Os`, Cortex-M4). `km_dks_update_cycles` turns a CPU cycle counter into milliseconds
(one division per pass, wrap safe) exactly like `km_layers_update_cycles`; `cycles_per_ms = 0` means "no clock" and counts
every pass as one 4 ms step, so queued events still drain (the timings are then meaningless, nothing sticks).

## Integration (P10)

1. Profile load: `km_dks_set_records()` with the profile's DKS section (0x10). For every `KM_CODE_DKS` table entry
   `apply_profile()` sets the engine trigger of that slot to FIXED with `km_dks_engine_points()` (press = `t_act`, release =
   `t_act - 10`). A DKS key can not be a rapid-trigger key.
2. Every main-loop pass, after `km_layers_update_*`: build `km_dks_press_t` for the pressed slots whose latched entry is DKS
   (the P0 accessor in `km_layers` gives code and arguments of the entry a pressed slot is latched to; `rec` = arg0), call
   `km_dks_update_cycles(..., km_engine_depth ...)`, then `lo.kbd_n = km_dks_merge(lo.kbd, lo.kbd_n, cap, &dks_out)` before the
   report is built (the merge keeps press order and skips usages that are already there).
3. `km_dks_reset()` together with `km_layers_reset()` on a fail-safe release, a USB reset or suspend, and a profile or table
   change. A layer change by itself needs nothing: the layer logic latches the entry of a pressed key until it is released, so
   a DKS key stays a DKS key, and a key that is no longer listed as DKS is simply treated as released (events 3 and 4).
4. SOCD: a DKS key that the SOCD filter suppresses is, for the layers, not pressed, so DKS sees a release (events 3 and 4);
   when it comes back it is a new press.
5. `d.events_applied`, `d.queue_flushes`, `d.overflow_presses`, `d.resets` are for the scope tag 0x0500.
6. Register `test_dks()` in `tests/test_main.c` / `tests/km_test.h` (P10 owns those two files).

## Tests (`tests/test_dks.c`)

Depth sequences go through a small model of the engine's press (at `t_act`) and release (below `t_act - 10`) with one pass per
millisecond, and through the REAL key engine and depth model (synthetic sensor frames with +-14 counts of noise, one frame =
43.3 us, the module called through `km_dks_update_cycles`):

* slow press and release (exact event times against the ramp), fast press, fast release, partial press (A only), no press at
  all, stopping between the start and release points;
* bottom, back up past `t_bot - 20`, bottom again with and without the retrigger flag; a new press starts afresh;
* hovering around `t_bot` with +-19 (20,000 passes): S goes down once and stays; 291 holds, 290 releases, 309 does not press,
  310 does; an unknown depth (0) is not "back up";
* taps: length exactly 10 ms, a pulse is not cut by event 4, a slam (down and up in 2 ms) still shows each tap for 10 ms, a tap
  on a held action changes nothing;
* event 4 releases everything; a release-only record; a record without actions;
* union: one usage on two actions, on two keys, "press while another action is held", 40 quick press/release cycles of one key
  while its events are still queued; a record change while the key is down;
* the whole thing again with the millisecond counter wrapping in the middle; a key held for 33 days;
* limits: nine DKS keys, bad record indices and slot numbers, a key listed twice, no depth callback, a full queue (flush),
  `km_dks_reset()` also with taps, reset while a key stays down;
* **400 random runs** (random valid records, 10 keys, depth jumps and walks, resets, passes 1-2 ms apart; 3,000 passes each) with
  at every pass: usages unique and from the table, at most 32, the internal books (slot map, counts, output = union of what the
  keys hold) correct; "nothing down for the settle time means output empty and nothing tracked"; after a reset the next output
  is empty; after releasing everything the output is empty within the bound;
* record validity (every boundary, all 256 usage values on all 4 actions), set rejection rules, event mask, engine points,
  merge (duplicates, capacity), cycle wrapper (5 ms in 0.1 ms passes reads 5, remainder kept, no clock).

`python firmware/tools/mutate_dks.py`: 133 one-expression mutations of `km_dks.c` (records, applier, queue, press closing, key
table, output cache, generator, hysteresis, taps, spacing, clock); it takes about 30-40 minutes. The first full pass had 10
survivors; they led to the tests "press after a tap", "second tap restarts the pulse", "events nobody asked for are not
applied", "two keys with different records", the white-box flush check and the exact tap-length check, and to removing one piece
of dead code. After that every one of them was killed (re-run one by one), except four that cannot change behaviour, listed with
their reason in the script (`EQUIVALENT`). Not repeated as one complete pass after the last test additions (tests only got stricter);
run it again before relying on it. Full suite with `test_dks()` and `test_socd()` registered: 35,456,691 checks, 0 failures.

## Cost (estimate from the Cortex-M4 `-Os` disassembly, not a measurement on the chip)

Counted as in `ENGINE_NOTES.md` and `SOCD_NOTES.md`: instructions of the build, loads 2 cycles, taken branches 2-3 cycles, a
call of `km_engine_depth` about 55-60 cycles (the engine notes).

| case | cycles per pass |
|---|---|
| nothing pressed, nothing to finish (the common case) | about 70 (copy of the cached output) |
| one DKS key down, record has events 2 / 3 (depth read each pass) | about 250 |
| four DKS keys down, all with depth reads, steady state | about 750 |
| a pass in which an event is applied | + about 170 per key and event, + up to 120 once to rebuild the output |
| four keys, two of them applying an event in the same pass (the worst realistic pass) | about 1.2 k |
| **absolute worst: eight keys, all with depth reads, all applying an event in the same pass** | **about 3.0 k** (cannot really happen: events of different keys are spread over time) |

The plan estimated "about 300 for four DKS keys" (section 2.5); the honest number for four keys held with depth reads is
about 750, still under 8 % of the 10.4 k frame, and the plan's goal "average per pass <= 1 k, worst <= 3 k" holds. The depth
callback is only called for tracked keys whose record has an action on event 2 or 3 (a record with only events 1 and 4 costs
about 60 cycles per key and pass). Everything is bounded by `KM_DKS_KEYS` = 8 entries and 4 actions; the work per entry of
the input list is a table lookup (the 76-byte slot map), so a long list costs about 22 cycles per entry. No allocation, no
division in the pass (the cycle wrapper divides once), no loop over the 76 slots.

## What this does not do (on purpose)

* It does not decide when a key is down or up: that is the engine, so glitch, fault, debounce and drift protection all apply.
* It does not change layers: a DKS key is a table entry like any other (`KM_CODE_DKS`, P0/P2 own the code), and the layer
  logic latches it until release.
* It keeps nothing across a power cycle: all state is RAM; the records live in the settings blob.
