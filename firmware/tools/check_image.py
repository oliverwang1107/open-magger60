#!/usr/bin/env python3
"""Sanity-check a built application image against the Magger60 memory map.

    python firmware/tools/check_image.py km_smoke.bin [km_smoke.elf]

Checks (FIRMWARE_Magger60.md section 2, CUSTOM_FIRMWARE_PLAN.md 7):
  * the vector table sits at file offset 0 (= flash 0x08008000) with 134 entries;
  * the initial SP is inside the 160 KB SRAM window and 8-byte aligned;
  * every non-zero vector is a Thumb address inside the image;
  * the image fits the 128 KB app window (it must stay below the settings sectors at 0x08028000);
  * with an ELF: no allocated section lies outside the flash / RAM windows.
  * with --usb-8khz / --usb-1khz: the USB descriptors in the image carry bInterval 1 / 4 on the keyboard endpoints and the matching bcdDevice.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

APP_BASE, APP_LIMIT = 0x08008000, 0x08028000
RAM_BASE, RAM_END = 0x20000000, 0x20028000
VECTORS = 134


def fail(msg: str) -> None:
    print("FAIL:", msg)
    sys.exit(1)


def tool(name: str) -> str:
    env_path = os.environ.get("PATH", "")
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        env_path = str(ucrt) + os.pathsep + env_path
    found = shutil.which(name, path=env_path)
    if not found:
        fail("tool not found: " + name)
    return found


def check_bin(path: Path) -> int:
    data = path.read_bytes()
    size = len(data)
    if size < VECTORS * 4:
        fail("image smaller than the vector table (%d bytes)" % size)
    if APP_BASE + size > APP_LIMIT:
        fail("image is %d bytes: it runs into the settings sectors at 0x%08X" % (size, APP_LIMIT))
    vec = struct.unpack_from("<%dI" % VECTORS, data, 0)
    sp = vec[0]
    if not (RAM_BASE < sp <= RAM_END) or sp % 8:
        fail("initial SP 0x%08X is not an 8-byte aligned address inside SRAM" % sp)
    reset = vec[1]
    if not (reset & 1) or not (APP_BASE + VECTORS * 4 <= reset - 1 < APP_BASE + size):
        fail("reset vector 0x%08X is not a Thumb address after the vector table" % reset)
    for i, v in enumerate(vec[1:], start=1):
        if v == 0:
            continue
        if not (v & 1) or not (APP_BASE <= v - 1 < APP_BASE + size):
            fail("vector %d = 0x%08X is not a Thumb address inside the image" % (i, v))
    print("bin ok : %d bytes (%.1f%% of the 128 KB window), SP 0x%08X, Reset 0x%08X"
          % (size, 100.0 * size / (APP_LIMIT - APP_BASE), sp, reset))
    return size


def check_elf(path: Path) -> None:
    out = subprocess.run([tool("arm-none-eabi-objdump"), "-h", str(path)], capture_output=True, text=True, check=True).stdout
    bad = []
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+\S+\s*\n\s*(.*)$", out, re.M):
        name, size, vma, lma, flags = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), m.group(5)
        if "ALLOC" not in flags or size == 0:
            continue
        in_flash = APP_BASE <= vma and vma + size <= APP_LIMIT
        in_ram = RAM_BASE <= vma and vma + size <= RAM_END
        if not (in_flash or in_ram):
            bad.append("%s vma 0x%08X size 0x%X" % (name, vma, size))
        if "LOAD" in flags and not (APP_BASE <= lma and lma + size <= APP_LIMIT):
            bad.append("%s load address 0x%08X size 0x%X outside flash window" % (name, lma, size))
    if bad:
        fail("sections outside the memory map: " + "; ".join(bad))
    print("elf ok : all allocated sections inside the flash/RAM windows")


USB_VID, USB_PID = 0x19F5, 0xFB49
EP_KBD_IN, EP_NKRO_IN, EP_VND_IN = 0x81, 0x83, 0x82


def check_usb(path: Path, want_8khz: bool) -> None:
    """Read the descriptors back out of the finished image and compare with the expectation (the build once ignored KM_USB_HS_8KHZ)."""
    data = path.read_bytes()
    dev = [m.start() for m in re.finditer(re.escape(bytes([0x12, 0x01, 0x00, 0x02])) + b".{4}" + struct.pack("<HH", USB_VID, USB_PID), data, re.S)]
    if not dev:
        fail("no USB device descriptor (VID 0x%04X PID 0x%04X) found in the image" % (USB_VID, USB_PID))
    bcds = sorted({struct.unpack_from("<H", data, d + 12)[0] for d in dev})
    want_bcd = {0x0003, 0x0004, 0x0005} if want_8khz else {0x0001, 0x0002, 0x0005}   # 0x0005 = the XInput descriptor set (M6 P11), present in every image
    if not set(bcds) <= want_bcd:
        fail("bcdDevice values in the image %s, expected %s" % (["0x%04X" % b for b in bcds], ["0x%04X" % b for b in sorted(want_bcd)]))
    found = {}
    for ep in (EP_KBD_IN, EP_NKRO_IN, EP_VND_IN):
        vals = {data[m.start() + 6] for m in re.finditer(re.escape(bytes([0x07, 0x05, ep, 0x03])) + b".{2}.", data, re.S)}
        found[ep] = vals
    want = {EP_KBD_IN: {1 if want_8khz else 4}, EP_NKRO_IN: {1 if want_8khz else 4}, EP_VND_IN: {4}}
    if found != want:
        fail("endpoint bInterval in the image %s, expected %s" % ({hex(k): sorted(v) for k, v in found.items()}, {hex(k): sorted(v) for k, v in want.items()}))
    print("usb ok : bcdDevice %s, bInterval kbd %d nkro %d vendor 4 (%s)" % (["0x%04X" % b for b in bcds], want[EP_KBD_IN].copy().pop(),
          want[EP_NKRO_IN].copy().pop(), "8 kHz set" if want_8khz else "1 kHz set"))


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--usb-")]
    usb = [a for a in sys.argv[1:] if a.startswith("--usb-")]
    if len(args) < 1:
        print(__doc__)
        return 2
    check_bin(Path(args[0]))
    if len(args) > 1:
        check_elf(Path(args[1]))
    if usb:
        check_usb(Path(args[0]), usb[0] == "--usb-8khz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
