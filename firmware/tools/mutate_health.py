#!/usr/bin/env python3
"""Mutation pass for the M5 T-E fail-safe logic (km_health.c, km_report_sync.c).

    python firmware/tools/mutate_health.py            # run every mutation, list survivors
    python firmware/tools/mutate_health.py --list     # just print them

Same idea as mutate_engine.py: each mutation changes one expression in a scratch copy of the sources, rebuilds only
tests/test_health.c + tests/test_report_sync.c (with a tiny main) and runs them. "killed" = the build works and a check
fails; a "survivor" means the tests do not notice the change.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SRC = ["core/src/km_health.c", "core/src/km_report_sync.c"]
H = "core/src/km_health.c"
S = "core/src/km_report_sync.c"

# (name, file, old text, new text); `old` must occur exactly once in the file.
MUTATIONS = [
    # ---- watchdog feed gating ----
    ("feed window one cycle too long", H, "return (uint32_t)(now - ts) <= KM_HEALTH_FEED_WINDOW_CYCLES;", "return (uint32_t)(now - ts) <= KM_HEALTH_FEED_WINDOW_CYCLES + 1u;"),
    ("feed window one cycle too short", H, "return (uint32_t)(now - ts) <= KM_HEALTH_FEED_WINDOW_CYCLES;", "return (uint32_t)(now - ts) < KM_HEALTH_FEED_WINDOW_CYCLES;"),
    ("scan progress not required", H, "if ((h->require & KM_PROG_SCAN) && !fresh(now_cycles, h->scan_ts)) {", "if (0) {"),
    ("usb progress not required", H, "if ((h->require & KM_PROG_USB) && !fresh(now_cycles, h->usb_ts)) {", "if (0) {"),
    ("scan progress not recorded", H, "        h->scan_ts = now_cycles;\n", "        (void)0;\n"),
    ("usb progress not recorded", H, "        h->usb_ts = now_cycles;\n", "        (void)0;\n"),
    ("progress flags never cleared", H, "    h->prog_pending = 0;\n    if ((h->require", "    if ((h->require"),
    ("engine progress required", H, "h->require = KM_PROG_SCAN | KM_PROG_USB;", "h->require = KM_PROG_SCAN | KM_PROG_USB | KM_PROG_ENGINE;"),
    ("window compared without wrap safety", H, "return (uint32_t)(now - ts) <= KM_HEALTH_FEED_WINDOW_CYCLES;", "return now <= ts + KM_HEALTH_FEED_WINDOW_CYCLES;"),
    # ---- fault flags ----
    ("keys allowed with a fault", H, "return h->fault_flags == 0;", "return (h->fault_flags & ~KM_FAULT_CAL_LOST) == 0;"),
    ("no initial CAL_LOST", H, "h->fault_flags = KM_FAULT_CAL_LOST;", "h->fault_flags = 0;"),
    ("calibration failure does not set CAL_LOST", H, "        h->calibration_refused++;\n        h->fault_flags |= KM_FAULT_CAL_LOST;", "        h->calibration_refused++;"),
    ("hold-off ignored", H, "if (!(h->fault_flags & h->transient) || !reached(now_cycles, h->hold_until)) {", "if (!(h->fault_flags & h->transient)) {"),
    ("hold-off one cycle early", H, "!reached(now_cycles, h->hold_until)", "!reached(now_cycles + 1u, h->hold_until)"),
    ("scan stall clears without a frame", H, "    if (!h->frame_seen_since_stall) {\n        clear = (uint8_t)(clear & ~KM_FAULT_SCAN_STALL);", "    if (0) {\n        clear = (uint8_t)(clear & ~KM_FAULT_SCAN_STALL);"),
    ("DMA error does not start a hold-off", H, "    h->dma_errors += new_errors;\n    h->fault_flags |= KM_FAULT_DMA_ERROR;\n    start_holdoff(h, now_cycles);", "    h->dma_errors += new_errors;\n    h->fault_flags |= KM_FAULT_DMA_ERROR;"),
    ("DMA error flag not raised", H, "    h->fault_flags |= KM_FAULT_DMA_ERROR;\n", "    (void)0;\n"),
    ("scan timeout does not raise the flag", H, "    h->fault_flags |= KM_FAULT_SCAN_STALL;\n", "    (void)0;\n"),
    ("engine limit off by one", H, "if (cycles > KM_HEALTH_ENGINE_LIMIT_CYCLES) {", "if (cycles >= KM_HEALTH_ENGINE_LIMIT_CYCLES) {"),
    ("engine budget off by one", H, "if (cycles > KM_HEALTH_ENGINE_BUDGET_CYCLES) {", "if (cycles >= KM_HEALTH_ENGINE_BUDGET_CYCLES) {"),
    ("engine budget overrun is a fault", H, "if (cycles > KM_HEALTH_ENGINE_BUDGET_CYCLES) {\n        h->engine_over_budget++;", "if (cycles > KM_HEALTH_ENGINE_BUDGET_CYCLES) {\n        h->engine_over_budget++;\n        h->fault_flags |= KM_FAULT_ENGINE_OVERRUN;"),
    ("engine max not tracked", H, "    if (cycles > h->engine_max_cycles) {\n        h->engine_max_cycles = cycles;\n    }", "    (void)0;"),
    ("suspend does not block keys", H, "        h->usb_suspends++;\n        h->fault_flags |= KM_FAULT_USB_SUSPENDED;", "        h->usb_suspends++;"),
    ("resume does not unblock keys", H, "        h->usb_resumes++;\n        h->fault_flags = (uint8_t)(h->fault_flags & ~KM_FAULT_USB_SUSPENDED);", "        h->usb_resumes++;"),
    ("bus reset keeps the suspend flag", H, "        h->usb_resets++;\n        h->usb_susp = false;", "        h->usb_resets++;"),
    ("suspend level ignores its own state", H, "    if (suspended != h->usb_susp) {", "    if (suspended) {"),
    ("reset cause keeps the clear-request bit", H, "return (uint8_t)((reg >> 24) & ~KM_RESET_RMV);", "return (uint8_t)((reg >> 24));"),
    ("keymask ignores slots >= 76", H, "        if (s < SLOTS) {", "        if (s < 80u) {"),
    ("keymask word index wrong", H, "out[s >> 4] = (uint16_t)(out[s >> 4] | (1u << (s & 15u)));", "out[s >> 4] = (uint16_t)(out[s >> 4] | (1u << (s & 7u)));"),
    # ---- report sync ----
    ("heartbeat one cycle early", S, "return (uint32_t)(now - then) >= span;", "return (uint32_t)(now - then) + 1u >= span;"),
    ("heartbeat one cycle late", S, "return (uint32_t)(now - then) >= span;", "return (uint32_t)(now - then) > span;"),
    ("no heartbeat while keys are down", S, "const bool heartbeat = (any_down || s->echo_pending) &&", "const bool heartbeat = (s->echo_pending) &&"),
    ("heartbeat while released", S, "const bool heartbeat = (any_down || s->echo_pending) &&", "const bool heartbeat = (1) &&"),
    ("no release echo", S, "s->echo_pending = !any_down && (differs ? s->had_down : s->echo_pending && !heartbeat);", "s->echo_pending = false;"),
    ("echo repeats for ever", S, "s->echo_pending = !any_down && (differs ? s->had_down : s->echo_pending && !heartbeat);", "s->echo_pending = !any_down && (differs ? s->had_down : s->echo_pending);"),
    ("desired state used while keys are not allowed", S, "const uint8_t *target = keys_allowed ? desired : s->released;", "const uint8_t *target = desired;"),
    ("failed send counted as success", S, "        s->retries++;\n", "        memcpy(s->last, target, s->len);\n        s->retries++;\n"),
    ("failed send not counted", S, "        s->retries++;\n", "        (void)0;\n"),
    ("forced flag survives a good send", S, "        s->force = false;\n        s->last_tx", "        s->last_tx"),
    ("forced flag ignored", S, "if (!differs && !s->force && !heartbeat) {", "if (!differs && !heartbeat) {"),
    ("no resync after the link came back", S, "    if (!s->link_was_up) {\n        s->link_was_up = true;\n        s->force = true;", "    if (!s->link_was_up) {\n        s->link_was_up = true;"),
    ("link loss does not force a resync", S, "            s->link_was_up = false;\n            s->force = true;", "            s->link_was_up = false;"),
    ("sends while the link is down", S, "    if (!io->link_up(io->ctx)) {", "    if (0) {"),
    ("busy limit one cycle early", S, "} else if (elapsed(now_cycles, s->busy_since, KM_SYNC_BUSY_LIMIT_CYCLES)) {", "} else if (elapsed(now_cycles + 1u, s->busy_since, KM_SYNC_BUSY_LIMIT_CYCLES)) {"),
    ("recovery never repeated", S, "            io->recover(io->ctx);\n            s->recoveries++;\n            s->busy_since = now_cycles;", "            io->recover(io->ctx);\n            s->recoveries++;\n            s->busy_since = now_cycles + 0x7FFFFFFFu;"),
    ("busy clock not restarted after a good send", S, "    s->busy_timing = false;\n    if (io->send", "    if (io->send"),
    ("busy endpoint is sent to anyway", S, "    if (!io->tx_ready(io->ctx)) {", "    if (0) {"),
    ("dead limit off by one", S, "return s->undelivered && elapsed(now_cycles, s->undelivered_since, KM_SYNC_DEAD_CYCLES);", "return s->undelivered && elapsed(now_cycles + 1u, s->undelivered_since, KM_SYNC_DEAD_CYCLES);"),
    ("dead clock never starts", S, "    if (!s->undelivered) {\n        s->undelivered_since = now_cycles;\n    }", "    (void)0;"),
    ("dead flag kept while the link is down", S, "        s->undelivered = false;      /* nobody", "        /* nobody"),
    ("forced release not counted", S, "        if (!keys_allowed && differs) {\n            s->forced_releases++;\n        }", "        (void)0;"),
]

EQUIVALENT = {
    "no resync after the link came back": "redundant by design: link loss already forced the resync (the next mutation removes that one)",
    "link loss does not force a resync": "redundant by design: link return forces it as well; removing BOTH is killed by the link-change tests",
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
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_report_sync();
    test_health();
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
    work = Path(tempfile.mkdtemp(prefix="km_mut_h_"))
    (work / "main.c").write_text(STUB)
    survivors, broken = [], []
    try:
        for i, (name, rel, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests", ignore=shutil.ignore_patterns("golden"))
            target = tree / rel
            text = target.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-52s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"), "-I" + str(tree / "tests/sim"),
                   str(work / "main.c"), str(tree / "tests/test_health.c"), str(tree / "tests/test_report_sync.c"),
                   *[str(tree / s) for s in SRC], "-o", str(exe)]
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
