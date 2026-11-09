#!/usr/bin/env python3
"""Build and run tools/filter_sim.c (host noise / latency model of candidate depth filters, FILTER_NOTES.md).

    python firmware/tools/filter_sim.py                       # noise table + latency table, mixed noise
    python firmware/tools/filter_sim.py --noise white --scale 1.5 --seconds 300
    python firmware/tools/filter_sim.py --all                 # every noise colour, scale 1 and 1.5 (what FILTER_NOTES.md quotes)
    python firmware/tools/filter_sim.py --self-test           # sanity checks of the model itself

The model is deliberately small and fast (a few seconds); the real engine is compared with it by `filter_eval.py` /
`run_tests.py --filter-modes` (F0 / F1B / F1C bit for bit). Noise colours are those of tests/filter_lab.c.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def env():
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def build(out_dir: Path) -> Path:
    exe = out_dir / ("filter_sim.exe" if os.name == "nt" else "filter_sim")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(HERE / "filter_sim.c"), "-lm"]
    subprocess.run(cmd, env=env(), check=True)
    return exe


def run(exe: Path, mode: str, noise: str, scale: float, seconds: float) -> str:
    r = subprocess.run([str(exe), mode, "noise=" + noise, "scale=%g" % scale, "seconds=%g" % seconds], env=env(), capture_output=True, text=True, check=True)
    return r.stdout


def self_test(exe: Path) -> int:
    fails = 0
    out = run(exe, "table", "mixed", 1.0, 60)
    sd = {}
    for line in out.splitlines():
        cells = [c.strip() for c in line.split("|")]
        if len(cells) > 3 and re.match(r"^[0-9.]+$", cells[2]):
            sd[cells[1]] = float(cells[2])
    f0 = sd.get("F0 now (MA16+1/4+1/8)", -1.0)
    # the documented operating point: the rapid-trigger stage of the current filter leaves a standard deviation of 2.7 - 2.9 counts
    if not 2.4 < f0 < 3.3:
        print("FAIL: F0 output sd", f0)
        fails += 1
    # a shorter mean is noisier, the adaptive two-stage chain is quieter than the current one
    if not sd["MA4"] > f0:
        print("FAIL: MA4 not noisier than F0")
        fails += 1
    if not sd["F1C adaptive, two stages"] < f0:
        print("FAIL: F1C not quieter than F0")
        fails += 1
    # same seed, same output
    if out != run(exe, "table", "mixed", 1.0, 60):
        print("FAIL: not deterministic")
        fails += 1
    print("filter_sim self-test:", "FAILED" if fails else "ok")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--noise", default="mixed", choices=["white", "mixed", "lowfreq", "spiky", "real"])
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="km_fsim_") as d:
        exe = build(Path(d))
        if a.self_test:
            return self_test(exe)
        combos = [(n, s) for n in ("real", "white", "mixed", "lowfreq", "spiky") for s in (1.0, 1.5)] if a.all else [(a.noise, a.scale)]
        for noise, scale in combos:
            print("### noise %s, scale %g\n" % (noise, scale))
            print(run(exe, "table", noise, scale, a.seconds))
            print(run(exe, "latency", noise, scale, a.seconds))
            if noise == "real":
                print(run(exe, "minrt", noise, scale, a.seconds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
