#!/usr/bin/env python3
"""magger60_compare_backups.py - compare two backup folders (tools/magger60_backup.py output) sector by sector.

Use it after a flash that is allowed to change only OUR settings sectors (the M5 settings store): it checks that

  * the bootloader copy (0x08000000-0x08007FFF) is byte-identical,
  * the settings area (0x08028000-0x0803FFFF) differs only in the sectors the firmware may write:
        0x08030000 and 0x08032000 (settings store A / B), 0x08034000 (calibration sector), 0x08036000 (light configuration),
        0x08038000 and 0x0803A000 (light scene A / B),
        and in sector 0x0802C000 only bit 0 of its first byte (the enter-bootloader flag, which the bootloader clears again),
  * the app-ID block (sector 0x08028000) is unchanged.

Read only; nothing is sent to the keyboard.

    python tools/magger60_compare_backups.py OLD_DIR NEW_DIR
    python tools/magger60_compare_backups.py --self-test
Exit code 0 = only allowed differences, 1 = unexpected differences (or unreadable input).
"""
import argparse
import sys
import tempfile
from pathlib import Path

SECTOR = 0x2000
BOOT_BASE = 0x08000000
SET_BASE = 0x08028000
ALLOWED_SECTORS = {0x08030000: "settings store A", 0x08032000: "settings store B", 0x08034000: "calibration",
                   0x08036000: "light configuration", 0x08038000: "light scene A", 0x0803A000: "light scene B"}
FLAG_SECTOR = 0x0802C000
BOOT_FILE = "bootloader_0x08000000.bin"
SET_FILE = "settings_0x08028000.bin"


def sectors(data, base):
    return {base + off: data[off:off + SECTOR] for off in range(0, len(data), SECTOR)}


def compare(old_boot, new_boot, old_set, new_set):
    """Return (lines, ok)."""
    lines, ok = [], True
    if old_boot == new_boot:
        lines.append("bootloader (32 KB incl. its status sector): identical")
    else:
        ok = False
        bad = [BOOT_BASE + o for o in range(0, min(len(old_boot), len(new_boot)), SECTOR)
               if old_boot[o:o + SECTOR] != new_boot[o:o + SECTOR]]
        lines.append("bootloader: DIFFERENT in sector(s) " + ", ".join("0x%08X" % a for a in bad) + "  <-- UNEXPECTED")
    if len(old_set) != len(new_set):
        return lines + ["settings area: different sizes (%d vs %d)  <-- UNEXPECTED" % (len(old_set), len(new_set))], False
    a, b = sectors(old_set, SET_BASE), sectors(new_set, SET_BASE)
    changed = 0
    for addr in sorted(a):
        if a[addr] == b[addr]:
            continue
        changed += 1
        if addr in ALLOWED_SECTORS:
            lines.append("sector 0x%08X (%s): changed (allowed)" % (addr, ALLOWED_SECTORS[addr]))
        elif addr == FLAG_SECTOR:
            rest_same = a[addr][1:] == b[addr][1:] and (a[addr][0] & 0xFE) == (b[addr][0] & 0xFE)
            if rest_same:
                lines.append("sector 0x%08X (profile / boot flag): only bit 0 of byte 0 differs (allowed)" % addr)
            else:
                ok = False
                lines.append("sector 0x%08X (profile): changed beyond the boot flag  <-- UNEXPECTED" % addr)
        else:
            ok = False
            what = "app-ID block" if addr == SET_BASE else "stock settings sector"
            lines.append("sector 0x%08X (%s): CHANGED  <-- UNEXPECTED" % (addr, what))
    if changed == 0:
        lines.append("settings area: identical")
    return lines, ok


def load(folder):
    f = Path(folder)
    return (f / BOOT_FILE).read_bytes(), (f / SET_FILE).read_bytes()


def self_test():
    boot = bytes(range(256)) * 128                       # 32 KB
    setg = bytearray(b"\xAA" * 0x18000)                  # 96 KB
    new = bytearray(setg)
    cases = []

    def run(name, nb, ns, want_ok):
        _, ok = compare(boot, nb, bytes(setg), bytes(ns))
        cases.append((name, ok == want_ok))

    run("identical", boot, setg, True)
    n = bytearray(new); n[0x8000] ^= 0xFF; run("store A changed", boot, n, True)
    n = bytearray(new); n[0xA000 + 5] ^= 0x01; run("store B changed", boot, n, True)
    n = bytearray(new); n[0xC010] ^= 0x01; run("calibration changed", boot, n, True)
    n = bytearray(new); n[0x4000] ^= 0x01; run("boot flag bit 0 only", boot, n, True)
    n = bytearray(new); n[0x4000] ^= 0x02; run("flag sector byte 0 bit 1", boot, n, False)
    n = bytearray(new); n[0x4001] ^= 0x01; run("flag sector other byte", boot, n, False)
    n = bytearray(new); n[0x0000] ^= 0x01; run("app-ID block changed", boot, n, False)
    n = bytearray(new); n[0x2000] ^= 0x01; run("stock sector 0x0802A000 changed", boot, n, False)
    nb = bytearray(boot); nb[100] ^= 1; run("bootloader changed", bytes(nb), new, False)
    run("settings size differs", boot, new[:-SECTOR], False)
    with tempfile.TemporaryDirectory() as td:                       # the folder loader
        for sub in ("a", "b"):
            (Path(td) / sub).mkdir()
            (Path(td) / sub / BOOT_FILE).write_bytes(boot)
            (Path(td) / sub / SET_FILE).write_bytes(bytes(setg))
        ob, os_ = load(Path(td) / "a")
        nb2, ns2 = load(Path(td) / "b")
        cases.append(("folder load", compare(ob, nb2, os_, ns2)[1]))
    bad = [n for n, good in cases if not good]
    for n, good in cases:
        print("  %-34s %s" % (n, "ok" if good else "FAILED"))
    print("SELF-TEST", "OK" if not bad else "FAILED")
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old", nargs="?")
    ap.add_argument("new", nargs="?")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.old or not args.new:
        ap.error("two backup folders are required")
    try:
        ob, os_ = load(args.old)
        nb, ns = load(args.new)
    except OSError as e:
        print("cannot read the backups:", e)
        return 1
    lines, ok = compare(ob, nb, os_, ns)
    print("\n".join(lines))
    print("RESULT:", "only allowed differences" if ok else "UNEXPECTED differences")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
