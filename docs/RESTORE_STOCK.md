# Restoring the stock firmware

You can always go back to the manufacturer's firmware as long as the keyboard's **bootloader is intact**. Nothing in this
project writes to the bootloader, but there is no debug probe access: if the bootloader were damaged by something else,
nobody can repair it from here. Read [DISCLAIMER.md](../DISCLAIMER.md) first.

## What you need

1. **The manufacturer's firmware update file** for your keyboard (a `.bin` of about 230 KB, the file their updater
   sends). This repository does not contain it and never will; get it from the manufacturer's download page. Keep it.
2. **A backup of your own keyboard's settings area** made with `magger60_backup.py` before you first flashed anything.
   The update protocol rewrites the settings area together with the app, so the image you flash must contain *your*
   settings (including the per-key calibration), not the factory defaults from the update file. `magger60_image.py`
   does that for you and refuses to build an image without it.
3. A second keyboard, and the keyboard connected directly to the PC (no hub).

If you do not have a backup of your settings area, stop and ask for help in an issue before flashing: without it the
stock firmware's calibration for your keys is lost.

## Step by step

```bash
# 1. Put the keyboard into update mode (USB ID 19F5:AF1F). Pick the case that matches:
#    a) this project's firmware is running (images from D4 on): 
python tools/magger60_enterboot.py --run --confirm ENTER-BOOT
#    b) anything else, including a frozen or dead app: unplug USB, hold ESC + LEFT CTRL, plug in, keep holding ~8 s.
python tools/magger60_flash.py --list            # must show the bootloader (PID AF1F)

# 2. Build the stock image: manufacturer app + YOUR settings area.
python tools/magger60_image.py build --vendor /path/to/your-stock-update.bin --settings backup1/settings_0x08028000.bin --out images/stock.bin
python tools/magger60_image.py check images/stock.bin --settings backup1/settings_0x08028000.bin

# 3. Dry run, read the output, then flash.
python tools/magger60_flash.py --image images/stock.bin --backup backup1
python tools/magger60_flash.py --image images/stock.bin --backup backup1 --run --confirm FLASH-0x08008000
```

The flasher refuses to run without a valid backup folder, refuses a keyboard that is not in update mode and refuses an
image that fails the checks (app-ID block, boot flag, stack pointer, reset vector, size). If it stops halfway, the
keyboard stays in update mode: run the same command again.

## Check afterwards

```bash
python tools/magger60_backup.py --run --out backup_after
python tools/magger60_compare_backups.py backup1 backup_after
```

Expected: the bootloader code and your settings are byte-identical. The one expected difference is the bootloader's own
status sector (`0x08006000`–`0x08007FFF`), which every update rewrites. The tool flags it as "different in sector
0x08006000" when you compare with a backup made *before* your first update; that single sector is fine, any other sector
is not. Compare with a backup taken *after* an earlier update and the result is "identical". The keyboard should be back
on `19F5:FB49` and behave as before.

## If something goes wrong

| Symptom | What to do |
|---|---|
| The keyboard is gone from USB after a flash | Unplug, hold ESC + left CTRL, plug in, hold about 8 seconds. It should show up as `19F5:AF1F`; flash again. |
| The flasher stops with an error | Do not unplug in the middle of an update. Start update mode again and re-run the same command. |
| It does not show up as `AF1F` even with the keys held | Try another USB port and cable, connect directly, release and retry. If it still does not work the bootloader may be damaged; there is no repair path here. |
| Keys behave oddly after restoring | The settings area may differ from your backup; compare with `magger60_compare_backups.py`, then re-flash with the image built from the correct backup. |

---

## 繁體中文摘要

只要**bootloader 沒壞**，隨時都能刷回原廠。需要：(1) 廠商的韌體更新檔（本專案不含，請自行向廠商取得）；
(2) 你自己在第一次刷寫前做的設定區備份（含每個鍵的校正）；(3) 備用鍵盤。流程：進更新模式（自製韌體在跑時用
`magger60_enterboot.py`，否則拔掉 USB、按住 **ESC＋左 CTRL** 再插上，約 8 秒）→ 用 `magger60_image.py build --vendor`
把廠商 app 和**你的**設定區合成映像 → 先乾跑再 `--run --confirm FLASH-0x08008000` → 重新備份並用
`magger60_compare_backups.py` 比對。沒有備份就先不要刷。bootloader 壞了無法修復（沒有除錯探針）。
