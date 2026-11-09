#!/usr/bin/env python3
"""Mutation pass for the USB code (M4 task T-C): change one length, address, report size or comparison at a time in a
scratch copy of firmware/, run the host tests, and require the tests to FAIL for every mutation.

    python firmware/tools/mutate_usb.py            # all mutations
    python firmware/tools/mutate_usb.py 3 7        # only mutations 3 and 7
    python firmware/tools/mutate_usb.py --full     # judge with the complete host test suite instead of the USB suite alone

A mutation that the tests do not notice ("SURVIVED") means a missing test: add one. A mutation that does not even compile
("BUILD ERROR") is a broken mutation, not a kill: fix the entry. The real tree is never modified.

By default only the USB library (usb/src) and tests/test_usb.c are built, with the same warning flags as run_tests.py (a few
seconds per mutation instead of a minute); every mutation here touches only those files. --full is the slow, complete check.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

DESC = "usb/src/km_usb_desc.c"
CTRL = "usb/src/km_usb_ctrl.c"
INP = "usb/src/km_usb_in.c"
REP = "usb/include/km/km_usb_reports.h"
IDS = "usb/include/km/km_usb_ids.h"

MUTATIONS = [
    ("NKRO endpoint packet size 64 -> 32", DESC, "EP(KM_USB_EP_NKRO_IN, 64,", "EP(KM_USB_EP_NKRO_IN, 32,"),
    ("boot interface protocol keyboard -> none", DESC, "IFACE(KM_USB_IF_KBD, 1, 0x01, 0x01)", "IFACE(KM_USB_IF_KBD, 1, 0x01, 0x00)"),
    ("vendor report count 64 -> 63", DESC, "H_REPORT_COUNT(KM_USB_VND_REPORT_LEN)", "H_REPORT_COUNT(KM_USB_VND_REPORT_LEN - 1u)"),
    ("NKRO bitmap 228 -> 227 bits", DESC, "KM_USB_NKRO_LAST_USAGE - 3u", "KM_USB_NKRO_LAST_USAGE - 4u"),
    ("bcdHID 1.11 -> 1.10", DESC, "HID_LEN, 0x21, 0x11, 0x01", "HID_LEN, 0x21, 0x10, 0x01"),
    ("bConfigurationValue 1 -> 2", DESC, "(nif), 1, 0,", "(nif), 2, 0,"),
    ("NKRO bitmap bytes 29 -> 28", REP, "KM_USB_NKRO_BITMAP_BYTES   29u", "KM_USB_NKRO_BITMAP_BYTES   28u"),
    ("serial string length byte", DESC, "buf[0] = (uint8_t)(2u + 2u * chars);", "buf[0] = (uint8_t)(2u * chars);"),
    ("NKRO send without report ID check", INP, "len == KM_USB_NKRO_REPORT_LEN && report[0] == KM_USB_NKRO_ID_KEYS", "len == KM_USB_NKRO_REPORT_LEN"),
    ("keyboard endpoint ready while busy", INP, "return dev->configuration != 0 && !dev->busy_kbd;", "return dev->configuration != 0;"),
    ("completion of EP1 ignored", INP, "if (ep == EP_KBD) {", "if (ep == EP_NKRO) {"),
    ("idle unit 4 ms -> 2 ms", INP, "UFRAMES_PER_IDLE_UNIT 32u", "UFRAMES_PER_IDLE_UNIT 16u"),
    ("SET_PROTOCOL value 2 accepted", CTRL, "iface == KM_USB_IF_KBD && s->w_length == 0 && s->w_value <= 1u", "iface == KM_USB_IF_KBD && s->w_length == 0 && s->w_value <= 2u"),
    ("SET_REPORT one byte too long accepted", CTRL, "s->w_length <= max_len)", "s->w_length <= max_len + 1u)"),
    ("GET_REPORT of output reports allowed", CTRL, "if (type != REPORT_INPUT) {", "if (type != REPORT_OUTPUT) {"),
    ("EP3 interval 1 ms -> 0", IDS, "KM_USB_BINTERVAL_NKRO   4u", "KM_USB_BINTERVAL_NKRO   0u"),
    ("vendor endpoint address OUT 0x0A -> 0x02", "usb/include/km/km_usb.h", "KM_USB_EP_VND_OUT    0x0Au", "KM_USB_EP_VND_OUT    0x02u"),
    ("SET_IDLE does not restart the timer", CTRL, "dev->idle_age[iface] = 0;", ""),
    # P5a: host keyboard LEDs (SET_REPORT data stage)
    ("LED mask keeps Compose (0x07 -> 0x0F)", REP, "KM_USB_LED_MASK            0x07u", "KM_USB_LED_MASK            0x0Fu"),
    ("LED bits stored without the mask", CTRL, "dev->host_leds = (uint8_t)(leds & KM_USB_LED_MASK);", "dev->host_leds = leds;"),
    ("one byte too many in the data stage fits", CTRL, "if (data == NULL || len > room) {", "if (data == NULL || len > room + 1u) {"),
    ("bus reset keeps the host LEDs", CTRL, "    dev->host_leds = 0;          /* a bus reset forgets", "    /* a bus reset forgets"),
    ("new SETUP does not abandon a data stage", CTRL, "    dev->out_active = false;   /* a new SETUP packet abandons", "    /* a new SETUP packet abandons"),
    ("IF2 report ID in the data not checked", CTRL, "dev->out_len == KM_USB_NKRO_LED_LEN && dev->out_buf[0] == KM_USB_NKRO_ID_KEYS", "dev->out_len == KM_USB_NKRO_LED_LEN"),
    ("IF2 LED bits read from the ID byte", CTRL, "leds = dev->out_buf[1];", "leds = dev->out_buf[0];"),
    ("vendor output report taken as LEDs", CTRL, "iface == KM_USB_IF_NKRO && dev->out_len == KM_USB_NKRO_LED_LEN", "iface != KM_USB_IF_KBD && dev->out_len == KM_USB_NKRO_LED_LEN"),
    ("data stage can be ended twice", CTRL, "    dev->out_active = false;\n    dev->out_bad = false;\n    dev->out_len = 0;\n    return applied;", "    dev->out_bad = false;\n    dev->out_len = 0;\n    return applied;"),
    ("SET_REPORT does not remember its SETUP", CTRL, "            dev->out_setup = *s;\n", ""),
    ("LED report counter not counting", CTRL, "    dev->led_reports++;\n", ""),
    # P5b: the PAD descriptor variant, the gamepad endpoint, the fall-back, the FIFO plan and the capability check
    ("PAD endpoint interval 1 ms -> 0", IDS, "KM_USB_BINTERVAL_PAD    4u", "KM_USB_BINTERVAL_PAD    0u"),
    ("PAD bcdDevice equals the plain one", IDS, "KM_USB_BCD_DEVICE_PAD   0x0002u", "KM_USB_BCD_DEVICE_PAD   0x0001u"),
    ("PAD endpoint address 0x84 -> 0x85", "usb/include/km/km_usb.h", "KM_USB_EP_PAD_IN     0x84u", "KM_USB_EP_PAD_IN     0x85u"),
    ("fall-back time 6 s -> 5 s", "usb/include/km/km_usb.h", "KM_USB_PAD_FALLBACK_MS 6000u", "KM_USB_PAD_FALLBACK_MS 5000u"),
    ("PAD endpoint descriptor packet size 16 -> 8", DESC, "EP(KM_USB_EP_PAD_IN, KM_USB_PAD_EP_MPS,", "EP(KM_USB_EP_PAD_IN, 8,"),
    ("PAD button count in the descriptor 16 -> 15", DESC, "H_REPORT_COUNT(KM_USB_PAD_BUTTONS)", "H_REPORT_COUNT(15)"),
    ("PAD hat range 1..8 -> 1..7", REP, "KM_USB_PAD_HAT_MAX         8u", "KM_USB_PAD_HAT_MAX         7u"),
    ("PAD sweep period of LX", REP, "KM_USB_PAD_SWEEP_LX_MS     4000u", "KM_USB_PAD_SWEEP_LX_MS     4100u"),
    ("PAD hat physical maximum 315 -> 360", DESC, "H_PHYSICAL_MAX16(315)", "H_PHYSICAL_MAX16(360)"),
    ("PAD hat unit degrees -> none", DESC, "H_UNIT(UNIT_DEGREES)", "H_UNIT(0)"),
    ("PAD hat physical range leaks onto the axes", DESC, "        H_PHYSICAL_MIN(0), H_PHYSICAL_MAX(0), H_UNIT(0),", ""),
    ("PAD hat without null state", DESC, "H_INPUT(F_DATA_VAR_ABS_NULL)", "H_INPUT(F_DATA_VAR_ABS)"),
    ("PAD sticks start at -32768", DESC, "H_LOGICAL_MIN16(0x8001)", "H_LOGICAL_MIN16(0x8000)"),
    ("PAD collection usage Game Pad -> Joystick", DESC, "H_USAGE(0x05), H_COLLECTION(COLL_APPLICATION),", "H_USAGE(0x04), H_COLLECTION(COLL_APPLICATION),"),
    ("PAD interface not in the PAD configuration", DESC, "CFG_HEADER(CFG_TOTAL_PAD, KM_USB_NUM_IF_PAD)", "CFG_HEADER(CFG_TOTAL_PAD, KM_USB_NUM_IF)"),
    ("PAD HID descriptor offset of IF3", DESC, "#define HID_OFFSET_3   (CFG_LEN + IF0_LEN + IF1_LEN + IF2_LEN + IF_LEN)", "#define HID_OFFSET_3   (CFG_LEN + IF0_LEN + IF1_LEN + IF2_LEN)"),
    ("PAD device descriptor is the plain one", DESC, "return pad_variant(v) ? k_device_pad : k_device;", "return k_device;"),
    ("PAD configuration is the plain one", DESC, "return pad_variant(v) ? k_config_pad : k_config;", "return k_config;"),
    ("PLAIN has a report descriptor for IF3", DESC, "    if (iface >= km_usb_variant_num_if(v)) {\n        return NULL;\n    }\n    switch (iface) {", "    if (iface >= 4u + 0u * (unsigned)v) {\n        return NULL;\n    }\n    switch (iface) {"),
    ("pad ready in the plain variant", INP, "return dev->variant == KM_USB_VARIANT_PAD && dev->configuration != 0 && !dev->busy_pad;", "return dev->configuration != 0 && !dev->busy_pad;"),
    ("pad ready while busy", INP, "dev->configuration != 0 && !dev->busy_pad;", "dev->configuration != 0;"),
    ("pad report of any length", INP, "len != KM_USB_PAD_REPORT_LEN || ", "len == 0 || "),
    ("EP4 completion ignored", INP, "} else if (ep == EP_PAD) {", "} else if (ep == 9u) {"),
    ("reset keeps the last pad report", INP, "    memset(dev->last_pad, 0, sizeof dev->last_pad);\n", ""),
    ("reset keeps the pad busy flag", INP, "    dev->busy_nkro = false;\n    dev->busy_pad = false;\n    dev->sent_kbd", "    dev->busy_nkro = false;\n    dev->sent_kbd"),
    ("pad send does not remember the report", INP, "    memcpy(dev->last_pad, report, KM_USB_PAD_REPORT_LEN);\n", ""),
    ("pad stick range off by one", INP, "s->lx >= lo", "s->lx >= lo - 1"),
    ("pad trigger range off by one", INP, "s->lt <= KM_USB_PAD_STICK_MAX", "s->lt <= KM_USB_PAD_STICK_MAX + 1"),
    ("pad pack: LX written from LY", INP, "put16(out + 3, (uint16_t)s->lx);", "put16(out + 3, (uint16_t)s->ly);"),
    ("pad pack: hat in the wrong byte", INP, "out[2] = s->hat;", "out[1] = s->hat;"),
    ("pad unpack accepts hat 9", INP, "s->hat <= KM_USB_PAD_HAT_MAX", "s->hat <= KM_USB_PAD_HAT_MAX + 1u"),
    ("pad sweep: buttons stay on 17 steps -> 16", INP, "(t_ms / 1000u) % 17u", "(t_ms / 1000u) % 16u"),
    ("pad sweep: trigger not halved", INP, "s->lt = (uint16_t)((triangle(t_ms, KM_USB_PAD_SWEEP_LT_MS) + KM_USB_PAD_STICK_MAX) / 2);", "s->lt = (uint16_t)(triangle(t_ms, KM_USB_PAD_SWEEP_LT_MS) + KM_USB_PAD_STICK_MAX);"),
    ("pad sweep: triangle starts at the bottom", INP, "return (int16_t)(max - (int32_t)(ph * 2u * (uint32_t)max / half));", "return (int16_t)(-max + (int32_t)(ph * 2u * (uint32_t)max / half));"),
    ("IF3 endpoint known in the plain variant", CTRL, "(addr == KM_USB_EP_PAD_IN && dev->variant == KM_USB_VARIANT_PAD)", "(addr == KM_USB_EP_PAD_IN && dev != NULL)"),
    ("interface count not per variant (class requests)", CTRL, "iface < km_usb_variant_num_if(dev->variant)) {   /* HID class", "iface < KM_USB_NUM_IF_PAD) {   /* HID class"),
    ("interface count not per variant (standard requests)", CTRL, "if (iface >= km_usb_variant_num_if(dev->variant)) {", "if (iface >= KM_USB_NUM_IF_PAD) {"),
    ("IF3 takes SET_REPORT", CTRL, ": iface == KM_USB_IF_NKRO ? KM_USB_NKRO_LED_LEN : 0u;", ": KM_USB_NKRO_LED_LEN;"),
    ("IF3 GET_REPORT missing", CTRL, "} else if (iface == KM_USB_IF_PAD && id == 0) {", "} else if (iface == KM_USB_IF_PAD && id == 7) {"),
    ("IF3 answers SET_IDLE", CTRL, "const bool keyboard = iface == KM_USB_IF_KBD || iface == KM_USB_IF_NKRO;", "const bool keyboard = iface != KM_USB_IF_VND;"),
    ("descriptor requests ignore the variant", CTRL, "p = km_usb_config_desc_for(dev->variant, &n);", "p = km_usb_config_desc(&n);"),
    ("device descriptor request ignores the variant", CTRL, "p = km_usb_device_desc_for(dev->variant, &n);", "p = km_usb_device_desc(&n);"),
    ("report descriptor request ignores the variant", CTRL, "p = km_usb_report_desc_for(dev->variant, iface, &n);", "p = km_usb_report_desc(iface, &n);"),
    ("drill refusal applies to the plain set too", CTRL, "dev->test_refuse_configuration && dev->variant == KM_USB_VARIANT_PAD", "dev->test_refuse_configuration"),
    ("drill refusal also blocks unconfiguring", CTRL, "s->w_value <= 1u && s->w_index == 0 &&\n                           s->w_length == 0 &&\n                           !(s->w_value == 1u && dev->test_refuse_configuration", "s->w_value <= 1u && s->w_index == 0 &&\n                           s->w_length == 0 &&\n                           !(dev->test_refuse_configuration"),
    ("guard restarts its clock at every reset", CTRL, "g->requested == KM_USB_VARIANT_PAD && !g->running && !g->accepted", "g->requested == KM_USB_VARIANT_PAD && !g->accepted"),
    ("guard also runs for the plain variant", CTRL, "if (g->requested == KM_USB_VARIANT_PAD && !g->running", "if (!g->running"),
    ("guard keeps running after the host configured", CTRL, "        g->accepted = true;\n        g->running = false;", "        g->accepted = true;"),
    ("guard fires again after the fall-back", CTRL, "    g->running = false;\n    g->fell_back = true;", "    g->fell_back = true;"),
    ("guard stays on the PAD set after the fall-back", CTRL, "    g->active = KM_USB_VARIANT_PLAIN;\n    return true;", "    return true;"),
    ("guard time compare ignores the clock wrap", CTRL, "(uint32_t)(now_ms - g->started_ms) < KM_USB_PAD_FALLBACK_MS", "now_ms < g->started_ms + KM_USB_PAD_FALLBACK_MS"),
    ("guard accepts a configuration after the fall-back", CTRL, "    if (!g->fell_back) {\n        g->accepted = true;", "    if (true) {\n        g->accepted = true;"),
    ("FIFO plan: EP4 missing in the PAD variant", CTRL, "const unsigned n = v == KM_USB_VARIANT_PAD ? 6u : 5u;", "const unsigned n = v == KM_USB_VARIANT_PAD ? 5u : 5u;"),
    ("FIFO plan: EP4 FIFO 8 words", CTRL, "{ 4, 0x10 },", "{ 4, 0x08 },"),
    ("FIFO plan: EP1 FIFO moved", CTRL, "{ 1, 0x10 }, { 2, 0x20 }", "{ 1, 0x18 }, { 2, 0x20 }"),
    ("hwcfg: IN endpoint count without +1", CTRL, "(((raw[3] >> 26) & 0xFu) + 1u)", "(((raw[3] >> 26) & 0xFu))"),
    ("hwcfg: device endpoint field position", CTRL, "(raw[1] >> 10) & 0xFu", "(raw[1] >> 9) & 0xFu"),
    ("hwcfg: FIFO depth field position", CTRL, "(uint16_t)(raw[2] >> 16)", "(uint16_t)(raw[2] >> 15)"),
    ("hwcfg: dedicated FIFO bit", CTRL, "((raw[3] >> 25) & 1u)", "((raw[3] >> 24) & 1u)"),
    ("hwcfg: direction bits per endpoint", CTRL, "(raw[0] >> (2u * n)) & 3u", "(raw[0] >> (2u * n)) & 1u"),
    ("hwcfg check: FIFO space ignored", CTRL, "if (hw->fifo_depth < plan[n - 1].start + plan[n - 1].depth) {", "if (hw->fifo_depth < plan[n - 1].start + plan[n - 1].depth - 1000) {"),
    ("hwcfg check: IN endpoint count ignored", CTRL, "if (in_eps[i] >= hw->num_in_eps) {", "if (in_eps[i] >= 99u) {"),
    ("hwcfg check: IN direction ignored", CTRL, "if (hw->ep_dir[in_eps[i]] != 0u && hw->ep_dir[in_eps[i]] != 1u) {", "if (hw->ep_dir[in_eps[i]] == 9u) {"),
    ("hwcfg check: PAD variant checks no EP4", CTRL, "const unsigned n_in = v == KM_USB_VARIANT_PAD ? 4u : 3u;", "const unsigned n_in = 3u;"),
]


STRICT = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wshadow", "-Wconversion", "-Wno-sign-conversion", "-O2"]
MINI_MAIN = """#include "km_test.h"
int km_test_checks, km_test_failures;
void test_usb(void);
int main(void) { test_usb(); printf("%d checks, %d failures\\n", km_test_checks, km_test_failures); return km_test_failures != 0; }
"""


def toolchain_env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def run_suite(tree: Path, full: bool = False) -> str:
    """'pass' (every check passed), 'fail' (the tests noticed) or 'error' (the tree does not build)."""
    if full:
        r = subprocess.run([sys.executable, str(tree / "firmware/tools/run_tests.py")], capture_output=True, text=True)
        return "pass" if r.returncode == 0 else "fail"
    fw = tree / "firmware"
    e = toolchain_env()
    cc = shutil.which("gcc", path=e["PATH"])
    inc = ["-I" + str(fw / "usb/include"), "-I" + str(fw / "tests")]
    work = tree / "mini"
    work.mkdir(exist_ok=True)
    (work / "mini_main.c").write_text(MINI_MAIN)
    objs = []
    for src in sorted((fw / "usb/src").glob("*.c")):
        obj = work / (src.stem + ".o")
        if subprocess.run([cc, *STRICT, *inc, "-c", str(src), "-o", str(obj)], env=e, capture_output=True).returncode != 0:
            return "error"
        objs.append(str(obj))
    for name, src, flags in (("t_usb", fw / "tests/test_usb.c", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]),
                             ("t_main", work / "mini_main.c", ["-std=c11", "-O2"])):
        obj = work / (name + ".o")
        if subprocess.run([cc, *flags, *inc, "-c", str(src), "-o", str(obj)], env=e, capture_output=True).returncode != 0:
            return "error"
        objs.append(str(obj))
    exe = work / ("mini.exe" if os.name == "nt" else "mini")
    if subprocess.run([cc, *objs, "-o", str(exe)], env=e, capture_output=True).returncode != 0:
        return "error"
    try:   # a mutation can also send a test into an endless loop: that is noticed too
        r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "fail"
    return "pass" if r.returncode == 0 else "fail"


def main() -> int:
    full = "--full" in sys.argv[1:]
    only = {int(a) for a in sys.argv[1:] if a != "--full"}
    tmp = Path(tempfile.mkdtemp(prefix="km_mut_"))
    try:
        tree = tmp / "tree"
        shutil.copytree(FW, tree / "firmware", ignore=shutil.ignore_patterns("__pycache__"))
        base = run_suite(tree, full)
        print("baseline:", "pass" if base == "pass" else "FAIL (cannot judge mutations)", flush=True)
        if base != "pass":
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
                print("%2d %-45s NOT APPLICABLE (%d matches)" % (n, name, text.count(old)), flush=True)
                survived += 1
                continue
            path.write_bytes(text.replace(old, new).replace("\n", "\r\n" if crlf else "\n").encode())
            verdict = run_suite(tree, full)
            path.write_bytes(src.encode())
            print("%2d %-45s %s" % (n, name, {"fail": "killed", "pass": "SURVIVED", "error": "BUILD ERROR"}[verdict]), flush=True)
            survived += 0 if verdict == "fail" else 1
        print("survivors:", survived)
        return 1 if survived else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
