/*
 * test_compute_v11.cpp - host state-machine test for the S12 ABI-readiness and
 * S13 compute substeps (amd/compute_v11.cpp).
 *
 * The R3Io stub plays the GPU: BAR5 is a dword array (SCRATCH_REG0 lives at
 * dword 0x2040), BAR0 is the 256 MiB carveout, and BAR2 doorbells are emulated.
 * Ringing doorbell 6 (kMesDoorbellCompute) makes the stub act as the CP: it
 * decodes the ring packets independently from the spec shapes (TYPE-3
 * opcode/count fields, nvd.h), executes SET_UCONFIG_REG into SCRATCH_REG0,
 * WRITE_DATA into the carveout page named by its VM address (vmid0 aperture =
 * fb_mc + carveout byte offset), and INDIRECT_BUFFER by walking the IB and
 * firing the RELEASE_MEM fence (64-bit seq at the named MC address) - exactly
 * the effects compute_v11_0_substep then verifies through the same bus.
 *
 * Branches: full chain, missing grant, fence timeout, SCRATCH_REG0 mismatch,
 * test-buffer mismatch, submit-tail abort (hdp_flush false), null io/env, and
 * the S12 branches (null env, layout-not-ok, pass).
 *
 * Planted-break guards:
 *   - the RELEASE_MEM preload expectation is built HERE from the packet spec
 *     (0xC0490006 / 0x06603514 / 0x40000000, fence MC = fb_mc + mes grant +
 *     kMesCmpWbOff + kMesWbFenceOff, seq 1, zero pad), so a wrong fence
 *     address or a broken 8-dword form fails;
 *   - the ring expectations pin the byte-offset semantics: SET_UCONFIG_REG at
 *     ring dword 0 (window offset 0x40, magic 0xCAFEDEAD), WRITE_DATA at
 *     dword 3 (dst = fb_mc + test_buf grant start), INDIRECT_BUFFER at
 *     dword 8 (IB = fb_mc + csib grant start, 8 dwords, vmid 0, VALID), so
 *     reordering the chain or swapping the two VM forms fails;
 *   - every bus (VRAM) access must carry a carveout BYTE OFFSET (< the BAR0
 *     window), never an MC address, and the doorbell must be index 6 carrying
 *     the full wptr (12 dwords), so an MC-form bus offset or a partial wptr
 *     fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#include <vector>

#include "amd/compute_v11.h"
/* abi_v11_0_substep (S12) has no header declaration: it lives beside the S13
 * entry in compute_v11.cpp and the kext calls it through the integration's
 * substep table. Declare it here so the host test links the same symbol. */
namespace amdgpu {
R3SubstepResult abi_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
} /* namespace amdgpu */
#include "amd/mes_v11_0.h"
#include "amd/pm4_v11.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_substep.h"

using namespace amdgpu;

namespace {

constexpr uint64_t kFbMcBase = 0x8000000000ull;
constexpr uint64_t kBar0Bytes = 0x10000000ull;
constexpr uint32_t kBar5Dwords = 0x200000u;
/* SCRATCH_REG0 BAR5 dword (compute_v11.cpp (E) reads it back here). */
constexpr uint32_t kScratchDword = 0x2040u;
/* SET_UCONFIG_REG window-relative offset naming SCRATCH_REG0 (0xC000+0x40). */
constexpr uint32_t kUconfigWinOff = 0x40u;
constexpr uint32_t kMagic = 0xCAFEDEADu;

/* Expected packet words, derived from the nvd.h shapes HERE (never from the
 * builders under test - a builder regression must fail these constants). */
constexpr uint32_t kExpRmW0 = 0xC0064900u; /* TYPE3, count 6, op 0x49 (8 dw) */
constexpr uint32_t kExpRmW1 = 0x06603514u; /* SEQ|GL2_WB|GLM_INV|GLM_WB|CP3|EV 0x14|EI 5 */
constexpr uint32_t kExpRmW2 = 0x40000000u; /* DATA_SEL(2) 64-bit */
constexpr uint32_t kExpWdW0 = 0xC0033700u; /* TYPE3, op 0x37, count 3 (5 dw) */
constexpr uint32_t kExpWdW1 = 0x00100500u; /* DST_SEL(5) memory | WR_CONFIRM */
constexpr uint32_t kExpIbW0 = 0xC0023F00u; /* TYPE3, op 0x3F, count 2 (4 dw) */
constexpr uint32_t kExpIbW3 = 0x00800008u; /* length 8 | vmid 0 | VALID(1<<23) */

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

struct BusOp {
    uint32_t bar;
    uint64_t off;
    bool write;
    uint32_t dwords;
};

struct DbOp {
    uint32_t index;
    uint64_t value;
    uint32_t dwords;
};

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> bar5;
    uint64_t now;
    std::vector<BusOp> vram_ops;
    std::vector<DbOp> doorbells;
    uint64_t ring_off;   /* carveout byte offset of the compute ring (cp_run) */
    bool run_gpu;        /* CP model executes the chain on doorbell 6 */
    bool stall_fence;    /* CP skips the RELEASE_MEM fence write */
    bool scratch_bad;    /* CP writes a wrong SCRATCH_REG0 value */
    bool buf_bad;        /* CP writes a wrong WRITE_DATA payload */
    bool hdp_fail;       /* hdp_flush returns false */

    Dev()
        : bar0(static_cast<size_t>(kBar0Bytes), 0u),
          bar5(kBar5Dwords, 0u),
          now(1000),
          ring_off(0),
          run_gpu(true),
          stall_fence(false),
          scratch_bad(false),
          buf_bad(false),
          hdp_fail(false)
    {
    }
};

static uint32_t b0_32(const Dev &d, uint64_t off)
{
    uint32_t v = 0;
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 4u);
    return v;
}

static void b0_put32(Dev &d, uint64_t off, uint32_t v)
{
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&d.bar0[static_cast<size_t>(off)], &v, 4u);
}

static uint64_t b0_64(const Dev &d, uint64_t off)
{
    uint64_t v = 0;
    if (off + 8ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 8u);
    return v;
}

static void b0_put64(Dev &d, uint64_t off, uint64_t v)
{
    if (off + 8ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&d.bar0[static_cast<size_t>(off)], &v, 8u);
}

/* Act as the CP: consume PM4 dwords [0, wptr) from the compute ring (doorbell
 * 6). Only the S13 shapes are decoded; anything else stops the walk, so a
 * malformed chain never fires the fence and the substep times out. */
static void cp_run(Dev &d, uint32_t wptr)
{
    if (!d.run_gpu)
        return;
    const uint64_t ring = d.ring_off;
    uint32_t pos = 0;
    uint32_t guard = 0;
    while (pos != wptr && guard++ < 64u) {
        const uint64_t a = ring + static_cast<uint64_t>(pos) * 4ull;
        const uint32_t w0 = b0_32(d, a);
        if ((w0 >> 30) != 3u)
            break;
        const uint32_t op = (w0 >> 8) & 0xffu;
        const uint32_t total = ((w0 >> 16) & 0x3fffu) + 2u;
        if (op == kPm4OpSetUconfigReg && total == 3u) {
            const uint32_t reg = b0_32(d, a + 4ull);
            const uint32_t val = b0_32(d, a + 8ull);
            if (reg == kUconfigWinOff)
                d.bar5[kScratchDword] = d.scratch_bad ? (val ^ 0xFFFFu) : val;
        } else if (op == kPm4OpWriteData && total == 5u) {
            const uint64_t lo = b0_32(d, a + 8ull);
            const uint64_t hi = b0_32(d, a + 12ull);
            const uint32_t data = b0_32(d, a + 16ull);
            const uint64_t dst = (hi << 32) | lo;
            if (dst >= kFbMcBase && dst + 4ull <= kFbMcBase + kBar0Bytes) {
                b0_put32(d, dst - kFbMcBase, d.buf_bad ? (data ^ 0xFFFFu) : data);
            }
        } else if (op == kPm4OpIndirectBuffer && total == 4u) {
            const uint64_t lo = b0_32(d, a + 4ull);
            const uint64_t hi = b0_32(d, a + 8ull);
            const uint32_t w3 = b0_32(d, a + 12ull);
            const uint64_t va = (hi << 32) | (lo & ~3ull);
            const uint32_t len = w3 & 0xfffffu;
            const uint32_t vmid = (w3 >> 24) & 0xfu;
            /* vmid0 aperture: the IB VM address is fb_mc + carveout offset. */
            if (vmid == 0u && va >= kFbMcBase && va < kFbMcBase + kBar0Bytes) {
                const uint64_t ib = va - kFbMcBase;
                for (uint32_t i = 0u; i + 2u <= len && i < 64u;) {
                    const uint32_t iw0 = b0_32(d, ib + static_cast<uint64_t>(i) * 4ull);
                    if ((iw0 >> 30) != 3u)
                        break;
                    const uint32_t iop = (iw0 >> 8) & 0xffu;
                    const uint32_t itot = ((iw0 >> 16) & 0x3fffu) + 2u;
                    if (iop == kPm4OpReleaseMem && itot == 8u) {
                        const uint64_t flo =
                            b0_32(d, ib + static_cast<uint64_t>(i + 3u) * 4ull);
                        const uint64_t fhi =
                            b0_32(d, ib + static_cast<uint64_t>(i + 4u) * 4ull);
                        const uint32_t seq =
                            b0_32(d, ib + static_cast<uint64_t>(i + 5u) * 4ull);
                        const uint64_t fmc = (fhi << 32) | flo;
                        if (fmc >= kFbMcBase && fmc + 8ull <= kFbMcBase + kBar0Bytes &&
                            !d.stall_fence)
                            b0_put64(d, fmc - kFbMcBase, seq);
                    } else {
                        break;
                    }
                    i += itot;
                }
            }
        } else {
            break;
        }
        pos += total;
    }
}

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= kBar5Dwords)
        return false;
    if (write)
        d.bar5[dword] = *value;
    else
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
    if (bar == kMesBarDoorbell) {
        if (!write)
            return false;
        DbOp op;
        op.index = static_cast<uint32_t>(off >> 2);
        op.value = *value;
        op.dwords = dwords;
        d.doorbells.push_back(op);
        if (op.index == kMesDoorbellCompute)
            cp_run(d, static_cast<uint32_t>(*value));
        return true;
    }
    if (bar != R3_IO_BAR_VRAM)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > static_cast<uint64_t>(d.bar0.size()))
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;
    BusOp bo;
    bo.bar = bar;
    bo.off = off;
    bo.write = write;
    bo.dwords = dwords;
    d.vram_ops.push_back(bo);
    if (write)
        memcpy(&d.bar0[static_cast<size_t>(off)], value, static_cast<size_t>(dwords) * 4u);
    else
        memcpy(value, &d.bar0[static_cast<size_t>(off)], static_cast<size_t>(dwords) * 4u);
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
static void io_fence(void *ctx)
{
    (void)ctx;
}
static bool io_hdp_flush(void *ctx)
{
    return !static_cast<Dev *>(ctx)->hdp_fail;
}

struct Fixture {
    R3Rt rt;
    R3EvidenceLines ev;
    R3SubstepEnv env;
    R3Io io;
};

static bool build_fixture(Fixture &f, Dev &d)
{
    memset(&f.rt, 0, sizeof(f.rt));
    R3LayoutInputs in;
    memset(&in, 0, sizeof(in));
    in.fb_mc_base = kFbMcBase;
    in.fb_phys_base = 0x6a0c000000ull;
    in.bar0_base = 0x7c0000000ull;
    in.real_vram_size = 4096ull * 1024ull * 1024ull;
    in.vbios_reserve = 1ull * 1024ull * 1024ull;
    in.bar0_size = kBar0Bytes;
    in.gop_end = 20ull * 1024ull * 1024ull;
    in.tmr_size = 0x4000000ull;
    in.gop_end_known = true;
    if (r3_layout_compute(&in, &f.rt.layout) != 0)
        return false;
    f.rt.ready_bits = 0u;
    f.rt.latch_bits = 0u;

    n48log_host_reset();
    r3_ev_init(&f.ev);

    memset(&f.env, 0, sizeof(f.env));
    f.env.rt = &f.rt;
    f.env.ev = &f.ev;
    f.env.fb_mc_base = kFbMcBase;
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
    f.io.fence = io_fence;
    f.io.hdp_flush = io_hdp_flush;
    return true;
}

static uint64_t grant_start(const R3LayoutPlan &plan, const char *tag)
{
    const R3Grant *g = r3_layout_grant(&plan, tag);
    return g ? g->start : 0ull;
}

/* ---- S12: ABI readiness ------------------------------------------------ */

static void test_s12(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "s12 fixture");

    R3SubstepResult r = abi_v11_0_substep(&f.io, nullptr);
    CHECK(!r3_substep_ok(&r), "s12 null env must fail");
    CHECK(strcmp(r.name, "no-env") == 0, "s12 null env name (got %s)", r.name);

    f.rt.layout.ok = false;
    r = abi_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "s12 layout-not-ok must fail");
    CHECK(strcmp(r.name, "layout-not-ok") == 0, "s12 layout name (got %s)", r.name);
    CHECK(r.section == kR3SecPre, "s12 layout section (got %d)", r.section);

    f.rt.layout.ok = true;
    r = abi_v11_0_substep(nullptr, &f.env); /* io is unused by S12 */
    CHECK(r3_substep_ok(&r), "s12 valid layout must pass");
}

/* ---- S13: full compute chain ------------------------------------------- */

static void test_s13_happy_path(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "s13 fixture");

    const R3LayoutPlan &plan = f.rt.layout;
    const uint64_t mes_off = grant_start(plan, "mes");
    const uint64_t cmp_ring_off = mes_off + kMesCmpRingOff;
    const uint64_t cmp_wb_off = mes_off + kMesCmpWbOff;
    const uint64_t cmp_fence_off = cmp_wb_off + kMesWbFenceOff;
    const uint64_t csib_off = grant_start(plan, "csib");
    const uint64_t test_off = grant_start(plan, "test_buf");
    CHECK(mes_off != 0ull && csib_off != 0ull && test_off != 0ull, "s13 grants present");
    d.ring_off = cmp_ring_off;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(r3_substep_ok(&r), "s13 happy path must pass (got %s)", r.name);

    /* (A) RELEASE_MEM preload at the csib grant start: 8-dword form, fence
     * MC address = fb_mc + mes + kMesCmpWbOff + kMesWbFenceOff, seq 1. */
    const uint64_t fence_mc = kFbMcBase + cmp_fence_off;
    const uint32_t rm[8] = {
        b0_32(d, csib_off + 0ull),  b0_32(d, csib_off + 4ull),
        b0_32(d, csib_off + 8ull),  b0_32(d, csib_off + 12ull),
        b0_32(d, csib_off + 16ull), b0_32(d, csib_off + 20ull),
        b0_32(d, csib_off + 24ull), b0_32(d, csib_off + 28ull),
    };
    CHECK(rm[0] == kExpRmW0, "rm w0 0x%x (want 0x%x)", rm[0], kExpRmW0);
    CHECK(rm[1] == kExpRmW1, "rm w1 0x%x (want 0x%x)", rm[1], kExpRmW1);
    CHECK(rm[2] == kExpRmW2, "rm w2 0x%x (want 0x%x)", rm[2], kExpRmW2);
    CHECK(rm[3] == static_cast<uint32_t>(fence_mc & 0xffffffffull),
          "rm fence lo 0x%x (want 0x%llx)", rm[3],
          static_cast<unsigned long long>(fence_mc & 0xffffffffull));
    CHECK(rm[4] == static_cast<uint32_t>(fence_mc >> 32), "rm fence hi 0x%x", rm[4]);
    CHECK(rm[5] == 1u, "rm seq %u", rm[5]);
    CHECK(rm[6] == 0u && rm[7] == 0u, "rm pad w6=0x%x w7=0x%x", rm[6], rm[7]);

    /* (B) ring packets at cmp_ring_off, byte-offset semantics per dword. */
    const uint64_t test_va = kFbMcBase + test_off;
    const uint64_t ib_va = kFbMcBase + csib_off;
    const uint32_t ring[12] = {
        b0_32(d, cmp_ring_off + 0ull),   b0_32(d, cmp_ring_off + 4ull),
        b0_32(d, cmp_ring_off + 8ull),   b0_32(d, cmp_ring_off + 12ull),
        b0_32(d, cmp_ring_off + 16ull),  b0_32(d, cmp_ring_off + 20ull),
        b0_32(d, cmp_ring_off + 24ull),  b0_32(d, cmp_ring_off + 28ull),
        b0_32(d, cmp_ring_off + 32ull),  b0_32(d, cmp_ring_off + 36ull),
        b0_32(d, cmp_ring_off + 40ull),  b0_32(d, cmp_ring_off + 44ull),
    };
    CHECK(ring[0] == 0xC0017900u, "set_uconfig w0 0x%x", ring[0]);
    CHECK(ring[1] == kUconfigWinOff, "set_uconfig reg off 0x%x", ring[1]);
    CHECK(ring[2] == kMagic, "set_uconfig magic 0x%x", ring[2]);
    CHECK(ring[3] == kExpWdW0, "write_data w0 0x%x", ring[3]);
    CHECK(ring[4] == kExpWdW1, "write_data ctrl 0x%x", ring[4]);
    CHECK(ring[5] == static_cast<uint32_t>(test_va & 0xffffffffull),
          "write_data dst lo 0x%x (want 0x%llx)", ring[5],
          static_cast<unsigned long long>(test_va & 0xffffffffull));
    CHECK(ring[6] == static_cast<uint32_t>(test_va >> 32), "write_data dst hi 0x%x", ring[6]);
    CHECK(ring[7] == kMagic, "write_data magic 0x%x", ring[7]);
    CHECK(ring[8] == kExpIbW0, "ib w0 0x%x", ring[8]);
    CHECK(ring[9] == static_cast<uint32_t>(ib_va & ~3ull),
          "ib base lo 0x%x (want 0x%llx)", ring[9],
          static_cast<unsigned long long>(ib_va & ~3ull));
    CHECK(ring[10] == static_cast<uint32_t>(ib_va >> 32), "ib base hi 0x%x", ring[10]);
    CHECK(ring[11] == kExpIbW3, "ib ctrl 0x%x (want 0x%x)", ring[11], kExpIbW3);

    /* (C) submit: one doorbell write, index 6, full wptr (12 dwords). */
    CHECK(d.doorbells.size() == 1, "doorbell count %zu", d.doorbells.size());
    if (d.doorbells.size() == 1) {
        const DbOp &db = d.doorbells[0];
        CHECK(db.index == kMesDoorbellCompute, "doorbell index %u", db.index);
        CHECK(db.value == 12ull, "doorbell wptr %llu", static_cast<unsigned long long>(db.value));
        CHECK(db.dwords == 2u, "doorbell dwords %u", db.dwords);
    }

    /* wptr推进: the WB page carries the 64-bit wptr at +16. */
    CHECK(b0_64(d, cmp_wb_off + kMesWbWptrOff) == 12ull, "wb wptr");

    /* (D) fence fired by the IB's RELEASE_MEM: seq 1 at the fence offset. */
    CHECK(b0_64(d, cmp_fence_off) == 1ull, "fence value");

    /* (E) SCRATCH_REG0 got the magic through the uconfig window. */
    CHECK(d.bar5[kScratchDword] == kMagic, "scratch reg 0x%x", d.bar5[kScratchDword]);

    /* (F) test buffer got the magic through WRITE_DATA. */
    CHECK(b0_32(d, test_off) == kMagic, "test buf 0x%x", b0_32(d, test_off));

    /* Bus channel: every VRAM access is a carveout BYTE OFFSET (< the BAR0
     * window), never an MC address (fb_mc = 0x8000000000 >> kBar0Bytes). */
    for (size_t i = 0; i < d.vram_ops.size(); i++) {
        CHECK(d.vram_ops[i].off < kBar0Bytes && d.vram_ops[i].off < kFbMcBase,
              "vram op %zu is not a carveout offset (bar %u off 0x%llx)", i,
              d.vram_ops[i].bar, static_cast<unsigned long long>(d.vram_ops[i].off));
    }
    bool saw_csib_w = false, saw_ring_w = false, saw_wb_w = false;
    bool saw_ring_r = false, saw_fence_r = false, saw_buf_r = false;
    for (size_t i = 0; i < d.vram_ops.size(); i++) {
        const BusOp &bo = d.vram_ops[i];
        if (bo.write) {
            saw_csib_w = saw_csib_w || bo.off == csib_off;
            saw_ring_w = saw_ring_w || bo.off == cmp_ring_off;
            saw_wb_w = saw_wb_w || (bo.off == cmp_wb_off + kMesWbWptrOff && bo.dwords == 2u);
        } else {
            saw_ring_r = saw_ring_r || bo.off == cmp_ring_off;
            saw_fence_r = saw_fence_r || (bo.off == cmp_fence_off && bo.dwords == 2u);
            saw_buf_r = saw_buf_r || bo.off == test_off;
        }
    }
    CHECK(saw_csib_w, "no bus write at the csib grant start");
    CHECK(saw_ring_w, "no bus write at the compute ring start");
    CHECK(saw_wb_w, "no 2-dword bus write at the WB wptr slot");
    CHECK(saw_ring_r, "no bus drain read of the ring");
    CHECK(saw_fence_r, "no 2-dword bus fence poll");
    CHECK(saw_buf_r, "no bus read of the test buffer");
}

static void test_s13_missing_grant(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "missing-grant fixture");
    /* Drop the test_buf grant from the plan the env points at. */
    bool dropped = false;
    for (uint32_t i = 0; i < f.rt.layout.grant_count; i++) {
        if (strcmp(f.rt.layout.grants[i].tag, "test_buf") == 0) {
            for (uint32_t j = i; j + 1u < f.rt.layout.grant_count; j++)
                f.rt.layout.grants[j] = f.rt.layout.grants[j + 1u];
            f.rt.layout.grant_count--;
            dropped = true;
            break;
        }
    }
    CHECK(dropped, "test_buf grant to drop");
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "missing grant must fail");
    CHECK(strcmp(r.name, "missing-grant") == 0, "name (got %s)", r.name);
    CHECK(r.section == kR3SecPre, "section (got %d)", r.section);
}

static void test_s13_fence_timeout(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fence-timeout fixture");
    d.stall_fence = true;
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "stalled fence must fail");
    CHECK(strcmp(r.name, "fence-timeout") == 0, "name (got %s)", r.name);
    CHECK(r.section == kR3SecChk, "section (got %d)", r.section);
    CHECK(r.step == 6u, "step (got %u)", r.step);
}

static void test_s13_scratch_mismatch(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "scratch-mismatch fixture");
    d.scratch_bad = true;
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "scratch mismatch must fail");
    CHECK(strcmp(r.name, "scratch-mismatch") == 0, "name (got %s)", r.name);
    CHECK(r.section == kR3SecChk, "section (got %d)", r.section);
    CHECK(r.step == 7u, "step (got %u)", r.step);
}

static void test_s13_buf_mismatch(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "buf-mismatch fixture");
    d.buf_bad = true;
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "test-buffer mismatch must fail");
    CHECK(strcmp(r.name, "buf-mismatch") == 0, "name (got %s)", r.name);
    CHECK(r.section == kR3SecChk, "section (got %d)", r.section);
    CHECK(r.step == 8u, "step (got %u)", r.step);
}

static void test_s13_submit_abort(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "submit-abort fixture");
    d.hdp_fail = true;
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(&f.io, &f.env);
    CHECK(!r3_substep_ok(&r), "hdp_flush failure must abort the submit");
    CHECK(strcmp(r.name, "submit-tail-fail") == 0, "name (got %s)", r.name);
    CHECK(r.section == kR3SecSeq, "section (got %d)", r.section);
    CHECK(r.step == 5u, "step (got %u)", r.step);
    CHECK(d.doorbells.empty(), "no doorbell may be rung after a failed hdp_flush");
}

static void test_s13_null_io(void)
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "null-io fixture");
    d.ring_off = grant_start(f.rt.layout, "mes") + kMesCmpRingOff;

    R3SubstepResult r = compute_v11_0_substep(nullptr, &f.env);
    CHECK(!r3_substep_ok(&r), "null io must fail");
    CHECK(strcmp(r.name, "null-io-or-env") == 0, "name (got %s)", r.name);

    r = compute_v11_0_substep(&f.io, nullptr);
    CHECK(!r3_substep_ok(&r), "null env must fail");
    CHECK(strcmp(r.name, "null-io-or-env") == 0, "name (got %s)", r.name);
}

} /* namespace */

int main(void)
{
    test_s12();
    test_s13_happy_path();
    test_s13_missing_grant();
    test_s13_fence_timeout();
    test_s13_scratch_mismatch();
    test_s13_buf_mismatch();
    test_s13_submit_abort();
    test_s13_null_io();

    printf("test_compute_v11: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
