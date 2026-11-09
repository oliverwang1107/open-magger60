#!/usr/bin/env python3
"""Build the keyboard's portable core (settings, key map, layers, key engine) as a WebAssembly module for the browser.

    python tools/build_wasm.py            # compile -> ui/wasm/km_core.wasm and ui/wasm/km_core.meta.json
    python tools/build_wasm.py --check    # no compiler needed: exit 1 if the committed .wasm is older than the C sources
    python tools/build_wasm.py --print-cc # show which compiler would be used

Read-only with respect to the keyboard: it only compiles files. Sources: the core files listed in CORE below (the same files
the keyboard firmware and the host tests use) + firmware/wasm/km_wasm_api.c (flat exports) + firmware/wasm/wasm_libc.c (memcpy
& co., so no C library is linked).

Compiler: the first of
  1. $KM_WASM_CC (a command line, for example "clang" or "zig cc")
  2. clang with a wasm32 target and wasm-ld on PATH
  3. zig on PATH
  4. the ziglang Python package: build/wasm-toolchain (python -m venv build/wasm-toolchain && build/wasm-toolchain/Scripts/pip install ziglang)
     or the current Python if it has the package. Zig bundles clang + lld, about 100 MB, nothing is installed system-wide.
The module is built for the WebAssembly 1.0 core instruction set (-mcpu=mvp): no bulk-memory, no SIMD, runs everywhere.
The result is deterministic for a given compiler version; meta.json records the compiler and a hash of every input
(line endings normalised), which ui/tests/wasm-parity.test.mjs compares so a stale module is caught.
"""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FW = REPO / "firmware"
OUT_DIR = REPO / "ui" / "wasm"
WASM = OUT_DIR / "km_core.wasm"
META = OUT_DIR / "km_core.meta.json"

# the hardware-free core the browser gets (km_settings needs km_engine for the key-config rule, km_engine needs km_depth_model)
CORE = ["km_settings.c", "km_keymap.c", "km_layers.c", "km_engine.c", "km_depth_model.c"]
GLUE = ["km_wasm_api.c"]
LIBC = "wasm_libc.c"
INC = FW / "core" / "include"
SHIM = FW / "wasm" / "include"   # string.h for a build without a C library


def norm_hash(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def inputs():
    files = [FW / "core" / "src" / n for n in CORE] + [FW / "wasm" / n for n in GLUE + [LIBC]]
    files += sorted((INC / "km").glob("*.h")) + sorted(SHIM.glob("*.h"))
    return files


def input_hashes():
    return {p.relative_to(REPO).as_posix(): norm_hash(p) for p in inputs()}


def combined(hashes):
    return hashlib.sha256("".join(f"{k} {v}\n" for k, v in sorted(hashes.items())).encode()).hexdigest()


def find_cc():
    env = os.environ.get("KM_WASM_CC")
    if env:
        return shlex.split(env), "KM_WASM_CC"
    clang, lld = shutil.which("clang"), shutil.which("wasm-ld")
    if clang and lld:
        return [clang, "--target=wasm32"], "clang"
    zig = shutil.which("zig")
    if zig:
        return [zig, "cc", "-target", "wasm32-freestanding"], "zig"
    for py in (REPO / "build" / "wasm-toolchain" / "Scripts" / "python.exe", REPO / "build" / "wasm-toolchain" / "bin" / "python", Path(sys.executable)):
        if py.exists():
            r = subprocess.run([str(py), "-m", "ziglang", "version"], capture_output=True, text=True)
            if r.returncode == 0:
                return [str(py), "-m", "ziglang", "cc", "-target", "wasm32-freestanding"], f"ziglang {r.stdout.strip()}"
    return None, None


def compiler_version(cc):
    r = subprocess.run(cc + ["--version"], capture_output=True, text=True)
    first = (r.stdout or r.stderr).strip().splitlines()
    return first[0] if first else "unknown"


def build():
    cc, how = find_cc()
    if cc is None:
        sys.exit("no wasm-capable C compiler found. Set KM_WASM_CC, or: python -m venv build/wasm-toolchain && "
                 "build/wasm-toolchain/Scripts/pip install ziglang   (see the docstring of this file)")
    common = ["-std=c11", "-O2", "-ffreestanding", "-nostdlib", "-mcpu=mvp", "-Wall", "-Wextra", "-Werror", f"-I{INC}", f"-I{SHIM}"]
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for p in [FW / "core" / "src" / n for n in CORE] + [FW / "wasm" / n for n in GLUE]:
            o = Path(tmp) / (p.stem + ".o")
            subprocess.run(cc + common + ["-c", str(p), "-o", str(o)], check=True)
            objs.append(str(o))
        lo = Path(tmp) / "wasm_libc.o"
        subprocess.run(cc + common + ["-fno-builtin", "-c", str(FW / "wasm" / LIBC), "-o", str(lo)], check=True)
        objs.append(str(lo))
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        subprocess.run(cc + ["-O2", "-nostdlib", "-mcpu=mvp", "-Wl,--no-entry", "-Wl,--gc-sections", "-Wl,--strip-all",
                             "-Wl,-z,stack-size=16384", "-o", str(WASM)] + objs, check=True)
    hashes = input_hashes()
    meta = {
        "about": "Written by tools/build_wasm.py. Do not edit by hand.",
        "sources": hashes,
        "source_hash": combined(hashes),
        "compiler": compiler_version(cc) if how != "KM_WASM_CC" else how,
        "wasm_sha256": hashlib.sha256(WASM.read_bytes()).hexdigest(),
        "wasm_bytes": WASM.stat().st_size,
    }
    META.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"{WASM.relative_to(REPO)}: {meta['wasm_bytes']} bytes, compiler {meta['compiler']} ({how})")


def check():
    if not WASM.exists() or not META.exists():
        print("ui/wasm/km_core.wasm is missing: run python tools/build_wasm.py")
        return 1
    meta = json.loads(META.read_text(encoding="utf-8"))
    now = input_hashes()
    if meta.get("source_hash") != combined(now):
        changed = [k for k, v in now.items() if meta["sources"].get(k) != v]
        print("ui/wasm/km_core.wasm is STALE, rebuild with python tools/build_wasm.py. Changed inputs: " + ", ".join(changed))
        return 1
    if hashlib.sha256(WASM.read_bytes()).hexdigest() != meta["wasm_sha256"]:
        print("ui/wasm/km_core.wasm does not match km_core.meta.json (edited by hand?): rebuild")
        return 1
    print(f"ui/wasm/km_core.wasm is up to date ({meta['wasm_bytes']} bytes, {meta['compiler']})")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--print-cc", action="store_true")
    a = ap.parse_args()
    if a.print_cc:
        print(find_cc())
    elif a.check:
        sys.exit(check())
    else:
        build()
