#!/usr/bin/env python3
"""Cross-compile the smoke image for the N32H48x and check it against the memory map.

    python firmware/tools/arm_smoke.py [--build-dir DIR]

Needs arm-none-eabi-gcc, cmake and ninja (on Windows the MSYS2 ucrt64 packages
mingw-w64-ucrt-x86_64-arm-none-eabi-gcc, -ninja; on Debian/Ubuntu gcc-arm-none-eabi, ninja-build).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
M6_IMAGES = ["km_m6a", "km_m6", "km_u5a", "km_u5b", "km_u5c"]


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def run(cmd, e) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], env=e, check=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir")
    args = ap.parse_args()
    e = env()
    for t in ("arm-none-eabi-gcc", "cmake", "ninja"):
        if not shutil.which(t, path=e["PATH"]):
            print("missing tool:", t, file=sys.stderr)
            return 2
    build = Path(args.build_dir) if args.build_dir else Path(tempfile.mkdtemp(prefix="km_arm_"))
    # Use a CMake that does not pick up the host compiler through CC/CXX.
    e.pop("CC", None)
    e.pop("CXX", None)
    run(["cmake", "-S", FW, "-B", build, "-G", "Ninja",
         "-DCMAKE_TOOLCHAIN_FILE=" + str(FW / "cmake/arm-none-eabi.cmake"), "-DCMAKE_BUILD_TYPE=MinSizeRel"], e)
    run(["cmake", "--build", build], e)
    run([sys.executable, FW / "tools/check_image.py", build / "km_smoke.bin", build / "km_smoke.elf"], e)
    # M6 P10: the integrated images and the USB diagnostic images are checked against the memory map as well
    for name in M6_IMAGES:
        run([sys.executable, FW / "tools/check_image.py", build / (name + ".bin"), build / (name + ".elf")], e)
    print("build dir:", build)
    return 0


if __name__ == "__main__":
    sys.exit(main())
