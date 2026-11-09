# Calibration persistence (M5 task T-F, ladder step L3-4)

Original work. Inputs: M5_PLAN.md (T-F, section 2A, flash layout), `STABILITY_NOTES.md`, `km_engine.h`, the flash behaviour that `flash_boot.c` /
`store_flash.c` already use. Nothing here comes from the stock firmware.

Files: `core/include/km/km_calstore.h` + `src/km_calstore.c` (append-only log in one sector), `km_calib.h` + `src/km_calib.c` (record format,
engine glue, save policy), `km_calmgr.h` + `src/km_calmgr.c` (boot restore, background save, host command), `port/n32h48x/store_flash.[ch]`
(`km_calstore_flash()`), `port/n32h48x/m5_main.c` (km_l33 only), tests `tests/test_calstore.c` (+ a block in `tests/test_store_flash.c`).

## 1. What is stored, and why only that

Per key the engine has a *baseline* (released level, ADC counts) and a *bottom* (bottomed-out level). The baseline is measured at every boot and
tracked while running (it moves with temperature), so restoring it would be wrong. The bottom is *learned*: it starts at `baseline - 1000` and is
lowered by each key's deepest press. After every power-up all keys start with the default stroke and only become accurate once they were pressed
all the way down, which skews depth, the fixed thresholds and the rapid-trigger distances in the meantime. The record keeps what was learned.

Record payload (308 bytes): version, slot count, 76 x { baseline, bottom } (u16 little endian; 0/0 = unknown). The baseline is only a *witness*: it
says whether the sensor still looks like the one the record was made for. Entries are validated on decode (base 1500..4000, span 400..1800, the
engine's own limits); anything else, including erased flash, zeros and another version, is "no record".

What "skip the calibration wait" really buys: the normal boot calibration is 64 frames = 2.7 ms, so the saving is not time. With a valid record
`km_cal_restore()` arms the engine from ONE live frame (the 16-point sums are already an average) plus the stored strokes; the held / railed /
implausible checks of `km_engine_calibrate_finish()` still run. The value is that the learned strokes are right from the first press.

Sanity check against the live sensor (`km_cal_restore`): for every key slot in the record the live level must be within 150 counts of the stored
baseline (150 = `KM_ENGINE_DRIFT_LIMIT`, the engine's own bound for how far a baseline may wander); at least 30 slots must be comparable
(`KM_ENGINE_CAL_MIN_SLOTS`) and at most 6 may differ (`KM_ENGINE_CAL_MAX_BAD`). Otherwise the record is dropped for this boot (the normal
calibration runs, later saves replace the record). Individual slots that differ (a key held while plugging in) get no stored bottom. The stroke is
applied as a span below the *live* baseline: `bottom = live baseline - (stored base - stored bottom)`.

## 2. The flash scheme: append-only log in one 8 KB sector

`km_store`'s two-sector ping-pong needs a second sector and an erase per save. The calibration has ONE sector (0x08034000), and an erase destroys
the only copy. So: slots of a fixed stride (336 bytes: 16 header + 312 payload rounded up to a 16-byte flash line, 24 slots; the header units are programmed in ascending address order because the flash controller refuses a unit below an already programmed one in its line), a save programs the next free slot, the sector is erased
only when it is full. Wear divides by 24; a normal save erases nothing.

Slot layout is the one of `km_store` (magic "KMCL" + CRC in the last-written 8-byte unit, then sequence + length, payload first). A slot is VALID
(magic, length, CRC), BLANK (all 0xFF) or DIRTY (anything else: a write that lost power, a failed program, foreign data). The newest record = the
valid slot with the newest sequence number (modulo 2^32). The next slot = the one after the *last non-blank* slot, so a half written slot is never
reused and no unit is programmed twice between erases.

Power loss, proven by `tests/test_calstore.c` (50 consecutive appends x every erase/program step x 4-5 partial-effect variants, plus three prepared
start states, about 8400 cut points, and an error / silent-bit-error injection at every step): the log always loads the previous record or the new
one, never anything else, and the next save after the accident works. The one accepted exception is the save that has to erase the full sector
(1 in 24): from the start of the erase until the commit unit of the new record is programmed no valid record exists, so a cut there loses the
stored calibration. It is derived data (without it the keyboard calibrates normally and re-learns), the window is about 40 program operations plus
the erase, and the job is run through without pausing once it has erased (`km_clog_job_must_finish`) so the window does not stretch over typing.

## 3. When to save

`km_cal_policy_due()`, evaluated once per second:
* some healthy key's bottom is at least 16 counts deeper than at the last save / calibration / restore (`KM_CAL_DELTA_COUNTS`; counts of the
  engine's absolute bottom, which only changes by learning, so baseline drift never triggers a write; shallower never counts);
* the sum of all bottoms has not changed for 20 s (`KM_CAL_QUIET_S`: learning is over, not one save per keypress);
* at least 600 s since the last save (`KM_CAL_MIN_INTERVAL_S`; the first save after a boot is exempt). A failed save counts as a save, so a bad
  flash cannot cause a retry storm.

Worst case for wear: 144 records a day = 6 erases a day, and bottoms only get deeper while learning (bounded by the key stroke), so after the first
days nothing changes. The host test simulates 30 days with a noticeable change every 6 hours: 120 records, 4 erases, and no flash access in
the idle hours between.

The host command `save_calibration` ("SCAL") writes at once if something changed noticeably, answers OK without writing if not, BUSY while a key is
down / a fault is active / the engine is not calibrated / the previous attempt was less than 10 s ago (`KM_CAL_REQUEST_GAP_S`), FAILED if the flash
refused (previous record intact). It runs to the end inside the request (about 42 flash operations).

## 4. Timing, the main loop, the watchdog

Background save (`km_calmgr_pass`, once per main loop pass): the job starts only after 1 s without a key, without a fault and without a report waiting
(`idle_long`); the start reads the sector (about 8 KB of byte reads plus the CRC of 24 records, roughly a millisecond). Then ONE flash operation per
pass: an 8-byte program (microseconds), or, once in 24 saves, the 8 KB sector erase. The job pauses while a key is down or a fault is active, except
after the erase (section 2). After every operation `adc_scan_restart()` is called (via the `after_flash` hook): the CPU is stalled while the flash is
busy, so the DMA/ADC chain may have been disturbed; the restart is cheap and idempotent (`STABILITY_NOTES.md` section 2), re-arms the frame timeout and
drops the frame in flight, so the 3 ms scan-stall rule does not fire.

Worst-case time of ONE pass, derived from the code, not measured: `store_flash.c` bounds every wait by a spin count: erase `0x400000` spins, program
`0x20000` spins; at 4-8 CPU cycles per spin of the status register that is roughly 70-140 ms for an erase and 2-4 ms for one program. The real
duration of an 8 KB sector erase and of a 64-bit program on the N32H48x is not recorded anywhere in this repository; typical values for this class of
flash are tens of milliseconds and tens of microseconds, but that is an expectation, not a fact (section 6). A whole job is at most one
erase + 41 programs + the verify (reads only): by those bounds 0.3 s, typically a few ms plus the erase.

Watchdog (stability rule 5): the progress-based feed (`km_health_should_feed`) is untouched. `store_flash.c` feeds the IWDG while it spins in a wait
(every 4096 spins), which is the same exception the settings store already makes; a pass is far shorter than the 500 ms feed window, the 1 s endpoint
rule and the 2 s IWDG period. The blocking host command stays under the same bounds (about 0.3 s worst case, inside the vendor request, like COMMIT).

## 5. Stability rules, checked

1. Fail-safe release: unchanged. A fault (scan, DMA, engine overrun...) pauses the job; keys are not allowed during a fault anyway.
2. Report reconciliation: not touched; a job never starts while a report is waiting (`undelivered`).
3. All waits have timeouts: the log is a state machine with no waiting loop; the flash driver's waits are bounded (section 4).
4. Time budget: one flash line per pass (two 8-byte units programmed back to back, lower first: on the keyboard a second unit of a line programmed after the main loop ran in between is refused); nothing in the 23 kHz frame path changed (the engine is not called differently).
5. Progress-based feed: unchanged (section 4).
6. Fault injection: power cut at every erase/program step, clean errors and silent bit errors at every step, torn slots in the way, full sector with a
   torn last slot, foreign data, a record that disagrees with the sensor, held keys at boot, a flash that refuses (all in `test_calstore.c`);
   mutation checks of the policy constants and of the pause / idle / rate-limit rules (each mutant is killed).
7. Observable: scope tag 0x0202 (8 values, published once per second instead of one 0x0003 sums snapshot, km_l33 only): [0] boot result (bits 0-3:
   0 no record, 1 restored, 2 record disagreed, 3 frame refused), bit 4 record valid, bit 5 job running, bit 6 save due, bit 7 changed noticeably
   [1] saves ok [2] saves failed [3] last result (`km_clog_result_t`) [4] strokes applied at boot [5] log at boot: valid records | dirty slots << 8
   [6] log at boot: next slot [7] seconds since the last save (0xFFFF = none).

## 6. Not verified on hardware

* The real erase / program times and whether the CPU really stalls during them (section 4).
* That `adc_scan_restart()` after a flash operation leaves typing unharmed (a missed frame or two); the IWDG feed inside `store_flash.c`.
* That the first erase of 0x08034000 does not upset anything (the sector holds the stock firmware's data in a freshly restored backup; the
  guard allows it; the first save erases it, by design).
* That a record written by one boot is restored by the next on the real sensor values (the tolerance of 150 counts is the engine's drift bound, not
  a measured temperature range).
* The depth/threshold effect: after a restore the 61 thresholds are recomputed one slot per frame (about 2.6 ms) with the default stroke until then.
