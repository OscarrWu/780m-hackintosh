#!/bin/sh
# run_survey_tests.sh - build and run the pure survey host tests on Linux.
#
# Host-only: never touches a GPU. The kext itself is built separately with
# `make -f Makefile.linux TARGET=rdna3 all`. Run from anywhere:
#     src/navi48-bringup/src/rdna3/tests/run_survey_tests.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)          # .../src/navi48-bringup
src=$root/src

CXX=${CXX:-clang++}
FLAGS="-std=c++17 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer \
       -fsanitize=address,undefined -I$src -I$src/rdna3 -DTARGET_RDNA3=1"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

echo "== test_survey"
$CXX $FLAGS -o "$out/test_survey" "$here/test_survey.cpp" "$src/common/LogChannel.cpp"
"$out/test_survey"

echo "== test_admission"
$CXX $FLAGS -o "$out/test_admission" "$here/test_admission.cpp"
"$out/test_admission"

echo "all survey host tests passed"
