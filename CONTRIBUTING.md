# Contributing

Thank you for looking. This is a small hobby project around one keyboard, so please keep changes focused and honest
about what has and has not been tested.

## The one hard rule: no vendor material

Do **not** submit anything derived from the keyboard manufacturer's firmware, installers, drivers, web configurator
or documentation that is not public. That includes:

- firmware images or parts of them, lookup tables or constants copied from them;
- decompiler or disassembler output, or code or pseudo-code you wrote by following it line by line;
- code, packet samples or data tables from the manufacturer's web driver or desktop software;
- data files "generated from" any of the above.

Allowed sources: public specifications (USB, HID and its usage tables, the chip vendor's public datasheets, reference
manuals and SVD files), measurements you made on your own keyboard (say how you measured), and your own design.
Facts about the device (an address, a field, a protocol step) are fine when you write them in your own words and
code, and say how you know them. If you are unsure whether something qualifies, ask in the issue first and do not
paste it.

New test data should come from a generator or measurement script that is in the repository, never from a pasted dump.

## Hardware safety rules

The keyboard has no debug probe, so a bad write can be permanent. Changes that can reach a keyboard are held to these:

1. **Never write below `0x08008000`** (bootloader area). Do not add a way to write an arbitrary flash address. The only
   flash write the firmware makes today is the boot flag in the settings sector; keep it behind `km_flash_guard`.
2. Tools that talk to a keyboard are **dry-run by default**, use allow-lists, re-check every outgoing packet, and need
   an explicit confirmation to write. Keep each new command either on the allow-list with a test, or out.
3. Every main loop feeds the watchdog from the first instruction. Keep the "enter bootloader from the host" command
   and the ESC + left CTRL rescue working in every image that can be flashed.
4. A change that is only host-tested must say so. Do not claim hardware results you do not have; record the
   evidence (what was flashed, what was observed, how the stock firmware was restored) in the relevant plan or notes file.
5. Follow the ladder: build and test on the host first, then flash one small step at a time with a fresh backup and a
   known way back. Never flash an image without a dry run of the flasher first.
6. Pull requests must not include built firmware images, backups of a keyboard's flash, or serial numbers.

## Before you open a pull request

```bash
python firmware/tools/run_tests.py                   # host unit tests
python tools/run_tool_selftests.py                   # every host tool against simulated keyboards
python firmware/tools/build_crashtest.py --target km_l31   # the ARM image still builds and fits the memory map
cd ui && npm test
```

Security problems: see [SECURITY.md](SECURITY.md). Everything else: open an issue.
Please open an issue to discuss a change before sending a pull request.

Code style: match the surrounding code (C11, warnings as errors). Add a test for
every behaviour change; for safety-relevant code also add a deliberate-break (mutation) check where the repository has
one. By contributing you agree that your contribution is licensed under the Apache License 2.0.
