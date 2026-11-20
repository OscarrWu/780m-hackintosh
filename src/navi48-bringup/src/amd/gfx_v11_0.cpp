//
//  gfx_v11_0.cpp - the GFX 11.0.1 readiness + constants-init substep (see gfx_v11_0.h for the
//  contract, the license header and the Linux provenance of every offset).
//
//  Section map (tech plan §3.3; requirement C3):
//    pre()   PSP ready + SMU ready (§8.1), a complete R3Io surface, GC BASE_IDX 0/1, the
//            teardown halt rows (W1) and the W1.5 write declarations.
//    seq()   GRBM_CNTL.READ_TIMEOUT=0xff and per-VMID SH_MEM_CONFIG/BASES (补遗-1/2), then a
//            per-VMID readback snapshot on the check channel (补遗-3, observation only).
//    chk()   S1 IMU version/status, S2 Linux RLC autoload readiness
//            (RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE + CP_STAT==0) with
//            RLC_STAT/RLC_GPM_STAT/GRBM_STATUS recorded as non-gating evidence,
//            S3 CP_ME_CNTL readback.
//    ev()    the S1-S3 readings into env->ev, the GFX ready bit and N48R3EVID-gfx-done.
//
//  Writes are limited to the declared W1.5 rows. The CP halt registers are only registered for
//  the teardown substep (W1), never written here; CP unhalt belongs to the MES substep (§3.5).
//
//  Kernel build constraints: -nostdinc, no libc, no exceptions/RTTI, no allocation. Only
//  <stdint.h>/<stddef.h> + libkern (memset/vsnprintf) + the RDNA3 headers.
//
#include "gfx_v11_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>   /* memset/vsnprintf, no libc under -nostdinc */
#else
#include <stdio.h>             /* host runner: same names from libc */
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "../rdna3/rdna3_runtime.h"
#include "../rdna3/golden_values.h"

namespace amdgpu {

namespace {

// Result names are static: R3SubstepResult::name outlives the substep (E1).
constexpr const char *kPreNoIo        = "pre:no-io";
constexpr const char *kPreNoEnv       = "pre:no-env";
constexpr const char *kPrePspNotReady = "pre:psp-not-ready";
constexpr const char *kPreSmuNotReady = "pre:smu-not-ready";
constexpr const char *kPreGcBase      = "pre:gc-base";

constexpr const char *kChkImuUnreadable = "chk:imu-unreadable";
constexpr const char *kChkImuNotRunning = "chk:imu-not-running";
constexpr const char *kChkAutoloadUnreadable = "chk:autoload-unreadable";
constexpr const char *kChkAutoloadTimeout    = "chk:autoload-timeout";
constexpr const char *kChkCpUnreadable  = "chk:cp-unreadable";

// Stage-2 golden-init result names and the deferred stage-3 sections.
constexpr const char *kSeqGoldenWrite      = "seq:golden-init";
constexpr const char *kStage3Vmid          = "stage3:vmid";
constexpr const char *kStage3Gds           = "stage3:gds";
constexpr const char *kStage3RlcSrm        = "stage3:rlc-srm";
constexpr const char *kStage3GcBase        = "stage3:gc-base";
constexpr const char *kStage3Register      = "stage3:register";
constexpr const char *kTagStage3           = "gfx-stage3";

// G16 / G13 / G14 / G17 registers (gc_11_0_0_offset.h; BASE_IDX lives in the tables below).
// CGTT_WD_CLK_CTRL has no gc_11_0_0 entry: Linux defines it in gfx_v11_0.c:60-61 (0x5086/1).
constexpr uint32_t kGfxGbAddrConfig       = 0x13deu;
constexpr uint32_t kGfxSpiGdbgPerVmidCntl = 0x1f72u;
constexpr uint32_t kGfxGdsVmid0Base       = 0x20a0u;
constexpr uint32_t kGfxGdsVmid0Size       = 0x20a1u;
constexpr uint32_t kGfxGdsGwsVmid0        = 0x20c0u;
constexpr uint32_t kGfxGdsOaVmid0         = 0x20d0u;
constexpr uint32_t kGfxRlcSrmCntl         = 0x4c80u;
constexpr uint32_t kGfxSpiGdbgTrapEn      = 0x00000008u; // TRAP_EN, shift 3
constexpr uint32_t kGfxRlcSrmAutoIncr     = 0x00000002u;
constexpr uint32_t kGfxRlcSrmEnable       = 0x00000001u;
constexpr uint32_t kGfxShMemLdsBases      = 0x00010002u; // LDS 1<<16 | scratch 2

constexpr const char *kTagPre = "gfx-pre";
constexpr const char *kTagSeq = "gfx-seq";
constexpr const char *kTagChk = "gfx-chk";

// §3.2 valid(): 0xFFFFFFFF is the read-path/BAR-failure sentinel, never a reading.
static inline bool gfx_valid(uint32_t v)
{
    return v != 0u && v != 0xFFFFFFFFu;
}

struct Gfx {
    const R3Io *io;
    const R3SubstepEnv *env;
    uint32_t gc0;   // GC BASE_IDX 0 (GRBM/CP_STAT)
    uint32_t gc1;   // GC BASE_IDX 1 (IMU/RLC/CP_ME_CNTL)

    // S1
    uint32_t imu_ver;
    uint32_t imu_rst;
    uint32_t imu_core;      // evidence only
    uint32_t imu_scratch10; // evidence only
    // S2: recorded for comparison only (never gates)
    uint32_t rlc_stat;
    uint32_t rlc_gpm;
    uint32_t rlc_f32;       // evidence only (K4 already read it)
    uint32_t grbm;
    uint32_t grbm2;
    // S2 readiness: BOOTLOAD_COMPLETE + CP_STAT==0
    uint32_t cp_stat;
    uint32_t bootload;
    uint32_t polls;
    // S3
    uint32_t cp_me_cntl;
};

void evf(R3EvidenceLines *ev, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void evf(R3EvidenceLines *ev, const char *tag, const char *fmt, ...)
{
    if (!ev)
        return;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r3_ev_add(ev, tag, buf);
}

uint32_t read_gc(const Gfx &g, int base_idx, uint32_t reg, bool *ok)
{
    const uint32_t base = (base_idx == 0) ? g.gc0 : g.gc1;
    return r3_io_read32(g.io, base + reg, ok);
}

// G01-G10 golden values: golden_settings_gc_11_0 (gfx_v11_0.c:319-321) and
// golden_settings_gc_11_0_1 (gfx_v11_0.c:323-334). Order: TCP_CNTL first, then 11.0.1.
struct GfxGoldenRow {
    uint8_t base_idx;
    uint32_t reg;
    uint32_t mask;
    uint32_t value;
};
static const GfxGoldenRow kGfxGoldenRows[] = {
    {1u, 0x19A2u, 0x20000000u, 0x20000000u}, // TCP_CNTL         gc_11_0_0_offset.h:6372-6373
    {1u, 0x5087u, 0x9fff8fffu, 0x00000010u}, // CGTT_GS_NGG...   :10618-10619
    {1u, 0x5086u, 0xffff8fffu, 0x00000010u}, // CGTT_WD_CLK...   gfx_v11_0.c:60-61
    {0u, 0x1F53u, 0x0007ffffu, 0x0000c200u}, // CPF_GCR_CNTL     :4538-4539
    {1u, 0x2E0Cu, 0xffff001bu, 0x00f01988u}, // GL2C_CTRL3       :8380-8381
    {0u, 0x1025u, 0xf0ffffffu, 0x00880007u}, // PA_CL_ENHANCE    :2098-2099
    {1u, 0x0944u, 0xfffffffdU, 0x00000008u}, // PA_SC_ENHANCE_3  :6220-6221
    {1u, 0x0960u, 0xfff891ffu, 0x55480100u}, // PA_SC_VRS...     :6270-6271
    {0u, 0x12E2u, 0xf7f7ffffu, 0x01030000u}, // TA_CNTL_AUX      :2296-2297
    {1u, 0x19A3u, 0xfcffffffu, 0x0000000au}, // TCP_CNTL2        :6374-6375
};
static_assert(sizeof(kGfxGoldenRows) / sizeof(kGfxGoldenRows[0]) == 10u,
              "G01-G10 golden table drift");

// Stage-2 declaration rows: four W1.5 rows (optional, idempotent) and ten golden rows
// (required, always written). The static_assert ties the table to the shared budget.
static const R3WriteRowReg kGfxStage2Rows[] = {
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 0u, 0x0DA0u, 1u,
     WClass::State, WNeed::Optional, "reg:gfx-w1.5-grbm"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x0900u, 1u,
     WClass::State, WNeed::Optional, "reg:gfx-w1.5-shmem"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x09E4u, 1u,
     WClass::State, WNeed::Optional, "reg:gfx-w1.5-shmem"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x09E3u, 1u,
     WClass::State, WNeed::Optional, "reg:gfx-w1.5-shmem"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x19A2u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x5087u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x5086u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 0u, 0x1F53u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x2E0Cu, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 0u, 0x1025u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x0944u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x0960u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 0u, 0x12E2u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
    {nullptr, static_cast<uint8_t>(amdgpu::IPBlock::GC), 1u, 0x19A3u, 1u,
     WClass::State, WNeed::Required, "reg:gfx-golden"},
};
static_assert(sizeof(kGfxStage2Rows) / sizeof(kGfxStage2Rows[0]) == kGfxStage2RowCount,
              "gfx stage-2 declaration table drift");


// ---------------------------------------------------------------------------------------
// PRE: dependency gates and address authority
// ---------------------------------------------------------------------------------------
R3SubstepResult pre(Gfx &g)
{
    R3SubstepResult r = r3_substep_pass();

    if (!r3_io_complete(g.io)) {
        r = r3_substep_fail(kR3SecPre, 0, kPreNoIo);
        evf(g.env ? g.env->ev : nullptr, kTagPre,
            "FAIL: I/O surface incomplete (mmio/smuio/delay/lock)");
        return r;
    }
    if (!g.env || !g.env->rt) {
        r = r3_substep_fail(kR3SecPre, 1, kPreNoEnv);
        evf(g.env ? g.env->ev : nullptr, kTagPre, "FAIL: no runtime block (ready bits)");
        return r;
    }

    // §8.1: GFX runs only after PSP and SMU are ready (the SMU EnableGfxImu(0x16,1) is
    // what powers the GFX core; nullarc E2 all-zero -> E3d alive).
    const uint32_t ready = g.env->rt->ready_bits;
    if ((ready & R3_IP_BIT(R3Ip::Psp)) == 0u) {
        r = r3_substep_fail(kR3SecPre, 2, kPrePspNotReady);
        evf(g.env->ev, kTagPre, "FAIL: PSP not ready (ready_bits=0x%08x)", ready);
        return r;
    }
    if ((ready & R3_IP_BIT(R3Ip::Smu)) == 0u) {
        r = r3_substep_fail(kR3SecPre, 3, kPreSmuNotReady);
        evf(g.env->ev, kTagPre, "FAIL: SMU not ready (ready_bits=0x%08x)", ready);
        return r;
    }

    // The GC base addresses are discovery facts, never constants (L8). Both SOC15 segments
    // are needed: IMU/RLC/CP_ME_CNTL are BASE_IDX 1, GRBM/CP_STAT are BASE_IDX 0.
    const amdgpu::IPBaseTable *ip = static_cast<const amdgpu::IPBaseTable *>(g.env->ip);
    if (!ip || !ip->isResolved(amdgpu::IPBlock::GC, 0) ||
        !ip->isResolved(amdgpu::IPBlock::GC, 1)) {
        r = r3_substep_fail(kR3SecPre, 4, kPreGcBase);
        evf(g.env->ev, kTagPre, "FAIL: GC BASE_IDX 0/1 unresolved");
        return r;
    }
    g.gc0 = ip->getBase(amdgpu::IPBlock::GC, 0);
    g.gc1 = ip->getBase(amdgpu::IPBlock::GC, 1);

    // Teardown-only halt rows (A#2): CP_ME_CNTL (ME/PFP halt) and CP_MEC_CNTL (MEC halt).
    // This substep never writes them; registering them under the Gfx owner is what lets the
    // teardown substep pass the write guard when it halts the CP (gfx_v11_0.c:3166-3185,
    // :3894-3908). A grant is an address+width+owner+class key, not a write record.
    static const r3gold::GoldenReg *const kGfxTd[] = {&r3gold::kCpMeCntl, &r3gold::kCpMecCntl};
    for (uint32_t i = 0; i < 2u; i++) {
        uint32_t halt = 0;
        if (!ip->resolve(kGfxTd[i]->ref(), &halt)) {
            r = r3_substep_fail(kR3SecPre, 5, kPreGcBase);
            evf(g.env->ev, kTagPre, "FAIL: teardown halt row %s unresolved",
                kGfxTd[i]->name);
            return r;
        }
        if (r3_wg_register_reg(halt, 1u, WClass::State, WSubstep::Gfx,
                               "reg:gfx-teardown") != 0) {
            r = r3_substep_fail(kR3SecPre, 6, "pre:gfx-halt-register");
            evf(g.env->ev, kTagPre, "FAIL: teardown halt grant %s rejected",
                kGfxTd[i]->name);
            return r;
        }
    }

    // Stage-2 declared rows (A:1151-1282; B §4.2): four W1.5 rows plus G01-G10 golden rows.
    // The declarations are the static address authority for the expect check; the grants are
    // what classify() matches at write time.
    if (r3_wg_expect_writes(kGfxStage2Rows, kGfxStage2RowCount, nullptr, 0u,
                            WSubstep::Gfx) != 0) {
        r = r3_substep_fail(kR3SecPre, 7, "pre:expect-cap");
        evf(g.env->ev, kTagPre, "FAIL: stage-2 rows rejected by the expect table");
        return r;
    }
    uint32_t row_addr[kGfxStage2RowCount];
    if (r3_wg_resolve_row_addrs(ip, kGfxStage2Rows, kGfxStage2RowCount, row_addr) !=
            static_cast<int>(kGfxStage2RowCount) ||
        r3_wg_expect_declared_addrs(row_addr, kGfxStage2RowCount) != 0) {
        r = r3_substep_fail(kR3SecPre, 8, kPreGcBase);
        evf(g.env->ev, kTagPre, "FAIL: stage-2 row addresses unresolved");
        return r;
    }
    for (uint32_t i = 0; i < kGfxStage2RowCount; i++) {
        if (r3_wg_register_reg(row_addr[i], 1u, WClass::State, WSubstep::Gfx,
                               kGfxStage2Rows[i].ctx) != 0) {
            r = r3_substep_fail(kR3SecPre, 9, "pre:gfx-stage2-register");
            evf(g.env->ev, kTagPre, "FAIL: stage-2 grant 0x%08x rejected", row_addr[i]);
            return r;
        }
    }

    evf(g.env->ev, kTagPre, "P0 gc0=0x%08x gc1=0x%08x ready=psp+smu (no write)",
        g.gc0, g.gc1);
    return r;
}

// ---------------------------------------------------------------------------------------
// SEQ: GRBM_CNTL.READ_TIMEOUT (补遗-1) + SH_MEM_CONFIG/BASES (补遗-2)
// ---------------------------------------------------------------------------------------

// Read-modify-write a GC register. The default skips the write when the masked value is
// unchanged (the audit's read-back gate); golden rows and Linux's unconditional writes pass
// force=true. ctx is the manifest label, never an authority key.
static bool gc_rmw(Gfx &g, uint8_t base_idx, uint32_t reg, uint32_t mask, uint32_t value,
                   const char *ctx = "gfx:w1.5", bool force = false)
{
    bool ok = false;
    const uint32_t abs = ((base_idx == 0u) ? g.gc0 : g.gc1) + reg;
    const uint32_t cur = r3_io_read32(g.io, abs, &ok);
    if (!ok)
        return false;
    const uint32_t next = (cur & ~mask) | (value & mask);
    if (!force && next == cur)
        return true;
    if (r3_wg_classify(abs, 1u, WClass::State, ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(g.io, abs, next);
}

static bool gc_write32(Gfx &g, uint8_t base_idx, uint32_t reg, uint32_t value,
                       const char *ctx = "gfx:w1.5")
{
    const uint32_t abs = ((base_idx == 0u) ? g.gc0 : g.gc1) + reg;
    if (r3_wg_classify(abs, 1u, WClass::State, ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(g.io, abs, value);
}

// gfx_v11_0_constants_init (gfx_v11_0.c:2172-2211): select each VMID through GRBM_GFX_CNTL
// (soc21_grbm_select(0,0,0,vmid) puts VMID at bit 4), then program SH_MEM_CONFIG and, for
// VMID != 0, SH_MEM_BASES. The loop bound is the GFXHUB num_ids (gmc_v11_0.c:857 sets
// first_kfd_vmid = 8; amdgpu_ids.c:594 makes that the GFXHUB count), i.e. VMID 0..7 — VMID
// 8..15 belong to init_compute_vmid (gfx_v11_0.c:2096-2124) and get LDS/scratch bases, not
// these apertures. DEFAULT_SH_MEM_CONFIG = 64-bit mode | unaligned | prefetch 3 (0x0000C00C);
// the bases are the GMC apertures >> 48 (private 0x1000, shared 0x2000 -> 0x20001000).
static bool seq_shmem(Gfx &g)
{
    static const uint32_t kGfxVmids = 8u;
    static const uint32_t kShMemConfig = 0x0000C00Cu;
    static const uint32_t kShMemBases = 0x20001000u;
    bool ok = true;
    for (uint32_t vmid = 0; ok && vmid < kGfxVmids; vmid++) {
        ok = gc_write32(g, 1u, 0x0900u, vmid << 4) &&
             gc_rmw(g, 1u, 0x09E4u, 0xFFFFFFFFu, kShMemConfig) &&
             (vmid == 0u || gc_rmw(g, 1u, 0x09E3u, 0xFFFFFFFFu, kShMemBases));
    }

    if (ok) {
        // 补遗-3: read the final state back for VMID 1..15 and log it on the check channel.
        // Observation only: a selector or read failure is logged, never gated, and no value
        // here feeds a decision. VMID 8..15 are deliberately not programmed above, so this
        // shows the untouched state for real-machine review.
        for (uint32_t vmid = 1u; vmid < 16u; vmid++) {
            const bool oksel = gc_write32(g, 1u, 0x0900u, vmid << 4);
            bool okc = false, okb = false;
            uint32_t cfg = 0, bases = 0;
            if (oksel) {
                cfg = read_gc(g, 1u, 0x09E4u, &okc);
                bases = read_gc(g, 1u, 0x09E3u, &okb);
            }
            evf(g.env->ev, kTagChk,
                "S0c shmem vmid%u config=0x%08x%s bases=0x%08x%s (readback, observe only)",
                vmid, cfg, oksel && okc ? "" : "(unreadable)",
                bases, oksel && okb ? "" : "(unreadable)");
        }
    }

    // Restore the selector on both paths: Linux only returns with VMID 0 selected, and a
    // failed step must not leave VMID-routed registers pointing at a stale VMID for the
    // following checks. Best effort — the step's outcome is already decided above.
    (void)gc_write32(g, 1u, 0x0900u, 0u);
    return ok;
}

// G01-G10: Linux applies both golden tables through its generic helper, which is an
// unconditional RMW (no read-back skip). The 11.0.1 table is applied on top of 11.0.
static bool seq_golden(Gfx &g)
{
    for (uint32_t i = 0; i < sizeof(kGfxGoldenRows) / sizeof(kGfxGoldenRows[0]); i++) {
        const GfxGoldenRow &row = kGfxGoldenRows[i];
        if (!gc_rmw(g, row.base_idx, row.reg, row.mask, row.value, "gfx:golden", true))
            return false;
    }
    evf(g.env->ev, kTagSeq, "S0c golden G01-G10 applied (10 rows, base_idx per table)");
    return true;
}

R3SubstepResult seq(Gfx &g)
{
    // 补遗-1: gfx_v11_0_constants_init raises GRBM_CNTL.READ_TIMEOUT to its 8-bit maximum
    // before touching the CP block; the reset default is too short for slow register reads.
    if (!gc_rmw(g, 0u, 0x0DA0u, 0xFFu, 0xFFu))
        return r3_substep_fail(kR3SecSeq, 0, "grbm-cntl-read-timeout");
    evf(g.env->ev, kTagSeq, "S0a GRBM_CNTL.READ_TIMEOUT=0xff (gc0+0x0DA0)");
    if (!seq_shmem(g))
        return r3_substep_fail(kR3SecSeq, 1, "sh-mem-init");
    evf(g.env->ev, kTagSeq, "S0b SH_MEM_CONFIG/BASES vmid0..7 (gc1+0x09E4/0x09E3)");
    if (!seq_golden(g))
        return r3_substep_fail(kR3SecSeq, 2, kSeqGoldenWrite);
    return r3_substep_pass();
}

// ---------------------------------------------------------------------------------------
// CHK: S1..S3
// ---------------------------------------------------------------------------------------
R3SubstepResult chk(Gfx &g)
{
    R3EvidenceLines *ev = g.env->ev;
    R3SubstepResult r;

    // S1: IMU version/status. The PSP load path does not call imu_v11_0_start, so accept
    // either Linux witness: the fw version register (gfx_v11_0.c:4996-4998) or the reset
    // status low 5 bits 0x1f (imu_v11_0.c:134-151). 0xFFFFFFFF is a read-path failure.
    bool okv = false, okr = false, okc = false, oks = false;
    g.imu_ver = read_gc(g, kGfx11ImuScratch0Base, kGfx11ImuScratch0, &okv);
    g.imu_rst = read_gc(g, kGfx11ImuGfxResetCtrlBase, kGfx11ImuGfxResetCtrl, &okr);
    g.imu_core = read_gc(g, kGfx11ImuCoreCtrlBase, kGfx11ImuCoreCtrl, &okc);
    g.imu_scratch10 = read_gc(g, kGfx11ImuScratch10Base, kGfx11ImuScratch10, &oks);
    evf(ev, kTagChk,
        "S1 IMU scratch0=0x%08x%s reset=0x%08x%s core=0x%08x scratch10=0x%08x",
        g.imu_ver, okv ? "" : "(unreadable)", g.imu_rst, okr ? "" : "(unreadable)",
        g.imu_core, g.imu_scratch10);
    if (!okv || !okr || g.imu_ver == 0xFFFFFFFFu || g.imu_rst == 0xFFFFFFFFu) {
        r = r3_substep_fail(kR3SecChk, 1, kChkImuUnreadable);
        r3_substep_note(&r, g.gc1 + kGfx11ImuScratch0, g.imu_ver, "IMU_SCRATCH_0");
        r3_substep_note(&r, g.gc1 + kGfx11ImuGfxResetCtrl, g.imu_rst, "IMU_GFX_RESET_CTRL");
        return r;
    }
    if (!gfx_valid(g.imu_ver) &&
        (g.imu_rst & kGfx11ImuResetDoneMask) != kGfx11ImuResetDone) {
        r = r3_substep_fail(kR3SecChk, 1, kChkImuNotRunning);
        r3_substep_note(&r, g.gc1 + kGfx11ImuScratch0, g.imu_ver, "IMU_SCRATCH_0");
        r3_substep_note(&r, g.gc1 + kGfx11ImuGfxResetCtrl, g.imu_rst, "IMU_GFX_RESET_CTRL");
        evf(ev, kTagChk, "FAIL: IMU not running (version 0, reset low5=0x%x)",
            g.imu_rst & kGfx11ImuResetDoneMask);
        return r;
    }
    bool okf = false;
    g.rlc_f32 = read_gc(g, kGfx11RlcF32UcodeVersionBase, kGfx11RlcF32UcodeVersion, &okf);
    evf(ev, kTagChk,
        "S1 PASS imu witness=%s rlc_f32 pre=n/a post=0x%08x%s "
        "(observation, not a gate; cross: psp-ready fw68/69)",
        gfx_valid(g.imu_ver) ? "version" : "reset-status",
        g.rlc_f32, okf ? "" : "(unreadable)");

    // S2 records: RLC_STAT / RLC_GPM_STAT / GRBM_STATUS / GRBM_STATUS2 are compared
    // against the nullarc readings, but they do not gate (C-06). A read fault is
    // recorded in the same line and does not fail the substep.
    bool ok_rlc_stat = false, okg = false, okb = false, okb2 = false;
    g.rlc_stat = read_gc(g, kGfx11RlcStatBase, kGfx11RlcStat, &ok_rlc_stat);
    g.rlc_gpm = read_gc(g, kGfx11RlcGpmStatBase, kGfx11RlcGpmStat, &okg);
    g.grbm = read_gc(g, kGfx11GrbmStatusBase, kGfx11GrbmStatus, &okb);
    g.grbm2 = read_gc(g, kGfx11GrbmStatus2Base, kGfx11GrbmStatus2, &okb2);
    evf(ev, kTagChk,
        "S2 record rlc_stat=0x%08x%s rlc_gpm=0x%08x%s grbm=0x%08x%s grbm2=0x%08x%s "
        "(comparison only, no gate)",
        g.rlc_stat, ok_rlc_stat ? "" : "(unreadable)", g.rlc_gpm, okg ? "" : "(unreadable)",
        g.grbm, okb ? "" : "(unreadable)", g.grbm2, okb2 ? "" : "(unreadable)");

    // S2 readiness: Linux gfx_v11_0_wait_for_rlc_autoload_complete polls
    // CP_STAT == 0 and RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE == 1. This replaces
    // the old "two equal RLC samples" rule (G-01).
    bool ok_cp = false, okboot = false;
    bool ready = false;
    const uint32_t polls_max = kGfx11AutoloadTimeoutUs / kGfx11AutoloadStepUs;
    for (uint32_t i = 0; i < polls_max; i++) {
        g.cp_stat = read_gc(g, kGfx11CpStatBase, kGfx11CpStat, &ok_cp);
        g.bootload = read_gc(g, kGfx11RlcRlcsBootloadStatusBase,
                             kGfx11RlcRlcsBootloadStatus, &okboot);
        g.polls = i + 1u;
        if (!ok_cp || !okboot || g.cp_stat == 0xFFFFFFFFu ||
            g.bootload == 0xFFFFFFFFu) {
            r = r3_substep_fail(kR3SecChk, 2, kChkAutoloadUnreadable);
            r3_substep_note(&r, g.gc0 + kGfx11CpStat, g.cp_stat, "CP_STAT");
            r3_substep_note(&r, g.gc1 + kGfx11RlcRlcsBootloadStatus, g.bootload,
                            "RLC_RLCS_BOOTLOAD_STATUS");
            evf(ev, kTagChk,
                "FAIL: autoload witness unreadable (cp_ok=%d boot_ok=%d cp=0x%08x "
                "boot=0x%08x)", ok_cp ? 1 : 0, okboot ? 1 : 0, g.cp_stat, g.bootload);
            return r;
        }
        if (g.cp_stat == 0u && (g.bootload & kGfx11BootloadComplete) != 0u) {
            ready = true;
            break;
        }
        r3_io_udelay(g.io, kGfx11AutoloadStepUs);
    }
    evf(ev, kTagChk,
        "S2 bootload=0x%08x complete=%u cp_stat=0x%08x busy(ce=%u pfp=%u me=%u cp=%u) "
        "polls=%u",
        g.bootload, (g.bootload & kGfx11BootloadComplete) ? 1u : 0u, g.cp_stat,
        (g.cp_stat & kGfx11CpStatCeBusy) ? 1u : 0u,
        (g.cp_stat & kGfx11CpStatPfpBusy) ? 1u : 0u,
        (g.cp_stat & kGfx11CpStatMeBusy) ? 1u : 0u,
        (g.cp_stat & kGfx11CpStatCpBusy) ? 1u : 0u, g.polls);
    if (!ready) {
        r = r3_substep_fail(kR3SecChk, 2, kChkAutoloadTimeout);
        r3_substep_note(&r, g.gc0 + kGfx11CpStat, g.cp_stat, "CP_STAT");
        r3_substep_note(&r, g.gc1 + kGfx11RlcRlcsBootloadStatus, g.bootload,
                        "RLC_RLCS_BOOTLOAD_STATUS");
        evf(ev, kTagChk,
            "FAIL: RLC autoload incomplete after %u us (bootload=0x%08x cp_stat=0x%08x)",
            kGfx11AutoloadTimeoutUs, g.bootload, g.cp_stat);
        return r;
    }
    evf(ev, kTagChk, "S2 PASS RLC autoload complete, CP idle");

    // S3: CP_ME_CNTL readback. The halt bits are evidence only: this substep must not
    // unhalt CP (that is MES S0).
    bool okm = false;
    g.cp_me_cntl = read_gc(g, kGfx11CpMeCntlBase, kGfx11CpMeCntl, &okm);
    evf(ev, kTagChk,
        "S3 CP_ME_CNTL=0x%08x%s halt(ce=%u pfp=%u me=%u)", g.cp_me_cntl,
        okm ? "" : "(unreadable)", (g.cp_me_cntl & kGfx11CpMeCntlCeHalt) ? 1u : 0u,
        (g.cp_me_cntl & kGfx11CpMeCntlPfpHalt) ? 1u : 0u,
        (g.cp_me_cntl & kGfx11CpMeCntlMeHalt) ? 1u : 0u);
    if (!okm || g.cp_me_cntl == 0xFFFFFFFFu) {
        r = r3_substep_fail(kR3SecChk, 3, kChkCpUnreadable);
        r3_substep_note(&r, g.gc1 + kGfx11CpMeCntl, g.cp_me_cntl, "CP_ME_CNTL");
        return r;
    }
    evf(ev, kTagChk, "S3 PASS cp_me_cntl=0x%08x (read-only)", g.cp_me_cntl);
    // G16: gfx_v11_0.c:2695-2754 reads GB_ADDR_CONFIG as a later config input. This
    // stage records it only, never writes, and a read fault is not fatal (T-E7).
    bool gb_ok = false;
    const uint32_t gb = r3_io_read32(g.io, g.gc0 + kGfxGbAddrConfig, &gb_ok);
    if (gb_ok)
        evf(ev, kTagChk, "G16 GB_ADDR_CONFIG=0x%08x", gb);
    else
        evf(ev, kTagChk, "G16 GB_ADDR_CONFIG unreadable (recorded, not fatal)");

    g.env->rt->ready_bits |= R3_IP_BIT(R3Ip::Gfx);
    return r3_substep_pass();
}

} // namespace

// ---------------------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------------------
R3SubstepResult gfx_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    // Small POD (< 100 B): it fits the 16 KiB kernel stack comfortably, unlike the PSP
    // substep's multi-KiB staging state.
    Gfx g;
    memset(&g, 0, sizeof g);
    g.io = io;
    g.env = env;

    if (!env || !env->rt) {
        R3SubstepResult r = r3_substep_fail(kR3SecPre, 1, kPreNoEnv);
        if (env && env->ev)
            evf(env->ev, kTagPre, "FAIL: no environment/runtime block");
        return r;
    }

    // Arm the owner first so any write outside the declared rows is DENIED and counted (the
    // deny count must stay 0; the test asserts it). pre() declares the W1.5 rows once the GC
    // bases are known.
    r3_wg_arm_substep(WSubstep::Gfx);
    R3SubstepResult r = pre(g);
    if (r.status == kR3SsecOk)
        r = seq(g);
    if (r.status == kR3SsecOk)
        r = chk(g);

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, 0)) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }
    r3_wg_disarm_substep();

    if (r.status == kR3SsecOk) {
        evf(env->ev, "gfx",
            "ready: bootload=0x%08x cp_stat=0x%08x imu=0x%08x rlc_stat=0x%08x "
            "rlc_gpm=0x%08x cp_me_cntl=0x%08x grbm=0x%08x grbm2=0x%08x (W1.5 writes)",
            g.bootload, g.cp_stat, g.imu_ver, g.rlc_stat, g.rlc_gpm, g.cp_me_cntl,
            g.grbm, g.grbm2);
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-gfx-done", "Yes", 3);
    } else {
        evf(env->ev, "gfx", "FAIL %s (section %s step %u)", r.name,
            r3_substep_section_name(r.section), static_cast<unsigned>(r.step));
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-gfx-done", "No", 2);
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// Stage 3 (deferred): per-VMID compute/GDS init and RLC SRM (gfx_v11_0.c:2096-2154,
// :2345-2356). These functions are complete but intentionally not called from
// gfx_v11_0_substep(); wiring them into the install sequence is a later decision.
// Precondition: the stage-2 substep has run in this boot, so the shared GRBM/SH_MEM rows
// (0x0900/0x09E4/0x09E3) are already granted.
// ---------------------------------------------------------------------------------------
static bool stage3_compute_vmid(Gfx &g)
{
    for (uint32_t vmid = 8u; vmid < 16u; vmid++) {
        const bool ok =
            gc_write32(g, 1u, 0x0900u, vmid << 4, "gfx:stage3") &&
            gc_write32(g, 1u, 0x09E4u, 0x0000C00Cu, "gfx:stage3") &&
            gc_write32(g, 1u, 0x09E3u, kGfxShMemLdsBases, "gfx:stage3") &&
            gc_rmw(g, 0u, kGfxSpiGdbgPerVmidCntl, kGfxSpiGdbgTrapEn, kGfxSpiGdbgTrapEn,
                   "gfx:stage3", true) &&
            gc_write32(g, 0u, kGfxGdsVmid0Base + 2u * vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsVmid0Size + 2u * vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsGwsVmid0 + vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsOaVmid0 + vmid, 0u, "gfx:stage3");
        if (!ok)
            return false;
    }
    // Linux returns with VMID 0 selected (soc21_grbm_select(0,0,0,0)).
    if (!gc_write32(g, 1u, 0x0900u, 0u, "gfx:stage3"))
        return false;
    evf(g.env->ev, kTagStage3,
        "G13 compute vmid8..15: SH_MEM_CONFIG=0xC00C BASES=0x10002, TRAP_EN, GDS=0");
    return true;
}

static bool stage3_gds_vmid(Gfx &g)
{
    for (uint32_t vmid = 1u; vmid < 16u; vmid++) {
        const bool ok =
            gc_write32(g, 0u, kGfxGdsVmid0Base + 2u * vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsVmid0Size + 2u * vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsGwsVmid0 + vmid, 0u, "gfx:stage3") &&
            gc_write32(g, 0u, kGfxGdsOaVmid0 + vmid, 0u, "gfx:stage3");
        if (!ok)
            return false;
    }
    evf(g.env->ev, kTagStage3, "G14 GDS vmid1..15 zeroed (base/size/gws/oa)");
    return true;
}

static bool stage3_rlc_srm(Gfx &g)
{
    bool ok = false;
    const uint32_t cur = read_gc(g, 1u, kGfxRlcSrmCntl, &ok);
    if (!ok)
        return false;
    const uint32_t next = cur | kGfxRlcSrmAutoIncr | kGfxRlcSrmEnable;
    if (!gc_write32(g, 1u, kGfxRlcSrmCntl, next, "gfx:stage3"))
        return false;
    evf(g.env->ev, kTagStage3, "G17 RLC_SRM_CNTL 0x%08x -> 0x%08x (SRM+auto-incr)",
        cur, next);
    return true;
}

// Stage-3 grants. G13/G14 address the GDS per-VMID instances with a stride
// (BASE/SIZE + 2*vmid, GWS/OA + vmid), and a register grant is an exact addr+width
// match, so every instance is registered separately: 2 fixed + 15*4 GDS = 62.
// This deferred path sits outside the production budget; wiring it in requires
// re-checking R3_WG_GRANT_MAX (see the TP-C2 self-audit report).
static bool stage3_register_rows(Gfx &g)
{
    if (r3_wg_register_reg(g.gc0 + kGfxSpiGdbgPerVmidCntl, 1u, WClass::State,
                           WSubstep::Gfx, "reg:gfx-stage3") != 0)
        return false;
    if (r3_wg_register_reg(g.gc1 + kGfxRlcSrmCntl, 1u, WClass::State,
                           WSubstep::Gfx, "reg:gfx-stage3") != 0)
        return false;
    for (uint32_t vmid = 1u; vmid < 16u; vmid++) {
        const uint32_t addrs[4] = {
            g.gc0 + kGfxGdsVmid0Base + 2u * vmid,
            g.gc0 + kGfxGdsVmid0Size + 2u * vmid,
            g.gc0 + kGfxGdsGwsVmid0 + vmid,
            g.gc0 + kGfxGdsOaVmid0 + vmid,
        };
        for (uint32_t i = 0; i < 4u; i++) {
            if (r3_wg_register_reg(addrs[i], 1u, WClass::State, WSubstep::Gfx,
                                   "reg:gfx-stage3") != 0)
                return false;
        }
    }
    return true;
}

R3SubstepResult gfx_v11_0_stage3_substep(const R3Io *io, const R3SubstepEnv *env)
{
    Gfx g;
    memset(&g, 0, sizeof g);
    g.io = io;
    g.env = env;
    if (!env || !env->rt)
        return r3_substep_fail(kR3SecPre, 1, kPreNoEnv);

    const amdgpu::IPBaseTable *ip = static_cast<const amdgpu::IPBaseTable *>(env->ip);
    if (!ip || !ip->isResolved(amdgpu::IPBlock::GC, 0) ||
        !ip->isResolved(amdgpu::IPBlock::GC, 1)) {
        evf(env->ev, kTagStage3, "FAIL: GC BASE_IDX 0/1 unresolved");
        return r3_substep_fail(kR3SecPre, 4, kStage3GcBase);
    }
    g.gc0 = ip->getBase(amdgpu::IPBlock::GC, 0);
    g.gc1 = ip->getBase(amdgpu::IPBlock::GC, 1);

    r3_wg_arm_substep(WSubstep::Gfx);
    R3SubstepResult r = r3_substep_pass();
    if (r3_wg_reserve(62u) != 0)
        r = r3_substep_fail(kR3SecPre, 5, kStage3Register);
    else if (!stage3_register_rows(g))
        r = r3_substep_fail(kR3SecPre, 6, kStage3Register);
    if (r.status == kR3SsecOk && !stage3_compute_vmid(g))
        r = r3_substep_fail(kR3SecSeq, 2, kStage3Vmid);
    if (r.status == kR3SsecOk && !stage3_gds_vmid(g))
        r = r3_substep_fail(kR3SecSeq, 3, kStage3Gds);
    if (r.status == kR3SsecOk && !stage3_rlc_srm(g))
        r = r3_substep_fail(kR3SecSeq, 4, kStage3RlcSrm);
    r3_wg_disarm_substep();
    if (r.status == kR3SsecOk)
        evf(env->ev, kTagStage3, "stage-3 deferred sections complete (G13/G14/G17)");
    return r;
}
} // namespace amdgpu
