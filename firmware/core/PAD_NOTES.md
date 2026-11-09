# Gamepad core (M6 package P4)

Original work. Inputs: the USB HID specification (report layout, hat switch), the M6 plan (`M6_PLAN.md` 2.2, 3.1, 6.5) and our
own depth numbers (`km_engine_depth`, 0.01 mm, resting noise about +-0.15 mm). Files: `core/include/km/km_pad.h`,
`core/src/km_pad.c`, `tests/test_pad.c`, `tools/mutate_pad.py`, `tools/magger60_pad_map.py`. No hardware access, no USB, no
dependency on the key engine: the depth comes in through a callback, the result goes out as a struct and as 15 bytes.

## What it does

Keys that are bound to the gamepad (table codes `0xEE` button, `0xEF` axis) turn into one gamepad state: 16 buttons, one hat,
two sticks and two triggers. A stick axis is "the deepest key of its + direction minus the deepest key of its - direction",
each through a response curve; buttons and hat directions are on/off. The module is a small state machine (it remembers a few
bits: which keys are held over a reset, which depth-activated buttons are on, the last stick values for the output band), and
it never touches the keyboard path: the keys also stay out of the keyboard report because `km_layers` treats `0xEE/0xEF` as
"no keyboard output" (P0).

## The report (what P5b builds the descriptor from)

`KM_PAD_REPORT_LEN = 15` bytes, no report id, little endian. The all-zero report is the neutral state.

| byte | size | field | range / meaning | HID usage |
|---|---|---|---|---|
| 0 | 1 | buttons 1-8 | bit n = button n+1 | Button page 1-8 |
| 1 | 1 | buttons 9-16 | bit n = button n+9 | Button page 9-16 |
| 2 | 1 | hat (low nibble), 0 (high nibble) | 0 neutral, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW | Generic Desktop Hat switch |
| 3-4 | 2 | LX | int16, -32767..32767 (never -32768), 0 centre | X (0x30) |
| 5-6 | 2 | LY | int16, negative = up | Y (0x31) |
| 7-8 | 2 | RX | int16 | Rx (0x33) |
| 9-10 | 2 | RY | int16, negative = up | Ry (0x34) |
| 11-12 | 2 | left trigger | uint16, 0..32767 | Z (0x32) |
| 13-14 | 2 | right trigger | uint16, 0..32767 | Rz (0x35) |

Descriptor rules that follow: buttons Logical 0..1 size 1 count 16; hat Logical **1..8**, Physical 0..315 degrees, **Null State**
(0 = neutral, which is what makes the zero report neutral), size 4, followed by a 4-bit constant pad; sticks Logical -32767..32767
size 16 count 4 with usages X, Y, Rx, Ry in that order; triggers Logical 0..32767 size 16 count 2 with usages Z, Rz. Total 120 bits.
`python tools/magger60_pad_map.py descriptor` prints a descriptor with exactly this shape (89 bytes) and checks it with a small
independent item walker; P5b may adopt it as it is. Windows, Linux and SDL number joystick axes by usage (X Y Z Rx Ry Rz = a0..a5),
so in games the left trigger is axis 2 and the right stick is axes 3 and 4. `magger60_pad_map.py sdl` prints the matching SDL
mapping line.

C view: `km_pad_report_t { uint16_t buttons; uint8_t hat; int16_t lx, ly, rx, ry; uint16_t lt, rt; }`;
`km_pad_report_pack()` writes the bytes above, `km_pad_report_unpack()` reads them, `km_pad_report_equal()` compares the packed
bytes (what `km_report_sync` compares), `km_pad_report_released()` gives the 15 zero bytes for `km_report_sync_init()`.
Packing is deterministic: no padding bytes, an out-of-range hat packs as 0.

## Input contract (the integrator, P10)

```c
km_pad_key_t keys[KM_PAD_MAX_KEYS];     /* { slot, code, arg0, arg1 } */
km_pad_in_t in = { keys, n, allowed, epoch, now_ms, depth_cb, ctx };
bool recomputed = km_pad_update(&pad, &in, &report);
```

* `keys` = every key that is **down** (as the engine says) whose table item (latched like `km_layers` latches it) is `0xEE` or
  `0xEF`. Keys that are not down are not listed. No key listed means the neutral report. This is the same "locked item of
  every pressed slot" accessor that P0 adds to `km_layers` for DKS.
* `allowed` = the same flag as for the keyboard (`km_health_keys_allowed()`).
* `epoch` = any number that changes whenever the meaning of the keys changes: the keymap generation, the active profile, the
  active layer if layer changes should release the pad. A change releases everything at once (below).
* `now_ms` = a free-running millisecond counter. The state is recomputed at most once per millisecond; the other calls return
  `false` and the unchanged state (this is what keeps the cost per main-loop pass low).
* `depth_cb(ctx, slot)` returns 0.01 mm. For now `km_engine_depth`; with P12c `km_engine_analog`. A NULL callback reads as 0.
* Entries that are invalid (`km_pad_entry_valid()`), have a slot >= 76, or come after `KM_PAD_MAX_KEYS = 20` are ignored and
  counted (`ignored_keys`). P0's `km_keymap_entry_valid()` should call `km_pad_entry_valid()` for `0xEE/0xEF` so there is one
  rule.

Because the key list only contains keys the engine reports as pressed, **a pad key must be sensitive from the start of the
stroke**. The integrator (`apply_profile()` in `m6_main.c`) gives every key whose table entry is a pad code, on any layer, the
engine configuration FIXED with `press = KM_PAD_ENGINE_PRESS_MM100 (0.20 mm)` and `release = KM_PAD_ENGINE_RELEASE_MM100
(0.10 mm)` (valid for `km_engine_cfg_valid`, checked in the test). With rapid trigger on such a key the engine would drop the key at
every small lift and the stick would fall back to zero. The curve start (default 0.20 mm, never below 0.20 mm) is the dead
zone of the key; the engine's own debounce (8 frames to press, 6 to release) and its glitch / fault protection stay in front of
the pad.

## Rules (each has a test)

* Released is exactly 0 / exactly neutral; full travel is exactly 32767 (sticks -32767).
* Curve: depth <= start gives 0, depth >= end gives 32767, in between a monotonic shape (linear, slow-then-fast `t^2`,
  fast-then-slow `1-(1-t)^2`, S `3t^2-2t^3`), all in integer arithmetic with one floor of an exact expression, so they cannot
  step backwards. Flag bit0 turns the curve by 180 degrees (`1 - f(1 - t)`); for linear and S that changes nothing, for the two
  other shapes it swaps them.
* Stick axis = deepest + key - deepest - key. Two opposing keys equally deep cancel to 0, the deeper one wins by the
  difference (a natural analog "last input wins" behaviour). Digital keys (axis bit4) give full deflection while down, so two
  opposing digital keys cancel to 0 and one alone is full. Triggers take the deepest key and have no negative direction.
* Diagonal: both keys of two axes add independently, so W+D at full depth is (32767, -32767) (a square) unless `circular` is on,
  then the vector is scaled back into the circle (`x^2 + y^2 <= 32767^2`, exact: the radius is rounded up before scaling).
  Short vectors are not stretched. `stick_deadzone` (0..50 %) is a radial dead zone at the centre; what is outside it is
  rescaled so that full deflection stays 32767. Order: circle, then dead zone, then clamp.
* Hat: buttons 17-20 are up/down/left/right; up+down cancel, left+right cancel; the remaining pair is the diagonal.
* Buttons: arg1 = 0 follows the engine's pressed state. arg1 = 1..68 is a depth-activated button: on from `arg1 * 0.05 mm`
  (raised to at least 0.20 mm), off again `KM_PAD_BTN_HYST_MM100 = 0.20 mm` shallower (or half the activation depth for
  shallow points). It can only fire deeper than the engine's own press point of that key, which is why pad keys get the low
  engine press point above.
* Output band: an axis value follows its input only after it moved `KM_PAD_OUT_HYST = 64` units (0.2 % of full scale, about half an
  8-bit step); 0, full scale and a change of sign always pass at once. This stops resting noise from producing a new USB report
  on every pass (the band acts before circle and dead zone, so those two rules stay exact). The analog path of P12c (`smooth`)
  is separate and comes before this module; `km_pad` does not read `opts.smooth`.
* Fault (`allowed == false`), an `epoch` change, `km_pad_reset()`, `km_pad_set_config()` and `km_pad_init()`: the state is
  neutral at once. Keys that are down at that moment are **held over**: they produce nothing until they have been released
  (left the list) once. So a key that is held through a fault, a profile switch or a USB reset does not come back by itself
  and nothing can get stuck.
* The invariant for the pipeline test (P10): **no pad key listed, or not allowed => the report is exactly neutral.** It is
  checked on 12 random sequences of 4000 steps each, together with bounds, the circle, determinism and the pack/unpack round trip.

## Defaults

`km_pad_default_curve()`: 0 linear 0.20..3.20 mm (sticks), 1 linear 0.20..2.00 mm (triggers), 2 slow-then-fast 0.20..3.20 mm
(fine control near the centre), 3 S 0.20..3.20 mm. An all-zero curve is invalid (a start below 0.20 mm sits in the resting noise)
and is replaced by the default of its index; `km_pad_set_config()` reports that with `false`. Options all zero = no dead zone,
square sticks. The end of every default curve is below the nominal 3.40 mm travel so that full scale is reached even on a key
whose learned bottom is a little short.

## Cost (39 us frame budget), estimates from the Cortex-M4 disassembly (`-Os`, hard float), not measured

`km_pad.c` is 1980 bytes of code, no division library calls (hardware divide only), 100 bytes of state, a stack frame of 132
bytes in `km_pad_update`. Per call:

| case | cycles |
|---|---|
| same millisecond as the last recompute (every call but one in about 25) | about 40 |
| recompute, nothing held | about 350 |
| recompute, 4 keys held (W A S D on the left stick, circle on) | about 1 600 |
| recompute, 8 pad keys held, all analog | about 3 200 |
| recompute, `KM_PAD_MAX_KEYS = 20` analog keys held (not a real hand), both sticks with circle and dead zone | about 6 500 |
| fault / epoch pass (clears and returns) | about 150 |

These include the call of the depth callback at about 55 cycles each (`km_engine_depth`); a key costs about 65 cycles of
validation, 17 of the held-over check and 70 plus the callback in the compute loop (a curve evaluation with its 32-bit divide
is about 80), and the two sticks cost about 450 each in the worst case (two integer square roots of about 150 and two divisions).
Averaged over frames it is 55 to 170 cycles per frame, because only one frame in about 25 does the work. The single heavy pass
(realistically 1.6k-3.2k) is within the plan's "worst pass <= 3k"; it is one pass of the main loop, so at worst the sensing rate
dips for one frame per millisecond while a hand holds 8+ analog keys. To be checked on hardware with `pass_cycles_max`.

## Tests

`tests/test_pad.c` (`test_pad()`, about 650 000 checks): report bytes and equality, curve validity and values (every shape x
mirror x many start/end pairs: monotonic, bounded, zero up to the start, full from the end), entry validity over the whole
argument space, stick/opposing/diagonal/circle/dead zone, buttons and hat (all 16 hat combinations), depth buttons with their
hysteresis, fault / epoch / reset / set_config release and the held-over rule, the 1 ms cadence including counter wrap, ignored
and surplus entries, the output band at exactly 63 and 64 units, a sweep of one key up and down for every shape and axis,
engine link (`KM_PAD_ENGINE_*` is a valid engine configuration), and random sequences.
`tools/mutate_pad.py` applies 95 one-expression mutations (92 killed, 3 equivalent with reasons) to `km_pad.c` and requires the tests to notice each one; the few
equivalent ones are listed in the script with the reason.

`test_pad()` is **not registered** in `tests/test_main.c` / `tests/km_test.h` yet (shared files, P10 does it): add
`void test_pad(void);` to `km_test.h` and `run("pad", test_pad);` to `test_main.c`. `core/src/*.c` is globbed, so `km_pad.c` is
already part of the core library; `test_pad.c` already compiles in `run_tests.py`.

## For the others

* **P0**: `km_pad.h` includes `km/km_m6_types.h` when it exists and uses its `km_pad_curve_t` / `km_pad_opts_t` (same field names as
  the plan); until then it defines identical ones. `KM_PAD_CODE_BTN/AXIS` (0xEE/0xEF) are defined here; `km_pad_entry_valid()` is
  the validity rule to call from `km_keymap_entry_valid()`. `km_pad_curve_valid()` / `km_pad_opts_valid()` are the rules for the PAD
  section (0x20): reject or replace; an invalid curve falls back to the default of its index in `km_pad_set_config()`.
  Curve flag bit0 is defined as "mirror" (see Rules); the plan only said "reverse".
* **P5b**: the report and the descriptor above. `pad_mode = 0` must leave the descriptors as they are today.
* **P9b**: the UI can draw the virtual gamepad from `km_pad_report_unpack()`-style bytes, or compute it from `LIVE_DEPTH`
  with the same curve formulas (they are plain integer arithmetic; `km_pad_curve_eval()` is the reference).
* **P10**: engine configuration of pad keys (above), the 1 ms cadence (just call it every pass), `epoch`, `km_report_sync` for
  the pad report with `km_pad_report_released()` as the released report and `KM_SYNC_MAX_LEN = 32 >= 15`, the registration of the test.
* **P12c**: when `km_engine_analog()` exists, pass it as the depth callback; the output band can then be loosened or removed
  if the analog path is quiet enough (target: resting noise <= 0.5 of an 8-bit step).
