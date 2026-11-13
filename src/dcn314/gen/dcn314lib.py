#!/usr/bin/env python3
"""dcn314lib.py - shared paths and provenance for the DCN 3.1.4 generator copies (src/dcn314/gen/).

This is the dcn314 sibling of tools/dcn41/dcn41lib.py. It is a NEW file: the RDNA4 base generator
and its lib are left byte-for-byte untouched (D14: base scripts zero change), so the RDNA4 tables can
still be regenerated independently.

Provenance (D14): the register input is Linux v7.2's dcn_3_1_4_offset.h / dcn_3_1_4_sh_mask.h, the
archived material registered in evidence/src/INDEX.md section 2.1 (sha256 recorded there). The base
dcn41lib.py still says 238650ef6c7c because that is the commit its author used; this copy says v7.2,
the version this project actually builds from.
"""
import pathlib
import sys

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]                       # .../src (the fork repo root)
SRC = ROOT / "src/dcn314"
AMD = ROOT / "re/linux-dc/drivers/gpu/drm/amd"   # read-only symlink to evidence/src/linux-7.2.0
ASIC_DCN = AMD / "include/asic_reg/dcn"
OFFSET_H = ASIC_DCN / "dcn_3_1_4_offset.h"
SHMASK_H = ASIC_DCN / "dcn_3_1_4_sh_mask.h"

LINUX_VERSION = "v7.2"                       # provenance written into every generated header
# Placeholder DCN 3.1.4 segment bases. Phoenix's real DMU segment bases are NOT measured yet; the
# stage-1 survey's DCN snapshot must replace these. They are deliberately monotonic and obviously
# synthetic so the generated allowlist is structurally valid (sorted, disjoint, coverable) without
# pretending to be this part's address space. Every generated range is annotated TODO(survey).
PLACEHOLDER_SEGS = (0x00000000, 0x00001000, 0x00010000, 0x00020000, 0x00030000)
PLACEHOLDER_BAR5_DWORDS = 0x40000
PLACEHOLDER_DENY_LO = 0x34000          # synthetic, above the largest placeholder abs (0x33820)
PLACEHOLDER_DENY_HI = 0x34fff


def need_linux():
    if not OFFSET_H.exists():
        raise SystemExit(f"missing {OFFSET_H}: the read-only Linux v7.2 archive (re/linux-dc) is needed")
