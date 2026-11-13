/*
 * test_sdma_msg.cpp - host state-machine test for the SDMA 6.0.1 substep (K7).
 *
 * The R3Io stub plays the whole GPU: BAR5 is a dword array (queue registers, F32_CNTL,
 * MINOR_PTR_UPDATE, STATUS), BAR0 is a 256 MiB byte array (the carveout). When the substep
 * writes RB_WPTR inside the MINOR_PTR_UPDATE window, the stub parses the staged WRITE_LINEAR
 * packets out of the ring and applies them to BAR0, exactly like the engine would - so the
 * fence poll and the byte-exact readback see real effects, and the MC addresses are checked.
 *
 * Every branch is exercised: happy path, the 13 PRE failures (I/O, environment,
 * both SDMA bases, MC base, layout, grant/owner/space/BAR0/CPU), the zero-fill,
 * source-stage, queue-register read/write I/O failures, readback-unreadable
 * and readback-mismatch,
 * status unreadable, window never opens, window stuck open, fence timeout,
 * PSP not ready, PSP latched, missing grant, and a relocated SDMA grant (the
 * substep must use the plan, never 0x0C000000).
 *
 * The planted-break guards are explicit:
 *   - `all_wptr_writes_in_window()` catches a RB_WPTR write made outside the window;
 *   - `test_readback_mismatch` catches a skipped byte comparison.
 * See tmp/programmer/K7/planted_break*.log for the recorded failures of the mutated copies.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "amdgpu_ip.h"
#include "amd/sdma_v6_0.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_wmanifest.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/golden_values.h"


using namespace amdgpu;

namespace {

constexpr uint64_t kFbMcBase = 0x8000000000ull;
constexpr uint64_t kBar0Bytes = 0x10000000ull; /* layout ceiling: 256 MiB */
constexpr uint32_t kBar5Dwords = 0x100000u;
constexpr uint32_t kSdmaBase0 = 0x20000u;
constexpr uint32_t kSdmaBase1 = 0xa000u; /* regSDMA0_F32_CNTL_BASE_IDX = 1 */

constexpr uint64_t kTmrSizeDefault = 0x4000000ull;  /* 64 MiB */
constexpr uint64_t kGopEndRelocate = 0x7f00000ull; /* 127 MiB -> 128 MiB guard, forces SDMA relocation */

static uint32_t g_fail = 0;
static uint32_t g_checks = 0;

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

/* ---------------------------------------------------------------------------------------
 * R3Io stub
 * ------------------------------------------------------------------------------------- */
struct WptrWrite {
    uint32_t dword;
    uint32_t value;
    uint32_t window;
};

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> bar5;
    uint64_t now;
    std::vector<uint32_t> writes;
    std::vector<uint32_t> reads;
    std::vector<uint64_t> bar0_writes;
    std::vector<WptrWrite> wptr_writes;
    bool window_never_opens;
    bool window_stuck_open;
    bool fence_ok;
    bool corrupt_dst;
    uint32_t fail_read_dword;
    uint32_t fail_write_dword;
    uint64_t fail_bus_write_at;
    uint64_t fail_bus_read_at;

    Dev()
        : bar0(static_cast<size_t>(kBar0Bytes), 0xa5u),
          bar5(kBar5Dwords, 0u),
          now(1000),
          window_never_opens(false),
          window_stuck_open(false),
          fence_ok(true),
          corrupt_dst(false),
          fail_read_dword(0xffffffffu),
          fail_write_dword(0xffffffffu),
          fail_bus_write_at(~0ull),
          fail_bus_read_at(~0ull)
    {
        bar5[kSdmaBase0 + kRegSdma0StatusReg] = 0x00000005u; /* IDLE | RB_EMPTY */
        bar5[kSdmaBase0 + kRegSdma0Queue0RbCntl] = 0x00040000u; /* timer=4 (nullarc) */
        bar5[kSdmaBase1 + kRegSdma0F32Cntl] = 0x08084001u;   /* HALT set (reset) */
    }
};

static Dev *g_dev = nullptr;
static R3LayoutPlan g_plan;

static uint32_t b0_32(const Dev &d, uint64_t off)
{
    uint32_t v = 0;
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 4u);
    return v;
}

static uint8_t b0_8(const Dev &d, uint64_t off)
{
    return off < static_cast<uint64_t>(d.bar0.size()) ? d.bar0[static_cast<size_t>(off)]
                                                      : 0u;
}

static void b0_put32(Dev &d, uint64_t off, uint32_t v)
{
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&d.bar0[static_cast<size_t>(off)], &v, 4u);
}

/* Play the engine: parse WRITE_LINEAR packets from the ring up to `wptr_bytes` and apply
 * them to BAR0. The packet destination is an MC address, so the MC -> BAR0 conversion is
 * exactly what the substep's address arithmetic must get right. */
static void engine_run(Dev &d, uint32_t wptr_bytes)
{
    const R3Grant *g = r3_layout_grant(&g_plan, "sdma");
    if (!g)
        return;
    const uint64_t ring = g->start + kSdmaRingOff;
    uint32_t pos = 0;
    while (pos + 16u <= wptr_bytes) {
        const uint32_t hdr = b0_32(d, ring + pos);
        const uint32_t op = hdr & kPktHeaderOpMask;
        const uint32_t sub = (hdr >> kPktHeaderSubOpShift) & 0xffu;
        if (op != kSdmaOpWrite || sub != kSdmaSubopWriteLinear)
            break;
        const uint32_t lo = b0_32(d, ring + pos + 4u);
        const uint32_t hi = b0_32(d, ring + pos + 8u);
        const uint32_t cnt = b0_32(d, ring + pos + 12u) & kWriteUntilCountMask;
        const uint32_t ndw = cnt + 1u;
        const uint64_t dst_mc = (static_cast<uint64_t>(hi) << 32) | lo;
        if (dst_mc < kFbMcBase)
            break;
        const uint64_t dst = dst_mc - kFbMcBase;
        for (uint32_t i = 0; i < ndw; i++) {
            const uint64_t at = dst + static_cast<uint64_t>(i) * 4ull;
            if (at + 4ull > static_cast<uint64_t>(d.bar0.size()))
                break;
            b0_put32(d, at, b0_32(d, ring + pos + 16u + static_cast<uint64_t>(i) * 4ull));
        }
        pos += 16u + ndw * 4u;
    }
    if (d.corrupt_dst) {
        const uint64_t dst = g->start + kSdmaDstOff;
        if (dst + 5ull < static_cast<uint64_t>(d.bar0.size()))
            d.bar0[static_cast<size_t>(dst + 5ull)] ^= 0xffu;
    }
}

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= kBar5Dwords)
        return false;

    const uint32_t minor = kSdmaBase0 + kRegSdma0Queue0MinorPtrUpdate;
    const uint32_t rb_wptr = kSdmaBase0 + kRegSdma0Queue0RbWptr;
    const uint32_t rb_wptr_hi = kSdmaBase0 + kRegSdma0Queue0RbWptrHi;

    if (write) {
        const uint32_t v = *value;
        if (dword == d.fail_write_dword)
            return false;
        d.writes.push_back(dword);
        if (dword == minor) {
            if (v == kMinorPtrUpdateMask && d.window_never_opens)
                d.bar5[dword] = 0u;
            else if (v == 0u && d.window_stuck_open)
                d.bar5[dword] = kMinorPtrUpdateMask;
            else
                d.bar5[dword] = v;
            return true;
        }

        d.bar5[dword] = v;
        if (dword == rb_wptr || dword == rb_wptr_hi) {
            WptrWrite w;
            w.dword = dword;
            w.value = v;
            w.window = d.bar5[minor] & kMinorPtrUpdateMask;
            d.wptr_writes.push_back(w);
            if (dword == rb_wptr && w.window != 0u && d.fence_ok)
                engine_run(d, v);
        }
        return true;
    }

    d.reads.push_back(dword);
    if (dword == d.fail_read_dword)
        return false;
    *value = d.bar5[dword];
    return true;
}

static bool io_smuio(void *ctx, uint32_t index, uint32_t *value, bool write)
{
    (void)ctx;
    (void)index;
    (void)value;
    (void)write;
    return false;
}

static bool io_bus(void *ctx, uint32_t bar, uint64_t off, uint64_t *value, bool write,
                   uint32_t dwords)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || bar != R3_IO_BAR_VRAM || dwords == 0u || dwords > 2u)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > static_cast<uint64_t>(d.bar0.size()))
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;
    if (write) {
        if (off == d.fail_bus_write_at)
            return false;
        memcpy(&d.bar0[static_cast<size_t>(off)], value, static_cast<size_t>(dwords) * 4u);
        d.bar0_writes.push_back(off);
    } else {
        if (off == d.fail_bus_read_at)
            return false;
        memcpy(value, &d.bar0[static_cast<size_t>(off)],
               static_cast<size_t>(dwords) * 4u);
    }
    return true;
}

static void io_udelay(void *ctx, uint32_t us)
{
    static_cast<Dev *>(ctx)->now += us;
}
static void io_mdelay(void *ctx, uint32_t ms)
{
    static_cast<Dev *>(ctx)->now += static_cast<uint64_t>(ms) * 1000ull;
}

static uint64_t io_now(void *ctx)
{
    return static_cast<Dev *>(ctx)->now;
}

static void io_lock(void *ctx)
{
    (void)ctx;
}

static void io_unlock(void *ctx)
{
    (void)ctx;
}

/* ---------------------------------------------------------------------------------------
 * Fixture
 * ------------------------------------------------------------------------------------- */
struct Prop {
    const char *key;
    char value[64];
};

struct PropStore {
    Prop props[8];
    int count;
};

static bool prop_set(void *user, const char *key, const char *bytes, size_t len)
{
    PropStore &s = *static_cast<PropStore *>(user);
    if (s.count >= 8 || len >= sizeof(s.props[0].value))
        return false;
    s.props[s.count].key = key;
    memcpy(s.props[s.count].value, bytes, len);
    s.props[s.count].value[len] = '\0';
    s.count++;
    return true;
}

struct Fixture {
    R3Rt rt;
    R3EvidenceLines ev;
    R3SubstepEnv env;
    R3Io io;
    IPBaseTable ip;
    PropStore store;
    R3EvSink sink;
};

static bool build_fixture(Fixture &f, Dev &d, bool psp_ready = true,
                          uint64_t tmr_size = kTmrSizeDefault,
                          uint64_t gop_end = 20ull * 1024ull * 1024ull)
{
    memset(&f.rt, 0, sizeof(f.rt));
    f.ip = IPBaseTable();
    f.ip.set(IPBlock::SDMA0, kSdmaBase0);
    f.ip.setBase(IPBlock::SDMA0, 1, kSdmaBase1);

    R3LayoutInputs in;
    memset(&in, 0, sizeof(in));
    in.fb_mc_base = kFbMcBase;
    in.fb_phys_base = 0x6a0c000000ull;
    in.bar0_base = 0x7c0000000ull;
    in.real_vram_size = 4096ull * 1024ull * 1024ull;
    in.vbios_reserve = 1ull * 1024ull * 1024ull;
    in.bar0_size = kBar0Bytes;
    in.gop_end = gop_end;
    in.tmr_size = tmr_size;
    in.gop_end_known = true;
    if (r3_layout_compute(&in, &f.rt.layout) != 0)
        return false;

    f.rt.ready_bits = psp_ready ? R3_IP_BIT(R3Ip::Psp) : 0u;
    f.rt.latch_bits = 0u;

    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.count = 0;
    f.sink.set_property = prop_set;
    f.sink.user = &f.store;

    memset(&f.env, 0, sizeof(f.env));
    f.env.rt = &f.rt;
    f.env.ev = &f.ev;
    f.env.sink = &f.sink;
    f.env.fb_mc_base = kFbMcBase;
    f.env.ip = &f.ip;
    f.env.hdp_flush_dword = 0x80000u;
    f.env.hdp_flush_valid = true;
    f.env.strict_expect = true;

    memset(&f.io, 0, sizeof(f.io));
    f.io.ctx = &d;
    f.io.mmio = io_mmio;
    f.io.smuio = io_smuio;
    f.io.bus = io_bus;
    f.io.udelay = io_udelay;
    f.io.mdelay = io_mdelay;
    f.io.now_us = io_now;
    f.io.lock = io_lock;
    f.io.unlock = io_unlock;
    f.io.hdp_flush = [](void *) { return true; };
    return true;
}

/* Step2: register write set from the declaration table (the single source of truth).
 * Replaces the hand-maintained kRegs[] list. */
static void register_sdma_rows(void)
{
    const uint32_t rc = getSdmaRowCount();
    const R3WriteRowReg *rows = getSdmaRows();
    for (uint32_t i = 0; i < rc; i++) {
        const uint32_t base = rows[i].base_idx == 0 ? kSdmaBase0 : kSdmaBase1;
        const uint32_t off = rows[i].golden
            ? static_cast<const r3gold::GoldenReg *>(rows[i].golden)->offset
            : rows[i].off;
        CHECK(r3_wg_register_reg(base + off, 1u, rows[i].cls, WSubstep::Sdma,
                                 "reg:sdma") == 0,
              "register sdma row %u (base 0x%x + off 0x%x)", i, base, off);
    }
}

struct RunResult {
    R3SubstepResult r;
    bool ready;
    uint64_t deny_before;
    uint64_t deny_after;
};

static RunResult run(Fixture &f, Dev &d)
{
    g_dev = &d;
    g_plan = f.rt.layout;

    d.writes.clear();
    d.reads.clear();
    d.bar0_writes.clear();
    d.wptr_writes.clear();
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.count = 0;

    r3_writeguard_install_mm();
    const R3Grant *g = r3_layout_grant(&g_plan, "sdma");
    if (g) {
        CHECK(r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                    static_cast<uint32_t>(g->size >> 2),
                                    WSubstep::Sdma, "layout:sdma") == 0,
              "register sdma grant");
    }
    register_sdma_rows();


    RunResult out;
    out.deny_before = r3_writeguard_deny_count();
    out.r = sdma_v6_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    out.ready = (f.rt.ready_bits & R3_IP_BIT(R3Ip::Sdma)) != 0u;
    return out;
}

static bool ev_has(const Fixture &f, const char *needle)
{
    (void)f;
    return strstr(n48log_host_text(), needle) != nullptr;
}

static const char *prop_get(const Fixture &f, const char *key)
{
    for (int i = 0; i < f.store.count; i++) {
        if (strcmp(f.store.props[i].key, key) == 0)
            return f.store.props[i].value;
    }
    return nullptr;
}

static bool all_wptr_writes_in_window(const Dev &d)
{
    if (d.wptr_writes.empty())
        return false;
    for (const WptrWrite &w : d.wptr_writes) {
        if (w.window == 0u)
            return false;
    }
    return true;
}

static const R3WMEntry *wm_find(uint32_t addr, WClass cls)
{
    const R3WManifest *m = r3_writeguard_manifest();
    for (uint32_t i = 0; i < m->count; i++) {
        const R3WMEntry &e = m->entries[i];
        if (e.addr == addr && e.cls == cls && e.status == WMStatus::Allow)
            return &e;
    }
    return nullptr;
}

static bool wm_has_deny(void)
{
    const R3WManifest *m = r3_writeguard_manifest();
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->entries[i].status == WMStatus::Deny)
            return true;
    }
    return false;
}

static uint32_t pattern_dword(uint32_t i)
{
    return 0x10000000u + i * 0x01010101u;
}

static void expect_copy_ok(Dev &d, const R3Grant *g)
{
    const uint64_t dst = g->start + kSdmaDstOff;
    uint32_t pattern[kSdmaCopyDwords];
    for (uint32_t i = 0; i < kSdmaCopyDwords; i++)
        pattern[i] = pattern_dword(i);
    const uint8_t *want = reinterpret_cast<const uint8_t *>(pattern);
    for (uint32_t i = 0; i < kSdmaCopyBytes; i++)
        CHECK(b0_8(d, dst + i) == want[i], "dst byte %u", i);
}

/* ---------------------------------------------------------------------------------------
 * Golden table (independent transcription of gc_11_0_0_offset.h / sh_mask.h /
 * sdma_v6_0_0_pkt_open.h; never derived from the implementation).
 * ------------------------------------------------------------------------------------- */
static void test_register_golden(void)
{
    struct Row {
        const char *name;
        uint32_t got;
        uint32_t want;
    };
    const Row rows[] = {
        {"SDMA0_STATUS_REG", kRegSdma0StatusReg, 0x0025},
        {"QUEUE0_RB_CNTL", kRegSdma0Queue0RbCntl, 0x0080},
        {"QUEUE0_RB_BASE", kRegSdma0Queue0RbBase, 0x0081},
        {"QUEUE0_RB_BASE_HI", kRegSdma0Queue0RbBaseHi, 0x0082},
        {"QUEUE0_RB_RPTR", kRegSdma0Queue0RbRptr, 0x0083},
        {"QUEUE0_RB_RPTR_HI", kRegSdma0Queue0RbRptrHi, 0x0084},
        {"QUEUE0_RB_WPTR", kRegSdma0Queue0RbWptr, 0x0085},
        {"QUEUE0_RB_WPTR_HI", kRegSdma0Queue0RbWptrHi, 0x0086},
        {"QUEUE0_RB_RPTR_ADDR_HI", kRegSdma0Queue0RbRptrAddrHi, 0x0088},
        {"QUEUE0_RB_RPTR_ADDR_LO", kRegSdma0Queue0RbRptrAddrLo, 0x0089},
        {"QUEUE0_IB_CNTL", kRegSdma0Queue0IbCntl, 0x008a},
        {"QUEUE0_RB_WPTR_POLL_ADDR_HI", kRegSdma0Queue0RbWptrPollAddrHi, 0x00b2},
        {"QUEUE0_RB_WPTR_POLL_ADDR_LO", kRegSdma0Queue0RbWptrPollAddrLo, 0x00b3},
        {"QUEUE0_MINOR_PTR_UPDATE", kRegSdma0Queue0MinorPtrUpdate, 0x00b5},
        {"SDMA0_F32_CNTL", kRegSdma0F32Cntl, 0x589a},
        {"SEM_WAIT_FAIL_TIMER_CNTL", kRegSdma0SemWaitFailTimerCntl, 0x0022},
        {"WATCHDOG_CNTL", kRegSdma0WatchdogCntl, 0x002e},
        {"UTCL1_CNTL", kRegSdma0Utcl1Cntl, 0x003c},
        {"UTCL1_PAGE", kRegSdma0Utcl1Page, 0x003f},
        {"QUEUE0_DOORBELL", kRegSdma0Queue0Doorbell, 0x0092},
    };
    for (const Row &r : rows)
        CHECK(r.got == r.want, "%s: got 0x%x want 0x%x", r.name, r.got, r.want);

    CHECK(kSdma0BaseIdx == 0, "SDMA0 RB base idx");
    CHECK(kSdmaF32BaseIdx == 1, "SDMA0 F32 base idx");
    CHECK(kSdmaOpWrite == 2, "SDMA_OP_WRITE");
    CHECK(kSdmaSubopWriteLinear == 0, "SDMA_SUBOP_WRITE_LINEAR");
    CHECK(kSdmaQueue0DoorbellEnableMask == 0x10000000u, "DOORBELL.ENABLE bit28");
    CHECK(kSdmaWatchdogHangCount == 20u, "WATCHDOG hang count 2s/100ms");
    CHECK(kWriteUntilDstLoOff == 1, "WRITE dst lo");
    CHECK(kWriteUntilDstHiOff == 2, "WRITE dst hi");
    CHECK(kWriteUntilCountOff == 3, "WRITE count");
    CHECK(kWriteUntilData0Off == 4, "WRITE data0");

    CHECK(kSdmaRingOff == 0x000000ull && kSdmaWbOff == 0x001000ull &&
              kSdmaWptrShadowOff == 0x002000ull && kSdmaSrcOff == 0x003000ull &&
              kSdmaDstOff == 0x004000ull,
          "region offsets");
    CHECK(kSdmaRegionBytes == kSdmaDstOff + kSdmaDstBytes, "region size");
    CHECK((1u << kSdmaRbSize) == kSdmaRingDwords, "RB_SIZE describes the 4 KiB ring");
    CHECK(kSdmaCopyBytes == 64u, "copy size");
}

/* ---------------------------------------------------------------------------------------
 * Happy path
 * ------------------------------------------------------------------------------------- */
static void test_happy_path(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    const R3Grant *g = r3_layout_grant(&f.rt.layout, "sdma");
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;

    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "happy status %s (%s %u)", rr.r.name,
          r3_substep_section_name(rr.r.section), rr.r.step);
    CHECK(strcmp(rr.r.name, "ok") == 0, "happy name");
    CHECK(rr.ready, "SDMA ready bit");
    CHECK(rr.deny_after == rr.deny_before, "no DENY (%llu -> %llu)",
          static_cast<unsigned long long>(rr.deny_before),
          static_cast<unsigned long long>(rr.deny_after));
    CHECK(!wm_has_deny(), "manifest has no DENY");
    CHECK(!r3_writeguard_manifest()->truncated, "manifest not truncated");

    const uint64_t ring = g->start + kSdmaRingOff;
    const uint64_t wb = g->start + kSdmaWbOff;
    const uint64_t shadow = g->start + kSdmaWptrShadowOff;
    const uint64_t src = g->start + kSdmaSrcOff;
    const uint64_t dst = g->start + kSdmaDstOff;
    const uint64_t fence = g->start + kSdmaFenceOff;
    const uint64_t ring_mc = kFbMcBase + ring;
    const uint64_t rptr_wb_mc = kFbMcBase + g->start + kSdmaRptrWbOff;
    const uint64_t shadow_mc = kFbMcBase + shadow;
    const uint64_t dst_mc = kFbMcBase + dst;
    const uint64_t fence_mc = kFbMcBase + fence;

    /* S1: the three zeroed regions end zero, not 0xA5. */
    CHECK(b0_8(d, ring + kSdmaRingBytes - 1u) == 0u, "ring zeroed");
    CHECK(b0_8(d, wb + kSdmaWbBytes - 1u) == 0u, "wb zeroed");
    CHECK(b0_8(d, shadow + kSdmaWptrShadowBytes - 1u) == 0u, "shadow zeroed");
    {
        uint64_t shadow_qword = 0;
        memcpy(&shadow_qword, &d.bar0[static_cast<size_t>(shadow)], 8u);
        CHECK(shadow_qword == 100u, "S3 shadow copy holds wptr 100 (got %llu)",
              static_cast<unsigned long long>(shadow_qword));
    }

    /* S2: source buffer and packet golden (independent layout). */
    for (uint32_t i = 0; i < kSdmaCopyDwords; i++)
        CHECK(b0_32(d, src + i * 4u) == pattern_dword(i), "source dword %u", i);

    uint32_t exp[25];
    uint32_t n = 0;
    exp[n++] = kSdmaOpWrite | (kSdmaSubopWriteLinear << kPktHeaderSubOpShift);
    exp[n++] = static_cast<uint32_t>(dst_mc);
    exp[n++] = static_cast<uint32_t>(dst_mc >> 32);
    exp[n++] = kSdmaCopyDwords - 1u;
    for (uint32_t i = 0; i < kSdmaCopyDwords; i++)
        exp[n++] = pattern_dword(i);
    exp[n++] = kSdmaOpWrite | (kSdmaSubopWriteLinear << kPktHeaderSubOpShift);
    exp[n++] = static_cast<uint32_t>(fence_mc);
    exp[n++] = static_cast<uint32_t>(fence_mc >> 32);
    exp[n++] = 0u;
    exp[n++] = kSdmaFenceMagic;
    CHECK(n == 25u, "packet dword count");
    for (uint32_t i = 0; i < n; i++)
        CHECK(b0_32(d, ring + i * 4u) == exp[i], "ring packet dword %u: 0x%08x != 0x%08x",
              i, b0_32(d, ring + i * 4u), exp[i]);
    CHECK(d.wptr_writes.size() == 2u, "WPTR lo+hi writes (%zu)",
          d.wptr_writes.size());
    CHECK(all_wptr_writes_in_window(d), "every WPTR write is inside the window");

    /* S3: queue0 programming. */
    const uint32_t rb_cntl = kSdmaBase0 + kRegSdma0Queue0RbCntl;
    const uint32_t rb_base = kSdmaBase0 + kRegSdma0Queue0RbBase;
    const uint32_t rb_base_hi = kSdmaBase0 + kRegSdma0Queue0RbBaseHi;
    const uint32_t rb_rptr = kSdmaBase0 + kRegSdma0Queue0RbRptr;
    const uint32_t rb_rptr_hi = kSdmaBase0 + kRegSdma0Queue0RbRptrHi;
    const uint32_t rb_wptr = kSdmaBase0 + kRegSdma0Queue0RbWptr;
    const uint32_t rb_wptr_hi = kSdmaBase0 + kRegSdma0Queue0RbWptrHi;
    const uint32_t rptr_addr_hi = kSdmaBase0 + kRegSdma0Queue0RbRptrAddrHi;
    const uint32_t rptr_addr_lo = kSdmaBase0 + kRegSdma0Queue0RbRptrAddrLo;
    const uint32_t poll_hi = kSdmaBase0 + kRegSdma0Queue0RbWptrPollAddrHi;
    const uint32_t poll_lo = kSdmaBase0 + kRegSdma0Queue0RbWptrPollAddrLo;
    const uint32_t ib_cntl = kSdmaBase0 + kRegSdma0Queue0IbCntl;
    const uint32_t minor = kSdmaBase0 + kRegSdma0Queue0MinorPtrUpdate;
    const uint32_t f32_cntl = kSdmaBase1 + kRegSdma0F32Cntl;

    CHECK(d.bar5[rb_cntl] == 0x00841815u, "RB_CNTL 0x%08x", d.bar5[rb_cntl]);
    CHECK(d.bar5[rb_base] == static_cast<uint32_t>(ring_mc >> 8), "RB_BASE 0x%08x",
          d.bar5[rb_base]);
    CHECK(d.bar5[rb_base_hi] == static_cast<uint32_t>(ring_mc >> 40), "RB_BASE_HI");
    CHECK(d.bar5[rb_rptr] == 0u && d.bar5[rb_rptr_hi] == 0u, "RPTR zero");
    CHECK(d.bar5[rptr_addr_hi] == static_cast<uint32_t>(rptr_wb_mc >> 32), "RPTR_ADDR_HI");
    CHECK(d.bar5[rptr_addr_lo] == (static_cast<uint32_t>(rptr_wb_mc) & 0xfffffffcu),
          "RPTR_ADDR_LO 0x%08x", d.bar5[rptr_addr_lo]);
    CHECK(d.bar5[poll_hi] == static_cast<uint32_t>(shadow_mc >> 32), "WPTR_POLL_HI");
    CHECK(d.bar5[poll_lo] == static_cast<uint32_t>(shadow_mc), "WPTR_POLL_LO");
    CHECK(d.bar5[f32_cntl] == 0x08084000u, "F32_CNTL unhalt 0x%08x", d.bar5[f32_cntl]);
    CHECK(d.bar5[ib_cntl] == 0x00000001u, "IB_CNTL enable 0x%x", d.bar5[ib_cntl]);
    CHECK(d.bar5[minor] == 0u, "window closed");
    CHECK(d.bar5[rb_wptr] == 100u, "WPTR kick 0x%x", d.bar5[rb_wptr]);
    CHECK(d.bar5[rb_wptr_hi] == 0u, "WPTR_HI zero");
    const uint32_t sem_timer = kSdmaBase0 + kRegSdma0SemWaitFailTimerCntl;
    const uint32_t watchdog = kSdmaBase0 + kRegSdma0WatchdogCntl;
    const uint32_t doorbell = kSdmaBase0 + kRegSdma0Queue0Doorbell;
    CHECK(d.bar5[sem_timer] == 0u, "SEM_WAIT_FAIL_TIMER_CNTL cleared");
    CHECK(d.bar5[watchdog] == (kSdmaWatchdogHangCount & kSdmaWatchdogHangCountMask),
          "WATCHDOG_CNTL hang count 0x%x", d.bar5[watchdog]);
    CHECK((d.bar5[doorbell] & kSdmaQueue0DoorbellEnableMask) == 0u,
          "QUEUE0_DOORBELL.ENABLE cleared");

    /* S4: the manifest line for the WPTR write carries both window readings. */
    const R3WMEntry *w = wm_find(rb_wptr, WClass::Protocol);
    CHECK(w != nullptr, "manifest WPTR entry");
    if (w) {
        CHECK(strstr(w->ctx, "pre=1") != nullptr, "manifest WPTR pre reading: %s",
              w->ctx);
        CHECK(strstr(w->ctx, "post=0") != nullptr, "manifest WPTR post reading: %s",
              w->ctx);
    }
    const R3WMEntry *rc = wm_find(rb_cntl, WClass::State);
    CHECK(rc != nullptr, "manifest RB_CNTL state entry");
    const R3WMEntry *data = nullptr;
    {
        const R3WManifest *m = r3_writeguard_manifest();
        for (uint32_t i = 0; i < m->count; i++) {
            if (m->entries[i].cls == WClass::Data &&
                strcmp(m->entries[i].tag, "layout:sdma") == 0) {
                data = &m->entries[i];
                break;
            }
        }
    }
    CHECK(data != nullptr, "manifest aggregated Data entry for layout:sdma");

    /* S5/S6: fence and byte-exact target. */
    CHECK(b0_32(d, fence) == kSdmaFenceMagic, "fence magic 0x%08x", b0_32(d, fence));
    expect_copy_ok(d, g);

    /* Evidence and N48R3EVID-* chunks. */
    CHECK(ev_has(f, "S1 zero ring"), "evidence S1");
    CHECK(ev_has(f, "S2 copy"), "evidence S2");
    CHECK(ev_has(f, "S3 wptr shadow=100"), "evidence S3 shadow");
    CHECK(ev_has(f, "S4 queue config"), "evidence S4 config");
    CHECK(ev_has(f, "S4 WPTR window pre=1 post=0"), "evidence S4 window");
    CHECK(ev_has(f, "S6 fence=0x"), "evidence S6");
    CHECK(ev_has(f, "S7 target copy ok"), "evidence S7");
    CHECK(ev_has(f, "S8 SDMA0_STATUS_REG=0x"), "evidence S8");
    CHECK(ev_has(f, "S8b sdma0_version"), "evidence S8b version observation");
    const char *done = prop_get(f, "N48R3EVID-sdma-done");
    const char *copy = prop_get(f, "N48R3EVID-sdma-copy");
    const char *wptr = prop_get(f, "N48R3EVID-sdma-wptr");
    CHECK(done && strcmp(done, "Yes") == 0, "done prop");
    CHECK(copy && strcmp(copy, "ok") == 0, "copy prop");
    CHECK(wptr && strstr(wptr, "pre=1") && strstr(wptr, "post=0"), "wptr prop: %s",
          wptr ? wptr : "(null)");
}

/* ---------------------------------------------------------------------------------------
 * Failure branches
 * ------------------------------------------------------------------------------------- */
static void test_psp_not_ready(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, false), "fixture");
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "pre:psp-not-ready") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty() && d.bar0_writes.empty(), "zero writes before PSP ready");
    CHECK(!rr.ready, "no ready bit");
    CHECK(rr.deny_after == rr.deny_before, "no DENY");
    const char *done = prop_get(f, "N48R3EVID-sdma-done");
    CHECK(done && strcmp(done, "No") == 0, "done=No");
}

static void test_psp_latched(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.rt.latch_bits = R3_IP_BIT(R3Ip::Psp);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "pre:psp-latched") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty() && d.bar0_writes.empty(), "zero writes when latched");
    CHECK(!rr.ready, "no ready bit");
}

static void test_no_grant(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    bool renamed = false;
    for (uint32_t i = 0; i < f.rt.layout.grant_count; i++) {
        R3Grant &g = f.rt.layout.grants[i];
        if (strcmp(g.tag, "sdma") == 0) {
            strcpy(g.tag, "sdma-x");
            renamed = true;
        }
    }
    CHECK(renamed, "sdma grant renamed");
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "pre:sdma-grant") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty() && d.bar0_writes.empty(), "zero writes without grant");
    CHECK(!rr.ready, "no ready bit");
}

static R3Grant *find_sdma_grant(R3LayoutPlan &plan)
{
    for (uint32_t i = 0; i < plan.grant_count; i++) {
        if (strcmp(plan.grants[i].tag, "sdma") == 0)
            return &plan.grants[i];
    }
    return nullptr;
}

static void expect_pre_fail(const RunResult &rr, const Dev &d, const char *name)
{
    CHECK(rr.r.status == kR3SsecFail, "%s: must fail", name);
    CHECK(strcmp(rr.r.name, name) == 0, "%s: name got %s", name, rr.r.name);
    CHECK(d.writes.empty() && d.bar0_writes.empty(), "%s: zero writes", name);
    CHECK(!rr.ready, "%s: no ready bit", name);
    CHECK(rr.deny_after == rr.deny_before, "%s: no DENY", name);
}

static void test_pre_no_io(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.io.bus = nullptr;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:no-io");
}

static void test_pre_no_env(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.env.rt = nullptr;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:no-env");
}

static void test_pre_sdma_base(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.ip = IPBaseTable();
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:sdma-base");
}

static void test_pre_f32_base(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.ip = IPBaseTable();
    f.ip.set(IPBlock::SDMA0, kSdmaBase0);
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:f32-base");
}

static void test_pre_no_mc_base(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.env.fb_mc_base = 0u;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:no-mc-base");
}

static void test_pre_layout(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    f.rt.layout.ok = false;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:layout-invalid");
}

static void test_pre_owner(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    g->owner = R3Owner::Smu;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:sdma-owner");
}

static void test_pre_space(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    g->size = kSdmaRegionBytes - 4u;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:sdma-space");
}

static void test_pre_bar0(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    f.rt.layout.bar0_limit = g->start + kSdmaRegionBytes - 1u;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:sdma-bar0");
}

static void test_pre_no_cpu(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    g->needs_cpu = false;
    RunResult rr = run(f, d);
    expect_pre_fail(rr, d, "pre:sdma-cpu");
}

static void test_zero_fill_failure(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    const R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    d.fail_bus_write_at = g->start + kSdmaRingOff;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "seq:zero-fill") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S1 zero ring/wb/shadow failed"), "evidence zero failure");
}

static void test_queue_regs_failure(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.fail_read_dword = kSdmaBase0 + kRegSdma0Queue0RbCntl;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "seq:queue-regs") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S4 RB_CNTL read failed"), "evidence queue failure");
}

static void test_source_stage_failure(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    const R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    d.fail_bus_write_at = g->start + kSdmaSrcOff;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "seq:ring-packet") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S2 source buffer stage failed"), "evidence source failure");
}

static void test_queue_write_failure(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.fail_write_dword = kSdmaBase0 + kRegSdma0Queue0RbCntl;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "seq:queue-regs") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S4 queue0 pointer/base write failed"), "evidence write failure");
}

static void test_readback_unreadable(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    const R3Grant *g = find_sdma_grant(f.rt.layout);
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    d.fail_bus_read_at = g->start + kSdmaDstOff;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "chk:readback-unreadable") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S7 target read failed"), "evidence read failure");
    const char *copy = prop_get(f, "N48R3EVID-sdma-copy");
    CHECK(copy && strcmp(copy, "unreadable") == 0, "copy prop unreadable");
}

static void test_window_never_opens(void)
{
    // SDMA-03: the window readback is evidence only. The substep proceeds and writes
    // RB_WPTR anyway; the engine never sees an open window, so the fence times out.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.window_never_opens = true;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail (engine never runs)");
    CHECK(strcmp(rr.r.name, "seq:fence-timeout") == 0, "name %s", rr.r.name);
    CHECK(d.wptr_writes.size() == 2u, "WPTR still written without a gating readback");
    CHECK(!all_wptr_writes_in_window(d), "writes happened outside the window");
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S4 WPTR window pre=0"), "window readback recorded");
    CHECK(ev_has(f, "S6 fence timeout"), "evidence fence timeout");
}

static void test_window_read_unreadable()
{
    // SDMA-03: the window readbacks are evidence only. Even an unreadable
    // MINOR_PTR_UPDATE must not gate the copy; the writes still happen.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.fail_read_dword = kSdmaBase0 + kRegSdma0Queue0MinorPtrUpdate;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "window read failure must not gate: %s", rr.r.name);
    CHECK(d.wptr_writes.size() == 2u, "WPTR written despite unreadable window");
    CHECK(all_wptr_writes_in_window(d), "WPTR writes stayed in the open window");
    CHECK(ev_has(f, "pre-read unreadable"), "pre-read failure recorded");
}

static void test_window_stuck_open(void)
{
    // SDMA-03: a stuck-open window is recorded, never gated. The copy itself succeeds.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.window_stuck_open = true;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "stuck-open readback must not gate: %s", rr.r.name);
    CHECK(d.wptr_writes.size() == 2u, "WPTR written inside the window");
    CHECK(all_wptr_writes_in_window(d), "WPTR writes in window");
    CHECK(rr.ready, "ready bit");
    CHECK(ev_has(f, "S4 WPTR window pre=1 post=1"), "post readback recorded");
    const char *wptr = prop_get(f, "N48R3EVID-sdma-wptr");
    CHECK(wptr && strstr(wptr, "post=1"), "wptr prop keeps post=1: %s",
          wptr ? wptr : "(null)");
}

static void test_fence_timeout(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.fence_ok = false;
    const uint64_t t0 = d.now;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "seq:fence-timeout") == 0, "name %s", rr.r.name);
    CHECK(d.now - t0 >= kSdmaFenceBudgetUs, "bounded by the 2 s budget (%llu us)",
          static_cast<unsigned long long>(d.now - t0));
    const R3Grant *g = r3_layout_grant(&f.rt.layout, "sdma");
    CHECK(g && b0_32(d, g->start + kSdmaFenceOff) == 0u, "fence never written");
    CHECK(!rr.ready, "no ready bit");
    CHECK(ev_has(f, "S6 fence timeout"), "evidence fence timeout");
}

static void test_readback_mismatch(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.corrupt_dst = true;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "chk:readback-mismatch") == 0, "name %s", rr.r.name);
    CHECK(rr.r.reading_count >= 2u, "readings carry expected/actual (%u)",
          rr.r.reading_count);
    bool saw_expected = false;
    bool saw_actual = false;
    for (uint32_t i = 0; i < rr.r.reading_count; i++) {
        if (strcmp(rr.r.readings[i].what, "expected") == 0)
            saw_expected = true;
        if (strcmp(rr.r.readings[i].what, "actual") == 0)
            saw_actual = true;
    }
    CHECK(saw_expected && saw_actual, "expected/actual readings");
    CHECK(!rr.ready, "no ready bit");
    const char *copy = prop_get(f, "N48R3EVID-sdma-copy");
    CHECK(copy && strcmp(copy, "mismatch") == 0, "copy prop mismatch");
    CHECK(ev_has(f, "S7 target mismatch"), "evidence mismatch");
}

static void test_status_unreadable(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.fail_read_dword = kSdmaBase0 + kRegSdma0StatusReg;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "must fail");
    CHECK(strcmp(rr.r.name, "chk:status") == 0, "name %s", rr.r.name);
    CHECK(!rr.ready, "no ready bit");
    const char *copy = prop_get(f, "N48R3EVID-sdma-copy");
    CHECK(copy && strcmp(copy, "ok") == 0, "copy itself succeeded");
    CHECK(ev_has(f, "S8 SDMA0_STATUS_REG"), "evidence status failure");
}

/* D-08: the F6 fallback (>128 MiB TMR) is no longer reachable — TMR > 64 MiB fails
 * closed. Instead, a 128 MiB GOP guard pushes the TMR into the C5 slot, forcing
 * SDMA/MES to relocate. The substep must still read the address from the plan. */
static void test_relocated_grant(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, kTmrSizeDefault, kGopEndRelocate), "fixture");
    const R3Grant *g = r3_layout_grant(&f.rt.layout, "sdma");
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    CHECK(g->start != R3_C5_SDMA_OFF, "sdma grant moved to 0x%llx",
          static_cast<unsigned long long>(g->start));
    CHECK(f.rt.layout.sdma_mes_moved, "SDMA/MES relocated by layout");
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "relocated status %s", rr.r.name);
    CHECK(rr.ready, "relocated ready bit");
    expect_copy_ok(d, g);
    CHECK(ev_has(f, "S7 target copy ok"), "relocated copy evidence");
}

} /* namespace */

int main(void)
{
    test_register_golden();
    test_happy_path();
    test_psp_not_ready();
    test_psp_latched();
    test_no_grant();
    test_pre_no_io();
    test_pre_no_env();
    test_pre_sdma_base();
    test_pre_f32_base();
    test_pre_no_mc_base();
    test_pre_layout();
    test_pre_owner();
    test_pre_space();
    test_pre_bar0();
    test_pre_no_cpu();
    test_zero_fill_failure();
    test_queue_regs_failure();
    test_source_stage_failure();
    test_queue_write_failure();
    test_readback_unreadable();
    test_window_never_opens();
    test_window_read_unreadable();
    test_window_stuck_open();
    test_fence_timeout();
    test_readback_mismatch();
    test_status_unreadable();
    test_relocated_grant();

    printf("test_sdma_msg: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
