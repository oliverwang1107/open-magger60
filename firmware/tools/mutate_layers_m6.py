#!/usr/bin/env python3
"""Mutation pass for the M6 P2 logic of km_layers (toggle keys, Mod-Morph, tap-hold with an ordinary key as the hold action,
the queued-release fix): change one expression at a time in a scratch copy of core/src/km_layers.c, rebuild the layer suites and
require at least one check to FAIL for every mutation.

    python firmware/tools/mutate_layers_m6.py            # baseline, then every mutation; exit 1 if one survives
    python firmware/tools/mutate_layers_m6.py --run      # only build and run the three layer suites once (no mutation)
    python firmware/tools/mutate_layers_m6.py 3 7        # only mutations 3 and 7
    python firmware/tools/mutate_layers_m6.py --list

The suites are built with a tiny main of their own (layers, layers_ext, layers_m6), so this works whether or not tests/test_main.c
already registers test_layers_m6 (that registration belongs to the integrator, task P10). The real tree is never modified.
A mutation that the tests do not notice ("SURVIVED") means a missing test: add one.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
LAY = "core/src/km_layers.c"
CORE = ["km_keymap.c", "km_layers.c", "km_report.c"]
TESTS = ["test_layers.c", "test_layers_ext.c", "test_layers_m6.c"]
MAIN = r'''
#include <stdbool.h>
#include "km_test.h"
int km_test_checks;
int km_test_failures;
size_t km_hex(const char *hex, uint8_t *out, size_t cap) { (void)hex; (void)out; (void)cap; return 0; }
void test_layers_m6(void);
static void run(const char *name, km_suite_fn fn)
{
    const int before = km_test_failures;
    fn();
    printf("%-12s %s\n", name, (km_test_failures == before) ? "ok" : "FAILED");
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    run("layers", test_layers);
    run("layers_ext", test_layers_ext);
    run("layers_m6", test_layers_m6);
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
'''

# (name, old text, new text); `old` must occur exactly once in km_layers.c.
MUTATIONS = [
    # ---- toggle keys
    ("toggle key: a press does not switch the usage off again",
     "            memmove(&g_tk[i], &g_tk[i + 1u], g_tk_n - i - 1u);\n            g_tk_n--;\n            return;",
     "            return;"),
    ("toggle key: the list is not capped",
     "    if (g_tk_n < KM_LAYERS_TOGGLE_KEYS) {\n        g_tk[g_tk_n++] = u;",
     "    if (g_tk_n < KM_LAYERS_TOGGLE_KEYS + 1u) {\n        g_tk[g_tk_n++] = u;"),
    ("toggle key: the cap is one too low",
     "    if (g_tk_n < KM_LAYERS_TOGGLE_KEYS) {\n        g_tk[g_tk_n++] = u;",
     "    if (g_tk_n + 1u < KM_LAYERS_TOGGLE_KEYS) {\n        g_tk[g_tk_n++] = u;"),
    ("toggle key: not cleared by a table change", "        g_toggled = 0;\n        g_tk_n = 0;\n        g_os_armed = 0;",
     "        g_toggled = 0;\n        g_os_armed = 0;"),
    ("toggle key: not cleared by a reset", "    g_toggled = 0;\n    g_tk_n = 0;\n    g_sup_keys = 0;", "    g_toggled = 0;\n    g_sup_keys = 0;"),
    ("toggle key: the usages are not reported", "    for (unsigned i = 0; i < g_tk_n; i++) {\n        ADD_KBD(g_tk[i]);\n    }", ""),
    ("toggle key: the usages come before the held keys",
     "    uint8_t mods = g_os_armed;\n    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        mods |= a->carry;",
     "    uint8_t mods = g_os_armed;\n    for (unsigned i = 0; i < g_tk_n; i++) {\n        ADD_KBD(g_tk[i]);\n    }\n    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        mods |= a->carry;"),
    ("toggle key: the press does not count as another key (one-shot)",
     "        /* like a layer toggle key: another key was pressed, but it does not use up an armed one-shot modifier */\n        note_press_for_osm();",
     "        /* like a layer toggle key: another key was pressed, but it does not use up an armed one-shot modifier */"),
    ("toggle key: the press uses up an armed one-shot modifier",
     "            toggle_key(e.a0);\n        }\n        act_set(slot, ACT_INERT, 0, 0);",
     "            toggle_key(e.a0);\n        }\n        act_set(slot, ACT_INERT, 0, take_armed());"),
    ("toggle key: switches on release as well", "    case ACT_PROFILE:\n        g_profile_req", "    case ACT_INERT:\n        if (g_ent[slot].code == KM_CODE_TOGGLE_KEY) {\n            toggle_key(g_ent[slot].arg0);\n        }\n        break;\n    case ACT_PROFILE:\n        g_profile_req"),
    ("toggle key: count not reported", "    out->toggle_keys = (uint8_t)g_tk_n;", "    out->toggle_keys = 0;"),
    # ---- Mod-Morph: the decision
    ("morph: record index not looked up (always record 0)", "const km_morph_rec_t *r = &g_morph[e.a1];", "const km_morph_rec_t *r = &g_morph[0];"),
    ("morph: the mask is ignored (any modifier morphs)", "raw_mods() & r->mod_mask", "raw_mods()"),
    ("morph: needs ALL bits of the mask", "(uint8_t)(raw_mods() & r->mod_mask);\n        if (hit != 0u) {", "(uint8_t)(raw_mods() & r->mod_mask);\n        if (hit != r->mod_mask) {"),
    ("morph: held modifier keys not counted", "        if (a->kind == ACT_KBD || a->kind == ACT_OSM) {\n            m |= mod_bit(a->code);\n        }", ""),
    ("morph: carried modifiers not counted", "    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        m |= a->carry;\n        if (a->kind == ACT_KBD", "    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        if (a->kind == ACT_KBD"),
    ("morph: armed one-shot modifiers not counted", "    uint8_t m = g_os_armed;\n    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        m |= a->carry;", "    uint8_t m = 0;\n    for (unsigned i = 0; i < g_order_n; i++) {\n        const act_t *a = &g_act[g_order[i]];\n        m |= a->carry;"),
    ("morph: toggled modifier keys not counted", "    for (unsigned i = 0; i < g_tk_n; i++) {\n        m |= mod_bit(g_tk[i]);\n    }\n", ""),
    ("morph: the modifier bit is off by one", "(uint8_t)(1u << (usage - KM_USAGE_MOD_FIRST)) : 0u", "(uint8_t)(1u << (usage - KM_USAGE_MOD_FIRST + 1u)) : 0u"),
    ("morph: the morphed usage is not used", "            usage = r->usage;\n            if ((r->flags", "            if ((r->flags"),
    ("morph: the choice is re-made at every update (not locked)", "    case KM_CODE_MOD_MORPH:\n        press_morph(slot, e);\n        return;", "    case KM_CODE_MOD_MORPH:\n        press_morph(slot, e);\n        g_act[slot].kind = ACT_NONE; g_order_n--;\n        return;"),
    ("morph: a modifier key's usage is a carry for armed one-shots", "act_set(slot, ACT_KBD, usage, km_usage_is_modifier(usage) ? 0u : take_armed());\n    if (sup", "act_set(slot, ACT_KBD, usage, take_armed());\n    if (sup"),
    ("morph: armed one-shot modifier not taken by the morph key", "act_set(slot, ACT_KBD, usage, km_usage_is_modifier(usage) ? 0u : take_armed());\n    if (sup", "act_set(slot, ACT_KBD, usage, 0u);\n    if (sup"),
    ("morph: does not count as another key for one-shot modifiers", "    note_press_for_osm();\n    act_set(slot, ACT_KBD, usage,", "    act_set(slot, ACT_KBD, usage,"),
    # ---- Mod-Morph: suppression
    ("morph: suppress flag ignored", "            if ((r->flags & KM_MORPH_FLAG_SUPPRESS) != 0u) {", "            if (false) {"),
    ("morph: suppress flag inverted", "            if ((r->flags & KM_MORPH_FLAG_SUPPRESS) != 0u) {", "            if ((r->flags & KM_MORPH_FLAG_SUPPRESS) == 0u) {"),
    ("morph: every modifier is suppressed, not just the triggering ones", "sup = (uint8_t)(hit & ~mod_bit(usage));", "sup = (uint8_t)(0xFFu & ~mod_bit(usage));"),
    ("morph: the mask is the record's, not the modifiers that were sent", "sup = (uint8_t)(hit & ~mod_bit(usage));", "sup = (uint8_t)(r->mod_mask & ~mod_bit(usage));"),
    ("morph: the key's own output may be suppressed", "sup = (uint8_t)(hit & ~mod_bit(usage));", "sup = hit;"),
    ("morph: suppression is not applied to the report", "((u) < KM_USAGE_MOD_FIRST || (sup & (1u << ((u) - KM_USAGE_MOD_FIRST))) == 0u)", "1"),
    ("morph: a released modifier stays suppressed (mask never shrinks)", "            a->suppress &= raw;\n", "            (void)raw;\n"),
    ("morph: suppress mask lost after the first update", "            a->suppress &= raw;\n", "            a->suppress = 0; (void)raw;\n"),
    ("morph: suppress key count not set", "        g_act[slot].sup_key = true;\n        g_sup_keys++;", "        g_act[slot].sup_key = true;"),
    ("morph: suppressed mask not reported", "    out->suppressed = sup;", "    out->suppressed = 0;"),
    ("morph: only the first suppress key counts", "            sup |= a->suppress;", "            sup = sup != 0u ? sup : a->suppress;"),
    ("morph: records not validated (usage 0x03 accepted)", "rec[i].usage >= 0x04u && rec[i].usage <= 0xE7u &&", "rec[i].usage <= 0xE7u &&"),
    ("morph: records not validated (usage 0xE8 accepted)", "rec[i].usage >= 0x04u && rec[i].usage <= 0xE7u &&", "rec[i].usage >= 0x04u &&"),
    ("morph: records not validated (unknown flag accepted)", "(rec[i].flags & ~KM_MORPH_FLAGS_MASK) == 0u && rec[i].rsvd == 0u", "rec[i].rsvd == 0u"),
    ("morph: records not validated (reserved byte accepted)", "(rec[i].flags & ~KM_MORPH_FLAGS_MASK) == 0u && rec[i].rsvd == 0u", "(rec[i].flags & ~KM_MORPH_FLAGS_MASK) == 0u"),
    ("morph: the old records are kept when new ones are set", "    memset(g_morph, 0, sizeof g_morph);\n    g_morph_n = 0;\n    if (rec == NULL) {", "    g_morph_n = 0;\n    if (rec == NULL) {"),
    ("morph: a reset forgets the records", "    g_tk_n = 0;\n    g_sup_keys = 0;", "    g_tk_n = 0;\n    g_sup_keys = 0;\n    g_morph_n = 0;"),
    # ---- tap-hold with an ordinary key as the hold action
    ("tap-hold: an ordinary hold key does not take an armed one-shot modifier", "act_set(slot, ACT_KBD, hold, km_usage_is_modifier(hold) ? 0u : take_armed());", "act_set(slot, ACT_KBD, hold, 0);"),
    ("tap-hold: a modifier hold key takes an armed one-shot modifier", "act_set(slot, ACT_KBD, hold, km_usage_is_modifier(hold) ? 0u : take_armed());", "act_set(slot, ACT_KBD, hold, take_armed());"),
    # ---- the queued-release fix
    ("release: handled at once even if an event of the slot is queued", "g_act[s].kind != ACT_NONE && !queued_for(s)", "g_act[s].kind != ACT_NONE && (queued_for(s) || true)"),
]
# Mutations that were tried and are EQUIVALENT (no observable change, so no test can kill them; the code keeps the safe form):
#  - toggle key: the 0x04..0xE7 range check in handle_press (a table that fails km_keymap_entry_valid never loads);
#  - morph: `e.a1 < g_morph_n` written as `<=` (g_morph[g_morph_n] is all zero, mask 0, never matches; the table rule keeps a1 < 8);
#  - morph: a record with mask 0 accepted (a mask of 0 never matches); the record count not capped (memory safety only);
#  - morph: suppress mask / count bookkeeping left over after the key is released (only keys in g_order are looked at; the
#    count only decides whether the pre-pass runs);
#  - release: the queue check looking at presses only / releases only / one event short: while a handled key has an event in the
#    queue, that event is its release followed by its next press, so every variant finds the same thing.


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def build_and_run(tree: Path, e: dict, timeout: int = 300):
    """Returns (built, passed, output). Library sources get the strict flags of run_tests.py, test sources the relaxed ones."""
    out = tree / "layers_m6_test.exe"
    (tree / "main_layers.c").write_text(MAIN)
    inc = ["-I" + str(tree / "firmware" / d) for d in ("core/include", "usb/include", "hid/include", "tests")]
    objs = []
    jobs = [(tree / "firmware/core/src" / s, True) for s in CORE] + [(tree / "firmware/tests" / t, False) for t in TESTS] + [(tree / "main_layers.c", False)]
    for src, strict in jobs:
        obj = tree / (src.stem + ".o")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wshadow", "-Wconversion", "-Wno-sign-conversion", "-O2"] if strict else ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
        r = subprocess.run(["gcc", *flags, *inc, "-c", str(src), "-o", str(obj)], capture_output=True, text=True, env=e)
        if r.returncode != 0:
            return False, False, r.stdout + r.stderr
        objs.append(str(obj))
    r = subprocess.run(["gcc", *objs, "-o", str(out)], capture_output=True, text=True, env=e)
    if r.returncode != 0:
        return False, False, r.stdout + r.stderr
    try:
        r = subprocess.run([str(out)], capture_output=True, text=True, env=e, timeout=timeout)
    except subprocess.TimeoutExpired:
        return True, False, "timeout"
    return True, r.returncode == 0, r.stdout


def main() -> int:
    args = sys.argv[1:]
    if "--list" in args:
        for n, (name, _, _) in enumerate(MUTATIONS, 1):
            print("%2d %s" % (n, name))
        return 0
    e = env()
    tmp = Path(tempfile.mkdtemp(prefix="km_mutl_"))
    try:
        tree = tmp / "tree"
        (tree / "firmware").mkdir(parents=True)
        shutil.copytree(FW / "core", tree / "firmware/core", ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copytree(FW / "tests", tree / "firmware/tests", ignore=shutil.ignore_patterns("__pycache__"))
        for d in ("usb/include", "hid/include"):
            shutil.copytree(FW / d, tree / "firmware" / d)
        built, passed, text = build_and_run(tree, e)
        if "--run" in args:
            print(text)
            return 0 if built and passed else 1
        print("baseline:", "pass" if built and passed else "FAIL (cannot judge mutations)")
        if not (built and passed):
            print(text[-3000:])
            return 2
        only = {int(a) for a in args if a.isdigit()}
        path = tree / "firmware" / LAY
        src = path.read_bytes().decode()
        crlf = "\r\n" in src
        text0 = src.replace("\r\n", "\n")
        survived = 0
        for n, (name, old, new) in enumerate(MUTATIONS, 1):
            if only and n not in only:
                continue
            if text0.count(old) != 1:
                print("%2d %-72s NOT APPLICABLE (%d matches)" % (n, name, text0.count(old)))
                survived += 1
                continue
            path.write_bytes(text0.replace(old, new).replace("\n", "\r\n" if crlf else "\n").encode())
            built, passed, _ = build_and_run(tree, e, timeout=120)
            path.write_bytes(src.encode())
            # a mutation that does not even compile is not a test result: it must be rewritten
            if not built:
                print("%2d %-72s DOES NOT BUILD" % (n, name))
                survived += 1
                continue
            print("%2d %-72s %s" % (n, name, "killed" if not passed else "SURVIVED"), flush=True)
            survived += 1 if passed else 0
        print("survivors:", survived)
        return 1 if survived else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
