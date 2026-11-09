#!/usr/bin/env python3
"""magger60_enterboot.py - ask a running Magger60 to restart into its bootloader (update mode, 19F5:AF1F).

Sends the ONE packet that makes a running keyboard enter its update mode (BOOTLOADER_ANALYSIS.md): a raw '_' packet,
cmd 6, which makes the firmware set the boot flag at 0x0802C000 and reset. The stock firmware and our own images
(M2-D4 and later) all react to it. Do NOT send anything else with a '_' header: the stock firmware executes raw
packets without checks.

    python tools/magger60_enterboot.py                                   # dry run: show the packet
    python tools/magger60_enterboot.py --run --confirm ENTER-BOOT        # send it and wait for 19F5:AF1F
    python tools/magger60_enterboot.py --self-test
"""
import argparse
import importlib.util
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("magger60_backup", HERE / "magger60_backup.py")
bk = importlib.util.module_from_spec(_spec)
sys.modules["magger60_backup"] = bk
_spec.loader.exec_module(bk)

# '_' 06 00 52 01 00 00 00 51 + zeros: header, cmd 6, xor 0, checksum (ignored for raw packets), len 1, offset 0, [7], data 0x51.
PACKET = bytes.fromhex("5f06005201000000" "51") + bytes(55)
CONFIRM = "ENTER-BOOT"
BOOT_PID = 0xAF1F


def self_test() -> int:
    ok = len(PACKET) == 64 and PACKET[:9].hex() == "5f0600520100000051" and not any(PACKET[9:])
    # the firmware library (firmware/tests/test_hid.c) asserts the same nine bytes
    print("SELF-TEST", "OK" if ok else "FAILED")
    return 0 if ok else 1




def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--confirm", default="")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    print("packet (first 16 bytes):", PACKET[:16].hex())
    if not args.run:
        print("Dry run: nothing was sent. Add --run --confirm %s to send it." % CONFIRM)
        return 0
    if args.confirm != CONFIRM:
        print("STOPPED: --confirm %s is required for --run" % CONFIRM)
        return 1
    try:
        iface = bk.pick_interface()
        import hid  # type: ignore
        if hasattr(hid, "device"):
            dev = hid.device()
            dev.open_path(iface["path"])
        else:
            dev = hid.Device(path=iface["path"])
        try:
            dev.write(b"\x00" + PACKET)
        finally:
            dev.close()
        print("sent; waiting for the bootloader (19F5:AF1F) ...")
        for _ in range(40):
            time.sleep(0.25)
            if any(i["pid"] == BOOT_PID for i in bk.find_interfaces()):
                print("the keyboard is in update mode (AF1F)")
                return 0
        print("no AF1F device after 10 s: unplug, hold ESC + left CTRL and plug in again")
        return 1
    except bk.BackupError as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
