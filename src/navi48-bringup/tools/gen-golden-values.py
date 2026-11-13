#!/usr/bin/env python3
"""gen-golden-values.py - emit src/rdna3/golden_values.h (D-03).

The table is the single authoritative source for RDNA3 (Phoenix / 780M)
register coordinates and for the baselines the preflight gate and the host
tests compare against.

Sources, in the order the design requires (D-03 / H4):
  1. Linux asic_reg headers - the (offset, _BASE_IDX) pair of every register.
     The segment number is part of AMD's address protocol, so it is extracted
     here and never written by hand.
  2. machine-config.md / the nullarc dump - discovered segment bases, IP
     versions and the MMHUB framebuffer baseline (measured values).
  3. The firmware blob and PSP interface header - sizes, fw_type values and
     the PSP GFX command numbers read from the Linux enum.

The output is committed, so the build never depends on paths outside the
repository (same arrangement as tools/gen-fwhdr-golden.py).

Usage:
    src/navi48-bringup/tools/gen-golden-values.py
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent          # .../src/navi48-bringup/tools
ROOT = HERE.parent                              # .../src/navi48-bringup
REPO = ROOT.parent.parent.parent.parent         # .../780m-hackintosh
ASIC = REPO / "evidence/src/linux-7.2.0/drivers/gpu/drm/amd/include/asic_reg"
FW = REPO / "evidence/src/linux-firmware-bda6eccc/amdgpu"
OUT = ROOT / "src/rdna3/golden_values.h"

HEADERS = {
    "MP0": "mp/mp_13_0_4_offset.h",
    "MP1": "mp/mp_13_0_4_offset.h",
    "GC": "gc/gc_11_0_0_offset.h",
    "SDMA0": "gc/gc_11_0_0_offset.h",
    "MMHUB": "mmhub/mmhub_3_0_1_offset.h",
    "OSSSYS": "oss/osssys_6_0_0_offset.h",
    "NBIO": "nbio/nbio_7_7_0_offset.h",
}

# Some registers have a per-IP-version override in the Linux driver that is
# authoritative for this chip (GC 11.0.1) over the generic asic_reg header
# (GC 11.0.0). Value: (offset, _BASE_IDX, source note).
IP_OVERRIDES = {
    ("GC", "RLC_RLCS_BOOTLOAD_STATUS"): (
        0x4E7E, 1,
        "Linux gfx_v11_0.c:62 IP 版本覆盖（gc_11_0_1）；"
        "通用 gc_11_0_0 值 0x4E82 对 gfx1103 不适用",
    ),
}

# (block, Linux reg name without "reg", gate, mask, baseline, value source)
# gate is RecordOnly (dynamic; log only), NonZero (value gate: != 0) or
# Equals (value gate: (read & mask) == baseline).
REGS = [
    # ---- MP0: PSP mailbox (mp_13_0_4_offset.h) -------------------------------
    ("MP0", "MP0_SMN_C2PMSG_33", "RecordOnly", 0, 0,
     "33 bit31 is an observation only (requirement C1.3 v1.4)"),
    ("MP0", "MP0_SMN_C2PMSG_35", "RecordOnly", 0, 0,
     "bootloader-ready handshake (BootstrapRegs)"),
    ("MP0", "MP0_SMN_C2PMSG_36", "RecordOnly", 0, 0,
     "primary firmware MC address >> 20"),
    ("MP0", "MP0_SMN_C2PMSG_58", "RecordOnly", 0, 0,
     "PSP version register (C0.4/D3)"),
    ("MP0", "MP0_SMN_C2PMSG_64", "RecordOnly", 0, 0,
     "GFX command / response"),
    ("MP0", "MP0_SMN_C2PMSG_67", "RecordOnly", 0, 0,
     "ring write pointer"),
    ("MP0", "MP0_SMN_C2PMSG_69", "RecordOnly", 0, 0,
     "ring low address"),
    ("MP0", "MP0_SMN_C2PMSG_70", "RecordOnly", 0, 0,
     "ring high address"),
    ("MP0", "MP0_SMN_C2PMSG_71", "RecordOnly", 0, 0,
     "ring size"),
    ("MP0", "MP0_SMN_C2PMSG_81", "NonZero", 0, 0,
     "SOS alive, value gate (requirement C0.4/C1.1 + D-02)"),
    ("MP0", "MP0_SMN_C2PMSG_101", "RecordOnly", 0, 0,
     "ring destroy (SR-IOV)"),
    ("MP0", "MP0_SMN_C2PMSG_102", "RecordOnly", 0, 0,
     "SR-IOV ring low address"),
    ("MP0", "MP0_SMN_C2PMSG_103", "RecordOnly", 0, 0,
     "SR-IOV ring high address"),

    # ---- MP1: SMU mailbox (mp_13_0_4_offset.h) -------------------------------
    ("MP1", "MP1_SMN_C2PMSG_66", "RecordOnly", 0, 0,
     "SMU message id"),
    ("MP1", "MP1_SMN_C2PMSG_82", "RecordOnly", 0, 0,
     "SMU parameter / return value"),
    ("MP1", "MP1_SMN_C2PMSG_90", "RecordOnly", 0, 0,
     "SMU response"),

    # ---- GC (gc_11_0_0_offset.h) --------------------------------------------
    ("GC", "CP_MEC_CNTL", "RecordOnly", 0, 0,
     "MEC halt control, C-07 write target"),
    ("GC", "CP_ME_CNTL", "RecordOnly", 0, 0,
     "ME halt control, C-07 write target"),
    ("GC", "CP_MES_CNTL", "RecordOnly", 0, 0,
     "MES halt control"),
    ("GC", "CP_MES_PRGRM_CNTR_START", "RecordOnly", 0, 0,
     "MES program counter start"),
    ("GC", "CP_MES_PRGRM_CNTR_START_HI", "RecordOnly", 0, 0,
     "MES program counter start, high"),
    ("GC", "CP_MES_GP3_LO", "RecordOnly", 0, 0,
     "MES GP3 low"),
    ("GC", "RLC_CP_SCHEDULERS", "RecordOnly", 0, 0,
     "RLC/CP scheduler control"),
    ("GC", "CP_STAT", "RecordOnly", 0, 0,
     "CP busy status"),
    ("GC", "RLC_RLCS_BOOTLOAD_STATUS", "RecordOnly", 0, 0,
     "RLC autoload progress"),
    ("GC", "RLC_GPM_STAT", "RecordOnly", 0, 0,
     "RLC GPM status (recorded, never gated per C-06)"),
    ("GC", "GRBM_STATUS", "RecordOnly", 0, 0,
     "graphics block status"),
    ("GC", "GRBM_STATUS2", "RecordOnly", 0, 0,
     "graphics block status 2"),
    ("GC", "GB_ADDR_CONFIG", "RecordOnly", 0, 0,
     "GB address configuration"),
    ("GC", "CP_HQD_ACTIVE", "RecordOnly", 0, 0,
     "HQD active bitmap"),
    ("GC", "GRBM_GFX_INDEX", "RecordOnly", 0, 0,
     "GRBM instance selector"),
    ("GC", "GCVM_L2_PROTECTION_FAULT_STATUS", "RecordOnly", 0, 0,
     "GC L2 protection fault status (D-02 failure dump)"),
    ("GC", "GCVM_L2_PROTECTION_FAULT_ADDR_LO32", "RecordOnly", 0, 0,
     "GC L2 protection fault address, low (D-02 failure dump)"),
    ("GC", "GCVM_L2_PROTECTION_FAULT_ADDR_HI32", "RecordOnly", 0, 0,
     "GC L2 protection fault address, high (D-02 failure dump)"),
    ("GC", "GCMC_VM_FB_OFFSET", "RecordOnly", 0, 0,
     "Linux gfxhub_v3_0_get_mc_fb_offset source; compare with MMHUB offset (TMR system_phy)"),

    # ---- SDMA0: queue0 (gc_11_0_0_offset.h; no separate 6_0_1 offset header) -
    ("SDMA0", "SDMA0_QUEUE0_RB_CNTL", "RecordOnly", 0, 0,
     "queue0 ring control"),
    ("SDMA0", "SDMA0_QUEUE0_RB_BASE", "RecordOnly", 0, 0,
     "queue0 ring base, low"),
    ("SDMA0", "SDMA0_QUEUE0_RB_BASE_HI", "RecordOnly", 0, 0,
     "queue0 ring base, high"),
    ("SDMA0", "SDMA0_QUEUE0_RB_RPTR_ADDR_LO", "RecordOnly", 0, 0,
     "queue0 read-pointer writeback address, low"),
    ("SDMA0", "SDMA0_QUEUE0_RB_RPTR_ADDR_HI", "RecordOnly", 0, 0,
     "queue0 read-pointer writeback address, high"),
    ("SDMA0", "SDMA0_QUEUE0_IB_CNTL", "RecordOnly", 0, 0,
     "queue0 indirect buffer control"),
    ("SDMA0", "SDMA0_F32_CNTL", "RecordOnly", 0, 0,
     "F32 unhalt (C-08)"),
    ("SDMA0", "SDMA0_QUEUE0_MINOR_PTR_UPDATE", "RecordOnly", 0, 0,
     "minor pointer update window (C4.3)"),
    ("SDMA0", "SDMA0_QUEUE0_RB_WPTR", "RecordOnly", 0, 0,
     "queue0 write pointer"),
    ("SDMA0", "SDMA0_QUEUE0_DOORBELL", "RecordOnly", 0, 0,
     "queue0 doorbell (SDMA-04)"),
    ("SDMA0", "SDMA0_SEM_WAIT_FAIL_TIMER_CNTL", "RecordOnly", 0, 0,
     "semaphore wait fail timer (SDMA-04)"),
    ("SDMA0", "SDMA0_WATCHDOG_CNTL", "RecordOnly", 0, 0,
     "SDMA watchdog control (SDMA-04)"),
    ("SDMA0", "SDMA0_STATUS_REG", "RecordOnly", 0, 0,
     "SDMA status, cross evidence (S7)"),
    ("SDMA0", "SDMA0_UTCL1_CNTL", "RecordOnly", 0, 0,
     "UTCL1 control (C-12 record item)"),
    ("SDMA0", "SDMA0_UTCL1_PAGE", "RecordOnly", 0, 0,
     "UTCL1 page control (C-12 record item)"),

    # ---- MMHUB: framebuffer location (mmhub_3_0_1_offset.h) ------------------
    ("MMHUB", "MMMC_VM_FB_OFFSET", "RecordOnly", 0, 0,
     "framebuffer offset"),
    ("MMHUB", "MMMC_VM_FB_LOCATION_BASE", "Equals", 0x00FFFFFF, 0x00008000,
     "machine-config.md:51 fb_mc_base=0x8000000000; raw = fb_mc >> 24, "
     "FB_BASE mask 0x00FFFFFF (mmhub_3_0_1_sh_mask.h:7219-7220)"),
    ("MMHUB", "MMMC_VM_FB_LOCATION_TOP", "Equals", 0x00FFFFFF, 0x000080FF,
     "machine-config.md:51 fb_mc_top=0x80ffffffff; raw = fb_mc >> 24, "
     "FB_TOP mask 0x00FFFFFF (mmhub_3_0_1_sh_mask.h:7222-7223)"),

    # ---- OSSSYS: IH ring snapshot (osssys_6_0_0_offset.h; ih_v6_0.c:29) -------
    # Phoenix reports OSSSYS 6.0.x and Linux maps it to ih_v6_0, which includes
    # osssys_6_0_0 (RPTR/WPTR at 0x0083/0x0084; the 7.x layout is different).
    ("OSSSYS", "IH_RB_CNTL", "RecordOnly", 0, 0,
     "IH ring control (read-only snapshot)"),
    ("OSSSYS", "IH_RB_RPTR", "RecordOnly", 0, 0,
     "IH read pointer (read-only snapshot)"),
    ("OSSSYS", "IH_RB_WPTR", "RecordOnly", 0, 0,
     "IH write pointer (read-only snapshot)"),

    # ---- NBIO: SMN indirect window (nbio_7_7_0_offset.h) ---------------------
    ("NBIO", "BIF_BX0_PCIE_INDEX2", "RecordOnly", 0, 0,
     "SMN indirect index, group 0 (A-04)"),
    ("NBIO", "BIF_BX0_PCIE_DATA2", "RecordOnly", 0, 0,
     "SMN indirect data, group 0 (A-04)"),
]

# Discovered segment bases (dword) - machine-config.md section 10, plus the
# MMHUB row from the E13 cross-check / nullarc 780M dump (same values).
SEGMENTS = [
    ("MP0", 0, 0x00004000, "machine-config.md:125 (survey)"),
    ("MP0", 1, 0x00016000, "machine-config.md:125 (survey); nullarc PSP mailbox"),
    ("MP1", 0, 0x00004000, "machine-config.md:126 (survey, v1.1 erratum)"),
    ("MP1", 1, 0x00016000, "machine-config.md:126; nullarc SMU mailbox"),
    ("GC", 0, 0x00001260, "machine-config.md:123 (survey); nullarc PhxBringup.cpp:487"),
    ("GC", 1, 0x0000A000, "machine-config.md:123 (survey)"),
    ("SDMA0", 0, 0x00001260, "machine-config.md:127 (survey)"),
    ("SDMA0", 1, 0x0000A000, "machine-config.md:127 (survey)"),
    ("MMHUB", 0, 0x00013200, "tmp/tech/E13 (survey); nullarc ip_discovery.txt:26"),
    ("MMHUB", 1, 0x0001A000, "tmp/tech/E13 (survey); nullarc ip_discovery.txt:26"),
    ("NBIO", 0, 0x00000000, "machine-config.md:129 (survey; group 0 is legitimately 0)"),
    ("NBIO", 1, 0x00000014, "machine-config.md:129 (survey)"),
    ("OSSSYS", 0, 0x00011400, "machine-config.md:128 (survey; IH 6.0.1)"),
]

IP_VERSIONS = [
    ("GC", 11, 0, 1, "machine-config.md:123"),
    ("MMHUB", 3, 0, 1, "tmp/tech/E13; nullarc ip_discovery.txt:26"),
    ("MP0", 13, 0, 4, "machine-config.md:125"),
    ("MP1", 13, 0, 4, "machine-config.md:126"),
    ("SDMA0", 6, 0, 1, "machine-config.md:127"),
    ("NBIO", 7, 7, 0, "machine-config.md:129"),
]

SCALARS = [
    ("smuPmfwVersionRaw", 0x004C6500,
     "nullarc PMFW 76.101.0 (C-03 baseline)"),
    ("smuDriverIfVersion", 8,
     "nullarc driver-if 8 (requirement C0.4)"),
    ("mmhubFbMcBase", 0x8000000000,
     "machine-config.md:51 (survey D3)"),
    ("mmhubFbMcTop", 0x80FFFFFFFF,
     "machine-config.md:51 (survey D3)"),
]

SMN = [
    ("MP1_FIRMWARE_FLAGS", 0x03B10028,
     "smnMP1_V13_0_4_FIRMWARE_FLAGS byte address (smu_v13_0.c:237-259)"),
]

# C1.2 firmware registration set (17 items). Values are read from Linux's
# enum psp_gfx_fw_type so the table cannot drift from the header.
PSP_GFX_IF = REPO / "evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h"
FW_TYPES = [
    ("GFX_FW_TYPE_SDMA_UCODE_TH0", "C1.2 SDMA thread 0"),
    ("GFX_FW_TYPE_SDMA_UCODE_TH1", "C1.2 SDMA thread 1"),
    ("GFX_FW_TYPE_CP_PFP", "C1.2 legacy PFP"),
    ("GFX_FW_TYPE_CP_ME", "C1.2 legacy ME"),
    ("GFX_FW_TYPE_CP_MEC", "C1.2 legacy MEC"),
    ("GFX_FW_TYPE_CP_MES", "C1.2 MES code"),
    ("GFX_FW_TYPE_MES_STACK", "C1.2 MES stack"),
    ("GFX_FW_TYPE_CP_MES_KIQ", "C1.2 MES KIQ"),
    ("GFX_FW_TYPE_MES_KIQ_STACK", "C1.2 MES KIQ stack"),
    ("GFX_FW_TYPE_IMU_I", "C1.2 IMU instruction"),
    ("GFX_FW_TYPE_IMU_D", "C1.2 IMU data"),
    ("GFX_FW_TYPE_RLC_RESTORE_LIST_GPM_MEM", "C1.2 RLC GPM"),
    ("GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_MEM", "C1.2 RLC SRM"),
    ("GFX_FW_TYPE_RLC_IRAM", "C1.2 RLC IRAM"),
    ("GFX_FW_TYPE_RLC_DRAM_BOOT", "C1.2 RLC DRAM boot"),
    ("GFX_FW_TYPE_RLC_P", "C1.2 RLC P"),
    ("GFX_FW_TYPE_RLC_G", "C1.2 RLC G, loaded last"),
]

# Message numbers used by stage 2 (D-03 "消息号/fw_type（两源）"). Numbers
# that Linux defines in a header are parsed, never written by hand; the two
# SMU boot steps are sequence values from requirement C0.3 (no named macro).
PSP_MESSAGES = [
    ("pspGfxCmdLoadIpFw", "LOAD_IP_FW",
     "nullarc PhxNativeABI.h:101"),
    ("pspGfxCmdAutoloadRlc", "AUTOLOAD_RLC",
     "nullarc PhxNativeABI.h:102"),
]
SMU_BOOT_MESSAGES = [
    ("smuBootStep1", 0x03,
     "requirement C0.3 order 0x3 -> 0x2 -> EnableGfxImu; bringup5.sh:39"),
    ("smuBootStep2", 0x02,
     "requirement C0.3; bringup5.sh:40"),
]
SMU_PPSMC = REPO / "evidence/src/linux-7.2.0/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_4_ppsmc.h"

MEC_BLOB = "gc_11_0_1_mec.bin"


def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def parse_header(path, reg_name):
    """Return (offset, offset_line, base_idx, base_idx_line)."""
    text = path.read_text()
    m = re.search(r"^#define\s+reg" + re.escape(reg_name) + r"\s+(0x[0-9A-Fa-f]+|\d+)\s*$",
                  text, re.M)
    if not m:
        raise SystemExit("error: reg%s not found in %s" % (reg_name, path))
    off = int(m.group(1), 0)
    off_line = text.count("\n", 0, m.start()) + 1
    m = re.search(r"^#define\s+reg" + re.escape(reg_name) + r"_BASE_IDX\s+(\d+)\s*$",
                  text, re.M)
    if not m:
        raise SystemExit("error: reg%s_BASE_IDX not found in %s" % (reg_name, path))
    idx = int(m.group(1))
    idx_line = text.count("\n", 0, m.start()) + 1
    return off, off_line, idx, idx_line


def parse_enum(path, prefix):
    """Return {suffix: (value, line)} for `PREFIX<suffix> = <int>` entries."""
    text = path.read_text()
    out = {}
    pat = r"^[ \t]*" + re.escape(prefix) + r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)"
    for m in re.finditer(pat, text, re.M):
        out[m.group(1)] = (int(m.group(2), 0),
                           text.count("\n", 0, m.start()) + 1)
    return out


def parse_define(path, name):
    """Return (value, line) for `#define <name> <int>`."""
    text = path.read_text()
    m = re.search(r"^[ \t]*#define\s+" + re.escape(name) +
                  r"\s+(0x[0-9A-Fa-f]+|\d+)", text, re.M)
    if not m:
        raise SystemExit("error: %s not found in %s" % (name, path))
    return int(m.group(1), 0), text.count("\n", 0, m.start()) + 1


def camel(reg_name):
    parts = reg_name.lower().split("_")
    return parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])


def c_name(reg_name):
    body = camel(reg_name)
    return "k" + body[:1].upper() + body[1:]


def c_block(block):
    return "amdgpu::IPBlock::" + block


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def fw_mec():
    blob = (FW / MEC_BLOB).read_bytes()
    size_bytes = u32(blob, 0)
    ucode_size = u32(blob, 20)
    # gfx_firmware_header_v1_0: jt_size is the fourth u32 after the 32-byte
    # common header (offset 40). Linux subtracts jt_size * 4 from the ucode
    # size (amdgpu_ucode.c:909).
    jt_size = u32(blob, 40)
    payload = ucode_size - jt_size * 4
    if payload <= 0 or payload > ucode_size:
        raise SystemExit("error: implausible MEC payload %d" % payload)
    return size_bytes, ucode_size, jt_size, payload


def build():
    out = []
    add = out.append
    add("// GENERATED by tools/gen-golden-values.py - DO NOT EDIT BY HAND.")
    add("//")
    add("// RDNA3 (Phoenix / 780M) golden values: the single authoritative source for")
    add("// register coordinates, discovered segment bases, IP/PMFW versions, firmware")
    add("// types/messages and the MEC payload size used by stage 2 (D-03/H4/C0.8).")
    add("//")
    add("// Register offsets and segment numbers (_BASE_IDX) come from the pinned Linux")
    add("// asic_reg headers; segment bases and baselines come from machine-config.md /")
    add("// the nullarc 780M dump; blob sizes and fw_type/command numbers come from")
    add("// the blob header and psp_gfx_if.h. Never")
    add("// use an implementation constant as a test expectation (H4).")
    add("//")
    add("// Usage - RDNA3 production code resolves a register through the discovery")
    add("// table with IPBaseTable::resolve():")
    add("//")
    add("//     uint32_t addr;")
    add("//     if (!ctx.ip.resolve(r3gold::kMp0SmnC2pmsg81.ref(), &addr))")
    add("//         return fail_closed;")
    add("//")
    add("// The old get(block) / SOC15_REG_OFFSET() form implies segment 0 and MUST")
    add("// NOT be used for a register whose _BASE_IDX is not 0 (A-01).")
    add("")
    add("#pragma once")
    add("")
    add("#include <stdint.h>")
    add("")
    add('#include "amd/amdgpu_ip.h"')
    add("")
    add("namespace r3gold {")
    add("")
    add("enum class Gate : uint8_t {")
    add("    RecordOnly = 0,  // dynamic register: log the value, never gate")
    add("    NonZero    = 1,  // value gate: the reading must be != 0 (and != all-ones)")
    add("    Equals     = 2,  // value gate: (reading & mask) == baseline")
    add("};")
    add("")
    add("struct GoldenReg {")
    add("    const char *name;        // Linux reg name without the `reg` prefix")
    add("    amdgpu::IPBlock block;")
    add("    uint32_t offset;         // dword offset inside the block's segment")
    add("    uint8_t base_idx;        // _BASE_IDX from the Linux header")
    add("    Gate gate;")
    add("    uint32_t mask;           // Equals only")
    add("    uint32_t baseline;       // Equals only (raw register value)")
    add("    const char *source;      // offset/group lines + baseline provenance")
    add("")
    add("    constexpr amdgpu::RegRef ref() const {")
    add("        return amdgpu::RegRef{block, offset, base_idx};")
    add("    }")
    add("};")
    add("")

    for block, name, gate, mask, baseline, vsrc in REGS:
        override = IP_OVERRIDES.get((block, name))
        if override:
            off, idx, src = override
        else:
            off, off_line, idx, idx_line = parse_header(ASIC / HEADERS[block],
                                                        name)
            hdr = Path(HEADERS[block]).name
            src = "%s:%d/%d (offset/_BASE_IDX)" % (hdr, off_line, idx_line)
            if vsrc:
                src += "; " + vsrc
        add("constexpr GoldenReg %s = {" % c_name(name))
        add("    %s, %s, 0x%04Xu, %du," % (c_str(name), c_block(block), off, idx))
        add("    Gate::%s, 0x%08Xu, 0x%08Xu," % (gate, mask, baseline))
        add("    %s," % c_str(src))
        add("};")
        add("")

    add("constexpr const GoldenReg *const kRegs[] = {")
    for block, name, *_ in REGS:
        add("    &%s," % c_name(name))
    add("};")
    add("constexpr uint32_t kRegCount = sizeof(kRegs) / sizeof(kRegs[0]);")
    add("")

    add("// Discovered segment bases (BAR5 dword). The resolver adds the register")
    add("// offset to the slot selected by _BASE_IDX; base 0 (NBIO group 0) is real.")
    add("struct GoldenSegment {")
    add("    amdgpu::IPBlock block;")
    add("    uint8_t base_idx;")
    add("    uint32_t base;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenSegment kSegments[] = {")
    for block, idx, base, src in SEGMENTS:
        add("    {%s, %du, 0x%08Xu, %s}," % (c_block(block), idx, base, c_str(src)))
    add("};")
    add("constexpr uint32_t kSegmentCount = sizeof(kSegments) / sizeof(kSegments[0]);")
    add("")

    add("// IP versions observed on the target machine (major.minor.rev).")
    add("struct GoldenIpVersion {")
    add("    amdgpu::IPBlock block;")
    add("    uint8_t major, minor, rev;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenIpVersion kIpVersions[] = {")
    for block, maj, minor, rev, src in IP_VERSIONS:
        add("    {%s, %du, %du, %du, %s}," % (c_block(block), maj, minor, rev, c_str(src)))
    add("};")
    add("constexpr uint32_t kIpVersionCount =")
    add("    sizeof(kIpVersions) / sizeof(kIpVersions[0]);")
    add("")

    add("// Scalar baselines: PMFW/driver-if versions and the MMHUB framebuffer")
    add("// window in MC address form (the register baseline is the raw value above).")
    add("struct GoldenScalar {")
    add("    const char *name;")
    add("    uint64_t value;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenScalar kScalars[] = {")
    for name, value, src in SCALARS:
        add("    {%s, 0x%Xull, %s}," % (c_str(name), value, c_str(src)))
    add("};")
    add("constexpr uint32_t kScalarCount = sizeof(kScalars) / sizeof(kScalars[0]);")
    add("")

    add("// SMN-space registers: byte addresses, not segment-resolved (C0.8(2)).")
    add("struct GoldenSmn {")
    add("    const char *name;")
    add("    uint32_t byte_addr;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenSmn kSmn[] = {")
    for name, addr, src in SMN:
        add("    {%s, 0x%08Xu, %s}," % (c_str(name), addr, c_str(src)))
    add("};")
    add("constexpr uint32_t kSmnCount = sizeof(kSmn) / sizeof(kSmn[0]);")
    add("")

    fw_types = parse_enum(PSP_GFX_IF, "GFX_FW_TYPE_")
    add("// PSP firmware types registered by stage 2 (C1.2; values from the")
    add("// Linux enum psp_gfx_fw_type). MEC jump tables are skipped in autoload")
    add("// mode; RLC 22/7 are not registered because the archived header size is 0.")
    add("struct GoldenFwType {")
    add("    const char *name;")
    add("    uint32_t type;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenFwType kFwTypes[] = {")
    for enum_name, note in FW_TYPES:
        suffix = enum_name[len("GFX_FW_TYPE_"):]
        if suffix not in fw_types:
            raise SystemExit("error: %s missing from psp_gfx_if.h" % enum_name)
        value, line = fw_types[suffix]
        add("    {%s, %du, %s}," % (c_str(suffix), value,
                                   c_str("psp_gfx_if.h:%d (enum); %s" % (line, note))))
    add("};")
    add("constexpr uint32_t kFwTypeCount =")
    add("    sizeof(kFwTypes) / sizeof(kFwTypes[0]);")
    add("")

    add("// Message numbers (PSP GFX commands and the SMU GFX bring-up sequence).")
    add("struct GoldenMessage {")
    add("    const char *name;")
    add("    uint32_t value;")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenMessage kMessages[] = {")
    psp_cmds = parse_enum(PSP_GFX_IF, "GFX_CMD_ID_")
    for name, suffix, note in PSP_MESSAGES:
        if suffix not in psp_cmds:
            raise SystemExit("error: GFX_CMD_ID_%s missing from psp_gfx_if.h" % suffix)
        value, line = psp_cmds[suffix]
        add("    {%s, 0x%02Xu, %s}," % (
            c_str(name), value,
            c_str("psp_gfx_if.h:%d GFX_CMD_ID_%s; %s" % (line, suffix, note))))
    for name, value, src in SMU_BOOT_MESSAGES:
        add("    {%s, 0x%02Xu, %s}," % (c_str(name), value, c_str(src)))
    smu_value, smu_line = parse_define(SMU_PPSMC, "PPSMC_MSG_EnableGfxImu")
    add("    {%s, 0x%02Xu, %s}," % (
        c_str("smuEnableGfxImu"), smu_value,
        c_str("smu_v13_0_4_ppsmc.h:%d PPSMC_MSG_EnableGfxImu; bringup5.sh:41" % smu_line)))
    add("};")
    add("constexpr uint32_t kMessageCount =")
    add("    sizeof(kMessages) / sizeof(kMessages[0]);")
    add("")

    size_bytes, ucode_size, jt_size, payload = fw_mec()
    add("// Firmware sizes read from the blob header by od/python (od -An -tu4).")
    add("// payload = ucode_size_bytes - jt_size * 4 (amdgpu_ucode.c:909).")
    add("struct GoldenFirmware {")
    add("    const char *name;")
    add("    uint32_t file_size_bytes;   // common header size_bytes")
    add("    uint32_t ucode_size_bytes;")
    add("    uint32_t jt_size_dwords;")
    add("    uint32_t payload_bytes;     // what PSP LOAD_IP_FW ships")
    add("    const char *source;")
    add("};")
    add("constexpr GoldenFirmware kFirmware[] = {")
    add("    {%s, %du, %du, %du, %du," % (c_str(MEC_BLOB), size_bytes, ucode_size,
                                        jt_size, payload))
    add("     %s}," % c_str("blob header od (size@0, ucode_size@20, jt_size@40); "
                         "INDEX.md M7; nullarc bringup5.sh:21"))
    add("};")
    add("constexpr uint32_t kFirmwareCount =")
    add("    sizeof(kFirmware) / sizeof(kFirmware[0]);")
    add("")
    add("} // namespace r3gold")
    add("")
    return "\n".join(out)


def main():
    text = build()
    OUT.write_text(text)
    print("wrote %s (%d bytes, %d registers)" % (OUT, len(text), len(REGS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
