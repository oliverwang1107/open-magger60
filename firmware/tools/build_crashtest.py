#!/usr/bin/env python3
"""Build a cross-compiled app target and check it against the memory map.

    python firmware/tools/build_crashtest.py [--target km_crashtest|km_d1|km_d2|km_d3|km_d4|km_k1|km_s1|km_s2|km_s3|km_store_probe] [--build-dir DIR]

km_crashtest = the M2-C rescue-drill app, km_d1 = the M2-D1 "alive" app, km_d2 = the M2-D2 USB enumeration app, km_d3 = D2 plus vendor cmd 3, km_d4 = D3 plus enter-bootloader, km_k1 = D4 with our own USB descriptors (M4 step K1), km_k2/km_k3 = scan + key engine + reports (K2 ESC only, K3 all keys), km_l30 = K3 plus the M5 T-E fail-safes (health monitor, progress-gated watchdog, report reconciliation), km_l30s / km_l30d = the same with a one-time scan stall at 20 s (recoverable / not recoverable, for the hardware test),
km_s1/km_s2/km_s3 = M3 sensing images (polled single readings / polled 76-slot scan / DMA full scan). km_store_probe = M5 T-D settings-store probe (BUILD-ONLY, never flashed). km_l31 / km_l33 = the full M5 image (trigger settings, layers, settings, config protocol; l31 keeps settings in RAM only, l33 also stores them in flash). km_l41 / km_l41n / km_l41d = l31 plus the lights (full hardware / PF5 untouched / dry run, LED_NOTES.md). km_l51 = l33 plus the lights at full hardware level (settings + calibration in flash, lights). km_m6a / km_m6 = the integrated M6 images (m6_main.c; km_m6a has no gamepad interface, km_m6 can present it), km_u5a / km_u5b / km_u5c = USB diagnostic images of the gamepad interface (usb_diag_main.c). km_noise = M6 P12a noise-measurement DIAGNOSTIC image (raw sample capture, no lights, never the daily image). Prints the path of the .bin;
turn it into a flashable image with tools/magger60_image.py.
"""
import argparse
import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("arm_smoke", HERE / "arm_smoke.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir")
    ap.add_argument("--target", default="km_crashtest", choices=["km_crashtest", "km_d1", "km_d2", "km_d3", "km_d4", "km_k1", "km_k2", "km_k3", "km_l30", "km_l30s", "km_l30d", "km_l31", "km_l33", "km_l41", "km_l41n", "km_l41d", "km_l51", "km_m6a", "km_m6", "km_m6p", "km_m6h", "km_m6q", "km_m6l", "km_m6hq", "km_m6hl", "km_m6f", "km_m6hf", "km_m6d", "km_u5a", "km_u5b", "km_u5c", "km_noise", "km_s1", "km_s2", "km_s3", "km_store_probe"])
    args = ap.parse_args()
    e = smoke.env()
    for t in ("arm-none-eabi-gcc", "cmake", "ninja"):
        if not shutil.which(t, path=e["PATH"]):
            print("missing tool:", t, file=sys.stderr)
            return 2
    build = Path(args.build_dir) if args.build_dir else Path(tempfile.mkdtemp(prefix="km_crash_"))
    e.pop("CC", None)
    e.pop("CXX", None)
    fw = smoke.FW
    smoke.run(["cmake", "-S", fw, "-B", build, "-G", "Ninja",
               "-DCMAKE_TOOLCHAIN_FILE=" + str(fw / "cmake/arm-none-eabi.cmake"), "-DCMAKE_BUILD_TYPE=MinSizeRel"], e)
    smoke.run(["cmake", "--build", build, "--target", args.target + ".elf"], e)
    usb_flag = []
    if args.target in ("km_m6h", "km_m6hq", "km_m6hl", "km_m6hf"):
        usb_flag = ["--usb-8khz"]
    elif args.target in ("km_l51", "km_m6a", "km_m6", "km_m6p", "km_m6q", "km_m6l", "km_m6f", "km_m6d"):
        usb_flag = ["--usb-1khz"]
    smoke.run([sys.executable, fw / "tools/check_image.py", build / (args.target + ".bin"), build / (args.target + ".elf")] + usb_flag, e)
    print(args.target, "bin:", build / (args.target + ".bin"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
