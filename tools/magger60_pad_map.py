#!/usr/bin/env python3
"""Magger60 gamepad: report layout, reference HID report descriptor, SDL game controller mapping string.

The firmware's gamepad core (firmware/core/src/km_pad.c, M6 package P4) produces a 15-byte input report with no report id:

    byte 0..1   buttons, bit n = button n+1                       (16 buttons)
    byte 2      low nibble hat 0 neutral, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW; high nibble 0
    byte 3..4   LX  int16 -32767..32767     usage Generic Desktop X
    byte 5..6   LY  int16                   usage Y            (negative = up)
    byte 7..8   RX  int16                   usage Rx
    byte 9..10  RY  int16                   usage Ry
    byte 11..12 left trigger  uint16 0..32767    usage Z
    byte 13..14 right trigger uint16 0..32767    usage Rz

Windows (DirectInput), Linux (joydev) and SDL number the axes of a joystick by usage (X, Y, Z, Rx, Ry, Rz) and not by their
place in the report, so the stick/trigger split above reads as a0 = LX, a1 = LY, a2 = left trigger, a3 = RX, a4 = RY,
a5 = right trigger. The mapping string below follows from that and from the button order chosen here.

    python tools/magger60_pad_map.py sdl                       # one line for gamecontrollerdb.txt / SDL_GAMECONTROLLERCONFIG
    python tools/magger60_pad_map.py sdl --platform linux      # the Linux flavour (the GUID carries bcdDevice)
    python tools/magger60_pad_map.py sdl --guid <32 hex>       # use the GUID that SDL itself prints (see below)
    python tools/magger60_pad_map.py layout                    # byte-by-byte layout table (--json for tools)
    python tools/magger60_pad_map.py descriptor                # reference HID report descriptor, checked against the layout
    python tools/magger60_pad_map.py selftest

The GUID is built from the USB ids the same way SDL2 does for a USB device (bus 0x03, vendor, product; Windows: version 0).
SDL can print the real one: SDL's `testcontroller` / `controllermap` tool, or Steam > Settings > Controller. If it differs,
pass it with --guid. The descriptor is a reference for the USB package (P5b): if it adopts it, this tool and the firmware
agree by construction; `descriptor` also verifies it (collections close, bits add up to the report length).
Only the Python standard library is needed.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

VID = 0x19F5      # firmware/usb/include/km/km_usb_ids.h (KM_USB_VID / KM_USB_PID); the gamepad is an interface of the same device
PID = 0xFB49
DEFAULT_BCD = 0x0002   # the planned bcdDevice of the "HID gamepad" descriptor variant (M6 plan 3.1.5)
NAME = "Magger60 Gamepad"
REPORT_LEN = 15

# ---- the layout (single source of truth for this tool; selftest compares it with km_pad.h) ----------------------------------

BUTTONS = 16
# report order, with the HID usage and the DirectInput/SDL order of the usage (X, Y, Z, Rx, Ry, Rz)
AXES = [
    # name, offset, usage (Generic Desktop), logical min, logical max
    ("leftx",        3, 0x30, -32767, 32767),
    ("lefty",        5, 0x31, -32767, 32767),
    ("rightx",       7, 0x33, -32767, 32767),
    ("righty",       9, 0x34, -32767, 32767),
    ("lefttrigger", 11, 0x32, 0, 32767),
    ("righttrigger", 13, 0x35, 0, 32767),
]
USAGE_ORDER = [0x30, 0x31, 0x32, 0x33, 0x34, 0x35]   # X Y Z Rx Ry Rz = a0..a5


def axis_index(usage: int) -> int:
    """SDL axis number of an axis with this usage, when only these six exist."""
    return sorted(u for _, _, u, _, _ in AXES).index(usage)


# default button roles (a suggestion for the UI's gamepad page; the firmware does not care what a button "is")
BUTTON_ROLES = {
    "a": 1, "b": 2, "x": 3, "y": 4, "leftshoulder": 5, "rightshoulder": 6, "back": 7, "start": 8,
    "leftstick": 9, "rightstick": 10, "guide": 11, "misc1": 12,
}
# SDL hat masks: up 1, right 2, down 4, left 8
DPAD = {"dpup": 1, "dpright": 2, "dpdown": 4, "dpleft": 8}


def layout_rows():
    rows = [("buttons 1-8", 0, 1, "bit n = button n+1"), ("buttons 9-16", 1, 1, "bit n = button n+9"),
            ("hat", 2, 1, "low nibble: 0 neutral, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW; high nibble 0")]
    names = {"leftx": "LX", "lefty": "LY (negative = up)", "rightx": "RX", "righty": "RY (negative = up)",
             "lefttrigger": "left trigger", "righttrigger": "right trigger"}
    for n, off, usage, lo, hi in AXES:
        rows.append((names[n], off, 2, "int16 LE %d..%d, usage 0x%02X (SDL a%d)" % (lo, hi, usage, axis_index(usage))
                     if lo < 0 else "uint16 LE %d..%d, usage 0x%02X (SDL a%d)" % (lo, hi, usage, axis_index(usage))))
    return rows


# ---- SDL mapping --------------------------------------------------------------------------------------------------------

def sdl_guid(vid: int, pid: int, platform: str, bcd: int) -> str:
    """32 hex digits, SDL2 layout: bus (2, LE) | crc16 of the name (2, 0 when ids are known) | vid (2 LE) | 0 | pid (2 LE) | 0 | version (2 LE) | 0 0."""
    version = bcd if platform == "linux" else 0
    raw = struct.pack("<HHHHHHBB", 0x0003, 0, vid, 0, pid, 0, 0, 0)[:12] + struct.pack("<H", version) + b"\x00\x00"
    return raw.hex()


def sdl_mapping(guid: str, name: str, platform: str) -> str:
    plat = {"windows": "Windows", "linux": "Linux", "mac": "Mac OS X"}[platform]
    parts = [guid, name]
    for role, n in BUTTON_ROLES.items():
        parts.append("%s:b%d" % (role, n - 1))
    sdl_axis = {"leftx": "leftx", "lefty": "lefty", "rightx": "rightx", "righty": "righty",
                "lefttrigger": "lefttrigger", "righttrigger": "righttrigger"}
    for n, _, usage, _, _ in AXES:
        parts.append("%s:a%d" % (sdl_axis[n], axis_index(usage)))
    for k, mask in DPAD.items():
        parts.append("%s:h0.%d" % (k, mask))
    parts.append("platform:" + plat)
    return ",".join(parts) + ","


def parse_mapping(line: str) -> dict:
    """The inverse, for the selftest: {'guid':..., 'name':..., key: value}."""
    items = [p for p in line.split(",") if p != ""]
    d = {"guid": items[0], "name": items[1]}
    for it in items[2:]:
        k, v = it.split(":", 1)
        d[k] = v
    return d


# ---- reference report descriptor ------------------------------------------------------------------------------------------

def item(prefix: int, data: bytes = b"") -> bytes:
    size = {0: 0, 1: 1, 2: 2, 4: 3}[len(data)]
    return bytes([prefix | size]) + data


def descriptor() -> bytes:
    d = bytearray()
    d += item(0x04, b"\x01")                         # Usage Page (Generic Desktop)
    d += item(0x08, b"\x05")                         # Usage (Game Pad)
    d += item(0xA0, b"\x01")                         # Collection (Application)
    d += item(0x04, b"\x09")                         #   Usage Page (Button)
    d += item(0x18, b"\x01")                         #   Usage Minimum (1)
    d += item(0x28, bytes([BUTTONS]))                #   Usage Maximum (16)
    d += item(0x14, b"\x00")                         #   Logical Minimum (0)
    d += item(0x24, b"\x01")                         #   Logical Maximum (1)
    d += item(0x74, b"\x01")                         #   Report Size (1)
    d += item(0x94, bytes([BUTTONS]))                #   Report Count (16)
    d += item(0x80, b"\x02")                         #   Input (Data, Variable, Absolute)
    d += item(0x04, b"\x01")                         #   Usage Page (Generic Desktop)
    d += item(0x08, b"\x39")                         #   Usage (Hat switch)
    d += item(0x14, b"\x01")                         #   Logical Minimum (1)
    d += item(0x24, b"\x08")                         #   Logical Maximum (8)
    d += item(0x34, b"\x00")                         #   Physical Minimum (0)
    d += item(0x44, struct.pack("<H", 315))          #   Physical Maximum (315)
    d += item(0x64, b"\x14")                         #   Unit (degrees)
    d += item(0x74, b"\x04")                         #   Report Size (4)
    d += item(0x94, b"\x01")                         #   Report Count (1)
    d += item(0x80, b"\x42")                         #   Input (Data, Variable, Absolute, Null State): 0 = neutral
    d += item(0x64, b"\x00")                         #   Unit (none)
    d += item(0x74, b"\x04")                         #   Report Size (4)
    d += item(0x94, b"\x01")                         #   Report Count (1)
    d += item(0x80, b"\x03")                         #   Input (Constant, Variable, Absolute): the unused nibble
    d += item(0x04, b"\x01")                         #   Usage Page (Generic Desktop)
    for _, _, usage, _, _ in AXES[:4]:
        d += item(0x08, bytes([usage]))              #   Usage X, Y, Rx, Ry
    d += item(0x14, struct.pack("<h", -32767))       #   Logical Minimum (-32767)
    d += item(0x24, struct.pack("<h", 32767))        #   Logical Maximum (32767)
    d += item(0x74, b"\x10")                         #   Report Size (16)
    d += item(0x94, b"\x04")                         #   Report Count (4)
    d += item(0x80, b"\x02")                         #   Input (Data, Variable, Absolute)
    for _, _, usage, _, _ in AXES[4:]:
        d += item(0x08, bytes([usage]))              #   Usage Z, Rz (the triggers)
    d += item(0x14, b"\x00")                         #   Logical Minimum (0)
    d += item(0x24, struct.pack("<h", 32767))        #   Logical Maximum (32767)
    d += item(0x74, b"\x10")                         #   Report Size (16)
    d += item(0x94, b"\x02")                         #   Report Count (2)
    d += item(0x80, b"\x02")                         #   Input (Data, Variable, Absolute)
    d += item(0xC0)                                  # End Collection
    return bytes(d)


def check_descriptor(desc: bytes) -> dict:
    """A small independent HID item walker: collections close, every variable input has as many usages as its count, the bits add up."""
    depth, i = 0, 0
    g = {"size": 0, "count": 0, "lmin": 0, "lmax": 0, "page": 0}
    usages, umin, umax = [], None, None
    bits, fields = 0, []
    while i < len(desc):
        pre = desc[i]
        i += 1
        n = {0: 0, 1: 1, 2: 2, 3: 4}[pre & 3]
        if pre == 0xFE:
            raise ValueError("long items are not used")
        data = desc[i:i + n]
        i += n
        typ, tag = (pre >> 2) & 3, pre >> 4
        sval = int.from_bytes(data, "little", signed=True) if n else 0
        uval = int.from_bytes(data, "little") if n else 0
        if typ == 1:     # global
            if tag == 0:
                g["page"] = uval
            elif tag == 1:
                g["lmin"] = sval
            elif tag == 2:
                g["lmax"] = uval if g["lmin"] >= 0 else sval
            elif tag == 7:
                g["size"] = uval
            elif tag == 9:
                g["count"] = uval
        elif typ == 2:   # local
            if tag == 0:
                usages.append(uval)
            elif tag == 1:
                umin = uval
            elif tag == 2:
                umax = uval
        elif typ == 0:   # main
            if tag == 0xA:
                depth += 1
            elif tag == 0xC:
                depth -= 1
                if depth < 0:
                    raise ValueError("End Collection without Collection")
            elif tag == 0x8:
                flags = uval
                if flags & 1:
                    bits += g["size"] * g["count"]
                else:
                    have = len(usages) if usages else ((umax - umin + 1) if umin is not None and umax is not None else 0)
                    if not (flags & 2) or have != g["count"]:
                        raise ValueError("input with %d usages for report count %d" % (have, g["count"]))
                    if g["lmax"] < g["lmin"]:
                        raise ValueError("logical max below min")
                    if g["size"] < 32 and g["lmax"] >= (1 << (g["size"] - (1 if g["lmin"] < 0 else 0))):
                        raise ValueError("logical max does not fit the report size")
                    bits += g["size"] * g["count"]
                    fields.append((g["page"], list(usages), umin, umax, g["size"], g["count"], g["lmin"], g["lmax"]))
            if tag in (0x8, 0x9, 0xB, 0xA, 0xC):
                usages, umin, umax = [], None, None
    if depth != 0:
        raise ValueError("unclosed collection")
    return {"bits": bits, "bytes": bits // 8, "fields": fields}


# ---- layout cross-check with the C header ------------------------------------------------------------------------------

def header_report_len(repo: Path):
    h = repo / "firmware/core/include/km/km_pad.h"
    if not h.exists():
        return None
    m = re.search(r"#define\s+KM_PAD_REPORT_LEN\s+(\d+)u?", h.read_text())
    return int(m.group(1)) if m else None


# ---- commands ------------------------------------------------------------------------------------------------------------

def cmd_sdl(a) -> int:
    guid = a.guid.lower() if a.guid else sdl_guid(a.vid, a.pid, a.platform, a.bcd)
    if not re.fullmatch(r"[0-9a-f]{32}", guid):
        print("the GUID must be 32 hex digits", file=sys.stderr)
        return 2
    print(sdl_mapping(guid, a.name, a.platform))
    return 0


def cmd_layout(a) -> int:
    rows = layout_rows()
    if a.json:
        print(json.dumps({"report_len": REPORT_LEN, "fields": [{"name": n, "offset": o, "size": s, "note": t} for n, o, s, t in rows],
                          "button_roles": BUTTON_ROLES}, indent=2))
        return 0
    print("report length %d bytes, no report id" % REPORT_LEN)
    print("%-6s %-5s %s" % ("byte", "size", "field"))
    for n, o, s, t in rows:
        print("%-6s %-5d %s: %s" % (("%d" % o) if s == 1 else "%d..%d" % (o, o + s - 1), s, n, t))
    return 0


def cmd_descriptor(a) -> int:
    d = descriptor()
    info = check_descriptor(d)
    if info["bytes"] != REPORT_LEN:
        print("descriptor is %d bytes of report, expected %d" % (info["bytes"], REPORT_LEN), file=sys.stderr)
        return 1
    print("# %d bytes, %d-byte input report, collections closed, field sizes checked" % (len(d), info["bytes"]))
    for k in range(0, len(d), 16):
        print(" ".join("%02X" % b for b in d[k:k + 16]))
    return 0


def selftest() -> int:
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    # layout: offsets are contiguous and end at the report length
    end = 0
    for _, o, s, _ in layout_rows():
        check(o == end, "layout gap at byte %d" % o)
        end = o + s
    check(end == REPORT_LEN + 0 and end == 15, "layout length %d" % end)
    h = header_report_len(Path(__file__).resolve().parents[1])
    check(h is None or h == REPORT_LEN, "km_pad.h says KM_PAD_REPORT_LEN %s" % h)

    # axis numbering by usage
    idx = {n: axis_index(u) for n, _, u, _, _ in AXES}
    check(idx == {"leftx": 0, "lefty": 1, "righttrigger": 5, "lefttrigger": 2, "rightx": 3, "righty": 4}, "axis numbers %r" % idx)

    # descriptor
    desc = descriptor()
    info = check_descriptor(desc)
    check(info["bytes"] == REPORT_LEN, "descriptor bytes %d" % info["bytes"])
    check([f[4] * f[5] for f in info["fields"]] == [16, 4, 64, 32], "descriptor fields %r" % [(f[4], f[5]) for f in info["fields"]])
    check(desc[:4] == bytes([0x05, 0x01, 0x09, 0x05]), "descriptor starts with Generic Desktop / Game Pad")
    check(desc[-1] == 0xC0, "descriptor ends with End Collection")
    for broken, what in ((desc[:-1], "unclosed collection"), (desc + b"\xC0", "extra End Collection")):
        try:
            check_descriptor(broken)
            check(False, "the checker missed: " + what)
        except ValueError:
            pass
    bad = bytearray(desc)
    pos = bytes(bad).index(bytes([0x95, 0x04]))      # report count of the sticks: 4 -> 3, but 4 usages are listed
    bad[pos + 1] = 3
    try:
        check_descriptor(bytes(bad))
        check(False, "the checker missed a usage/count mismatch")
    except ValueError:
        pass

    # SDL string
    for platform, bcd in (("windows", DEFAULT_BCD), ("linux", 0x0111)):
        g = sdl_guid(VID, PID, platform, bcd)
        check(len(g) == 32 and re.fullmatch(r"[0-9a-f]{32}", g) is not None, "guid format " + g)
        check(g.startswith("03000000f5190000" + "49fb0000"), "guid ids " + g)
        check(g.endswith("0000" if platform == "windows" else "11010000"), "guid version " + g)
        line = sdl_mapping(g, NAME, platform)
        m = parse_mapping(line)
        check(line.endswith(","), "line ends with a comma")
        check(m["a"] == "b0" and m["start"] == "b7" and m["guide"] == "b10", "buttons " + line)
        check(m["leftx"] == "a0" and m["lefty"] == "a1" and m["lefttrigger"] == "a2" and m["rightx"] == "a3" and
              m["righty"] == "a4" and m["righttrigger"] == "a5", "axes " + line)
        check(m["dpup"] == "h0.1" and m["dpright"] == "h0.2" and m["dpdown"] == "h0.4" and m["dpleft"] == "h0.8", "dpad " + line)
        check(m["platform"] == ("Windows" if platform == "windows" else "Linux"), "platform " + line)
        check(len(set(v for k, v in m.items() if k not in ("guid", "name", "platform"))) == len(m) - 3, "an input used twice")
    for n, role in BUTTON_ROLES.items():
        check(1 <= role <= BUTTONS, "role %s outside the 16 buttons" % n)
    check(len(set(BUTTON_ROLES.values())) == len(BUTTON_ROLES), "two roles on one button")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("selftest ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sdl", help="print the SDL game controller mapping line")
    s.add_argument("--vid", type=lambda x: int(x, 0), default=VID)
    s.add_argument("--pid", type=lambda x: int(x, 0), default=PID)
    s.add_argument("--bcd", type=lambda x: int(x, 0), default=DEFAULT_BCD, help="bcdDevice of the gamepad variant (Linux GUID only)")
    s.add_argument("--name", default=NAME)
    s.add_argument("--platform", choices=("windows", "linux", "mac"), default="windows")
    s.add_argument("--guid", help="use this GUID (32 hex digits) as printed by SDL")
    s.set_defaults(fn=cmd_sdl)
    s = sub.add_parser("layout", help="the report layout, byte by byte")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_layout)
    s = sub.add_parser("descriptor", help="reference HID report descriptor (hex), verified against the layout")
    s.set_defaults(fn=cmd_descriptor)
    s = sub.add_parser("selftest")
    s.set_defaults(fn=lambda a: selftest())
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
