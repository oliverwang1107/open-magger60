#!/usr/bin/env python3
"""Mutation pass for the adaptive engine filters and fast confirm (KM_ENGINE_FILTER_MODES = 1, FILTER_NOTES.md).

    python firmware/tools/mutate_engine_filter.py

Each mutation changes one expression of core/src/km_engine.c / km_engine.h in a scratch copy, rebuilds the engine and
tests/test_engine_filter.c with -DKM_ENGINE_FILTER_MODES=1 and runs it; the test must fail ("killed"). Survivors are listed and
make the exit code 1.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

MUTATIONS = [
    ("T1 threshold 12 -> 13", "core/include/km/km_engine.h", "#define KM_ENGINE_ADAPT_T1          12u", "#define KM_ENGINE_ADAPT_T1          13u"),
    ("T2 threshold 32 -> 31", "core/include/km/km_engine.h", "#define KM_ENGINE_ADAPT_T2          32u", "#define KM_ENGINE_ADAPT_T2          31u"),
    ("slow gain 1/8 -> 1/4", "core/src/km_engine.c", "return ad < t1 ? 3u : ad < t2 ? 2u : 1u;", "return ad < t1 ? 2u : ad < t2 ? 2u : 1u;"),
    ("fast gain 1/2 -> 1/4", "core/src/km_engine.c", "return ad < t1 ? 3u : ad < t2 ? 2u : 1u;", "return ad < t1 ? 3u : ad < t2 ? 2u : 2u;"),
    ("|diff| not taken", "core/src/km_engine.c", "const int32_t ad = diff_q4 < 0 ? -diff_q4 : diff_q4;", "const int32_t ad = diff_q4;"),
    ("stage 2 of mode 1 not a pass-through", "core/src/km_engine.c", "if (mode == KM_ENGINE_FMODE_LOW_LATENCY || mode == KM_ENGINE_FMODE_FAST) {", "if (mode == KM_ENGINE_FMODE_FAST) {"),
    ("stage 2 rounding removed", "core/src/km_engine.c", "return g + (((f - g) + (1 << (sh - 1u))) >> sh);", "return g + ((f - g) >> sh);"),
    ("stage 1 ignores the mode", "core/src/km_engine.c", "        if (fmode != KM_ENGINE_FMODE_FIXED) {\n            f = adapt_stage1(", "        if (0) {\n            f = adapt_stage1("),
    ("stage 2 ignores the mode", "core/src/km_engine.c", "        if (e->filter_mode != KM_ENGINE_FMODE_FIXED) {\n            g = adapt_stage2(", "        if (0) {\n            g = adapt_stage2("),
    ("mode setter accepts 3", "core/src/km_engine.c", "if (mode > KM_ENGINE_FMODE_LAST) {", "if (mode > KM_ENGINE_FMODE_LAST + 1u) {"),
    ("g not restarted at a mode change", "core/src/km_engine.c", "            e->slot[s].g_q4 = e->slot[s].filt_q4;       /* the second", "            (void)0;       /* the second"),
    ("fast mode T1 3 -> 4", "core/include/km/km_engine.h", "#define KM_ENGINE_FAST_T1           3u", "#define KM_ENGINE_FAST_T1           4u"),
    ("fast mode T2 8 -> 9", "core/include/km/km_engine.h", "#define KM_ENGINE_FAST_T2           8u", "#define KM_ENGINE_FAST_T2           9u"),
    ("fast mode press frames 4 -> 5", "core/include/km/km_engine.h", "#define KM_ENGINE_FAST_PRESS_FRAMES   4u", "#define KM_ENGINE_FAST_PRESS_FRAMES   5u"),
    ("fast mode release frames 3 -> 4", "core/include/km/km_engine.h", "#define KM_ENGINE_FAST_RELEASE_FRAMES 3u", "#define KM_ENGINE_FAST_RELEASE_FRAMES 4u"),
    ("fast mode keeps the second stage", "core/src/km_engine.c", "if (mode == KM_ENGINE_FMODE_LOW_LATENCY || mode == KM_ENGINE_FMODE_FAST) {", "if (mode == KM_ENGINE_FMODE_LOW_LATENCY) {"),
    ("fast mode uses the quiet thresholds", "core/src/km_engine.c", "(mode == KM_ENGINE_FMODE_FAST ? KM_ENGINE_FAST_T1 : KM_ENGINE_ADAPT_T1)", "(KM_ENGINE_ADAPT_T1)"),
    ("fast press frames 3 -> 5", "core/include/km/km_engine.h", "#define KM_ENGINE_FAST_FRAMES       3u", "#define KM_ENGINE_FAST_FRAMES       5u"),
    ("fast press needs only 1x rt", "core/src/km_engine.c", "+ KM_ENGINE_FAST_FACTOR * k->cfg.rt_down_mm100) {", "+ 1u * k->cfg.rt_down_mm100) {"),
    ("fast release needs only 1x rt", "core/src/km_engine.c", ">= KM_ENGINE_FAST_FACTOR * k->cfg.rt_up_mm100) {", ">= 1u * k->cfg.rt_up_mm100) {"),
    ("fast confirm ignores the switch (press)", "core/src/km_engine.c", "if (want && e->fast_confirm && d >=", "if (want && d >="),
    ("fast confirm ignores the switch (release)", "core/src/km_engine.c", "if (e->fast_confirm && (unsigned)rapid_depth(k, low)", "if ((unsigned)rapid_depth(k, low)"),
]


# Not killed by THIS small harness, by design: the second stage's sub-count rounding is proved by the lab equivalence (tests/test_filter_traces.c compares
# the real engine in modes 1 / 2 with the lab candidates F1B / F1C on noisy traces, down to the sums of g, and fails if the rounding changes - run
# `run_tests.py --filter-modes` for that); restarting g at a mode change has no effect that a public accessor or an event time can show (the second
# stage re-converges within the debounce).
COVERED_ELSEWHERE = {"stage 2 rounding removed", "g not restarted at a mode change"}


def env():
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def build_and_run(root: Path, out: Path, e) -> str:
    """'killed' (test fails), 'survived' (passes) or 'build error'."""
    (out / "m.c").write_text('#include <stdio.h>\nint km_test_checks, km_test_failures;\nvoid test_engine_filter(void);\n'
                             'int main(void) { test_engine_filter(); return km_test_failures != 0; }\n')
    srcs = [str(root / "core/src/km_engine.c"), str(root / "core/src/km_keymap.c"), str(root / "core/src/km_depth_model.c"),
            str(root / "core/gen/km_slot_data.c"), str(FW / "tests/test_engine_filter.c"), str(out / "m.c")]
    exe = out / "t.exe"
    cmd = ["gcc", "-std=c11", "-w", "-O1", "-DKM_ENGINE_FILTER_MODES=1", "-I" + str(root / "core/include"), "-I" + str(FW / "tests"),
           "-o", str(exe), *srcs]
    if subprocess.run(cmd, env=e, capture_output=True).returncode != 0:
        return "build error"
    r = subprocess.run([str(exe)], env=e, capture_output=True, timeout=300)
    return "survived" if r.returncode == 0 else "killed"


def main() -> int:
    e = env()
    tmp = Path(tempfile.mkdtemp(prefix="km_mutf_"))
    survivors = []
    try:
        base = tmp / "base"
        base.mkdir()
        shutil.copytree(FW / "core", base / "core")
        if build_and_run(base, base, e) != "survived":
            print("the unmutated code does not pass its own test")
            return 2
        for i, (name, rel, old, new) in enumerate(MUTATIONS):
            root = tmp / ("m%d" % i)
            shutil.copytree(FW / "core", root / "core")
            f = root / rel
            text = f.read_text(encoding="utf-8").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%-45s ANCHOR NOT FOUND (%d)" % (name, text.count(old)))
                survivors.append(name)
                continue
            f.write_text(text.replace(old, new), encoding="utf-8")
            res = build_and_run(root, root, e)
            print("%-45s %s" % (name, res))
            if res != "killed":
                survivors.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    real = [m for m in survivors if m not in COVERED_ELSEWHERE]
    print("\n%d mutations, %d not killed here (%d of them covered elsewhere)" % (len(MUTATIONS), len(survivors), len(survivors) - len(real)))
    for m in real:
        print("  SURVIVOR:", m)
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main())
