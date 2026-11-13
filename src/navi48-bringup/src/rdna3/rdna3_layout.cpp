/*
 * rdna3_layout.cpp - the RDNA3 carveout layout allocator (B3/B4/A5, D3/D4/D14/D15).
 *
 * Pure functions only: no MMIO, no IOKit, no allocation. See rdna3_layout.h for the address
 * model, the L1-L8 rules and the region skeleton this file implements. The allocator reads
 * nothing; every input is a stage-0 survey observation supplied by the caller.
 *
 * Placement order: C1 fw_buf, C2 PSP ring/cmd/fence directly after it, C4 TMR (preferred
 * slot, else the lowest aligned hole, else the F6 fallback above 0x10000000), then C5
 * SDMA/MES at their fixed interval unless the TMR occupies it (then L3 moves them up). Every
 * hole search only consults the grants already placed, so the result cannot contradict L5;
 * each plan is re-checked with r3_layout_validate() before it is published.
 *
 * Failure is fail closed: a failed r3_layout_compute() publishes no grants at all (grant_count
 * 0) and a static fail_reason, so a caller that ignores the return code still cannot use a
 * partial plan.
 */
#include "rdna3_layout.h"

#include <string.h>

namespace {

/* L3/F6 fallback floor: a natural-aligned start at or above the BAR0 ceiling. */
constexpr uint64_t kTmrFallbackOff = R3_BAR0_LAYOUT_LIMIT;

constexpr uint64_t kGrantAlign = 4096ull; /* every grant start is page aligned for zeroing */

/* Grant names (L6). Static storage: a plan never owns its tag. */
constexpr char kTagFwBuf[] = "fw_buf";
constexpr char kTagRing[] = "psp_ring";
constexpr char kTagCmd[] = "psp_cmd";
constexpr char kTagFence[] = "psp_fence";
constexpr char kTagTmr[] = "tmr";
constexpr char kTagSdma[] = "sdma";
constexpr char kTagMes[] = "mes";

/* Fail reasons (static; the caller logs them verbatim, E1). */
constexpr const char *kFailGop = "gop-range-unconfirmed";
constexpr const char *kFailTmr = "tmr-size-invalid";
constexpr const char *kFailTmrExceedsSlot = "tmr-exceeds-slot";
constexpr const char *kFailTop = "top-guard-invalid";
constexpr const char *kFailRoom = "no-room";
constexpr const char *kFailInput = "invalid-input";
uint64_t align_up(uint64_t v, uint64_t align)
{
    if (align == 0)
        return v;
    const uint64_t rem = v % align;
    return rem ? v + (align - rem) : v;
}

uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

bool fits(uint64_t start, uint64_t size, uint64_t limit)
{
    return start <= limit && size <= limit - start;
}

/* D-08: the TMR start alignment is 1 MiB (Linux PSP_TMR_ALIGNMENT = 1 MiB,
 * amdgpu_psp.c:913-917). The previous natural-alignment (max(1 MiB, tmr_size)) was
 * stricter than Linux and had no two-source basis. A TMR larger than the 64 MiB
 * reserved slot fails closed (D-08: R > slot → fail). */
uint64_t tmr_align(uint64_t tmr_size)
{
    (void)tmr_size;
    return R3_L3_MIB;
}

/* The placed grants this file hands to the hole search: the prefix of plan->grants that has
 * already been decided. `n` is the number of valid entries. */
struct Placed {
    const R3Grant *grants;
    uint32_t n;
};

/* First `align`-aligned `size`-byte hole at or above `from` that does not overlap any placed
 * grant and ends at or below `limit`. `align` is the
 * caller's minimum (a page for ordinary grants, max(1 MiB, tmr_size) for the TMR), so the
 * returned start satisfies both. */
bool place_hole_aligned(const Placed &p, uint64_t from, uint64_t size, uint64_t align,
                        uint64_t limit, uint64_t *out)
{
    if (size == 0 || !fits(from, size, limit))
        return false;
    uint64_t s = align_up(from, align);
    for (uint32_t iter = 0; iter <= p.n; iter++) {
        if (!fits(s, size, limit))
            return false;
        bool bumped = false;
        for (uint32_t i = 0; i < p.n; i++) {
            const R3Grant &g = p.grants[i];
            if (s < g.end && g.start < s + size) {
                s = align_up(g.end, align);
                bumped = true;
            }
        }
        if (!bumped) {
            *out = s;
            return true;
        }
    }
    return false;
}

/* First page-aligned `size`-byte hole at or above `from` that does not overlap any placed
 * grant and ends at or below `limit`. A candidate that
 * only touches an obstacle at its endpoint overlaps nothing, and any positive intersection
 * bumps the cursor past the obstacle, so the search terminates in at most n+1 rounds. */
bool place_hole(const Placed &p, uint64_t from, uint64_t size, uint64_t limit, uint64_t *out)
{
    return place_hole_aligned(p, from, size, kGrantAlign, limit, out);
}

/* One grant appended to the plan under construction. */
bool add_grant(R3LayoutPlan *p, uint32_t *n, const char *tag, uint64_t start, uint64_t end,
               R3Owner owner, bool needs_cpu)
{
    if (*n >= R3_LAYOUT_MAX_GRANTS || end <= start)
        return false;
    R3Grant &g = p->grants[*n];
    strncpy(g.tag, tag, sizeof(g.tag) - 1);
    g.tag[sizeof(g.tag) - 1] = '\0';
    g.start = start;
    g.end = end;
    g.size = end - start;
    g.owner = owner;
    g.cls = R3B2Class::DataMove; /* every carveout allocation is ② data movement */
    g.needs_cpu = needs_cpu;
    (*n)++;
    return true;
}

/* Fail closed: no grants are published on any failure path. */
int fail(R3LayoutPlan *out, const char *reason)
{
    out->ok = false;
    out->fail_reason = reason;
    out->grant_count = 0;
    return -1;
}

} // namespace

int r3_layout_compute(const R3LayoutInputs *in, R3LayoutPlan *out)
{
    if (!in || !out) {
        if (out)
            fail(out, kFailInput);
        return -1;
    }
    *out = R3LayoutPlan{};

    /* L8: the measured scanout range is a hard gate. The 64 MiB default is never a stand-in. */
    if (!in->gop_end_known)
        return fail(out, kFailGop);

    /* L2: the TMR size comes only from the PSP LOAD_TOC response. */
    if (in->tmr_size == 0)
        return fail(out, kFailTmr);
    /* D-08: a TMR larger than the 64 MiB reserved slot fails closed. The slot is the
     * layout's conservative ceiling; a larger value means the firmware behaviour changed
     * and the operator must investigate (nullarc measured exactly 64 MiB). */
    if (in->tmr_size > R3_TMR_SLOT_SIZE)
        return fail(out, kFailTmrExceedsSlot);
    /* L4: [top_guard, real_vram_size) is no-write. */
    if (in->real_vram_size == 0 || in->vbios_reserve > in->real_vram_size)
        return fail(out, kFailTop);

    /* L1: the GOP guard follows the measured scanout end with 16 MiB granularity. */
    const uint64_t gop_guard_end = align_up(in->gop_end, R3_GOP_GUARD_ALIGN);
    out->gop_guard_end = gop_guard_end < R3_GOP_GUARD_MIN ? R3_GOP_GUARD_MIN : gop_guard_end;

    const uint64_t top_reserve =
        in->vbios_reserve > R3_TOPGUARD_MIN ? in->vbios_reserve : R3_TOPGUARD_MIN;
    out->top_guard = in->real_vram_size - top_reserve;
    /* L4: every grant ends at or below min(256 MiB, top_guard). The BAR0 window is the third
     * bound (L7 the CPU grants must also fit it), kept separate so the plan reports both. */
    out->bar0_limit = min_u64(in->bar0_size, out->top_guard);
    out->limit = min_u64(R3_BAR0_LAYOUT_LIMIT, out->bar0_limit);
    out->tmr_in_bar0 = true;

    uint32_t n = 0;
    Placed placed{out->grants, 0};

    /* C1: firmware staging 16 MiB (nullarc fw_buf). */
    const uint64_t fw_buf = out->gop_guard_end;
    if (!fits(fw_buf, R3_FW_BUF_SIZE, out->limit) ||
        !add_grant(out, &n, kTagFwBuf, fw_buf, fw_buf + R3_FW_BUF_SIZE, R3Owner::Psp, true))
        return fail(out, kFailRoom);
    placed.n = n;

    /* C2: PSP ring / cmd / fence, packed back to back after fw_buf. Each grant spans its own
     * zero-fill area (ring 16 KiB, cmd/fence 4 KiB each), so L5's disjointness covers the
     * clears. This is tighter than the nullarc +0/+0x4000/+0x8000 offsets
     * (PhxBringup.cpp:888-895): with those, the 16 KiB ring clear would reach the cmd buffer. */
    const uint64_t psp_base = fw_buf + R3_FW_BUF_SIZE;
    const uint64_t ring = psp_base;
    const uint64_t cmd = ring + R3_PSP_RING_ZERO;
    const uint64_t fence = cmd + R3_PSP_CMD_ZERO;
    if (!fits(fence, R3_PSP_FENCE_ZERO, out->limit) ||
        !add_grant(out, &n, kTagRing, ring, ring + R3_PSP_RING_ZERO, R3Owner::Psp, true) ||
        !add_grant(out, &n, kTagCmd, cmd, cmd + R3_PSP_CMD_ZERO, R3Owner::Psp, true) ||
        !add_grant(out, &n, kTagFence, fence, fence + R3_PSP_FENCE_ZERO, R3Owner::Psp, true))
        return fail(out, kFailRoom);
    placed.n = n;

    const uint64_t tmr_align_bytes = tmr_align(in->tmr_size);
    const uint64_t psp_end = fence + R3_PSP_FENCE_ZERO;

    /* C4/L3 step 1: where does the TMR go?
     *   (i)  the preferred slot 0x08000000 when it fits below the ceiling and is free;
     *   (ii) otherwise the lowest natural-aligned hole at or above the GOP guard;
     *   (iii) nothing below the ceiling (TMR > 128 MiB, or no aligned hole): the F6 fallback
     *         above 0x10000000, which the CPU cannot zero or check through BAR0. */
    uint64_t tmr_start = 0;
    bool tmr_in_bar0 = false;
    if (in->tmr_size <= R3_TMR_BAR0_MAX) {
        tmr_in_bar0 = R3_TMR_PREFERRED % tmr_align_bytes == 0 &&
                      fits(R3_TMR_PREFERRED, in->tmr_size, out->limit) &&
                      place_hole_aligned(placed, R3_TMR_PREFERRED, in->tmr_size, tmr_align_bytes,
                                         out->limit, &tmr_start) &&
                      tmr_start == R3_TMR_PREFERRED;
        if (!tmr_in_bar0)
            tmr_in_bar0 = place_hole_aligned(placed, out->gop_guard_end, in->tmr_size,
                                             tmr_align_bytes, out->limit, &tmr_start);
    }
    if (!tmr_in_bar0) {
        /* F6: strictly above the BAR0 ceiling, but still below the top guard — requirement B3
         * forbids overlapping the discovery TMR at the top of VRAM (it sits in the last 64 KiB,
         * inside the min 1 MiB top guard) and the PSP runtime DB area. A TMR that cannot be
         * placed even there has no legal position: fail closed, never shrink or relocate the
         * top protection. */
        tmr_start = align_up(kTmrFallbackOff + 1, tmr_align_bytes);
        if (!fits(tmr_start, in->tmr_size, out->top_guard))
            return fail(out, kFailRoom);
    }

    out->tmr_in_bar0 = tmr_in_bar0;
    out->tmr_reordered = tmr_start != R3_TMR_PREFERRED;
    /* Publish the TMR grant before placing SDMA/MES so the hole search sees it as an
     * obstacle and never overlaps it (D-08 exposed this when the TMR stays in BAR0
     * instead of going to the F6 fallback). */
    if (!add_grant(out, &n, kTagTmr, tmr_start, tmr_start + in->tmr_size, R3Owner::Psp, false))
        return fail(out, kFailRoom);
    placed.n = n;

    /* C5/L3 step 2: SDMA/MES keep the fixed interval unless the TMR occupies part of it, it
     * does not fit below the ceiling, or the TMR went to the F6 area; then L3 "移前" applies
     * and they are packed right after the PSP region. */
    uint64_t sdma_start = 0;
    uint64_t mes_start = 0;
    const bool c5_must_move =
        !tmr_in_bar0 ||
        (tmr_start < R3_C5_MES_OFF + R3_C5_SIZE && R3_C5_SDMA_OFF < tmr_start + in->tmr_size) ||
        !fits(R3_C5_SDMA_OFF, R3_C5_SIZE, out->limit) ||
        !fits(R3_C5_MES_OFF, R3_C5_SIZE, out->limit);
    if (!c5_must_move && fits(R3_C5_SDMA_OFF, R3_C5_SIZE, out->limit) &&
        fits(R3_C5_MES_OFF, R3_C5_SIZE, out->limit) &&
        place_hole(placed, R3_C5_SDMA_OFF, R3_C5_SIZE, out->limit, &sdma_start) &&
        sdma_start == R3_C5_SDMA_OFF &&
        place_hole(placed, R3_C5_MES_OFF, R3_C5_SIZE, out->limit, &mes_start) &&
        mes_start == R3_C5_MES_OFF) {
        out->sdma_mes_moved = false;
    } else {
        if (!place_hole(placed, psp_end, R3_C5_SIZE, out->limit, &sdma_start) ||
            !place_hole(placed, sdma_start + R3_C5_SIZE, R3_C5_SIZE, out->limit, &mes_start) ||
            mes_start + R3_C5_SIZE > out->limit)
            return fail(out, kFailRoom);
        out->sdma_mes_moved = true;
    }

    /* Step 3: publish SDMA and MES (TMR already added above). */
    if (!add_grant(out, &n, kTagSdma, sdma_start, sdma_start + R3_C5_SIZE, R3Owner::Sdma, true) ||
        !add_grant(out, &n, kTagMes, mes_start, mes_start + R3_C5_SIZE, R3Owner::Mes, true))
        return fail(out, kFailRoom);
    out->grant_count = n; /* lookups below see the full set */

    /* L2/L3 derived TMR fields: the PSP descriptor needs all three address forms. */
    const R3Grant *tmr = r3_layout_grant(out, kTagTmr);
    out->tmr_size = in->tmr_size;
    out->tmr_mc_addr = r3_layout_mc_addr(in->fb_mc_base, tmr->start);
    out->tmr_system_phy = r3_layout_system_phy(in->fb_phys_base, tmr->start);
    out->tmr_bar0_addr = out->tmr_in_bar0 ? r3_layout_bar0_addr(in->bar0_base, tmr->start) : 0ull;

    out->ok = true;
    out->fail_reason = nullptr;

    /* L5/L6/L7 on our own output: never publish a plan that breaks a rule. */
    const char *why = nullptr;
    if (!r3_layout_validate(out, &why)) {
        out->ok = false;
        out->fail_reason = why;
        out->grant_count = 0;
        return -1;
    }
    return 0;
}

bool r3_layout_validate(const R3LayoutPlan *plan, const char **why)
{
    if (why)
        *why = nullptr;
    if (!plan)
        return false;
    if (plan->grant_count > R3_LAYOUT_MAX_GRANTS) {
        if (why)
            *why = "tag-count";
        return false;
    }
    for (uint32_t i = 0; i < plan->grant_count; i++) {
        const R3Grant &g = plan->grants[i];
        /* L6: tag, owner substep, B2 class. */
        if (g.tag[0] == '\0') {
            if (why)
                *why = "tag-empty";
            return false;
        }
        if (static_cast<unsigned>(g.owner) > static_cast<unsigned>(R3Owner::Mes)) {
            if (why)
                *why = "owner-out-of-range";
            return false;
        }
        if (static_cast<unsigned>(g.cls) > static_cast<unsigned>(R3B2Class::StateConfig)) {
            if (why)
                *why = "class-out-of-range";
            return false;
        }
        if (g.end <= g.start || g.size != g.end - g.start) {
            if (why)
                *why = "tag-extent";
            return false;
        }
        /* L7: anything the CPU zeroes or writes must end inside the BAR0 window. */
        if (g.needs_cpu && g.end > plan->bar0_limit) {
            if (why)
                *why = "bar0-unreachable";
            return false;
        }
        /* L5 with the zero-fill rule (PM ruling): the ring grant must own the whole 16 KiB
         * the ring driver clears, otherwise its clear would cross into a neighbour. */
        if (strcmp(g.tag, kTagRing) == 0 && g.size < R3_PSP_RING_ZERO) {
            if (why)
                *why = "overlap";
            return false;
        }
        /* D-08: the TMR start is 1 MiB aligned (Linux PSP_TMR_ALIGNMENT). */
        if (strcmp(g.tag, kTagTmr) == 0 && plan->tmr_size != 0 &&
            g.start % R3_L3_MIB != 0) {
            if (why)
                *why = "align-tmr";
            return false;
        }
        /* L5: pairwise disjoint, including the zero-fill extension range. */
        for (uint32_t j = i + 1; j < plan->grant_count; j++) {
            const R3Grant &o = plan->grants[j];
            /* Each interval spans its own zero-fill area (C2 note), so L5 compares the
             * grant intervals directly. */
            const uint64_t i_end = g.end;
            const uint64_t j_end = o.end;
            if (g.start < j_end && o.start < i_end) {
                if (why)
                    *why = "overlap";
                return false;
            }
        }
    }
    return true;
}

const R3Grant *r3_layout_grant(const R3LayoutPlan *plan, const char *tag)
{
    if (!plan || !tag)
        return nullptr;
    for (uint32_t i = 0; i < plan->grant_count && i < R3_LAYOUT_MAX_GRANTS; i++)
        if (strcmp(plan->grants[i].tag, tag) == 0)
            return &plan->grants[i];
    return nullptr;
}

uint64_t r3_layout_mc_addr(uint64_t fb_mc_base, uint64_t off) { return fb_mc_base + off; }

uint64_t r3_layout_system_phy(uint64_t fb_phys_base, uint64_t off) { return fb_phys_base + off; }

uint64_t r3_layout_bar0_addr(uint64_t bar0_base, uint64_t off) { return bar0_base + off; }

/* T8: reject the nullarc system_phy values before comparing (tech plan §4.4). The raw value
 * 0x6A0C000000 comes from a runtime FB_OFFSET that was never read on that machine, and the
 * archived 0x6A0000000 contradicts it; on this machine the value is recomputed from
 * MMMC_VM_FB_OFFSET << 24 and nothing else. */
bool r3_layout_system_phy_valid(uint64_t fb_phys_base, uint64_t off, uint64_t system_phy)
{
    constexpr uint64_t kNullarcRaw = 0x6a0c000000ull;
    constexpr uint64_t kNullarcArchived = 0x6a0000000ull;
    if (system_phy == kNullarcRaw || system_phy == kNullarcArchived)
        return false;
    return system_phy == r3_layout_system_phy(fb_phys_base, off);
}

const char *r3_b2_class_name(R3B2Class cls)
{
    switch (cls) {
    case R3B2Class::AccessEnable:
        return "access-enable";
    case R3B2Class::DataMove:
        return "data-move";
    case R3B2Class::Protocol:
        return "protocol";
    case R3B2Class::StateConfig:
        return "state-config";
    }
    return "unknown";
}

const char *r3_owner_name(R3Owner owner)
{
    switch (owner) {
    case R3Owner::Psp:
        return "psp";
    case R3Owner::Smu:
        return "smu";
    case R3Owner::Gfx:
        return "gfx";
    case R3Owner::Sdma:
        return "sdma";
    case R3Owner::Mes:
        return "mes";
    }
    return "unknown";
}
