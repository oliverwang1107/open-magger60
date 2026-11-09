# The portable core as WebAssembly (prototype)

The keyboard's hardware-free C code (settings blob, key map, layers, the key-config rule in the key engine) is compiled
for the browser, so the configurator can ask **the code the keyboard runs** instead of keeping a second, hand-written
implementation in sync. Original work; nothing here comes from a vendor.

| Piece | Where |
|---|---|
| flat C API (about 25 exports, no malloc, no libc) | `firmware/wasm/km_wasm_api.c`, `wasm_libc.c`, `include/string.h` |
| build script | `tools/build_wasm.py` (`--check` needs no compiler) |
| result + build record | `ui/wasm/km_core.wasm`, `ui/wasm/km_core.meta.json` (hash of every input) |
| JavaScript wrapper (optional, loaded by `ui/js/main.js` as `window.kmCore`) | `ui/wasm/km_wasm.js` |
| layer vectors from the native build | `firmware/tools/gen_layer_vectors.py` -> `ui/tests/vectors/layer_vectors.json` |
| parity tests | `ui/tests/wasm-parity.test.mjs` (part of `npm test`) |

## What the module does

`defaultBlob`, `decode` (accept / refuse / migrate a version 1 blob, returns the blob re-encoded), `encode`, edit calls
(`setCfg`, `setEntry`, `setOpts`, `setActive`) plus `valid`, the single rules (`usageValid`, `entryValid`, `optsValid`,
`cfgValid`), `crc32`, and the layer engine (`layersLoad(profile)`, `layersUpdate(downSlots, nowMs)`: the keyboard usages,
the active layer, toggled layers, armed one-shots, profile-switch request and the media usage that a set of pressed keys
turns into). The JavaScript side has no twin of the layer engine, so this is new capability, not only a cross-check.

## Parity tests

`ui/tests/wasm-parity.test.mjs` (394 checks) replays on the wasm module
* every vector the firmware produced for the settings codec (`settings_vectors.json`): the default blob, the format 1/2 edit cases,
  usage / entry / option / key-config probes, migrations, corrupted blobs, **and the format 3 (M6, task P0) vectors**: 88 edit cases
  (DKS, Mod-Morph, SOCD, gamepad curves, device options, 50+ rejections) written by the wasm writer and compared byte for byte
  including the version byte, 151 damaged and 42 hand-built v3 blobs, the exhaustive entry rules (10 codes x 65,536 argument pairs in
  both versions) and the list of key slots. Wherever a JS codec answer exists for the same probe, the same test demands that too;
* fourteen 200-300-key-press scenarios (`layer_vectors.json`: plain, FN, media / profile keys, toggle, tap-hold incl. a key as hold action,
  one-shot, three tap policies, a wrapping millisecond clock, M6 codes, and the P2 toggle keys and Mod-Morph records with and without suppression) whose expected output **and locked table entries**
  (`km_layers_locked_entry`) were made by the native gcc build of the real `km_layers.c`;
* that `km_core.wasm` was built from the C files that are in the tree (hash check), so a core change without a rebuild turns
  the suite red with the sentence "rebuild with python tools/build_wasm.py".

Not in the module: the DKS / SOCD / pad runtime (their settings records are covered; their behaviour can be added as more exports).
The toggle-key and Mod-Morph output of `km_layers` (task P2) is in: `layersUpdate` returns `toggleKeys` and `suppressed`, and `layersLoad` hands
the profile's Mod-Morph records to `km_layers_set_morph`.

## Toolchain (decision and cost)

The machine had no wasm-capable C compiler (gcc from MSYS2 cannot target wasm; no clang, emscripten, zig; Rust has the
`wasm32-unknown-unknown` target but cannot compile C). Used: the **`ziglang` Python wheel** (Zig 0.16.0 bundles clang 21.1 and
lld), 99 MB download, installed into a throw-away virtual environment `build/wasm-toolchain` (git-ignored), nothing
system-wide. `tools/build_wasm.py` finds a compiler in this order: `$KM_WASM_CC`, clang + wasm-ld, zig, that virtual
environment. Alternatives that work the same: MSYS2 `mingw-w64-ucrt-x86_64-clang` + `lld` (several hundred MB), Emscripten (not needed:
no libc, no JS glue). The module is built for the WebAssembly 1.0 instruction set (`-mcpu=mvp`), `-Wall -Wextra -Werror`
clean, and the build is deterministic (same inputs and compiler give the same bytes).

## Numbers (Node 22, this PC)

| | |
|---|---|
| module size | 30,674 bytes raw (about 11 KB gzip) (settings format 3 and the larger decoder doubled it) |
| load + instantiate | about 2.2 ms |
| decode + re-encode of a 7,944-byte blob | wasm 102 microseconds, the JS codec 326 microseconds (about 3x faster; irrelevant for a human, interesting only as a sanity check) |
| one layer update (4 keys down) | 1.2 microseconds |
| linear memory | one 64 KiB page in total (static data, a 16 KiB stack; `km_settings_t` about 8 KB and the 8 KB I/O buffer are static) |

## Risks and effort

* **Coupling.** The glue touches struct members in a few places (`km_engine_key_cfg_t`, `km_layer_opts_t`, the M6 records of
  `km_profile_t`); they are listed at the top of `km_wasm_api.c`. Moving from settings format 2 to format 3 (task P0) needed
  one new group of setters and no change to the blob-level calls; a future struct change fails to compile there at once. The blob-level calls
  (`decode`, `encode`, `defaultBlob`, `crc32`) use only the public functions and need only a rebuild.
* **Stale binary.** `km_core.wasm` is committed (so the page works without a compiler, and the Cloudflare deployment ships it).
  Stale-ness is caught by `npm test` (hash of all inputs), and `python tools/build_wasm.py --check` says the same without any
  compiler. A CI step that rebuilds and compares the bytes is possible once the compiler version is pinned (not done).
* **Single instance, no re-entrancy.** One settings object and one layer state live in the module; two pages would share
  them. Fine for one configurator; a worker-per-use design would need an instance per user.
* **Decoder errors are yes / no.** The C decoder returns `bool`, so the module cannot say *why* a blob was refused; the JS codec
  still gives the reasons shown to people (`short`, `crc`, `version`, ...). A reason code in the C API would close this, and
  it is the natural thing to ask of P0.
* **Security.** The page's Content-Security-Policy needs `'wasm-unsafe-eval'` for `script-src` (already in
  `deploy/cloudflare/_headers.template`); no other relaxation.
* **Is it worth replacing the JS codec?** Not now. The hand-written codec has the human-readable errors and ships no binary; the
  parity tests already make it fail whenever it differs from the firmware, which is the benefit wanted. Switching would
  add a binary to review and a toolchain to the build for a codec that is fully covered. The module becomes valuable for
  things that **cannot** be duplicated cheaply: the layer engine (a typing preview, "what would this keymap send?", checking tap-hold
  timing) and, later, the engine's trigger logic for a simulated key depth preview.
