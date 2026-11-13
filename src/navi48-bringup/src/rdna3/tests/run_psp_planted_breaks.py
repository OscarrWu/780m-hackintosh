#!/usr/bin/env python3
"""run_psp_planted_breaks.py - H3 harness for the K4 PSP substep.

Each break is a named mutation of the real sources inside a THROWAWAY copy of
src/navi48-bringup/src; the host suite is rebuilt there and must fail. The real tree is never
modified. Every break's expectation comes from the tech plan (§3.1) / requirement C1, so a
"not caught" result is a genuine test gap and is reported as such.

Breaks:
  B1 sos-check-skipped      The P3 SOS-alive gate always passes.                       MUST FAIL
  B2 autoload-early         AUTOLOAD_RLC is issued BEFORE RLC_G.                        MUST FAIL
  B3 load-order-swapped     Two LOAD_IP_FW entries are emitted out of table order.      MUST FAIL
  B4 fw-size-hardcoded      LOAD_IP_FW fw_size is replaced by a constant.               MUST FAIL
  B5 tmr-size-ignored       SETUP_TMR sends a fixed 4 MiB instead of resp.tmr_size.     MUST FAIL
  B6 system-phy-constant    system_phy is the nullarc constant, not the local PA.       MUST FAIL
  B7 no-fw-zeroing          The ring/cmd/fence zero-fill is dropped.                    MUST FAIL
  B8 rs64-loaded            RS64 (87) is added to the load set.                         MUST FAIL
  B9 no-ready-bit           The PSP ready bit is never set.                             MUST FAIL
  B10 fence-poll-unbounded  The fence poll becomes unbounded.                           MUST FAIL
  B11 mec-jt-not-deducted   MEC ships ucode_size_bytes (jump table included).            MUST FAIL
  B12 c2pmsg33-hard-gate    S10 fails when C2PMSG_33 bit31 is clear.                     MUST FAIL
  B13 fence-timeout-ok      A real fence timeout is treated as success.                  MUST FAIL
  B14 s12-version-gated     The PSP S12 becomes a hard failure (old behavior).           MUST FAIL

Usage: python3 run_psp_planted_breaks.py [--only B1,B3]
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))          # .../rdna3/tests
SRC = os.path.dirname(os.path.dirname(ROOT))               # .../navi48-bringup/src
BRINGUP = os.path.dirname(SRC)                             # .../navi48-bringup
FW = os.path.join(BRINGUP, "firmware")

FW_BLOBS = [
    "psp_13_0_4_toc", "psp_13_0_4_ta", "gc_11_0_1_imu", "gc_11_0_1_rlc", "gc_11_0_1_pfp",
    "gc_11_0_1_me", "gc_11_0_1_mec", "gc_11_0_1_mes_2", "gc_11_0_1_mes1", "sdma_6_0_1",
]

# (name, file, old text, new text)
BREAKS = [
    ("B1 sos-check-skipped", "amd/psp_v13_0.cpp",
     "    if (!ok33 || !ok81 || v81 == 0u) {",
     "    if (false) {"),

    ("B2 autoload-early", "amd/psp_v13_0.cpp",
     "        // S9: AUTOLOAD_RLC IMMEDIATELY after RLC_G(8) (amdgpu_psp.c:3533-3541). Placed\n"
     "        // inside the loop body, keyed on fw_type, exactly like Linux.\n"
     "        if (row.entry->fw_type == kFwTypeRlcG) {",
     "        if (row.entry->fw_type == 2u) {"),   # autoload after PFP

    ("B3 load-order-swapped", "rdna3/fw_table_rdna3.cpp",
     '    {  2, amdgpu::FwId::GC11_PFP,   "pfp"         },\n'
     '    {  1, amdgpu::FwId::GC11_ME,    "me"          },',
     '    {  1, amdgpu::FwId::GC11_ME,    "me"          },\n'
     '    {  2, amdgpu::FwId::GC11_PFP,   "pfp"         },'),

    ("B4 fw-size-hardcoded", "amd/psp_v13_0.cpp",
     "        put_u32(fw_body, kLoadIpFwSize, row.seg->size);",
     "        put_u32(fw_body, kLoadIpFwSize, 267008u);"),   # nullarc MEC constant

    ("B5 tmr-size-ignored", "amd/psp_v13_0.cpp",
     "    put_u32(tmr_body, kSetupTmrSize, static_cast<uint32_t>(p.tmr_size));",
     "    put_u32(tmr_body, kSetupTmrSize, 0x400000u);"),

    ("B6 system-phy-constant", "amd/psp_v13_0.cpp",
     "    put_u32(tmr_body, kSetupTmrSysPhyLo, static_cast<uint32_t>(p.tmr_sys_phy));",
     "    put_u32(tmr_body, kSetupTmrSysPhyLo, 0);"),

    ("B7 no-fw-zeroing", "amd/psp_v13_0.cpp",
     "    if (!zero_grant(p, ring, R3_PSP_RING_ZERO, \"s2-ring-zero\") ||",
     "    if (false ||"),

    ("B8 rs64-loaded", "amd/psp_v13_0.cpp",
     "        put_u32(fw_body, kLoadIpFwType, row.entry->fw_type);",
     "        put_u32(fw_body, kLoadIpFwType," 
     "                 row.entry->fw_type == 2u ? 87u : row.entry->fw_type);"),

    ("B9 no-ready-bit", "amd/psp_v13_0.cpp",
     "        env->rt->ready_bits |= R3_IP_BIT(R3Ip::Psp);",
     "        (void)0;"),

    ("B10 fence-poll-unbounded", "amd/psp_v13_0.cpp",
     "    for (uint32_t i = 0; i < kPspFencePolls; i++) {",
     "    for (uint32_t i = 0; i < 1u; i++) {"),   # never waits

    ("B11 mec-jt-not-deducted", "rdna3/rdna3_fwhdr.cpp",
     "    const uint32_t payload =\n"
     "        out->ucode_size_bytes - static_cast<uint32_t>(jt_bytes);",
     "    const uint32_t payload = out->ucode_size_bytes;"),

    ("B12 c2pmsg33-hard-gate", "amd/psp_v13_0.cpp",
     "    const uint32_t v33 = reg_read(p, r3gold::kMp0SmnC2pmsg33, &ok);\n"
     "    evf(ev, kTagChk,",
     "    const uint32_t v33 = reg_read(p, r3gold::kMp0SmnC2pmsg33, &ok);\n"
     "    if (!ok || (v33 & kPspC2pmsg33ReadyBit) == 0u)\n"
     "        return r3_substep_fail(kR3SecChk, 10, \"chk:c2pmsg33-gate\");\n"
     "    evf(ev, kTagChk,"),

    ("B13 fence-timeout-ok", "amd/psp_v13_0.cpp",
     "        r3_io_udelay(p.io, kPspFencePollUs);\n"
     "    }\n"
     "    return false;\n"
     "}",
     "        r3_io_udelay(p.io, kPspFencePollUs);\n"
     "    }\n"
     "    return true;\n"
     "}"),

    ("B14 s12-version-gated", "amd/psp_v13_0.cpp",
     "            \"S12 psp_version pre=---- post=0x%08x%s (record, not a gate)\", psp_ver,\n"
     "            oks ? \"\" : \" (unreadable)\");\n"
     "    }\n"
     "    return r3_substep_pass();\n"
     "}",
     "            \"S12 psp_version pre=---- post=0x%08x%s (record, not a gate)\", psp_ver,\n"
     "            oks ? \"\" : \" (unreadable)\");\n"
     "    }\n"
     "    return r3_substep_fail(kR3SecChk, 12, \"chk:fw-version-gated\");\n"
     "}"),
]


def prepare(tmp):
    """Copy the source tree, build the fw objects once, return paths."""
    tree = os.path.join(tmp, "src")
    shutil.copytree(SRC, tree)
    fwobj = []
    for name in FW_BLOBS:
        blob = os.path.join(FW, name + ".bin")
        c = os.path.join(tmp, "fw_" + name + ".c")
        o = os.path.join(tmp, "fw_" + name + ".o")
        subprocess.run(["python3", os.path.join(BRINGUP, "tools", "embed-firmware.py"),
                        blob, c, "fw_" + name], check=True, capture_output=True)
        subprocess.run(["clang", "-std=c11", "-O1", "-c", c, "-o", o], check=True,
                       capture_output=True)
        fwobj.append(o)
    return tree, fwobj


def build_and_run(tree, fwobj, log_path):
    cxx = os.environ.get("CXX", "clang++")
    exe = os.path.join(os.path.dirname(tree), "t")
    cmd = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
           "-fsanitize=address,undefined",
           "-I" + tree, "-I" + os.path.join(tree, "rdna3"),
           "-I" + os.path.join(tree, "rdna3", "tests"),
           "-o", exe,
           os.path.join(tree, "rdna3", "tests", "test_psp_msg.cpp"),
           os.path.join(tree, "rdna3", "rdna3_substep.cpp"),
           os.path.join(tree, "amd", "psp_v13_0.cpp"),
           os.path.join(tree, "rdna3", "rdna3_fwhdr.cpp"),
           os.path.join(tree, "rdna3", "rdna3_layout.cpp"),
           os.path.join(tree, "rdna3", "rdna3_writeguard.cpp"),
           os.path.join(tree, "rdna3", "rdna3_wmanifest.cpp"),
           os.path.join(tree, "rdna3", "fw_table_rdna3.cpp")] + fwobj
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        with open(log_path, "a") as f:
            f.write("  BUILD FAILED (also a catch):\n" + b.stderr[-2000:] + "\n")
        return False, "build-failure"
    r = subprocess.run([exe], capture_output=True, text=True)
    with open(log_path, "a") as f:
        f.write(r.stdout[-6000:] + "\n")
    # A semantic catch is a completed run with failed checks; a crash is not evidence of a
    # caught break (it is reported separately so it cannot be mistaken for one).
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    tail = lines[-1] if lines else ""
    if r.returncode < 0:
        return False, f"crashed({r.returncode})"
    return r.returncode == 0, tail


def main():
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    # Evidence lands in <project>/tmp/programmer/K4/planted/, never inside the source tree.
    # BRINGUP = <project>/src/src/navi48-bringup -> project root is three levels up.
    project = os.path.dirname(os.path.dirname(os.path.dirname(BRINGUP)))
    out_dir = os.environ.get("PSP_BREAK_LOG_DIR",
                             os.path.join(project, "tmp", "programmer", "K4", "planted"))
    os.makedirs(out_dir, exist_ok=True)
    log_path = os.path.join(out_dir, "11-breaks-final.log")
    open(log_path, "w").close()

    results = []
    with tempfile.TemporaryDirectory() as base:
        # baseline: untouched copy must be green
        tree, fwobj = prepare(os.path.join(base, "base"))
        ok, tail = build_and_run(tree, fwobj, log_path)
        print(f"BASELINE: {'green' if ok else 'RED'} ({tail})")
        results.append(("BASELINE", ok, tail))
        shutil.rmtree(os.path.join(base, "base"))

        for name, rel, old, new in BREAKS:
            if only and name.split()[0] not in only:
                continue
            work = os.path.join(base, name.split()[0])
            tree, fwobj = prepare(work)
            path = os.path.join(tree, rel)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            if old not in text:
                print(f"{name}: PATTERN NOT FOUND in {rel}")
                results.append((name, False, "pattern-missing"))
                shutil.rmtree(work)
                continue
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(old, new, 1))
            with open(log_path, "a") as f:
                f.write(f"\n===== {name} =====\n")
            ok, tail = build_and_run(tree, fwobj, log_path)
            caught = not ok
            print(f"{name}: {'CAUGHT' if caught else 'NOT CAUGHT'} ({tail})")
            results.append((name, caught, tail))
            shutil.rmtree(work)

    bad = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(bad)}/{len(results)} as expected"
          f" (baseline green + every planted break caught)")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
