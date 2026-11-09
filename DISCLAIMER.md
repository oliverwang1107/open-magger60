# Disclaimer

Please read this before you build, flash or run anything from this repository.

## 1. Risk: this can permanently damage your keyboard

This project replaces the firmware of a keyboard. Flashing firmware can leave a keyboard unusable.

- **The author has no debug probe (SWD/JTAG) for this keyboard.** If the keyboard's own bootloader (the first 32 KB of
  flash, below `0x08008000`) were ever damaged, there is no known way to repair it. The tools in this repository are written so that they cannot write
  there, but "written so that" is not a guarantee, and your keyboard may differ from the author's.
- A firmware image that does not work normally can be replaced again through the keyboard's bootloader (hold ESC and the
  left CTRL key while plugging in USB). That path was tested on the author's keyboard with a deliberately broken image.
  It depends on the bootloader being intact, on the keyboard being the same model and hardware revision, and on you
  following the steps.
- Keyboards of other models, other hardware revisions or other firmware versions may behave differently. Nothing here has
  been tested on any keyboard except the author's own.
- Make and keep a backup (`tools/magger60_backup.py`) **before** the first flash, and keep a copy of it on another
  computer. Keep the stock firmware image for your keyboard, which you must obtain from the manufacturer yourself
  (it is not part of this repository).

## 2. No warranty, no liability

The software is licensed under the Apache License 2.0 (`LICENSE`), which includes a disclaimer of warranty and a
limitation of liability. In plain words: it is provided "as is", without warranty of any kind, and the authors are not
responsible for damaged hardware, lost data, voided warranties, or anything else that results from using it. You use it
at your own risk. Flashing third-party firmware may void the manufacturer's warranty.

## 3. Not affiliated with the manufacturer

This is an independent, unofficial, hobby project. It is not affiliated with, authorized by, sponsored by or endorsed by
Luminkey or any other company that makes or sells the keyboard, its chip or its software. The manufacturer does not
support this project; please do not ask them for help with it.

## 4. Trademarks

"Luminkey", "Magger60" and any other product, company or chip names mentioned in this repository are the property of
their respective owners. They are used only to say which hardware the project is meant for (nominative use). The
project's own name and visuals are original and do not use anyone's logos or artwork.

The USB vendor and product identifiers that the keyboard reports (`19F5:FB49`, and `19F5:AF1F` for its bootloader) are
assigned to or chosen by the manufacturer. The firmware currently reports the same pair because the keyboard's
bootloader and the host tools recognize the keyboard by it. This is for compatibility only and does not imply any
relationship with the manufacturer. A different identifier may be chosen in a later version.

## 5. Origin of the material in this repository

- Everything in this repository was written by the author (with the help of an AI coding assistant), from public
  specifications (USB 2.0, HID 1.11, the HID Usage Tables, the chip vendor's public register description) and from
  measurements the author made on the author's own keyboard.
- **No vendor firmware, no vendor software, no vendor web-driver code and no decompiler output is included in this
  repository, and nothing in this repository needs any of them to build or to pass its tests.** There are no files that
  were extracted from the manufacturer's firmware images, installers or website.
- Hardware facts that the project depends on, such as register addresses, the flash layout, the bootloader's update
  protocol and the pin and sensor arrangement, are facts about the device. Where they were first learned by examining
  the keyboard's own firmware for interoperability, the repository states only the facts, in the author's own words
  and code.
- To be accurate about the limits of that statement: the author did examine the manufacturer's firmware and software while
  working out those facts, so this is **not** a "clean-room" reimplementation in the strict legal sense, where one person
  studies the original and a different person, who has never seen it, writes the new code. The rule for this repository is
  that expression and data are never copied: the code, the tables and the documents are original, and values that look
  like they could be vendor data (for example the key-depth curve and the key-to-sensor map) are produced here from the
  author's own measurements. If you believe something in this repository is nevertheless derived from vendor material,
  please open an issue so it can be removed or replaced.
- One table is known to be identical to the manufacturer's: the map between sensing slots and light numbers in
  `firmware/core/src/km_led.c`. It records which light sits under which key, which is a physical fact of the keyboard's
  wiring, so every correct implementation for this board ends up with the same values. It was derived here from the
  author's own measured slot map (`firmware/core/data/slotmap_measured.json`) and the board's serpentine light order,
  and the identity with the manufacturer's table was only noticed afterwards, when the two were compared. The publication
  check lists it as an explicit, documented exception.
- This is not legal advice. If you plan to redistribute or build on this project commercially, ask a lawyer.

## 6. Security and privacy

The project has had no security review. The firmware is not intended for any use where a malfunctioning keyboard could
cause harm. The configuration tool in `ui/` stores profiles in your browser only and does not upload anything.
