/*
 * sdma_v6_0.cpp - SDMA 6.0.1 queue0 power-up substep (stage 2, K7).
 *
 * Sequence (tech plan §3.4 S1-S8, Linux sdma_v6_0_gfx_resume_instance order):
 *   S1  zero ring / writeback / wptr-shadow (②, inside the layout "sdma" grant, L5)
 *   S2  stage the copy source pattern and one WRITE_LINEAR packet + one fence packet (②)
 *   S3  publish the write pointer into its shadow copy (③, SDMA-01)
 *   S4  program queue0 RB/RPTR/WB + open MINOR_PTR_UPDATE=1 -> RB_WPTR -> =0 (③/④)
 *   S5  SEM_WAIT_FAIL_TIMER=0, arm WATCHDOG, DOORBELL.ENABLE=0, F32 unhalt,
 *       RB_ENABLE, IB_ENABLE (④)
 *   S6  poll the fence dword in the writeback buffer, 2 s budget
 *   S7  read the target back through BAR0 and compare byte by byte (C4.2)
 *   S8  SDMA0_STATUS_REG cross evidence + UTCL1_CNTL/PAGE record (C-12 Deviation)
 *
 * WRITE_LINEAR has a destination address but no source address: the CPU stages the
 * source buffer's bytes into the packet payload, so the source and the destination are
 * both the layout grant's copy-test buffers and the engine moves them with the opcode
 * nullarc E4a validated. The packet layout is sdma_v6_0_0_pkt_open.h.
 *
 * No retries (F6): a failed window reading or a fence timeout returns immediately.
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c (Copyright 2020 Advanced Micro Devices, Inc.,
 * MIT licensed - see NOTICE and sdma_v6_0.h). Only the register/protocol semantics are
 * reproduced onto the mac-amdgpu (Cobalt64) IP-module conventions; every offset is
 * cited from the pinned headers at its use site.
 */
#include "sdma_v6_0.h"

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
#include "../rdna3/rdna3_writeguard.h"

namespace amdgpu {
namespace {

/* Static failure names (R3SubstepResult::name must outlive the call). */
constexpr const char *kFailNoIo        = "pre:no-io";
constexpr const char *kFailNoEnv       = "pre:no-env";
constexpr const char *kFailNoBase      = "pre:sdma-base";
constexpr const char *kFailNoF32Base   = "pre:f32-base";
constexpr const char *kFailNoMcBase    = "pre:no-mc-base";
constexpr const char *kFailLayout      = "pre:layout-invalid";
constexpr const char *kFailGrant       = "pre:sdma-grant";
constexpr const char *kFailOwner       = "pre:sdma-owner";
constexpr const char *kFailSpace       = "pre:sdma-space";
constexpr const char *kFailBar0        = "pre:sdma-bar0";
constexpr const char *kFailNoCpu       = "pre:sdma-cpu";
constexpr const char *kFailPspReady    = "pre:psp-not-ready";
constexpr const char *kFailPspLatched  = "pre:psp-latched";
constexpr const char *kFailNoHdp       = "pre:no-hdp-window";
constexpr const char *kFailHdp         = "seq:hdp-flush";
constexpr const char *kFailZero        = "seq:zero-fill";
constexpr const char *kFailRing        = "seq:ring-packet";
constexpr const char *kFailQueue       = "seq:queue-regs";
constexpr const char *kFailWindow      = "seq:wptr-window";
constexpr const char *kFailShadow      = "seq:wptr-shadow";
constexpr const char *kFailFence       = "seq:fence-timeout";
constexpr const char *kFailReadUnread  = "chk:readback-unreadable";
constexpr const char *kFailReadMismatch = "chk:readback-mismatch";
constexpr const char *kFailStatus      = "chk:status";

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

/* Everything the substep needs. Static: a plan (<=768 B) plus the window label must not
 * sit on the 16 KiB kernel stack. R3Io::lock is NOT a substep-level contract (only SMU
 * holds it for its whole entry; PSP holds it inside post(); GFX/SDMA/MES never take it,
 * same statement as rdna3_dump.cpp:488-491): serialisation comes from the stage ladder,
 * which runs the five substeps one after another from a single trigger path
 * (rdna3_stages.cpp kStages). */
struct Sdma {
    const R3Io         *io;
    const R3SubstepEnv *env;
    R3LayoutPlan        plan;
    uint64_t            mc_base;

    uint64_t            grant_start; /* BAR0 byte offset of the layout "sdma" grant */
    uint64_t            ring_bar0;
    uint64_t            wb_bar0;
    uint64_t            shadow_bar0;
    uint64_t            src_bar0;
    uint64_t            dst_bar0;
    uint64_t            fence_bar0;

    uint64_t            ring_mc;
    uint64_t            rptr_wb_mc;
    uint64_t            shadow_mc;
    uint64_t            dst_mc;
    uint64_t            fence_mc;

    uint32_t            sdma0_base;
    uint32_t            sdma_f32_base;

    uint32_t            wptr_bytes;
    uint32_t            window_pre;
    uint32_t            window_post;
    uint32_t            utcl1_cntl;
    uint32_t            utcl1_page;
    uint32_t            fence_us;
    uint32_t            status_value;
    uint32_t            sdma0_version_pre;
    bool                sdma0_version_pre_ok;
    uint32_t            sdma0_version_post;
    bool                sdma0_version_post_ok;
    bool                utcl1_ok;
    bool                fence_measured;
    bool                copy_ok;

    /* The writeguard stores the ctx pointer in the manifest. We publish "pre=1" when the
     * WPTR write happens and overwrite the same buffer with the post reading after the
     * window closes, so the manifest line carries both readings (S4 requirement). */
    char                wptr_ctx[96];

    /* Stable property text: R3EvSink may retain the pointer (contract), so it lives with
     * the static substep instance rather than on this frame. */
    char                wptr_prop[64];
};

uint32_t field_set(uint32_t v, uint32_t mask, uint32_t shift, uint32_t value)
{
    return (v & ~mask) | ((value << shift) & mask);
}

uint32_t field_clear(uint32_t v, uint32_t mask)
{
    return v & ~mask;
}
uint32_t reg_read(Sdma &s, uint32_t abs, bool *ok)
{
    return r3_io_read32(s.io, abs, ok);
}

bool reg_write(Sdma &s, uint32_t abs, uint32_t value, R3WClassId cls, const char *ctx)
{
    if (r3_wg_classify(abs, 1, to_wclass(cls), ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(s.io, abs, value);
}

bool bus_write(Sdma &s, uint64_t bar0_off, uint64_t value, uint32_t dwords, const char *ctx)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u)
        return false;
    if (r3_wg_classify(bar0_off >> 2, dwords, WClass::Data, ctx) != WVerdict::TxnRead)
        return false;
    return s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &value, true, dwords);
}

bool bus_read(Sdma &s, uint64_t bar0_off, uint32_t dwords, uint64_t *value)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u || !value)
        return false;
    uint64_t v = 0;
    if (!s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, dwords))
        return false;
    *value = v;
    return true;
}

/* Contract 6/7: order host writes, then make them visible to the engine before a kick. */
bool fence_barrier(Sdma &s)
{
    if (s.io && s.io->fence)
        s.io->fence(s.io->ctx);
    return r3_io_hdp_flush(s.io);
}

/* Write `dwords` little-endian dwords, 8 B at a time when aligned, then a 4 B tail. */
bool write_dwords(Sdma &s, uint64_t bar0_off, const uint32_t *src, uint32_t dwords,
                  const char *ctx)
{
    uint32_t i = 0;
    while (i < dwords) {
        const uint64_t off = bar0_off + (uint64_t)i * 4ull;
        uint32_t n = 1;
        if (((off & 7ull) == 0ull) && (dwords - i) >= 2u)
            n = 2;
        uint64_t v = 0;
        memcpy(&v, &src[i], (size_t)n * 4u);
        if (!bus_write(s, off, v, n, ctx))
            return false;
        i += n;
    }
    return true;
}

/* Explicit 32 B zeroing, matching K4's ring clear so the L5 area is observably zeroed. */
bool zero_region(Sdma &s, uint64_t bar0_off, uint64_t bytes, const char *ctx)
{
    for (uint64_t off = 0; off < bytes; off += 32ull) {
        for (uint32_t i = 0; i < 4u; i++) {
            if (!bus_write(s, bar0_off + off + (uint64_t)i * 8ull, 0ull, 2u, ctx))
                return false;
        }
    }
    return true;
}

/* Deterministic source pattern. WRITE_LINEAR carries the payload, so the CPU reads the
 * pattern it staged into the source buffer and emits the same dwords in the packet. */
void fill_pattern(uint32_t *dst, uint32_t dwords)
{
    for (uint32_t i = 0; i < dwords; i++)
        dst[i] = 0x10000000u + i * 0x01010101u;
}

uint32_t rb_cntl_programmed(uint32_t current)
{
    uint32_t v = field_set(current, kRbCntlRbSizeMask, kRbCntlRbSizeShift, kSdmaRbSize);
    v = field_set(v, kRbCntlWptrPollEnableMask, 8, 0);
    v = field_set(v, kRbCntlF32WptrPollEnableMask, 11, 1);
    v = field_set(v, kRbCntlRptrWbEnableMask, 12, 1);
    v = field_set(v, kRbCntlRbPrivMask, 23, 1);
    return field_clear(v, kRbCntlRbEnableMask);
}

bool is_sdma_ready(const R3SubstepEnv *env)
{
    return env && env->rt && (env->rt->ready_bits & R3_IP_BIT(R3Ip::Psp));
}

R3SubstepResult pre(Sdma &s)
{
    R3SubstepResult r = r3_substep_pass();

    if (!r3_io_complete(s.io) || !s.io->bus) {
        evf(s.env, "sdma-pre", "P0 missing io callback (bus=%d)",
            s.io && s.io->bus ? 1 : 0);
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    }
    if (!s.env || !s.env->rt) {
        evf(s.env, "sdma-pre", "P0 missing environment/runtime");
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    }
    if (!s.env->hdp_flush_valid) {
        evf(s.env, "sdma-pre", "P0 no HDP visibility window (submit would be unsafe)");
        return r3_substep_fail(kR3SecPre, 0, kFailNoHdp);
    }

    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    if (!ip || !ip->isResolved(IPBlock::SDMA0, kSdma0BaseIdx)) {
        evf(s.env, "sdma-pre", "P0 SDMA0 base idx %d unresolved", kSdma0BaseIdx);
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    }
    if (!ip->isResolved(IPBlock::SDMA0, kSdmaF32BaseIdx)) {
        evf(s.env, "sdma-pre", "P0 SDMA0 F32_CNTL base idx %d unresolved",
            kSdmaF32BaseIdx);
        return r3_substep_fail(kR3SecPre, 0, kFailNoF32Base);
    }
    s.sdma0_base = ip->getBase(IPBlock::SDMA0, kSdma0BaseIdx);
    s.sdma_f32_base = ip->getBase(IPBlock::SDMA0, kSdmaF32BaseIdx);

    /* D3/§8.2: S0 pre for the SDMA0 version observation (before the F32 unhalt). It is
     * evidence only and never gates; the S8 post is recorded separately. */
    bool vok = false;
    s.sdma0_version_pre = reg_read(s, s.sdma0_base + kRegSdma0Version, &vok);
    s.sdma0_version_pre_ok = vok;

    s.mc_base = s.env->fb_mc_base;
    if (s.mc_base == 0ull) {
        evf(s.env, "sdma-pre", "P0 fb_mc_base is zero");
        return r3_substep_fail(kR3SecPre, 0, kFailNoMcBase);
    }

    s.plan = s.env->rt->layout;
    if (!s.plan.ok) {
        evf(s.env, "sdma-pre", "P1 layout invalid: %s",
            s.plan.fail_reason ? s.plan.fail_reason : "?");
        return r3_substep_fail(kR3SecPre, 1, kFailLayout);
    }

    const R3Grant *g = r3_layout_grant(&s.plan, "sdma");
    if (!g) {
        evf(s.env, "sdma-pre", "P1 layout has no \"sdma\" grant");
        return r3_substep_fail(kR3SecPre, 1, kFailGrant);
    }
    if (g->owner != R3Owner::Sdma) {
        evf(s.env, "sdma-pre", "P1 \"sdma\" grant owner=%u (want %u)",
            static_cast<unsigned>(g->owner), static_cast<unsigned>(R3Owner::Sdma));
        return r3_substep_fail(kR3SecPre, 1, kFailOwner);
    }
    if (g->size < kSdmaRegionBytes) {
        evf(s.env, "sdma-pre", "P1 \"sdma\" grant %llu B < %llu B",
            static_cast<unsigned long long>(g->size),
            static_cast<unsigned long long>(kSdmaRegionBytes));
        return r3_substep_fail(kR3SecPre, 1, kFailSpace);
    }
    if (!g->needs_cpu) {
        evf(s.env, "sdma-pre", "P1 \"sdma\" grant is not CPU reachable");
        return r3_substep_fail(kR3SecPre, 1, kFailNoCpu);
    }
    if (g->start + kSdmaRegionBytes > s.plan.bar0_limit) {
        evf(s.env, "sdma-pre", "P1 \"sdma\" region %llu..%llu past BAR0 limit %llu",
            static_cast<unsigned long long>(g->start),
            static_cast<unsigned long long>(g->start + kSdmaRegionBytes),
            static_cast<unsigned long long>(s.plan.bar0_limit));
        return r3_substep_fail(kR3SecPre, 1, kFailBar0);
    }

    s.grant_start = g->start;
    s.ring_bar0 = s.grant_start + kSdmaRingOff;
    s.wb_bar0 = s.grant_start + kSdmaWbOff;
    s.shadow_bar0 = s.grant_start + kSdmaWptrShadowOff;
    s.src_bar0 = s.grant_start + kSdmaSrcOff;
    s.dst_bar0 = s.grant_start + kSdmaDstOff;
    s.fence_bar0 = s.grant_start + kSdmaFenceOff;

    s.ring_mc = s.mc_base + s.ring_bar0;
    s.rptr_wb_mc = s.mc_base + s.grant_start + kSdmaRptrWbOff;
    s.shadow_mc = s.mc_base + s.shadow_bar0;
    s.dst_mc = s.mc_base + s.dst_bar0;
    s.fence_mc = s.mc_base + s.fence_bar0;

    if (!is_sdma_ready(s.env)) {
        evf(s.env, "sdma-pre", "P2 PSP not ready (ready_bits=0x%08x)",
            s.env->rt->ready_bits);
        return r3_substep_fail(kR3SecPre, 2, kFailPspReady);
    }
    if (s.env->rt->latch_bits & R3_IP_BIT(R3Ip::Psp)) {
        evf(s.env, "sdma-pre", "P2 PSP latched (latch_bits=0x%08x)",
            s.env->rt->latch_bits);
        return r3_substep_fail(kR3SecPre, 2, kFailPspLatched);
    }

    evf(s.env, "sdma-pre",
        "P0 sdma0=0x%x f32=0x%x mc=0x%llx P1 grant=0x%llx..0x%llx "
        "ring_mc=0x%llx dst_mc=0x%llx fence_mc=0x%llx P2 psp=ready",
        s.sdma0_base, s.sdma_f32_base, static_cast<unsigned long long>(s.mc_base),
        static_cast<unsigned long long>(g->start),
        static_cast<unsigned long long>(g->end),
        static_cast<unsigned long long>(s.ring_mc),
        static_cast<unsigned long long>(s.dst_mc),
        static_cast<unsigned long long>(s.fence_mc));
    (void)r;
    return r;
}

R3SubstepResult seq_zero_and_stage(Sdma &s)
{
    /* S1: ring / writeback / wptr-shadow, all inside the layout grant (L5). */
    if (!zero_region(s, s.ring_bar0, kSdmaRingBytes, "s1-ring-zero") ||
        !zero_region(s, s.wb_bar0, kSdmaWbBytes, "s1-wb-zero") ||
        !zero_region(s, s.shadow_bar0, kSdmaWptrShadowBytes, "s1-wptr-shadow-zero")) {
        evf(s.env, "sdma-seq", "S1 zero ring/wb/shadow failed");
        return r3_substep_fail(kR3SecSeq, 1, kFailZero);
    }
    evf(s.env, "sdma-seq",
        "S1 zero ring=%llu wb=%llu shadow=%llu B @ bar0 0x%llx/0x%llx/0x%llx",
        static_cast<unsigned long long>(kSdmaRingBytes),
        static_cast<unsigned long long>(kSdmaWbBytes),
        static_cast<unsigned long long>(kSdmaWptrShadowBytes),
        static_cast<unsigned long long>(s.ring_bar0),
        static_cast<unsigned long long>(s.wb_bar0),
        static_cast<unsigned long long>(s.shadow_bar0));

    /* S2: stage the source buffer, then the WRITE_LINEAR copy packet and the fence
     * packet. Both destinations are MC addresses (B4: CPU touches BAR0 only). */
    uint32_t pattern[kSdmaCopyDwords];
    fill_pattern(pattern, kSdmaCopyDwords);
    if (!write_dwords(s, s.src_bar0, pattern, kSdmaCopyDwords, "s2-copy-source")) {
        evf(s.env, "sdma-seq", "S2 source buffer stage failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailRing);
    }

    uint32_t pkt[kWriteUntilData0Off + kSdmaCopyDwords + kWriteUntilData0Off + 1u];
    uint32_t n = 0;
    pkt[n++] = kSdmaOpWrite | (kSdmaSubopWriteLinear << kPktHeaderSubOpShift);
    pkt[n++] = static_cast<uint32_t>(s.dst_mc);
    pkt[n++] = static_cast<uint32_t>(s.dst_mc >> 32);
    pkt[n++] = kSdmaCopyDwords - 1u;
    for (uint32_t i = 0; i < kSdmaCopyDwords; i++)
        pkt[n++] = pattern[i];

    pkt[n++] = kSdmaOpWrite | (kSdmaSubopWriteLinear << kPktHeaderSubOpShift);
    pkt[n++] = static_cast<uint32_t>(s.fence_mc);
    pkt[n++] = static_cast<uint32_t>(s.fence_mc >> 32);
    pkt[n++] = 0u;
    pkt[n++] = kSdmaFenceMagic;
    s.wptr_bytes = n * 4u;

    if (!write_dwords(s, s.ring_bar0, pkt, n, "s2-ring-packet")) {
        evf(s.env, "sdma-seq", "S2 ring packet stage failed");
        return r3_substep_fail(kR3SecSeq, 2, kFailRing);
    }
    evf(s.env, "sdma-seq",
        "S2 copy src=0x%llx dst=0x%llx %u B; fence=0x%llx magic=0x%08x; "
        "ring packet=%u dwords wptr=%u B",
        static_cast<unsigned long long>(s.mc_base + s.src_bar0),
        static_cast<unsigned long long>(s.dst_mc),
        kSdmaCopyBytes,
        static_cast<unsigned long long>(s.fence_mc), kSdmaFenceMagic,
        n, s.wptr_bytes);

    return r3_substep_pass();
}

R3SubstepResult seq_wptr_shadow(Sdma &s)
{
    /* SDMA-01 / C-08: the runtime path publishes the write pointer through the shadow
     * copy before kicking. Linux sdma_v6_0_ring_set_wptr() stores lower_32_bits(wptr<<2)
     * into ring->wptr_cpu_addr, a 64-bit slot; the register receives the same value. */
    const uint64_t value = static_cast<uint64_t>(s.wptr_bytes);
    if (!bus_write(s, s.shadow_bar0, value, 2u, "s3-wptr-shadow")) {
        evf(s.env, "sdma-seq", "S3 wptr shadow write failed @bar0 0x%llx",
            static_cast<unsigned long long>(s.shadow_bar0));
        return r3_substep_fail(kR3SecSeq, 3, kFailShadow);
    }
    evf(s.env, "sdma-seq",
        "S3 wptr shadow=%u B @bar0 0x%llx (mc 0x%llx, 64-bit slot)",
        s.wptr_bytes, static_cast<unsigned long long>(s.shadow_bar0),
        static_cast<unsigned long long>(s.shadow_mc));
    return r3_substep_pass();
}

R3SubstepResult seq_queue_config(Sdma &s)
{
    const uint32_t rb_cntl = s.sdma0_base + kRegSdma0Queue0RbCntl;
    const uint32_t rb_base = s.sdma0_base + kRegSdma0Queue0RbBase;
    const uint32_t rb_base_hi = s.sdma0_base + kRegSdma0Queue0RbBaseHi;
    const uint32_t rb_rptr = s.sdma0_base + kRegSdma0Queue0RbRptr;
    const uint32_t rb_rptr_hi = s.sdma0_base + kRegSdma0Queue0RbRptrHi;
    const uint32_t rb_rptr_addr_hi = s.sdma0_base + kRegSdma0Queue0RbRptrAddrHi;
    const uint32_t rb_rptr_addr_lo = s.sdma0_base + kRegSdma0Queue0RbRptrAddrLo;
    const uint32_t rb_wptr_poll_hi = s.sdma0_base + kRegSdma0Queue0RbWptrPollAddrHi;
    const uint32_t rb_wptr_poll_lo = s.sdma0_base + kRegSdma0Queue0RbWptrPollAddrLo;

    bool ok = false;
    const uint32_t cur = reg_read(s, rb_cntl, &ok);
    if (!ok) {
        evf(s.env, "sdma-seq", "S4 RB_CNTL read failed @0x%x", rb_cntl);
        return r3_substep_fail(kR3SecSeq, 4, kFailQueue);
    }
    const uint32_t rb = rb_cntl_programmed(cur);

    /* S-1/S-2 (batch 1c): Linux sdma_v6_0_gfx_resume_instance() (sdma_v6_0.c:495-539)
     * writes the address-class registers first and defers RB_CNTL to the end of the
     * sequence (the value is only computed at :534-536 and lands on the hardware with
     * RB_ENABLE at :607-608), and the WPTR_POLL_ADDR pair is written LO then HI
     * (:523-526). This sequence matches that order: pointers/addresses first,
     * WPTR_POLL LO->HI, RB_CNTL last - its enable bits stay clear here and RB_ENABLE is
     * armed in S5 (sdma_v6_0.c:607-608). */
    if (!reg_write(s, rb_rptr, 0u, kR3WState, "s4-rptr") ||
        !reg_write(s, rb_rptr_hi, 0u, kR3WState, "s4-rptr-hi") ||
        !reg_write(s, rb_wptr_poll_lo, static_cast<uint32_t>(s.shadow_mc),
                   kR3WState, "s4-wptr-poll-lo") ||
        !reg_write(s, rb_wptr_poll_hi, static_cast<uint32_t>(s.shadow_mc >> 32),
                   kR3WState, "s4-wptr-poll-hi") ||
        !reg_write(s, rb_rptr_addr_hi, static_cast<uint32_t>(s.rptr_wb_mc >> 32),
                   kR3WState, "s4-rptr-wb-hi") ||
        !reg_write(s, rb_rptr_addr_lo,
                   static_cast<uint32_t>(s.rptr_wb_mc) & 0xFFFFFFFCu,
                   kR3WState, "s4-rptr-wb-lo") ||
        !reg_write(s, rb_base, static_cast<uint32_t>(s.ring_mc >> 8),
                   kR3WState, "s4-rb-base") ||
        !reg_write(s, rb_base_hi, static_cast<uint32_t>(s.ring_mc >> 40),
                   kR3WState, "s4-rb-base-hi") ||
        !reg_write(s, rb_cntl, rb, kR3WState, "s4-rb-cntl-config")) {
        evf(s.env, "sdma-seq", "S4 queue0 pointer/base write failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailQueue);
    }
    evf(s.env, "sdma-seq",
        "S4 queue config rb_cntl=0x%08x rb_base=0x%08x/0x%08x rptr_wb=0x%08x/0x%08x "
        "wptr_poll=0x%08x/0x%08x",
        rb, static_cast<uint32_t>(s.ring_mc >> 8),
        static_cast<uint32_t>(s.ring_mc >> 40),
        static_cast<uint32_t>(s.rptr_wb_mc >> 32),
        static_cast<uint32_t>(s.rptr_wb_mc) & 0xFFFFFFFCu,
        static_cast<uint32_t>(s.shadow_mc >> 32),
        static_cast<uint32_t>(s.shadow_mc));
    return r3_substep_pass();
}

R3SubstepResult seq_wptr_window(Sdma &s)
{
    const uint32_t minor = s.sdma0_base + kRegSdma0Queue0MinorPtrUpdate;
    const uint32_t rb_wptr = s.sdma0_base + kRegSdma0Queue0RbWptr;
    const uint32_t rb_wptr_hi = s.sdma0_base + kRegSdma0Queue0RbWptrHi;

    bool ok = false;
    const uint32_t before = reg_read(s, minor, &ok);
    if (!ok)
        evf(s.env, "sdma-seq",
            "S4 MINOR_PTR_UPDATE pre-read unreadable @0x%x (recorded, not gated)", minor);

    if (!reg_write(s, minor, kMinorPtrUpdateMask, kR3WProtocol, "s4-window-open")) {
        evf(s.env, "sdma-seq", "S4 MINOR_PTR_UPDATE=1 write failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailWindow);
    }
    bool pok = false;
    const uint32_t pre = reg_read(s, minor, &pok);
    s.window_pre = pre;
    snprintf(s.wptr_ctx, sizeof(s.wptr_ctx), "s4-wptr-window pre=%u post=pending", pre);

    if (!fence_barrier(s)) {
        evf(s.env, "sdma-seq", "S4 HDP flush failed before RB_WPTR");
        return r3_substep_fail(kR3SecSeq, 4, kFailHdp);
    }
    if (!reg_write(s, rb_wptr, s.wptr_bytes, kR3WProtocol, s.wptr_ctx) ||
        !reg_write(s, rb_wptr_hi, 0u, kR3WProtocol, s.wptr_ctx)) {
        snprintf(s.wptr_ctx, sizeof(s.wptr_ctx), "s4-wptr-window pre=%u post=write-failed",
                 pre);
        evf(s.env, "sdma-seq", "S4 RB_WPTR write failed (pre=0x%x)", pre);
        /* SF-3 (batch 1c, INV-B2): a failed RB_WPTR write must not leave the queue with
         * MINOR_PTR_UPDATE=1 (no half-updated queue config on the failure path); close
         * the window before the failure exit, mirroring the normal close below. The
         * cleanup is best-effort: if it also fails, record it and keep the original
         * failure reason. */
        if (!reg_write(s, minor, 0u, kR3WProtocol, "s4-window-close")) {
            evf(s.env, "sdma-seq",
                "S4 MINOR_PTR_UPDATE=0 cleanup write failed after RB_WPTR failure");
        }
        return r3_substep_fail(kR3SecSeq, 4, kFailWindow);
    }

    if (!reg_write(s, minor, 0u, kR3WProtocol, "s4-window-close")) {
        snprintf(s.wptr_ctx, sizeof(s.wptr_ctx), "s4-wptr-window pre=%u post=close-failed",
                 pre);
        evf(s.env, "sdma-seq", "S4 MINOR_PTR_UPDATE=0 write failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailWindow);
    }
    bool cok = false;
    const uint32_t post = reg_read(s, minor, &cok);
    s.window_post = post;
    snprintf(s.wptr_ctx, sizeof(s.wptr_ctx), "s4-wptr-window pre=%u post=%u", pre, post);

    /* SDMA-03 / C-09: the readbacks are manifest evidence only. Linux never reads this
     * bit back and neither does nullarc, so neither readback gates the substep. */
    if (!pok || !cok) {
        evf(s.env, "sdma-seq",
            "S4 MINOR_PTR_UPDATE readback unreadable (pre_ok=%d post_ok=%d)",
            pok ? 1 : 0, cok ? 1 : 0);
    }
    evf(s.env, "sdma-seq",
        "S4 WPTR window pre=%u post=%u before=%u wptr=%u B (readback recorded, not gated; "
        "rb_wptr=0x%x)",
        pre, post, before, s.wptr_bytes, rb_wptr);
    return r3_substep_pass();
}

R3SubstepResult seq_queue_enable(Sdma &s)
{
    const uint32_t rb_cntl = s.sdma0_base + kRegSdma0Queue0RbCntl;
    const uint32_t ib_cntl = s.sdma0_base + kRegSdma0Queue0IbCntl;
    const uint32_t f32_cntl = s.sdma_f32_base + kRegSdma0F32Cntl;
    const uint32_t sem_timer = s.sdma0_base + kRegSdma0SemWaitFailTimerCntl;
    const uint32_t watchdog = s.sdma0_base + kRegSdma0WatchdogCntl;
    const uint32_t doorbell = s.sdma0_base + kRegSdma0Queue0Doorbell;

    bool ok = false;

    /* SDMA-04: Linux clears the semaphore wait-fail timer and arms the hang counter;
     * clearing DOORBELL.ENABLE prevents a residue from kicking queue0. */
    if (!reg_write(s, sem_timer, 0u, kR3WState, "s5-sem-wait-timer") ||
        !reg_write(s, watchdog, kSdmaWatchdogHangCount & kSdmaWatchdogHangCountMask,
                   kR3WState, "s5-watchdog")) {
        evf(s.env, "sdma-seq", "S5 SEM_WAIT_FAIL_TIMER/WATCHDOG write failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }
    uint32_t cur_db = reg_read(s, doorbell, &ok);
    if (!ok) {
        evf(s.env, "sdma-seq", "S5 QUEUE0_DOORBELL read failed @0x%x", doorbell);
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }
    const uint32_t db = field_clear(cur_db, kSdmaQueue0DoorbellEnableMask);
    if (!fence_barrier(s)) {
        evf(s.env, "sdma-seq", "S5 HDP flush failed before doorbell");
        return r3_substep_fail(kR3SecSeq, 5, kFailHdp);
    }
    if (!reg_write(s, doorbell, db, kR3WState, "s5-doorbell-enable-clear")) {
        evf(s.env, "sdma-seq", "S5 QUEUE0_DOORBELL.ENABLE=0 write failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }

    /* Linux order: unhalt the engine before RB_ENABLE, then IB_ENABLE. */
    const uint32_t cur_rb = reg_read(s, rb_cntl, &ok);
    if (!ok) {
        evf(s.env, "sdma-seq", "S5 RB_CNTL read failed @0x%x", rb_cntl);
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }
    const uint32_t cur_f32 = reg_read(s, f32_cntl, &ok);
    if (!ok) {
        evf(s.env, "sdma-seq", "S5 F32_CNTL read failed @0x%x", f32_cntl);
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }
    const uint32_t f32 = field_clear(field_clear(cur_f32, kF32CntlHaltMask),
                                     kF32CntlTh1ResetMask);
    if (!reg_write(s, f32_cntl, f32, kR3WState, "s5-f32-unhalt") ||
        !reg_write(s, rb_cntl, cur_rb | kRbCntlRbEnableMask, kR3WState, "s5-rb-enable")) {
        evf(s.env, "sdma-seq", "S5 F32/RB enable failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }

    const uint32_t cur_ib = reg_read(s, ib_cntl, &ok);
    if (!ok) {
        evf(s.env, "sdma-seq", "S5 IB_CNTL read failed @0x%x", ib_cntl);
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }
    if (!reg_write(s, ib_cntl, cur_ib | kIbCntlIbEnableMask, kR3WState,
                   "s5-ib-enable")) {
        evf(s.env, "sdma-seq", "S5 IB_CNTL enable failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailQueue);
    }

    evf(s.env, "sdma-seq",
        "S5 sem_timer=0 watchdog=%u doorbell=0x%08x f32=0x%08x rb=0x%08x ib=0x%08x",
        kSdmaWatchdogHangCount & kSdmaWatchdogHangCountMask, db, f32,
        cur_rb | kRbCntlRbEnableMask, cur_ib | kIbCntlIbEnableMask);
    return r3_substep_pass();
}

R3SubstepResult seq_fence(Sdma &s)
{
    const uint64_t t0 = s.io->now_us ? s.io->now_us(s.io->ctx) : 0ull;
    uint32_t polls = 0;
    uint32_t last = 0;
    bool found = false;

    for (; polls < kSdmaFencePolls; polls++) {
        uint64_t v = 0;
        if (!bus_read(s, s.fence_bar0, 1u, &v)) {
            evf(s.env, "sdma-seq", "S6 fence read failed @bar0 0x%llx",
                static_cast<unsigned long long>(s.fence_bar0));
            return r3_substep_fail(kR3SecSeq, 6, kFailFence);
        }
        last = static_cast<uint32_t>(v);
        if (last == kSdmaFenceMagic) {
            found = true;
            break;
        }
        r3_io_udelay(s.io, kSdmaFencePollUs);
    }

    s.fence_measured = s.io->now_us != nullptr;
    s.fence_us = s.fence_measured
                     ? static_cast<uint32_t>(s.io->now_us(s.io->ctx) - t0)
                     : polls * kSdmaFencePollUs;
    if (!found) {
        evf(s.env, "sdma-seq",
            "S6 fence timeout after %u us (%u polls), last=0x%08x want=0x%08x",
            s.fence_us, polls, last, kSdmaFenceMagic);
        return r3_substep_fail(kR3SecSeq, 6, kFailFence);
    }

    evf(s.env, "sdma-seq", "S6 fence=0x%08x after %u us (%u polls)",
        last, s.fence_us, polls);
    return r3_substep_pass();
}

R3SubstepResult chk_readback(Sdma &s)
{
    uint8_t got[kSdmaCopyBytes];
    for (uint32_t off = 0; off < kSdmaCopyBytes; off += 8u) {
        uint64_t v = 0;
        if (!bus_read(s, s.dst_bar0 + off, 2u, &v)) {
            evf(s.env, "sdma-chk", "S7 target read failed @bar0 0x%llx",
                static_cast<unsigned long long>(s.dst_bar0 + off));
            return r3_substep_fail(kR3SecChk, 7, kFailReadUnread);
        }
        memcpy(&got[off], &v, 8u);
    }

    uint32_t pattern[kSdmaCopyDwords];
    fill_pattern(pattern, kSdmaCopyDwords);
    const uint8_t *want = reinterpret_cast<const uint8_t *>(pattern);

    for (uint32_t i = 0; i < kSdmaCopyBytes; i++) {
        if (got[i] != want[i]) {
            R3SubstepResult r = r3_substep_fail(kR3SecChk, 7, kFailReadMismatch);
            r3_substep_note(&r, static_cast<uint32_t>(s.dst_bar0 + i), want[i],
                            "expected");
            r3_substep_note(&r, static_cast<uint32_t>(s.dst_bar0 + i), got[i], "actual");
            r3_substep_note(&r, static_cast<uint32_t>(s.dst_bar0 + i), i, "offset");
            evf(s.env, "sdma-chk",
                "S7 target mismatch @+%u want=0x%02x got=0x%02x (dst_mc=0x%llx)",
                i, want[i], got[i], static_cast<unsigned long long>(s.dst_mc));
            return r;
        }
    }

    s.copy_ok = true;
    evf(s.env, "sdma-chk", "S7 target copy ok: %u B byte-identical @mc 0x%llx",
        kSdmaCopyBytes, static_cast<unsigned long long>(s.dst_mc));
    return r3_substep_pass();
}

R3SubstepResult chk_status(Sdma &s)
{
    const uint32_t status = s.sdma0_base + kRegSdma0StatusReg;
    bool ok = false;
    const uint32_t v = reg_read(s, status, &ok);
    s.status_value = v;
    if (!ok) {
        R3SubstepResult r = r3_substep_fail(kR3SecChk, 8, kFailStatus);
        r3_substep_note(&r, status, v, "SDMA0_STATUS_REG");
        evf(s.env, "sdma-chk", "S8 SDMA0_STATUS_REG @0x%x unreadable", status);
        return r;
    }

    /* C-12 Deviation: MC passthrough needs no UTCL1 VMID/page programming (nullarc
     * evidence on the same silicon). Stage 3 will add the Linux UTCL1 setup. Record the
     * current defaults; an unreadable value is recorded, never gated. */
    const uint32_t utcl1_cntl = s.sdma0_base + kRegSdma0Utcl1Cntl;
    const uint32_t utcl1_page = s.sdma0_base + kRegSdma0Utcl1Page;
    bool uok = false, pok = false;
    s.utcl1_cntl = reg_read(s, utcl1_cntl, &uok);
    s.utcl1_page = reg_read(s, utcl1_page, &pok);
    s.utcl1_ok = uok && pok;
    evf(s.env, "sdma-chk",
        "S8 SDMA0_STATUS_REG=0x%08x idle=%u rb_empty=%u (cross evidence); "
        "UTCL1_CNTL=0x%08x%s UTCL1_PAGE=0x%08x%s (defaults, C-12 Deviation)",
        v, (v & kStatusIdleMask) ? 1u : 0u, (v & kStatusRbEmptyMask) ? 1u : 0u,
        s.utcl1_cntl, uok ? "" : "(unreadable)", s.utcl1_page,
        pok ? "" : "(unreadable)");

    /* D3/§8.2: SDMA0_VERSION post observation (after F32 unhalt). Evidence only. */
    bool vokp = false;
    s.sdma0_version_post = reg_read(s, s.sdma0_base + kRegSdma0Version, &vokp);
    s.sdma0_version_post_ok = vokp;
    if (s.sdma0_version_pre_ok) {
        evf(s.env, "sdma-chk",
            "S8b sdma0_version pre=0x%08x post=0x%08x%s (observation, not a gate)",
            s.sdma0_version_pre, s.sdma0_version_post, vokp ? "" : " (unreadable)");
    } else {
        evf(s.env, "sdma-chk",
            "S8b sdma0_version pre=---- post=0x%08x%s (observation, not a gate)",
            s.sdma0_version_post, vokp ? "" : " (unreadable)");
    }
    return r3_substep_pass();
}

} /* namespace */

R3SubstepResult sdma_v6_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    static Sdma s;
    memset(&s, 0, sizeof(s));
    s.io = io;
    s.env = env;

    r3_wg_arm_substep(WSubstep::Sdma);
    r3_wg_expect_writes(getSdmaRows(), getSdmaRowCount(), nullptr, 0, WSubstep::Sdma);
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env ? env->ip : nullptr, getSdmaRows(), getSdmaRowCount(),
                            declared);
    r3_wg_expect_declared_addrs(declared, getSdmaRowCount());

    R3SubstepResult r = pre(s);
    if (r.status == kR3SsecOk)
        r = seq_zero_and_stage(s);
    if (r.status == kR3SsecOk)
        r = seq_wptr_shadow(s);
    if (r.status == kR3SsecOk)
        r = seq_queue_config(s);
    if (r.status == kR3SsecOk)
        r = seq_wptr_window(s);
    if (r.status == kR3SsecOk)
        r = seq_queue_enable(s);
    if (r.status == kR3SsecOk)
        r = seq_fence(s);
    if (r.status == kR3SsecOk)
        r = chk_readback(s);
    if (r.status == kR3SsecOk)
        r = chk_status(s);

    if (r.status == kR3SsecOk) {
        if (env && env->rt)
            env->rt->ready_bits |= R3_IP_BIT(R3Ip::Sdma);
        set_prop(env, "N48R3EVID-sdma-done", "Yes");
        set_prop(env, "N48R3EVID-sdma-copy", "ok");
        snprintf(s.wptr_prop, sizeof(s.wptr_prop), "pre=%u post=%u",
                 s.window_pre, s.window_post);
        set_prop(env, "N48R3EVID-sdma-wptr", s.wptr_prop);
        evf(env, "sdma",
            "done: copy ok, wptr pre=%u post=%u, fence=%u us, status=0x%08x",
            s.window_pre, s.window_post, s.fence_us, s.status_value);
    } else {
        set_prop(env, "N48R3EVID-sdma-done", "No");
        if (s.copy_ok)
            set_prop(env, "N48R3EVID-sdma-copy", "ok");
        else if (r.section == kR3SecChk && r.step == 7)
            set_prop(env, "N48R3EVID-sdma-copy",
                     strcmp(r.name, kFailReadUnread) == 0 ? "unreadable" : "mismatch");
        else
            set_prop(env, "N48R3EVID-sdma-copy", "n/a");
        snprintf(s.wptr_prop, sizeof(s.wptr_prop), "pre=%u post=%u",
                 s.window_pre, s.window_post);
        set_prop(env, "N48R3EVID-sdma-wptr", s.wptr_prop);
        evf(env, "sdma", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
    }

    // Step3: verify expectations before disarming
    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getSdmaRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }


    r3_wg_disarm_substep();
    return r;
}

/* Step2: declaration table (rdna3_writeguard.h R3WriteRowReg), the single source of truth
 * for the SDMA register write set. 17 rows, P5: unique address per row. */
namespace {
constexpr R3WriteRowReg kSdmaRows[] = {
    {&r3gold::kSdma0Queue0RbCntl,       static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::Required,      "s4-rb-cntl"},
    {&r3gold::kSdma0Queue0RbBase,       static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::Required,      "s4-rb-base"},
    {&r3gold::kSdma0Queue0RbBaseHi,     static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::Required,      "s4-rb-base-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0RbRptr, 1, WClass::State,          WNeed::Required,      "s4-rptr"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0RbRptrHi, 1, WClass::State,        WNeed::Required,      "s4-rptr-hi"},
    {&r3gold::kSdma0Queue0RbWptr,       static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::Protocol, WNeed::OnSuccessPath, "s4-wptr"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0RbWptrHi, 1, WClass::Protocol,     WNeed::OnSuccessPath, "s4-wptr-hi"},
    {&r3gold::kSdma0Queue0RbRptrAddrHi, static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::Required,      "s4-rptr-wb-hi"},
    {&r3gold::kSdma0Queue0RbRptrAddrLo, static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::Required,      "s4-rptr-wb-lo"},
    {&r3gold::kSdma0Queue0IbCntl,       static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::State,    WNeed::OnSuccessPath, "s4-ib-cntl"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0RbWptrPollAddrHi, 1, WClass::State, WNeed::Required,     "s4-wptr-poll-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0RbWptrPollAddrLo, 1, WClass::State, WNeed::Required,     "s4-wptr-poll-lo"},
    {&r3gold::kSdma0Queue0MinorPtrUpdate, static_cast<uint8_t>(IPBlock::SDMA0), 0, 0, 1, WClass::Protocol, WNeed::Required,    "s4-window"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0SemWaitFailTimerCntl, 1, WClass::State,  WNeed::Required,      "s5-sem-wait-timer"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0WatchdogCntl, 1, WClass::State,          WNeed::Required,      "s5-watchdog"},
    {nullptr, static_cast<uint8_t>(IPBlock::SDMA0), 0, kRegSdma0Queue0Doorbell, 1, WClass::State,         WNeed::OnSuccessPath, "s5-doorbell"},
    {&r3gold::kSdma0F32Cntl,            static_cast<uint8_t>(IPBlock::SDMA0), 1, 0, 1, WClass::State,    WNeed::Required,      "s5-f32-unhalt"},
};
constexpr uint32_t kSdmaRowCount = sizeof(kSdmaRows) / sizeof(kSdmaRows[0]);
static_assert(kSdmaRowCount == 17, "sdma declaration table must have 17 rows");
constexpr const char *kSdmaLayoutTag = "sdma";
} // anonymous namespace

const R3WriteRowReg* getSdmaRows(void) { return kSdmaRows; }
uint32_t getSdmaRowCount(void) { return kSdmaRowCount; }
const char* getSdmaLayoutTag(void) { return kSdmaLayoutTag; }

} /* namespace amdgpu */
