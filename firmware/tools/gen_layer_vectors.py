#!/usr/bin/env python3
"""Layer-engine test vectors: the REAL km_layers.c compiled natively with gcc, replayed by the WebAssembly build in the browser.

    python firmware/tools/gen_layer_vectors.py            # write ui/tests/vectors/layer_vectors.json
    python firmware/tools/gen_layer_vectors.py --check    # exit 1 if the committed file no longer matches the firmware code

A fixed-seed generator makes key-press scenarios (press order, millisecond clock, tables with FN, toggle, tap-hold and one-shot
entries under the three tap policies); a small C program built from the real firmware/core/src/km_layers.c + km_keymap.c runs each
one and prints what km_layers_update_ms() answered after every step. ui/tests/wasm-parity.test.mjs replays the same scenarios on
ui/wasm/km_core.wasm (the same C compiled for the browser) and demands identical answers. Nothing is pasted by hand.

Answer format per step (hex), the same bytes km_w_layers_update() writes:
   kbd_n layer toggled armed profile_request pending consumer_lo consumer_hi toggle_keys suppressed kbd[0..kbd_n)
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

FW = Path(__file__).resolve().parents[1]
REPO = FW.parent
OUT = REPO / "ui" / "tests" / "vectors" / "layer_vectors.json"

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_m6_types.h"

int main(int argc, char **argv)

{
    (void)argc;
    FILE *f = fopen(argv[1], "r");
    char w[16];
    uint8_t usage[KM_LAYERS][KM_KEYMAP_SLOTS], arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS];
    km_layer_opts_t opts;
    km_morph_rec_t morph[KM_MORPH_MAX];
    unsigned morph_n = 0;
    while (fscanf(f, "%15s", w) == 1) {
        if (!strcmp(w, "SC")) {
            km_keymap_default_table(usage);
            memset(arg, 0, sizeof arg);
            km_layer_opts_default(&opts);
            memset(morph, 0, sizeof morph);
            morph_n = 0;
        } else if (!strcmp(w, "MORPH")) {
            unsigned i, m, u, fl;
            fscanf(f, "%u %u %u %u", &i, &m, &u, &fl);
            morph[i].mod_mask = (uint8_t)m; morph[i].usage = (uint8_t)u; morph[i].flags = (uint8_t)fl;
            if (morph_n < i + 1u) morph_n = i + 1u;
        } else if (!strcmp(w, "OPTS")) {
            unsigned a, b, c;
            fscanf(f, "%u %u %u", &a, &b, &c);
            opts.tap_term_ms = (uint16_t)a; opts.oneshot_ms = (uint16_t)b; opts.tap_policy = (uint8_t)c;
        } else if (!strcmp(w, "E")) {
            unsigned l, s, c, a0, a1;
            fscanf(f, "%u %u %u %u %u", &l, &s, &c, &a0, &a1);
            usage[l][s] = (uint8_t)c; arg[l][s][0] = (uint8_t)a0; arg[l][s][1] = (uint8_t)a1;
        } else if (!strcmp(w, "GO")) {
            int ok = km_layers_set_table(usage, arg, &opts);
            km_layers_set_morph(morph, morph_n);
            km_layers_reset();
            printf("LOAD %d\n", ok);
        } else if (!strcmp(w, "STEP")) {
            unsigned now, n;
            uint8_t down[KM_KEYMAP_SLOTS];
            km_layers_out_t o;
            fscanf(f, "%u %u", &now, &n);
            for (unsigned i = 0; i < n; i++) { unsigned s; fscanf(f, "%u", &s); down[i] = (uint8_t)s; }
            km_layers_update_ms(down, n, now, &o);
            printf("OUT %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", o.kbd_n, o.layer, o.toggled, o.armed, o.profile_request,
                   o.pending ? 1 : 0, o.consumer & 0xff, o.consumer >> 8, o.toggle_keys, o.suppressed);
            for (unsigned i = 0; i < o.kbd_n; i++) printf("%02x", o.kbd[i]);
            printf("\n");
            printf("LOCK");
            for (unsigned i = 0; i < n; i++) {
                km_layers_entry_t e;
                if (km_layers_locked_entry(down[i], &e)) printf(" %02x%02x%02x", e.code, e.arg0, e.arg1); else printf(" -");
            }
            printf("\n");
        }
    }
    return 0;
}
'''


def scenarios():
    rnd = random.Random(0x4C41594552)          # "LAYER"
    # real key slots of the default map (km_keymap_slot_is_key): plain letters, the FN key, and keys that are media / profile codes on layer 1
    letters = [2, 6, 9, 12, 13, 16, 21, 34, 42, 45, 49, 52, 61, 72, 73, 74, 75]
    media = [7, 11, 25, 27, 29, 30, 38, 43, 44, 62]
    fn = 14
    out = []

    def run(name, edits, opts, keys, steps, mean_gap, morph=()):
        now = rnd.randrange(0, 1 << 32) if name.startswith("wrap") else 1000
        down, seq = [], []
        for _ in range(steps):
            now = (now + rnd.randrange(1, mean_gap * 2)) & 0xFFFFFFFF
            k = rnd.choice(keys)
            if k in down:
                down.remove(k)
            elif len(down) < 6:
                down.append(k)
            seq.append((now, list(down)))
        out.append({"name": name, "edits": edits, "opts": opts, "morph": [list(m) for m in morph], "steps": seq})

    run("default_fn", [], None, letters + [fn], 250, 40)
    run("default_media_profile", [], None, letters[:6] + [fn] + media, 250, 40)
    # extended codes: toggle layer 2 on '-' and layer 1 on '3', tap-hold space (hold = layer 1) and A (hold = left shift),
    # one-shot control on LCTRL; layer 2 gives two keys a letter, layer 1 gives 'V' volume-down
    ext_keys = letters + [fn, 3, 4, 63, 73, 5]
    for pol in (0, 1, 2):
        edits = [[0, 3, 0xE8, 2, 0], [0, 4, 0xE8, 1, 0], [0, 63, 0xE9, 0x2C, 0xF0], [0, 73, 0xE9, 0x04, 0xE1], [0, 5, 0xEA, 0xE0, 0],
                 [2, 2, 0x1D, 0, 0], [2, 6, 0x1B, 0, 0], [1, 7, 0xF5, 0, 0]]
        run(f"ext_policy{pol}", edits, [200, 800, pol], ext_keys, 300, 60)
        run(f"ext_fast_policy{pol}", edits, [40, 150, pol], ext_keys, 300, 25)
    run("wrap_clock", [[0, 63, 0xE9, 0x2C, 0xF0], [0, 5, 0xEA, 0xE0, 0]], [100, 300, 2], letters + [63, 5], 300, 80)
    # settings format 3 (M6): a tap-hold whose hold action is an ordinary key (not only a layer or a modifier), and the M6 codes
    # 0xEB..0xEF, 0xFD, which the layer module itself treats as keys that do nothing (the engine / pad / light modules act on them)
    hold_key = [[0, 63, 0xE9, 0x2C, 0x28], [0, 73, 0xE9, 0x04, 0x29], [0, 3, 0xEB, 0x05, 0], [0, 4, 0xEC, 0x06, 0], [0, 5, 0xFD, 1, 0]]
    for pol in (0, 2):
        run(f"m6_hold_is_key_policy{pol}", hold_key, [150, 500, pol], letters + [fn, 63, 73, 3, 4, 5], 300, 40)
    # P2: toggle keys (usage state switched on and off, several at once) and Mod-Morph (modifier mask -> another usage, with and
    # without suppressing the modifier). Records: 0 = shift turns the key into 'delete'(0x4C) and keeps shift out; 1 = ctrl or alt
    # turns it into 'F5'(0x3E) and leaves the modifier in; 2 = left-GUI turns it into 'home'(0x4A)
    morph = [(0x02, 0x4C, 1), (0x11, 0x3E, 0), (0x08, 0x4A, 0)]
    p2 = [[0, 3, 0xEB, 0x05, 0], [0, 4, 0xEB, 0x06, 0], [0, 8, 0xEB, 0x07, 0], [0, 10, 0xEB, 0xE1, 0],
          [0, 2, 0xEC, 0x14, 0], [0, 6, 0xEC, 0x08, 1], [0, 9, 0xEC, 0x0D, 2], [1, 12, 0xEC, 0x0F, 0]]
    mods = [0, 28, 5, 23, 59, 68, 65]    # LSHIFT RSHIFT LCTRL LALT LWIN RCTRL RALT
    for pol in (0, 2):
        run(f"p2_toggle_morph_policy{pol}", p2 + [[0, 63, 0xE9, 0x2C, 0xF0]], [150, 500, pol], [3, 4, 8, 10, 2, 6, 9, 12, 14, 63] + mods, 300, 40, morph)
    run("p2_morph_no_records", p2, [150, 500, 0], [2, 6, 9, 12, 14] + mods, 200, 40, ())
    return out


def spec_text(scs):
    lines = []
    for s in scs:
        lines.append("SC")
        if s["opts"]:
            lines.append("OPTS %d %d %d" % tuple(s["opts"]))
        for i, m in enumerate(s["morph"]):
            lines.append("MORPH %d %d %d %d" % (i, *m))
        for e in s["edits"]:
            lines.append("E " + " ".join(map(str, e)))
        lines.append("GO")
        for now, down in s["steps"]:
            lines.append(f"STEP {now} {len(down)} " + " ".join(map(str, down)))
    return "\n".join(lines) + "\n"


def build_and_run(scs):
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    cc = shutil.which("gcc", path=e["PATH"])
    if not cc:
        sys.exit("gcc not found (on Windows install MSYS2 ucrt64)")
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        (tmp / "h.c").write_text(HARNESS)
        exe = tmp / ("h.exe" if os.name == "nt" else "h")
        r = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-I" + str(FW / "core/include"), str(tmp / "h.c"),
                            str(FW / "core/src/km_layers.c"), str(FW / "core/src/km_keymap.c"), "-o", str(exe)], env=e)
        if r.returncode:
            sys.exit("harness build failed")
        (tmp / "spec.txt").write_text(spec_text(scs))
        res = subprocess.run([str(exe), str(tmp / "spec.txt")], env=e, capture_output=True, text=True, check=True).stdout.splitlines()
    return res


def make():
    scs = scenarios()
    res = build_and_run(scs)
    i = 0
    for s in scs:
        assert res[i].startswith("LOAD"), res[i]
        s["load"] = int(res[i].split()[1])
        i += 1
        s["expect"], s["locked"] = [], []
        for _ in s["steps"]:
            s["expect"].append(res[i].split()[1])
            assert res[i + 1].startswith("LOCK")
            s["locked"].append(" ".join(res[i + 1].split()[1:]))
            i += 2
        s["steps"] = [[now, down] for now, down in s["steps"]]
    assert i == len(res)
    return {"about": "Generated by firmware/tools/gen_layer_vectors.py from the real km_layers.c / km_keymap.c. Do not edit by hand.",
            "format": "expect: kbd_n layer toggled armed profile_request pending consumer_lo consumer_hi kbd[]; locked: per down slot code+arg0+arg1 of km_layers_locked_entry, or -", "scenarios": scs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    doc = json.dumps(make(), separators=(",", ":")) + "\n"
    if a.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != doc:
            print("ui/tests/vectors/layer_vectors.json is stale: run python firmware/tools/gen_layer_vectors.py")
            return 1
        print("layer vectors up to date")
        return 0
    OUT.write_text(doc, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)} ({len(doc)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
