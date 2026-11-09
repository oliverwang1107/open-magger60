#!/usr/bin/env python3
"""Mutation pass for the settings format version 3 and the M6 table-entry rules (M6 task P0): change one comparison, bound,
offset or flag at a time in a scratch copy of firmware/, run the host tests, and require the tests to FAIL for every mutation.

    python firmware/tools/mutate_settings.py            # all mutations
    python firmware/tools/mutate_settings.py 3 7        # only mutations 3 and 7

A mutation that the tests do not notice ("SURVIVED") means a missing test: add one. The real tree is never modified.
The JavaScript codec has the same kind of check against the same vectors: ui/tests/settings-codec.test.mjs (see
ui/tests/mutate_codec.mjs).
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

SET = "core/src/km_settings.c"
KEY = "core/src/km_keymap.c"
LAY = "core/src/km_layers.c"

MUTATIONS = [
    # ---- record rules
    ("SOCD: the two slots may be the same", SET, "p->slot_a != p->slot_b &&", ""),
    ("Mod-Morph: an empty modifier mask is fine", SET, "r != NULL && r->mod_mask != 0u &&", "r != NULL &&"),
    ("DKS: start point floor 40 -> 39", SET, "act < KM_DKS_ACT_MIN_MM100 ||", "act < KM_DKS_ACT_MIN_MM100 - 1u ||"),
    ("DKS: minimum gap 40 -> 39", SET, "bot < act + KM_DKS_GAP_MIN_MM100", "bot < act + KM_DKS_GAP_MIN_MM100 - 1u"),
    ("DKS: an unused action may carry events", SET, "r->act[i].usage == 0u ? r->act[i].ev != 0u : !usage_ok(r->act[i].usage)", "r->act[i].usage == 0u ? false : !usage_ok(r->act[i].usage)"),
    ("SOCD: a slot may be in two pairs", SET, "used[p->socd[i].slot_a] || used[p->socd[i].slot_b]", "used[p->socd[i].slot_a]"),
    # (the bounds on the section count (64) and on a section reaching into the CRC are not mutated: without the first the reader would write outside
    # its bookkeeping arrays, and the second is repeated by the exact-size rule of every section; neither has an observable difference to test.)
    # (a count of 9 DKS records is refused by the bound on the count; the records after the 8th are not looked at when the bound is removed, so the
    # mutation changes nothing a test can see except by reading outside the array: not listed)
    ("DKS index may equal the record count", SET, "a0 >= pr->dks_n", "a0 > pr->dks_n"),
    ("Mod-Morph index may equal the record count", SET, "a1 >= pr->morph_n", "a1 > pr->morph_n"),
    ("pad curve: span 4 -> 5", SET, "(unsigned)c->start_mm100_5 + KM_PAD_CURVE_SPAN_MIN", "(unsigned)c->start_mm100_5 + KM_PAD_CURVE_SPAN_MIN + 1u"),
    ("pad option: dead zone 50 -> 49", SET, "o->stick_deadzone > KM_PAD_DEADZONE_MAX", "o->stick_deadzone >= KM_PAD_DEADZONE_MAX"),
    ("device: filter mode 3 accepted", SET, "d->filter_mode > KM_FILTER_MODE_LAST", "d->filter_mode > KM_FILTER_MODE_LAST + 1u"),
    # ---- which version is written
    ("needs v3: SOCD pairs ignored", SET, "pr->morph_n != 0u || pr->socd_n != 0u ||", "pr->morph_n != 0u ||"),
    ("needs v3: only the first device byte compared", SET, "return memcmp(d, &zero, sizeof zero) == 0;", "return memcmp(d, &zero, 1u) == 0;"),
    ("needs v3: a default PAD section is always written", SET, "static bool has_pad(const km_profile_t *p)   { return !pad_is_default(p); }", "static bool has_pad(const km_profile_t *p)   { return true; }"),
    # ---- the writer
    ("v3 writer: key configs by position, not by key slot", SET, "p = put_cfg(p, &q->key[ks[i]]);", "p = put_cfg(p, &q->key[i]);"),
    ("v3 writer: number of keys 61 -> 76", SET, "p[11] = KM_KEYMAP_KEYS;", "p[11] = KM_KEYMAP_SLOTS;"),
    ("v3 writer: layer codes by position", SET, "*p++ = q->layer_usage[l][ks[i]];", "*p++ = q->layer_usage[l][i];"),
    ("v3 writer: DKS usage and events swapped", SET, "p[8 + 2u * a] = q->dks[i].act[a].usage;", "p[8 + 2u * a] = q->dks[i].act[a].ev;"),
    ("v3 writer: SOCD mode and flags swapped", SET, "p[2] = q->socd[i].mode;", "p[2] = q->socd[i].flags;"),
    # ---- the reader
    ("v3 reader: section scope 4 accepted", SET, "if (scope != KM_SETTINGS_SCOPE_GLOBAL && scope >= KM_PROFILES) {", "if (scope != KM_SETTINGS_SCOPE_GLOBAL && scope > KM_PROFILES) {"),
    ("v3 reader: the same section twice accepted", SET, "seen_id[j] == id && seen_scope[j] == scope", "seen_id[j] == id && seen_scope[j] != scope"),
    ("v3 reader: unknown required id 0x7F accepted", SET, "} else if (id < KM_SETTINGS_SEC_OPTIONAL) {", "} else if (id < KM_SETTINGS_SEC_OPTIONAL - 1u) {"),
    ("v3 reader: bytes between the last section and the CRC accepted", SET, "if (p != end) {", "if (p > end) {"),
    ("v3 reader: 8177 bytes accepted", SET, "len > KM_SETTINGS_V3_MAX_SIZE", "len > KM_SETTINGS_V3_MAX_SIZE + 1u"),
    ("v3 reader: reserved header byte 13 not checked", SET, "buf[13] != 0u ||", ""),
    ("v3 reader: 76 slots accepted in the key count", SET, "buf[11] != KM_KEYMAP_KEYS)", "buf[11] != KM_KEYMAP_KEYS && buf[11] != KM_KEYMAP_SLOTS)"),
    ("v3 reader: DKS count byte not compared with the length", SET, "d->len != 1u + d->p[0] * KM_SETTINGS_DKS_BYTES", "d->len < 1u + d->p[0] * KM_SETTINGS_DKS_BYTES"),
    ("v3 reader: DEVICE with a profile scope accepted", SET, "if (scope != KM_SETTINGS_SCOPE_GLOBAL) {\n                return false;\n            }\n            dev.p", "dev.p"),
    ("v2 reader: version 3 entries accepted in a version 2 blob", SET, "if (!km_keymap_entry_valid_v2(tmp.profile[pr]", "if (!km_keymap_entry_valid(tmp.profile[pr]"),
    # ---- table-entry rules
    ("toggle key: second argument may be set", KEY, "case KM_CODE_TOGGLE_KEY:\n        return usage_valid(arg0) && arg1 == 0u;", "case KM_CODE_TOGGLE_KEY:\n        return usage_valid(arg0);"),
    ("Mod-Morph entry: index 8 accepted", KEY, "arg1 < KM_MORPH_MAX", "arg1 <= KM_MORPH_MAX"),
    ("DKS entry: index 8 accepted", KEY, "arg0 < KM_DKS_MAX && arg1 == 0u", "arg0 <= KM_DKS_MAX && arg1 == 0u"),
    ("gamepad button: start point 68 refused", KEY, "arg1 <= KM_PAD_BTN_START_MAX", "arg1 < KM_PAD_BTN_START_MAX"),
    ("gamepad axis: a trigger may go negative", KEY, "!(axis >= KM_PAD_AXIS_LT && (a & KM_PAD_AXIS_NEG) != 0u)", "!(axis > KM_PAD_AXIS_LT && (a & KM_PAD_AXIS_NEG) != 0u)"),
    ("gamepad axis: axis 6 accepted", KEY, "axis < KM_PAD_AXIS_COUNT", "axis <= KM_PAD_AXIS_COUNT"),
    ("light action: 7 refused", KEY, "arg0 <= KM_LIGHT_ACT_LAST && arg1 == 0u", "arg0 < KM_LIGHT_ACT_LAST && arg1 == 0u"),
    ("tap-hold: hold may equal the tap key", KEY, "(usage_valid(arg1) && arg1 != arg0)", "usage_valid(arg1)"),
    ("version 2 rule: a plain key as the hold action", KEY, "return tap_action_valid(arg0) && layer_or_modifier_hold(arg1);\n    case KM_CODE_ONESHOT_MOD:", "return tap_action_valid(arg0) && (layer_or_modifier_hold(arg1) || usage_valid(arg1));\n    case KM_CODE_ONESHOT_MOD:"),
    # ---- the locked-entry accessor
    ("locked entry: arg0 taken from arg1", LAY, "g_ent[slot].arg0 = e.a0;", "g_ent[slot].arg0 = e.a1;"),
    ("locked entry: reported for keys that are up", LAY, "out == NULL || g_act[slot].kind == ACT_NONE", "out == NULL"),
]


def run_suite(tree: Path) -> bool:
    r = subprocess.run([sys.executable, str(tree / "firmware/tools/run_tests.py")], capture_output=True, text=True)
    return r.returncode == 0


def main() -> int:
    only = {int(a) for a in sys.argv[1:]}
    tmp = Path(tempfile.mkdtemp(prefix="km_mut_"))
    try:
        tree = tmp / "tree"
        shutil.copytree(FW, tree / "firmware", ignore=shutil.ignore_patterns("__pycache__"))
        base = run_suite(tree)
        print("baseline:", "pass" if base else "FAIL (cannot judge mutations)")
        if not base:
            return 2
        survived = 0
        for n, (name, rel, old, new) in enumerate(MUTATIONS, 1):
            if only and n not in only:
                continue
            path = tree / "firmware" / rel
            src = path.read_bytes().decode()
            crlf = "\r\n" in src
            text = src.replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d %-62s NOT APPLICABLE (%d matches)" % (n, name, text.count(old)))
                survived += 1
                continue
            path.write_bytes(text.replace(old, new).replace("\n", "\r\n" if crlf else "\n").encode())
            killed = not run_suite(tree)
            path.write_bytes(src.encode())
            print("%2d %-62s %s" % (n, name, "killed" if killed else "SURVIVED"), flush=True)
            survived += 0 if killed else 1
        print("survivors:", survived)
        return 1 if survived else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
