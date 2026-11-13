#!/bin/bash
# check-x87 - V2 "x87 = 0" gate (Main adjudication 2026-11-02).
#
# 1)  llvm-objdump -d cannot tell code from bytes inside __text, and a switch
#     jump table reads as garbage x87 (rdna4 fw_table has 30 such bytes).  A
#     naive opcode grep therefore reports 30 false x87 "instructions".
# 2)  tools/x87gate.py is CFG-aware: an x87 mnemonic only fails the build when
#     it is reachable from its function's entry.  Unreachable matches are
#     reported as data, not failed.
#
# Runs on the freshly linked EXEC.  Add to the post-link gate chain in
# Makefile.linux after check-firmware / GATE_OURS.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-python3}"
for target in "${@:-rdna3}"; do
    exec_file="$REPO/src/navi48-bringup/build/$target/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup"
    [ -f "$exec_file" ] || { echo "check-x87: missing $exec_file" >&2; exit 2; }
    "$PY" "$REPO/tools/x87gate.py" "$exec_file"
done
