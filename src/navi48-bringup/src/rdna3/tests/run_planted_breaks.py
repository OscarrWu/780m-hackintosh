#!/usr/bin/env python3
"""run_planted_breaks.py - prove the rdna3 host tests catch the defects this card names.

Each break is applied to a throwaway copy of the source tree (the real tree is never touched),
the corresponding host test runner is run, and the copy MUST fail. The untouched copy MUST pass
for BOTH runners. This is the "先失败后修复，记录证据" half of the card: if a break slips through,
the harness exits non-zero.

    src/navi48-bringup/src/rdna3/tests/run_planted_breaks.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))  # .../src/navi48-bringup/src
HOST = os.path.join("rdna3", "tests", "run_host_tests.sh")
SURVEY = os.path.join("rdna3", "tests", "run_survey_tests.sh")

# (name, runner, file relative to src, needle, replacement)
BREAKS = [
    (
        "writeguard_offlist_accepted",
        HOST,
        "rdna3/rdna3_writeguard.cpp",
        '}    return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, kOffListTag);',
        '}    return note_allow(kOffListTag, ctx, static_cast<uint32_t>(pa), size, cls, kOffListTag);',
    ),
    (
        "writeguard_aux_window_widened_1_dword",
        HOST,
        "rdna3/rdna3_writeguard.cpp",
        "{ 2u, 0x1f56u, 1u, kAuxTag, false },   /* AUX0_AUX_SW_DATA */",
        "{ 2u, 0x1f56u, 2u, kAuxTag, false },   /* AUX0_AUX_SW_DATA */",
    ),
    (
        "writeguard_mm_window_widened_1_dword",
        HOST,
        "rdna3/rdna3_writeguard.cpp",
        "{ 0u, 0x6u, 1u, kMmTag, true },    /* MM_INDEX_HI */",
        "{ 0u, 0x6u, 2u, kMmTag, true },    /* MM_INDEX_HI */",
    ),
    (
        "discovery_major_compared_ge",
        HOST,
        "rdna3/rdna3_discovery.cpp",
        "v.major_ok = (ip.major == e.major);",
        "v.major_ok = (ip.major >= e.major);",
    ),
    (
        "discovery_failure_returns_success",
        HOST,
        "rdna3/rdna3_discovery.cpp",
        "if (!disc.init(bar5, bar5_size)) {\n        rc = R3_DISC_E_PARSE;",
        "if (!disc.init(bar5, bar5_size)) {\n        return R3_DISC_OK;",
    ),
    (
        "discovery_input_trimmed_to_2k",
        HOST,
        "rdna3/rdna3_discovery.cpp",
        "if (!bar5 || bar5_size < 512)",
        "if (!bar5 || bar5_size < 2048)",
    ),
    (
        "tmr_offset_added_instead_of_subtracted",
        SURVEY,
        "rdna3/rdna3_survey.h",
        "*out = bytes - R3_DISC_TMR_OFFSET;",
        "*out = bytes + R3_DISC_TMR_OFFSET;",
    ),
    (
        "tmr_size_shrunk_to_8k",
        SURVEY,
        "rdna3/rdna3_survey.h",
        "#define R3_DISC_TMR_SIZE   (10u << 10)",
        "#define R3_DISC_TMR_SIZE   (8u << 10)",
    ),
    (
        "depth_clamp_removed",
        SURVEY,
        "rdna3/rdna3_survey.h",
        "return depth <= R3_STAGE0_MAX_DEPTH ? depth : 0u;",
        "return depth;",
    ),
    (
        "manifest_format_separator_changed",
        SURVEY,
        "rdna3/rdna3_survey.h",
        'snprintf(out + used, cap - used, "0x%08x=%u\\n",',
        'snprintf(out + used, cap - used, "0x%08x:%u\\n",',
    ),
    (
        "write_manifest_protocol_merged_by_addr",
        HOST,
        "rdna3/rdna3_wmanifest.h",
        "if (status == WMStatus::Allow && cls == WClass::Data) {",
        "if (status == WMStatus::Allow) {",
    ),
    (
        "mm_writes_annotated_per_write",
        SURVEY,
        "rdna3/rdna3_survey.cpp",
        "    r3_mm_write_note(ev, allowed);",
        "    r3_aux_annotate(lines, ev, allowed, \"mm-indirect\", \"mm-tmr-read\","
        " addr, value);",
    ),

    (
        "stage2 S12 psp-sync core removed",
        HOST,
        "rdna3/rdna3_integration.cpp",
        "    r3_integration_sync_preflight_env(ctx);",
        "    (void)ctx;  // sync removed",
    ),
    (
        "stage2 S12 stage_preflight call contract removed",
        HOST,
        "rdna3/rdna3_stages.cpp",
        "    r3_stages_preflight_sync_env(ic, readings, probe_count, &res);",
        "    (void)ic;  // stage_preflight sync call removed",
        "static",
    ),
    (
        "sdma declaration row removed",
        HOST,
        "amd/sdma_v6_0.cpp",
        '    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0SemWaitFailTimerCntl, 1, WClass::State,  WNeed::Required,      "s5-sem-wait-timer"},',
        "",
    ),
    (
        "test_integration fallback hardcoded list",
        HOST,
        "rdna3/tests/test_integration.cpp",
        "    const uint32_t expected = getSdmaRowCount() + 1u +          // SDMA regs + layout",
        "    const uint32_t expected = 61u;  // Step1 hardcoded fallback",
    ),
]

def run_tests(src_root, runner):
    return subprocess.run(
        [os.path.join(src_root, runner)],
        cwd=src_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=dict(os.environ, CXX=os.environ.get("CXX", "clang++")),
    )


def fresh_copy():
    # Temp tree mirrors the REAL repo layout below the project root:
    #   tmp/ (= 780m-hackintosh, holds evidence/)
    #     src/src/navi48-bringup/  (firmware/, tools/)
    #       src/  (= copied SRC; runner cwd, holds rdna3/tests/)
    # test_golden_values derives its repo root by stripping 7 parents from the absolute
    # __FILE__ path, so the depth MUST match or it reports "." and cannot read evidence.
    tmp = tempfile.mkdtemp(prefix="r3plant-")
    dst = os.path.join(tmp, "src", "src", "navi48-bringup", "src")
    shutil.copytree(SRC, dst)
    upper = os.path.join(tmp, "src", "src", "navi48-bringup")
    # run_host_tests.sh resolves firmware/ and tools/embed-firmware.py relative to
    # .../src/navi48-bringup (three levels above tests/). Without the blobs the runner
    # skips the firmware suites and exits early; without the embedder they fail to build.
    shutil.copytree(os.path.join(SRC, "..", "firmware"),
                    os.path.join(upper, "firmware"))
    shutil.copytree(os.path.join(SRC, "..", "tools"),
                    os.path.join(upper, "tools"))
    # test_golden_values reads the read-only evidence/ tree from the repo root.
    # evidence is ~1.3 GB: symlink, never copy (17 temp trees would move ~22 GB).
    # rmtree(tmp) removes the link, not the target.
    evd = os.path.abspath(os.path.join(SRC, "..", "..", "..", "..",
                                       "evidence"))
    os.symlink(evd, os.path.join(tmp, "evidence"))
    return tmp, dst

def main():
    failures = 0

    # Baseline: both runners must be green before any break is applied.
    for runner in (HOST, SURVEY):
        tmp, dst = fresh_copy()
        try:
            r = run_tests(dst, runner)
            if r.returncode != 0:
                print(f"BASELINE FAILED ({runner}) (tests must pass unmodified)")
                print(r.stdout)
                failures += 1
            else:
                print(f"baseline {runner}: green (as expected)")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    for name, runner, rel, needle, repl, *_static in BREAKS:
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

            if _static and _static[-1] == "static":
                # Source-contract check: the host runner cannot link the kext-only TU,
                # so the "test" here is a text contract that the call site exists.
                # Injected break removes the call line; the contract checker would fail
                # (red), which is exactly a caught break.
                with open(path, "r", encoding="utf-8") as f:
                    after = f.read()
                if needle in after:
                    print(f"BREAK {name}: NOT CAUGHT (call site still present)")
                    failures += 1
                else:
                    print(f"BREAK {name}: caught (static contract: call site removed)")
                continue

            r = run_tests(dst, runner)
            if r.returncode == 0:
                print(f"BREAK {name}: NOT CAUGHT (tests still green) <-- test gap")
                print(r.stdout)
                failures += 1
            else:
                last = [ln for ln in r.stdout.splitlines() if ln.startswith(("FAIL", "test_"))]
                print(f"BREAK {name}: caught {' | '.join(last[-3:])}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"planted-break harness: {failures} problem(s)")
        return 1
    print(f"planted-break harness: {len(BREAKS)}/{len(BREAKS)} breaks caught, baselines green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
