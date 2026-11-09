#!/usr/bin/env python3
"""Mutation pass for the light scene (km_lightscene.c: the KMLS blob, its validity rules and its storage call).

    python firmware/tools/mutate_lightscene.py            # run every mutation, list survivors
    python firmware/tools/mutate_lightscene.py --list     # just print them
    python firmware/tools/mutate_lightscene.py --only anim

Same idea as mutate_ledstore.py: each mutation changes one expression in a scratch copy of km_lightscene.c, that file is recompiled (all
other objects come from one clean build) and the suite lightscene is run. "killed" = the build works and a check fails; a "survivor" means
the tests do not notice the change.
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SUITES = ["lightscene"]

L = "core/src/km_lightscene.c"

# (name, file, old text, new text); `old` must occur exactly once in the file.
MUTATIONS = [
    # ---- decode: header ----
    ("version not checked", L, "get16(buf + 4) != KM_LS_VERSION || ", ""),
    ("total length field not checked", L, "get16(buf + 6) != len || ", ""),
    ("section count not limited", L, " || buf[8] > KM_LS_MAX_RULES", ""),
    ("magic not checked", L, "memcmp(buf, \"KMLS\", 4) != 0 ||", "0 ||"),
    ("size limit not checked", L, "len > KM_LS_MAX_DECODE ||", ""),
    ("reserved header bytes not checked", L, "for (unsigned i = 9; i < KM_LS_HEADER_BYTES; i++) {", "for (unsigned i = 9; i < 9; i++) {"),
    ("last reserved header byte not checked", L, "for (unsigned i = 9; i < KM_LS_HEADER_BYTES; i++) {", "for (unsigned i = 9; i < KM_LS_HEADER_BYTES - 1u; i++) {"),
    ("checksum not checked", L, "if (km_crc32(0, buf, len - KM_LS_CRC_BYTES) != get32(buf + len - KM_LS_CRC_BYTES)) {", "if (0) {"),
    ("checksum over one byte too little", L, "if (km_crc32(0, buf, len - KM_LS_CRC_BYTES) !=", "if (km_crc32(0, buf, len - KM_LS_CRC_BYTES - 1u) !="),
    # ---- decode: sections ----
    ("section flags not checked", L, "if (flags != 0u || id <= prev || plen > end - o) {", "if ((void)flags, id <= prev || plen > end - o) {"),
    ("the same id may repeat", L, "if (flags != 0u || id <= prev || plen > end - o) {", "if (flags != 0u || id < prev || plen > end - o) {"),
    ("ids need not increase", L, "if (flags != 0u || id <= prev || plen > end - o) {", "if (flags != 0u || (id <= prev && flags == 77u) || plen > end - o) {"),
    ("section length not checked against the blob", L, "if (flags != 0u || id <= prev || plen > end - o) {", "if (flags != 0u || id <= prev) {"),
    ("section header bounds not checked", L, "        if (end - o < 4u) {\n            return false;\n        }\n", ""),
    ("unknown required section accepted", L, "default:              ok = id >= 0x80u; break;", "default:              ok = true; break;"),
    ("unknown optional section refused", L, "default:              ok = id >= 0x80u; break;", "default:              ok = false; break;"),
    ("bytes after the last section accepted", L, "    if (o != end) {\n        return false;", "    if (o > end) {\n        return false;"),
    ("decoder keeps the old value of missing sections", L, "    km_lightscene_default(&t);\n    const unsigned end", "    t = *s;\n    const unsigned end"),
    # ---- decode: section contents ----
    ("rule section length not checked against the count", L, "if (len < 1u || p[0] > KM_LS_MAX_RULES || len != 1u + p[0] * KM_LS_RULE_BYTES) {", "if (len < 1u || p[0] > KM_LS_MAX_RULES) {"),
    ("rule count not limited", L, "if (len < 1u || p[0] > KM_LS_MAX_RULES ||", "if (len < 1u ||"),
    ("decoded rules not validated", L, "        if (!km_ls_rule_valid(r)) {\n            return false;\n        }\n    }\n    return true;\n}\n\nstatic bool parse_keys", "        (void)r;\n    }\n    return true;\n}\n\nstatic bool parse_keys"),
    ("keys section length loosened", L, "if (len != KM_LS_KEYS_BYTES) {", "if (len < KM_LS_KEYS_BYTES) {"),
    ("decoded palette index not checked", L, "        if (t->key_idx[i] >= KM_LS_PALETTE) {\n            return false;\n        }\n    }\n    return true;\n}\n\nstatic bool parse_anim", "    }\n    return true;\n}\n\nstatic bool parse_anim"),
    ("animation section length loosened", L, "if (len != KM_LS_ANIM_BYTES ||", "if (len < KM_LS_ANIM_BYTES ||"),
    ("animation flags not checked", L, "(p[0] & ~KM_LS_ANIM_MASK) != 0u ||", "0 ||"),
    ("animation reserved bytes not checked", L, " || get16(p + 6) != 0u", ""),
    ("animation offset not range checked", L, "    if (off < -KM_LS_ANIM_OFFSET_MAX || off > KM_LS_ANIM_OFFSET_MAX) {\n        return false;\n    }\n    t->anim_flags", "    t->anim_flags"),
    ("animation offset read as unsigned", L, "const int32_t off = (int32_t)get32(p + 2);", "const int32_t off = (int32_t)(get32(p + 2) & 0x7FFFFFFFu);"),
    # ---- validity ----
    ("source 0 accepted", L, "if (r->source < KM_LS_SRC_HOST_NUM || r->source > KM_LS_SRC_LAST) {", "if (r->source > KM_LS_SRC_LAST) {"),
    ("source above the last accepted", L, "if (r->source < KM_LS_SRC_HOST_NUM || r->source > KM_LS_SRC_LAST) {", "if (r->source < KM_LS_SRC_HOST_NUM) {"),
    ("layer parameter range", L, "(r->source == KM_LS_SRC_LAYER) ? LAYERS_MAX", "(r->source == KM_LS_SRC_LAYER) ? 8u"),
    ("profile parameter range", L, "(r->source == KM_LS_SRC_PROFILE) ? PROFILES_MAX", "(r->source == KM_LS_SRC_PROFILE) ? 8u"),
    ("parameter allowed on parameter-less sources", L, "PROFILES_MAX : 1u;", "PROFILES_MAX : 2u;"),
    ("light index range", L, "r->led >= KM_LED_COUNT ||", "r->led > KM_LED_COUNT ||"),
    ("rule flag bits not checked", L, "(r->flags & ~KM_LS_FLAG_MASK) != 0u", "0"),
    ("off behaviour 3 accepted", L, "    if (off > KM_LS_OFF_DIM) {\n        return false;\n    }\n", ""),
    ("dim without the DIM behaviour accepted", L, "return off == KM_LS_OFF_DIM || r->dim == 0u;", "return true;"),
    ("too many rules valid", L, "if (s->n_rules > KM_LS_MAX_RULES ||", "if (0 ||"),
    ("animation flags valid check", L, "(s->anim_flags & ~KM_LS_ANIM_MASK) != 0u ||", "0 ||"),
    ("animation offset below range valid", L, "s->anim_offset_ms < -KM_LS_ANIM_OFFSET_MAX ||", "0 ||"),
    ("animation offset above range valid", L, " || s->anim_offset_ms > KM_LS_ANIM_OFFSET_MAX) {", ") {"),
    ("scene rules not checked", L, "        if (!km_ls_rule_valid(&s->rule[i])) {", "        if (0) {"),
    ("scene palette index not checked", L, "        if (s->key_idx[i] >= KM_LS_PALETTE) {", "        if (0) {"),
    # ---- encode ----
    ("rule colour channels swapped", L, "buf[o + 4] = r->on.r;", "buf[o + 4] = r->on.g;"),
    ("rule dim lost", L, "buf[o + 7] = r->dim;", "buf[o + 7] = 0;"),
    ("rule flags lost", L, "buf[o + 3] = r->flags;", "buf[o + 3] = 0;"),
    ("rule light lost", L, "buf[o + 2] = r->led;", "buf[o + 2] = 0;"),
    ("palette blue replaced by green", L, "buf[o++] = s->palette[i].b;", "buf[o++] = s->palette[i].g;"),
    ("key indices not written", L, "memcpy(buf + o, s->key_idx, KM_LED_COUNT);", "memset(buf + o, 0, KM_LED_COUNT);"),
    ("animation clip lost", L, "buf[o + 1] = s->anim_clip;", "buf[o + 1] = 0;"),
    ("animation offset lost", L, "put32(buf + o + 2, (uint32_t)s->anim_offset_ms);", "put32(buf + o + 2, 0);"),
    ("animation flags lost", L, "buf[o + 0] = s->anim_flags;", "buf[o + 0] = 0;"),
    ("section count in the header", L, "    buf[8] = 3;", "    buf[8] = 2;"),
    ("checksum over too little", L, "put32(buf + o, km_crc32(0, buf, o));", "put32(buf + o, km_crc32(0, buf, o - 1u));"),
    ("total length field", L, "put16(buf + 6, total);", "put16(buf + 6, total - 4u);"),
    ("encode does not validate", L, "if (buf == NULL || s == NULL || !km_lightscene_valid(s)) {", "if (buf == NULL || s == NULL) {"),
    ("encode ignores the buffer size", L, "    if (cap < total) {\n        return 0;\n    }\n", "    if (cap < total && cap == 12345u) {\n        return 0;\n    }\n"),
    ("default palette green", L, "s->palette[0].g = COLOR_DEFAULT_G;", "s->palette[0].g = 0;"),
    ("default palette blue", L, "s->palette[0].b = COLOR_DEFAULT_B;", "s->palette[0].b = 0;"),
    # ---- storage ----
    ("save writes the whole buffer", L, "km_store_write(st, buf, n);", "km_store_write(st, buf, sizeof buf);"),
]

EQUIVALENT = {
    "section length not checked against the blob": "defence in depth: without it the section would reach into the checksum and the closing length test (o != end) refuses the blob anyway; the check keeps the parsers from READING past the buffer",
    "section header bounds not checked": "defence in depth: the header would be read from the checksum bytes and the closing length test refuses the blob anyway",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def load_runner():
    spec = importlib.util.spec_from_file_location("run_tests", FW / "tools" / "run_tests.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", help="run only mutations whose name contains this text")
    args = ap.parse_args()
    muts = [m for m in MUTATIONS if m[0] not in EQUIVALENT]
    if args.list:
        for i, m in enumerate(muts, 1):
            print("%2d  %s" % (i, m[0]))
        return 0

    rt = load_runner()
    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_ls_"))

    def compile_one(src: Path, obj: Path, kind: str, root: Path) -> bool:
        inc = ["-I" + str(root / "core/include"), "-I" + str(root / "hid/include"), "-I" + str(root / "usb/include"),
               "-I" + str(root / "tests"), "-I" + str(root / "tests/sim"), "-I" + str(root / "port/n32h48x")]
        if kind == "lib":
            cmd = [cc, *rt.STRICT, *inc, "-c", str(src), "-o", str(obj)]
        else:
            cmd = [cc, *rt.STRICT, "-DKM_FLASH_HOST_TEST", "-DKM_SENSE_HOST_TEST", "-DKM_STORE_HOST_TEST", "-DKM_LED_HOST_TEST", *inc,
                   "-c", str(src), "-o", str(obj)]
        return subprocess.run(cmd, env=e, capture_output=True).returncode == 0

    survivors, broken = [], []
    try:
        # one clean build of everything, kept as objects keyed by the source path relative to FW
        objs = {}
        kinds = {}
        for src in rt.LIB_SOURCES:
            kinds[src] = "lib"
        for src in rt.SIM_SOURCES:
            kinds[src] = "sim"
        for src, kind in kinds.items():
            rel = src.relative_to(FW).as_posix()
            obj = work / ("base_" + rel.replace("/", "_") + ".o")
            if not compile_one(src, obj, kind, FW):
                print("clean build failed:", rel)
                return 2
            objs[rel] = obj
        # led_hw.c at levels 1 and 0, as run_tests.py does
        extra = []
        for level, prefix in ((1, "l1_"), (0, "l0_")):
            obj = work / ("base_led_hw_level%d.o" % level)
            ren = ["-Dled_hw_%s=%s%s" % (n, prefix, n) for n in ("start", "kill", "send", "poll", "get_diag", "test_reset", "test_last_frame")]
            ren = [r.replace(prefix + "test_reset", prefix + "reset") for r in ren]
            cmd = [cc, *rt.STRICT, "-DKM_LED_HOST_TEST", "-DKM_LED_HW_LEVEL=%d" % level, *ren, *rt.INCLUDES, "-c",
                   str(FW / "port/n32h48x/led_hw.c"), "-o", str(obj)]
            if subprocess.run(cmd, env=e, capture_output=True).returncode != 0:
                print("clean build failed: led_hw level", level)
                return 2
            extra.append(str(obj))
        for src in rt.TEST_SOURCES:
            obj = work / ("t_" + src.stem + ".o")
            cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-DKM_SENSE_HOST_TEST", *rt.INCLUDES, "-c", str(src), "-o", str(obj)]
            if subprocess.run(cmd, env=e, capture_output=True).returncode != 0:
                print("clean build failed:", src)
                return 2
            extra.append(str(obj))

        def run_suites(exe: Path):
            try:
                r = subprocess.run([str(exe), *SUITES], env=e, capture_output=True, text=True, timeout=300)
                return r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                return 1, "(timeout)"

        exe = work / "clean.exe"
        subprocess.run([cc, *[str(o) for o in objs.values()], *extra, "-o", str(exe)], env=e, check=True)
        rc, out = run_suites(exe)
        if rc != 0:
            print("the clean build does not pass its own suites:\n" + out[-600:])
            return 2

        for i, (name, rel, old, new) in enumerate(muts, 1):
            if args.only and args.only not in name:
                continue
            src = FW / rel
            text = src.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-52s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            msrc = work / ("mut_" + src.name)
            msrc.write_text(text.replace(old, new), newline="")
            mobj = work / ("mut_%d.o" % i)
            kind = kinds[src]
            if not compile_one(msrc, mobj, kind, FW):
                print("%2d  %-52s BUILD FAILED" % (i, name))
                broken.append(name)
                continue
            mexe = work / ("m%d.exe" % i)
            link = [str(mobj) if k == rel else str(o) for k, o in objs.items()]
            r = subprocess.run([cc, *link, *extra, "-o", str(mexe)], env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-52s LINK FAILED" % (i, name))
                broken.append(name)
                continue
            rc, out = run_suites(mexe)
            if rc == 0:
                print("%2d  %-52s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%2d  %-52s killed  %s" % (i, name, first[-70:]))
            for f in (mexe, mobj, msrc):
                f.unlink(missing_ok=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(muts), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
