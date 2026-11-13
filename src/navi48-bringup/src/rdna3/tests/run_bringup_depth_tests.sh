#!/bin/sh
# run_bringup_depth_tests.sh - build and run the n48r3-depth entry-wiring host test.
#
# Host-only: never touches a GPU. Self-contained so it does not need an edit to
# run_host_tests.sh (a file owned by the concurrent Fix-Tiers card). Run from anywhere:
#     src/navi48-bringup/src/rdna3/tests/run_bringup_depth_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)          # .../src/navi48-bringup
src=$root/src

CXX=${CXX:-clang++}
FLAGS="-std=c++17 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer \
       -fsanitize=address,undefined -I$src -I$src/rdna3 -I$root/tests/dalshim \
       -DTARGET_RDNA3=1"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

echo "== test_bringup_depth"
$CXX $FLAGS -o "$out/test_bringup_depth" "$here/test_bringup_depth.cpp" \
    "$src/common/BootArgFamily.cpp"
"$out/test_bringup_depth"

echo "all bringup-depth host tests passed"
