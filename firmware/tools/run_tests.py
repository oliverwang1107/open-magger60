#!/usr/bin/env python3
"""Build and run the host unit tests with plain gcc (no make/cmake needed).

    python firmware/tools/run_tests.py            # strict warnings, run the suite
    python firmware/tools/run_tests.py --ubsan    # also build with -fsanitize=undefined if the compiler has it
    python firmware/tools/run_tests.py --cc clang
    python firmware/tools/run_tests.py --filter-modes   # everything again with KM_ENGINE_FILTER_MODES=1 (the adaptive filters compiled in)

On Windows the MSYS2 ucrt64 compiler is put first on PATH (the Git Bash /mingw64/bin
toolchain on PATH makes gcc die silently).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]

# The real flash_boot.c, compiled against the simulated registers of tests/sim/flash_sim_regs.h, and the real
# M3 sensing driver (sense_*.c, adc_scan.c) against the simulated ADC/DMA/timer of tests/test_sense.c, and store_flash.c
# (M5 settings store) against the simulated flash controller of tests/test_store_flash.c.
SIM_SOURCES = [FW / "port/n32h48x/flash_boot.c", FW / "port/n32h48x/sense_tables.c", FW / "port/n32h48x/sense_hw.c",
               FW / "port/n32h48x/adc_scan.c", FW / "port/n32h48x/store_flash.c",
               FW / "port/n32h48x/led_hw.c", FW / "port/n32h48x/m5_led.c"]
LIB_SOURCES = sorted((FW / "core/src").glob("*.c")) + sorted((FW / "core/gen").glob("*.c")) + sorted((FW / "hid/src").glob("*.c")) + sorted((FW / "usb/src").glob("*.c"))
TEST_SOURCES = sorted((FW / "tests").glob("*.c"))
INCLUDES = ["-I" + str(FW / "core/include"), "-I" + str(FW / "hid/include"), "-I" + str(FW / "usb/include"), "-I" + str(FW / "tests"), "-I" + str(FW / "tests/sim"),
            "-I" + str(FW / "port/n32h48x")]

STRICT = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wshadow", "-Wconversion",
          "-Wno-sign-conversion", "-O2"]


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--ubsan", action="store_true")
    ap.add_argument("--filter-modes", action="store_true", help="compile with -DKM_ENGINE_FILTER_MODES=1 (the adaptive engine filters)")
    ap.add_argument("--keep", action="store_true", help="keep the build directory")
    ap.add_argument("suites", nargs="*", help="run only these suites (names as printed by the test binary)")
    args = ap.parse_args()

    e = env()
    cc = shutil.which(args.cc, path=e["PATH"])
    if not cc:
        print("compiler not found:", args.cc, file=sys.stderr)
        return 2

    extra = []
    if args.filter_modes:
        extra.append("-DKM_ENGINE_FILTER_MODES=1")
    if args.ubsan:
        extra += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-g"]

    build = Path(tempfile.mkdtemp(prefix="km_tests_"))
    exe = build / ("km_tests.exe" if os.name == "nt" else "km_tests")
    try:
        # Library sources get the strict flags; test sources are a little more relaxed
        # (they use sprintf/long long helpers) but still -Wall -Wextra -Werror.
        objs = []
        for src in LIB_SOURCES:
            obj = build / (src.stem + ".o")
            cmd = [cc, *STRICT, *extra, *INCLUDES, "-c", str(src), "-o", str(obj)]
            r = subprocess.run(cmd, env=e)
            if r.returncode != 0:
                print("compile failed:", src, file=sys.stderr)
                return 1
            objs.append(str(obj))
        for src in SIM_SOURCES:
            obj = build / ("sim_" + src.stem + ".o")
            cmd = [cc, *STRICT, *extra, "-DKM_FLASH_HOST_TEST", "-DKM_SENSE_HOST_TEST", "-DKM_STORE_HOST_TEST", "-DKM_LED_HOST_TEST", *INCLUDES, "-c", str(src), "-o", str(obj)]
            r = subprocess.run(cmd, env=e)
            if r.returncode != 0:
                print("compile failed:", src, file=sys.stderr)
                return 1
            objs.append(str(obj))
        # led_hw.c again at build levels 1 (PF5 untouched) and 0 (dry run), with renamed functions so that one test binary can check
        # what each level touches (tests/test_led_hw.c)
        for level, prefix in ((1, "l1_"), (0, "l0_")):
            obj = build / ("sim_led_hw_level%d.o" % level)
            ren = ["-Dled_hw_%s=%s%s" % (n, prefix, n) for n in ("start", "kill", "send", "poll", "get_diag", "test_reset", "test_last_frame")]
            ren = [r.replace(prefix + "test_reset", prefix + "reset") for r in ren]
            cmd = [cc, *STRICT, *extra, "-DKM_LED_HOST_TEST", "-DKM_LED_HW_LEVEL=%d" % level, *ren, *INCLUDES, "-c", str(FW / "port/n32h48x/led_hw.c"), "-o", str(obj)]
            r = subprocess.run(cmd, env=e)
            if r.returncode != 0:
                print("compile failed: led_hw.c level", level, file=sys.stderr)
                return 1
            objs.append(str(obj))
        for src in TEST_SOURCES:
            obj = build / ("t_" + src.stem + ".o")
            cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-DKM_SENSE_HOST_TEST", *extra, *INCLUDES,
                   "-c", str(src), "-o", str(obj)]
            r = subprocess.run(cmd, env=e)
            if r.returncode != 0:
                print("compile failed:", src, file=sys.stderr)
                return 1
            objs.append(str(obj))
        r = subprocess.run([cc, *extra, *objs, "-o", str(exe)], env=e)
        if r.returncode != 0:
            print("link failed", file=sys.stderr)
            return 1
        return subprocess.run([str(exe), *args.suites], env=e).returncode
    finally:
        if not args.keep:
            shutil.rmtree(build, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
