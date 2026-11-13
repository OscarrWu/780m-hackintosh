//
//  smu_v13_0.cpp - the SMU 13.0.4 power-up substep (see smu_v13_0.h for the contract,
//  the license header and the Linux provenance of every offset).
//
//  Section map (tech plan §3.2; requirement C0.3/C0.5/C0.7/C2.1-C2.3):
//    pre()  P0 I/O + runtime, P1 MP1 segment 1 resolved, P2 PSP ready, P3 GC/SDMA bases,
//           P4 MP1_FIRMWARE_FLAGS bit0 (SMN, RO): bit0=0 warns+continues; a read
//           failure stops before any mailbox write (read-path fault = fail closed).
//    seq()  S2 mailbox config -> S3 0x3 -> S4 0x2 -> S5 0x16(1) -> S6 effect verdict with
//           the one D11 recheck -> S7 0x12 feature low/high (audit only).
//    chk()  C0.4 self-check: versions non-zero, effect verdict passed, D10 audit notes.
//    ev()   the §3.2 fields into env->ev.
//
//  Every mailbox write goes through reg_write() and is classified by the K3 whitelist
//  under WSubstep::Smu before it is issued. The SMN read and the whole mailbox sequence
//  run under R3Io::lock (the red line: no unlocked use of the shared PCIE_INDEX2/DATA2
//  pair). The SMN adapter owns the hard timeout; this file performs exactly one SMN read.
//
//  The effect criterion is the final gate, not the response code: a 0x16 whose mailbox
//  poll timed out but whose effect is established is an async-success. A 0xFFFFFFFF read
//  is a read-path/BAR fault and is never treated as a valid value (J-2).
//
//  Kernel build constraints: -nostdinc, no libc, no exceptions/RTTI, no allocation. Only
//  <stdint.h>/<stddef.h> + libkern (memset/vsnprintf) + the RDNA3 headers.
//
#include "smu_v13_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>   /* memset/vsnprintf, no libc under -nostdinc */
#else
#include <stdio.h>             /* host runner: same names from libc */
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "../rdna3/golden_values.h"

#include "../rdna3/rdna3_runtime.h"
#include "../rdna3/rdna3_writeguard.h"

namespace amdgpu {

namespace {

// Result names are static: R3SubstepResult::name outlives the substep (E1).
constexpr const char *kPreNoIo        = "pre:no-io";
constexpr const char *kPreNoEnv       = "pre:no-env";
constexpr const char *kPreMp1Missing  = "pre:mp1-unresolved";
constexpr const char *kPrePspNotReady = "pre:psp-not-ready";
constexpr const char *kPreGcMissing   = "pre:gc-base-unresolved";
constexpr const char *kPreFlagsUnread = "pre:fwflags-unreadable";

// C0.5/C2.3: version messages are audit-only. A writeguard DENY is a local
// integration error (fail-closed), not an SMU response, so it still fails.
constexpr const char *kSeqDriverIf   = "seq:driver-if";
constexpr const char *kSeqPmfwVersion = "seq:pmfw-version";
constexpr const char *kSeqEnableImu   = "seq:enable-gfx-imu";
constexpr const char *kSeqEffect      = "seq:effect-not-established";
constexpr const char *kSeqEffectFault = "seq:effect-read-fault";

constexpr const char *kChkEffect      = "chk:effect-not-established";

constexpr const char *kTagPre = "smu-pre";
constexpr const char *kTagSeq = "smu-seq";
constexpr const char *kTagChk = "smu-chk";

// The effect poll is bounded by both time and a sample cap: a broken clock must never
// turn the verdict into an unbounded loop (the red line is "no hang", not "trust now_us").
constexpr uint32_t kEffectMaxSamples =
    (kSmuEffectFastUs / kSmuEffectFastStepUs) + (kSmuEffectSlowUs / kSmuEffectSlowStepUs) + 8u;

static_assert(kSmuEffectFastStepUs >= kSmuEffectMinGapUs,
              "two consecutive effect samples must be >= 10 ms apart");

struct Smu {
    const R3Io *io;
    const R3SubstepEnv *env;

    uint32_t mp1_base;
    uint32_t msg_addr;
    uint32_t arg_addr;
    uint32_t resp_addr;
    uint32_t gc0;
    uint32_t gc1;
    uint32_t sdma0;

    bool first_msg;          // S3 is the first message (Linux SMU_FW_INIT: skip pre-poll)

    uint32_t driver_if;
    uint32_t pmfw;
    uint32_t feature_lo;
    uint32_t feature_hi;
    bool driver_if_ok;
    bool pmfw_ok;
    bool feature_lo_ok;
    bool feature_hi_ok;

    bool effect_pass;
    bool async_success;
    bool effect_fault;
    uint32_t gfx_imu_sends;
    uint32_t effect_samples;
    uint64_t effect_elapsed_us;
    uint32_t last_grbm_status;
    uint32_t last_rlc_stat;
    uint32_t last_grbm_status2;
    uint32_t last_gb_addr_config;
    uint32_t last_sdma0_status;
};

struct Tx {
    bool sent;           // the message id was actually written
    bool read_fault;     // a mailbox read failed, or returned the 0xFFFFFFFF sentinel
    uint32_t resp;       // post-poll response (0 = timeout)
    bool resp_ok;        // the post-poll read itself succeeded
    uint32_t ret;        // C2PMSG_82 after an OK response (when requested)
    bool ret_ok;
    bool denied;         // writeguard DENY (local config error, not an SMU response)
};

struct EffectSample {
    uint32_t grbm_status;
    uint32_t grbm_status2;
    uint32_t gb_addr_config;
    uint32_t rlc_stat;
    uint32_t sdma0_status;
    bool grbm_ok;
    bool grbm2_ok;
    bool gb_ok;
    bool rlc_ok;
    bool sdma_ok;
};

struct EffectResult {
    bool pass;
    bool read_fault;
    uint32_t samples;
    uint64_t elapsed_us;
    EffectSample last;
    uint32_t c2pmsg90_at_end;
    bool c2pmsg90_ok_at_end;
    bool phase_check;
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

// ---------------------------------------------------------------------------------------
// whitelisted mailbox access (BAR5 dword, ③ Protocol under WSubstep::Smu)
// ---------------------------------------------------------------------------------------
bool reg_write(Smu &p, uint32_t abs, uint32_t value, const char *ctx,
               bool *denied = nullptr)
{
    if (denied) *denied = false;
    if (r3_wg_classify(abs, 1u, WClass::Protocol, ctx) != WVerdict::TxnRead) {
        if (denied) *denied = true;
        return false;
    }
    return r3_io_write32(p.io, abs, value);
}

uint32_t reg_read(Smu &p, uint32_t abs, bool *ok)
{
    return r3_io_read32(p.io, abs, ok);
}

// The one SMN read (P4). smuio takes the byte address Linux passes to RREG32_PCIE; the
// substep holds R3Io::lock, the adapter owns the hard timeout.
uint32_t smn_read(Smu &p, uint32_t index, bool *ok)
{
    uint32_t v = 0xFFFFFFFFu;
    const bool good = p.io->smuio && p.io->smuio(p.io->ctx, index, &v, false);
    if (ok)
        *ok = good;
    return v;
}

bool valid_value(uint32_t v)
{
    return v != 0u && v != 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------------------
// PRE
// ---------------------------------------------------------------------------------------
R3SubstepResult pre(Smu &p)
{
    R3SubstepResult r = r3_substep_pass();
    if (!r3_io_complete(p.io)) {
        evf(p.env ? p.env->ev : nullptr, kTagPre,
            "FAIL: I/O surface incomplete (mmio/smuio/delay/lock)");
        return r3_substep_fail(kR3SecPre, 0, kPreNoIo);
    }
    if (!p.env || !p.env->rt) {
        evf(p.env ? p.env->ev : nullptr, kTagPre, "FAIL: no runtime block");
        return r3_substep_fail(kR3SecPre, 1, kPreNoEnv);
    }

    const IPBaseTable *ip = static_cast<const IPBaseTable *>(p.env->ip);
    if (!ip || !ip->isResolved(IPBlock::MP1, kSmuMp1BaseIdx)) {
        evf(p.env->ev, kTagPre, "FAIL: MP1 segment %d unresolved", kSmuMp1BaseIdx);
        return r3_substep_fail(kR3SecPre, 2, kPreMp1Missing);
    }
    p.mp1_base = ip->getBase(IPBlock::MP1, kSmuMp1BaseIdx);

    // Dependency gate (§3.2): EnableGfxImu assumes the PSP already delivered the SMU
    // firmware. A missing bit is a refusal, never "try the mailbox anyway".
    if ((p.env->rt->ready_bits & R3_IP_BIT(R3Ip::Psp)) == 0u) {
        evf(p.env->ev, kTagPre, "FAIL: PSP not ready (ready_bits=0x%08x)",
            p.env->rt->ready_bits);
        return r3_substep_fail(kR3SecPre, 3, kPrePspNotReady);
    }

    // The effect criterion needs the GC and SDMA bases; a missing segment would silently
    // read register 0 and misclassify "not powered" as "not established".
    if (!ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1) ||
        !ip->isResolved(IPBlock::SDMA0, 0)) {
        evf(p.env->ev, kTagPre, "FAIL: GC/SDMA base unresolved (GC0=%u GC1=%u SDMA0=%u)",
            ip->isResolved(IPBlock::GC, 0) ? 1u : 0u, ip->isResolved(IPBlock::GC, 1) ? 1u : 0u,
            ip->isResolved(IPBlock::SDMA0, 0) ? 1u : 0u);
        return r3_substep_fail(kR3SecPre, 4, kPreGcMissing);
    }
    p.gc0 = ip->getBase(IPBlock::GC, 0);
    p.gc1 = ip->getBase(IPBlock::GC, 1);
    p.sdma0 = ip->getBase(IPBlock::SDMA0, 0);

    // P4: MP1_FIRMWARE_FLAGS bit0 (smu_v13_0.c:237-259). Read-only; §3.2/§8.1:
    // bit0=0 is a status observation (warn and continue), not a verdict; the C0.5
    // effect criterion (GRBM_STATUS valid x2) is the final gate. A read failure
    // is a read-path fault (fail closed, §8.1).
    bool ok = false;
    const uint32_t flags = smn_read(p, kSmuFirmwareFlagsIndex, &ok);
    if (!ok || flags == 0xFFFFFFFFu) {
        evf(p.env->ev, kTagPre,
            "FAIL: MP1_FIRMWARE_FLAGS unreadable (read-fault, stop): "
            "smn=0x%08x v=0x%08x ok=%u",
            kSmuFirmwareFlagsIndex, flags, ok ? 1u : 0u);
        r = r3_substep_fail(kR3SecPre, 5, kPreFlagsUnread);
        r3_substep_note(&r, kSmuFirmwareFlagsIndex, flags, "MP1_FIRMWARE_FLAGS");
        return r;
    }
    if ((flags & kSmuFirmwareFlagsInterruptsEnabled) == 0u)
        evf(p.env->ev, kTagPre,
            "WARN: MP1_FIRMWARE_FLAGS=0x%08x bit0=0 (PMFW interrupt flag clear); "
            "continue, C0.5 effect verdict is the final gate", flags);
    evf(p.env->ev, kTagPre,
        "S1/P4 FIRMWARE_FLAGS=0x%08x bit0=%u (RO, smn=0x%08x); MP1 base[1]=0x%08x",
        flags, (flags & kSmuFirmwareFlagsInterruptsEnabled) ? 1u : 0u,
        kSmuFirmwareFlagsIndex, p.mp1_base);
    return r;
}

// ---------------------------------------------------------------------------------------
// mailbox transaction (smu_cmn.c:459-578)
// ---------------------------------------------------------------------------------------
int poll_nonzero(Smu &p, uint32_t abs, uint32_t timeout_us, uint32_t step_us,
                 uint32_t *out, bool *out_ok)
{
    uint32_t v = 0;
    const uint32_t polls = timeout_us / step_us;
    for (uint32_t i = 0; i < polls; i++) {
        bool ok = false;
        v = reg_read(p, abs, &ok);
        if (!ok) {
            *out = v;
            *out_ok = false;
            return -1;
        }
        if (v != 0u) {
            *out = v;
            *out_ok = true;
            return 1;
        }
        if (step_us >= 1000u)
            r3_io_mdelay(p.io, step_us / 1000u);
        else
            r3_io_udelay(p.io, step_us);
    }
    *out = v;
    *out_ok = true;
    return 0;
}

Tx send_msg(Smu &p, uint32_t msg, uint32_t arg, bool want_ret, const char *ctx)
{
    Tx t;
    t.sent = false;
    t.read_fault = false;
    t.denied = false;
    t.resp = 0;
    t.resp_ok = true;
    t.ret = 0;
    t.ret_ok = false;

    // Pre-poll: the previous message must have completed (first message skipped).
    if (!p.first_msg) {
        uint32_t v = 0;
        bool ok = false;
        const int w = poll_nonzero(p, p.resp_addr, kSmuMailboxTimeoutUs, kSmuMailboxPollUs,
                                   &v, &ok);
        if (w != 1) {
            t.read_fault = !ok;
            evf(p.env->ev, kTagSeq, "%s pre-poll %s v=0x%08x", ctx,
                ok ? "timeout" : "read-fault", v);
            return t;
        }
        // Linux refuses NONE (handled by the timeout) and the unknown/EREMOTEIO codes:
        // 0xFB and 0xFFFFFFFF must not be overwritten by a new message.
        if (v == 0xFFFFFFFFu || v == kSmuRespDebugEnd) {
            t.read_fault = (v == 0xFFFFFFFFu);
            evf(p.env->ev, kTagSeq, "%s pre-poll blocks on 0x%08x", ctx, v);
            return t;
        }
    }

    if (!reg_write(p, p.resp_addr, 0u, "smu-mbox-resp-clear", &t.denied)) {
        evf(p.env->ev, kTagSeq, "%s FAIL: resp clear DENY/failed", ctx);
        return t;
    }
    if (!reg_write(p, p.arg_addr, arg, "smu-mbox-arg", &t.denied)) {
        evf(p.env->ev, kTagSeq, "%s FAIL: arg write DENY/failed", ctx);
        return t;
    }
    if (!reg_write(p, p.msg_addr, msg, ctx, &t.denied)) {
        evf(p.env->ev, kTagSeq, "%s FAIL: msg write DENY/failed", ctx);
        return t;
    }
    p.first_msg = false;
    t.sent = true;

    uint32_t v = 0;
    bool ok = false;
    const int w = poll_nonzero(p, p.resp_addr, kSmuMailboxTimeoutUs, kSmuMailboxPollUs, &v, &ok);
    if (w < 0) {
        t.read_fault = true;
        t.resp = 0xFFFFFFFFu;
        t.resp_ok = false;
        evf(p.env->ev, kTagSeq, "%s post-poll read-fault", ctx);
        return t;
    }
    if (w == 0) {
        t.resp = 0u;
        t.resp_ok = true;
        evf(p.env->ev, kTagSeq, "%s post-poll timeout after %u us", ctx, kSmuMailboxTimeoutUs);
        return t;
    }
    t.resp = v;
    t.resp_ok = true;
    if (v == 0xFFFFFFFFu) {
        t.read_fault = true;
        evf(p.env->ev, kTagSeq, "%s post-poll 0xFFFFFFFF (read-path fault)", ctx);
        return t;
    }
    if (v == kSmuRespOk && want_ret) {
        bool rok = false;
        const uint32_t rv = reg_read(p, p.arg_addr, &rok);
        t.ret = rv;
        t.ret_ok = rok && rv != 0xFFFFFFFFu;
        if (!t.ret_ok)
            t.read_fault = true;
    }
    return t;
}

// ---------------------------------------------------------------------------------------
// effect criterion (J-2+J-5)
// ---------------------------------------------------------------------------------------
EffectSample take_sample(Smu &p)
{
    EffectSample s;
    s.grbm_status = reg_read(p, p.gc0 + kSmuGrbmStatus, &s.grbm_ok);
    s.rlc_stat = reg_read(p, p.gc1 + kSmuRlcStat, &s.rlc_ok);
    s.grbm_status2 = reg_read(p, p.gc0 + kSmuGrbmStatus2, &s.grbm2_ok);
    s.gb_addr_config = reg_read(p, p.gc0 + kSmuGbAddrConfig, &s.gb_ok);
    s.sdma0_status = reg_read(p, p.sdma0 + kSmuSdma0StatusReg, &s.sdma_ok);
    return s;
}

bool sample_gate(const EffectSample &s)
{
    return s.grbm_ok && valid_value(s.grbm_status);
}

bool sample_fault(const EffectSample &s)
{
    return !s.grbm_ok || !s.rlc_ok || s.grbm_status == 0xFFFFFFFFu ||
           s.rlc_stat == 0xFFFFFFFFu;
}

EffectResult effect_poll(Smu &p)
{
    EffectResult r;
    r.pass = false;
    r.read_fault = false;
    r.samples = 0;
    r.elapsed_us = 0;
    r.c2pmsg90_at_end = 0;
    r.c2pmsg90_ok_at_end = true;
    r.phase_check = false;
    r.last.grbm_status = 0;
    r.last.grbm_status2 = 0;
    r.last.gb_addr_config = 0;
    r.last.rlc_stat = 0;
    r.last.sdma0_status = 0;
    r.last.grbm_ok = false;
    r.last.grbm2_ok = false;
    r.last.gb_ok = false;
    r.last.rlc_ok = false;
    r.last.sdma_ok = false;

    const bool have_clock = p.io->now_us != nullptr;
    const uint64_t t0 = have_clock ? p.io->now_us(p.io->ctx) : 0u;
    uint64_t acc = 0;
    uint32_t streak = 0;
    bool slow = false;
    bool any_fault = false;

    while (r.samples < kEffectMaxSamples) {
        const uint64_t elapsed = have_clock ? (p.io->now_us(p.io->ctx) - t0) : acc;
        if (elapsed >= kSmuEffectTotalUs)
            break;

        const EffectSample s = take_sample(p);
        r.samples++;
        r.last = s;
        if (sample_fault(s))
            any_fault = true;
        if (sample_gate(s))
            streak++;
        else
            streak = 0;
        if (streak >= 2u) {
            r.pass = true;
            break;
        }

        // T-2/T-3: 2 s fast, then the 10 s slow window only while C2PMSG_90 is still 0.
        if (!slow && elapsed >= kSmuEffectFastUs) {
            bool ok90 = false;
            const uint32_t v90 = reg_read(p, p.resp_addr, &ok90);
            r.c2pmsg90_at_end = v90;
            r.c2pmsg90_ok_at_end = ok90;
            r.phase_check = true;
            if (!ok90 || v90 != 0u)
                break;
            slow = true;
        }

        const uint32_t step_ms = slow ? (kSmuEffectSlowStepUs / 1000u)
                                      : (kSmuEffectFastStepUs / 1000u);
        r3_io_mdelay(p.io, step_ms);
        acc += (uint64_t)step_ms * 1000u;
    }

    r.elapsed_us = have_clock ? (p.io->now_us(p.io->ctx) - t0) : acc;
    // S-06: the recheck looks at the answer captured at the END of the effect window.
    // The 2 s phase check is the end for the non-zero case; if the slow window ran on,
    // a late explicit answer must still be seen, so read once more at its end.
    if (slow && !r.pass) {
        bool ok90 = false;
        const uint32_t v90 = reg_read(p, p.resp_addr, &ok90);
        if (ok90) {
            r.c2pmsg90_at_end = v90;
            r.c2pmsg90_ok_at_end = true;
        }
    }
    r.read_fault = any_fault;
    // §8.1: a read-path fault is fail-closed even when GRBM_STATUS alone reached
    // two valid samples; the companion values must never silently authorise a pass.
    if (r.read_fault)
        r.pass = false;
    return r;
}

// ---------------------------------------------------------------------------------------
// SEQ
// ---------------------------------------------------------------------------------------
R3SubstepResult seq(Smu &p)
{
    R3EvidenceLines *ev = p.env->ev;
    R3SubstepResult r;

    // S2: mailbox configuration. BASE_IDX=1 (mp_13_0_4_offset.h:300-349).
    p.msg_addr = p.mp1_base + kSmuC2pmsg66;
    p.arg_addr = p.mp1_base + kSmuC2pmsg82;
    p.resp_addr = p.mp1_base + kSmuC2pmsg90;
    evf(ev, kTagSeq,
        "S2 mailbox msg=0x%08x arg=0x%08x resp=0x%08x (MP1 base[1]=0x%08x, BASE_IDX=%d)",
        p.msg_addr, p.arg_addr, p.resp_addr, p.mp1_base, kSmuMp1BaseIdx);

    // S3: GetDriverIfVersion. Evidence only: C0.5/C2.3 make the effect criterion
    // the final gate, so a timeout/error here is audited and the sequence continues.
    Tx t3 = send_msg(p, kSmuMsgGetDriverIfVersion, 0u, true, "smu-mbox-0x3-driver-if");
    evf(ev, kTagSeq, "S3 0x3 GetDriverIfVersion sent=%u resp=0x%02x ret=0x%08x ok=%u",
        t3.sent ? 1u : 0u, t3.resp, t3.ret, t3.ret_ok ? 1u : 0u);
    if (t3.denied) {
        r = r3_substep_fail(kR3SecSeq, 3, kSeqDriverIf);
        r3_substep_note(&r, p.resp_addr, 0u, "writeguard DENY");
        return r;
    }
    if (t3.sent && !t3.read_fault && t3.resp == kSmuRespOk && t3.ret_ok) {
        p.driver_if = t3.ret;
        p.driver_if_ok = true;
    } else {
        evf(ev, kTagSeq,
            "S3 audit: driver-if unavailable (sent=%u resp=0x%08x); not the gate "
            "(C0.5/C2.3)", t3.sent ? 1u : 0u, t3.resp);
    }

    // S4: GetPmfwVersion. Same audit-only treatment.
    Tx t2 = send_msg(p, kSmuMsgGetPmfwVersion, 0u, true, "smu-mbox-0x2-pmfw-version");
    evf(ev, kTagSeq, "S4 0x2 GetPmfwVersion sent=%u resp=0x%02x ret=0x%08x ok=%u",
        t2.sent ? 1u : 0u, t2.resp, t2.ret, t2.ret_ok ? 1u : 0u);
    if (t2.denied) {
        r = r3_substep_fail(kR3SecSeq, 4, kSeqPmfwVersion);
        r3_substep_note(&r, p.resp_addr, 0u, "writeguard DENY");
        return r;
    }
    if (t2.sent && !t2.read_fault && t2.resp == kSmuRespOk && t2.ret_ok) {
        p.pmfw = t2.ret;
        p.pmfw_ok = true;
    } else {
        evf(ev, kTagSeq,
            "S4 audit: PMFW version unavailable (sent=%u resp=0x%08x); not the gate "
            "(C0.5/C2.3)", t2.sent ? 1u : 0u, t2.resp);
    }

    // S5: EnableGfxImu(1). This is the only message the D11 recheck may resend.
    Tx t16 = send_msg(p, kSmuMsgEnableGfxImu, kSmuEnableImuArgGfxoff, false,
                      "s5-0x16-enable-gfx-imu");
    if (t16.sent)
        p.gfx_imu_sends++;
    evf(ev, kTagSeq, "S5 0x16 EnableGfxImu(1) sent=%u resp=0x%02x", t16.sent ? 1u : 0u,
        t16.resp);
    if (!t16.sent || t16.read_fault) {
        r = r3_substep_fail(kR3SecSeq, 5, kSeqEnableImu);
        r3_substep_note(&r, p.resp_addr, t16.resp, "C2PMSG_90");
        return r;
    }

    // S6: effect verdict (the final gate).
    EffectResult e1 = effect_poll(p);
    p.effect_samples = e1.samples;
    p.effect_elapsed_us = e1.elapsed_us;
    p.last_grbm_status = e1.last.grbm_status;
    p.last_rlc_stat = e1.last.rlc_stat;
    p.last_grbm_status2 = e1.last.grbm_status2;
    p.last_gb_addr_config = e1.last.gb_addr_config;
    p.last_sdma0_status = e1.last.sdma0_status;
    evf(ev, kTagSeq,
        "S6 effect#1 pass=%u samples=%u elapsed=%llu us grbm=0x%08x rlc=0x%08x "
        "gb=0x%08x grbm2=0x%08x sdma0=0x%08x c2p90@2s=0x%08x phase=%u fault=%u",
        e1.pass ? 1u : 0u, e1.samples, (unsigned long long)e1.elapsed_us,
        e1.last.grbm_status, e1.last.rlc_stat, e1.last.gb_addr_config,
        e1.last.grbm_status2, e1.last.sdma0_status, e1.c2pmsg90_at_end,
        e1.phase_check ? 1u : 0u, e1.read_fault ? 1u : 0u);

    if (e1.pass) {
        p.effect_pass = true;
        if (t16.resp == kSmuRespNone) {
            p.async_success = true;
            evf(ev, kTagSeq, "verdict: async-success (mailbox timeout, effect valid)");
        }
    } else if (e1.read_fault) {
        p.effect_fault = true;
        r = r3_substep_fail(kR3SecSeq, 6, kSeqEffectFault);
        r3_substep_note(&r, p.gc0 + kSmuGrbmStatus, e1.last.grbm_status, "GRBM_STATUS");
        r3_substep_note(&r, p.gc1 + kSmuRlcStat, e1.last.rlc_stat, "RLC_STAT");
        return r;
    } else if (t16.resp == kSmuRespNone || t16.resp == kSmuRespBusy) {
        // D11: one recheck, only for timeout (0) or busy (0xFC). The standard
        // transaction's pre-poll is the "wait C2PMSG_90 non-zero (2 s)" step: a stuck 0
        // means the message is never resent. The guard lives in rt->substep.flags.
        // S-06: a window-end C2PMSG_90 that is an explicit error code means PMFW answered
        // late; that is not a timeout, so it must not be rechecked.
        const bool late_explicit =
            e1.phase_check && e1.c2pmsg90_ok_at_end &&
            (e1.c2pmsg90_at_end == kSmuRespFailed ||
             e1.c2pmsg90_at_end == kSmuRespBadPrereq ||
             e1.c2pmsg90_at_end == kSmuRespUnknownCmd);
        EffectResult e2;
        if (late_explicit) {
            evf(ev, kTagSeq,
                "S6 recheck suppressed: window-end C2PMSG_90=0x%02x is an explicit error",
                e1.c2pmsg90_at_end);
        } else if ((p.env->rt->substep.flags & R3_SUBSTEP_RECHECK_USED) != 0u) {
            evf(ev, kTagSeq,
                "S6 recheck suppressed: R3_SUBSTEP_RECHECK_USED already latched (no resend)");
        } else {
            r3_wg_set_path("retry");
            Tx t16b = send_msg(p, kSmuMsgEnableGfxImu, kSmuEnableImuArgGfxoff, false,
                               "s5b-0x16-recheck");
            if (!t16b.sent) {
                evf(ev, kTagSeq,
                    "S6 recheck: C2PMSG_90 stayed 0 within %u us -> not resent (P-2/P-3)",
                    kSmuMailboxTimeoutUs);
            } else {
                p.env->rt->substep.flags |= R3_SUBSTEP_RECHECK_USED;
                p.gfx_imu_sends++;
                evf(ev, kTagSeq, "S6 recheck: 0x16(1) resent, resp=0x%02x", t16b.resp);
                if (t16b.read_fault) {
                    p.effect_fault = true;
                    r = r3_substep_fail(kR3SecSeq, 6, kSeqEffectFault);
                    r3_substep_note(&r, p.resp_addr, t16b.resp, "C2PMSG_90");
                    return r;
                }
                e2 = effect_poll(p);
                p.effect_samples = e2.samples;
                p.effect_elapsed_us = e2.elapsed_us;
                p.last_grbm_status = e2.last.grbm_status;
                p.last_rlc_stat = e2.last.rlc_stat;
                p.last_grbm_status2 = e2.last.grbm_status2;
                p.last_gb_addr_config = e2.last.gb_addr_config;
                p.last_sdma0_status = e2.last.sdma0_status;
                evf(ev, kTagSeq,
                    "S6 effect#2 pass=%u samples=%u elapsed=%llu us grbm=0x%08x rlc=0x%08x "
                    "gb=0x%08x grbm2=0x%08x sdma0=0x%08x c2p90@2s=0x%08x phase=%u fault=%u",
                    e2.pass ? 1u : 0u, e2.samples, (unsigned long long)e2.elapsed_us,
                    e2.last.grbm_status, e2.last.rlc_stat, e2.last.gb_addr_config,
                    e2.last.grbm_status2, e2.last.sdma0_status, e2.c2pmsg90_at_end,
                    e2.phase_check ? 1u : 0u, e2.read_fault ? 1u : 0u);
                if (e2.pass) {
                    p.effect_pass = true;
                } else if (e2.read_fault) {
                    p.effect_fault = true;
                    r = r3_substep_fail(kR3SecSeq, 6, kSeqEffectFault);
                    r3_substep_note(&r, p.gc0 + kSmuGrbmStatus, e2.last.grbm_status,
                                    "GRBM_STATUS");
                    r3_substep_note(&r, p.gc1 + kSmuRlcStat, e2.last.rlc_stat, "RLC_STAT");
                    return r;
                }
            }
        }
        if (!p.effect_pass) {
            r = r3_substep_fail(kR3SecSeq, 6, kSeqEffect);
            r3_substep_note(&r, p.gc0 + kSmuGrbmStatus, p.last_grbm_status, "GRBM_STATUS");
            r3_substep_note(&r, p.gc1 + kSmuRlcStat, p.last_rlc_stat, "RLC_STAT");
            return r;
        }
    } else {
        // Mailbox OK but the effect is not established, or an explicit error code
        // (0xFF/0xFD/0xFE/0xFB): never recheck.
        if (t16.resp == kSmuRespOk)
            evf(ev, kTagSeq, "S6 mailbox OK but effect not established -> fail (no recheck)");
        else
            evf(ev, kTagSeq, "S6 explicit error 0x%02x -> no recheck", t16.resp);
        r = r3_substep_fail(kR3SecSeq, 6, kSeqEffect);
        r3_substep_note(&r, p.resp_addr, t16.resp, "C2PMSG_90");
        return r;
    }

    // S7: GetEnabledSmuFeatures low/high. Audit only (C2.2); a failure here never gates.
    Tx flo = send_msg(p, kSmuMsgGetEnabledSmuFeatures, 0u, true, "smu-mbox-0x12-feature-low");
    if (flo.sent && !flo.read_fault && flo.resp == kSmuRespOk && flo.ret_ok) {
        p.feature_lo = flo.ret;
        p.feature_lo_ok = true;
    }
    Tx fhi = send_msg(p, kSmuMsgGetEnabledSmuFeatures, 1u, true, "smu-mbox-0x12-feature-high");
    if (fhi.sent && !fhi.read_fault && fhi.resp == kSmuRespOk && fhi.ret_ok) {
        p.feature_hi = fhi.ret;
        p.feature_hi_ok = true;
    }
    evf(ev, kTagSeq,
        "S7 0x12 features=0x%08x_%08x low_ok=%u high_ok=%u (audit, read after 0x16)",
        p.feature_hi, p.feature_lo, p.feature_lo_ok ? 1u : 0u, p.feature_hi_ok ? 1u : 0u);
    return r3_substep_pass();
}

// ---------------------------------------------------------------------------------------
// CHK
// ---------------------------------------------------------------------------------------
R3SubstepResult chk(Smu &p)
{
    R3EvidenceLines *ev = p.env->ev;
    R3SubstepResult r;

    const uint32_t pmfw_major = (p.pmfw >> 16) & 0xFFu;
    const uint32_t pmfw_minor = (p.pmfw >> 8) & 0xFFu;
    const uint32_t pmfw_debug = p.pmfw & 0xFFu;
    evf(ev, kTagChk,
        "C0.4 PMFW=%u.%u.%u (raw=0x%08x) driver-if=%u (raw=0x%08x)%s%s",
        pmfw_major, pmfw_minor, pmfw_debug, p.pmfw, p.driver_if, p.driver_if,
        (p.pmfw_ok && p.pmfw != 0u) ? "" : " pmfw=unavailable",
        (p.driver_if_ok && p.driver_if != 0u) ? "" : " driver-if=unavailable");

    // C0.4 evidence is audited, not gated: C0.5/C2.3 make the effect criterion the
    // final gate, so an unavailable/zero version must not latch the SMU off.
    if (!p.pmfw_ok || p.pmfw == 0u)
        evf(ev, kTagChk,
            "C0.4 audit: PMFW version unavailable/zero (0x%08x); effect gate is final "
            "(C0.5/C2.3)", p.pmfw);
    if (!p.driver_if_ok || p.driver_if == 0u)
        evf(ev, kTagChk,
            "C0.4 audit: driver-if unavailable/zero (0x%08x); effect gate is final "
            "(C0.5/C2.3)", p.driver_if);
    if (!p.effect_pass) {
        r = r3_substep_fail(kR3SecChk, 2, kChkEffect);
        r3_substep_note(&r, p.gc0 + kSmuGrbmStatus, p.last_grbm_status, "GRBM_STATUS");
        r3_substep_note(&r, p.gc1 + kSmuRlcStat, p.last_rlc_stat, "RLC_STAT");
        return r;
    }

    // D10: a value difference is audited, never silently accepted and never fatal.
    if (p.pmfw_ok && p.pmfw != 0u && p.pmfw != kSmuBaselinePmfw)
        evf(ev, kTagChk,
            "D10 PMFW 0x%08x != nullarc 76.101.0 (0x%08x): audit, not silently accepted",
            p.pmfw, kSmuBaselinePmfw);
    if (p.driver_if_ok && p.driver_if != 0u && p.driver_if != kSmuBaselineDriverIf)
        evf(ev, kTagChk,
            "D10 driver-if %u != nullarc %u: audit, not silently accepted",
            p.driver_if, kSmuBaselineDriverIf);
    if (p.feature_lo_ok && p.feature_hi_ok) {
        if (p.feature_hi != kSmuBaselineFeatureHi || p.feature_lo != kSmuBaselineFeatureLo)
            evf(ev, kTagChk,
                "D10 features 0x%08x_%08x != nullarc 0x%08x_%08x (read after 0x16; "
                "nullarc read before 0x16): time-point difference, audit",
                p.feature_hi, p.feature_lo, kSmuBaselineFeatureHi, kSmuBaselineFeatureLo);
        else
            evf(ev, kTagChk, "features match nullarc baseline 0x%08x_%08x",
                p.feature_hi, p.feature_lo);
    } else {
        evf(ev, kTagChk, "feature unavailable (0x12 audit only, non-gating)");
    }
    return r3_substep_pass();
}

} // namespace

// ---------------------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------------------
R3SubstepResult smu_v13_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    Smu p;
    memset(&p, 0, sizeof p);
    p.io = io;
    p.env = env;
    p.first_msg = true;

    if (!env || !env->rt) {
        R3SubstepResult r = r3_substep_fail(kR3SecPre, 1, kPreNoEnv);
        if (env && env->ev)
            evf(env->ev, kTagPre, "FAIL: no environment/runtime block");
        return r;
    }

    // The whole substep (SMN read + mailbox) is serialised: the PCIE_INDEX2/DATA2 pair
    // and the mailbox registers are shared. The SMN adapter's hard timeout is the only
    // blocking bound outside this file.
    const bool have_io = r3_io_complete(io);
    r3_wg_arm_substep(WSubstep::Smu);
    r3_wg_expect_writes(getSmuRows(), getSmuRowCount(), nullptr, 0, WSubstep::Smu);
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env->ip, getSmuRows(), getSmuRowCount(), declared);
    r3_wg_expect_declared_addrs(declared, getSmuRowCount());
    if (have_io)
        io->lock(io->ctx);

    R3SubstepResult r = pre(p);
    if (r.status == kR3SsecOk)
        r = seq(p);
    if (r.status == kR3SsecOk)
        r = chk(p);

    if (have_io)
        io->unlock(io->ctx);

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getSmuRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }
    r3_wg_disarm_substep();

    if (r.status == kR3SsecOk) {
        env->rt->ready_bits |= R3_IP_BIT(R3Ip::Smu);
        evf(env->ev, "smu",
            "ready: pmfw=%u.%u.%u driver-if=%u 0x16 sends=%u effect=%s%s samples=%u "
            "elapsed=%llu us features=0x%08x_%08x",
            (p.pmfw >> 16) & 0xFFu, (p.pmfw >> 8) & 0xFFu, p.pmfw & 0xFFu,
            p.driver_if, p.gfx_imu_sends,
            p.async_success ? "async-success" : "sync", p.effect_fault ? "(fault)" : "",
            p.effect_samples, (unsigned long long)p.effect_elapsed_us,
            p.feature_hi, p.feature_lo);
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-smu-done", "Yes", 3);
    } else {
        evf(env->ev, "smu", "FAIL %s (section %s step %u)", r.name,
            r3_substep_section_name(r.section), static_cast<unsigned>(r.step));
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-smu-done", "No", 2);
    }
    return r;
}

/* Step2: declaration table (rdna3_writeguard.h R3WriteRowReg). */
namespace {
constexpr R3WriteRowReg kSmuRows[] = {
    {&r3gold::kMp1SmnC2pmsg66, static_cast<uint8_t>(IPBlock::MP1), 0, 0, 1, WClass::Protocol, WNeed::Required, "smu-mbox-msg"},
    {&r3gold::kMp1SmnC2pmsg82, static_cast<uint8_t>(IPBlock::MP1), 0, 0, 1, WClass::Protocol, WNeed::Required, "smu-mbox-arg"},
    {&r3gold::kMp1SmnC2pmsg90, static_cast<uint8_t>(IPBlock::MP1), 0, 0, 1, WClass::Protocol, WNeed::Required, "smu-mbox-resp"},
};
constexpr uint32_t kSmuRowCount = sizeof(kSmuRows) / sizeof(kSmuRows[0]);
static_assert(kSmuRowCount == 3, "smu declaration table must have 3 rows");
} // anonymous namespace

const R3WriteRowReg* getSmuRows(void) { return kSmuRows; }
uint32_t getSmuRowCount(void) { return kSmuRowCount; }

} // namespace amdgpu
