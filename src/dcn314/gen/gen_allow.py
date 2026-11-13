#!/usr/bin/env python3
"""gen_allow.py - generate src/dcn314/dcn314_allow_ranges.h: the DCN 3.1.4 display write allowlist.

  python3 src/dcn314/gen/gen_allow.py            rewrite the header
  python3 src/dcn314/gen/gen_allow.py --check    fail if the header is not a fresh generation

dcn314 sibling of tools/dcn41/gen_allow.py (kept unchanged). Deny by default: this table is the only
thing that can make a write we originate legal. Offsets and BASE_IDX come from Linux v7.2's
dcn_3_1_4_offset.h. Segment bases are PLACEHOLDERS (see dcn314lib.PLACEHOLDER_SEGS): Phoenix's real DMU
bases are not measured yet, so every emitted range is tagged TODO(survey 回填) and must not be read as
this part's address space. The register offsets themselves are real.
"""
import re
import sys
import collections

import dcn314lib as L

OFFSETS = L.OFFSET_H
OUT = L.SRC / "dcn314_allow_ranges.h"
REGS_H = L.SRC / "dcn314_regs.h"

SEGS = L.PLACEHOLDER_SEGS
ALLOWED_BASE_IDX = (1, 2, 3)
MERGE_GAP = 0x40
BAR5_DWORDS = L.PLACEHOLDER_BAR5_DWORDS
EXCLUDE_FAMILY_PREFIXES = ("AZ",)

HARD_DENY = [
    (0x00000000, 0x000000ff,
     "the low BAR5 page: MM_INDEX/MM_DATA/MM_INDEX_HI (0x0/0x1/0x6) and NBIO's "
     "BIF_BX1_PCIE_INDEX2/DATA2 (0xe/0xf), RSMU_INDEX/DATA (0x14/0x15) and "
     "BIF_BX0_PCIE_INDEX/DATA (0x20/0x21) - indirect windows into all of SMN"),
]

# TODO(survey 回填): Phoenix MP1/SMU/PSP/MP0 segment bases are NOT measured. This synthetic block sits
# just above the placeholder display ranges and only exercises the BLOCK layer in tests; the survey must
# replace it with the real deny blocks from the DCN snapshot.
DENY_BLOCKS = [
    (L.PLACEHOLDER_DENY_LO, L.PLACEHOLDER_DENY_HI,
     "TODO(survey 回填): placeholder SMU/PSP-class block; replace with the real Phoenix segment bases"),
]


def parse_offsets():
    txt = OFFSETS.read_text()
    off, bidx = {}, {}
    for m in re.finditer(r'^#define\s+(reg\w+)\s+(0x[0-9a-fA-F]+)\s*$', txt, re.M):
        off[m.group(1)] = int(m.group(2), 16)
    for m in re.finditer(r'^#define\s+(reg\w+)_BASE_IDX\s+(\d+)\s*$', txt, re.M):
        bidx[m.group(1)] = int(m.group(2))
    if len(off) < 5000:
        raise SystemExit(f"gen_allow: only {len(off)} offsets parsed from {OFFSETS} - wrong file?")
    return off, bidx


def family(name):
    return name[3:].split("_")[0]



def build_ranges():
    off, bidx = parse_offsets()
    per_base = collections.defaultdict(dict)
    excluded_audio, excluded_window = [], []
    for name, v in off.items():
        b = bidx.get(name)
        if b is None or b not in ALLOWED_BASE_IDX:
            continue
        fam = family(name)
        if fam.startswith(EXCLUDE_FAMILY_PREFIXES):
            excluded_audio.append(name)
            continue
        if SEGS[b] + v >= BAR5_DWORDS:
            excluded_window.append(name)
            continue
        per_base[b][v] = fam
    ranges = []
    for b in sorted(per_base):
        items = sorted(per_base[b].items())
        run_lo, run_hi = items[0][0], items[0][0]
        fams = [items[0][1]]
        for v, fam in items[1:]:
            if v - run_hi <= MERGE_GAP:
                run_hi = v
                fams.append(fam)
            else:
                ranges.append((b, run_lo, run_hi, fams))
                run_lo, run_hi, fams = v, v, [fam]
        ranges.append((b, run_lo, run_hi, fams))
    out = []
    for b, lo, hi, fams in ranges:
        uniq = set(fams)
        name = fams[0] if len(uniq) == 1 else f"{fams[0]}..{fams[-1]}"
        out.append({"base": b, "off_lo": lo, "off_hi": hi,
                    "abs_lo": SEGS[b] + lo, "abs_hi": SEGS[b] + hi,
                    "name": name, "nfam": len(uniq), "nreg": len(fams)})
    out.sort(key=lambda r: r["abs_lo"])
    print(f"gen_allow: excluded {len(excluded_audio)} Azalia and {len(excluded_window)} out-of-BAR5 register(s)")
    return out


def audit(ranges):
    problems = []
    for i, r in enumerate(ranges):
        if r["abs_hi"] >= BAR5_DWORDS:
            problems.append(f"range {r['name']} ends at 0x{r['abs_hi']:x}, outside the BAR5 window")
        if i and r["abs_lo"] <= ranges[i - 1]["abs_hi"]:
            problems.append(f"range {r['name']} overlaps {ranges[i - 1]['name']}")
    for r in ranges:
        for lo, hi, why in HARD_DENY + DENY_BLOCKS:
            if r["abs_lo"] <= hi and lo <= r["abs_hi"]:
                problems.append(f"range {r['name']} 0x{r['abs_lo']:x}-0x{r['abs_hi']:x} intersects "
                                f"denied 0x{lo:x}-0x{hi:x} ({why.split(':')[0]})")
    coverage = 0
    if REGS_H.exists():
        txt = REGS_H.read_text()
        for m in re.finditer(r'^/\* ((?:reg\w+ 0x[0-9a-f]+(?:, )?)+); BASE_IDX (\d) \*/$', txt, re.M):
            b = int(m.group(2))
            for rm in re.finditer(r'reg\w+ (0x[0-9a-f]+)', m.group(1)):
                a = SEGS[b] + int(rm.group(1), 16)
                coverage += 1
                if not any(r["abs_lo"] <= a <= r["abs_hi"] for r in ranges):
                    problems.append(f"dcn314_regs.h register at absolute 0x{a:x} (BASE_IDX {b}) "
                                    f"is NOT inside any allowed range")
        if coverage < 100:
            problems.append(f"only {coverage} dcn314_regs.h registers checked - the parse is wrong")
    return problems, coverage


def render(ranges, coverage):
    total = sum(r["abs_hi"] - r["abs_lo"] + 1 for r in ranges)
    A = []
    a = A.append
    a("/* dcn314_allow_ranges.h - GENERATED by src/dcn314/gen/gen_allow.py; do not edit (regenerate, then --check).")
    a(" *")
    a(" * DCN 3.1.4 DISPLAY write allowlist as absolute BAR5 dword ranges. Absolute = seg[BASE_IDX] + offset.")
    a(" * TODO(survey 回填): the segment bases below are PLACEHOLDERS. Phoenix's real DMU bases are NOT measured;")
    a(" * the stage-1 survey's DCN snapshot must replace dcn314lib.PLACEHOLDER_SEGS and regenerate this file. Do")
    a(" * NOT treat these absolute addresses as this part's address space. The offsets/BASE_IDX are real (Linux")
    a(f" * {L.LINUX_VERSION} dcn_3_1_4_offset.h); offsets within one segment merge when <= {MERGE_GAP:#x} dwords apart.")
    a(" *")
    a(" * Segments 0 and 4 are DENIED outright (0: Azalia audio / indirect-window overlap; 4: unused, outside")
    a(" * BAR5). HARD_DENY is checked before the allow table, so a mis-generated range cannot leak an indirect")
    a(" * window. Every range carries TODO(survey 回填) until the placeholder bases are replaced.")
    a(" *")
    a(f" * {len(ranges)} ranges, {total} dwords ({total * 4} bytes) writable, out of a {BAR5_DWORDS}-dword BAR5.")
    a(f" * Coverage: all {coverage} register addresses src/dcn314/dcn314_regs.h can form fall inside a range.")
    a(" */")
    a("#ifndef N48_DCN314_ALLOW_RANGES_H")
    a("#define N48_DCN314_ALLOW_RANGES_H")
    a("")
    a("#include <stdint.h>")
    a("")
    a("struct dcn314_allow_range {")
    a("    uint32_t abs_lo;      /* first writable absolute BAR5 dword, inclusive */")
    a("    uint32_t abs_hi;      /* last writable absolute BAR5 dword, inclusive  */")
    a("    uint8_t base_idx;     /* DCN segment this range came from              */")
    a("    const char *name;     /* the hardware block(s) it covers               */")
    a("};")
    a("")
    a("struct dcn314_deny_range {")
    a("    uint32_t abs_lo;")
    a("    uint32_t abs_hi;")
    a("    const char *why;")
    a("};")
    a("")
    a(f"#define DCN314_ALLOW_RANGE_COUNT {len(ranges)}u")
    a(f"#define DCN314_ALLOW_TOTAL_DWORDS {total}u")
    a(f"#define DCN314_ALLOW_BAR5_DWORDS {BAR5_DWORDS}u")
    a("")
    a("/* Checked FIRST: indirect-access windows. A write here selects a target anywhere in SMN/VRAM. */")
    a(f"#define DCN314_HARD_DENY_COUNT {len(HARD_DENY)}u")
    a("static const struct dcn314_deny_range dcn314_hard_deny[DCN314_HARD_DENY_COUNT] = {")
    for lo, hi, why in HARD_DENY:
        a(f'    {{ {lo:#010x}u, {hi:#010x}u, "{why}" }},')
    a("};")
    a("")
    a("/* Checked SECOND; none intersects an allowed range (gen_allow.py asserts it). */")
    a(f"#define DCN314_DENY_BLOCK_COUNT {len(DENY_BLOCKS)}u")
    a("static const struct dcn314_deny_range dcn314_deny_blocks[DCN314_DENY_BLOCK_COUNT] = {")
    for lo, hi, why in DENY_BLOCKS:
        a(f'    {{ {lo:#010x}u, {hi:#010x}u, "{why}" }},')
    a("};")
    a("")
    a("/* Checked LAST; only these make a write legal. Sorted by abs_lo, disjoint. TODO(survey 回填) each. */")
    a("static const struct dcn314_allow_range dcn314_allow_ranges[DCN314_ALLOW_RANGE_COUNT] = {")
    for r in ranges:
        a(f'    {{ {r["abs_lo"]:#010x}u, {r["abs_hi"]:#010x}u, {r["base"]}u, "{r["name"]}" }},'
          f'  /* TODO(survey 回填): seg{r["base"]} + {r["off_lo"]:#07x}..{r["off_hi"]:#07x}, {r["nreg"]} regs */')
    a("};")
    a("")
    a("/* The segment bases this table was generated for. dcn314_allow_init refuses any other set. */")
    a("static const uint32_t dcn314_allow_expected_segs[5] = {")
    a("    " + ", ".join(f"{s:#010x}u" for s in SEGS))
    a("};")
    a("")
    a("#endif /* N48_DCN314_ALLOW_RANGES_H */")
    return "\n".join(A) + "\n"


def main():
    check = "--check" in sys.argv
    ranges = build_ranges()
    problems, coverage = audit(ranges)
    if problems:
        for p in problems:
            print("gen_allow: PROBLEM:", p)
        raise SystemExit(f"gen_allow: {len(problems)} problem(s) - refusing to write the allowlist")
    text = render(ranges, coverage)
    total = sum(r["abs_hi"] - r["abs_lo"] + 1 for r in ranges)
    if check:
        cur = OUT.read_text() if OUT.exists() else ""
        if cur != text:
            raise SystemExit(f"gen_allow --check: {OUT} is NOT a fresh generation")
        print(f"gen_allow --check: OK - {len(ranges)} ranges, {total} writable dwords, "
              f"{coverage} regs covered")
        return 0
    OUT.write_text(text)
    print(f"gen_allow: wrote {OUT} - {len(ranges)} ranges, {total} writable dwords, {coverage} regs covered")
    return 0


if __name__ == "__main__":
    sys.exit(main())