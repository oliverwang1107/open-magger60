# SOCD filter (M6 package P1)

Original work: the four modes are the common, publicly documented ideas of "last input wins", "neutral" and "first input
wins" (the "Rappy Snappy" idea), plus "deeper key wins", which only a Hall-effect keyboard can do. The code, the data format
and the numbers are our own. Nothing here comes from the stock firmware.

Files: `core/include/km/km_socd.h`, `core/src/km_socd.c`, `tests/test_socd.c`, `tools/mutate_socd.py`.

> **Reminder for users (also goes into the configurator).** Some games and tournament rules forbid SOCD handling. It is
> **off by default** (mode 0), nothing switches it on by itself, and using it is the user's responsibility.

**白話**：A 和 D（或 W 和 S）同時按著的時候，遊戲通常會互相抵消、角色停住。SOCD 讓鍵盤自己決定要送哪一個：
後按的贏、都不送、先按的贏，或「按得比較深的贏」（類比，Hall 感測器才做得到）。預設全關。

## Where it sits

```
km_engine (pressed slots, oldest press first) -> km_socd_filter -> km_layers_update_* -> reports
```

The filter works on physical slots and knows nothing about layers except one flag (below). A suppressed key is, for the layer
logic, simply not pressed, so no half-decided tap-hold is left behind; when the key comes back it is a fresh press for the
layers. Everything downstream (layers latching what a key emitted, report sync) therefore stays consistent: nothing can get
stuck, because the layer logic sees an ordinary press and an ordinary release for every change of the filtered list.

## Public API (`km_socd.h`)

```c
typedef struct { uint8_t slot_a, slot_b, mode, flags; } km_socd_pair_t;   /* the type of the plan, section 2.2 */
typedef uint16_t (*km_socd_depth_fn)(void *ctx, unsigned slot);           /* km_engine_depth(&engine, slot) */

void     km_socd_init(km_socd_t *s);
void     km_socd_reset(km_socd_t *s);                        /* forget the running state, keep the pairs */
bool     km_socd_pair_valid(const km_socd_pair_t *p);
bool     km_socd_set_pairs(km_socd_t *s, const km_socd_pair_t *pairs, unsigned n);   /* n <= 4 */
unsigned km_socd_filter(km_socd_t *s, const uint8_t *down_in, unsigned n_in, unsigned layer,
                        km_socd_depth_fn depth, void *ctx, uint8_t *down_out);
bool     km_socd_suppressed(const km_socd_t *s, unsigned slot);
```

The plan's sketch had no state object; one is needed because "deeper key wins" has to remember who currently wins (see
below) and for the diagnostics counters (`suppress_events`, `calls_in_conflict`, for the scope tag 0x0500). `down_out` may be
the same buffer as `down_in`. `layer` is `km_layers_out_t.layer` of the previous update.

`km_socd_t` is 120 bytes of RAM, `km_socd.c` is about 0.8 KB of code (`-Os`, Cortex-M4).

## Modes (only while BOTH keys of a pair are down)

| mode | name | both down | A down, then D | D released first | A released first |
|---|---|---|---|---|---|
| 0 | off | both reported | | | |
| 1 | last input wins | the later key | D | A comes back (it never left) | D stays |
| 2 | neutral | neither | none | the other one is reported | the other one is reported |
| 3 | first input wins | the earlier key | A | D (the held key acts once A is gone) | D is reported |
| 4 | deeper key wins | the deeper key, with hysteresis | see below | | |

* "Earlier / later" is the position in the engine's pressed list (oldest press first). That makes modes 1-3 **stateless**:
  the result is a pure function of the list. A key that is released and pressed again is "later" again. Keys that go down in
  the same frame are listed in ascending slot order, so the higher slot counts as later (deterministic; a one-frame difference
  is not something a finger can mean).
* Mode 3: once the first key is released while the second is still held, the second one is reported. (The alternative, to
  ignore the second key until it is pressed again, would make a held key do nothing for no visible reason.)
* **Mode 4 (deeper wins).** The depth callback gives 0.01 mm (the engine's filtered reading through the depth model). The
  winner changes only when the other key is deeper by `KM_SOCD_DEEPER_HYST_MM100` = 30 (0.30 mm) **or more**; at 29 it
  stays. The sensor noise is about +-0.15 mm, so two keys at "equal" depth cannot make the output flicker (the test wobbles
  both depths by +-0.12 mm 20,000 times: zero changes; a slow drift of one key makes exactly one change, at the 30 mark, and
  the way back makes exactly one at the mirror point). When a conflict starts, the key that was already reported (the earlier
  one) is the winner, unless the new key is already deeper by the hysteresis. The winner is forgotten the moment the pair is
  not in conflict (one key up, a layer where the pair sleeps, an empty list).
  Without a depth callback every depth counts as equal, which makes the mode behave like mode 3.
* Flag bit0 = the pair works only while the active layer is 0. When the layer changes while a pair is in conflict, the
  suppression starts / stops at once; the layers see that exactly like a press or release. (A user who switches layers while
  holding both keys of a pair therefore sees a key appear or disappear for the layer logic, never a stuck key.)

## Validity

`km_socd_pair_valid`: mode 0..4, flags 0 or 1 (reserved bits must be 0 even for an unused entry); for a used pair two
different slots that both carry a key (`km_keymap_slot_is_key`; the mirror and floating slots are not keys). An unused pair
(mode 0) is ignored whatever its slots are. `km_socd_set_pairs` also rejects a slot in two **used** pairs. A rejected set
(or an invalid pair anywhere in it) leaves **all** pairs off and returns false: the same "a bad configuration never half
works" rule as the engine (`km_engine_apply_key_cfg`). Used pairs are packed to the front internally, so a mode-0 entry
between used pairs costs nothing per pass.

## Invariants (checked in `tests/test_socd.c`)

1. The output is the input with entries removed: same order, nothing added or repeated.
2. An empty input gives an empty output and clears every bit of state, so a fail-safe release can never leave a key suppressed
   or reported.
3. A key that is in no active pair, or whose partner is up, is always reported (no permanent suppression).
4. With both keys down: modes 1 and 3 report exactly one key, mode 2 none, mode 4 exactly one with the depth rule above.
5. Modes 1-3 keep no state: the result of a filter that was just reset equals the result of the running one.
6. In-place filtering gives the same list and the same state as filtering into another buffer.
7. Entries that are no slot (>= 76) pass through untouched and never take part (the layer logic ignores them).

## Tests (`python firmware/tools/run_tests.py` once `test_socd()` is registered in `test_main.c` / `km_test.h`)

About 220,000 checks: configuration (validity, rejected sets, unused entries, packing); hand-written truth tables for each
mode (one scenario per documented sentence, with other keys in the list to check that they keep their place); **every
sequence of up to 8 presses and releases of the two keys** (3 modes x 65,536 sequences) against a model that only knows
time stamps; the deeper-wins boundaries (29 / 30, start of a conflict, equal depths, no callback, noise, drift up and down);
600 random runs (4 random disjoint pairs, random modes and layers and depths, other keys, two entries that are not slots,
same-frame presses, "fail-safe" empty calls) that check invariants 1-7 at every step and that releasing everything leaves
nothing reported and nothing remembered; "no stuck suppression" for every mode; the real key engine driven by synthetic
sensor frames through `km_engine_update` -> `km_socd_filter` (LAST, FIRST, NEUTRAL, DEEPER with the engine's own depth);
edge cases (slots >= 76 right behind the role table, repeated entries, a 300-entry list, 61 entries, counters).

`python firmware/tools/mutate_socd.py`: 71 one-expression mutations of `km_socd.c` (press order, each mode, the hysteresis
edges, owner handling, layer flag, bit masks, output copy, counters, validation, packing). All 71 are killed except two that
cannot change behaviour (listed with their reason in the script: an extra NULL guard that `km_socd_pair_valid(NULL)` already
covers, and the single-key early exit, which is a pure speed-up). It builds its own tiny main, so it does not depend on `test_main.c`.

## Cost (estimate from the Cortex-M4 `-Os` disassembly, not a measurement on the chip)

Counted as in `ENGINE_NOTES.md`: instructions of the build, loads 2 cycles, taken branches 2-3 cycles, an LDM/STM of n
registers n+1 cycles. `n` = entries of the pressed list.

| case | cycles per pass |
|---|---|
| no pair configured, or fewer than two keys pressed | 70-100 (prologue, state clear, counters) |
| 2 pairs (WASD), 6 keys pressed, no conflict | 250-300 (scan 13/entry, pair loop 25/pair, counters 35, copy 40) |
| the same with one conflict (modes 1-3) | 320-370 (+ suppression, copy loop 12/entry instead of the copy) |
| 4 pairs all in conflict, modes 1-3, 10 keys | 600-700 |
| 4 pairs all in DEEPER conflict, 10 keys (8 depth callbacks, `km_engine_depth` ~55 cycles each) | 1.1-1.3 k |
| **absolute worst: 4 DEEPER conflicts and a full 64-entry list** | **about 2.5 k** |

The plan estimated "< 100, worst 150" (section 2.5); that was optimistic. The realistic range for a game session (WASD
pairs, a handful of keys) is 250-400 cycles, about 3 % of the 10.4 k frame, far from the 3 k worst-pass target even in
the absolute worst case. No division, no allocation, no loop over the 76 slots. The depth callback runs only while a DEEPER
pair is in conflict (2 calls per such pair).

## For the integration (P10)

* `km_socd_set_pairs()` from the profile's SOCD section (0x12) when a profile is loaded; log / drop to "all off" when it
  returns false (the settings decoder should already have refused such a profile).
* Once per main-loop pass, between `km_engine_down_slots()` and `km_layers_update_*`:
  `n = km_socd_filter(&socd, down, n, lo_prev.layer, depth_cb, &g_engine, down_buf);`
  with `depth_cb` = `km_engine_depth`.
* On a fail-safe release, a USB reset or a profile change: `km_socd_reset()` (or simply pass an empty list for one pass).
* `s.suppress_events` and `s.calls_in_conflict` are for the new scope tag 0x0500.
* Register `test_socd()` in `tests/test_main.c` / `tests/km_test.h` (P10 owns those two files).
