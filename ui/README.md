# Fluxline — Magger60 configurator (M5 preview)

A static, build-less web configurator for the Magger60 hall-effect keyboard running this project's custom firmware:
live key depth, per-key actuation / Rapid Trigger, four layers, four profiles, calibration and health, and the keyboard's lights. Plain HTML + CSS +
ES modules, no npm install. Everything in `ui/` is original code and data; nothing is copied or extracted from a
vendor's software or firmware. Default language is Traditional Chinese (zh-TW); English is one click away. Dark theme
first, light theme included; works from phone width up.

## Run

```bash
cd ui
python serve.py            # http://localhost:8765/  (same as http.server, but with caching off)
# or: python -m http.server 8765
```

Open the URL in desktop **Chrome or Edge** (WebHID is needed only for a real keyboard; the demo keyboard works in any
modern browser). `localhost` counts as a secure context, so WebHID is allowed there.

Tests (pure logic and the device layer against the simulated keyboard; no browser, no installs, Node 20+):

```bash
cd ui && npm test          # = node --test
```

Cross-language vectors for the settings codec are generated from the real firmware code (needs a C compiler; on Windows
the MSYS2 ucrt64 gcc):

```bash
python firmware/tools/gen_settings_vector.py           # rewrites ui/tests/vectors/settings_vectors.json
python firmware/tools/gen_settings_vector.py --check   # exit 1 if the committed file no longer matches the firmware code
```

## What works with what

| | Demo keyboard (default) | Keyboard with our firmware (`KMFW`) | Keyboard with other firmware |
|---|---|---|---|
| Connect | built in, nothing to do | `Device -> Connect a keyboard` (WebHID, VID 0x19F5 / PID 0xFB49, configuration interface) | same button; the app asks the version first |
| What is sent | simulated, same code path | version query, then the configuration protocol `0xC6` (CONFIG_PROTOCOL.md) and the read-only scope channel `0xC5` | **only** the version query (command 3); everything else is blocked before it is sent |
| Settings (triggers, layers, active profile) | read / saved in a simulated flash kept in `localStorage` | read; written only by **Save to keyboard** after a confirmation | edits stay in this browser |
| Live depth | simulated fingers + your PC keyboard | polled with `LIVE_DEPTH` (about 30-50 Hz) | not available |
| Calibration, diagnostics, health | simulated | real (health counters only if the firmware publishes scope tag 0x0300) | not available |
| Lights (effect, brightness, speed, colour) | simulated, RAM only | `LED_GET` / `LED_SET`, only if `GET_INFO` feature bit 6 is set (km_l41 / km_l51); RAM only | not available |
| Animation page (play an animation live on the lights) | simulated keyboard takes the stream | command `0xC7` light frame stream (`STREAM_PROTOCOL.md`), only if `GET_INFO` feature bit 9 is set; RAM only, ends by itself about a second after the page stops sending | not available |

The demo keyboard is not a mock in the sense of "fake data": `MockDevice` is a `KmfwDevice` talking to `SimKeyboard`
(`js/device/sim_keyboard.js`, an independent implementation of CONFIG_PROTOCOL.md and the scope channel), so staging,
the confirmation words, read-back, BUSY, INVALID, lost packets and unplugging all behave as with hardware.
**Not verified on hardware:** everything in the second column. No real keyboard was available while this was built.

### Safety rules (enforced in code, covered by tests)

- Only these packets can ever be built (`js/device/config_protocol.js`, re-checked on the exact bytes by `assertSendable`):
  command 3 (version), command `0xC6` ops 1-9 plus `0x20` LED_GET / `0x21` LED_SET (the lights; `LED_SET` is checked for effect 0..6 and speed 1..255), command `0xC5` (read-only scope), command `0xC7` (the light frame stream: START / FRAME / STOP / KEEPALIVE only, checked byte by byte in `core/stream_protocol.js`). Raw `'_'` packets, XOR-masked
  packets and other commands are refused.
- Nothing 0xC6 is sent before the build string of command 3 carries `KMFW`.
- Every op that changes persistent state carries its own confirmation word: `SAVE` (commit), `PROF` (select profile),
  `RCAL` (recalibrate), `SCAL` (save calibration). The words are not interchangeable.
- `KmfwDevice.armWrites("SAVE")` arms exactly one write and is used up whether the write works or not. The UI arms
  right after the person pressed **Save** in the confirmation dialog.
- A save is: validate locally -> stage in RAM -> COMMIT (length + CRC-32) -> read everything back -> compare byte for
  byte. A keyboard that answers BUSY (a key is held) is retried for a few seconds, then the error is shown. On any
  failure the keyboard keeps its old settings (a half-staged blob is never used by the firmware).
- Editing never writes. The top bar says "Unsaved changes" until the keyboard holds exactly what the editor shows.
- The lights are the one exception to "ask first": `LED_SET` changes only the keyboard's RAM (no settings, no flash, no commit counter,
  no confirmation word in the protocol), the keyboard keeps its own current and brightness limits, and the page sends nothing until
  you touch a control. Tests prove the lights never move `flashWrites`, `commits` or the settings blob.

## What is stored where

| Data | Where |
|---|---|
| Per key: trigger (mode, actuation, release, rapid-trigger down / up steps, top / bottom dead zone, continuous), 4 layers of key codes; which profile is active | **on the keyboard**, in the 7944-byte settings blob (format version 2, `firmware/core/include/km/km_settings.h`); also per profile: the tap-hold / one-shot timing options |
| Profile names, layer names | **this browser only** (`localStorage` key `magger60-ui:meta`); the settings format has no place for them |
| The keyboard's lights (effect, brightness, speed, colour) | **the keyboard's RAM only**: gone at the next restart or unplug, back to the firmware's default (demo effect). The Lighting page says so. The browser keeps nothing. |
| Per-profile `lighting` and per-key `color` of the old browser-only lighting page | still carried in the profile JSON and in `meta` for compatibility, but no page edits them any more (kept so old exports still import) |
| The demo keyboard's simulated flash | this browser only (`magger60-ui:mock-flash`) |
| Language, theme, which profile you are editing | this browser only (`magger60-ui:prefs`) |
| Unsaved edits | memory only: a reload discards them (the browser asks first if there are any) |

"Transparent" on layers 1-3 is the key map's code `0xFF` and is stored as such. An overlay entry that equals the base layer's
is shown as transparent when read back (it does the same). Only for a firmware whose settings stop at 0xFA does the app write
transparent out as a copy of the base layer's code.

## Pages

- **Live**: depth bars on the layout, every key with its trigger marker, readout and graph of the selected key,
  update rate. When the firmware publishes the pressed-key mask (tag 0x0301) the "triggered" colour is what the
  keyboard really reports; otherwise it is the reference model in `core/trigger.js`.
- **Trigger**: click keys (Shift/Ctrl add, drag a box, row / column tabs, "Apply to all keys"), choose Normal / Rapid
  Trigger / Continuous RT. The controls follow what the engine uses per mode (ENGINE_NOTES.md): Normal = actuation +
  release; Rapid Trigger = actuation as the lowest re-trigger depth + press / release steps + dead zones; Continuous =
  the same without the actuation point. The travel diagram redraws while you edit and marks the 0.20 mm noise floor.
  A value outside a limit is moved and the field says why.
- **Layers**: four layers on the ANSI-60 layout, pick a key, pick an action from a searchable list (letters, numbers,
  modifiers, media keys, hold layer 1 / 2 / 3, switch to profile 1-4, transparent, no key). Fn is an ordinary key: hold layer 1 on it
  is only the factory default. Layers belong to a profile (the page says which one it edits). Layer names are browser-only.
  An informational hint appears when a layer has entries but no key switches to it. Actions the keyboard cannot store at all
  (mouse, system, key combinations) are not offered; an action the connected firmware's settings cannot store is marked with a warning. "Selected keys to default" and "Reset layer" restore the key map's own table (`firmware/core/LAYERS_NOTES.md`).
- **Lighting** (nav: Lighting / 燈光): the keyboard's own 61 lights. Pick one of the seven effects (off, static, breathing, rainbow wave,
  reactive, depth heat, demo; each has a one-line description), then brightness (0-100 %), speed (1-100) and colour. Controls the effect
  does not use are hidden (colour for rainbow / depth heat / off, speed for static / depth heat / off, brightness for off). Every change
  is sent at once with `LED_SET`; while a slider moves, requests are merged so only one is on the wire and the last value always wins.
  The page reads the current values with `LED_GET` when it opens, shows a live preview of the layout (an approximation: reactive and depth
  heat follow the real key depth, the fade-out after a release is not drawn), and says plainly that **the values are lost when the keyboard
  restarts** (persistence is a later firmware task). Without a keyboard, with other firmware, or without feature bit 6 (no lights / old
  firmware) the page shows a short explanation instead of controls. The page is `js/pages/lighting.js`; the wire format, scales and
  preview maths are `js/core/led.js`, the sender is `js/core/led_link.js`, the tests are `tests/led.test.mjs`.
- **Profiles**: four profiles; **Edit** picks the one the other pages show, **Make active** switches the one the keyboard
  uses (asks, stored on the keyboard), copy, reset, JSON import / export.
- **Device**: connection and every problem in plain words, firmware info, what works, engine diagnostics (scope tag
  0x0200) with **Recalibrate** / **Save calibration** (each asks first), and the stability counters (tag 0x0300) when present.

## Connection problems the UI explains (`js/device/errors.js`)

`unsupported` (no WebHID), `cancelled`, `stock_firmware` (read-only explanation), `proto_old`, `proto_new`,
`settings_size` (the keyboard's settings area is smaller than the blob), `crc`, `settings` (unreadable settings on the
keyboard), `busy`, `invalid`, `timeout`, `lost` (unplugged mid-transfer), `refused`, `other`. Each has a title and an
explanation in both languages (`problem.<kind>.title|body`).

## Layout of the code

```
index.html, css/            tokens.css, base.css, components.css, app.css, m5.css (sync bar, dialogs, trigger diagram, device)
js/main.js                  app shell: rail, top bar, sync bar, routing
js/state.js                 shared state: selection, profiles, device, sync with the keyboard (pull / save / activate)
js/sync.js                  load / dirty check / save flow, no DOM (unit-tested)
js/live-feed.js             depth stream -> pressed flags, history, press/release events
js/core/limits.js           THE constants module: firmware settings rules + UI limits (noise floor etc.)
js/core/settings_codec.js   encode / decode the km_settings v2 blob (v1 blobs are read and migrated; byte exact, CRC-32, every firmware rule)
js/core/profile_codec.js    UI profiles <-> settings blob (key ids <-> slots, bindings <-> key codes, transparent handling)
js/core/profile.js          profile schema v2 (+ optional `layerOpts`), defaults, validation, v1 migration, layer reachability / toggle traps
js/core/trigger.js          trigger defaults, normalizeTrigger (the rules), reference model of pressed state
js/core/led.js              the lights: effect table (what each effect uses), LED_SET / LED_GET bytes, % and 1-100 scales, preview colours
js/core/led_link.js         sender behind the Lights page: merges changes, one request in flight, newest wins (no DOM, unit-tested)
js/core/                    units, layout (ansi60.js, wiring.js), HID usages, lighting (old browser-only model), raw_codec
js/device/                  Device.js (interface), KmfwDevice.js, MockDevice.js, WebHidDevice.js (read-only shim),
                            sim_keyboard.js (the simulated keyboard), config_protocol.js, ConfigClient.js, scope_client.js,
                            hid_transport.js, errors.js, legacy_cmd3.js
js/pages/                   live, performance (Trigger), remap (Layers), lighting, profiles, device
js/ui/                      keyboard canvas (SVG), graph (canvas), controls, kb-card, dialog (confirmations), sync-bar
js/locales/                 zh-TW.js + zh-TW-m5.js (default), en.js + en-m5.js
tests/                      node --test; tests/vectors/settings_vectors.json is generated from the firmware code
```

### One place for the limits

`js/core/limits.js` holds every number that decides what a trigger setting may be. The UI limits are tighter than the
firmware's settings format because of the sensor noise (about +-0.15 mm at rest): nothing below 0.20 mm. They follow
`km_engine_cfg_valid` of M5 task T-A (commit a441c13): rapid-trigger steps and (non-zero) dead zones >= 0.20 mm,
`rt_down <= travel - bottom dead zone`, `max(top, 0.20) + bottom + 0.20 <= travel`. When the engine rule changes,
change `limits.js` only; `tests/trigger-limits.test.mjs` fails if another file defines its own limit.

### Device interface (`js/device/Device.js`, `KmfwDevice.js`)

```
status, kind ("mock" | "kmfw" | "webhid"), readOnly, capabilities {live, readProfile, writeProfile}, info
features {settings, selectProfile, recalibrate, saveCalibration, live, lights, diag, health, keyMask}   // detected at connect
problem {kind, detail}                                    // why the last connect / transfer failed
connect(), disconnect(), getInfo()
readSettings() -> Uint8Array            writeSettings(blob)       // needs armWrites("SAVE"), stages + commits + verifies
selectProfile(i, "PROF")  recalibrate("RCAL")  saveCalibration("SCAL")
readLights() -> {effect, brightness, speed, r, g, b}   writeLights(state) -> the state now in force   // features.lights; RAM only, no arming
readEngineDiag() readHealth() readKeyMask()                       // null when the firmware does not publish them
subscribeLive(cb)   cb(depth: Uint16Array(76), tMs)               // 0.01 mm per unit, indexed by sensor slot
events: "status", "info"
```

## Units, layout, wiring

- Depth and all trigger distances are **0.01 mm** in the depth array and in the blob, **mm** in the UI and JSON
  (`core/units.js`). Travel is 0-3.40 mm.
- The layout is a plain ANSI 60% key list written for this project (`js/core/ansi60.js`, 61 keys, ids follow the browser's
  `KeyboardEvent.code` names, Esc in the top-left corner and Fn third from the right as on the board).
- Profiles address keys by **key id** (`"KeyW"`); the blob and the depth array by **sensor slot** (0..75).
  `js/core/wiring.js` maps one to the other (board wiring from the project's own analysis).

## Profile JSON (version 2, our own format; version 1 files still import)

Exported by `Profiles -> Export`, validated on import (`core/profile.js: validateProfile`). A file that fails validation is
rejected as a whole and the errors are listed (path + reason). Version 1 files (two layers, no release point) are upgraded
on import: layers 2 and 3 become transparent, the release point is derived (`actuation - 0.4`), values below the noise
floor are lifted to it.

```jsonc
{
  "kind": "fluxline-profile",          // required, exact
  "version": 2,                        // required, integer; files from a newer version are refused
  "name": "FPS",                       // string, <= 32 chars, may be "" (UI shows "Profile n")
  "lighting": { "effect": "static", "brightness": 80, "speed": 2, "color": "#5eead4" },   // browser-only
  "keys": {                            // keyed by key id; missing keys get defaults
    "KeyW": {
      "trigger": {
        "mode": "rapid",               // "normal" (FIXED) | "rapid" (RAPID, continuous 0) | "continuous" (RAPID, continuous 1)
        "actuation": 1.0,              // mm, 0.30..3.40
        "release": 0.8,                // mm, 0.20..3.30, at least 0.10 below the actuation point
        "rapidDown": 0.25,             // mm, 0.20..3.40 (downward travel that triggers again)
        "rapidUp": 0.25,               // mm, 0.20..3.40 (upward travel that releases)
        "topDead": 0.2,                // mm, 0 or 0.20..1.00
        "bottomDead": 0.2              // mm, 0 or 0.20..1.00
      },
      "bindings": [ { "type": "key", "usage": 26 }, null, null, null ],   // [base, Fn, layer 2, layer 3]; null = transparent
      "color": "#ffffff"               // per-key colour (browser-only)
    }
  }
}
```

Binding types (usage numbers are the public USB HID Usage Tables values): `{type:"key", usage, mods?}`,
`{type:"media", usage}`, `{type:"layer", layer:1..3}`, `{type:"profile", index:1..4}`, `{type:"none"}`, and for the
browser-only model `mouse`, `wheel`, `system`. **The keyboard stores** keys without modifiers (0x04..0xE7), the six media
keys of the key map (volume up / down, mute, previous, play / pause, next), hold layer 1, switch profile 1-4, none and
(as above) transparent. Anything else is listed under the Save button and blocks it; nothing is dropped silently.

## Firmware rules mirrored here, and gaps still open

The settings rules are the integrated firmware's (branch `claude/m5-integration`): `km_settings_cfg_valid` is `km_engine_cfg_valid`
(FIXED looks only at press / release; RAPID checks distances >= 0.20 mm, dead zones 0 or >= 0.20 mm, the actuation point unless
continuous, `rt_down <= travel - bottom dead zone`, and that the zones leave room), `km_settings_usage_valid` is `km_keymap_code_valid`
(0, 0x04..0xE7, the extended codes 0xE8..0xEA with their argument bytes, and every code from 0xF0 up). `js/core/settings_codec.js` /
`limits.js` implement exactly that, and `tests/settings-codec.test.mjs` proves it against vectors generated from the firmware code
(50 key-config probes including RAPID boundaries, all 256 usage bytes, 292 table-entry probes with arguments, 17 layer-option probes,
40 corrupted blobs, 39 whole-settings cases, 12 version 1 blobs that must migrate or be refused the way the firmware does).

1. (Fixed in the integration) `km_settings_default()` now copies `km_keymap_default_table()`: the firmware's default blob is the key map
   table (Fn holds layer 1, overlay entries 0xFF) and equals the UI's own default, byte for byte (`tests/profile-codec.test.mjs`).
   A blob from an older firmware whose default read function codes as 0 still loads (Fn shows as "no key").
2. The settings format's comment in `km_settings.c` says "0xF0..0xFC and 0xFF"; the code accepts every code >= 0xF0 (0xFD / 0xFE too).
   The UI never writes 0xFD / 0xFE.
3. (Fixed in the integration, kept as history) the configuration protocol buffer of 4096 bytes was smaller than the 5488-byte blob; the
   UI still checks `max_settings` on connect and shows "settings area too small" for such a firmware.
4. A firmware that stops at 0xFA (older builds) is still handled: `workspaceToBlob(..., rule)` writes transparent out as the base layer's
   code; covered by a test with a narrower rule.
5. **Settings format version 2 (task L4).** The blob grew to 7944 bytes (per key-layer entry two argument bytes, per profile 6 bytes of
   layer options). The configuration protocol buffer is 8192 bytes. A keyboard still running a format-1 firmware (settings area 6144
   bytes) is refused on connect with the "settings area too small" message: write the matching firmware first. Version 1 blobs
   (an older save, the demo keyboard's stored flash) are still read and are written back as version 2.

## Layers page: toggle, tap-hold and one-shot keys

Besides the earlier actions the list offers **Toggle layer 1..3** (press: on, press again: off; ends with a profile switch) and the
**One-shot modifiers** group (tap, then the modifier goes with the next key). A **tap / hold key** needs two choices and has its own
composer under the list: choose what a tap sends (any keyboard key or one of the six media keys) and what holding does (hold layer
1..3 or a modifier), then "Give to selected keys". Selecting a tap / hold key fills the composer with its two choices.
The "Timing" box (always visible, per profile, stored on the keyboard) sets the hold time (30..1000 ms), how another key pressed
meanwhile is treated (only by time / typing-friendly "permissive hold" / instant "hold on other key") and how long an armed one-shot
modifier waits (0 = for the next key, else 100..10000 ms). A note appears when a toggled layer could be switched on but no key
switches it off again. Bindings in the profile JSON: `{type: "toggle", layer}`, `{type: "oneshot", usage}` and
`{type: "taphold", tap: {type: "key"|"media", usage}, hold: {type: "layer", layer} | {type: "key", usage: 224..231}}`; the options are
`layerOpts: {tapTermMs, oneShotMs, tapPolicy: "timeout"|"permissive"|"other-key"}` (optional in a file, defaults are filled in).

## Using the keyboard canvas

Click selects a key; Shift/Ctrl-click adds; drag on empty space draws a box; the numbers above/left of the keys select a
column/row; arrow keys move, Space selects, Shift+arrows extend, Ctrl+A selects all, Esc clears.

## Still needs real hardware

The lights (`LED_GET` / `LED_SET` on a real keyboard: the answers, how fast sliders can be followed over USB, that the preview resembles
what the keyboard shows), connecting through real WebHID, the real firmware's timing (live rate, flash write time, BUSY behaviour), the health tag
layout (decoded from the T-E branch's `km_health.h`, not from a running keyboard), the engine diagnostics layout (from
`m4_main.c`), and the feel of the trigger settings. The reference model for "triggered" in `core/trigger.js` is not the
engine; it is replaced by the keyboard's own mask when the firmware publishes one.
