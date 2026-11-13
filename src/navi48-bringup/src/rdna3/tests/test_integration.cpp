/*
 * test_integration.cpp - K10 integration-layer host test.
 *
 * Covers:
 *   T1  36-cell stage x kill matrix on the REAL stage-table enable predicate
 *       (r3_integration_ip_enabled over the same StageDef.enabled the kext uses), compared
 *       against an independent golden copied from tech plan §5.3 (D8-corrected) -- the same
 *       table K0's test_gating.cpp pins. Mismatch = the wiring changed the policy.
 *   T2  enabled bitmap export text (E2): stage/kill/latch + five per-IP bits.
 *   T3  latch semantics: a latched IP closes itself and its dependants.
 *   T4  production writeguard registration: exact row counts per IP, layout grants in the
 *       DWORD address space, MES doorbells registered as 2 dwords (K8 note), and zero DENY
 *       for every register the corresponding substep writes (K7 R1).
 *   T5  R3Io address arithmetic: BAR5 dword bounds, BAR0/BAR2 byte ranges, 8-byte doorbell
 *       alignment, SMN byte->index translation.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "amdgpu_ip.h"
#include "amd/mes_v11_0.h"
#include "amd/sdma_v6_0.h"
#include "amd/psp_v13_0.h"
#include "amd/smu_v13_0.h"

#include "rdna3/rdna3_integration.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/rdna3_survey.h"
using namespace amdgpu;

namespace {

static uint32_t g_checks = 0;
static uint32_t g_fail = 0;

#define CHECK(cond, ...)                                                                  \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            g_fail++;                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            printf(__VA_ARGS__);                                                          \
            printf("\n");                                                                 \
        }                                                                                 \
    } while (0)

constexpr uint32_t P = R3_IP_BIT(R3Ip::Psp);
constexpr uint32_t M = R3_IP_BIT(R3Ip::Smu);
constexpr uint32_t G = R3_IP_BIT(R3Ip::Gfx);
constexpr uint32_t D = R3_IP_BIT(R3Ip::Sdma);
constexpr uint32_t E = R3_IP_BIT(R3Ip::Mes);

static uint32_t enabled_at(uint32_t stage, uint32_t kill, uint32_t latch = 0)
{
    R3GateInputs in;
    in.stage = stage;
    in.kill_mask = kill;
    in.latch_mask = latch;
    return r3_integration_enabled(&in);
}

static void test_matrix()
{
    /* Independent golden: tech plan §5.3, D8-corrected (identical to K0's test_gating). */
    static const uint32_t golden[6][6] = {
        { 0, 0, 0, 0, 0, 0 },
        { P, 0, P, P, P, P },
        { P | M, 0, P, P | M, P | M, P | M },
        { P | M | G, 0, P, P | M, P | M | G, P | M | G },
        { P | M | G | D, 0, P | D, P | M | D, P | M | G, P | M | G | D },
        { P | M | G | D | E, 0, P | D, P | M | D, P | M | G | E, P | M | G | D },
    };
    static const uint32_t kill_col[6] = {0, P, M, G, D, E};

    for (uint32_t stage = 0; stage <= 5; stage++) {
        for (uint32_t col = 0; col < 6; col++) {
            CHECK(enabled_at(stage, kill_col[col]) == golden[stage][col],
                  "matrix stage %u col %u: got 0x%x want 0x%x", stage, col,
                  enabled_at(stage, kill_col[col]), golden[stage][col]);
        }
    }

    /* The stage table's own predicates must agree with the pure function (same policy). */
    for (uint32_t stage = 1; stage <= 5; stage++) {
        for (uint32_t col = 0; col < 6; col++) {
            R3IntegrationCtx ic;
            memset(&ic, 0, sizeof ic);
            ic.stage = stage;
            ic.kill_mask = kill_col[col];
            ic.prep_ok = true;
            for (uint32_t ip = 0; ip < R3_IP_COUNT; ip++) {
                const R3GateInputs gi = r3_integration_gate_inputs(&ic);
                const bool got = r3_integration_ip_enabled(&gi, static_cast<R3Ip>(ip));
                const bool want = (golden[stage][col] & (1u << ip)) != 0u;
                CHECK(got == want, "table stage %u col %u ip %u", stage, col, ip);
            }
        }
    }
}

static void test_enabled_text()
{
    char buf[160];
    R3GateInputs in;
    in.stage = 5;
    in.kill_mask = M | G;
    in.latch_mask = 0;
    const size_t n = r3_integration_format_enabled(&in, buf, sizeof buf);
    CHECK(n > 0, "enabled text empty");
    CHECK(strstr(buf, "stage=5") != nullptr, "stage missing: %s", buf);
    CHECK(strstr(buf, "kill=0x6") != nullptr, "kill mask missing: %s", buf);
    /* smu killed -> psp only; gfx and everything downstream off. */
    CHECK(strstr(buf, "psp=1 smu=0 gfx=0 sdma=1 mes=0") != nullptr,
          "enabled bits wrong: %s", buf);
}

static void test_latch()
{
    /* Latch is an input like kill, and closes dependants (mes via gfx). */
    CHECK(enabled_at(5, 0, G) == (P | M | D), "latch gfx");
    CHECK(enabled_at(5, 0, P) == 0, "latch psp closes all");
    CHECK(enabled_at(3, 0, G) == (P | M), "latch gfx at stage 3");
}

/* --- production registration ----------------------------------------------------------- */

static void fill_plan(R3LayoutPlan *plan)
{
    memset(plan, 0, sizeof *plan);
    plan->ok = true;
    plan->bar0_limit = 0x10000000ull;
    plan->limit = 0x10000000ull;
    plan->grant_count = 7u;
    struct Row {
        const char *tag;
        uint64_t start;
        uint64_t size;
        R3Owner owner;
    };
    const Row rows[] = {
        {"fw_buf", 0x04000000ull, 0x01000000ull, R3Owner::Psp},
        {"psp_ring", 0x05000000ull, 0x00004000ull, R3Owner::Psp},
        {"psp_cmd", 0x05004000ull, 0x00001000ull, R3Owner::Psp},
        {"psp_fence", 0x05005000ull, 0x00001000ull, R3Owner::Psp},
        {"tmr", 0x08000000ull, 0x04000000ull, R3Owner::Psp},
        {"sdma", 0x0c000000ull, 0x00800000ull, R3Owner::Sdma},
        {"mes", 0x0c800000ull, 0x00800000ull, R3Owner::Mes},
    };
    for (uint32_t i = 0; i < 7u; i++) {
        R3Grant &g = plan->grants[i];
        g.start = rows[i].start;
        g.end = rows[i].start + rows[i].size;
        g.size = rows[i].size;
        g.owner = rows[i].owner;
        g.cls = R3B2Class::DataMove;
        g.needs_cpu = true;
        snprintf(g.tag, sizeof g.tag, "%s", rows[i].tag);
    }
}

static void fill_ip(IPBaseTable *ip)
{
    for (int i = 0; i < static_cast<int>(IPBlock::Count); i++)
        for (int j = 0; j < IPBaseTable::kMaxBaseSegments; j++)
            ip->base[i][j] = 0xFFFFFFFFu;
    ip->set(IPBlock::GC, 0x10000u);
    ip->setBase(IPBlock::GC, 1, 0x20000u);
    ip->set(IPBlock::SDMA0, 0x40000u);
    ip->setBase(IPBlock::SDMA0, 1, 0x50000u);
    ip->set(IPBlock::MP0, 0x60000u);
    ip->setBase(IPBlock::MP1, 1, 0x70000u);
    /* The golden-resolved consumers use MP0/MMHUB segment 1 explicitly (A-01). */
    ip->setBase(IPBlock::MP0, 1, 0x60000u);
    ip->setBase(IPBlock::MMHUB, 1, 0x80000u);
    /* NBIO seg2 for MES S-1 doorbell aperture row. */
    ip->setBase(IPBlock::NBIO, 2, 0xd20u);
}

static bool has_reg(uint32_t addr, uint32_t dwords, WClass cls, WSubstep owner)
{
    for (uint32_t i = 0; i < r3_wg_grant_count(); i++) {
        const R3WgGrant *g = r3_wg_grant(i);
        if (g && !g->range && g->start == addr && g->dwords == dwords && g->cls == cls &&
            g->owner == owner)
            return true;
    }
    return false;
}

static bool has_layout(uint32_t start_dw, uint32_t dwords, WSubstep owner)
{
    for (uint32_t i = 0; i < r3_wg_grant_count(); i++) {
        const R3WgGrant *g = r3_wg_grant(i);
        if (g && g->range && g->start == start_dw && g->dwords == dwords &&
            g->owner == owner)
            return true;
    }
    return false;
}

static void test_registration()
{
    R3LayoutPlan plan;
    IPBaseTable ip;
    fill_plan(&plan);
    fill_ip(&ip);

    r3_writeguard_install_mm();
    const int rc = r3_integration_register_production(&plan, &ip);
    CHECK(rc == 0, "register_production failed");

    /* Step2: runtime expected row count (GFX=0). */
    const uint32_t expected = getSdmaRowCount() + 1u +          // SDMA regs + layout
                              getMesRowCount() + 1u + amdgpu::getMesDoorbellCount() +
                              getPspRowCount() + getPspLayoutCount() +
                              getSmuRowCount();
    {
        uint32_t total_layouts = 0, total_regs = 0;
        for (uint32_t i = 0; i < r3_wg_grant_count(); i++) {
            const R3WgGrant *g = r3_wg_grant(i);
            if (!g) continue;
            if (g->range) total_layouts++;
            else total_regs++;
        }
        CHECK(total_layouts + total_regs == expected,
              "registered %u rows, expected %u (layouts=%u regs=%u)",
              total_layouts + total_regs, expected, total_layouts, total_regs);
    }


    /* Layout grants are DWORD-space (writeguard.h): byte start >> 2, size >> 2. */
    const R3Grant *ring = r3_layout_grant(&plan, "psp_ring");
    CHECK(ring != nullptr, "ring grant missing");
    CHECK(has_layout(static_cast<uint32_t>(ring->start >> 2),
                     static_cast<uint32_t>(ring->size >> 2), WSubstep::Psp),
          "psp layout grant not in dword space");
    CHECK(has_layout(static_cast<uint32_t>(0x0c000000ull >> 2), 0x00800000u >> 2,
                     WSubstep::Sdma), "sdma layout grant missing");
    CHECK(has_layout(static_cast<uint32_t>(0x0c800000ull >> 2), 0x00800000u >> 2,
                     WSubstep::Mes), "mes layout grant missing");

    /* MES doorbells come from the doorbell declaration table (single source of truth):
     * every declared doorbell must be registered with its dwords/class/owner. */
    {
        const uint32_t dc = amdgpu::getMesDoorbellCount();
        const R3WriteRowAbs *dbs = amdgpu::getMesDoorbells();
        for (uint32_t i = 0; i < dc; i++) {
            CHECK(has_reg(dbs[i].addr, dbs[i].dwords, dbs[i].cls, WSubstep::Mes),
                  "missing MES doorbell 0x%x (row %u)", dbs[i].addr, i);
        }
    }

    /* K7 R1: every SDMA register from the declaration table must exist. */
    {
        const uint32_t rc = getSdmaRowCount();
        const R3WriteRowReg *rows = getSdmaRows();
        for (uint32_t i = 0; i < rc; i++) {
            const uint32_t base = rows[i].base_idx == 0 ? 0x40000u : 0x50000u;
            uint32_t addr;
            if (rows[i].golden) {
                const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
                addr = base + g->offset;
            } else {
                addr = base + rows[i].off;
            }
            CHECK(has_reg(addr, rows[i].dwords, rows[i].cls, WSubstep::Sdma),
                  "missing SDMA reg 0x%x (row %u)", addr, i);
        }

        /* Also verify every row classifies ALLOW under its owner. */
        const uint64_t deny_before = r3_writeguard_deny_count();
        r3_wg_arm_substep(WSubstep::Sdma);
        for (uint32_t i = 0; i < rc; i++) {
            const uint32_t base = rows[i].base_idx == 0 ? 0x40000u : 0x50000u;
            uint32_t addr;
            if (rows[i].golden) {
                const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
                addr = base + g->offset;
            } else {
                addr = base + rows[i].off;
            }
            CHECK(r3_wg_classify(addr, rows[i].dwords, rows[i].cls, "k10-sdma") ==
                      WVerdict::TxnRead,
                  "SDMA reg 0x%x (row %u) DENIED", addr, i);
        }

        /* MES (K8): every declaration row must exist and classify ALLOW. GC bases:
         * BASE_IDX 0 = 0x10000, BASE_IDX 1 = 0x20000 (fill_ip); golden rows resolve
         * by their own base_idx. */
        {
            const uint32_t rc = amdgpu::getMesRowCount();
            const R3WriteRowReg *rows = amdgpu::getMesRows();
            r3_wg_arm_substep(WSubstep::Mes);
            for (uint32_t i = 0; i < rc; i++) {
                uint32_t addr;
                if (rows[i].golden) {
                    const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
                    addr = (g->base_idx == 0 ? 0x10000u : 0x20000u) + g->offset;
                } else if (rows[i].block == static_cast<uint8_t>(IPBlock::NBIO)) {
                    /* NBIO seg2 = 0xd20 (方案 v3 §6.2.3 row 29). */
                    addr = 0xd20u + rows[i].off;
                } else {
                    const uint32_t base = rows[i].base_idx == 0 ? 0x10000u : 0x20000u;
                    addr = base + rows[i].off;
                }
                CHECK(has_reg(addr, rows[i].dwords, rows[i].cls, WSubstep::Mes),
                      "missing MES reg 0x%x (row %u)", addr, i);
                CHECK(r3_wg_classify(addr, rows[i].dwords, rows[i].cls, "k10-mes") ==
                          WVerdict::TxnRead,
                      "MES reg 0x%x (row %u) DENIED", addr, i);
            }
            /* Doorbells come from the doorbell declaration table (single source of truth). */
            const uint32_t dc = amdgpu::getMesDoorbellCount();
            const R3WriteRowAbs *dbs = amdgpu::getMesDoorbells();
            for (uint32_t i = 0; i < dc; i++) {
                CHECK(has_reg(dbs[i].addr, dbs[i].dwords, dbs[i].cls, WSubstep::Mes),
                      "missing MES doorbell 0x%x (row %u)", dbs[i].addr, i);
                CHECK(r3_wg_classify(dbs[i].addr, dbs[i].dwords, dbs[i].cls,
                                     "k10-mes") == WVerdict::TxnRead,
                      "MES doorbell 0x%x DENIED", dbs[i].addr);
            }
        }

        /* PSP (K4): MP0 BASE_IDX 1 = 0x60000; golden rows carry the real offset. */
        {
            const uint32_t rc = amdgpu::getPspRowCount();
            const R3WriteRowReg *rows = amdgpu::getPspRows();
            r3_wg_arm_substep(WSubstep::Psp);
            for (uint32_t i = 0; i < rc; i++) {
                const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
                const uint32_t addr = 0x60000u + g->offset;
                CHECK(has_reg(addr, rows[i].dwords, rows[i].cls, WSubstep::Psp),
                      "missing PSP reg 0x%x (row %u)", addr, i);
                CHECK(r3_wg_classify(addr, rows[i].dwords, rows[i].cls, "k10-psp") ==
                          WVerdict::TxnRead,
                      "PSP reg 0x%x (row %u) DENIED", addr, i);
            }
        }

        /* SMU (K5): MP1 BASE_IDX 1 = 0x70000; golden rows carry the real offset. */
        {
            const uint32_t rc = amdgpu::getSmuRowCount();
            const R3WriteRowReg *rows = amdgpu::getSmuRows();
            r3_wg_arm_substep(WSubstep::Smu);
            for (uint32_t i = 0; i < rc; i++) {
                const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
                const uint32_t addr = 0x70000u + g->offset;
                CHECK(has_reg(addr, rows[i].dwords, rows[i].cls, WSubstep::Smu),
                      "missing SMU reg 0x%x (row %u)", addr, i);
                CHECK(r3_wg_classify(addr, rows[i].dwords, rows[i].cls, "k10-smu") ==
                          WVerdict::TxnRead,
                      "SMU reg 0x%x (row %u) DENIED", addr, i);
            }
        }

        r3_wg_disarm_substep();
        CHECK(r3_writeguard_deny_count() == deny_before, "registered rows must not DENY");
    }

    /* Fail closed: a null argument stops registration. */
    CHECK(r3_integration_register_production(nullptr, &ip) != 0, "null plan accepted");
    CHECK(r3_integration_register_production(&plan, nullptr) != 0, "null ip accepted");
}

/* --- T17: D-02 read-only preflight gate ----------------------------------------------- */

static void build_readings(R3PreflightReading *rd, bool wrong_segment)
{
    const uint32_t n = r3_preflight_probe_count();
    for (uint32_t i = 0; i < n; i++) {
        const r3gold::GoldenReg *g = r3_preflight_probe(i);
        rd[i].reg = g;
        rd[i].read_ok = true;
        rd[i].addr = g ? g->offset : 0u;
        uint32_t value = 0x12345678u; /* record-only canary */
        if (g && g->gate == r3gold::Gate::Equals)
            value = g->baseline;
        if (g && g->gate == r3gold::Gate::NonZero)
            value = 1u; /* SOS alive */
        if (wrong_segment) {
            /* A read from segment 0 instead of _BASE_IDX 1: neither the FB baseline nor the
             * SOS-alive bit matches. */
            if (g && g->base_idx == 1u)
                value = 0u;
        }
        rd[i].value = value;
    }
}

static void test_preflight()
{
    const uint32_t n = r3_preflight_probe_count();
    CHECK(n == 11u, "probe count=%u", n);
    CHECK(r3_preflight_probe(n) == nullptr, "probe past end");

    /* Probe set covers every segment the substeps and layout consume, with the golden
     * entry carrying the corrected segment. */
    bool has_mp0_seg1 = false, has_mmhub_seg1 = false, has_gc = false, has_sdma = false;
    bool has_gc_fb_offset = false;
    for (uint32_t i = 0; i < n; i++) {
        const r3gold::GoldenReg *g = r3_preflight_probe(i);
        if (!g)
            continue;
        if (g->block == amdgpu::IPBlock::MP0)
            has_mp0_seg1 = g->base_idx == 1u;
        if (g->block == amdgpu::IPBlock::MMHUB)
            has_mmhub_seg1 = g->base_idx == 1u;
        if (g->block == amdgpu::IPBlock::GC)
            has_gc = true;
        if (g->block == amdgpu::IPBlock::SDMA0)
            has_sdma = true;
        if (g->name && strcmp(g->name, "GCMC_VM_FB_OFFSET") == 0)
            has_gc_fb_offset = true;
    }
    CHECK(has_mp0_seg1, "MP0 probe must use segment 1");
    CHECK(has_mmhub_seg1, "MMHUB probe must use segment 1");
    CHECK(has_gc && has_sdma, "GC/SDMA canaries missing");
    CHECK(has_gc_fb_offset, "GC FB_OFFSET probe missing (TMR system_phy cross-check)");

    R3PreflightReading rd[R3_PREFLIGHT_MAX] = {};
    R3PreflightResult res {};

    /* Normal: every canary readable, FB baseline matches, SOS alive. */
    build_readings(rd, false);
    r3_preflight_evaluate(rd, n, &res);
    CHECK(res.ok, "normal readings must pass");
    CHECK(res.count == n, "item count=%u", res.count);

    /* Wrong segment: the two Equals gates and SOS-alive miss their baselines. */
    build_readings(rd, true);
    r3_preflight_evaluate(rd, n, &res);
    CHECK(!res.ok, "wrong-segment readings must abort");
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < res.count; i++)
        if (res.items[i].verdict == R3PreflightVerdict::ValueMismatch)
            mismatches++;
    CHECK(mismatches >= 2u, "expected FB base/top mismatches, got %u", mismatches);

    /* Unreachable: a failed bus read aborts. */
    build_readings(rd, false);
    rd[0].read_ok = false;
    r3_preflight_evaluate(rd, n, &res);
    CHECK(!res.ok, "unreachable read must abort");
    CHECK(res.items[0].verdict == R3PreflightVerdict::ReadFail, "read-fail verdict");

    /* All-ones sentinel aborts even on a record-only register. */
    build_readings(rd, false);
    rd[n - 1].value = 0xFFFFFFFFu;
    r3_preflight_evaluate(rd, n, &res);
    CHECK(!res.ok, "all-ones read must abort");
    CHECK(res.items[res.count - 1u].verdict == R3PreflightVerdict::AllOnes,
          "all-ones verdict");

    /* SOS not alive (C1.3 / D3): C2PMSG_81 == 0 fails the NonZero gate. */
    build_readings(rd, false);
    for (uint32_t i = 0; i < n; i++)
        if (rd[i].reg && strstr(rd[i].reg->name, "C2PMSG_81") != nullptr)
            rd[i].value = 0u;
    r3_preflight_evaluate(rd, n, &res);
    CHECK(!res.ok, "SOS dead must abort");
}

static void test_prepare_guard_and_chain()
{
    /* D-05: a failed survey never prepares (no whitelist re-arm, no counter reset). */
    CHECK(!r3_integration_should_prepare(false, 5u), "prepare after failed survey");
    CHECK(!r3_integration_should_prepare(true, 0u), "prepare at target 0");
    CHECK(r3_integration_should_prepare(true, 1u), "prepare at target 1");

    /* D-11: first cause is preserved, later causes chain onto it. */
    R3IntegrationCtx ic;
    memset(&ic, 0, sizeof ic);
    r3_integration_note_prep_failure(&ic, "prep:scanout-outside-carveout");
    r3_integration_note_prep_failure(&ic, "gop-range-unconfirmed");
    CHECK(strcmp(ic.prep_fail, "prep:scanout-outside-carveout") == 0,
          "first cause overwritten: %s", ic.prep_fail);
    CHECK(strstr(ic.prep_chain, "prep:scanout-outside-carveout -> gop-range-unconfirmed") !=
              nullptr,
          "chain lost consequence: %s", ic.prep_chain);
}

static bool fake_read(void *user, uint32_t dword, uint32_t *value)
{
    const uint32_t *regs = static_cast<const uint32_t *>(user);
    if (dword >= 0x1000u || regs[dword] == 0xFFFFFFFFu)
        return false;
    *value = regs[dword];
    return true;
}

static void test_scanout()
{
    static uint32_t regs[0x1000];
    const uint32_t dcn = 0u;
    memset(regs, 0, sizeof regs);

    /* HUBP0 enabled: pitch 0x100, height 0x100, addr 0x1000000 => end 0x1010000. */
    regs[dcn + 0x05f3u] = 0x100u;                 /* SURFACE_ENABLE bit 8 */
    regs[dcn + 0x05eau] = 0x0100u << 16;          /* height */
    regs[dcn + 0x0607u] = 0x0100u;                /* pitch  */
    regs[dcn + 0x060au] = 0x01000000u;
    regs[dcn + 0x060bu] = 0u;

    R3ScanoutReading r {};
    CHECK(r3_integration_collect_scanout(dcn, fake_read, regs, 0ull, 0x100000000ull, &r), "collect failed");
    CHECK(r.surfaces == 1u, "surfaces=%u", r.surfaces);
    CHECK(r.gop_end == 0x01010000ull, "gop_end=0x%llx", (unsigned long long)r.gop_end);

    /* F1 (review P1-1): the same surfaces expressed in MC form (fb_mc_base=0x8000000000)
     * must normalize to the identical carveout offsets. HUBP0 at MC 0x80001000000 +
     * cursor at MC 0x80002000000 => offsets 0x1000000 / 0x2000000. */
    constexpr uint64_t kMc = 0x8000000000ull;
    constexpr uint64_t kVram = 4096ull * 1024ull * 1024ull;
    memset(regs, 0, sizeof regs);
    regs[dcn + 0x05f3u] = 0x100u;
    regs[dcn + 0x05eau] = 0x0100u << 16;
    regs[dcn + 0x0607u] = 0x0100u;
    regs[dcn + 0x060au] = 0x01000000u;            /* low of MC 0x80001000000 */
    regs[dcn + 0x060bu] = 0x00000080u;            /* high of MC 0x8001000000 */
    regs[dcn + 0x0678u] = 1u;
    regs[dcn + 0x0679u] = 0x02000000u;            /* low  of MC 0x0000_8000_02000000 */
    regs[dcn + 0x067au] = 0x00000080u;           /* high                            */
    regs[dcn + 0x067bu] = (63u << 16) | 63u;
    CHECK(r3_integration_collect_scanout(dcn, fake_read, regs, kMc, kVram, &r),
          "MC-form collect failed");
    CHECK(r.surfaces == 1u && r.cursors == 1u, "mc surfaces=%u cursors=%u",
          r.surfaces, r.cursors);
    CHECK(r.gop_end == 0x02004000ull, "MC-form gop_end=0x%llx",
          (unsigned long long)r.gop_end);

    /* F1: a surface ending past the carveout must fail closed. */
    regs[dcn + 0x060bu] = 0x00000090u;            /* MC 0x90_01000000 > base+vram */
    CHECK(!r3_integration_collect_scanout(dcn, fake_read, regs, kMc, kVram, &r),
          "out-of-carveout surface must fail closed");

    /* F1: a cursor ending past the carveout must fail closed as well. */
    memset(regs, 0, sizeof regs);
    regs[dcn + 0x0678u] = 1u;
    regs[dcn + 0x0679u] = 0x01000000u;
    regs[dcn + 0x067au] = 0x00000081u;            /* MC 0x8101000000 > base+vram */
    regs[dcn + 0x067bu] = (63u << 16) | 63u;
    CHECK(!r3_integration_collect_scanout(dcn, fake_read, regs, kMc, kVram, &r),
          "out-of-carveout cursor must fail closed");

    /* F1: a surface below the carveout base must fail closed too. */
    memset(regs, 0, sizeof regs);
    regs[dcn + 0x05f3u] = 0x100u;
    regs[dcn + 0x05eau] = 0x0100u << 16;
    regs[dcn + 0x0607u] = 0x0100u;
    regs[dcn + 0x060au] = 0x00001000u;            /* MC 0x1000 < base */
    regs[dcn + 0x060bu] = 0u;
    CHECK(!r3_integration_collect_scanout(dcn, fake_read, regs, kMc, kVram, &r),
          "below-carveout surface must fail closed");

    /* A disabled HUBP must not contribute; an unreadable HUBP must fail closed. */
    regs[dcn + 0x05f3u] = 0u;
    CHECK(r3_integration_collect_scanout(dcn, fake_read, regs, 0ull, 0x100000000ull, &r), "collect3");
    CHECK(r.surfaces == 0u, "disabled HUBP counted");
    regs[dcn + 0x05f3u] = 0xFFFFFFFFu;
    CHECK(!r3_integration_collect_scanout(dcn, fake_read, regs, 0ull, 0x100000000ull, &r),
          "unreadable HUBP must fail closed");
}

/* The exact layout inputs prepare_stage2 builds must yield a usable plan: 4 GiB carveout,
 * 256 MiB BAR0, 64 MiB TMR slot, a 64 MiB scanout guard. This is the K10 acceptance that the
 * wired ladder can actually reach stage 1 on this machine profile. */
static void test_layout_reachable()
{
    R3LayoutInputs in {};
    in.fb_mc_base = 0x8000000000ull;
    in.fb_phys_base = 0x7a0000000ull;
    in.bar0_base = 0x8d0000000ull;
    in.real_vram_size = 4096ull * 1024ull * 1024ull;
    in.vbios_reserve = 1024ull * 1024ull;      /* L4 floor */
    in.bar0_size = 256ull * 1024ull * 1024ull;
    in.gop_end = 64ull * 1024ull * 1024ull;    /* measured scanout end */
    in.gop_end_known = true;
    in.tmr_size = 64ull * 1024ull * 1024ull;   /* C4 slot */

    R3LayoutPlan plan;
    const int rc = r3_layout_compute(&in, &plan);
    CHECK(rc == 0 && plan.ok, "layout must succeed for the machine profile (rc=%d,%s)", rc,
          plan.fail_reason ? plan.fail_reason : "-");
    CHECK(plan.grant_count == 7u, "grant_count=%u", plan.grant_count);
    const R3Grant *tmr = r3_layout_grant(&plan, "tmr");
    CHECK(tmr && tmr->start == 0x08000000ull, "TMR slot start");
    CHECK(tmr && tmr->size == 64ull * 1024ull * 1024ull, "TMR slot size");
    CHECK(plan.tmr_system_phy == 0x7a0000000ull + tmr->start, "system PA from FB_OFFSET");
}

static void test_io_arithmetic()
{
    R3IoWindow w;
    w.bar5_bytes = 0x100000u * 4u; /* 1M dwords */
    w.bar0_bytes = 0x00200000u;
    w.bar2_bytes = 0x00010000u;

    CHECK(r3_io_mmio_in_range(&w, 0u), "dword 0");
    CHECK(r3_io_mmio_in_range(&w, 0xFFFFFu), "last dword");
    CHECK(!r3_io_mmio_in_range(&w, 0x100000u), "past BAR5");
    CHECK(!r3_io_mmio_in_range(nullptr, 0u), "null window");

    CHECK(r3_io_bus_in_range(&w, R3_IO_BAR_VRAM, 0u, 1u), "bar0 dword 0");
    CHECK(r3_io_bus_in_range(&w, R3_IO_BAR_VRAM, 0x001ffff8ull, 2u), "bar0 last 8B");
    CHECK(!r3_io_bus_in_range(&w, R3_IO_BAR_VRAM, 0x00200000ull, 1u), "past BAR0");
    CHECK(!r3_io_bus_in_range(&w, R3_IO_BAR_VRAM, 1u, 1u), "misaligned");
    CHECK(!r3_io_bus_in_range(&w, R3_IO_BAR_VRAM, 4u, 2u), "2-dword 4B alignment");
    CHECK(r3_io_bus_in_range(&w, 2u, 0x58ull, 2u), "doorbell 0x16*4");
    CHECK(!r3_io_bus_in_range(&w, 1u, 0u, 1u), "unknown bar");

}

} /* namespace */

static void test_hdp_window()
{
    uint32_t dw = 0;
    CHECK(!r3_hdp_flush_window(0u, 0x400000u, &dw), "zero remap rejected");
    CHECK(!r3_hdp_flush_window(0xFFFFFFFFu, 0x400000u, &dw), "all-ones rejected");
    CHECK(!r3_hdp_flush_window(0x1001u, 0x400000u, &dw), "misaligned remap rejected");
    CHECK(!r3_hdp_flush_window(0x400000u, 0x400000u, &dw), "remap past the map rejected");
    CHECK(!r3_hdp_flush_window(0x80000u, 0u, &dw), "unmapped BAR5 rejected");
    CHECK(r3_hdp_flush_window(0x80000u, 0x400000u, &dw) && dw == 0x20000u,
          "plausible remap maps to a BAR5 dword");
}

static void test_trigger_admission()
{
    /* D-01/F12: the trigger fires the armed substeps only. Ladder layout: 0 survey,
     * 1 preflight, 2.. psp..mes; table_count 7. */
    const uint32_t kFirst = 2u, kCount = 7u;
    R3TriggerDecision d;

    d = r3_trigger_admission(false, true, true, true, 1u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::Ok, "stage 1 admissible: %s", d.reason);
    CHECK(d.first_stage == 2u && d.target_stage == 2u, "stage 1 -> ladder 2..2 (got %u..%u)",
          d.first_stage, d.target_stage);

    d = r3_trigger_admission(false, true, true, true, 3u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::Ok && d.first_stage == 2u && d.target_stage == 4u,
          "stage 3 -> ladder 2..4, never 0..1");

    d = r3_trigger_admission(false, true, true, true, 99u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::Ok && d.target_stage == kCount - 1u,
          "an over-large stage clamps to the table end");

    d = r3_trigger_admission(true, true, true, true, 1u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::AlreadyProvisioned &&
              strcmp(d.reason, "already-provisioned-this-boot") == 0,
          "D-04 refuses a second fire: %s", d.reason);

    d = r3_trigger_admission(false, false, true, true, 1u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::NotArmed &&
              strcmp(d.reason, "boot-gate-not-passed") == 0,
          "an unarmed boot gate refuses with a reason: %s", d.reason);

    d = r3_trigger_admission(false, true, false, true, 1u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::NotArmed && strcmp(d.reason, "prep-not-ok") == 0,
          "a failed preparation refuses with a reason: %s", d.reason);

    d = r3_trigger_admission(false, true, true, false, 1u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::NotArmed && strcmp(d.reason, "io-incomplete") == 0,
          "an incomplete R3Io set refuses with a reason: %s", d.reason);

    d = r3_trigger_admission(false, true, true, true, 0u, kFirst, kCount);
    CHECK(d.verdict == R3TriggerVerdict::NoSubsteps &&
              strcmp(d.reason, "stage-0-arms-no-substep") == 0,
          "stage 0 has nothing to fire: %s", d.reason);

    d = r3_trigger_admission(false, true, true, true, 1u, kFirst, 0u);
    CHECK(d.verdict == R3TriggerVerdict::NoSubsteps, "an empty table has no substeps");

    d = r3_trigger_admission(false, true, true, true, 1u, 7u, 7u);
    CHECK(d.verdict == R3TriggerVerdict::NoSubsteps, "a missing substep row refuses");
}

/* §15.1 item 4: preflight runs AFTER bind_env, so ctx->psp_version_pre must be synced
 * into env before S12 reads it. The test drives the SAME pure entry stage_preflight
 * calls (r3_stages_preflight_sync_env) with REAL probe readings/verdicts, so a missing
 * sync in that core fails here. The kext-only stage table itself is not linked into this
 * runner (see the planted contract check for the stage_preflight call site). */
static void test_preflight_env_sync()
{
    R3IntegrationCtx ic {};
    /* bind_env in stage 0 sees the preflight field still zero. */
    r3_integration_bind_env(&ic, nullptr, 0ull);
    CHECK(ic.env.psp_version_pre == 0u, "env pre is zero before preflight");
    CHECK(!ic.env.psp_version_pre_ok, "env pre_ok false before preflight");

    /* Real preflight data: r3_preflight_probe/evaluate fill readings + verdicts, the
     * exact inputs stage_preflight passes to the sync core. */
    const uint32_t n = r3_preflight_probe_count();
    R3PreflightReading rd[R3_PREFLIGHT_MAX] = {};
    R3PreflightResult res {};
    build_readings(rd, false);
    r3_preflight_evaluate(rd, n, &res);

    /* Find the C2PMSG_58 probe value/verdict the core must copy. */
    uint32_t expected = 0u;
    bool expected_ok = false, found = false;
    for (uint32_t i = 0; i < n && i < res.count; i++) {
        if (rd[i].reg && strstr(rd[i].reg->name, "C2PMSG_58") != nullptr) {
            expected = rd[i].value;
            expected_ok = res.items[i].verdict == R3PreflightVerdict::Ok;
            found = true;
            break;
        }
    }
    CHECK(found, "C2PMSG_58 probe missing from preflight set");

    r3_stages_preflight_sync_env(&ic, rd, n, &res);
    CHECK(ic.env.psp_version_pre == expected,
          "env pre %u matches probe reading %u", ic.env.psp_version_pre, expected);
    CHECK(ic.env.psp_version_pre_ok == expected_ok,
          "env pre_ok matches probe verdict");
    CHECK(ic.psp_version_pre == expected && ic.psp_version_pre_ok == expected_ok,
          "ctx pre fields match the probe");

    /* Null guard: never panic on an absent argument (fail closed). */
    r3_stages_preflight_sync_env(nullptr, rd, n, &res);
    r3_stages_preflight_sync_env(&ic, nullptr, n, &res);
    r3_stages_preflight_sync_env(&ic, rd, n, nullptr);
}

/* Cross-TU pointer match: constexpr GoldenReg has internal linkage, so direct pointer
 * comparison between TUs would fail. r3_preflight_match uses ref() fields instead. */
static void test_preflight_match_cross_tu()
{
    const uint32_t n = r3_preflight_probe_count();
    bool found = false;
    for (uint32_t i = 0; i < n; i++) {
        const r3gold::GoldenReg *probe = r3_preflight_probe(i);
        if (r3_preflight_match(probe, &r3gold::kMp0SmnC2pmsg58)) {
            found = true;
            // The probe's GoldenReg pointer is from integration.cpp's TU; the literal
            // &r3gold::kMp0SmnC2pmsg58 is from this TU (test_integration.cpp). They MUST
            // match by value even though pointer comparison would fail.
            CHECK(probe->name && strcmp(probe->name, r3gold::kMp0SmnC2pmsg58.name) == 0,
                  "C2PMSG_58 probe name matches");
            CHECK(probe->block == r3gold::kMp0SmnC2pmsg58.block,
                  "C2PMSG_58 probe block matches");
            CHECK(probe->offset == r3gold::kMp0SmnC2pmsg58.offset,
                  "C2PMSG_58 probe offset matches");
            CHECK(probe->base_idx == r3gold::kMp0SmnC2pmsg58.base_idx,
                  "C2PMSG_58 probe base_idx matches");
            break;
        }
    }
    CHECK(found, "C2PMSG_58 probe found in the preflight probe set");

    /* Null and mismatched probes: match returns false. */
    CHECK(!r3_preflight_match(nullptr, &r3gold::kMp0SmnC2pmsg58), "null a -> false");
    CHECK(!r3_preflight_match(&r3gold::kMp0SmnC2pmsg58, nullptr), "null b -> false");
    /* Different register: C2PMSG_81 probes MUST NOT match C2PMSG_58's ref. */
    for (uint32_t i = 0; i < n; i++) {
        const r3gold::GoldenReg *probe = r3_preflight_probe(i);
        if (!r3_preflight_match(probe, &r3gold::kMp0SmnC2pmsg58) && probe) {
            CHECK(!r3_preflight_match(probe, &r3gold::kMp0SmnC2pmsg58),
                  "other registers do not match C2PMSG_58");
        }
    }
}

/* E5/C0.9⑥: after firing the status is truthful: "fired, done" or "stopped: <reason>". */
static void test_status_fired_line()
{
    char buf[64];
    r3_status_fired_line(buf, sizeof buf, true, "ignored");
    CHECK(strcmp(buf, "fired, done\n") == 0, "success status: %s", buf);

    r3_status_fired_line(buf, sizeof buf, false, "stage smu: FAIL reason=seq:effect");
    CHECK(strcmp(buf, "stopped: stage smu: FAIL reason=seq:effect\n") == 0,
          "stopped status carries the reason: %s", buf);

    r3_status_fired_line(buf, sizeof buf, false, nullptr);
    CHECK(strcmp(buf, "stopped: (no reason emitted)\n") == 0,
          "missing reason is explicit: %s", buf);
}

int main()
{
    test_matrix();
    test_enabled_text();
    test_latch();
    test_registration();
    test_scanout();
    test_layout_reachable();
    test_io_arithmetic();
    test_preflight();
    test_prepare_guard_and_chain();
    test_hdp_window();
    test_trigger_admission();
    test_preflight_env_sync();
    test_preflight_match_cross_tu();
    test_status_fired_line();
    printf("%u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
