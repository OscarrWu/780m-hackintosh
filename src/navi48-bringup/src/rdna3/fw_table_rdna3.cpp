//  fw_table_rdna3.cpp — RDNA3 (gfx11.0.x / Phoenix, 780M) build of the
//  amdgpu::fw_get()/fw_find_by_name() interface declared in ../fw/fw_table.h.
//
//  Stage 2: ten RDNA3 blobs are embedded (A1) from firmware/*.bin via
//  tools/embed-firmware.py; every other row is a zero-length entry, and a
//  use site must fail closed on size == 0 (the stage-1 contract, kept).
//  Each embedded row also registers provenance (sha256 / linux-firmware
//  commit / source path) for the load evidence A4 demands.
//
//  No allocation, no STL, no exceptions/RTTI.  Same strategy as the base
//  src/amd/fw_table.cpp: plain zero-initialized globals (.bss), filled by
//  build_table() on first use.
//
//  Material (A2, INDEX M7): linux-firmware main @ bda6eccc,
//  evidence/src/linux-firmware-bda6eccc/amdgpu/.  The build re-checks every
//  blob's sha256 against INDEX M7 (Makefile check-firmware →
//  tools/verify-fw-sha256.py) and the host test T4 re-checks these rows, so a
//  drifted or substituted blob cannot reach the GPU unnoticed.
//
#include "fw_table.h"
#include "../rdna3/rdna3_fwhdr.h"

namespace amdgpu {

namespace {

constexpr uint32_t kCount = static_cast<uint32_t>(amdgpu::FwId::Count);

FwBlob g_table[kCount];
bool   g_built = false;

// ---- extern "C" symbols from generated src/fw/fw_*.c files ---------------
// The ten RDNA3 blobs of A1.  No RDNA4 blob symbol is referenced here: the
// RDNA3 target does not build them (B2 isolation), so those rows stay empty.
extern "C" {
extern const uint8_t fw_psp_13_0_4_toc[];
extern const size_t  fw_psp_13_0_4_toc_size;
extern const uint8_t fw_psp_13_0_4_ta[];
extern const size_t  fw_psp_13_0_4_ta_size;
extern const uint8_t fw_gc_11_0_1_imu[];
extern const size_t  fw_gc_11_0_1_imu_size;
extern const uint8_t fw_gc_11_0_1_rlc[];
extern const size_t  fw_gc_11_0_1_rlc_size;
extern const uint8_t fw_gc_11_0_1_pfp[];
extern const size_t  fw_gc_11_0_1_pfp_size;
extern const uint8_t fw_gc_11_0_1_me[];
extern const size_t  fw_gc_11_0_1_me_size;
extern const uint8_t fw_gc_11_0_1_mec[];
extern const size_t  fw_gc_11_0_1_mec_size;
extern const uint8_t fw_gc_11_0_1_mes_2[];
extern const size_t  fw_gc_11_0_1_mes_2_size;
extern const uint8_t fw_gc_11_0_1_mes1[];
extern const size_t  fw_gc_11_0_1_mes1_size;
extern const uint8_t fw_sdma_6_0_1[];
extern const size_t  fw_sdma_6_0_1_size;
}

void build_table() {
    if (g_built) return;

    // ---- RDNA3 (gfx11.0.x) rows: the ten embedded blobs -----------------
    g_table[static_cast<uint32_t>(FwId::PSP13_TOC)] =
        FwBlob{ fw_psp_13_0_4_toc, fw_psp_13_0_4_toc_size,
                "psp_13_0_4_toc", FwId::PSP13_TOC };
    g_table[static_cast<uint32_t>(FwId::PSP13_TA)] =
        FwBlob{ fw_psp_13_0_4_ta, fw_psp_13_0_4_ta_size,
                "psp_13_0_4_ta", FwId::PSP13_TA };
    g_table[static_cast<uint32_t>(FwId::GC11_IMU)] =
        FwBlob{ fw_gc_11_0_1_imu, fw_gc_11_0_1_imu_size,
                "gc_11_0_1_imu", FwId::GC11_IMU };
    g_table[static_cast<uint32_t>(FwId::GC11_RLC)] =
        FwBlob{ fw_gc_11_0_1_rlc, fw_gc_11_0_1_rlc_size,
                "gc_11_0_1_rlc", FwId::GC11_RLC };
    g_table[static_cast<uint32_t>(FwId::GC11_PFP)] =
        FwBlob{ fw_gc_11_0_1_pfp, fw_gc_11_0_1_pfp_size,
                "gc_11_0_1_pfp", FwId::GC11_PFP };
    g_table[static_cast<uint32_t>(FwId::GC11_ME)] =
        FwBlob{ fw_gc_11_0_1_me, fw_gc_11_0_1_me_size,
                "gc_11_0_1_me", FwId::GC11_ME };
    g_table[static_cast<uint32_t>(FwId::GC11_MEC)] =
        FwBlob{ fw_gc_11_0_1_mec, fw_gc_11_0_1_mec_size,
                "gc_11_0_1_mec", FwId::GC11_MEC };
    g_table[static_cast<uint32_t>(FwId::GC11_MES_2)] =
        FwBlob{ fw_gc_11_0_1_mes_2, fw_gc_11_0_1_mes_2_size,
                "gc_11_0_1_mes_2", FwId::GC11_MES_2 };
    g_table[static_cast<uint32_t>(FwId::GC11_MES1)] =
        FwBlob{ fw_gc_11_0_1_mes1, fw_gc_11_0_1_mes1_size,
                "gc_11_0_1_mes1", FwId::GC11_MES1 };
    g_table[static_cast<uint32_t>(FwId::SDMA6)] =
        FwBlob{ fw_sdma_6_0_1, fw_sdma_6_0_1_size,
                "sdma_6_0_1", FwId::SDMA6 };

    // ---- RDNA3 rows that stay zero-length -------------------------------
    // PSP_SOS: the SOS is resident platform firmware; the driver never loads
    //   it (C1 P3 checks C2PMSG_81 instead) and no such file exists.
    // GC11_UNI_MES: uni_mes packaging is GC 12.x only.
    // GC11_MES: mes_2.bin's fallback; registered for A3, not embedded.
    // SMU13: no smu_13_0_4.bin exists — PMFW is preloaded by platform
    //   firmware and the driver only uses the mailbox (A1).
    // VCN402 / DCN314_DMUB: stage 4 / stage 6 material (A3).
    g_table[static_cast<uint32_t>(FwId::PSP13_SOS)] =
        FwBlob{ nullptr, 0, "psp_13_0_4_sos", FwId::PSP13_SOS };
    g_table[static_cast<uint32_t>(FwId::GC11_UNI_MES)] =
        FwBlob{ nullptr, 0, "gc_11_0_1_uni_mes", FwId::GC11_UNI_MES };
    g_table[static_cast<uint32_t>(FwId::GC11_MES)] =
        FwBlob{ nullptr, 0, "gc_11_0_1_mes", FwId::GC11_MES };
    g_table[static_cast<uint32_t>(FwId::SMU13)] =
        FwBlob{ nullptr, 0, "smu_13_0_4", FwId::SMU13 };
    g_table[static_cast<uint32_t>(FwId::VCN402)] =
        FwBlob{ nullptr, 0, "vcn_4_0_2", FwId::VCN402 };
    g_table[static_cast<uint32_t>(FwId::DCN314_DMUB)] =
        FwBlob{ nullptr, 0, "dcn_3_1_4_dmcub", FwId::DCN314_DMUB };

    // ---- RDNA4 (gfx1201) rows: index-space only, always empty here -------
    // The RDNA3 kext builds no RDNA4 blob (B2), so these rows exist to keep
    // the FwId numbering shared with the base table and nothing more.
    g_table[static_cast<uint32_t>(FwId::PSP_SOS)] =
        FwBlob{ nullptr, 0, "psp_14_0_3_sos", FwId::PSP_SOS };
    g_table[static_cast<uint32_t>(FwId::PSP_TA)] =
        FwBlob{ nullptr, 0, "psp_14_0_3_ta", FwId::PSP_TA };
    g_table[static_cast<uint32_t>(FwId::GC_IMU)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_imu", FwId::GC_IMU };
    g_table[static_cast<uint32_t>(FwId::GC_PFP)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_pfp", FwId::GC_PFP };
    g_table[static_cast<uint32_t>(FwId::GC_ME)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_me", FwId::GC_ME };
    g_table[static_cast<uint32_t>(FwId::GC_MEC)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_mec", FwId::GC_MEC };
    g_table[static_cast<uint32_t>(FwId::GC_MES)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_mes", FwId::GC_MES };
    g_table[static_cast<uint32_t>(FwId::GC_MES1)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_mes1", FwId::GC_MES1 };
    g_table[static_cast<uint32_t>(FwId::GC_UNI_MES)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_uni_mes", FwId::GC_UNI_MES };
    g_table[static_cast<uint32_t>(FwId::GC_RLC)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_rlc", FwId::GC_RLC };
    g_table[static_cast<uint32_t>(FwId::GC_TOC)] =
        FwBlob{ nullptr, 0, "gc_12_0_1_toc", FwId::GC_TOC };
    g_table[static_cast<uint32_t>(FwId::SMU)] =
        FwBlob{ nullptr, 0, "smu_14_0_3", FwId::SMU };
    g_table[static_cast<uint32_t>(FwId::SDMA)] =
        FwBlob{ nullptr, 0, "sdma_7_0_1", FwId::SDMA };

    g_built = true;
}

// Manual equality check — same rationale as the base fw_table.cpp.
bool str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

} // anonymous namespace

const FwBlob *fw_get(FwId id) {
    build_table();
    const uint32_t idx = static_cast<uint32_t>(id);
    if (idx >= kCount) return nullptr;
    return &g_table[idx];
}

const FwBlob *fw_find_by_name(const char *name) {
    if (name == nullptr) return nullptr;
    build_table();
    for (uint32_t i = 0; i < kCount; ++i) {
        if (str_eq(g_table[i].name, name)) return &g_table[i];
    }
    return nullptr;
}

} // namespace amdgpu

/* ---------------------------------------------------------------------- */
/*  Provenance of the ten embedded blobs (A2/A4)                           */
/* ---------------------------------------------------------------------- */

namespace {

// sha256 and size as registered in evidence/src/INDEX.md §2.5 (M7); the
// build compares the files on disk against the same table, so a row here and
// a blob on disk can only disagree if someone edits one of them.
const R3FwProvenance kProvenance[] = {
    { amdgpu::FwId::PSP13_TOC, "psp_13_0_4_toc",
      "11e3529d748e2b6b204681183360a6ead80665b8deda996dfcf35973f789fc3d",
      "bda6eccc", "amdgpu/psp_13_0_4_toc.bin",   2560 },
    { amdgpu::FwId::PSP13_TA, "psp_13_0_4_ta",
      "f4a2687e1cd940b016e308bc87b9b840f845526d9ef21147416d89c1c72a7944",
      "bda6eccc", "amdgpu/psp_13_0_4_ta.bin",    254976 },
    { amdgpu::FwId::GC11_IMU, "gc_11_0_1_imu",
      "ac71f4eec713fc35b4a1fe27531e3eb04edd81eeac2cef64df01ac50d8510805",
      "bda6eccc", "amdgpu/gc_11_0_1_imu.bin",    132352 },
    { amdgpu::FwId::GC11_RLC, "gc_11_0_1_rlc",
      "5f07dc1f0a75ecd9cb56d805ea869184a50ed9e43d811ebf833b8906534650ef",
      "bda6eccc", "amdgpu/gc_11_0_1_rlc.bin",    157040 },
    { amdgpu::FwId::GC11_PFP, "gc_11_0_1_pfp",
      "449c72cae1c57838185197ad4003aeee7ccbdba892a28bb2624d9e85f57a2df1",
      "bda6eccc", "amdgpu/gc_11_0_1_pfp.bin",    263424 },
    { amdgpu::FwId::GC11_ME, "gc_11_0_1_me",
      "62c5e749ad848941e5d8b85f66eeb2b969ee82919724dfd94dbf6e354be8befc",
      "bda6eccc", "amdgpu/gc_11_0_1_me.bin",     263424 },
    { amdgpu::FwId::GC11_MEC, "gc_11_0_1_mec",
      "b0a4006b3934adc1d7996e6d901206dea37203b4839debe4a5ed4e393a615cbb",
      "bda6eccc", "amdgpu/gc_11_0_1_mec.bin",    268160 },
    { amdgpu::FwId::GC11_MES_2, "gc_11_0_1_mes_2",
      "962f56c7e661479e3cabf89c1fcd6344f10cfa8b8d1f672fc445bb21414c1392",
      "bda6eccc", "amdgpu/gc_11_0_1_mes_2.bin",  266976 },
    { amdgpu::FwId::GC11_MES1, "gc_11_0_1_mes1",
      "3a8b60ea88b7adbbc71da0ddee0370d5d65c1d66186fd4f4ba9d16a854b24db8",
      "bda6eccc", "amdgpu/gc_11_0_1_mes1.bin",   235600 },
    { amdgpu::FwId::SDMA6, "sdma_6_0_1",
      "708c2c2c45262c98ebe8e34e32c3f1ce8eb5b007bab560c9ea9b576a8e4d6768",
      "bda6eccc", "amdgpu/sdma_6_0_1.bin",       34560 },
};

} // anonymous namespace

const R3FwProvenance *r3_fw_provenance(amdgpu::FwId id) {
    const size_t n = sizeof kProvenance / sizeof kProvenance[0];
    for (size_t i = 0; i < n; ++i) {
        if (kProvenance[i].id == id) return &kProvenance[i];
    }
    return nullptr;
}

/* ---------------------------------------------------------------------- */
/*  17-piece PSP LOAD_IP_FW order (D2: Linux ucode_id ascending)           */
/* ---------------------------------------------------------------------- */

__attribute__((used))
const R3PspLoadEntry r3_psp_load_order[R3_PSP_LOAD_COUNT] = {
    { 71, amdgpu::FwId::SDMA6,      "sdma_th0"    },
    { 72, amdgpu::FwId::SDMA6,      "sdma_th1"    },
    {  2, amdgpu::FwId::GC11_PFP,   "pfp"         },
    {  1, amdgpu::FwId::GC11_ME,    "me"          },
    {  4, amdgpu::FwId::GC11_MEC,   "mec"         },
    { 33, amdgpu::FwId::GC11_MES_2, "mes_code"    },
    { 34, amdgpu::FwId::GC11_MES_2, "mes_data"    },
    { 81, amdgpu::FwId::GC11_MES1,  "mes1_code"   },
    { 82, amdgpu::FwId::GC11_MES1,  "mes1_data"   },
    { 68, amdgpu::FwId::GC11_IMU,   "imu_i"       },
    { 69, amdgpu::FwId::GC11_IMU,   "imu_d"       },
    { 20, amdgpu::FwId::GC11_RLC,   "rlc_gpm"     },
    { 21, amdgpu::FwId::GC11_RLC,   "rlc_srm"     },
    { 26, amdgpu::FwId::GC11_RLC,   "rlc_iram"    },
    { 48, amdgpu::FwId::GC11_RLC,   "rlc_dram"    },
    { 25, amdgpu::FwId::GC11_RLC,   "rlc_p"       },
    {  8, amdgpu::FwId::GC11_RLC,   "rlc_g"       },
};
