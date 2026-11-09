#!/usr/bin/env python3
"""run_tool_selftests.py - run the --self-test of every host tool in tools/ (simulated keyboards, no hardware).

    python tools/run_tool_selftests.py

Needs nothing outside the repository. Exit code 0 only when every self-test passes. Tools that need a keyboard to do
anything useful are not started without their --self-test flag; hid_survey.py has no self-test and is not run.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# (script, arguments). `python -I` keeps the current directory out of the import path.
SELF_TESTS = [
    ("magger60_flash.py", ["--self-test"]),
    ("magger60_image.py", ["--self-test"]),
    ("magger60_backup.py", ["--self-test"]),
    ("magger60_compare_backups.py", ["--self-test"]),
    ("magger60_enterboot.py", ["--self-test"]),
    ("magger60_info.py", ["--self-test"]),
    ("magger60_scope.py", ["--self-test"]),
    ("magger60_noise.py", ["--self-test"]),
    ("magger60_config.py", ["--self-test"]),
    ("magger60_stream.py", ["--self-test"]),
    ("magger60_pad_map.py", ["selftest"]),
    ("magger60_padtest.py", ["--self-test"]),
    ("badapple_convert.py", ["--self-test"]),
    ("magger60_keymap_measure.py", ["--self-test"]),
    ("magger60_slot_ranges.py", ["--self-test"]),
    ("magger60_soak.py", ["--self-test"]),
    ("magger60_depth_check.py", ["selftest"]),
    ("svd_to_annotations.py", ["--self-test"]),
]


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    bad = []
    for script, args in SELF_TESTS:
        path = HERE / script
        if not path.exists():
            continue                                     # a tool that is not part of this tree
        r = subprocess.run([sys.executable, "-I", str(path)] + args, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
        print("%-4s %-30s %s" % ("ok" if r.returncode == 0 else "FAIL", script, last[:80]))
        if r.returncode != 0:
            bad.append(script)
            print(r.stdout[-2000:] + r.stderr[-2000:])
    print("%d tool self-test(s) failed" % len(bad) if bad else "all tool self-tests passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
