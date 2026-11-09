# Layers, FN key, media keys (M5 task T-C) and toggle / tap-hold / one-shot keys (task L4)

Original work. Inputs: the USB HID Usage Tables (keyboard page 0x07, consumer page 0x0C) and our own slot map
(`core/data/slotmap_measured.json`). Files: `core/include/km/km_keymap.h`, `km_layers.h`, `km_report.h` and the
matching `core/src/*.c`; tests in `tests/test_layers.c`.

## Table entries ("key codes")

One code byte per layer and slot (plus two argument bytes since settings format version 2, see below), `KM_LAYERS = 4` layers x 76 slots (`km_keymap_default_table()` gives the built-in table,
`km_layers_set_map()` loads a replacement and refuses an invalid one).

| value | meaning |
|---|---|
| `0x00` | no key |
| `0x04..0xE7` | keyboard-page usage |
| `0xF0` | hold: layer 1 while the key is down |
| `0xF1..0xF4` | switch to profile 1..4, requested when the key is **released** |
| `0xF5` `0xF6` `0xF7` | volume down (consumer `0xEA`), volume up (`0xE9`), mute (`0xE2`) |
| `0xF8` `0xF9` `0xFA` | previous track (`0xB6`), play/pause (`0xCD`), next track (`0xB5`) |
| `0xFB` `0xFC` | hold: layer 2 / layer 3 (extension so that layers 2 and 3 can be reached) |
| `0xE8` | **toggle layer** (arg0 = layer 1..3), see below |
| `0xE9` | **tap-hold** (arg0 = tap action, arg1 = hold action), see below |
| `0xEA` | **one-shot modifier** (arg0 = modifier usage 0xE0..0xE7), see below |
| `0xFD` | light action (arg0 = action 0..7; 0 does nothing, the old 'reserved' meaning); owned by the lighting code, silent here |
| `0xFE` | reserved: accepted, does nothing |
| `0xFF` | transparent: use the layer-0 entry of this slot (on layer 0: no key) |
| `0xEB` | **toggle key** (arg0 = usage), see the M6 section below |
| `0xEC` | **Mod-Morph** (arg0 = usage, arg1 = record), see the M6 section below |
| `0xED..0xEF` | DKS, gamepad button / axis: owned by other modules, silent here |
| `0x01..0x03` | rejected (HID error codes) |

The extended codes `0xE8..0xEA` are only valid together with their argument bytes; every other code needs both argument bytes
to be 0 (`km_keymap_entry_valid()`). `km_keymap_usage()` returns 0 for them (they are not keyboard usages), `km_keymap_arg()`
returns the argument bytes of a loaded table.

`km_keymap_usage()` still returns only keyboard usages (function codes read as 0), so layer 0 looks exactly like the
M4 map to old code; `km_keymap_code()` returns the raw entry. The only layer-0 change is that the FN key (slot 14,
named `FN`) holds `0xF0`.

## The rule that prevents stuck keys

`km_layers_update_ms(down_slots, n, now_ms, out)` (or `km_layers_update_cycles()` with the CPU cycle counter, or the old
`km_layers_update()` with a clock that stands still) remembers, per slot, the code the key got **when its press was handled**, and uses
that until the key is released. So releasing FN before the other key leaves the key as it was (and its release is the
release of what it really sent); pressing FN while keys are down changes none of them. FN and a key appearing in the
same update: the order of the list (press order) decides. Slots with no key never emit. After a USB reset or a
fail-safe "release everything", call `km_layers_reset()`.

Outputs: `kbd[]` usages (press order, each once; feed `km_report_build_boot_usages` / `km_report_build_nkro_usages`),
`consumer` (one usage), `profile_request` (1..4 for one update), `layer`.

Media keys: only the most recently pressed media key is reported (`km_report_build_consumer(usage, report)`,
id 2, usage little endian, 0 = released); when it is released the older one still held comes back.

## Toggle layer (`0xE8`)

Pressing the key switches its layer on, pressing again switches it off; releasing does nothing. Rules:

- A toggled layer stays on until it is toggled off, until another table is loaded (`km_layers_set_table` / `km_layers_set_map`,
  which is what a profile switch and a settings commit do) or until `km_layers_reset()` (boot, USB reset, fail-safe release).
- Held layers (FN, tap-hold holds) and toggled layers are independent; the active layer is the **highest** of all of them. Releasing
  a hold never turns a toggled layer off, toggling a layer that is also held leaves it on after the hold ends.
- Keys that are already down keep the code they were pressed with, so toggling changes no key under a finger and every release
  matches its press (the stuck-key rule above).
- The key that switches the layer off has to be reachable on that layer: the entry on the layer itself, or transparent so that
  the layer-0 toggle entry applies. The configurator warns when no such key exists. Escape hatches: toggle again, change profile.
- `out->toggled` (bit mask) and `out->layer` report the state.

## Tap-hold (`0xE9`)

arg0 = tap action (keyboard usage `0x04..0xE7`, or a media code `0xF5..0xFA`), arg1 = hold action (`KM_CODE_HOLD_L1/L2/L3` = layer
while held, or a modifier usage `0xE0..0xE7` = modifier while held). A key press does not decide anything yet ("undecided",
`out->pending`). The profile's layer options (`km_layer_opts_t`: `tap_term_ms` 30..1000, default 200; `tap_policy`) decide:

| event | result |
|---|---|
| released before `tap_term_ms` (measured from the press, with the times the events were *seen*) | **tap**: the tap action is sent for `KM_LAYERS_STEP_MS` (10 ms), long enough for the host, then released |
| still down at `tap_term_ms` | **hold**: the layer / modifier starts and lasts until the key is released |
| policy `KM_TAP_POLICY_OTHER_KEY` (2): another key is pressed | **hold** at once |
| policy `KM_TAP_POLICY_PERMISSIVE` (1): another key is pressed **and released** while this one is still down | **hold** |
| policy `KM_TAP_POLICY_TIMEOUT` (0, default): other keys | no effect on the decision |

Other keys pressed while a tap-hold key is undecided are **never lost and never reordered**: their presses wait in a 64-event
queue and are replayed in order after the decision, so they see the new layer / modifier after a hold and come *after* the key
of a tap. Releases of keys that were already handled (pressed before the tap-hold key) are not delayed. Events that had to wait
are replayed `KM_LAYERS_STEP_MS` apart so that every press is visible to the host for at least 10 ms (the report path only ever
sends the current state, so a shorter press could vanish); a hold of a modifier is seen 10 ms before the key that triggered it.
If the queue ever overflows the undecided key becomes a hold and everything is processed at once (no key can stay down, at worst
a quick tap merges). Two undecided tap-hold keys: the first decides, then the second one's decision uses its *own* press time.
The time is a free-running millisecond counter (32 bit, wrap-safe); `km_layers_update_cycles()` converts a cycle counter (the
main loop passes its `now` and `KM_MS_CYCLES`).

## One-shot modifier (`0xEA`)

arg0 = modifier usage. Pressing the key sends the modifier at once, so holding it and typing is an ordinary modifier. If no other
key was pressed while it was held and it was released within `tap_term_ms` (a tap), the modifier stays on ("armed", `out->armed`)
and goes with the **next key that is pressed**, until that key is released. A plain modifier key, layer keys, toggle keys, profile
keys and media keys do not use it up; a tap of a tap-hold key does (the modifier rides on the tap). Several one-shot modifiers
can be armed at once. An armed modifier ends after `oneshot_ms` (default 2000, 0 = never), on a table change and on a reset.

## Settings format version 2

Because tap-hold needs two parameters the settings format grew (`km_settings.h` is the whole specification): per profile the
layer options (6 bytes) and per layer entry two argument bytes: blob 7944 bytes (was 5488), version field 2. `km_settings_decode()`
still reads version 1 and migrates it (arguments 0, default options); only version 2 is written. The configuration protocol buffer
`KM_CONFIG_MAX_SETTINGS` is 8192 bytes (CONFIG_PROTOCOL.md); the store sector holds 8176. A keyboard running a format-1 firmware
cannot take a version 2 blob (the protocol buffer is too small): the configurator then asks for the firmware update.

## Default layer 1 (FN held)

Everything not listed is transparent. FN itself is the hold key. The board has no grave key, so Tab gives it.

| key | gives | key | gives |
|---|---|---|---|
| `1 2 3 4 5 6 7 8 9 0 - =` | F1 .. F12 | `I` `J` `K` `L` | Up, Left, Down, Right |
| `Backspace` | Delete | `U` `O` | Home, End |
| `Tab` | ` (grave) | `P` `;` | Page up, Page down |
| `[` `]` | volume down, up | `M` | mute |
| `,` `.` `/` | previous, play/pause, next | `Z` `X` `C` `V` | profile 1, 2, 3, 4 |

Layers 2 and 3 are all transparent by default; they only matter with a loaded table (settings, T-D / T-B).
The FN layer is the owner's to change later; nothing else depends on these particular choices.

## Integration notes for m5_main.c

- Per main-loop pass (every pass, not only when a key changes: tap-hold decisions and one-shot timeouts are timers):
  `km_layers_update_cycles(km_engine_down_slots(...), n, now, KM_MS_CYCLES, &out)`; build the boot or NKRO report from `out.kbd`
  and the consumer report from `out.consumer` (send it only when it changes; keep the keyboard report and the consumer
  report as two separate "last sent" states).
- `out.profile_request != 0`: switch profile (load that profile's table with `km_layers_set_table(codes, args, opts)`, which also ends toggled layers and armed one-shot modifiers), save the active
  profile through the settings store. Keys that are down keep what they were pressed with.
- The consumer report must be re-sent as 0 on every fail-safe release (T-E), together with the keyboard reports.

## M6 task P2: toggle keys, Mod-Morph, tap-hold with an ordinary key as the hold action

Files: `km_layers.[ch]` (the only ones changed), `tests/test_layers_m6.c`, `tools/mutate_layers_m6.py`. The argument encodings are P0's
(`km_keymap.h`, `km_m6_types.h`); the rules below are also written in `km_layers.h`.

### Toggle key (`0xEB`, arg0 = usage 0x04..0xE7, a modifier too)

* A press flips the usage: on -> in `out->kbd` as if a key held it down; the next press -> off. Releasing does nothing.
* The state belongs to the **usage**, not the slot: two toggle keys with the same usage switch the same thing; a plain key with the
  same usage and a toggled usage are reported once.
* At most `KM_LAYERS_TOGGLE_KEYS` (16) usages are on at once; a press that would be the 17th is ignored (the usage stays off, nothing
  can stick). Switching one off makes room again.
* RAM only. Ends on: the same usage toggled again, `km_layers_reset()` (boot, USB reset, fail-safe release: the caller must call it,
  as `m5_main.c` already does), a table change (`km_keymap_generation()`, i.e. a profile switch or a settings commit). The escape
  hatches the configurator must keep: the toggle key itself, a profile key.
* Output order: held keys (press order), then the toggled usages in the order they were switched on, then modifiers carried by keys.
  A 6-key boot report therefore keeps held keys first.
* Like a layer toggle key it counts as "another key" for tap-hold and one-shot decisions, but does not use up an armed one-shot modifier.
* A toggle key pressed behind an undecided tap-hold key waits in the queue and is handled with the layer the decision gives (latching).
* `out->toggle_keys` = how many are on (for a lighting rule "toggle on").

### Mod-Morph (`0xEC`, arg0 = usage sent normally, arg1 = record index)

* Records come from `km_layers_set_morph(rec, n)` (copy of the active profile's `morph[]` / `morph_n`). **The integrator must call it
  next to `km_layers_set_table()` whenever a profile becomes active** (`m5_main.c` `apply_profile()` / `m6_main.c`). `km_layers_reset()`
  keeps the records. A record that breaks `km_settings_morph_valid()` is stored as an empty one (never morphs); more than 8 are cut.
* When the key's press is **handled**, the modifiers being sent right then are collected: held modifier keys (also a one-shot key that
  is held, and a tap-hold hold that has been decided), modifiers carried by other keys, an **armed** one-shot modifier, a **toggled**
  modifier key. A modifier whose tap-hold key is still undecided does not count (the morph key waits behind it in the queue anyway).
  If one of them is in `mod_mask` the key is `usage` of the record, otherwise `arg0`. The choice is locked until release.
  An index without a record, or beyond `morph_n`, makes the key a plain `arg0` key.
* Armed one-shot: it counts, and an ordinary-key output takes it (`Shift+Delete`), exactly like any key; a morph into a modifier
  leaves it armed (a plain modifier never uses it up).
* **Suppress** (`flags` bit 0): the modifiers that triggered the morph (`mod_mask` and the modifiers sent) are left out of the report
  while the key is down and come back when it is released, if their keys are still down. Details decided here:
  * a suppressed modifier that is **released** during the morph is dropped from the key's mask at once (nothing phantom comes back
    when the morph key goes up);
  * a modifier **pressed again** while the morph key is still down is a new press and is **not** suppressed;
  * a modifier that goes down in the same update after the morph key is not suppressed;
  * the key's own output is never suppressed (mask Shift, usage Shift: the Shift stays);
  * two morph keys: the union of their masks is hidden; each mask ends with its own key.
* The accessor `km_layers_locked_entry()` returns the table entry (`0xEC`, arg0, arg1) of a morph key; the choice is not visible there.
  `out->suppressed` = the modifiers currently hidden (diagnostics, tests).

### Tap-hold with an ordinary key as the hold action (format 3)

`pend_hold()` already made any usage a held key; P2 only adds that an **ordinary** (non-modifier) hold key takes an armed one-shot
modifier with it (like an ordinary key) and a modifier hold leaves it armed (as before). `tap_term_ms`, the three policies and the
queue are unchanged. The key is reported by the accessor with its tap-hold entry.

### Bug fixed on the way (reported by P0, "Known issue")

A key pressed, released and pressed again while its first press waited in the queue, and then released as soon as the first press was
handled, ended with a queued press and no release: stuck until `km_layers_reset()`. A release is now handled at once only when no
event of that slot is queued (`queued_for()`), otherwise it is queued behind them. `test_release_behind_queue` reproduces it; the
randomised test of P0 (`test_m6_codes`) no longer waits 500 ms before pressing a key again.

### Changes to existing tests (all in `test_layers_ext.c`, none changes what a layer rule means)

1. `test_m6_codes`: the table had a toggle key (`0xEB`) and a Mod-Morph key (`0xEC`) in the list of codes that are **silent**. They now
   have output (that is the point of P2), so they left that list; the accessor is checked for them in `test_layers_m6.c`.
2. Same test, randomised part: `KM_CODE_TOGGLE_KEY` (its state would stay on after everything is released, the test ends with "idle")
   became `KM_CODE_LIGHT` (another silent code); the 500 ms guard before re-pressing was removed (see the bug above).
3. `test_layers_m6` is registered in `test_main.c` / `km_test.h` (`run("layers_m6", ...)`), as the lead asked.

### Tests (`tests/test_layers_m6.c`)

Hand-written cases for every rule above (toggle: basic, sharing, modifier, cap, ends, one-shot, tap-hold, layers; morph: basic, lock,
suppress, sources of modifiers, edges, more, tap-hold; tap-hold hold key; queued release) and two randomised runs (together with the
older layer suites about 170 M checks):

* `test_random_exact` (500 seeds) and `test_random_faults` (500 seeds): a table of 12 keys with random roles (plain with layer variants,
  tap-hold with layer / modifier / key hold, layer toggle, layer hold, one-shot, toggle key, Mod-Morph, plain modifier), random
  tap term / policy / one-shot time, random Mod-Morph records (changed at random during the run), 60-160 actions with gaps from 0 to
  250 ms, bursts of up to 11 key changes in one millisecond, fail-safe resets (all keys released + `km_layers_reset()`), "the engine
  reports nothing down", main-loop stalls of up to 900 ms, table reloads, and a millisecond counter that wraps during the run (every
  4th seed). `_faults` reloads the table at any moment, `_exact` only when all keys have been up for 1.5 s.
* At every millisecond: usages in range and unique; no suppressed modifier in the report; **every reported usage has a possible
  cause** (a toggle key of the table, a key that is down or was let go in the last 1.5 s and can produce it, or a one-shot modifier).
* After everything is released and 4 s have passed: nothing undecided, nothing armed or suppressed, no locked slot, no consumer; in
  `_exact` the reported usages are exactly the toggle keys that a model says are on (model: parity of the toggle-key presses since
  the last reset / table load) and the toggled layers match the model parity; in both, **loading a table always ends every toggle**
  (the way out).
* `python firmware/tools/mutate_layers_m6.py`: 44 mutations of `km_layers.c` (every decision of the new code), all killed; the
  equivalent ones that were tried are listed in the script. `--run` only builds and runs the layer suites.

### Cost (estimate, not measured on the keyboard)

Host timing of `km_layers_update_ms()` (gcc -O2, ns per call, before P2 / after): idle 29.3 / 29.0; 6 keys held 43.1 / 45.7; 6 keys
with one key pressed and released every call 50.9 / 54.0; 2 toggles on + 6 keys held 50.1; 2 morph keys suppressing Shift 44.3.
So: no change on an idle keyboard, +6 % with keys held, +16 % in the worst mixed case I could build. The existing function is
dominated by fixed work on every pass (clearing `usage_seen[256]` and `down[76]`, a loop over 76 slots for releases: about 700-900
cycles on the Cortex-M4 from instruction counts, not measured). What P2 adds on a pass: idle, two compares (about 6 cycles); toggled
usages, one `ADD_KBD` each (about 10 cycles, at most 16); an active suppress key, one pass over the pressed keys (about 8 cycles per
pressed key) before the report is built; the morph decision at the press event runs `raw_mods()` once (about 8 cycles per pressed key
plus the toggle list). **Worst pass: well under +500 cycles (5 % of the 9.3k-cycle frame), typical +10..50**: far inside the M6 plan
budget (average <= 1 k, worst <= 3 k). Code size: the object grew from 2.9 KB to 4.1 KB of text (+0.4 KB of state). Needs a real
`pass_cycles_max` reading in P10.

### What the integrator must do

* `km_layers_set_morph(prof->morph, prof->morph_n)` after every `km_layers_set_table(...)` (profile switch, settings commit, boot).
* Keep calling `km_layers_reset()` on boot / USB reset / fail-safe release: this is what ends the toggled usages there.
* Nothing else: the report builders already take `out->kbd`.
