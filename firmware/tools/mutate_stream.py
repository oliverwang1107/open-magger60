#!/usr/bin/env python3
"""Mutation pass for the light frame stream (M6 P7a): change one comparison, constant or statement at a time in a scratch copy
of firmware/, build tests/test_stream.c as a standalone program and require it to FAIL (or crash) for every mutation.

    python firmware/tools/mutate_stream.py            # all mutations
    python firmware/tools/mutate_stream.py 3 7        # only mutations 3 and 7

A mutation that the tests do not notice ("SURVIVED") means a missing test: add one. The real tree is never modified. Only the
stream suite is built (-DKM_STREAM_STANDALONE), so a pass takes about a minute; the file that holds the mutation (or, for a
header, everything) is recompiled, the rest comes from a cache.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

ST = "hid/src/km_stream.c"
VD = "hid/src/km_vendor.c"
HID_H = "hid/include/km/km_hid.h"

# (name, file, text that must occur exactly once, replacement)
MUTATIONS = [
    # ---- time-out and clock
    ("time-out at >= becomes >", ST, "(uint32_t)(now_ms - s->last_rx_ms) >= s->timeout_ms", "(uint32_t)(now_ms - s->last_rx_ms) > s->timeout_ms"),
    ("tick does not store the clock", ST, "    s->now_ms = now_ms;\n    if (s->state", "    if (s->state"),
    ("closing keeps the session id", ST, "    s->state = KM_STREAM_IDLE;\n    s->session = 0;\n", "    s->state = KM_STREAM_IDLE;\n"),
    ("init does not enable", ST, "    s->enabled = true;\n", ""),
    ("START without time-out gets 0", ST, "timeout != 0 ? timeout : KM_STREAM_TIMEOUT_DEFAULT_MS;", "timeout != 0 ? timeout : 0;"),
    ("time-out range: minimum excluded", ST, "timeout < KM_STREAM_TIMEOUT_MIN_MS", "timeout <= KM_STREAM_TIMEOUT_MIN_MS"),
    ("time-out range: maximum excluded", ST, "timeout > KM_STREAM_TIMEOUT_MAX_MS", "timeout >= KM_STREAM_TIMEOUT_MAX_MS"),
    ("disabling does not close the session", ST, "    if (!enabled) {\n        close_session(s);\n    }\n", ""),
    ("START ignores 'disabled'", ST, "    if (!s->enabled) {\n        return send_reply", "    if (false) {\n        return send_reply"),
    # ---- acknowledgements
    ("ack limit < becomes <=", ST, "< KM_STREAM_ACK_MIN_MS", "<= KM_STREAM_ACK_MIN_MS"),
    ("every reply counts for the ack limit", ST, "    if (limited) {\n        s->replied = true;", "    if (true) {\n        s->replied = true;"),
    ("ack limit never applies", ST, "if (limited && s->replied &&", "if (false && s->replied &&"),
    ("reply echoes no sequence number", ST, "req->offset & 0xFFFFu, d, REPLY_BYTES", "0, d, REPLY_BYTES"),
    ("reply does not echo the op", ST, "d[0]  = req->len > 0 ? req->data[0] : 0;", "d[0]  = 0;"),
    ("reply formats mask without gray", ST, "d[19] = (uint8_t)((1u << KM_STREAM_FMT_RGB8) | (1u << KM_STREAM_FMT_GRAY8));", "d[19] = (uint8_t)(1u << KM_STREAM_FMT_RGB8);"),
    ("reply time-out high byte lost", ST, "d[21] = (uint8_t)(s->timeout_ms >> 8);", "d[21] = 0;"),
    ("reply counters swapped (lost/stale)", ST, "d[12] = (uint8_t)s->stats.lost;", "d[12] = (uint8_t)s->stats.stale;"),
    ("reply last seq high byte lost", ST, "d[5]  = (uint8_t)(s->last_seq >> 8);", "d[5]  = 0;"),
    ("reply window wrong", ST, "d[7]  = (uint8_t)KM_STREAM_WINDOW;", "d[7]  = (uint8_t)(KM_STREAM_WINDOW + 1u);"),
    # ---- validation and malformed packets
    ("bad run: > instead of >=", ST, "s->bad_run >= KM_STREAM_BAD_RUN_MAX", "s->bad_run > KM_STREAM_BAD_RUN_MAX"),
    ("malformed packets are not counted", ST, "    sat_inc(&s->stats.rejected);\n    if (s->bad_run", "    if (s->bad_run"),
    ("a valid packet does not reset the bad run", ST, "    s->last_seq = seq;\n    s->bad_run = 0;\n", "    s->last_seq = seq;\n"),
    ("a short request is answered", ST, "if (req->len < COMMON_BYTES) {", "if (req->len < 2) {"),
    ("flags are not checked", ST, "        (req->data[2] & (uint8_t)~KM_STREAM_FLAG_ACK_REQ) != 0) {", "        false) {"),
    ("XOR key accepted", ST, "req->xor_key != 0 ||", ""),
    ("wide offset accepted", ST, "req->offset > 0xFFFFu ||", ""),
    ("frame chunks are 'control'", ST, "const bool control = op != KM_STREAM_OP_FRAME;", "const bool control = true;"),
    ("keep-alive is an unknown op", ST, "op != KM_STREAM_OP_STOP && op != KM_STREAM_OP_KEEPALIVE)", "op != KM_STREAM_OP_STOP)"),
    ("START: session 0 accepted", ST, "if (d[1] == 0 || d[3]", "if (d[3]"),
    ("START: any version accepted", ST, "d[3] != KM_STREAM_PROTO_VERSION ||", ""),
    ("START: format 2 accepted", ST, "d[4] > KM_STREAM_FMT_GRAY8", "d[4] > 2"),
    ("START: reserved byte ignored", ST, "d[4] > KM_STREAM_FMT_GRAY8 || d[7] != 0 ||", "d[4] > KM_STREAM_FMT_GRAY8 ||"),
    ("START: length not checked", ST, "if (req->len != START_BYTES) {", "if (req->len < START_BYTES) {"),
    ("START does not set the sequence base", ST, "    s->last_seq = (uint16_t)req->offset;\n", ""),
    ("START keeps the statistics", ST, "    memset(&s->stats, 0, sizeof s->stats);\n", ""),
    ("START leaves OPEN", ST, "    s->state = KM_STREAM_OPEN;\n    s->mask = 0;", "    s->state = KM_STREAM_LIVE;\n    s->mask = 0;"),
    ("START keeps the old frame number", ST, "    s->mask = 0;\n    s->cur_valid = false;\n    s->cur_done = false;\n    s->last_seq", "    s->mask = 0;\n    s->last_seq"),
    ("frame: header length 6 -> 4", ST, "if (req->len < FRAME_HDR_BYTES) {\n        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, false", "if (req->len < 4) {\n        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, false"),
    ("frame: any session accepted", ST, "if (s->state == KM_STREAM_IDLE || d[1] != s->session) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, false", "if (s->state == KM_STREAM_IDLE) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, false"),
    ("frame: idle state accepted", ST, "if (s->state == KM_STREAM_IDLE || d[1] != s->session) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, false", "if (d[1] != s->session) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, false"),
    ("frame: count 0 accepted", ST, "if (count == 0 || first", "if (first"),
    ("frame: runs over the end", ST, "count > KM_STREAM_LIGHTS - first)", "count > KM_STREAM_LIGHTS)"),
    ("frame: last light excluded", ST, "count > KM_STREAM_LIGHTS - first)", "count >= KM_STREAM_LIGHTS - first)"),
    ("frame: length may be longer", ST, "req->len != FRAME_HDR_BYTES + count * bpp", "req->len < FRAME_HDR_BYTES + count * bpp"),
    ("frame: gray and rgb swapped in the length", ST, "s->fmt == KM_STREAM_FMT_GRAY8 ? 1u : 3u", "s->fmt == KM_STREAM_FMT_GRAY8 ? 3u : 1u"),
    ("frame: wrong pixel offset", ST, "uint8_t *dst = s->staging + first * 3u;", "uint8_t *dst = s->staging + first * 2u;"),
    ("frame: gray not expanded to blue", ST, "dst[3u * i] = dst[3u * i + 1u] = dst[3u * i + 2u] = src[i];", "dst[3u * i] = dst[3u * i + 1u] = src[i];"),
    ("frame: coverage of a chunk one bit short", ST, "s->mask |= ((1ull << count) - 1ull) << first;", "s->mask |= ((1ull << (count - 1u)) - 1ull) << first;"),
    ("frame: mask of full frame wrong", ST, "#define FULL_MASK       ((1ull << KM_STREAM_LIGHTS) - 1ull)", "#define FULL_MASK       ((1ull << (KM_STREAM_LIGHTS - 1u)) - 1ull)"),
    # ---- sequence numbers and frame numbers
    ("seq: half the space is 'ahead'", ST, "diff == 0u || diff >= 0x8000u", "diff == 0u || diff > 0x8000u"),
    ("seq: older numbers accepted", ST, "if (diff == 0u || diff >= 0x8000u) {", "if (diff == 0u) {"),
    ("seq: gap off by one", ST, "return (int)(diff - 1u);", "return (int)diff;"),
    ("seq: lost counter wraps instead of saturating", ST, "s->stats.lost = lost > 0xFFFFu ? 0xFFFFu : (uint16_t)lost;", "s->stats.lost = (uint16_t)lost;"),
    ("seq: accepted packet does not keep alive", ST, "    s->bad_run = 0;\n    s->last_rx_ms = s->now_ms;\n}", "    s->bad_run = 0;\n}"),
    ("frame number: 128 ahead is newer", ST, "(fdiff != 0 && fdiff < 0x80u)", "(fdiff != 0 && fdiff <= 0x80u)"),
    ("frame number: every different number is newer", ST, "(fdiff != 0 && fdiff < 0x80u)", "(fdiff != 0)"),
    ("frame number: same number restarts the frame", ST, "(fdiff != 0 && fdiff < 0x80u)", "(true)"),
    ("dropped frames: complete frames counted too", ST, "s->mask != 0 && !s->cur_done", "s->mask != 0"),
    ("dropped frames: not counted", ST, "            sat_inc(&s->stats.dropped);                 /* the older frame never completed */\n", ""),
    ("committed frame accepts more chunks", ST, "    if (s->cur_done) {                                   /* a chunk of the frame that is on show already */", "    if (false) {"),
    ("duplicates are not counted stale", ST, "    if (gap < 0) {\n        sat_inc(&s->stats.stale);\n        return want && send_reply(s, req, KM_STREAM_ERR_STALE, true, out);", "    if (gap < 0) {\n        return want && send_reply(s, req, KM_STREAM_ERR_STALE, true, out);"),
    # ---- commit and consumer interface
    ("commit does not count the serial", ST, "    s->serial++;\n    s->stats.committed++;", "    s->stats.committed++;"),
    ("commit does not count in the statistics", ST, "    s->serial++;\n    s->stats.committed++;", "    s->serial++;"),
    ("commit does not mark the frame done", ST, "    s->cur_done = true;\n    s->state = KM_STREAM_LIVE;", "    s->state = KM_STREAM_LIVE;"),
    ("commit does not go LIVE", ST, "    s->cur_done = true;\n    s->state = KM_STREAM_LIVE;", "    s->cur_done = true;"),
    ("commit copies one byte too few", ST, "memcpy(s->ready, s->staging, sizeof s->ready);", "memcpy(s->ready, s->staging, sizeof s->ready - 1u);"),
    ("commit does not store the frame number", ST, "    s->ready_frame = s->cur_frame;\n", ""),
    ("active() is true as soon as OPEN", ST, "return s->state == KM_STREAM_LIVE;\n}\n\nconst uint8_t", "return s->state != KM_STREAM_IDLE;\n}\n\nconst uint8_t"),
    ("frame() returns a frame when not active", ST, "return s->state == KM_STREAM_LIVE ? s->ready : NULL;", "return s->ready;"),
    ("serial() returns the committed counter", ST, "uint32_t km_stream_serial(const km_stream_t *s)\n{\n    return s->serial;", "uint32_t km_stream_serial(const km_stream_t *s)\n{\n    return s->stats.committed;"),
    # ---- STOP / KEEPALIVE
    ("STOP: any session may stop", ST, "    if (req->len != COMMON_BYTES) {\n        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, true, true, out);\n    }\n    if (s->state == KM_STREAM_IDLE || req->data[1] != s->session) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);\n    }\n    close_session(s);",
     "    if (req->len != COMMON_BYTES) {\n        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, true, true, out);\n    }\n    if (s->state == KM_STREAM_IDLE) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);\n    }\n    close_session(s);"),
    ("STOP does not close", ST, "    close_session(s);\n    return send_reply(s, req, KM_STREAM_OK, false, out);\n}\n\nstatic bool op_keepalive", "    return send_reply(s, req, KM_STREAM_OK, false, out);\n}\n\nstatic bool op_keepalive"),
    ("keep-alive: any session keeps alive", ST, "    if (s->state == KM_STREAM_IDLE || req->data[1] != s->session) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);\n    }\n    const int gap", "    if (s->state == KM_STREAM_IDLE) {\n        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);\n    }\n    const int gap"),
    ("keep-alive does not keep alive", ST, "    seq_accept(s, (uint16_t)req->offset, gap);\n    return send_reply(s, req, KM_STREAM_OK, false, out);\n}\n\nbool km_stream_handle", "    return send_reply(s, req, KM_STREAM_OK, false, out);\n}\n\nbool km_stream_handle"),
    ("keep-alive: repeats refresh the time-out", ST, "        sat_inc(&s->stats.stale);\n        return send_reply(s, req, KM_STREAM_ERR_STALE, false, out);", "        sat_inc(&s->stats.stale);\n        s->last_rx_ms = s->now_ms;\n        return send_reply(s, req, KM_STREAM_ERR_STALE, false, out);"),
    # ---- dispatch and numbers
    ("dispatch without the NULL check", VD, "req.cmd == KM_CMD_STREAM && s_stream != NULL", "req.cmd == KM_CMD_STREAM"),
    ("dispatch ignores the stream command", VD, "req.cmd == KM_CMD_STREAM && s_stream != NULL", "false && s_stream != NULL"),
    ("command number 0xC7 -> 0xC8", HID_H, "#define KM_CMD_STREAM       0xC7u", "#define KM_CMD_STREAM       0xC8u"),
]

CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wshadow", "-Wconversion", "-Wno-sign-conversion", "-O1"]


def env():
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


class Build:
    """Object cache: only what includes a mutated file is rebuilt."""

    def __init__(self, tree: Path, work: Path):
        self.work = work
        self.fw = tree / "firmware"
        self.inc = ["-I" + str(self.fw / "hid/include"), "-I" + str(self.fw / "core/include"), "-I" + str(self.fw / "tests")]
        self.e = env()
        self.libs = [self.fw / "hid/src" / n for n in ("km_hid.c", "km_vendor.c", "km_scope.c", "km_config.c", "km_stream.c")]
        self.test = self.fw / "tests/test_stream.c"

    def cc(self, src: Path, obj: Path, extra=()) -> bool:
        r = subprocess.run(["gcc", *CFLAGS, *extra, *self.inc, "-c", str(src), "-o", str(obj)], env=self.e, capture_output=True, text=True)
        return r.returncode == 0

    def obj(self, src: Path, tag: str) -> Path:
        return self.work / (tag + "_" + src.stem + ".o")

    def build_all(self, tag: str) -> bool:
        ok = all(self.cc(s, self.obj(s, tag)) for s in self.libs)
        return ok and self.cc(self.test, self.obj(self.test, tag), ["-DKM_STREAM_STANDALONE"])

    def link_run(self, tag, timeout=120):
        """Objects of `tag` where present, base objects for the rest. True = the program passed, False = failed or crashed or hung,
        None = did not link."""
        objs = []
        for s in self.libs + [self.test]:
            o = self.obj(s, tag)
            objs.append(str(o if o.exists() else self.obj(s, "base")))
        exe = self.work / (tag + ".exe")
        if subprocess.run(["gcc", *objs, "-o", str(exe)], env=self.e, capture_output=True).returncode != 0:
            return None
        try:
            return subprocess.run([str(exe)], env=self.e, capture_output=True, timeout=timeout).returncode == 0
        except subprocess.TimeoutExpired:
            return False


def main() -> int:
    only = {int(a) for a in sys.argv[1:]}
    tmp = Path(tempfile.mkdtemp(prefix="km_mut_stream_"))
    try:
        tree = tmp / "tree"
        shutil.copytree(FW, tree / "firmware", ignore=shutil.ignore_patterns("__pycache__"))
        b = Build(tree, tmp)
        if not b.build_all("base") or b.link_run("base") is not True:
            print("baseline: FAIL (cannot judge mutations)")
            return 2
        print("baseline: pass")
        survived = 0
        for n, (name, rel, old, new) in enumerate(MUTATIONS, 1):
            if only and n not in only:
                continue
            path = tree / "firmware" / rel
            src = path.read_bytes().decode()
            crlf = "\r\n" in src
            text = src.replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d %-52s NOT APPLICABLE (%d matches)" % (n, name, text.count(old)))
                survived += 1
                continue
            path.write_bytes(text.replace(old, new).replace("\n", "\r\n" if crlf else "\n").encode())
            tag = "m%d" % n
            if rel.endswith(".h"):
                ok = b.build_all(tag)
            else:
                ok = b.cc(path, b.obj(path, tag))
            if not ok:
                print("%2d %-52s does not compile (the compiler rejects it)" % (n, name))
                path.write_bytes(src.encode())
                continue
            res = b.link_run(tag)
            path.write_bytes(src.encode())
            killed = res is not True
            print("%2d %-52s %s" % (n, name, "killed" if killed else "SURVIVED"))
            survived += 0 if killed else 1
        print("survivors:", survived)
        return 1 if survived else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
