#!/usr/bin/env python3
"""gen_csb_gfx11.py - emit src/amd/csb_gfx11_data.h (RLC clear-state buffer).

The CSIB table must match the Linux clear-state blob byte for byte.  It is
too large to keep aligned by hand (the previous table had drifted), so the
committed header is generated and a --check mode keeps it from drifting again.

Sources (the Linux driver is the only primary source; nullarc is not read):
  evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/clearstate_gfx11.h
      gfx11_SECT_CONTEXT_def_1..7[] + gfx11_SECT_CONTEXT_defs[]
  evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c:846-894
      packet order and the PA_SC_TILE_STEERING_OVERRIDE packet (offset 0xd7)
  evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c:2506-2564
      preamble start / data parser / preamble end emission format
  evidence/src/linux-7.2.0/drivers/gpu/drm/amd/include/asic_reg/gc/
      gc_11_0_0_offset.h - regPA_SC_TILE_STEERING_OVERRIDE offset

Usage (from src/navi48-bringup or anywhere):
    python3 tools/gen_csb_gfx11.py            # rewrite the header
    python3 tools/gen_csb_gfx11.py --check    # fail if the header is stale
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent          # .../src/navi48-bringup/tools
ROOT = HERE.parent                              # .../src/navi48-bringup
REPO = ROOT
while not (REPO / "evidence/src/linux-7.2.0").is_dir():
    if REPO.parent == REPO:
        raise SystemExit("evidence/src/linux-7.2.0 not found above " + str(ROOT))
    REPO = REPO.parent                          # .../780m-hackintosh

AMD = REPO / "evidence/src/linux-7.2.0/drivers/gpu/drm/amd"
CLEARSTATE = AMD / "amdgpu/clearstate_gfx11.h"
GFX_C = AMD / "amdgpu/gfx_v11_0.c"
GFX_COMMON_C = AMD / "amdgpu/amdgpu_gfx.c"
GC_OFFSET_H = AMD / "include/asic_reg/gc/gc_11_0_0_offset.h"
OUT = ROOT / "src/amd/csb_gfx11_data.h"

# PM4 encoding, from cikd.h: PACKET3(op, n) and the packet opcodes.
PACKET3 = lambda op, cnt: (3 << 30) | ((cnt & 0x3FFF) << 16) | (op << 8)
SET_CONTEXT_REG = 0x69
PREAMBLE_CNTL = 0x4A
CONTEXT_CONTROL = 0x28
CLEAR_STATE = 0x12
BEGIN_CLEAR_STATE = 0x20000000          # PACKET3_PREAMBLE_BEGIN_CLEAR_STATE
END_CLEAR_STATE = 0x30000000            # PACKET3_PREAMBLE_END_CLEAR_STATE
BASE = 0xA000                           # PACKET3_SET_CONTEXT_REG_START

TOTAL_DWORDS = 960
TILE_OFFSET = 0x00D7                    # regPA_SC_TILE_STEERING_OVERRIDE
TILE_VALUE = 0                          # gfx_v11_0.c:2183 (all GFX11)


def fail(msg):
    raise SystemExit("gen_csb_gfx11: " + msg)


def check(cond, msg):
    if not cond:
        fail(msg)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_array(text, name):
    """Return the integer initialisers of `static const ... name[] = {...};`."""
    m = re.search(r"\b" + re.escape(name) +
                  r"\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    check(m is not None, "array %s not found" % name)
    body = strip_comments(m.group(1))
    tokens = re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)
    check(bool(tokens), "array %s is empty" % name)
    return [int(tok, 0) for tok in tokens]


def parse_defs(text):
    """Return [(symbol, reg_index, reg_count)] in table order."""
    m = re.search(r"gfx11_SECT_CONTEXT_defs\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
                  text, re.S)
    check(m is not None, "gfx11_SECT_CONTEXT_defs not found")
    body = strip_comments(m.group(1))
    entries = re.findall(
        r"\{\s*(gfx11_SECT_CONTEXT_def_\d+)\s*,\s*"
        r"(0[xX][0-9a-fA-F]+|\d+)\s*,\s*(0[xX][0-9a-fA-F]+|\d+)\s*\}", body)
    check(bool(entries), "gfx11_SECT_CONTEXT_defs has no entries")
    return [(sym, int(idx, 0), int(cnt, 0)) for sym, idx, cnt in entries]


def assert_emission_sources(gfx_text, common_text, offset_text):
    """Cross-check the generator's packet layout against the Linux emitters.

    Eight structural assertions (A:902-994): the committed table is only
    correct if these two source files still describe the same sequence.
    """
    # 1. gfx_v11_0_get_csb_buffer emits one SET_CONTEXT_REG(1) after the
    #    parsed data, with the PA_SC_TILE_STEERING_OVERRIDE offset.
    check(re.search(r"ctx_reg_offset\s*=\s*SOC15_REG_OFFSET\(GC,\s*0,\s*"
                    r"regPA_SC_TILE_STEERING_OVERRIDE\)\s*-\s*"
                    r"PACKET3_SET_CONTEXT_REG_START;", gfx_text) is not None,
          "gfx_v11_0.c tile-steering offset expression changed")
    # 2. ... that packet carries exactly one register (count == 1).
    check("PACKET3(PACKET3_SET_CONTEXT_REG, 1)" in gfx_text,
          "gfx_v11_0.c no longer emits SET_CONTEXT_REG(1)")
    # 3. ... and the value written for GFX11 is zero.
    check("adev->gfx.config.pa_sc_tile_steering_override = 0;" in gfx_text,
          "gfx_v11_0.c tile-steering value is no longer 0")
    # 4. The register offset itself is 0x00d7 in the GC 11.0.0 offset table.
    check(re.search(r"#define\s+regPA_SC_TILE_STEERING_OVERRIDE\s+0x00d7\b",
                    offset_text) is not None,
          "gc_11_0_0_offset.h regPA_SC_TILE_STEERING_OVERRIDE is not 0x00d7")

    # 5. Preamble start, in order: PREAMBLE_CNTL(0), BEGIN_CLEAR_STATE,
    #    CONTEXT_CONTROL(1) with two 0x80000000 payload words.
    check(re.search(r"PACKET3\(PACKET3_PREAMBLE_CNTL, 0\).*?"
                    r"PACKET3_PREAMBLE_BEGIN_CLEAR_STATE.*?"
                    r"PACKET3\(PACKET3_CONTEXT_CONTROL, 1\)",
                    common_text, re.S) is not None,
          "amdgpu_gfx.c preamble start sequence changed")
    check(common_text.count("buffer[count++] = cpu_to_le32(0x80000000);") == 2,
          "amdgpu_gfx.c context-control payload changed")

    # 6. Data parser: SET_CONTEXT_REG(ext->reg_count) + relative index.
    check("PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count)" in common_text,
          "amdgpu_gfx.c parser packet header changed")
    check("ext->reg_index - PACKET3_SET_CONTEXT_REG_START" in common_text,
          "amdgpu_gfx.c parser relative index changed")

    # 7. Preamble end, in order: PREAMBLE_CNTL(0) then END_CLEAR_STATE.
    check(re.search(r"PACKET3\(PACKET3_PREAMBLE_CNTL, 0\).*?"
                    r"PACKET3_PREAMBLE_END_CLEAR_STATE",
                    common_text, re.S) is not None,
          "amdgpu_gfx.c preamble end sequence changed")
    # 8. Trailer, in order: CLEAR_STATE(0) with one zero payload word.
    check(re.search(r"PACKET3_PREAMBLE_END_CLEAR_STATE.*?"
                    r"PACKET3\(PACKET3_CLEAR_STATE, 0\).*?"
                    r"cpu_to_le32\(0\)", common_text, re.S) is not None,
          "amdgpu_gfx.c clear-state trailer changed")


def build():
    check(CLEARSTATE.is_file(), "missing source %s" % CLEARSTATE)
    check(GFX_C.is_file(), "missing source %s" % GFX_C)
    check(GFX_COMMON_C.is_file(), "missing source %s" % GFX_COMMON_C)
    check(GC_OFFSET_H.is_file(), "missing source %s" % GC_OFFSET_H)

    raw = CLEARSTATE.read_bytes()
    text = raw.decode("utf-8")
    digest = hashlib.sha256(raw).hexdigest()

    check(re.search(r"\{\s*gfx11_SECT_CONTEXT_defs\s*,\s*SECT_CONTEXT\s*\}",
                    text) is not None,
          "gfx11_cs_data no longer maps defs to SECT_CONTEXT")

    defs = parse_defs(text)
    check(len(defs) == 7, "expected 7 SECT_CONTEXT extents, got %d" % len(defs))
    check(all(cnt < 0x4000 for _, _, cnt in defs),
          "an extent count does not fit PACKET3's 14-bit field")

    assert_emission_sources(GFX_C.read_text(), GFX_COMMON_C.read_text(),
                            GC_OFFSET_H.read_text())

    out = [PACKET3(PREAMBLE_CNTL, 0), BEGIN_CLEAR_STATE]
    out += [PACKET3(CONTEXT_CONTROL, 1), 0x80000000, 0x80000000]
    for sym, index, count in defs:
        data = parse_array(text, sym)
        check(len(data) == count,
              "%s has %d values but the table says %d" % (sym, len(data), count))
        check(index >= BASE, "%s reg_index 0x%x is below 0x%x" % (sym, index, BASE))
        check(index - BASE < 0x4000,
              "%s reg_index 0x%x does not fit 14 bits" % (sym, index))
        out += [PACKET3(SET_CONTEXT_REG, count), index - BASE]
        out += data
    out += [PACKET3(SET_CONTEXT_REG, 1), TILE_OFFSET, TILE_VALUE]
    out += [PACKET3(PREAMBLE_CNTL, 0), END_CLEAR_STATE,
            PACKET3(CLEAR_STATE, 0), 0]

    check(len(out) == TOTAL_DWORDS,
          "generated %d dwords, expected %d" % (len(out), TOTAL_DWORDS))
    check(all(0 <= w <= 0xFFFFFFFF for w in out), "a dword does not fit 32 bits")
    data_total = sum(cnt for _, _, cnt in defs)
    check(data_total + 2 * len(defs) + 9 + 3 == TOTAL_DWORDS,
          "segment arithmetic does not add up to %d" % TOTAL_DWORDS)
    pos = 5
    for _, index, count in defs:
        w = out[pos]
        check((w >> 30) == 3 and ((w >> 8) & 0xFF) == SET_CONTEXT_REG and
              ((w >> 16) & 0x3FFF) == count and out[pos + 1] == index - BASE,
              "generated extent at %d does not match the defs table" % pos)
        pos += 2 + count
    check(pos == TOTAL_DWORDS - 7, "extent scan does not end at the tile packet")

    return out, defs, digest


def render(dwords, defs, digest):
    counts = "+".join(str(cnt) for _, _, cnt in defs)
    lines = [
        "/*",
        " * csb_gfx11_data.h - RLC clear-state buffer (CSIB) for GFX11, 960 dwords.",
        " *",
        " * GENERATED by tools/gen_csb_gfx11.py from the Linux amdgpu sources.",
        " * DO NOT EDIT; rerun the generator instead:",
        " *   python3 src/src/navi48-bringup/tools/gen_csb_gfx11.py",
        " *",
        " * Sources (Linux is the primary source; no second-hand copy is used):",
        " *   evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/clearstate_gfx11.h",
        " *     (sha256=%s)" % digest,
        " *     gfx11_SECT_CONTEXT_def_1..7 + gfx11_SECT_CONTEXT_defs",
        " *   evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c:846-894",
        " *     (packet order; PA_SC_TILE_STEERING_OVERRIDE at offset 0x00d7)",
        " *   evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c:2506-2564",
        " *     (preamble start / data parser / preamble end)",
        " *",
        " * The clearstate data is MIT (Copyright 2021 Advanced Micro Devices, Inc.;",
        " * see the license header in clearstate_gfx11.h). The GFX11 emission format",
        " * is used as a reference only, per NOTICE (no GPL code is copied).",
        " *",
        " * Segments: %s = %d data dwords; %d total." % (
            counts, sum(cnt for _, _, cnt in defs), len(dwords)),
        " * The RLC CSIB preamble is written to the D4 CSIB region and pointed to",
        " * via RLC_CSIB_ADDR_LO/HI/LENGTH (gfx_v11_0_init_csb, gfx_v11_0.c:2289-2291).",
        " */",
        "#ifndef NAVI48_CSB_GFX11_DATA_H",
        "#define NAVI48_CSB_GFX11_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        "static const uint32_t kCsbGfx11[%d] = {" % len(dwords),
    ]
    for i in range(0, len(dwords), 8):
        chunk = dwords[i:i + 8]
        lines.append("    " + " ".join("0x%08x," % w for w in chunk))
    lines += ["};", "", "#endif /* NAVI48_CSB_GFX11_DATA_H */", ""]
    return "\n".join(lines)


def main(argv=None):
    global CLEARSTATE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clearstate", type=Path, default=CLEARSTATE,
                    help="Linux clearstate_gfx11.h (primary source)")
    ap.add_argument("--out", type=Path, default=OUT,
                    help="header to write/check")
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed header differs from a fresh build")
    args = ap.parse_args(argv)

    CLEARSTATE = args.clearstate
    dwords, defs, digest = build()
    text = render(dwords, defs, digest)

    if args.check:
        if not args.out.is_file():
            fail("missing %s — run the generator" % args.out)
        current = args.out.read_text()
        if current == text:
            print("OK: %s matches Linux clearstate_gfx11.h" % args.out)
            return 0

        old = parse_array(current, "kCsbGfx11")
        # A short initialiser list is zero-filled up to the declared extent,
        # so pad it before comparing: the pre-regeneration header (960 slots,
        # 946 initialisers) then reports the audited 44 differing dwords.
        m = re.search(r"kCsbGfx11\s*\[\s*(\d+)\s*\]", current)
        declared = int(m.group(1)) if m else len(old)
        old = old[:declared] + [0] * (declared - len(old))
        diffs = []
        for i in range(max(len(old), len(dwords))):
            a = old[i] if i < len(old) else None
            b = dwords[i] if i < len(dwords) else None
            if a != b:
                diffs.append((i, a, b))
        print("FAIL: %s is stale against Linux clearstate_gfx11.h" % args.out,
              file=sys.stderr)
        print("  %d differing dwords (old -> new), first 10:" % len(diffs),
              file=sys.stderr)
        for i, a, b in diffs[:10]:
            print("  [%3d] %s -> %s" % (
                i,
                "missing" if a is None else "0x%08x" % a,
                "missing" if b is None else "0x%08x" % b), file=sys.stderr)
        if not diffs:
            print("  values match but the text differs (formatting/comments);"
                  " rerun the generator", file=sys.stderr)
        return 1

    args.out.write_text(text)
    print("wrote %s (%d dwords, source sha256=%s)" % (args.out, len(dwords), digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
