/*
 * test_mes_api.cpp - host state-machine test for the MES 11.0 substep (K8).
 *
 * The R3Io stub plays the GPU: BAR5 is a dword array (MES/CP registers), BAR0 is the
 * 256 MiB carveout, and BAR2 doorbells are emulated. Writing doorbell 0x16 with a
 * non-zero pointer makes the stub act as the MES scheduler: it parses the 64-dword API
 * frame and the trailing 6-dword query frame out of the SCH ring, records the API
 * fields, writes the completion status (the address the API packet names) and the ring
 * fence (WB+32), exactly as mes_v11_0_submit_pkt_and_poll_completion does.
 *
 * Branches: full chain, ADD_QUEUE(COMPUTE) api-status failure, ADD_QUEUE(GFX) failure
 * recorded while COMPUTE succeeds, CP_MES_CNTL start readback recorded (not gated),
 * CP_MES_GP3_LO version recorded (0/all-ones not gated), read faults fail closed,
 * SET_HW api-status failure, fence timeout, and the PRE failures (PSP/GFX ready, grant).
 *
 * Planted-break guards:
 *   - CP_MES_PRGRM_CNTR_START is compared against a header parse done HERE, so a
 *     hard-coded mes_uc_start_addr fails (tmp/programmer/K8/planted_break_*.log);
 *   - the happy path requires an ADD_QUEUE(GFX) frame, so skipping S8 fails;
 *   - the API frame must arrive on doorbell 0x16 and the KIQ kick must use 0x18, so
 *     swapping the two doorbells fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "amdgpu_ip.h"
#include "fw_table.h"
#include "amd/mes_v11_0.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_fwhdr.h"
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
constexpr uint32_t kGc0 = 0x10000u;
constexpr uint32_t kGc1 = 0x20000u;
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

struct ApiOp {
    uint32_t opcode;
    uint64_t status_addr;
    uint64_t status_value;
    uint32_t queue_type;
    uint32_t doorbell_offset;
    uint32_t flags;
    uint32_t pipe_id;
    uint32_t queue_id;
    uint64_t mqd_addr;
    uint64_t wptr_addr;
    /* SET_HW_RESOURCES */
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint64_t sch_ctx;
    uint64_t query_fence;
    uint32_t hw_flags;
    uint32_t os_tdr;
    uint32_t gc_base1;
};

struct FenceOp {
    uint64_t addr;
    uint64_t value;
};

struct DbOp {
    uint32_t index;
    uint32_t value;
    uint32_t dwords;
};

struct RegRead {
    uint32_t dword;
    uint32_t me;
    uint32_t pipe;
};

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> bar5;
    uint64_t now;
    std::vector<uint32_t> writes;
    std::vector<DbOp> doorbells;
    std::vector<ApiOp> apis;
    std::vector<FenceOp> fences;
    std::vector<char> kick_order; /* 'f' fence call, 'd' doorbell write */
    std::vector<RegRead> reads;
    uint32_t cur_me;
    uint32_t cur_pipe;
    uint32_t prgrm_lo[2];
    uint32_t prgrm_hi[2];
    bool prgrm_seen[2];
    uint32_t gp3_reads[2];
    uint32_t mes_cntl_reset;
    uint32_t mes_cntl_active;
    uint32_t sch_wptr_seen;
    bool readback_ok;
    bool cntl_readback_fail;
    bool gp3_read_fail;
    int stall_status_op;
    int stall_status_qtype;
    bool stall_fence;
    int stall_fence_op;
    bool gfx_queued;

    Dev()
        : bar0(static_cast<size_t>(kBar0Bytes), 0xa5u),
          bar5(kBar5Dwords, 0u),
          now(1000),
          cur_me(0),
          cur_pipe(0),
          prgrm_lo{0, 0},
          prgrm_hi{0, 0},
          prgrm_seen{false, false},
          gp3_reads{0, 0},
          mes_cntl_reset(0),
          mes_cntl_active(0),
          sch_wptr_seen(0),
          readback_ok(true),
          cntl_readback_fail(false),
          gp3_read_fail(false),
          stall_status_op(-1),
          stall_status_qtype(-1),
          stall_fence(false),
          stall_fence_op(-1),
          gfx_queued(false)
    {
        bar5[kGc1 + kRegMesGp3Lo] = 0x00770000u;
        bar5[kGc1 + kRegMesCntl] = 0u;
        bar5[kGc0 + kRegCpHqdActive] = 1u;
        bar5[kGc0 + kRegCpGfxHqdActive] = 1u;
    }
};

static R3LayoutPlan g_plan;

static uint32_t b0_32(const Dev &d, uint64_t off)
{
    uint32_t v = 0;
    if (off + 4ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&v, &d.bar0[static_cast<size_t>(off)], 4u);
    return v;
}

static void b0_put64(Dev &d, uint64_t off, uint64_t v)
{
    if (off + 8ull <= static_cast<uint64_t>(d.bar0.size()))
        memcpy(&d.bar0[static_cast<size_t>(off)], &v, 8u);
}

static uint64_t grant_mc(uint32_t sub_off)
{
    const R3Grant *g = r3_layout_grant(&g_plan, "mes");
    return kFbMcBase + (g ? g->start : 0ull) + sub_off;
}

static uint64_t grant_bar0(uint32_t sub_off)
{
    const R3Grant *g = r3_layout_grant(&g_plan, "mes");
    return (g ? g->start : 0ull) + sub_off;
}

static bool want_status(const Dev &d, uint32_t op, uint32_t qtype)
{
    if (d.stall_status_op != static_cast<int>(op))
        return true;
    return d.stall_status_qtype >= 0 && static_cast<uint32_t>(d.stall_status_qtype) != qtype;
}

static bool want_fence(const Dev &d, uint32_t op)
{
    if (!d.stall_fence)
        return true;
    return d.stall_fence_op >= 0 && static_cast<uint32_t>(d.stall_fence_op) != op;
}

/* Act as the MES scheduler: consume API frames [sch_wptr_seen, wptr). */
static void mes_run(Dev &d, uint32_t wptr)
{
    const uint64_t ring = grant_bar0(kMesSchRingOff);
    uint32_t pos = d.sch_wptr_seen;
    while (pos + kMesApiFrameDwords + kMesApiFrameDwords <= wptr) {
        const uint64_t base = ring + static_cast<uint64_t>(pos) * 4ull;
        const uint32_t hdr = b0_32(d, base);
        const uint32_t type = hdr & 0xfu;
        const uint32_t op = (hdr >> 4) & 0xffu;
        if (type != kMesApiTypeScheduler)
            break;

        ApiOp a;
        memset(&a, 0, sizeof(a));
        a.opcode = op;
        if (op == kMesApiSetHwRsrc) {
            MesApiSetHwResources p;
            memcpy(&p, &d.bar0[static_cast<size_t>(base)], sizeof(p));
            a.status_addr = p.api_status.api_completion_fence_addr;
            a.status_value = p.api_status.api_completion_fence_value;
            a.vmid_mask_mmhub = p.vmid_mask_mmhub;
            a.vmid_mask_gfxhub = p.vmid_mask_gfxhub;
            a.sch_ctx = p.g_sch_ctx_gpu_mc_ptr;
            a.query_fence = p.query_status_fence_gpu_mc_ptr;
            a.hw_flags = p.flags;
            a.os_tdr = p.os_tdr_timeout_in_sec;
            a.gc_base1 = p.gc_base[1];
        } else if (op == kMesApiAddQueue) {
            MesApiAddQueue p;
            memcpy(&p, &d.bar0[static_cast<size_t>(base)], sizeof(p));
            a.status_addr = p.api_status.api_completion_fence_addr;
            a.status_value = p.api_status.api_completion_fence_value;
            a.queue_type = p.queue_type;
            a.doorbell_offset = p.doorbell_offset;
            a.flags = p.flags;
            a.pipe_id = p.pipe_id;
            a.queue_id = p.queue_id;
            a.mqd_addr = p.mqd_addr;
            a.wptr_addr = p.wptr_addr;
            if (p.queue_type == kMesQueueTypeGfx)
                d.gfx_queued = true;
        } else if (op == kMesApiSetHwRsrc1) {
            /* S5.1 (opcode 19): api_status at offset 4 (方案 v3 §6.5). */
            MesApiSetHwResources1 p1;
            memcpy(&p1, &d.bar0[static_cast<size_t>(base)], sizeof(p1));
            a.status_addr = p1.api_status.api_completion_fence_addr;
            a.status_value = p1.api_status.api_completion_fence_value;
        } else {
            MesApiQuerySchedulerStatus p;
            memcpy(&p, &d.bar0[static_cast<size_t>(base)], sizeof(p));
            a.status_addr = p.api_status.api_completion_fence_addr;
            a.status_value = p.api_status.api_completion_fence_value;
        }
        d.apis.push_back(a);

        if (a.status_addr >= kFbMcBase && want_status(d, op, a.queue_type))
            b0_put64(d, a.status_addr - kFbMcBase, 1ull);

        const uint64_t fb = ring + static_cast<uint64_t>(pos + kMesApiFrameDwords) * 4ull;
        MesApiQuerySchedulerStatus fq;
        memcpy(&fq, &d.bar0[static_cast<size_t>(fb)], sizeof(fq));
        FenceOp f;
        f.addr = fq.api_status.api_completion_fence_addr;
        f.value = fq.api_status.api_completion_fence_value;
        d.fences.push_back(f);
        if (f.addr >= kFbMcBase && want_fence(d, op))
            b0_put64(d, f.addr - kFbMcBase, f.value);

        pos += kMesApiFrameDwords + kMesApiFrameDwords;
    }
    d.sch_wptr_seen = wptr;
}

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= kBar5Dwords)
        return false;

    const uint32_t grbm = kGc1 + kRegGrbmGfxCntl;
    if (write) {
        const uint32_t v = *value;
        d.writes.push_back(dword);
        d.bar5[dword] = v;
        if (dword == grbm) {
            d.cur_pipe = v & 0x3u;
            d.cur_me = (v >> 2) & 0x3u;
        } else if (dword == kGc1 + kRegMesPrgrmCntrStart) {
            d.prgrm_lo[d.cur_pipe] = v;
            d.prgrm_seen[d.cur_pipe] = true;
        } else if (dword == kGc1 + kRegMesPrgrmCntrStartHi) {
            d.prgrm_hi[d.cur_pipe] = v;
        } else if (dword == kGc1 + kRegMesCntl) {
            if (v & (kMesCntlPipe0Reset | kMesCntlPipe1Reset))
                d.mes_cntl_reset = v;
            if (v & (kMesCntlPipe0Active | kMesCntlPipe1Active))
                d.mes_cntl_active = v;
        }
        return true;
    }

    RegRead rr;
    rr.dword = dword;
    rr.me = d.cur_me;
    rr.pipe = d.cur_pipe;
    d.reads.push_back(rr);
    if (dword == kGc1 + kRegMesGp3Lo && d.cur_me == 3u && d.cur_pipe < 2u)
        d.gp3_reads[d.cur_pipe]++;
    if (d.gp3_read_fail && dword == kGc1 + kRegMesGp3Lo)
        return false;
    if (dword == kGc1 + kRegMesCntl) {
        if (d.cntl_readback_fail && d.bar5[dword] != 0u)
            return false;
        uint32_t v = d.bar5[dword];
        if (!d.readback_ok)
            v &= ~(kMesCntlPipe0Active | kMesCntlPipe1Active);
        *value = v;
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
    if (bar == kMesBarDoorbell) {
        if (!write || (dwords != 1u && dwords != 2u) ||
            (off & (dwords == 2u ? 7ull : 3ull)) != 0ull)
            return false;
        DbOp op;
        op.index = static_cast<uint32_t>(off >> 2);
        op.value = static_cast<uint32_t>(*value);
        op.dwords = dwords;
        d.doorbells.push_back(op);
        d.kick_order.push_back('d');
        if (op.index == 0x16u && op.value != 0u)
            mes_run(d, op.value);
        return true;
    }
    if (bar != R3_IO_BAR_VRAM)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > static_cast<uint64_t>(d.bar0.size()))
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;
    if (write) {
        memcpy(&d.bar0[static_cast<size_t>(off)], value, static_cast<size_t>(dwords) * 4u);
    } else {
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

static bool build_fixture(Fixture &f, Dev &d, bool psp_ready, bool gfx_ready)
{
    memset(&f.rt, 0, sizeof(f.rt));
    f.ip = IPBaseTable();
    f.ip.set(IPBlock::GC, kGc0);
    f.ip.setBase(IPBlock::GC, 1, kGc1);
    f.ip.set(IPBlock::MMHUB, kMmhub0);
    f.ip.setBase(IPBlock::MMHUB, 1, 0x1a000u);   /* mmhub seg1 for golden registers */
    f.ip.set(IPBlock::OSSSYS, kOsssys0);
    /* NBIO seg2 for doorbell aperture (RCC_DOORBELL_APER_EN at base+0xC0). */
    f.ip.setBase(IPBlock::NBIO, 2, 0xd20u);

    /* Pre-populate MMHUB FB registers so S-1 hub pre can read them:
     * fb_off_raw=0x6a0c → fb_phys_base=0x6a0c000000, fb_base_raw=0x8000 → mc_base=0x8000000000,
     * fb_top_raw=0x80ff → mc_top=0x80ffffffff. mmhub seg1 = 0x1a000. */
    const uint32_t mmhub1 = 0x1a000u;
    d.bar5[mmhub1 + 0x08D7u] = 0x6a0cu; /* MMMC_VM_FB_OFFSET */
    d.bar5[mmhub1 + 0x08ECu] = 0x8000u; /* FB_LOCATION_BASE */
    d.bar5[mmhub1 + 0x08EDu] = 0x80ffu; /* FB_LOCATION_TOP */
    /* GC GCMC_VM_FB_OFFSET = 0 (triggers the C write branch). */
    d.bar5[kGc0 + 0x15a7u] = 0u;
    /* NBIF doorbell aperture (NBIO seg2 + 0xC0 = 0xd20 + 0xC0 = 0xDE0). */
    d.bar5[0xd20u + 0xC0u] = 0u;
    /* Pre-set RMW target registers with reset defaults for value-level assertions.
     * MX_L1: set bit5=1, bits7-10=0xf to verify mask clears them.
     * FAULT_CNTL: set bits30-31=1 to verify mask clears them. */
    d.bar5[kGc0 + 0x1688u] = 0x01fffe00u; /* GCVM_CONTEXT0_CNTL reset */
    d.bar5[kGc0 + 0x167fu] = 0x7a0u;       /* MX_L1: bit5=1, ECO=0xf to test mask */
    d.bar5[kGc0 + 0x15bcu] = 0x00080602u;  /* GCVM_L2_CNTL reset */
    d.bar5[kGc0 + 0x15c4u] = 0xc0000000u;  /* FAULT_CNTL: bits30-31=1 to test mask clears them */
    d.bar5[kGc0 + 0x15c5u] = 0x000a0000u;  /* GCVM_FAULT_CNTL2 default */
    d.bar5[kGc0 + 0x15d4u] = 0xe1u;        /* GCVM_L2_CNTL4 default+bit5: distinguish abs vs RMW */
    d.bar5[kGc0 + 0x15beu] = 0x80120007u;  /* GCVM_L2_CNTL3 default */
    d.bar5[kGc0 + 0x15dau] = 0x3fe0u;      /* GCVM_L2_CNTL5 default */
    d.bar5[kGc0 + 0x167bu] = 0u;            /* GCMC_VM_AGP_BOT reset */
    d.bar5[kGc0 + 0x1678u] = 0u;            /* GCMC_VM_FB_LOCATION_BASE reset */
    d.bar5[kGc0 + 0x1679u] = 0u;            /* GCMC_VM_FB_LOCATION_TOP reset */
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

    f.rt.ready_bits = R3_IP_BIT(R3Ip::Psp);
    if (gfx_ready)
        f.rt.ready_bits |= R3_IP_BIT(R3Ip::Gfx);
    if (!psp_ready)
        f.rt.ready_bits &= ~R3_IP_BIT(R3Ip::Psp);
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
    f.io.hdp_flush = [](void *) { return true; };
    return true;
}

/* Step2: register write set from the declaration table (the single source of truth).
 * Replaces the hand-maintained kRegs[] list. Doorbells kept explicit. */
static void register_mes_rows(uint32_t gc0, uint32_t gc1)
{
    const uint32_t rc = getMesRowCount();
    const R3WriteRowReg *rows = getMesRows();
    for (uint32_t i = 0; i < rc; i++) {
        uint32_t addr;
        if (rows[i].golden) {
            const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
            addr = (g->block == IPBlock::GC && g->base_idx == 1) ? gc1 : gc0;
            addr += g->offset;
        } else {
            /* Non-golden rows: GC seg0/1 or NBIO seg2 (S-1 hub rows). */
            if (rows[i].block == static_cast<uint8_t>(IPBlock::NBIO)) {
                addr = 0xd20u + rows[i].off;  /* NBIO seg2 = 0xd20 */
            } else {
                const uint32_t base = rows[i].base_idx == 0 ? gc0 : gc1;
                addr = base + rows[i].off;
            }
        }
        CHECK(r3_wg_register_reg(addr, 1u, rows[i].cls, WSubstep::Mes, "reg:mes") == 0,
              "register mes row %u (addr 0x%x)", i, addr);
    }
}

struct RunResult {
    R3SubstepResult r;
    bool ready;
    uint64_t deny_before;
    uint64_t deny_after;
};

static RunResult run(Fixture &f)
{
    g_plan = f.rt.layout;

    r3_writeguard_install_mm();
    const R3Grant *g = r3_layout_grant(&g_plan, "mes");
    if (g) {
        CHECK(r3_wg_register_layout(static_cast<uint32_t>(g->start >> 2),
                                    static_cast<uint32_t>(g->size >> 2),
                                    WSubstep::Mes, "layout:mes") == 0,
              "register mes grant");
    }
    register_mes_rows(kGc0, kGc1);
    const uint32_t dc = getMesDoorbellCount();
    const R3WriteRowAbs *dbs = getMesDoorbells();
    for (uint32_t i = 0; i < dc; i++) {
        CHECK(r3_wg_register_reg(dbs[i].addr, dbs[i].dwords, dbs[i].cls,
                                 WSubstep::Mes, "reg:mes-db") == 0,
              "register mes doorbell %u (addr 0x%x)", i, dbs[i].addr);
    }

    RunResult out;
    out.deny_before = r3_writeguard_deny_count();
    out.r = mes_v11_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    out.ready = (f.rt.ready_bits & R3_IP_BIT(R3Ip::Mes)) != 0u;
    return out;
}

static bool ev_has(const Fixture &f, const char *tag)
{
    (void)f;
    return n48log_host_has_tag(tag);
}

static const char *prop_get(const Fixture &f, const char *key)
{
    for (int i = 0; i < f.store.count; i++) {
        if (strcmp(f.store.props[i].key, key) == 0)
            return f.store.props[i].value;
    }
    return nullptr;
}

static bool has_db(const Dev &d, uint32_t index, uint32_t value)
{
    for (const DbOp &op : d.doorbells) {
        if (op.index == index && op.value == value)
            return true;
    }
    return false;
}

/* Independent header parse: the substep must use this, not a constant. ME-01: the
 * header value is already an MC address (nullarc mesboot.c:139-142) and is written
 * raw >>2; there is no fb_mc_base relocation. */
static bool expected_uc(uint64_t *sched, uint64_t *kiq)
{
    const FwBlob *b = fw_get(FwId::GC11_MES_2);
    R3FwImage img;
    if (!b || !b->data ||
        r3_fw_parse_mes(b->data, b->size, kFbMcBase, &img) != R3_FW_HDR_OK)
        return false;
    *sched = img.mes_uc_start_addr;
    b = fw_get(FwId::GC11_MES1);
    if (!b || !b->data || r3_fw_parse_mes(b->data, b->size, kFbMcBase, &img) != R3_FW_HDR_OK)
        return false;
    *kiq = img.mes_uc_start_addr;
    return *sched != 0ull && *kiq != 0ull;
}


/* Verify every address field in an MQD was written as a MC address, not BAR0.
 * has_eop: build_compute_mqd writes EOP_BASE (165/166); build_gfx_mqd does not.
 * gfx_align: GFX WPTR_POLL uses ~3 alignment (0xfffffffc), compute uses ~7 (0xfffffff8). */
static void check_mqd_mc(const Dev &d, const char *label,
                          uint32_t mqd_off, uint32_t ring_off,
                          uint32_t wb_off, uint32_t eop_off,
                          bool has_eop, bool gfx_align)
{
    const uint64_t mqd_bar0 = grant_bar0(mqd_off);
    const uint64_t mqd_mc = grant_mc(mqd_off);
    const uint64_t ring_mc = grant_mc(ring_off);
    const uint64_t wb_mc = grant_mc(wb_off);
    const uint64_t wb_wptr_mc = wb_mc + kMesWbWptrOff;
    const uint32_t poll_mask = gfx_align ? 0xfffffffcull : 0xfffffff8ull;
    uint32_t v;

    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 128u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(mqd_mc & 0xfffffffcull), "%s MQD_BASE_LO 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 129u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(mqd_mc >> 32), "%s MQD_BASE_HI 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 136u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(ring_mc >> 8), "%s PQ_BASE_LO 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 137u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(ring_mc >> 40), "%s PQ_BASE_HI 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 139u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(wb_mc & 0xfffffffcull), "%s RPTR_REPORT_LO 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 140u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>((wb_mc >> 32) & 0xffffu), "%s RPTR_REPORT_HI 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 141u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>(wb_wptr_mc & poll_mask), "%s WPTR_POLL_LO 0x%08x", label, v);
    memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 142u * 4u)], 4u);
    CHECK(v == static_cast<uint32_t>((wb_wptr_mc >> 32) & 0xffffu), "%s WPTR_POLL_HI 0x%08x", label, v);

    if (has_eop) {
        const uint64_t eop_mc = grant_mc(eop_off);
        memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 165u * 4u)], 4u);
        CHECK(v == static_cast<uint32_t>(eop_mc >> 8), "%s EOP_BASE_LO 0x%08x", label, v);
        memcpy(&v, &d.bar0[static_cast<size_t>(mqd_bar0 + 166u * 4u)], 4u);
        CHECK(v == static_cast<uint32_t>(eop_mc >> 40), "%s EOP_BASE_HI 0x%08x", label, v);
    }
}

/* Verify HQD register values (last program_hqd was SCH, so those are the HQD values). */
static void check_hqd_mc(const Dev &d)
{
    CHECK(d.bar5[kGc0 + kRegCpMqdBaseAddr] ==
              static_cast<uint32_t>(grant_mc(kMesSchMqdOff) & 0xfffffffcull),
          "SCH HQD MQD_BASE_LO 0x%08x", d.bar5[kGc0 + kRegCpMqdBaseAddr]);
    CHECK(d.bar5[kGc0 + kRegCpMqdBaseAddrHi] ==
              static_cast<uint32_t>(grant_mc(kMesSchMqdOff) >> 32),
          "SCH HQD MQD_BASE_HI 0x%08x", d.bar5[kGc0 + kRegCpMqdBaseAddrHi]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqBase] ==
              static_cast<uint32_t>(grant_mc(kMesSchRingOff) >> 8),
          "SCH HQD PQ_BASE_LO 0x%08x", d.bar5[kGc0 + kRegCpHqdPqBase]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqBaseHi] ==
              static_cast<uint32_t>(grant_mc(kMesSchRingOff) >> 40),
          "SCH HQD PQ_BASE_HI 0x%08x", d.bar5[kGc0 + kRegCpHqdPqBaseHi]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqRptrReportAddr] ==
              static_cast<uint32_t>(grant_mc(kMesSchWbOff) & 0xfffffffcull),
          "SCH HQD RPTR_REPORT_LO 0x%08x", d.bar5[kGc0 + kRegCpHqdPqRptrReportAddr]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqRptrReportAddrHi] ==
              static_cast<uint32_t>((grant_mc(kMesSchWbOff) >> 32) & 0xffffu),
          "SCH HQD RPTR_REPORT_HI 0x%08x", d.bar5[kGc0 + kRegCpHqdPqRptrReportAddrHi]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqWptrPollAddr] ==
              static_cast<uint32_t>((grant_mc(kMesSchWbOff) + kMesWbWptrOff) & 0xfffffff8u),
          "SCH HQD WPTR_POLL_LO 0x%08x", d.bar5[kGc0 + kRegCpHqdPqWptrPollAddr]);
    CHECK(d.bar5[kGc0 + kRegCpHqdPqWptrPollAddrHi] ==
              static_cast<uint32_t>(((grant_mc(kMesSchWbOff) + kMesWbWptrOff) >> 32) & 0xffffu),
          "SCH HQD WPTR_POLL_HI 0x%08x", d.bar5[kGc0 + kRegCpHqdPqWptrPollAddrHi]);
}

static void check_common_success(Fixture &f, Dev &d, const RunResult &rr)
{
    CHECK(kMesDoorbellSch == 0x16u && kMesDoorbellKiq == 0x18u,
          "doorbell constants swapped: sch=0x%x kiq=0x%x", kMesDoorbellSch,
          kMesDoorbellKiq);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(rr.ready, "MES ready bit not set");
    CHECK(rr.deny_after == rr.deny_before, "writeguard denied %llu writes",
          static_cast<unsigned long long>(rr.deny_after - rr.deny_before));

    /* S0: CP unhalt - note that S-1 hub-pre writes happen before S0 now, so CP_MEC/ME_CNTL
     * are NOT the first writes. Verify they exist somewhere in the write stream. */
    CHECK(d.writes.size() >= 2u, "S0 writes missing");
    bool found_mec = false, found_me = false;
    for (uint32_t i = 0; i < d.writes.size(); i++) {
        if (d.writes[i] == kGc1 + 0x0802u) found_mec = true;
        if (d.writes[i] == kGc1 + 0x0803u) found_me = true;
    }
    CHECK(found_mec, "CP_MEC_CNTL not written");
    CHECK(found_me, "CP_ME_CNTL not written");
    CHECK(d.bar5[kGc1 + 0x0802u] == 0u, "CP_MEC_CNTL=0");
    CHECK(d.bar5[kGc1 + 0x0803u] == kMesCpMeCntlCeHalt, "CP_ME_CNTL=0x01000000 (CE_HALT)");
    CHECK(!r3_writeguard_manifest()->truncated, "write manifest truncated");
    CHECK(rr.r.reading_count == 4u, "readings=%u", rr.r.reading_count);

    /* B-01: every MES doorbell must be preceded by the contract-6 fence call. */
    CHECK(!d.kick_order.empty(), "no kick sequence recorded");
    for (size_t i = 0; i < d.kick_order.size(); i++) {
        if (d.kick_order[i] == 'd') {
            CHECK(i > 0u && d.kick_order[i - 1u] == 'f',
                  "doorbell %zu not preceded by fence", i);
        }
    }
    if (rr.r.reading_count == 4u) {
        CHECK(rr.r.readings[0].value ==
                  (kMesCntlPipe0Active | kMesCntlPipe1Active), "cntl reading=0x%x",
              rr.r.readings[0].value);
        CHECK(rr.r.readings[1].value == 0x00770000u, "gp3 sched reading=0x%x",
              rr.r.readings[1].value);
        CHECK(rr.r.readings[2].value == 0x00770000u, "gp3 kiq reading=0x%x",
              rr.r.readings[2].value);
        CHECK(strcmp(rr.r.readings[3].what, "COMPUTE HQD") == 0 &&
                  rr.r.readings[3].value == 1u, "compute reading %s=0x%x",
              rr.r.readings[3].what, rr.r.readings[3].value);
    }
    CHECK(d.bar5[kGc0 + kRegCpRbDoorbellRangeLower] == (kMesDoorbellGfx << 2),
          "gfx doorbell range lower=0x%x", d.bar5[kGc0 + kRegCpRbDoorbellRangeLower]);
    CHECK(d.bar5[kGc0 + kRegCpRbDoorbellRangeUpper] == kCpHqdDoorbellRangeUpper,
          "gfx doorbell range upper=0x%x", d.bar5[kGc0 + kRegCpRbDoorbellRangeUpper]);
    CHECK(prop_get(f, "N48R3EVID-mes-done") &&
              strcmp(prop_get(f, "N48R3EVID-mes-done"), "Yes") == 0,
          "done property");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-compute") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-compute"), "ok") == 0,
          "compute property");
    CHECK(prop_get(f, "N48R3EVID-mes-hub") &&
              strcmp(prop_get(f, "N48R3EVID-mes-hub"),
                     "fb_off_raw=0x6a0c\nc=written\nnbif=0x1/bit1\nrows=29") == 0,
          "mes-hub property=%s", prop_get(f, "N48R3EVID-mes-hub"));
    CHECK(prop_get(f, "N48R3EVID-mes-set-hw-1") &&
              strcmp(prop_get(f, "N48R3EVID-mes-set-hw-1"), "op19=skip ver=0x0") == 0,
          "mes-set-hw-1 property=%s", prop_get(f, "N48R3EVID-mes-set-hw-1"));
    CHECK(prop_get(f, "N48R3EVID-mes-queues") &&
              strcmp(prop_get(f, "N48R3EVID-mes-queues"), "prefix=ok") == 0,
          "mes-queues property=%s", prop_get(f, "N48R3EVID-mes-queues"));
    if (d.apis.size() == 4u && d.fences.size() == 4u) {
        char want[256];
        snprintf(want, sizeof(want),
                 "compute=ok fence=0x%llx status=0x1 mqd_mc=0x%llx wptr_mc=0x%llx "
                 "db=%u; gfx=ok fence=0x%llx status=0x1 mqd_mc=0x%llx "
                 "wptr_mc=0x%llx db=%u",
                 static_cast<unsigned long long>(d.fences[2].value),
                 static_cast<unsigned long long>(d.apis[2].mqd_addr),
                 static_cast<unsigned long long>(d.apis[2].wptr_addr),
                 kMesDoorbellCompute,
                 static_cast<unsigned long long>(d.fences[3].value),
                 static_cast<unsigned long long>(d.apis[3].mqd_addr),
                 static_cast<unsigned long long>(d.apis[3].wptr_addr),
                 kMesDoorbellGfx);
        const char *rq = prop_get(f, "N48R3EVID-mes-addq-resp");
        CHECK(rq && strcmp(rq, want) == 0, "mes-addq-resp=%s want=%s",
              rq ? rq : "(null)", want);
    }
    for (const char *tag : {"mes-pre", "mes-start", "mes-version", "mes-kiq", "mes-sch",
                            "mes-set-hw", "mes-query", "mes-addq-compute",
                            "mes-addq-gfx", "mes-queues", "mes-addr"}) {
        CHECK(ev_has(f, tag), "missing evidence tag %s", tag);
    }

    uint64_t sched = 0, kiq = 0;
    CHECK(expected_uc(&sched, &kiq), "firmware header parse failed");
    CHECK(d.prgrm_seen[0] && d.prgrm_seen[1], "PRGRM_CNTR_START not written for both pipes");
    CHECK(d.prgrm_lo[0] == static_cast<uint32_t>(sched >> 2) &&
              d.prgrm_hi[0] == static_cast<uint32_t>(sched >> 34),
          "SCH start lo/hi 0x%08x:0x%08x want 0x%08x:0x%08x", d.prgrm_lo[0],
          d.prgrm_hi[0], static_cast<uint32_t>(sched >> 2),
          static_cast<uint32_t>(sched >> 34));
    CHECK(d.prgrm_lo[1] == static_cast<uint32_t>(kiq >> 2) &&
              d.prgrm_hi[1] == static_cast<uint32_t>(kiq >> 34),
          "KIQ start lo/hi 0x%08x:0x%08x want 0x%08x:0x%08x", d.prgrm_lo[1],
          d.prgrm_hi[1], static_cast<uint32_t>(kiq >> 2),
          static_cast<uint32_t>(kiq >> 34));
    CHECK((d.mes_cntl_reset & (kMesCntlPipe0Reset | kMesCntlPipe1Reset)) ==
              (kMesCntlPipe0Reset | kMesCntlPipe1Reset),
          "reset bits missing: 0x%08x", d.mes_cntl_reset);
    CHECK((d.mes_cntl_active & (kMesCntlPipe0Active | kMesCntlPipe1Active)) ==
              (kMesCntlPipe0Active | kMesCntlPipe1Active),
          "active bits missing: 0x%08x", d.mes_cntl_active);
    CHECK(d.gp3_reads[0] > 0u && d.gp3_reads[1] > 0u, "GP3 version read pipes=%u/%u",
          d.gp3_reads[0], d.gp3_reads[1]);

    CHECK(has_db(d, 0x18u, 0u), "KIQ doorbell 0x18 not kicked");
    CHECK(has_db(d, 0x16u, 0u), "SCH doorbell 0x16 not kicked at init");
    CHECK(d.apis.size() == 4u, "api count=%zu", d.apis.size());
    if (d.apis.size() == 4u) {
        CHECK(d.apis[0].opcode == kMesApiSetHwRsrc, "op0=%u", d.apis[0].opcode);
        CHECK(d.apis[1].opcode == kMesApiQuerySchedulerStatus, "op1=%u", d.apis[1].opcode);
        CHECK(d.apis[2].opcode == kMesApiAddQueue &&
                  d.apis[2].queue_type == kMesQueueTypeCompute,
              "op2=%u type=%u", d.apis[2].opcode, d.apis[2].queue_type);
        CHECK(d.apis[3].opcode == kMesApiAddQueue &&
                  d.apis[3].queue_type == kMesQueueTypeGfx,
              "op3=%u type=%u", d.apis[3].opcode, d.apis[3].queue_type);
    }
    CHECK(d.fences.size() == 4u, "fence packet count=%zu", d.fences.size());
    for (size_t i = 0; i < d.fences.size(); i++) {
        CHECK(d.fences[i].addr == grant_mc(kMesSchWbOff) + kMesWbFenceOff,
              "fence %zu addr=0x%llx", i,
              static_cast<unsigned long long>(d.fences[i].addr));
        CHECK(d.fences[i].value == i + 1u, "fence %zu value=%llu", i,
              static_cast<unsigned long long>(d.fences[i].value));
    }
    for (const ApiOp &a : d.apis) {
        CHECK(a.status_addr == grant_mc(kMesSchWbOff) + kMesWbStatusOff,
              "api %u status addr=0x%llx", a.opcode,
              static_cast<unsigned long long>(a.status_addr));
        CHECK(a.status_value == 1u, "api %u status value=%llu", a.opcode,
              static_cast<unsigned long long>(a.status_value));
    }

    if (d.apis.size() == 4u) {
        const ApiOp &sh = d.apis[0];
        CHECK(sh.vmid_mask_mmhub == 0xffffff00u, "mmhub vmid mask=0x%x", sh.vmid_mask_mmhub);
        CHECK(sh.vmid_mask_gfxhub == 0xffffff00u, "gfxhub vmid mask=0x%x",
              sh.vmid_mask_gfxhub);
        CHECK(sh.sch_ctx == grant_mc(kMesSchWbOff) + kMesWbSchCtxOff, "sch ctx");
        CHECK(sh.query_fence == grant_mc(kMesSchWbOff) + kMesWbQueryFenceOff,
              "query fence");
        CHECK(sh.hw_flags == (kMesFlagsDisableReset | kMesFlagsUseDiffVmidCompute |
                              kMesFlagsDisableMesLog | kMesFlagsLevelProcessQuantum |
                              kMesFlagsRegActivePoll),
              "set-hw flags=0x%x", sh.hw_flags);
        CHECK(sh.os_tdr == 0u, "os tdr=%u", sh.os_tdr);
        CHECK(sh.gc_base1 == kGc1, "gc_base[1]=0x%x", sh.gc_base1);

        const ApiOp &cq = d.apis[2];
        CHECK(cq.doorbell_offset == kMesDoorbellCompute, "compute db=%u",
              cq.doorbell_offset);
        CHECK(cq.mqd_addr == grant_mc(kMesCmpMqdOff), "compute mqd");
        CHECK(cq.wptr_addr == grant_mc(kMesCmpWbOff) + kMesWbWptrOff, "compute wptr");
        CHECK((cq.flags & kMesAddQueueFlagMapLegacyKq) != 0u &&
                  (cq.flags & (1u << 11)) == 0u && (cq.flags & (1u << 13)) != 0u,
              "compute map_legacy_kq bit13, flags=0x%x", cq.flags);
        CHECK(cq.pipe_id == 0u && cq.queue_id == 0u, "compute pipe/queue");

        const ApiOp &gq = d.apis[3];
        CHECK(gq.doorbell_offset == kMesDoorbellGfx, "gfx db=0x%x", gq.doorbell_offset);
        CHECK(gq.mqd_addr == grant_mc(kMesGfxMqdOff), "gfx mqd");
        CHECK(gq.wptr_addr == grant_mc(kMesGfxWbOff) + kMesWbWptrOff, "gfx wptr");
        CHECK((gq.flags & (1u << 13)) != 0u && (gq.flags & (1u << 11)) == 0u,
              "gfx map_legacy_kq bit13, flags=0x%x", gq.flags);
    }

    CHECK(has_db(d, 0x16u, 128u), "first API kick wptr not 128");
    {
        uint32_t eop = 0;
        memcpy(&eop, &d.bar0[static_cast<size_t>(grant_bar0(kMesCmpMqdOff) + 167u * 4u)],
               4u);
        CHECK(eop == 0x00000008u, "MQD dword167 EOP_SIZE=8 (got 0x%08x)", eop);
    }

    /* R1-R26: MQD/HQD address fields must be MC, not BAR0 offset. */
    check_mqd_mc(d, "KIQ", kMesKiqMqdOff, kMesKiqRingOff, kMesKiqWbOff, kMesKiqEopOff, true, false);
    check_mqd_mc(d, "SCH", kMesSchMqdOff, kMesSchRingOff, kMesSchWbOff, kMesSchEopOff, true, false);
    check_mqd_mc(d, "CMP", kMesCmpMqdOff, kMesCmpRingOff, kMesCmpWbOff, kMesCmpEopOff, true, false);
    check_mqd_mc(d, "GFX", kMesGfxMqdOff, kMesGfxRingOff, kMesGfxWbOff, kMesGfxEopOff, false, true);
    check_hqd_mc(d);
    CHECK(has_db(d, 0x16u, 256u), "second API kick wptr not 256");
    CHECK(has_db(d, 0x16u, 384u), "third API kick wptr not 384");
    CHECK(has_db(d, 0x16u, 512u), "fourth API kick wptr not 512");

    /* Value-level assertions for S-1 hub register final values (方案 v3 §6.2.3).
     * build_fixture pre-sets RMW targets with reset defaults; check the RMW result. */
    {
        const uint32_t gc0 = kGc0;
        CHECK(d.bar5[gc0 + 0x1688u] == 0x01fffe01u,
              "CONTEXT0=0x%08x want 0x01fffe01", d.bar5[gc0 + 0x1688u]);
        CHECK(d.bar5[gc0 + 0x167fu] == 0x1859u,
              "MX_L1=0x%08x want 0x1859", d.bar5[gc0 + 0x167fu]);
        CHECK(d.bar5[gc0 + 0x15bcu] == 0x00080e01u,
              "L2_CNTL=0x%08x want 0x00080e01", d.bar5[gc0 + 0x15bcu]);
        CHECK(d.bar5[gc0 + 0x15c4u] == 0x1ffcu,
              "FAULT_CNTL=0x%08x want 0x1ffc (bits30-31 cleared)", d.bar5[gc0 + 0x15c4u]);
        CHECK(d.bar5[gc0 + 0x15d4u] == 0x1u,
              "L2_CNTL4=0x%08x want 0x1", d.bar5[gc0 + 0x15d4u]);
        CHECK(d.bar5[gc0 + 0x15beu] == 0x80130009u,
              "L2_CNTL3=0x%08x want 0x80130009", d.bar5[gc0 + 0x15beu]);
        CHECK(d.bar5[gc0 + 0x15dau] == 0x3fe0u,
              "L2_CNTL5=0x%08x want 0x3fe0", d.bar5[gc0 + 0x15dau]);
        CHECK(d.bar5[gc0 + 0x167bu] == 0xffffffffu,
              "AGP_BOT=0x%08x want 0xffffffff", d.bar5[gc0 + 0x167bu]);
        CHECK(d.bar5[gc0 + 0x15a7u] == 0x6a0cu,
              "GC_FB_OFFSET(C)=0x%08x want 0x6a0c", d.bar5[gc0 + 0x15a7u]);
        CHECK(d.bar5[0xd20u + 0xC0u] == 1u,
              "NBIF_doorbell_aper=0x%08x want 1", d.bar5[0xd20u + 0xC0u]);
        CHECK(d.bar5[gc0 + 0x15c5u] == 0x000e0000u,
              "FAULT_CNTL2=0x%08x want 0xe0000", d.bar5[gc0 + 0x15c5u]);
        CHECK(d.bar5[gc0 + 0x1678u] == 0x8000u,
              "FB_BASE=0x%08x want 0x8000", d.bar5[gc0 + 0x1678u]);
        CHECK(d.bar5[gc0 + 0x1679u] == 0x80ffu,
              "FB_TOP=0x%08x want 0x80ff", d.bar5[gc0 + 0x1679u]);
    }

    bool cmp_read = false;
    for (const RegRead &r : d.reads) {
        if (r.dword == kGc0 + kRegCpHqdActive && r.me == 1u)
            cmp_read = true;
    }
    CHECK(cmp_read, "S9 did not read the compute HQD state");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-gfx") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-gfx"), "ok") == 0,
          "gfx property");
}

static void test_happy_path()
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    RunResult rr = run(f);
    check_common_success(f, d, rr);
}

static void test_start_readback_recorded()
{
    // C5.2 / tech plan §3.5: the S1 CP_MES_CNTL readback is recorded, never gated on the
    // PIPE ACTIVE bits (Linux mes_v11_0.c:957-1005 and nullarc mesboot.c:147-161 only
    // wait/print). §8.1: only a read fault fails closed.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.readback_ok = false;   /* stub returns CP_MES_CNTL without the ACTIVE bits */
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(rr.ready, "ready bit not set");
    CHECK(d.apis.size() == 4u, "api count=%zu", d.apis.size());
    CHECK(rr.r.reading_count == 4u, "readings=%u", rr.r.reading_count);
    if (rr.r.reading_count == 4u) {
        CHECK(strcmp(rr.r.readings[0].what, "CP_MES_CNTL") == 0, "what=%s",
              rr.r.readings[0].what);
        CHECK(rr.r.readings[0].value == 0u, "cntl readback recorded as 0x%x (not gated)",
              rr.r.readings[0].value);
    }
    CHECK(prop_get(f, "N48R3EVID-mes-done") &&
              strcmp(prop_get(f, "N48R3EVID-mes-done"), "Yes") == 0, "done=Yes");
}

static void test_start_readback_fault()
{
    // §8.1: a read path fault on the S1 CP_MES_CNTL readback still fails closed.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.cntl_readback_fail = true;   /* stub fails the post-active readback read */
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "expected failure");
    CHECK(strcmp(rr.r.name, "seq:start") == 0, "name=%s", rr.r.name);
    CHECK(rr.r.section == kR3SecSeq && rr.r.step == 1u, "section/step %u/%u",
          rr.r.section, rr.r.step);
    CHECK(!rr.ready, "ready bit set on failure");
    CHECK(d.apis.empty(), "APIs submitted after failed start");
}

static void test_gp3_zero_passes()
{
    // C5.2 / tech plan §3.5: the S2 CP_MES_GP3_LO version is recorded, never gated on
    // being non-zero; 0 / all-ones are observations only (Linux only prints the value).
    const uint32_t versions[2] = {0u, 0xffffffffu};
    for (uint32_t v : versions) {
        Dev d;
        Fixture f;
        CHECK(build_fixture(f, d, true, true), "fixture");
        d.bar5[kGc1 + kRegMesGp3Lo] = v;
        RunResult rr = run(f);
        CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s (gp3=0x%08x)", rr.r.status,
              rr.r.name, v);
        CHECK(rr.ready, "ready bit not set (gp3=0x%08x)", v);
        CHECK(rr.r.reading_count == 4u, "readings=%u (gp3=0x%08x)", rr.r.reading_count, v);
        if (rr.r.reading_count == 4u) {
            CHECK(rr.r.readings[1].value == v, "gp3 sched reading=0x%x",
                  rr.r.readings[1].value);
            CHECK(rr.r.readings[2].value == v, "gp3 kiq reading=0x%x",
                  rr.r.readings[2].value);
        }
    }
}

static void test_gp3_read_fault()
{
    // §8.1: a read path fault on the S2 CP_MES_GP3_LO reads still fails closed.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.gp3_read_fail = true;
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "expected failure");
    CHECK(strcmp(rr.r.name, "chk:version") == 0, "name=%s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 2u, "section/step %u/%u",
          rr.r.section, rr.r.step);
    CHECK(!rr.ready, "ready bit set on failure");
    CHECK(d.apis.empty(), "APIs submitted after failed version read");
}

static void test_query_gate_unchanged()
{
    // C5.2: SET_HW_RESOURCES + QUERY remain the only gates. Dropping the S1/S2
    // observation gates must not weaken the real protocol gate (§8.1 uniqueness).
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_status_op = static_cast<int>(kMesApiQuerySchedulerStatus);
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(strcmp(rr.r.name, "chk:api-status") == 0, "name=%s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 6u, "section/step %u/%u",
          rr.r.section, rr.r.step);
    CHECK(!rr.ready, "ready bit set on QUERY failure");
    CHECK(d.apis.size() == 2u && d.apis[1].opcode == kMesApiQuerySchedulerStatus,
          "api count=%zu", d.apis.size());
}

static void test_set_hw_status_failure()
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_status_op = static_cast<int>(kMesApiSetHwRsrc);
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "expected failure");
    CHECK(strcmp(rr.r.name, "chk:api-status") == 0, "name=%s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 5u, "section/step %u/%u",
          rr.r.section, rr.r.step);
    CHECK(!rr.ready, "ready bit set on failure");
    CHECK(d.apis.size() == 1u && d.apis[0].opcode == kMesApiSetHwRsrc,
          "api count=%zu", d.apis.size());
    CHECK(prop_get(f, "N48R3EVID-mes-addq-compute") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-compute"), "n/a") == 0,
          "compute property n/a");
}

static void test_compute_addq_status_failure()
{
    // C5.2 v1.5 (F7=c'): a rejected COMPUTE ADD_QUEUE is recorded, never fatal and never
    // latched. S9 still exports, and the GFX attempt follows it.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_status_op = static_cast<int>(kMesApiAddQueue);
    d.stall_status_qtype = static_cast<int>(kMesQueueTypeCompute);
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(rr.ready, "ready bit not set");
    CHECK(d.gfx_queued, "GFX attempt must follow the rejected COMPUTE add-queue");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-compute") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-compute"), "fail:status") == 0,
          "compute property fail:status");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-gfx") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-gfx"), "ok") == 0,
          "gfx property ok");
}

static void test_both_addq_rejected()
{
    // Both ADD_QUEUE submissions fail at the fence; both are recorded and S9 still passes.
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_fence = true;
    d.stall_fence_op = static_cast<int>(kMesApiAddQueue);
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(prop_get(f, "N48R3EVID-mes-addq-compute") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-compute"), "fail:fence") == 0,
          "compute fail:fence");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-gfx") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-gfx"), "fail:fence") == 0,
          "gfx fail:fence");
}

static void test_gfx_attempt_fails_overall_passes()
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_status_op = static_cast<int>(kMesApiAddQueue);
    d.stall_status_qtype = static_cast<int>(kMesQueueTypeGfx);
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(rr.ready, "ready bit not set");
    CHECK(d.gfx_queued, "GFX attempt missing");
    CHECK(prop_get(f, "N48R3EVID-mes-addq-gfx") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-gfx"), "fail:status") == 0,
          "gfx property=%s", prop_get(f, "N48R3EVID-mes-addq-gfx"));
    CHECK(prop_get(f, "N48R3EVID-mes-addq-compute") &&
              strcmp(prop_get(f, "N48R3EVID-mes-addq-compute"), "ok") == 0,
          "compute property");
    CHECK(ev_has(f, "mes-addq-gfx"), "gfx evidence missing");
}

static void test_fence_timeout()
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.stall_fence = true;
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecFail, "expected failure");
    CHECK(strcmp(rr.r.name, "seq:fence-timeout") == 0, "name=%s", rr.r.name);
    CHECK(rr.r.section == kR3SecSeq && rr.r.step == 5u, "section/step %u/%u",
          rr.r.section, rr.r.step);
    CHECK(!rr.ready, "ready bit set on failure");
}

static void test_set_hw_1_op19()
{
    Dev d;
    Fixture f;
    CHECK(build_fixture(f, d, true, true), "fixture");
    d.bar5[kGc1 + kRegMesGp3Lo] = 0x52u; /* MES version = 0x52: opcode 19 is not skipped */
    RunResult rr = run(f);
    CHECK(rr.r.status == kR3SsecOk, "status=%u name=%s", rr.r.status, rr.r.name);
    CHECK(rr.ready, "ready bit not set");
    CHECK(d.apis.size() == 5u, "api count=%zu", d.apis.size());
    if (d.apis.size() == 5u)
        CHECK(d.apis[1].opcode == kMesApiSetHwRsrc1, "op1=%u", d.apis[1].opcode);
    char want[64];
    snprintf(want, sizeof(want), "op19=ok fence=0x%llx status=0x1",
             static_cast<unsigned long long>(d.fences[1].value));
    CHECK(prop_get(f, "N48R3EVID-mes-set-hw-1") &&
              strcmp(prop_get(f, "N48R3EVID-mes-set-hw-1"), want) == 0,
          "mes-set-hw-1=%s want=%s", prop_get(f, "N48R3EVID-mes-set-hw-1"), want);
}

static void test_pre_failures()
{
    {
        Dev d;
        Fixture f;
        CHECK(build_fixture(f, d, false, true), "fixture");
        RunResult rr = run(f);
        CHECK(strcmp(rr.r.name, "pre:psp-not-ready") == 0, "name=%s", rr.r.name);
        CHECK(d.writes.empty(), "wrote registers with PSP not ready");
    }
    {
        Dev d;
        Fixture f;
        CHECK(build_fixture(f, d, true, false), "fixture");
        RunResult rr = run(f);
        CHECK(strcmp(rr.r.name, "pre:gfx-not-ready") == 0, "name=%s", rr.r.name);
    }
    {
        Dev d;
        Fixture f;
        CHECK(build_fixture(f, d, true, true), "fixture");
        const R3Grant *g = r3_layout_grant(&f.rt.layout, "mes");
        CHECK(g != nullptr, "mes grant");
        for (uint32_t i = 0; i < f.rt.layout.grant_count; i++) {
            if (&f.rt.layout.grants[i] == g) {
                f.rt.layout.grants[i].tag[0] = 'x';
                f.rt.layout.grants[i].tag[1] = '\0';
            }
        }
        RunResult rr = run(f);
        CHECK(strcmp(rr.r.name, "pre:mes-grant") == 0, "name=%s", rr.r.name);
    }
}

} /* namespace */

int main(void)
{
    test_happy_path();
    test_start_readback_recorded();
    test_start_readback_fault();
    test_gp3_zero_passes();
    test_gp3_read_fault();
    test_set_hw_status_failure();
    test_query_gate_unchanged();
    test_compute_addq_status_failure();
    test_both_addq_rejected();
    test_gfx_attempt_fails_overall_passes();
    test_fence_timeout();
    test_set_hw_1_op19();
    test_pre_failures();

    printf("test_mes_api: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
