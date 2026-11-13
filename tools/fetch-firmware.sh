#!/bin/bash
#
# fetch-firmware.sh - copy the AMD firmware the Navi48 bring-up kext embeds from a local
# linux-firmware checkout into src/navi48-bringup/firmware/ (gitignored; never commit it).
#
# This script does NOT use the network. Clone linux-firmware yourself first:
#     git clone https://gitlab.com/kernel-firmware/linux-firmware.git
#   (mirror: https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
#
# Usage:  tools/fetch-firmware.sh <path-to-linux-firmware-checkout>
#
# The blobs are AMD's, under AMD's redistribution licence (LICENSE.amdgpu in linux-firmware).
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 <path-to-linux-firmware-checkout>" >&2
    echo "clone it first: git clone https://gitlab.com/kernel-firmware/linux-firmware.git" >&2
    exit 2
fi
SRC="$1/amdgpu"
[ -d "$SRC" ] || { echo "fetch-firmware.sh: no amdgpu/ directory in $1" >&2; exit 2; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$REPO/src/navi48-bringup/firmware"
mkdir -p "$DST"

# name  sha256 of the copy this project was developed against (a different linux-firmware
# revision may differ: a mismatch is a warning, not an error)
# Two targets share one firmware/ directory: the RDNA4 ladder (default) and the RDNA3
# stage-2 ladder (TARGET=rdna3). Both sets are copied so either target can be built from
# a clean checkout; each Makefile still checks only its own ten. A checkout that carries
# only one target's blobs is therefore usable: missing RDNA4 blobs are a warning (the
# rdna4 build fails closed on them), missing RDNA3 blobs are an error because RDNA3 is the
# deployment target.
RDNA4_BLOBS="
gc_12_0_1_imu     b3b301fb636efc77b63ce4d2ced0f90c851d03c19681852faa45598e6f5773fd
gc_12_0_1_me      5549655093724b3e3b5fa016e8f1ef6ce3976209432230852cd94acbc1c245cb
gc_12_0_1_mec     72b25088072d630d115748ee3607d0db481b22a15260af3d0d599e516a0d7abb
gc_12_0_1_pfp     6dc9e6d22e0f9ddd04f12eedb5d1f6affae6d76d2993567aafde0e1619071c15
gc_12_0_1_rlc     6ba4459532246a5c415d3cb33c9b1248294e48f67b827e2accb292a8d1a5c0ec
gc_12_0_1_uni_mes bf26e0300989ebd011e991d76396e2c1673e1caa502fb6d8413e387eb1456676
psp_14_0_3_sos    23bea01a0c6f36d00759d0765d46cb4cb4aa87398b2fbccacbf547a890c0bf51
psp_14_0_3_ta     38d6691bf64232946cd5ce5735363c7c2877830ff640b9b95516403257fe2922
sdma_7_0_1        73c29e1c1714ebc95d2221ba56e187910902891593010653bf9518937e414a59
smu_14_0_3        3221ef2ddb341570eeb727e1e16f170bfb2ea1230be4a7eb248d0b183fbfbf15
"
RDNA3_BLOBS="
psp_13_0_4_toc    11e3529d748e2b6b204681183360a6ead80665b8deda996dfcf35973f789fc3d
psp_13_0_4_ta     f4a2687e1cd940b016e308bc87b9b840f845526d9ef21147416d89c1c72a7944
gc_11_0_1_imu     ac71f4eec713fc35b4a1fe27531e3eb04edd81eeac2cef64df01ac50d8510805
gc_11_0_1_rlc     5f07dc1f0a75ecd9cb56d805ea869184a50ed9e43d811ebf833b8906534650ef
gc_11_0_1_pfp     449c72cae1c57838185197ad4003aeee7ccbdba892a28bb2624d9e85f57a2df1
gc_11_0_1_me      62c5e749ad848941e5d8b85f66eeb2b969ee82919724dfd94dbf6e354be8befc
gc_11_0_1_mec     b0a4006b3934adc1d7996e6d901206dea37203b4839debe4a5ed4e393a615cbb
gc_11_0_1_mes_2   962f56c7e661479e3cabf89c1fcd6344f10cfa8b8d1f672fc445bb21414c1392
gc_11_0_1_mes1    3a8b60ea88b7adbbc71da0ddee0370d5d65c1d66186fd4f4ba9d16a854b24db8
sdma_6_0_1        708c2c2c45262c98ebe8e34e32c3f1ce8eb5b007bab560c9ea9b576a8e4d6768
"

missing_rdna3=0

copy_blobs() {
    local target="$1" blobs="$2" name want got
    while read -r name want; do
        [ -n "$name" ] || continue
        if [ -f "$SRC/$name.bin" ]; then
            cp "$SRC/$name.bin" "$DST/$name.bin"
        elif [ -f "$SRC/$name.bin.zst" ] && command -v zstd >/dev/null 2>&1; then
            zstd -dqf -o "$DST/$name.bin" "$SRC/$name.bin.zst"
        elif [ "$target" = rdna3 ]; then
            echo "MISSING  $name.bin (not in $SRC) - required for RDNA3" >&2
            missing_rdna3=1
            continue
        else
            echo "WARN     $name.bin (not in $SRC) - RDNA4 build will fail closed" >&2
            continue
        fi
        got="$(shasum -a 256 "$DST/$name.bin" | awk '{print $1}')"
        if [ "$got" = "$want" ]; then echo "ok       $name.bin ($(wc -c < "$DST/$name.bin" | tr -d ' ') bytes)"
        else echo "DIFFERS  $name.bin (sha256 $got; developed against $want) - copied anyway"; fi
    done <<< "$blobs"
}

copy_blobs rdna4 "$RDNA4_BLOBS"
copy_blobs rdna3 "$RDNA3_BLOBS"
cp "$1/LICENSE.amdgpu" "$DST/" 2>/dev/null || true
if [ "$missing_rdna3" -ne 0 ]; then
    echo "error: RDNA3 firmware is incomplete in $SRC (see MISSING above)" >&2
    exit 1
fi
echo "copied into $DST"
