/*
 * ih_v6_0.cpp - IH 6.0.1 (OSSSYS) ring bring-up substep (stage 3, S10).
 *
 * Sequence (stage-3 implementation doc §7; sole source Linux
 * drivers/gpu/drm/amd/amdgpu/ih_v6_0.c - nullarc has NO IH implementation,
 * R10):
 *   P   prerequisites: PSP ready, "ih_ring"/"ih_rptr_wb" layout grants,
 *       MMHUB FB_OFFSET (GPA basis, same runtime read as mes_v11_0 S-1)
 *   S10.0  toggle(false): RB_ENABLE=0 + ENABLE_INTR=0, then rptr/wptr=0
 *          (ih_v6_0.c:127-188, :330)
 *   S10.1  IH_CHICKEN.MC_SPACE_GPA_ENABLE=1 for the bus-address ring
 *          (:338-343; the kext loads firmware itself, DIRECT-equivalent)
 *   S10.2  ring programming, ih_v6_0_enable_ring order (:265-308):
 *          RB_BASE/HI ([39:8] of the 40-bit GPA) -> RB_CNTL (config form,
 *          :215-236) -> RB_WPTR_ADDR_LO/HI (hi & 0xFFFF) -> RB_WPTR/RPTR=0
 *          -> DOORBELL_RPTR (index 0x2F0 【推测】, :238-255/:305)
 *   S10.3  toggle(true) polling form: RB_ENABLE=1 with the CLEAR_OVERFLOW
 *          0->1->0 dance (:139-163), ENABLE_INTR stays 0 (no MSI; fence
 *          path B3 never depends on IH)
 *   S10.4  readback gate: every programmed register must read back equal;
 *          wptr writeback shadow must equal RB_WPTR. Entry decode is a
 *          gated extra (B3): a ring that comes up with no entries is
 *          recorded, never fatal.
 *
 * Address form: GPA (system physical) - see ih_v6_0.h for the rationale.
 *
 * No retries (F6): every failed access returns with its own name.
 * Provenance (PC-8): structure port of Linux ih_v6_0.c (Copyright 2019
 * Advanced Micro Devices, Inc., MIT licensed - see NOTICE). Only the
 * register semantics are reproduced onto the mac-amdgpu (Cobalt64)
 * IP-module conventions; every offset is cited at its use site.
 */
#include "ih_v6_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h> /* snprintf/vsnprintf/memset, no libc under -nostdinc */
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "../rdna3/rdna3_layout.h"
#include "../rdna3/rdna3_runtime.h"
#include "../rdna3/golden_values.h"

namespace amdgpu {
/* Step-2 getters are defined at the bottom of this file; the ladder wiring
 * (rdna3_ip_entries.h) belongs to T-E, so forward-declare here. */
const R3WriteRowReg* getIhRows(void);
uint32_t getIhRowCount(void);
const R3WriteRowLayout* getIhLayoutRows(void);
uint32_t getIhLayoutRowCount(void);
const char* getIhLayoutTag(void);

/* amdgpu_ih_decode_iv_helper (amdgpu_ih.c:263-296), field for field. The
 * ring is 8 dwords per entry; rptr/wptr step in BYTES (one entry = 32). */
void ih_decode_iv(const uint32_t iv[8], IhEntry *entry)
{
    if (!entry)
        return;
    const uint32_t dw0 = iv[0];
    entry->client_id = dw0 & 0xffu;
    entry->src_id = (dw0 >> 8) & 0xffu;
    entry->ring_id = (dw0 >> 16) & 0xffu;
    entry->vmid = (dw0 >> 24) & 0xfu;
    entry->vmid_src = dw0 >> 31;
    entry->timestamp = static_cast<uint64_t>(iv[1]) |
                       (static_cast<uint64_t>(iv[2] & 0xffffu) << 32);
    entry->timestamp_src = iv[2] >> 31;
    entry->pasid = iv[3] & 0xffffu;
    entry->node_id = (iv[3] >> 16) & 0xffu;
    entry->src_data[0] = iv[4];
    entry->src_data[1] = iv[5];
    entry->src_data[2] = iv[6];
    entry->src_data[3] = iv[7];
}

namespace {


/* Static failure names (R3SubstepResult::name must outlive the call). */
constexpr const char *kFailNoIo      = "pre:no-io";
constexpr const char *kFailNoEnv     = "pre:no-env";
constexpr const char *kFailNoBase    = "pre:ih-base";
constexpr const char *kFailGolden    = "pre:ih-golden";
constexpr const char *kFailLayout    = "pre:layout-invalid";
constexpr const char *kFailGrant     = "pre:ih-grant";
constexpr const char *kFailOwner     = "pre:ih-owner";
constexpr const char *kFailSpace     = "pre:ih-space";
constexpr const char *kFailNoCpu     = "pre:ih-cpu";
constexpr const char *kFailFbOffset  = "pre:fb-offset";
constexpr const char *kFailPspReady  = "pre:psp-not-ready";
constexpr const char *kFailPspLatched = "pre:psp-latched";
constexpr const char *kFailDisable   = "seq:ih-disable";
constexpr const char *kFailChicken   = "seq:ih-chicken";
constexpr const char *kFailRing      = "seq:ih-ring";
constexpr const char *kFailEnable    = "seq:ih-enable";
constexpr const char *kFailReadback  = "chk:ih-readback";

WClass to_wclass(R3WClassId c)
{
    switch (c) {
    case kR3WAccess:   return WClass::Access;
    case kR3WData:     return WClass::Data;
    case kR3WProtocol: return WClass::Protocol;
    case kR3WState:    return WClass::State;
    }
    return WClass::Access;
}

void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
{
    if (!env || !env->ev)
        return;
    char line[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r3_ev_add(env->ev, tag, line);
}

bool set_prop(const R3SubstepEnv *env, const char *key, const char *value)
{
    if (!env || !env->sink || !env->sink->set_property || !value)
        return false;
    size_t n = 0;
    while (value[n])
        n++;
    return env->sink->set_property(env->sink->user, key, value, n);
}

/* Everything the substep needs. Static: the plan must not sit on the 16 KiB
 * kernel stack. No R3Io::lock (serialisation comes from the stage ladder,
 * rdna3_stages.cpp kStages). */
struct Ih {
    const R3Io         *io;
    const R3SubstepEnv *env;
    R3LayoutPlan        plan;
    uint64_t            phys_base;   /* MMHUB FB_OFFSET << 24 (GPA basis) */

    uint32_t            osssys0;
    /* Golden-table-resolved addresses (P1 owns the table; base_idx comes
     * from it). IH_CHICKEN is a fallback row (not in the golden table). */
    uint32_t            rb_cntl, rb_base, rb_base_hi, rb_rptr, rb_wptr;
    uint32_t            wptr_addr_hi, wptr_addr_lo, doorbell_rptr;
    uint32_t            chicken;

    uint64_t            ring_start, wb_start; /* carveout offsets (bytes) */
    uint64_t            ring_gpa, wb_gpa;     /* system physical forms   */

    uint32_t            cntl_config;   /* S10.2 programmed form  */
    uint32_t            cntl_enabled;  /* S10.3 final form       */
    uint32_t            cntl_readback, base_readback, db_readback;
    uint32_t            wptr_reg, wptr_shadow;
    bool                has_entry;
    uint32_t            entry_client, entry_src;
    char                entry_desc[48];
};

uint32_t field_set(uint32_t v, uint32_t mask, uint32_t shift, uint32_t value)
{
    return (v & ~mask) | ((value << shift) & mask);
}

uint32_t reg_read(Ih &s, uint32_t abs, bool *ok)
{
    return r3_io_read32(s.io, abs, ok);
}

bool resolve_golden(const Ih &s, const r3gold::GoldenReg &reg, uint32_t *out)
{
    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    uint32_t abs = 0;
    if (!ip || !ip->resolve(reg.ref(), &abs))
        return false;
    *out = abs;
    return true;
}

bool reg_write(Ih &s, uint32_t abs, uint32_t value, R3WClassId cls, const char *ctx)
{
    if (r3_wg_classify(abs, 1, to_wclass(cls), ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(s.io, abs, value);
}

bool bus_write(Ih &s, uint64_t bar0_off, uint64_t value, uint32_t dwords, const char *ctx)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u)
        return false;
    if (r3_wg_classify(bar0_off >> 2, dwords, WClass::Data, ctx) != WVerdict::TxnRead)
        return false;
    return s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &value, true, dwords);
}

bool bus_read(Ih &s, uint64_t bar0_off, uint32_t dwords, uint64_t *value)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u || !value)
        return false;
    uint64_t v = 0;
    if (!s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, dwords))
        return false;
    *value = v;
    return true;
}

/* Linux ih_v6_0_rb_cntl (ih_v6_0.c:215-236), bus-address form:
 * MC_SPACE=2 (GPA), WPTR_OVERFLOW_CLEAR=1, WPTR_OVERFLOW_ENABLE=1,
 * RB_SIZE=order_base_2(ring/4)=14, WPTR_WRITEBACK_ENABLE=1, MC_SNOOP=1,
 * MC_RO=0, MC_VMID=0. RPTR_REARM=0: polling design, no MSI (Linux :280
 * uses !!msi_enabled; project amdgpu_ih.h runs the ring polled). */
uint32_t ih_rb_cntl(uint32_t cntl)
{
    cntl = field_set(cntl, kIhRbCntlMcSpaceMask, kIhRbCntlMcSpaceShift,
                     kIhMcSpaceGpa);
    cntl |= kIhRbCntlWptrOverflowClr;
    cntl |= kIhRbCntlWptrOverflowEn;
    cntl = field_set(cntl, kIhRbCntlSizeMask, kIhRbCntlSizeShift, kIhRbSizeOrder);
    cntl |= kIhRbCntlWptrWbEnable;
    cntl |= kIhRbCntlMcSnoop;
    cntl &= ~kIhRbCntlMcRo;
    cntl &= ~kIhRbCntlMcVmidMask;
    cntl &= ~kIhRbCntlRptrRearm;
    return cntl;
}

/* Linux ih_v6_0_doorbell_rptr (ih_v6_0.c:238-255) with use_doorbell=true. */
uint32_t ih_doorbell_rptr(void)
{
    return (kIhDoorbellIndex & kIhDoorbellRptrOffsetMask) | kIhDoorbellRptrEnable;
}

R3SubstepResult pre(Ih &s)
{
    R3SubstepResult r = r3_substep_pass();

    if (!r3_io_complete(s.io) || !s.io->bus) {
        evf(s.env, "ih-pre", "P0 missing io callback (bus=%d)",
            s.io && s.io->bus ? 1 : 0);
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    }
    if (!s.env || !s.env->rt) {
        evf(s.env, "ih-pre", "P0 missing environment/runtime");
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    }

    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    if (!ip || !ip->isResolved(IPBlock::OSSSYS, 0)) {
        evf(s.env, "ih-pre", "P0 OSSSYS base idx 0 unresolved");
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }
    s.osssys0 = ip->getBase(IPBlock::OSSSYS, 0);

    struct { const r3gold::GoldenReg *reg; uint32_t *out; } gold[8] = {
        {&r3gold::kIhRbCntl,       &s.rb_cntl},
        {&r3gold::kIhRbBase,       &s.rb_base},
        {&r3gold::kIhRbBaseHi,     &s.rb_base_hi},
        {&r3gold::kIhRbRptr,       &s.rb_rptr},
        {&r3gold::kIhRbWptr,       &s.rb_wptr},
        {&r3gold::kIhRbWptrAddrHi, &s.wptr_addr_hi},
        {&r3gold::kIhRbWptrAddrLo, &s.wptr_addr_lo},
        {&r3gold::kIhDoorbellRptr, &s.doorbell_rptr},
    };
    for (uint32_t i = 0; i < 8u; i++) {
        if (!resolve_golden(s, *gold[i].reg, gold[i].out)) {
            evf(s.env, "ih-pre", "P0 IH golden %u unresolved", i);
            return r3_substep_fail(kR3SecPre, 2, kFailGolden);
        }
    }
    /* IH_CHICKEN: fallback row (osssys_6_0_0_offset.h:625, BASE_IDX 0;
     * not in the golden table). */
    s.chicken = s.osssys0 + kRegIhChicken;

    s.plan = s.env->rt->layout;
    if (!s.plan.ok) {
        evf(s.env, "ih-pre", "P1 layout invalid: %s",
            s.plan.fail_reason ? s.plan.fail_reason : "?");
        return r3_substep_fail(kR3SecPre, 3, kFailLayout);
    }

    const R3Grant *ring = r3_layout_grant(&s.plan, "ih_ring");
    if (!ring) {
        evf(s.env, "ih-pre", "P1 layout has no \"ih_ring\" grant");
        return r3_substep_fail(kR3SecPre, 4, kFailGrant);
    }
    if (ring->owner != R3Owner::Ih) {
        evf(s.env, "ih-pre", "P1 \"ih_ring\" grant owner=%u (want %u)",
            static_cast<unsigned>(ring->owner), static_cast<unsigned>(R3Owner::Ih));
        return r3_substep_fail(kR3SecPre, 4, kFailOwner);
    }
    if (ring->size < kIhRingBytes) {
        evf(s.env, "ih-pre", "P1 \"ih_ring\" grant %llu B < %llu B",
            static_cast<unsigned long long>(ring->size),
            static_cast<unsigned long long>(kIhRingBytes));
        return r3_substep_fail(kR3SecPre, 4, kFailSpace);
    }
    const R3Grant *wb = r3_layout_grant(&s.plan, "ih_rptr_wb");
    if (!wb) {
        evf(s.env, "ih-pre", "P1 layout has no \"ih_rptr_wb\" grant");
        return r3_substep_fail(kR3SecPre, 4, kFailGrant);
    }
    if (wb->owner != R3Owner::Ih) {
        evf(s.env, "ih-pre", "P1 \"ih_rptr_wb\" grant owner=%u (want %u)",
            static_cast<unsigned>(wb->owner), static_cast<unsigned>(R3Owner::Ih));
        return r3_substep_fail(kR3SecPre, 4, kFailOwner);
    }
    if (wb->size < kIhRptrWbBytes) {
        evf(s.env, "ih-pre", "P1 \"ih_rptr_wb\" grant %llu B < %llu B",
            static_cast<unsigned long long>(wb->size),
            static_cast<unsigned long long>(kIhRptrWbBytes));
        return r3_substep_fail(kR3SecPre, 4, kFailSpace);
    }
    if (!ring->needs_cpu || !wb->needs_cpu) {
        evf(s.env, "ih-pre", "P1 IH grants are not CPU reachable");
        return r3_substep_fail(kR3SecPre, 4, kFailNoCpu);
    }
    s.ring_start = ring->start;
    s.wb_start = wb->start;

    /* GPA basis: MMHUB FB_OFFSET (same runtime read as mes_v11_0 S-1;
     * Linux vram_mc2pa on the bare-metal APU path). */
    uint32_t fb_off_addr = 0;
    if (!resolve_golden(s, r3gold::kMmmcVmFbOffset, &fb_off_addr)) {
        evf(s.env, "ih-pre", "mmhub FB_OFFSET golden unresolved");
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    uint32_t fb_base_addr = 0;
    if (!resolve_golden(s, r3gold::kMmmcVmFbLocationBase, &fb_base_addr)) {
        evf(s.env, "ih-pre", "mmhub FB_LOCATION_BASE golden unresolved");
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    bool ok = false;
    const uint32_t raw_off = reg_read(s, fb_off_addr, &ok);
    if (!ok) {
        evf(s.env, "ih-pre", "P1 MMHUB FB_OFFSET read failed");
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    if (raw_off == 0u) {
        evf(s.env, "ih-pre", "MMHUB FB_OFFSET=0 (fatal: ring GPA would be low PA)");
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    s.phys_base = static_cast<uint64_t>(raw_off) << 24;
    /* T8 cross-check: FB_BASE must agree with env->fb_mc_base. */
    const uint32_t fb_base_raw = reg_read(s, fb_base_addr, &ok);
    if (!ok) {
        evf(s.env, "ih-pre", "P1 MMHUB FB_LOCATION_BASE read failed");
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    const uint64_t mmhub_mc_base = static_cast<uint64_t>(fb_base_raw) << 24;
    if (mmhub_mc_base != s.env->fb_mc_base) {
        evf(s.env, "ih-pre", "T8 mmhub FB_BASE=0x%x (->0x%llx) != env mc_base=0x%llx",
            fb_base_raw, static_cast<unsigned long long>(mmhub_mc_base),
            static_cast<unsigned long long>(s.env->fb_mc_base));
        return r3_substep_fail(kR3SecPre, 5, kFailFbOffset);
    }
    s.ring_gpa = s.phys_base + s.ring_start;
    s.wb_gpa = s.phys_base + s.wb_start;

    /* PSP only: the IH is independent of GFX/SDMA/MES readiness. */
    if (!(s.env->rt->ready_bits & R3_IP_BIT(R3Ip::Psp))) {
        evf(s.env, "ih-pre", "P2 PSP not ready (ready_bits=0x%08x)",
            s.env->rt->ready_bits);
        return r3_substep_fail(kR3SecPre, 6, kFailPspReady);
    }
    if (s.env->rt->latch_bits & R3_IP_BIT(R3Ip::Psp)) {
        evf(s.env, "ih-pre", "P2 PSP latched (latch_bits=0x%08x)",
            s.env->rt->latch_bits);
        return r3_substep_fail(kR3SecPre, 7, kFailPspLatched);
    }

    evf(s.env, "ih-pre",
        "P0 osssys=0x%x cntl=0x%x base=0x%x db=0x%x chicken=0x%x "
        "P1 ring=0x%llx..0x%llx wb=0x%llx..0x%llx gpa=0x%llx/0x%llx P2 psp=ready",
        s.osssys0, s.rb_cntl, s.rb_base, s.doorbell_rptr, s.chicken,
        static_cast<unsigned long long>(s.ring_start),
        static_cast<unsigned long long>(s.ring_start + kIhRingBytes),
        static_cast<unsigned long long>(s.wb_start),
        static_cast<unsigned long long>(s.wb_start + kIhRptrWbBytes),
        static_cast<unsigned long long>(s.ring_gpa),
        static_cast<unsigned long long>(s.wb_gpa));

    /* Teardown grants (W1b): the inverse sequence writes IH_RB_CNTL/RPTR/WPTR, and the
     * S10.0 disable already writes the same three addresses. A grant is an
     * address+width+owner+class key, so registering it here also lets those power-up
     * writes through (they were rejected while the IH had no grant at all). */
    struct { uint32_t addr; const char *tag; } td[3] = {
        {s.rb_cntl, "reg:teardown:ih-rb-cntl"},
        {s.rb_rptr, "reg:teardown:ih-rb-rptr"},
        {s.rb_wptr, "reg:teardown:ih-rb-wptr"},
    };
    for (uint32_t i = 0; i < 3u; i++) {
        if (r3_wg_register_reg(td[i].addr, 1u, WClass::State, WSubstep::Ih, td[i].tag) != 0) {
            evf(s.env, "ih-pre", "P3 teardown grant %s rejected", td[i].tag);
            return r3_substep_fail(kR3SecPre, 8, "pre:ih-teardown-register");
        }
    }
    (void)r;
    return r;
}

/* S10.0: Linux ih_v6_0_toggle_ring_interrupts(ih, false) (ih_v6_0.c:127-188,
 * called from irq_init :330). RB_ENABLE=0; ENABLE_INTR=0 is the ring0 field
 * (:168) and stays 0 under the polling discipline; then rptr/wptr=0
 * (:181-182). */
R3SubstepResult seq_disable(Ih &s)
{
    bool ok = false;
    uint32_t tmp = reg_read(s, s.rb_cntl, &ok);
    if (!ok) {
        evf(s.env, "ih-seq", "S10.0 RB_CNTL read failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailDisable);
    }
    tmp = field_set(tmp, kIhRbCntlEnable, 0, 0);
    tmp = field_set(tmp, kIhRbCntlEnableIntr, 17, 0);
    if (!reg_write(s, s.rb_cntl, tmp, kR3WState, "ih-disable")) {
        evf(s.env, "ih-seq", "S10.0 RB_CNTL disable write failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailDisable);
    }
    /* :181-182 set rptr, wptr to 0 */
    if (!reg_write(s, s.rb_rptr, 0, kR3WState, "ih-disable") ||
        !reg_write(s, s.rb_wptr, 0, kR3WState, "ih-disable")) {
        evf(s.env, "ih-seq", "S10.0 rptr/wptr clear failed");
        return r3_substep_fail(kR3SecSeq, 1, kFailDisable);
    }
    evf(s.env, "ih-seq", "S10.0 disabled: cntl=0x%08x rptr=wptr=0", tmp);
    return r3_substep_pass();
}

/* S10.1: Linux ih_v6_0.c:338-343 - with the bus-address ring,
 * IH_CHICKEN.MC_SPACE_GPA_ENABLE=1. The condition there is load_type
 * DIRECT/RLC_BACKDOOR_AUTO; the kext loads every firmware itself, so the
 * condition holds. */
R3SubstepResult seq_chicken(Ih &s)
{
    bool ok = false;
    uint32_t tmp = reg_read(s, s.chicken, &ok);
    if (!ok) {
        evf(s.env, "ih-seq", "S10.1 IH_CHICKEN read failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailChicken);
    }
    tmp = field_set(tmp, kIhChickenMcSpaceGpaEnable, 4, 1);
    if (!reg_write(s, s.chicken, tmp, kR3WState, "ih-chicken")) {
        evf(s.env, "ih-seq", "S10.1 IH_CHICKEN write failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailChicken);
    }
    evf(s.env, "ih-seq", "S10.1 chicken=0x%08x (MC_SPACE_GPA_ENABLE)", tmp);
    return r3_substep_pass();
}

/* S10.2: ring programming in Linux ih_v6_0_enable_ring order
 * (ih_v6_0.c:265-308). The task card's list enumerates the register set;
 * the write order follows the source (INV-A1). */
R3SubstepResult seq_ring(Ih &s)
{
    /* Zero the writeback shadows first: Linux gets zeroed memory from
     * dma_alloc_coherent (amdgpu_ih.c:74-77 puts the wptr shadow at
     * +ring_size and the rptr shadow right after); the carveout does not
     * self-zero. 8 bytes cover both shadows. */
    if (!bus_write(s, s.wb_start + kIhWbWptrShadowOff, 0, 1, "ih-wb-zero") ||
        !bus_write(s, s.wb_start + kIhWbRptrShadowOff, 0, 1, "ih-wb-zero")) {
        evf(s.env, "ih-seq", "S10.2 writeback shadow zero failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailRing);
    }

    /* :273-275 Ring Buffer base. [39:8] of 40-bit address. */
    if (!reg_write(s, s.rb_base, static_cast<uint32_t>(s.ring_gpa >> 8),
                   kR3WState, "ih-ring-base") ||
        !reg_write(s, s.rb_base_hi,
                   static_cast<uint32_t>((s.ring_gpa >> 40) & 0xffu),
                   kR3WState, "ih-ring-base")) {
        evf(s.env, "ih-seq", "S10.2 RB_BASE write failed (gpa=0x%llx)",
            static_cast<unsigned long long>(s.ring_gpa));
        return r3_substep_fail(kR3SecSeq, 1, kFailRing);
    }

    /* :277-292 RB_CNTL config form (RMW). */
    bool ok = false;
    uint32_t tmp = reg_read(s, s.rb_cntl, &ok);
    if (!ok) {
        evf(s.env, "ih-seq", "S10.2 RB_CNTL read failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailRing);
    }
    tmp = ih_rb_cntl(tmp);
    s.cntl_config = tmp;
    if (!reg_write(s, s.rb_cntl, tmp, kR3WState, "ih-ring-cntl")) {
        evf(s.env, "ih-seq", "S10.2 RB_CNTL config write failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailRing);
    }

    /* :295-299 wptr writeback address (hi masked to 0xFFFF). */
    if (!reg_write(s, s.wptr_addr_lo, static_cast<uint32_t>(s.wb_gpa & 0xffffffffull),
                   kR3WState, "ih-wptr-addr") ||
        !reg_write(s, s.wptr_addr_hi,
                   static_cast<uint32_t>((s.wb_gpa >> 32) & 0xffffu),
                   kR3WState, "ih-wptr-addr")) {
        evf(s.env, "ih-seq", "S10.2 RB_WPTR_ADDR write failed (gpa=0x%llx)",
            static_cast<unsigned long long>(s.wb_gpa));
        return r3_substep_fail(kR3SecSeq, 3, kFailRing);
    }

    /* :301-303 set rptr, wptr to 0. */
    if (!reg_write(s, s.rb_wptr, 0, kR3WState, "ih-ring-wptr") ||
        !reg_write(s, s.rb_rptr, 0, kR3WState, "ih-ring-rptr")) {
        evf(s.env, "ih-seq", "S10.2 RB_WPTR/RPTR clear failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailRing);
    }

    /* :305 DOORBELL_RPTR (index 0x2F0 【推测】 - confirm on hardware). */
    const uint32_t db = ih_doorbell_rptr();
    if (!reg_write(s, s.doorbell_rptr, db, kR3WState, "ih-doorbell-rptr")) {
        evf(s.env, "ih-seq", "S10.2 DOORBELL_RPTR write failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailRing);
    }

    evf(s.env, "ih-seq",
        "S10.2 ring: base=0x%llx>>8 hi=0x%llx cntl=0x%08x wb=0x%llx "
        "db=0x%08x",
        static_cast<unsigned long long>(s.ring_gpa),
        static_cast<unsigned long long>((s.ring_gpa >> 40) & 0xffull),
        tmp, static_cast<unsigned long long>(s.wb_gpa), db);
    return r3_substep_pass();
}

/* S10.3: Linux ih_v6_0_toggle_ring_interrupts(ih, true) (ih_v6_0.c:127-188,
 * called from irq_init :396), polling adaptation: ENABLE_INTR stays 0 (no
 * MSI; the fence path B3 never depends on IH). The CLEAR_OVERFLOW 0->1->0
 * dance is verbatim (:139-163). */
R3SubstepResult seq_enable(Ih &s)
{
    bool ok = false;
    uint32_t tmp = reg_read(s, s.rb_cntl, &ok);
    if (!ok) {
        evf(s.env, "ih-seq", "S10.3 RB_CNTL read failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailEnable);
    }
    /* :137 RB_ENABLE=1; :143 unset CLEAR so the next step switches 0->1. */
    tmp = field_set(tmp, kIhRbCntlEnable, 0, 1);
    tmp = field_set(tmp, kIhRbCntlWptrOverflowClr, 31, 0);
    if (!reg_write(s, s.rb_cntl, tmp, kR3WState, "ih-enable")) {
        evf(s.env, "ih-seq", "S10.3 RB_CNTL enable write 1 failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailEnable);
    }
    /* :152 clear RB_OVERFLOW. */
    tmp = field_set(tmp, kIhRbCntlWptrOverflowClr, 31, 1);
    if (!reg_write(s, s.rb_cntl, tmp, kR3WState, "ih-enable")) {
        evf(s.env, "ih-seq", "S10.3 RB_CNTL enable write 2 failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailEnable);
    }
    /* :163 unset CLEAR immediately so new overflows can be detected;
     * :168 ENABLE_INTR=0 (polling). */
    tmp = field_set(tmp, kIhRbCntlWptrOverflowClr, 31, 0);
    tmp = field_set(tmp, kIhRbCntlEnableIntr, 17, 0);
    if (!reg_write(s, s.rb_cntl, tmp, kR3WState, "ih-enable")) {
        evf(s.env, "ih-seq", "S10.3 RB_CNTL enable write 3 failed");
        return r3_substep_fail(kR3SecSeq, 0, kFailEnable);
    }
    s.cntl_enabled = tmp;
    evf(s.env, "ih-seq", "S10.3 enabled: cntl=0x%08x (ENABLE_INTR=0, polled)",
        tmp);
    return r3_substep_pass();
}

/* S10.4: readback gate + writeback liveness + entry extra (gated, B3). */
R3SubstepResult chk_ring(Ih &s)
{
    bool ok = false;
    const uint32_t cntl = reg_read(s, s.rb_cntl, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t base = reg_read(s, s.rb_base, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t base_hi = reg_read(s, s.rb_base_hi, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t rptr = reg_read(s, s.rb_rptr, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t wptr = reg_read(s, s.rb_wptr, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t wa_lo = reg_read(s, s.wptr_addr_lo, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t wa_hi = reg_read(s, s.wptr_addr_hi, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    const uint32_t db = reg_read(s, s.doorbell_rptr, &ok);
    if (!ok) return r3_substep_fail(kR3SecChk, 0, kFailReadback);
    s.cntl_readback = cntl;
    s.base_readback = base;
    s.db_readback = db;
    s.wptr_reg = wptr;

    if (cntl != s.cntl_enabled) {
        evf(s.env, "ih-chk", "RB_CNTL readback 0x%08x != 0x%08x",
            cntl, s.cntl_enabled);
        return r3_substep_fail(kR3SecChk, 1, kFailReadback);
    }
    if (base != static_cast<uint32_t>(s.ring_gpa >> 8) ||
        base_hi != static_cast<uint32_t>((s.ring_gpa >> 40) & 0xffu)) {
        evf(s.env, "ih-chk", "RB_BASE readback 0x%08x/0x%08x != gpa 0x%llx",
            base, base_hi, static_cast<unsigned long long>(s.ring_gpa));
        return r3_substep_fail(kR3SecChk, 2, kFailReadback);
    }
    if (rptr != 0u) {
        evf(s.env, "ih-chk", "RB_RPTR readback 0x%08x != 0", rptr);
        return r3_substep_fail(kR3SecChk, 3, kFailReadback);
    }
    if (wa_lo != static_cast<uint32_t>(s.wb_gpa & 0xffffffffull) ||
        wa_hi != static_cast<uint32_t>((s.wb_gpa >> 32) & 0xffffu)) {
        evf(s.env, "ih-chk", "RB_WPTR_ADDR readback 0x%08x/0x%04x != wb 0x%llx",
            wa_lo, wa_hi, static_cast<unsigned long long>(s.wb_gpa));
        return r3_substep_fail(kR3SecChk, 4, kFailReadback);
    }
    if (db != ih_doorbell_rptr()) {
        evf(s.env, "ih-chk", "DOORBELL_RPTR readback 0x%08x != 0x%08x",
            db, ih_doorbell_rptr());
        return r3_substep_fail(kR3SecChk, 5, kFailReadback);
    }

    /* Writeback liveness: the shadow must track the register wptr (the
     * polling design reads the shadow, never the register, at runtime). */
    uint64_t v = 0;
    if (!bus_read(s, s.wb_start + kIhWbWptrShadowOff, 1, &v)) {
        evf(s.env, "ih-chk", "wptr shadow read failed");
        return r3_substep_fail(kR3SecChk, 6, kFailReadback);
    }
    s.wptr_shadow = static_cast<uint32_t>(v);
    if (s.wptr_shadow != wptr) {
        evf(s.env, "ih-chk", "wptr shadow 0x%08x != RB_WPTR 0x%08x (writeback broken)",
            s.wptr_shadow, wptr);
        return r3_substep_fail(kR3SecChk, 6, kFailReadback);
    }

    /* Entry extra (gated, never fatal): with no interrupt sources enabled
     * no entries are expected; if some arrived anyway, decode the first
     * one (rptr is 0) as evidence that the path works. */
    if (wptr != 0u) {
        uint32_t iv[8];
        bool all = true;
        for (uint32_t i = 0; i < 8u; i++) {
            uint64_t d = 0;
            if (!bus_read(s, s.ring_start + (uint64_t)i * 4ull, 1, &d)) {
                all = false;
                break;
            }
            iv[i] = static_cast<uint32_t>(d);
        }
        if (all) {
            IhEntry e;
            ih_decode_iv(iv, &e);
            s.has_entry = true;
            s.entry_client = e.client_id;
            s.entry_src = e.src_id;
            snprintf(s.entry_desc, sizeof(s.entry_desc), "client=0x%02x src=0x%02x",
                     e.client_id, e.src_id);
        }
    }

    evf(s.env, "ih-chk",
        "S10.4 readback ok: cntl=0x%08x base=0x%08x/0x%02x rptr=0 wptr=0x%08x "
        "wa=0x%08x/0x%04x db=0x%08x shadow=0x%08x entries=%s",
        cntl, base, base_hi, wptr, wa_lo, wa_hi, db, s.wptr_shadow,
        s.has_entry ? s.entry_desc : "none");
    return r3_substep_pass();
}

} /* namespace */

R3SubstepResult ih_v6_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    static Ih s;
    memset(&s, 0, sizeof(s));
    s.io = io;
    s.env = env;

    r3_wg_arm_substep(WSubstep::Ih);
    if (r3_wg_expect_writes(getIhRows(), getIhRowCount(),
                            getIhLayoutRows(), getIhLayoutRowCount(), WSubstep::Ih) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env ? env->ip : nullptr, getIhRows(), getIhRowCount(),
                            declared);
    if (r3_wg_expect_declared_addrs(declared, getIhRowCount()) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");

    R3SubstepResult r = pre(s);
    if (r.status == kR3SsecOk)
        r = seq_disable(s);
    if (r.status == kR3SsecOk)
        r = seq_chicken(s);
    if (r.status == kR3SsecOk)
        r = seq_ring(s);
    if (r.status == kR3SsecOk)
        r = seq_enable(s);
    if (r.status == kR3SsecOk)
        r = chk_ring(s);

    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, s.rb_cntl, s.cntl_readback, "RB_CNTL");
        r3_substep_note(&r, s.rb_base, s.base_readback, "RB_BASE");
        r3_substep_note(&r, s.doorbell_rptr, s.db_readback, "DOORBELL_RPTR");
        r3_substep_note(&r, 0u, s.wptr_shadow, "WB wptr");
        set_prop(env, "N48R3EVID-ih-done", "Yes");
        set_prop(env, "N48R3EVID-ih-entries", s.has_entry ? s.entry_desc : "none");
        evf(env, "ih", "done: cntl=0x%08x ring_gpa=0x%llx entries=%s",
            s.cntl_readback, static_cast<unsigned long long>(s.ring_gpa),
            s.has_entry ? s.entry_desc : "none");
    } else {
        set_prop(env, "N48R3EVID-ih-done", "No");
        set_prop(env, "N48R3EVID-ih-entries", "n/a");
        evf(env, "ih", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
    }

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getIhRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }

    r3_wg_disarm_substep();
    return r;
}

/* Step2: declaration table (rdna3_writeguard.h R3WriteRowReg), the single
 * source of truth for the IH register write set. 9 rows: the 8 golden
 * OSSSYS ring registers + IH_CHICKEN (fallback form, not in the golden
 * table). Row order follows the write sequence. */
namespace {
constexpr R3WriteRowReg kIhRows[] = {
    {&r3gold::kIhRbCntl,       static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-rb-cntl"},
    {&r3gold::kIhRbBase,       static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-rb-base"},
    {&r3gold::kIhRbBaseHi,     static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-rb-base-hi"},
    {&r3gold::kIhRbRptr,       static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-rb-rptr"},
    {&r3gold::kIhRbWptr,       static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-rb-wptr"},
    {&r3gold::kIhRbWptrAddrHi, static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-wptr-addr-hi"},
    {&r3gold::kIhRbWptrAddrLo, static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-wptr-addr-lo"},
    {&r3gold::kIhDoorbellRptr, static_cast<uint8_t>(IPBlock::OSSSYS), 0, 0, 1,
     WClass::State, WNeed::Required, "ih-doorbell-rptr"},
    {nullptr, static_cast<uint8_t>(IPBlock::OSSSYS), 0, kRegIhChicken, 1,
     WClass::State, WNeed::Required, "ih-chicken"},
};
constexpr uint32_t kIhRowCount = sizeof(kIhRows) / sizeof(kIhRows[0]);
static_assert(kIhRowCount == 9, "ih declaration table must have 9 rows");
static_assert(kIhRowCount <= R3_WG_EXPECT_MAX, "ih expectation set exceeds R3_WG_EXPECT_MAX");

/* Layout expectations: the substep only writes the writeback page (the ring
 * itself is never written - Linux does not zero it either). */
constexpr R3WriteRowLayout kIhLayoutRows[] = {
    {"ih_rptr_wb", WSubstep::Ih, WNeed::Required},
};
constexpr uint32_t kIhLayoutRowCount =
    sizeof(kIhLayoutRows) / sizeof(kIhLayoutRows[0]);
static_assert(kIhLayoutRowCount == 1, "ih layout table must have 1 row");
constexpr const char *kIhLayoutTag = "ih";
} // anonymous namespace

const R3WriteRowReg* getIhRows(void) { return kIhRows; }
uint32_t getIhRowCount(void) { return kIhRowCount; }
const R3WriteRowLayout* getIhLayoutRows(void) { return kIhLayoutRows; }
uint32_t getIhLayoutRowCount(void) { return kIhLayoutRowCount; }
const char* getIhLayoutTag(void) { return kIhLayoutTag; }

} /* namespace amdgpu */
