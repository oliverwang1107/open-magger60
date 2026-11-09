#!/usr/bin/env python3
"""Mutation pass for the M6 P1 SOCD filter (core/src/km_socd.c).

    python firmware/tools/mutate_socd.py            # run every mutation, list survivors
    python firmware/tools/mutate_socd.py --list     # just print them
    python firmware/tools/mutate_socd.py --only hyst

Same idea as mutate_health.py / mutate_engine.py: each mutation changes one expression in a scratch copy of the sources,
rebuilds tests/test_socd.c (with a tiny main, so it does not depend on the suite registration in test_main.c) and runs it.
"killed" = the build works and a check fails (or the program crashes / hangs); a "survivor" means the tests do not notice
the change and the test suite has a hole.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SRC = ["core/src/km_socd.c", "core/src/km_keymap.c", "core/src/km_engine.c", "core/src/km_depth_model.c"]
F = "core/src/km_socd.c"

# (name, old text, new text); `old` must occur exactly once in km_socd.c.
MUTATIONS = [
    # ---- order-based modes ----
    ("LAST: the later key loses", "mark(supp, a_first ? sa : sb);     /* the earlier key loses */", "mark(supp, a_first ? sb : sa);"),
    ("FIRST: the earlier key loses", "mark(supp, a_first ? sb : sa);     /* the later key loses */", "mark(supp, a_first ? sa : sb);"),
    ("NEUTRAL: only key A is held back", "                mark(supp, sa);\n                mark(supp, sb);\n                break;", "                mark(supp, sa);\n                break;"),
    ("NEUTRAL: only key B is held back", "                mark(supp, sa);\n                mark(supp, sb);\n                break;", "                mark(supp, sb);\n                break;"),
    ("press order reversed", "const unsigned a_first = pa < pb;", "const unsigned a_first = pa > pb;"),
    ("position of a repeated entry: last place counts", "if (r != 0u && pos[r - 1u] == NONE) {", "if (r != 0u) {"),
    ("positions start as 0 instead of 'not down'", "memset(pos, NONE, sizeof pos);", "memset(pos, 0, sizeof pos);"),
    ("conflict needs only one key down", "if (pa == NONE || pb == NONE ||", "if (pa == NONE && pb == NONE ||"),
    ("conflict when A is down", "if (pa == NONE || pb == NONE ||", "if (pa == NONE ||"),
    ("conflict when B is down", "if (pa == NONE || pb == NONE ||", "if (pb == NONE ||"),
    ("list scan not capped to the byte range", "const unsigned scan = n_in < NONE ? n_in : NONE;", "const unsigned scan = n_in;"),
    ("list scan one entry short", "const unsigned scan = n_in < NONE ? n_in : NONE;", "const unsigned scan = n_in < NONE ? n_in - 1u : NONE;"),
    ("slots >= 76 are scanned as keys", "            if (slot < KM_SOCD_SLOTS) {\n                const unsigned r = s->role[slot];", "            if (slot < 255u) {\n                const unsigned r = s->role[slot];"),
    # ---- deeper wins ----
    ("DEEPER: hysteresis for B takes '>' not '>='", "if (db >= da + KM_SOCD_DEEPER_HYST_MM100) {", "if (db > da + KM_SOCD_DEEPER_HYST_MM100) {"),
    ("DEEPER: hysteresis for A takes '>' not '>='", "} else if (da >= db + KM_SOCD_DEEPER_HYST_MM100) {", "} else if (da > db + KM_SOCD_DEEPER_HYST_MM100) {"),
    ("DEEPER: no hysteresis for B", "if (db >= da + KM_SOCD_DEEPER_HYST_MM100) {", "if (db > da) {"),
    ("DEEPER: no hysteresis for A", "} else if (da >= db + KM_SOCD_DEEPER_HYST_MM100) {", "} else if (da > db) {"),
    ("DEEPER: hysteresis one unit too wide", "if (db >= da + KM_SOCD_DEEPER_HYST_MM100) {", "if (db >= da + KM_SOCD_DEEPER_HYST_MM100 + 1u) {"),
    ("DEEPER: A never takes over again", "} else if (da >= db + KM_SOCD_DEEPER_HYST_MM100) {\n                        s->owner[p] = 1u;\n                    }", "}"),
    ("DEEPER: B never takes over", "                        if (db >= da + KM_SOCD_DEEPER_HYST_MM100) {\n                            s->owner[p] = 2u;\n                        }", "                        (void)0;"),
    ("DEEPER: starting winner is the later key", "s->owner[p] = a_first ? 1u : 2u;", "s->owner[p] = a_first ? 2u : 1u;"),
    ("DEEPER: starting winner is always A", "s->owner[p] = a_first ? 1u : 2u;", "s->owner[p] = 1u;"),
    ("DEEPER: winner chosen the wrong way round", "mark(supp, s->owner[p] == 1u ? sb : sa);", "mark(supp, s->owner[p] == 1u ? sa : sb);"),
    ("DEEPER: owner test swapped", "if (s->owner[p] == 1u) {\n                        if (db", "if (s->owner[p] == 2u) {\n                        if (db"),
    ("DEEPER: winner forgotten only when both keys are up", "                s->owner[p] = 0u;                  /* no conflict (or the pair sleeps on this layer) */", "                if (pa == NONE && pb == NONE) {\n                    s->owner[p] = 0u;\n                }"),
    ("DEEPER: winner kept when the list is empty", "        memset(s->owner, 0, sizeof s->owner);      /* fewer than two keys pressed: no conflict can be running */", "        (void)0;"),
    ("DEEPER: winner kept while the pair sleeps on a layer", "                s->owner[p] = 0u;                  /* no conflict (or the pair sleeps on this layer) */", "                if (pa == NONE || pb == NONE) {\n                    s->owner[p] = 0u;\n                }"),
    ("DEEPER: depth callback called without a check", "if (depth != NULL) {", "if (1) {"),
    ("DEEPER: depths swapped", "const unsigned da = depth(ctx, sa);\n                    const unsigned db = depth(ctx, sb);", "const unsigned da = depth(ctx, sb);\n                    const unsigned db = depth(ctx, sa);"),
    ("DEEPER: depth of the wrong key (A twice)", "const unsigned db = depth(ctx, sb);", "const unsigned db = depth(ctx, sa);"),
    # ---- layer flag ----
    ("layer flag ignored", "((s->flags[p] & KM_SOCD_FLAG_LAYER0) != 0u && layer != 0u)", "0"),
    ("layer flag always on", "((s->flags[p] & KM_SOCD_FLAG_LAYER0) != 0u && layer != 0u)", "(layer != 0u)"),
    ("layer flag: layer 1 still counts as base", "((s->flags[p] & KM_SOCD_FLAG_LAYER0) != 0u && layer != 0u)", "((s->flags[p] & KM_SOCD_FLAG_LAYER0) != 0u && layer > 1u)"),
    # ---- output list ----
    ("suppressed slot test uses the wrong word", "if (slot < KM_SOCD_SLOTS && ((supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u) {\n            continue;", "if (slot < KM_SOCD_SLOTS && ((supp[0] >> (slot & 31u)) & 1u) != 0u) {\n            continue;"),
    ("suppressed slot test uses the wrong bit", "if (slot < KM_SOCD_SLOTS && ((supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u) {\n            continue;", "if (slot < KM_SOCD_SLOTS && ((supp[slot >> 5] >> (slot & 15u)) & 1u) != 0u) {\n            continue;"),
    ("slots >= 76 can be suppressed", "if (slot < KM_SOCD_SLOTS && ((supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u) {\n            continue;", "if (slot < 255u && ((supp[(slot >> 5) % 3u] >> (slot & 31u)) & 1u) != 0u) {\n            continue;"),
    ("mark sets the wrong word", "supp[slot >> 5] |= 1u << (slot & 31u);", "supp[slot >> 6] |= 1u << (slot & 31u);"),
    ("mark sets the wrong bit", "supp[slot >> 5] |= 1u << (slot & 31u);", "supp[slot >> 5] |= 1u << (slot & 15u);"),
    ("output list not copied when nothing is suppressed", "            memcpy(down_out, down_in, n_in);", "            (void)0;"),
    ("output list returns the input length", "        down_out[w++] = (uint8_t)slot;\n    }\n    return w;", "        down_out[w++] = (uint8_t)slot;\n    }\n    return n_in;"),
    ("output writes without advancing", "        down_out[w++] = (uint8_t)slot;", "        down_out[w] = (uint8_t)slot;"),
    ("suppression state kept from the last call (not rebuilt)", "    s->supp[0] = supp[0];\n    s->supp[1] = supp[1];\n    s->supp[2] = supp[2];", "    s->supp[0] |= supp[0];\n    s->supp[1] |= supp[1];\n    s->supp[2] |= supp[2];"),
    ("km_socd_suppressed ignores the word", "return slot < KM_SOCD_SLOTS && ((s->supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u;", "return slot < KM_SOCD_SLOTS && ((s->supp[0] >> (slot & 31u)) & 1u) != 0u;"),
    # ---- counters ----
    ("event counter counts every call", "popcount_small(supp[0] & ~s->supp[0]) + popcount_small(supp[1] & ~s->supp[1]) +\n                          popcount_small(supp[2] & ~s->supp[2]);", "popcount_small(supp[0]) + popcount_small(supp[1]) + popcount_small(supp[2]);"),
    ("event counter ignores word 2", "popcount_small(supp[0] & ~s->supp[0]) + popcount_small(supp[1] & ~s->supp[1]) +\n                          popcount_small(supp[2] & ~s->supp[2]);", "popcount_small(supp[0] & ~s->supp[0]) + popcount_small(supp[1] & ~s->supp[1]);"),
    ("popcount counts twice", "        c++;\n    }\n    return c;", "        c += 2u;\n    }\n    return c;"),
    ("conflict counter not kept", "            s->calls_in_conflict++;", "            (void)0;"),
    # ---- configuration ----
    ("pair validity: mode limit too high", "p->mode > KM_SOCD_MODE_LAST_VALID ||", "p->mode > KM_SOCD_MODE_LAST_VALID + 1u ||"),
    ("pair validity: flags limit too high", "p->flags > KM_SOCD_FLAG_LAYER0)", "p->flags > 3u)"),
    ("pair validity: a key against itself", "return p->slot_a != p->slot_b && km_keymap_slot_is_key", "return km_keymap_slot_is_key"),
    ("pair validity: slot A may be a non-key", "km_keymap_slot_is_key(p->slot_a) && km_keymap_slot_is_key(p->slot_b);", "km_keymap_slot_is_key(p->slot_b);"),
    ("pair validity: slot B may be a non-key", "km_keymap_slot_is_key(p->slot_a) && km_keymap_slot_is_key(p->slot_b);", "km_keymap_slot_is_key(p->slot_a);"),
    ("pair validity: a mode-0 entry is checked like a used one", "    if (p->mode == KM_SOCD_OFF) {\n        return true;", "    if (p->mode == 99u) {\n        return true;"),
    ("set_pairs: NULL pairs accepted", "    if (pairs == NULL || n > KM_SOCD_MAX) {\n        return false;", "    if (n > KM_SOCD_MAX) {\n        return false;"),
    ("set_pairs: five pairs accepted", "n > KM_SOCD_MAX) {", "n > KM_SOCD_MAX + 1u) {"),
    ("set_pairs: an invalid pair does not drop the earlier ones", "            clear_config(s);                       /* the pairs loaded so far are dropped too */\n            return false;", "            return false;"),
    ("set_pairs: a slot in two pairs accepted (A side)", "if (s->role[pairs[i].slot_a] != 0u || s->role[pairs[i].slot_b] != 0u) {", "if (s->role[pairs[i].slot_b] != 0u) {"),
    ("set_pairs: a slot in two pairs accepted (B side)", "if (s->role[pairs[i].slot_a] != 0u || s->role[pairs[i].slot_b] != 0u) {", "if (s->role[pairs[i].slot_a] != 0u) {"),
    ("set_pairs: a duplicate does not drop the earlier pairs", "            clear_config(s);                       /* a slot in two pairs */\n            return false;", "            return false;"),
    ("set_pairs: mode-0 entries are used", "        if (pairs[i].mode == KM_SOCD_OFF) {\n            continue;\n        }", "        (void)0;"),
    ("set_pairs: role of key A off by one pair", "s->role[pairs[i].slot_a] = (uint8_t)(1u + 2u * u);", "s->role[pairs[i].slot_a] = (uint8_t)(2u + 2u * u);"),
    ("set_pairs: role of key B off by one", "s->role[pairs[i].slot_b] = (uint8_t)(2u + 2u * u);", "s->role[pairs[i].slot_b] = (uint8_t)(1u + 2u * u);"),
    ("set_pairs: flags not stored", "        s->flags[u] = pairs[i].flags;\n", "        s->flags[u] = 0u;\n"),
    ("set_pairs: mode not stored (always LAST)", "        s->mode[u] = pairs[i].mode;\n", "        s->mode[u] = KM_SOCD_LAST;\n"),
    ("set_pairs: slots stored swapped", "        s->slot_a[u] = pairs[i].slot_a;\n        s->slot_b[u] = pairs[i].slot_b;", "        s->slot_a[u] = pairs[i].slot_b;\n        s->slot_b[u] = pairs[i].slot_a;"),
    ("set_pairs: n_used not counted", "        s->n_used++;\n", "        s->n_used = 1u;\n"),
    ("used pairs not packed: the pair loop stops one short", "for (unsigned p = 0; p < s->n_used; p++) {", "for (unsigned p = 0; p + 1u < s->n_used; p++) {"),
    ("one-key lists are scanned as a conflict base too (n_in >= 1)", "if (s->n_used != 0u && n_in >= 2u) {", "if (s->n_used != 0u && n_in >= 1u) {"),
    ("two-key lists are skipped (n_in >= 3)", "if (s->n_used != 0u && n_in >= 2u) {", "if (s->n_used != 0u && n_in >= 3u) {"),
    ("pairs after a mode-0 entry lose their place", "        const unsigned u = s->n_used;", "        const unsigned u = i;"),
    ("reset keeps the DEEPER winners", "void km_socd_reset(km_socd_t *s)\n{\n    memset(s->owner, 0, sizeof s->owner);", "void km_socd_reset(km_socd_t *s)\n{"),
]

# Mutations that cannot change behaviour, with the reason.
EQUIVALENT = {
    "one-key lists are scanned as a conflict base too (n_in >= 1)": "pure speed-up of the original: one key can not be in conflict, the scan then finds nothing",
    "set_pairs: NULL pairs accepted": "km_socd_pair_valid(NULL) is false, so the loop refuses a NULL array with n > 0 anyway (belt and braces)",
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
size_t km_hex(const char *hex, uint8_t *out, size_t cap) { (void)hex; (void)out; (void)cap; return 0; }
void test_socd(void);
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_socd();
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
            print("%2d  %s" % (i, m[0]))
        return 0

    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_socd_"))
    (work / "main.c").write_text(STUB)
    survivors, broken = [], []
    try:
        for i, (name, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests", ignore=shutil.ignore_patterns("golden"))
            target = tree / F
            text = target.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-58s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"),
                   str(work / "main.c"), str(tree / "tests/test_socd.c"),
                   *[str(tree / s) for s in SRC], *[str(p) for p in sorted((tree / "core/gen").glob("*.c"))], "-o", str(exe)]
            r = subprocess.run(cmd, env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-58s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                broken.append(name)
                continue
            try:
                r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=120)
                rc, out = r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                rc, out = 1, "(timeout)"
            if rc == 0 and name in EQUIVALENT:
                print("%2d  %-58s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif rc == 0:
                print("%2d  %-58s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%2d  %-58s killed  %s" % (i, name, first[:70]))
            shutil.rmtree(tree, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
