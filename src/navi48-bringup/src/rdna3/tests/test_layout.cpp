/*
 * test_layout.cpp - host tests for the RDNA3 carveout layout allocator (T7/T8).
 *
 * T7: L1-L8 one test group per rule, plus the six GOP-pitch cases of R4 §3.1 and the
 *     interval-graph golden of R4 §4.4/§4.5. T8: MC / system-PA / BAR0 address forms,
 *     with the nullarc system_phy constants explicitly rejected as expectations.
 *
 * The allocator is pure (no MMIO, no allocation, no IOKit), so this file builds with a
 * plain host clang++:
 *
 *   clang++ -std=c++17 -Wall -Wextra -Werror -g -O1 \
 *       -fsanitize=address,undefined -I src -I src/rdna3 \
 *       src/navi48-bringup/src/rdna3/tests/test_layout.cpp \
 *       src/navi48-bringup/src/rdna3/rdna3_layout.cpp -o /tmp/test_layout
 *   /tmp/test_layout
 *
 * (paths relative to the fork repository root)
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "rdna3_layout.h"
#include "rdna3_runtime.h"

static int gFailures;
static int gChecks;

#define CHECK(cond, ...) do { \
    gChecks++; \
    if (!(cond)) { \
        gFailures++; \
        std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

static constexpr uint64_t MiB = 1024ull * 1024ull;

/* The phase-1 machine baseline (R4 §3/§4): 4096 MiB carveout, MMHUB-authoritative MC
 * base, system physical base 0x7a0000000, BAR0 0x8d0000000/256 MiB. gop_end is the
 * folded max over active HUBP/CURSOR surfaces; the caller must set gop_end_known. */
static R3LayoutInputs machine_inputs()
{
    R3LayoutInputs in = {};
    in.fb_mc_base = 0x8000000000ull;
    in.fb_phys_base = 0x7a0000000ull;
    in.bar0_base = 0x8d0000000ull;
    in.real_vram_size = 4096ull * MiB;
    in.vbios_reserve = 0ull; /* L4 floors this at 1 MiB */
    in.bar0_size = 256ull * MiB;
    in.gop_end = 0x13c6800ull; /* pitch case A */
    in.tmr_size = 64ull * MiB; /* only for tests; runtime takes LOAD_TOC resp.tmr_size */
    in.gop_end_known = true;
    return in;
}

static const R3Grant *grant(const R3LayoutPlan &p, const char *tag)
{
    return r3_layout_grant(&p, tag);
}

static void expect_grant(const R3LayoutPlan &p, const char *tag, uint64_t start,
                         uint64_t end, R3Owner owner, bool needs_cpu)
{
    const R3Grant *g = grant(p, tag);
    CHECK(g != nullptr, "grant %s present", tag);
    if (!g)
        return;
    CHECK(g->start == start, "%s start 0x%llx want 0x%llx", tag,
          (unsigned long long)g->start, (unsigned long long)start);
    CHECK(g->end == end, "%s end 0x%llx want 0x%llx", tag,
          (unsigned long long)g->end, (unsigned long long)end);
    CHECK(g->size == end - start, "%s size 0x%llx want 0x%llx", tag,
          (unsigned long long)g->size, (unsigned long long)(end - start));
    CHECK(g->owner == owner, "%s owner", tag);
    CHECK(g->cls == R3B2Class::DataMove, "%s class", tag);
    CHECK(g->needs_cpu == needs_cpu, "%s needs_cpu", tag);
}

/* T16: the runtime block and the plan must stay small enough to live in the driver
 * instance and be copied/snapshotted without a large kernel-stack allocation. */
static void test_size_limits()
{
    CHECK(sizeof(R3LayoutPlan) <= R3_LAYOUT_MAX_BYTES,
          "R3LayoutPlan %zu > %u", sizeof(R3LayoutPlan), R3_LAYOUT_MAX_BYTES);
    CHECK(sizeof(R3Rt) <= R3_RT_MAX_BYTES,
          "R3Rt %zu > %u", sizeof(R3Rt), R3_RT_MAX_BYTES);
    CHECK(R3_LAYOUT_MAX_GRANTS >= 7u, "grant capacity");
    CHECK(sizeof(R3SubstepCtx) == 16u, "R3SubstepCtx size %zu", sizeof(R3SubstepCtx));
    CHECK(offsetof(R3SubstepCtx, stage) == 4u, "R3SubstepCtx.stage offset");
    CHECK(offsetof(R3Rt, layout) == 0u, "R3Rt.layout offset");

    R3Rt rt = {};
    rt.ready_bits = 1u << static_cast<unsigned>(R3Ip::Psp);
    rt.latch_bits = 0u;
    rt.substep.ip = R3Ip::Psp;
    rt.substep.stage = 1u;
    rt.substep.step = 3u;
    rt.substep.flags = R3_SUBSTEP_RECHECK_USED;
    CHECK(rt.substep.flags == R3_SUBSTEP_RECHECK_USED, "substep flags");
}

/* T8 / §4.1: the three address forms and the runtime system_phy recomputation. */
static void test_address_model()
{
    CHECK(r3_layout_mc_addr(0x8000000000ull, 0x08000000ull) == 0x8008000000ull,
          "MC = fb_mc_base + off");
    CHECK(r3_layout_system_phy(0x7a0000000ull, 0x08000000ull) == 0x7a8000000ull,
          "system PA = fb_phys_base + off");
    CHECK(r3_layout_bar0_addr(0x8d0000000ull, 0x08000000ull) == 0x8d8000000ull,
          "CPU = BAR0 + off");

    const uint64_t phy = r3_layout_system_phy(0x7a0000000ull, 0x08000000ull);
    CHECK(phy != 0x6a0c000000ull, "nullarc raw system_phy rejected as expectation");
    CHECK(phy != 0x6a0000000ull, "nullarc archived system_phy rejected as expectation");
    CHECK(r3_layout_system_phy_valid(0x7a0000000ull, 0x08000000ull, 0x7a8000000ull),
          "runtime-computed system_phy accepted");
    CHECK(!r3_layout_system_phy_valid(0x7a0000000ull, 0x08000000ull, 0x6a0c000000ull),
          "hard-coded 0x6a0c000000 rejected");
    CHECK(!r3_layout_system_phy_valid(0x7a0000000ull, 0x08000000ull, 0x6a0000000ull),
          "hard-coded 0x6a0000000 rejected");
}

/* L1 + R4 §3.1: the six pitch cases and the 4096-pitch triple-buffer note. */
static void test_gop_pitch_cases()
{
    struct Case {
        const char *name;
        uint32_t pitch;
        uint32_t height;
        uint32_t buffers;
        uint64_t gop_end;
        uint64_t guard;
    };
    static const Case cases[] = {
        { "A 2880x1800x1", 2880u, 1800u, 1u, 0x13c6800ull, 0x4000000ull },
        { "B 3072x1800x1", 3072u, 1800u, 1u, 0x1518000ull, 0x4000000ull },
        { "C 3072x1856x1", 3072u, 1856u, 1u, 0x15c0000ull, 0x4000000ull },
        { "D 3072x1800x2", 3072u, 1800u, 2u, 0x2a30000ull, 0x4000000ull },
        { "E 3072x1800x3", 3072u, 1800u, 3u, 0x3f48000ull, 0x4000000ull },
        { "F 3072x1856x3", 3072u, 1856u, 3u, 0x4140000ull, 0x5000000ull },
        { "G 4096x1800x3", 4096u, 1800u, 3u, 0x5460000ull, 0x6000000ull },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case &c = cases[i];
        const uint64_t computed_end =
            (uint64_t)c.pitch * (uint64_t)c.height * 4ull * (uint64_t)c.buffers;
        CHECK(computed_end == c.gop_end, "%s golden gop_end 0x%llx want 0x%llx",
              c.name, (unsigned long long)computed_end, (unsigned long long)c.gop_end);

        R3LayoutInputs in = machine_inputs();
        in.gop_end = c.gop_end;
        R3LayoutPlan p = {};
        const int rc = r3_layout_compute(&in, &p);
        CHECK(rc == 0 && p.ok, "%s plan ok", c.name);
        if (!p.ok)
            continue;
        CHECK(p.gop_guard_end == c.guard, "%s guard 0x%llx want 0x%llx", c.name,
              (unsigned long long)p.gop_guard_end, (unsigned long long)c.guard);
        const R3Grant *fw = grant(p, "fw_buf");
        CHECK(fw && fw->start == c.guard, "%s fw_buf starts after guard", c.name);
        /* The TMR slot is independent of the GOP guard until the guard reaches it. */
        CHECK(grant(p, "tmr") && grant(p, "tmr")->start == R3_TMR_PREFERRED,
              "%s preferred TMR slot", c.name);
        CHECK(grant(p, "sdma") && grant(p, "sdma")->start == 0x0c000000ull,
              "%s C5 starts at 192 MiB", c.name);
        CHECK(grant(p, "mes") && grant(p, "mes")->end == 0x0d000000ull,
              "%s C5 ends at 208 MiB", c.name);
    }
}

/* L1 planted-break target: a hard-coded 17/20 MiB guard must not survive. */
static void test_l1_guard_not_hardcoded()
{
    R3LayoutInputs in = machine_inputs();
    in.gop_end = 0x4140000ull; /* case F: guard must rise to 80 MiB */
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "case F plan ok");
    CHECK(p.gop_guard_end == 0x5000000ull, "guard 80 MiB, got 0x%llx",
          (unsigned long long)p.gop_guard_end);
    CHECK(p.gop_guard_end != 0x1100000ull, "guard is not nullarc ring 17 MiB");
    CHECK(p.gop_guard_end != 0x1400000ull, "guard is not nullarc TMR 20 MiB");
    const R3Grant *fw = grant(p, "fw_buf");
    CHECK(fw && fw->start == 0x5000000ull, "fw_buf after 80 MiB guard");

    in.gop_end = 0x1100000ull; /* a 17 MiB GOP still gets the 64 MiB floor */
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "17 MiB GOP plan ok");
    CHECK(p.gop_guard_end == 0x4000000ull, "guard floor 64 MiB");
    fw = grant(p, "fw_buf");
    CHECK(fw && fw->start == 0x4000000ull, "fw_buf not at 17 MiB");
}

/* L8 planted-break target: unknown GOP range is fail-closed, never a 64 MiB default. */
static void test_l8_gop_unknown_fail_closed()
{
    R3LayoutInputs in = machine_inputs();
    in.gop_end = 0ull;
    in.gop_end_known = false;
    R3LayoutPlan p;
    std::memset(&p, 0xa5, sizeof(p));
    const int rc = r3_layout_compute(&in, &p);
    CHECK(rc != 0, "unknown GOP returns non-zero");
    CHECK(!p.ok, "unknown GOP plan not ok");
    CHECK(p.fail_reason != nullptr &&
              std::strcmp(p.fail_reason, "gop-range-unconfirmed") == 0,
          "fail_reason exact, got %s", p.fail_reason ? p.fail_reason : "(null)");
    CHECK(p.grant_count == 0u, "no grants on fail-closed plan");
}

/* L2: TMR size comes only from the LOAD_TOC response; the slot is 1 MiB aligned and
 * preferred. D-08: TMR > 64 MiB fails closed (tmr-exceeds-slot). */
static void test_l2_tmr_size_and_alignment()
{
    struct Case {
        uint64_t size;
        uint64_t start;
        uint64_t end;
        bool reordered;
        bool moved;
    };
    static const Case cases[] = {
        { 32ull * MiB, 0x08000000ull, 0x0a000000ull, false, false },
        { 64ull * MiB, 0x08000000ull, 0x0c000000ull, false, false },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case &c = cases[i];
        R3LayoutInputs in = machine_inputs();
        in.tmr_size = c.size;
        R3LayoutPlan p = {};
        const int rc = r3_layout_compute(&in, &p);
        CHECK(rc == 0 && p.ok, "tmr 0x%llx plan ok", (unsigned long long)c.size);
        if (!p.ok)
            continue;
        CHECK(p.tmr_size == c.size, "plan.tmr_size echoes input");
        const R3Grant *tmr = grant(p, "tmr");
        CHECK(tmr != nullptr, "tmr grant present");
        if (!tmr)
            continue;
        CHECK(tmr->start == c.start && tmr->end == c.end,
              "tmr 0x%llx at [0x%llx,0x%llx), got [0x%llx,0x%llx)",
              (unsigned long long)c.size, (unsigned long long)c.start,
              (unsigned long long)c.end, (unsigned long long)tmr->start,
              (unsigned long long)tmr->end);
        CHECK(tmr->size == c.size, "tmr size is the PSP value");
        /* D-08: alignment is 1 MiB (Linux PSP_TMR_ALIGNMENT), not natural alignment. */
        CHECK(tmr->start % MiB == 0, "tmr start 0x%llx %% 1MiB",
              (unsigned long long)tmr->start);
        CHECK(p.tmr_reordered == c.reordered, "tmr_reordered");
        CHECK(p.sdma_mes_moved == c.moved, "sdma_mes_moved");
        CHECK(p.tmr_in_bar0, "TMR in BAR0");
        CHECK(p.tmr_mc_addr == 0x8000000000ull + tmr->start, "tmr MC address");
        CHECK(p.tmr_system_phy == 0x7a0000000ull + tmr->start, "tmr system PA");
        CHECK(p.tmr_bar0_addr == 0x8d0000000ull + tmr->start, "tmr BAR0 address");
    }

    R3LayoutInputs in = machine_inputs();
    in.tmr_size = 0ull;
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "zero TMR size rejected");
    CHECK(p.fail_reason && std::strcmp(p.fail_reason, "tmr-size-invalid") == 0,
          "zero TMR reason, got %s", p.fail_reason ? p.fail_reason : "(null)");

    /* D-08: TMR > 64 MiB fails closed with "tmr-exceeds-slot". */
    static const uint64_t oversize[] = { 65ull * MiB, 96ull * MiB, 100ull * MiB,
                                         128ull * MiB, 192ull * MiB };
    for (unsigned i = 0; i < sizeof(oversize) / sizeof(oversize[0]); i++) {
        in = machine_inputs();
        in.tmr_size = oversize[i];
        p = {};
        CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "oversize TMR 0x%llx rejected",
              (unsigned long long)oversize[i]);
        CHECK(p.fail_reason && std::strcmp(p.fail_reason, "tmr-exceeds-slot") == 0,
              "oversize TMR reason, got %s", p.fail_reason ? p.fail_reason : "(null)");
    }

    /* D-08: alignment is always 1 MiB regardless of TMR size. */
    static const uint64_t aligns[] = { MiB, 3ull * MiB, 32ull * MiB, 64ull * MiB };
    for (unsigned i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        in = machine_inputs();
        in.tmr_size = aligns[i];
        p = {};
        CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "align case 0x%llx",
              (unsigned long long)aligns[i]);
        const R3Grant *tmr = grant(p, "tmr");
        CHECK(tmr && tmr->start % MiB == 0, "start %% 1MiB");
    }
}

/* L3: when the preferred slot is unusable, reorder within BAR0. D-08: TMR > 64 MiB
 * fails closed (tmr-exceeds-slot), so the F6 fallback (>128 MiB) is no longer reachable
 * from valid inputs — those cases now assert rejection instead. */
static void test_l3_degraded_reorder_and_f6()
{
    R3LayoutInputs in = machine_inputs();
    in.gop_end = 0x7f00000ull; /* 127 MiB -> 128 MiB guard */
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "128 MiB guard plan ok");
    CHECK(p.gop_guard_end == 0x8000000ull, "guard 128 MiB");
    CHECK(p.tmr_reordered, "TMR moved off the preferred slot");
    CHECK(p.sdma_mes_moved, "SDMA/MES packed before TMR");
    expect_grant(p, "fw_buf", 0x8000000ull, 0x9000000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_ring", 0x9000000ull, 0x9004000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_cmd", 0x9004000ull, 0x9005000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_fence", 0x9005000ull, 0x9006000ull, R3Owner::Psp, true);
    expect_grant(p, "sdma", 0x9006000ull, 0x9806000ull, R3Owner::Sdma, true);
    expect_grant(p, "mes", 0x9806000ull, 0xa006000ull, R3Owner::Mes, true);
    expect_grant(p, "tmr", 0xc000000ull, 0x10000000ull, R3Owner::Psp, false);

    /* D-08: TMR > 64 MiB fails closed. The F6 fallback path is no longer reachable
     * because the slot ceiling rejects the input before placement is attempted. */
    in = machine_inputs();
    in.tmr_size = 192ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "192 MiB TMR rejected (D-08)");
    CHECK(p.fail_reason && std::strcmp(p.fail_reason, "tmr-exceeds-slot") == 0,
          "192 MiB reason, got %s", p.fail_reason ? p.fail_reason : "(null)");

    in = machine_inputs();
    in.tmr_size = 129ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "129 MiB TMR rejected (D-08)");
    CHECK(p.fail_reason && std::strcmp(p.fail_reason, "tmr-exceeds-slot") == 0,
          "129 MiB reason, got %s", p.fail_reason ? p.fail_reason : "(null)");
}

/* L4: top guard = real_vram_size - max(VBIOS reserve, 1 MiB); no grant crosses it. */
static void test_l4_top_guard()
{
    R3LayoutInputs in = machine_inputs();
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "default plan ok");
    CHECK(p.top_guard == 0xfff00000ull, "top guard 1 MiB floor, got 0x%llx",
          (unsigned long long)p.top_guard);
    for (uint32_t i = 0; i < p.grant_count; i++)
        CHECK(p.grants[i].end <= p.top_guard, "%s below top guard", p.grants[i].tag);

    in.vbios_reserve = 4096ull * MiB - 200ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "200 MiB top guard plan ok");
    CHECK(p.top_guard == 0x0c800000ull, "top guard 200 MiB");
    CHECK(p.sdma_mes_moved, "SDMA/MES moved to fit below 200 MiB");
    for (uint32_t i = 0; i < p.grant_count; i++)
        CHECK(p.grants[i].end <= p.top_guard, "%s below 200 MiB", p.grants[i].tag);

    /* A 180 MiB ceiling has NO legal order for a 64 MiB TMR: the natural alignment is
     * max(1 MiB, 64 MiB) = 64 MiB, so the lowest free aligned slot above the 64 MiB guard is
     * 0x08000000 (0x04000000 is the guard itself), and 0x08000000 + 64 MiB = 0x0C000000 runs
     * past 0x0B400000. That is exactly the L8/L3 "no legal arrangement" case, so the plan must
     * fail closed rather than squeeze the TMR into an unaligned slot. */
    in.vbios_reserve = 4096ull * MiB - 180ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "180 MiB top guard fails closed");
    CHECK(p.grant_count == 0u, "failed plan publishes no grants");

    in.vbios_reserve = 4096ull * MiB + 1ull;
    p = {};
    CHECK(r3_layout_compute(&in, &p) != 0 && !p.ok, "oversized VBIOS reserve rejected");
    CHECK(p.fail_reason && std::strcmp(p.fail_reason, "top-guard-invalid") == 0,
          "top guard reason, got %s", p.fail_reason ? p.fail_reason : "(null)");
}

/* L5 planted-break target: r3_layout_validate() must reject any overlapping grants. */
static void test_l5_pairwise_disjoint()
{
    R3LayoutInputs in = machine_inputs();
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "valid plan");
    const char *why = nullptr;
    CHECK(r3_layout_validate(&p, &why), "allocator plan validates: %s", why ? why : "");

    R3LayoutPlan bad = p;
    bad.grants[1] = bad.grants[0]; /* two grants on exactly the same interval */
    why = nullptr;
    CHECK(!r3_layout_validate(&bad, &why), "overlapping grants rejected");
    CHECK(why && std::strstr(why, "overlap") != nullptr,
          "overlap reason, got %s", why ? why : "(null)");
}

/* L6: every grant carries a non-empty tag, an owner substep and a B2 class. */
static void test_l6_owner_and_class()
{
    CHECK(std::strcmp(r3_b2_class_name(R3B2Class::AccessEnable), "access-enable") == 0,
          "class name 1");
    CHECK(std::strcmp(r3_b2_class_name(R3B2Class::DataMove), "data-move") == 0,
          "class name 2");
    CHECK(std::strcmp(r3_b2_class_name(R3B2Class::Protocol), "protocol") == 0,
          "class name 3");
    CHECK(std::strcmp(r3_b2_class_name(R3B2Class::StateConfig), "state-config") == 0,
          "class name 4");
    CHECK(std::strcmp(r3_owner_name(R3Owner::Psp), "psp") == 0, "owner psp");
    CHECK(std::strcmp(r3_owner_name(R3Owner::Sdma), "sdma") == 0, "owner sdma");
    CHECK(std::strcmp(r3_owner_name(R3Owner::Mes), "mes") == 0, "owner mes");

    R3LayoutInputs in = machine_inputs();
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "valid plan");
    for (uint32_t i = 0; i < p.grant_count; i++) {
        const R3Grant &g = p.grants[i];
        CHECK(g.tag[0] != '\0', "grant %u tag", i);
        CHECK(static_cast<unsigned>(g.owner) <= static_cast<unsigned>(R3Owner::Mes),
              "grant %s owner", g.tag);
        CHECK(static_cast<unsigned>(g.cls) <=
                  static_cast<unsigned>(R3B2Class::StateConfig),
              "grant %s class", g.tag);
    }
    CHECK(grant(p, "fw_buf") && grant(p, "fw_buf")->owner == R3Owner::Psp, "fw_buf/psp");
    CHECK(grant(p, "tmr") && grant(p, "tmr")->owner == R3Owner::Psp, "tmr/psp");
    CHECK(grant(p, "sdma") && grant(p, "sdma")->owner == R3Owner::Sdma, "sdma/sdma");
    CHECK(grant(p, "mes") && grant(p, "mes")->owner == R3Owner::Mes, "mes/mes");

    R3LayoutPlan bad = p;
    bad.grants[0].owner = static_cast<R3Owner>(99);
    const char *why = nullptr;
    CHECK(!r3_layout_validate(&bad, &why), "bad owner rejected");
    CHECK(why && std::strstr(why, "owner") != nullptr, "owner reason");

    bad = p;
    bad.grants[0].cls = static_cast<R3B2Class>(99);
    why = nullptr;
    CHECK(!r3_layout_validate(&bad, &why), "bad class rejected");
    CHECK(why && std::strstr(why, "class") != nullptr, "class reason");

    bad = p;
    bad.grants[0].tag[0] = '\0'; /* empty tag */
    why = nullptr;
    CHECK(!r3_layout_validate(&bad, &why), "missing tag rejected");
    CHECK(why && std::strstr(why, "tag") != nullptr, "tag reason");
}

/* L7: every grant the CPU zeroes/writes must end inside the BAR0 window. */
static void test_l7_bar0_reachability()
{
    R3LayoutInputs in = machine_inputs();
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "default plan ok");
    for (uint32_t i = 0; i < p.grant_count; i++)
        if (p.grants[i].needs_cpu)
            CHECK(p.grants[i].end <= p.bar0_limit, "%s inside BAR0", p.grants[i].tag);

    R3LayoutPlan bad = p;
    bad.bar0_limit = 0x4000000ull; /* pretend BAR0 ends at the GOP guard */
    const char *why = nullptr;
    CHECK(!r3_layout_validate(&bad, &why), "CPU grant outside BAR0 rejected");
    CHECK(why && std::strstr(why, "bar0") != nullptr, "BAR0 reason, got %s",
          why ? why : "(null)");

    in.bar0_size = 192ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "192 MiB BAR0 plan ok");
    CHECK(p.bar0_limit == 192ull * MiB, "bar0_limit 192 MiB");
    CHECK(p.sdma_mes_moved, "SDMA/MES moved to fit 192 MiB BAR0");
    for (uint32_t i = 0; i < p.grant_count; i++)
        if (p.grants[i].needs_cpu)
            CHECK(p.grants[i].end <= 192ull * MiB, "%s inside 192 MiB BAR0",
                  p.grants[i].tag);

    /* 128 MiB is exactly the preferred TMR slot end, so a 64 MiB TMR still has a legal order
     * (TMR at 0x08000000 is not usable here; it is re-placed below and C5 follows). L7 must
     * still hold for every CPU grant. */
    in.bar0_size = 128ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "128 MiB BAR0 still has a plan");
    CHECK(p.bar0_limit == 128ull * MiB, "bar0_limit 128 MiB");
    for (uint32_t i = 0; i < p.grant_count; i++)
        if (p.grants[i].needs_cpu)
            CHECK(p.grants[i].end <= 128ull * MiB, "%s inside 128 MiB BAR0", p.grants[i].tag);

    in = machine_inputs();
    in.tmr_size = 192ull * MiB;
    p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "F6 plan ok");
    CHECK(!p.tmr_in_bar0, "F6 TMR outside BAR0");
    const R3Grant *tmr = grant(p, "tmr");
    CHECK(tmr && !tmr->needs_cpu, "F6 TMR does not need CPU access");
}

/* R4 §4.4/§4.5 interval graph golden for the normal case. */
static void test_interval_graph_golden()
{
    R3LayoutInputs in = machine_inputs();
    R3LayoutPlan p = {};
    CHECK(r3_layout_compute(&in, &p) == 0 && p.ok, "golden plan ok");
    CHECK(p.grant_count == 7u, "seven grants, got %u", p.grant_count);
    CHECK(p.gop_guard_end == 0x04000000ull, "C0 end 64 MiB");
    CHECK(p.top_guard == 0xfff00000ull, "top guard 1 MiB floor");
    CHECK(p.bar0_limit == 0x10000000ull, "BAR0 256 MiB");
    CHECK(p.limit == 0x10000000ull, "layout ceiling 256 MiB");

    expect_grant(p, "fw_buf", 0x04000000ull, 0x05000000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_ring", 0x05000000ull, 0x05004000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_cmd", 0x05004000ull, 0x05005000ull, R3Owner::Psp, true);
    expect_grant(p, "psp_fence", 0x05005000ull, 0x05006000ull, R3Owner::Psp, true);
    expect_grant(p, "tmr", 0x08000000ull, 0x0c000000ull, R3Owner::Psp, false);
    expect_grant(p, "sdma", 0x0c000000ull, 0x0c800000ull, R3Owner::Sdma, true);
    expect_grant(p, "mes", 0x0c800000ull, 0x0d000000ull, R3Owner::Mes, true);

    /* C6 (208-256 MiB) is deliberately unallocated; nothing may leak into it. */
    for (uint32_t i = 0; i < p.grant_count; i++) {
        const R3Grant &g = p.grants[i];
        CHECK(!(g.start < 0x10000000ull && g.end > 0x0d000000ull),
              "%s does not enter C6", g.tag);
    }

    CHECK(p.tmr_mc_addr == 0x8008000000ull, "tmr MC 0x8008000000");
    CHECK(p.tmr_system_phy == 0x7a8000000ull, "tmr system PA 0x7a8000000");
    CHECK(p.tmr_bar0_addr == 0x8d8000000ull, "tmr BAR0 0x8d8000000");
    CHECK(p.tmr_system_phy != 0x6a0c000000ull, "not nullarc raw");
    CHECK(p.tmr_system_phy != 0x6a0000000ull, "not nullarc archived");
}

int main()
{
    test_size_limits();
    test_address_model();
    test_gop_pitch_cases();
    test_l1_guard_not_hardcoded();
    test_l8_gop_unknown_fail_closed();
    test_l2_tmr_size_and_alignment();
    test_l3_degraded_reorder_and_f6();
    test_l4_top_guard();
    test_l5_pairwise_disjoint();
    test_l6_owner_and_class();
    test_l7_bar0_reachability();
    test_interval_graph_golden();

    if (gFailures == 0) {
        std::printf("all %d layout checks passed\n", gChecks);
        return 0;
    }
    std::printf("%d/%d layout checks FAILED\n", gFailures, gChecks);
    return 1;
}
