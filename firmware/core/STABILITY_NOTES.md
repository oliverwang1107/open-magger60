# Stability and fail-safes (M5 task T-E, ladder step L3-0)

Original work. Inputs: M5_PLAN.md section 2A (the seven stability rules), our own K3 observations, the repository's own driver code and
tests. Nothing here comes from the stock firmware.

Files: `core/include/km/km_health.h` + `src/km_health.c`, `km_report_sync.h` + `src/km_report_sync.c` (pure logic, no hardware, time is a
cycle counter passed in); `port/n32h48x/{adc_scan,usb_hs,reset_cause,m5_stab_main}.c`; tests `tests/test_health.c`,
`tests/test_report_sync.c`, `tests/sim/stab_sim.h`, additions to `tests/test_sense.c`; tools `tools/magger60_soak.py`,
`firmware/tools/{bench_engine_budget.py,bench_engine_budget.c,mutate_health.py}`. Image: `km_l30` (build only until L3-0 is approved).

## 1. The policy in one page

One rule: **a stopped keyboard is acceptable, a stuck key is not.**

| failure | detected by | what the keyboard does first | how it ends |
|---|---|---|---|
| ADC/DMA chain stops (no frame for 3 ms) | `adc_scan_frame_timeout()` (measured from the frame release, so a slow consumer is not a stall) | SCAN_STALL fault: all-released report at once | `adc_scan_restart()` rebuilds both DMA channels, clears every pending flag, restarts at mux phase 0; typing resumes 30 ms after frames flow again. If frames never come back: no progress, no watchdog feed, reset 2.0-2.5 s after the stall began |
| DMA transfer error | flag noticed when a phase completes, or by `adc_scan_restart()` | frame dropped (never pushed into the sums), DMA_ERROR fault, 30 ms hold-off | resumes after the hold-off with clean frames. An error in every frame = no progress = reset |
| calibration missing / refused / lost | `km_health_on_calibration()`, `km_health_cal_lost()` | CAL_LOST fault (set at boot) | cleared by the next accepted calibration |
| USB suspend | `DSTS.SUSPSTS` as a level, every pass | USB_SUSPENDED fault, link considered down (nothing is sent) | on resume the sync layer **forces** the current state to the host even if it equals the last one (the host may hold a key from before the suspend) |
| USB bus reset / re-enumeration | `km_usbhs_get_events().resets`, `km_usbhs_configured()` | configuration dropped by the driver; link down | link up again = forced resync |
| a report is refused by the driver | `send()` returns false | - | retried every pass for ever (counted in `report_retries`) |
| a report is accepted but never reaches the host | - (cannot be seen) | - | while a key is down the state is repeated every 500 ms (heartbeat); after a release the release is repeated once after 500 ms (echo) |
| IN endpoint stays busy | `tx_ready()` false with a report waiting for 100 ms | - | `km_usbhs_in_recover()` (disable endpoint, flush FIFO, free the busy flag), repeated every 100 ms |
| endpoint cannot be repaired | report due for 1 s with the link up (`km_report_sync_stalled`) | all-released was already requested | the USB progress flag is withheld, the watchdog stops being fed, reset about 3.5 s later; the reset drops the USB connection, which releases every key on the host |
| engine update far too long (> 60 000 cycles = 250 us) | `km_health_on_engine()` | ENGINE_OVERRUN fault, 30 ms hold-off | resumes after the hold-off |
| main loop or USB poll hangs | no progress | (cannot act) | no feed: reset after 2.0 s (the watchdog period) |
| unexpected exception | `Default_Handler` loops without feeding (app_startup.c) | - | reset after 2 s |

"All-released" is not a one-shot: while any fault flag is set, `km_report_sync_pump()` sends the all-released report as the *target*, so a
release that fails is retried until it is accepted, and a key that is still physically down is NOT reported until the faults are gone.

## 2. The watchdog policy, and its false-reset risk

`km_health_should_feed()` is true only if both the scan side (`km_health_on_frame`) and the USB side (the poll ran **and** the
report endpoint is not dead) made progress within the last 500 ms. The loop calls `km_iwdg_feed()` only then; it is the only feed in the
main loop. The IWDG period is 2 s, so a stall of either side ends in a reset between 2.0 s and 2.5 s after it began (the last feed can
happen up to 500 ms after the last progress). The bootloader that runs after the reset restarts its own watchdog and jumps back into the
app, so this is a restart (about one second of dead keyboard plus re-enumeration), never a brick; ESC + left CTRL rescue still works.

False-reset risks, honestly:
* **USB side, 1 s rule.** If the host stops taking reports from the keyboard endpoint for 1 s while the bus is NOT suspended (a hung host
  controller driver, a hub in trouble), the keyboard resets itself. Cost: one extra re-enumeration. Benefit: the key state on the host
  is cleared. Suspend is excluded because the link is down then. This is the rule most likely to fire without a real fault; the counters
  `boots` / `uptime` / `prev_faults` (scope tag 0x0300) and the soak tool's FIRMWARE_RESET event show it.
* **Scan side.** The 3 ms frame timeout cannot fire because the main loop is slow (it is measured from the frame release and a ready
  frame never counts). It CAN fire if something blocks the CPU or the scan interrupts for more than 3 ms. Today nothing does; the flash
  writes of M5 task T-D will (a sector erase stalls the bus): the store code must run with keys not allowed and call
  `adc_scan_restart()` afterwards, which is cheap and idempotent.
* **Engine and calibration** are deliberately not part of the feed rule (a calibration that keeps failing because a key is held must
  not reset the keyboard).
* The 500 ms window and the 1 s endpoint rule are constants (`KM_HEALTH_FEED_WINDOW_CYCLES`, `KM_SYNC_DEAD_CYCLES`); both are exercised
  at their exact boundaries by the host tests, and by `mutate_health.py` (51 mutations, no survivor that is not documented as equivalent).

## 3. Observability

* Scope tag **0x0300** (24 values) health counters and reset cause, layout in `km_health.h`; **0x0301** (8 values) the keys the
  keyboard is currently reporting as a 76-bit mask + sequence + fault flags + "undelivered" flag. 0x0301 is published every 20 ms,
  0x0300 every 100 ms (`m5_stab_main.c` has the rotation). `tools/magger60_soak.py` compares 0x0301 with what Windows sees.
* Reset cause: `reset_cause.c` reads the top byte of `RCC_CTRLSTS` (0x40021014, register name and address from the SVD-derived
  peripheral table of this repository) and clears it. **The flag bit positions are UNVERIFIED for the N32H48x.** The code is harmless if
  they are wrong: the clear request is written only if one of the assumed flag bits was set, and a different layout just gives an odd
  number. The bootloader runs first and may already have cleared the flags; "none" is a normal reading.
* Breadcrumb: a 16-byte RAM record at 0x20025F00 (unused RAM; the linker script puts the stack right behind .bss, ending at
  0x20005B28) keeps the fault flags that were active before the last restart and a boot counter. **UNVERIFIED** that the bootloader
  leaves that RAM alone; if it does not, `prev_faults` reads 0xFFFF ("none known").

## 4. Test evidence (host)

`python firmware/tools/run_tests.py`: suites `report_sync` and `health` plus additions to `sense`.
* Unit tests of the exact boundaries of every constant, both with the cycle counter near 0 and a few thousand cycles before it wraps.
* Fault-injection scenarios (`tests/sim/stab_sim.h` models the endpoint, the host, the scan chain and the 2 s watchdog; `sim_pass()` is the
  same call sequence as the main loop): lost reports, a lost release (healed by the echo), endpoint busy for ever (recovery frees it;
  recovery impossible = watchdog reset about 3.5 s after the failure), a release refused 1000 times then accepted, bus reset with a key
  down, suspend with a key down that is released while suspended, DMA error burst, scan stall that recovers and one that does not
  (feed stops, reset in 2.0-2.6 s), USB poll dead, calibration lost while keys are down, engine overrun, and a random mix of all of them
  (60 s of simulated time per run). Property checked each time: no key on the host during a fault, no stuck key after the keyboard
  released it.
* What is NOT covered: two reports lost in a row without any error (beyond what a heartbeat can heal), and everything that depends on
  the real DWC2 core (see section 6).

## 5. The ~10.7 k-cycle engine update

Measured on the keyboard in K3: usually about 3.4 k cycles per `km_engine_update()` call, occasionally up to about 10.7 k, nearly the whole
frame budget (10.4 k cycles = 43 us).

`python firmware/tools/bench_engine_budget.py` runs the real `km_engine.c` over seven signal scenarios built on the real per-key endpoints of
`core/data/slot_ranges_s2.json` plus +-15 count noise (idle, typing strokes, all 61 keys at once, thermal drift of every baseline, a settings
change that dirties every key, the worst case of the dirty-slot search, supply glitches), replays each five times and keeps the fastest
run of every frame (removes host scheduling noise), counts depth-model calls with the linker's `--wrap`, and converts host time to M4
cycles by anchoring the median idle frame to 3000 cycles (the 2.6-3.3 k that `ENGINE_NOTES.md` derives from the compiler output). That
conversion is an estimate; a second column models each frame from its parts (3000 base + 22 per dirty-search step + 45 per depth-model
call + 40 per drift look of a key).

Result (this run, mean cost of frames of each class, M4 cycles estimated by the anchor / by the model):

| frame class | share of frames | anchor | model |
|---|---|---|---|
| normal | ~94 % | 3.0 k | 3.0 k |
| **drift look** (every 16th frame, all keys) | 6.25 % | 6.4-6.9 k | 5.4 k |
| threshold recompute (22-23 depth-model calls) | ~0.1-0.7 % | 6.0-8.2 k | 4.1 k |
| **drift look + recompute in the same frame** | ~0.01-0.6 % | **10.5-11.5 k** | 6.5 k (7.9 k with the 61-step search) |

**Cause (most likely, from the code and the bench):** two independent pieces of work in `km_engine.c` that both fall on the same frame now and then.
1. `drift_step()` runs for **every** idle key on every 16th frame (`drift_tick`), i.e. 61 keys' worth of work (~25 instructions each, ~2.4 k
   cycles) on one frame out of 16: that frame costs about twice a normal one.
2. `refresh_one()` recomputes the thresholds of one dirty slot per frame: two bisections of about 11 depth-model calls each (22-23 calls,
   ~1.2-1.5 k cycles on the M4), after a search for the dirty slot that does a 32-bit modulo (`% n_active`) per step and takes up to 61
   steps (~1.3 k cycles) when the only dirty slot is the farthest one from the cursor.
A drift look that coincides with a recompute is the heaviest frame the engine can produce without pressing anything: 3.0 k + 2.4 k + 1.5 k
(+ 1.3 k search) = 6.5-7.9 k by the model, 10.5-11.5 k by the anchored host ratio. Together with the scan interrupts that run inside the timed
window (the next frame is started before the engine runs), the 10.7 k seen on hardware is consistent with it. The two estimates bracket the
observation; they do not prove it. Recompute frames are common after boot, after `km_engine_set_key_cfg`, while a key learns its bottom
(`learn_step` marks it dirty every frame it descends), and when baselines drift together (one dirty slot per frame for ~61 frames).

**Hardware proof, L3-0:** the image now times the engine **net of the scan interrupts** (`adc_scan_irq_cycles()` is read before and after)
and publishes both: scope 0x0300 value [6] = engine's own longest update, [23] = longest including interrupts, and 0x0100 value [19] = longest
interrupt time inside one update. If [6] stays below about 8 k while [23] reaches 10.7 k, the rest is interrupt time.

**Recommended fix (not applied: `km_engine.c` belongs to task T-A):**
1. Spread the drift look: slot `s` is looked at when `((frame + s) & 15) == 0` instead of all slots when `(frame & 15) == 0`. Same period per key,
   about 4 keys per frame, ~160 cycles per frame instead of ~2.4 k on one. This alone removes the 6 k class.
2. Make the dirty search O(1): keep the dirty slots in a small ring buffer (queue of slot indices, filled by `mark_dirty`) instead of searching
   with a modulo per step; at least replace `% n_active` by an increment with wrap.
3. Do not recompute in a frame that also has the (already small) drift work, or split the recompute: the press edge in one frame, the release
   edge in the next (halves the 22 calls). A cheaper alternative: start each bisection from the previous threshold +- a few counts (a
   baseline move of a few counts needs 2-3 model calls instead of 11).
Expected worst frame afterwards: 3.0 k + ~0.2 k + ~0.8 k = about 4 k cycles.
`km_health_on_engine()` counts every update above 10 400 cycles in `engine_over_budget` (a statistic) and raises the ENGINE_OVERRUN fault only above
60 000 cycles, so today's 10.7 k outliers do not interrupt typing.

**Fixed (M5 T-F part 5, `km_engine.c`).** Items 1 and 2 of the recommendation are applied; item 3 is not needed after item 1.
* The baseline look is spread: slot `s` looks when `(frame + s) % 16 == 0` (each key still every 16th frame, so the dynamics are the same, only the
  phase differs), 4-5 keys per frame instead of all 61 on one. Modelled cost of that work: 160-200 cycles per frame instead of 2.4 k on one in 16.
* The dirty search in `refresh_one()` is an increment with wrap, not a 32-bit modulo per step (model: 8 cycles a step instead of 22; the farthest dirty
  slot costs about 0.5 k instead of 1.3 k). A dirty queue was not needed. T-A's change of the rapid-trigger depth-model calls
  (`a44d173`) is in other functions (`rapid_up` / `rapid_down`) and does not overlap.
* Modelled worst frame (bench model): 4.8 k cycles (4 baseline looks + a recompute with its 22 depth-model calls + the full 61-step search), was 7.0 k
  by the same model and 10.5-11.5 k by the host-anchored estimate. Re-run `python firmware/tools/bench_engine_budget.py` to see the table.
* Regression check: `tests/test_engine_budget.inc` counts the work of every frame (baseline looks per frame, recomputes per frame) on the real engine
  and prices it with the bench model; it fails if the look is concentrated on one frame again (verified by putting the old code back) or the
  worst modelled frame exceeds 6 k. Host TIMING is too noisy for a test, so the check is on work, not time.
* NOT proven: that the 10.7 k seen on the keyboard really was this coincidence (section 5 above said "most likely"); that is settled by the L3-0
  counters `[6]` / `[23]` / `0x0100[19]` after the fix, which should now show an engine maximum near 5-6 k instead of 10.7 k.

## 6. Unverified until L3-0 on the real keyboard

* The ordering and effect of `adc_scan_restart()` on a real stalled chain (use `km_l30s`: stalls once at 20 s, must recover and type again;
  `km_l30d`: restarts disabled, must send all-released and be reset by the watchdog in about 2.5 s, then recover).
* The DMA transfer-error flag position (`SENSE_DMA_TEIF_*`, "assumed STM32-style" since M3) - the DMA error path has never seen a real error.
* `GINTSTS.USBSUSP` / `WKUPINT` / `DSTS.SUSPSTS` behaviour with a real host sleeping and waking (the suspend level is what the fail-safe uses;
  the event counters are statistics).
* `km_usbhs_in_recover()`: the DWC2 NAK / disable / flush sequence is written from the programming model, not tested. It only runs after an
  endpoint has been busy for 100 ms with a report waiting; the dead-endpoint rule is the backstop.
* `RCC_CTRLSTS` reset flag layout and RAM survival of the breadcrumb (section 3).
* That `adc_scan_irq_cycles()` bookkeeping (4 extra `CYCCNT` reads per frame, ~60 cycles) does not change the frame time noticeably.
* That the 1 s endpoint rule and the 500 ms feed window never reset the keyboard in normal use (8 h soak with `tools/magger60_soak.py`).

## 6a. Flash writes of the calibration store (M5 T-F)

The calibration record (`CALIBRATION_NOTES.md`) is the first flash write that happens while the keyboard is in use, so it follows the rules of this
document: it starts only after 1 s without a key, without a fault flag and without a report waiting; it is a job of one flash operation per main loop
pass (an 8-byte program, or once in 24 saves the 8 KB sector erase), paused while a key is down or a fault is active; `adc_scan_restart()` is called
after every operation (this is the "store code must call `adc_scan_restart()` afterwards" of section 2); the progress-based IWDG feed is untouched,
`store_flash.c` feeds inside its bounded waits. The one non-pausable stretch is the 40 programs after the erase (otherwise no valid record exists
while the user types). The settings save (`COMMIT`) is still a blocking call and may still show up as a scan stall afterwards; the calibration path
does not. Real erase / program durations are not measured (L3-4).

## 7. The soak tool

`tools/magger60_soak.py --run --duration 28800 --csv soak.csv` (Windows, `hidapi`, our firmware only). See its docstring for the limits:
default-layer mapping only, time resolution of a few tens of ms, the mask is the keyboard's target state, Raw Input shows what Windows decoded.
`--self-test` and `--sim` need no hardware (the self-test also cross-checks the decoders against the real `km_health.c` packers when gcc exists).
