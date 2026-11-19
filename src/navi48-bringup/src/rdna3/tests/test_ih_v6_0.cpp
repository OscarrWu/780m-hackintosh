/*
 * test_ih_v6_0.cpp - host tests for the IH 6.0.1 substep (S10).
 *
 * Part 1: ih_decode_iv golden cases (amdgpu_ih.c:263-296 semantics).
 * Part 2: substep behaviour against a Dev stub (pattern: test_mes_api.cpp).
 * The stub plays the GPU: BAR5 is a dword array (OSSSYS/MMHUB registers),
 * BAR0 is the carveout window. On the final RB_CNTL enable write the stub
 * can simulate the IH hardware firing one IV entry (register wptr + ring
 * entry + writeback shadow) or corrupting the writeback shadow.
 *
 * Branches: full chain, PSP-not-ready, missing ih_ring grant, FB_OFFSET=0,
 * RB_CNTL readback fault, entry extra (gated, never fatal), writeback
 * shadow mismatch.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "amdgpu_ip.h"
#include "amd/ih_v6_0.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/golden_values.h"

using namespace amdgpu;


/* The substep entry + declaration-table getters live in ih_v6_0.cpp; the
 * shared contract header (rdna3_ip_entries.h) belongs to T-E, so declare
 * them here for the host test. */
namespace amdgpu {
R3SubstepResult ih_v6_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg *getIhRows(void);
uint32_t getIhRowCount(void);
const R3WriteRowLayout *getIhLayoutRows(void);
uint32_t getIhLayoutRowCount(void);
const char *getIhLayoutTag(void);
} /* namespace amdgpu */
namespace {

constexpr uint64_t kFbMcBase = 0x8000000000ull;
constexpr uint64_t kFbPhysBase = 0x6a0c000000ull;
constexpr uint64_t kBar0Base = 0x7c0000000ull;
constexpr uint64_t kBar0Bytes = 0x10000000ull;
constexpr uint32_t kBar5Dwords = 0x200000u;
constexpr uint32_t kMmhub0 = 0x30000u;
constexpr uint32_t kOsssys0 = 0x40000u;

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

/* ---- Dev stub ---------------------------------------------------------- */

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> bar5;
    std::vector<char> kick_order;
    struct Db {
        uint32_t index;
        uint64_t value;
        uint32_t dwords;
    };
    std::vector<Db> doorbells;
    std::vector<uint32_t> writes;
    uint64_t now;
    uint32_t cntl_writes;
    bool fail_readback; /* fail RB_CNTL reads after the enable writes */
    bool corrupt_wb;    /* garbage into the wptr shadow at enable */
    bool fire_entry;    /* simulate one IV entry at enable */
    bool hdp_ok;

    Dev()
        : bar0(static_cast<size_t>(kBar0Bytes), 0xa5u),
          bar5(kBar5Dwords, 0u),
          now(1000),
          cntl_writes(0),
          fail_readback(false),
          corrupt_wb(false),
          fire_entry(false),
          hdp_ok(true)
    {
    }
};

/* Register/offset addresses the stub needs (set by run()). */
static uint32_t g_rb_cntl;
static uint32_t g_rb_wptr;
static uint64_t g_ring_off;
static uint64_t g_wb_off;

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= kBar5Dwords)
        return false;
    if (write) {
        const uint32_t v = *value;
        d.writes.push_back(dword);
        d.bar5[dword] = v;
        if (dword == g_rb_cntl && ++d.cntl_writes == 5u) {
            /* Fifth RB_CNTL write = the final enable form (S10.0 once,
             * S10.2 once, S10.3 three times): the stub IH fires. */
            if (d.fire_entry) {
                const uint32_t iv[8] = {0x0500B50Au, 0x12345678u, 0x0000ABCDu,
                                        0x00561234u, 0xDEADBEEFu, 0xCAFEBABEu,
                                        0x11111111u, 0x22222222u};
                for (uint32_t i = 0; i < 8u; i++)
                    memcpy(&d.bar0[static_cast<size_t>(g_ring_off) + i * 4u],
                           &iv[i], 4u);
                const uint32_t wptr = 32u; /* one entry, byte counter */
                d.bar5[g_rb_wptr] = wptr;
                memcpy(&d.bar0[static_cast<size_t>(g_wb_off)], &wptr, 4u);
            }
            if (d.corrupt_wb) {
                const uint32_t bad = 0xDEADu;
                memcpy(&d.bar0[static_cast<size_t>(g_wb_off)], &bad, 4u);
            }
        }
        return true;
    }
    if (d.fail_readback && dword == g_rb_cntl && d.cntl_writes >= 5u)
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

static bool io_bus(void *ctx, uint32_t bar, uint64_t off, uint64_t *value,
                   bool write, uint32_t dwords)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value)
        return false;
    if (bar == 2u) { /* doorbell aperture: IH must never ring one */
        d.doorbells.push_back({static_cast<uint32_t>(off >> 2), *value, dwords});
        return true;
    }
    if (bar != R3_IO_BAR_VRAM)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > static_cast<uint64_t>(d.bar0.size()))
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;
    if (write)
        memcpy(&d.bar0[static_cast<size_t>(off)], value,
               static_cast<size_t>(dwords) * 4u);
    else
        memcpy(value, &d.bar0[static_cast<size_t>(off)],
               static_cast<size_t>(dwords) * 4u);
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
static void io_lock(void *ctx) { (void)ctx; }
static void io_unlock(void *ctx) { (void)ctx; }

struct Prop {
    const char *key;
    char value[256];
};

struct PropStore {
    Prop props[12];
    int count;
};

static bool prop_set(void *user, const char *key, const char *bytes, size_t len)
{
    PropStore &s = *static_cast<PropStore *>(user);
    if (s.count >= 12 || len >= sizeof(s.props[0].value))
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

static bool build_fixture(Fixture &f, Dev &d, bool psp_ready)
{
    memset(&f.rt, 0, sizeof(f.rt));
    f.ip = IPBaseTable();
    f.ip.set(IPBlock::OSSSYS, kOsssys0);
    f.ip.set(IPBlock::MMHUB, kMmhub0);
    f.ip.setBase(IPBlock::MMHUB, 1, 0x1a000u); /* mmhub seg1 for golden registers */

    /* MMMC_VM_FB_OFFSET=0x6a0c -> fb_phys_base=0x6a0c000000;
     * FB_LOCATION_BASE=0x8000 -> mc_base=0x8000000000 (T8 cross-check). */
    const uint32_t mmhub1 = 0x1a000u;
    d.bar5[mmhub1 + 0x08D7u] = 0x6a0cu;
    d.bar5[mmhub1 + 0x08ECu] = 0x8000u;
    d.bar5[mmhub1 + 0x08EDu] = 0x80ffu;

    R3LayoutInputs in;
    memset(&in, 0, sizeof(in));
    in.fb_mc_base = kFbMcBase;
    in.fb_phys_base = kFbPhysBase;
    in.bar0_base = kBar0Base;
    in.real_vram_size = 4096ull * 1024ull * 1024ull;
    in.vbios_reserve = 1ull * 1024ull * 1024ull;
    in.bar0_size = kBar0Bytes;
    in.gop_end = 20ull * 1024ull * 1024ull;
    in.tmr_size = 0x4000000ull;
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
    f.io.fence = [](void *ctx) {
        static_cast<Dev *>(ctx)->kick_order.push_back('f');
    };
    f.io.unlock = io_unlock;
    f.io.hdp_flush = [](void *c) { return static_cast<Dev *>(c)->hdp_ok; };
    return true;
}

/* Step2: register the write set from the declaration table. All IH rows are
 * OSSSYS base_idx 0 (golden) or the chicken fallback row. */
static void register_ih_rows(void)
{
    const uint32_t rc = getIhRowCount();
    const R3WriteRowReg *rows = getIhRows();
    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr;
        if (rows[i].golden) {
            const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
            addr = kOsssys0 + g->offset;
        } else {
            addr = kOsssys0 + rows[i].off;
        }
        CHECK(r3_wg_register_reg(addr, 1u, rows[i].cls, WSubstep::Ih, "reg:ih") == 0,
              "register ih row %u (addr 0x%x)", i, addr);
    }
}

/* Layout plan handed to the writeguard (pattern: test_mes_api.cpp). */
static R3LayoutPlan g_plan_layout;

struct RunResult {
    R3SubstepResult r;
    uint64_t deny_before;
    uint64_t deny_after;
};

static RunResult run(Fixture &f)
{
    g_plan_layout = f.rt.layout;

    r3_writeguard_install_mm();
    const R3Grant *ring = r3_layout_grant(&g_plan_layout, "ih_ring");
    const R3Grant *wb = r3_layout_grant(&g_plan_layout, "ih_rptr_wb");
    if (ring) {
        CHECK(r3_wg_register_layout(static_cast<uint32_t>(ring->start >> 2),
                                    static_cast<uint32_t>(ring->size >> 2),
                                    WSubstep::Ih, "layout:ih-ring") == 0,
              "register ih_ring grant");
    }
    if (wb) {
        CHECK(r3_wg_register_layout(static_cast<uint32_t>(wb->start >> 2),
                                    static_cast<uint32_t>(wb->size >> 2),
                                    WSubstep::Ih, "layout:ih-wb") == 0,
              "register ih_rptr_wb grant");
    }
    register_ih_rows();

    /* Stub addresses: RB_CNTL/RB_WPTR dword addresses + carveout offsets. */
    g_rb_cntl = kOsssys0 + r3gold::kIhRbCntl.offset;
    g_rb_wptr = kOsssys0 + r3gold::kIhRbWptr.offset;
    g_ring_off = ring ? ring->start : 0ull;
    g_wb_off = wb ? wb->start : 0ull;

    RunResult out;
    out.deny_before = r3_writeguard_deny_count();
    out.r = ih_v6_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    return out;
}

static bool prop_get(const Fixture &f, const char *key, char *out, size_t cap)
{
    for (int i = 0; i < f.store.count; i++) {
        if (strcmp(f.store.props[i].key, key) == 0) {
            snprintf(out, cap, "%s", f.store.props[i].value);
            return true;
        }
    }
    return false;
}

static bool ev_has(const Fixture &f, const char *tag)
{
    (void)f;
    return n48log_host_has_tag(tag);
}

static uint32_t b0_32(const Dev &d, uint64_t off)
{
    uint32_t v = 0;
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 4u);
    return v;
}

/* Expected final RB_CNTL (S10.3): MC_SPACE=2<<28 | MC_SNOOP | WPTR_OVERFLOW_EN
 * | WPTR_WRITEBACK | RB_SIZE=14<<1 | RB_ENABLE=1; CLEAR=0, ENABLE_INTR=0
 * (polling), MC_RO=0, MC_VMID=0, RPTR_REARM=0. */
constexpr uint32_t kExpectCntl = (2u << 28) | (1u << 20) | (1u << 16) |
                                 (1u << 8) | (14u << 1) | (1u << 0);
constexpr uint32_t kExpectDb = (kIhDoorbellIndex & kIhDoorbellRptrOffsetMask) |
                               kIhDoorbellRptrEnable;

/* ---- Part 1: decoder golden cases -------------------------------------- */

static void test_decode_golden(void)
{
    /* Case 1: full-field entry. dw0 = client 0x0A | src 0xB5<<8 | ring 0<<16
     * | vmid 5<<24 | vmid_src 0<<31. */
    {
        const uint32_t iv[8] = {0x0500B50Au, 0x12345678u, 0x0000ABCDu,
                                0x00561234u, 0xDEADBEEFu, 0xCAFEBABEu,
                                0x11111111u, 0x22222222u};
        IhEntry e;
        memset(&e, 0xAA, sizeof(e));
        ih_decode_iv(iv, &e);
        CHECK(e.client_id == 0x0Au, "client_id 0x%x", e.client_id);
        CHECK(e.src_id == 0xB5u, "src_id 0x%x", e.src_id);
        CHECK(e.ring_id == 0x00u, "ring_id 0x%x", e.ring_id);
        CHECK(e.vmid == 5u, "vmid %u", e.vmid);
        CHECK(e.vmid_src == 0u, "vmid_src %u", e.vmid_src);
        CHECK(e.timestamp == 0x0000ABCD12345678ull, "timestamp 0x%llx",
              static_cast<unsigned long long>(e.timestamp));
        CHECK(e.timestamp_src == 0u, "timestamp_src %u", e.timestamp_src);
        CHECK(e.pasid == 0x1234u, "pasid 0x%x", e.pasid);
        CHECK(e.node_id == 0x56u, "node_id 0x%x", e.node_id);
        CHECK(e.src_data[0] == 0xDEADBEEFu && e.src_data[1] == 0xCAFEBABEu &&
                  e.src_data[2] == 0x11111111u && e.src_data[3] == 0x22222222u,
              "src_data");
    }

    /* Case 2: upper bits - vmid_src=1, timestamp_src=1, dw2[30:16] ignored
     * by the timestamp, pasid=0, node_id=0xFF. */
    {
        const uint32_t iv[8] = {0xFFFFFFFFu, 0x00000001u, 0x80001234u,
                                0xFFFF0000u, 0u, 0u, 0u, 0u};
        IhEntry e;
        memset(&e, 0, sizeof(e));
        ih_decode_iv(iv, &e);
        CHECK(e.client_id == 0xFFu && e.src_id == 0xFFu && e.ring_id == 0xFFu,
              "dw0 upper fields");
        CHECK(e.vmid == 0xFu, "vmid %u", e.vmid);
        CHECK(e.vmid_src == 1u, "vmid_src %u", e.vmid_src);
        CHECK(e.timestamp == 0x0000123400000001ull, "timestamp 0x%llx",
              static_cast<unsigned long long>(e.timestamp));
        CHECK(e.timestamp_src == 1u, "timestamp_src %u", e.timestamp_src);
        CHECK(e.pasid == 0u, "pasid %u", e.pasid);
        CHECK(e.node_id == 0xFFu, "node_id 0x%x", e.node_id);
    }

    /* Case 3: zero entry decodes to all zero. */
    {
        const uint32_t iv[8] = {0};
        IhEntry e;
        memset(&e, 0xAA, sizeof(e));
        ih_decode_iv(iv, &e);
        CHECK(e.client_id == 0u && e.src_id == 0u && e.ring_id == 0u &&
                  e.vmid == 0u && e.vmid_src == 0u && e.timestamp == 0ull &&
                  e.timestamp_src == 0u && e.pasid == 0u && e.node_id == 0u &&
                  e.src_data[0] == 0u && e.src_data[3] == 0u,
              "zero entry");
    }

    /* Case 4: null entry pointer is a no-op (no crash). */
    {
        const uint32_t iv[8] = {0};
        ih_decode_iv(iv, nullptr);
        CHECK(true, "null entry tolerated");
    }
}

/* ---- Part 2: substep tests --------------------------------------------- */

static void check_common_success(Fixture &f, Dev &d, RunResult &rr)
{
    CHECK(rr.r.status == kR3SsecOk, "status %d name %s", rr.r.status,
          rr.r.name ? rr.r.name : "?");
    CHECK(rr.deny_before == rr.deny_after, "deny count changed");
    CHECK(d.doorbells.empty(), "IH must not ring doorbells");
    CHECK(d.kick_order.empty(), "IH substep makes no fence calls");
    CHECK(rr.r.reading_count == 4u, "readings %u", rr.r.reading_count);
    if (rr.r.reading_count == 4u) {
        CHECK(rr.r.readings[0].addr == g_rb_cntl &&
                  strcmp(rr.r.readings[0].what, "RB_CNTL") == 0,
              "reading 0");
        CHECK(rr.r.readings[0].value == kExpectCntl, "reading 0 value 0x%08x",
              rr.r.readings[0].value);
        CHECK(rr.r.readings[1].addr == kOsssys0 + r3gold::kIhRbBase.offset &&
                  strcmp(rr.r.readings[1].what, "RB_BASE") == 0,
              "reading 1");
        CHECK(rr.r.readings[2].addr == kOsssys0 + r3gold::kIhDoorbellRptr.offset &&
                  strcmp(rr.r.readings[2].what, "DOORBELL_RPTR") == 0,
              "reading 2");
        CHECK(rr.r.readings[2].value == kExpectDb, "reading 2 value 0x%08x",
              rr.r.readings[2].value);
        CHECK(strcmp(rr.r.readings[3].what, "WB wptr") == 0, "reading 3");
    }
    CHECK((f.rt.ready_bits & R3_IP_BIT(R3Ip::Psp)) != 0u, "psp bit kept");
    CHECK((f.rt.ready_bits & ~R3_IP_BIT(R3Ip::Psp)) == 0u,
          "IH adds no ready bit (0x%08x)", f.rt.ready_bits);

    char val[256];
    CHECK(prop_get(f, "N48R3EVID-ih-done", val, sizeof(val)) &&
              strcmp(val, "Yes") == 0,
          "ih-done prop");
    CHECK(ev_has(f, "ih-pre") && ev_has(f, "ih-seq") && ev_has(f, "ih-chk") &&
              ev_has(f, "ih"),
          "evidence tags");

    /* Register-level assertions (planted-break guards). */
    const uint64_t ring_gpa = kFbPhysBase + g_ring_off;
    const uint64_t wb_gpa = kFbPhysBase + g_wb_off;
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbBase.offset] ==
              static_cast<uint32_t>(ring_gpa >> 8),
          "RB_BASE 0x%08x",
          d.bar5[kOsssys0 + r3gold::kIhRbBase.offset]);
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbBaseHi.offset] ==
              static_cast<uint32_t>((ring_gpa >> 40) & 0xffu),
          "RB_BASE_HI");
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbWptrAddrLo.offset] ==
              static_cast<uint32_t>(wb_gpa & 0xffffffffull),
          "WPTR_ADDR_LO");
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbWptrAddrHi.offset] ==
              static_cast<uint32_t>((wb_gpa >> 32) & 0xffffu),
          "WPTR_ADDR_HI");
    CHECK(d.bar5[kOsssys0 + kRegIhChicken] == (1u << 4), "IH_CHICKEN");
    CHECK(d.bar5[g_rb_cntl] == kExpectCntl, "final RB_CNTL 0x%08x",
          d.bar5[g_rb_cntl]);
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbRptr.offset] == 0u, "RB_RPTR");
}


static void test_happy_path(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    RunResult rr = run(f);
    check_common_success(f, d, rr);
    /* Idle-ring invariants that the fired-entry case intentionally breaks. */
    CHECK(d.bar5[kOsssys0 + r3gold::kIhRbWptr.offset] == 0u, "RB_WPTR");
    CHECK(b0_32(d, g_wb_off) == 0u && b0_32(d, g_wb_off + 4) == 0u,
          "wb shadows zeroed");

    char val[256];
    CHECK(prop_get(f, "N48R3EVID-ih-entries", val, sizeof(val)) &&
              strcmp(val, "none") == 0,
          "ih-entries prop");
}

static void test_psp_not_ready(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, false), "fixture");
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status");
    CHECK(strcmp(rr.r.name, "pre:psp-not-ready") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty(), "no writes on pre failure");
    CHECK(rr.deny_before == rr.deny_after, "deny count changed");
}

static void test_grant_missing(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    /* Corrupt the ih_ring grant tag so the lookup fails. */
    bool renamed = false;
    for (uint32_t i = 0; i < f.rt.layout.grant_count; i++) {
        if (strcmp(f.rt.layout.grants[i].tag, "ih_ring") == 0) {
            f.rt.layout.grants[i].tag[0] = '\0';
            renamed = true;
        }
    }
    CHECK(renamed, "grant rename");
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status");
    CHECK(strcmp(rr.r.name, "pre:ih-grant") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty(), "no writes on pre failure");
}

static void test_fb_offset_zero(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    d.bar5[0x1a000u + 0x08D7u] = 0u; /* MMMC_VM_FB_OFFSET = 0 */
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status");
    CHECK(strcmp(rr.r.name, "pre:fb-offset") == 0, "name %s", rr.r.name);
    CHECK(d.writes.empty(), "no writes on pre failure");
}

static void test_readback_fault(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    d.fail_readback = true; /* RB_CNTL reads fail after the enable writes */
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status");
    CHECK(strcmp(rr.r.name, "chk:ih-readback") == 0, "name %s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk, "section");
}

static void test_entry_extra(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    d.fire_entry = true;
    RunResult rr = run(f);
    check_common_success(f, d, rr);
    /* The entry extra must not fail the substep; readings[3] carries the
     * live wptr shadow (32 = one entry). */
    CHECK(rr.r.readings[3].value == 32u, "wb wptr %u",
          rr.r.readings[3].value);
    char val[256];
    CHECK(prop_get(f, "N48R3EVID-ih-entries", val, sizeof(val)) &&
              strcmp(val, "client=0x0a src=0xb5") == 0,
          "ih-entries prop (%s)", val);
}

static void test_writeback_mismatch(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, true), "fixture");
    d.corrupt_wb = true;
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status");
    CHECK(strcmp(rr.r.name, "chk:ih-readback") == 0, "name %s", rr.r.name);
}

} /* namespace */

int main(void)
{
    test_decode_golden();
    test_happy_path();
    test_psp_not_ready();
    test_grant_missing();
    test_fb_offset_zero();
    test_readback_fault();
    test_entry_extra();
    test_writeback_mismatch();

    printf("test_ih_v6_0: %u checks, %u failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
