# Documentation index

Where to find what. Most plan and notes files are currently written in **Traditional Chinese (zh-TW)**; the protocol,
firmware-module and UI documents are in English. A full English translation of the plan files is planned.
Start with the [top-level README](../README.md) for status, safety and how to build and flash.


## I want to ...

| Goal | Read |
|---|---|
| understand what works on real hardware and what does not | [README.md](../README.md) (status table), then [HARDWARE_LOG.md](HARDWARE_LOG.md) |
| flash a keyboard, or get back to stock | [README.md](../README.md) (put it on a keyboard), [RESTORE_STOCK.md](RESTORE_STOCK.md) |
| know the risks and the legal position | [DISCLAIMER.md](../DISCLAIMER.md), [SECURITY.md](../SECURITY.md) |
| write a tool that talks to the bootloader | [BOOTLOADER_ANALYSIS.md](../BOOTLOADER_ANALYSIS.md) (zh-TW) |
| write a client for the settings channel | [CONFIG_PROTOCOL.md](../CONFIG_PROTOCOL.md) (English) |
| run or change the configurator | [ui/README.md](../ui/README.md) (English) |
| put the configurator on the web, private to you (Cloudflare Workers + Access) | [deploy/cloudflare/README.md](../deploy/cloudflare/README.md) (zh-TW) |
| read the firmware's design | [firmware/README.md](../firmware/README.md) and the module notes below |
| contribute | [CONTRIBUTING.md](../CONTRIBUTING.md) |

## Project plan and milestones (zh-TW)

| File | Contents |
|---|---|
| [CUSTOM_FIRMWARE_PLAN.md](../CUSTOM_FIRMWARE_PLAN.md) | The overall plan: scope, levels L0–L6, recovery matrix, decisions |
| [M2_PLAN.md](../M2_PLAN.md) | L0 "alive": backup, round trip, rescue drill, own start-up (D1), USB (D2), version query (D3), enter bootloader (D4), with the hardware results |
| [M3_PLAN.md](../M3_PLAN.md) | L1 "visible": the sensing scan (S1–S4) and its results |
| [M4_PLAN.md](../M4_PLAN.md) | L2 "typing": depth model, key engine, USB descriptors; ladder K1–K3 |
| [M5_PLAN.md](../M5_PLAN.md) | L3 "usable": stability rules, rapid trigger, settings store, layers, configuration protocol |
| [M6_PLAN.md](../M6_PLAN.md) | L4–L6 "playable": settings format v3, key behaviours (SOCD, DKS, toggle, mod-morph), gamepad, lights and animation; work packages |

## Hardware and bring-up notes (zh-TW)

| File | Contents |
|---|---|
| [FIRMWARE_Magger60.md](../FIRMWARE_Magger60.md) | Hardware facts only: chip, clock, flash layout, peripheral addresses, pins, rules for custom firmware |
| [BOOTLOADER_ANALYSIS.md](../BOOTLOADER_ANALYSIS.md) | Observed bootloader behaviour and the update protocol; why flashing without a probe is safe enough |
| [USB_BRINGUP_NOTES.md](../USB_BRINGUP_NOTES.md) | The USB HS device driver: boot order and lessons |
| [SENSING_BRINGUP_NOTES.md](../SENSING_BRINGUP_NOTES.md) | The sensing driver (mux, ADC, DMA, timer): structure, hardware results, known problems |
| [K1_NOTES.md](../K1_NOTES.md) | The own USB descriptors and report sending (task T-C) |
| [LATENCY_NOTES.md](../LATENCY_NOTES.md) | Why the M6 image felt laggy: measurement plan, ranked causes, the fast report path, the 8 kHz plan |
| [USB8K_NOTES.md](../USB8K_NOTES.md) | 8 kHz USB reports: what was built, the diagnostics (scope tags 0x0601-0x0604), and the root cause found on hardware |

## Firmware module notes (English)

| File | Contents |
|---|---|
| [firmware/README.md](../firmware/README.md) | Directory layout, how to run the tests, the safety rules in code (zh-TW) |
| [firmware/core/ENGINE_NOTES.md](../firmware/core/ENGINE_NOTES.md) | Key engine, key map, HID reports |
| [firmware/core/LAYERS_NOTES.md](../firmware/core/LAYERS_NOTES.md) | Layers, FN key, media keys |
| [firmware/core/STABILITY_NOTES.md](../firmware/core/STABILITY_NOTES.md) | Fail-safes: health monitor, progress-gated watchdog, report reconciliation |
| [firmware/core/SETTINGS_NOTES.md](../firmware/core/SETTINGS_NOTES.md) | Settings blob format 3 (sections), the lowest-version write rule, compatibility |
| [firmware/core/PIPELINE_NOTES.md](../firmware/core/PIPELINE_NOTES.md) | The M6 key pipeline (SOCD -> layers -> DKS / gamepad / light actions), hold-over rules, cost estimate |
| [firmware/core/SOCD_NOTES.md](../firmware/core/SOCD_NOTES.md) | SOCD filter (opposing keys): modes, rules, cost |
| [firmware/core/DKS_NOTES.md](../firmware/core/DKS_NOTES.md) | Dynamic keystroke: events, rules, cost |
| [firmware/core/PAD_NOTES.md](../firmware/core/PAD_NOTES.md) | Gamepad core: bindings, curves, the 15-byte report |
| [firmware/usb/USB_PAD_NOTES.md](../firmware/usb/USB_PAD_NOTES.md) | USB side of the gamepad: descriptor variants, fall-back, host lock lights, diagnostic images (zh-TW) |
| [firmware/core/NOISE_NOTES.md](../firmware/core/NOISE_NOTES.md) | Raw sample capture image for measuring the sensor noise |
| [firmware/core/FILTER_NOTES.md](../firmware/core/FILTER_NOTES.md) | Candidate sensing filters evaluated on the host |
| [FILTER_NOTES.md](../FILTER_NOTES.md) | Where the latency of a key press goes, the filter modes built into the firmware (`KM_ENGINE_FILTER_MODES`), evaluation on measured noise; **not hardware-verified** (zh-TW) |
| [DRIFT_NOTES.md](../DRIFT_NOTES.md) | Calibration drift compensation (`km_drift`, image `km_m6d`): design, simulation, hardware test plan; **not hardware-verified** (zh-TW) |
| [firmware/core/CALIBRATION_NOTES.md](../firmware/core/CALIBRATION_NOTES.md) | Persisting the learned per-key calibration in flash: record format, write rules, power-cut tests |
| [LED_NOTES.md](../LED_NOTES.md) | The lights: wire format, light order, power line, limits, idle-off, saving the settings, light scenes and layers (zh-TW) |
| [LIGHT_PROFILE_NOTES.md](../LIGHT_PROFILE_NOTES.md) | Lights per key profile: flash layout, backward compatibility; **not hardware-verified** |
| [STREAM_PROTOCOL.md](../STREAM_PROTOCOL.md) | Wire format of the light frame stream (command `0xC7`) |
| [docs/BADAPPLE_NOTES.md](BADAPPLE_NOTES.md) | The low-resolution animation: clip format, converter, player (zh-TW) |
| [firmware/wasm/WASM_NOTES.md](../firmware/wasm/WASM_NOTES.md) | The portable core compiled to WebAssembly for the configurator: API, parity tests, toolchain, numbers, risks |
| [CONFIG_PROTOCOL.md](../CONFIG_PROTOCOL.md) | Wire format of the settings channel (command `0xC6`), protocol version 2 (device status, light scene) |
| [ui/README.md](../ui/README.md) | The configurator: pages, codecs, simulated keyboard, tests |

## Tools

All host tools are in [`tools/`](../tools/) and start with a docstring that says what they do, whether they can write,
and how to run their `--self-test`. `python tools/run_tool_selftests.py` runs them all against simulated keyboards.

| Tool | Writes to the keyboard? | Purpose |
|---|---|---|
| `magger60_backup.py` | no (read-only allow-list) | back up the bootloader and the settings area |
| `magger60_image.py` | no | build and check a flashable image |
| `magger60_flash.py` | **yes** (update protocol only, dry run by default) | flash an image in update mode |
| `magger60_enterboot.py` | one boot-flag write by the firmware | ask a running keyboard to restart into update mode |
| `magger60_info.py` | no | read the version string |
| `magger60_scope.py` | no | live view of the raw sensor values (custom firmware) |
| `magger60_config.py` | settings (custom firmware, with confirmation word) | read/write the settings blob |
| `magger60_compare_backups.py` | no | check that two backups differ only where allowed |
| `magger60_soak.py`, `magger60_depth_check.py`, `magger60_keymap_measure.py`, `magger60_slot_ranges.py` | no | long-run typing test, depth checks, measurements |
| `magger60_noise.py` | no (reads a RAM capture through the scope channel) | measure the real sensor noise with the diagnostic image |
| `magger60_led.py` | light settings (custom firmware; can save them to flash) | read/change the light effect, brightness, colour, idle-off time |
| `magger60_stream.py` | no flash writes (frames are shown, not stored) | send light frames to the keyboard in real time |
| `magger60_pad_map.py`, `magger60_padtest.py`, `magger60_xinputtest.py` | no | gamepad report layout / SDL mapping, view the USB gamepad from the PC, view what XInput reports |
| `badapple_convert.py` | no | turn a video **you own** into a light-animation clip (the tool is ours; no video or sound is in this repository) |
| `make_local_clip.py` | no | turn a video you own into a *local* clip for the configurator's animation page (`ui/local/`, git-ignored, never published) |
| `build_wasm.py` | no | build the portable core as WebAssembly for the configurator |
| `hid_survey.py` | no (Windows, query only) | list HID devices |
| `svd_to_annotations.py` | no | turn your own copy of a CMSIS-SVD file into a register label table |
| `run_tool_selftests.py` | no | run all tool self-tests |

## Not in this repository (on purpose)

The manufacturer's firmware, installers, web configurator and the dumps of a keyboard's flash (`extracted/`, `backup*/`,
`images/`, `vendor/` are git-ignored). Anything you need from them you have to get yourself.
