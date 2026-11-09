#!/usr/bin/env python3
"""magger60_keymap_measure.py - measure which sensor slot belongs to which physical key (PUBLISH_PLAN.md R1).

Input: a CSV written by `magger60_scope.py --run --csv FILE` while the S2/S3 firmware was publishing the 76 slot
values (tag 2 or 3) and the user pressed every key once, in a known order. Nothing here comes from the vendor: the
table is *measured* from the user's own keyboard.

For every slot the idle level is the median over the whole run (a key is down for well under half of the time). A
press is a group of snapshots in which at least one slot is more than THRESHOLD below its idle level; the slot with
the deepest drop is the key that was pressed, neighbours that moved only a little are reported as crosstalk.

    python tools/magger60_keymap_measure.py keys.csv                  # report, uses the default press order below
    python tools/magger60_keymap_measure.py keys.csv --json out.json  # also write the measured slot -> key table
    python tools/magger60_keymap_measure.py --self-test
"""
import argparse
import csv
import json
import statistics
import sys

# Press order used by the guided measurement: ANSI 60 %, left to right, top to bottom (61 keys).
ORDER = (
    "ESC 1 2 3 4 5 6 7 8 9 0 - = BACKSPACE "
    "TAB Q W E R T Y U I O P [ ] \\ "
    "CAPS A S D F G H J K L ; ' ENTER "
    "LSHIFT Z X C V B N M , . / RSHIFT "
    "LCTRL LWIN LALT SPACE RALT RWIN FN RCTRL"
).split()
ROW_LENGTHS = (14, 14, 13, 12, 8)
THRESHOLD = 250          # raw ADC counts below the idle level (a full press moves ~1000)
MIN_SAMPLES = 3          # a press must last at least this many snapshots
GAP_S = 0.25             # quiet time that separates two presses


def order_with_reversed_rows(rows_reversed):
    """ORDER, but the keys of the given rows (1-based) were pressed right to left."""
    out, pos = [], 0
    for n, length in enumerate(ROW_LENGTHS, 1):
        row = ORDER[pos:pos + length]
        out.extend(reversed(row) if n in rows_reversed else row)
        pos += length
    return out


def load(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if r["tag"] in ("2", "3"):
                rows.append((float(r["t_s"]), [int(r["s%d" % i]) for i in range(76)]))
    return rows


def find_presses(rows, threshold=THRESHOLD, min_samples=MIN_SAMPLES, gap_s=GAP_S):
    idle = [statistics.median(r[1][s] for r in rows) for s in range(76)]
    active = []                                    # (t, drops)
    for t, vals in rows:
        drops = [idle[s] - vals[s] for s in range(76)]
        active.append((t, drops, max(drops) > threshold))
    presses, cur, last_t = [], [], None
    for t, drops, on in active:
        if on:
            if cur and t - last_t > gap_s:
                presses.append(cur)
                cur = []
            cur.append((t, drops))
            last_t = t
    if cur:
        presses.append(cur)
    out = []
    for p in presses:
        if len(p) < min_samples:
            continue
        peak = [max(d[s] for _, d in p) for s in range(76)]
        best = max(range(76), key=lambda s: peak[s])
        side = [(s, peak[s]) for s in range(76) if s != best and peak[s] > threshold // 2]
        out.append({"t": p[0][0], "slot": best, "peak": peak[best], "samples": len(p), "crosstalk": side})
    return idle, out


def report(idle, presses, order=ORDER):
    print("%d presses found, %d expected" % (len(presses), len(order)))
    used, table = {}, {}
    print("\n #  expected key  slot   drop  samples  notes")
    for i, p in enumerate(presses):
        name = order[i] if i < len(order) else "?"
        notes = []
        if p["slot"] in used:
            notes.append("slot already used by %s" % used[p["slot"]])
        if p["crosstalk"]:
            notes.append("also moved: " + ", ".join("s%d(-%d)" % c for c in p["crosstalk"]))
        used.setdefault(p["slot"], name)
        table[p["slot"]] = name
        print("%2d  %-12s  %4d  %5d  %7d  %s" % (i + 1, name, p["slot"], p["peak"], p["samples"], "; ".join(notes)))
    never = [s for s in range(76) if s not in table]
    print("\nslots without a key press: %s" % never)
    print("idle level of those slots: %s" % [round(idle[s]) for s in never])
    return table


def self_test():
    import random
    random.seed(1)
    rows, t = [], 0.0
    for key in range(61):
        slot = (key * 5) % 76
        for k in range(30):                              # idle, then a press of ~10 snapshots
            vals = [3300 + random.randint(-15, 15) for _ in range(76)]
            if 10 <= k < 20:
                vals[slot] -= 900
                vals[(slot + 1) % 76] -= 60
            rows.append((t, vals))
            t += 0.02
        t += 0.6
    idle, presses = find_presses(rows)
    ok = len(presses) == 61 and all(p["slot"] == (i * 5) % 76 for i, p in enumerate(presses)) and len(ORDER) == 61
    print("SELF-TEST", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?")
    ap.add_argument("--json")
    ap.add_argument("--reverse-rows", default="", help="rows (1-5, comma separated) that were pressed right to left")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.csv:
        ap.error("a CSV file is required")
    rows = load(args.csv)
    if not rows:
        print("no tag 2/3 snapshots in the file")
        return 1
    reversed_rows = {int(x) for x in args.reverse_rows.split(",") if x.strip()}
    order = order_with_reversed_rows(reversed_rows)
    idle, presses = find_presses(rows)
    table = report(idle, presses, order)
    if args.json:
        no_key = {str(s): {"idle": round(idle[s]), "kind": "mirror of the ADC4 channel sampled three times" if idle[s] > 2500 else "no sensor (floating input)"}
                  for s in range(76) if s not in table}
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"version": 1, "source": "measured on the real keyboard, one press per key",
                       "rows_pressed_right_to_left": sorted(reversed_rows),
                       "slots": {str(s): k for s, k in sorted(table.items())}, "no_key_slots": no_key}, fh, indent=1)
        print("wrote", args.json)
    return 0 if len(presses) == len(order) else 2


if __name__ == "__main__":
    sys.exit(main())
