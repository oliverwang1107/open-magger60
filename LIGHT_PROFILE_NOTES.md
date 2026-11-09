# Lights per key profile (design and test notes)

Status: implemented on branch `claude/profile-lights-6a3ed1` (firmware `km_m6a` / `km_m6`, UI lights page). Not flashed yet.

## What the user gets

* Each of the four key profiles (Fn+Z / X / C / V, or the config protocol, or the UI) can have **its own lights**: effect, brightness, speed, colour and idle-off time.
* Switching profile switches the lights at once. Nothing is written to flash when you switch (RAM only), so the scan loop never waits.
* A profile that never had its own lights follows the **shared (global) lights**. On a keyboard that was running the older single-record firmware, the old lights become the shared lights, so nothing is lost.
* The **key-colour scene** (per-key colours, indicator rules, animation settings: `km_lightscene.h`) stays **global** for now. Reason: a scene blob is up to 214 bytes and has two 8 KB flash sectors already; four per-profile scenes would need new sectors. The new table reserves a `scene reference` byte (must be 0 today) so a later version can add it without a format break.

## Flash layout (no new sector)

Same sector as before, `0x08036000` (`KM_FLASH_LED_CONFIG`, `km_flash_guard.h`), still a `km_clog` append-only log, but with a different record:

| | version 1 (old) | version 2 (new, `km_ledprof.h`) |
|---|---|---|
| payload | 16 bytes, one light configuration | 64 bytes, **the whole table** |
| slot (stride) | 32 bytes | 80 bytes |
| saves per sector erase | 256 | 102 |

The table is 4 header bytes (`version = 2`, `mask`, 2 zero bytes) + 5 entries of 12 bytes: entry 0 is the **global fallback**, entries 1..4 are profile 0..3. An entry is `effect, brightness, speed, r, g, b, idle_s (u16 LE), scene_ref (0), 3 zero bytes`. `mask` bit i = entry i is set; a cleared entry must be all zeros. Decoding is strict (wrong version, stray bits, non-zero reserved bytes, invalid effect/speed = "no table").

Why the whole table in every record: one valid record always has all five entries, so the once-in-102 sector erase loses nothing, and a power cut during a save leaves the previous record (the same power-fail properties as the existing log, which is unchanged code).

### Backward compatibility (old sector, old blob)

* **Boot** reads the newest valid **v2** record. If there is none, it reads the newest valid **v1** record of the same sector (a 16-byte `km_clog` view of the same sector) and uses it as the global entry. An empty or damaged sector gives the built-in defaults, as before.
* **First v2 save on an old sector**: the 16-byte slots look like foreign slots to the 80-byte log. They are skipped (never programmed twice) and the new record goes behind them, **without an erase** unless the sector is completely full (then one erase first; the global lights are part of the new record, so only a power cut exactly inside that erase can lose them: lights fall back to the defaults).
* **Downgrade** to a v1 firmware: it finds no valid v1 record newer than the old ones and shows the last global lights it knew. Harmless.
* `km_m5` / `km_l51` images keep the 16-byte log and one set of lights (they pass the old log to `m5_led_persist_attach`; profile calls are no-ops there).

## Firmware behaviour

* `m5_led_set_profile(p)` (called from `cfg_select_profile`, `cfg_commit` and at boot after the attach) applies profile p's lights from the **RAM copy** of the table: own entry, else global, else built-in defaults, and wakes the lights if they were faded out. No flash access.
* `LED_SET` with SAVE stores into the **active** profile's entry. The save is the existing deferred, coalesced, idle-only background job (`km_ledstore_t`, one flash operation per main-loop pass, RAM-resident programming window of `store_flash.c`). The only change: the job carries the 64-byte table (`km_ledstore_request_raw`).
* An unsaved live preview is replaced by the stored lights of the profile you switch to.
* A save for another profile is accepted too (protocol below); it is stored without touching the lights now.

## Config protocol (still version 2, additive)

* New feature bit 11 (`0x0800`, `KM_CFG_FEAT_LED_PROFILE`), set only when the extended light ops carry the per-profile callbacks. A host that does not know the bit never sends the new forms; an older firmware answers them `BAD_LENGTH`.
* `LED_GET` with two argument bytes `1, profile` → the usual 12 bytes about that profile. `LED_SET` with ten bytes (the nine + `profile`) → the usual 12-byte reply about that profile. `profile` = 0..3, or 255 = the profile in force; anything else is `BAD_ARG`.
* The active profile behaves exactly like the nine-byte `LED_SET`. Another profile **requires the SAVE flag** (otherwise `RANGE`: there is nothing to preview on keys that are not showing it); it applies nothing now.
* Status bit 5 (`0x20`, `PROFILE_OWN`): the profile has lights of its own (clear = it follows the global lights). The same bit appears in the nine-byte/one-argument replies for the active profile.
* The nine-byte and plain six-byte forms are unchanged and address the active profile.

## UI

* `LedKeeper` takes `{ profile, live }`. The lights page edits the profile selected in the top bar. If the keyboard is in that profile (`live`), changes go out live (always to "the profile in force", 255) and the save names the profile. If not, nothing is sent live, only one coalesced save to that profile, and the page says so.
* A banner says whose lights these are and whether they follow the shared lights. Changing the selected profile (top bar, Profiles page "Make active") rebuilds the page for that profile; an unsaved change is saved to the old profile first.
* The demo keyboard (`MockDevice`) models the same forms. `profile_codec.js` is unchanged on purpose: the lights are **not** in the settings blob (they have their own ops and sector); the browser-only `profile.lighting` field keeps its fully saturated defaults.

## Decisions I made (change them if you disagree)

1. Global fallback entry kept (not "every profile always has its own"): it is what carries an old keyboard's lights over.
2. Scene stays global (see above).
3. A profile with nothing stored shows built-in defaults when there is no global entry either.
4. Per-profile `LED_SET` for a non-active profile needs `SAVE`.
5. No "clear this profile's own lights" command yet (the table code supports it: `km_ledprof_clear`).
6. The key-profile switch by keyboard (Fn+Z/X/C/V) is not reported to the open UI live; the UI learns the active profile on connect / after its own switch.

## Test on hardware (after flashing km_m6a, then `local/apply_profile.sh`)

1. Plug in, open the UI, connect. Lights page shows "profile 1" banner.
2. Set profile 1 to red static. Wait ~3 s ("saved" on the page). Press Fn+X: lights change to the **shared/default** lights (profile 2 has nothing yet).
3. On profile 2 set blue breathing, wait for "saved". Fn+Z → red; Fn+X → blue. Repeat with Fn+C / Fn+V.
4. Unplug and replug. Fn+Z/X/C/V still give red / blue / defaults.
5. In the UI pick profile 3 in the top bar while the keyboard is in profile 1: the banner says the keys will not change; pick a colour; wait for "saved"; the keys stay red. Press Fn+C: the keys change to the colour you stored.
6. Typing / gaming feel while pressing Fn+Z/X/C/V quickly and while dragging a slider: no stalls (the switch is RAM only).
7. Upgrade path: a keyboard whose lights were set with the previous firmware keeps them as the shared lights (profiles without own lights).

## Risks

* The v2 log has 102 saves per erase (was 256): wear is still negligible (a save needs 3 s of quiet and 10 s gap).
* First save on a sector completely full of v1 records needs one sector erase; a power cut exactly inside it loses the lights (defaults). Everything else in flash is untouched.
* Not exercised on real flash; the host tests use the simulated flash. The shared programming code (`km_clog`, `store_flash.c`) is the one already proven on the keyboard; only the record size changed (80-byte stride, still 16-byte aligned).
* RAM +136 bytes, flash +1.7 KB for `km_m6a`.
