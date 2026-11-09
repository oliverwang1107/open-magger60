# Hardware evidence log

What was actually done on a real keyboard, in order, and how each result was checked. One keyboard (the author's), one
hardware revision, Windows 11. Dates are 2026. This is a summary; the plan files hold the details
([M2](../M2_PLAN.md), [M3](../M3_PLAN.md), [M4](../M4_PLAN.md), [M5](../M5_PLAN.md), [M6](../M6_PLAN.md), all zh-TW).

<!-- TODO(status): the 8 kHz row and the M5 rows are written from commit messages and the author's reports; add the measured numbers (scope tags 0x0601-0x0604) to USB8K_NOTES.md and here before publishing -->

**Method.** Every step that writes to the keyboard needed the owner's explicit go-ahead, a fresh read-only backup beforehand
and a known way back. After each series the stock firmware was flashed back and a new backup was compared byte for
byte with the previous one. A second keyboard was always within reach.

## Ladder

| Date | Step | What was done | Evidence / result |
|---|---|---|---|
| 10-27 | Backup | `magger60_backup.py --run`: bootloader (32 KB) and settings area (96 KB), each region read twice and compared | Both reads identical; the folder was copied to a second computer before anything was flashed |
| 10-27 | M2-B round trip | Stock firmware + the backed-up settings flashed through `magger60_flash.py` (14,359 packets) | No errors; keyboard returned to normal mode; new backup identical to the old one except two words in the bootloader's own status sector, as predicted |
| 10-27 | M2-C rescue drill | A valid but dead app (never feeds the watchdog) flashed | Keyboard vanished from USB; holding ESC + left CTRL (no replug) brought the bootloader back (`19F5:AF1F`); stock flashed back, backup identical |
| 10-27 | M2-D1 alive | Own start-up code (clock, vector table, watchdog feed) | Ran without being reset; stock restored, backup identical |
| 10-27 | M2-D2 USB | Own polled USB HS device driver | Enumerated under Windows on the first try; stock restored, backup identical |
| 10-27 | M2-D3 version | Own firmware answers the version command | `magger60_info.py` printed the own build string |
| 10-27 | M2-D4 enter bootloader | Own firmware writes the boot flag (its first flash write) and resets | `magger60_enterboot.py --run` took the running image to update mode twice with no key held; backup afterwards identical to the one before |
| 10-28 | M3-S1 polled ADC | One polled sensor reading per key | Released/pressed values for ESC and left CTRL moved as expected |
| 10-28 | M3-S2 scan | Polled 76-slot scan | First build did not enumerate (cause not proven; rescued with ESC + left CTRL; one flash write error at one offset, retry fine); second build with bounded waits worked |
| 10-28 | R1 key map | Every key pressed once | 61 distinct slots, 15 slots without a key; table kept in `firmware/core/data/slotmap_measured.json` |
| 10-28 | M3-S3 DMA scan | Timer-triggered ADC + DMA scan plus injected channels for the WASD keys | About 39 µs per four-phase frame (about 25.8 kHz), no DMA errors; pressing keys moved exactly the expected slots |
| 10-28 | M4-K1 descriptors | Own USB descriptors only (no scanning, no key reports) | Enumerates as the keyboard with a boot interface, a vendor interface and an NKRO/consumer interface; version query works |
| 10-28 | M4-K2 | Scan + engine, only ESC allowed through the gate | ESC reached Windows; the other keys did not |
| 10-28 | M4-K3 typing | All keys enabled | 60 of 61 keys typed correctly in a scripted test (the 61st, Fn, has no function in that image), at most 6 keys at once. One unexplained event: Windows saw SPACE held for about 50 s while the firmware reported no key down (another keyboard or a lost release report; not reproduced) |
| 10-28 | Stock restored | Stock firmware flashed back through the enter-bootloader command | New backup identical (bootloader incl. status sector, and settings) |
| 10-28/29 | M5 L31 | Full-feature image with the settings kept in RAM (rapid trigger, layers and profiles, configuration protocol, stability fail-safes) | Reported working by the author; the results of the individual checklist steps (layers, tap/hold, one-shot keys, profile switch) and of the 8-hour soak and hot-plug series of the plan are not recorded in this repository |
| 10-29 | M5 L33 first flash save | First image that writes the settings sectors (the riskiest step of the ladder, because it is a new kind of flash write) | **The first settings save failed** with a flash programming error. Cause found by experiment: the controller refuses to program an 8-byte unit below one already programmed in the same 16-byte line, so the record header is now written in ascending order and interrupts are masked around each unit. Saves then succeeded and survived an unplug |
| 10-29 | M5 L33 second fix | The same save, in some builds still refused on the second unit of a line | Cause: the two word stores of one unit must reach the controller back to back, and a flash instruction fetch between them breaks that. The programming window now runs from RAM and uses one `STRD`. Four settings saves in a row, a calibration save and the restore after replug worked |
| 10-29 | M5 `km_l51` | L33 plus the lights (SPI + DMA output) at full hardware level | Lights run; settings and calibration saves succeed while the lights run (no LED timeouts, no flicker); calibration restored and settings identical after replug |
| 11-02 | M6 base image `km_m6a` | The M6 integration image (key behaviours, lights, streaming, animation hooks, gamepad descriptors off by default) | Ran on the keyboard, but felt laggy in a rhythm game where `km_l51` felt smooth. Not measured at that time. Started the latency investigation (`LATENCY_NOTES.md`). The keyboard was then put back on the stock firmware at the author's request |
| 11-04 | 8 kHz `km_m6h` | Keyboard and NKRO endpoints with 125 µs polling | The scope diagnostics showed the keyboard enumerated at high speed (about 8000 start-of-frame per second) but the endpoints still advertised 4 microframes: the 8 kHz setting was only in the compile flags of the image, not in the shared descriptor library. Fixed, and `check_image.py` now reads the interval back out of the built binary. After the fix the author reports 8 kHz polling working on the keyboard (`USB8K_NOTES.md`; the date of the successful run and the figures are still to be added) |

## Not done yet

- The power-cut handling of the settings store, the calibration log and the lights log is tested against simulated flash
  only (a cut at every step of every save). Nobody has pulled the cable during a real save.
- Hardware-untested, host-tested only: per-profile lighting (`LIGHT_PROFILE_NOTES.md`), the sensing filter modes and the images
  that carry them (`FILTER_NOTES.md`), calibration drift compensation (`DRIFT_NOTES.md`), the individual M6 key behaviours
  (SOCD, dynamic keystrokes, toggle and mod-morph keys), the gamepad descriptors, light scenes, the light stream and the animation.
- The configurator has only talked to the simulated keyboard; WebHID against the real keyboard, and the Cloudflare deployment
  in `deploy/`, have not been tried.
- Behaviour after hot-plugging, long-term stuck-key behaviour, other keyboards or hardware revisions: untested.
- No debug probe has been attached to this keyboard; the rescue path above is the only recovery that has been proven.

## What the host tests do and do not prove

The host test suite (millions of checks, including mutation tests that deliberately break the code) proves the logic
against simulated sensors, simulated registers and simulated flash. It cannot prove the timing, the analogue behaviour or
the interaction with real USB hosts; that is what the ladder above is for.
