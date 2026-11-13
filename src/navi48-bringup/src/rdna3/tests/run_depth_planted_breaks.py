#!/usr/bin/env python3
"""run_depth_planted_breaks.py - prove the n48r3-depth host test catches both defects.

Break 1 flips the central depth default away from 0 (T1); break 2 flips the out-of-range
fallback away from 0. Each break is applied to a throwaway copy of the source tree (the
real tree is never touched); the copy MUST fail, the untouched copy MUST pass.

    src/navi48-bringup/src/rdna3/tests/run_depth_planted_breaks.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))  # .../src/navi48-bringup/src
RUNNER = os.path.join("rdna3", "tests", "run_bringup_depth_tests.sh")

# (name, file relative to src, needle, replacement)
BREAKS = [
    (
        "depth_default_flipped_nonzero",
        "common/BootArgFamily.cpp",
        "static TgtDefaultPolicy gPolicy = { 0, 0, 0 };",
        "static TgtDefaultPolicy gPolicy = { 0, 0, 2 };",
    ),
    (
        "depth_clamp_fallback_nonzero",
        "rdna3/NaviR3Bringup.hpp",
        "return raw <= 5u ? raw : 0u;",
        "return raw <= 5u ? raw : 1u;",
    ),
]


def run_tests(src_root):
    return subprocess.run(
        [os.path.join(src_root, RUNNER)],
        cwd=src_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=dict(os.environ, CXX=os.environ.get("CXX", "clang++")),
    )


def fresh_copy():
    tmp = tempfile.mkdtemp(prefix="r3depth-")
    dst = os.path.join(tmp, "src")
    shutil.copytree(SRC, dst)
    # The runner resolves $root/tests/dalshim (the pexpert KPI shim); the source copy
    # above only carries src/, so bring the shim along at the matching temp path.
    shutil.copytree(
        os.path.join(SRC, "..", "tests", "dalshim"),
        os.path.join(tmp, "tests", "dalshim"),
    )
    return tmp, dst

def main():
    failures = 0

    tmp, dst = fresh_copy()
    try:
        r = run_tests(dst)
        if r.returncode != 0:
            print("BASELINE FAILED (tests must pass unmodified)")
            print(r.stdout)
            failures += 1
        else:
            print("baseline: green (as expected)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for name, rel, needle, repl in BREAKS:
        tmp, dst = fresh_copy()
        try:
            path = os.path.join(dst, rel)
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
            if text.count(needle) != 1:
                print(f"BREAK {name}: needle count {text.count(needle)} != 1 (harness error)")
                failures += 1
                continue
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(needle, repl, 1))

            r = run_tests(dst)
            if r.returncode == 0:
                print(f"BREAK {name}: NOT CAUGHT (tests still green) <-- test gap")
                print(r.stdout)
                failures += 1
            else:
                last = [ln for ln in r.stdout.splitlines() if ln.startswith("FAIL")]
                print(f"BREAK {name}: caught (tests failed) {' | '.join(last[-3:])}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"depth planted-break harness: {failures} problem(s)")
        return 1
    print("depth planted-break harness: 2/2 breaks caught, baseline green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
