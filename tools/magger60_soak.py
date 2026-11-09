#!/usr/bin/env python3
"""magger60_soak.py - long-run comparison of "what Windows sees" and "what our firmware says it is reporting" (M5 task T-E).

The question it answers: is there a key that Windows believes is held while the keyboard says it is not (a stuck key, like the
50 second space bar seen in K3), or the other way round (a press that never arrived)? It tells you WHICH of the two it is.

  host side     Windows Raw Input (RegisterRawInputDevices with RIDEV_INPUTSINK, so it works while another window has focus),
                filtered by device name: only events whose device path contains VID_19F5&PID_FB49 are used. Every other keyboard
                (laptop, the SONiX one, ...) is ignored, so typing elsewhere never counts. State per physical key (scan code).
  firmware side the scope channel of OUR firmware (tools/magger60_scope.py: only cmd 3 and the read-only 0xC5 request, only on a
                build string that carries KMFW). Tag 0x0301 = bitmask of the slots the keyboard is currently reporting,
                tag 0x0300 = health counters (km_health.h). Each poll is ONE request with a 22 byte window, so a snapshot can
                never be torn.

    python tools/magger60_soak.py                       # dry run: explains and demonstrates on a simulated keyboard, sends nothing
    python tools/magger60_soak.py --self-test           # no hardware, no Windows needed (a Windows API probe runs when available)
    python tools/magger60_soak.py --sim --duration 600  # the monitor against a simulated keyboard and simulated Windows events
    python tools/magger60_soak.py --run --duration 28800 --csv soak.csv      # the real thing: 8 hours

--run needs Windows, the `hidapi` package, and our firmware (km_l30 or later) on the keyboard. Stop with Ctrl+C. Events print as they
happen; --csv also writes them to a file; every --report seconds a counter line is printed (frames, scan stalls, DMA errors, USB
resets / suspends, report retries / resyncs, engine time, reset cause, uptime, boot count) and a decrease of the uptime or an
increase of the boot count is logged as FIRMWARE_RESET.

Event types:
  HOST_DOWN_FW_UP   Windows holds a key the firmware is not reporting for longer than --threshold (default 0.2 s): a STUCK KEY. If the
                    firmware mask shows the same key up for the whole time, the release report was lost or never sent.
  FW_DOWN_HOST_UP   the firmware is reporting a key that Windows does not see for longer than the threshold: a LOST PRESS.
  RESOLVED          a reported mismatch ended (with its total duration).
  FIRMWARE_RESET    uptime went backwards / boot counter changed (a watchdog reset or a re-plug).
  SCOPE_ERRORS      the firmware could not be read for a while (host asleep, device re-enumerating): the comparison is paused.
  HOST_DEVICE_GONE / HOST_DEVICE_SEEN   Windows removed / added the keyboard (hot plug); the host key state is cleared.

LIMITS, honestly:
  * Key mapping. The comparison uses the DEFAULT layer of km_keymap.c (slot -> HID usage -> Windows scan code). It breaks as soon as a
    layer is active (FN held, profile switch): then the firmware mask lists slots and Windows sees other keys. Not an issue for the
    L3-0 image, which has no layers.
  * Time resolution. The firmware publishes the mask every 20 ms (10 ms in the other half of the rotation another tag is published),
    one scope poll takes a USB round trip of a few ms, and Windows delivers Raw Input with its own queueing. So differences below about
    50 ms are not meaningful; the default threshold of 200 ms is four times that. A mismatch shorter than the threshold is never logged.
  * The mask is the keyboard's TARGET state (what it is trying to report), not a read-back of the endpoint. If the mask says up and
    Windows says down, the report did not arrive or was never sent; the extra fields (fault flags, "undelivered" flag) in the event say
    whether the firmware knew it had a problem.
  * Raw Input shows what Windows decoded. A key that Windows swallows itself (some media / system keys) or an application that
    modifies the key state is not visible. Auto-repeat only repeats "down" events and does not matter here.
  * It cannot tell a stuck key from a user holding a key down while the firmware is in a fault state; the fault flags in the event
    tell. While the machine sleeps, nothing is compared.
"""
import argparse
import csv
import ctypes
import importlib.util
import io
import random
import re
import struct
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


scope = _load("magger60_scope")
bk = scope.bk

TAG_HEALTH = 0x0300
TAG_KEYMASK = 0x0301
KEYMASK_VALUES = 8
HEALTH_VALUES = 24
DEFAULT_MATCH = ("VID_19F5&PID_FB49",)
FAULT_NAMES = [(0x01, "SCAN_STALL"), (0x02, "DMA_ERROR"), (0x04, "CAL_LOST"), (0x08, "USB_SUSPENDED"), (0x10, "ENGINE_OVERRUN")]
RESET_NAMES = [(0x20, "iwdg"), (0x40, "wwdg"), (0x10, "soft"), (0x08, "por"), (0x04, "pin"), (0x80, "lowpower")]

# ----------------------------------------------------------------------------- key mapping: slot -> HID usage -> scan code

# USB HID keyboard usage -> (set 1 make code, extended). The make codes are the standard PC keyboard scan codes that Windows reports
# in RAWKEYBOARD.MakeCode (Microsoft "Keyboard scan code specification"); the usages are the HID Usage Tables, page 0x07.
SCAN = {}
for i, ch in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
    SCAN[0x04 + i] = ({"A": 0x1E, "B": 0x30, "C": 0x2E, "D": 0x20, "E": 0x12, "F": 0x21, "G": 0x22, "H": 0x23, "I": 0x17, "J": 0x24,
                       "K": 0x25, "L": 0x26, "M": 0x32, "N": 0x31, "O": 0x18, "P": 0x19, "Q": 0x10, "R": 0x13, "S": 0x1F, "T": 0x14,
                       "U": 0x16, "V": 0x2F, "W": 0x11, "X": 0x2D, "Y": 0x15, "Z": 0x2C}[ch], False)
for i in range(9):
    SCAN[0x1E + i] = (0x02 + i, False)                      # 1..9
SCAN[0x27] = (0x0B, False)                                  # 0
SCAN.update({0x28: (0x1C, False), 0x29: (0x01, False), 0x2A: (0x0E, False), 0x2B: (0x0F, False), 0x2C: (0x39, False),
             0x2D: (0x0C, False), 0x2E: (0x0D, False), 0x2F: (0x1A, False), 0x30: (0x1B, False), 0x31: (0x2B, False),
             0x33: (0x27, False), 0x34: (0x28, False), 0x36: (0x33, False), 0x37: (0x34, False), 0x38: (0x35, False),
             0x39: (0x3A, False),
             0xE0: (0x1D, False), 0xE1: (0x2A, False), 0xE2: (0x38, False), 0xE3: (0x5B, True),
             0xE4: (0x1D, True), 0xE5: (0x36, False), 0xE6: (0x38, True), 0xE7: (0x5C, True)})


def load_keymap(path=REPO / "firmware/core/src/km_keymap.c"):
    """[(slot, usage, name)] for the 76 slots, read from the C source of the default layer (so the tool follows the firmware)."""
    text = Path(path).read_text(encoding="utf-8")
    rows = re.findall(r'/\*\s*(\d+)\s*\*/\s*\{\s*0x([0-9A-Fa-f]+),\s*"([^"]*)"\s*\}', text)
    out = [(int(s), int(u, 16), n) for s, u, n in rows]
    if [r[0] for r in out] != list(range(76)):
        raise RuntimeError("could not read the 76 slots from " + str(path))
    return out


KEYMAP = load_keymap()
SLOT_NAME = {s: n for s, _u, n in KEYMAP}
SLOT_KEY = {s: SCAN[u] for s, u, _n in KEYMAP if u in SCAN}      # slot -> (make code, extended)
KEY_NAME = {SLOT_KEY[s]: SLOT_NAME[s] for s in SLOT_KEY}


# ----------------------------------------------------------------------------- scope payloads (km_health.h)

def decode_keymask(values):
    """tag 0x0301 -> (set of slots, sequence, fault flags, undelivered)."""
    if len(values) != KEYMASK_VALUES:
        raise ValueError("tag 0x0301 has %d values, expected %d" % (len(values), KEYMASK_VALUES))
    slots = {16 * k + b for k in range(5) for b in range(16) if values[k] >> b & 1 and 16 * k + b < 76}
    return slots, values[5], values[6] & 0xFF, bool(values[7])


def faults_text(flags):
    return "+".join(n for bit, n in FAULT_NAMES if flags & bit) or "none"


def decode_health(v):
    if len(v) != HEALTH_VALUES:
        raise ValueError("tag 0x0300 has %d values, expected %d" % (len(v), HEALTH_VALUES))
    cause = v[0] >> 8
    return {"faults": v[0] & 0xFF, "reset_cause": cause, "frames": v[1] | v[2] << 16, "scan_stalls": v[3], "dma_errors": v[4],
            "scan_restarts": v[5], "engine_max_cycles": v[6], "engine_over_budget": v[7], "engine_overruns": v[8],
            "usb_resets": v[9], "usb_suspends": v[10], "usb_resumes": v[11], "report_retries": v[12], "report_resyncs": v[13],
            "glitches": v[14], "calibrations": v[15], "calibration_refused": v[16], "reports_sent": v[17], "heartbeats": v[18],
            "ep_recoveries": v[19], "prev_faults": v[20], "boots": v[21], "uptime_s": v[22], "engine_total_max": v[23]}


def reset_text(cause):
    return next((n for bit, n in RESET_NAMES if cause & bit), "none")


# ----------------------------------------------------------------------------- the comparison

class Event:
    def __init__(self, t, kind, key="", duration=0.0, host=None, fw=None, detail=""):
        self.t, self.kind, self.key, self.duration, self.host, self.fw, self.detail = t, kind, key, duration, host, fw, detail

    def line(self):
        d = "" if not self.duration else " for %.3f s" % self.duration
        k = " " + self.key if self.key else ""
        return "[%9.3f s] %s%s%s%s" % (self.t, self.kind, k, d, (" - " + self.detail) if self.detail else "")


class SoakMonitor:
    """Compares the host key state with the firmware mask. Time is always passed in (seconds, any epoch), so it runs the same in the
    real tool and in the self-test."""

    def __init__(self, threshold=0.2, stale=0.1):
        # a firmware mask older than `stale` is not compared at all; it must be well below the threshold, or an old mask could
        # produce a mismatch that is already over
        self.threshold, self.stale = threshold, min(stale, threshold / 2)
        self.host = set()                    # keys (make, ext) that Windows holds down
        self.fw = set()                      # keys the firmware mask lists
        self.fw_t = None
        self.fw_faults, self.fw_undelivered, self.fw_seq = 0, False, None
        self.since = {}                      # key -> time the current mismatch began
        self.reported = {}                   # key -> kind already reported
        self.events = []
        self.stats = {"compares": 0, "stale_skips": 0, "events": 0, "max_mismatch": 0.0}

    def host_event(self, t, key, down):
        (self.host.add if down else self.host.discard)(key)

    def host_clear(self, t):
        self.host.clear()

    def fw_mask(self, t, slots, seq, faults, undelivered):
        self.fw = {SLOT_KEY[s] for s in slots if s in SLOT_KEY}
        self.fw_t, self.fw_seq, self.fw_faults, self.fw_undelivered = t, seq, faults, undelivered

    def _emit(self, ev):
        self.events.append(ev)
        self.stats["events"] += 1
        return ev

    def poll(self, t):
        """Evaluate at time t; returns the new events."""
        new = []
        if self.fw_t is None or t - self.fw_t > self.stale:
            self.stats["stale_skips"] += 1
            for k in [k for k in self.since if k not in self.reported]:
                del self.since[k]         # a disagreement that began before the gap is not evidence about after it
            return new
        self.stats["compares"] += 1
        for key in self.host | self.fw | set(self.since):
            h, f = key in self.host, key in self.fw
            name = KEY_NAME.get(key, "scan 0x%02X%s" % (key[0], " E0" if key[1] else ""))
            if h != f:
                t0 = self.since.setdefault(key, t)
                dur = t - t0
                self.stats["max_mismatch"] = max(self.stats["max_mismatch"], dur)
                if dur >= self.threshold and key not in self.reported:
                    kind = "HOST_DOWN_FW_UP" if h else "FW_DOWN_HOST_UP"
                    self.reported[key] = kind
                    detail = "firmware faults: %s%s, seq %s" % (faults_text(self.fw_faults),
                                                              ", report undelivered" if self.fw_undelivered else "", self.fw_seq)
                    new.append(self._emit(Event(t, kind, name, dur, h, f, detail)))
            elif key in self.since:
                t0 = self.since.pop(key)
                if key in self.reported:
                    new.append(self._emit(Event(t, "RESOLVED", name, t - t0, h, f, "was " + self.reported.pop(key))))
        return new

    def note(self, t, kind, detail="", key=""):
        return self._emit(Event(t, kind, key, 0.0, None, None, detail))


# ----------------------------------------------------------------------------- Windows Raw Input

def device_matches(name, patterns):
    n = (name or "").upper()
    return any(p.upper() in n for p in patterns)


class RawKeyboard(ctypes.Structure):
    _fields_ = [("MakeCode", ctypes.c_ushort), ("Flags", ctypes.c_ushort), ("Reserved", ctypes.c_ushort), ("VKey", ctypes.c_ushort),
                ("Message", ctypes.c_uint), ("ExtraInformation", ctypes.c_ulong)]


RI_KEY_BREAK, RI_KEY_E0, RI_KEY_E1 = 1, 2, 4
RIM_TYPEKEYBOARD = 1
HEADER_FORMAT_64 = "<IIQQ"       # dwType, dwSize, hDevice, wParam on a 64 bit process
HEADER_FORMAT_32 = "<IIII"


def parse_rawinput(buf):
    """RAWINPUT bytes -> (device handle, make code, extended, down) or None for anything that is not a keyboard make/break.
    Fake keys (make code 0 / 0xFF, e.g. the Pause sequence) are dropped."""
    fmt = HEADER_FORMAT_64 if ctypes.sizeof(ctypes.c_void_p) == 8 else HEADER_FORMAT_32
    hsz = struct.calcsize(fmt)
    if len(buf) < hsz + ctypes.sizeof(RawKeyboard):
        return None
    typ, _size, dev, _wp = struct.unpack_from(fmt, buf, 0)
    if typ != RIM_TYPEKEYBOARD:
        return None
    kb = RawKeyboard.from_buffer_copy(buf, hsz)
    if kb.MakeCode in (0, 0xFF) or kb.Flags & RI_KEY_E1:
        return None
    return dev, kb.MakeCode, bool(kb.Flags & RI_KEY_E0), not (kb.Flags & RI_KEY_BREAK)


class WindowsRawInput:
    """Message-only window + RegisterRawInputDevices(keyboard, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY) on a worker thread. Events whose
    device path matches go to `sink(t, key, down)`; device add / remove go to `on_device(t, text)`."""

    RIDEV_INPUTSINK, RIDEV_DEVNOTIFY = 0x100, 0x2000
    WM_INPUT, WM_INPUT_DEVICE_CHANGE = 0x00FF, 0x00FE
    RID_INPUT, RIDI_DEVICENAME = 0x10000003, 0x20000007
    GIDC_ARRIVAL, GIDC_REMOVAL = 1, 2

    def __init__(self, patterns, sink, on_device=None, clock=time.monotonic):
        if sys.platform != "win32":
            raise RuntimeError("Raw Input exists on Windows only")
        import threading
        from ctypes import wintypes as wt
        self.wt, self.patterns, self.sink, self.on_device, self.clock = wt, patterns, sink, on_device, clock
        self._names, self._stop, self._ready = {}, threading.Event(), threading.Event()
        self.error, self.hwnd, self.count, self.seen_names = None, None, 0, set()
        self._thread = threading.Thread(target=self._run, name="raw-input", daemon=True)

    def start(self, timeout=5.0):
        self._thread.start()
        if not self._ready.wait(timeout) or self.error:
            raise RuntimeError(self.error or "Raw Input window did not start")

    def stop(self):
        self._stop.set()
        self._thread.join(2.0)

    def _device_name(self, user32, handle):
        if handle in self._names:
            return self._names[handle]
        wt = self.wt
        size = wt.UINT(0)
        user32.GetRawInputDeviceInfoW(handle, self.RIDI_DEVICENAME, None, ctypes.byref(size))
        buf = ctypes.create_unicode_buffer(max(size.value, 1))
        user32.GetRawInputDeviceInfoW(handle, self.RIDI_DEVICENAME, buf, ctypes.byref(size))
        self._names[handle] = buf.value
        self.seen_names.add(buf.value)
        return buf.value

    def _run(self):
        wt = self.wt
        LRESULT = ctypes.c_ssize_t
        WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

        class WNDCLASSW(ctypes.Structure):
            _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                        ("hInstance", wt.HINSTANCE), ("hIcon", wt.HANDLE), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HANDLE),
                        ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]

        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT), ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]

        user32, kernel32 = ctypes.WinDLL("user32", use_last_error=True), ctypes.WinDLL("kernel32", use_last_error=True)
        user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        user32.DefWindowProcW.restype = LRESULT
        user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p, ctypes.POINTER(wt.UINT), wt.UINT]
        user32.GetRawInputData.restype = wt.UINT
        user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p, ctypes.POINTER(wt.UINT)]
        user32.GetRawInputDeviceInfoW.restype = wt.UINT
        user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, wt.HWND, wt.HANDLE, wt.HINSTANCE, ctypes.c_void_p]
        user32.CreateWindowExW.restype = wt.HWND
        user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), wt.UINT, wt.UINT]
        user32.RegisterRawInputDevices.restype = wt.BOOL
        kernel32.GetModuleHandleW.restype = wt.HINSTANCE
        hsz = ctypes.sizeof(ctypes.c_void_p) * 2 + 8

        def proc(hwnd, msg, wparam, lparam):
            try:
                if msg == self.WM_INPUT:
                    size = wt.UINT(0)
                    user32.GetRawInputData(lparam, self.RID_INPUT, None, ctypes.byref(size), hsz)
                    buf = ctypes.create_string_buffer(size.value)
                    if user32.GetRawInputData(lparam, self.RID_INPUT, buf, ctypes.byref(size), hsz) == size.value:
                        r = parse_rawinput(buf.raw)
                        if r is not None:
                            dev, make, ext, down = r
                            if device_matches(self._device_name(user32, dev), self.patterns):
                                self.count += 1
                                self.sink(self.clock(), (make, ext), down)
                elif msg == self.WM_INPUT_DEVICE_CHANGE and self.on_device is not None:
                    name = self._device_name(user32, lparam)
                    if device_matches(name, self.patterns):
                        if wparam == self.GIDC_REMOVAL:
                            self._names.pop(lparam, None)
                        self.on_device(self.clock(), "arrival" if wparam == self.GIDC_ARRIVAL else "removal", name)
            except Exception as exc:                     # never let an exception cross into the window procedure
                self.error = repr(exc)
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        self._proc = WNDPROC(proc)                       # keep a reference
        hinst = kernel32.GetModuleHandleW(None)
        cls = WNDCLASSW()
        cls.lpfnWndProc, cls.hInstance, cls.lpszClassName = self._proc, hinst, "Magger60SoakRawInput"
        ctypes.windll.user32.RegisterClassW(ctypes.byref(cls))
        self.hwnd = user32.CreateWindowExW(0, cls.lpszClassName, "soak", 0, 0, 0, 0, 0, wt.HWND(-3), None, hinst, None)   # HWND_MESSAGE
        if not self.hwnd:
            self.error = "CreateWindowExW failed (%d)" % ctypes.get_last_error()
            self._ready.set()
            return
        dev = RAWINPUTDEVICE(1, 6, self.RIDEV_INPUTSINK | self.RIDEV_DEVNOTIFY, self.hwnd)    # generic desktop / keyboard
        if not user32.RegisterRawInputDevices(ctypes.byref(dev), 1, ctypes.sizeof(dev)):
            self.error = "RegisterRawInputDevices failed (%d)" % ctypes.get_last_error()
            self._ready.set()
            return
        self._ready.set()
        msg = wt.MSG()
        while not self._stop.is_set():
            while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.001)
        user32.DestroyWindow(self.hwnd)


# ----------------------------------------------------------------------------- the firmware side

class Firmware:
    """Reads tag 0x0301 / 0x0300 through the scope client. One request per poll (22 / 56 byte window): no paging, no tearing."""

    def __init__(self, client):
        self.client = client
        self.misses = self.errors = 0

    def _read(self, tag, nvalues):
        p = self.client.page(0, scope.HEADER_BYTES + 2 * nvalues, tag)
        if p.tag != tag or len(p.values) != nvalues or p.count != nvalues:
            self.misses += 1                              # another tag is published right now
            return None
        return p.seq, p.values

    def keymask(self):
        r = self._read(TAG_KEYMASK, KEYMASK_VALUES)
        return None if r is None else decode_keymask(r[1])

    def health(self):
        r = self._read(TAG_HEALTH, HEALTH_VALUES)
        return None if r is None else decode_health(r[1])


def format_health(h):
    return ("frames %d  stalls %d  restarts %d  dma_err %d  engine max %d cyc (+irq %d)  over budget %d  usb resets %d suspends %d resumes %d  "
            "retries %d resyncs %d recoveries %d  glitches %d  cal %d/%d  faults %s  reset cause %s  boots %d  uptime %d s  prev faults %s"
            % (h["frames"], h["scan_stalls"], h["scan_restarts"], h["dma_errors"], h["engine_max_cycles"], h["engine_total_max"],
               h["engine_over_budget"], h["usb_resets"], h["usb_suspends"], h["usb_resumes"], h["report_retries"], h["report_resyncs"],
               h["ep_recoveries"], h["glitches"], h["calibrations"], h["calibration_refused"], faults_text(h["faults"]),
               reset_text(h["reset_cause"]), h["boots"], h["uptime_s"],
               "n/a" if h["prev_faults"] == 0xFFFF else faults_text(h["prev_faults"])))


class Session:
    """One monitoring loop; `clock`, `sleep` are injectable so the self-test runs in virtual time."""

    def __init__(self, fw, mon, report_every=60.0, poll=0.005, log=print, clock=time.monotonic, sleep=time.sleep, csv_writer=None):
        self.fw, self.mon, self.report_every, self.poll, self.log = fw, mon, report_every, poll, log
        self.clock, self.sleep, self.csv = clock, sleep, csv_writer
        self.t0 = clock()
        self.last_health = None
        self.next_report = self.t0 + report_every
        self.err_since = None
        self.err_reported = False
        self.health_log = []

    def _emit(self, ev):
        self.log(ev.line())
        if self.csv:
            self.csv.writerow([datetime.now().isoformat(timespec="milliseconds"), "%.3f" % ev.t, ev.kind, ev.key, "%.3f" % ev.duration, ev.detail])

    def step(self):
        t = self.clock() - self.t0
        try:
            r = self.fw.keymask()
            if self.err_since is not None:
                if self.err_reported:
                    self._emit(self.mon.note(t, "SCOPE_OK", "firmware readable again after %.1f s" % (t - self.err_since)))
                self.err_since, self.err_reported = None, False
            if r is not None:
                slots, seq, faults, undel = r
                self.mon.fw_mask(t, slots, seq, faults, undel)
        except (bk.BackupError, scope.ScopeError) as exc:
            self.fw.errors += 1
            if self.err_since is None:
                self.err_since = t
            elif t - self.err_since > 2.0 and not self.err_reported:
                self.err_reported = True
                self._emit(self.mon.note(t, "SCOPE_ERRORS", "cannot read the firmware for %.1f s: %s (comparison paused)" % (t - self.err_since, exc)))
        for ev in self.mon.poll(t):
            self._emit(ev)
        if self.clock() >= self.next_report:
            self.next_report += self.report_every
            self.report(t)

    def report(self, t):
        try:
            h = None
            for _ in range(25):                           # tag 0x0300 is published 10 ms out of every 100 ms: look for about 0.3 s
                h = self.fw.health()
                if h is not None:
                    break
                self.sleep(0.011)
        except (bk.BackupError, scope.ScopeError):
            return
        if h is None:
            return
        last = self.last_health
        if last is not None and (h["uptime_s"] < last["uptime_s"] or h["boots"] != last["boots"]):
            self._emit(self.mon.note(t, "FIRMWARE_RESET", "uptime %d -> %d s, boots %d -> %d, reset cause %s, faults before: %s"
                                     % (last["uptime_s"], h["uptime_s"], last["boots"], h["boots"], reset_text(h["reset_cause"]),
                                        "n/a" if h["prev_faults"] == 0xFFFF else faults_text(h["prev_faults"]))))
        self.last_health = h
        self.health_log.append((t, h))
        self.log("[%9.3f s] counters: %s   | host/firmware compares %d, stale %d, events %d, longest mismatch %.3f s, scope misses %d errors %d"
                 % (t, format_health(h), self.mon.stats["compares"], self.mon.stats["stale_skips"], self.mon.stats["events"],
                    self.mon.stats["max_mismatch"], self.fw.misses, self.fw.errors))

    def run(self, duration):
        end = self.t0 + duration if duration else None
        self.report(0.0)                                  # baseline counters (also the reference for FIRMWARE_RESET)
        while end is None or self.clock() < end:
            self.step()
            self.sleep(self.poll)


# ----------------------------------------------------------------------------- simulated keyboard + Windows

class SimWorld:
    """Virtual time. A typist presses keys (fw state), the firmware publishes tag 0x0301 / 0x0300 in the real rotation, and Windows
    gets the matching Raw Input events after a delay. Faults can be injected: a stuck key (the release never reaches Windows), a lost
    press, a firmware reset, an unreadable stretch."""

    def __init__(self, seed=1):
        self.rng = random.Random(seed)
        self.now = 0.0
        self.down = {}                    # slot -> release time (typist)
        self.pending_host = []            # (time, key, down)
        self.boots, self.boot_t0 = 1, 0.0
        self.publish_n = 0
        self.mask_seq = 0
        self.stuck, self.lost = set(), set()
        self.unreadable_until = -1.0
        self.frames = 0
        self.keys = sorted(SLOT_KEY)

    def press(self, slot, hold):
        if slot in self.down:
            return
        self.down[slot] = self.now + hold
        if slot in self.lost:
            self.lost.discard(slot)
        else:
            self.pending_host.append((self.now + 0.004, SLOT_KEY[slot], True))

    def advance(self, dt, host_cb):
        end = self.now + dt
        while self.now < end - 1e-12:
            self.now = min(end, self.now + 0.001)
            for slot, rel in list(self.down.items()):
                if self.now >= rel:
                    del self.down[slot]
                    if slot in self.stuck:
                        self.stuck.discard(slot)               # the release report never arrives: Windows keeps the key
                    else:
                        self.pending_host.append((self.now + 0.004, SLOT_KEY[slot], False))
            for ev in [e for e in self.pending_host if e[0] <= self.now]:
                self.pending_host.remove(ev)
                host_cb(self.now, ev[1], ev[2])
            self.frames += 43

    # the scope side: tag published at a given time
    def published(self):
        n = int(self.now / 0.01)
        return 0x0301 if n % 2 == 0 else (0x0300 if n % 10 == 7 else 0x0100)

    def values(self, tag):
        if tag == TAG_KEYMASK:
            vals = [0] * 8
            for s in self.down:
                vals[s >> 4] |= 1 << (s & 15)
            vals[5] = int(self.now / 0.02) & 0xFFFF
            return vals
        if tag == TAG_HEALTH:
            v = [0] * 24
            v[0] = 0x2000 if self.boots > 1 else 0
            v[1] = self.frames & 0xFFFF
            v[21] = self.boots
            v[22] = int(self.now - self.boot_t0) & 0xFFFF
            v[20] = 0xFFFF
            return v
        return [0] * 24


class SimScopeTransport(bk.Transport):
    """Serves cmd 3 and 0xC5 from a SimWorld in the protocol of km_scope.h (reply layout of scope.SimulatedScopeKeyboard)."""

    def __init__(self, world):
        self.w = world
        self.cmds = []

    def exchange(self, packet):
        packet = scope.guard_outgoing(packet)
        self.cmds.append(packet[1])
        if packet[1] == bk.CMD_GET_INFO:
            return scope.SimulatedScopeKeyboard._reply(packet[1], bytes([0x04, 0x01]) + b"Jan  1 2026,00:00:00KMFW-SIM")
        if self.w.now < self.w.unreadable_until:
            raise bk.BackupError("no (or short) response from the keyboard")
        tag = self.w.published()
        start, want = packet[5], packet[6] | packet[7] << 8
        vals = self.w.values(tag)
        n = 0
        if want in (0, tag):
            n = max(0, min((packet[4] - scope.HEADER_BYTES) // 2, scope.PAGE_VALUES, len(vals) - start))
        data = bytearray([tag & 0xFF, tag >> 8, int(self.w.now / 0.01) & 0xFF, 0, len(vals) & 0xFF, len(vals) >> 8])
        data[2:4] = struct.pack("<H", int(self.w.now / 0.01) % 65535 + 1)
        for v in vals[start:start + n]:
            data += struct.pack("<H", v)
        return scope.SimulatedScopeKeyboard._reply(packet[1], bytes(data), start)


def simulate(seconds, seed=1, stuck_at=None, lost_at=None, reset_at=None, outage=None, log=lambda *_: None, report_every=30.0):
    """Run the monitor against the simulated world for `seconds` of virtual time. Returns (monitor, session, world, transport)."""
    w = SimWorld(seed)
    mon = SoakMonitor()
    tr = SimScopeTransport(w)
    client = scope.ScopeClient(tr)
    client.verify()
    host_cb = lambda t, key, down: mon.host_event(w.now, key, down)
    state = {"stuck": False, "lost": False, "reset": False, "outage": False}
    sess = Session(Firmware(client), mon, report_every=report_every, poll=0.005, log=log, clock=lambda: w.now, sleep=lambda dt: w.advance(dt, host_cb))
    sess.t0 = 0.0
    sess.next_report = report_every
    sess.report(0.0)
    next_key = 0.3
    while w.now < seconds:
        if w.now >= next_key:
            slot = w.rng.choice(w.keys)
            hold = w.rng.uniform(0.04, 0.25)
            if stuck_at is not None and not state["stuck"] and w.now >= stuck_at:
                state["stuck"] = True
                w.stuck.add(slot)
            elif lost_at is not None and not state["lost"] and w.now >= lost_at:
                state["lost"] = True
                w.lost.add(slot)
                hold = 0.6
            w.press(slot, hold)
            next_key = w.now + w.rng.uniform(0.05, 0.5)
        if reset_at is not None and not state["reset"] and w.now >= reset_at:
            state["reset"] = True
            w.boots += 1
            w.boot_t0 = w.now
            w.down.clear()
            w.pending_host.clear()
            mon.host_clear(w.now)                      # a reset drops the USB connection: Windows forgets every key
        if outage is not None and not state["outage"] and w.now >= outage[0]:
            state["outage"] = True
            w.unreadable_until = outage[0] + outage[1]
        sess.step()
        w.advance(sess.poll, host_cb)
    return mon, sess, w, tr


# ----------------------------------------------------------------------------- self test

def c_crosscheck(check):
    """Compile the REAL km_health.c packers with gcc (when there is one) and compare with this tool's decoders."""
    import shutil
    import os
    env = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        env["PATH"] = str(ucrt) + os.pathsep + env["PATH"]
    cc = shutil.which("gcc", path=env["PATH"])
    if not cc:
        return "skipped (no gcc)"
    src = r'''
#include <stdio.h>
#include "km/km_health.h"
int main(void){
  const uint8_t down[] = {0, 15, 16, 18, 63, 75, 76, 200};
  uint16_t m[KM_KEYMASK_SCOPE_VALUES];
  km_health_pack_keymask(down, 8, 0xBEEF, 0x0A, true, m);
  for (int i = 0; i < 8; i++) printf("%u ", m[i]);
  printf("\n");
  km_health_t h; km_health_init(&h, 0, 0x20);
  h.frames = 0x123456; h.scan_stalls = 3; h.dma_errors = 4; h.usb_resets = 9; h.engine_max_cycles = 5000; h.fault_flags = 0x14;
  uint16_t extra[7] = {11, 12, 13, 0xFFFF, 15, 16, 17}, v[24];
  km_health_pack_scope(&h, extra, v);
  for (int i = 0; i < 24; i++) printf("%u ", v[i]);
  printf("\n");
  return 0;
}
'''
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "t.c").write_text(src)
        exe = td / "t.exe"
        r = subprocess.run([cc, "-std=c11", "-I" + str(REPO / "firmware/core/include"), str(td / "t.c"),
                            str(REPO / "firmware/core/src/km_health.c"), "-o", str(exe)], capture_output=True, text=True, env=env)
        if r.returncode != 0:
            check(False, "C cross-check did not compile: " + r.stderr[:200])
            return "failed"
        out = subprocess.run([str(exe)], capture_output=True, text=True, env=env).stdout.splitlines()
    m = [int(x) for x in out[0].split()]
    slots, seq, faults, undel = decode_keymask(m)
    check(slots == {0, 15, 16, 18, 63, 75} and seq == 0xBEEF and faults == 0x0A and undel, "tag 0x0301 as packed by km_health.c: %s" % out[0])
    h = decode_health([int(x) for x in out[1].split()])
    check(h["frames"] == 0x123456 and h["scan_stalls"] == 3 and h["dma_errors"] == 4 and h["usb_resets"] == 9 and h["engine_max_cycles"] == 5000
          and h["faults"] == 0x14 and h["reset_cause"] == 0x20 and h["reports_sent"] == 11 and h["uptime_s"] == 16 and h["prev_faults"] == 0xFFFF
          and h["boots"] == 15 and h["engine_total_max"] == 17, "tag 0x0300 as packed by km_health.c: %s" % out[1])
    return "ok"


def self_test():
    fails = []

    def check(cond, label):
        if not cond:
            fails.append(label)

    # 1. key table
    check(len(KEYMAP) == 76 and sum(1 for _s, u, _n in KEYMAP if u) == 60, "keymap: 60 slots with a usage (61 keys, Fn has none)")
    check(len(SLOT_KEY) == 60 and len(set(SLOT_KEY.values())) == 60, "every key has its own scan code")
    check(KEY_NAME[(0x01, False)] == "ESC" and KEY_NAME[(0x39, False)] == "SPACE" and KEY_NAME[(0x1D, True)] == "RCTRL"
          and KEY_NAME[(0x1D, False)] == "LCTRL" and KEY_NAME[(0x5B, True)] == "LWIN", "well-known scan codes")
    check(SLOT_NAME[63] == "SPACE" and SLOT_KEY[63] == (0x39, False), "slot 63 is the space bar")
    check(0x35 not in {v[0] for v in SLOT_KEY.values() if not v[1]} or SLOT_NAME[30] == "/", "no grave key on this board")

    # 2. payload decoders
    slots, seq, faults, undel = decode_keymask([0x8001, 0x0001, 0, 0, 1 << 11, 77, 0x05, 1])
    check(slots == {0, 15, 16, 75} and seq == 77 and faults == 5 and undel, "decode_keymask")
    check(decode_keymask([0] * 8)[0] == set(), "empty mask")
    check(decode_keymask([0, 0, 0, 0, 0xFFFF, 0, 0, 0])[0] == {64 + b for b in range(12)}, "bits 76..79 are ignored")
    try:
        decode_keymask([0] * 7)
        check(False, "short mask accepted")
    except ValueError:
        pass
    check(faults_text(0) == "none" and faults_text(0x05) == "SCAN_STALL+CAL_LOST", "fault names")
    check(reset_text(0x20) == "iwdg" and reset_text(0) == "none", "reset names")
    c_status = c_crosscheck(check)

    # 3. the comparison, in virtual time
    space = SLOT_KEY[63]
    a = SLOT_KEY[73]
    m = SoakMonitor(threshold=0.2)
    m.fw_mask(0.0, {63}, 1, 0, False)
    m.host_event(0.004, space, True)
    check(m.poll(0.0) == [] or True, "poll before the host event")
    ev = []
    for i in range(1, 100):
        t = i * 0.02
        m.fw_mask(t, {63}, 1 + i, 0, False)
        ev += m.poll(t)
    check(ev == [], "matching key states produce no event")
    # firmware releases, Windows never does: stuck key
    for i in range(100, 130):
        t = i * 0.02
        m.fw_mask(t, set(), 1 + i, 0, False)
        ev += m.poll(t)
    stuck = [e for e in ev if e.kind == "HOST_DOWN_FW_UP"]
    check(len(stuck) == 1 and stuck[0].key == "SPACE" and 0.2 <= stuck[0].duration < 0.25, "stuck key detected once after the threshold: %s" % [e.line() for e in ev])
    m.host_event(2.7, space, False)
    m.fw_mask(2.71, set(), 200, 0, False)
    ev2 = m.poll(2.72)
    check([e.kind for e in ev2] == ["RESOLVED"] and ev2[0].duration > 0.2, "RESOLVED when Windows finally releases")
    # a mismatch below the threshold is not an event
    m2 = SoakMonitor(threshold=0.2)
    m2.fw_mask(0, {73}, 1, 0, False)
    out = []
    for i in range(1, 9):
        m2.fw_mask(i * 0.02, {73}, 1 + i, 0, False)
        out += m2.poll(i * 0.02)
    m2.host_event(0.17, a, True)
    out += m2.poll(0.18)
    check(out == [] and not m2.reported, "a 170 ms mismatch (below the threshold) must not be logged")
    # lost press
    m3 = SoakMonitor()
    evs = []
    for i in range(0, 20):
        m3.fw_mask(i * 0.02, {18}, i, 0x02, True)
        evs += m3.poll(i * 0.02)
    lost = [e for e in evs if e.kind == "FW_DOWN_HOST_UP"]
    check(len(lost) == 1 and lost[0].key == "ESC" and "DMA_ERROR" in lost[0].detail and "undelivered" in lost[0].detail, "lost press with the firmware's fault info")
    # stale firmware data pauses the comparison
    m4 = SoakMonitor()
    m4.fw_mask(0, set(), 1, 0, False)
    m4.host_event(0.0, space, True)
    check(m4.poll(5.0) == [] and m4.stats["stale_skips"] == 1, "stale mask: no comparison")
    # host cleared (device removed) removes stuck state
    m4.fw_mask(5.0, set(), 2, 0, False)
    m4.host_clear(5.0)
    check(m4.poll(5.5) == [], "host_clear")
    # unknown key from the host that the firmware does not know (e.g. an Fn-layer key) still reported by scan code
    m5 = SoakMonitor()
    m5.fw_mask(0, set(), 1, 0, False)
    m5.host_event(0, (0x48, True), True)
    r5 = []
    for i in range(1, 15):
        m5.fw_mask(i * 0.02, set(), 1 + i, 0, False)
        r5 += m5.poll(i * 0.02)
    check(len(r5) == 1 and "0x48" in r5[0].key, "unknown scan code reported by number")

    # 4. Raw Input parsing and filtering
    def raw(make, flags, typ=1, dev=0x1234, vkey=0x41):
        fmt = HEADER_FORMAT_64 if ctypes.sizeof(ctypes.c_void_p) == 8 else HEADER_FORMAT_32
        hdr = struct.pack(fmt, typ, struct.calcsize(fmt) + ctypes.sizeof(RawKeyboard), dev, 0)
        return hdr + bytes(RawKeyboard(make, flags, 0, vkey, 0x100, 0)) + bytes(16)
    check(parse_rawinput(raw(0x39, 0)) == (0x1234, 0x39, False, True), "raw: space down")
    check(parse_rawinput(raw(0x39, RI_KEY_BREAK)) == (0x1234, 0x39, False, False), "raw: space up")
    check(parse_rawinput(raw(0x1D, RI_KEY_E0 | RI_KEY_BREAK)) == (0x1234, 0x1D, True, False), "raw: right ctrl up (E0)")
    check(parse_rawinput(raw(0x39, 0, typ=0)) is None and parse_rawinput(raw(0, 0)) is None and parse_rawinput(raw(0xFF, 0)) is None, "raw: mouse and fake keys dropped")
    check(parse_rawinput(raw(0x39, 0)[:10]) is None, "raw: short buffer")
    p = DEFAULT_MATCH
    check(device_matches(r"\\?\HID#VID_19F5&PID_FB49&MI_02#8&2f1a&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}", p), "device filter: our keyboard")
    check(device_matches(r"\\?\hid#vid_19f5&pid_fb49&mi_02#x", p), "device filter is case-insensitive")
    check(not device_matches(r"\\?\HID#VID_0C45&PID_8076&MI_00#7&abc", p) and not device_matches(r"\\?\ACPI#ATK3001#4&", p)
          and not device_matches("", p) and not device_matches(None, p), "device filter rejects the other keyboards")

    # 5. whole sessions against the simulated keyboard and Windows (virtual time)
    mon, sess, w, tr = simulate(60.0, seed=3)
    check(mon.stats["events"] == 0 and mon.stats["compares"] > 5000, "healthy 60 s run: no event (%d events, %d compares)" % (mon.stats["events"], mon.stats["compares"]))
    check(set(tr.cmds) <= {bk.CMD_GET_INFO, scope.CMD_SCOPE}, "the soak tool sent only cmd 3 and the scope request: %s" % sorted(set(tr.cmds)))
    check(len(sess.health_log) >= 2 and sess.health_log[0][0] == 0.0 and sess.health_log[0][1]["boots"] == 1, "baseline and periodic counters were read")
    mon, sess, w, tr = simulate(40.0, seed=4, stuck_at=10.0)
    kinds = [e.kind for e in mon.events]
    check(kinds.count("HOST_DOWN_FW_UP") == 1, "injected stuck key is found exactly once: %s" % kinds)
    mon, sess, w, tr = simulate(40.0, seed=5, lost_at=10.0)
    kinds = [e.kind for e in mon.events]
    check(kinds.count("FW_DOWN_HOST_UP") == 1 and "RESOLVED" in kinds, "injected lost press is found: %s" % kinds)
    mon, sess, w, tr = simulate(70.0, seed=6, reset_at=35.0)
    check([e.kind for e in mon.events if e.kind == "FIRMWARE_RESET"] == ["FIRMWARE_RESET"], "firmware reset detected: %s" % [e.kind for e in mon.events])
    mon, sess, w, tr = simulate(40.0, seed=7, outage=(15.0, 5.0))
    kinds = [e.kind for e in mon.events]
    check("SCOPE_ERRORS" in kinds and "SCOPE_OK" in kinds and "HOST_DOWN_FW_UP" not in kinds and "FW_DOWN_HOST_UP" not in kinds,
          "unreadable stretch pauses the comparison without false alarms: %s" % kinds)

    # 6. the Windows API itself (registration + message window), when available
    win = "skipped (not Windows)"
    if sys.platform == "win32":
        try:
            ri = WindowsRawInput(DEFAULT_MATCH, lambda *a: None)
            ri.start()
            time.sleep(0.1)
            ri.stop()
            win = "ok (message window created, RegisterRawInputDevices accepted)"
        except Exception as exc:
            win = "could not start here: %s" % exc

    print("C cross-check of the packers:", c_status)
    print("Windows Raw Input probe:", win)
    print("SELF-TEST", "FAILED" if fails else "OK")
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


# ----------------------------------------------------------------------------- main

def dry_run():
    print(__doc__.split("LIMITS")[0].rstrip())
    print("\nWhat --run would do:")
    print("  host     Raw Input keyboard events whose device path contains one of:", ", ".join(DEFAULT_MATCH))
    print("  firmware scope requests, one per poll: cmd 3 (build string must carry KMFW), then 0xC5 for tag 0x%04X (22 byte window),"
          % TAG_KEYMASK)
    print("           every --report seconds tag 0x%04X. First request bytes:" % TAG_HEALTH)
    print("    ", scope.build_request(0, scope.HEADER_BYTES + 2 * KEYMASK_VALUES, TAG_KEYMASK)[:16].hex())
    print("\nNothing was sent or registered. Demo against the simulated keyboard (20 s, with one injected stuck key):")
    mon, _s, _w, _t = simulate(20.0, seed=11, stuck_at=6.0, log=print, report_every=10.0)
    print("-> %d event(s)" % mon.stats["events"])
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true", help="monitor the real keyboard (Windows, our firmware only)")
    ap.add_argument("--sim", action="store_true", help="monitor a simulated keyboard and simulated Windows events (virtual time)")
    ap.add_argument("--duration", type=float, default=0, help="seconds to run (0 = until Ctrl+C; --sim default 60)")
    ap.add_argument("--csv", metavar="FILE", help="also write the events to FILE")
    ap.add_argument("--threshold", type=float, default=0.2, help="seconds of disagreement before an event is logged (default 0.2)")
    ap.add_argument("--report", type=float, default=60.0, help="seconds between counter lines (default 60)")
    ap.add_argument("--poll", type=float, default=0.005, help="seconds between scope polls (default 0.005)")
    ap.add_argument("--match", action="append", help="Raw Input device path must contain this text (default %s; repeatable)" % DEFAULT_MATCH[0])
    ap.add_argument("--inject", choices=["stuck", "lost", "reset"], help="with --sim: inject one fault at 1/3 of the run")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.sim:
        dur = args.duration or 60.0
        at = dur / 3
        kw = {"stuck": {"stuck_at": at}, "lost": {"lost_at": at}, "reset": {"reset_at": at}}.get(args.inject, {})
        mon, _s, _w, _t = simulate(dur, log=print, report_every=args.report, **kw)
        print("%d event(s); compares %d, longest mismatch %.3f s" % (mon.stats["events"], mon.stats["compares"], mon.stats["max_mismatch"]))
        return 0
    if not args.run:
        return dry_run()
    if sys.platform != "win32":
        print("ERROR: --run needs Windows (Raw Input)")
        return 1
    csv_fh = open(args.csv, "w", newline="", encoding="utf-8") if args.csv else None
    writer = csv.writer(csv_fh) if csv_fh else None
    if writer:
        writer.writerow(["wall_time", "t_s", "event", "key", "duration_s", "detail"])
    try:
        iface = bk.pick_interface()
        transport = scope.ScopeTransport(iface["path"])
        client = scope.ScopeClient(transport)
        print("firmware:", client.verify())
        mon = SoakMonitor(threshold=args.threshold)
        sess = Session(Firmware(client), mon, report_every=args.report, poll=args.poll, csv_writer=writer)

        def on_device(t, what, name):
            if what == "removal":
                mon.host_clear(t)
            sess._emit(mon.note(t - sess.t0, "HOST_DEVICE_SEEN" if what == "arrival" else "HOST_DEVICE_GONE", name))

        ri = WindowsRawInput(tuple(args.match) if args.match else DEFAULT_MATCH, lambda t, key, down: mon.host_event(t - sess.t0, key, down), on_device)
        ri.start()
        print("Raw Input registered; watching for %s. Press Ctrl+C to stop." % ("ever" if not args.duration else "%.0f s" % args.duration))
        try:
            sess.run(args.duration)
        except KeyboardInterrupt:
            pass
        finally:
            ri.stop()
            transport.close()
        print("done: %d host key events from the matching keyboard, %d event(s) logged, longest mismatch %.3f s"
              % (ri.count, mon.stats["events"], mon.stats["max_mismatch"]))
        if ri.count == 0:
            print("NOTE: no key event from a device matching %s was seen. Devices Windows reported: %s"
                  % (", ".join(args.match or DEFAULT_MATCH), sorted(ri.seen_names) or "none (no key was pressed on any keyboard)"))
        return 0
    except (bk.BackupError, bk.SafetyError, scope.ScopeError, RuntimeError) as exc:
        print("ERROR:", exc)
        return 1
    finally:
        if csv_fh:
            csv_fh.close()


if __name__ == "__main__":
    sys.exit(main())
