#!/usr/bin/env python3
"""Mutation pass for the animation decoder (km_anim.c, M6 package P7b).

    python firmware/tools/mutate_anim.py            # run every mutation, list survivors
    python firmware/tools/mutate_anim.py --list     # just print them

Same idea as mutate_health.py: each mutation changes one expression in a scratch copy of km_anim.c, rebuilds tests/test_anim.c
(stand-alone, -DKM_ANIM_STANDALONE) and runs it. "killed" = the build works and a check fails (or the run hangs / crashes);
a "survivor" means the tests do not notice the change.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
A = "core/src/km_anim.c"

# (name, old text, new text); `old` must occur exactly once in km_anim.c.
MUTATIONS = [
    # ---- bit reader and codes
    ("reader may run 8 bits past the end", "n > r->end - r->pos", "n > r->end - r->pos + 8u"),
    ("reader ignores an earlier error", "if (r->err || n > r->end - r->pos) {", "if (n > r->end - r->pos) {"),
    ("Exp-Golomb prefix unlimited", "++q > EG_MAX_PREFIX", "++q > 20u"),
    ("Exp-Golomb order ignored", "return (((1u << q) - 1u) << k) + br_bits(r, q + k);", "return (((1u << q) - 1u)) + br_bits(r, q + k);"),
    ("Exp-Golomb suffix one bit short", "br_bits(r, q + k);", "br_bits(r, q + k - (q + k > 0u));"),
    # ---- frame decoding
    ("more than 61 changes accepted", "count > KM_ANIM_LIGHTS)", "count > KM_ANIM_LIGHTS + 1u)"),
    ("cell 61 accepted", "if (r.err || cell >= KM_ANIM_LIGHTS) {", "if (r.err || cell > KM_ANIM_LIGHTS) {"),
    ("no-op change accepted", "            if (v == a->cur[cell]) {\n                return KM_ANIM_E_NOOP;\n            }\n", ""),
    ("1-bit change sets instead of toggles", "a->cur[cell] ^= 1u;", "a->cur[cell] = 1u;"),
    ("gap counted from the previous cell, not after it", "        cell++;\n    }\n    a->bitpos = r.pos;", "    }\n    a->bitpos = r.pos;"),
    ("group may end early", "if (((pos + 7u) >> 3) != group_end_byte(a, g)) {", "if (((pos + 7u) >> 3) > group_end_byte(a, g)) {"),
    ("padding bits not checked", "if ((pos & 7u) != 0u && (a->info.data[pos >> 3] & (0xFFu >> (pos & 7u))) != 0u) {", "if (0) {"),
    ("padding mask one bit short", "(0xFFu >> (pos & 7u))", "(0x7Fu >> (pos & 7u))"),
    ("last frame of the clip is not a group end", " || frame + 1u == a->info.frames;", ";"),
    ("key frame group end not checked", "if (e == KM_ANIM_OK && last_in_group(a, g * a->info.key_interval)) {", "if (0) {"),
    ("frame group end not checked", "if (e == KM_ANIM_OK && last_in_group(a, next)) {", "if (0) {"),
    ("key frame read with one bit too few", "a->cur[i] = (uint8_t)br_bits(&r, a->info.bits);", "a->cur[i] = (uint8_t)br_bits(&r, a->info.bits - (a->info.bits > 1u));"),
    ("blend source frame not kept", "    memcpy(a->prev, a->cur, sizeof a->cur);\n    if (next % a->info.key_interval == 0u) {", "    if (next % a->info.key_interval == 0u) {"),
    ("seek leaves the blend source stale", "    memcpy(a->prev, a->cur, sizeof a->cur);\n    a->dec_idx = (uint16_t)(g * a->info.key_interval);", "    a->dec_idx = (uint16_t)(g * a->info.key_interval);"),
    ("diff frame may read to the end of the data", "br_t r = { a->info.data, a->bitpos, group_end_byte(a, g) * 8u, false };", "br_t r = { a->info.data, a->bitpos, a->info.data_len * 8u, false };"),
    ("seek ignores the index", "br_t r = { a->info.data, rd16(a->info.index + 2u * g) * 8u, group_end_byte(a, g) * 8u, false };", "br_t r = { a->info.data, 0u, group_end_byte(a, g) * 8u, false };"),
    # ---- the clock and the cursor
    ("frame boundary a little late", "(unsigned)(x / 1000u), rem", "(unsigned)((x) / 1001u), rem"),
    ("end reached one frame late", "if (x >= total) {", "if (x >= total + 1000u) {"),
    ("end of clip off by one tick", "if (x >= total) {", "if (x > total) {"),
    ("no loop wrap", "        x %= total;", "        x = total - 1u;"),
    ("negative offset not held", "    if (pos_ms < 0) {\n        pos_ms = 0;\n    }\n", ""),
    ("clock uses absolute time", "(int64_t)(uint32_t)(now_ms - a->start_ms)", "(int64_t)now_ms"),
    ("fps not applied", "uint64_t x = (uint64_t)pos_ms * a->info.fps;", "uint64_t x = (uint64_t)pos_ms * 10u;"),
    ("blend does not look one frame ahead", "        want = f + 1u;\n", "        want = f;\n"),
    ("blend wrap goes nowhere", "            if (loop) {\n                want = 0;", "            if (0) {\n                want = 0;"),
    ("blend at the end does not hold the last frame", "                want = n - 1u;\n                alpha = KM_ANIM_ALPHA_STEPS;", "                want = n - 1u;"),
    ("blend rounds down", "KM_ANIM_ALPHA_STEPS / 2u) /\n", "0u) /\n"),
    ("blend has the wrong direction", "pal[a->prev[i]] * (KM_ANIM_ALPHA_STEPS - alpha) + pal[a->cur[i]] * alpha", "pal[a->prev[i]] * alpha + pal[a->cur[i]] * (KM_ANIM_ALPHA_STEPS - alpha)"),
    ("alpha steps wrong", "alpha = (rem * KM_ANIM_ALPHA_STEPS) / 1000u;", "alpha = (rem * 8u) / 1000u;"),
    ("lagging output is blended", "    if (a->dec_idx != want || !blend) {\n        alpha = KM_ANIM_ALPHA_STEPS;", "    if (!blend) {\n        alpha = KM_ANIM_ALPHA_STEPS;"),
    ("never seeks", "if (d > 1u + want % key) {", "if (0) {"),
    ("seeks every call", "if (d > 1u + want % key) {", "if (d > 0u) {"),
    ("seek threshold ignores the wanted group start", "if (d > 1u + want % key) {", "if (d > 1u) {"),
    ("backwards distance wrong in a loop", "d = (want + n - a->dec_idx) % n;", "d = want - a->dec_idx;"),
    ("backwards without a loop is a short step", "d = want > a->dec_idx ? want - a->dec_idx : n;", "d = want > a->dec_idx ? want - a->dec_idx : 1u;"),
    ("unbounded catch-up", "steps < KM_ANIM_MAX_STEPS", "steps < 1000u"),
    ("catch-up of one frame per call", "steps < KM_ANIM_MAX_STEPS", "steps < 1u"),
    ("an error does not stop the player", "    a->state = KM_ANIM_ERROR;\n", ""),
    ("an error does not invalidate the cursor", "    a->err = (uint8_t)e;\n    a->cur_valid = 0;", "    a->err = (uint8_t)e;"),
    ("a new frame is not remembered", "    a->has_out = 1u;\n", ""),
    ("unchanged frame is reported again", "    if (a->has_out && a->last_idx == a->dec_idx && a->last_alpha == alpha) {\n        return false;\n    }\n", ""),
    ("start keeps the old output marker", "    a->err = KM_ANIM_OK;\n    a->has_out = 0;", "    a->err = KM_ANIM_OK;"),
    ("stop does not stop", "void km_anim_stop(km_anim_t *a)\n{\n    a->state = KM_ANIM_IDLE;", "void km_anim_stop(km_anim_t *a)\n{"),
    ("work counter not reset", "    a->work = 0;\n    if (a->state != KM_ANIM_PLAYING) {", "    if (a->state != KM_ANIM_PLAYING) {"),
    # ---- header
    ("magic not checked", 'if (memcmp(data, "KMAN", 4) != 0) {', "if (0) {"),
    ("version not checked", "if (data[4] != KM_ANIM_VERSION) {", "if (0) {"),
    ("light count not checked", "if (data[5] != KM_ANIM_LIGHTS || ", "if ("),
    ("3 bits accepted", "(bits != 1u && bits != 2u && bits != 4u)", "(bits != 1u && bits != 2u && bits != 3u && bits != 4u)"),
    ("fps 61 accepted", "fps > 60u", "fps > 61u"),
    ("fps 0 accepted", "fps < 1u || ", ""),
    ("zero frames accepted", "frames < 1u ||", ""),
    ("key interval 0 accepted", "key < 1u ||", "0 ||"),
    ("reserved flag bits accepted", "(flags & ~1u) != 0u", "0"),
    ("kc 5 accepted", "kc > 4u", "kc > 5u"),
    ("kg 5 accepted", "kg > 4u", "kg > 5u"),
    ("group count not checked", "groups != groups_for(frames, key) ||", ""),
    ("too many groups accepted", "groups > KM_ANIM_MAX_GROUPS ||", ""),
    ("empty data accepted", "data_len < 1u ||", ""),
    ("reserved field not checked", "|| rd16(data + 18) != 0u", ""),
    ("length not checked", "    if (len != total) {", "    if (len < total) {"),
    ("short length not checked", "if (len < KM_ANIM_HEADER_BYTES + 2u + 2u + 1u + 4u) {", "if (0) {"),
    ("palette may repeat", "if (pal[i] <= pal[i - 1u]) {", "if (pal[i] < pal[i - 1u]) {"),
    ("palette not checked", "if (pal[i] <= pal[i - 1u]) {", "if (0) {"),
    ("first group may start late", "(g == 0u && off != 0u) || ", ""),
    ("index may repeat", "(g > 0u && off <= prev)", "(g > 0u && off < prev)"),
    ("index may point past the data", " || off >= data_len || next > data_len", " || next > data_len"),
    ("tiny groups accepted", " ||\n            next - off < min_group) {", ") {"),
    ("stored checksum read from the wrong place", "info->crc = rd32(data + total - 4u);", "info->crc = rd32(data + total - 8u);"),
    # ---- verifier
    ("checksum not compared", "if ((v->crc ^ 0xFFFFFFFFu) != info->crc) {", "if (0) {"),
    ("checksum not finalized", "if ((v->crc ^ 0xFFFFFFFFu) != info->crc) {", "if (v->crc != info->crc) {"),
    ("checksum skips the last byte", "const uint32_t body = info->total_len - 4u;", "const uint32_t body = info->total_len - 5u;"),
    ("verifier skips the frame decode", "    for (unsigned i = 0; i < budget && v->frame < info->frames; i++) {", "    for (unsigned i = 0; i < budget && v->frame < 0u; i++) {"),
    ("verifier finishes one frame early", "    if (v->frame == info->frames) {", "    if (v->frame + 1u >= info->frames) {"),
    ("verifier ignores a decode error", "        if (e != KM_ANIM_OK) {\n            v->err = (uint8_t)e;", "        if (0) {\n            v->err = (uint8_t)e;"),
    ("duration rounds up", "return (uint32_t)(((uint64_t)info->frames * 1000u) / info->fps);", "return (uint32_t)(((uint64_t)info->frames * 1000u + info->fps - 1u) / info->fps);"),
]

# mutations that cannot change behaviour (name -> why)
EQUIVALENT = {
    "reader ignores an earlier error": "the error flag is sticky and every caller looks at it; the bounds test alone keeps reads inside",
    "Exp-Golomb prefix unlimited": "a prefix longer than 7 zeros means a value >= 255, which the count / cell checks refuse anyway (the limit only "
                                   "avoids long loops and shift overflow in the unmutated code)",
    "more than 61 changes accepted": "62 changes cannot fit into 61 cells: the cell check refuses the last one",
    "index may repeat": "an index entry equal to the previous one gives a group of 0 bytes, which the minimum group size refuses",
    "index may point past the data": "an offset past the data leaves less than a key frame for its group: the minimum group size refuses it",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


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
    work = Path(tempfile.mkdtemp(prefix="km_mut_a_"))
    survivors, broken = [], []
    try:
        for i, (name, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests")
            target = tree / A
            text = target.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-52s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-DKM_ANIM_STANDALONE", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"),
                   str(tree / "tests/test_anim.c"), str(target), "-o", str(exe)]
            r = subprocess.run(cmd, env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-52s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                broken.append(name)
                continue
            try:
                r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=120)
                rc, out = r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                rc, out = 1, "(timeout)"
            if rc == 0 and name in EQUIVALENT:
                print("%2d  %-52s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif rc == 0:
                print("%2d  %-52s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%2d  %-52s killed  %s" % (i, name, first[:80]))
            shutil.rmtree(tree, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
