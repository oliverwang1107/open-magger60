#!/usr/bin/env python3
"""Show what Windows' XInput API reports for the Magger60 in Xbox-compatible mode (pad_mode = 2, M6 P11).

    python tools/magger60_xinputtest.py            # live view of the four XInput slots (Ctrl+C stops)
    python tools/magger60_xinputtest.py --once     # one snapshot, then exit (0 = a controller answered, 2 = none)
    python tools/magger60_xinputtest.py --self-test

Windows only (XInput1_4.dll through ctypes, no packages). This reads what a game sees; it cannot tell WHICH controller a slot is, so
unplug other pads first. It only reads: nothing is sent to the keyboard except the rumble call of --rumble (a short pulse on slot
`--slot`, which the keyboard records but cannot feel - there is no motor).
"""
import argparse
import ctypes
import sys
import time

ERROR_SUCCESS = 0
ERROR_DEVICE_NOT_CONNECTED = 1167

BUTTON_BITS = [(0x0001, "DUp"), (0x0002, "DDown"), (0x0004, "DLeft"), (0x0008, "DRight"), (0x0010, "Start"), (0x0020, "Back"),
               (0x0040, "L3"), (0x0080, "R3"), (0x0100, "LB"), (0x0200, "RB"), (0x1000, "A"), (0x2000, "B"), (0x4000, "X"),
               (0x8000, "Y")]


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte), ("bRightTrigger", ctypes.c_ubyte),
                ("sThumbLX", ctypes.c_short), ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short), ("sThumbRY", ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [("dwPacketNumber", ctypes.c_uint32), ("Gamepad", XINPUT_GAMEPAD)]


class XINPUT_VIBRATION(ctypes.Structure):
    _fields_ = [("wLeftMotorSpeed", ctypes.c_ushort), ("wRightMotorSpeed", ctypes.c_ushort)]


def describe(state):
    g = state.Gamepad
    names = [n for bit, n in BUTTON_BITS if g.wButtons & bit]
    return (f"pkt {state.dwPacketNumber:6d}  LX {g.sThumbLX:6d} LY {g.sThumbLY:6d}  RX {g.sThumbRX:6d} RY {g.sThumbRY:6d}  "
            f"LT {g.bLeftTrigger:3d} RT {g.bRightTrigger:3d}  [{' '.join(names)}]")


def load():
    if sys.platform != "win32":
        raise RuntimeError("XInput exists on Windows only")
    for dll in ("XInput1_4.dll", "XInput1_3.dll", "XInput9_1_0.dll"):
        try:
            return ctypes.WinDLL(dll)
        except OSError:
            continue
    raise RuntimeError("no XInput DLL found")


def self_test():
    assert ctypes.sizeof(XINPUT_GAMEPAD) == 12, "XINPUT_GAMEPAD is 12 bytes in the public header"
    assert ctypes.sizeof(XINPUT_STATE) == 16
    st = XINPUT_STATE(7, XINPUT_GAMEPAD(0x1001, 255, 0, -5, 6, 0, 0))
    out = describe(st)
    assert "DUp" in out and "A" in out and "LT 255" in out and "LX     -5" in out, out
    print("self-test ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--rumble", action="store_true", help="send a 0.5 s rumble pulse to --slot first")
    ap.add_argument("--slot", type=int, default=0)
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        x = load()
    except RuntimeError as exc:
        print("ERROR:", exc)
        return 1
    if args.rumble:
        vib = XINPUT_VIBRATION(40000, 40000)
        x.XInputSetState(args.slot, ctypes.byref(vib))
        time.sleep(0.5)
        x.XInputSetState(args.slot, ctypes.byref(XINPUT_VIBRATION(0, 0)))
    last = None
    try:
        while True:
            lines = []
            found = 0
            for slot in range(4):
                st = XINPUT_STATE()
                rc = x.XInputGetState(slot, ctypes.byref(st))
                if rc == ERROR_SUCCESS:
                    found += 1
                    lines.append(f"slot {slot}: {describe(st)}")
                else:
                    lines.append(f"slot {slot}: " + ("not connected" if rc == ERROR_DEVICE_NOT_CONNECTED else f"error {rc}"))
            if args.once:
                print("\n".join(lines))
                if not found:
                    print("No XInput controller answered. In Device Manager look for a yellow mark on the Magger60 'Xbox' / unknown device,\n"
                          "and check that the keyboard really holds pad mode 2 (gamepad page: Xbox-compatible, Save, replug).")
                return 0 if found else 2
            text = "\n".join(lines)
            if text != last:
                print("\x1b[2J\x1b[H" + text, flush=True)
                last = text
            time.sleep(0.01)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
