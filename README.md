# open-magger60

Open custom firmware, host tools and a configurator for the **Luminkey Magger60 HE** hall-effect keyboard
(Nations N32H48x, Cortex-M4F). An unofficial hobby project, built step by step on the author's own keyboard.

> **Warning: flashing firmware can permanently brick a keyboard.** There is no warranty, this project is not affiliated
> with the manufacturer, and no vendor files are included. Read [DISCLAIMER.md](DISCLAIMER.md) before you run anything
> that talks to a keyboard. Everything that does not touch a keyboard (host tests, the simulated UI) is safe to run.

*(繁體中文摘要在[文末](#繁體中文摘要)。)*

## What this is, and what it is not

The keyboard ships with a closed firmware and a closed web configurator. This project runs **original firmware** on it,
and builds everything needed to do that safely without a debug probe: a portable C core with host-side tests, a USB
stack, a sensing driver, a depth model and key engine (rapid trigger, layers, profiles, a settings store), an image
builder and flasher that only speak the keyboard's own USB update protocol, and a browser-based configurator.

It is **not** a finished product and not yet something to rely on as your only keyboard (see the status below). It is
not a drop-in replacement for the manufacturer's configurator and does not try to be compatible with it.

## Status: what has been shown on real hardware, and what has not

<!-- TODO(status): re-check this table against the newest hardware results on the day you publish (last checked against the notes on 2026-11-08) -->

Everything in the "shown on hardware" rows was done on one keyboard (the author's); the evidence is summarised in
[docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md) and written down step by step in the plan files (currently in Traditional
Chinese; English documentation is planned, see [docs/README.md](docs/README.md)). After each series of experiments the
stock firmware was flashed back and the keyboard's flash was read back and compared byte for byte with the earlier backup.

| Step | What it showed | State |
|---|---|---|
| Read-only backup | Dump of the bootloader and settings area using only read commands from an allow-list | shown on hardware |
| Round trip | Stock firmware flashed back through the update protocol; readback identical to the backup (apart from one expected bootloader status word) | shown on hardware |
| Rescue drill | A deliberately dead app was flashed; holding ESC + left CTRL while plugging in USB brought the bootloader back | shown on hardware |
| D1 "alive" | Own start-up code, 240 MHz clock, watchdog feed; the bootloader did not reset it | shown on hardware |
| D2 USB | Own USB device driver enumerated as a composite HID device under Windows | shown on hardware |
| D3 version query | Own firmware answers the version command (`cmd 3`) with its own build string | shown on hardware |
| D4 enter bootloader | A USB command makes the own firmware set the boot flag (its first flash write) and reset into the bootloader | shown on hardware |
| S1–S3 sensing | Own ADC + multiplexer + DMA scan of all key sensors: all 61 keys mapped one-to-one by pressing them; about 25.8 kHz hardware scan rate | shown on hardware |
| K1 own USB descriptors | Own boot-keyboard, vendor and NKRO descriptors enumerate under Windows; the version query works | shown on hardware |
| K2 / K3 typing | Own depth model + key engine + HID reports: only ESC reached Windows in K2; in K3, 60 of the 61 keys typed correctly in a scripted test (the 61st, Fn, has no function in that image), at most 6 keys at once | shown on hardware |
| M5 L31 / L33 (rapid trigger, layers, profiles, settings store in flash, calibration, config protocol, stability fail-safes) | `km_l31` (settings in RAM) and `km_l33` (settings and calibration saved to flash) run on the keyboard. The first settings save on real hardware **failed** with a flash programming error; two fixes in the flash driver later, saves succeed and survive an unplug (see [docs/HARDWARE_LOG.md](docs/HARDWARE_LOG.md)). `km_l51` adds the lights and saves settings, calibration and lights while they run | shown on hardware (one keyboard; the 8-hour soak and hot-plug series are **not** written up) |
| 8 kHz USB reports (`km_m6h`) | Keyboard and NKRO endpoints advertise 125 µs polling; the author reports it works on the keyboard. A first attempt did not take effect because the descriptor setting never reached the compiled descriptors; the on-hardware diagnostics found that (`USB8K_NOTES.md`) | shown on hardware (author's report; the final measurements are not yet written into the notes) |
| M6 features (SOCD, dynamic keystrokes, toggle / mod-morph keys, gamepad, light scenes and streaming, animation) | Own implementations, host-tested (millions of checks, mutation tests). The M6 base image `km_m6a` has been flashed and runs, but felt laggy in a rhythm game, which started the latency work (`LATENCY_NOTES.md`) | host tests; only the base image was tried on hardware, the individual features are **not** hardware-verified |
| Per-profile lighting, sensing filter modes (`km_m6l` / `km_m6q` / `km_m6f`), calibration drift compensation (`km_m6d`) | Designed, implemented and host-tested (`LIGHT_PROFILE_NOTES.md`, `FILTER_NOTES.md`, `DRIFT_NOTES.md`) | host tests only, **not flashed yet** |
| Configurator (`ui/`) | Runs against a simulated keyboard; the same codec and protocol code is tested against the firmware's own test vectors. Pages: live view, trigger, layers, behaviours, gamepad, lights, animation, profiles, device | simulated; **not verified against a real keyboard in a browser** |

Known open points, stated plainly:

- The first flash of the second sensing image (S2) reported a write error at one offset and the resulting image did not
  enumerate on USB; the rescue worked and a second version worked. The real cause of the first failure is **not known**.
- During the K3 typing test, Windows once saw a space bar held for about 50 seconds while the firmware reported no key
  down. It could not be reproduced or explained (another keyboard, or a lost release report). Watch for stuck keys.
- The settings store's power-cut handling and the flash-write retry path are only tested against a simulated flash
  controller. Ordinary saves work on the keyboard, but nobody has pulled the cable in the middle of one.
- The measured noise-based filter evaluation (`FILTER_NOTES.md`) and the drift module (`DRIFT_NOTES.md`) are built on
  noise measured on one keyboard; whether they help in real use has not been tested.
- The configurator and the web deployment (`deploy/`) have not been tried against a real keyboard through WebHID.
- **Sporadic large input lag (known, not fixed).** On the author's keyboard, typing has sometimes become delayed by roughly
  100-200 ms for every key until the keyboard was unplugged and plugged in again. The firmware's counters showed key reports
  waiting a long time for the host to collect them (the longest wait is saturated at 65 ms in the counter), but the cause
  is **not known**: it may be in the firmware's USB handling (the suspend/resume path only counts events) or on the host.
  It was seen on the 1 kHz images; earlier reports of lag in rhythm games may be the same problem. Treat the firmware as
  experimental, keep a way to restore the stock firmware, and report it if you see it.
- Only one keyboard (one hardware revision) has been used.

## Layout

```
firmware/core/            portable C11 core, no hardware access: sensor scan accumulation, depth model, key engine
                          (rapid trigger), key map and layers, settings store, health monitor, HID report builders,
                          flash-write guard
firmware/hid/             vendor-channel protocol (64-byte packets): version query, read-only "scope" channel that
                          publishes sensor values, the configuration protocol (CONFIG_PROTOCOL.md), recognition of
                          the one exact enter-bootloader request
firmware/usb/             USB descriptors, control-request policy and IN-report bookkeeping (hardware independent)
firmware/port/n32h48x/    the chip-specific part: start-up, linker script, USB HS driver, ADC/DMA/timer sensing driver,
                          flash code, and one small main per bring-up step (d1..d4, s1..s3, k1..k3, l30..l51, m6a..)
firmware/tests/           host unit tests (also compile the chip-specific flash and sensing code against simulated registers)
firmware/tools/           test runner, ARM build/check scripts, mutation-test and generator scripts
tools/                    host tools (Python): backup, image builder, flasher, info, scope viewer, config tool, soak test,
                          enter-bootloader, key-map measurement, device survey
ui/                       static, build-less web configurator with a simulated keyboard (HTML + ES modules)
docs/                     documentation index and the hardware evidence log
```

The core never touches hardware and the chip-specific code never contains policy, so most of the logic can be
tested on an ordinary PC.

## Build and test (no keyboard needed)

Requirements: a C compiler for the host (`gcc` or `clang`), Python 3.13, Node.js 20 or newer for the UI tests. For
the ARM images: `arm-none-eabi-gcc`, CMake 3.20+ and Ninja. Only the flasher, backup, scope and config tools need a
keyboard and `pip install hidapi`.

```bash
# host unit tests (plain gcc, no make needed)
python firmware/tools/run_tests.py

# cross-compile the ARM images and check them against the memory map
python firmware/tools/arm_smoke.py
python firmware/tools/build_crashtest.py --target km_l31    # or km_k3, km_l33, km_s3, ...
# or with CMake directly
cmake -S firmware -B build-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=firmware/cmake/arm-none-eabi.cmake
cmake --build build-arm --target km_l31.elf

# self-tests of all host tools against simulated keyboards (no hardware)
python tools/run_tool_selftests.py

# configurator (simulated keyboard); open http://localhost:8765/ in Chrome or Edge
cd ui && npm test            # logic tests, no install step
python serve.py
```

On Windows, put the MSYS2 `ucrt64` toolchain first on `PATH`; the scripts do that for you when it is installed in
`C:\msys64`. Building does not flash anything: the build scripts only produce files.

## Put it on a keyboard (only if you accept the risk)

Do this only on the same model (Magger60 HE, USB `19F5:FB49`) and with a second keyboard within reach. Every step that
writes is a dry run until you add `--run` and the confirmation phrase; read the output of each dry run first.

```bash
pip install hidapi

# 1. Read-only backup of the bootloader and the settings area. Copy the folder to another computer.
python tools/magger60_backup.py --list
python tools/magger60_backup.py --run --out backup1

# 2. Build an app and turn it into a flashable image that keeps YOUR settings area from the backup.
python firmware/tools/build_crashtest.py --target km_l31          # prints the path of the .bin
python tools/magger60_image.py build --app <that .bin> --settings backup1/settings_0x08028000.bin --out images/l31.bin

# 3. Put the keyboard into update mode (USB 19F5:AF1F):
#    - stock firmware running: unplug, hold ESC + LEFT CTRL, plug in, keep holding for about 8 seconds;
#    - this project's firmware running (D4 and later): python tools/magger60_enterboot.py --run --confirm ENTER-BOOT
python tools/magger60_flash.py --list

# 4. Dry run first, then the real flash.
python tools/magger60_flash.py --image images/l31.bin --backup backup1
python tools/magger60_flash.py --image images/l31.bin --backup backup1 --run --confirm FLASH-0x08008000

# 5. Check that it is alive, then compare a fresh backup with the old one after you restore stock (see below).
python tools/magger60_info.py
```

`images/` and `backup*/` are git-ignored on purpose: backups contain the manufacturer's bootloader, which must never be
published. Details of what the update protocol does and why it cannot touch the bootloader: [BOOTLOADER_ANALYSIS.md](BOOTLOADER_ANALYSIS.md).

## Restore the stock firmware

You need the manufacturer's firmware update file for your keyboard; it is not part of this repository and you must get
it from the manufacturer yourself. Short version: enter update mode (hold **ESC + left CTRL while plugging in USB**, or
use `magger60_enterboot.py` while this project's firmware runs), build a stock image from your update file and your own
settings backup, and flash it:

```bash
python tools/magger60_image.py build --vendor /path/to/your-stock-update.bin --settings backup1/settings_0x08028000.bin --out images/stock.bin
python tools/magger60_flash.py --image images/stock.bin --backup backup1 --run --confirm FLASH-0x08008000
```

Full walkthrough with the checks to make afterwards: [docs/RESTORE_STOCK.md](docs/RESTORE_STOCK.md).
If the bootloader itself were ever damaged, nothing in this repository can repair it (there is no debug probe access).

## Safety philosophy

The keyboard has no debug probe attached, so the approach is "never write what you cannot get back" plus several
independent checks:

- **Dry run by default.** The flasher, the backup tool and the enter-bootloader tool print what they would send and
  do nothing until you pass `--run` (and, for writes, an explicit `--confirm` phrase).
- **Allow-lists, checked twice.** Each tool can only build packets from a short list of known commands, and every
  outgoing packet is re-parsed by an independent guard before it is sent. The backup tool cannot send write commands;
  the flasher can only send the update-protocol commands, with the data equal to the image bytes.
- **Never the bootloader area.** The update protocol the flasher speaks cannot address anything below `0x08008000`;
  the flasher refuses to start without a verified backup, refuses a keyboard that is not in update mode, and checks
  the image (app-ID, boot flag, stack pointer, reset vector, size) first. The firmware's own flash writes are limited to
  the settings sectors and guarded in code and tested against a simulated flash controller.
- **Backups and byte-exact comparison.** Before every flash a fresh backup is made; after the stock firmware is
  flashed back, a new backup is compared with the old one.
- **A simulator for every tool,** plus deliberately broken variants (mutation tests) to prove the tests would notice.
- **Watchdog from the first instruction,** and a progress-gated watchdog in the full images, so a hung app resets itself.
- **Rescue:** hold **ESC + left CTRL while plugging in USB** to start the keyboard in its bootloader (update mode,
  USB ID `19F5:AF1F`), then flash a good image. This was verified on the author's keyboard with a dead app. It only
  works if the bootloader is intact.

None of this makes flashing risk-free. See [SECURITY.md](SECURITY.md) for what to report and [DISCLAIMER.md](DISCLAIMER.md) for the legal side.

## Documentation

[docs/README.md](docs/README.md) is the index: plans and results per milestone, the protocol and bootloader notes,
and the per-module design notes in `firmware/`. [CONFIG_PROTOCOL.md](CONFIG_PROTOCOL.md) is the wire format of the
settings channel; [ui/README.md](ui/README.md) describes the configurator.

## Contributing, security, license

See [CONTRIBUTING.md](CONTRIBUTING.md) (in short: no vendor-derived material, and follow the hardware safety rules) and
[SECURITY.md](SECURITY.md). Apache License 2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Copyright 2026 yaya.

---

## 繁體中文摘要

這是為 **Luminkey Magger60 HE** 磁軸鍵盤（N32H48x，Cortex-M4F）寫的**非官方自製韌體**，連同主機端工具、
測試與網頁設定介面。作者用自己的鍵盤一步一步做出來。

- **風險：刷寫可能讓鍵盤永久變磚。** 沒有任何保證；這個專案與廠商無關，也不含任何廠商檔案或程式碼。
  請先讀 [DISCLAIMER.md](DISCLAIMER.md)。主機端測試與模擬介面不碰鍵盤，可以安心執行。
- **目前進度（誠實版）：** 已在作者的鍵盤上驗證：備份、刷回原廠並逐位元組比對、救援（插 USB 時按住 ESC＋左 CTRL）、
  自己的啟動程式、USB 列舉、版本命令、USB 命令進 bootloader、自己的感測掃描（61 鍵全部對應）、自己的 USB 描述元，
  以及用自己的深度模型和按鍵引擎打字（K3：61 鍵中 60 鍵打字正確，Fn 在該映像沒有功能）。
  M5 的快速觸發、圖層、設定檔、設定儲存（含校正）、燈光（`km_l31`／`km_l33`／`km_l51`）也已在鍵盤上跑過；
  第一次存設定時 flash 寫入失敗，修了兩次驅動才穩定。8 kHz USB 回報（`km_m6h`）依作者回報可用。
  M6 的按鍵行為、手把、燈光場景與動畫、逐設定檔燈光、濾波模式、漂移補償目前只通過主機端測試，**還沒在鍵盤上驗證**。
- **沒有除錯探針：** 如果 bootloader 被寫壞就無法修復，所以工具預設乾跑、用白名單、不會寫 bootloader 區域，並要求先備份。
- **不用鍵盤就能試：** `python firmware/tools/run_tests.py`（主機測試）、`python tools/run_tool_selftests.py`、
  `cd ui && npm test`、`python ui/serve.py`（模擬鍵盤）。
- **刷寫與還原原廠：** 步驟見上面的英文章節與 [docs/RESTORE_STOCK.md](docs/RESTORE_STOCK.md)；救援方式是拔掉 USB、按住 ESC＋左 CTRL 再插上。
- 詳細的階段文件目前是繁體中文，索引在 [docs/README.md](docs/README.md)。
