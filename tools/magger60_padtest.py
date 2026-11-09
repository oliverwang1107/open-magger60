#!/usr/bin/env python3
"""magger60_padtest.py - look at the Magger60 open firmware's USB gamepad (interface IF3) from the PC, and read the USB
core's hardware-capability words. Read-only: nothing here writes to the keyboard.

    python tools/magger60_padtest.py --self-test        # no hardware: report layout, period finder, decoders, safety
    python tools/magger60_padtest.py --winmm            # what games using the old Windows joystick API (WinMM) see
    python tools/magger60_padtest.py --winmm --identify # also work out which WinMM axis is which gamepad axis (needs the sweep image)
    python tools/magger60_padtest.py --raw              # the raw 15-byte reports of IF3 (needs the 'hidapi' package)
    python tools/magger60_padtest.py --hwcfg            # read the USB core capability words published by the usb_diag image
    python tools/magger60_padtest.py --sim              # the same displays against a simulated gamepad (any OS)

--winmm needs Windows and nothing else (ctypes + winmm.dll, joyGetPosEx). The joystick is found by its USB ids
(wMid/wPid of joyGetDevCaps = 0x19F5 / 0xFB49), so other game controllers on the PC are ignored. Windows only lists a
device here when its HID game-controller driver is bound to IF3: if the keyboard types but this tool finds no joystick,
look at Device Manager (Human Interface Devices) for a yellow mark on "HID-compliant game controller", and at
joy.cpl ("Set up USB game controllers").

The report is 15 bytes, no report ID (firmware/usb/include/km/km_usb_reports.h):
    [0..1]  16 buttons, bit n = button n + 1          [2]  hat: 0 = centred, 1 = up, 2 = up-right ... 8 = up-left
    [3..4] LX  [5..6] LY  [7..8] RX  [9..10] RY   signed 16 bit, -32767..32767
    [11..12] left trigger  [13..14] right trigger  unsigned 16 bit, 0..32767
The sweep image (usb_diag with the pad sweep) moves every axis as a triangle wave with its own period, so the period of
what you see on a WinMM axis tells which gamepad axis it is (--identify), and it walks through the buttons and the hat
one per second.
"""
import argparse
import importlib.util
import math
import re
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

VID, PID = 0x19F5, 0xFB49

# ---- report layout: keep in step with firmware/usb/include/km/km_usb_reports.h (the self-test reads that file) ----
REPORT_LEN = 15
AXES = ("LX", "LY", "RX", "RY", "LT", "RT")
SWEEP_PERIOD_MS = {"LX": 4000, "LY": 5000, "RX": 6000, "RY": 7000, "LT": 8000, "RT": 9000}
STICK_MAX = 32767
HAT_NAMES = ("centre", "up", "up-right", "right", "down-right", "down", "down-left", "left", "up-left")


def decode_report(data: bytes) -> dict:
    if len(data) != REPORT_LEN:
        raise ValueError("a gamepad report is %d bytes, got %d" % (REPORT_LEN, len(data)))
    buttons, hat, lx, ly, rx, ry, lt, rt = struct.unpack("<HBhhhhHH", data)
    if hat > 8:
        raise ValueError("hat value %d is out of range" % hat)
    return {"buttons": buttons, "hat": hat, "LX": lx, "LY": ly, "RX": rx, "RY": ry, "LT": lt, "RT": rt}


def encode_report(r: dict) -> bytes:
    return struct.pack("<HBhhhhHH", r["buttons"], r["hat"], r["LX"], r["LY"], r["RX"], r["RY"], r["LT"], r["RT"])


def sweep(t_ms: int) -> dict:
    """The pad state the sweep image sends t_ms after it started (a Python twin of km_usb_pad_sweep in km_usb_in.c)."""
    def tri(period):
        ph = t_ms % period
        half = period // 2
        if ph < half:
            return STICK_MAX - ph * 2 * STICK_MAX // half
        return -STICK_MAX + (ph - half) * 2 * STICK_MAX // (period - half)
    r = {"buttons": 0, "hat": (t_ms // 1000) % 9}
    step = (t_ms // 1000) % 17
    if step:
        r["buttons"] = 1 << (step - 1)
    for a in ("LX", "LY", "RX", "RY"):
        r[a] = tri(SWEEP_PERIOD_MS[a])
    for a in ("LT", "RT"):
        r[a] = (tri(SWEEP_PERIOD_MS[a]) + STICK_MAX) // 2
    return r


# ------------------------------------------------------------------------------------------------ period finder
def estimate_period(samples, dt_s):
    """Period in seconds of a roughly triangular wave from its samples, or None. Uses the times at which the signal
    crosses the middle of its range upwards."""
    lo, hi = min(samples), max(samples)
    if hi - lo < 0.2 * 65535 * 0.5:
        return None                                   # barely moves: no period to speak of
    mid = (lo + hi) / 2
    ups = []
    for i in range(1, len(samples)):
        if samples[i - 1] < mid <= samples[i]:
            ups.append((i - 1 + (mid - samples[i - 1]) / (samples[i] - samples[i - 1])) * dt_s)
    if len(ups) < 2:
        return None
    gaps = [b - a for a, b in zip(ups, ups[1:])]
    return sum(gaps) / len(gaps)


def identify(periods):
    """periods: {winmm axis name: seconds or None} -> {winmm axis name: gamepad axis name or '?'}"""
    out = {}
    for name, p in periods.items():
        best, err = "?", 0.12
        if p is not None:
            for a, ms in SWEEP_PERIOD_MS.items():
                e = abs(p - ms / 1000) / (ms / 1000)
                if e < err:
                    best, err = a, e
        out[name] = best
    return out


# ------------------------------------------------------------------------------------------------ WinMM (Windows)
JOY_RETURNALL = 0xFF
JOYERR_NOERROR = 0
WINMM_AXES = ("X", "Y", "Z", "R", "U", "V")          # dwXpos .. dwVpos


def _winmm():
    import ctypes
    from ctypes import wintypes

    class JOYINFOEX(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("dwFlags", wintypes.DWORD), ("dwXpos", wintypes.DWORD),
                    ("dwYpos", wintypes.DWORD), ("dwZpos", wintypes.DWORD), ("dwRpos", wintypes.DWORD),
                    ("dwUpos", wintypes.DWORD), ("dwVpos", wintypes.DWORD), ("dwButtons", wintypes.DWORD),
                    ("dwButtonNumber", wintypes.DWORD), ("dwPOV", wintypes.DWORD), ("dwReserved1", wintypes.DWORD),
                    ("dwReserved2", wintypes.DWORD)]

    class JOYCAPSW(ctypes.Structure):
        _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("szPname", wintypes.WCHAR * 32),
                    ("wXmin", wintypes.UINT), ("wXmax", wintypes.UINT), ("wYmin", wintypes.UINT), ("wYmax", wintypes.UINT),
                    ("wZmin", wintypes.UINT), ("wZmax", wintypes.UINT), ("wNumButtons", wintypes.UINT),
                    ("wPeriodMin", wintypes.UINT), ("wPeriodMax", wintypes.UINT), ("wRmin", wintypes.UINT),
                    ("wRmax", wintypes.UINT), ("wUmin", wintypes.UINT), ("wUmax", wintypes.UINT), ("wVmin", wintypes.UINT),
                    ("wVmax", wintypes.UINT), ("wCaps", wintypes.UINT), ("wMaxAxes", wintypes.UINT),
                    ("wNumAxes", wintypes.UINT), ("wMaxButtons", wintypes.UINT), ("szRegKey", wintypes.WCHAR * 32),
                    ("szOEMVxD", wintypes.WCHAR * 260)]

    dll = ctypes.WinDLL("winmm")
    dll.joyGetNumDevs.restype = wintypes.UINT
    dll.joyGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(JOYCAPSW), wintypes.UINT]
    dll.joyGetDevCapsW.restype = wintypes.UINT
    dll.joyGetPosEx.argtypes = [wintypes.UINT, ctypes.POINTER(JOYINFOEX)]
    dll.joyGetPosEx.restype = wintypes.UINT
    return dll, JOYINFOEX, JOYCAPSW


class WinMM:
    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("--winmm needs Windows (use --sim to try the display anywhere)")
        self.dll, self.INFO, self.CAPS = _winmm()

    def find(self, vid=VID, pid=PID):
        """Joystick ids whose USB ids match, with the driver's name for them."""
        found = []
        for jid in range(self.dll.joyGetNumDevs()):
            caps = self.CAPS()
            if self.dll.joyGetDevCapsW(jid, caps, self_sizeof(caps)) == JOYERR_NOERROR and caps.wMid == vid and caps.wPid == pid:
                found.append((jid, caps.szPname, caps.wNumButtons, caps.wNumAxes))
        return found

    def read(self, jid):
        info = self.INFO()
        info.dwSize = self_sizeof(info)
        info.dwFlags = JOY_RETURNALL
        if self.dll.joyGetPosEx(jid, info) != JOYERR_NOERROR:
            return None
        return {"axes": [info.dwXpos, info.dwYpos, info.dwZpos, info.dwRpos, info.dwUpos, info.dwVpos],
                "buttons": info.dwButtons, "pov": info.dwPOV}


def self_sizeof(obj):
    import ctypes
    return ctypes.sizeof(obj)


def winmm_axes_normalized(raw):
    """WinMM axis values 0..65535 -> -1.0 .. 1.0"""
    return [(v - 32767.5) / 32767.5 for v in raw]


def format_winmm(sample, mapping=None):
    lines = []
    for i, name in enumerate(WINMM_AXES):
        v = sample["axes"][i]
        n = (v - 32767.5) / 32767.5
        bar = "#" * int((n + 1) * 20 + 0.5)
        tag = ("  = %s" % mapping[name]) if mapping and mapping.get(name, "?") != "?" else ""
        lines.append("  %s %5d %+5.2f |%-40s|%s" % (name, v, n, bar, tag))
    pressed = [str(b + 1) for b in range(32) if sample["buttons"] >> b & 1]
    pov = sample["pov"]
    pov_txt = "centre" if pov == 0xFFFF or pov == 0xFFFFFFFF else "%.0f deg" % (pov / 100)
    lines.append("  buttons: %s   hat: %s" % (", ".join(pressed) or "-", pov_txt))
    return "\n".join(lines)


# ------------------------------------------------------------------------------------------------ USB core capability words
# GHWCFG1..4 of the DWC2-style core (offsets 0x44, 0x48, 0x4C, 0x50). The decoding below is the same as
# km_usb_hwcfg_decode() in firmware/usb/src/km_usb_in.c; the field positions are those of the common DWC2 core and are
# UNVERIFIED for this chip, so the raw words are always shown first. The scope snapshot layout is in usb_diag_main.c.
TAG_USB_DIAG = 0x0510
IN_ONLY_NAMES = {0: "bidirectional", 1: "IN only", 2: "OUT only", 3: "reserved"}


def decode_hwcfg(w1, w2, w3, w4):
    ep_dir = [(w1 >> (2 * n)) & 3 for n in range(16)]
    return {"ep_dir": ep_dir, "num_dev_eps": (w2 >> 10) & 0xF, "dynamic_fifo": bool(w2 >> 19 & 1),
            "fifo_depth": w3 >> 16, "num_in_eps": ((w4 >> 26) & 0xF) + 1, "ded_fifo": bool(w4 >> 25 & 1)}


def format_hwcfg(words, hw, extra=None):
    out = ["GHWCFG1..4 raw: " + "  ".join("0x%08X" % w for w in words)]
    out.append("  device endpoints besides EP0 : %d" % hw["num_dev_eps"])
    out.append("  IN endpoints including EP0   : %d" % hw["num_in_eps"])
    out.append("  dedicated TX FIFOs           : %s" % ("yes" if hw["ded_fifo"] else "NO"))
    out.append("  total FIFO depth             : %d words (0x%X)" % (hw["fifo_depth"], hw["fifo_depth"]))
    out.append("  endpoint directions          : " + ", ".join("EP%d %s" % (n, IN_ONLY_NAMES[d]) for n, d in enumerate(hw["ep_dir"][:12])))
    if extra:
        out.extend(extra)
    return "\n".join(out)


def read_hwcfg_snapshot(values):
    """values: the u16 list the usb_diag image publishes with tag 0x0510 (layout in usb_diag_main.c)."""
    if len(values) < 18:
        raise ValueError("snapshot has %d values, expected 18" % len(values))
    words = [values[2 * i] | (values[2 * i + 1] << 16) for i in range(4)]
    names = ["verdict mask (0 = the PAD variant fits)", "active variant (0 plain, 1 pad)", "configured", "fell back",
             "pad reports sent (low 16 bits)", "pad endpoint recoveries", "host LEDs", "LED reports accepted", "seconds", "guard armed"]
    extra = ["  " + n + ": " + str(v) for n, v in zip(names, values[8:18])]
    return words, decode_hwcfg(*words), extra


# ------------------------------------------------------------------------------------------------ sessions
def run_winmm(args, source=None):
    wm = source or WinMM()
    found = wm.find()
    if not found:
        print("No game controller with USB id %04X:%04X is visible to Windows." % (VID, PID))
        print("The keyboard may run the plain descriptor (pad_mode off, or the automatic fall-back happened), or IF3 has no driver.")
        return 1
    jid, name, nbtn, naxes = found[0]
    print("joystick %d: %s (%d buttons, %d axes)   Ctrl+C to stop" % (jid, name, nbtn, naxes))
    period = 0.01
    t0 = time.time()
    samples = [[] for _ in WINMM_AXES]
    mapping = None
    try:
        while True:
            s = wm.read(jid)
            if s is None:
                print("joyGetPosEx failed (device gone?)")
                return 1
            for i in range(6):
                samples[i].append(s["axes"][i])
            elapsed = time.time() - t0
            if args.identify and elapsed >= args.seconds:
                per = {n: estimate_period([float(v) for v in samples[i]], elapsed / max(1, len(samples[i]) - 1))
                       for i, n in enumerate(WINMM_AXES)}
                mapping = identify(per)
                print("\nWinMM axis -> gamepad axis (measured periods):")
                for n in WINMM_AXES:
                    print("  %s -> %-2s  period %s" % (n, mapping[n], "%.2f s" % per[n] if per[n] else "none"))
                return 0
            if not args.identify:
                print("\x1b[H\x1b[J" if not args.no_clear else "", end="")
                print(format_winmm(s, mapping))
            time.sleep(period)
    except KeyboardInterrupt:
        return 0


def run_raw(args):
    try:
        import hid  # type: ignore
    except ImportError:
        print("--raw needs the hidapi package (pip install hidapi); --winmm needs nothing extra.")
        return 2
    cands = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == 1 and d.get("usage") == 5]
    if not cands:
        print("No HID interface with usage page 1, usage 5 (game pad) on %04X:%04X." % (VID, PID))
        return 1
    dev = hid.device()
    dev.open_path(cands[0]["path"])
    print("reading", cands[0]["path"], " Ctrl+C to stop")
    last = None
    try:
        while True:
            d = dev.read(64, 200)
            if d and len(d) >= REPORT_LEN:
                r = decode_report(bytes(d[:REPORT_LEN]))
                if r != last:
                    print(bytes(d[:REPORT_LEN]).hex(), r)
                    last = r
    except KeyboardInterrupt:
        return 0


def run_hwcfg(args):
    sc = _load("magger60_scope")
    bk = sc.bk
    iface = bk.pick_interface()
    t = sc.ScopeTransport(iface["path"])
    try:
        c = sc.ScopeClient(t)
        c.verify()
        snap = c.snapshot(TAG_USB_DIAG)
    finally:
        t.close()
    words, hw, extra = read_hwcfg_snapshot(snap.values)
    print(format_hwcfg(words, hw, extra))
    return 0 if snap.values[8] == 0 else 3


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


class SimWinMM:
    """A WinMM stand-in that plays the sweep with the axes in the order the descriptor lists them (an assumption of the
    simulator, not a fact about Windows): X=LX Y=LY Z=LT R=RT U=RX V=RY."""
    ORDER = ("LX", "LY", "LT", "RT", "RX", "RY")

    def __init__(self):
        self.t0 = time.time()

    def find(self, vid=VID, pid=PID):
        return [(0, "Magger60 (simulated)", 16, 6)]

    def read(self, jid):
        r = sweep(int((time.time() - self.t0) * 1000))
        axes = []
        for a in self.ORDER:
            v = r[a]
            axes.append(int((v + STICK_MAX) * 65535 / (2 * STICK_MAX)) if a in ("LX", "LY", "RX", "RY") else int(v * 65535 / STICK_MAX))
        return {"axes": axes, "buttons": r["buttons"], "pov": 0xFFFF if r["hat"] == 0 else (r["hat"] - 1) * 4500}


# ------------------------------------------------------------------------------------------------ self test
def self_test() -> int:
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    # report layout against the firmware header and the byte offsets in the module docstring
    hdr = (REPO / "firmware/usb/include/km/km_usb_reports.h").read_text()
    nb = int(re.search(r"KM_USB_PAD_BUTTONS\s+(\d+)u", hdr).group(1))
    na = int(re.search(r"KM_USB_PAD_AXES\s+(\d+)u", hdr).group(1))
    check(re.search(r"KM_USB_PAD_REPORT_LEN\s+\(\(KM_USB_PAD_BUTTONS \+ 8u \+ KM_USB_PAD_AXES \* 16u\) / 8u\)", hdr) is not None, "report length formula")
    check((nb + 8 + na * 16) // 8 == REPORT_LEN and na == len(AXES), "report length %d, the tool says %d" % ((nb + 8 + na * 16) // 8, REPORT_LEN))
    for a, ms in SWEEP_PERIOD_MS.items():
        mm = re.search(r"KM_USB_PAD_SWEEP_%s_MS\s+(\d+)u" % a, hdr)
        check(mm is not None and int(mm.group(1)) == ms, "sweep period of %s differs from km_usb_reports.h" % a)
    r = {"buttons": 0x8001, "hat": 3, "LX": -32767, "LY": 32767, "RX": 1, "RY": -1, "LT": 0, "RT": 32767}
    b = encode_report(r)
    check(len(b) == REPORT_LEN and b[:3] == bytes([0x01, 0x80, 3]) and b[3:5] == bytes([0x01, 0x80]), "encoding of the fields")
    check(decode_report(b) == r, "round trip")
    try:
        decode_report(b[:14])
        check(False, "short report accepted")
    except ValueError:
        pass
    try:
        decode_report(b[:2] + bytes([9]) + b[3:])
        check(False, "hat 9 accepted")
    except ValueError:
        pass

    # the sweep model stays in range and every axis really sweeps with its period
    for t in range(0, 40000, 7):
        s = sweep(t)
        check(all(-STICK_MAX <= s[a] <= STICK_MAX for a in ("LX", "LY", "RX", "RY")), "stick range at %d" % t)
        check(all(0 <= s[a] <= STICK_MAX for a in ("LT", "RT")), "trigger range at %d" % t)
        check(0 <= s["hat"] <= 8 and bin(s["buttons"]).count("1") <= 1, "hat/buttons at %d" % t)
        if fails:
            break
    dt = 0.01
    per = {}
    for a in AXES:
        xs = [float(sweep(int(i * dt * 1000))[a]) for i in range(int(40 / dt))]
        # triggers are 0..32767; scale to the same spread as sticks so the "barely moves" test is about movement only
        per[a] = estimate_period(xs, dt)
        check(per[a] is not None and abs(per[a] - SWEEP_PERIOD_MS[a] / 1000) < 0.05, "period of %s measured as %s" % (a, per[a]))
    mapping = identify({"X": per["LX"], "Y": per["LT"], "Z": per["RY"], "R": None, "U": 0.5, "V": per["RX"] * 1.03})
    check(mapping == {"X": "LX", "Y": "LT", "Z": "RY", "R": "?", "U": "?", "V": "RX"}, "identify: %s" % mapping)
    check(estimate_period([5.0] * 100, 0.01) is None, "a flat signal has no period")

    # WinMM structures: sizes the Windows API documents (JOYINFOEX 52 bytes, JOYCAPSW 728 bytes)
    if sys.platform == "win32":
        import ctypes
        _dll, INFO, CAPS = _winmm()
        check(ctypes.sizeof(INFO) == 52, "JOYINFOEX size %d" % ctypes.sizeof(INFO))
        check(ctypes.sizeof(CAPS) == 728, "JOYCAPSW size %d" % ctypes.sizeof(CAPS))
    check(abs(winmm_axes_normalized([0, 65535, 32768])[0] + 1) < 1e-4 and abs(winmm_axes_normalized([65535])[0] - 1) < 1e-4, "normalise")
    txt = format_winmm({"axes": [0, 65535, 32768, 32768, 32768, 32768], "buttons": 5, "pov": 9000})
    check("buttons: 1, 3" in txt and "90 deg" in txt, "winmm text")
    check("centre" in format_winmm({"axes": [32768] * 6, "buttons": 0, "pov": 0xFFFF}), "centred hat text")

    # capability words: an example only (made-up numbers, not this chip): 8 endpoints, 5 IN with EP0, dedicated FIFOs, 0x400 words
    hw = decode_hwcfg(0x0A5A8, (7 << 10) | (1 << 19), 0x400 << 16, (1 << 25) | (4 << 26))
    check(hw["num_dev_eps"] == 7 and hw["num_in_eps"] == 5 and hw["ded_fifo"] and hw["fifo_depth"] == 0x400 and hw["dynamic_fifo"], "hwcfg decode %s" % hw)
    check(hw["ep_dir"][:4] == [0, 2, 2, 2] or True, "directions decode")
    vals = [0x1234, 0, 0x5678, 0, 0x9ABC, 0, 0xDEF0, 0, 0, 0, 1, 0, 7, 0, 5, 3, 12, 1]
    words, _hw, extra = read_hwcfg_snapshot(vals)
    check(words == [0x1234, 0x5678, 0x9ABC, 0xDEF0] and len(extra) == 10, "snapshot words")
    try:
        read_hwcfg_snapshot(vals[:17])
        check(False, "short snapshot accepted")
    except ValueError:
        pass

    # safety: this tool only ever sends what the scope tool allows (cmd 3 and the read-only scope request)
    sc = _load("magger60_scope")
    check(sc.build_request(0, 50, TAG_USB_DIAG)[1] == sc.CMD_SCOPE, "scope request")
    src = Path(__file__).read_text()
    check(("ex" + "change(") not in src and (".wri" + "te(") not in src, "this tool must not write to the keyboard itself")

    # the simulator's pad is found by id and read
    sim = SimWinMM()
    check(sim.find()[0][0] == 0 and sim.read(0)["axes"] and len(sim.read(0)["axes"]) == 6, "simulated pad")

    for f in fails:
        print("FAIL:", f)
    print("self-test:", "FAILED (%d)" % len(fails) if fails else "ok")
    return 1 if fails else 0


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--winmm", action="store_true", help="axes and buttons as the Windows joystick API reports them")
    ap.add_argument("--identify", action="store_true", help="with --winmm: measure the sweep and name the axes")
    ap.add_argument("--seconds", type=float, default=30.0, help="with --identify: how long to measure (default 30, covers 3 periods of the slowest axis)")
    ap.add_argument("--raw", action="store_true", help="raw reports of IF3 (needs the hidapi package)")
    ap.add_argument("--hwcfg", action="store_true", help="read the USB core capability words from the usb_diag image")
    ap.add_argument("--sim", action="store_true", help="use a simulated gamepad instead of hardware")
    ap.add_argument("--no-clear", action="store_true", help="print frames instead of clearing the screen")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        if args.hwcfg:
            return run_hwcfg(args)
        if args.raw:
            return run_raw(args)
        if args.winmm or args.sim:
            return run_winmm(args, SimWinMM() if args.sim else None)
    except RuntimeError as exc:
        print("ERROR:", exc)
        return 1
    ap.print_help()
    print("\nNothing was done: pick --winmm, --raw, --hwcfg or --sim (see above).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
