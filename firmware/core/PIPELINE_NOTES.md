# Key pipeline (M6 package P10): km_pipeline

`km_pipeline.[hc]` is the one place that decides the ORDER in which the key engine's pressed list passes the M6 modules. It is pure
logic (no hardware, no allocation, no waiting), so `tests/test_pipeline_m6.c` runs the same code the keyboard runs. The image
`port/n32h48x/m6_main.c` (a copy of `m5_main.c` with the additions) only supplies the callbacks (`km_engine_depth`), the clock and the
report paths.

```
engine pressed list (oldest first)
  -> km_socd_filter       opposing keys: the losing key vanishes from the list (the layers never see it)
  -> km_layers_update     layers, tap-hold, one-shot, toggle keys, Mod-Morph, media keys, profile keys  -> keyboard usages
  -> for the keys that are down: km_layers_locked_entry() says what each key is LOCKED to
       KM_CODE_DKS      -> km_dks_update   extra keyboard usages, merged into the keyboard list (once each)
       KM_CODE_PAD_*    -> km_pad_update   one gamepad state (a 15-byte report of its own, IF3)
       KM_CODE_LIGHT    -> light actions   reported ONCE per press (the firmware changes effect / brightness / speed / on-off, RAM only)
```

## Rules

* **A table that has no DKS / gamepad / light-action entry costs nothing extra**: `set_profile()` scans the 4 x 76 table codes once; the
  per-key lookups (`km_layers_locked_entry`) are skipped when the profile has none.
* **Reset** (`km_pipeline_reset`): layers (toggle keys, tap-hold, one-shot, latched codes), SOCD, DKS, gamepad, light-action memory. The
  pipeline does it itself on the first refused pass (`allowed == false`, a health fault); `m6_main.c` also calls it on a USB bus reset,
  followed by `km_pipeline_hold_over()`.
* **Hold-over**: keys that are down when the pipeline is reset, when a fault ends, or when a profile is activated are ignored by DKS, the
  gamepad and the light actions until they have been released once (no output comes back by itself). The layer logic treats them as new
  presses, as it always did.
* **Gamepad epoch** = table generation x 8 + the layer a key pressed now would use: a layer change zeroes the sticks and holds the keys over.
  `km_pad_update` marks the keys it finds down at its FIRST update after a reset as held over, so the pipeline makes that update happen in
  the pass right after the reset (`pad_poke`) and keeps calling it while any key is held over (so it sees the release).
* **Engine settings** (`km_pipeline_slot_cfg`): a slot that carries a DKS entry on any layer is FIXED with press = t_act, release = t_act - 10
  (the engine's press / release are DKS events 1 / 4); a slot that carries a gamepad entry is FIXED with press 0.20 mm, release 0.10 mm. The
  first such entry, layer 0 first, decides. The user's RAPID settings on such a key are replaced.
* **What may be on with every key up**: toggled keys (until pressed again, a reset or a table change) and armed one-shot modifiers (until the
  next key; with the one-shot timeout 0 for ever). Nothing else: the random test checks this after every run.

## Cost (estimate: module notes + instruction counts, NOT measured on the chip)

The main loop is a "consumer": the scan runs by DMA, a pass that is too long only delays the next frame (and counts an `overrun` in scope tag
0x0100 when a frame is lost). What matters is the LONGEST pass, not the average. Added by M6 on top of the km_l51 pass (engine 3.4k..4.8k only
in the pass that consumes a frame; layers about 0.9k; USB / reports / sync about 1k; one lights slice 1.5k..2.5k):

| M6 part | typical pass | worst realistic pass | source |
|---|---|---|---|
| pipeline overhead (clock, clearing the output, table flags) | 0.2k | 0.3k | counted |
| SOCD | 0.25k..0.4k | 1.2k (4 DEEPER conflicts, 10 keys) | `SOCD_NOTES.md` |
| lookups of locked entries (about 45 cycles per pressed key) | 0.3k (6 keys) | 0.6k (10 keys) | counted |
| layers: Toggle / Mod-Morph additions | +0.01k..0.05k | +0.5k | `LAYERS_NOTES.md` |
| DKS | 0 idle, 0.75k (4 keys held) | 1.2k (4 keys, 2 events in one pass) | `DKS_NOTES.md` |
| gamepad (recompute once per ms; the other passes cost about 40) | 1.6k (WASD on the left stick) | 3.2k (8 analog keys) | `PAD_NOTES.md` |
| light actions, scene glue, indicator inputs | < 0.05k | 0.1k | counted |
| stream packet (one 64-byte report parsed and copied) | 0 | 1k | `km_stream.h` |
| animation tick | < 0.02k | 1k (frame boundary) | `km_anim.h` |
| indicator rules in a lights slice | +10 % of a slice | +0.8k | `LED_NOTES.md` 18.6 |

Everything at once is a pass of about 8.5k (km_l51 worst) + 0.3 + 1.2 + 0.6 + 0.5 + 1.2 + 3.2 + 1 + 1 + 0.8 = **about 18k** only if a stream packet,
an animation frame boundary, a gamepad recompute, a DKS event and a lights slice with rules all land in the SAME pass as an engine frame, which
the structure makes unlikely (they are spread over time). A game session with WASD on the gamepad, a few keys and the stream off is about
8.5k + 0.3 + 0.4 + 0.3 + 1.6 + 0.1 = **about 11k in the pass that also recomputes the gamepad**: slightly above one frame (9.3k..10.4k), one pass
per millisecond. The honest summary: M6 stays inside the plan's "worst pass <= 3k added" per feature, but the sum of the heaviest features can
exceed one frame in a single pass; whether that costs frames is what scope tag 0x0500 [2] (longest pass) and [3] (passes above 9300 cycles)
and tag 0x0100 (overruns) measure on the keyboard (`M6_TEST_GUIDE.md` section 3). If it does, the cheapest levers are: the lights slice
(`M5_LED_WORK_UNITS`), computing the gamepad every 2 ms, and skipping the stream copy while a key is down.

## Tests

`tests/test_pipeline_m6.c`: equivalence with the plain layer logic, engine settings, the order of the stages, DKS events with a simulated
engine, gamepad, light actions once per press, hold-over after a profile change and a fault, faults dropping everything, 400 random runs of
1500 passes (random valid tables including toggle keys, Mod-Morph, tap-hold, DKS, gamepad and light codes, random profile changes, faults,
resets) with the structural invariants every pass and the "all keys up" invariant at the end, bounded work. `tools/mutate_pipeline.py`: 35
mutations, none survives (3 documented equivalents).
