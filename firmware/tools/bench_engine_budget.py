#!/usr/bin/env python3
"""Worst-case per-frame work of the real key engine (M5 task T-E, stability rule 4: time budget).

    python firmware/tools/bench_engine_budget.py                 # build + run + print the table
    python firmware/tools/bench_engine_budget.py --scale 3       # three times as many frames
    python firmware/tools/bench_engine_budget.py --idle-cycles 2600   # re-anchor the host -> M4 cycle conversion

Builds firmware/tools/bench_engine_budget.c with the REAL km_engine.c, km_depth_model.c and km_keymap.c (host gcc, -O2) and feeds it
seven recorded-style signal scenarios built on the real per-key endpoints of core/data/slot_ranges_s2.json plus +-15 count noise:
idle noise, typing strokes, all 61 keys at once, a thermal drift of all baselines, a settings change that dirties every key, the
worst case of the threshold-refresh scan (one dirty slot as far from the cursor as possible), common-mode glitches.

Every scenario is replayed 5 times with identical random numbers and the fastest run of each frame counts, which removes the
operating-system scheduling spikes of the host. Per scenario it prints the engine time per frame as measured on the host (nanoseconds) and as an ESTIMATE of Cortex-M4F cycles.
The estimate is a conversion, not a measurement: the median idle frame of the host run is anchored to --idle-cycles (default 3000, the
middle of the 2.6-3.3 k cycles that core/ENGINE_NOTES.md derives from the compiler output for an idle frame) and every other frame is
scaled by the same factor. A second column models the frame from its parts (see MODEL below). Neither includes the scan interrupts:
see core/STABILITY_NOTES.md for why that matters for the 10.7 k figure seen on the keyboard.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

# MODEL: M4 cycles of the parts of one engine frame (estimates, see STABILITY_NOTES.md)
MODEL_BASE = 3000          # the 61-slot loop of an idle frame (ENGINE_NOTES: 2.6-3.3 k)
MODEL_PER_ITER = 8         # one step of the dirty search in refresh_one(): load, test, increment with wrap (was 22 with a 32-bit modulo per step)
MODEL_PER_CALL = 45        # one km_depth_model_mm100 call (km_depth_model.c: about 40 cycles)
MODEL_RECOMPUTE_FIXED = 120  # make_cal, the two thresholds, bookkeeping around the bisections
MODEL_PER_DRIFT_SLOT = 40  # drift_step() of one idle key on the 16th frame: ~25 instructions + the loads/stores of a 28-byte slot


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def write_ranges(path: Path) -> None:
    d = json.loads((FW / "core/data/slot_ranges_s2.json").read_text())["keys"]
    lines = []
    for s in range(76):
        k = d.get(str(s))
        lines.append("%d %d" % ((k["released"], k["bottom"]) if k else (3300, 2300)))
    path.write_text("\n".join(lines) + "\n")


def build(work: Path, e: dict) -> Path:
    cc = shutil.which("gcc", path=e["PATH"])
    if not cc:
        sys.exit("gcc not found")
    exe = work / ("bench_engine_budget.exe" if os.name == "nt" else "bench_engine_budget")
    cmd = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-I" + str(FW / "core/include"),
           str(FW / "tools/bench_engine_budget.c"), str(FW / "core/src/km_engine.c"), str(FW / "core/src/km_depth_model.c"),
           str(FW / "core/src/km_keymap.c"), "-Wl,--wrap=km_depth_model_mm100", "-o", str(exe)]
    r = subprocess.run(cmd, env=e, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build failed:\n" + r.stderr)
    return exe


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scale", type=int, default=1, help="multiply the number of frames of every scenario")
    ap.add_argument("--idle-cycles", type=float, default=3000.0, help="M4 cycles that the median idle frame is anchored to")
    ap.add_argument("--json", help="write the numbers to this file")
    args = ap.parse_args()

    e = env()
    work = Path(tempfile.mkdtemp(prefix="km_bench_"))
    try:
        write_ranges(work / "ranges.txt")
        exe = build(work, e)
        r = subprocess.run([str(exe), str(work / "ranges.txt"), str(args.scale)], env=e, capture_output=True, text=True, timeout=1800)
        if r.returncode != 0:
            sys.exit("bench failed:\n" + r.stdout + r.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    hz = 1.0
    scen, tops, classes = {}, {}, {}
    for line in r.stdout.splitlines():
        f = line.split("\t")
        if f[0] == "H":
            hz = float(f[4])
        elif f[0] == "S":
            scen[f[1]] = [int(x) for x in f[2:]]
        elif f[0] == "C":
            classes[f[1]] = [int(x) for x in f[2:]]
        elif f[0] == "T":
            tops.setdefault(f[1], []).append([int(x) for x in f[2:]])
    idle_median = scen["idle"][2]
    k = args.idle_cycles / max(idle_median, 1)
    ns = 1e9 / hz

    print("host tick rate %.2f GHz; the median idle frame (%d ticks = %.0f ns) is anchored to %.0f M4 cycles (x%.2f)\n"
          % (hz / 1e9, idle_median, idle_median * ns, args.idle_cycles, k))
    print("%-22s %9s | %7s %7s %7s %8s %8s | %8s %8s %8s | %s" %
          ("scenario", "frames", "mean ns", "p99 ns", "p99.9", "max ns", "", "est mean", "est p99.9", "est max", "frames with recompute"))
    out = {}
    for name, v in scen.items():
        n, mean, p50, p99, p999, mx, nrec, _ = v
        print("%-22s %9d | %7.0f %7.0f %7.0f %8.0f %8s | %8.0f %8.0f %8.0f | %d" %
              (name, n, mean * ns, p99 * ns, p999 * ns, mx * ns, "", mean * k, p999 * k, mx * k, nrec))
        out[name] = {"frames": n, "mean_ns": mean * ns, "p99_ns": p99 * ns, "p999_ns": p999 * ns, "max_ns": mx * ns,
                     "est_cycles_mean": mean * k, "est_cycles_p999": p999 * k, "est_cycles_max": mx * k, "recompute_frames": nrec}
    print("\nmean cost by frame class (host ticks -> est. cycles by anchor), per scenario:  normal | drift look | recompute | drift look + recompute")
    for name, c in classes.items():
        cells = []
        for cnt, tk in (c[0:2], c[2:4], c[4:6], c[6:8]):
            cells.append("%6d fr %5d tk %6.0f cy" % (cnt, tk, tk * k) if cnt else "%6d fr %-18s" % (cnt, "-"))
        print(" %-22s %s" % (name, " | ".join(cells)))
    print("\nfive most expensive frames per scenario  (ticks -> est. cycles | depth-model calls | refresh iterations | new presses | drift look | MODEL cycles)")
    for name, rows in tops.items():
        print(" %s" % name)
        for tk, calls, it, new, drift in rows:
            model = (MODEL_BASE + it * MODEL_PER_ITER + calls * MODEL_PER_CALL + (MODEL_RECOMPUTE_FIXED if calls else 0)
                     + drift * MODEL_PER_DRIFT_SLOT)
            print("   %6d ticks -> %6.0f cycles | calls %2d | iter %2d | new %d | drift %d | model %6d" % (tk, tk * k, calls, it, new, drift, model))
    print("\nMODEL worst cases, M4 cycles of the engine alone (without the scan interrupts):")
    for label, parts in (("idle frame", 0), ("drift look, 4 keys (spread, current)", 4 * MODEL_PER_DRIFT_SLOT),
                         ("drift look, 61 keys (before the spread)", 61 * MODEL_PER_DRIFT_SLOT),
                         ("recompute (22 depth calls)", 22 * MODEL_PER_CALL + MODEL_RECOMPUTE_FIXED),
                         ("recompute + 61-step dirty search", 22 * MODEL_PER_CALL + MODEL_RECOMPUTE_FIXED + 61 * MODEL_PER_ITER),
                         ("4-key drift + recompute + 61-step search (current worst)", 4 * MODEL_PER_DRIFT_SLOT + 22 * MODEL_PER_CALL + MODEL_RECOMPUTE_FIXED + 61 * MODEL_PER_ITER),
                         ("61-key drift + recompute + 61-step search (before)", 61 * MODEL_PER_DRIFT_SLOT + 22 * MODEL_PER_CALL + MODEL_RECOMPUTE_FIXED + 61 * MODEL_PER_ITER)):
        print("  %-42s %6d" % (label, MODEL_BASE + parts))
    # the single slowest frame is dominated by what the host operating system did to that one run, so the headline is the slowest
    # class MEAN (frames of the same kind: normal / drift look / recompute / both), over all scenarios with at least 20 such frames
    best = max(((c[i + 1], name, lbl) for name, c in classes.items()
                for i, lbl in ((0, "normal"), (2, "drift look"), (4, "recompute"), (6, "drift look + recompute")) if c[i] >= 20))
    print("\nslowest frame class: '%s' in '%s': %d ticks = %.0f ns = est. %.0f M4 cycles   (frame budget 10400 cycles = 43 us; the MODEL says %d)"
          % (best[2], best[1], best[0], best[0] * ns, best[0] * k,
             MODEL_BASE + 4 * MODEL_PER_DRIFT_SLOT + 22 * MODEL_PER_CALL + MODEL_RECOMPUTE_FIXED))
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
