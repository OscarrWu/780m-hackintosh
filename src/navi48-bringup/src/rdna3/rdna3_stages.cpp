// rdna3_stages.cpp - the RDNA3 stage ladder (common StageRunner), the stage-2 integration
// point and the read-only failure dump.
//
// Stage 0 is the read-only survey plus the one-time stage-2 preparation (layout planning and
// production whitelist registration). Preparation is not a power-up: it performs no write and
// is therefore not gated by kill/latch (StageRunner R5 already exempts stage 0).
//
// Stages 1..5 are the five IP substeps in power-up order psp -> smu -> gfx -> sdma -> mes.
// Their enabled() predicates are the F2/F3 gate: stage depth && !killed && !latched &&
// dependency closure, plus "preparation succeeded". The policy itself lives in K0's
// r3_gate_enabled(); rdna3_integration maps it onto the R3Ip bit space.
//
// Failure dump: a stage-0 failure keeps the stage-1 read-only dump (GCVM + IH snapshot). A
// stage-2 failure uses the K9 dump, whose available set is declared per failing substep and
// which never touches IH (stage 2 does not initialise it).
#include "rdna3_survey.h"
#include "rdna3_writeguard.h"
#include "common/TargetConfig.h"
#include "rdna3_dump.h"
#include "golden_values.h"
#include "rdna3_integration.h"
#include "rdna3_teardown.h"

#include "amdgpu_regs.h"
#include "rdna3_ip_entries.h"
#include <IOKit/IOLib.h>
#include <stdarg.h>

namespace {

using amdgpu::DeviceContext;
using amdgpu::IPBlock;

// C4 TMR slot reserved by the layout (tech plan §4.2: 0x08000000..0x0C000000, 64 MiB).
// PSP's LOAD_TOC response is validated against this slot by the PSP substep (S6), so the
// layout never has to guess the runtime size and still plans every grant before any write.
constexpr uint64_t R3_TMR_LAYOUT_RESERVE = 64ull * 1024ull * 1024ull;

// Selected stage-0 depth (0..5 = T1..T6). Set once by the entry class before the ladder runs;
// the survey module reads it through r3_stage0_depth(). Out-of-range input falls back to T1.
static uint32_t g_stage0_depth = 0;

static void emit(StageCtx &ctx, const char *text) {
    if (ctx.log) ctx.log(ctx.user, text);
}

// D-06: every stage-side line carries the N48R3LOG prefix (the runner's own lines are P4b).
static void emitf(StageCtx &ctx, const char *fmt, ...) {
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    char line[224];
    snprintf(line, sizeof line, "%s %s", TGT_LOG_PREFIX, body);
    emit(ctx, line);
}

// v1.7: per-step progress evidence. The value is a short token published as its own property
// so a user-space poller can persist the last completed step to disk; a freeze therefore leaves
// evidence that survives the forced power-off (the final summary/log do not).
static void progress(StageCtx &ctx, const char *text) {
    R3SurveyStageState *st = static_cast<R3SurveyStageState *>(ctx.user);
    if (st && st->sink.set_property && text && text[0])
        st->sink.set_property(st->sink.user, "N48R3EVID-progress", text, strlen(text));
}

// D-10: one dump line goes to the main log, the survey line arena, and the independent
// N48R3EVID-failure-dump payload assembled by the caller. `text` carries no prefix; the export
// contract adds "<TGT_LOG_PREFIX> <tag>: ".
static void dump_line(StageCtx &ctx, R3SurveyStageState *st, char *evid, size_t cap,
                      size_t *used, const char *text) {
    char line[224];
    snprintf(line, sizeof line, "%s %s", TGT_LOG_PREFIX, text);
    emit(ctx, line);
    if (st) r3_lines_add(&st->lines, "dumper", text);
    if (used && *used < cap) {
        const size_t n = tgt_log_line(evid + *used, cap - *used, "dumper", text);
        if (n >= cap - *used) *used = cap - 1;
        else *used += n;
    }
}

// ---------------------------------------------------------------------------------------
// Stage-2 preparation: layout plan + production whitelist registration (K10)
// ---------------------------------------------------------------------------------------
// A pre-power-up step: no register is written. It runs at the end of stage 0 because the
// survey is what resolves the IP base table and the D3 VRAM readings the plan consumes.
// Failing here must stop stages 1..5 (fail closed): an unregistered whitelist turns the
// first power-up write into a DENY on real hardware (K7 R1).
static R3IntegrationCtx *integ_ctx(StageCtx &ctx) {
    return static_cast<R3IntegrationCtx *>(ctx.substep);
}

// Same lookup as rdna3_survey's file-local helper (the discovery report is a flat table).
static const R3IpBase *find_ip(const R3DiscoveryReport &d, uint16_t hw) {
    for (uint32_t i = 0; i < d.ip_count; i++)
        if (d.ips[i].hwId == hw) return &d.ips[i];
    return nullptr;
}

// Read-only callback over the BAR5-mapped DeviceContext (survey's read path).
static bool read_dc_reg(void *user, uint32_t dword, uint32_t *value) {
    DeviceContext *d = static_cast<DeviceContext *>(user);
    if (!d || !value || !d->rmmio || (size_t)dword * 4u + 4u > d->rmmioSize)
        return false;
    *value = d->rmmio[dword];
    return true;
}

static bool prepare_stage2(StageCtx &ctx, R3IntegrationCtx *ic, R3SurveyStageState *st) {
    if (!ic) {
        return false;
    }
    ic->prepared = true;
    ic->prep_ok = false;
    ic->prep_fail = nullptr;
    ic->prep_chain[0] = '\0';

    if (!st || !st->hw.dev) {
        r3_integration_note_prep_failure(ic, "prep:no-device");
        return false;
    }
    DeviceContext &d = *st->hw.dev;

    // L8: every layout input must be a MEASURED reading. MC base and the BAR0 window come
    // from stage 0; the remaining read-only inputs (FB system offset, live scanout end) are
    // collected here, the "small new read-only set" §4.2 assigns to the integration card.
    // The three MMHUB FB registers are BASE_IDX 1 and resolve through the golden table (A-01).
    uint32_t fb_off_addr = 0, fb_base_addr = 0, fb_top_addr = 0;
    if (!d.ip.resolve(r3gold::kMmmcVmFbOffset.ref(), &fb_off_addr) ||
        !d.ip.resolve(r3gold::kMmmcVmFbLocationBase.ref(), &fb_base_addr) ||
        !d.ip.resolve(r3gold::kMmmcVmFbLocationTop.ref(), &fb_top_addr)) {
        r3_integration_note_prep_failure(ic, "prep:mmhub-unresolved");
        return false;
    }
    const uint32_t fb_off = amdgpu::RREG32(d, fb_off_addr);
    const uint32_t fb_base = amdgpu::RREG32(d, fb_base_addr);
    const uint32_t fb_top = amdgpu::RREG32(d, fb_top_addr);
    if (fb_off == 0xFFFFFFFFu || fb_base == 0xFFFFFFFFu || fb_top == 0xFFFFFFFFu) {
        r3_integration_note_prep_failure(ic, "prep:fb-unreadable");
        return false;
    }

    R3LayoutInputs in {};
    // Address roles (verified against Linux gmc_v11_0_vram_gtt_location + amdgpu_gmc_vram_mc2pa
    // and nullarc PhxBringup::fbPhys): MC base = MMHUB FB_LOCATION_BASE; system physical base
    // = MMHUB FB_OFFSET. GC's GCMC_VM_FB_OFFSET belongs to the SR-IOV/virtual resume path
    // in Linux, but on this bare-metal APU the VBIOS left it at 0 (preflight r3
    // GCMC_VM_FB_OFFSET=0). MES S-1 now writes it conditionally (方案 v3 §6.2.2/§4.2).
    in.fb_mc_base = static_cast<uint64_t>(fb_base) << 24;
    in.fb_phys_base = static_cast<uint64_t>(fb_off) << 24;   /* §4.4: runtime-only */
    in.bar0_base = st->hw.bar0_base;
    /* §4.1/R4: real_vram_size = (FB_LOCATION_TOP - BASE + 1) << 24 (inclusive top). */
    in.real_vram_size = (static_cast<uint64_t>(fb_top) - fb_base + 1ull) << 24;
    in.vbios_reserve = R3_TOPGUARD_MIN;   /* L4 floor until the VBIOS value is measured */
    in.bar0_size = st->hw.bar0_size;
    // P3-4 (review): passing a fixed 64 MiB slot means the allocator's L3 reorder/degrade
    // path is intentionally unreachable from the integration layer; a PSP size that does not
    // fit the slot fails closed at S6 instead. Kept as a conscious conservative cut.
    // L2 tension: the TMR size is only known from PSP LOAD_TOC, but the layout must be
    // planned before any power-up write. §4.2 settles it: C4 is a 64 MiB SLOT at
    // 0x08000000..0x0C000000 ("尺寸=PSP 返回；起点 64 MiB 自然对齐；缺省槽位"), so the plan
    // reserves the slot and PSP's S6 assertion validates the returned size against it. A
    // larger PSP value is caught by the substep before it writes past the grant.
    in.tmr_size = R3_TMR_LAYOUT_RESERVE;
    in.gop_end = 0;
    in.gop_end_known = false;

    // Live scanout end (L1). A read failure leaves gop_end_known=false (fail closed).
    const R3IpBase *dmu = st->hw.disc ? find_ip(*st->hw.disc, IpDiscovery::HwDmu) : nullptr;
    if (dmu && dmu->numBases > 2u) {
        const uint32_t dcn = dmu->bases[2];
        R3ScanoutReading scan {};
        if (r3_integration_collect_scanout(dcn, &read_dc_reg, &d, in.fb_mc_base,
                                           in.real_vram_size, &scan)) {
            in.gop_end = scan.gop_end;
            in.gop_end_known = true;
        } else {
            r3_integration_note_prep_failure(ic, "prep:scanout-outside-carveout");
        }
    } else {
        r3_integration_note_prep_failure(ic, "prep:dcn-unresolved");
    }

    R3LayoutPlan plan;
    if (r3_layout_compute(&in, &plan) != 0 || !plan.ok) {
        r3_integration_note_prep_failure(ic,
                                         plan.fail_reason ? plan.fail_reason : "prep:layout");
        emitf(ctx, "stage2 prep: layout FAIL %s",
              ic->prep_chain[0] ? ic->prep_chain : "-");
        return false;
    }
    ic->rt.layout = plan;

    // Arm the whitelist, then add every stage-2 row. Prefer the full DMU-segment table (the
    // same one stage 0 installed) so the AUX/MM windows survive; fall back to the MM-only arm
    // when DMU is unresolved. Either way this is a fresh arm: re-installing clears grants and
    // resets the counters, so it must happen before any stage-2 write and after stage 0.
    int arm_rc = -1;
    if (dmu && dmu->numBases >= R3_WG_SEGS) {
        uint32_t seg[R3_WG_SEGS];
        for (uint32_t b = 0; b < R3_WG_SEGS; b++) seg[b] = dmu->bases[b];
        arm_rc = r3_writeguard_install(seg);
    } else {
        arm_rc = r3_writeguard_install_mm();
    }
    if (arm_rc != 0) {
        r3_integration_note_prep_failure(ic, "prep:whitelist-arm");
        return false;
    }
    if (r3_integration_register_production(&plan, &d.ip) != 0) {
        r3_integration_note_prep_failure(ic, "prep:whitelist-register");
        return false;
    }

    // R1 (v1.7): discover the HDP memory-flush window (read-only) and register its single
    // dword. Linux flushes through this window before the PSP doorbell; nullarc does the same
    // on this exact silicon. Submitting without it lets the PSP read a stale/zero ring frame
    // and act on address 0, which is the failure mode that froze the machine. A missing
    // window is therefore fail closed at boot rather than a silent skip.
    // The window is a special NBIO register: offset 0x012D and _BASE_IDX 2 from
    // nbio_7_7_0_offset.h:3856/3857. The base project falls back to group 0 for other NBIO
    // generations; this project has a single target (nbio_7_7_0), so a segment-2 resolve
    // failure means the address model is incomplete and preparation stops here (a
    // plausible-looking wrong window would silently flush the wrong dword; C0.10).
    // It is not in the golden register table because group 2 is outside the recorded
    // segment subset (H4: no invented segment values).
    uint32_t hdp_addr = 0u, hdp_remap = 0u, hdp_dw = 0u;
    bool hdp_ok = false;
    const amdgpu::RegRef hdp_ref{amdgpu::IPBlock::NBIO, 0x012Du, 2u};
    if (d.ip.resolve(hdp_ref, &hdp_addr)) {
        hdp_remap = amdgpu::RREG32(d, hdp_addr);
        hdp_ok = r3_hdp_flush_window(hdp_remap, ic->window.bar5_bytes, &hdp_dw) &&
                 r3_wg_register_shared_reg(hdp_dw, 1u, WClass::Protocol, WSubstep::Psp,
                                           "reg:hdp-flush") == 0;
    }
    if (!hdp_ok) {
        r3_integration_note_prep_failure(ic, "prep:hdp-window");
        emitf(ctx, "stage2 prep: HDP remap=0x%08x abs=0x%08x bar5=%llu",
              hdp_remap, hdp_addr, (unsigned long long)ic->window.bar5_bytes);
        return false;
    }
    r3_wg_disarm_substep();

    // The environment every substep receives. fb_mc_base is the D3 reading; a zero value is
    // rejected by the substeps themselves (fail closed at the use point).
    // Bind the SAME MC base the plan used (the MMHUB FB_LOCATION_BASE reading), so grant
    // MC addresses and the substeps' fb_mc_base can never disagree.
    r3_integration_bind_env(ic, &d.ip, in.fb_mc_base);
    ic->env.hdp_flush_dword = hdp_dw;
    ic->env.hdp_flush_valid = true;
    ic->env.strict_expect = false;

    // Read manifest: the substeps' MMIO wrappers note every read; stage 0 already exported
    // its own. Nothing to do here beyond keeping the manifest object addressable.
    if (ic->prep_fail) {
        emitf(ctx, "stage2 prep: FAIL %s", ic->prep_chain);
        return false;
    }
    ic->prep_ok = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// Stage 1..5 wrappers
// ---------------------------------------------------------------------------------------
typedef R3SubstepResult (*R3SubstepFn)(const R3Io *, const R3SubstepEnv *);

static StageResult run_substep(StageCtx &ctx, R3Ip ip,
                               R3SubstepFn fn, const char *name) {
    R3IntegrationCtx *ic = integ_ctx(ctx);
    if (!ic || !ic->prep_ok) {
        emitf(ctx, "stage %s: FAIL reason=%s prep_chain=%s", name,
              ic && ic->prep_fail ? ic->prep_fail : "prep:missing",
              ic ? ic->prep_chain : "-");
        return kStageFail;
    }
    if (!r3_io_complete(&ic->io)) {
        emitf(ctx, "stage %s: FAIL reason=pre:no-io", name);
        return kStageFail;
    }

    char tok[48];
    snprintf(tok, sizeof tok, "substep-%s", name ? name : "?");
    progress(ctx, tok);

    R3SubstepResult r = fn(&ic->io, &ic->env);
    r3_integration_note_outcome(ic, ip, true, &r);
    snprintf(tok, sizeof tok, "substep-%s-%s", name ? name : "?",
             r3_substep_ok(&r) ? "ok" : "fail");
    progress(ctx, tok);

    if (r3_substep_ok(&r)) {
        emitf(ctx, "stage %s: PASS (%s step %u)", name, r.name ? r.name : "?", r.step);
    } else {
        char snap[160];
        size_t used = 0;
        snap[0] = '\0';
        for (uint32_t i = 0; i < r.reading_count && used + 1u < sizeof snap; i++) {
            const int n = snprintf(snap + used, sizeof snap - used, " %s=0x%08x",
                                   r.readings[i].what ? r.readings[i].what : "?",
                                   r.readings[i].value);
            if (n <= 0)
                break;
            if (static_cast<size_t>(n) >= sizeof snap - used) {
                used = sizeof snap - 1u;
                break;
            }
            used += static_cast<size_t>(n);
        }
        emitf(ctx, "stage %s: FAIL reason=%s step=%u snapshot=%s", name,
              r.name ? r.name : "?", r.step, snap[0] ? snap : "-");
    }

    if (r3_substep_ok(&r)) {
        ic->rt.ready_bits |= R3_IP_BIT(ip);
        return kStageOk;
    }

    // §5.4: latch the failed IP BEFORE the dump runs, so the evidence summary and any later
    // gate see the off state (the ladder itself stops anyway; this is the field the plan
    // mandates for a future resume path). The dump hook runs after this in StageRunner.
    ic->rt.latch_bits |= R3_IP_BIT(ip);
    ic->failed = true;
    ic->failed_ip = ip;
    return kStageFail;
}

static StageResult stage1_psp(StageCtx &ctx) {
    return run_substep(ctx, R3Ip::Psp, amdgpu::psp_v13_0_substep, "psp");
}
static StageResult stage2_smu(StageCtx &ctx) {
    return run_substep(ctx, R3Ip::Smu, amdgpu::smu_v13_0_substep, "smu");
}
static StageResult stage3_gfx(StageCtx &ctx) {
    return run_substep(ctx, R3Ip::Gfx, amdgpu::gfx_v11_0_substep, "gfx");
}
static StageResult stage4_sdma(StageCtx &ctx) {
    return run_substep(ctx, R3Ip::Sdma, amdgpu::sdma_v6_0_substep, "sdma");
}
static StageResult stage5_mes(StageCtx &ctx) {
    return run_substep(ctx, R3Ip::Mes, amdgpu::mes_v11_0_substep, "mes");
}

// D-02: the read-only gate between the survey and the first power-up write. It samples the
// probe set through R3Io (B5 read manifest), hands the readings to the pure evaluator and
// publishes the N48R3EVID-preflight comparison table. All judgement lives in
// r3_preflight_evaluate(); this function only collects.
// The probes are 10 bounded single-dword BAR5 reads (never SMN, never polled); the R3Io
// lock is held across them so they serialise with every substep, and no L2/flush path runs.
static const char *preflight_verdict(R3PreflightVerdict v) {
    switch (v) {
    case R3PreflightVerdict::Ok: return "ok";
    case R3PreflightVerdict::ReadFail: return "read-fail";
    case R3PreflightVerdict::AllOnes: return "all-ones";
    case R3PreflightVerdict::ValueMismatch: return "value-mismatch";
    }
    return "?";
}

static StageResult stage_preflight(StageCtx &ctx) {
    R3SurveyStageState *st = static_cast<R3SurveyStageState *>(ctx.user);
    R3IntegrationCtx *ic = integ_ctx(ctx);
    progress(ctx, "preflight");
    if (!st || !st->hw.dev || !ic) {
        emitf(ctx, "stage preflight: FAIL reason=pre:no-device");
        return kStageFail;
    }
    const R3Io *io = &ic->io;
    if (!io->mmio) {
        emitf(ctx, "stage preflight: FAIL reason=pre:no-io");
        return kStageFail;
    }

    const uint32_t probe_count = r3_preflight_probe_count();
    R3PreflightReading readings[R3_PREFLIGHT_MAX] = {};
    const bool locked = io->lock && io->unlock;
    if (locked)
        io->lock(io->ctx);
    for (uint32_t i = 0; i < probe_count && i < R3_PREFLIGHT_MAX; i++) {
        const r3gold::GoldenReg *g = r3_preflight_probe(i);
        uint32_t addr = 0;
        bool ok = false;
        uint32_t value = 0;
        const bool resolved = g && st->hw.dev->ip.resolve(g->ref(), &addr);
        if (resolved)
            value = r3_io_read32(io, addr, &ok);
        readings[i].reg = g;
        readings[i].read_ok = resolved && ok;
        readings[i].addr = addr;
        readings[i].value = value;
    }
    if (locked)
        io->unlock(io->ctx);

    R3PreflightResult res {};
    r3_preflight_evaluate(readings, probe_count, &res);

    /* D3/S12: the PSP version pre comes from the existing C2PMSG_58 probe (preflight stays
     * 11 probes). The RLC/SDMA version observations belong to their own substeps; adding
     * them here would let an all-ones/read failure of an observation block the trigger.
     * Extraction + env sync live in ONE pure core (r3_stages_preflight_sync_env), CALLED
     * FROM HERE, so deleting this call is caught by the host runner (test_preflight_env_
     * sync exercises the same entry); the stage table itself is kext-only. */
    r3_stages_preflight_sync_env(ic, readings, probe_count, &res);

    // The comparison table is the D3 baseline rebuild: MMHUB FB base/top and the PSP
    // version are recorded with the corrected segments (v1.4).
    static char evid[2048];
    size_t used = 0;
    evid[0] = '\0';
    const int head = snprintf(evid, sizeof evid, "preflight: %s items=%u\n",
                              res.ok ? "PASS" : "FAIL", res.count);
    if (head > 0)
        used = static_cast<size_t>(head) < sizeof evid ? static_cast<size_t>(head)
                                                       : sizeof evid - 1u;
    for (uint32_t i = 0; i < res.count && used + 1u < sizeof evid; i++) {
        const R3PreflightItem &it = res.items[i];
        const int n = snprintf(evid + used, sizeof evid - used,
                               "preflight: %s addr=0x%08x value=0x%08x verdict=%s\n",
                               it.name, it.addr, it.value,
                               preflight_verdict(it.verdict));
        if (n <= 0)
            break;
        if (static_cast<size_t>(n) >= sizeof evid - used) {
            used = sizeof evid - 1u;
            break;
        }
        used += static_cast<size_t>(n);
    }
    if (st->sink.set_property)
        st->sink.set_property(st->sink.user, "N48R3EVID-preflight", evid, used);
    progress(ctx, res.ok ? "preflight-ok" : "preflight-fail");

    if (res.ok) {
        emitf(ctx, "stage preflight: PASS items=%u", res.count);
        return kStageOk;
    }
    const R3PreflightItem *bad = nullptr;
    for (uint32_t i = 0; i < res.count; i++) {
        if (res.items[i].verdict != R3PreflightVerdict::Ok) {
            bad = &res.items[i];
            break;
        }
    }
    emitf(ctx, "stage preflight: FAIL reason=%s addr=0x%08x value=0x%08x verdict=%s",
          bad ? bad->name : "?", bad ? bad->addr : 0u, bad ? bad->value : 0u,
          bad ? preflight_verdict(bad->verdict) : "?");
    return kStageFail;
}

// enabled(ip): preparation succeeded, then K0's bitmap gate. Stage 0 is exempt (R5).
static bool enabled_ip(StageCtx &ctx, R3Ip ip) {
    R3IntegrationCtx *ic = integ_ctx(ctx);
    if (!ic || !ic->prep_ok) {
        return false;
    }
    const R3GateInputs g = r3_integration_gate_inputs(ic);
    return r3_integration_ip_enabled(&g, ip);
}

static bool stage1_enabled(const StageCtx &ctx) {
    return enabled_ip(const_cast<StageCtx &>(ctx), R3Ip::Psp);
}
static bool stage2_enabled(const StageCtx &ctx) {
    return enabled_ip(const_cast<StageCtx &>(ctx), R3Ip::Smu);
}
static bool stage3_enabled(const StageCtx &ctx) {
    return enabled_ip(const_cast<StageCtx &>(ctx), R3Ip::Gfx);
}
static bool stage4_enabled(const StageCtx &ctx) {
    return enabled_ip(const_cast<StageCtx &>(ctx), R3Ip::Sdma);
}
static bool stage5_enabled(const StageCtx &ctx) {
    return enabled_ip(const_cast<StageCtx &>(ctx), R3Ip::Mes);
}

// C0.9 (v1.7): stage 2 consumes the survey products. Admission is data-driven: it resolves
// the exact golden refs the consumers use (never the depth knob) and reports fb-dual as an
// advisory acceptance item. It runs after the survey and before any preparation, so a
// missing product stops the ladder without arming the whitelist (D-05 kept).
static bool stage2_admission(StageCtx &ctx, R3SurveyStageState *st) {
    R3IntegrationCtx *ic = integ_ctx(ctx);
    if (!st || !ic) return false;
    st->gate_ok = false;
    st->gate_summary[0] = '\0';

    R3AdmissionInputs in {};
    in.survey_aborted = st->summary.global_abort;
    in.disc_present = st->hw.disc != nullptr && st->hw.disc->ip_count > 0u;
    uint32_t addr = 0;
    DeviceContext *d = st->hw.dev;
    in.mp0_1   = d && d->ip.resolve(r3gold::kMp0SmnC2pmsg81.ref(), &addr);
    in.mp1_1   = d && d->ip.resolve(r3gold::kMp1SmnC2pmsg90.ref(), &addr);
    in.gc_0    = d && d->ip.resolve(r3gold::kGrbmStatus.ref(), &addr);
    in.sdma_0  = d && d->ip.resolve(r3gold::kSdma0StatusReg.ref(), &addr);
    in.mmhub_1 = d && d->ip.resolve(r3gold::kMmmcVmFbLocationBase.ref(), &addr);
    const R3IpBase *dmu = st->hw.disc ? find_ip(*st->hw.disc, IpDiscovery::HwDmu) : nullptr;
    in.dcn_2 = dmu && dmu->numBases > 2u;
    in.fb_dual_seen = st->hw.fb_dual_seen;
    in.fb_dual_ok = st->hw.fb_dual_ok;

    R3DepthPlan plan = st->plan;
    if (plan.floor == 0u && ic->stage >= 1u) {
        // Defensive: start() always supplies the plan; keep the reason readable otherwise.
        plan.floor = r3_stage_survey_floor(ic->stage);
        plan.effective = r3_stage0_depth();
        plan.requested = plan.effective;
        plan.reason = "default-floor";
    }

    const R3AdmissionResult r = r3_admission_eval(in);
    if (!r.ok) {
        char line[192];
        r3_admission_fail_line(line, sizeof line, r, plan);
        emitf(ctx, "%s", line);
        return false;
    }
    char line[128];
    r3_admission_pass_line(line, sizeof line, plan.effective, r.fb_dual);
    emitf(ctx, "%s", line);
    r3_admission_summary(st->gate_summary, sizeof st->gate_summary, plan.effective, r.fb_dual);
    st->gate_ok = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// Stage 0: survey + preparation
// ---------------------------------------------------------------------------------------
static StageResult stage0_survey(StageCtx &ctx) {
    R3SurveyStageState *st = static_cast<R3SurveyStageState *>(ctx.user);
    if (!st) {
        emit(ctx, "N48R3LOG stage0: FAIL null survey state");
        return kStageFail;
    }

    // C0.9: every run is judged on its own products (the trigger path re-runs stage 0).
    // Clear the previous run's marks and the base table: a failed discovery is caught by the
    // report count and by sentinel bases, never by stale data from an earlier run.
    if (st->hw.disc) st->hw.disc->ip_count = 0u;
    if (st->hw.dev) st->hw.dev->ip = amdgpu::IPBaseTable{};
    st->hw.fb_dual_seen = false;
    st->hw.fb_dual_ok = false;

    progress(ctx, "stage0-survey");
    const int rc = r3_survey_run_hw(&st->hw, &st->lines, st->text, sizeof st->text,
                                    &st->sink, &st->summary);

    emitf(ctx, "stage0 survey %s: ok=%u failed=%u abort=%d done=%d",
          rc == 0 ? "PASS" : "FAIL", st->summary.ok, st->summary.failed,
          st->summary.global_abort ? 1 : 0, r3_survey_finalize(st->summary) ? 1 : 0);

    // Stage-2 preparation runs only when the survey completed and the operator asked for
    // it (target >= 1). D-05: a failed survey must not re-arm the whitelist or reset the
    // writeguard counters/manifest -- the ladder stops in the read-only phase instead.
    R3IntegrationCtx *ic = integ_ctx(ctx);
    const bool survey_ok = rc == 0;
    progress(ctx, "stage0-admission");

    // C0.9: stage 2's mandatory inputs must exist before preparation/preflight. The check is
    // data-driven and runs before the whitelist is armed; D-05 keeps a failed survey from
    // reaching it at all.
    if (survey_ok && ic && ic->stage >= 1u) {
        if (!stage2_admission(ctx, st)) return kStageFail;
    }

    if (ic && r3_integration_should_prepare(survey_ok, ic->stage)) {
        progress(ctx, "stage0-prep");
        const bool ok = prepare_stage2(ctx, ic, st);
        emitf(ctx, "stage2 prep %s: %s vram_top_reserve=1MiB (floor; VBIOS value pending "
                   "R1 measurement)", ok ? "PASS" : "FAIL",
              ok ? "layout+whitelist" : (ic->prep_chain[0] ? ic->prep_chain : "?"));
        // Fail closed: a stage-2 boot whose layout/whitelist is not ready must not run a
        // single power-up substep. Returning Ok here would let the runner skip 1..5 and
        // report a successful ladder.
        if (!ok) return kStageFail;
    }
    if (!survey_ok) {
        emitf(ctx, "stage0: FAIL reason=survey did not complete; preparation skipped (D-05)");
        return kStageFail;
    }
    progress(ctx, "stage0-ready");
    return kStageOk;
}

// ---------------------------------------------------------------------------------------
// Stage-1 read-only dump (survey path). Stage 2 uses the K9 dump instead.
// ---------------------------------------------------------------------------------------
// The dump is part of the survey's read set, so its reads go through the same manifest
// (R3ReadManifest). RREG32 refuses an out-of-range dword without a bus read, so the note is
// gated exactly like manifest_rreg() in rdna3_survey.cpp. Recording never blocks the dump.
static uint32_t dump_rreg(R3SurveyStageState *st, uint32_t abs_dword) {
    DeviceContext &d = *st->hw.dev;
    if (d.rmmio && (size_t)abs_dword * 4u + 4u <= d.rmmioSize)
        r3_manifest_note(&st->hw.manifest, abs_dword);
    return amdgpu::RREG32(d, abs_dword);
}

// Read-only failure dump (stage-1 contract): latched GCVM/MMVM protection-fault status and the
// IH ring pointers. It NEVER advances IH_RB_RPTR and never clears a fault; it only snapshots.
static void dump_failure_state_stage1(StageCtx &ctx) {
    R3SurveyStageState *st = static_cast<R3SurveyStageState *>(ctx.user);
    char evid[1024];
    size_t evid_used = 0;
    evid[0] = '\0';

    // E5 (v1.7): publish the stop reason first. Copy it out before emitting: emit runs
    // logThunk again and would overwrite the capture with this very line.
    {
        const char *reason = (st && st->last_log[0]) ? st->last_log : nullptr;
        char text[224];
        r3_stop_line(text, sizeof text, reason);
        if (st)
            snprintf(st->stop_reason, sizeof st->stop_reason, "%s",
                     (reason && reason[0]) ? reason : "(no reason emitted)");
        char line[256];
        snprintf(line, sizeof line, "%s %s", TGT_LOG_PREFIX, text);
        emit(ctx, line);
        if (st) r3_lines_add(&st->lines, "stop", text);
        const size_t n = tgt_log_line(evid, sizeof evid, "stop", text);
        evid_used = n < sizeof evid ? n : sizeof evid - 1u;
    }

    if (!st || !st->hw.dev) {
        dump_line(ctx, st, evid, sizeof evid, &evid_used, "dumper: no device context");
    } else {
        DeviceContext &d = *st->hw.dev;
        char text[192];

        uint32_t gcvm_status_addr = 0, gcvm_lo_addr = 0, gcvm_hi_addr = 0;
        if (d.ip.resolve(r3gold::kGcvmL2ProtectionFaultStatus.ref(), &gcvm_status_addr) &&
            d.ip.resolve(r3gold::kGcvmL2ProtectionFaultAddrLo32.ref(), &gcvm_lo_addr) &&
            d.ip.resolve(r3gold::kGcvmL2ProtectionFaultAddrHi32.ref(), &gcvm_hi_addr)) {
            const uint32_t status = dump_rreg(st, gcvm_status_addr);
            const uint32_t lo = dump_rreg(st, gcvm_lo_addr);
            const uint32_t hi = dump_rreg(st, gcvm_hi_addr);
            snprintf(text, sizeof text, "dumper: GCVM status=0x%08x addr=0x%08x%08x",
                     status, hi, lo);
            dump_line(ctx, st, evid, sizeof evid, &evid_used, text);
        } else {
            dump_line(ctx, st, evid, sizeof evid, &evid_used, "dumper: GCVM base unresolved");
        }

        // The MMVM fault offset is not present in the pinned Linux headers for this chip, so it is
        // deliberately not guessed (tech plan 4.3). GCVM above is the verified fault source.
        dump_line(ctx, st, evid, sizeof evid, &evid_used,
                  "dumper: MMVM skipped (offset not in pinned headers)");

        // A-05: the IH snapshot resolves through the golden table. Phoenix reports OSSSYS
        // 6.0.1 and Linux's ih_v6_0 includes osssys_6_0_0, where RPTR/WPTR are 0x0083/0x0084.
        uint32_t ih_cntl_addr = 0, ih_rptr_addr = 0, ih_wptr_addr = 0;
        if (d.ip.resolve(r3gold::kIhRbCntl.ref(), &ih_cntl_addr) &&
            d.ip.resolve(r3gold::kIhRbRptr.ref(), &ih_rptr_addr) &&
            d.ip.resolve(r3gold::kIhRbWptr.ref(), &ih_wptr_addr)) {
            const uint32_t cntl = dump_rreg(st, ih_cntl_addr);
            const uint32_t rptr = dump_rreg(st, ih_rptr_addr);
            const uint32_t wptr = dump_rreg(st, ih_wptr_addr);
            const uint32_t rbsize = (cntl >> 1) & 0x1fu;
            const uint32_t mask = (rbsize > 0 && rbsize < 32) ? ((1u << rbsize) - 1u) : 0u;
            const uint32_t pending = (wptr - rptr) & mask;
            snprintf(text, sizeof text,
                     "dumper: IH snapshot rptr=%u wptr=%u pending=%u cntl=0x%08x "
                     "(read-only; rptr not advanced)",
                     rptr, wptr, pending, cntl);
            dump_line(ctx, st, evid, sizeof evid, &evid_used, text);
        } else {
            dump_line(ctx, st, evid, sizeof evid, &evid_used, "dumper: IH base unresolved");
        }
    }

    // The dump reads are appended after the survey exported its manifest; publish the completed
    // list so N48R3EVID-read-manifest also covers the failure-dump path.
    if (st && st->sink.set_property) {
        const size_t mlen = r3_manifest_format(&st->hw.manifest, st->hw.manifest_text,
                                               sizeof st->hw.manifest_text);
        if (mlen > 0)
            st->sink.set_property(st->sink.user, "N48R3EVID-read-manifest",
                                  st->hw.manifest_text, mlen);
        st->sink.set_property(st->sink.user, "N48R3EVID-failure-dump", evid, evid_used);
    }
}

// ---------------------------------------------------------------------------------------
// The ladder table
// ---------------------------------------------------------------------------------------
static const StageDef kStages[] = {
    {"survey", stage0_survey, nullptr},
    {"preflight", stage_preflight, nullptr},
    {"psp",  stage1_psp,  stage1_enabled},
    {"smu",  stage2_smu,  stage2_enabled},
    {"gfx",  stage3_gfx,  stage3_enabled},
    {"sdma", stage4_sdma, stage4_enabled},
    {"mes",  stage5_mes,  stage5_enabled},
};

} // namespace

const StageTable &r3_stage_table() {
    static const StageTable table = {kStages, (uint32_t)(sizeof kStages / sizeof kStages[0])};
    return table;
}

// The dump hook chosen by StageRunner. Stage 0 keeps the stage-1 snapshot; a stage-2 failure
// uses K9's per-substep dump (no IH, reads only).
void r3_dump_failure_state(StageCtx &ctx) {
    R3IntegrationCtx *ic = integ_ctx(ctx);
    if (ic && ic->failed) {
        R3DumpEnv de;
        de.io = &ic->io;
        de.sub = &ic->env;
        de.has_failed_ip = true;
        de.failed_ip = ic->failed_ip;
        r3_dump_failure_state(&de);
        return;
    }
    dump_failure_state_stage1(ctx);
}

void r3_stage0_set_depth(uint32_t depth) {
    const uint32_t clamped = r3_stage_depth_clamp(depth);
    if (depth != clamped) {
        IOLog("N48R3LOG stage0: depth %u out of range 0..%u; using 0 (T1)\n",
              depth, R3_STAGE0_MAX_DEPTH);
    }
    g_stage0_depth = clamped;
}

uint32_t r3_stage0_depth(void) {
    return g_stage0_depth;
}
