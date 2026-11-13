#!/usr/bin/env python3
"""Planted-break evidence for the dcn314 host tests.

Each entry below is a real defect injected into a throw-away copy of src/dcn314. The target test must
fail with the defect in place (the removed poll bound is caught by the per-run timeout) and must pass
again as soon as the original bytes are restored. The real tree is never touched.

    python3 tests/run_planted_breaks.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC", "clang")
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-fsanitize=address,undefined"]
RUN_TIMEOUT = 10

TESTS = {
    "regs": (["dcn314_core.c", "dcn314_ro.c"], "tests/test_dcn314.c"),
    "allow": (["dcn314_core.c", "dcn314_allow.c"], "tests/test_dcn314_allow.c"),
    "aux": (["dcn314_core.c", "dcn314_aux_min.c"], "tests/test_dcn314_aux.c"),
}

CASES = [
    dict(name="regs-otg-control-offset-drift", target="regs",
         path="dcn314_regs.h",
         old="((unsigned)((i) == 0 ? 0x1b41u :",
         new="((unsigned)((i) == 0 ? 0x1b42u :"),
    dict(name="allow-range-hi-off-by-one", target="allow",
         path="dcn314_allow.c",
         old="abs_dword <= dcn314_allow_ranges[i].abs_hi",
         new="abs_dword < dcn314_allow_ranges[i].abs_hi"),
    dict(name="aux-poll-bound-removed", target="aux",
         path="dcn314_io.h",
         old="    for (i = 0; i <= max_try; i++) {",
         new="    for (i = 0; ; i++) {"),
    dict(name="aux-write-gate-removed", target="aux",
         path="dcn314_aux_min.c",
         old="    if (dev->write_ok == NULL || !dev->write_ok(dev->cookie, abs, value))\n"
             "        return DCN314_E_DENY;\n",
         new=""),
    dict(name="aux-reply-truncation-removed", target="aux",
         path="dcn314_aux_min.c",
         old="        if (data_len > sizeof(out->data)) {\n"
             "            out->bytes = (uint8_t)sizeof(out->data);\n"
             "            out->truncated = true;\n"
             "        }\n",
         new=""),
]


def build_run(root, target, tag):
    srcs, test = TESTS[target]
    work = Path(tempfile.mkdtemp(prefix="dcn314ex_"))
    exe = work / tag
    cmd = [CC, *CFLAGS, "-I.", *srcs, test, "-o", str(exe)]
    try:
        c = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        if c.returncode != 0:
            return "compile", c.returncode, c.stderr.strip()[-600:]
        try:
            r = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True,
                               timeout=RUN_TIMEOUT)
            return "run", r.returncode, (r.stdout + r.stderr).strip()
        except subprocess.TimeoutExpired:
            return "timeout", -1, "exceeded %ds" % RUN_TIMEOUT
    finally:
        shutil.rmtree(work)


def main():
    print("dcn314 planted-break evidence (CC=%s)" % CC)
    ok = True

    for target in TESTS:
        kind, rc, out = build_run(ROOT, target, "clean_" + target)
        if kind == "run" and rc == 0:
            print("CLEAN PASS %s: %s" % (target, out.splitlines()[-1] if out else ""))
        else:
            ok = False
            print("CLEAN FAIL %s: %s rc=%s\n%s" % (target, kind, rc, out))

    for case in CASES:
        base = Path(tempfile.mkdtemp(prefix="dcn314pb_"))
        tmp = base / "dcn314"
        shutil.copytree(ROOT, tmp)
        p = tmp / case["path"]
        orig = p.read_text()
        if orig.count(case["old"]) != 1:
            ok = False
            print("SETUP FAIL %s: needle count %d" % (case["name"], orig.count(case["old"])))
            shutil.rmtree(base)
            continue

        p.write_text(orig.replace(case["old"], case["new"], 1))
        kind, rc, out = build_run(tmp, case["target"], "broken")
        caught = kind == "timeout" or (kind == "run" and rc != 0)
        ok = ok and caught

        p.write_text(orig)
        kind2, rc2, out2 = build_run(tmp, case["target"], "fixed")
        fixed = kind2 == "run" and rc2 == 0
        ok = ok and fixed

        print("%s %s: broken=%s(%s) restored=%s(%s)" % (
            "CAUGHT" if caught else "MISSED", case["name"], kind,
            "timeout" if kind == "timeout" else rc, kind2, rc2))
        if caught and kind == "run":
            fails = [l for l in out.splitlines() if l.startswith("FAIL")]
            if fails:
                print("    symptom: " + fails[0])
        elif caught:
            print("    symptom: hold on the unbounded poll, killed by the %ds timeout" % RUN_TIMEOUT)
        shutil.rmtree(base)

    print("RESULT:", ("%d/%d breaks caught, clean tests green" % (len(CASES), len(CASES)))
          if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
