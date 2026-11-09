#!/usr/bin/env python3
"""Cross-language test vectors for the gamepad page: the REAL km_pad.c against the JavaScript port (ui/js/core/pad_sim.js).

    python ui/tests/vectors/gen_pad_vectors.py                 # build a tiny C harness, write ui/tests/vectors/pad_vectors.json
    python ui/tests/vectors/gen_pad_vectors.py --check         # compare with the committed file (exit 1 if stale)
    python ui/tests/vectors/gen_pad_vectors.py --core <dir>    # directory that holds include/km/km_pad.h and src/km_pad.c
                                                               # (default firmware/core; before the gamepad core is merged: its worktree)
    python ui/tests/vectors/gen_pad_vectors.py --padmap <file> # tools/magger60_pad_map.py (SDL mapping lines), default tools/magger60_pad_map.py

What it does: compiles a small C program (in a temporary directory, nothing is kept) from the real km_pad.c, feeds it
  - every response curve of a grid (start, end, shape, mirror) at every depth 0..340 (0.01 mm),
  - the default curves, the complete argument rules of the table codes 0xEE / 0xEF,
  - random key sequences through km_pad_update (curves, dead zone, circle, hat, depth buttons, the 1 ms cadence, faults, epoch changes,
    invalid and surplus entries),
and writes what the firmware answered. tests/pad.test.mjs then makes the JavaScript port give exactly the same numbers. The SDL
mapping lines come from tools/magger60_pad_map.py. Nothing is pasted by hand: change km_pad.c, run this, and the test shows whether
the page's maths still agrees. On Windows the MSYS2 ucrt64 compiler is put first on PATH (as in run_tests.py).
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent / "pad_vectors.json"
SLOTS = 76

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "km/km_pad.h"

static uint16_t g_depth[KM_PAD_SLOTS];
static uint16_t depth_cb(void *ctx, unsigned slot) { (void)ctx; return slot < KM_PAD_SLOTS ? g_depth[slot] : 0u; }

static km_pad_t pad;

int main(void) {
    char cmd[8];
    km_pad_init(&pad);
    while (scanf("%7s", cmd) == 1) {
        if (strcmp(cmd, "I") == 0) {
            km_pad_init(&pad);
            memset(g_depth, 0, sizeof g_depth);
        } else if (strcmp(cmd, "C") == 0) {
            km_pad_curve_t c[KM_PAD_CURVES];
            km_pad_opts_t o;
            unsigned v[4], dz, circ, smooth;
            memset(&o, 0, sizeof o);
            for (unsigned i = 0; i < KM_PAD_CURVES; i++) {
                if (scanf("%u %u %u %u", &v[0], &v[1], &v[2], &v[3]) != 4) return 2;
                c[i].start_mm100_5 = (uint8_t)v[0]; c[i].end_mm100_5 = (uint8_t)v[1]; c[i].shape = (uint8_t)v[2]; c[i].flags = (uint8_t)v[3];
            }
            if (scanf("%u %u %u", &dz, &circ, &smooth) != 3) return 2;
            o.stick_deadzone = (uint8_t)dz; o.circular = (uint8_t)circ; o.smooth = (uint8_t)smooth;
            printf("C %d\n", km_pad_set_config(&pad, c, &o) ? 1 : 0);
        } else if (strcmp(cmd, "R") == 0) {
            km_pad_reset(&pad);
            printf("R\n");
        } else if (strcmp(cmd, "U") == 0) {
            unsigned now, allowed, epoch, n;
            km_pad_key_t keys[64];
            if (scanf("%u %u %u %u", &now, &allowed, &epoch, &n) != 4 || n > 64) return 2;
            memset(g_depth, 0, sizeof g_depth);
            for (unsigned i = 0; i < n; i++) {
                unsigned slot, code, a0, a1, depth;
                if (scanf("%u %u %u %u %u", &slot, &code, &a0, &a1, &depth) != 5) return 2;
                keys[i].slot = (uint8_t)slot; keys[i].code = (uint8_t)code; keys[i].arg0 = (uint8_t)a0; keys[i].arg1 = (uint8_t)a1;
                if (slot < KM_PAD_SLOTS) g_depth[slot] = (uint16_t)depth;
            }
            km_pad_in_t in = { keys, n, allowed != 0u, epoch, now, depth_cb, NULL };
            km_pad_report_t rep;
            uint8_t bytes[KM_PAD_REPORT_LEN];
            const bool re = km_pad_update(&pad, &in, &rep);
            km_pad_report_pack(&rep, bytes);
            printf("U %d ", re ? 1 : 0);
            for (unsigned i = 0; i < KM_PAD_REPORT_LEN; i++) printf("%02x", bytes[i]);
            printf("\n");
        } else if (strcmp(cmd, "V") == 0) {
            unsigned v[4];
            if (scanf("%u %u %u %u", &v[0], &v[1], &v[2], &v[3]) != 4) return 2;
            km_pad_curve_t c = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3] };
            printf("V %d", km_pad_curve_valid(&c) ? 1 : 0);
            for (unsigned d = 0; d <= 340u; d++) printf(" %d", (int)km_pad_curve_eval(&c, (uint16_t)d));
            printf("\n");
        } else if (strcmp(cmd, "D") == 0) {
            unsigned i;
            km_pad_curve_t c;
            if (scanf("%u", &i) != 1) return 2;
            km_pad_default_curve(i, &c);
            printf("D %u %u %u %u\n", c.start_mm100_5, c.end_mm100_5, c.shape, c.flags);
        } else if (strcmp(cmd, "E") == 0) {
            unsigned code, a0;
            if (scanf("%u %u", &code, &a0) != 2) return 2;
            int max = -1, gap = 0;
            for (unsigned a1 = 0; a1 < 256u; a1++) {
                if (km_pad_entry_valid((uint8_t)code, (uint8_t)a0, (uint8_t)a1)) { if (max != (int)a1 - 1) gap = 1; max = (int)a1; }
            }
            printf("E %d %d\n", max, gap);
        } else if (strcmp(cmd, "O") == 0) {
            unsigned dz, circ, smooth, rs;
            if (scanf("%u %u %u %u", &dz, &circ, &smooth, &rs) != 4) return 2;
            km_pad_opts_t o;
            memset(&o, 0, sizeof o);
            o.stick_deadzone = (uint8_t)dz; o.circular = (uint8_t)circ; o.smooth = (uint8_t)smooth; o.rsvd[0] = (uint8_t)rs;
            printf("O %d\n", km_pad_opts_valid(&o) ? 1 : 0);
        } else {
            return 3;
        }
    }
    return 0;
}
"""


def find_compiler():
    for cand in ("gcc", "cc", "clang"):
        path = shutil.which(cand)
        if path:
            return path
    return None


def build(core: Path, tmp: Path) -> Path:
    exe = tmp / ("pad_harness.exe" if os.name == "nt" else "pad_harness")
    (tmp / "harness.c").write_text(HARNESS)
    cc = find_compiler()
    if not cc:
        sys.exit("no C compiler found (gcc / cc / clang)")
    cmd = [cc, "-std=c11", "-O1", "-Wall", "-I", str(core / "include"), str(tmp / "harness.c"), str(core / "src" / "km_pad.c"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("compile failed:\n" + r.stdout + r.stderr)
    return exe


def run(exe: Path, lines):
    r = subprocess.run([str(exe)], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness failed (%d): %s" % (r.returncode, r.stderr))
    return r.stdout.strip().split("\n")


# ---- scenarios -------------------------------------------------------------------------------------------------------------

def rand_curve(rng):
    start = rng.choice([4, 4, 5, 6, 8, 12, 20, 40, 60, 64])
    end = rng.choice([start + 4, start + 5, start + 10, 40, 64, 68, 68])
    end = max(start + 4, min(68, end))
    if start + 4 > 68:
        start, end = 64, 68
    return [start, end, rng.randrange(4), rng.randrange(2)]


def rand_binding(rng):
    kind = rng.random()
    if kind < 0.55:
        axis = rng.randrange(6)
        neg = 1 if axis < 4 and rng.random() < 0.5 else 0
        digital = 1 if rng.random() < 0.15 else 0
        return [0xEF, axis | (neg << 3) | (digital << 4), rng.randrange(4)]
    if kind < 0.8:
        return [0xEE, rng.randrange(1, 17), rng.choice([0, 0, 0, 4, 6, 10, 20, 40, 68])]
    return [0xEE, rng.randrange(17, 21), rng.choice([0, 0, 4, 10])]


def make_scenario(rng, n_steps, many=False):
    curves = [rand_curve(rng) for _ in range(4)]
    invalid_curve = rng.random() < 0.15
    if invalid_curve:
        curves[rng.randrange(4)] = [0, 0, 0, 0]
    opts = [rng.choice([0, 0, 5, 10, 25, 50]), rng.randrange(2), rng.randrange(4)]
    pool_slots = rng.sample(range(SLOTS), 24 if many else rng.randrange(6, 15))
    binds = {s: rand_binding(rng) for s in pool_slots}
    depth = {s: 0 for s in pool_slots}
    held = {s: False for s in pool_slots}
    now = rng.choice([0, 1000, 0xFFFFFF00])
    epoch = 0
    steps = []
    for _ in range(n_steps):
        now = (now + rng.choice([0, 1, 1, 1, 2, 3])) & 0xFFFFFFFF
        allowed = 0 if rng.random() < 0.03 else 1
        if rng.random() < 0.02:
            epoch += 1
        keys = []
        for s in pool_slots:
            if rng.random() < 0.12:
                held[s] = not held[s]
                if not held[s]:
                    depth[s] = 0
            if held[s]:
                depth[s] = max(0, min(340, depth[s] + rng.choice([-30, -8, -1, 0, 1, 5, 20, 80, 340])))
                if depth[s] >= 0:
                    keys.append([s] + binds[s] + [depth[s]])
        if rng.random() < 0.05:   # entries the core must ignore
            keys.append([rng.choice([76, 90, 200]), 0xEF, 0, 0, 100])
            keys.append([rng.randrange(SLOTS), rng.choice([0x04, 0xEC, 0xED]), 1, 0, 100])
            keys.append([rng.randrange(SLOTS), 0xEF, 0x28, 0, 100])        # reserved bits of the axis argument
        rng.shuffle(keys)
        steps.append([now, allowed, epoch, keys])
    return {"curves": curves, "opts": opts, "steps": steps, "reset_at": [i for i in range(n_steps) if rng.random() < 0.01]}


def generate(exe: Path, padmap: Path):
    rng = random.Random(6)
    out = {"note": "generated by ui/tests/vectors/gen_pad_vectors.py from the real km_pad.c; do not edit"}

    # curves: a grid
    grid = []
    for start in (4, 5, 8, 20, 40, 64):
        for span in (4, 5, 12, 60):
            end = start + span
            if end > 68:
                continue
            for shape in range(4):
                for flags in (0, 1):
                    grid.append([start, end, shape, flags])
    grid += [[4, 64, 0, 0], [4, 40, 0, 0], [4, 64, 1, 0], [4, 64, 3, 0], [4, 68, 2, 1], [0, 0, 0, 0], [3, 64, 0, 0], [4, 7, 0, 0],
             [4, 69, 0, 0], [4, 64, 4, 0], [4, 64, 0, 2], [67, 68, 0, 0], [10, 10, 0, 0], [68, 68, 0, 0]]
    res = run(exe, ["V %d %d %d %d" % tuple(c) for c in grid])
    out["curves"] = []
    for c, line in zip(grid, res):
        parts = line.split()
        out["curves"].append({"curve": c, "valid": parts[1] == "1", "values": [int(x) for x in parts[2:]]})

    res = run(exe, ["D %d" % i for i in range(8)])
    out["defaults"] = [[int(x) for x in line.split()[1:]] for line in res]

    # table entries: the largest valid arg1 per (code, arg0); validity along arg1 is contiguous from 0
    q = ["E %d %d" % (code, a0) for code in (0xEE, 0xEF) for a0 in range(256)]
    res = run(exe, q)
    entry = {"238": {}, "239": {}}
    for line, (code, a0) in zip(res, [(c, a) for c in (0xEE, 0xEF) for a in range(256)]):
        _, mx, gap = line.split()
        assert gap == "0", "valid arg1 values of 0x%02X/%d are not contiguous" % (code, a0)
        if int(mx) >= 0:
            entry[str(code)][str(a0)] = int(mx)
    out["entry_max_arg1"] = entry

    # options validity
    optq = [(dz, c, sm, rs) for dz in (0, 10, 50, 51, 255) for c in (0, 1, 2) for sm in (0, 3, 4) for rs in (0, 1)]
    res = run(exe, ["O %d %d %d %d" % o for o in optq])
    out["opts_valid"] = [[list(o), line.split()[1] == "1"] for o, line in zip(optq, res)]

    # key sequences
    scenarios = [make_scenario(rng, 220) for _ in range(34)] + [make_scenario(rng, 160, many=True) for _ in range(2)]
    dump = []
    for sc in scenarios:
        lines = ["I", "C " + " ".join(" ".join(str(v) for v in c) for c in sc["curves"]) + " " + " ".join(str(v) for v in sc["opts"])]
        order = []
        for i, (now, allowed, epoch, keys) in enumerate(sc["steps"]):
            if i in sc["reset_at"]:
                lines.append("R")
                order.append("R")
            lines.append("U %d %d %d %d %s" % (now, allowed, epoch, len(keys), " ".join(" ".join(str(v) for v in k) for k in keys)))
            order.append("U")
        res = run(exe, lines)
        cfg_ok = res[0].split()[1] == "1"
        results = [r for r, o in zip(res[1:], order) if o == "U"]
        dump.append({
            "curves": sc["curves"], "opts": sc["opts"], "config_ok": cfg_ok, "reset_at": sc["reset_at"],
            "steps": [{"now": s[0], "allowed": s[1], "epoch": s[2], "keys": s[3]} for s in sc["steps"]],
            "out": [{"recomputed": r.split()[1] == "1", "report": r.split()[2]} for r in results],
        })
    out["scenarios"] = dump

    # SDL mapping lines from the Python tool
    sdl = {}
    for plat in ("windows", "linux", "mac"):
        r = subprocess.run([sys.executable, "-I", str(padmap), "sdl", "--platform", plat], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("magger60_pad_map.py failed: " + r.stderr)
        sdl[plat] = r.stdout.strip()
    out["sdl"] = sdl
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--core", type=Path, default=REPO / "firmware" / "core")
    ap.add_argument("--padmap", type=Path, default=REPO / "tools" / "magger60_pad_map.py")
    a = ap.parse_args()
    for var in ("PATH",):   # MSYS2 first on Windows, as in run_tests.py
        if os.name == "nt" and Path("C:/msys64/ucrt64/bin").exists():
            os.environ[var] = "C:/msys64/ucrt64/bin" + os.pathsep + os.environ[var]
    if not (a.core / "src" / "km_pad.c").exists():
        sys.exit("%s has no src/km_pad.c: pass --core <dir of the gamepad core>" % a.core)
    with tempfile.TemporaryDirectory() as d:
        exe = build(a.core, Path(d))
        data = generate(exe, a.padmap)
    text = json.dumps(data, separators=(",", ":")) + "\n"
    if a.check:
        if not OUT.exists() or OUT.read_text() != text:
            print("pad_vectors.json is stale: run ui/tests/vectors/gen_pad_vectors.py")
            return 1
        print("pad_vectors.json is up to date")
        return 0
    OUT.write_text(text)
    print("wrote %s (%d bytes, %d curves, %d scenarios)" % (OUT, len(text), len(data["curves"]), len(data["scenarios"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
