#!/bin/sh
# run_host_tests.sh - build and run the rdna3 host unit tests on Linux.
#
# These tests are host-only (they never touch a GPU); the kext itself is built separately with
# `make -f Makefile.linux TARGET=rdna3 all`. Run from anywhere:
#     src/navi48-bringup/src/rdna3/tests/run_host_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)          # .../src/navi48-bringup
src=$root/src

CXX=${CXX:-clang++}
CC=${CC:-clang}
FLAGS="-std=c++17 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer \
       -fsanitize=address,undefined -I$src -I$src/rdna3 -I$src/rdna3/tests"
STUB="$here/n48log_stub.cpp"   # host stand-in for the n48log ring (see n48log_stub.h)

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

echo "== test_ip_expect"
$CXX $FLAGS -o "$out/test_ip_expect" "$here/test_ip_expect.cpp"
"$out/test_ip_expect"

echo "== test_writeguard"
$CXX $FLAGS -o "$out/test_writeguard" "$here/test_writeguard.cpp" "$src/rdna3/rdna3_writeguard.cpp" \
    "$src/rdna3/rdna3_wmanifest.cpp" "$src/rdna3/rdna3_substep.cpp" "$STUB"
"$out/test_writeguard"

echo "== test_discovery"
$CXX $FLAGS -o "$out/test_discovery" "$here/test_discovery.cpp" \
    "$src/rdna3/rdna3_discovery.cpp" "$src/ipdiscovery.cpp"
"$out/test_discovery"

# The firmware tests (T4/T5/T6) parse the REAL embedded blobs, so they need
# the generated src/fw/fw_*.c arrays: build them exactly as the Makefile does
# (firmware/%.bin -> src/fw/fw_%.c), but into the temp dir so the tree is not
# touched.  Without the blobs these two suites cannot run -- say so loudly
# rather than silently skipping the only byte-level firmware evidence.
FW_RDNA3="psp_13_0_4_toc psp_13_0_4_ta gc_11_0_1_imu gc_11_0_1_rlc \
          gc_11_0_1_pfp gc_11_0_1_me gc_11_0_1_mec gc_11_0_1_mes_2 \
          gc_11_0_1_mes1 sdma_6_0_1"
fw_missing=""
for f in $FW_RDNA3; do
    [ -f "$root/firmware/$f.bin" ] || fw_missing="$fw_missing $f.bin"
done
if [ -n "$fw_missing" ]; then
    echo "SKIP test_fwhdr / test_fwtable_rdna3_impl: missing firmware/$fw_missing"
    echo "     (copy them from the archive: evidence/src/linux-firmware-bda6eccc/amdgpu/)"
    echo "all rdna3 host tests passed (firmware tests skipped)"
    exit 0
fi
for f in $FW_RDNA3; do
    python3 "$root/tools/embed-firmware.py" "$root/firmware/$f.bin" \
        "$out/fw_$f.c" "fw_$f" >/dev/null
    $CC -std=c11 -O1 -c "$out/fw_$f.c" -o "$out/fw_$f.o"
done

echo "== test_fwhdr (T5/T6)"
$CXX $FLAGS -o "$out/test_fwhdr" "$here/test_fwhdr.cpp" \
    "$src/rdna3/rdna3_fwhdr.cpp" "$src/rdna3/fw_table_rdna3.cpp" "$out"/fw_*.o
"$out/test_fwhdr"

echo "== test_fwtable_rdna3_impl (T4)"
$CXX $FLAGS -o "$out/test_fwtable_rdna3_impl" "$here/test_fwtable_rdna3_impl.cpp" \
    "$src/rdna3/fw_table_rdna3.cpp" "$src/rdna3/rdna3_fwhdr.cpp" "$out"/fw_*.o
"$out/test_fwtable_rdna3_impl"

echo "== test_psp_msg (T13 / sequence)"
# The kernel TUs include only libkern; the host runners add the libc formatting headers.
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_psp_msg" \
    "$here/test_psp_msg.cpp" "$src/rdna3/rdna3_substep.cpp" "$src/amd/psp_v13_0.cpp" \
    "$src/rdna3/rdna3_fwhdr.cpp" "$src/rdna3/rdna3_layout.cpp" \
    "$src/rdna3/rdna3_writeguard.cpp" "$src/rdna3/rdna3_wmanifest.cpp" \
    "$src/rdna3/fw_table_rdna3.cpp" "$out"/fw_*.o "$STUB"
"$out/test_psp_msg"

echo "== test_smu_msg (K5, SMU 13.0.4)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_smu_msg" \
    "$here/test_smu_msg.cpp" "$src/amd/smu_v13_0.cpp" "$src/rdna3/rdna3_substep.cpp" \
    "$src/rdna3/rdna3_writeguard.cpp" "$src/rdna3/rdna3_wmanifest.cpp" "$STUB"
"$out/test_smu_msg"

echo "== test_gfx_status (K6, read-only GFX)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_gfx_status" \
    "$here/test_gfx_status.cpp" "$src/amd/gfx_v11_0.cpp" "$src/rdna3/rdna3_substep.cpp" \
    "$src/rdna3/rdna3_writeguard.cpp" "$src/rdna3/rdna3_wmanifest.cpp" "$STUB"
"$out/test_gfx_status"

echo "== test_sdma_msg (K7, SDMA0 queue0)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_sdma_msg" \
    "$here/test_sdma_msg.cpp" "$src/amd/sdma_v6_0.cpp" "$src/rdna3/rdna3_substep.cpp" \
    "$src/rdna3/rdna3_layout.cpp" "$src/rdna3/rdna3_writeguard.cpp" "$src/rdna3/rdna3_wmanifest.cpp" "$STUB"
"$out/test_sdma_msg"

echo "== test_mes_api (K8, MES scheduler)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_mes_api" \
    "$here/test_mes_api.cpp" "$src/amd/mes_v11_0.cpp" \
    "$src/rdna3/rdna3_substep.cpp" "$src/rdna3/rdna3_fwhdr.cpp" \
    "$src/rdna3/rdna3_layout.cpp" "$src/rdna3/rdna3_writeguard.cpp" \
    "$src/rdna3/rdna3_wmanifest.cpp" "$src/rdna3/fw_table_rdna3.cpp" \
    "$out"/fw_*.o "$STUB"
"$out/test_mes_api"

echo "== test_teardown (K9, dump + reverse teardown)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_teardown" \
    "$here/test_teardown.cpp" "$src/rdna3/rdna3_dump.cpp" \
    "$src/rdna3/rdna3_teardown.cpp" "$src/rdna3/rdna3_writeguard.cpp" \
    "$src/rdna3/rdna3_wmanifest.cpp" "$src/rdna3/rdna3_substep.cpp" \
    "$src/rdna3/rdna3_layout.cpp" "$src/rdna3/rdna3_integration.cpp" \
    "$src/amd/sdma_v6_0.cpp" "$src/amd/mes_v11_0.cpp" \
    "$src/amd/psp_v13_0.cpp" "$src/amd/smu_v13_0.cpp" \
    "$src/rdna3/rdna3_fwhdr.cpp" "$src/rdna3/fw_table_rdna3.cpp" \
    "$out"/fw_*.o "$src/common/StageRunner.cpp" "$STUB"
"$out/test_teardown"

echo "== test_integration (K10, matrix + production registration + R3Io arithmetic)"
$CXX $FLAGS -include stdio.h -include string.h -o "$out/test_integration" \
    "$here/test_integration.cpp" "$src/rdna3/rdna3_integration.cpp" \
    "$src/rdna3/rdna3_writeguard.cpp" "$src/rdna3/rdna3_wmanifest.cpp" \
    "$src/rdna3/rdna3_substep.cpp" "$src/rdna3/rdna3_layout.cpp" \
    "$src/rdna3/rdna3_dump.cpp" "$src/amd/sdma_v6_0.cpp" \
    "$src/amd/mes_v11_0.cpp" "$src/amd/psp_v13_0.cpp" \
    "$src/amd/smu_v13_0.cpp" \
    "$src/rdna3/rdna3_fwhdr.cpp" "$src/rdna3/fw_table_rdna3.cpp" \
    "$out"/fw_*.o "$src/common/StageRunner.cpp" "$STUB"
"$out/test_integration"


echo "== test_golden_values (T18, P1 address mechanism)"
$CXX $FLAGS -o "$out/test_golden_values" "$here/test_golden_values.cpp"
"$out/test_golden_values"

echo "== test_userclient_info (S3 info offset-chunked read)"
# The driver TU is compiled with plain -Wall (the kernel build's level, Makefile.linux):
# its unused IOExternalMethodDispatch/OSObject parameters would fail -Wextra without
# any real defect. ucmock/ models the offset-chunked call shape (tmp/main/技术方案-S3-info分块读取改造.md);
# dalshim/ supplies the real kIOReturn values.
UCFLAGS="-std=c++17 -Wall -Werror -g -O1 -fno-omit-frame-pointer \
         -fsanitize=address,undefined -I$here/ucmock -I$root/tests/dalshim \
         -I$src -I$src/rdna3"
$CXX $UCFLAGS -o "$out/test_userclient_info" "$here/test_userclient_info.cpp"
"$out/test_userclient_info"


echo "== n48r3test tool tests (host, no IOKit)"
# The userspace tool's argument parsing lives in the same C file; build it with
# N48R3_HOST_TEST exactly as documented in the file header and run its suites.
tool_src=$root/../../tools/pc/n48r3test.c
$CC -DN48R3_HOST_TEST -Wall -Wextra -Werror -o "$out/n48r3test_host" "$tool_src"
"$out/n48r3test_host"
echo "all rdna3 host tests passed"
