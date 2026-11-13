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
#include "rdna3_ip_entries.h"
#include "rdna3_writeguard.h"

namespace {

using amdgpu::IPBaseTable;
using amdgpu::IPBlock;

/* The reverse power-up order (§9: mes -> sdma -> gfx -> smu). */
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

/* D-09: the used-register registry only REUSES rows the power-up substep already
 * registered. It never adds new rows (teardown must not widen the whitelist beyond
 * what power-up wrote). When a row is absent (the substep did not write that register),
 * the caller skips the action and logs "no reversible object". */
bool td_ensure_reg(uint32_t addr, WClass cls, WSubstep owner, const char *tag)
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

/* --- MES ------------------------------------------------------------------------------ */
bool td_mes_queue(TdLog &l, uint32_t grbm, uint32_t active, uint32_t pipe, const char *name)
{
    const char *ctx = "teardown:mes";
    if (!td_write(l, grbm, grbm_select(3u, pipe), ctx, "mes-grbm-select", nullptr))
        return false;

    uint32_t before = 0;
    if (!td_read(l.env, active, &before)) {
        td_line(l, "teardown: mes %s hqd-active unreadable", name);
        return false;
    }
    uint32_t after = 0;
    if (!td_write(l, active, 0u, ctx, "mes-hqd-active", &after))
        return false;

    /* Linux waits for CP_HQD_ACTIVE.ACTIVE to clear after deactivate; bounded here.
     * Without a delay callback one readback is the whole boundary. */
    int p = 1;
    if ((after & 1u) != 0u && l.env->io->udelay)
        p = r3_io_poll(l.env->io, active, 1u, 0u, kHqdPolls, kHqdPollUs, &after);
    if (p != 1) {
        td_line(l, "teardown: mes %s hqd-active 0x%08x -> 0x%08x (not clear)",
                name, before, after);
        return false;
    }
    td_line(l, "teardown: mes %s hqd-active 0x%08x -> 0x%08x", name, before, after);
    return true;
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

    if (!td_ensure_reg(grbm, WClass::State, WSubstep::Mes, "reg:teardown-mes") ||
        !td_ensure_reg(active, WClass::State, WSubstep::Mes, "reg:teardown-mes") ||
        !td_ensure_reg(cntl, WClass::State, WSubstep::Mes, "reg:teardown-mes")) {
        td_line(l, "teardown: mes skip (no reversible object; power-up rows absent)");
        return true;
    }

    r3_wg_arm_substep(WSubstep::Mes);
    bool ok = true;

    /* Reverse of seq_sch then seq_kiq (SCH = pipe0, KIQ = pipe1). */
    ok &= td_mes_queue(l, grbm, active, 0u, "sch");
    ok &= td_mes_queue(l, grbm, active, 1u, "kiq");
    ok &= td_write(l, grbm, grbm_select(0u, 0u), "teardown:mes", "mes-grbm-deselect",
                   nullptr);

    uint32_t before = 0;
    if (!td_read(l.env, cntl, &before)) {
        td_line(l, "teardown: mes CP_MES_CNTL unreadable");
        ok = false;
    } else {
        const uint32_t want =
            (before & ~(amdgpu::kMesCntlPipe0Active | amdgpu::kMesCntlPipe1Active)) |
            amdgpu::kMesCntlPipe0Reset | amdgpu::kMesCntlPipe1Reset;
        uint32_t after = 0;
        if (!td_write(l, cntl, want, "teardown:mes", "mes-cntl-reset", &after)) {
            ok = false;
        } else if ((after & (amdgpu::kMesCntlPipe0Reset | amdgpu::kMesCntlPipe1Reset)) !=
                   (amdgpu::kMesCntlPipe0Reset | amdgpu::kMesCntlPipe1Reset)) {
            td_line(l, "teardown: mes CP_MES_CNTL readback=0x%08x (reset bits missing)",
                    after);
            ok = false;
        } else {
            td_line(l, "teardown: mes CP_MES_CNTL 0x%08x -> 0x%08x (pipes reset)",
                    before, after);
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

    const uint32_t failed_bit =
        (env->failed) ? R3_IP_BIT(env->failed_ip) : 0u;
    const uint32_t started = rt->ready_bits | failed_bit;
    rep.started = started;

    /* T4: a failing PSP stops the ladder with no inverse sequence. Snapshot is the
     * failure dump (already taken); teardown performs nothing and says so. */
    if (env->failed && env->failed_ip == R3Ip::Psp) {
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
    td_line(l, "teardown: begin order=mes,sdma,gfx,smu psp=alive reset=none");

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
        case R3Ip::Gfx:
            rep.none |= bit;
            td_line(l, "teardown: gfx none");
            break;
        case R3Ip::Smu:
            rep.none |= bit;
            td_line(l, "teardown: smu none (no inverse in minimal message set)");
            break;
        case R3Ip::Psp:
            break; /* never reached: kOrder excludes PSP */
        }
    }

    rep.skipped |= R3_IP_BIT(R3Ip::Psp); /* PSP is deliberately left alive */
    rep.writes = l.writes;
    td_line(l, "teardown: done writes=%u acted=0x%x none=0x%x failed=0x%x",
            rep.writes, rep.acted, rep.none, rep.write_fail);
    td_publish(l);
    return rep;
}
