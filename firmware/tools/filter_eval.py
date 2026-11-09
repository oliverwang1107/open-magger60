#!/usr/bin/env python3
"""filter_eval.py - compare candidate sensing filters on the REAL key engine logic (M6 task P12b, core/FILTER_NOTES.md).

Builds tools/filter_lab_cli.c with tests/filter_lab*.c (the engine runs frame by frame exactly as on the keyboard: raw samples ->
km_scan sums -> engine) and drives it. Synthetic noise models with documented standard deviations, or REAL captures made with
tools/magger60_noise.py (KMTR files).

    python firmware/tools/filter_eval.py --self-test              # build + checks (needs gcc; also runs the lab-engine sync check)
    python firmware/tools/filter_eval.py --matrix                 # the full comparison tables (several minutes)
    python firmware/tools/filter_eval.py --matrix --quick         # a smaller grid (about a minute)
    python firmware/tools/filter_eval.py --trace noise_data/A_idle_all [more files or directories]   # real captures
    python firmware/tools/filter_eval.py --bench                  # host time per frame of the variants
    python firmware/tools/filter_eval.py --make-test-trace        # rewrite firmware/tests/traces/synthetic_idle_2col.kmtr

Nothing here talks to a keyboard. All tables are SYNTHETIC unless they come from --trace; read core/FILTER_NOTES.md for what that means.
"""
import argparse
import concurrent.futures
import importlib.util
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import math
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
REPO = FW.parent
CORE_SRC = sorted((FW / "core/src").glob("*.c")) + [FW / "core/gen/km_slot_data.c"]
LAB_SRC = [FW / "tools/filter_lab_cli.c", FW / "tests/filter_lab.c", FW / "tests/filter_lab_engine.c"]
FILTERS = ["F0", "F1A", "F1B", "F1C", "F4M", "F4C", "F5"]
FILTER_TEXT = {
    "F0": "engine as it is", "F1A": "adaptive gain, rapid-trigger slow stage only", "F1B": "one adaptive stage instead of two",
    "F1C": "adaptive gain in both stages", "F4M": "3-point median on raw samples", "F4C": "clamp raw samples to +-45",
    "F5": "common-mode subtraction",
}
PROFILES = ["legacy", "white", "mixed", "lowfreq", "commonmode", "spiky", "tonal"]


# --------------------------------------------------------------------------- build and run

def env():
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def build(build_dir: Path) -> Path:
    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    if not cc:
        raise SystemExit("gcc not found")
    exe = build_dir / ("filter_lab.exe" if os.name == "nt" else "filter_lab")
    cmd = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(FW / "core/include"), "-I" + str(FW / "tests"),
           *map(str, LAB_SRC), *map(str, CORE_SRC), "-o", str(exe)]
    r = subprocess.run(cmd, env=e, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("build failed:\n" + r.stdout + r.stderr)
    return exe


def parse_result(line: str) -> dict:
    """'RESULT cmd=quiet filter=F0 rt=20 sd=3.2' -> {'cmd': 'quiet', 'filter': 'F0', 'rt': 20.0, 'sd': 3.2}"""
    if not line.startswith("RESULT "):
        raise ValueError("not a RESULT line: %r" % line[:80])
    out = {}
    for tok in line[len("RESULT "):].split():
        k, _, v = tok.partition("=")
        try:
            out[k] = float(v)
        except ValueError:
            out[k] = v
    return out


class Lab:
    def __init__(self, exe: Path):
        self.exe = exe

    def run(self, cmd: str, **kw) -> dict:
        args = [str(self.exe), cmd] + ["%s=%s" % (k, v) for k, v in kw.items()]
        r = subprocess.run(args, capture_output=True, text=True, env=env())
        if r.returncode != 0:
            raise RuntimeError("%s failed: %s %s" % (" ".join(args), r.stdout.strip(), r.stderr.strip()))
        res = parse_result(r.stdout.strip().splitlines()[-1])
        if "error" in res:
            raise RuntimeError("%s -> %s" % (" ".join(args), r.stdout.strip()))
        return res

    def many(self, jobs, workers=None):
        """jobs = list of (cmd, kwargs); returns the results in the same order."""
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers or os.cpu_count() or 2) as ex:
            return list(ex.map(lambda j: self.run(j[0], **j[1]), jobs))


# --------------------------------------------------------------------------- tables

def md_table(header, rows):
    w = [max(len(str(r[i])) for r in [header] + rows) for i in range(len(header))]
    lines = ["| " + " | ".join(str(h).ljust(w[i]) for i, h in enumerate(header)) + " |",
             "|" + "|".join("-" * (w[i] + 2) for i in range(len(header))) + "|"]
    for r in rows:
        lines.append("| " + " | ".join(str(c).ljust(w[i]) for i, c in enumerate(r)) + " |")
    return "\n".join(lines)


def false_noise(res):
    """False events on keys that never move: at rest and held still at 0.3 / 1.5 / 3.0 mm."""
    return int(res["false_rest"] + res["false_h30"] + res["false_h150"] + res["false_h300"])


def false_wobble(res):
    return int(res["false_w30"] + res["false_w150"])


# --------------------------------------------------------------------------- the comparison

def matrix(lab: Lab, quick: bool, out=print):
    secs = 15 if quick else 60
    results = {}
    out("# Candidate filters on the real engine - SYNTHETIC noise (see core/FILTER_NOTES.md for what that means)\n")

    # 1. false events
    jobs, keys = [], []
    for p in PROFILES:
        for f in FILTERS:
            kw = dict(filter=f, profile=p, rt=20, seconds=secs, seed=1)
            if f == "F5" or p == "commonmode":
                kw["nref"] = 32
            jobs.append(("quiet", kw))
            keys.append((p, f))
    res = lab.many(jobs)
    results["quiet_rt20"] = {"%s/%s" % k: r for k, r in zip(keys, res)}
    hours = 4 * secs / 3600.0
    rows = []
    for p in PROFILES:
        row = [p]
        for f in FILTERS:
            r = results["quiet_rt20"]["%s/%s" % (p, f)]
            row.append("%d (%d)" % (false_noise(r), false_wobble(r)))
        rows.append(row)
    out("## 1. False events at rt = 0.20 mm: keys that do not move (wobble keys in brackets)\n")
    out("%d s per run, 4 still keys (at rest, held at 0.3 / 1.5 / 3.0 mm) + 2 keys with an 8 Hz finger wobble of +-0.15 x rt. "
        "Zero is what the engine needs. Counted: press / release events after the first second.\n" % secs)
    out(md_table(["noise \\ filter"] + FILTERS, rows) + "\n")
    rows = []
    for p in PROFILES:
        row = [p]
        for f in FILTERS:
            r = results["quiet_rt20"]["%s/%s" % (p, f)]
            row.append("%.2f / %.2f" % (r["sd_f_2"], r["sd_g_2"]))
        rows.append(row)
    out("sd of the main filter f / the slow stage g (counts) of the key held at 1.5 mm:\n")
    out(md_table(["noise \\ filter"] + FILTERS, rows) + "\n")

    # 2. latency
    speeds = [50, 300] if quick else [50, 100, 300, 500]
    lat_filters = ["F0", "F1A", "F1B", "F1C"]
    jobs, keys = [], []
    for prof in ("mixed", "legacy"):
        for sp in speeds:
            for f in lat_filters:
                jobs.append(("latency", dict(filter=f, profile=prof, rt=25, speed=sp, seeds=2 if quick else 5, cycles=20, seed=7)))
                keys.append((prof, sp, f))
    for sp in speeds[-2:]:
        for f in lat_filters:
            jobs.append(("latency", dict(filter=f, profile="mixed", mode="fixed", speed=sp, seeds=2 if quick else 5, cycles=20, seed=7)))
            keys.append(("fixed", sp, f))
    res = lab.many(jobs)
    results["latency"] = {"%s/%s/%s" % k: r for k, r in zip(keys, res)}
    out("## 2. Latency: true crossing -> engine event (ms, median / p95) and extra events\n")
    out("Rapid trigger, rt = 0.25 mm, strokes 0 -> 1.5 mm -> 0 at the given speed; 'press' = rt_down below the shallowest point, "
        "'release' = rt_up above the deepest. 'fixed' = a FIXED key (1.2 / 0.8 mm). The debounce alone is 0.34 ms (press) and "
        "0.26 ms (release). extra = events with no partner on the true path.\n")
    for prof in ("mixed", "legacy", "fixed"):
        rows = []
        for sp in (speeds if prof != "fixed" else speeds[-2:]):
            for f in lat_filters:
                r = results["latency"]["%s/%d/%s" % (prof, sp, f)]
                rows.append(["%d mm/s" % sp, f, "%.2f / %.2f" % (r["press_med_ms"], r["press_p95_ms"]),
                             "%.2f / %.2f" % (r["rel_med_ms"], r["rel_p95_ms"]), "%d" % r["extra"], "%d" % r["missed"]])
        out("noise: %s\n" % prof)
        out(md_table(["speed", "filter", "press ms", "release ms", "extra", "missed"], rows) + "\n")

    # 3. smallest safe rapid-trigger distance
    scales = [0.5, 1.0] if quick else [0.5, 0.75, 1.0, 1.5]
    rts = [10, 15, 20, 30] if quick else [8, 10, 12, 15, 20, 25, 30, 40]
    f3 = ["F0", "F1B"] if quick else ["F0", "F1A", "F1B", "F1C"]
    jobs, keys = [], []
    for f in f3:
        for sc in scales:
            for rt in rts:
                jobs.append(("quiet", dict(filter=f, profile="mixed", rt=rt, seconds=30 if not quick else 15, scale=sc, seed=3, wig=0)))
                keys.append((f, sc, rt))
    res = lab.many(jobs)
    results["floor"] = {"%s/%g/%d" % k: r for k, r in zip(keys, res)}
    rows = []
    for f in f3:
        for sc in scales:
            safe = [rt for rt in rts if false_noise(results["floor"]["%s/%g/%d" % (f, sc, rt)]) == 0]
            rows.append([f, "%.2f" % sc, ("%.2f mm" % (min(safe) / 100.0)) if safe else "none <= %.2f mm" % (max(rts) / 100.0),
                         " ".join("%d" % false_noise(results["floor"]["%s/%g/%d" % (f, sc, rt)]) for rt in rts)])
    out("## 3. Smallest rapid-trigger distance with zero false events (candidate F3: a per-key floor)\n")
    out("noise 'mixed' x scale (scale 1 = the documented operating point, f sd 3.3 counts; a scale below 1 stands for a quieter key). "
        "Last column: false events for rt = %s units. The lab engine accepts distances below the real 0.20 mm floor.\n"
        % ", ".join(str(r) for r in rts))
    out(md_table(["filter", "noise scale", "smallest safe rt", "false events per rt"], rows) + "\n")

    # 4. analog path
    jobs, keys = [], []
    for p in ("white", "mixed", "legacy"):
        jobs.append(("quiet", dict(filter="F0", profile=p, rt=20, seconds=secs, seed=2, wig=0)))
        keys.append(("quiet", p))
        jobs.append(("step", dict(filter="F0", profile=p, seeds=4 if quick else 10, seed=5)))
        keys.append(("step", p))
    res = lab.many(jobs)
    results["analog"] = {"%s/%s" % k: r for k, r in zip(keys, res)}
    names = ["engine depth (f, integer counts)", "IIR 1/2 + hysteresis", "IIR 1/4 + hysteresis", "IIR 1/4, no hysteresis"]
    out("## 4. Analog path (candidate F2): output in 8-bit ticks (0..255 over 3.4 mm; 1 tick = 0.0133 mm), evaluated at 1 kHz\n")
    out("Standstill sd in ticks and tick changes per second (max 1000) of a key held at 1.5 mm; 90 % rise time of a 1 mm step. "
        "Target: sd <= 0.5 tick.\n")
    rows = []
    for p in ("white", "mixed", "legacy"):
        q, s = results["analog"]["quiet/" + p], results["analog"]["step/" + p]
        for i, n in enumerate(names):
            rows.append([p, n, "%.2f" % q["a%d_sd_2" % i], "%.0f" % q["a%d_jps_2" % i], "%d" % q["a%d_pp_2" % i], "%.2f ms" % s["a%d_rise90_ms" % i]])
    out(md_table(["noise", "path", "sd ticks", "changes/s", "p-p ticks", "step 90 %"], rows) + "\n")
    return results


# --------------------------------------------------------------------------- real traces

def find_traces(paths):
    out = []
    for p in paths:
        p = Path(p)
        out += sorted(p.glob("*.kmtr")) if p.is_dir() else [p]
    return out


def trace_report(lab: Lab, paths, mode="rapid", rt=20, out=print):
    files = find_traces(paths)
    if not files:
        out("no *.kmtr files found")
        return {}
    res = {}
    for f in files:
        rows = []
        for flt in FILTERS:
            r = lab.run("trace", file=str(f), filter=flt, mode=mode, rt=rt, nref=32 if flt == "F5" else 0)
            res["%s/%s" % (f.name, flt)] = r
            cols = sorted({k.split("_")[0] for k in r if k.startswith("slot")}, key=lambda s: int(s[4:]))
            for c in cols:
                rows.append([flt, c, "%.2f" % r["%s_sd_f" % c], "%.2f" % r["%s_sd_g" % c], "%.2f" % r["%s_a0_sd" % c], "%.2f" % r["%s_a2_sd" % c],
                             "%d" % r["%s_a2_jumps" % c], "%d" % r["events"]])
        out("## %s (%s mode, rt %d)\n" % (f.name, mode, rt))
        out(md_table(["filter", "column", "sd f", "sd g", "sd ticks (engine)", "sd ticks (IIR 1/4 + hyst.)", "tick changes", "events"], rows) + "\n")
    return res


# --------------------------------------------------------------------------- test traces

def make_test_trace(path=None):
    path = Path(path or FW / "tests/traces/synthetic_idle_2col.kmtr")
    spec = importlib.util.spec_from_file_location("magger60_noise", REPO / "tools/magger60_noise.py")
    noise = importlib.util.module_from_spec(spec)
    sys.modules["magger60_noise"] = noise
    spec.loader.exec_module(noise)
    rng = random.Random(20261030)
    fs = 23300.0
    rows = []
    for f in range(6000):
        a = int(round(3300 + rng.gauss(0, 15)))
        b = int(round(3290 + rng.gauss(0, 15) + 3 * math.sin(2 * math.pi * 1000 * f / fs)))
        flag = noise.FLAG if f % 97 == 0 else 0
        rows.append([a | flag, b | flag])
    noise.write_kmtr(path, [0, 1], rows, fs)
    return path, noise


# --------------------------------------------------------------------------- self test

def self_test() -> int:
    fails = []

    def check(c, m):
        if not c:
            fails.append(m)

    # parser and tables
    r = parse_result("RESULT cmd=quiet filter=F0 rt=20 sd=3.25 error_free=yes")
    check(r == {"cmd": "quiet", "filter": "F0", "rt": 20.0, "sd": 3.25, "error_free": "yes"}, "parse_result: %r" % r)
    try:
        parse_result("hello")
        fails.append("parse_result accepted garbage")
    except ValueError:
        pass
    t = md_table(["a", "bb"], [["1", "2"], ["333", "4"]])
    check(t.splitlines()[0] == "| a   | bb |" and t.splitlines()[2] == "| 1   | 2  |", "md_table: %r" % t)
    check(false_noise({"false_rest": 1, "false_h30": 2, "false_h150": 3, "false_h300": 4}) == 10, "false_noise")

    # the generated lab engine is in sync with the engine (otherwise its numbers say nothing about core/src/km_engine.c)
    g = subprocess.run([sys.executable, str(FW / "tools/gen_filter_lab_engine.py"), "--check"], capture_output=True, text=True)
    check(g.returncode == 0, "gen_filter_lab_engine.py --check: " + g.stdout + g.stderr)

    with tempfile.TemporaryDirectory() as td:
        lab = Lab(build(Path(td)))
        # a run is deterministic and has the documented operating point
        a = lab.run("quiet", filter="F0", profile="white", rt=20, seconds=8, seed=4)
        b = lab.run("quiet", filter="F0", profile="white", rt=20, seconds=8, seed=4)
        check(a == b, "the lab is not deterministic")
        check(2.8 < a["sd_f_0"] < 3.9, "sd_f at the operating point: %s" % a["sd_f_0"])
        check(a["false_rest"] == 0, "false events at rest with documented noise")
        # more noise -> more false events, a bigger distance -> fewer
        noisy = lab.run("quiet", filter="F0", profile="white", rt=20, seconds=8, seed=4, scale=2.5)
        check(false_noise(noisy) > false_noise(a), "2.5x noise must produce more false events")
        wide = lab.run("quiet", filter="F0", profile="white", rt=40, seconds=8, seed=4, scale=2.5)
        check(false_noise(wide) < false_noise(noisy), "a larger rt must produce fewer false events")
        # latency: a faster stroke has a smaller latency in ms than a slow one does not hold in general, but all strokes are matched
        l = lab.run("latency", filter="F0", profile="none", rt=25, speed=300, seeds=1, cycles=4)
        check(l["matched"] == 8 and l["missed"] == 0 and l["extra"] == 0, "noise-free strokes: %r" % l)
        check(0.3 < l["press_med_ms"] < 2.5 and 0.3 < l["rel_med_ms"] < 2.5, "noise-free latency out of range")
        s = lab.run("step", filter="F0", profile="none", seeds=1)
        check(s["a0_rise90_ms"] < s["a1_rise90_ms"] < s["a2_rise90_ms"], "step rise times must grow with the filter length: %r" % s)
        # F1B must be faster than F0 on a fast stroke (noise free)
        lb = lab.run("latency", filter="F1B", profile="none", rt=25, speed=300, seeds=1, cycles=4)
        check(lb["rel_med_ms"] < l["rel_med_ms"], "F1B release latency")
        # a trace written by the capture tool is read back: 4 idle columns of known noise
        path, noise = make_test_trace(Path(td) / "t.kmtr")
        tr = lab.run("trace", file=str(path), filter="F0", mode="rapid", rt=20)
        check(tr["events"] == 0 and 2.7 < tr["slot0_sd_f"] < 4.2, "trace replay: %r" % {k: v for k, v in tr.items() if k in ("events", "slot0_sd_f")})
        # a trace with a pressed key shows events: column 0 steps from 3300 down to 2800 and back
        rows, rng = [], random.Random(5)
        for f in range(8000):
            base = 2800 if 2000 <= f < 5000 else 3300
            rows.append([int(round(base + rng.gauss(0, 10))), int(round(3300 + rng.gauss(0, 10)))])
        pth = Path(td) / "press.kmtr"
        noise.write_kmtr(pth, [0, 1], rows, 23300.0)
        tp = lab.run("trace", file=str(pth), filter="F0", mode="rapid", rt=25)
        check(tp["events"] == 2, "a press and a release in the trace, got %s events" % tp["events"])
        # a bad file is an error, not a crash
        bad = Path(td) / "bad.kmtr"
        bad.write_bytes(b"KMTR\x01")
        try:
            lab.run("trace", file=str(bad))
            fails.append("a truncated trace was accepted")
        except RuntimeError:
            pass
        # the committed test trace is the one make_test_trace would write
        committed = (FW / "tests/traces/synthetic_idle_2col.kmtr").read_bytes()
        check(committed == path.read_bytes(), "tests/traces/synthetic_idle_2col.kmtr is not what --make-test-trace writes")
        # a tiny matrix runs end to end and prints something sensible
        lines = []
        res = matrix(lab, True, out=lines.append) if os.environ.get("FILTER_EVAL_FULL_SELFTEST") else None
        if res is not None:
            check(any("False events" in ln for ln in lines), "matrix output")
    print("SELF-TEST", "FAILED" if fails else "OK")
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


# --------------------------------------------------------------------------- main

def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--matrix", action="store_true")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--trace", nargs="+", metavar="FILE_OR_DIR")
    ap.add_argument("--mode", choices=["rapid", "fixed"], default="rapid", help="--trace: trigger mode of the evaluated keys")
    ap.add_argument("--rt", type=int, default=20, help="--trace: rapid-trigger distance in 0.01 mm")
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--make-test-trace", action="store_true")
    ap.add_argument("--out", help="write the raw results as JSON to this file")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.make_test_trace:
        print("wrote", make_test_trace()[0])
        return 0
    if not (args.matrix or args.trace or args.bench):
        ap.print_help()
        return 0
    with tempfile.TemporaryDirectory() as td:
        lab = Lab(build(Path(td)))
        res = {}
        if args.matrix:
            res["matrix"] = matrix(lab, args.quick)
        if args.trace:
            res["trace"] = trace_report(lab, args.trace, args.mode, args.rt)
        if args.bench:
            res["bench"] = lab.run("bench", seconds=3)
            print(json.dumps(res["bench"], indent=1))
        if args.out:
            Path(args.out).write_text(json.dumps(res, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
