/*
 * test_gmc.cpp - host tests for the GMC substeps (stage-3 batch T-B, S6..S9).
 *
 * Coverage:
 *   - pure functions: PTE/PDE bit shapes, four-level walk construction,
 *     GART entry builders, VMID 8..15 LRU lending;
 *   - S6 uma: MC aperture recheck + sysmem_grants backfill (read-only);
 *   - S7 gart: VMID0 flat configuration on both hubs + GART table contents +
 *     SDMA WRITE_LINEAR through the GART VA (emulated walker);
 *   - S8 gpuvm0: nullarc vmBind sequence + R10 absolute cross-check + SDMA
 *     verification through the four-level GPUVM VA;
 *   - S9 vmid8: VM_CONTEXT8 sequence + PER_VMID invalidate + FAULT_DEFAULT.
 *
 * The fake device mirrors test_mes_api.cpp: BAR5 is the MMIO dword space,
 * BAR0 is the VRAM window. The SDMA engine is emulated: when the driver
 * clears MINOR_PTR_UPDATE the stub replays WRITE_LINEAR packets from the
 * ring, translating the destination VA through the very page tables the
 * substep built (the same walk the GPU would perform).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "amdgpu_ip.h"
#include "amd/gmc_v11_0.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_wmanifest.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/golden_values.h"

using namespace amdgpu;

namespace {

constexpr uint64_t kFbMcBase = 0x8000000000ull;
constexpr uint64_t kBar0Bytes = 0x10000000ull;
constexpr uint32_t kBar5Dwords = 0x200000u;
/* GC seg0 base 0x1260 puts GCVM_CONTEXT0_CNTL at absolute dword 0x28E8,
 * matching the nullarc R10 measurements the substep cross-checks against. */
constexpr uint32_t kGc0 = 0x1260u;
constexpr uint32_t kGc1 = 0x20000u;
constexpr uint32_t kMmhub0 = 0x30000u;
constexpr uint32_t kMmhub1 = 0x1a000u;
constexpr uint32_t kSdma0 = 0x50000u;
constexpr uint32_t kSdma1 = 0x60000u;
constexpr uint64_t kFbPhysBase = 0x6a0c000000ull;

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

/* ---- fake device ---- */
struct TraceEnt {
    char kind;          /* 'B' sdma-window write, 'Z' other BAR0 write, 'R' readback, 'M' mmio kick, 'F' hdp flush */
    uint32_t dword;
};

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> bar5;
    uint64_t now;
    std::vector<uint32_t> writes;
    uint32_t sdma_rptr;
    bool sdma_translate;
    uint32_t stuck_dword; /* accepts the write but latches nothing (readback stays stale) */

    /* S7 device-visibility model */
    std::vector<uint8_t> bar0_copy;   /* device/MCU view, updated only by hdp_flush */
    std::vector<TraceEnt> trace;
    size_t flush_at;                  /* index of the last hdp flush in trace */
    bool flush_copies;                /* false: flush invalidates but does not publish */
    bool ring_read_corrupt;           /* readback flips the third packet dword */
    bool shadow_read_stale;           /* shadow readback returns stale_shadow */
    uint32_t stale_shadow;            /* pre-flush shadow value */
    uint32_t mcu_polls;
    uint8_t mcu_line[64];             /* MCU fetch line cache */
    uint64_t mcu_line_off;
    bool mcu_line_valid;
    Dev()
        : bar0(static_cast<size_t>(kBar0Bytes), 0xa5u), bar5(kBar5Dwords, 0u),
          now(1000), sdma_rptr(0), sdma_translate(true), stuck_dword(0xffffffffu),
          bar0_copy(static_cast<size_t>(kBar0Bytes), 0u),
          flush_at(static_cast<size_t>(-1)), flush_copies(true),
          ring_read_corrupt(false), shadow_read_stale(false), stale_shadow(0u),
          mcu_polls(0u), mcu_line_off(0u), mcu_line_valid(false)
    {
        memset(mcu_line, 0, sizeof(mcu_line));
    }
};

/* ---- fake sysmem allocator: three contiguous pools at fake phys ---- */
struct FakeMem {
    std::vector<uint8_t> buf[3];
    uint64_t phys[3];
    FakeMem()
    {
        for (int i = 0; i < 3; i++)
            phys[i] = 0;
    }
};
static FakeMem g_mem;

static uint64_t fake_alloc(void *ctx, uint64_t bytes, void **cpu)
{
    (void)ctx;
    for (int i = 0; i < 3; i++) {
        if (g_mem.phys[i] == 0) {
            g_mem.buf[i].assign(static_cast<size_t>(bytes), 0);
            g_mem.phys[i] = 0x1000000000ull + static_cast<uint64_t>(i) * 0x1000000ull;
            if (cpu)
                *cpu = g_mem.buf[i].data();
            return g_mem.phys[i];
        }
    }
    return 0;
}
static GmcSysmemAlloc g_fake_alloc = { nullptr, fake_alloc };

/* Each sequence test re-runs S6, which allocates all three pools again. */
static void reset_fake_mem(void)
{
    for (int i = 0; i < 3; i++)
        g_mem.phys[i] = 0;
}

/* ---- VA translation (mirrors the GPU walker for the SDMA emulation) ---- */
static const uint8_t *mem_for(uint64_t pa, uint64_t *off)
{
    for (int i = 0; i < 3; i++) {
        if (g_mem.phys[i] != 0 && pa >= g_mem.phys[i] &&
            pa < g_mem.phys[i] + g_mem.buf[i].size()) {
            *off = pa - g_mem.phys[i];
            return g_mem.buf[i].data();
        }
    }
    return nullptr;
}

static uint64_t translate_va(uint64_t va)
{
    if (va < (1ull << 32)) {
        /* flat GART (S7) */
        if (g_mem.phys[0] == 0)
            return ~0ull;
        uint64_t pte;
        memcpy(&pte, g_mem.buf[0].data() + static_cast<size_t>(gmc_gart_index(va)) * 8, 8);
        if (!(pte & kGmcPtValid))
            return ~0ull;
        return (pte & kGmcPtAddrMask) + (va & 0xfffull);
    }
    /* four-level walk (S8/S9): the root lives at the start of the ctx0 pool */
    if (g_mem.phys[1] == 0)
        return ~0ull;
    uint64_t pa = g_mem.phys[1];
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t off;
        const uint8_t *base = mem_for(pa, &off);
        if (!base)
            return ~0ull;
        uint64_t e;
        memcpy(&e, base + off + static_cast<size_t>((va >> shift) & 511u) * 8, 8);
        if (!(e & kGmcPtValid))
            return ~0ull;
        pa = e & kGmcPtAddrMask;
    }
    return pa + (va & 0xfffull);
}

/* ---- SDMA emulation: MCU line cache + wptr shadow polling ---- */
static uint64_t g_sdma_ring_off = 0; /* BAR0 byte offset of the SDMA ring */

/* gmc_v11_0.cpp keeps this offset file-local; the fixtures repeat it here. */
static constexpr uint32_t kSdmaShadowOff = 0x2000u;

static bool in_sdma_window(uint64_t off)
{
    return g_sdma_ring_off != 0u &&
           ((off >= g_sdma_ring_off && off < g_sdma_ring_off + 0x1000ull) ||
            (off >= g_sdma_ring_off + kSdmaShadowOff &&
             off < g_sdma_ring_off + kSdmaShadowOff + 8ull));
}

static uint32_t mcu_read32(Dev &d, uint64_t off)
{
    const uint64_t line = off & ~static_cast<uint64_t>(63u);
    if (!d.mcu_line_valid || line != d.mcu_line_off) {
        if (line + 64u > d.bar0_copy.size())
            return 0u;
        memcpy(d.mcu_line, &d.bar0_copy[static_cast<size_t>(line)], 64u);
        d.mcu_line_off = line;
        d.mcu_line_valid = true;
    }
    uint32_t v = 0u;
    memcpy(&v, d.mcu_line + static_cast<size_t>(off - d.mcu_line_off), 4u);
    return v;
}

static void mcu_poll(Dev &d)
{
    if (!d.sdma_translate)
        return;
    d.mcu_polls++;
    if (g_sdma_ring_off == 0u)
        return;

    /* The engine fetches the wptr shadow from its own (uncached) view; without
     * a successful flush this is the stale line, so the queue never advances. */
    const uint32_t shadow = mcu_read32(d, g_sdma_ring_off + kSdmaShadowOff);
    const uint32_t wptr_raw = shadow & ~3u;
    if (wptr_raw > kGmcSdmaRingBytes)
        return; /* a value this emulation cannot walk (garbage shadow) */
    const uint32_t target = wptr_raw % kGmcSdmaRingBytes;
    const uint32_t remaining =
        (target + kGmcSdmaRingBytes - d.sdma_rptr) % kGmcSdmaRingBytes;
    if (remaining == 0u || (remaining % kGmcSdmaPacketBytes) != 0u)
        return; /* caught up, or a partial packet the engine would not walk */

    while (d.sdma_rptr != target) {
        uint32_t pkt[kGmcSdmaPacketDwords];
        for (uint32_t i = 0; i < kGmcSdmaPacketDwords; i++) {
            const uint64_t off = g_sdma_ring_off +
                (d.sdma_rptr + i * 4u) % kGmcSdmaRingBytes;
            pkt[i] = mcu_read32(d, off);
        }
        if (pkt[0] == kGmcSdmaWriteLinearHdr) {
            const uint64_t va = static_cast<uint64_t>(pkt[1]) |
                                (static_cast<uint64_t>(pkt[2]) << 32);
            const uint64_t pa = translate_va(va);
            if (pa != ~0ull && pa >= kFbPhysBase &&
                (pa - kFbPhysBase) + 4u <= kBar0Bytes)
                memcpy(&d.bar0[static_cast<size_t>(pa - kFbPhysBase)], &pkt[4], 4u);
        }
        d.sdma_rptr = (d.sdma_rptr + kGmcSdmaPacketBytes) % kGmcSdmaRingBytes;
        d.bar5[kSdma0 + 0x0083u] = d.sdma_rptr;
    }
}

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= kBar5Dwords)
        return false;
    if (write) {
        d.writes.push_back(dword);
        if (dword == d.stuck_dword)
            return true; /* accepts the write but latches nothing */
        d.bar5[dword] = *value;
        /* A kick is the live RB_WPTR write plus the closing MINOR=0 write;
         * the opening MINOR=1 is part of the same protected sequence but is
         * not counted as a separate kick. */
        if (dword == kSdma0 + 0x0085u)
            d.trace.push_back({'M', dword});
        if (dword == kSdma0 + 0x00B5u && *value == 0u) {
            d.trace.push_back({'M', dword});
            mcu_poll(d); /* MINOR_PTR_UPDATE cleared: poll the queue */
        }
        return true;
    }
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
    if (!value || dwords == 0u || dwords > 2u)
        return false;
    if (bar != R3_IO_BAR_VRAM)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > kBar0Bytes)
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;

    const uint64_t bytes = static_cast<uint64_t>(dwords) * 4ull;
    if (write) {
        memcpy(&d.bar0[static_cast<size_t>(off)], value, bytes);
        d.trace.push_back({in_sdma_window(off) ? 'B' : 'Z',
                           static_cast<uint32_t>(off >> 2)});
        return true;
    }

    if (in_sdma_window(off))
        d.trace.push_back({'R', static_cast<uint32_t>(off >> 2)});
    if (d.shadow_read_stale && off == g_sdma_ring_off + kSdmaShadowOff) {
        uint64_t stale = static_cast<uint64_t>(d.stale_shadow); /* upper dword 0 */
        memcpy(value, &stale, bytes);
    } else {
        memcpy(value, &d.bar0[static_cast<size_t>(off)], bytes);
        if (d.ring_read_corrupt && off == g_sdma_ring_off + 8u)
            *value = ~*value; /* third packet dword never matches */
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
static void io_lock(void *)
{
}
static void io_unlock(void *)
{
}

static bool io_hdp(void *ctx)
{
    Dev &d = *static_cast<Dev *>(ctx);
    d.flush_at = d.trace.size();
    d.trace.push_back({'F', 0u});
    if (d.flush_copies) {
        d.bar0_copy = d.bar0;
        d.mcu_line_valid = false; /* published memory: next fetch reloads */
    }
    /* A non-publishing flush leaves the engine's cached line intact: this is
     * the boot7 failure mode, the device keeps using what it already fetched. */
    return true;
}

static uint32_t bar0_read32(const Dev &d, uint64_t off)
{
    uint32_t v = 0u;
    if (off + 4u <= d.bar0.size())
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 4u);
    return v;
}

static uint32_t bar0_copy_read32(const Dev &d, uint64_t off)
{
    uint32_t v = 0u;
    if (off + 4u <= d.bar0_copy.size())
        memcpy(&v, &d.bar0_copy[static_cast<size_t>(off)], 4u);
    return v;
}

/* ---- property store ---- */
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

/* ---- fixture ---- */
struct Fixture {
    R3Rt rt;
    R3EvidenceLines ev;
    R3SubstepEnv env;
    R3Io io;
    IPBaseTable ip;
    PropStore store;
    R3EvSink sink;
};

static bool build_fixture(Fixture &f, Dev &d)
{
    memset(&f.rt, 0, sizeof(f.rt));
    f.ip = IPBaseTable();
    f.ip.set(IPBlock::GC, kGc0);
    f.ip.setBase(IPBlock::GC, 1, kGc1);
    f.ip.set(IPBlock::MMHUB, kMmhub0);
    f.ip.setBase(IPBlock::MMHUB, 1, kMmhub1);
    f.ip.set(IPBlock::SDMA0, kSdma0);
    f.ip.setBase(IPBlock::SDMA0, 1, kSdma1);

    /* MMHUB + GC FB aperture (S6 recheck reads and cross-checks these) */
    d.bar5[kMmhub1 + 0x08D7u] = 0x6a0cu; /* MMMC_VM_FB_OFFSET */
    d.bar5[kMmhub1 + 0x08ECu] = 0x8000u; /* FB_LOCATION_BASE */
    d.bar5[kMmhub1 + 0x08EDu] = 0x80ffu; /* FB_LOCATION_TOP */
    d.bar5[kGc0 + 0x15a7u] = 0x6a0cu;    /* GCMC_VM_FB_OFFSET (must match MMHUB) */
    d.bar5[kGc0 + 0x1678u] = 0x8000u;    /* GCMC_VM_FB_LOCATION_BASE */
    d.bar5[kGc0 + 0x1679u] = 0x80ffu;    /* GCMC_VM_FB_LOCATION_TOP */
    /* invalidate ack/sem: ack reads back all-vmids-set, sem reads acquired */
    d.bar5[kGc0 + 0x16CEu] = 0xffffu;
    d.bar5[kMmhub1 + 0x0786u] = 0xffffu;
    d.bar5[kMmhub1 + 0x0762u] = 1u;
    /* CONTEXT0/8 CNTL reset defaults */
    d.bar5[kGc0 + 0x1688u] = 0x01fffe00u;
    d.bar5[kGc0 + 0x1690u] = 0x01fffe00u;
    d.bar5[kMmhub1 + 0x0740u] = 0x01fffe00u;
    d.bar5[kMmhub1 + 0x0748u] = 0x01fffe00u;
    /* FAULT_CNTL reset */
    d.bar5[kGc0 + 0x15c4u] = 0x00001ffcu; /* FAULT_CNTL reset default (13 enable bits) */
    /* SDMA RB_RPTR */
    d.bar5[kSdma0 + 0x0083u] = 0u;

    R3LayoutInputs in;
    memset(&in, 0, sizeof(in));
    in.fb_mc_base = kFbMcBase;
    in.fb_phys_base = kFbPhysBase;
    in.bar0_base = 0x7c0000000ull;
    in.real_vram_size = 4096ull * 1024ull * 1024ull;
    in.vbios_reserve = 1ull * 1024ull * 1024ull;
    in.bar0_size = kBar0Bytes;
    in.gop_end = 20ull * 1024ull * 1024ull;
    in.tmr_size = 0x4000000ull;
    in.gop_end_known = true;
    if (r3_layout_compute(&in, &f.rt.layout) != 0)
        return false;

    f.rt.ready_bits = R3_IP_BIT(R3Ip::Psp) | R3_IP_BIT(R3Ip::Gfx) |
                      R3_IP_BIT(R3Ip::Sdma) | R3_IP_BIT(R3Ip::Mes);
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
    f.io.hdp_flush = io_hdp;
    return true;
}

/* The SDMA ring/shadow live in the C5 sdma grant; the emulation needs the
 * BAR0 offset to replay packets. The writeguard needs the grant registered
 * under the Sdma owner (the integration layer does this on the real path). */
static void prepare_sdma(const Fixture &f)
{
    const R3Grant *g = r3_layout_grant(&f.rt.layout, "sdma");
    CHECK(g != nullptr, "sdma grant");
    if (!g)
        return;
    g_sdma_ring_off = g->start;
    CHECK(r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                static_cast<uint32_t>(g->size >> 2),
                                WSubstep::Sdma, "layout:sdma") == 0,
          "register sdma grant");
}

/* ---- pure-function tests ---- */
static void test_pt_bits(void)
{
    CHECK(kGmcPteVram == 0x71ull, "vram pte flags (nullarc E6)");
    CHECK(kGmcPteSystem == (0x77ull | (4ull << 48)), "system pte flags (nullarc)");
    CHECK(kGmcPdeFlags == 7ull, "pde flags (VALID|SYSTEM|SNOOPED)");
    CHECK(kGmcRootFlags == 3ull, "root flags (VALID|SYSTEM)");
    CHECK(gmc_gart_entry_vram(0x6a0c00001000ull) == (0x6a0c00001000ull | 0x71ull),
          "gart vram entry");
    CHECK(gmc_gart_entry_system(0x100000001000ull) ==
              ((0x100000001000ull & kGmcPtAddrMask) | kGmcPteSystem),
          "gart system entry");
    CHECK(gmc_gart_index(0x12345000ull) == 0x12345u, "gart index");
    CHECK(gmc_gart_index(0x12345678ull) == 0x12345u, "gart index masks intra-page");
}

static void test_pt_walk(void)
{
    static uint8_t buf[kGmcPtGrantBytes];
    memset(buf, 0, sizeof(buf));
    GmcPtPool pool;
    gmc_pt_pool_init(&pool, 0x2000000000ull, buf);
    const uint64_t va = 0x100000000ull + 0x1000ull;
    uint64_t *leaf = gmc_pt_walk(&pool, va, 1);
    CHECK(leaf != nullptr, "walk allocates the path");
    CHECK(pool.used == 3u, "three intermediate tables for one va");
    CHECK(gmc_pt_map(&pool, va, 0x6a0c00002000ull, kGmcPteVram) == 0, "map");
    CHECK(*leaf == (0x6a0c00002000ull | kGmcPteVram), "leaf pte value");
    /* PDE shape: root entry points at root+4KiB with flags 7 */
    uint64_t root_e;
    memcpy(&root_e, buf, 8);
    CHECK(((va >> 39) & 511u) == 0u, "root index for va 0x100000000");
    CHECK((root_e & kGmcPtAddrMask) == 0x2000000000ull + kGmcPtRootBytes,
          "first pool page");
    CHECK((root_e & kGmcPdeFlags) == kGmcPdeFlags, "pde flags");
    /* walk without alloc on a fresh pool misses */
    GmcPtPool ro;
    gmc_pt_pool_init(&ro, 0x2000000000ull, buf);
    CHECK(gmc_pt_walk(&ro, 0x300000000ull, 0) == nullptr, "no-alloc miss");
    /* capacity exhaustion */
    GmcPtPool full;
    gmc_pt_pool_init(&full, 0x2000000000ull, buf);
    full.used = kGmcPtPoolPages;
    CHECK(gmc_pt_walk(&full, 0x400000000ull, 1) == nullptr, "pool exhausted");
}

static void test_lru(void)
{
    GmcVmidLru lru;
    gmc_vmid_lru_init(&lru);
    uint8_t evicted = kGmcVmidNone;
    CHECK(gmc_vmid_lru_acquire(&lru, 1, &evicted) == kGmcFirstUserVmid,
          "first acquire");
    CHECK(evicted == kGmcVmidNone, "no eviction on a free slot");
    for (uint8_t i = 2; i <= 8; i++)
        CHECK(gmc_vmid_lru_acquire(&lru, i, &evicted) == kGmcFirstUserVmid + i - 1,
              "slot %u", i);
    /* all 8 taken: the next acquire evicts the LRU owner (owner 1) */
    evicted = kGmcVmidNone;
    CHECK(gmc_vmid_lru_acquire(&lru, 9, &evicted) == kGmcFirstUserVmid,
          "recycle slot 0");
    CHECK(evicted == 1u, "evicts the least recently used owner");
    /* touch moves the stamp; release frees the slot */
    gmc_vmid_lru_touch(&lru, kGmcFirstUserVmid + 1);
    gmc_vmid_lru_release(&lru, kGmcFirstUserVmid + 2);
    evicted = kGmcVmidNone;
    CHECK(gmc_vmid_lru_acquire(&lru, 10, &evicted) == kGmcFirstUserVmid + 2,
          "released slot reused");
    CHECK(evicted == kGmcVmidNone, "free slot after release");
    gmc_vmid_lru_release(&lru, 3u); /* out of range: no-op */
    CHECK(lru.slot[3].owner != kGmcVmidNone, "out-of-range release ignored");
}

/* ---- substep tests (S6..S9 run in order: later steps reuse the pools and
 * the row addresses the earlier steps resolved) ---- */
static void test_s6_uma(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    gmc_v11_0_bind_sysmem(&g_fake_alloc);
    r3_writeguard_install_mm();
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 ok (name=%s step=%u)", r.name, r.step);
    /* sysmem grants backfilled into the runtime layout */
    CHECK(f.rt.layout.sysmem_grant_count == 3u, "three sysmem grants");
    CHECK(f.rt.layout.sysmem_grants[0].start == g_mem.phys[0], "gart table phys");
    CHECK(f.rt.layout.sysmem_grants[1].start == g_mem.phys[1], "ctx0 pool phys");
    CHECK(f.rt.layout.sysmem_grants[2].start == g_mem.phys[2], "vmid8 pool phys");
    CHECK(f.rt.layout.sysmem_grants[0].end == g_mem.phys[0] + kGmcGartBytes,
          "gart span");
    CHECK(f.rt.layout.sysmem_grants[1].end == g_mem.phys[1] + kGmcPtGrantBytes,
          "ctx0 span");
    /* S6 performs no MMIO writes */
    CHECK(d.writes.empty(), "s6 is read-only");
}

static void test_s7_gart(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in s7 seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 ok (name=%s step=%u)", r.name, r.step);
    /* W1: one 20-byte packet was appended; both pointers are BYTE offsets. */
    CHECK(d.bar5[kSdma0 + 0x0085u] == 20u, "doorbell wptr is a byte offset (got 0x%x)",
          d.bar5[kSdma0 + 0x0085u]);
    CHECK(d.bar5[kSdma0 + 0x0083u] == 20u, "consumed rptr is a byte offset (got 0x%x)",
          d.bar5[kSdma0 + 0x0083u]);
    /* VMID0 flat configuration on both hubs (CNTL is an RMW: DEPTH stays 0) */
    CHECK(d.bar5[kGc0 + 0x1688u] == 0x01fffe01u, "gc ctx0 cntl RMW (got 0x%x)",
          d.bar5[kGc0 + 0x1688u]);
    CHECK(d.bar5[kGc0 + 0x16F3u] == static_cast<uint32_t>(g_mem.phys[0] | 3u),
          "gc ptb lo");
    CHECK(d.bar5[kGc0 + 0x16F4u] == static_cast<uint32_t>(g_mem.phys[0] >> 32),
          "gc ptb hi");
    CHECK(d.bar5[kGc0 + 0x1713u] == 0u, "gc start lo");
    CHECK(d.bar5[kGc0 + 0x1733u] == 0xffffffffu, "gc end lo");
    CHECK(d.bar5[kGc0 + 0x1734u] == 0x0fu, "gc end hi");
    CHECK(d.bar5[kMmhub1 + 0x0740u] == 0x01fffe01u, "mmhub ctx0 cntl");
    CHECK(d.bar5[kMmhub1 + 0x07ABu] == static_cast<uint32_t>(g_mem.phys[0] | 3u),
          "mmhub ptb lo");
    /* Linux program_invalidation: 18 engines x (LO=0xffffffff, HI=0x1f), stride 2, both hubs */
    CHECK(d.bar5[kGc0 + 0x16CFu] == 0xffffffffu, "gc arm eng0 lo");
    CHECK(d.bar5[kGc0 + 0x16D0u] == 0x1fu, "gc arm eng0 hi");
    CHECK(d.bar5[kGc0 + 0x16F1u] == 0xffffffffu, "gc arm eng17 lo");
    CHECK(d.bar5[kGc0 + 0x16F2u] == 0x1fu, "gc arm eng17 hi");
    CHECK(d.bar5[kMmhub1 + 0x0787u] == 0xffffffffu, "mmhub arm eng0 lo");
    CHECK(d.bar5[kMmhub1 + 0x0788u] == 0x1fu, "mmhub arm eng0 hi");
    CHECK(d.bar5[kMmhub1 + 0x07A9u] == 0xffffffffu, "mmhub arm eng17 lo");
    CHECK(d.bar5[kMmhub1 + 0x07AAu] == 0x1fu, "mmhub arm eng17 hi");
    CHECK(strstr(n48log_host_text(), "gmc-inv-arm: armed 18 engines per hub") != nullptr,
          "gmc-inv-arm evidence line");
    /* GART entry 0 covers the first test page */
    const R3Grant *tb = r3_layout_grant(&f.rt.layout, "test_buf");
    CHECK(tb != nullptr, "test_buf grant");
    if (!tb)
        return;
    const uint64_t page0_pa = kFbPhysBase + tb->start;
    uint64_t pte;
    memcpy(&pte, g_mem.buf[0].data(), 8);
    CHECK(pte == gmc_gart_entry_vram(page0_pa), "gart entry 0");
    /* SDMA verification wrote through the GART VA */
    uint32_t got;
    memcpy(&got, &d.bar0[tb->start + kGmcTestPageIntra], 4);
    CHECK(got == kGmcTestMagic, "sdma wrote through the gart (got 0x%x)", got);
}

/* S5 leaves queue0's RB_RPTR at 100 bytes (boot6 driver log); dword and byte
 * semantics only diverge when rptr != 0, so this is the regression that fails
 * on the old implementation. */
static void test_s7_gart_nonzero_rptr(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    d.bar5[kSdma0 + 0x0083u] = 100u;
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in nonzero-rptr seq (name=%s step=%u)", r.name, r.step);
    CHECK(d.bar5[kSdma0 + 0x0083u] == 100u, "s6 does not move the sdma rptr");
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 ok with rptr=100 (name=%s step=%u)", r.name, r.step);
    CHECK(d.bar5[kSdma0 + 0x0085u] == 120u, "doorbell = 100+20 bytes (got 0x%x)",
          d.bar5[kSdma0 + 0x0085u]);
    CHECK(d.bar5[kSdma0 + 0x0083u] == 120u, "consumed rptr = 120 bytes (got 0x%x)",
          d.bar5[kSdma0 + 0x0083u]);
    const R3Grant *tb = r3_layout_grant(&f.rt.layout, "test_buf");
    CHECK(tb != nullptr, "test_buf grant");
    if (!tb)
        return;
    uint32_t got;
    memcpy(&got, &d.bar0[tb->start + kGmcTestPageIntra], 4);
    CHECK(got == kGmcTestMagic, "packet appended at +100 was executed (got 0x%x)", got);
}

/* Failure evidence: the fixed snapshot must survive r3_substep_fail(), and the
 * overflow diagnostics must still reach the process log. */
static void test_s7_verify_failure_readings(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in verify-failure seq (name=%s step=%u)", r.name, r.step);
    /* Audit B#23: diagnostics must address GC through the discovered seg0
     * base (kGc0), not the old 0x10000 literal. Poison the literal so a
     * regression reads 0xdeadbeef instead of the register value. */
    d.bar5[0x10000u + 0x15A7u] = 0xdeadbeefu;
    d.sdma_translate = false;
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status != kR3SsecOk, "s7 fails when the sdma write never lands");
    CHECK(r.step == 4u, "s7 fails at the verify step (got %u)", r.step);

    bool saw_mismatch = false, saw_fault = false, saw_rptr = false;
    bool saw_shadow = false, saw_wptr = false, saw_cntl = false, saw_f32 = false;
    for (uint32_t i = 0; i < r.reading_count; i++) {
        const R3SubstepReading &e = r.readings[i];
        if (strcmp(e.what, "gart verify mismatch") == 0)
            saw_mismatch = true;
        if (strcmp(e.what, "gart-diag:mmhub-l2-fault-status") == 0) {
            saw_fault = true;
            CHECK(e.value == d.bar5[kMmhub1 + 0x070Cu],
                  "fault-status reading matches the register (got 0x%x)", e.value);
        }
        if (strcmp(e.what, "sdma rptr after poll") == 0)
            saw_rptr = true;
        if (strcmp(e.what, "sdma wptr shadow") == 0)
            saw_shadow = true;
        if (strcmp(e.what, "sdma rb wptr") == 0)
            saw_wptr = true;
        if (strcmp(e.what, "sdma rb cntl") == 0)
            saw_cntl = true;
        if (strcmp(e.what, "sdma f32 cntl") == 0)
            saw_f32 = true;
    }
    CHECK(saw_mismatch, "failure keeps the mismatch reading");
    CHECK(saw_fault, "failure keeps the fault-status reading");
    CHECK(saw_rptr, "failure keeps the sdma rptr reading");
    CHECK(saw_shadow, "failure keeps the sdma wptr shadow reading");
    CHECK(saw_wptr, "failure keeps the sdma rb wptr reading");
    CHECK(saw_cntl, "failure keeps the sdma rb cntl reading");
    CHECK(saw_f32, "failure keeps the sdma f32 cntl reading");
    CHECK(r.reading_count > 0u && r.reading_count <= 8u, "readings snapshot bounded");
    CHECK(d.bar5[kSdma0 + 0x0083u] == 0u, "no packet consumed");
    CHECK(strstr(n48log_host_text(), "gmc-gart: verify-fail: sdma rptr") != nullptr,
          "verify-fail evidence line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag mmhub-mx-l1=0x") != nullptr,
          "mx-l1 diag line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag pte0-lo=0x") != nullptr,
          "pte0 diag line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag sdma status=0x") != nullptr,
          "sdma status diag line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag sdma shadow hi=0x") != nullptr,
          "sdma shadow hi diag line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag gfxhub-fb-offset=0x6a0c") != nullptr,
          "gfxhub fb-offset uses the GC seg0 base, not 0x10000");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag mmhub-fb-offset=0x6a0c") != nullptr,
          "mmhub fb-offset uses the MMHUB seg1 base");
}

/* A register that accepts the write but never latches it must be caught by the arming readback:
 * the write is not the evidence, the read-back value is (Linux drives the same check through
 * its WREG32/RREG32 sequence). */
static void test_s7_arm_readback_mismatch(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in arm-mismatch seq (name=%s step=%u)", r.name, r.step);
    d.stuck_dword = kGc0 + 0x16CFu; /* GC TLB invalidation engine 0 range LO */
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status != kR3SsecOk, "s7 fails when an arming range does not read back");
    CHECK(strstr(n48log_host_text(), "gmc-inv-arm: readback mismatch") != nullptr,
          "readback mismatch evidence line");
}

/* The flush must sit between the shadow writes and the doorbell: the MCU only
 * sees the new wptr once the flush published it, and the kick must come last
 * so the engine cannot observe a half-written packet. */
static void test_s7_visibility_ordering(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in visibility seq (name=%s step=%u)", r.name, r.step);

    const size_t mark = d.trace.size();
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 ok with the visibility gate (name=%s step=%u)",
          r.name, r.step);
    CHECK(d.flush_at < d.trace.size(), "hdp flush recorded in the trace");

    size_t bytes_before = 0u, kick_before = 0u, kick_after = 0u, reads_after = 0u;
    for (size_t i = mark; i < d.trace.size(); i++) {
        if (i < d.flush_at && d.trace[i].kind == 'B')
            bytes_before++;
        if (d.trace[i].kind == 'M') {
            if (i < d.flush_at)
                kick_before++;
            else
                kick_after++;
        }
    }
    for (size_t i = d.flush_at + 1; i < d.trace.size(); i++) {
        if (d.trace[i].kind == 'M')
            break;
        if (d.trace[i].kind == 'R')
            reads_after++;
    }

    CHECK(bytes_before == 6u, "six sdma writes before flush (got %zu)", bytes_before);
    CHECK(kick_before == 0u, "no kick before flush (got %zu)", kick_before);
    CHECK(kick_after == 2u, "both kick writes after flush (got %zu)", kick_after);
    CHECK(reads_after == 6u, "six readbacks between flush and kick (got %zu)", reads_after);
}

/* Discriminating experiment: with a publishing flush the engine consumes the
 * queue, so the reduction is real and not just a CPU-side illusion. */
static void test_s7_mcu_poll_semantics(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in mcu-poll seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 ok when the flush publishes (name=%s step=%u)",
          r.name, r.step);
    CHECK(d.flush_copies, "fixture defaults to publishing flushes");
    CHECK(d.mcu_polls >= 1u, "mcu polled the queue shadow (got %u)", d.mcu_polls);
    CHECK(d.bar5[kSdma0 + 0x0085u] == 20u, "kick wptr is 20 bytes (got 0x%x)",
          d.bar5[kSdma0 + 0x0085u]);
    CHECK(d.bar5[kSdma0 + 0x0083u] == 20u, "engine consumed 20 bytes (got 0x%x)",
          d.bar5[kSdma0 + 0x0083u]);
    CHECK(bar0_read32(d, g_sdma_ring_off + kSdmaShadowOff) == 20u,
          "cpu shadow reads 20 (got 0x%x)",
          bar0_read32(d, g_sdma_ring_off + kSdmaShadowOff));
    CHECK(bar0_copy_read32(d, g_sdma_ring_off + kSdmaShadowOff) == 20u,
          "device view shadow reads 20 (got 0x%x)",
          bar0_copy_read32(d, g_sdma_ring_off + kSdmaShadowOff));
}

/* Reproduces the boot7 signature: the flush is called but publishes nothing.
 * Every CPU-side readback is correct, the engine still serves its stale line
 * and therefore never advances rptr. */
static void test_s7_stale_shadow_no_consume(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in stale-shadow seq (name=%s step=%u)", r.name, r.step);

    (void)mcu_read32(d, g_sdma_ring_off + kSdmaShadowOff);
    CHECK(d.mcu_line_valid, "engine warmed the shadow line");

    d.flush_copies = false;
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status != kR3SsecOk, "s7 fails when the flush does not publish");
    CHECK(r.step == 4u, "s7 fails at the verify step (got %u)", r.step);

    CHECK(d.bar5[kSdma0 + 0x0085u] == 0x14u, "kick wrote wptr=0x14 (got 0x%x)",
          d.bar5[kSdma0 + 0x0085u]);
    CHECK(d.bar5[kSdma0 + 0x0083u] == 0u, "device consumed nothing (got 0x%x)",
          d.bar5[kSdma0 + 0x0083u]);
    CHECK(d.mcu_polls >= 1u, "mcu polled the queue shadow (got %u)", d.mcu_polls);

    const uint64_t shadow_off = g_sdma_ring_off + kSdmaShadowOff;
    CHECK(bar0_read32(d, shadow_off) == 0x14u, "host shadow is published (got 0x%x)",
          bar0_read32(d, shadow_off));
    CHECK(bar0_copy_read32(d, shadow_off) == 0u, "device view is still stale (got 0x%x)",
          bar0_copy_read32(d, shadow_off));
    CHECK(d.mcu_line_valid && d.mcu_line_off == shadow_off,
          "failed flush leaves the engine's cached line in place");
    CHECK(mcu_read32(d, shadow_off) == 0u, "engine still serves the cached shadow");

    uint64_t cpu = 0u;
    CHECK(io_bus(&d, R3_IO_BAR_VRAM, shadow_off, &cpu, false, 2u), "cpu shadow readback");
    CHECK(static_cast<uint32_t>(cpu) == 0x14u,
          "cpu readback sees 0x14 although the device does not (got 0x%x)",
          static_cast<uint32_t>(cpu));

    bool saw_shadow = false;
    for (uint32_t i = 0; i < r.reading_count; i++) {
        const R3SubstepReading &e = r.readings[i];
        if (strcmp(e.what, "sdma wptr shadow") == 0) {
            saw_shadow = true;
            CHECK(e.value == 0x14u, "snapshot shadow is 0x14 (got 0x%x)", e.value);
        }
    }
    CHECK(saw_shadow, "failure keeps the shadow reading");
    CHECK(strstr(n48log_host_text(), "gmc-gart: verify-fail: sdma rptr") != nullptr,
          "verify-fail evidence line");
}

static void test_s7_packet_readback_mismatch(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in pkt-back seq (name=%s step=%u)", r.name, r.step);

    d.ring_read_corrupt = true;
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status != kR3SsecOk, "s7 fails when a packet dword does not read back");
    CHECK(r.step == 4u, "s7 fails at the verify step (got %u)", r.step);
    CHECK(r.reading_count == 8u, "prep failure fills all eight slots (got %u)",
          r.reading_count);
    CHECK(r.readings[0].value == 2u, "verdict step is kSdmaStepPktBack (got %u)",
          r.readings[0].value);
    CHECK(strcmp(r.readings[0].what, "sdma submit step") == 0,
          "slot 0 is the submit step (got %s)", r.readings[0].what);
    CHECK(r.readings[1].value == 0xffffffffu,
          "slot 1 keeps the flipped dword (got 0x%x)", r.readings[1].value);
    CHECK(strcmp(r.readings[1].what, "sdma pkt back") == 0,
          "slot 1 is the packet readback (got %s)", r.readings[1].what);

    CHECK(d.bar5[kSdma0 + 0x0085u] == 0u, "no kick on a packet readback mismatch");
    CHECK(d.bar5[kSdma0 + 0x00b5u] == 0u, "MINOR stays 0");
    CHECK(d.bar5[kSdma0 + 0x0083u] == 0u, "no packet consumed");
    size_t kicks = 0u;
    for (size_t i = 0; i < d.trace.size(); i++)
        if (d.trace[i].kind == 'M')
            kicks++;
    CHECK(kicks == 0u, "no kick write reached the mmio (got %zu)", kicks);
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag sdma pkt back=0x") != nullptr,
          "packet readback evidence line");
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag sdma status=0x") != nullptr,
          "sdma status diag line");
}

static void test_s7_shadow_readback_mismatch(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in shadow-back seq (name=%s step=%u)", r.name, r.step);

    d.shadow_read_stale = true;
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status != kR3SsecOk, "s7 fails when the shadow does not read back");
    CHECK(r.step == 4u, "s7 fails at the verify step (got %u)", r.step);
    CHECK(r.reading_count == 8u, "prep failure fills all eight slots (got %u)",
          r.reading_count);
    CHECK(r.readings[0].value == 3u, "verdict step is kSdmaStepShadowBack (got %u)",
          r.readings[0].value);
    CHECK(strcmp(r.readings[2].what, "sdma wptr shadow") == 0,
          "slot 2 is the shadow readback (got %s)", r.readings[2].what);
    CHECK(r.readings[2].value == 0u, "stale shadow reads 0 (got 0x%x)",
          r.readings[2].value);

    CHECK(d.bar5[kSdma0 + 0x0085u] == 0u, "no kick on a shadow readback mismatch");
    CHECK(d.bar5[kSdma0 + 0x00b5u] == 0u, "MINOR stays 0");
    size_t kicks = 0u;
    for (size_t i = 0; i < d.trace.size(); i++)
        if (d.trace[i].kind == 'M')
            kicks++;
    CHECK(kicks == 0u, "no kick write reached the mmio (got %zu)", kicks);
    CHECK(strstr(n48log_host_text(), "gmc-gart: diag sdma shadow back=0x") != nullptr,
          "shadow readback evidence line");
}

static void test_s8_gpuvm0(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in s8 seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 in s8 seq (name=%s step=%u)", r.name, r.step);
    r = gpuvm0_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s8 ok (name=%s step=%u)", r.name, r.step);
    /* nullarc vmBind sequence, direct write */
    CHECK(d.bar5[kGc0 + 0x1688u] == kGmcVbindCntl, "gc ctx0 cntl direct");
    CHECK(d.bar5[kGc0 + 0x16F3u] == static_cast<uint32_t>(g_mem.phys[1] | 3u),
          "gc ptb lo");
    CHECK(d.bar5[kGc0 + 0x1713u] == 0u, "gc start lo");
    CHECK(d.bar5[kGc0 + 0x1733u] == 0xffffffffu, "gc end lo");
    CHECK(d.bar5[kMmhub1 + 0x0740u] == kGmcVbindCntl, "mmhub ctx0 cntl");
    /* R10 absolute cross-check holds with this fixture's GC base */
    CHECK(kGc0 + 0x1688u == kGmcR10Ctx0CntlAbs, "r10 cntl abs");
    CHECK(kGc0 + 0x16F3u == kGmcR10Ctx0BaseLoAbs, "r10 base abs");
    CHECK(kGc0 + 0x1713u == kGmcR10Ctx0StartLoAbs, "r10 start abs");
    CHECK(kGc0 + 0x1733u == kGmcR10Ctx0EndLoAbs, "r10 end abs");
    /* SDMA verification wrote through the four-level GPUVM VA */
    const R3Grant *tb = r3_layout_grant(&f.rt.layout, "test_buf");
    CHECK(tb != nullptr, "test_buf grant");
    if (!tb)
        return;
    uint32_t got;
    memcpy(&got, &d.bar0[tb->start + 0x1000u + kGmcTestPageIntra], 4);
    CHECK(got == kGmcTestMagic, "sdma wrote through the gpuvm (got 0x%x)", got);
}

static void test_s9_vmid8(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in s9 seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 in s9 seq (name=%s step=%u)", r.name, r.step);
    r = gpuvm0_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s8 in s9 seq (name=%s step=%u)", r.name, r.step);
    r = vmid8_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s9 ok (name=%s step=%u)", r.name, r.step);
    CHECK(d.bar5[kGc0 + 0x1690u] == kGmcVbindCntl, "gc ctx8 cntl");
    CHECK(d.bar5[kGc0 + 0x1703u] == static_cast<uint32_t>(g_mem.phys[2] | 3u),
          "gc ctx8 ptb lo");
    CHECK(d.bar5[kGc0 + 0x1723u] == 0u, "gc ctx8 start lo");
    CHECK(d.bar5[kGc0 + 0x1743u] == 0xffffffffu, "gc ctx8 end lo");
    CHECK(d.bar5[kMmhub1 + 0x0748u] == kGmcVbindCntl, "mmhub ctx8 cntl");
    CHECK(d.bar5[kGc0 + 0x15c4u] == 0x00001ffcu, "fault default verified read-only");
    /* PER_VMID invalidate carried bit 8 */
    CHECK(d.bar5[kGc0 + 0x16BCu] == (kGmcInvBase | (1u << 8) | 1u),
          "gc inv req vmid8 (got 0x%x)", d.bar5[kGc0 + 0x16BCu]);
    /* the S9 path exists in the vmid8 pool (S11 prep, no GPU verification) */
    const R3Grant *tb = r3_layout_grant(&f.rt.layout, "test_buf");
    CHECK(tb != nullptr, "test_buf grant");
    if (!tb)
        return;
    GmcPtPool ro;
    gmc_pt_pool_init(&ro, g_mem.phys[2], g_mem.buf[2].data());
    uint64_t *e = gmc_pt_walk(&ro, kGmcTestVaVmid8, 0);
    CHECK(e != nullptr, "s9 path exists");
    if (e) {
        const uint64_t want =
            ((kFbPhysBase + tb->start + 2 * 4096ull) & kGmcPtAddrMask) | kGmcPteVram;
        CHECK(*e == want, "s9 leaf pte (got 0x%llx want 0x%llx)",
              (unsigned long long)*e, (unsigned long long)want);
    }
    /* W4/§7.2: the same install_mm accumulates sdma layout (1) + S6 layouts (6)
     * + S7 GART rows (21) + S9 vmid8 first-15 rows = 43 grants. */
    CHECK(r3_wg_grant_count() ==
              kGmcSeqLayoutCount + kGartRowCount + kVmid8FirstInvalidateRow +
              kSdmaLayoutCount,
          "chain budget == 43 (got %u)", r3_wg_grant_count());
}

/* #6 regression: FAULT_CNTL (0x15c4) is verified read-only. The old path
 * rewrote it with kGmcFaultDefault; seed a value with the top control bits set
 * and require a fail-closed rejection that leaves it untouched. */
static void test_s9_fault_default_readonly(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.bar5[kGc0 + 0x15c4u] = 0xc0001ffcu;
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in fault seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 in fault seq (name=%s step=%u)", r.name, r.step);
    r = gpuvm0_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s8 in fault seq (name=%s step=%u)", r.name, r.step);
    r = vmid8_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecFail, "bad FAULT_CNTL rejected");
    CHECK(r.section == kR3SecSeq && r.step == 4u,
          "rejected at seq step 4 (section=%u step=%u)",
          static_cast<unsigned>(r.section), r.step);
    CHECK(d.bar5[kGc0 + 0x15c4u] == 0xc0001ffcu,
          "FAULT_CNTL untouched (got 0x%x)", d.bar5[kGc0 + 0x15c4u]);
}

/* #6 regression: the low 13 enable bits are checked too, not just the top
 * control bits. One flipped low bit must also fail closed and stay untouched. */
static void test_s9_fault_default_low_bits(void)
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d), "fixture");
    d.bar5[kGc0 + 0x15c4u] = 0x00001ffdu;
    reset_fake_mem();
    r3_writeguard_install_mm();
    prepare_sdma(f);
    R3SubstepResult r = uma_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s6 in fault low-bits seq (name=%s step=%u)", r.name, r.step);
    r = gart_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s7 in fault low-bits seq (name=%s step=%u)", r.name, r.step);
    r = gpuvm0_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecOk, "s8 in fault low-bits seq (name=%s step=%u)", r.name, r.step);
    r = vmid8_v11_0_substep(&f.io, &f.env);
    CHECK(r.status == kR3SsecFail, "bad FAULT_CNTL low bits rejected");
    CHECK(r.section == kR3SecSeq && r.step == 4u,
          "rejected at seq step 4 (section=%u step=%u)",
          static_cast<unsigned>(r.section), r.step);
    CHECK(d.bar5[kGc0 + 0x15c4u] == 0x00001ffdu,
          "FAULT_CNTL untouched (got 0x%x)", d.bar5[kGc0 + 0x15c4u]);
}
} /* namespace */

int main(void)
{
    test_pt_bits();
    test_pt_walk();
    test_lru();
    test_s6_uma();
    test_s7_gart();
    test_s7_gart_nonzero_rptr();
    test_s7_verify_failure_readings();
    test_s7_arm_readback_mismatch();
    test_s7_visibility_ordering();
    test_s7_mcu_poll_semantics();
    test_s7_stale_shadow_no_consume();
    test_s7_packet_readback_mismatch();
    test_s7_shadow_readback_mismatch();
    test_s8_gpuvm0();
    test_s9_vmid8();
    test_s9_fault_default_readonly();
    test_s9_fault_default_low_bits();
    printf("test_gmc: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
