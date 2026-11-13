/*
 * rdna3_integration.cpp - see rdna3_integration.h. Pure registration/gating/formatting.
 *
 * Register lists are the ones the substep tests prove complete (test_sdma_msg.cpp:366-381,
 * test_mes_api.cpp:464-518, test_psp_msg.cpp:575-578, test_smu_msg.cpp:335-339). Keeping
 * them in one place is deliberate: the production path and the host tests must describe
 * the same whitelist, otherwise a green test says nothing about the kext (K7 R1).
 *
 * Address spaces (the two-space trap): R3Grant::start is a BAR0 BYTE offset; writeguard
 * layout grants use DWORDS (writeguard.h), so plan grants are registered as start>>2 and
 * size/4. Register windows are already absolute BAR5 dwords.
 */
#include "rdna3_integration.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "rdna3_ip_entries.h"
#include "common/StageRunner.h"
#include "rdna3_dump.h"
#include "rdna3_writeguard.h"

namespace {
using amdgpu::IPBaseTable;
using amdgpu::IPBlock;




constexpr const char *kTagLayoutPsp = "layout:psp";
constexpr const char *kTagLayoutSdma = "layout:sdma";
constexpr const char *kTagLayoutMes = "layout:mes";
constexpr const char *kTagRegPsp = "reg:psp";
constexpr const char *kTagRegSmu = "reg:smu";
constexpr const char *kTagRegSdma = "reg:sdma";
constexpr const char *kTagRegMes = "reg:mes";



/* Step2: resolves an R3WriteRowReg declaration table row. Golden entries resolve through
 * IPBaseTable::resolve(), fallback rows use explicit block/base_idx/off. */

bool row_addr(const IPBaseTable *ip, const R3WriteRowReg &r, uint32_t *addr)
{
    if (!ip || !addr)
        return false;
    if (r.golden)
        return ip->resolve(static_cast<const r3gold::GoldenReg *>(r.golden)->ref(), addr);
    const IPBlock block = static_cast<IPBlock>(r.block);
    if (!ip->isResolved(block, r.base_idx))
        return false;
    *addr = ip->getBase(block, r.base_idx) + r.off;
    return true;
}

/* PSP: MP0 C2PMSG group (psp_v13_0.cpp reg_write sites; mp_13_0_4 _BASE_IDX 1).
 * Step2: iterates the declaration table from psp_v13_0.cpp. */
uint32_t register_psp(const R3LayoutPlan *plan, const IPBaseTable *ip)
{
    uint32_t layout_rows = 0;
    const uint32_t lc = amdgpu::getPspLayoutCount();
    const R3WriteRowLayout *layouts = amdgpu::getPspLayouts();

    for (uint32_t i = 0; i < lc; i++) {
        const R3Grant *g = r3_layout_grant(plan, layouts[i].tag);
        if (!g)
            continue;
        if (r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                  static_cast<uint32_t>(g->size >> 2),
                                  layouts[i].owner, kTagLayoutPsp) != 0)
            return 0u;
        layout_rows++;
    }

    uint32_t n = 0;
    const uint32_t rc = amdgpu::getPspRowCount();
    const R3WriteRowReg *rows = amdgpu::getPspRows();

    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr = 0;
        if (row_addr(ip, rows[i], &addr) &&
            r3_wg_register_reg(addr, rows[i].dwords, rows[i].cls,
                               WSubstep::Psp, kTagRegPsp) == 0)
            n++;
    }
    return layout_rows + n;
}

/* SMU: MP1 mailbox (smu_v13_0.h:88-91; BASE_IDX 1). Step2: declaration table. */
uint32_t register_smu(const IPBaseTable *ip)
{
    uint32_t n = 0;
    const uint32_t rc = amdgpu::getSmuRowCount();
    const R3WriteRowReg *rows = amdgpu::getSmuRows();

    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr = 0;
        if (row_addr(ip, rows[i], &addr) &&
            r3_wg_register_reg(addr, rows[i].dwords, rows[i].cls,
                               WSubstep::Smu, kTagRegSmu) == 0)
            n++;
    }
    return n;
}

/* SDMA: layout:sdma + 17 reg rows. Step2: declaration table. */
uint32_t register_sdma(const R3LayoutPlan *plan, const IPBaseTable *ip)
{
    uint32_t n = 0;
    const R3Grant *g = r3_layout_grant(plan, amdgpu::getSdmaLayoutTag());
    if (g && r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                   static_cast<uint32_t>(g->size >> 2),
                                   WSubstep::Sdma, kTagLayoutSdma) == 0)
        n++;

    const uint32_t rc = amdgpu::getSdmaRowCount();
    const R3WriteRowReg *rows = amdgpu::getSdmaRows();

    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr = 0;
        if (row_addr(ip, rows[i], &addr) &&
            r3_wg_register_reg(addr, rows[i].dwords, rows[i].cls,
                               WSubstep::Sdma, kTagRegSdma) == 0)
            n++;
    }
    return n;
}

/* MES: layout:mes + 53 reg rows (S-1 hub 29 + seq 24) + 2 doorbells = 1+55. */
uint32_t register_mes(const R3LayoutPlan *plan, const IPBaseTable *ip)
{
    uint32_t n = 0;
    const R3Grant *g = r3_layout_grant(plan, amdgpu::getMesLayoutTag());
    if (g && r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                   static_cast<uint32_t>(g->size >> 2),
                                   WSubstep::Mes, kTagLayoutMes) == 0)
        n++;

    const uint32_t rc = amdgpu::getMesRowCount();
    const R3WriteRowReg *rows = amdgpu::getMesRows();

    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr = 0;
        if (row_addr(ip, rows[i], &addr) &&
            r3_wg_register_reg(addr, rows[i].dwords, rows[i].cls,
                               WSubstep::Mes, kTagRegMes) == 0)
            n++;
    }
    /* Doorbells are 64-bit stores at index*4 bytes; the guard matches dwords, so 2.
     * Registered from the MES doorbell declaration table (single source of truth). */
    const uint32_t dc = amdgpu::getMesDoorbellCount();
    const R3WriteRowAbs *dbs = amdgpu::getMesDoorbells();
    for (uint32_t i = 0; i < dc; i++) {
        if (r3_wg_register_reg(dbs[i].addr, dbs[i].dwords, dbs[i].cls,
                               WSubstep::Mes, "reg:mes-db") == 0)
            n++;
    }
    return n;
}

/* Step2: kExpectedRows=87 (MES 53 regs + 2 doorbells = 55; GFX=0).
 * PSP(5+5) + SMU(3) + SDMA(1+17) + MES(1+55) = 10+3+18+56 = 87 */
constexpr uint32_t kExpectedRows = 5u + 5u + 3u + 1u + 17u + 1u + 55u;
} /* namespace */



uint32_t r3_integration_enabled(const R3GateInputs *in)
{
    if (!in)
        return 0u;
    const uint32_t enabled = r3_gate_enabled(in->stage, in->kill_mask, in->latch_mask);
    /* R3GateIp and R3Ip share the same order by construction (psp=0..mes=4); the
     * static_asserts keep that true if either enum moves. */
    static_assert(static_cast<uint32_t>(R3Ip::Psp) == static_cast<uint32_t>(kR3GatePsp),
                  "R3Ip/R3GateIp bit order drift");
    static_assert(static_cast<uint32_t>(R3Ip::Mes) == static_cast<uint32_t>(kR3GateMes),
                  "R3Ip/R3GateIp bit order drift");
    static_assert(R3_IP_COUNT == static_cast<uint32_t>(kR3GateIpCount),
                  "R3Ip/R3GateIp count drift");
    return enabled & ((1u << R3_IP_COUNT) - 1u);
}

bool r3_integration_ip_enabled(const R3GateInputs *in, R3Ip ip)
{
    if (!in)
        return false;
    return (r3_integration_enabled(in) & R3_IP_BIT(ip)) != 0u;
}

R3TriggerDecision r3_trigger_admission(bool provisioned, bool gate_ok, bool prep_ok,
                                       bool io_ok, uint32_t stage, uint32_t substep_stage,
                                       uint32_t table_count)
{
    R3TriggerDecision d = { R3TriggerVerdict::NotArmed, "not-armed", 0u, 0u };

    if (provisioned) {
        d.verdict = R3TriggerVerdict::AlreadyProvisioned;
        d.reason = "already-provisioned-this-boot";
        return d;
    }
    if (table_count == 0u || substep_stage >= table_count) {
        d.verdict = R3TriggerVerdict::NoSubsteps;
        d.reason = "no-substep-stage";
        return d;
    }
    if (stage == 0u) {
        // F1: n48r3-stage=0 is the read-only ladder; nothing was armed to fire.
        d.verdict = R3TriggerVerdict::NoSubsteps;
        d.reason = "stage-0-arms-no-substep";
        return d;
    }
    if (!gate_ok) {
        // C0.9/E5: admission or the preflight gate stopped the boot ladder.
        d.verdict = R3TriggerVerdict::NotArmed;
        d.reason = "boot-gate-not-passed";
        return d;
    }
    if (!prep_ok) {
        // L8/D-05: no plan/whitelist means the first write would be a DENY.
        d.verdict = R3TriggerVerdict::NotArmed;
        d.reason = "prep-not-ok";
        return d;
    }
    if (!io_ok) {
        // The substeps' entry check (r3_io_complete) would fail before the first step; keep
        // the refusal at the gate so the boot status and the evidence agree (2026-11-05 R1).
        d.verdict = R3TriggerVerdict::NotArmed;
        d.reason = "io-incomplete";
        return d;
    }

    const uint32_t last = table_count - 1u;
    const uint32_t target = stage >= last ? last : stage + 1u;
    if (target < substep_stage) {
        d.verdict = R3TriggerVerdict::NoSubsteps;
        d.reason = "target-below-substeps";
        return d;
    }

    d.verdict = R3TriggerVerdict::Ok;
    d.reason = "ok";
    d.first_stage = substep_stage;
    d.target_stage = target;
    return d;
}

uint32_t r3_integration_register_row_count(void)
{
    return kExpectedRows;
}

int r3_integration_register_production(const R3LayoutPlan *plan,
                                       const void *ip_base_table)
{
    if (!plan || !ip_base_table)
        return -1;
    const IPBaseTable *ip = static_cast<const IPBaseTable *>(ip_base_table);

    /* A zero return from a section means "a row was rejected" or "a base needed by that
     * section is unresolved". Both are fatal before the first power-up write. */
    if (register_psp(plan, ip) != 5u + 5u)
        return -1;
    if (register_smu(ip) != 3u)
        return -1;
    if (register_sdma(plan, ip) != 1u + 17u)
        return -1;
    if (register_mes(plan, ip) != 1u + 55u)
        return -1;
    return 0;
}

void r3_integration_bind_env(R3IntegrationCtx *ctx, const void *ip_base_table,
                             uint64_t fb_mc_base)
{
    if (!ctx)
        return;
    ctx->env.rt = &ctx->rt;
    ctx->env.ev = &ctx->ev;
    ctx->env.sink = &ctx->sink;
    ctx->env.fb_mc_base = fb_mc_base;
    ctx->env.ip = ip_base_table;
    ctx->env.psp_version_pre = ctx->psp_version_pre;
    ctx->env.psp_version_pre_ok = ctx->psp_version_pre_ok;
}

/* §15.1 item 4 (S12): extract the PSP version pre from the preflight probe readings and
 * sync it into env. This is the ONE place the extraction + sync happen; stage_preflight
 * calls it at its tail and host tests exercise the same pure entry, so deleting the call
 * in stage_preflight is caught by the host runner (the stage table itself is kext-only). */
void r3_stages_preflight_sync_env(R3IntegrationCtx *ctx,
                                  const R3PreflightReading *readings, uint32_t probe_count,
                                  const R3PreflightResult *res)
{
    if (!ctx || !readings || !res)
        return;
    ctx->psp_version_pre = 0u;
    ctx->psp_version_pre_ok = false;
    for (uint32_t i = 0; i < probe_count && i < res->count; i++) {
        if (r3_preflight_match(readings[i].reg, &r3gold::kMp0SmnC2pmsg58)) {
            ctx->psp_version_pre = readings[i].value;
            ctx->psp_version_pre_ok = res->items[i].verdict == R3PreflightVerdict::Ok;
            break;
        }
    }
    r3_integration_sync_preflight_env(ctx);
}

void r3_integration_note_outcome(R3IntegrationCtx *ctx, R3Ip ip, bool ran,
                                 const R3SubstepResult *result)
{
    if (!ctx)
        return;
    if (ctx->outcome_count >= R3_INTEG_MAX_OUTCOMES)
        return;
    R3IpOutcome &o = ctx->outcomes[ctx->outcome_count++];
    o.ip = ip;
    o.ran = ran;
    if (ran && result)
        o.result = *result;
    else
        o.result = r3_substep_fail(kR3SecPre, 0, "not-run");
    o.ready = (ctx->rt.ready_bits & R3_IP_BIT(ip)) ? 1u : 0u;
}

R3GateInputs r3_integration_gate_inputs(const R3IntegrationCtx *ctx)
{
    R3GateInputs g;
    g.stage = ctx ? ctx->stage : 0u;
    g.kill_mask = ctx ? ctx->kill_mask : 0u;
    g.latch_mask = ctx ? ctx->rt.latch_bits : 0u;
    return g;
}

namespace {

/* DCN 3.1.4 offsets (BASE_IDX 2). HUBP instance stride is 0x100; cursor stride 0x20. */
constexpr uint32_t kHubpDchubpCntl = 0x05f3u;
constexpr uint32_t kHubpViewportDim = 0x05eau;
constexpr uint32_t kHubpreqSurfacePitch = 0x0607u;
constexpr uint32_t kHubpreqSurfaceAddr = 0x060au;
constexpr uint32_t kHubpreqSurfaceAddrHi = 0x060bu;
constexpr uint32_t kCursorControl = 0x0678u;
constexpr uint32_t kCursorSurfaceAddr = 0x0679u;
constexpr uint32_t kCursorSurfaceAddrHi = 0x067au;
constexpr uint32_t kCursorSize = 0x067bu;
constexpr uint32_t kHubpStride = 0x100u;
constexpr uint32_t kCursorStride = 0x20u;
/* HUBP0_DCHUBP_CNTL.SURFACE_ENABLE is bit 8 (dcn_3_1_4_sh_mask.h). */
constexpr uint32_t kHubpSurfaceEnable = 1u << 8;
constexpr uint32_t kCursorEnable = 1u;         /* CURSOR_CONTROL.CURSOR_ENABLE bit 0 */
constexpr uint32_t kPitchMask = 0x00003FFFu;
constexpr uint32_t kHeightShift = 16u;
constexpr uint32_t kHeightMask = 0x3FFFu;

uint64_t surface_end(uint64_t addr, uint32_t pitch, uint32_t height)
{
    if (pitch == 0u || height == 0u)
        return 0ull;
    return addr + static_cast<uint64_t>(pitch) * height;
}

} /* namespace */

bool r3_integration_collect_scanout(uint32_t dcn_seg2_base, R3Read32Fn read, void *user,
                                    uint64_t fb_mc_base, uint64_t real_vram_size,
                                    R3ScanoutReading *out)
{
    if (!read || !out)
        return false;
    R3ScanoutReading r {};
    uint32_t v = 0;

    /* MC form -> carveout offset (tech plan §4.1). The layout consumes byte offsets, so the
     * raw surface address must lose its FB base before it feeds L1. A surface that starts
     * below the carveout or ends past its real size is outside the aperture the layout knows
     * about: fail closed instead of producing a guard that covers nothing. */
    const auto normalize = [&](uint64_t mc_addr, uint64_t end_mc) -> bool {
        if (mc_addr < fb_mc_base || end_mc > fb_mc_base + real_vram_size)
            return false;
        return true;
    };

    for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t hubp = dcn_seg2_base + i * kHubpStride;
        if (!read(user, hubp + kHubpDchubpCntl, &v))
            return false;
        if ((v & kHubpSurfaceEnable) == 0u)
            continue;

        uint32_t dim = 0, pitch = 0, lo = 0, hi = 0;
        if (!read(user, hubp + kHubpViewportDim, &dim) ||
            !read(user, hubp + kHubpreqSurfacePitch, &pitch) ||
            !read(user, hubp + kHubpreqSurfaceAddr, &lo) ||
            !read(user, hubp + kHubpreqSurfaceAddrHi, &hi))
            return false;
        const uint32_t height = (dim >> kHeightShift) & kHeightMask;
        const uint64_t addr_mc = (static_cast<uint64_t>(hi) << 32) | lo;
        const uint64_t end_mc = surface_end(addr_mc, pitch & kPitchMask, height);
        if (!normalize(addr_mc, end_mc))
            return false;
        const uint64_t end = end_mc - fb_mc_base;
        if (end > r.gop_end)
            r.gop_end = end;
        r.surfaces++;
    }

    for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t cur = dcn_seg2_base + i * kCursorStride;
        if (!read(user, cur + kCursorControl, &v))
            return false;
        if ((v & kCursorEnable) == 0u)
            continue;
        uint32_t lo = 0, hi = 0, size = 0;
        if (!read(user, cur + kCursorSurfaceAddr, &lo) ||
            !read(user, cur + kCursorSurfaceAddrHi, &hi) ||
            !read(user, cur + kCursorSize, &size))
            return false;
        const uint64_t addr_mc = (static_cast<uint64_t>(hi) << 32) | lo;
        /* CURSOR_SIZE: width 6:0 (+1), height 22:16 (+1), in pixels, 4 B/px. */
        const uint32_t w = (size & 0x7fu) + 1u;
        const uint32_t h = ((size >> 16) & 0x7fu) + 1u;
        const uint64_t end_mc = addr_mc + static_cast<uint64_t>(w) * h * 4ull;
        if (!normalize(addr_mc, end_mc))
            return false;
        const uint64_t end = end_mc - fb_mc_base;
        if (end > r.gop_end)
            r.gop_end = end;
        r.cursors++;
    }

    *out = r;
    return true;
}

bool r3_io_mmio_in_range(const R3IoWindow *w, uint32_t dword)
{
    if (!w || w->bar5_bytes < 4u)
        return false;
    const uint64_t off = static_cast<uint64_t>(dword) * 4ull;
    return off + 4ull <= w->bar5_bytes;
}

bool r3_io_bus_in_range(const R3IoWindow *w, uint32_t bar, uint64_t byte_off,
                        uint32_t dwords)
{
    if (!w || (dwords != 1u && dwords != 2u) || (byte_off & 3ull) != 0ull)
        return false;
    if (dwords == 2u && (byte_off & 7ull) != 0ull)
        return false;
    uint64_t bytes = 0;
    if (bar == R3_IO_BAR_VRAM)
        bytes = w->bar0_bytes;
    else if (bar == 2u)
        bytes = w->bar2_bytes;
    else
        return false;
    if (bytes < 4u)
        return false;
    return byte_off + static_cast<uint64_t>(dwords) * 4ull <= bytes;
}


size_t r3_integration_format_enabled(const R3GateInputs *in, char *out, size_t cap)
{
    if (!out || cap == 0u)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;
    const uint32_t e = r3_integration_enabled(in);
    const int n = snprintf(out, cap,
                           "stage=%u kill=0x%x latch=0x%x psp=%u smu=%u gfx=%u sdma=%u "
                           "mes=%u\n",
                           in->stage, in->kill_mask, in->latch_mask,
                           (e & R3_IP_BIT(R3Ip::Psp)) ? 1u : 0u,
                           (e & R3_IP_BIT(R3Ip::Smu)) ? 1u : 0u,
                           (e & R3_IP_BIT(R3Ip::Gfx)) ? 1u : 0u,
                           (e & R3_IP_BIT(R3Ip::Sdma)) ? 1u : 0u,
                           (e & R3_IP_BIT(R3Ip::Mes)) ? 1u : 0u);
    return n > 0 ? static_cast<size_t>(n) : 0u;
}

size_t r3_integration_format_summary(uint32_t stage, uint32_t effective_stage,
                                     uint32_t ready_bits, uint32_t latch_bits,
                                     uint64_t deny_count,
                                     const R3IpOutcome *outcomes, uint32_t count,
                                     char *out, size_t cap)
{
    if (!out || cap == 0u)
        return 0;
    out[0] = '\0';

    size_t used = 0;
    int n = snprintf(out, cap, "stage=%u effective=%u ready=0x%x latch=0x%x deny=%llu\n",
                     stage, effective_stage, ready_bits, latch_bits,
                     static_cast<unsigned long long>(deny_count));
    if (n <= 0)
        return 0;
    used = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1u;

    for (uint32_t i = 0; i < count && used + 1u < cap; i++) {
        const R3IpOutcome &o = outcomes[i];
        const char *name = o.ran ? (o.result.name ? o.result.name : "?") : "not-run";
        const char *sec = o.ran ? r3_substep_section_name(o.result.section) : "-";
        n = snprintf(out + used, cap - used,
                     "ip=%s done=%s name=%s section=%s step=%u ready=%u\n",
                     r3_dump_ip_name(o.ip), o.ready ? "yes" : "no", name, sec,
                     o.ran ? o.result.step : 0u, o.ready ? 1u : 0u);
        if (n <= 0)
            break;
        if (static_cast<size_t>(n) >= cap - used) {
            used = cap - 1u;
            break;
        }
        used += static_cast<size_t>(n);
    }
    return used;
}

/* --- D-02 preflight gate (pure) ------------------------------------------------------- */

namespace {

/* One canary per segment the five substeps and the layout consume, the two MMHUB FB value
 * gates and the MP0 SOS-alive gate. C2PMSG_58 records the PSP version so the D3 baseline
 * is rebuilt from a correctly-addressed read (v1.4). */
const r3gold::GoldenReg *const kPreflightProbes[] = {
    &r3gold::kMp0SmnC2pmsg81,
    &r3gold::kMp0SmnC2pmsg58,
    &r3gold::kMp1SmnC2pmsg90,
    &r3gold::kGrbmStatus,
    &r3gold::kRlcGpmStat,
    &r3gold::kSdma0StatusReg,
    &r3gold::kSdma0F32Cntl,
    &r3gold::kGcmcVmFbOffset,
    &r3gold::kMmmcVmFbOffset,
    &r3gold::kMmmcVmFbLocationBase,
    &r3gold::kMmmcVmFbLocationTop,
};

constexpr uint32_t kPreflightProbeCount =
    sizeof kPreflightProbes / sizeof kPreflightProbes[0];
static_assert(kPreflightProbeCount <= R3_PREFLIGHT_MAX,
              "preflight probe set exceeds R3_PREFLIGHT_MAX");

} /* namespace */

uint32_t r3_preflight_probe_count(void)
{
    return kPreflightProbeCount;
}

const r3gold::GoldenReg *r3_preflight_probe(uint32_t index)
{
    return index < kPreflightProbeCount ? kPreflightProbes[index] : nullptr;
}

void r3_preflight_evaluate(const R3PreflightReading *readings, uint32_t count,
                           R3PreflightResult *out)
{
    if (!out)
        return;
    out->ok = true;
    out->count = 0;
    if (!readings) {
        out->ok = false;
        return;
    }
    if (count > R3_PREFLIGHT_MAX) {
        out->ok = false;
        count = R3_PREFLIGHT_MAX;
    }

    for (uint32_t i = 0; i < count; i++) {
        const R3PreflightReading &r = readings[i];
        R3PreflightItem &it = out->items[out->count++];
        it.name = (r.reg && r.reg->name) ? r.reg->name : "?";
        it.addr = r.addr;
        it.value = r.value;

        /* Layer 1 (completion): the bus read must finish and must not return the
         * 0xFFFFFFFF "unreadable" sentinel (upstream probe precedent). */
        if (!r.reg || !r.read_ok) {
            it.verdict = R3PreflightVerdict::ReadFail;
            out->ok = false;
            continue;
        }
        if (r.value == 0xFFFFFFFFu) {
            it.verdict = R3PreflightVerdict::AllOnes;
            out->ok = false;
            continue;
        }

        /* Layer 2 (value gate): only registers whose golden entry declares a gate. */
        it.verdict = R3PreflightVerdict::Ok;
        switch (r.reg->gate) {
        case r3gold::Gate::RecordOnly:
            break;
        case r3gold::Gate::NonZero:
            if (r.value == 0u) {
                it.verdict = R3PreflightVerdict::ValueMismatch;
                out->ok = false;
            }
            break;
        case r3gold::Gate::Equals:
            if ((r.value & r.reg->mask) != r.reg->baseline) {
                it.verdict = R3PreflightVerdict::ValueMismatch;
                out->ok = false;
            }
            break;
        default:
            it.verdict = R3PreflightVerdict::ValueMismatch;
            out->ok = false;
            break;
        }
    }
}

bool r3_integration_should_prepare(bool survey_ok, uint32_t stage)
{
    return survey_ok && stage >= 1u;
}

void r3_integration_note_prep_failure(R3IntegrationCtx *ctx, const char *reason)
{
    if (!ctx || !reason || reason[0] == '\0')
        return;
    if (!ctx->prep_fail) {
        ctx->prep_fail = reason;
        const int n = snprintf(ctx->prep_chain, sizeof ctx->prep_chain, "%s", reason);
        if (n < 0)
            ctx->prep_chain[0] = '\0';
        else if (static_cast<size_t>(n) >= sizeof ctx->prep_chain)
            ctx->prep_chain[sizeof ctx->prep_chain - 1u] = '\0';
        return;
    }
    const size_t used = strlen(ctx->prep_chain);
    if (used + 1u < sizeof ctx->prep_chain)
        snprintf(ctx->prep_chain + used, sizeof ctx->prep_chain - used, " -> %s", reason);
}
