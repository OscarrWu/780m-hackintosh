#!/bin/sh
# run_common_tests.sh - build and run the common/ host unit tests on Linux.
#
# Host-only: never touches a GPU. The kext is built separately with
# `make -f Makefile.linux TARGET=rdna3 all`. BootArgFamily and LogChannel are
# target-dependent, so each is built and run twice, with and without
# -DTARGET_RDNA3 (the two compile lines are in the test file headers). Run from
# anywhere:
#     src/navi48-bringup/src/common/tests/run_common_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)          # .../src/navi48-bringup
src=$root/src

CXX=${CXX:-clang++}
FLAGS="-std=c++17 -Wall -Wextra -Werror -O1 -I$src/common -I$root/tests/dalshim"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

echo "== test_bootarg (navi48)"
$CXX $FLAGS -o "$out/test_bootarg_navi48" "$here/test_bootarg.cpp" "$src/common/BootArgFamily.cpp"
"$out/test_bootarg_navi48"

echo "== test_bootarg (rdna3)"
$CXX $FLAGS -DTARGET_RDNA3=1 -o "$out/test_bootarg_rdna3" "$here/test_bootarg.cpp" "$src/common/BootArgFamily.cpp"
"$out/test_bootarg_rdna3"

echo "== test_logchannel (navi48)"
$CXX $FLAGS -o "$out/test_logchannel_navi48" "$here/test_logchannel.cpp" "$src/common/LogChannel.cpp"
"$out/test_logchannel_navi48"

echo "== test_logchannel (rdna3)"
$CXX $FLAGS -DTARGET_RDNA3=1 -o "$out/test_logchannel_rdna3" "$here/test_logchannel.cpp" "$src/common/LogChannel.cpp"
"$out/test_logchannel_rdna3"

echo "== test_stagerunner"
$CXX $FLAGS -o "$out/test_stagerunner" "$here/test_stagerunner.cpp" "$src/common/StageRunner.cpp"
"$out/test_stagerunner"

echo "== test_gating"
$CXX $FLAGS -o "$out/test_gating" "$here/test_gating.cpp" "$src/common/StageRunner.cpp"
"$out/test_gating"

echo "all common host tests passed"
