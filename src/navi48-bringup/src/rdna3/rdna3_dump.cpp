/*
 * rdna3_dump.cpp - the RDNA3 stage-2 failure dump (tech plan §8.4, requirement F4).
 *
 * Hard boundaries (the reason this file may never become a new hang source):
 *   - reads only: no MMIO write, no doorbell, no mailbox submission, no BAR0 write;
 *   - no polling and no delay: every group is a fixed, small list of single-dword reads;
 *   - each group starts with a readability probe. r3_io_read32 yields 0xFFFFFFFF on a bus
 *     failure, so a probe of 0xFFFFFFFF abandons the group and records "dump:skip:<obj>";
 *     a later read failure aborts the rest of that group ("dump:abort:<obj>");
 *   - the shared SMN pair is NOT touched here: the psp/smu groups use only MMIO
 *     (C2PMSG/MP1 mailbox) and the L2 fault registers, so the dump never contends for
 *     PCIE_INDEX2/DATA2 and cannot deadlock behind a substep's SMN transaction;
 *   - IH draining is absent by construction (stage 2 never initialises IH, C0.6). No
 *     OSSSYS register is read and no line mentions IH.
 */
#include "rdna3_dump.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>   /* snprintf/vsnprintf/strstr, no libc under -nostdinc */
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "rdna3_ip_entries.h"
#include "golden_values.h"
#include "rdna3_layout.h"

namespace {

using amdgpu::IPBaseTable;
using amdgpu::IPBlock;

/* MMHUB L2 fault registers (mmhub_3_0_1_offset.h:1058-1063, BASE_IDX 1). */
constexpr int      kMmhubFaultBase  = 1;
constexpr uint32_t kMmvmFaultStatus = 0x070cu;
constexpr uint32_t kMmvmFaultLo     = 0x070du;
constexpr uint32_t kMmvmFaultHi     = 0x070eu;

/* SDMA0 registers beyond the substep's list (gc_11_0_0_offset.h:86-90, BASE_IDX 0). */
constexpr uint32_t kSdma0Status2Reg = 0x0038u;
constexpr uint32_t kSdma0EdcCounter = 0x0036u;

/* Fence snapshot: the first 8 dwords of the PSP fence grant (32 B of a 4 KiB buffer). */
constexpr uint32_t kFenceDwords = 8u;

/* A readability probe that returns this is "unreadable", never a value. */
constexpr uint32_t kUnreadable = 0xFFFFFFFFu;

struct DumpWriter {
    const R3DumpEnv *env;
    char *buf;
    size_t cap;
    size_t used;
    bool full;
};

bool dump_read32(const R3DumpEnv *env, uint32_t dword, uint32_t *value)
{
    if (!env || !env->io || !env->io->mmio || !value)
        return false;
    bool ok = false;
    const uint32_t v = r3_io_read32(env->io, dword, &ok);
    if (!ok || v == kUnreadable)
        return false;
    *value = v;
    return true;
}

bool dump_read_bus32(const R3DumpEnv *env, uint64_t bar0_off, uint32_t *value)
{
    if (!env || !env->io || !env->io->bus || !value || (bar0_off & 3ull) != 0ull)
        return false;
    uint64_t v = 0;
    if (!env->io->bus(env->io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, 1u))
        return false;
    if (static_cast<uint32_t>(v) == kUnreadable)
        return false;
    *value = static_cast<uint32_t>(v);
    return true;
}

/* Append one line to the group buffer and the evidence arena. A full buffer marks the
 * group abort; the partial text already stored is still published. */
void dw_line(DumpWriter &w, const char *fmt, ...)
{
    if (!w.buf || w.full || w.used + 1u >= w.cap)
        return;

    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1u] = '\0';

    const int n = snprintf(w.buf + w.used, w.cap - w.used, "%s\n", line);
    if (n <= 0 || static_cast<size_t>(n) >= w.cap - w.used) {
        w.full = true;
        w.buf[w.used] = '\0';
    } else {
        w.used += static_cast<size_t>(n);
    }
    if (w.env && w.env->sub && w.env->sub->ev)
        r3_ev_add(w.env->sub->ev, "dump", line);
}

void dw_skip(DumpWriter &w, const char *obj)
{
    char line[96];
    snprintf(line, sizeof line, "dump:skip:%s", obj);
    dw_line(w, "%s", line);
}

void dw_abort(DumpWriter &w, const char *obj)
{
    char line[96];
    snprintf(line, sizeof line, "dump:abort:%s", obj);
    dw_line(w, "%s", line);
}

const IPBaseTable *dump_ip(const R3DumpEnv *env)
{
    return (env && env->sub) ? static_cast<const IPBaseTable *>(env->sub->ip) : nullptr;
}

/* --- psp: MP0 C2PMSG group ------------------------------------------------------------ */
void dump_psp_c2pmsg(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip) {
        dw_skip(w, "psp-c2pmsg");
        return;
    }

    /* psp_v13_0.h: 33 IFWI/PSP boot, 58 SOS fw version, 64 ring create, 81 SOS alive.
     * C2PMSG_67 is write-only ("set, never read" in psp_v13_0.h) and is deliberately
     * absent; 69/70/71 are the ring address/size the substep programmed. All are
     * mp_13_0_4 _BASE_IDX 1 and resolve through the golden table (A-01). */
    static const r3gold::GoldenReg *const regs[7] = {
        &r3gold::kMp0SmnC2pmsg33, &r3gold::kMp0SmnC2pmsg58, &r3gold::kMp0SmnC2pmsg64,
        &r3gold::kMp0SmnC2pmsg69, &r3gold::kMp0SmnC2pmsg70, &r3gold::kMp0SmnC2pmsg71,
        &r3gold::kMp0SmnC2pmsg81,
    };
    const char *labels[7] = {"c2pmsg33", "c2pmsg58", "c2pmsg64", "c2pmsg69",
                             "c2pmsg70", "c2pmsg71", "c2pmsg81"};
    uint32_t addr[7] = {0};
    uint32_t v[7];

    for (uint32_t i = 0; i < 7u; i++) {
        if (!ip->resolve(regs[i]->ref(), &addr[i])) {
            if (i == 0u) {
                dw_skip(w, "psp-c2pmsg");
            } else {
                for (uint32_t j = 0; j < i; j++)
                    dw_line(w, "dump: psp %s=0x%08x", labels[j], v[j]);
                dw_abort(w, "psp-c2pmsg");
            }
            return;
        }
    }

    /* Readability probe: the first register decides whether the block answers at all. */
    if (!dump_read32(env, addr[0], &v[0])) {
        dw_skip(w, "psp-c2pmsg");
        return;
    }
    for (uint32_t i = 1; i < 7u; i++) {
        if (!dump_read32(env, addr[i], &v[i])) {
            for (uint32_t j = 0; j < i; j++)
                dw_line(w, "dump: psp %s=0x%08x", labels[j], v[j]);
            dw_abort(w, "psp-c2pmsg");
            return;
        }
    }
    dw_line(w, "dump: psp %s=0x%08x %s=0x%08x %s=0x%08x %s=0x%08x %s=0x%08x "
               "%s=0x%08x %s=0x%08x",
            labels[0], v[0], labels[1], v[1], labels[2], v[2], labels[3], v[3],
            labels[4], v[4], labels[5], v[5], labels[6], v[6]);
}

/* --- psp: fence buffer ---------------------------------------------------------------- */
void dump_psp_fence(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    if (!env || !env->sub || !env->sub->rt) {
        dw_skip(w, "psp-fence");
        return;
    }
    const R3Grant *g = r3_layout_grant(&env->sub->rt->layout, "psp_fence");
    if (!g) {
        dw_skip(w, "psp-fence");
        return;
    }

    uint32_t v[kFenceDwords] = {0};
    uint32_t n = 0;
    for (; n < kFenceDwords; n++) {
        if (!dump_read_bus32(env, g->start + static_cast<uint64_t>(n) * 4ull, &v[n]))
            break;
    }
    if (n == 0u) {
        dw_skip(w, "psp-fence");
        return;
    }
    /* Only dwords actually read are printed; a short read keeps the partial values and
     * records the abort (F4: a read failure stops the rest of this group). */
    if (n >= 4u) {
        dw_line(w, "dump: psp fence[0..3]=0x%08x,0x%08x,0x%08x,0x%08x",
                v[0], v[1], v[2], v[3]);
    } else {
        for (uint32_t i = 0; i < n; i++)
            dw_line(w, "dump: psp fence[%u]=0x%08x", i, v[i]);
    }
    if (n >= 8u)
        dw_line(w, "dump: psp fence[4..7]=0x%08x,0x%08x,0x%08x,0x%08x",
                v[4], v[5], v[6], v[7]);
    else
        dw_abort(w, "psp-fence");
}

/* --- psp/smu: MP1 mailbox ------------------------------------------------------------- */
void dump_smu_mailbox(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip || !ip->isResolved(IPBlock::MP1, amdgpu::kSmuMp1BaseIdx)) {
        dw_skip(w, "smu-mailbox");
        return;
    }
    const uint32_t base = ip->getBase(IPBlock::MP1, amdgpu::kSmuMp1BaseIdx);

    uint32_t r66 = 0, r82 = 0, r90 = 0;
    if (!dump_read32(env, base + amdgpu::kSmuC2pmsg66, &r66)) {
        dw_skip(w, "smu-mailbox");
        return;
    }
    if (!dump_read32(env, base + amdgpu::kSmuC2pmsg82, &r82) ||
        !dump_read32(env, base + amdgpu::kSmuC2pmsg90, &r90)) {
        dw_line(w, "dump: smu c2pmsg66=0x%08x", r66);
        dw_abort(w, "smu-mailbox");
        return;
    }
    dw_line(w, "dump: smu c2pmsg66=0x%08x c2pmsg82=0x%08x c2pmsg90=0x%08x",
            r66, r82, r90);
    /* MP1_FIRMWARE_FLAGS needs the shared SMN pair; the dump never touches it (F4: the
     * dump must not contend for PCIE_INDEX2/DATA2 or wait on a bus transaction). */
    dw_line(w, "dump: smu firmware_flags=not-read (SMN excluded from the dump)");
}

/* --- gfx state ------------------------------------------------------------------------ */
void dump_gfx_state(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip || !ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1)) {
        dw_skip(w, "gfx-state");
        return;
    }
    const uint32_t gc0 = ip->getBase(IPBlock::GC, 0);
    const uint32_t gc1 = ip->getBase(IPBlock::GC, 1);

    /* gfx_v11_0.h offsets; all reads, no CP_ME_CNTL write (gfx is a read-only substep). */
    uint32_t imu = 0, reset = 0, rlc = 0, gpm = 0, me = 0, grbm = 0, grbm2 = 0, cp = 0;
    if (!dump_read32(env, gc1 + amdgpu::kGfx11ImuScratch0, &imu)) {
        dw_skip(w, "gfx-state");
        return;
    }
    if (!dump_read32(env, gc1 + amdgpu::kGfx11ImuGfxResetCtrl, &reset) ||
        !dump_read32(env, gc1 + amdgpu::kGfx11RlcStat, &rlc) ||
        !dump_read32(env, gc1 + amdgpu::kGfx11RlcGpmStat, &gpm) ||
        !dump_read32(env, gc1 + amdgpu::kGfx11CpMeCntl, &me) ||
        !dump_read32(env, gc0 + amdgpu::kGfx11GrbmStatus, &grbm) ||
        !dump_read32(env, gc0 + amdgpu::kGfx11GrbmStatus2, &grbm2) ||
        !dump_read32(env, gc0 + amdgpu::kGfx11CpStat, &cp)) {
        dw_line(w, "dump: gfx imu_scratch0=0x%08x imu_reset=0x%08x", imu, reset);
        dw_abort(w, "gfx-state");
        return;
    }
    dw_line(w, "dump: gfx imu_scratch0=0x%08x imu_reset=0x%08x rlc_stat=0x%08x "
               "rlc_gpm_stat=0x%08x",
            imu, reset, rlc, gpm);
    dw_line(w, "dump: gfx cp_me_cntl=0x%08x grbm_status=0x%08x grbm_status2=0x%08x "
               "cp_stat=0x%08x",
            me, grbm, grbm2, cp);
}

/* --- sdma state ----------------------------------------------------------------------- */
void dump_sdma_state(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip || !ip->isResolved(IPBlock::SDMA0, 0) ||
        !ip->isResolved(IPBlock::SDMA0, 1)) {
        dw_skip(w, "sdma-state");
        return;
    }
    const uint32_t sdma0 = ip->getBase(IPBlock::SDMA0, 0);
    const uint32_t sdma1 = ip->getBase(IPBlock::SDMA0, 1);

    /* sdma_v6_0.h: status/EDC are SDMA0 BASE_IDX 0; F32_CNTL is BASE_IDX 1. */
    uint32_t status = 0, status2 = 0, edc = 0, rb = 0, rptr = 0, wptr = 0, f32 = 0;
    if (!dump_read32(env, sdma0 + amdgpu::kRegSdma0StatusReg, &status)) {
        dw_skip(w, "sdma-state");
        return;
    }
    if (!dump_read32(env, sdma0 + kSdma0Status2Reg, &status2) ||
        !dump_read32(env, sdma0 + kSdma0EdcCounter, &edc) ||
        !dump_read32(env, sdma0 + amdgpu::kRegSdma0Queue0RbCntl, &rb) ||
        !dump_read32(env, sdma0 + amdgpu::kRegSdma0Queue0RbRptr, &rptr) ||
        !dump_read32(env, sdma0 + amdgpu::kRegSdma0Queue0RbWptr, &wptr) ||
        !dump_read32(env, sdma1 + amdgpu::kRegSdma0F32Cntl, &f32)) {
        dw_line(w, "dump: sdma status=0x%08x status2=0x%08x", status, status2);
        dw_abort(w, "sdma-state");
        return;
    }
    dw_line(w, "dump: sdma status=0x%08x status2=0x%08x edc=0x%08x", status, status2, edc);
    dw_line(w, "dump: sdma rb_cntl=0x%08x rptr=0x%08x wptr=0x%08x f32_cntl=0x%08x",
            rb, rptr, wptr, f32);
}

/* --- mes state ------------------------------------------------------------------------ */
void dump_mes_state(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip || !ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1)) {
        dw_skip(w, "mes-state");
        return;
    }
    const uint32_t gc0 = ip->getBase(IPBlock::GC, 0);
    const uint32_t gc1 = ip->getBase(IPBlock::GC, 1);

    /* The dump must not write GRBM_GFX_CNTL, so CP_MES_GP3_LO is read in the CURRENT
     * context only and labelled as such; the queue-selection registers are not touched. */
    uint32_t cntl = 0, gp3 = 0, hqd = 0, gfx_hqd = 0, sched = 0, grbm = 0, cp = 0;
    if (!dump_read32(env, gc1 + amdgpu::kRegMesCntl, &cntl)) {
        dw_skip(w, "mes-state");
        return;
    }
    if (!dump_read32(env, gc1 + amdgpu::kRegMesGp3Lo, &gp3) ||
        !dump_read32(env, gc0 + amdgpu::kRegCpHqdActive, &hqd) ||
        !dump_read32(env, gc0 + amdgpu::kRegCpGfxHqdActive, &gfx_hqd) ||
        !dump_read32(env, gc1 + amdgpu::kRegRlcCpSchedulers, &sched) ||
        !dump_read32(env, gc0 + amdgpu::kGfx11GrbmStatus, &grbm) ||
        !dump_read32(env, gc0 + amdgpu::kGfx11CpStat, &cp)) {
        dw_line(w, "dump: mes cp_mes_cntl=0x%08x gp3_lo(current)=0x%08x", cntl, gp3);
        dw_abort(w, "mes-state");
        return;
    }
    dw_line(w, "dump: mes cp_mes_cntl=0x%08x cp_mes_gp3_lo(current)=0x%08x "
               "cp_hqd_active(current)=0x%08x cp_gfx_hqd_active=0x%08x",
            cntl, gp3, hqd, gfx_hqd);
    dw_line(w, "dump: mes rlc_cp_schedulers=0x%08x grbm_status=0x%08x cp_stat=0x%08x",
            sched, grbm, cp);
}

/* --- GC / MMHUB L2 fault -------------------------------------------------------------- */
void dump_gcvm(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip) {
        dw_skip(w, "gcvm");
        return;
    }
    uint32_t a_status = 0, a_lo = 0, a_hi = 0;
    if (!ip->resolve(r3gold::kGcvmL2ProtectionFaultStatus.ref(), &a_status) ||
        !ip->resolve(r3gold::kGcvmL2ProtectionFaultAddrLo32.ref(), &a_lo) ||
        !ip->resolve(r3gold::kGcvmL2ProtectionFaultAddrHi32.ref(), &a_hi)) {
        dw_skip(w, "gcvm");
        return;
    }

    uint32_t st = 0, lo = 0, hi = 0;
    if (!dump_read32(env, a_status, &st)) {
        dw_skip(w, "gcvm");
        return;
    }
    if (!dump_read32(env, a_lo, &lo) || !dump_read32(env, a_hi, &hi)) {
        dw_line(w, "dump: gcvm status=0x%08x", st);
        dw_abort(w, "gcvm");
        return;
    }
    dw_line(w, "dump: gcvm status=0x%08x addr=0x%08x%08x", st, hi, lo);
}

void dump_mmvm(DumpWriter &w)
{
    const R3DumpEnv *env = w.env;
    const IPBaseTable *ip = dump_ip(env);
    if (!ip || !ip->isResolved(IPBlock::MMHUB, kMmhubFaultBase)) {
        dw_skip(w, "mmvm");
        return;
    }
    const uint32_t base = ip->getBase(IPBlock::MMHUB, kMmhubFaultBase);

    uint32_t st = 0, lo = 0, hi = 0;

    if (!dump_read32(env, base + kMmvmFaultStatus, &st)) {
        dw_skip(w, "mmvm");
        return;
    }
    if (!dump_read32(env, base + kMmvmFaultLo, &lo) ||
        !dump_read32(env, base + kMmvmFaultHi, &hi)) {
        dw_line(w, "dump: mmvm status=0x%08x", st);
        dw_abort(w, "mmvm");
        return;
    }
    dw_line(w, "dump: mmvm status=0x%08x addr=0x%08x%08x", st, hi, lo);
}

typedef void (*DumpGroupFn)(DumpWriter &);

void run_group(const R3DumpEnv *env, const char *name, DumpGroupFn fn)
{
    char buf[768];
    buf[0] = '\0';
    DumpWriter w{env, buf, sizeof buf, 0u, false};
    fn(w);

    if (w.used > 0u && env && env->sub && env->sub->sink &&
        env->sub->sink->set_property) {
        char key[48];
        snprintf(key, sizeof key, "N48R3EVID-dump-%s", name);
        env->sub->sink->set_property(env->sub->sink->user, key, buf, w.used);
    }
}

/* Highest ready IP when there is no failed substep (a stop with a live ladder). */
bool highest_ready(const R3Rt *rt, R3Ip *out)
{
    if (!rt || !out)
        return false;
    for (int i = static_cast<int>(R3Ip::Mes); i >= static_cast<int>(R3Ip::Psp); i--) {
        if ((rt->ready_bits & (1u << static_cast<unsigned>(i))) != 0u) {
            *out = static_cast<R3Ip>(i);
            return true;
        }
    }
    return false;
}

void summary_prop(const R3DumpEnv *env, R3Ip ip, bool has_ip)
{
    if (!env || !env->sub || !env->sub->sink || !env->sub->sink->set_property)
        return;
    char text[160];
    const int n = snprintf(text, sizeof text,
                           "failed=%s availability=%s ih=none writes=none\n",
                           has_ip ? r3_dump_ip_name(ip) : "none",
                           has_ip ? "per-substep" : "none");
    if (n > 0)
        env->sub->sink->set_property(env->sub->sink->user, "N48R3EVID-dump-summary",
                                     text, static_cast<size_t>(n));
    if (env->sub->ev)
        r3_ev_add(env->sub->ev, "dump",
                  "dump: summary (interrupt-ring snapshot omitted; reads only)");
}

} /* namespace */

const char *r3_dump_ip_name(R3Ip ip)
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

void r3_dump_failure_state(const R3DumpEnv *env)
{
    if (!env || !env->sub)
        return;

    R3Ip ip = env->failed_ip;
    bool has_ip = env->has_failed_ip;
    if (!has_ip && env->sub->rt)
        has_ip = highest_ready(env->sub->rt, &ip);

    /* Serialise the snapshot against every other MMIO user. The lock is NOT a substep-level
     * contract (only SMU holds it for its whole entry; PSP holds it inside post(); GFX/
     * SDMA/MES never take it), so the caller must invoke the dump after the failing substep
     * returned and outside any lock. One short acquisition; no delay/poll follows. */
    const bool locked = env->io && env->io->lock && env->io->unlock;
    if (locked)
        env->io->lock(env->io->ctx);

    if (!has_ip) {
        if (env->sub->ev)
            r3_ev_add(env->sub->ev, "dump", "dump: no started IP (nothing to snapshot)");
    } else {
        switch (ip) {
        case R3Ip::Psp:
            run_group(env, "psp", dump_psp_c2pmsg);
            run_group(env, "psp-fence", dump_psp_fence);
            run_group(env, "gcvm", dump_gcvm);
            run_group(env, "mmvm", dump_mmvm);
            break;
        case R3Ip::Smu:
            run_group(env, "smu", dump_smu_mailbox);
            run_group(env, "gcvm", dump_gcvm);
            break;
        case R3Ip::Gfx:
            run_group(env, "gfx", dump_gfx_state);
            run_group(env, "gcvm", dump_gcvm);
            run_group(env, "mmvm", dump_mmvm);
            break;
        case R3Ip::Sdma:
            run_group(env, "sdma", dump_sdma_state);
            run_group(env, "gcvm", dump_gcvm);
            run_group(env, "mmvm", dump_mmvm);
            break;
        case R3Ip::Mes:
            run_group(env, "mes", dump_mes_state);
            run_group(env, "gcvm", dump_gcvm);
            run_group(env, "mmvm", dump_mmvm);
            break;
        }
    }

    summary_prop(env, ip, has_ip);

    if (locked)
        env->io->unlock(env->io->ctx);
}
