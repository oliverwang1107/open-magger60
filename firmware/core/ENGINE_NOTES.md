# Key engine, key map, reports (M4 task T-B)

Original work; inputs are the public HID spec / usage tables and our own measurements
(`core/data/slot_ranges_s2.json`, `slotmap_measured.json`). Nothing here comes from the stock firmware.

Files: `core/include/km/km_engine.h`, `km_keymap.h`, `km_report.h` and the matching `core/src/*.c`;
tests in `tests/test_engine.c` (+ `test_engine_calib.inc`, `test_engine_run.inc`); tools
`tools/mutate_engine.py` (mutation pass) and `tools/bench_engine.c` (host timing).

## Integration checklist (do this when T-A is merged)

1. `core/include/km/km_depth_model.h` + `core/src/km_depth_model.c` arrive with T-A.
2. Delete `tests/km/km_depth_model.h` (temporary prototype). `tests/test_engine.c` then uses the real
   model automatically (its linear stand-in is compiled only while `KM_TEMP_DEPTH_STANDIN` is defined).
3. In `CMakeLists.txt` delete the `list(FILTER ... km_engine\\.c$)` line and the `target_sources(...)` line, so
   `km_engine.c` is built into `km_core` (it is already compiled for ARM with the temporary header: 2.2 KB
   of code at `-Os`). `km_keymap.c` and `km_report.c` are already in `km_core`.
4. Re-run `python firmware/tools/run_tests.py` and `python firmware/tools/mutate_engine.py`.

## How it works

Per slot and frame: Q4 low-pass (gain 1/4, ~4 frames time constant) -> compare with two threshold
*counts* -> press/release state machine with hysteresis and consecutive-frame debounce (press 3 frames,
release 6 frames). The thresholds are the readings at which `km_depth_model_mm100` reaches the configured
press / release depth. They are found once by bisection (about 11 model calls each) whenever the baseline,
the bottom reading or the trigger settings of a slot change, at most one slot per frame, so the hot loop
never calls the depth model and has no division. Defaults: press 1.20 mm, release 0.80 mm
(`km_engine_key_cfg_t`, per key).

Baseline: taken at boot from a quiet stretch (`km_engine_calibrate_begin/idle/finish`, at least 16 frames,
mean per slot). Afterwards it drifts slowly (time constant ~1.4 s) but only while the key is up and the
reading is within 24 counts of it, and never more than 150 counts away from the boot value.

Bottom reading: starts at baseline - 1000 counts (smallest measured span: 998) and is lowered whenever a
pressed key reads deeper (the first full press teaches the stroke). `km_engine_set_bottom()` loads a stored
value. Until a key has bottomed out once, its 1.20 mm point is at most a little early (up to ~0.3 mm for the
longest-span keys); it is exact afterwards.

Calibration refusals / flags (`km_engine_cal_result_t`):

| result | meaning |
|---|---|
| `KM_ENGINE_CAL_OK` | armed. Slots reading outside 1500..4000 counts (floating ~1360, railed) are disabled; slots that look pressed (350 counts below the median) or moved (max-min > 120) get the median as a guessed baseline (`SLOT_ASSUMED`) that converges once the key rests |
| `TOO_FEW_FRAMES` | fewer than 16 frames fed |
| `KEYS_HELD` | more than 6 slots look pressed, or the median level is below 2500 (most keys down) |
| `UNSTABLE` | more than 6 slots moved during the stretch |
| `IMPLAUSIBLE` | fewer than 30 usable key slots |

On any refusal the engine stays unarmed (`km_engine_update` does nothing); call `begin` and retry.

Crosstalk / glitch guards: the 15 slots without a key (8 ADC4 mirrors, 7 floating inputs) are masked by the
key map and never evaluated; a slot reading below 1500 counts (lower than any real bottom, ~1800) is forced
up and re-armed only when it reads like a released key again; 8 or more simultaneous new presses in one frame
are rejected as a glitch (supply dip, common-mode shift), counted in `km_engine_glitch_count()`.

Trigger algorithms: `trigger_fixed()` (above) and `trigger_rapid()` (section "Rapid trigger" below) are the only
places that decide press/release; `km_engine_key_cfg_t.mode` selects per key (FIXED = 0, RAPID = 1, anything
else is rejected and falls back to the FIXED defaults).

## Rapid trigger (M5 task T-A)

Per key, `km_engine_key_cfg_t.mode = KM_ENGINE_TRIG_RAPID` selects it; `KM_ENGINE_TRIG_FIXED` (default) is
unchanged. Everything below is in `km_engine.c` (`trigger_rapid()`, `rapid_up()`, `rapid_down()`); tests in
`tests/test_engine_rapid.inc`. Depths are in 0.01 mm of travel (0..340) as given by the depth model.

### Settings

| field | meaning | valid |
|---|---|---|
| `rt_down_mm100` | press after this much downward travel from the shallowest point of the stroke | 20..340 |
| `rt_up_mm100` | release after this much upward travel from the deepest point | 20..340 |
| `top_dead_mm100` | no press above this depth; falling back into it releases | 0 (= 20) or 20..340 |
| `bottom_dead_mm100` | the last this much of travel counts as fully pressed | 0 (none) or 20..340 |
| `continuous` | 1: may be pressed again anywhere until the key is back in the top zone | 0 or 1 |
| `press_mm100` | only with `continuous = 0`: the actuation point | 20..340 |

Also required: `rt_down <= 340 - bottom_dead` (a press must be reachable) and
`max(top_dead, 20) + bottom_dead + 20 <= 340` (the zones leave room). `km_engine_cfg_valid()` rejects everything
else, including any other `mode`, `continuous > 1` and values left over from erased flash. FIXED configs do not
look at the rapid-trigger fields, and continuous RAPID configs do not look at `press_mm100` / `release_mm100`.
`km_engine_set_key_cfg()` refuses an invalid config and changes nothing. `km_engine_apply_key_cfg()` (for
configs from storage or the host) puts the **FIXED defaults** on the key instead and returns false. The threshold
recompute does the same if it ever finds a corrupt config in memory. A config change takes effect at once
(thresholds recomputed, memory of the stroke cleared); a key that is down stays down and is judged by the new
rules from then on.

### The rules

Let `d` be the depth of the key (see "Signal path" for which reading), `ext` the memory of the stroke,
`td = max(top_dead, 20)`, `H = 10` (KM_ENGINE_RT_EDGE_HYST_MM100). `ext` is kept as a reading (counts), not as a depth:
the depth model never increases with the reading, so "the shallowest depth since ..." is the depth of the highest
reading and "the deepest" the depth of the lowest one; everything below is stated in depths.

* **Key up.** `ext` is the shallowest depth since the key last rose: it is reset to 0 while the key reads
  at rest (depth < 0.05 mm) and lowered whenever `d` is below it. The key is pressed when, for
  KM_ENGINE_PRESS_FRAMES (8) consecutive frames, `d > td` **and** `d >= ext + rt_down`
  **and** (`continuous` or `d >= press_mm100`) and the fast filter does not contradict (below).
  From rest that is a press at `max(td + 1, rt_down)`.
* **Key down.** `ext` becomes the deepest depth since the press. The key is released after
  KM_ENGINE_RELEASE_FRAMES (6) consecutive frames in which either
  * `ext - d >= rt_up` (reversal), or
  * `d <= td - H` (back in the top zone, 0.10 mm inside its edge; the hysteresis keeps a finger resting on
    the edge from pressing and releasing by noise), and the fast filter does not contradict.
  After a release `ext` starts again as the shallowest point: from `d` (reversal), or `td - H` (top zone).
* **Bottom zone.** While `d >= 340 - bottom_dead` the key counts as fully pressed: `ext = 340`, no reversal can
  release it. Coming back up, it is released where it leaves the zone or after `rt_up`, whichever is deeper:
  at depth `340 - max(bottom_dead, rt_up)`.
* **continuous = 1**: after a reversal release the key can be pressed again at any depth below the top zone,
  `rt_down` below its shallowest point; it only starts from scratch when it is back in the top zone (the
  rest region resets `ext`).
  **continuous = 0**: the same, but a press (the first one and every later one) additionally needs
  `d >= press_mm100`: rapid trigger only works below the actuation point; above it the key behaves like a
  normal key with that actuation point and has to pass it again. (This is what the UI model in
  `ui/js/core/trigger.js` calls "rapid"; its "continuous" additionally requires one pass of the actuation
  point before the first press, the engine does not, as specified for this task.)
* Other protections are the same as for FIXED: debounce 8 / 6 frames, the glitch guard (8 presses in one frame are
  all dropped, and a rapid key then qualifies again from the top), faults (a slot below 1500 counts is forced up and
  re-armed only near rest), baseline drift tracking while the key is up and at rest, bottom learning while down,
  disabled slots forced up. No loop in the rapid path is unbounded; the only loop-like code is the bisection of
  the cold recompute (11 steps).

### Signal path and why the floor is 0.20 mm

Rapid trigger remembers extremes of its own input, so it measures the **peak-to-peak noise** of that input,
not its standard deviation. The sensor noise is +-15 counts uniform (sigma 8.7 counts); the depth model's slope
at rest is `T (1 + a) / span` = 1.02 (span 998) .. 0.75 (span 1365) units of 0.01 mm per count, so +-15 counts are
at most +-15 units.

* The engine's main filter (gain 1/4) leaves sigma 3.3 counts and excursions up to 12. A reversal detector on
  it needs more than the p-p of about 24; simulated with 15-40 minutes of noise: a pressed key held in the
  steepest part of the travel is released by noise after 1-10 s with `rt_up = 20`, after minutes with 24, never
  (600 s x 4) with 28.
* Rapid trigger therefore uses a second low-pass stage behind it (`g`, gain 1/8, snapped to the main filter while
  the key reads at rest): sigma 1.9 counts, largest excursion 7.7, p-p about 15 counts = about 15.7 units at the
  steepest slope. Simulated 900 s x 6 with `rt_up = 20` and a 6-frame debounce: no false release. The
  floor of 20 units therefore has 4 units (25 %) of margin on the worst-case key and much more on all others.
  That is the derivation of `KM_ENGINE_RT_MIN_MM100 = 20`. Lower values are refused.
* Deeper in the travel the slope drops (0.25 at mid travel, 0.11 near the bottom), so the same counts of
  noise are a fraction of the floor; the floor is set by the top 0.5 mm.
* Costs of the second stage: group delay about 10 frames (0.43 ms) instead of 3, so a rapid-trigger key
  needs about 0.8 ms from the real crossing to the press. A full-scale jump of the reading is a press after 8 frames.
  Reversals that last less than about 1 ms (a few frames) are smoothed away; real fingers cannot do that.
* The noise also inflates the remembered extreme by up to about 8 counts, so the effective `rt_up` of a key
  held in the top 0.5 mm is up to 0.08 mm smaller than set (measured: release at 133 instead of 130 on a
  150 stroke with rt_up 20). `rt_down` is early by the same amount.
* The fast filter still has a vote ("direction guard"): a press needs `f <= g + 8 counts` and a release
  `f >= g - 8 counts`, i.e. the key must still be moving that way or holding. This rejects the tail of a
  one- or two-frame spike (the slow filter would otherwise keep the key "pressed" for 15 frames after the
  spike). Spikes of three frames and longer, as with a FIXED key that has a threshold this small, are not
  rejected (they look like a very fast finger and are bounded by the glitch guard only).

### Cost

See "Timing estimate" below. The depth model is not called for a key at rest, for a key held in the bottom zone, or
for a key that is held or only wiggles by noise: `ext` is a reading, and a press or release needs a stroke of at least
`mc_dn` / `mc_up` counts away from it. Those two numbers (recompute, cold) are the counts that the depth `rt - 1`
takes from rest; because the depth curve is concave and rises most at rest, no stroke starting anywhere can reach
`rt` in fewer counts (the `- 1` covers the rounding of the depth). So the stroke test is only a necessary condition:
when it passes, the same depth comparison as before decides, so press and release points are unchanged. The test
costs a subtract and a compare; the model (two calls, the extreme and the current reading) only runs in the few
frames in which a stroke of rt size is actually under way (the 8 / 6 frames of the debounce).
Checked against the version without the test (commit a441c13) on 60 random scenarios of 76 keys x 6000 frames
(random configurations, random-walk fingers, 360,000 frames): the pressed list differs in 23 frames, all one key a
frame or a few frames early or late, caused by the places where `ext` is now a reading (a threshold recompute
in mid-stroke maps the same reading to a slightly different depth; after a top-zone release the memory
starts at the reading of the zone edge instead of the depth `td - H`, which no reading has exactly).

## Reports

Both builders take the pressed slots in press order (`km_engine_down_slots()`: oldest first, simultaneous
presses in ascending slot order) and look them up on layer 0.

* Boot report, 8 bytes: modifiers, 0, six key usages in **press order** (stable while keys stay down);
  more than six distinct non-modifier keys -> all six = `0x01` (ErrorRollOver), modifiers still reported.
* NKRO report, **29 bytes**, no report id, no separate modifier byte: bit `usage & 7` of byte `usage >> 3`,
  usages 0..231 (232 bits; modifiers 0xE0..0xE7 are bits 0..7 of byte 28; bits for usages 0..3 are always 0).
  Matching descriptor items: Usage Page (Keyboard), Usage Minimum (0), Usage Maximum (231), Logical
  Minimum (0), Logical Maximum (1), Report Size (1), Report Count (232), Input (Data, Variable, Absolute).

Key map layer 0: ANSI 60% with Esc in the grave position (there is no grave key on this board); the Fn
key (slot 14) is a physical key with usage 0 (no-op); the 15 non-key slots have usage 0.

## Timing estimate for `km_engine_update` (76 slots, 61 active)

* Host (x86-64, `-O2`, `tools/bench_engine.c`): ~130 ns per frame (about 2 ns per active slot).
* Cortex-M4F, `-Os`/`-O2` code read from `arm-none-eabi-gcc` output: the common path of an idle key
  (load sum, clamp, Q4 filter, store, compare, debounce counter reset, loop) is about 32 instructions,
  ~40-50 cycles on the in-order M4 with taken branches and load-use stalls. 61 active slots ~2.4-3.1 k
  cycles, plus the drift look every 16th frame (~25 instructions per slot, ~100 cycles/frame averaged) and
  ~60 cycles of bookkeeping: **about 2.6-3.3 k cycles per frame, i.e. 11-14 us at 240 MHz, 25-32 % of the
  43.5 us frame period at 23 kHz**. A frame in which a threshold recompute runs adds ~11 depth-model calls
  x2 (T-A quotes its own cost). This is an estimate, not a hardware measurement. If the CPU budget is
  tight, call `km_engine_update` on every second frame and halve `KM_ENGINE_PRESS_FRAMES` / `RELEASE_FRAMES`.
* RAM: `km_engine_t` is about 4.8 KB since T-A (76 slots x 52 B + calibration scratch 0.8 KB + lists; the slot
  grew by the 16-byte config and 12 bytes of rapid-trigger state).
* **Cost of rapid trigger (T-A), measured on the host and counted in the Cortex-M4 disassembly of the `km_k3` build
  (`-Os`, 3.4 KB of code for the engine, up from 2.2 KB):** (host `tools/bench_engine.c`, ns per frame, 61 keys)

  | case | before T-A | FIXED keys | RAPID keys | RAPID, first T-A version | depth-model calls per frame (RAPID) |
  |---|---|---|---|---|---|
  | all keys up, noise only | 95 | 111 | 133 | 133 | 0 (2.5) |
  | 13 keys held still (down one after the other) | 94 | 113 | 144 | 160 | 0 (8.0) |
  | 13 keys moving (strokes of 2.5 mm, 8 counts per frame) | 98 | 115 | 168 | 181 | 7.8 (15.2) |
  | all 61 keys moving (not a real use) | 97 | 115 | 299 | 352 | 36.9 (61) |

  (Calls per frame in brackets: first T-A version. The host is a PC, so only the differences between the columns
  mean something.)
  On the M4 (instruction counts of the `-Os` build): the dispatch FIXED / RAPID and the 52-byte slot stride are 5 more
  instructions for every active key (about +430 cycles per frame for a keyboard that is all FIXED); an idle RAPID key
  runs 5 more instructions than an idle FIXED key (+7 cycles, +430 for 61 keys). So a keyboard with every key on RAPID
  costs about **+850 cycles per frame at idle** (3.4 k -> 4.3 k of the 10.4 k frame budget). A finger holding a key adds
  about 30 cycles (no depth model, the stroke test is a subtract and a compare); in the 6-8 frames of a press or release
  a key runs the depth model twice, +100 cycles per key and frame. So 5 fingers holding ~ +1.0 k in total, the 13
  possible fingers ~ +1.25 k, and in the worst frame (13 keys all in the debounce of a press or release) ~ +2.2 k
  (below the 10.4 k budget; T-E counts frames that run over in `engine_over_budget`). Before the stroke test (first
  T-A version) a held finger cost +75 cycles and 13 fingers ~ +1.9 k. Further saving, if the hardware measurement asks
  for it: remember the depth of the extreme next to its reading (halves the model calls in the debounce frames), or
  run `km_engine_update` on every second frame. The numbers are estimates from instruction counts, not measurements
  on the chip.
## Tests and mutation pass

`python firmware/tools/run_tests.py`: the engine suite has 11,748 checks, 8,397 of them for rapid trigger
(`tests/test_engine_rapid.inc`: validation boundaries; exact press / release depths on noise-free ramps; continuous
and not continuous; top and bottom zones; about a minute of simulated noise on idle and held keys; reversals of 10..100 in
three speeds at two depths; exact debounce latencies; spikes and railed dips; a slow noisy press 40 times; all 61 measured key
endpoints, with known and unknown bottom; drift; glitch, fault, disable, mode switch, corrupt config; 60 random
walks with random configs that must end with the key up and consistent events; an exhaustive check of the stroke test
that sits in front of the depth model, on 12 measured keys and nine pairs of distances; a RAPID start with the key up
and 1.5 mm down). The older part (3,351 checks;
synthetic ramps/steps with
+-15 count noise, spikes and bounce, hysteresis, cfg validation, 6+ keys and rollover, glitch limit, drift,
calibration refusals, disabled/fault slots, mirror slots, all 61 measured key endpoints as ramps, exact
actuation point and exact debounce latency, hand-written report bytes, key map against the measured slot map).
`python firmware/tools/mutate_engine.py`: 116 one-expression mutations of engine, key map and reports
(70 of them for rapid trigger: config validation, floor, zones, filter, direction guard, debounce, extremes,
bottom zone, stroke test, fallback to defaults); 107 are killed, 9 are equivalent (differences of 1/16 count or < 1 count,
or paths that are redundant with another one, each with its reason listed in the script). The first pass
had 8 survivors; they led to the exact-latency / exact-threshold tests, the 12-bit clamp test and the
moving-key limit test. The pass after the stroke test went in
had 4 survivors; they led to the exhaustive stroke-test check and the RAPID-start test.
