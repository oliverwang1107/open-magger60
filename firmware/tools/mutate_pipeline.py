#!/usr/bin/env python3
"""Mutation pass for the M6 key pipeline (core/src/km_pipeline.c, tests/test_pipeline_m6.c).

    python firmware/tools/mutate_pipeline.py            # run every mutation, list survivors
    python firmware/tools/mutate_pipeline.py --list
    python firmware/tools/mutate_pipeline.py --only holdover

Same idea as mutate_socd.py: each mutation changes one expression in a scratch copy of the sources, rebuilds tests/test_pipeline_m6.c with a
tiny main and runs it. "killed" = a check fails (or the program crashes / hangs); a survivor is a hole in the tests.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
F = "core/src/km_pipeline.c"

MUTATIONS = [
    ("order: layers see the unfiltered list (SOCD result unused)", "km_layers_update_cycles(p->tmp, nd,", "km_layers_update_cycles(in->down, in->n,"),
    ("SOCD sees layer 0 always", "nd = km_socd_filter(&p->socd, in->down, nd, p->prev_layer,", "nd = km_socd_filter(&p->socd, in->down, nd, 0,"),
    ("previous layer never remembered", "    p->prev_layer = out->lo.layer;\n", ""),
    ("fault does not reset the modules", "        if (!p->blocked) {\n            km_pipeline_reset(p);\n            p->blocked = true;\n        }", "        p->blocked = true;"),
    ("fault still runs the pipeline", "    if (!in->allowed) {", "    if (!in->allowed && 0) {"),
    ("recovery from a fault does not hold keys over", "        bits_from_list(p->holdover, in->down, nd);\n        p->epoch++;", "        p->epoch++;"),
    ("recovery does not poke the gamepad", "        p->epoch++;\n        p->pad_poke = true;\n    }", "        p->epoch++;\n    }"),
    ("profile change does not hold keys over", "    bits_from_list(p->holdover, down, n > KM_ENGINE_MAX_DOWN ? KM_ENGINE_MAX_DOWN : n);\n}\n\nbool km_pipeline_slot_cfg", "}\n\nbool km_pipeline_slot_cfg"),
    ("held-over keys never become normal again", "p->holdover[w] &= now_down[w];", "p->holdover[w] |= 0u;"),
    ("DKS ignores the hold-over", "&& e.arg0 < KM_DKS_MAX && !bit_get(p->holdover, slot)) {", "&& e.arg0 < KM_DKS_MAX) {"),
    ("DKS record index not checked", "&& e.arg0 < KM_DKS_MAX && !bit_get", "&& !bit_get"),
    ("DKS output not merged", "out->lo.kbd_n = (uint8_t)km_dks_merge(out->lo.kbd, out->lo.kbd_n, KM_KEYMAP_SLOTS, &d);", "(void)d;"),
    ("DKS not updated while it finishes (only with keys down)", "if (n_dks != 0u || km_dks_busy(&p->dks)) {", "if (n_dks != 0u) {"),
    ("gamepad keys listed even when not a pad code", "} else if (is_pad_code(e.code)) {", "} else if (e.code >= KM_CODE_TOGGLE_KEY) {"),
    ("gamepad epoch never changes with the layer", "p->epoch * 8u + out->lo.layer", "p->epoch * 8u"),
    ("gamepad epoch never changes with a table change", "    p->epoch++;\n    p->pad_poke = true;\n    p->light_down[0]", "    p->pad_poke = true;\n    p->light_down[0]"),
    ("gamepad not updated while a key is held over", " || bits_any(p->pad.blocked)) {", ") {"),
    ("gamepad start-up pass not forced", "if (n_pad != 0u || p->pad_poke ||", "if (n_pad != 0u ||"),
    ("pad key list not bounded", "if (n_pad < KM_PAD_MAX_KEYS) {", "if (1) {"),
    ("light action repeats while held", "if (!bit_get(p->light_down, slot)) {\n                    bit_set(p->light_down, slot);", "if (1) {\n                    bit_set(p->light_down, slot);"),
    ("light action fires for a held-over key", "                    if (!bit_get(p->holdover, slot)) {\n                        if (out->light_n", "                    if (1) {\n                        if (out->light_n"),
    ("light action memory not cleared on release", "                p->light_down[w] &= now_down[w];", "                p->light_down[w] |= 0u;"),
    ("light action argument 0 accepted", "e.arg0 >= 1u && e.arg0 <= KM_LIGHT_ACT_LAST", "e.arg0 <= KM_LIGHT_ACT_LAST"),
    ("light actions unbounded", "if (out->light_n < KM_PIPE_MAX_LIGHT) {", "if (1) {"),
    ("slot config: DKS point not used", "out->press_mm100 = press;\n                out->release_mm100 = release;", "out->press_mm100 = 120;\n                out->release_mm100 = 110;"),
    ("slot config: gamepad key keeps the user's trigger", "            memset(out, 0, sizeof *out);\n            out->mode = KM_ENGINE_TRIG_FIXED;\n            out->press_mm100 = KM_PAD_ENGINE_PRESS_MM100;", "            out->mode = KM_ENGINE_TRIG_FIXED;\n            out->press_mm100 = KM_PAD_ENGINE_PRESS_MM100;"),
    ("slot config: only layer 0 is looked at", "for (unsigned l = 0; l < KM_LAYERS; l++) {\n        const uint8_t code = prof->layer_usage[l][slot];", "for (unsigned l = 0; l < 1; l++) {\n        const uint8_t code = prof->layer_usage[l][slot];"),
    ("slot config: record index not checked", "if (rec < prof->dks_n && km_dks_engine_points", "if (km_dks_engine_points"),
    ("table scan misses layer 3", "    for (unsigned l = 0; l < KM_LAYERS; l++) {\n        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {\n            const uint8_t c", "    for (unsigned l = 0; l + 1u < KM_LAYERS; l++) {\n        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {\n            const uint8_t c"),
    ("reset keeps the toggle keys", "void km_pipeline_reset(km_pipeline_t *p)\n{\n    km_layers_reset();", "void km_pipeline_reset(km_pipeline_t *p)\n{"),
    ("reset keeps the gamepad", "    km_pad_reset(&p->pad);\n    p->light_down", "    p->light_down"),
    ("reset keeps DKS", "    km_dks_reset(&p->dks);\n    km_pad_reset", "    km_pad_reset"),
    ("set_profile does not load the Mod-Morph records", "    km_layers_set_morph(prof->morph, prof->morph_n);\n", ""),
    ("set_profile does not load the SOCD pairs", "    (void)km_socd_set_pairs(&p->socd, prof->socd, prof->socd_n);\n", ""),
    ("set_profile does not load the gamepad curves", "    (void)km_pad_set_config(&p->pad, prof->pad_curve, &prof->pad_opts);\n", ""),
]

EQUIVALENT = {
    "recovery does not poke the gamepad": "km_pipeline_reset() (run by the first refused pass) already sets the poke and nothing clears it before the recovery",
    "DKS record index not checked": "km_dks_update_* ignores a record index it does not have (tests/test_dks.c); the check only saves the list slot",
    "gamepad epoch never changes with a table change": "km_pad_set_config() already releases the gamepad and the poke makes the next update hold the keys over",
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
void test_pipeline_m6(void);
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_pipeline_m6();
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
'''


def build_lib(e, cc, tree: Path, work: Path):
    """Compile every core source once (objects are reused by all mutations except km_pipeline.c itself)."""
    return None


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
    work = Path(tempfile.mkdtemp(prefix="km_mut_pipe_"))
    (work / "main.c").write_text(STUB)
    # every other core file is compiled once into objects; only km_pipeline.c is rebuilt per mutation
    objs = []
    incs = ["-I" + str(FW / "core/include"), "-I" + str(FW / "tests")]
    srcs = [p for p in sorted((FW / "core/src").glob("*.c")) if p.name != "km_pipeline.c"] + sorted((FW / "core/gen").glob("*.c")) + [
        FW / "tests/test_pipeline_m6.c", work / "main.c"]
    for s in srcs:
        o = work / (s.parent.name + "_" + s.stem + ".o")
        r = subprocess.run([cc, "-std=gnu11", "-O1", "-w", *incs, "-c", str(s), "-o", str(o)], env=e, capture_output=True, text=True)
        if r.returncode != 0:
            print("cannot build", s, r.stderr[:300])
            return 2
        objs.append(str(o))
    survivors, broken = [], []
    try:
        for i, (name, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            text = (FW / F).read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-62s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            mut = work / "km_pipeline_mut.c"
            mut.write_text(text.replace(old, new), newline="")
            exe = work / "t.exe"
            r = subprocess.run([cc, "-std=gnu11", "-O1", "-w", *incs, str(mut), *objs, "-o", str(exe)], env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-62s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                broken.append(name)
                continue
            try:
                r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=300)
                rc, out = r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                rc, out = 1, "(timeout)"
            if rc == 0 and name in EQUIVALENT:
                print("%2d  %-62s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif rc == 0:
                print("%2d  %-62s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%2d  %-62s killed  %s" % (i, name, first[-70:]))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
