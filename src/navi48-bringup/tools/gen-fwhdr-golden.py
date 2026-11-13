#!/usr/bin/env python3
"""gen-fwhdr-golden.py — emit the T6 header-parsing golden from the blobs.

The host test must not read the firmware files (the kext tree does not carry
them; they are build inputs), yet T6 has to compare byte-level header values
from an INDEPENDENT source (H4).  This script is that source: it reads the ten
ARCHIVED blobs straight out of evidence/src/linux-firmware-bda6eccc/amdgpu/,
decodes the header fields with Python's own struct/int arithmetic (a second
implementation, deliberately not the C++ one under test), and writes:

  * the per-field table the test asserts against, and
  * the first 256 bytes of each blob as C initialisers, so the test can run
    the parser on real header bytes (not a hand-written mock).

Usage (from src/navi48-bringup):
    python3 tools/gen-fwhdr-golden.py            # rewrite the .inc
    python3 tools/gen-fwhdr-golden.py --check    # verify it is current

The generated file src/rdna3/tests/fwhdr_golden.inc is TRACKED: the test must
build on a machine that has no firmware archive.  When the archive is absent
--check reports "skipped" and exits 0 (nothing to compare against).
"""

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path

# Same ten blobs as A1 / the Makefile's FW_RDNA3, in table order.
BLOBS = [
    "psp_13_0_4_toc",
    "psp_13_0_4_ta",
    "gc_11_0_1_imu",
    "gc_11_0_1_rlc",
    "gc_11_0_1_pfp",
    "gc_11_0_1_me",
    "gc_11_0_1_mec",
    "gc_11_0_1_mes_2",
    "gc_11_0_1_mes1",
    "sdma_6_0_1",
]

HEAD_BYTES = 256   # enough for the largest header (RLC v2.3: 204 bytes)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                       # src/navi48-bringup
OUT = ROOT / "src/rdna3/tests/fwhdr_golden.inc"
ARCHIVE = (ROOT / "../../../../evidence/src/linux-firmware-bda6eccc/amdgpu").resolve()


def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def u16(b, off):
    return int.from_bytes(b[off:off + 2], "little")


def common(b):
    """common_firmware_header — amdgpu_ucode.h:30-41."""
    return dict(
        size_bytes=u32(b, 0),
        header_size_bytes=u32(b, 4),
        version_major=u16(b, 8),
        version_minor=u16(b, 10),
        ip_major=u16(b, 12),
        ip_minor=u16(b, 14),
        ucode_version=u32(b, 16),
        ucode_size_bytes=u32(b, 20),
        array_offset=u32(b, 24),
        crc32=u32(b, 28),
    )


def segs_of(name, b):
    """The expected segment list for one blob, as (fw_type, offset, size).

    Field families: sdma v2_0 (amdgpu_ucode.h:328-336, amdgpu_ucode.c:891-901),
    imu v1_0 (:432-438, :1026-1031), mes v1_0 (:201-213, :977-996),
    rlc v2_1..v2_3 (:249-286, amdgpu_rlc.c:281-492).  Sizes come from each
    sub-blob's own field, never from common.ucode_size_bytes.
    """
    c = common(b)
    ao = c["array_offset"]
    if name.startswith("psp_"):
        # TOC: LOAD_TOC payload (not a LOAD_IP_FW type).  TA: LOAD_TA payload.
        ft = 24 if name.endswith("toc") else 75
        return [(ft, ao, c["size_bytes"] - ao)]
    if name.startswith("sdma_"):
        return [
            (71, ao, u32(b, 36)),
            (72, u32(b, 48), u32(b, 52)),
        ]
    if name.endswith("_imu"):
        iram = u32(b, 32)
        return [
            (68, ao, iram),
            (69, ao + iram, u32(b, 40)),
        ]
    if "mes" in name:
        code, data = (81, 82) if name.endswith("mes1") else (33, 34)
        return [
            (code, u32(b, 40), u32(b, 36)),
            (data, u32(b, 52), u32(b, 48)),
        ]
    if name.endswith("_rlc"):
        vmin = c["version_minor"]
        out = []
        if vmin >= 1:
            out.append((20, u32(b, 136), u32(b, 132)))
            out.append((21, u32(b, 152), u32(b, 148)))
        if vmin >= 2:
            out.append((26, u32(b, 160), u32(b, 156)))
            out.append((48, u32(b, 168), u32(b, 164)))
        if vmin >= 3:
            out.append((25, u32(b, 184), u32(b, 180)))
            if u32(b, 116):
                out.append((22, u32(b, 120), u32(b, 116)))
            if u32(b, 196):
                out.append((7, u32(b, 200), u32(b, 196)))
        out.append((8, ao, c["ucode_size_bytes"]))
        return out
    # pfp / me / mec: one payload, "declared length − array offset".
    return [(None, ao, c["size_bytes"] - ao)]


INDEX_M7_ROW = re.compile(
    r"^\|\s*`(amdgpu/([^`]+)\.bin)`\s*\|\s*(\d+)\s*\|\s*`([0-9a-f]{64})`",
    re.M)


def index_sha256():
    """sha256 per bare blob name, straight out of evidence/src/INDEX.md M7.

    This is the registration A2 pinned; taking it from the INDEX (not from
    the file we just hashed) is what makes the T4 comparison meaningful.
    """
    idx = (ROOT / "../../../../evidence/src/INDEX.md").resolve()
    if not idx.is_file():
        return {}
    # group 2 = bare name, group 4 = sha256 (group 3 is the byte count)
    return {m.group(2): m.group(4)
            for m in INDEX_M7_ROW.finditer(idx.read_text())}


def build():
    rows = []
    for name in BLOBS:
        path = ARCHIVE / (name + ".bin")
        if not path.is_file():
            raise SystemExit("missing archived blob: %s" % path)
        b = path.read_bytes()
        c = common(b)
        segs = segs_of(name, b)
        head = b[:HEAD_BYTES]
        if "mes" in name:
            starts = (u32(b, 60), u32(b, 56), u32(b, 68), u32(b, 64))
        else:
            starts = (0, 0, 0, 0)
        rows.append((name, len(b), c, segs, head, starts))

    out = []
    out.append("// fwhdr_golden.inc — T6 golden, generated by tools/gen-fwhdr-golden.py.")
    out.append("// Source: evidence/src/linux-firmware-bda6eccc/amdgpu/*.bin")
    out.append("// (linux-firmware main @ bda6eccc).  Field values decoded independently")
    out.append("// of the C++ parser under test.  DO NOT EDIT by hand: regenerate and")
    out.append("// commit, so the test stays runnable without the archive.")
    out.append("""
struct GoldenSeg {
    uint32_t fw_type;   // 0xFFFFFFFFu = "unset" (r3_fw_parse_common leaves 0)
    uint32_t offset;
    uint32_t size;
};

struct GoldenBlob {
    const char          *name;
    amdgpu::FwId         id;
    uint32_t             file_size;
    uint32_t             header_size_bytes;
    uint32_t             version_major;
    uint32_t             version_minor;
    uint32_t             ucode_version;
    uint32_t             ucode_size_bytes;
    uint32_t             array_offset;
    uint32_t             seg_count;
    GoldenSeg            segs[8];
    uint32_t             mes_uc_start_hi, mes_uc_start_lo;
    uint32_t             mes_data_start_hi, mes_data_start_lo;
    const uint8_t       *head256;   // real archived header bytes
    const char          *sha256;    // INDEX M7's digest, verbatim
};

static const uint8_t kHead256[][16][16] = {""")

    for name, _, _, _, head, _ in rows:
        assert len(head) == HEAD_BYTES
        # head256 is stored one element per 16-byte line, braced so the
        # 256-byte sub-array initialises without -Wmissing-braces.
        out.append("    {")
        out.append("        // %s" % name)
        for i in range(0, HEAD_BYTES, 16):
            out.append("        { " + "".join("0x%02x," % x for x in head[i:i + 16])
                       + " },")
        out.append("    },")
    out.append("};\n")

    id_of = {
        "psp_13_0_4_toc": "PSP13_TOC", "psp_13_0_4_ta": "PSP13_TA",
        "gc_11_0_1_imu": "GC11_IMU", "gc_11_0_1_rlc": "GC11_RLC",
        "gc_11_0_1_pfp": "GC11_PFP", "gc_11_0_1_me": "GC11_ME",
        "gc_11_0_1_mec": "GC11_MEC", "gc_11_0_1_mes_2": "GC11_MES_2",
        "gc_11_0_1_mes1": "GC11_MES1", "sdma_6_0_1": "SDMA6",
    }

    sha256s = index_sha256()
    out.append("static const GoldenBlob kGolden[] = {")
    for idx, (name, fsize, c, segs, _, starts) in enumerate(rows):
        uc_hi, uc_lo, d_hi, d_lo = starts
        segs_c = ", ".join(
            "{%s, %d, %d}" % ("0xFFFFFFFFu" if ft is None else str(ft), off, sz)
            for ft, off, sz in segs
        )
        out.append(
            '    { "%s", amdgpu::FwId::%s, %d, %d, %d, %d, %d, %d, %d, %d,\n'
            "      { %s },\n"
            '      0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, kHead256[%d][0],\n'
            '      "%s" },'
            % (name, id_of[name], fsize, c["header_size_bytes"],
               c["version_major"], c["version_minor"], c["ucode_version"],
               c["ucode_size_bytes"], c["array_offset"], len(segs),
               segs_c, uc_hi, uc_lo, d_hi, d_lo, idx, sha256s[name])
        )
    out.append("};\n")
    out.append("#define GOLDEN_COUNT (sizeof kGolden / sizeof kGolden[0])\n")

    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if the tracked .inc differs from a fresh build")
    args = ap.parse_args()

    if not ARCHIVE.is_dir():
        msg = "firmware archive not present (%s) — nothing to compare" % ARCHIVE
        if args.check:
            print("skipped: " + msg)
            return 0
        raise SystemExit(msg)

    text = build()
    if args.check:
        current = OUT.read_text() if OUT.is_file() else ""
        if current != text:
            print("FAIL: %s is stale — rerun tools/gen-fwhdr-golden.py" % OUT,
                  file=sys.stderr)
            return 1
        print("OK: %s matches the archived blobs" % OUT)
        return 0

    OUT.write_text(text)
    print("wrote %s (%d blobs, %d lines)" % (OUT, len(BLOBS), text.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
