#!/usr/bin/env python3
"""magger60_slot_ranges.py - per-key released / bottomed-out sensor levels from a guided key-press log (M3 step S4).

Input: a scope CSV (tag 2 raw values or tag 3 sixteen-point sums) in which every key was pressed once, and the measured
slot map (firmware/core/data/slotmap_measured.json). For every key it reports the idle level (median of the whole run)
and the level at the deepest point of its own press (the lowest value seen while that key was the one moving). These
are the endpoints that the depth model (PUBLISH_PLAN.md R2) is fitted between.

    python tools/magger60_slot_ranges.py keys.csv --json out.json [--sums]
    python tools/magger60_slot_ranges.py --self-test
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import magger60_keymap_measure as km   # noqa: E402

MAP = Path(__file__).resolve().parent.parent / "firmware/core/data/slotmap_measured.json"


def ranges(rows, slot_to_key):
    idle = [statistics.median(r[1][s] for r in rows) for s in range(76)]
    low = list(idle)
    for _t, vals in rows:
        for s in range(76):
            if vals[s] < low[s]:
                low[s] = vals[s]
    out = {}
    for s_str, key in slot_to_key.items():
        s = int(s_str)
        out[s_str] = {"key": key, "released": round(idle[s]), "bottom": low[s], "span": round(idle[s]) - low[s]}
    return out


def self_test():
    rows = [(0.0, [3000] * 76), (0.1, [3000] * 76), (0.2, [3000] * 3 + [1900] + [3000] * 72), (0.3, [3000] * 76), (0.4, [3000] * 76)]
    r = ranges(rows, {"3": "X"})
    ok = r["3"] == {"key": "X", "released": 3000, "bottom": 1900, "span": 1100}
    print("SELF-TEST", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?")
    ap.add_argument("--json")
    ap.add_argument("--sums", action="store_true", help="the CSV holds tag 3 sixteen-point sums: divide by 16")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.csv:
        ap.error("a CSV file is required")
    rows = km.load(args.csv)
    if args.sums:
        rows = [(t, [v // 16 for v in vals]) for t, vals in rows]
    slot_to_key = json.loads(MAP.read_text(encoding="utf-8"))["slots"]
    res = ranges(rows, slot_to_key)
    spans = sorted(v["span"] for v in res.values())
    print("%d keys; span released -> bottom: min %d, median %d, max %d (ADC counts)" % (len(res), spans[0], spans[len(spans) // 2], spans[-1]))
    if args.json:
        Path(args.json).write_text(json.dumps({"version": 1, "unit": "ADC counts, 12 bit",
                                               "source": "guided one-press-per-key log", "keys": res}, indent=1), encoding="utf-8")
        print("wrote", args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
