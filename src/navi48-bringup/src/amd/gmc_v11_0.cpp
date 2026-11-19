/*
 * gmc_v11_0.cpp - GMC (Gfx/MM Hub) substeps for RDNA3 bring-up, gfx1103 (780M).
 *
 * Stage-3 batch T-B, four substeps (实施文档 §6):
 *   S6 uma     - MC aperture read-only recheck + layout D4-D8 registration +
 *                sysmem_grants backfill (three system-memory pools).
 *   S7 gart    - flat GART table in system memory (512K x 8B), VMID0 flat
 *                configuration on both hubs, TLB invalidate (engine 17,
 *                degraded Linux shape), SDMA WRITE_LINEAR through GART VA.
 *   S8 gpuvm0  - four-level page table for VMID0 (root 4KiB + 4MiB pool),
 *                nullarc vmBind register sequence with absolute dword
 *                cross-check against R10, VRAM test page mapped, SDMA
 *                verification through GPUVM.
 *   S9 vmid8   - same shape for VMID 8 (D3 pool), PER_VMID invalidate,
 *                FAULT_DEFAULT for the test window; register-level gate only.
 *
 * Provenance: Linux drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c (flush shape,
 * Copyright 2019 Advanced Micro Devices, Inc., MIT licensed), gfxhub_v3_0.c /
 * mmhub_v3_0.c (VMID config), amdgpu_vm.h (PTE/PDE bits), amdgpu_vm_cpu.c
 * (mb + cache-flush discipline), and the nullarc on-silicon measurements
 * (R10: PhxBringup.cpp vmBind/vmFlushTlb/vmPte, mesboot gart()/sdma_write).
 * Every offset is cited at its use site; every step fails closed, never
 * retries, never widens.
 */
#include "gmc_v11_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h> /* snprintf/vsnprintf/memcpy/memset, no libc under -nostdinc */
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "../rdna3/golden_values.h"
#include "../rdna3/rdna3_layout.h"
#include "../rdna3/rdna3_runtime.h"

namespace amdgpu {
namespace {

/* ---- logging / property helpers (mes_v11_0.cpp shape) ---- */

void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
{
    char text[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (env && env->ev)
        r3_ev_add(env->ev, tag, text);
}

/* r3_substep_fail() clears reading_count, so a note written to a passing
 * result would be lost; build the failed result first, then attach it. */
R3SubstepResult fail_note(R3SubstepSection section, uint32_t step, const char *name,
                          uint32_t addr, uint32_t value, const char *what)
{
    R3SubstepResult f = r3_substep_fail(section, step, name);
    r3_substep_note(&f, addr, value, what);
    return f;
}

void set_prop(const R3SubstepEnv *env, const char *key, const char *value)
{
    if (!env || !env->sink || !env->sink->set_property || !value)
        return;
    size_t n = 0;
    while (value[n])
        n++;
    env->sink->set_property(env->sink->user, key, value, n);
}

/* ---- failure names (static strings, mes shape) ---- */
constexpr const char *kFailNoIo = "pre:no-io";
constexpr const char *kFailNoEnv = "pre:no-env";
constexpr const char *kFailNoBase = "pre:no-base";
constexpr const char *kFailNoGrant = "pre:no-grant";
constexpr const char *kFailNoSysmem = "pre:no-sysmem";
constexpr const char *kFailRegister = "pre:register";
constexpr const char *kFailRegisterFull = "pre:register-full";
constexpr const char *kFailMcMismatch = "seq:mc-mismatch";
constexpr const char *kFailLayout = "seq:layout";
constexpr const char *kFailGartRegs = "seq:gart-regs";
constexpr const char *kFailInvalidate = "seq:invalidate";
constexpr const char *kFailGartVerify = "chk:gart-verify";
constexpr const char *kFailPtBuild = "seq:pt-build";
constexpr const char *kFailVbind = "seq:vbind";
constexpr const char *kFailCrossCheck = "seq:cross-check";
constexpr const char *kFailGpuvmVerify = "chk:gpuvm-verify";
constexpr const char *kFailFaultDefault = "seq:fault-default";
constexpr const char *kFailVmid8Chk = "chk:vmid8";
constexpr const char *kFailExpectMismatch = "expect-write-mismatch";
constexpr const char *kFailExpectCap = "pre:expect-cap";

/* ---- sysmem allocation state (bound by T-E / host tests) ----
 * Kept outside the per-call state so the four substeps share the pools and
 * the S6-derived fb physical base. */
struct GmcSysmemBlock {
    uint64_t phys;
    void *cpu;
    uint64_t bytes;
};
GmcSysmemBlock g_gmc_sysmem[3];
const GmcSysmemAlloc *g_gmc_alloc = nullptr;
uint64_t g_gmc_fb_phys_base = 0;

/* SDMA ring/shadow offsets inside the C5 sdma grant (sdma_v6_0.h layout,
 * re-declared here: sdma_v6_0.h is outside this batch's writable set). */
constexpr uint32_t kGmcSdmaRingOff = 0x000000u;
/* RB_RPTR golden symbol added in T-E; gc_11_0_0_offset.h:196/197. */
constexpr uint32_t kGmcSdmaRbRptrOff = 0x0083u;  // kept as fast-path, golden resolver preferred
/* WPTR shadow offset within SDMA grant (sdma_v6_0.h:109). */
constexpr uint32_t kGmcSdmaWptrShadowOff = 0x2000u;
/* sdma_submit failure steps (W3 verdicts). Doorbell is reserved for phase 2
 * and must not be used by the phase-1 submit path. */
enum : uint32_t {
    kSdmaStepNone = 0u,
    kSdmaStepFlush = 1u,
    kSdmaStepPktBack = 2u,
    kSdmaStepShadowBack = 3u,
};
/* MMHUB invalidate engine 17 golden symbols added in T-E;
 * mmhub_3_0_1_offset.h (BASE_IDX=1): ENG17_SEM=0x0762, ENG17_REQ=0x0774. */
constexpr uint32_t kGmcMmInvEng17SemOff = 0x0762u;  // kept as reference; golden resolver preferred
constexpr uint32_t kGmcMmInvEng17ReqOff = 0x0774u;
__attribute__((unused)) static void use_inv_offsets(void) { (void)kGmcMmInvEng17SemOff; (void)kGmcMmInvEng17ReqOff; }
constexpr uint32_t kGmcMmInvEng17AckOff = 0x0786u;  // ACK has no golden symbol

/* TLB invalidation engine address-range arming (S7). Linux arms all 18
 * engines before any request is issued; each engine owns a LO/HI dword pair
 * with stride eng_addr_distance = 2:
 *   gfxhub_v3_0_program_invalidation (gfxhub_v3_0.c): VM_INVALIDATE_ENG0_
 *     ADDR_RANGE_LO32 = 0x16cf (gc_11_0_0_offset.h, BASE_IDX 0);
 *   mmhub_v3_0_1_program_invalidation (mmhub_v3_0_1.c): VM_INVALIDATE_ENG0_
 *     ADDR_RANGE_LO32 = 0x0787 (mmhub_3_0_1_offset.h, BASE_IDX 1).
 * LO=0xffffffff / HI=0x1f covers the whole 48-bit GPU address space. */
constexpr uint32_t kGcInvArmLoOff = 0x16cfu;
constexpr uint32_t kGcInvArmHiOff = 0x16d0u;
constexpr uint32_t kMmInvArmLoOff = 0x0787u;
constexpr uint32_t kMmInvArmHiOff = 0x0788u;
constexpr uint32_t kInvArmEngines = 18u;
constexpr uint32_t kInvArmRangeLo = 0xffffffffu;
constexpr uint32_t kInvArmRangeHi = 0x1fu;
__attribute__((unused)) static void use_inv_arm_offsets(void)
{
    (void)kGcInvArmHiOff;
    (void)kMmInvArmHiOff;
}

/* D7 test-buffer pages mapped through the GART (1MiB / 4KiB). */
constexpr uint32_t kGmcGartTestPages = 256u;

/* ---- writeguard class mapping ---- */
WClass to_wclass(R3WClassId id)
{
    switch (id) {
    case kR3WAccess:
        return WClass::Access;
    case kR3WData:
        return WClass::Data;
    case kR3WProtocol:
        return WClass::Protocol;
    case kR3WState:
    default:
        return WClass::State;
    }
}

/* ---- row declaration tables (the single source of truth for the write set) ----
 * S7/S8 share one table: the vmBind register sequence is the same register
 * group (VM_CONTEXT0) on both hubs, plus the invalidate trio and the SDMA
 * submission pair. S9 has its own table (VM_CONTEXT8 + fault default); its
 * invalidate rows are declared but NOT re-registered (S7 owns those windows). */
constexpr R3WriteRowReg kGartRows[] = {
    /* GC VM_CONTEXT0 group (golden) */
    {&r3gold::kGcvmContext0Cntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-cntl"},
    {&r3gold::kGcvmContext0PageTableBaseAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-base-lo"},
    {&r3gold::kGcvmContext0PageTableBaseAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-base-hi"},
    {&r3gold::kGcvmContext0PageTableStartAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-start-lo"},
    {&r3gold::kGcvmContext0PageTableStartAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-start-hi"},
    {&r3gold::kGcvmContext0PageTableEndAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-end-lo"},
    {&r3gold::kGcvmContext0PageTableEndAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx0-end-hi"},
    /* MMHUB VM_CONTEXT0 group (golden) */
    {&r3gold::kMmvmContext0Cntl, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-cntl"},
    {&r3gold::kMmvmContext0PageTableBaseAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-base-lo"},
    {&r3gold::kMmvmContext0PageTableBaseAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-base-hi"},
    {&r3gold::kMmvmContext0PageTableStartAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-start-lo"},
    {&r3gold::kMmvmContext0PageTableStartAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-start-hi"},
    {&r3gold::kMmvmContext0PageTableEndAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-end-lo"},
    {&r3gold::kMmvmContext0PageTableEndAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx0-end-hi"},
    /* invalidate engine 17 (GC golden; MMHUB now uses golden symbols added in T-E) */
    {&r3gold::kGcvmInvalidateEng17Req, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:inv-req"},
    {&r3gold::kMmvmInvalidateEng17Sem, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:mm-inv-sem"},
    {&r3gold::kMmvmInvalidateEng17Req, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:mm-inv-req"},
    /* SDMA submission (golden) */
    {&r3gold::kSdma0Queue0RbWptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:sdma-wptr"},
    {&r3gold::kSdma0Queue0MinorPtrUpdate, static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:sdma-minor"},
    /* invalidation engine address-range arming (S7 only): 18 engines x 2
     * adjacent dwords per hub (Linux program_invalidation, stride 2) */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kGcInvArmLoOff, 36u,
     WClass::State, WNeed::OnSuccessPath, "gmc:inv-arm-gc"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, kMmInvArmLoOff, 36u,
     WClass::State, WNeed::OnSuccessPath, "gmc:inv-arm-mm"},
};
static_assert(sizeof(kGartRows) / sizeof(kGartRows[0]) == kGartRowCount,
              "gart row table shape");
/* Rows [0, kGartVbindRowCount) are the vmBind/invalidate/SDMA set S8 re-walks;
 * the arm rows start at this index and are written by S7 only (the registers
 * persist across substeps, so S8 neither rewrites nor expects them). */
constexpr uint32_t kGartVbindRowCount = 19u;
static_assert(kGartVbindRowCount <= R3_WG_EXPECT_MAX,
              "gart vbind expectation set exceeds R3_WG_EXPECT_MAX");
static_assert(kGartVbindRowCount < kGartRowCount, "vbind row subset");
static_assert(kGartRows[kGartVbindRowCount].golden == nullptr &&
              kGartRows[kGartVbindRowCount].dwords == kInvArmEngines * 2u &&
              kGartRows[kGartVbindRowCount + 1u].golden == nullptr &&
              kGartRows[kGartVbindRowCount + 1u].dwords == kInvArmEngines * 2u,
              "arm rows must trail the vbind set");

constexpr R3WriteRowReg kVmid8Rows[] = {
    /* GC VM_CONTEXT8 group (golden) */
    {&r3gold::kGcvmContext8Cntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-cntl"},
    {&r3gold::kGcvmContext8PageTableBaseAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-base-lo"},
    {&r3gold::kGcvmContext8PageTableBaseAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-base-hi"},
    {&r3gold::kGcvmContext8PageTableStartAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-start-lo"},
    {&r3gold::kGcvmContext8PageTableStartAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-start-hi"},
    {&r3gold::kGcvmContext8PageTableEndAddrLo32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-end-lo"},
    {&r3gold::kGcvmContext8PageTableEndAddrHi32, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:ctx8-end-hi"},
    /* MMHUB VM_CONTEXT8 group (golden) */
    {&r3gold::kMmvmContext8Cntl, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-cntl"},
    {&r3gold::kMmvmContext8PageTableBaseAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-base-lo"},
    {&r3gold::kMmvmContext8PageTableBaseAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-base-hi"},
    {&r3gold::kMmvmContext8PageTableStartAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-start-lo"},
    {&r3gold::kMmvmContext8PageTableStartAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-start-hi"},
    {&r3gold::kMmvmContext8PageTableEndAddrLo32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-end-lo"},
    {&r3gold::kMmvmContext8PageTableEndAddrHi32, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:mm-ctx8-end-hi"},
    /* fault default (golden) */
    {&r3gold::kGcvmL2ProtectionFaultCntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::State, WNeed::OnSuccessPath, "gmc:fault-default"},
    /* invalidate engine 17 (registered by S7; declared for the expect set) */
    {&r3gold::kGcvmInvalidateEng17Req, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:inv-req"},
    {&r3gold::kMmvmInvalidateEng17Sem, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:mm-inv-sem"},
    {&r3gold::kMmvmInvalidateEng17Req, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1,
     WClass::Protocol, WNeed::OnSuccessPath, "gmc:mm-inv-req"},
};
static_assert(sizeof(kVmid8Rows) / sizeof(kVmid8Rows[0]) == kVmid8RowCount,
              "vmid8 row table shape");
/* Rows 15..17 are the invalidate trio S7 already registered. */

/* ---- per-call state (file-scope static: never on the 16 KiB kernel stack) ---- */
struct Gmc {
    const R3Io *io;
    const R3SubstepEnv *env;
    R3LayoutPlan *plan;
    uint32_t mc_base;
    const IPBaseTable *ip;

    /* golden-resolved absolute dword addresses (S6 aperture recheck) */
    uint32_t mm_fb_offset, mm_fb_base, mm_fb_top;
    uint32_t mm_agp_top, mm_agp_bot, mm_agp_base;
    uint32_t mm_sys_lo, mm_sys_hi, mm_def_lsb, mm_def_msb;
    uint32_t gc_fb_offset, gc_fb_base, gc_fb_top;
    uint32_t gc_agp_top, gc_agp_bot, gc_agp_base;
    uint32_t gc_sys_lo, gc_sys_hi, gc_def_lsb, gc_def_msb;

    /* resolved row addresses (S7/S8/S9 write set) */
    uint32_t rows_addr[R3_WG_EXPECT_MAX];

    /* frequently used rows, copied out for readability */
    uint32_t gc_inv_req, gc_inv_ack, mm_inv_sem, mm_inv_req, mm_inv_ack;
    uint32_t sdma_wptr, sdma_minor;
    /* last sdma_submit, kept for the S7 failure diagnostic (W3) */
    uint32_t sdma_rptr_reg;
    uint32_t sdma_rptr_before;
    uint32_t last_sdma_wptr_bytes;
    /* phase-1 publish/readback verdict + raw readback values */
    uint32_t sdma_fail_step;
    uint32_t sdma_pkt_back;
    uint32_t sdma_shadow_back;
    uint32_t sdma_shadow_hi;   /* upper dword, observation only */

    /* grants / sysmem blocks */
    const R3Grant *sdma_grant;
    const R3Grant *test_grant;
    GmcSysmemBlock *gart_blk;
    GmcSysmemBlock *ctx0_blk;
    GmcSysmemBlock *vmid8_blk;

    /* S6 aperture readback */
    uint32_t mm_fb_offset_raw, gc_fb_offset_raw;
    uint32_t mm_fb_base_raw, gc_fb_base_raw;
};
Gmc g_gmc;

/* ---- register / bus helpers (mes_v11_0.cpp shape) ---- */

uint32_t reg_read(Gmc &s, uint32_t abs, bool *ok)
{
    return r3_io_read32(s.io, abs, ok);
}

int reg_write(Gmc &s, uint32_t abs, uint32_t value, R3WClassId cls, const char *ctx)
{
    if (r3_wg_classify(abs, 1, to_wclass(cls), ctx) != WVerdict::TxnRead)
        return -1;
    r3_io_write32(s.io, abs, value);
    return 0;
}

int bus_write(Gmc &s, uint64_t bar0_off, uint32_t value, uint32_t dwords, const char *ctx)
{
    if (bar0_off & 3)
        return -1;
    if (dwords != 1 && dwords != 2)
        return -1;
    if (r3_wg_classify(bar0_off >> 2, dwords, WClass::Data, ctx) != WVerdict::TxnRead)
        return -1;
    uint64_t v = value;
    s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, true, dwords);
    return 0;
}

int bus_read(Gmc &s, uint64_t bar0_off, uint32_t *value, uint32_t dwords, const char *ctx)
{
    if (bar0_off & 3)
        return -1;
    if (dwords != 1 && dwords != 2)
        return -1;
    uint64_t v = 0;
    if (!s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, dwords))
        return -1;
    *value = static_cast<uint32_t>(v);
    (void)ctx;
    return 0;
}

/* Polls a BAR0 dword until (v & mask) == want (BAR0 is not MMIO, so the
 * generic r3_io_poll cannot be used here). */
int poll_bus(Gmc &s, uint64_t bar0_off, uint32_t mask, uint32_t want,
             uint32_t polls, uint32_t us_per_poll, uint32_t *value)
{
    for (uint32_t i = 0; i < polls; i++) {
        uint32_t v = 0;
        if (bus_read(s, bar0_off, &v, 1, "gmc:poll"))
            return -1;
        if ((v & mask) == want) {
            if (value)
                *value = v;
            return 1;
        }
        s.io->udelay(s.io->ctx, us_per_poll);
    }
    return 0;
}


/* 32-byte stride zero fill (mes zero_region shape). */
int zero_region(Gmc &s, uint64_t bar0_off, uint32_t bytes, const char *ctx)
{
    if ((bar0_off & 31) || (bytes & 31))
        return -1;
    for (uint32_t off = 0; off < bytes; off += 32)
        for (uint32_t q = 0; q < 4; q++)
            if (bus_write(s, bar0_off + off + q * 8ull, 0, 1, ctx) ||
                bus_write(s, bar0_off + off + q * 8ull + 4ull, 0, 1, ctx))
                return -1;
    return 0;
}

/* Flushes every 64B line of a system-memory region (nullarc vmPte discipline:
 * the GPU walker may not snoop CPU caches). */
void flush_region(void *cpu, uint64_t bytes)
{
    uint8_t *p = static_cast<uint8_t *>(cpu);
    for (uint64_t off = 0; off < bytes; off += 64)
        gmc_pt_flush_line(p + off);
}

/* Compiler barrier; the clflush memory clobbers carry the ordering, this
 * marks the batch boundary (amdgpu_vm_cpu.c:127 mb() discipline). */
void gmc_mb(void)
{
    __asm__ __volatile__("" ::: "memory");
}

/* ---- row address resolution (integration row_addr shape) ---- */

bool row_addr(const IPBaseTable *ip, const R3WriteRowReg &row, uint32_t *addr)
{
    if (row.golden)
        return ip->resolve(static_cast<const r3gold::GoldenReg *>(row.golden)->ref(), addr);
    if (!ip->isResolved(static_cast<IPBlock>(row.block), row.base_idx))
        return false;
    const uint32_t base = ip->getBase(static_cast<IPBlock>(row.block), row.base_idx);
    if (base == 0xffffffffu)
        return false;
    *addr = base + row.off;
    return true;
}

/* ---- TLB invalidate (Linux gmc_v11_0.c:220-312 degraded, nullarc R10) ----
 * Engine 17 fixed; req = base | (1 << vmid) | 1; ack poll = 1 << vmid.
 * GC hub: no semaphore (Linux use_semaphore is MMHUB0-only). MMHUB hub:
 * semaphore acquire (timeout recorded, not fatal), request, ack poll,
 * semaphore release. HDP flush precedes each hub's request. */
/* Resolves the invalidate ack addresses (GC golden; MMHUB fallback 0x0786,
 * mmhub_3_0_1_offset.h BASE_IDX=1 - no golden symbol, gap reported). */
int resolve_inv_addrs(Gmc &s)
{
    if (!s.ip->resolve(r3gold::kGcvmInvalidateEng17Ack.ref(), &s.gc_inv_ack))
        return -1;
    if (!s.ip->isResolved(IPBlock::MMHUB, 1))
        return -1;
    const uint32_t mm_base = s.ip->getBase(IPBlock::MMHUB, 1);
    if (mm_base == 0xffffffffu)
        return -1;
    s.mm_inv_ack = mm_base + kGmcMmInvEng17AckOff;
    return 0;
}

int invalidate_hubs(Gmc &s, uint32_t vmid, const char *ctx)
{
    const uint32_t bit = 1u << vmid;
    const uint32_t req = kGmcInvBase | bit | 1u;

    r3_io_hdp_flush(s.io);
    if (reg_write(s, s.gc_inv_req, req, kR3WProtocol, ctx))
        return -1;
    uint32_t ack = 0;
    if (r3_io_poll(s.io, s.gc_inv_ack, bit, bit, kGmcInvPolls, kGmcInvPollUs, &ack) != 1) {
        evf(s.env, "gmc-inv", "gc ack timeout (ack=0x%x)", ack);
        return -1;
    }

    r3_io_hdp_flush(s.io);
    uint32_t sem = 0;
    if (r3_io_poll(s.io, s.mm_inv_sem, 1u, 1u, kGmcInvPolls, kGmcInvPollUs, &sem) != 1)
        evf(s.env, "gmc-inv", "mmhub sem acquire timeout (sem=0x%x)", sem);
    if (reg_write(s, s.mm_inv_req, req, kR3WProtocol, ctx))
        return -1;
    if (r3_io_poll(s.io, s.mm_inv_ack, bit, bit, kGmcInvPolls, kGmcInvPollUs, &ack) != 1) {
        evf(s.env, "gmc-inv", "mmhub ack timeout (ack=0x%x)", ack);
        return -1;
    }
    if (reg_write(s, s.mm_inv_sem, 0u, kR3WProtocol, ctx))
        return -1;
    return 0;
}

/* Arms the invalidation engine address ranges (Linux
 * gfxhub_v3_0_program_invalidation / mmhub_v3_0_1_program_invalidation).
 * The 18 engines form one contiguous 36-dword register block per hub, so the
 * block is classified once at its declared width and then written dword by
 * dword (MMIO has no multi-dword register access). The readback below is the
 * hardware-state proof: every following invalidate request assumes these
 * windows are established (nullarc: range was 0 -> flushes did nothing). */
int arm_invalidation(Gmc &s, const char *ctx)
{
    for (uint32_t hub = 0; hub < 2u; hub++) {
        const uint32_t lo = s.rows_addr[kGartVbindRowCount + hub];
        if (r3_wg_classify(lo, kInvArmEngines * 2u, WClass::State, ctx) != WVerdict::TxnRead)
            return -1;
        for (uint32_t e = 0; e < kInvArmEngines; e++) {
            if (!r3_io_write32(s.io, lo + 2u * e, kInvArmRangeLo) ||
                !r3_io_write32(s.io, lo + 2u * e + 1u, kInvArmRangeHi)) {
                evf(s.env, "gmc-inv-arm", "write failed hub=%u eng=%u", hub, e);
                return -1;
            }
        }
    }
    for (uint32_t hub = 0; hub < 2u; hub++) {
        const uint32_t lo = s.rows_addr[kGartVbindRowCount + hub];
        for (uint32_t e = 0; e < kInvArmEngines; e++) {
            bool ok_lo = false;
            bool ok_hi = false;
            const uint32_t got_lo = reg_read(s, lo + 2u * e, &ok_lo);
            const uint32_t got_hi = reg_read(s, lo + 2u * e + 1u, &ok_hi);
            if (!ok_lo || !ok_hi || got_lo != kInvArmRangeLo || got_hi != kInvArmRangeHi) {
                evf(s.env, "gmc-inv-arm",
                    "readback mismatch hub=%u eng=%u lo=0x%x hi=0x%x",
                    hub, e, got_lo, got_hi);
                return -1;
            }
        }
    }
    evf(s.env, "gmc-inv-arm", "armed 18 engines per hub (gc=0x%x mm=0x%x)",
        s.rows_addr[kGartVbindRowCount], s.rows_addr[kGartVbindRowCount + 1u]);
    return 0;
}

/* ---- SDMA WRITE_LINEAR submission (nullarc mesboot sdma_write, E4a) ----
 * The queue was started by stage 2 (sdma_v6_0 substep); this only appends a
 * packet. Ring/shadow writes live in the SDMA layout grant (owner Sdma), so
 * the armed substep flips to Sdma for that section and back to Gmc for the
 * protected WPTR update (the flip is exactly what arm_substep is for). */
int sdma_submit(Gmc &s, uint64_t dst_va, uint32_t value, const char *ctx)
{
    /* Every call starts from a clean verdict: an early exit (e.g. rptr out of
     * range) must not inherit the readback values of a previous submit. */
    s.sdma_fail_step = kSdmaStepNone;
    s.sdma_pkt_back = 0u;
    s.sdma_shadow_back = 0u;
    s.sdma_shadow_hi = 0u;

    r3_wg_arm_substep(WSubstep::Sdma);

    /* Ring pointers are BYTE offsets (Linux sdma_v6_0.c ring_get_rptr
     * shifts the dword read right by 2; the S5 path programs the queue the
     * same way). RB_RPTR has no golden symbol (gap reported). */
    bool ok = false;
    const uint32_t sdma_base = s.ip->getBase(IPBlock::SDMA0, 0);
    if (sdma_base == 0xffffffffu) {
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }
    const uint32_t rptr_reg = sdma_base + kGmcSdmaRbRptrOff;
    const uint32_t rptr_bytes = reg_read(s, rptr_reg, &ok);
    if (!ok || (rptr_bytes & 3u) != 0u ||
        rptr_bytes > kGmcSdmaRingBytes - kGmcSdmaPacketBytes) {
        evf(s.env, "gmc-gart", "sdma rptr out of range: rptr=0x%x ring=0x%x",
            rptr_bytes, kGmcSdmaRingBytes);
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }

    /* WRITE_LINEAR packet: header, dst lo/hi, count (0 = one dword), value. */
    const uint32_t pkt[kGmcSdmaPacketDwords] = {
        kGmcSdmaWriteLinearHdr,
        static_cast<uint32_t>(dst_va & 0xffffffffu),
        static_cast<uint32_t>(dst_va >> 32),
        0u,
        value,
    };
    const uint64_t ring_off = s.sdma_grant->start + kGmcSdmaRingOff +
        static_cast<uint64_t>(rptr_bytes);
    const uint32_t wptr_bytes = rptr_bytes + kGmcSdmaPacketBytes;
    /* the ring is a dword stream: write it dword by dword (write_dwords
     * requires 8-byte alignment, a 5-dword packet does not have it) */
    bool ring_ok = true;
    for (uint32_t i = 0; i < kGmcSdmaPacketDwords; i++)
        if (bus_write(s, ring_off + static_cast<uint64_t>(i) * 4ull, pkt[i], 1, ctx)) {
            ring_ok = false;
            break;
        }
    if (!ring_ok ||
        bus_write(s, s.sdma_grant->start + kGmcSdmaWptrShadowOff, wptr_bytes, 1, ctx)) {
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }

    /* INV-C1/C2: the packet and the wptr shadow are memory the engine reads.
     * Publish them, then verify the writes actually landed, before any kick.
     * Arm is still Sdma here, so every failure path flips back to Gmc first
     * (same shape as the early exits above). */
    if (s.io->fence)
        s.io->fence(s.io->ctx);
    if (!r3_io_hdp_flush(s.io)) {
        s.sdma_fail_step = kSdmaStepFlush;
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }

    for (uint32_t i = 0; i < kGmcSdmaPacketDwords; ++i) {
        uint32_t back = 0u;
        if (bus_read(s, ring_off + static_cast<uint64_t>(i) * 4ull, &back, 1u,
                     "gmc:sdma-pkt-back") ||
            back != pkt[i]) {
            s.sdma_fail_step = kSdmaStepPktBack;
            s.sdma_pkt_back = back;
            r3_wg_arm_substep(WSubstep::Gmc);
            return -1;
        }
    }

    uint64_t sh = 0ull;
    if (!s.io->bus ||
        !s.io->bus(s.io->ctx, R3_IO_BAR_VRAM,
                   s.sdma_grant->start + kGmcSdmaWptrShadowOff,
                   &sh, false, 2u)) {
        s.sdma_fail_step = kSdmaStepShadowBack;   /* transfer failed: no data */
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }
    s.sdma_shadow_back = static_cast<uint32_t>(sh);
    s.sdma_shadow_hi = static_cast<uint32_t>(sh >> 32);   /* observation only */
    if (s.sdma_shadow_back != wptr_bytes) {
        s.sdma_fail_step = kSdmaStepShadowBack;
        r3_wg_arm_substep(WSubstep::Gmc);
        return -1;
    }
    s.sdma_fail_step = kSdmaStepNone;

    /* protected WPTR update (nullarc: MINOR_PTR_UPDATE=1, write, =0) */
    r3_wg_arm_substep(WSubstep::Gmc);
    if (reg_write(s, s.sdma_minor, 1u, kR3WProtocol, ctx) ||
        reg_write(s, s.sdma_wptr, wptr_bytes, kR3WProtocol, ctx) ||
        reg_write(s, s.sdma_minor, 0u, kR3WProtocol, ctx))
        return -1;
    s.sdma_rptr_reg = rptr_reg;
    s.sdma_rptr_before = rptr_bytes;
    s.last_sdma_wptr_bytes = wptr_bytes;
    return 0;
}

/* ---- R10 absolute dword cross-check ----
 * The golden table must resolve the context registers to the same absolute
 * dwords nullarc measured (GC base 0x1260); a mismatch stops the substep and
 * records both sides. */
int cross_check(Gmc &s, const uint32_t *rows, uint32_t vmid)
{
    struct Row {
        uint32_t got;
        uint32_t want;
        const char *name;
    };
    const Row tab[4] = {
        {rows[0], kGmcR10Ctx0CntlAbs + kGmcR10CtxDistance * vmid, "cntl"},
        {rows[1], kGmcR10Ctx0BaseLoAbs + kGmcR10CtxAddrDistance * vmid, "base-lo"},
        {rows[3], kGmcR10Ctx0StartLoAbs + kGmcR10CtxAddrDistance * vmid, "start-lo"},
        {rows[5], kGmcR10Ctx0EndLoAbs + kGmcR10CtxAddrDistance * vmid, "end-lo"},
    };
    for (uint32_t i = 0; i < 4; i++) {
        if (tab[i].got != tab[i].want) {
            evf(s.env, "gmc-cross", "%s: golden=0x%x r10=0x%x",
                tab[i].name, tab[i].got, tab[i].want);
            return -1;
        }
    }
    return 0;
}

/* ---- vmBind register sequence (nullarc R10 §2.3) ----
 * CNTL=0x01fffe07, PDB|3, START=0, END=0xffffffff|0xf, on both hubs.  The
 * flat-GART caller (S7) keeps its own RMW-ed CNTL (DEPTH=0, mesboot gart()
 * :122 0x01fffe01) and passes write_cntl=false. */
int vbind_write(Gmc &s, const uint32_t *regs, uint64_t root, bool write_cntl,
                const char *ctx)
{
    if (write_cntl &&
        reg_write(s, regs[0], kGmcVbindCntl, kR3WState, ctx))
        return -1;
    if (reg_write(s, regs[1], static_cast<uint32_t>((root | kGmcRootFlags) & 0xffffffffu),
                  kR3WState, ctx) ||
        reg_write(s, regs[2], static_cast<uint32_t>(root >> 32), kR3WState, ctx) ||
        reg_write(s, regs[3], kGmcVbindStartLo, kR3WState, ctx) ||
        reg_write(s, regs[4], kGmcVbindStartHi, kR3WState, ctx) ||
        reg_write(s, regs[5], kGmcVbindEndLo, kR3WState, ctx) ||
        reg_write(s, regs[6], kGmcVbindEndHi, kR3WState, ctx))
        return -1;
    return 0;
}

/* ---- S6: MC aperture read-only recheck ---- */

R3SubstepResult pre_uma(const R3Io *io, const R3SubstepEnv *env)
{
    if (!io || !r3_io_complete(io))
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    if (!env || !env->rt || !env->ip)
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    Gmc &s = g_gmc;
    s.io = io;
    s.env = env;
    s.plan = &env->rt->layout;
    s.mc_base = static_cast<uint32_t>(env->fb_mc_base >> 24);
    s.ip = static_cast<const IPBaseTable *>(env->ip);
    if (!s.ip->isResolved(IPBlock::GC, 0) || !s.ip->isResolved(IPBlock::MMHUB, 1))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    if (!g_gmc_alloc || !g_gmc_alloc->alloc)
        return r3_substep_fail(kR3SecPre, 0, kFailNoSysmem);

    struct Row {
        const r3gold::GoldenReg *reg;
        uint32_t Gmc::*field;
    };
    const Row tabs[20] = {
        {&r3gold::kMmmcVmFbOffset, &Gmc::mm_fb_offset},
        {&r3gold::kMmmcVmFbLocationBase, &Gmc::mm_fb_base},
        {&r3gold::kMmmcVmFbLocationTop, &Gmc::mm_fb_top},
        {&r3gold::kMmmcVmAgpTop, &Gmc::mm_agp_top},
        {&r3gold::kMmmcVmAgpBot, &Gmc::mm_agp_bot},
        {&r3gold::kMmmcVmAgpBase, &Gmc::mm_agp_base},
        {&r3gold::kMmmcVmSystemApertureLowAddr, &Gmc::mm_sys_lo},
        {&r3gold::kMmmcVmSystemApertureHighAddr, &Gmc::mm_sys_hi},
        {&r3gold::kMmmcVmSystemApertureDefaultAddrLsb, &Gmc::mm_def_lsb},
        {&r3gold::kMmmcVmSystemApertureDefaultAddrMsb, &Gmc::mm_def_msb},
        {&r3gold::kGcmcVmFbOffset, &Gmc::gc_fb_offset},
        {&r3gold::kGcmcVmFbLocationBase, &Gmc::gc_fb_base},
        {&r3gold::kGcmcVmFbLocationTop, &Gmc::gc_fb_top},
        {&r3gold::kGcmcVmAgpTop, &Gmc::gc_agp_top},
        {&r3gold::kGcmcVmAgpBot, &Gmc::gc_agp_bot},
        {&r3gold::kGcmcVmAgpBase, &Gmc::gc_agp_base},
        {&r3gold::kGcmcVmSystemApertureLowAddr, &Gmc::gc_sys_lo},
        {&r3gold::kGcmcVmSystemApertureHighAddr, &Gmc::gc_sys_hi},
        {&r3gold::kGcmcVmSystemApertureDefaultAddrLsb, &Gmc::gc_def_lsb},
        {&r3gold::kGcmcVmSystemApertureDefaultAddrMsb, &Gmc::gc_def_msb},
    };
    for (const Row &t : tabs)
        if (!s.ip->resolve(t.reg->ref(), &(s.*t.field)))
            return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    return r3_substep_pass();
}

R3SubstepResult seq_mc_aperture(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Read-only recheck of the MC aperture (20 registers, both hubs). The FB
     * base/offset pair is compared against the stage-2 record; the rest is
     * read and logged as evidence. */
    bool ok = false;
    s.mm_fb_offset_raw = reg_read(s, s.mm_fb_offset, &ok);
    if (!ok)
        return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);
    s.gc_fb_offset_raw = reg_read(s, s.gc_fb_offset, &ok);
    if (!ok)
        return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);
    s.mm_fb_base_raw = reg_read(s, s.mm_fb_base, &ok);
    if (!ok)
        return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);
    s.gc_fb_base_raw = reg_read(s, s.gc_fb_base, &ok);
    if (!ok)
        return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);

    const uint32_t want_base = s.mc_base & 0x00ffffffu;
    if ((s.mm_fb_base_raw & 0x00ffffffu) != want_base ||
        (s.gc_fb_base_raw & 0x00ffffffu) != want_base ||
        s.mm_fb_offset_raw != s.gc_fb_offset_raw) {
        evf(s.env, "gmc-uma",
            "aperture mismatch: mm-base=0x%x gc-base=0x%x want=0x%x mm-off=0x%x gc-off=0x%x",
            s.mm_fb_base_raw, s.gc_fb_base_raw, want_base,
            s.mm_fb_offset_raw, s.gc_fb_offset_raw);
        return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);
    }
    /* fb physical base derived from the FB_OFFSET readback (S7/S8/S9 use it
     * to build VRAM page physical addresses). */
    g_gmc_fb_phys_base = static_cast<uint64_t>(s.mm_fb_offset_raw) << 24;

    /* remaining aperture registers: read + evidence */
    struct Row {
        uint32_t Gmc::*field;
        const char *name;
    };
    const Row rest[16] = {
        {&Gmc::mm_fb_top, "mm-fb-top"}, {&Gmc::mm_agp_top, "mm-agp-top"},
        {&Gmc::mm_agp_bot, "mm-agp-bot"}, {&Gmc::mm_agp_base, "mm-agp-base"},
        {&Gmc::mm_sys_lo, "mm-sys-lo"}, {&Gmc::mm_sys_hi, "mm-sys-hi"},
        {&Gmc::mm_def_lsb, "mm-def-lsb"}, {&Gmc::mm_def_msb, "mm-def-msb"},
        {&Gmc::gc_fb_top, "gc-fb-top"}, {&Gmc::gc_agp_top, "gc-agp-top"},
        {&Gmc::gc_agp_bot, "gc-agp-bot"}, {&Gmc::gc_agp_base, "gc-agp-base"},
        {&Gmc::gc_sys_lo, "gc-sys-lo"}, {&Gmc::gc_sys_hi, "gc-sys-hi"},
        {&Gmc::gc_def_lsb, "gc-def-lsb"}, {&Gmc::gc_def_msb, "gc-def-msb"},
    };
    for (const Row &row : rest) {
        uint32_t v = reg_read(s, s.*row.field, &ok);
        if (!ok)
            return r3_substep_fail(kR3SecSeq, 1, kFailMcMismatch);
        evf(s.env, "gmc-uma", "%s=0x%x", row.name, v);
    }
    r3_substep_note(&r, s.mm_fb_base, s.mm_fb_base_raw, "mmhub fb-base");
    r3_substep_note(&r, s.gc_fb_base, s.gc_fb_base_raw, "gc fb-base");
    r3_substep_note(&r, s.mm_fb_offset, s.mm_fb_offset_raw, "mmhub fb-offset");
    r3_substep_note(&r, s.gc_fb_offset, s.gc_fb_offset_raw, "gc fb-offset");
    return r;
}

/* ---- S6: layout registration + sysmem backfill ---- */

int sysmem_alloc_block(Gmc &s, uint32_t index, const char *tag, uint64_t bytes)
{
    void *cpu = nullptr;
    const uint64_t phys = g_gmc_alloc->alloc(g_gmc_alloc->ctx, bytes, &cpu);
    if (!phys || !cpu)
        return -1;
    if (phys & 4095ull)
        return -1;
    g_gmc_sysmem[index].phys = phys;
    g_gmc_sysmem[index].cpu = cpu;
    g_gmc_sysmem[index].bytes = bytes;
    for (uint32_t i = 0; i < s.plan->sysmem_grant_count; i++) {
        R3SysmemGrant &g = s.plan->sysmem_grants[i];
        if (strncmp(g.tag, tag, sizeof(g.tag)) != 0)
            continue;
        g.start = phys;
        g.end = phys + bytes;
        return 0;
    }
    return -1;
}

R3SubstepResult seq_layout(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Register the D4-D8 layout windows so later CPU writes into these
     * regions classify as Data writes (registration carries the IP-level
     * owner; the manifest records the window automatically). D7 test_buf is
     * registered under Gmc: this batch's substeps are its writers (the T-A
     * grant records owner Abi; the deviation is reported in the delivery). */
    struct Row {
        const char *grant_tag;
        const char *wg_tag;
        WSubstep owner;
        bool readonly;
    };
    const Row rows[6] = {
        {"csib", "layout:csib", WSubstep::Gfx, false},
        {"query_fence", "layout:query_fence", WSubstep::Mes, false},
        {"fence_ro", "layout:fence_ro", WSubstep::Abi, true},
        {"test_buf", "layout:test_buf", WSubstep::Gmc, false},
        {"ih_ring", "layout:ih_ring", WSubstep::Ih, false},
        {"ih_rptr_wb", "layout:ih_rptr_wb", WSubstep::Ih, false},
    };
    /* 预算预检：6 条 layout 在注册前整体预留，表满在阶段入口失败
     * （seq:layout-full），而不是逐条 register 返回 -2 折叠成 seq:layout。 */
    if (r3_wg_reserve(kGmcSeqLayoutCount) != 0) {
        evf(s.env, "gmc-uma", "grant budget: need=%u used=%u cap=%u",
            kGmcSeqLayoutCount, r3_wg_grant_count(),
            static_cast<uint32_t>(R3_WG_GRANT_MAX));
        return r3_substep_fail(kR3SecSeq, 2, "seq:layout-full");
    }
    const uint32_t layout_before = r3_wg_grant_count();
    for (const Row &row : rows) {
        const R3Grant *g = r3_layout_grant(s.plan, row.grant_tag);
        if (!g)
            return r3_substep_fail(kR3SecSeq, 2, kFailNoGrant);
        const int rc = row.readonly
            ? r3_wg_register_layout_readonly(static_cast<uint32_t>(g->start >> 2),
                                             static_cast<uint32_t>(g->size >> 2),
                                             row.owner, row.wg_tag)
            : r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                    static_cast<uint32_t>(g->size >> 2),
                                    row.owner, row.wg_tag);
        if (rc == -2) {
            evf(s.env, "gmc-uma", "layout register full: %s (rc=%d)", row.wg_tag, rc);
            return r3_substep_fail(kR3SecSeq, 2, "seq:layout-full");
        }
        if (rc != 0) {
            evf(s.env, "gmc-uma", "layout register failed: %s (rc=%d)", row.wg_tag, rc);
            return r3_substep_fail(kR3SecSeq, 2, kFailLayout);
        }
    }
    if (r3_wg_grant_count() - layout_before != kGmcSeqLayoutCount) {
        evf(s.env, "gmc-uma", "layout increment %u != %u",
            r3_wg_grant_count() - layout_before, kGmcSeqLayoutCount);
        return r3_substep_fail(kR3SecSeq, 2, kFailLayout);
    }

    /* Allocate the three D1-D3 system-memory pools and back-fill the plan
     * (the layout records size/owner only). */
    if (sysmem_alloc_block(s, 0, "gart_table", kGmcGartBytes) ||
        sysmem_alloc_block(s, 1, "ctx0_root_pool", kGmcPtGrantBytes) ||
        sysmem_alloc_block(s, 2, "vmid8_root_pool", kGmcPtGrantBytes)) {
        evf(s.env, "gmc-uma", "sysmem allocation failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailNoSysmem);
    }
    /* Re-validate the plan with the back-filled sysmem grants. */
    const char *why = nullptr;
    if (!r3_layout_validate(s.plan, &why)) {
        evf(s.env, "gmc-uma", "validate failed: %s", why ? why : "?");
        return r3_substep_fail(kR3SecSeq, 2, kFailLayout);
    }
    r3_substep_note(&r, 0, 0, "layout registered, sysmem back-filled");
    return r;
}

/* ---- S7: flat GART ---- */

R3SubstepResult pre_gart(const R3Io *io, const R3SubstepEnv *env)
{
    if (!io || !r3_io_complete(io))
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    if (!env || !env->rt || !env->ip)
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    Gmc &s = g_gmc;
    s.io = io;
    s.env = env;
    s.plan = &env->rt->layout;
    s.mc_base = static_cast<uint32_t>(env->fb_mc_base >> 24);
    s.ip = static_cast<const IPBaseTable *>(env->ip);
    if (!s.ip->isResolved(IPBlock::GC, 0) || !s.ip->isResolved(IPBlock::MMHUB, 1) ||
        !s.ip->isResolved(IPBlock::SDMA0, 0))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    s.sdma_grant = r3_layout_grant(s.plan, "sdma");
    s.test_grant = r3_layout_grant(s.plan, "test_buf");
    if (!s.sdma_grant || !s.test_grant)
        return r3_substep_fail(kR3SecPre, 0, kFailNoGrant);
    if (g_gmc_sysmem[0].phys == 0 || !g_gmc_sysmem[0].cpu)
        return r3_substep_fail(kR3SecPre, 0, kFailNoSysmem);
    s.gart_blk = &g_gmc_sysmem[0];
    if (g_gmc_fb_phys_base == 0)
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);

    /* 预算预检：21 条 GART 行整体预留；表满在阶段入口失败
     * （pre:register-full），不再逐条 register 折叠成 pre:register。 */
    if (r3_wg_reserve(kGartRowCount) != 0) {
        evf(s.env, "gmc-gart", "grant budget: need=%u used=%u cap=%u",
            kGartRowCount, r3_wg_grant_count(),
            static_cast<uint32_t>(R3_WG_GRANT_MAX));
        R3SubstepResult fr = r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
        r3_substep_note(&fr, kGartRowCount, r3_wg_grant_count(), "gart need/used");
        return fr;
    }
    const uint32_t grant_before = r3_wg_grant_count();
    /* register the write windows and resolve the row addresses */
    for (uint32_t i = 0; i < kGartRowCount; i++) {
        uint32_t addr = 0;
        if (!row_addr(s.ip, kGartRows[i], &addr))
            return r3_substep_fail(kR3SecPre, 0, kFailRegister);
        s.rows_addr[i] = addr;
        const int rc = r3_wg_register_reg(addr, kGartRows[i].dwords, kGartRows[i].cls,
                                          WSubstep::Gmc, "reg:gmc");
        if (rc == -2) {
            evf(s.env, "gmc-gart", "register full: row %u rc=%d", i, rc);
            return r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
        }
        if (rc != 0)
            return r3_substep_fail(kR3SecPre, 0, kFailRegister);
    }
    if (r3_wg_grant_count() - grant_before != kGartRowCount) {
        evf(s.env, "gmc-gart", "register increment %u != %u",
            r3_wg_grant_count() - grant_before, kGartRowCount);
        return r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
    }
    s.gc_inv_req = s.rows_addr[14];
    s.mm_inv_sem = s.rows_addr[15];
    s.mm_inv_req = s.rows_addr[16];
    s.sdma_wptr = s.rows_addr[17];
    s.sdma_minor = s.rows_addr[18];
    if (resolve_inv_addrs(s))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    return r3_substep_pass();
}

R3SubstepResult seq_gart_build(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Flat GART table in system memory: zero the whole 4 MiB, flush every
     * cache line (the GPU walker may not snoop CPU caches), then map the D7
     * test-buffer pages (VRAM PA | 0x71, nullarc mesboot gart() shape). */
    GmcSysmemBlock *blk = s.gart_blk;
    memset(blk->cpu, 0, kGmcGartBytes);
    flush_region(blk->cpu, kGmcGartBytes);
    gmc_mb();

    const uint64_t test_pa = g_gmc_fb_phys_base + s.test_grant->start;
    volatile uint64_t *tab = static_cast<volatile uint64_t *>(blk->cpu);
    for (uint32_t i = 0; i < kGmcGartTestPages; i++) {
        const uint32_t idx = gmc_gart_index(kGmcTestVaGart + i * 4096ull);
        tab[idx] = gmc_gart_entry_vram(test_pa + i * 4096ull);
        gmc_pt_flush_line(&tab[idx]);
    }
    gmc_mb();
    r3_substep_note(&r, 0, 0, "gart table built (test buffer mapped)");
    return r;
}

R3SubstepResult seq_gart_regs(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Linux gart_enable's program_invalidation runs before the invalidate
     * requests; the ranges persist, so this is the one place that arms them. */
    if (arm_invalidation(s, "gmc:inv-arm"))
        return r3_substep_fail(kR3SecSeq, 2, kFailGartRegs);
    /* VMID0 flat GART configuration on both hubs (Linux gfxhub_v3_0.c
     * enable_system_domain shape: RMW the CNTL keeping DEPTH=0/RETRY off,
     * then the table base and the full-VA START/END from the nullarc
     * mesboot gart() measurements). */
    const uint64_t root = s.gart_blk->phys;
    for (uint32_t hub = 0; hub < 2; hub++) {
        const uint32_t *regs = s.rows_addr + (hub == 0 ? 0 : 7);
        const char *ctx = hub == 0 ? "gmc:ctx0" : "gmc:mm-ctx0";
        bool ok = false;
        uint32_t cntl = reg_read(s, regs[0], &ok);
        if (!ok)
            return r3_substep_fail(kR3SecSeq, 2, kFailGartRegs);
        cntl &= ~(0x6u | 0x80u); /* PAGE_TABLE_DEPTH=0, RETRY_PERMISSION_OR_INVALID=0 */
        cntl |= 0x1u;            /* ENABLE_CONTEXT=1 */
        if (reg_write(s, regs[0], cntl, kR3WState, ctx) ||
            vbind_write(s, regs, root, false, ctx))
            return fail_note(kR3SecSeq, 2, kFailGartRegs, regs[0], cntl, ctx);
    }
    r3_substep_note(&r, s.rows_addr[0], 0, "gart regs ok");
    return r;
}

R3SubstepResult seq_gart_invalidate(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    if (invalidate_hubs(s, 0, "gmc:gart-inv"))
        return fail_note(kR3SecSeq, 3, kFailInvalidate, 0u, 0u, "invalidate failed");
    r3_substep_note(&r, 0, 0, "gart invalidate ok");
    return r;
}

R3SubstepResult seq_gart_verify(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Gate: clear the D7 test page through BAR0, let SDMA write it through
     * the GART VA, read the value back (nullarc E4a shape). */
    if (zero_region(s, s.test_grant->start, 4096u, "gmc:gart-clear") ||
        sdma_submit(s, kGmcTestVaGart + kGmcTestPageIntra, kGmcTestMagic, "gmc:gart-sdma")) {
        /* Prep failure: the submit-side verdict (which step failed, what the
         * readback returned) plus the queue state that separates H5 from G.
         * Eight slots, order = priority; r3_substep_note drops overflow. */
        R3SubstepResult f = r3_substep_fail(kR3SecSeq, 4, kFailGartVerify);
        const uint32_t base = s.ip->getBase(IPBlock::SDMA0, 0);
        const uint32_t f32_base = s.ip->getBase(IPBlock::SDMA0, 1);
        bool ok = false;
        const uint32_t rb_wptr = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0Queue0RbWptr, &ok);
        const uint32_t rb_cntl = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0Queue0RbCntl, &ok);
        const uint32_t f32_cntl = f32_base == 0xffffffffu
            ? 0u : reg_read(s, f32_base + kRegSdma0F32Cntl, &ok);
        const uint32_t rb_rptr = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0Queue0RbRptr, &ok);
        const uint32_t status = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0StatusReg, &ok);

        r3_substep_note(&f, 0u, s.sdma_fail_step, "sdma submit step");
        r3_substep_note(&f, 0u, s.sdma_pkt_back, "sdma pkt back");
        r3_substep_note(&f, 0u, s.sdma_shadow_back, "sdma wptr shadow");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0Queue0RbWptr,
                        rb_wptr, "sdma rb wptr");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0Queue0RbCntl,
                        rb_cntl, "sdma rb cntl");
        r3_substep_note(&f, f32_base == 0xffffffffu ? 0u : f32_base + kRegSdma0F32Cntl,
                        f32_cntl, "sdma f32 cntl");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0Queue0RbRptr,
                        rb_rptr, "sdma rb rptr");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0StatusReg,
                        status, "sdma status");

        evf(s.env, "gmc-gart", "verify-prep fail: sdma step=%u", s.sdma_fail_step);
        evf(s.env, "gmc-gart", "diag sdma pkt back=0x%x", s.sdma_pkt_back);
        evf(s.env, "gmc-gart", "diag sdma shadow back=0x%x", s.sdma_shadow_back);
        evf(s.env, "gmc-gart", "diag sdma status=0x%x", status);
        evf(s.env, "gmc-gart", "diag sdma shadow hi=0x%x", s.sdma_shadow_hi);
        return f;
    }
    uint32_t got = 0;
    const int hit = poll_bus(s, s.test_grant->start + kGmcTestPageIntra, 0xffffffffu,
                             kGmcTestMagic, kGmcSdmaPolls, kGmcSdmaPollUs, &got);
    if (hit != 1 || got != kGmcTestMagic) {
        /* r3_substep_fail() clears reading_count: build the failed result
         * before attaching the snapshot. */
        R3SubstepResult f = r3_substep_fail(kR3SecChk, 4, kFailGartVerify);
        /* W3: submit-side facts. RB_RPTR is a byte offset; the dword index is
         * the derived value (Linux sdma_v6_0.c ring_get_rptr >> 2). */
        bool rptr_ok = false;
        const uint32_t rptr_now = s.sdma_rptr_reg
                                      ? reg_read(s, s.sdma_rptr_reg, &rptr_ok)
                                      : 0u;
        evf(s.env, "gmc-gart",
            "verify-fail: sdma rptr%s=0x%x (dword=0x%x) before=0x%x doorbell-wptr=0x%x",
            rptr_ok ? "" : "(unreadable)", rptr_now, rptr_now >> 2u,
            s.sdma_rptr_before, s.last_sdma_wptr_bytes);

        r3_substep_note(&f, static_cast<uint32_t>(s.test_grant->start + kGmcTestPageIntra),
                        got, "gart verify mismatch");

        /* Eight slots are a fixed snapshot (rdna3_substep.h): the four poll
         * verdicts first, then the four publish-side observations. */
        bool diag_ok = false;
        const uint32_t fault_status = r3_io_read32(s.io, 0x1a000u + 0x070Cu, &diag_ok);
        r3_substep_note(&f, 0x1a000u + 0x070Cu, fault_status,
                        "gart-diag:mmhub-l2-fault-status");
        const uint32_t fault_cntl = r3_io_read32(s.io, 0x1a000u + 0x0708u, &diag_ok);
        r3_substep_note(&f, 0x1a000u + 0x0708u, fault_cntl,
                        "gart-diag:mmhub-l2-fault-cntl");
        r3_substep_note(&f, s.sdma_rptr_reg, rptr_now, "sdma rptr after poll");

        /* Publish-side observations for a polling failure: did the write land
         * in BAR, and is the queue enabled / not halted? Slots 5-8 (index 4..7). */
        uint64_t sh = 0ull;
        uint32_t shadow = 0u;
        if (s.io->bus &&
            s.io->bus(s.io->ctx, R3_IO_BAR_VRAM,
                      s.sdma_grant->start + kGmcSdmaWptrShadowOff,
                      &sh, false, 2u)) {
            shadow = static_cast<uint32_t>(sh);
            s.sdma_shadow_hi = static_cast<uint32_t>(sh >> 32);
        }
        const uint32_t base = s.ip->getBase(IPBlock::SDMA0, 0);
        const uint32_t f32_base = s.ip->getBase(IPBlock::SDMA0, 1);
        bool sdma_ok = false;
        const uint32_t rb_wptr = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0Queue0RbWptr, &sdma_ok);
        const uint32_t rb_cntl = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0Queue0RbCntl, &sdma_ok);
        const uint32_t f32_cntl = f32_base == 0xffffffffu
            ? 0u : reg_read(s, f32_base + kRegSdma0F32Cntl, &sdma_ok);
        const uint32_t status = base == 0xffffffffu
            ? 0u : reg_read(s, base + kRegSdma0StatusReg, &sdma_ok);

        r3_substep_note(&f,
                        static_cast<uint32_t>(s.sdma_grant->start + kGmcSdmaWptrShadowOff),
                        shadow, "sdma wptr shadow");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0Queue0RbWptr,
                        rb_wptr, "sdma rb wptr");
        r3_substep_note(&f, base == 0xffffffffu ? 0u : base + kRegSdma0Queue0RbCntl,
                        rb_cntl, "sdma rb cntl");
        r3_substep_note(&f, f32_base == 0xffffffffu ? 0u : f32_base + kRegSdma0F32Cntl,
                        f32_cntl, "sdma f32 cntl");
        evf(s.env, "gmc-gart", "diag sdma status=0x%x", status);
        evf(s.env, "gmc-gart", "diag sdma shadow hi=0x%x", s.sdma_shadow_hi);

        /* Remaining diagnostic set: process log only (no snapshot slots). */
        const uint32_t mx_l1 = r3_io_read32(s.io, 0x1a000u + 0x08F3u, &diag_ok);
        evf(s.env, "gmc-gart", "diag mmhub-mx-l1=0x%x", mx_l1);
        const uint32_t l2_cntl = r3_io_read32(s.io, 0x1a000u + 0x0700u, &diag_ok);
        evf(s.env, "gmc-gart", "diag mmhub-l2-cntl=0x%x", l2_cntl);
        const uint32_t fault_status_legacy = r3_io_read32(s.io, 0x1a000u + 0x070Au, &diag_ok);
        evf(s.env, "gmc-gart", "diag mmhub-l2-0x070a=0x%x", fault_status_legacy);
        const uint32_t mm_fb_off = r3_io_read32(s.io, 0x1a000u + 0x08D7u, &diag_ok);
        evf(s.env, "gmc-gart", "diag mmhub-fb-offset=0x%x", mm_fb_off);
        const uint32_t gc_fb_off = r3_io_read32(s.io, 0x10000u + 0x15A7u, &diag_ok);
        evf(s.env, "gmc-gart", "diag gfxhub-fb-offset=0x%x", gc_fb_off);
        {
            volatile const uint64_t *tab =
                static_cast<volatile const uint64_t *>(g_gmc_sysmem[0].cpu);
            evf(s.env, "gmc-gart", "diag pte0-lo=0x%x",
                static_cast<uint32_t>(tab[0] & 0xffffffffu));
            evf(s.env, "gmc-gart", "diag pte0-hi=0x%x",
                static_cast<uint32_t>(tab[0] >> 32u));
        }
        const uint32_t def_lsb = r3_io_read32(s.io, 0x1a000u + 0x08D8u, &diag_ok);
        const uint32_t def_msb = r3_io_read32(s.io, 0x1a000u + 0x08D9u, &diag_ok);
        evf(s.env, "gmc-gart", "diag mmhub-default-addr=0x%x%08x", def_msb, def_lsb);
        return f;
    }
    r3_substep_note(&r, static_cast<uint32_t>(s.test_grant->start + kGmcTestPageIntra),
                    got, "gart verify ok");
    return r;
}

/* ---- S8: VMID0 four-level GPUVM ---- */

R3SubstepResult pre_gpuvm0(const R3Io *io, const R3SubstepEnv *env)
{
    if (!io || !r3_io_complete(io))
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    if (!env || !env->rt || !env->ip)
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    Gmc &s = g_gmc;
    s.io = io;
    s.env = env;
    s.plan = &env->rt->layout;
    s.mc_base = static_cast<uint32_t>(env->fb_mc_base >> 24);
    s.ip = static_cast<const IPBaseTable *>(env->ip);
    if (!s.ip->isResolved(IPBlock::GC, 0) || !s.ip->isResolved(IPBlock::MMHUB, 1) ||
        !s.ip->isResolved(IPBlock::SDMA0, 0))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    s.sdma_grant = r3_layout_grant(s.plan, "sdma");
    s.test_grant = r3_layout_grant(s.plan, "test_buf");
    if (!s.sdma_grant || !s.test_grant)
        return r3_substep_fail(kR3SecPre, 0, kFailNoGrant);
    if (g_gmc_sysmem[1].phys == 0 || !g_gmc_sysmem[1].cpu)
        return r3_substep_fail(kR3SecPre, 0, kFailNoSysmem);
    s.ctx0_blk = &g_gmc_sysmem[1];
    if (g_gmc_fb_phys_base == 0)
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);

    /* the write windows were registered by S7 (same addresses, same owner);
     * resolve the row addresses for the expect set */
    for (uint32_t i = 0; i < kGartRowCount; i++) {
        uint32_t addr = 0;
        if (!row_addr(s.ip, kGartRows[i], &addr))
            return r3_substep_fail(kR3SecPre, 0, kFailRegister);
        s.rows_addr[i] = addr;
    }
    s.gc_inv_req = s.rows_addr[14];
    s.mm_inv_sem = s.rows_addr[15];
    s.mm_inv_req = s.rows_addr[16];
    s.sdma_wptr = s.rows_addr[17];
    s.sdma_minor = s.rows_addr[18];
    if (resolve_inv_addrs(s))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    return r3_substep_pass();
}

R3SubstepResult seq_pt_build0(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Four-level page table for VMID0: root 4 KiB at the grant start, then a
     * 4 MiB pool of 4 KiB pages the walk hands out on demand. */
    GmcSysmemBlock *blk = s.ctx0_blk;
    memset(blk->cpu, 0, kGmcPtGrantBytes);
    flush_region(blk->cpu, kGmcPtGrantBytes);
    gmc_mb();

    GmcPtPool pool;
    gmc_pt_pool_init(&pool, blk->phys, blk->cpu);
    const uint64_t page_pa = g_gmc_fb_phys_base + s.test_grant->start + 4096ull;
    if (gmc_pt_map(&pool, kGmcTestVaGpuvm, page_pa, kGmcPteVram)) {
        r3_substep_note(&r, 0, 0, "pt map failed");
        return r3_substep_fail(kR3SecSeq, 1, kFailPtBuild);
    }
    r3_substep_note(&r, 0, 0, "gpuvm0 page table built");
    return r;
}

R3SubstepResult seq_vbind0(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* nullarc vmBind register sequence (R10 §2.3) on both hubs, with the
     * absolute dword cross-check against the R10 measurements. */
    if (cross_check(s, s.rows_addr, 0)) {
        r3_substep_note(&r, 0, 0, "r10 cross-check failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailCrossCheck);
    }
    const uint64_t root = s.ctx0_blk->phys;
    for (uint32_t hub = 0; hub < 2; hub++) {
        const uint32_t *regs = s.rows_addr + (hub == 0 ? 0 : 7);
        const char *ctx = hub == 0 ? "gmc:ctx0" : "gmc:mm-ctx0";
        if (vbind_write(s, regs, root, true, ctx)) {
            r3_substep_note(&r, regs[0], 0, ctx);
            return r3_substep_fail(kR3SecSeq, 2, kFailVbind);
        }
    }
    r3_substep_note(&r, s.rows_addr[0], kGmcVbindCntl, "vmid0 bound");
    return r;
}

R3SubstepResult seq_gpuvm_invalidate(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    if (invalidate_hubs(s, 0, "gmc:gpuvm-inv")) {
        r3_substep_note(&r, 0, 0, "invalidate failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailInvalidate);
    }
    r3_substep_note(&r, 0, 0, "gpuvm0 invalidate ok");
    return r;
}

R3SubstepResult seq_gpuvm_verify(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Gate: clear the D7 test page through BAR0, let SDMA write it through
     * the VMID0 GPUVM path, read the value back. */
    if (zero_region(s, s.test_grant->start + 4096u, 4096u, "gmc:gpuvm-clear") ||
        sdma_submit(s, kGmcTestVaGpuvm + kGmcTestPageIntra, kGmcTestMagic, "gmc:gpuvm-sdma")) {
        r3_substep_note(&r, 0, 0, "gpuvm verify prep failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailGpuvmVerify);
    }
    uint32_t got = 0;
    const int hit = poll_bus(s, s.test_grant->start + 4096u + kGmcTestPageIntra, 0xffffffffu,
                             kGmcTestMagic, kGmcSdmaPolls, kGmcSdmaPollUs, &got);
    if (hit != 1 || got != kGmcTestMagic) {
        r3_substep_note(&r, static_cast<uint32_t>(s.test_grant->start + 4096u + kGmcTestPageIntra),
                        got, "gpuvm verify mismatch");
        return r3_substep_fail(kR3SecChk, 4, kFailGpuvmVerify);
    }
    r3_substep_note(&r, static_cast<uint32_t>(s.test_grant->start + 4096u + kGmcTestPageIntra),
                    got, "gpuvm verify ok");
    return r;
}

/* ---- S9: VMID8 ---- */

R3SubstepResult pre_vmid8(const R3Io *io, const R3SubstepEnv *env)
{
    if (!io || !r3_io_complete(io))
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    if (!env || !env->rt || !env->ip)
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    Gmc &s = g_gmc;
    s.io = io;
    s.env = env;
    s.plan = &env->rt->layout;
    s.mc_base = static_cast<uint32_t>(env->fb_mc_base >> 24);
    s.ip = static_cast<const IPBaseTable *>(env->ip);
    if (!s.ip->isResolved(IPBlock::GC, 0) || !s.ip->isResolved(IPBlock::MMHUB, 1))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    s.test_grant = r3_layout_grant(s.plan, "test_buf");
    if (!s.test_grant)
        return r3_substep_fail(kR3SecPre, 0, kFailNoGrant);
    if (g_gmc_sysmem[2].phys == 0 || !g_gmc_sysmem[2].cpu)
        return r3_substep_fail(kR3SecPre, 0, kFailNoSysmem);
    s.vmid8_blk = &g_gmc_sysmem[2];
    if (g_gmc_fb_phys_base == 0)
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    /* 预算预检：S9 只注册前 15 行（15..17 由 S7 注册）；表满在阶段入口失败
     * （pre:register-full），不再逐条 register 折叠成 pre:register。 */
    if (r3_wg_reserve(kVmid8FirstInvalidateRow) != 0) {
        evf(s.env, "gmc-vmid8", "grant budget: need=%u used=%u cap=%u",
            kVmid8FirstInvalidateRow, r3_wg_grant_count(),
            static_cast<uint32_t>(R3_WG_GRANT_MAX));
        return r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
    }
    const uint32_t grant_before = r3_wg_grant_count();
    /* register the VM_CONTEXT8 + fault-default windows (rows 15..17 are the
     * invalidate trio S7 already registered) and resolve the row addresses */
    for (uint32_t i = 0; i < kVmid8RowCount; i++) {
        uint32_t addr = 0;
        if (!row_addr(s.ip, kVmid8Rows[i], &addr))
            return r3_substep_fail(kR3SecPre, 0, kFailRegister);
        s.rows_addr[i] = addr;
        if (i < kVmid8FirstInvalidateRow) {
            const int rc = r3_wg_register_reg(addr, kVmid8Rows[i].dwords,
                                              kVmid8Rows[i].cls,
                                              WSubstep::Gmc, "reg:gmc");
            if (rc == -2) {
                evf(s.env, "gmc-vmid8", "register full: row %u rc=%d", i, rc);
                return r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
            }
            if (rc != 0)
                return r3_substep_fail(kR3SecPre, 0, kFailRegister);
        }
    }
    if (r3_wg_grant_count() - grant_before != kVmid8FirstInvalidateRow) {
        evf(s.env, "gmc-vmid8", "register increment %u != %u",
            r3_wg_grant_count() - grant_before, kVmid8FirstInvalidateRow);
        return r3_substep_fail(kR3SecPre, 0, kFailRegisterFull);
    }
    s.gc_inv_req = s.rows_addr[kVmid8FirstInvalidateRow];
    s.mm_inv_sem = s.rows_addr[kVmid8FirstInvalidateRow + 1];
    s.mm_inv_req = s.rows_addr[kVmid8FirstInvalidateRow + 2];
    if (resolve_inv_addrs(s))
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    return r3_substep_pass();
}

R3SubstepResult seq_pt_build8(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Four-level page table for VMID 8 (D3 pool), same shape as S8; the
     * mapped VRAM page is S11 preparation (no GPU-side gate here). */
    GmcSysmemBlock *blk = s.vmid8_blk;
    memset(blk->cpu, 0, kGmcPtGrantBytes);
    flush_region(blk->cpu, kGmcPtGrantBytes);
    gmc_mb();

    GmcPtPool pool;
    gmc_pt_pool_init(&pool, blk->phys, blk->cpu);
    const uint64_t page_pa = g_gmc_fb_phys_base + s.test_grant->start + 2 * 4096ull;
    if (gmc_pt_map(&pool, kGmcTestVaVmid8, page_pa, kGmcPteVram)) {
        r3_substep_note(&r, 0, 0, "pt map failed");
        return r3_substep_fail(kR3SecSeq, 1, kFailPtBuild);
    }
    r3_substep_note(&r, 0, 0, "vmid8 page table built");
    return r;
}

R3SubstepResult seq_vbind8(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* nullarc vmBind register sequence for VMID 8, with the absolute dword
     * cross-check against the R10 vmid=8 measurements. */
    if (cross_check(s, s.rows_addr, 8)) {
        r3_substep_note(&r, 0, 0, "r10 cross-check failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailCrossCheck);
    }
    const uint64_t root = s.vmid8_blk->phys;
    for (uint32_t hub = 0; hub < 2; hub++) {
        const uint32_t *regs = s.rows_addr + (hub == 0 ? 0 : 7);
        const char *ctx = hub == 0 ? "gmc:ctx8" : "gmc:mm-ctx8";
        if (vbind_write(s, regs, root, true, ctx)) {
            r3_substep_note(&r, regs[0], 0, ctx);
            return r3_substep_fail(kR3SecSeq, 2, kFailVbind);
        }
    }
    r3_substep_note(&r, s.rows_addr[0], kGmcVbindCntl, "vmid8 bound");
    return r;
}

R3SubstepResult seq_vmid8_invalidate(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* PER_VMID invalidate (gfxhub_v3_0.c:54-72 bit layout: 1 << vmid). */
    if (invalidate_hubs(s, 8, "gmc:vmid8-inv")) {
        r3_substep_note(&r, 0, 0, "invalidate failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailInvalidate);
    }
    r3_substep_note(&r, 0, 0, "vmid8 invalidate ok");
    return r;
}

R3SubstepResult seq_fault_default(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Test-window FAULT_DEFAULT (mesboot env_on("FAULT_DEFAULT") measured
     * value: all seven protection-fault enable-default bits set). */
    if (reg_write(s, s.rows_addr[14], kGmcFaultDefault, kR3WState, "gmc:fault-default")) {
        r3_substep_note(&r, 0, 0, "fault default write failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailFaultDefault);
    }
    r3_substep_note(&r, s.rows_addr[14], kGmcFaultDefault, "fault default set");
    return r;
}

R3SubstepResult chk_vmid8(Gmc &s)
{
    R3SubstepResult r = r3_substep_pass();
    /* Register-level gate: read back the VM_CONTEXT8 group on both hubs and
     * the fault-default register (IB validation is an S11 add-on). */
    bool ok = false;
    struct Row {
        uint32_t abs;
        uint32_t want;
        const char *name;
    };
    const uint64_t root = s.vmid8_blk->phys;
    const Row gc_rows[4] = {
        {s.rows_addr[0], kGmcVbindCntl, "gc-ctx8-cntl"},
        {s.rows_addr[1], static_cast<uint32_t>((root | kGmcRootFlags) & 0xffffffffu), "gc-ctx8-base-lo"},
        {s.rows_addr[3], kGmcVbindStartLo, "gc-ctx8-start-lo"},
        {s.rows_addr[5], kGmcVbindEndLo, "gc-ctx8-end-lo"},
    };
    const Row mm_rows[4] = {
        {s.rows_addr[7], kGmcVbindCntl, "mm-ctx8-cntl"},
        {s.rows_addr[8], static_cast<uint32_t>((root | kGmcRootFlags) & 0xffffffffu), "mm-ctx8-base-lo"},
        {s.rows_addr[10], kGmcVbindStartLo, "mm-ctx8-start-lo"},
        {s.rows_addr[12], kGmcVbindEndLo, "mm-ctx8-end-lo"},
    };
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t got = reg_read(s, gc_rows[i].abs, &ok);
        if (!ok || got != gc_rows[i].want) {
            r3_substep_note(&r, gc_rows[i].abs, got, gc_rows[i].name);
            return r3_substep_fail(kR3SecChk, 5, kFailVmid8Chk);
        }
    }
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t got = reg_read(s, mm_rows[i].abs, &ok);
        if (!ok || got != mm_rows[i].want) {
            r3_substep_note(&r, mm_rows[i].abs, got, mm_rows[i].name);
            return r3_substep_fail(kR3SecChk, 5, kFailVmid8Chk);
        }
    }
    const uint32_t fault = reg_read(s, s.rows_addr[14], &ok);
    if (!ok || fault != kGmcFaultDefault) {
        r3_substep_note(&r, s.rows_addr[14], fault, "fault-default");
        return r3_substep_fail(kR3SecChk, 5, kFailVmid8Chk);
    }
    r3_substep_note(&r, s.rows_addr[14], fault, "vmid8 gate ok");
    return r;
}

/* ---- substep entries ---- */

R3SubstepResult run_uma(const R3Io *io, const R3SubstepEnv *env)
{
    memset(&g_gmc, 0, sizeof(g_gmc));
    r3_wg_arm_substep(WSubstep::Gmc);
    r3_wg_expect_writes(nullptr, 0, nullptr, 0, WSubstep::Gmc);

    R3SubstepResult r = pre_uma(io, env);
    if (r.status == kR3SsecOk)
        r = seq_mc_aperture(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_layout(g_gmc);

    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, 0, 0, "uma: aperture rechecked, layout registered, sysmem back-filled");
        set_prop(env, "N48R3EVID-gmc-uma-done", "Yes");
    } else {
        evf(env, "gmc-uma", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
        r3_wg_set_path("failure");
    }

    if (r3_wg_verify_and_clear(env, 0))
        r = r3_substep_fail(kR3SecSeq, 0, kFailExpectMismatch);
    r3_wg_disarm_substep();
    return r;
}

R3SubstepResult run_gart(const R3Io *io, const R3SubstepEnv *env)
{
    memset(&g_gmc, 0, sizeof(g_gmc));
    r3_wg_arm_substep(WSubstep::Gmc);
    if (r3_wg_expect_writes(kGartRows, kGartRowCount, nullptr, 0, WSubstep::Gmc) != 0)
        return r3_substep_fail(kR3SecPre, 0, kFailExpectCap);

    R3SubstepResult r = pre_gart(io, env);
    if (r.status == kR3SsecOk &&
        r3_wg_expect_declared_addrs(g_gmc.rows_addr, kGartRowCount) != 0)
        r = r3_substep_fail(kR3SecPre, 0, kFailExpectCap);
    if (r.status == kR3SsecOk)
        r = seq_gart_build(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_gart_regs(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_gart_invalidate(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_gart_verify(g_gmc);

    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, 0, 0, "gart: table built, VMID0 flat, invalidate ok, SDMA verify ok");
        set_prop(env, "N48R3EVID-gmc-gart-done", "Yes");
    } else {
        evf(env, "gmc-gart", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
        r3_wg_set_path("failure");
    }

    if (r3_wg_verify_and_clear(env, kGartRowCount))
        r = r3_substep_fail(kR3SecSeq, 0, kFailExpectMismatch);
    r3_wg_disarm_substep();
    return r;
}

R3SubstepResult run_gpuvm0(const R3Io *io, const R3SubstepEnv *env)
{
    memset(&g_gmc, 0, sizeof(g_gmc));
    r3_wg_arm_substep(WSubstep::Gmc);
    /* S8 re-walks the vmBind set only: the arm rows belong to S7 and are
     * already armed (the registers persist across substeps). */
    if (r3_wg_expect_writes(kGartRows, kGartVbindRowCount, nullptr, 0, WSubstep::Gmc) != 0)
        return r3_substep_fail(kR3SecPre, 0, kFailExpectCap);

    R3SubstepResult r = pre_gpuvm0(io, env);
    if (r.status == kR3SsecOk &&
        r3_wg_expect_declared_addrs(g_gmc.rows_addr, kGartVbindRowCount) != 0)
        r = r3_substep_fail(kR3SecPre, 0, kFailExpectCap);
    if (r.status == kR3SsecOk)
        r = seq_pt_build0(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_vbind0(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_gpuvm_invalidate(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_gpuvm_verify(g_gmc);

    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, 0, 0, "gpuvm0: table built, vmid0 bound, invalidate ok, SDMA verify ok");
        set_prop(env, "N48R3EVID-gmc-gpuvm0-done", "Yes");
    } else {
        evf(env, "gmc-gpuvm0", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
        r3_wg_set_path("failure");
    }

    if (r3_wg_verify_and_clear(env, kGartVbindRowCount))
        r = r3_substep_fail(kR3SecSeq, 0, kFailExpectMismatch);
    r3_wg_disarm_substep();
    return r;
}

R3SubstepResult run_vmid8(const R3Io *io, const R3SubstepEnv *env)
{
    memset(&g_gmc, 0, sizeof(g_gmc));
    r3_wg_arm_substep(WSubstep::Gmc);
    if (r3_wg_expect_writes(kVmid8Rows, kVmid8RowCount, nullptr, 0, WSubstep::Gmc) != 0)
        return r3_substep_fail(kR3SecPre, 0, kFailExpectCap);

    R3SubstepResult r = pre_vmid8(io, env);
    if (r.status == kR3SsecOk &&
        r3_wg_expect_declared_addrs(g_gmc.rows_addr, kVmid8RowCount) != 0)
        r = r3_substep_fail(kR3SecPre, 0, kFailExpectCap);
    if (r.status == kR3SsecOk)
        r = seq_pt_build8(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_vbind8(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_vmid8_invalidate(g_gmc);
    if (r.status == kR3SsecOk)
        r = seq_fault_default(g_gmc);
    if (r.status == kR3SsecOk)
        r = chk_vmid8(g_gmc);

    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, 0, 0, "vmid8: table built, vmid8 bound, invalidate ok, gate ok");
        set_prop(env, "N48R3EVID-gmc-vmid8-done", "Yes");
    } else {
        evf(env, "gmc-vmid8", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
        r3_wg_set_path("failure");
    }

    if (r3_wg_verify_and_clear(env, kVmid8RowCount))
        r = r3_substep_fail(kR3SecSeq, 0, kFailExpectMismatch);
    r3_wg_disarm_substep();
    return r;
}

} /* namespace */

namespace {
struct GttBlock {
    uint64_t phys;
    void    *cpu;
    uint32_t pages;
    bool     used;
};
constexpr uint32_t kGmcMaxGttBlocks = 128u;  /* 64 ABI BOs + recycle headroom */
GttBlock g_gtt_blocks[kGmcMaxGttBlocks] = {};
uint32_t g_gtt_block_n = 0;
} /* namespace */
/* ---- sysmem allocation binding (T-E / host tests) ---- */

int gmc_v11_0_bind_sysmem(const GmcSysmemAlloc *alloc)
{
    g_gmc_alloc = alloc;
    memset(g_gmc_sysmem, 0, sizeof(g_gmc_sysmem));
    g_gmc_fb_phys_base = 0;
    memset(g_gtt_blocks, 0, sizeof(g_gtt_blocks));
    return 0;
}

/* ---- stage-3 GTT BO helpers (R1 ABI / A-07 F-03) --------------------------
 * The ABI allocator asks for physically contiguous, 4KiB-aligned sysmem pages
 * through the same GmcSysmemAlloc the stage ladder's GART-table setup binds,
 * then programs GART PTEs for the returned VA range via the existing table
 * (gmc_gart_entry_system + flush), so no second address authority appears.
 * The allocator has no free callback (see GmcSysmemAlloc), so freed blocks
 * are accounted on a small recycle stack instead of being returned. */


uint64_t gmc_v11_0_sysmem_alloc_pages(uint32_t pages, void **cpu)
{
    if (pages == 0 || cpu == nullptr || g_gmc_alloc == nullptr ||
        g_gmc_alloc->alloc == nullptr)
        return 0;
    /* Recycle a previously freed block of at least this size first. */
    for (uint32_t i = 0; i < g_gtt_block_n; i++) {
        GttBlock &b = g_gtt_blocks[i];
        if (!b.used && b.pages >= pages) {
            b.used = true;
            *cpu = b.cpu;
            return b.phys;
        }
    }
    if (g_gtt_block_n >= kGmcMaxGttBlocks)
        return 0;
    void *mapped = nullptr;
    const uint64_t phys = g_gmc_alloc->alloc(g_gmc_alloc->ctx,
                                             static_cast<uint64_t>(pages) * 4096ull,
                                             &mapped);
    if (phys == 0 || mapped == nullptr)
        return 0;
    GttBlock &b = g_gtt_blocks[g_gtt_block_n++];
    b.phys = phys;
    b.cpu  = mapped;
    b.pages = pages;
    b.used  = true;
    *cpu = mapped;
    return phys;
}

void gmc_v11_0_sysmem_free_pages(uint64_t phys)
{
    for (uint32_t i = 0; i < g_gtt_block_n; i++) {
        if (g_gtt_blocks[i].used && g_gtt_blocks[i].phys == phys) {
            g_gtt_blocks[i].used = false;
            return;
        }
    }
}

int gmc_v11_0_gart_map_va(uint64_t gart_va, uint64_t page_phys, uint32_t pages)
{
    if (pages == 0 || g_gmc_sysmem[0].cpu == nullptr)
        return -1;   /* GART table not set up (bind/seq_gart_build) */
    if ((gart_va & 0xFFFull) != 0 || (page_phys & 0xFFFull) != 0)
        return -1;
    const uint64_t first = gmc_gart_index(gart_va);
    if (first + pages > kGmcGartEntries)
        return -1;
    volatile uint64_t *tab = static_cast<volatile uint64_t *>(g_gmc_sysmem[0].cpu);
    for (uint32_t i = 0; i < pages; i++) {
        const uint64_t idx = first + i;
        tab[idx] = gmc_gart_entry_system(page_phys + static_cast<uint64_t>(i) * 4096ull);
        gmc_pt_flush_line(&tab[idx]);
    }
    gmc_mb();
    return 0;
}

void gmc_v11_0_gart_unmap_va(uint64_t gart_va, uint32_t pages)
{
    if (g_gmc_sysmem[0].cpu == nullptr)
        return;
    const uint64_t first = gmc_gart_index(gart_va);
    volatile uint64_t *tab = static_cast<volatile uint64_t *>(g_gmc_sysmem[0].cpu);
    for (uint32_t i = 0; i < pages; i++) {
        const uint64_t idx = first + i;
        tab[idx] = 0;
        gmc_pt_flush_line(&tab[idx]);
    }
    gmc_mb();
}

/* ---- public entries ---- */

R3SubstepResult uma_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    return run_uma(io, env);
}

R3SubstepResult gart_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    return run_gart(io, env);
}

R3SubstepResult gpuvm0_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    return run_gpuvm0(io, env);
}

R3SubstepResult vmid8_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    return run_vmid8(io, env);
}

} /* namespace amdgpu */
