#!/usr/bin/env python3
"""gen_regs.py - generate src/dcn314/dcn314_regs.h from Linux v7.2 DCN 3.1.4 headers.

  python3 src/dcn314/gen/gen_regs.py            write dcn314_regs.h
  python3 src/dcn314/gen/gen_regs.py --check    fail if the header is stale

dcn314 sibling of tools/dcn41/gen_regs.py (kept unchanged). Every offset is emitted per instance
straight from the header; every field mask/shift from the instance-0 name. The generator refuses
when an instance is missing, instances disagree on BASE_IDX, or their mask/shift differs.
"""
import re
import sys

import dcn314lib as L

OUT = L.SRC / "dcn314_regs.h"

SPEC = [
    # ---- OTG: timing readout, scanout position, frame counter, sync status (survey D6/D8) ----
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_CONTROL", ["OTG_MASTER_EN"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_H_TOTAL", ["OTG_H_TOTAL"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_V_TOTAL", ["OTG_V_TOTAL"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_H_BLANK_START_END", ["OTG_H_BLANK_START", "OTG_H_BLANK_END"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_V_BLANK_START_END", ["OTG_V_BLANK_START", "OTG_V_BLANK_END"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_STATUS_POSITION", ["OTG_VERT_COUNT", "OTG_HORZ_COUNT"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_NOM_VERT_POSITION", ["OTG_VERT_COUNT_NOM"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_STATUS_FRAME_COUNT", ["OTG_FRAME_COUNT"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_GLOBAL_SYNC_STATUS",
     ["VSTARTUP_INT_EN", "VSTARTUP_EVENT_OCCURRED", "VSTARTUP_INT_STATUS",
      "VUPDATE_NO_LOCK_INT_EN", "VUPDATE_NO_LOCK_EVENT_OCCURRED", "VUPDATE_NO_LOCK_INT_STATUS"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_MASTER_UPDATE_LOCK", ["OTG_MASTER_UPDATE_LOCK", "UPDATE_LOCK_STATUS"]),
    ("OTG", "OTG{i}_", [0, 1, 2, 3], "OTG_GLOBAL_CONTROL2", ["OTG_MASTER_UPDATE_LOCK_SEL"]),

# (group, instance prefix pattern (uses {i}) or explicit list of instances, register, fields)
# The register name is reg<prefix><reg>; for explicit letter instances the prefix already holds it.
    # ---- HUBP / HUBPREQ: plane surface address, format, tiling, viewport (survey D6) ----
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCHUBP_CNTL", ["HUBP_IN_BLANK"]),
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCSURF_SURFACE_CONFIG", ["SURFACE_PIXEL_FORMAT"]),
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCSURF_TILING_CONFIG",
     ["SW_MODE", "DIM_TYPE", "META_LINEAR", "PIPE_ALIGNED"]),
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCSURF_ADDR_CONFIG",
     ["NUM_PIPES", "PIPE_INTERLEAVE", "MAX_COMPRESSED_FRAGS", "NUM_PKRS"]),
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCSURF_PRI_VIEWPORT_START",
     ["PRI_VIEWPORT_X_START", "PRI_VIEWPORT_Y_START"]),
    ("HUBP", "HUBP{i}_", [0, 1, 2, 3], "DCSURF_PRI_VIEWPORT_DIMENSION",
     ["PRI_VIEWPORT_WIDTH", "PRI_VIEWPORT_HEIGHT"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "DCSURF_PRIMARY_SURFACE_ADDRESS", ["PRIMARY_SURFACE_ADDRESS"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH",
     ["PRIMARY_SURFACE_ADDRESS_HIGH"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "DCSURF_SURFACE_CONTROL",
     ["PRIMARY_SURFACE_TMZ", "PRIMARY_SURFACE_DCC_EN"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "DCSURF_SURFACE_EARLIEST_INUSE",
     ["SURFACE_EARLIEST_INUSE_ADDRESS"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "DCSURF_SURFACE_EARLIEST_INUSE_HIGH",
     ["SURFACE_EARLIEST_INUSE_ADDRESS_HIGH"]),
    ("HUBPREQ", "HUBPREQ{i}_", [0, 1, 2, 3], "VMID_SETTINGS_0", ["VMID"]),
    # ---- AUX: the only registers the minimal read transaction may write (gated by the caller) ----
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_CONTROL", ["AUX_EN", "AUX_RESET", "AUX_RESET_DONE"]),
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_SW_CONTROL",
     ["AUX_SW_GO", "AUX_SW_WR_BYTES", "AUX_SW_START_DELAY"]),
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_ARB_CONTROL",
     ["AUX_SW_USE_AUX_REG_REQ", "AUX_REG_RW_CNTL_STATUS", "AUX_SW_DONE_USING_AUX_REG"]),
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_INTERRUPT_CONTROL", ["AUX_SW_DONE_ACK"]),
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_SW_STATUS",
     ["AUX_SW_DONE", "AUX_SW_REPLY_BYTE_COUNT", "AUX_SW_HPD_DISCON", "AUX_SW_RX_TIMEOUT",
      "AUX_SW_RX_TIMEOUT_STATE", "AUX_SW_RX_INVALID_STOP", "AUX_SW_RX_RECV_NO_DET",
      "AUX_SW_RX_RECV_INVALID_H", "AUX_SW_RX_RECV_INVALID_L"]),
    ("AUX", "DP_AUX{i}_", [0, 1, 2, 3, 4], "AUX_SW_DATA",
     ["AUX_SW_DATA_RW", "AUX_SW_DATA", "AUX_SW_INDEX", "AUX_SW_AUTOINCREMENT_DISABLE"]),
    # ---- DIG / DP: link enable, front/back-end clock, DP link status (survey D7) ----
    ("DIG", "DIG{i}_", [0, 1, 2, 3, 4], "DIG_FE_CNTL", ["DIG_SOURCE_SELECT", "DIG_SYMCLK_FE_ON"]),
    ("DIG", "DIG{i}_", [0, 1, 2, 3, 4], "DIG_BE_EN_CNTL", ["DIG_ENABLE", "DIG_SYMCLK_BE_ON"]),
    ("DP", "DP{i}_", [0, 1, 2, 3, 4], "DP_LINK_CNTL", ["DP_LINK_STATUS", "DP_EMBEDDED_PANEL_MODE"]),
    ("DP", "DP{i}_", [0, 1, 2, 3, 4], "DP_DPHY_CNTL", ["DPHY_FEC_EN", "DPHY_FEC_ACTIVE_STATUS"]),
    # ---- PWRSEQ: DIO PWM state (current brightness / period / reference divider) (survey D7) ----
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "BL_PWM_CNTL",
     ["BL_PWM_EN", "BL_ACTIVE_INT_FRAC_CNT", "BL_PWM_FRACTIONAL_EN"]),
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "BL_PWM_PERIOD_CNTL", ["BL_PWM_PERIOD", "BL_PWM_PERIOD_BITCNT"]),
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "PANEL_PWRSEQ_REF_DIV1", ["BL_PWM_REF_DIV"]),
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "PANEL_PWRSEQ_REF_DIV2",
     ["XTAL_REF_DIV", "MICROSECOND_TIME_BASE_DIV"]),
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "PANEL_PWRSEQ_CNTL",
     ["PANEL_PWRSEQ_TARGET_STATE", "PANEL_DIGON", "PANEL_BLON"]),
    ("PWRSEQ", "PWRSEQ{i}_", [0, 1], "PANEL_PWRSEQ_STATE",
     ["PANEL_PWRSEQ_TARGET_STATE_R", "PANEL_PWRSEQ_DIGON", "PANEL_PWRSEQ_BLON",
      "PANEL_PWRSEQ_STATE", "PANEL_PWRSEQ_DONE"]),
]

DIO_LINKS = "ABCDEF"   # regDIO_LINKA_CNTL .. regDIO_LINKF_CNTL (single-instance link encoders)

DEF = re.compile(r"^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)L?\s*$")


def parse(path):
    d = {}
    for line in path.read_text().splitlines():
        m = DEF.match(line.strip())
        if m:
            d[m.group(1)] = int(m.group(2), 0)
    return d


def emit_reg(out, off, msk, group, pat, instances, reg, fields):
    names = [f"reg{pat.format(i=i)}{reg}" for i in instances]
    vals, bases = [], set()
    for n in names:
        if n not in off or f"{n}_BASE_IDX" not in off:
            raise SystemExit(f"gen_regs: {n} or its _BASE_IDX missing: refusing")
        vals.append(off[n])
        bases.add(off[f"{n}_BASE_IDX"])
    if len(bases) != 1:
        raise SystemExit(f"gen_regs: {reg}: instances disagree on BASE_IDX {sorted(bases)}: refusing")
    base = bases.pop()
    cname = f"DCN314_{group}_{reg}"
    out.append("/* " + ", ".join(f"{n} 0x{v:04x}" for n, v in zip(names, vals)) + f"; BASE_IDX {base} */")
    chain = " : ".join(f"(i) == {i} ? 0x{v:04x}u" for i, v in zip(instances, vals))
    out.append(f"#define {cname}(i) ((unsigned)({chain} : DCN314_BAD_OFFSET))")
    out.append(f"#define {cname}_BASE_IDX {base}u")
    first = instances[0]
    for fld in fields:
        mname0 = f"{pat.format(i=first)}{reg}__{fld}"
        if f"{mname0}_MASK" not in msk or f"{mname0}__SHIFT" not in msk:
            raise SystemExit(f"gen_regs: {mname0}_MASK/__SHIFT missing: refusing")
        m0, s0 = msk[f"{mname0}_MASK"], msk[f"{mname0}__SHIFT"]
        for i in instances[1:]:
            mi = f"{pat.format(i=i)}{reg}__{fld}"
            if f"{mi}_MASK" in msk and (msk[f"{mi}_MASK"], msk[f"{mi}__SHIFT"]) != (m0, s0):
                raise SystemExit(f"gen_regs: {mi} differs from {mname0}: refusing")
        out.append(f"#define {cname}__{fld}_MASK 0x{m0:08x}u  /* {mname0}_MASK */")
        out.append(f"#define {cname}__{fld}__SHIFT {s0}u")
    out.append("")
    return base, max(vals)


def generate():
    L.need_linux()
    off, msk = parse(L.OFFSET_H), parse(L.SHMASK_H)
    out = [
        "/* dcn314_regs.h - GENERATED by src/dcn314/gen/gen_regs.py; do not edit (regenerate, then --check).",
        " *",
        f" * Source: Linux {L.LINUX_VERSION}, drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_4_offset.h and",
        " * dcn_3_1_4_sh_mask.h (MIT). Each offset is the header's regNAME value for that instance, relative to the",
        " * DCN segment named by its _BASE_IDX; absolute BAR5 dword = seg[BASE_IDX] + offset. The seg[] bases are the",
        " * stage-1 survey's, NOT baked in here (see dcn314.h). Field masks/shifts are the instance-0 names; the",
        " * generator refused if any instance disagreed.",
        " */",
        "#ifndef N48_DCN314_REGS_H",
        "#define N48_DCN314_REGS_H",
        "",
        "#define DCN314_BAD_OFFSET 0xFFFFFFFFu",
        "",
    ]
    max_by_base = {}
    for group, pat, instances, reg, fields in SPEC:
        base, mx = emit_reg(out, off, msk, group, pat, instances, reg, fields)
        max_by_base[base] = max(max_by_base.get(base, 0), mx)
    # DIO link encoders: one non-instanced register per letter A..F.
    for letter in DIO_LINKS:
        name = f"regDIO_LINK{letter}_CNTL"
        if name not in off or f"{name}_BASE_IDX" not in off:
            raise SystemExit(f"gen_regs: {name} or its _BASE_IDX missing: refusing")
        base = off[f"{name}_BASE_IDX"]
        max_by_base[base] = max(max_by_base.get(base, 0), off[name])
        cname = f"DCN314_DIO_LINK{letter}_CNTL"
        out.append(f"/* {name} 0x{off[name]:04x}; BASE_IDX {base} */")
        out.append(f"#define {cname} 0x{off[name]:04x}u")
        out.append(f"#define {cname}_BASE_IDX {base}u")
        for fld in ("ENC_TYPE_SEL", "HPO_HDMI_ENC_SEL", "HPO_DP_ENC_SEL"):
            k = f"DIO_LINK{letter}_CNTL__{fld}"
            if f"{k}_MASK" not in msk or f"{k}__SHIFT" not in msk:
                raise SystemExit(f"gen_regs: {k}_MASK/__SHIFT missing: refusing")
            out.append(f"#define {cname}__{fld}_MASK 0x{msk[k + '_MASK']:08x}u  /* {k}_MASK */")
            out.append(f"#define {cname}__{fld}__SHIFT {msk[k + '__SHIFT']}u")
        out.append("")
    out.append("/* largest offset used per BASE_IDX: dcn314_dev_init() checks seg[b] + this is inside the BAR5 window */")
    for b in sorted(max_by_base):
        out.append(f"#define DCN314_MAX_OFFSET_BASE{b} 0x{max_by_base[b]:04x}u")
    out.append(f"#define DCN314_BASE_IDX_USED_MASK 0x{sum(1 << b for b in max_by_base):x}u")
    out += ["", "#endif /* N48_DCN314_REGS_H */", ""]
    return "\n".join(out)


def main():
    text = generate()
    if "--check" in sys.argv:
        cur = OUT.read_text() if OUT.exists() else ""
        if cur != text:
            print(f"gen_regs --check: {OUT} is STALE (regenerate)")
            return 1
        print(f"gen_regs --check: {OUT.name} matches a fresh generation ({text.count(chr(10))} lines)")
        return 0
    OUT.write_text(text)
    print(f"gen_regs: wrote {OUT} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
