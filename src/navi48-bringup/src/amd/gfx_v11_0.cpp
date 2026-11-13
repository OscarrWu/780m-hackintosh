//
//  gfx_v11_0.cpp - the GFX 11.0.1 read-only readiness substep (see gfx_v11_0.h for the
//  contract, the license header and the Linux provenance of every offset).
//
//  Section map (tech plan §3.3; requirement C3):
//    pre()   PSP ready + SMU ready (§8.1), a complete R3Io surface, GC BASE_IDX 0/1.
//    seq()   empty by design: this substep issues no register/protocol write.
//    chk()   S1 IMU version/status, S2 Linux RLC autoload readiness
//            (RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE + CP_STAT==0) with
//            RLC_STAT/RLC_GPM_STAT/GRBM_STATUS recorded as non-gating evidence,
//            S3 CP_ME_CNTL readback.
//    ev()    the S1-S3 readings into env->ev, the GFX ready bit and N48R3EVID-gfx-done.
//
//  Write count is 0 by construction: this file contains no r3_io_write32 / reg_write call.
//  CP unhalt belongs to the MES substep (§3.5); issuing it here would widen the R2 risk
//  surface without adding a GFX acceptance fact.
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
#include "../rdna3/rdna3_writeguard.h"

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

    evf(g.env->ev, kTagPre, "P0 gc0=0x%08x gc1=0x%08x ready=psp+smu (no write)",
        g.gc0, g.gc1);
    return r;
}

// ---------------------------------------------------------------------------------------
// SEQ: intentionally empty (the substep is read-only)
// ---------------------------------------------------------------------------------------
R3SubstepResult seq(Gfx &g)
{
    evf(g.env->ev, kTagSeq, "S0 no write by design (CP unhalt belongs to the MES substep)");
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

    // Read-only substep: arm the owner so any accidental write would be DENIED and counted
    // (the deny count must stay 0; the test asserts it).
    r3_wg_arm_substep(WSubstep::Gfx);
    r3_wg_expect_writes(nullptr, 0, nullptr, 0, WSubstep::Gfx);
    r3_wg_expect_declared_addrs(nullptr, 0);   /* GFX declares zero register rows */
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
            "rlc_gpm=0x%08x cp_me_cntl=0x%08x grbm=0x%08x grbm2=0x%08x (0 writes)",
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

} // namespace amdgpu
