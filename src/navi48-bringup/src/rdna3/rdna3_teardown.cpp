/*
 * rdna3_teardown.cpp - reverse-order teardown (tech plan §9, requirement F5).
 *
 * Nothing here is invented: every write has a Linux call site in the matching resume
 * routine and the register itself was programmed by the substep being reversed.
 *   MES  CP_HQD_ACTIVE=0 per (me3, pipe) + CP_MES_CNTL RESET
 *        (mes_v11_0_destroy_queue / mes_v11_0_enable(false), mes_v11_0.c:1529-1537,
 *         1007-1016; reverse of seq_start/program_hqd in mes_v11_0.cpp).
 *   SDMA SDMA0_QUEUE0_RB_CNTL.RB_ENABLE=0 + SDMA0_F32_CNTL.HALT=1
 *        (sdma_v6_0_gfx_resume_instance, sdma_v6_0.c:401-403; sdma_v6_0_enable,
 *         sdma_v6_0.c:467-469; reverse of seq_program_queue in sdma_v6_0.cpp).
 *   GFX  no action: the GFX substep writes nothing (tech plan §3.3).
 *   SMU  no action: the inverse of EnableGfxImu is a power-policy decision with no Linux
 *        call site and no nullarc evidence (C0.7). Logged, not performed.
 *   PSP  never reversed (F5): no inverse sequence exists in either source.
 *
 * Failure containment: every write is preceded by a classify() against the power-up table
 * under the matching owner and followed by a readback. No write is retried, no wait is
 * unbounded (the HQD deactivate poll is 100 x 10 us), and a failed IP action does not stop
 * the remaining IPs. This makes the worst form "quiet failure + complete log".
 */
#include "rdna3_teardown.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "amd/mes_v11_0.h"
#include "amd/gfx_v11_0.h"
#include "amd/sdma_v6_0.h"
#include "rdna3_ip_entries.h"
#include "rdna3_layout.h"
#include "rdna3_writeguard.h"

namespace {

using amdgpu::IPBaseTable;
using amdgpu::IPBlock;

/* The reverse power-up order (existing IPs only; full spec order compute/abi->queue->
 * doorbell/IH->vmid8->gpuvm0->gart->mes->sdma->gfx->smu->psp is the T-E target when all
 * S6-S13 stages are wired). T-D adds IH disable (td_ih) as a separate OSSSYS call before
 * the IP loop since IH does not map to a power-up R3Ip.
 * REMOVE_QUEUE via MES API is not performed here: the teardown is register-only (GRBM/GPUIO
 * via R3Io). The S11 ADD_QUEUE queues are HW-deactivated via CP_HQD_ACTIVE=0 in td_mes
 * (register-level reverse of program_hqd). A MES API REMOVE_QUEUE would need SCH-ring/
 * doorbell infrastructure (mes_v11_0.cpp) and is deferred to when the teardown gains a
 * bus-level submission path. */
constexpr R3Ip kOrder[4] = {R3Ip::Mes, R3Ip::Sdma, R3Ip::Gfx, R3Ip::Smu};

/* Bounded deactivate wait: Linux mes_v11_0_destroy_queue polls usec_timeout x 1 us; a
 * 1 ms ceiling is the same order and can never hang the boot. */
constexpr uint32_t kHqdPollUs = 10u;
constexpr uint32_t kHqdPolls = 100u;

struct TdLog {
    const R3TeardownEnv *env;
    char buf[1280];
    size_t used;
    uint32_t writes;
    bool full;
};

void td_line(TdLog &l, const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1u] = '\0';

    if (l.env && l.env->sub && l.env->sub->ev)
        r3_ev_add(l.env->sub->ev, "teardown", line);

    if (!l.full && l.used + 1u < sizeof l.buf) {
        const int n = snprintf(l.buf + l.used, sizeof l.buf - l.used, "%s\n", line);
        if (n > 0 && static_cast<size_t>(n) < sizeof l.buf - l.used)
            l.used += static_cast<size_t>(n);
        else
            l.full = true;
    }
}

void td_publish(TdLog &l)
{
    if (l.env && l.env->sub && l.env->sub->sink && l.env->sub->sink->set_property &&
        l.used > 0u)
        l.env->sub->sink->set_property(l.env->sub->sink->user, "N48R3EVID-teardown",
                                      l.buf, l.used);
}

const IPBaseTable *td_ip(const R3TeardownEnv *env)
{
    return (env && env->sub) ? static_cast<const IPBaseTable *>(env->sub->ip) : nullptr;
}

bool td_read(const R3TeardownEnv *env, uint32_t addr, uint32_t *value)
{
    if (!env || !env->io || !env->io->mmio || !value)
        return false;
    bool ok = false;
    *value = r3_io_read32(env->io, addr, &ok);
    return ok;
}

/* INV-D13: a reversible object is torn down only when the power-up substep really wrote it.
 * Registration alone is not enough (the row may exist because the plan declares it), so the
 * predicate is "registered for this owner/class AND at least one ALLOW write went through it".
 * A missing row means the substep did not run: the caller skips and logs "no reversible
 * object" instead of widening the whitelist. */
bool td_ensure_reg(uint32_t addr, WClass cls, WSubstep owner, const char *tag)
{
    (void)tag;
    const uint32_t n = r3_wg_grant_count();
    for (uint32_t i = 0; i < n; i++) {
        const R3WgGrant *g = r3_wg_grant(i);
        if (g && !g->range && g->start == addr && g->dwords == 1u &&
            g->owner == owner && g->cls == cls)
            return r3_wg_was_written(addr);
    }
    return false;
}

/* T-E.11: teardown-only rows are declared for the inverse and never written at power-up,
 * so their precondition is registration alone (the row is the evidence that the object
 * exists). Same shape as td_ensure_reg, different predicate — never merge the two. */
bool td_ensure_teardown_reg(uint32_t addr, WClass cls, WSubstep owner, const char *tag)
{
    (void)tag;
    const uint32_t n = r3_wg_grant_count();
    for (uint32_t i = 0; i < n; i++) {
        const R3WgGrant *g = r3_wg_grant(i);
        if (g && !g->range && g->start == addr && g->dwords == 1u &&
            g->owner == owner && g->cls == cls)
            return true;
    }
    return false;
}

/* One bounded write: classify against the armed owner, write, then read back. The
 * readback is the boundary §9 demands; a failure is logged and returned, never retried. */
bool td_write(TdLog &l, uint32_t addr, uint32_t value, const char *ctx,
              const char *what, uint32_t *readback)
{
    const R3TeardownEnv *env = l.env;
    if (!env || !env->io || !env->io->mmio) {
        td_line(l, "teardown: io-missing %s", what);
        return false;
    }
    if (r3_wg_classify(addr, 1u, WClass::State, ctx) != WVerdict::TxnRead) {
        td_line(l, "teardown: write-deny %s @0x%08x", what, addr);
        return false;
    }
    if (!r3_io_write32(env->io, addr, value)) {
        td_line(l, "teardown: write-fail %s @0x%08x", what, addr);
        return false;
    }
    l.writes++;

    uint32_t v = 0;
    if (!td_read(env, addr, &v)) {
        td_line(l, "teardown: readback-fail %s @0x%08x", what, addr);
        return false;
    }
    if (readback)
        *readback = v;
    return true;
}

uint32_t grbm_select(uint32_t me, uint32_t pipe)
{
    return (pipe << amdgpu::kGrbmPipeIdShift) | (me << amdgpu::kGrbmMeIdShift);
}

/* --- IH -------------------------------------------------------------------------------- */
/* T-E.7: the inverse of S10.0, matching Linux ih_v6_0_toggle_ring_interrupts(..., false)
 * (ih_v6_0.c:127-188): a single RMW of IH_RB_CNTL clears RB_ENABLE (:137) and ENABLE_INTR
 * (:168, ring0 only), then IH_RB_RPTR/WPTR are zeroed (:181-182). Only those bits are
 * touched - the ring geometry and the other fields stay as power-up left them.
 * These three addresses are also written during power-up (S10.2 base/cntl/rptr/wptr,
 * S10.3 enable), so INV-D13 applies: td_ensure_reg requires each to have been written.
 * Returns 1 when the action ran, 0 when they were never written (skip), -1 on a
 * read/write/readback failure (counted by the caller, never fatal). */
int td_ih(TdLog &l)
{
    const IPBaseTable *ip = td_ip(l.env);
    if (!ip || !ip->isResolved(IPBlock::OSSSYS, 0)) {
        td_line(l, "teardown: ih fail (OSSSYS base unresolved)");
        return -1;
    }
    const uint32_t osssys0 = ip->getBase(IPBlock::OSSSYS, 0);
    /* Offsets from osssys_6_0_0_offset.h (IH v6, not v7). IH_RB_CNTL=0x0080 (same
     * in both); IH_RB_RPTR=0x0083, IH_RB_WPTR=0x0084 (v6-specific, differ from v7). */
    const uint32_t rb_cntl = osssys0 + 0x0080u;
    const uint32_t rb_rptr = osssys0 + 0x0083u;
    const uint32_t rb_wptr = osssys0 + 0x0084u;

    if (!td_ensure_reg(rb_cntl, WClass::State, WSubstep::Ih, "reg:teardown:ih-rb-cntl") ||
        !td_ensure_reg(rb_rptr, WClass::State, WSubstep::Ih, "reg:teardown:ih-rb-rptr") ||
        !td_ensure_reg(rb_wptr, WClass::State, WSubstep::Ih, "reg:teardown:ih-rb-wptr")) {
        td_line(l, "teardown: ih skip (no reversible object)");
        return 0;
    }

    r3_wg_arm_substep(WSubstep::Ih);
    bool ok = true;
    const char *ctx = "teardown:ih";

    uint32_t before = 0, after = 0;
    if (!td_read(l.env, rb_cntl, &before)) {
        td_line(l, "teardown: ih RB_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t value =
            before & ~(amdgpu::kIhRbCntlRbEnable | amdgpu::kIhRbCntlEnableIntr);
        if (!td_write(l, rb_cntl, value, ctx, "ih-rb-cntl", &after)) {
            ok = false;
        } else {
            td_line(l, "teardown: ih RB_CNTL 0x%08x -> 0x%08x (interrupts off)", before,
                    after);
        }
    }

    /* :181-182 set rptr, wptr to 0. */
    if (!td_write(l, rb_rptr, 0u, ctx, "ih-rb-rptr", nullptr))
        ok = false;
    if (!td_write(l, rb_wptr, 0u, ctx, "ih-rb-wptr", nullptr))
        ok = false;

    r3_wg_disarm_substep();
    return ok ? 1 : -1;
}

/* --- MES ------------------------------------------------------------------------------ */
/* T-E.3, one queue: GRBM pipe select -> read CP_HQD_ACTIVE -> if active, DEQUEUE and poll
 * ACTIVE clear (bounded) -> close the doorbell -> clear WPTR/RPTR -> deselect. An already
 * inactive queue is logged and left untouched (idempotent: no DEQUEUE, no pointer writes). */
bool td_mes_queue(TdLog &l, uint32_t grbm, uint32_t active, uint32_t dequeue,
                  uint32_t dbctl, uint32_t wptr_lo, uint32_t wptr_hi, uint32_t rptr,
                  uint32_t pipe, const char *name)
{
    const char *ctx = "teardown:mes";
    if (!td_write(l, grbm, grbm_select(3u, pipe), ctx, "mes-grbm-select", nullptr))
        return false;

    uint32_t before = 0;
    if (!td_read(l.env, active, &before)) {
        td_line(l, "teardown: mes %s hqd-active unreadable", name);
        (void)td_write(l, grbm, grbm_select(0u, 0u), ctx, "mes-grbm-deselect", nullptr);
        return false;
    }
    if ((before & 1u) == 0u) {
        td_line(l, "teardown: mes %s already inactive", name);
        return td_write(l, grbm, grbm_select(0u, 0u), ctx, "mes-grbm-deselect", nullptr);
    }

    if (!td_write(l, dequeue, 1u, ctx, "mes-hqd-dequeue", nullptr)) {
        (void)td_write(l, grbm, grbm_select(0u, 0u), ctx, "mes-grbm-deselect", nullptr);
        return false;
    }

    uint32_t after = 0;
    const int p = r3_io_poll(l.env->io, active, 1u, 0u, kHqdPolls, kHqdPollUs, &after);
    bool ok = true;
    if (p != 1) {
        /* Linux continues after the bounded wait: the queue is not clear, but the
         * doorbell/pointers are still closed below and the failure is reported. */
        td_line(l, "teardown: mes %s dequeue 0x%08x -> 0x%08x (not clear; continuing)",
                name, before, after);
        ok = false;
    }

    uint32_t db_before = 0;
    if (!td_read(l.env, dbctl, &db_before)) {
        td_line(l, "teardown: mes %s doorbell-control unreadable", name);
        ok = false;
    } else {
        const uint32_t db_off = (db_before & ~amdgpu::kCpHqdPqDoorbellEn) |
                                amdgpu::kCpHqdPqDoorbellHit;
        if (!td_write(l, dbctl, db_off, ctx, "mes-hqd-doorbell", nullptr) ||
            !td_write(l, dbctl, 0u, ctx, "mes-hqd-doorbell-clear", nullptr))
            ok = false;
    }

    if (!td_write(l, wptr_lo, 0u, ctx, "mes-hqd-wptr-lo", nullptr) ||
        !td_write(l, wptr_hi, 0u, ctx, "mes-hqd-wptr-hi", nullptr) ||
        !td_write(l, rptr, 0u, ctx, "mes-hqd-rptr", nullptr))
        ok = false;

    if (p == 1)
        td_line(l, "teardown: mes %s hqd-active 0x%08x -> 0x%08x (dequeued)", name,
                before, after);
    if (!td_write(l, grbm, grbm_select(0u, 0u), ctx, "mes-grbm-deselect", nullptr))
        ok = false;
    return ok;
}

bool td_mes(TdLog &l)
{
    const IPBaseTable *ip = td_ip(l.env);
    if (!ip || !ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1)) {
        td_line(l, "teardown: mes fail (GC base unresolved)");
        return false;
    }
    const uint32_t gc0 = ip->getBase(IPBlock::GC, 0);
    const uint32_t gc1 = ip->getBase(IPBlock::GC, 1);
    const uint32_t grbm = gc1 + amdgpu::kRegGrbmGfxCntl;
    const uint32_t active = gc0 + amdgpu::kRegCpHqdActive;
    const uint32_t cntl = gc1 + amdgpu::kRegMesCntl;
    const uint32_t dequeue = gc0 + amdgpu::kCpHqdDequeue;
    const uint32_t dbctl = gc0 + amdgpu::kRegCpHqdPqDoorbellControl;
    const uint32_t wptr_lo = gc0 + amdgpu::kCpHqdPqWptrLo;
    const uint32_t wptr_hi = gc0 + amdgpu::kCpHqdPqWptrHi;
    const uint32_t rptr = gc0 + amdgpu::kRegCpHqdPqRptr;

    /* Halt precondition (T-E.4): S1 wrote GRBM and CP_MES_CNTL once a MES instance exists. */
    if (!td_ensure_reg(grbm, WClass::State, WSubstep::Mes, "reg:teardown-mes") ||
        !td_ensure_reg(cntl, WClass::State, WSubstep::Mes, "reg:teardown-mes")) {
        td_line(l, "teardown: mes skip (no reversible object; power-up rows absent)");
        return true;
    }

    r3_wg_arm_substep(WSubstep::Mes);
    bool ok = true;

    /* Queue inverse (T-E.3). DEQUEUE and the WPTR pair are declared for the inverse, so
     * registration is the evidence (T-E.11); ACTIVE and the doorbell control were written
     * by program_hqd, so INV-D13 needs a real power-up write. A missing queue row drops
     * only the queue step - the halt below still runs (T-E.8). */
    const bool queue_rows =
        td_ensure_teardown_reg(dequeue, WClass::State, WSubstep::Mes, "reg:teardown:mes") &&
        td_ensure_teardown_reg(wptr_lo, WClass::State, WSubstep::Mes, "reg:teardown:mes") &&
        td_ensure_teardown_reg(wptr_hi, WClass::State, WSubstep::Mes, "reg:teardown:mes") &&
        td_ensure_teardown_reg(rptr, WClass::State, WSubstep::Mes, "reg:teardown:mes");
    const bool queue_objs =
        td_ensure_reg(active, WClass::State, WSubstep::Mes, "reg:teardown-mes") &&
        td_ensure_reg(dbctl, WClass::State, WSubstep::Mes, "reg:teardown-mes");
    if (queue_rows && queue_objs) {
        /* Reverse of seq_sch then seq_kiq (SCH = pipe0, KIQ = pipe1). */
        ok &= td_mes_queue(l, grbm, active, dequeue, dbctl, wptr_lo, wptr_hi, rptr, 0u, "sch");
        ok &= td_mes_queue(l, grbm, active, dequeue, dbctl, wptr_lo, wptr_hi, rptr, 1u, "kiq");
    } else {
        td_line(l, "teardown: mes queue rows absent (halt only)");
    }

    /* T-E.4: one RMW clears PIPE0/1_ACTIVE and sets INVALIDATE_ICACHE, PIPE0/1_RESET and
     * HALT. The PIPE1_RESET form matches seq_start (both pipes reset on init). */
    uint32_t before = 0;
    if (!td_read(l.env, cntl, &before)) {
        td_line(l, "teardown: mes CP_MES_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t want =
            (before & ~(amdgpu::kMesCntlPipe0Active | amdgpu::kMesCntlPipe1Active)) |
            amdgpu::kMesCntlPipe0Reset | amdgpu::kMesCntlPipe1Reset |
            amdgpu::kMesCntlInvalidateIcache | amdgpu::kMesCntlHalt;
        uint32_t after = 0;
        const uint32_t need = amdgpu::kMesCntlHalt | amdgpu::kMesCntlPipe0Reset |
                              amdgpu::kMesCntlPipe1Reset;
        if (!td_write(l, cntl, want, "teardown:mes", "mes-cntl-halt", &after)) {
            ok = false;
        } else if ((after & need) != need) {
            td_line(l, "teardown: mes CP_MES_CNTL readback=0x%08x (halt/reset bits missing)",
                    after);
            ok = false;
        } else {
            td_line(l, "teardown: mes CP_MES_CNTL 0x%08x -> 0x%08x (halted)", before, after);
        }
    }

    r3_wg_disarm_substep();
    return ok;
}

/* --- SDMA ----------------------------------------------------------------------------- */
bool td_sdma(TdLog &l)
{
    const IPBaseTable *ip = td_ip(l.env);
    if (!ip || !ip->isResolved(IPBlock::SDMA0, 0) ||
        !ip->isResolved(IPBlock::SDMA0, 1)) {
        td_line(l, "teardown: sdma fail (SDMA0 base unresolved)");
        return false;
    }
    const uint32_t sdma0 = ip->getBase(IPBlock::SDMA0, 0);
    const uint32_t sdma1 = ip->getBase(IPBlock::SDMA0, 1);
    const uint32_t rb = sdma0 + amdgpu::kRegSdma0Queue0RbCntl;
    const uint32_t ib = sdma0 + amdgpu::kRegSdma0Queue0IbCntl;
    const uint32_t f32 = sdma1 + amdgpu::kRegSdma0F32Cntl;

    if (!td_ensure_reg(rb, WClass::State, WSubstep::Sdma, "reg:teardown-sdma") ||
        !td_ensure_reg(f32, WClass::State, WSubstep::Sdma, "reg:teardown-sdma")) {
        td_line(l, "teardown: sdma skip (no reversible object; power-up rows absent)");
        return true;
    }

    r3_wg_arm_substep(WSubstep::Sdma);
    bool ok = true;

    uint32_t rb_before = 0;
    if (!td_read(l.env, rb, &rb_before)) {
        td_line(l, "teardown: sdma RB_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t rb_want = rb_before & ~amdgpu::kRbCntlRbEnableMask;
        uint32_t rb_after = 0;
        if (!td_write(l, rb, rb_want, "teardown:sdma", "sdma-rb-disable", &rb_after)) {
            ok = false;
        } else if ((rb_after & amdgpu::kRbCntlRbEnableMask) != 0u) {
            td_line(l, "teardown: sdma RB_CNTL readback=0x%08x (RB_ENABLE still set)",
                    rb_after);
            ok = false;
        } else {
            td_line(l, "teardown: sdma RB_CNTL 0x%08x -> 0x%08x (RB_ENABLE=0)",
                    rb_before, rb_after);
        }
    }

    /* T-E.2: RB -> IB -> F32. IB is OnSuccessPath (S4); when its row was never written
     * the IB step is skipped on its own without taking RB/F32 down with it. */
    if (td_ensure_reg(ib, WClass::State, WSubstep::Sdma, "reg:teardown-sdma")) {
        uint32_t ib_before = 0;
        if (!td_read(l.env, ib, &ib_before)) {
            td_line(l, "teardown: sdma IB_CNTL unreadable");
            ok = false;
        } else {
            const uint32_t ib_want = ib_before & ~amdgpu::kIbCntlIbEnableMask;
            uint32_t ib_after = 0;
            if (!td_write(l, ib, ib_want, "teardown:sdma", "sdma-ib-disable", &ib_after)) {
                ok = false;
            } else if ((ib_after & amdgpu::kIbCntlIbEnableMask) != 0u) {
                td_line(l, "teardown: sdma IB_CNTL readback=0x%08x (IB_ENABLE still set)",
                        ib_after);
                ok = false;
            } else {
                td_line(l, "teardown: sdma IB_CNTL 0x%08x -> 0x%08x (IB_ENABLE=0)",
                        ib_before, ib_after);
            }
        }
    } else {
        td_line(l, "teardown: sdma IB_CNTL not reversible; ib step skipped");
    }

    uint32_t f32_before = 0;
    if (!td_read(l.env, f32, &f32_before)) {
        td_line(l, "teardown: sdma F32_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t f32_want = f32_before | amdgpu::kF32CntlHaltMask;
        uint32_t f32_after = 0;
        if (!td_write(l, f32, f32_want, "teardown:sdma", "sdma-f32-halt", &f32_after)) {
            ok = false;
        } else if ((f32_after & amdgpu::kF32CntlHaltMask) == 0u) {
            td_line(l, "teardown: sdma F32_CNTL readback=0x%08x (HALT missing)",
                    f32_after);
            ok = false;
        } else {
            td_line(l, "teardown: sdma F32_CNTL 0x%08x -> 0x%08x (HALT=1)",
                    f32_before, f32_after);
        }
    }

    r3_wg_disarm_substep();
    return ok;
}
/* --- GFX ------------------------------------------------------------------------------ */
bool td_gfx(TdLog &l)
{
    const IPBaseTable *ip = td_ip(l.env);
    if (!ip || !ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1)) {
        td_line(l, "teardown: gfx fail (GC base unresolved)");
        return false;
    }
    const uint32_t gc0 = ip->getBase(IPBlock::GC, 0);
    const uint32_t gc1 = ip->getBase(IPBlock::GC, 1);
    const uint32_t me_cntl = gc1 + amdgpu::kGfx11CpMeCntl;
    const uint32_t me_stat = gc0 + amdgpu::kGfx11CpStat;
    const uint32_t mec_cntl = gc1 + amdgpu::kGfx11CpMecCntl;

    /* Teardown-only rows registered by the gfx entry (tag "reg:gfx-teardown"). */
    if (!td_ensure_teardown_reg(me_cntl, WClass::State, WSubstep::Gfx, "reg:gfx-teardown") ||
        !td_ensure_teardown_reg(mec_cntl, WClass::State, WSubstep::Gfx, "reg:gfx-teardown")) {
        td_line(l, "teardown: gfx skip (teardown rows not registered)");
        return true;
    }

    r3_wg_arm_substep(WSubstep::Gfx);
    bool ok = true;

    /* T-E.6: halt ME and PFP in one RMW (CE_HALT preserved), then wait for CP_STAT to
     * drain. The wait is bounded by T_E_POLL_MAX; a timeout is reported, not fatal. */
    uint32_t me_before = 0;
    if (!td_read(l.env, me_cntl, &me_before)) {
        td_line(l, "teardown: gfx CP_ME_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t want =
            me_before | amdgpu::kGfx11CpMeCntlMeHalt | amdgpu::kGfx11CpMeCntlPfpHalt;
        uint32_t after = 0;
        if (!td_write(l, me_cntl, want, "teardown:gfx", "gfx-me-halt", &after)) {
            ok = false;
        } else if ((after & (amdgpu::kGfx11CpMeCntlMeHalt | amdgpu::kGfx11CpMeCntlPfpHalt)) !=
                   (amdgpu::kGfx11CpMeCntlMeHalt | amdgpu::kGfx11CpMeCntlPfpHalt)) {
            td_line(l, "teardown: gfx CP_ME_CNTL readback=0x%08x (ME/PFP halt missing)", after);
            ok = false;
        } else {
            td_line(l, "teardown: gfx CP_ME_CNTL 0x%08x -> 0x%08x (ME/PFP halted)", me_before,
                    after);
        }
    }

    uint32_t stat = 0;
    const int p = r3_io_poll(l.env->io, me_stat, 0xFFFFFFFFu, 0u, kHqdPolls, kHqdPollUs, &stat);
    if (p == 1) {
        td_line(l, "teardown: gfx CP_STAT idle");
    } else if (p == 0) {
        td_line(l, "teardown: gfx CP_STAT timeout=0x%08x (continuing)", stat);
        ok = false;
    } else {
        td_line(l, "teardown: gfx CP_STAT unreadable");
        ok = false;
    }

    uint32_t mec_before = 0;
    if (!td_read(l.env, mec_cntl, &mec_before)) {
        td_line(l, "teardown: gfx CP_MEC_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t want =
            mec_before | amdgpu::kGfx11CpMecCntlMe1Halt | amdgpu::kGfx11CpMecCntlMe2Halt;
        uint32_t after = 0;
        if (!td_write(l, mec_cntl, want, "teardown:gfx", "gfx-mec-halt", &after)) {
            ok = false;
        } else if ((after & (amdgpu::kGfx11CpMecCntlMe1Halt | amdgpu::kGfx11CpMecCntlMe2Halt)) !=
                   (amdgpu::kGfx11CpMecCntlMe1Halt | amdgpu::kGfx11CpMecCntlMe2Halt)) {
            td_line(l, "teardown: gfx CP_MEC_CNTL readback=0x%08x (ME1/ME2 halt missing)",
                    after);
            ok = false;
        } else {
            td_line(l, "teardown: gfx CP_MEC_CNTL 0x%08x -> 0x%08x (ME1/ME2 halted)",
                    mec_before, after);
        }
    }
    r3_io_udelay(l.env->io, 50u);

    r3_wg_disarm_substep();
    return ok;
}


} /* namespace */

const char *r3_teardown_ip_name(R3Ip ip)
{
    switch (ip) {
    case R3Ip::Psp: return "psp";
    case R3Ip::Smu: return "smu";
    case R3Ip::Gfx: return "gfx";
    case R3Ip::Sdma: return "sdma";
    case R3Ip::Mes: return "mes";
    }
    return "?";
}

R3TeardownReport r3_teardown_run(const R3TeardownEnv *env)
{
    R3TeardownReport rep;
    memset(&rep, 0, sizeof rep);

    TdLog l;
    memset(&l, 0, sizeof l);
    l.env = env;

    const R3Rt *rt = (env && env->sub) ? env->sub->rt : nullptr;
    if (!env || !env->io || !rt) {
        /* Still record the decision: an unreadable environment is a quiet skip. */
        td_line(l, "teardown: skip (no runtime/io)");
        td_publish(l);
        return rep;
    }

    uint32_t failed_bit = 0u;
    if (env->failed && env->failed_ip_valid &&
        static_cast<uint32_t>(env->failed_ip) < static_cast<uint32_t>(R3_IP_COUNT)) {
        failed_bit = R3_IP_BIT(env->failed_ip);
    }
    const uint32_t started = rt->ready_bits | failed_bit;
    rep.started = started;

    /* T4: a failing PSP stops the ladder with no inverse sequence. Snapshot is the
     * failure dump (already taken); teardown performs nothing and says so. */
    if (env->failed && env->failed_ip_valid && env->failed_ip == R3Ip::Psp) {
        rep.psp_failed = true;
        td_line(l, "teardown: none (psp failed; stop + snapshot; psp left alive)");
        td_publish(l);
        return rep;
    }

    /* T1: only a non-PSP started IP is teardown-worthy. PSP-only/no-IP is a no-op. */
    if ((started & ~R3_IP_BIT(R3Ip::Psp)) == 0u) {
        td_line(l, "teardown: none (no non-PSP IP started; psp left alive)");
        td_publish(l);
        return rep;
    }

    rep.triggered = true;
    td_line(l, "teardown: begin order=mes,sdma,gfx,smu,ih psp=alive reset=none");

    for (uint32_t i = 0; i < 4u; i++) {
        const R3Ip ip = kOrder[i];
        const uint32_t bit = R3_IP_BIT(ip);
        if ((started & bit) == 0u) {
            rep.skipped |= bit;
            td_line(l, "teardown: %s skip (not started)", r3_teardown_ip_name(ip));
            continue;
        }

        switch (ip) {
        case R3Ip::Mes: {
            const uint32_t w0 = l.writes;
            const bool ok = td_mes(l);
            if (l.writes > w0)
                rep.acted |= bit;
            else
                rep.none |= bit;
            if (!ok)
                rep.write_fail |= bit;
            break;
        }
        case R3Ip::Sdma: {
            const uint32_t w0 = l.writes;
            const bool ok = td_sdma(l);
            if (l.writes > w0)
                rep.acted |= bit;
            else
                rep.none |= bit;
            if (!ok)
                rep.write_fail |= bit;
            break;
        }
        case R3Ip::Gfx: {
            const uint32_t w0 = l.writes;
            const bool ok = td_gfx(l);
            if (l.writes > w0)
                rep.acted |= bit;
            else
                rep.none |= bit;
            if (!ok)
                rep.write_fail |= bit;
            break;
        }
        case R3Ip::Smu:
            rep.none |= bit;
            td_line(l, "teardown: smu none (no inverse in minimal message set)");
            break;
        case R3Ip::Psp:
            break; /* never reached: kOrder excludes PSP */
        }
    }

    /* T-E.7: IH is the last block (after GFX; SMU has no inverse), so no failure line is
     * lost before the interrupt path goes down. It is enabled early in power-up, hence
     * not gated on `started`; INV-D13 still requires its three rows to have been written. */
    {
        /* R3Ip has no IH member: td_ih logs its own outcome and its writes feed
         * rep.writes; a failed IH action is counted in ih_fail (never fatal). */
        const int ih = td_ih(l);
        if (ih < 0)
            rep.ih_fail = 1u;
    }

    rep.skipped |= R3_IP_BIT(R3Ip::Psp); /* PSP is deliberately left alive */
    rep.writes = l.writes;
    td_line(l, "teardown: done writes=%u acted=0x%x none=0x%x failed=0x%x ih_fail=%u",
            rep.writes, rep.acted, rep.none, rep.write_fail, rep.ih_fail);
    td_publish(l);
    return rep;
}
