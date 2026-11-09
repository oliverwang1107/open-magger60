#!/usr/bin/env python3
"""Mutation pass for the light-configuration persistence and the idle-off (km_led.c idle part, km_ledstore.c, the extended light ops in
km_config.c and m5_led.c).

    python firmware/tools/mutate_ledstore.py            # run every mutation, list survivors
    python firmware/tools/mutate_ledstore.py --list     # just print them
    python firmware/tools/mutate_ledstore.py --only wake

Same idea as mutate_health.py / mutate_engine.py: each mutation changes one expression in a scratch copy of ONE source file, that file is
recompiled (all other objects come from one clean build) and the suites led, led_hw, ledstore and config are run. "killed" = the build works
and a check fails; a "survivor" means the tests do not notice the change.
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
SUITES = ["led", "led_hw", "ledstore", "config"]

L = "core/src/km_led.c"
S = "core/src/km_ledstore.c"
C = "hid/src/km_config.c"
M = "port/n32h48x/m5_led.c"

# (name, file, old text, new text); `old` must occur exactly once in the file.
MUTATIONS = [
    # ---- idle-off in the LED core ----
    ("idle gain not applied to the output", L, "l->out16 = (l->gain == KM_LED_IDLE_GAIN_FULL) ? l->scale16 :", "l->out16 = (1) ? l->scale16 :"),
    ("idle gain applied before the budget only", L, "(uint32_t)(((uint64_t)l->scale16 * ((uint32_t)l->gain + 1u)) >> 16);", "l->scale16;"),
    ("idle time ignored", L, "} else if (!l->idle && (uint32_t)(now_ms - l->last_act_ms) >= (uint32_t)l->idle_s * 1000u) {", "} else if (0) {"),
    ("idle time in the wrong unit", L, "(uint32_t)l->idle_s * 1000u) {", "(uint32_t)l->idle_s * 100u) {"),
    ("idle starts one tick early", L, "(uint32_t)(now_ms - l->last_act_ms) >= (uint32_t)l->idle_s * 1000u", "(uint32_t)(now_ms - l->last_act_ms) + 1000u >= (uint32_t)l->idle_s * 1000u"),
    ("0 does not mean never", L, "    } else if (l->idle_s == 0u) {\n        l->idle = false;", "    } else if (0) {\n        l->idle = false;"),
    ("clock compared without wrap safety", L, "(uint32_t)(now_ms - l->last_act_ms) >= (uint32_t)l->idle_s * 1000u", "now_ms >= l->last_act_ms + (uint32_t)l->idle_s * 1000u"),
    ("activity does not restart the clock", L, "        l->last_act_ms = now_ms;\n        l->act_armed = true;", "        l->act_armed = true;"),
    ("activity does not clear the idle state", L, "        l->activity = false;\n        l->idle = false;\n    } else if", "        l->activity = false;\n    } else if"),
    ("first frame does not start the clock", L, "if (l->activity || !l->act_armed) {", "if (l->activity) {"),
    ("fade out jumps", L, "const uint32_t step = (uint32_t)KM_LED_IDLE_GAIN_FULL * dt / KM_LED_IDLE_FADE_OUT_MS;", "const uint32_t step = KM_LED_IDLE_GAIN_FULL;"),
    ("fade in jumps never completes", L, "const uint32_t up = (uint32_t)l->gain + (uint32_t)KM_LED_IDLE_GAIN_FULL * dt / KM_LED_IDLE_FADE_IN_MS;", "const uint32_t up = (uint32_t)l->gain;"),
    ("a key does not wake the lights", L, "    if (n != 0u) {\n        km_led_wake(l);", "    if (0) {\n        km_led_wake(l);"),
    ("wake does not count as activity", L, "void km_led_wake(km_led_t *l)\n{\n    l->activity = true;", "void km_led_wake(km_led_t *l)\n{"),
    ("only a key with a light wakes", L, "    if (n != 0u) {\n        km_led_wake(l);                     /* any key counts, also one without a light */\n    }\n    for (unsigned i = 0; i < n; i++) {\n        const unsigned slot = down_slots[i];\n        if (slot < KM_LED_SLOTS) {\n            const uint8_t led = k_led_of_slot[slot];\n            if (led != KM_LED_NONE) {\n                l->lvl[led] = 255;",
     "    for (unsigned i = 0; i < n; i++) {\n        const unsigned slot = down_slots[i];\n        if (slot < KM_LED_SLOTS) {\n            const uint8_t led = k_led_of_slot[slot];\n            if (led != KM_LED_NONE) {\n                km_led_wake(l);\n                l->lvl[led] = 255;"),
    ("default idle time", L, "l->idle_s = KM_LED_IDLE_DEFAULT_S;", "l->idle_s = 0;"),
    ("changing the idle time keeps the latch", L, "        l->idle_s = seconds;\n        l->idle = false;", "        l->idle_s = seconds;"),
    ("idle state not reported while fading in", L, "return l->idle || l->gain != KM_LED_IDLE_GAIN_FULL;\n}\n\nvoid km_led_note_keys", "return l->idle;\n}\n\nvoid km_led_note_keys"),
    # ---- record format ----
    ("version not checked", S, "buf[0] != KM_LEDSTORE_VERSION || buf[7] != 0u", "buf[7] != 0u"),
    ("byte 7 not checked", S, "buf[0] != KM_LEDSTORE_VERSION || buf[7] != 0u", "buf[0] != KM_LEDSTORE_VERSION"),
    ("reserved bytes not checked", S, "    for (unsigned i = 10; i < KM_LEDSTORE_PAYLOAD_BYTES; i++) {", "    for (unsigned i = 10; i < 10; i++) {"),
    ("last reserved byte not checked", S, "    for (unsigned i = 10; i < KM_LEDSTORE_PAYLOAD_BYTES; i++) {", "    for (unsigned i = 10; i < KM_LEDSTORE_PAYLOAD_BYTES - 1u; i++) {"),
    ("length not checked", S, "len != KM_LEDSTORE_PAYLOAD_BYTES || buf[0]", "len < KM_LEDSTORE_PAYLOAD_BYTES - 1u || buf[0]"),
    ("decoded record not validated", S, "    if (!km_ledstore_rec_valid(&t)) {\n        return false;\n    }\n    *r = t;", "    *r = t;"),
    ("encode does not validate", S, "if (buf == NULL || cap < KM_LEDSTORE_PAYLOAD_BYTES || !km_ledstore_rec_valid(r)) {", "if (buf == NULL || cap < KM_LEDSTORE_PAYLOAD_BYTES || r == NULL) {"),
    ("idle time high byte lost", S, "buf[9] = (uint8_t)(r->idle_s >> 8);", "buf[9] = 0;"),
    ("idle time byte order swapped", S, "t.idle_s = (uint16_t)(buf[8] | ((unsigned)buf[9] << 8));", "t.idle_s = (uint16_t)(buf[9] | ((unsigned)buf[8] << 8));"),
    ("colour channels swapped", S, "buf[5] = r->cfg.color.g;", "buf[5] = r->cfg.color.b;"),
    ("load without checking the log's record size", S, "log->payload_len != KM_LEDSTORE_PAYLOAD_BYTES || !km_clog_read(log, buf, sizeof buf)", "!km_clog_read(log, buf, sizeof buf)"),
    ("boot does not note the load", S, "    m->loaded = km_ledstore_load(m->log, r);\n    return m->loaded;", "    (void)km_ledstore_load(m->log, r);\n    return false;"),
    # ---- when it is written ----
    ("no settle time (no coalescing)", S, "if ((uint32_t)(now_s - m->last_req_s) < KM_LEDSTORE_SETTLE_S) {", "if (0) {"),
    ("settle time counted from the first request only", S, "    if (m->req_fresh) {\n        m->req_fresh = false;\n        m->last_req_s = now_s;\n    }", "    if (m->req_fresh) {\n        m->req_fresh = false;\n    }"),
    ("request stamp not refreshed by a new request", S, "    m->req_fresh = true;", "    (void)0;"),
    ("no minimum gap", S, "if (m->started_once && (uint32_t)(now_s - m->last_start_s) < KM_LEDSTORE_MIN_GAP_S) {", "if (0) {"),
    ("minimum gap measured from the end of a save", S, "    m->started_once = true;\n    m->last_start_s = now_s;", "    m->started_once = true;"),
    ("starts without idle_long", S, "if (m->gave_up || m->req_seq == m->saved_seq || !idle_long) {", "if (m->gave_up || m->req_seq == m->saved_seq || !(idle_long | 1)) {"),
    ("steps while a key is down", S, "if (km_clog_job_must_finish(&m->job) || idle_now) {", "if (km_clog_job_must_finish(&m->job) || idle_now || 1) {"),
    ("never runs on after the erase", S, "if (km_clog_job_must_finish(&m->job) || idle_now) {", "if (idle_now) {"),
    ("saves without a request", S, "if (m->gave_up || m->req_seq == m->saved_seq || !idle_long) {", "if (m->gave_up || !idle_long) {"),
    ("saved request marked at request time", S, "    m->req_seq++;\n", "    m->req_seq++;\n    m->saved_seq = m->req_seq;\n"),
    ("a newer request during a job is lost", S, "        m->saved_seq = m->job_seq;\n        m->tries = 0;\n    } else {\n        m->saves_failed++;", "        m->saved_seq = m->req_seq;\n        m->tries = 0;\n    } else {\n        m->saves_failed++;"),
    ("identical record stays pending", S, "        m->saved_seq = m->job_seq;\n        m->tries = 0;\n        m->last = (uint8_t)KM_CLOG_UNCHANGED;", "        m->last = (uint8_t)KM_CLOG_UNCHANGED;"),
    ("failure counted as success", S, "        m->saves_failed++;                            /* the previous record is still the newest valid one */", "        m->saves_ok++;\n        m->saved_seq = m->job_seq;"),
    ("never gives up", S, "        if (++m->tries >= KM_LEDSTORE_MAX_TRIES) {\n            m->gave_up = true;\n        }\n    }\n}", "        m->tries++;\n    }\n}"),
    ("gives up at once", S, "        if (++m->tries >= KM_LEDSTORE_MAX_TRIES) {\n            m->gave_up = true;\n        }\n    }\n}", "        m->gave_up = true;\n    }\n}"),
    ("a new request does not end the giving up", S, "    m->tries = 0;\n    m->gave_up = false;\n    return true;", "    m->tries = 0;\n    return true;"),
    ("hook not called", S, "    if (m->after_flash != NULL) {\n        m->after_flash(m->ctx);\n    }", "    (void)0;"),
    ("must_finish always false", S, "return km_clog_job_active(&m->job) && km_clog_job_must_finish(&m->job);", "(void)m;\n    return false;"),
    # ---- protocol ----
    ("unknown flag bits accepted", C, "if ((k->a[ARG0 + 8u] & (uint8_t)~KM_CFG_LED_FLAG_SAVE) != 0u) {", "if (0) {"),
    ("GET argument not checked", C, "        if (k->a[ARG0] != 1u) {\n            return KM_CFG_ERR_BAD_ARG;\n        }\n        return led_reply_ex(c, k);", "        return led_reply_ex(c, k);"),
    ("extended form without the ops answers OK", C, "    if (ex && c->led != NULL && (c->led_ex == NULL || c->led_ex->get == NULL || c->led_ex->set == NULL)) {\n        return KM_CFG_ERR_BAD_LENGTH;", "    if (ex && c->led != NULL && (c->led_ex == NULL || c->led_ex->get == NULL || c->led_ex->set == NULL)) {\n        return KM_CFG_OK;"),
    # ---- glue ----
    ("save request accepted without persistence", M, "    if (save && g_lps.log == NULL) {\n        return KM_CFG_ERR_UNSUPPORTED;", "    if (0) {\n        return KM_CFG_ERR_UNSUPPORTED;"),
    ("save flag ignored", M, "    if (save && !km_ledstore_request(&g_lps, &r)) {", "    if (0) {"),
    ("every set asks for a save", M, "    if (save && !km_ledstore_request(&g_lps, &r)) {", "    if (!km_ledstore_request(&g_lps, &r) && save) {"),
    ("legacy set asks for a save", M, "    km_led_wake(&g_led);                  /* a change from the host is something to look at: faded-out lights come back */\n    return KM_CFG_OK;", "    km_led_wake(&g_led);\n    { km_ledstore_rec_t q; q.cfg = c; q.idle_s = g_led.idle_s; (void)km_ledstore_request(&g_lps, &q); }\n    return KM_CFG_OK;"),
    ("idle time not applied", M, "    km_led_set_idle(&g_led, r.idle_s);\n    km_led_wake(&g_led);", "    km_led_wake(&g_led);"),
    ("set does not wake the lights", M, "    km_led_set_idle(&g_led, r.idle_s);\n    km_led_wake(&g_led);", "    km_led_set_idle(&g_led, r.idle_s);"),
    ("stored idle time not applied at boot", M, "        km_led_set_idle(&g_led, r.idle_s);\n    }\n}", "    }\n}"),
    ("stored configuration not applied at boot", M, "        (void)km_led_set_cfg(&g_led, &r.cfg);                      /* valid by construction (km_ledstore_decode) */", "        (void)0;"),
    ("persistent bit missing", M, "(g_lps.log != NULL ? KM_CFG_LED_ST_PERSISTENT : 0u)", "0u"),
    ("loaded bit missing", M, "(g_lps.loaded ? KM_CFG_LED_ST_LOADED : 0u)", "0u"),
    ("pending bit missing", M, "(km_ledstore_pending(&g_lps) ? KM_CFG_LED_ST_SAVE_PENDING : 0u)", "0u"),
    ("failed bit missing", M, "(g_lps.log != NULL && g_lps.last >= (uint8_t)KM_CLOG_ERR_ARG ? KM_CFG_LED_ST_SAVE_FAILED : 0u)", "0u"),
    ("idle bit missing", M, "(km_led_idle(&g_led) ? KM_CFG_LED_ST_IDLE_OFF : 0u)", "0u"),
    ("idle seconds low byte wrong", M, "    v[6] = (uint8_t)(g_led.idle_s & 0xFFu);", "    v[6] = (uint8_t)(g_led.idle_s >> 8);"),
    ("save counter not reported", M, "    v[10] = sat8(g_lps.saves_ok);", "    v[10] = 0;"),
    ("persistence pass does nothing", M, "    return km_ledstore_pass(&g_lps, now_s, idle_now, idle_long);", "    (void)now_s; (void)idle_now; (void)idle_long;\n    return false;"),
    ("urgent never", M, "    return km_ledstore_must_finish(&g_lps);", "    return false;"),
]

EQUIVALENT = {
    "load without checking the log's record size": "defence in depth: km_clog_read already refuses a buffer smaller than the log's record",
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
    work = Path(tempfile.mkdtemp(prefix="km_mut_l_"))

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
