# Security and safety

This is hobby firmware for one keyboard model. "Security" here means two different things.

## 1. Hardware safety (the one that matters most)

Flashing can permanently disable a keyboard. The rules the tools and the firmware follow, and the rescue path, are in
[README.md](README.md#safety-philosophy) and [DISCLAIMER.md](DISCLAIMER.md). In short:

- no write below `0x08008000` (the bootloader area) is possible through the tools, and the firmware never writes there;
- every writing tool is a dry run until you add `--run` and its confirmation phrase, and the flasher needs a verified backup;
- rescue: unplug USB, hold **ESC + left CTRL**, plug in (the keyboard starts in its bootloader, `19F5:AF1F`);
- a damaged bootloader cannot be repaired from here (no debug probe access).

Do not flash on a keyboard you cannot do without, and keep a second keyboard within reach.

## 2. Software security

What exists and what does not:

- **No network code.** The firmware only talks USB. The configurator in `ui/` is a static page that runs in your browser,
  stores profiles in your browser only and uploads nothing; it talks to the keyboard through WebHID.
- **The keyboard's USB protocol is unauthenticated.** Any program on the PC that can open the keyboard's vendor HID
  interface can read the sensor values and, with the right sequence, change settings or ask the keyboard to restart into
  its bootloader. Commands that write settings need a multi-step sequence and read back, which protects against
  accidents, not against a hostile program on your PC. The firmware cannot be flashed by a command alone: the update
  itself is done by the manufacturer's bootloader.
- **No key logging.** The firmware does not keep a history of key presses. The "scope" channel publishes raw sensor
  values (live), which reveal which keys are being pressed at that moment to any program that can read it.
- The project has had **no formal security review**. It is not meant for any use where a malfunctioning keyboard could
  cause harm.

## Reporting a problem

- A flaw that could damage hardware, or lets a program write somewhere it should not: please report it privately first.
- Anything else (bugs, stuck keys, documentation errors): open an issue.

Private reports: use the repository's "Report a vulnerability" button (GitHub private vulnerability reporting) once it is
enabled.

Please include the firmware image or commit, what you did, what you expected, and what happened. Do not attach backups
of a keyboard's flash (they contain the manufacturer's bootloader).
