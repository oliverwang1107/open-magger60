#!/usr/bin/env python3
"""Mutation pass for the gamepad core (M6 package P4: core/src/km_pad.c, tests/test_pad.c).

    python firmware/tools/mutate_pad.py            # run every mutation, list survivors
    python firmware/tools/mutate_pad.py --list     # just print them

Same idea as mutate_health.py: each mutation changes one expression in a scratch copy of km_pad.c, rebuilds tests/test_pad.c
with a tiny main (test_pad() is not registered in test_main.c until the integration package) and runs it. "killed" = the
build works and a check fails; a "survivor" means the tests do not notice the change.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
P = "core/src/km_pad.c"
SRC = [P, "core/src/km_engine.c", "core/src/km_depth_model.c", "core/src/km_keymap.c"]

# (name, old text, new text); `old` must occur exactly once in km_pad.c.
MUTATIONS = [
    # ---- validity ----
    ("curve start may be below 0.20 mm", "c->start_mm100_5 >= KM_PAD_MIN_START_MM100 / 5u", "c->start_mm100_5 >= 3u"),
    ("curve may be 0.05 mm wide", "c->end_mm100_5 >= c->start_mm100_5 + 2u", "c->end_mm100_5 >= c->start_mm100_5 + 1u"),
    ("curve end beyond the travel", "c->end_mm100_5 <= 68u", "c->end_mm100_5 <= 69u"),
    ("curve shape 4 accepted", "c->shape <= KM_PAD_SHAPE_S", "c->shape <= 4u"),
    ("curve flags not checked", " &&\n           (c->flags & (uint8_t)~KM_PAD_CURVE_MIRROR) == 0u;", ";"),
    ("dead zone 51 accepted", "o->stick_deadzone > 50u", "o->stick_deadzone > 51u"),
    ("circular 2 accepted", "o->circular > 1u", "o->circular > 2u"),
    ("smooth 4 accepted", "o->smooth > 3u", "o->smooth > 4u"),
    ("reserved option bytes not checked", "if (o->rsvd[i] != 0u) {", "if (0) {"),
    ("button 0 accepted", "return arg0 >= 1u && arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 68u;", "return arg0 >= 0u && arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 68u;"),
    ("button 21 accepted", "arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 68u", "arg0 <= KM_PAD_BTN_HAT_RIGHT + 1u && arg1 <= 68u"),
    ("activation point 69 accepted", "arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 68u", "arg0 <= KM_PAD_BTN_HAT_RIGHT && arg1 <= 69u"),
    ("axis argument reserved bits not checked", "(arg0 & 0xE0u) != 0u || axis >= KM_PAD_AXES", "axis >= KM_PAD_AXES"),
    ("axis 6 accepted", "axis >= KM_PAD_AXES ||", "axis > KM_PAD_AXES ||"),
    ("curve index 4 accepted", "arg1 >= KM_PAD_CURVES) {", "arg1 > KM_PAD_CURVES) {"),
    ("negative trigger accepted", "return axis <= KM_PAD_AXIS_RY || (arg0 & KM_PAD_AXIS_NEG) == 0u;", "return true;"),
    # ---- curve shapes ----
    ("slow-then-fast shape is linear", "return (t * t) >> 15;", "return t;"),
    ("fast-then-slow shape is wrong", "return SCALE15 - ((u * u + (SCALE15 - 1u)) >> 15);", "return (t * t) >> 15;"),
    ("S shape is linear", "return (uint32_t)(((uint64_t)t * t * (3u * SCALE15 - 2u * t)) >> 30);", "return t;"),
    ("S shape not 3t^2-2t^3", "(3u * SCALE15 - 2u * t)", "(2u * SCALE15 - t)"),
    ("curve is already full at its end", "if (d >= e) {", "if (d > e) {"),
    ("curve starts to rise at its start", "if (d <= s) {", "if (d < s) {"),
    ("curve scaled over the wrong width", "uint32_t t = ((d - s) << 15) / (e - s);", "uint32_t t = ((d - s) << 15) / (e - s + 1u);"),
    ("mirror flag ignored", "y = SCALE15 - shape_q15(c->shape, SCALE15 - t);", "y = shape_q15(c->shape, t);"),
    ("no rounding to 32767", "return (int32_t)((y * (uint32_t)MAXV + (SCALE15 / 2u)) >> 15);", "return (int32_t)((y * (uint32_t)MAXV) >> 15);"),
    ("curve index ignored", "km_pad_curve_eval(&p->curve[k->arg1],", "km_pad_curve_eval(&p->curve[0],"),
    # ---- report ----
    ("hat not limited when packed", "out[2] = r->hat <= 8u ? r->hat : (uint8_t)0;", "out[2] = r->hat;"),
    ("buttons high byte missing", "put16(out + 0, r->buttons);", "out[0] = (uint8_t)r->buttons; out[1] = 0;"),
    ("axes swapped in the report", "put16(out + 5, (uint16_t)r->ly);\n    put16(out + 7, (uint16_t)r->rx);", "put16(out + 7, (uint16_t)r->ly);\n    put16(out + 5, (uint16_t)r->rx);"),
    ("triggers swapped in the report", "put16(out + 11, r->lt);\n    put16(out + 13, r->rt);", "put16(out + 13, r->lt);\n    put16(out + 11, r->rt);"),
    ("low byte first is wrong", "p[0] = (uint8_t)(v & 0xFFu);\n    p[1] = (uint8_t)(v >> 8);", "p[1] = (uint8_t)(v & 0xFFu);\n    p[0] = (uint8_t)(v >> 8);"),
    ("unpack keeps a hat above 8", "if (r->hat > 8u) {\n        r->hat = 0u;\n    }", ""),
    ("unpack ignores the hat nibble mask", "r->hat = (uint8_t)(in[2] & 0x0Fu);", "r->hat = in[2];"),
    ("equality compares only buttons", "return memcmp(x, y, KM_PAD_REPORT_LEN) == 0;", "return memcmp(x, y, 2) == 0;"),
    ("equality ignores the last byte", "return memcmp(x, y, KM_PAD_REPORT_LEN) == 0;", "return memcmp(x, y, KM_PAD_REPORT_LEN - 1u) == 0;"),
    ("neutral ignores the last byte", "for (unsigned i = 0; i < KM_PAD_REPORT_LEN; i++) {\n        if (x[i] != 0u) {", "for (unsigned i = 0; i < KM_PAD_REPORT_LEN - 1u; i++) {\n        if (x[i] != 0u) {"),
    # ---- config / reset ----
    ("bad curve kept", "        if (curves != NULL && km_pad_curve_valid(&curves[i])) {", "        if (curves != NULL) {"),
    ("bad options kept", "if (opts != NULL && km_pad_opts_valid(opts)) {", "if (opts != NULL) {"),
    ("config change does not release", "    km_pad_reset(p);\n    return all_ok;", "    return all_ok;"),
    ("reset does not release", "    memset(&p->cur, 0, sizeof p->cur);\n    memset(p->raw, 0, sizeof p->raw);\n    memset(p->blocked, 0, sizeof p->blocked);", "    memset(p->blocked, 0, sizeof p->blocked);"),
    ("reset does not re-arm", "    memset(p->btn_on, 0, sizeof p->btn_on);\n    p->rearm = true;\n    p->resets++;", "    memset(p->btn_on, 0, sizeof p->btn_on);\n    p->resets++;"),
    ("init does not re-arm", "    load_defaults(p);\n    p->rearm = true;", "    load_defaults(p);"),
    # ---- update ----
    ("fault does not release", "        memset(&p->cur, 0, sizeof p->cur);\n        memset(p->raw, 0, sizeof p->raw);\n        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->rearm = true;\n        p->epoch = in->epoch;", "        memset(p->raw, 0, sizeof p->raw);\n        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->rearm = true;\n        p->epoch = in->epoch;"),
    ("fault does not clear the stick memory", "        memset(&p->cur, 0, sizeof p->cur);\n        memset(p->raw, 0, sizeof p->raw);\n        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->rearm = true;\n        p->epoch = in->epoch;", "        memset(&p->cur, 0, sizeof p->cur);\n        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->rearm = true;\n        p->epoch = in->epoch;"),
    ("fault does not hold keys over", "        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->rearm = true;\n        p->epoch = in->epoch;", "        memset(p->btn_on, 0, sizeof p->btn_on);\n        p->epoch = in->epoch;"),
    ("fault counted at every pass", "        if (!p->rearm) {\n            p->faults++;\n        }", "        p->faults++;"),
    ("epoch change ignored", "if (p->started && in->epoch != p->epoch) {", "if (0) {"),
    ("epoch not remembered", "    p->epoch = in->epoch;\n    if (p->started && !p->rearm", "    if (p->started && !p->rearm"),
    ("cadence one millisecond too long", "(uint32_t)(in->now_ms - p->last_ms) < KM_PAD_PERIOD_MS", "(uint32_t)(in->now_ms - p->last_ms) <= KM_PAD_PERIOD_MS"),
    ("no cadence", "if (p->started && !p->rearm && (uint32_t)(in->now_ms - p->last_ms) < KM_PAD_PERIOD_MS) {", "if (0) {"),
    ("cadence delays a re-arm", "if (p->started && !p->rearm && (uint32_t)", "if (p->started && (uint32_t)"),
    ("slot range not checked", "use[i] = k->slot < KM_PAD_SLOTS && km_pad_entry_valid(k->code, k->arg0, k->arg1);", "use[i] = km_pad_entry_valid(k->code, k->arg0, k->arg1);"),
    ("entry validity not checked", "use[i] = k->slot < KM_PAD_SLOTS && km_pad_entry_valid(k->code, k->arg0, k->arg1);", "use[i] = k->slot < KM_PAD_SLOTS;"),
    ("key limit not applied", "(in->n < KM_PAD_MAX_KEYS ? in->n : KM_PAD_MAX_KEYS)", "in->n"),
    ("ignored keys not counted", "        } else {\n            p->ignored_keys++;\n        }", "        }"),
    ("extra keys not counted", "        p->ignored_keys += in->n - n;", "        (void)0;"),
    ("held-over keys not remembered", "            p->blocked[w] = listed[w];", "            p->blocked[w] = 0u;"),
    ("held-over keys not ignored", "        if (use[i] && bit_get(p->blocked, in->keys[i].slot)) {", "        if (0) {"),
    ("held-over keys never freed", "        p->blocked[w] &= listed[w];\n", ""),
    ("button memory kept for unlisted keys", "        p->btn_on[w] &= listed[w];\n", ""),
    # ---- bindings ----
    ("activation point not raised to 0.20 mm", "                if (act < KM_PAD_BTN_MIN_ACT_MM100) {\n                    act = KM_PAD_BTN_MIN_ACT_MM100;\n                }", ""),
    ("button hysteresis missing", "on = bit_get(p->btn_on, k->slot) ? d + hyst >= act : d >= act;", "on = d >= act;"),
    ("button hysteresis one unit short", "d + hyst >= act :", "d + hyst > act :"),
    ("button hysteresis one unit long", "d + hyst >= act :", "d + hyst + 1u >= act :"),
    ("button activates one unit late", ": d >= act;", ": d > act;"),
    ("button memory not saved", "                if (on) {\n                    bit_set(p->btn_on, k->slot);", "                if (on) {\n                    (void)0;"),
    ("button memory not cleared", "                } else {\n                    bit_clear(p->btn_on, k->slot);\n                }", "                }"),
    ("engine-state button keeps old memory", "            if (k->arg1 == 0u) {\n                bit_clear(p->btn_on, k->slot);\n            } else {", "            if (k->arg1 == 0u) {\n                (void)0;\n            } else {"),
    ("button bit off by one", "buttons = (uint16_t)(buttons | (1u << (k->arg0 - 1u)));", "buttons = (uint16_t)(buttons | (1u << k->arg0));"),
    ("hat direction bits shifted", "hatbits |= 1u << (k->arg0 - KM_PAD_BTN_HAT_UP);", "hatbits |= 1u << (k->arg0 - KM_PAD_BTN_HAT_UP + 1u);"),
    ("digital axis follows the curve", "(k->arg0 & KM_PAD_AXIS_DIGITAL) != 0u\n                                  ? MAXV", "0\n                                  ? MAXV"),
    ("negative bit ignored", "int32_t *dst = ((k->arg0 & KM_PAD_AXIS_NEG) != 0u) ? &neg[axis] : &pos[axis];", "int32_t *dst = &pos[axis];"),
    ("shallower key wins", "            if (v > *dst) {", "            if (v < *dst) {"),
    ("keys of one direction add up", "            if (v > *dst) {\n                *dst = v;\n            }", "            *dst += v;"),
    ("hat up and down do not cancel", "((bits & 2u) != 0u ? 1 : 0) - ((bits & 1u) != 0u ? 1 : 0)", "((bits & 2u) != 0u ? 1 : 0)"),
    ("hat left and right do not cancel", "((bits & 8u) != 0u ? 1 : 0) - ((bits & 4u) != 0u ? 1 : 0)", "((bits & 8u) != 0u ? 1 : 0)"),
    ("hat NE and SE swapped", "{ 8u, 1u, 2u },\n    { 7u, 0u, 3u },\n    { 6u, 5u, 4u },", "{ 8u, 1u, 4u },\n    { 7u, 0u, 3u },\n    { 6u, 5u, 2u },"),
    # ---- sticks ----
    ("opposing keys do not subtract", "p->raw[a] = (int16_t)out_hyst(p->raw[a], pos[a] - neg[a]);", "p->raw[a] = (int16_t)out_hyst(p->raw[a], pos[a] > neg[a] ? pos[a] : -neg[a]);"),
    ("band acts after the circle", "        v[a] = p->raw[a];", "        v[a] = pos[a] - neg[a];"),
    ("circle limit never applied", "if (circular && r2 > max2) {", "if (0) {"),
    ("circle applied to every deflection", "if (circular && r2 > max2) {", "if (circular) {"),
    ("circle rounded down", "return r * r < n ? r + 1u : r;", "return r;"),
    ("circle limit larger than the range", "*x = *x * MAXV / r;", "*x = *x * (MAXV + 5) / r;"),
    ("dead zone does not zero its edge", "if (r2 <= dz * dz) {", "if (r2 < dz * dz) {"),
    ("dead zone not rescaled", "const uint32_t rn = (r - dz) * (uint32_t)MAXV / ((uint32_t)MAXV - dz);", "const uint32_t rn = r;"),
    ("dead zone not clamped", "    return v > MAXV ? MAXV : (v < -MAXV ? -MAXV : v);", "    return v;"),
    ("dead zone percent wrong", "const uint32_t dz = deadzone_pct * (uint32_t)MAXV / 100u;", "const uint32_t dz = deadzone_pct * (uint32_t)MAXV / 50u;"),
    ("band follows at once", "(d >= KM_PAD_OUT_HYST || d <= -KM_PAD_OUT_HYST) ? v : prev", "v"),
    ("band one unit too wide", "(d >= KM_PAD_OUT_HYST || d <= -KM_PAD_OUT_HYST)", "(d > KM_PAD_OUT_HYST || d < -KM_PAD_OUT_HYST)"),
    ("band holds zero", "if (v == 0 || v == MAXV || v == -MAXV || (v < 0) != (prev < 0) || prev == 0) {", "if (v == MAXV || v == -MAXV || (v < 0) != (prev < 0) || prev == 0) {"),
    ("band holds full scale", "if (v == 0 || v == MAXV || v == -MAXV ||", "if (v == 0 ||"),
    ("band holds a change of sign", "(v < 0) != (prev < 0) || prev == 0", "prev == 0"),
    ("triggers without the band", "r->lt = (uint16_t)out_hyst((int32_t)r->lt, pos[KM_PAD_AXIS_LT]);", "r->lt = (uint16_t)pos[KM_PAD_AXIS_LT];"),
    ("left and right stick mixed up", "stick(&v[2], &v[3], p->opts.stick_deadzone, p->opts.circular != 0u);", "stick(&v[1], &v[3], p->opts.stick_deadzone, p->opts.circular != 0u);"),
    ("depth read without a callback crashes", "return in->depth != NULL ? in->depth(in->ctx, slot) : 0u;", "return in->depth(in->ctx, slot);"),
]

EQUIVALENT = {
    "curve is already full at its end": "at d == end the formula itself gives full scale; the early return only guards the divisor",
    "curve starts to rise at its start": "at d == start the formula itself gives 0",
    "dead zone does not zero its edge": "at r == dead zone the rescale gives exactly 0 as well",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


STUB = r'''
#include <stdio.h>
#include "km_test.h"
int km_test_checks, km_test_failures;
void test_pad(void);
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_pad();
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
'''


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", help="run only mutations whose name contains this text")
    args = ap.parse_args()
    if args.list:
        for i, m in enumerate(MUTATIONS, 1):
            print("%3d %s" % (i, m[0]))
        return 0

    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_pad_"))
    (work / "main.c").write_text(STUB)
    survivors, broken = [], []
    try:
        for i, (name, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests", ignore=shutil.ignore_patterns("golden"))
            target = tree / P
            text = target.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%3d %-52s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"),
                   str(work / "main.c"), str(tree / "tests/test_pad.c"), *[str(tree / s) for s in SRC], "-o", str(exe)]
            r = subprocess.run(cmd, env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%3d %-52s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                broken.append(name)
                continue
            try:
                r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=120)
                rc, out = r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                rc, out = 1, "(timeout)"
            if rc == 0 and name in EQUIVALENT:
                print("%3d %-52s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif rc == 0:
                print("%3d %-52s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%3d %-52s killed  %s" % (i, name, first[-70:]))
            shutil.rmtree(tree, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
