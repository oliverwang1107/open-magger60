#!/usr/bin/env python3
"""magger60_image.py - assemble and validate a flashable Magger60 image (CUSTOM_FIRMWARE_PLAN.md / M2_PLAN.md M2-A).

An image is a raw file that starts at flash address 0x08008000 (the same layout as the manufacturer's update files).
The official update protocol writes it sequentially from there, so it also overwrites the
settings sectors; this tool therefore puts the settings BACKUP (from magger60_backup.py) into
the image instead of defaults, and refuses anything that would leave the keyboard without a
valid app-ID block.

    python tools/magger60_image.py --self-test
    python tools/magger60_image.py build --vendor /path/to/your-stock-update.bin \
        --settings backup1/settings_0x08028000.bin --out images/stock.bin
    python tools/magger60_image.py build --app firmware/build/app.bin \
        --settings backup1/settings_0x08028000.bin --out images/custom.bin
    python tools/magger60_image.py check images/stock.bin

--vendor is for restoring the original firmware: you must obtain the manufacturer's update file yourself and pass
its path (this repository contains none and no default path is assumed). Building and checking custom firmware
(--app) needs no such file.

Nothing here talks to a keyboard.
"""
import argparse
import hashlib
import struct
import sys
import tempfile
from pathlib import Path

APP_BASE = 0x08008000
APP_LIMIT = 0x08028000            # app code must end below the settings sectors
SETTINGS_BASE = 0x08028000
SETTINGS_SIZE = 0x18000           # 0x08028000-0x0803FFFF
SETTINGS_END = SETTINGS_BASE + SETTINGS_SIZE
FLAG_ADDR = 0x0802C000            # bootloader flag, bit0 = "stay in update mode"
APP_ID_MAGIC = 0xFB4919F5         # PID FB49 << 16 | VID 19F5, little-endian at 0x08028000
APP_ID_LEN = 14                   # bytes the plan says must equal the stock ones
SRAM_LO, SRAM_HI = 0x20000000, 0x20028000
SECTOR = 0x2000
MAX_IMAGE = SETTINGS_END - APP_BASE + 0x200   # settings end + room for the Keil RW tail (0x128 bytes)
VENDOR_TAIL_OFF = 0x38000         # manufacturer update file: compressed RW data after the settings

REPO = Path(__file__).resolve().parents[1]


class ImageError(Exception):
    pass


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def check_image(image: bytes, reference_id: bytes = None) -> list:
    """Return a list of problems; an empty list means the image passes every check."""
    bad = []
    if len(image) < SETTINGS_END - APP_BASE:
        bad.append("image ends at 0x%08X, before the end of the settings sectors 0x%08X"
                   % (APP_BASE + len(image), SETTINGS_END))
        return bad
    if len(image) > MAX_IMAGE:
        bad.append("image is %d bytes, more than the %d allowed" % (len(image), MAX_IMAGE))
    sp, reset = u32(image, 0), u32(image, 4)
    if not SRAM_LO <= sp <= SRAM_HI:
        bad.append("initial SP 0x%08X is outside SRAM 0x%08X-0x%08X" % (sp, SRAM_LO, SRAM_HI))
    if not (reset & 1):
        bad.append("reset vector 0x%08X is not a Thumb address (bit0 clear)" % reset)
    if not APP_BASE <= (reset & ~1) < APP_LIMIT:
        bad.append("reset vector 0x%08X is outside the app window 0x%08X-0x%08X" % (reset, APP_BASE, APP_LIMIT - 1))
    off = SETTINGS_BASE - APP_BASE
    magic = u32(image, off)
    if magic != APP_ID_MAGIC:
        bad.append("app-ID at 0x08028000 is 0x%08X, expected 0x%08X (the bootloader would never start the app)"
                   % (magic, APP_ID_MAGIC))
    if reference_id is not None and image[off:off + APP_ID_LEN] != reference_id[:APP_ID_LEN]:
        bad.append("the 14-byte ID block differs from the reference one")
    flag = image[FLAG_ADDR - APP_BASE]
    if flag & 1:
        bad.append("boot flag at 0x0802C000 has bit0 set (0x%02X): the bootloader would stay in update mode" % flag)
    return bad


def build_image(app: bytes, settings: bytes, tail: bytes = b"") -> bytes:
    if len(settings) != SETTINGS_SIZE:
        raise ImageError("settings backup must be exactly 0x%X bytes, got 0x%X" % (SETTINGS_SIZE, len(settings)))
    if len(app) > APP_LIMIT - APP_BASE:
        raise ImageError("app is 0x%X bytes; the window is 0x%X (0x08008000-0x08027FFF)"
                         % (len(app), APP_LIMIT - APP_BASE))
    if len(app) < 8:
        raise ImageError("app is too short to contain a vector table")
    image = bytearray(b"\xFF" * (SETTINGS_BASE - APP_BASE))
    image[:len(app)] = app
    image += settings
    image += tail
    return bytes(image)


def split_vendor(vendor: bytes):
    """The manufacturer's update file = app (0x20000 bytes), settings defaults (0x18000), RW-data tail."""
    if len(vendor) < VENDOR_TAIL_OFF:
        raise ImageError("the --vendor file is too short (%d bytes); is it a manufacturer update file?" % len(vendor))
    return vendor[:SETTINGS_BASE - APP_BASE], vendor[VENDOR_TAIL_OFF:]


def describe(image: bytes) -> str:
    return ("%d bytes (0x08008000-0x%08X), sha256 %s, SP 0x%08X, Reset 0x%08X"
            % (len(image), APP_BASE + len(image) - 1, hashlib.sha256(image).hexdigest()[:16],
               u32(image, 0), u32(image, 4)))


# --------------------------------------------------------------------------- CLI

def cmd_build(args) -> int:
    settings = Path(args.settings).read_bytes()
    tail = b""
    if args.vendor:
        app, tail = split_vendor(Path(args.vendor).read_bytes())
    else:
        app = Path(args.app).read_bytes()
    ref = settings[:APP_ID_LEN]
    image = build_image(app, settings, tail)
    problems = check_image(image, ref)
    if problems:
        print("REFUSED, not writing %s:" % args.out)
        for p in problems:
            print("  -", p)
        return 1
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print("wrote %s: %s" % (out, describe(image)))
    return 0


def cmd_check(args) -> int:
    image = Path(args.image).read_bytes()
    ref = Path(args.settings).read_bytes()[:APP_ID_LEN] if args.settings else None
    problems = check_image(image, ref)
    print(describe(image))
    if problems:
        print("FAILED:")
        for p in problems:
            print("  -", p)
        return 1
    print("OK: all image checks pass")
    return 0


# --------------------------------------------------------------------------- self-test

def _fake_settings(flag=0x50, magic=APP_ID_MAGIC):
    s = bytearray(b"\xFF" * SETTINGS_SIZE)
    s[0:4] = struct.pack("<I", magic)
    s[4:14] = bytes.fromhex("04011122334455667788")
    s[FLAG_ADDR - SETTINGS_BASE] = flag
    return bytes(s)


def _fake_app(sp=0x20009F50, reset=0x0800822D, size=0x4000):
    a = bytearray(size)
    a[0:8] = struct.pack("<II", sp, reset)
    for i in range(8, size):
        a[i] = (i * 7 + 3) & 0xFF
    return bytes(a)


def self_test() -> int:
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)
            print("FAIL:", msg)

    settings = _fake_settings()
    app = _fake_app()
    good = build_image(app, settings)
    check(check_image(good, settings[:APP_ID_LEN]) == [], "a good image is rejected: %s" % check_image(good))
    check(len(good) == SETTINGS_END - APP_BASE, "unexpected image size")
    check(good[0x20000:0x20000 + SETTINGS_SIZE] == settings, "settings are not placed at 0x08028000")
    check(good[len(app):0x20000] == b"\xFF" * (0x20000 - len(app)), "gap between app and settings is not 0xFF")

    def rejects(label, image, ref=None, expect=None):
        probs = check_image(image, ref)
        check(bool(probs), "%s was accepted" % label)
        if expect:
            check(any(expect in p for p in probs), "%s: no problem mentioning %r in %s" % (label, expect, probs))

    rejects("bad SP", build_image(_fake_app(sp=0x30000000), settings), expect="SP")
    rejects("SP at 0", build_image(_fake_app(sp=0), settings), expect="SP")
    rejects("even reset vector", build_image(_fake_app(reset=0x0800822C), settings), expect="Thumb")
    rejects("reset in bootloader", build_image(_fake_app(reset=0x08000221), settings), expect="app window")
    rejects("reset in settings", build_image(_fake_app(reset=0x08028001), settings), expect="app window")
    rejects("wrong app-ID", build_image(app, _fake_settings(magic=0x12345678)), expect="app-ID")
    rejects("erased app-ID", build_image(app, b"\xFF" * SETTINGS_SIZE), expect="app-ID")
    rejects("boot flag bit0", build_image(app, _fake_settings(flag=0x51)), expect="bit0")
    rejects("ID differs from reference", good, ref=b"\x00" * 14, expect="ID block differs")
    rejects("truncated", good[:0x20000], expect="before the end")
    rejects("oversized", good + b"\x00" * 0x1000, expect="more than")

    for label, fn in (
        ("app too big", lambda: build_image(b"\x00" * (0x20000 + 1), settings)),
        ("settings too short", lambda: build_image(app, settings[:-1])),
        ("settings too long", lambda: build_image(app, settings + b"\x00")),
        ("empty app", lambda: build_image(b"", settings)),
    ):
        try:
            fn()
            check(False, "%s was accepted" % label)
        except ImageError:
            pass

    # update file + backup settings -> stock-equivalent image (a synthetic stand-in; no real file is needed)
    vendor = bytearray(0x38128)
    vendor[0:8] = struct.pack("<II", 0x20009F50, 0x0800822D)
    vendor[0x20000:0x20000 + SETTINGS_SIZE] = _fake_settings()      # "factory defaults"
    vendor[0x38000:] = bytes(0x128)
    v_app, v_tail = split_vendor(bytes(vendor))
    check(len(v_app) == 0x20000 and len(v_tail) == 0x128, "vendor split sizes are wrong")
    custom_settings = bytearray(settings)
    custom_settings[0x6000:0x6004] = b"\xDE\xAD\xBE\xEF"            # "your calibration"
    img = build_image(v_app, bytes(custom_settings), v_tail)
    check(check_image(img, settings[:APP_ID_LEN]) == [], "vendor-based image is rejected")
    check(img[0x20000 + 0x6000:0x20000 + 0x6004] == b"\xDE\xAD\xBE\xEF", "backup settings were not used")
    check(len(img) == 0x38128, "vendor-based image should keep the Keil RW tail (0x38128 bytes)")

    # CLI round trip, including a refusal that must not write a file
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "s.bin").write_bytes(settings)
        (td / "a.bin").write_bytes(app)
        ns = argparse.Namespace(settings=str(td / "s.bin"), vendor=None, app=str(td / "a.bin"), out=str(td / "o/i.bin"))
        check(cmd_build(ns) == 0 and (td / "o/i.bin").read_bytes() == good, "build did not produce the expected image")
        (td / "s2.bin").write_bytes(_fake_settings(flag=0x01))
        ns2 = argparse.Namespace(settings=str(td / "s2.bin"), vendor=None, app=str(td / "a.bin"), out=str(td / "bad.bin"))
        check(cmd_build(ns2) == 1 and not (td / "bad.bin").exists(), "a refused build wrote a file")

    print("SELF-TEST", "FAILED (%d)" % len(failures) if failures else "OK")
    return 1 if failures else 0


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build", help="assemble an image from an app (or the vendor .bin) and the settings backup")
    g = b.add_mutually_exclusive_group(required=True)
    g.add_argument("--app", help="app binary linked at 0x08008000 (at most 128 KB)")
    g.add_argument("--vendor", help="the manufacturer's update file for restoring stock firmware (obtain it yourself): "
                                    "its app and RW tail are used, its settings defaults are not")
    b.add_argument("--settings", required=True, help="settings_0x08028000.bin from magger60_backup.py (96 KB)")
    b.add_argument("--out", required=True)
    c = sub.add_parser("check", help="run every image check on an existing file")
    c.add_argument("image")
    c.add_argument("--settings", help="settings backup whose ID block the image must match")
    args = ap.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.cmd == "build":
            return cmd_build(args)
        if args.cmd == "check":
            return cmd_check(args)
    except (ImageError, OSError) as exc:
        print("error:", exc)
        return 2
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
