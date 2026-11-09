#!/usr/bin/env python3
"""Fit and check the Magger60 reading -> depth model (firmware/core/src/km_depth_model.c).

The firmware turns a Hall reading into a key depth with ONE shape parameter (see the derivation at
the top of km_depth_model.c):

    depth = T * n * (1 + a) / (1 + a * n),     n = (released - reading) / (released - bottom)

with T = 3.40 mm, and a stored as A = a * 64 (KM_DEPTH_CURVE_A_Q6 in km_depth_model.h). The default
A = 128 (a = 2.0) is a physical prior; nothing between the two measured endpoints has ever been
measured. This tool fits `a` from a few feeler-gauge readings, validates a CSV, and shows what a
given curve does. Only the Python standard library is needed.

    python tools/magger60_depth_check.py selftest            # built-in tests with synthetic data
    python tools/magger60_depth_check.py template > gauge.csv  # empty log with the format explained
    python tools/magger60_depth_check.py fit gauge.csv       # fit a, residuals, advice
    python tools/magger60_depth_check.py curve --slot 18      # reading -> depth table for one key
    python tools/magger60_depth_check.py study               # how well the rational curve follows
                                                              # the exact magnet-physics curve

---- Feeler-gauge procedure (what the owner has to do, about 20 minutes) ------------------------
Pick 2-3 keys (a letter key, a big key is not needed). With the keyboard running the scope
firmware (km_s3 or later, `tools/magger60_scope.py` shows the live readings of all slots), press
the key down so that the stem is held at a known depth and log the reading. Depth = how far the
key is below its rest position, 0 .. 3.40 mm. One practical way is a gauge of thickness g placed
between the keycap (or stem top) and the switch housing / bottom-out stop, so that the key is
stopped g short of bottom: depth = 3.40 - g. Use g = 0.5, 1.0, 1.5, 2.0, 2.5 mm (depth 2.9, 2.4,
1.9, 1.4, 0.9), press gently until the gauge is clamped, hold still, log 3-5 readings per gauge
(they are median-ed). The CSV below takes either the gauge gap or the depth, whichever is easier.
Caveat: the fit assumes the nominal travel 3.40 mm for every key; a key with a different real
travel shows up as a bias in the residuals of ALL its points, not as a curve-shape change.

---- CSV format (one reading per row) ----------------------------------------------------------
    # comment lines start with #
    slot,gap_mm,reading          <- header; "gap_mm" or "depth_mm", plus optional "key" column
    18,0.5,2231
    18,0.5,2229
    ...
slot      sensor slot number (core/data/slotmap_measured.json)
gap_mm    thickness of the gauge = distance still missing to bottom (depth = 3.40 - gap_mm)
depth_mm  alternatively the depth itself
reading   ADC counts / 16, the value the scope channel shows (smaller = deeper)
Rows at depth 0 (gap 3.40) and at depth 3.40 (gap 0) are treated as fresh released / bottom
readings of that slot and replace the values from core/data/slot_ranges_s2.json; other rows are
the points of the fit. A key needs at least 3 distinct depths between 0.2 and 3.2 mm for the fit
to say anything (5 are better); the more keys, the better the shared `a` is determined.
"""
import argparse
import csv
import json
import math
import random
import statistics
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RANGES_JSON = ROOT / "firmware/core/data/slot_ranges_s2.json"

TRAVEL_MM = 3.40
TRAVEL_MM100 = 340          # KM_TRAVEL_MM100
A_DEFAULT_Q6 = 128          # KM_DEPTH_CURVE_A_Q6
A_MAX_Q6 = 1024             # KM_DEPTH_CURVE_A_Q6_MAX
MIN_SPAN = 64               # KM_DEPTH_MIN_SPAN
MAX_SPAN = 4095             # KM_DEPTH_MAX_SPAN


# ---- the model, bit-exact with km_depth_model_curve_mm100() -------------------------------------

def depth_mm100(released, bottom, value, a_q6=A_DEFAULT_Q6):
    """Integer model, the same arithmetic as the C code (0.01 mm)."""
    if released <= bottom:
        return 0
    span = released - bottom
    if span < MIN_SPAN or span > MAX_SPAN:
        return 0
    a = min(a_q6, A_MAX_Q6)
    v = min(max(value, bottom), released)
    num = released - v
    den = (span << 6) + a * num
    top = TRAVEL_MM100 * (64 + a) * num
    return (top + (den >> 1)) // den


def depth_mm_float(n, a):
    """The ideal curve in mm for a normalised press n in 0..1 and curvature a."""
    return TRAVEL_MM * n * (1.0 + a) / (1.0 + a * n)


def depth_mm_physical(n, p, s_t):
    """The exact magnet-physics curve (power law B ~ r^-p, press closes the fraction s_t of the gap)."""
    k = (1.0 - s_t) ** (-p) - 1.0
    return TRAVEL_MM * (1.0 - (1.0 + n * k) ** (-1.0 / p)) / s_t


def normalised(released, bottom, value):
    return (released - value) / float(released - bottom)


# ---- data ---------------------------------------------------------------------------------------

def load_ranges(path=RANGES_JSON):
    """{slot: (released, bottom, key name)} from our own measurement."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    return {int(s): (int(k["released"]), int(k["bottom"]), k.get("key", "")) for s, k in doc["keys"].items()}


class FormatError(Exception):
    pass


def parse_gauge_csv(text):
    """Parse a gauge log. Returns [(slot, depth_mm, reading)] in file order."""
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    if not lines:
        raise FormatError("no data rows")
    rows = list(csv.reader(lines))
    header = [h.strip().lower() for h in rows[0]]
    for need in ("slot", "reading"):
        if need not in header:
            raise FormatError("header needs a '%s' column (found: %s)" % (need, ",".join(header)))
    if ("gap_mm" in header) == ("depth_mm" in header):
        raise FormatError("header needs exactly one of gap_mm / depth_mm")
    i_slot, i_read = header.index("slot"), header.index("reading")
    by_gap = "gap_mm" in header
    i_pos = header.index("gap_mm" if by_gap else "depth_mm")
    out = []
    for n, row in enumerate(rows[1:], start=2):
        if len(row) < len(header) - (1 if "key" in header and header.index("key") == len(header) - 1 else 0):
            raise FormatError("row %d: too few columns" % n)
        try:
            slot = int(row[i_slot])
            pos = float(row[i_pos])
            reading = float(row[i_read])
        except ValueError:
            raise FormatError("row %d: not a number: %s" % (n, ",".join(row)))
        depth = TRAVEL_MM - pos if by_gap else pos
        if not (-0.05 <= depth <= TRAVEL_MM + 0.05):
            raise FormatError("row %d: depth %.2f mm is outside 0..3.40 (is a gauge thicker than the travel?)" % (n, depth))
        if not (0 <= reading <= 4095 + 100):
            raise FormatError("row %d: reading %s is not a 12-bit ADC count / 16 value" % (n, row[i_read]))
        if not (0 <= slot < 76):
            raise FormatError("row %d: slot %d is not in 0..75" % (n, slot))
        out.append((slot, depth, reading))
    return out


def build_points(rows, ranges):
    """Group the rows by slot. Returns ({slot: (released, bottom)}, [(slot, depth_mm, median reading)], notes)."""
    notes = []
    cal = {}
    pts = {}
    ends = {}
    for slot, depth, reading in rows:
        if depth <= 0.02:
            ends.setdefault(slot, {}).setdefault("released", []).append(reading)
        elif depth >= TRAVEL_MM - 0.02:
            ends.setdefault(slot, {}).setdefault("bottom", []).append(reading)
        else:
            pts.setdefault((slot, round(depth, 3)), []).append(reading)
    for slot in sorted({s for s, _ in pts} | set(ends)):
        if slot not in ranges and not ("released" in ends.get(slot, {}) and "bottom" in ends.get(slot, {})):
            raise FormatError("slot %d is not in slot_ranges_s2.json and the CSV has no depth-0 / depth-3.40 rows for it" % slot)
        rel, bot, _ = ranges.get(slot, (None, None, ""))
        e = ends.get(slot, {})
        if "released" in e:
            rel = int(round(statistics.median(e["released"])))
            notes.append("slot %d: released = %d from the CSV (JSON said %s)" % (slot, rel, ranges.get(slot, (None,))[0]))
        if "bottom" in e:
            bot = int(round(statistics.median(e["bottom"])))
            notes.append("slot %d: bottom = %d from the CSV (JSON said %s)" % (slot, bot, ranges.get(slot, (None, None))[1]))
        cal[slot] = (rel, bot)
    points = []
    for (slot, depth), vals in sorted(pts.items()):
        points.append((slot, depth, statistics.median(vals), len(vals)))
    return cal, points, notes


# ---- fitting ------------------------------------------------------------------------------------

def fit_a(samples):
    """samples: [(n, depth_mm)]. Returns (best A_q6, rms_mm at best, rms_mm linear (A=0)); least squares over every
    integer A_q6 in 0..1024 (the 1/64 resolution of the firmware, there is nothing finer to choose)."""
    def rms(a_q6):
        a = a_q6 / 64.0
        return math.sqrt(sum((depth_mm_float(n, a) - d) ** 2 for n, d in samples) / len(samples))
    best = min(range(A_MAX_Q6 + 1), key=rms)
    return best, rms(best), rms(0)


def analyse(rows, ranges, out=print):
    """Fit and report. Returns the fitted A_q6 or None when the data cannot support a fit."""
    cal, points, notes = build_points(rows, ranges)
    for n in notes:
        out("note: " + n)
    depths = sorted({d for _, d, _, _ in points})
    if len(depths) < 3:
        out("ERROR: need at least 3 distinct depths between 0.02 and 3.38 mm, found %d" % len(depths))
        return None
    samples = []
    out("%5s %-10s %7s %8s %8s %7s %7s" % ("slot", "key", "depth", "reading", "n", "model", "resid"))
    for slot, depth, reading, cnt in points:
        rel, bot = cal[slot]
        n = normalised(rel, bot, reading)
        samples.append((n, depth))
    a_q6, rms_best, rms_lin = fit_a(samples)
    bad_order = 0
    for slot in sorted({s for s, *_ in points}):
        series = [(d, r) for s, d, r, _ in points if s == slot]
        for (d1, r1), (d2, r2) in zip(series, series[1:]):
            if r2 > r1 + 15:
                bad_order += 1
                out("warning: slot %d reads higher at %.2f mm than at %.2f mm (%.0f vs %.0f); a stuck gauge or wrong slot?" % (slot, d2, d1, r2, r1))
    for (slot, depth, reading, cnt), (n, _) in zip(points, samples):
        rel, bot = cal[slot]
        model = depth_mm100(rel, bot, int(round(reading)), a_q6) / 100.0
        out("%5d %-10s %7.2f %8.0f %8.3f %7.2f %+7.2f" % (slot, ranges.get(slot, (0, 0, ""))[2][:10], depth, reading, n, model, model - depth))
        if reading > rel + 15 or reading < bot - 15:
            out("warning: slot %d reading %.0f lies outside released %d .. bottom %d; calibration stale?" % (slot, reading, rel, bot))
    out("")
    out("%d points, %d keys, %d distinct depths" % (len(points), len({s for s, *_ in points}), len(depths)))
    out("best a = %.4f  (A_q6 = %d)   rms residual %.3f mm   [straight line a = 0: %.3f mm; current default A_q6 = %d: %.3f mm]" % (
        a_q6 / 64.0, a_q6, rms_best, rms_lin, A_DEFAULT_Q6, fit_a_rms(samples, A_DEFAULT_Q6)))
    if a_q6 in (0, A_MAX_Q6):
        out("warning: the fit hit the end of its range; the true curve is not of this family (or the data is wrong).")
    if rms_best > 0.10:
        out("warning: residual above 0.10 mm: the curve family does not follow these points (off-axis sensor? wrong gauge convention? "
            "one bad key?). Check the per-point residuals before trusting the number.")
    out("")
    out("To use it: set KM_DEPTH_CURVE_A_Q6 to %d in firmware/core/include/km/km_depth_model.h" % a_q6)
    return a_q6


def fit_a_rms(samples, a_q6):
    a = a_q6 / 64.0
    return math.sqrt(sum((depth_mm_float(n, a) - d) ** 2 for n, d in samples) / len(samples))


# ---- study: rational curve against the exact physics curve --------------------------------------

def study(out=print):
    """Max error of the best one-parameter rational curve against the exact power-law curve."""
    worst = 0.0
    out("%5s %5s   %8s %9s   %9s" % ("p", "sT", "best a", "max err", "d(n=.5)"))
    ns = [i / 400.0 for i in range(401)]
    for p in (1.5, 2.0, 2.5, 3.0, 4.0):
        for s_t in (0.25, 0.35, 0.5, 0.6, 0.7):
            exact = [depth_mm_physical(n, p, s_t) for n in ns]
            best = None
            for a_q6 in range(0, A_MAX_Q6 + 1):
                a = a_q6 / 64.0
                err = max(abs(depth_mm_float(n, a) - e) for n, e in zip(ns[::8], exact[::8]))   # coarse scan
                if best is None or err < best[0]:
                    best = (err, a_q6)
            a = best[1] / 64.0
            err = max(abs(depth_mm_float(n, a) - e) for n, e in zip(ns, exact))
            out("%5.1f %5.2f   %8.2f %7.3f mm   %7.2f mm" % (p, s_t, a, err, exact[200]))
            if p <= 3.0 and s_t <= 0.6:
                worst = max(worst, err)
    out("worst error for p 1.5..3, sT 0.25..0.6: %.3f mm" % worst)
    return worst


def curve_table(released, bottom, a_q6, out=print, step=None):
    span = released - bottom
    step = step or max(1, span // 20)
    out("released %d, bottom %d, span %d, A_q6 %d (a = %.3f); one count near rest = %.4f mm, near bottom = %.4f mm" % (
        released, bottom, span, a_q6, a_q6 / 64.0, TRAVEL_MM * (1 + a_q6 / 64.0) / span, TRAVEL_MM / (1 + a_q6 / 64.0) / span))
    out("%8s %7s %9s" % ("reading", "n", "depth mm"))
    v = released
    while v >= bottom:
        out("%8d %7.3f %9.2f" % (v, normalised(released, bottom, v), depth_mm100(released, bottom, v, a_q6) / 100.0))
        v -= step
    out("%8d %7.3f %9.2f" % (bottom, 1.0, depth_mm100(released, bottom, bottom, a_q6) / 100.0))


TEMPLATE = """# Magger60 feeler-gauge log for tools/magger60_depth_check.py (format: see the docstring of that script).
# gap_mm = thickness of the gauge that stops the key short of bottom; depth = 3.40 - gap_mm.
# Optional: rows with gap_mm 3.40 (key at rest) and 0 (pressed to bottom) refresh released / bottom of that slot.
# Replace the example rows (slot 18 = ESC) by your readings: 3-5 rows per gauge.
slot,gap_mm,reading
# 18,3.40,<reading at rest>
# 18,2.5,<reading>
# 18,2.0,<reading>
# 18,1.5,<reading>
# 18,1.0,<reading>
# 18,0.5,<reading>
# 18,0,<reading pressed all the way>
"""


# ---- self test ----------------------------------------------------------------------------------

def synth_csv(ranges, slots, a_true=None, physical=None, noise=3, rng=None, gaps=(0.5, 1.0, 1.5, 2.0, 2.5), per_gauge=3):
    """Synthetic gauge log from the measured endpoints of `slots` and a known true curve."""
    rng = rng or random.Random(1)
    lines = ["slot,gap_mm,reading"]
    for slot in slots:
        rel, bot, _ = ranges[slot]
        for gap in gaps:
            depth = TRAVEL_MM - gap
            # invert the true curve: n for this depth
            lo, hi = 0.0, 1.0
            for _ in range(60):
                mid = (lo + hi) / 2
                d = depth_mm_float(mid, a_true) if physical is None else depth_mm_physical(mid, *physical)
                if d < depth:
                    lo = mid
                else:
                    hi = mid
            n = (lo + hi) / 2
            for _ in range(per_gauge):
                lines.append("%d,%.2f,%d" % (slot, gap, round(rel - n * (rel - bot) + rng.uniform(-noise, noise))))
    return "\n".join(lines) + "\n"


def selftest():
    ranges = load_ranges()
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL:", what)

    check(len(ranges) == 61, "61 measured keys")
    # endpoints exact, monotonic, in range, for every measured key
    for slot, (rel, bot, key) in ranges.items():
        check(depth_mm100(rel, bot, rel) == 0, "slot %d released -> 0" % slot)
        check(depth_mm100(rel, bot, bot) == TRAVEL_MM100, "slot %d bottom -> 340" % slot)
        prev = 0
        for v in range(rel + 20, bot - 20, -1):
            d = depth_mm100(rel, bot, v)
            if d < prev or d > TRAVEL_MM100:
                check(False, "slot %d not monotonic at %d" % (slot, v))
                break
            prev = d
    check(depth_mm100(3300, 2300, 2800, 128) == 255, "hand-computed value (a=2, n=.5 -> 2.55 mm)")
    check(depth_mm100(3300, 2300, 2800, 0) == 170, "a=0 is linear")
    check(depth_mm100(3300, 2300, 0) == 340 and depth_mm100(3300, 2300, 65535) == 0, "clamping")
    check(depth_mm100(2000, 2000, 1990) == 0 and depth_mm100(2000, 2100, 2050) == 0, "degenerate calibrations give 0")

    # fit recovers a known curve from synthetic gauge data
    rng = random.Random(7)
    slots = [18, 1, 12]
    for a_true in (0.5, 2.0, 5.0):
        rows = parse_gauge_csv(synth_csv(ranges, slots, a_true=a_true, rng=rng))
        quiet = []
        a_q6 = analyse(rows, ranges, out=quiet.append)
        check(a_q6 is not None and abs(a_q6 / 64.0 - a_true) < 0.15 * max(1.0, a_true), "fit a=%.1f -> %s" % (a_true, a_q6))
    # data from the exact physics curve: the rational curve must stay within 0.1 mm of it
    rows = parse_gauge_csv(synth_csv(ranges, slots, physical=(2.5, 0.5), rng=rng))
    quiet = []
    a_q6 = analyse(rows, ranges, out=quiet.append)
    check(a_q6 is not None and any("rms residual 0.0" in ln for ln in quiet), "physics-curve data fits to < 0.1 mm rms")
    # the depth CSV variant and the endpoint override rows
    text = "# c\nslot,depth_mm,reading,key\n18,0,3300,ESC\n18,3.4,2100,ESC\n" + "\n".join(
        "18,%.2f,%d,ESC" % (d, 3300 - n * 1200) for d, n in ((0.9, .31), (1.9, .62), (2.9, .9)))
    cal, pts, notes = build_points(parse_gauge_csv(text), ranges)
    check(cal[18] == (3300, 2100) and len(pts) == 3 and len(notes) == 2, "endpoint rows override the JSON")
    # bad input is rejected with a clear message
    for bad, why in (("slot,reading\n1,2\n", "no depth column"), ("slot,gap_mm,reading\n1,5.0,2000\n", "gap > travel"),
                     ("slot,gap_mm,reading\nx,1,2\n", "not a number"), ("slot,gap_mm,reading\n99,1,2000\n", "bad slot"),
                     ("", "empty")):
        try:
            parse_gauge_csv(bad)
            check(False, "rejects: " + why)
        except FormatError:
            pass
    # too few depths
    quiet = []
    check(analyse(parse_gauge_csv("slot,gap_mm,reading\n18,1.0,2400\n18,2.0,2800\n"), ranges, out=quiet.append) is None, "refuses < 3 depths")
    # the study and the table run and the headline claim holds
    quiet = []
    check(study(out=quiet.append) < 0.09, "rational curve within 0.09 mm of the physics curve for p 1.5..3, sT 0.25..0.6")
    curve_table(3300, 2300, 128, out=quiet.append)
    check(not fails, "all")
    print("selftest: %s" % ("FAILED (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


# ---- command line -------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest", help="run the built-in tests (synthetic data)")
    sub.add_parser("template", help="print an empty gauge log")
    sub.add_parser("study", help="rational curve vs exact physics curve")
    p = sub.add_parser("fit", help="fit the curvature from a gauge log")
    p.add_argument("csv")
    p.add_argument("--ranges", default=str(RANGES_JSON), help="released/bottom per slot (default: our measured JSON)")
    p = sub.add_parser("curve", help="reading -> depth table")
    p.add_argument("--slot", type=int, help="take released/bottom of this slot from the JSON")
    p.add_argument("--released", type=int)
    p.add_argument("--bottom", type=int)
    p.add_argument("--a-q6", type=int, default=A_DEFAULT_Q6, help="curvature * 64 (default %d)" % A_DEFAULT_Q6)
    p.add_argument("--ranges", default=str(RANGES_JSON))
    args = ap.parse_args(argv)

    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "template":
        sys.stdout.write(TEMPLATE)
        return 0
    if args.cmd == "study":
        study()
        return 0
    if args.cmd == "fit":
        try:
            rows = parse_gauge_csv(Path(args.csv).read_text(encoding="utf-8"))
            return 0 if analyse(rows, load_ranges(args.ranges)) is not None else 1
        except FormatError as e:
            print("CSV error:", e, file=sys.stderr)
            return 2
    if args.cmd == "curve":
        if args.slot is not None:
            rel, bot, key = load_ranges(args.ranges)[args.slot]
            print("slot %d (%s)" % (args.slot, key))
        elif args.released is not None and args.bottom is not None:
            rel, bot = args.released, args.bottom
        else:
            print("give --slot or --released and --bottom", file=sys.stderr)
            return 2
        curve_table(rel, bot, args.a_q6)
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
