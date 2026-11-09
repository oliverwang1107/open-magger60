#!/usr/bin/env python3
"""Cross-language test vectors for the light scene blob "KMLS": the REAL firmware encoder/decoder against the JavaScript codec.

    python firmware/tools/gen_lightscene_vector.py            # build the C harness, write ui/tests/vectors/lightscene_vectors.json
    python firmware/tools/gen_lightscene_vector.py --check    # same, but only compare with the committed file (exit 1 if stale)

Same method as gen_settings_vector.py: a tiny C program (temporary directory, nothing kept) is compiled from the real firmware/core/src/km_lightscene.c,
fed scenes to encode and blobs to decode, and what the firmware answered is written down as hex. ui/tests/lightscene-codec.test.mjs then makes
the JavaScript codec (ui/js/core/lightscene_codec.js) produce / accept / refuse exactly the same bytes. Nothing is pasted by hand.

On Windows the MSYS2 ucrt64 compiler is put first on PATH (as in run_tests.py).
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
REPO = FW.parent
OUT = REPO / "ui" / "tests" / "vectors" / "lightscene_vectors.json"

LEDS, PALETTE, MAX_RULES = 61, 16, 8

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "km/km_lightscene.h"

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static void print_hex(const uint8_t *b, unsigned n) { for (unsigned i = 0; i < n; i++) printf("%02x", b[i]); }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f) return 3;
    static uint8_t buf[4096];
    static char hex[8200];
    char op[16], name[96];
    while (fscanf(f, "%15s", op) == 1) {
        if (!strcmp(op, "ENC")) {
            unsigned n, af, clip;
            int off;
            km_lightscene_t s;
            km_lightscene_default(&s);
            if (fscanf(f, "%95s %u %u %u %d", name, &n, &af, &clip, &off) != 5) return 4;
            s.n_rules = (uint8_t)n;
            for (unsigned i = 0; i < n && i < KM_LS_MAX_RULES; i++) {
                unsigned v[8];
                for (unsigned k = 0; k < 8; k++) if (fscanf(f, "%u", &v[k]) != 1) return 5;
                s.rule[i].source = (uint8_t)v[0]; s.rule[i].param = (uint8_t)v[1]; s.rule[i].led = (uint8_t)v[2]; s.rule[i].flags = (uint8_t)v[3];
                s.rule[i].on.r = (uint8_t)v[4]; s.rule[i].on.g = (uint8_t)v[5]; s.rule[i].on.b = (uint8_t)v[6]; s.rule[i].dim = (uint8_t)v[7];
            }
            for (unsigned i = 0; i < KM_LS_PALETTE; i++) {
                unsigned r, g, b;
                if (fscanf(f, "%u %u %u", &r, &g, &b) != 3) return 6;
                s.palette[i].r = (uint8_t)r; s.palette[i].g = (uint8_t)g; s.palette[i].b = (uint8_t)b;
            }
            for (unsigned i = 0; i < KM_LED_COUNT; i++) {
                unsigned v;
                if (fscanf(f, "%u", &v) != 1) return 7;
                s.key_idx[i] = (uint8_t)v;
            }
            s.anim_flags = (uint8_t)af; s.anim_clip = (uint8_t)clip; s.anim_offset_ms = off;
            unsigned len = km_lightscene_encode(&s, buf, sizeof buf);
            printf("R %s ", name);
            if (len) print_hex(buf, len); else printf("REJECT");
            printf("\n");
        } else if (!strcmp(op, "DEC")) {
            if (fscanf(f, "%95s %8199s", name, hex) != 2) return 8;
            unsigned len = !strcmp(hex, "-") ? 0 : (unsigned)strlen(hex) / 2;
            for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
            km_lightscene_t s;
            memset(&s, 0, sizeof s);
            printf("R %s ", name);
            if (km_lightscene_decode(&s, buf, len)) {
                static uint8_t again[KM_LS_MAX_ENCODED];
                unsigned n = km_lightscene_encode(&s, again, sizeof again);
                if (n) print_hex(again, n); else printf("REJECT");
            } else {
                printf("REJECT");
            }
            printf("\n");
        } else if (!strcmp(op, "CRC")) {
            if (fscanf(f, "%8199s", hex) != 1) return 9;
            unsigned len = (unsigned)strlen(hex) / 2;
            for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
            printf("R CRC %08x\n", km_crc32(0, buf, len));
        } else {
            return 10;
        }
    }
    fclose(f);
    return 0;
}
'''


def blank_model():
    return {"rules": [], "palette": [[0, 150, 255]] + [[0, 0, 0]] * (PALETTE - 1), "keys": [0] * LEDS,
            "anim": {"flags": 0, "clip": 0, "offsetMs": 0}}


def rule(source, param, led, flags, rgb, dim=0):
    return {"source": source, "param": param, "led": led, "flags": flags, "color": list(rgb), "dim": dim}


def random_model(seed):
    r = random.Random(seed)
    m = blank_model()
    for _ in range(r.randint(0, MAX_RULES)):
        src = r.randint(1, 8)
        param = r.randint(0, 3) if src in (4, 5) else 0
        off = r.randint(0, 2)
        flags = off | (4 if r.random() < 0.5 else 0)
        m["rules"].append(rule(src, param, r.randint(0, LEDS - 1), flags, [r.randint(0, 255) for _ in range(3)], r.randint(0, 255) if off == 2 else 0))
    m["palette"] = [[r.randint(0, 255) for _ in range(3)] for _ in range(PALETTE)]
    m["keys"] = [r.randint(0, PALETTE - 1) for _ in range(LEDS)]
    m["anim"] = {"flags": r.randint(0, 3), "clip": r.randint(0, 255), "offsetMs": r.randint(-3600000, 3600000)}
    return m


def edit(m, fn):
    import copy
    c = copy.deepcopy(m)
    fn(c)
    return c


def enc_cases():
    out = []

    def add(name, m):
        out.append({"name": name, "model": m})

    add("default", blank_model())
    for i in range(10):
        add(f"random_{i}", random_model(100 + i))
    full = random_model(7)
    full["rules"] = [rule(1 + (i % 8), (i % 4) if (1 + (i % 8)) in (4, 5) else 0, i * 7, i % 3 | (4 if i % 2 else 0), [i, 255 - i, i * 9 % 256], 40 if i % 3 == 2 else 0) for i in range(8)]
    add("eight_rules", full)
    add("caps_lock_example", edit(blank_model(), lambda m: m["rules"].append(rule(2, 0, 28, 2, [255, 0, 0], 30))))
    add("layer3_blink", edit(blank_model(), lambda m: m["rules"].append(rule(4, 3, 0, 1 | 4, [0, 255, 0]))))
    add("offset_max", edit(blank_model(), lambda m: m["anim"].update(offsetMs=3600000)))
    add("offset_min", edit(blank_model(), lambda m: m["anim"].update(offsetMs=-3600000)))
    add("offset_minus_one", edit(blank_model(), lambda m: m["anim"].update(offsetMs=-1)))
    add("anim_flags_3", edit(blank_model(), lambda m: m["anim"].update(flags=3, clip=255)))
    add("palette_index_15", edit(blank_model(), lambda m: m["keys"].__setitem__(60, 15)))
    # refused: each of these must be REJECT in both languages
    add("reject_offset_over", edit(blank_model(), lambda m: m["anim"].update(offsetMs=3600001)))
    add("reject_offset_under", edit(blank_model(), lambda m: m["anim"].update(offsetMs=-3600001)))
    add("reject_anim_flags_4", edit(blank_model(), lambda m: m["anim"].update(flags=4)))
    add("reject_palette_index_16", edit(blank_model(), lambda m: m["keys"].__setitem__(0, 16)))
    add("reject_palette_index_255", edit(blank_model(), lambda m: m["keys"].__setitem__(30, 255)))
    add("reject_nine_rules", edit(full, lambda m: m["rules"].append(rule(1, 0, 0, 0, [1, 2, 3]))))
    for name, bad in [
        ("source_0", rule(0, 0, 0, 0, [1, 1, 1])), ("source_9", rule(9, 0, 0, 0, [1, 1, 1])),
        ("caps_param_1", rule(2, 1, 0, 0, [1, 1, 1])), ("layer_4", rule(4, 4, 0, 0, [1, 1, 1])), ("profile_4", rule(5, 4, 0, 0, [1, 1, 1])),
        ("led_61", rule(2, 0, 61, 0, [1, 1, 1])), ("led_255", rule(2, 0, 255, 0, [1, 1, 1])),
        ("off_3", rule(2, 0, 0, 3, [1, 1, 1])), ("flag_bit3", rule(2, 0, 0, 8, [1, 1, 1])), ("flag_bit7", rule(2, 0, 0, 128, [1, 1, 1])),
        ("dim_without_dim_mode", rule(2, 0, 0, 1, [1, 1, 1], 5)), ("dim_with_keep", rule(2, 0, 0, 0, [1, 1, 1], 1)),
    ]:
        add(f"reject_rule_{name}", edit(blank_model(), lambda m, bad=bad: m["rules"].append(bad)))
    for name, ok in [("layer_3", rule(4, 3, 0, 0, [1, 1, 1])), ("profile_3", rule(5, 3, 0, 0, [1, 1, 1])), ("led_60", rule(2, 0, 60, 0, [1, 1, 1])),
                     ("dim_mode_dim_0", rule(2, 0, 0, 2, [1, 1, 1], 0)), ("dim_mode_dim_255", rule(2, 0, 0, 2 | 4, [1, 1, 1], 255))]:
        add(f"rule_{name}", edit(blank_model(), lambda m, ok=ok: m["rules"].append(ok)))
    return out


def seal(b):
    b = bytearray(b)
    b[-4:] = zlib.crc32(bytes(b[:-4])).to_bytes(4, "little")
    return bytes(b)


def le16(v):
    return int(v).to_bytes(2, "little")


def section(sid, payload, flags=0):
    return bytes([sid, flags]) + le16(len(payload)) + bytes(payload)


def blob(sections, count=None, version=1, magic=b"KMLS", reserved=b"\0" * 7, length=None, fix_crc=True):
    body = b"".join(sections)
    n = len(sections) if count is None else count
    total = 16 + len(body) + 4
    head = magic + le16(version) + le16(total if length is None else length) + bytes([n]) + reserved
    raw = head + body + b"\0\0\0\0"
    return seal(raw) if fix_crc else raw


def dec_cases(good):
    """Blobs for the decoder: (name, bytes). good = the firmware's own encoding of a scene with rules (used as the base)."""
    out = [("intact", good)]
    keys = bytes([10, 20, 30] + [0] * 45 + [15] + [0] * 60)
    anim = bytes([3, 7]) + (1234).to_bytes(4, "little", signed=False) + b"\0\0"
    rules1 = bytes([1]) + bytes([2, 0, 28, 2, 255, 0, 0, 30])
    out += [
        ("no_sections", blob([])),
        ("only_rules", blob([section(1, rules1)])),
        ("only_keys", blob([section(2, keys)])),
        ("only_anim", blob([section(3, anim)])),
        ("rules_zero", blob([section(1, bytes([0]))])),
        ("all_three_by_hand", blob([section(1, rules1), section(2, keys), section(3, anim)])),
        ("optional_after_known", blob([section(1, rules1), section(3, anim), section(0x80, b"abc"), section(0xF0, b"")])),
        ("optional_only", blob([section(0x80, b"x" * 40)])),
        ("optional_in_front", blob([section(0x80, b"abc"), section(3, anim)])),
        ("eight_optional", blob([section(0x80 + i, b"") for i in range(8)])),
        ("nine_optional", blob([section(0x80 + i, b"") for i in range(9)])),
        ("unknown_required_04", blob([section(4, b"\0")])),
        ("unknown_required_7f", blob([section(0x7F, b"")])),
        ("id_0", blob([section(0, b"")])),
        ("duplicate_anim", blob([section(3, anim), section(3, anim)])),
        ("order_swapped", blob([section(3, anim), section(2, keys)])),
        ("section_flags_1", blob([section(3, anim, flags=1)])),
        ("anim_len_7", blob([section(3, anim[:7])])),
        ("anim_len_9", blob([section(3, anim + b"\0")])),
        ("anim_reserved", blob([section(3, anim[:6] + b"\1\0")])),
        ("anim_flags_4", blob([section(3, bytes([4]) + anim[1:])])),
        ("anim_offset_over", blob([section(3, anim[:2] + (3600001).to_bytes(4, "little") + b"\0\0")])),
        ("anim_offset_max", blob([section(3, anim[:2] + (3600000).to_bytes(4, "little") + b"\0\0")])),
        ("anim_offset_neg_max", blob([section(3, anim[:2] + (-3600000 & 0xFFFFFFFF).to_bytes(4, "little") + b"\0\0")])),
        ("anim_offset_neg_over", blob([section(3, anim[:2] + (-3600001 & 0xFFFFFFFF).to_bytes(4, "little") + b"\0\0")])),
        ("anim_offset_int_min", blob([section(3, anim[:2] + (0x80000000).to_bytes(4, "little") + b"\0\0")])),
        ("keys_len_108", blob([section(2, keys[:-1])])),
        ("keys_len_110", blob([section(2, keys + b"\0")])),
        ("keys_index_16", blob([section(2, keys[:-1] + bytes([16]))])),
        ("rules_count_9", blob([section(1, bytes([9]) + bytes([1, 0, 0, 0, 0, 0, 0, 0]) * 9)])),
        ("rules_count_mismatch", blob([section(1, bytes([2]) + rules1[1:])])),
        ("rules_empty_section", blob([section(1, b"")])),
        ("rule_source_0", blob([section(1, bytes([1, 0, 0, 0, 0, 0, 0, 0, 0]))])),
        ("rule_dim_without_mode", blob([section(1, bytes([1, 2, 0, 5, 1, 1, 1, 1, 9]))])),
        ("rule_off_3", blob([section(1, bytes([1, 2, 0, 5, 3, 1, 1, 1, 0]))])),
        ("header_count_9", blob([], count=9)),
        ("header_count_too_few", blob([section(3, anim)], count=0)),
        ("header_count_too_many", blob([section(3, anim)], count=2)),
        ("header_version_0", blob([], version=0)),
        ("header_version_2", blob([], version=2)),
        ("header_magic", blob([], magic=b"KMLX")),
        ("header_reserved_1", blob([], reserved=b"\1" + b"\0" * 6)),
        ("header_reserved_last", blob([], reserved=b"\0" * 6 + b"\1")),
        ("header_length_plus_1", blob([section(3, anim)], length=16 + 12 + 4 + 1)),
        ("header_length_minus_1", blob([section(3, anim)], length=16 + 12 + 4 - 1)),
        ("crc_not_repaired", blob([section(3, anim)], fix_crc=False)),
        ("trailing_byte", seal(good[:-4] + b"\0\0\0\0\0")),
        ("empty", b""),
        ("too_short", good[:19]),
        ("all_zero", bytes(150)),
        ("all_ff", b"\xff" * 150),
    ]
    big = bytes([0x55]) * 900
    out += [
        ("size_924_optional", blob([section(0x80, big)])),
        ("size_1018_optional", blob([section(0x80, big), section(0x81, big[:90])])),
        ("size_1024_optional", blob([section(0x80, big), section(0x81, big[:96])])),
        ("size_1028_optional", blob([section(0x80, big), section(0x81, big[:100])])),
    ]
    for cut in (0, 1, 15, 16, 17, 19, 20, 21, 100, len(good) - 5, len(good) - 4, len(good) - 1):
        out.append((f"prefix_{cut}", good[:cut]))
    return out


SWEEP_VALUES = (0x00, 0x01, 0x7F, 0x80, 0xFF)


def build_spec(encs, decs):
    lines = ["CRC 313233343536373839"]
    for c in encs:
        m = c["model"]
        n = len(m["rules"])
        lines.append(f"ENC {c['name']} {n} {m['anim']['flags']} {m['anim']['clip']} {m['anim']['offsetMs']}")
        for r in m["rules"][:MAX_RULES]:
            lines.append(" ".join(str(x) for x in (r["source"], r["param"], r["led"], r["flags"], *r["color"], r["dim"])))
        lines.append(" ".join(str(x) for rgb in m["palette"] for x in rgb))
        lines.append(" ".join(str(x) for x in m["keys"]))
    for name, b in decs:
        lines.append(f"DEC {name} {b.hex() or '-'}")
    return "\n".join(lines) + "\n"


def compile_harness(tmp: Path):
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    cc = shutil.which("gcc", path=e["PATH"])
    if not cc:
        sys.exit("gcc not found (on Windows install MSYS2 ucrt64)")
    src = tmp / "harness.c"
    src.write_text(HARNESS)
    exe = tmp / ("harness.exe" if os.name == "nt" else "harness")
    cmd = [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-I" + str(FW / "core/include"), str(src)]
    cmd += [str(FW / "core/src" / f) for f in ("km_lightscene.c", "km_store.c", "km_settings.c", "km_keymap.c", "km_engine.c", "km_depth_model.c")]
    cmd += ["-o", str(exe)]
    if subprocess.run(cmd, env=e).returncode:
        sys.exit("harness build failed")
    return exe, e


def run(exe, env, spec_text, tmp, name):
    spec = tmp / name
    spec.write_text(spec_text)
    res = subprocess.run([str(exe), str(spec)], env=env, capture_output=True, text=True)
    if res.returncode:
        sys.exit(f"harness exited with {res.returncode}\n{res.stderr}")
    answers = {}
    for line in res.stdout.splitlines():
        parts = line.split(" ")
        if parts[0] == "R":
            answers[parts[1]] = parts[2] if len(parts) > 2 else ""
    return answers


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="do not write; fail if the committed vector file differs")
    args = ap.parse_args()

    encs = enc_cases()
    with tempfile.TemporaryDirectory(prefix="km_scene_vec_") as td:
        tmp = Path(td)
        exe, env = compile_harness(tmp)
        a1 = run(exe, env, build_spec(encs, []), tmp, "spec1.txt")
        sweep_base = bytes.fromhex(a1["random_3"])       # a scene with rules, of whatever size the generator gave it
        decs = dec_cases(bytes.fromhex(a1["caps_lock_example"]))
        sweep = [(pos, v) for pos in range(len(sweep_base) - 4) for v in SWEEP_VALUES if sweep_base[pos] != v]
        for pos, v in sweep:
            b = bytearray(sweep_base)
            b[pos] = v
            decs.append((f"sweep_{pos}_{v}", seal(bytes(b))))
        a2 = run(exe, env, build_spec([], decs), tmp, "spec2.txt")

    vec = {
        "about": "Generated by firmware/tools/gen_lightscene_vector.py from the real km_lightscene.c. Do not edit by hand.",
        "crc_check_123456789": a1["CRC"],
        "max_encoded": 214,
        "enc": [{"name": c["name"], "model": c["model"], "encoded": None if a1[c["name"]] == "REJECT" else a1[c["name"]]} for c in encs],
        "dec": [{"name": n, "input": b.hex(), "reencoded": None if a2[n] == "REJECT" else a2[n]} for n, b in decs if not n.startswith("sweep_")],
        "sweep": {"base": sweep_base.hex(), "values": list(SWEEP_VALUES),
                  "results": [[pos, v, None if a2[f"sweep_{pos}_{v}"] == "REJECT" else a2[f"sweep_{pos}_{v}"]] for pos, v in sweep]},
    }
    text = json.dumps(vec, indent=1, sort_keys=False) + "\n"
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print("stale: run python firmware/tools/gen_lightscene_vector.py")
            return 1
        print("up to date:", OUT)
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    n_ok = sum(1 for c in vec["enc"] if c["encoded"]) + sum(1 for c in vec["dec"] if c["reencoded"])
    print("wrote", OUT, f"({len(vec['enc'])} encode cases, {len(vec['dec'])} decode cases, {len(sweep)} sweep cases, {n_ok} accepted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
