/*
 * test_psp_msg.cpp - host tests for the PSP 13.0.4 substep (K4 / T13 + the sequence state
 * machine, tech plan §10).
 *
 * Everything here runs on a RAM-backed fake of the two apertures (BAR5 dwords + the BAR0
 * carveout window) and the 17 real embedded blobs, so the whole sequence - including the
 * SOS-alive refusal, every failure branch and the fence timeout - executes on Linux with no
 * GPU. Golden values come from independent sources, never from the code under test (H4):
 *
 *   T13  frame/command layout: the offsets are re-derived here from the upstream C
 *        declarations with an independent field walker (a flat 4/1-byte field table), and
 *        the expected bytes are written out by hand.
 *   Order: r3_psp_load_order (D2) is checked against a hand-written list of the 17 fw_types
 *        in Linux enumeration order, plus the R1 §3.6 payload sizes taken from the archived
 *        firmware headers (an independent `od` transcription, not the parser).
 *   Exclusion: the fw_types that must never reach the wire (MEC JT 5/6, RLC 22/7, RS64
 *        87/88/89) are asserted against the observed command list.
 *
 * Planted breaks (H3: the failure is recorded before the fix) are listed in the header of
 * run_psp_planted_breaks.py; each mutation MUST make this suite fail.
 */
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "amdgpu_ip.h"
#include "fw_table.h"
#include "amd/psp_v13_0.h"
#include "n48log_stub.h"
#include "rdna3/golden_values.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_substep.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/rdna3_wmanifest.h"
static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// =========================================================================================
// The fake device
// =========================================================================================
static const uint32_t kBar5Dwords = 0x100000u;    // 4 MiB of BAR5
static const uint64_t kBar0Bytes  = 0x20000000ull; // 512 MiB carveout view
static const uint32_t kMp0Base    = 0x40000u;      // stand-in discovery base (MP0)
// Stand-in HDP memory-flush window (a BAR5 dword inside the fake map); production discovers
// it from BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL and registers the same row.
static const uint32_t kHdpDword   = 0x80000u;
static const uint32_t kGcBase1    = 0x30000u;      // GC BASE_IDX 1 (RLC_F32_UCODE_VERSION)
static const uint32_t kSdma0Base0 = 0x20000u;      // SDMA0 BASE_IDX 0 (SDMA0_VERSION)

struct CmdRecord {
    uint32_t id;
    uint32_t body_dwords[16];   // the psp_gfx_* command structure (offsets from the T13 table)
};

struct Dev {
    std::vector<uint32_t> bar5;
    std::vector<uint8_t>  bar0;
    std::vector<uint32_t> reads;        // BAR5 dwords read, in order
    std::vector<uint32_t> writes;       // BAR5 dwords written, in order
    std::vector<uint64_t> bar0_writes;  // BAR0 byte offsets written, in order
    std::vector<CmdRecord> cmds;        // command buffers the hook saw, in order
    std::vector<uint32_t> load_sizes;   // LOAD_IP_FW fw_size, in wire order
    uint32_t tmr_body[6] = {};          // SETUP_TMR body dwords (addr_lo/hi, size, flags, pa lo/hi)
    std::vector<uint32_t> load_order;   // fw_types of the LOAD_IP_FW commands, in order
    uint64_t now;
    uint32_t c2p33;            // before the ring command
    uint32_t c2p81;
    uint32_t c2p64_ready;      // C2PMSG_64 before the ring command
    uint32_t c2p64_resp;       // C2PMSG_64 after it
    uint32_t c2p58;
    uint32_t tmr_size;
    uint32_t fail_cmd_id;      // command id that answers nonzero status (0 = none)
    uint32_t fail_status;
    uint32_t fail_fw_type;     // LOAD_IP_FW piece that answers nonzero status (0 = none)
    bool     fence_ok;
    uint32_t fence_delay_us;
    bool     c2p33_ready_after;
    bool     cmd_buf_fields_nonzero;
    uint32_t fence_calls;
    uint32_t doorbells;
    bool     doorbell_without_fence;
    bool     wptr_reset_seen;   // C2PMSG_67 was reset to 0 after ring create (nullarc form)

    Dev()
    {
        bar5.assign(kBar5Dwords, 0);
        // 0xA5 everywhere: a zero-fill that never happened leaves 0xA5 behind, so the S2
        // assertions cannot pass on a fresh zeroed buffer by accident.
        bar0.assign(kBar0Bytes, 0xA5);
        bar5[kMp0Base + r3gold::kMp0SmnC2pmsg33.offset] = 0x80000000u;
        bar5[kMp0Base + r3gold::kMp0SmnC2pmsg81.offset] = 0x3a000000u;
        bar5[kMp0Base + r3gold::kMp0SmnC2pmsg64.offset] = 0x80000000u;
        now = 1000;
        c2p33 = 0x80000000u;
        c2p81 = 0x3a000000u;
        c2p64_ready = 0x80000000u;
        c2p64_resp = 0x80000000u;
        c2p58 = 0u;              // stage-1 baseline
        tmr_size = 0x4000000u;   // nullarc's measured value, used as the input here
        fail_cmd_id = 0;
        fail_status = 0;
        fail_fw_type = 0;
        fence_ok = true;
        fence_delay_us = 800;
        c2p33_ready_after = true;
        cmd_buf_fields_nonzero = false;
        fence_calls = 0;
        doorbells = 0;
        doorbell_without_fence = false;
        wptr_reset_seen = false;
    }

    uint32_t mp0(uint32_t reg) const { return kMp0Base + reg; }
};

static Dev *g_dev;

static R3LayoutPlan g_plan;   // the plan under test; io_mmio needs it to find ring/fence

template <typename T>
static void bar0_get(const Dev &d, uint64_t off, T *out) { memcpy(out, &d.bar0[off], sizeof(T)); }

template <typename T>
static void bar0_put(Dev &d, uint64_t off, T v) { memcpy(&d.bar0[off], &v, sizeof(T)); }

static bool io_mmio(void *, uint32_t dword, uint32_t *value, bool write)
{
    if (dword >= kBar5Dwords)
        return false;
    if (write) {
        g_dev->writes.push_back(dword);
        g_dev->bar5[dword] = *value;
        if (dword == g_dev->mp0(r3gold::kMp0SmnC2pmsg67.offset)) {
            // The ring-create WPTR reset (nullarc: wreg(C2PMSG_67, 0)) is not a doorbell; a
            // later genuine wrap to 0 is still a doorbell because doorbells > 0.
            const bool wptr_reset = (*value == 0u && g_dev->doorbells == 0u);
            if (wptr_reset)
                g_dev->wptr_reset_seen = true;
            if (!wptr_reset) {
            // B-01: the platform barrier must have run before every doorbell.
            if (g_dev->fence_calls <= g_dev->doorbells)
                g_dev->doorbell_without_fence = true;
            g_dev->doorbells++;
            // Doorbell: the PSP consumes the frame at the pointer it is being handed and
            // writes the fence buffer after its response latency (fence_ok=false: never).
            const R3Grant *ring = r3_layout_grant(&g_plan, "psp_ring");
            const R3Grant *fence = r3_layout_grant(&g_plan, "psp_fence");
            const uint32_t slot_dw = (*value + 1024u - amdgpu::kPspFrameDwords) % 1024u;
            if (ring && fence && g_dev->fence_ok) {
                uint32_t fv = 0;
                bar0_get(*g_dev, ring->start + slot_dw * 4u + amdgpu::kRbFrameFenceVal, &fv);
                g_dev->now += g_dev->fence_delay_us;
                bar0_put(*g_dev, fence->start, fv);
            }
            }
        }
        if (dword == g_dev->mp0(r3gold::kMp0SmnC2pmsg64.offset) &&
            (*value & 0xFFFF0000u) == 0x20000u) {
            // ring create: the response appears, and the firmware "runs" - IFWI/PSP ready
            // and the version registers come alive.
            g_dev->bar5[dword] = g_dev->c2p64_resp;
            g_dev->bar5[g_dev->mp0(r3gold::kMp0SmnC2pmsg33.offset)] =
                g_dev->c2p33_ready_after ? g_dev->c2p33 : 0u;
            g_dev->bar5[g_dev->mp0(r3gold::kMp0SmnC2pmsg58.offset)] = g_dev->c2p58;
        }
        return true;
    }
    g_dev->reads.push_back(dword);
    *value = g_dev->bar5[dword];
    return true;
}

// Test double for the kext's contract-⑦ callback: classify the shared HDP row, write 0,
// read back. The production implementation lives in NaviR3Bringup::ioHdpFlush.
static bool io_hdp(void *ctx) {
    const uint32_t d = kHdpDword;
    if (r3_wg_classify(d, 1u, WClass::Protocol, "hdp-flush") != WVerdict::TxnRead)
        return false;
    uint32_t zero = 0;
    if (!io_mmio(ctx, d, &zero, true)) return false;
    uint32_t rb = 0;
    (void)io_mmio(ctx, d, &rb, false);
    return true;
}

static bool io_bus(void *, uint32_t bar, uint64_t off, uint64_t *value, bool write,
                   uint32_t dwords)
{
    if (bar != R3_IO_BAR_VRAM || (off & 7ull) != 0u || dwords == 0u || dwords > 2u ||
        off + dwords * 4ull > kBar0Bytes) {
        return false;
    }
    if (!write) {
        uint64_t v = 0;
        memcpy(&v, &g_dev->bar0[off], dwords * 4u);
        *value = v;
        return true;
    }
    memcpy(&g_dev->bar0[off], value, dwords * 4u);
    g_dev->bar0_writes.push_back(off);
    return true;
}

static void io_udelay(void *, uint32_t us) { g_dev->now += us; }
static void io_mdelay(void *, uint32_t ms) { g_dev->now += (uint64_t)ms * 1000u; }
static uint64_t io_now_us(void *) { return g_dev->now; }
static void io_fence(void *ctx) { static_cast<Dev *>(ctx)->fence_calls++; }
static void io_lock(void *) {}
static void io_unlock(void *) {}



// =========================================================================================
// fixture
// =========================================================================================
struct PropSink {
    char key[64];
    char value[32];
    int calls;
};

struct Fixture {
    Fixture() { memset(this, 0, sizeof *this); }
    R3Rt rt;
    R3EvidenceLines ev;
    R3SubstepEnv env;
    R3Io io;
    amdgpu::IPBaseTable ip;
    R3EvSink sink;
    PropSink props[64];
    int prop_count;
};

// This machine's stage-0 readings (report: FB MC base 0x8000000000, phys 0x7a0000000).
static const uint64_t kFbMcBase = 0x8000000000ull;
static const uint64_t kFbPhysBase = 0x7a0000000ull;
static const uint64_t kBar0Base = 0x8d0000000ull;

// The evidence publisher: records what the substep exports through N48R3EVID-* properties.
static bool prop_set(void *user, const char *key, const char *bytes, size_t len)
{
    Fixture *f = static_cast<Fixture *>(user);
    if (!f || f->prop_count >= 64 || !key || !bytes)
        return false;
    PropSink &p = f->props[f->prop_count++];
    size_t kn = strlen(key) < sizeof p.key - 1 ? strlen(key) : sizeof p.key - 1;
    memcpy(p.key, key, kn);
    p.key[kn] = '\0';
    size_t vn = len < sizeof p.value - 1 ? len : sizeof p.value - 1;
    memcpy(p.value, bytes, vn);
    p.value[vn] = '\0';
    p.calls++;
    return true;
}

static bool build_fixture(Fixture &f, Dev &d, uint64_t tmr_size)
{
    R3LayoutInputs in = {};
    in.fb_mc_base = kFbMcBase;
    in.fb_phys_base = kFbPhysBase;
    in.bar0_base = kBar0Base;
    in.real_vram_size = 4096ull << 20;
    in.vbios_reserve = 1ull << 20;
    in.bar0_size = 256ull << 20;
    in.gop_end = 20ull << 20;
    in.gop_end_known = true;
    in.tmr_size = tmr_size;
    memset(&f.rt, 0, sizeof f.rt);
    if (r3_layout_compute(&in, &f.rt.layout) != 0)
        return false;
    n48log_host_reset();
    r3_ev_init(&f.ev);
    memset(&f.env, 0, sizeof f.env);
    f.env.rt = &f.rt;
    f.env.ev = &f.ev;
    f.env.fb_mc_base = kFbMcBase;
    f.env.hdp_flush_dword = kHdpDword;
    f.env.hdp_flush_valid = true;
    f.env.strict_expect = true;
    memset(&f.ip, 0, sizeof f.ip);
    f.ip.setBase(amdgpu::IPBlock::MP0, 1, kMp0Base);
    f.ip.setBase(amdgpu::IPBlock::GC, 1, kGcBase1);
    f.ip.set(amdgpu::IPBlock::SDMA0, kSdma0Base0);
    f.env.ip = &f.ip;
    f.env.sink = &f.sink;
    f.sink.set_property = prop_set;
    f.sink.user = &f;
    f.prop_count = 0;
    memset(&f.io, 0, sizeof f.io);
    f.io.ctx = &d;
    f.io.mmio = io_mmio;
    f.io.smuio = [](void *, uint32_t, uint32_t *, bool) { return false; };
    f.io.bus = io_bus;
    f.io.udelay = io_udelay;
    f.io.mdelay = io_mdelay;
    f.io.now_us = io_now_us;
    f.io.lock = io_lock;
    f.io.fence = io_fence;
    f.io.hdp_flush = io_hdp;
    f.io.unlock = io_unlock;
    return true;
}

// =========================================================================================
// golden: T13 offsets, re-derived from the upstream declarations
// =========================================================================================
// An independent field walker: (name, size, count) rows, walked with natural alignment.
struct Field { const char *name; uint32_t size; uint32_t count; };

static uint32_t walk(const Field *fields, uint32_t n, const char *want, uint32_t *size_out)
{
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t align = fields[i].size > 4u ? 4u : fields[i].size;
        off = (off + align - 1u) & ~(align - 1u);
        if (strcmp(fields[i].name, want) == 0) {
            if (size_out)
                *size_out = fields[i].size * fields[i].count;
            return off;
        }
        off += fields[i].size * fields[i].count;
    }
    return 0xFFFFFFFFu;
}

// psp_gfx_rb_frame (psp_gfx_if.h:497-512)
static const Field kRbFrame[] = {
    {"cmd_buf_addr_lo", 4, 1}, {"cmd_buf_addr_hi", 4, 1}, {"cmd_buf_size", 4, 1},
    {"fence_addr_lo", 4, 1},   {"fence_addr_hi", 4, 1},   {"fence_value", 4, 1},
    {"sid_lo", 4, 1},          {"sid_hi", 4, 1},          {"vmid", 1, 1},
    {"frame_type", 1, 1},      {"reserved1", 1, 2},       {"reserved2", 4, 7},
};

// psp_gfx_cmd_resp (psp_gfx_if.h:466-487) up to the command union; the union itself is
// checked field by field against each command structure below.
static const Field kCmdRespHead[] = {
    {"buf_size", 4, 1}, {"buf_version", 4, 1}, {"cmd_id", 4, 1},
    {"resp_buf_addr_lo", 4, 1}, {"resp_buf_addr_hi", 4, 1},
    {"resp_offset", 4, 1}, {"resp_buf_size", 4, 1}, {"cmd", 4, 1},
};

// psp_gfx_cmd_setup_tmr (psp_gfx_if.h:191-205)
static const Field kSetupTmr[] = {
    {"buf_phy_addr_lo", 4, 1}, {"buf_phy_addr_hi", 4, 1}, {"buf_size", 4, 1},
    {"tmr_flags", 4, 1}, {"system_phy_addr_lo", 4, 1}, {"system_phy_addr_hi", 4, 1},
};

// psp_gfx_cmd_load_ip_fw (psp_gfx_if.h:326-333)
static const Field kLoadIpFw[] = {
    {"fw_phy_addr_lo", 4, 1}, {"fw_phy_addr_hi", 4, 1}, {"fw_size", 4, 1}, {"fw_type", 4, 1},
};

// psp_gfx_cmd_load_toc (psp_gfx_if.h:341-346)
static const Field kLoadToc[] = {
    {"toc_phy_addr_lo", 4, 1}, {"toc_phy_addr_hi", 4, 1}, {"toc_size", 4, 1},
};

// psp_gfx_resp (psp_gfx_if.h:441-461)
static const Field kResp[] = {
    {"status", 4, 1}, {"session_id", 4, 1}, {"fw_addr_lo", 4, 1}, {"fw_addr_hi", 4, 1},
    // uresp: union psp_gfx_uresp, largest member psp_gfx_uresp_reserved[8] = 8 dwords.
    {"tmr_size", 4, 1}, {"reserved", 4, 11}, {"uresp", 4, 8},
};

static uint32_t rb_off(const char *f) { return walk(kRbFrame, 12, f, nullptr); }
static uint32_t cmd_off(const char *f) { return walk(kCmdRespHead, 8, f, nullptr); }
static uint32_t resp_off(const char *f) { return walk(kResp, 7, f, nullptr); }
static uint32_t tmr_off(const char *f) { return walk(kSetupTmr, 6, f, nullptr); }
static uint32_t ipfw_off(const char *f) { return walk(kLoadIpFw, 4, f, nullptr); }
static uint32_t toc_off(const char *f) { return walk(kLoadToc, 3, f, nullptr); }

static void test_t13_offsets()
{
    // The struct sizes first: a walker that disagrees with the header comment sizes is wrong.
    uint32_t sz = 0;
    (void)walk(kRbFrame, 12, "reserved2", &sz);
    CHECK(walk(kRbFrame, 12, "reserved2", &sz) + sz == 64u, "rb_frame total %u != 64",
          walk(kRbFrame, 12, "reserved2", &sz) + sz);
    const uint32_t resp_total = walk(kResp, 7, "uresp", &sz) + sz;
    CHECK(resp_total == 96u, "psp_gfx_resp total %u != 96", resp_total);
    const uint32_t cmd_base = walk(kCmdRespHead, 8, "cmd", &sz);
    CHECK(cmd_base == 28u, "cmd union offset %u != 28", cmd_base);
    (void)cmd_base;

    // ... and the values the implementation must use (psp_v13_0.h constants).
    CHECK(amdgpu::kRbFrameCmdLo == rb_off("cmd_buf_addr_lo"), "kRbFrameCmdLo");
    CHECK(amdgpu::kRbFrameCmdHi == rb_off("cmd_buf_addr_hi"), "kRbFrameCmdHi");
    CHECK(amdgpu::kRbFrameCmdSize == rb_off("cmd_buf_size"), "kRbFrameCmdSize");
    CHECK(amdgpu::kRbFrameFenceLo == rb_off("fence_addr_lo"), "kRbFrameFenceLo");
    CHECK(amdgpu::kRbFrameFenceHi == rb_off("fence_addr_hi"), "kRbFrameFenceHi");
    CHECK(amdgpu::kRbFrameFenceVal == rb_off("fence_value"), "kRbFrameFenceVal");
    CHECK(amdgpu::kPspFrameBytes == 64u && amdgpu::kPspFrameDwords == 16u, "frame is 64 B/16 dw");

    CHECK(amdgpu::kCmdRespBufSize == cmd_off("buf_size"), "CmdRespBufSize");
    CHECK(amdgpu::kCmdRespBufVersion == cmd_off("buf_version"), "CmdRespBufVersion");
    CHECK(amdgpu::kCmdRespCmdId == cmd_off("cmd_id"), "CmdRespCmdId");
    CHECK(amdgpu::kCmdRespCmd == cmd_off("cmd"), "CmdRespCmd");
    // psp_gfx_if.h:481 reserves 864 - sizeof(union) - 28 bytes; 864 is the response offset.
    CHECK(amdgpu::kCmdRespResp == 864u, "resp offset");
    CHECK(amdgpu::kCmdRespTmrSize == resp_off("tmr_size"), "CmdRespTmrSize");
    CHECK(amdgpu::kCmdRespTmrSize == 16u, "resp.tmr_size is +16");

    CHECK(amdgpu::kSetupTmrAddrLo == tmr_off("buf_phy_addr_lo"), "SetupTmrAddrLo");
    CHECK(amdgpu::kSetupTmrAddrHi == tmr_off("buf_phy_addr_hi"), "SetupTmrAddrHi");
    CHECK(amdgpu::kSetupTmrSize == tmr_off("buf_size"), "SetupTmrSize");
    CHECK(amdgpu::kSetupTmrFlags == tmr_off("tmr_flags"), "SetupTmrFlags");
    CHECK(amdgpu::kSetupTmrSysPhyLo == tmr_off("system_phy_addr_lo"), "SetupTmrSysPhyLo");
    CHECK(amdgpu::kSetupTmrSysPhyHi == tmr_off("system_phy_addr_hi"), "SetupTmrSysPhyHi");

    CHECK(amdgpu::kLoadIpFwAddrLo == ipfw_off("fw_phy_addr_lo"), "LoadIpFwAddrLo");
    CHECK(amdgpu::kLoadIpFwAddrHi == ipfw_off("fw_phy_addr_hi"), "LoadIpFwAddrHi");
    CHECK(amdgpu::kLoadIpFwSize == ipfw_off("fw_size"), "LoadIpFwSize");
    CHECK(amdgpu::kLoadIpFwType == ipfw_off("fw_type"), "LoadIpFwType");

    CHECK(amdgpu::kLoadTocAddrLo == toc_off("toc_phy_addr_lo"), "LoadTocAddrLo");
    CHECK(amdgpu::kLoadTocAddrHi == toc_off("toc_phy_addr_hi"), "LoadTocAddrHi");
    CHECK(amdgpu::kLoadTocSize == toc_off("toc_size"), "LoadTocSize");

    // command ids and mailbox masks (psp_gfx_if.h:88-111, amdgpu_psp.h:57-65)
    CHECK(amdgpu::kGfxCmdSetupTmr == 0x5u, "SETUP_TMR id");
    CHECK(amdgpu::kGfxCmdLoadIpFw == 0x6u, "LOAD_IP_FW id");
    CHECK(amdgpu::kGfxCmdLoadToc == 0x20u, "LOAD_TOC id");
    CHECK(amdgpu::kGfxCmdAutoloadRlc == 0x21u, "AUTOLOAD_RLC id");
    CHECK(amdgpu::kFwTypeRlcG == 8u, "GFX_FW_TYPE_RLC_G");
    CHECK(amdgpu::kPspMboxRespFlag == 0x80000000u && amdgpu::kPspMboxStatusMask == 0xFFFFu,
          "MBOX_TOS_RESP flag + GFX_CMD_STATUS_MASK");
    CHECK(amdgpu::kPspMboxReadyMask == 0x8000FFFFu,
          "MBOX_TOS_READY_MASK (amdgpu_psp.h:58: RESPONSE|STATUS)");
    CHECK(amdgpu::kPspC2pmsg33ReadyBit == 0x80000000u, "C2PMSG_33 bit31");
    CHECK(amdgpu::kSetupTmrFlagVirtPhy == 0x2u, "virt_phy_addr bitfield = bit 1");
    CHECK(amdgpu::kPspC2pmsg64RingInitKm == 0x20000u, "PSP_RING_TYPE__KM << 16");
    CHECK(amdgpu::kPspRingBytes == 0x1000u, "ring size on the wire");
    // The MP0 mailbox offsets now live only in the golden table (A-03); the numbers are the
    // independent mp_13_0_4_offset.h values.
    CHECK(r3gold::kMp0SmnC2pmsg33.offset == 0x61u &&
              r3gold::kMp0SmnC2pmsg58.offset == 0x7au &&
              r3gold::kMp0SmnC2pmsg64.offset == 0x80u &&
              r3gold::kMp0SmnC2pmsg67.offset == 0x83u &&
              r3gold::kMp0SmnC2pmsg69.offset == 0x85u &&
              r3gold::kMp0SmnC2pmsg70.offset == 0x86u &&
              r3gold::kMp0SmnC2pmsg71.offset == 0x87u &&
              r3gold::kMp0SmnC2pmsg81.offset == 0x91u,
          "C2PMSG golden offsets (mp_13_0_4_offset.h:31-171)");
    CHECK(r3gold::kMp0SmnC2pmsg33.base_idx == 1, "MP0 mailbox golden group 1 (0x16000)");
}

// =========================================================================================
// golden: the 17-piece table (R1 §3.6 sizes, hand transcribed from `od`)
// =========================================================================================
struct GoldenPiece {
    uint32_t fw_type;
    uint32_t size;       // payload length from the archived header (R1 §3.6 table)
};

static const GoldenPiece kGolden[17] = {
    {71, 17408}, {72, 16896},  // sdma_6_0_1  TH0 / TH1
    {2, 263168}, {1, 263168}, {4, 267008},   // pfp / me / mec (MEC excludes JT 224*4)
    {33, 135648}, {34, 131072},              // mes_2 code / stack
    {81, 104272}, {82, 131072},              // mes1 code / stack
    {68, 66048}, {69, 66048},                // imu_i / imu_d
    {20, 2560}, {21, 21104}, {26, 66048}, {48, 33280}, {25, 8704},  // rlc sub-blobs
    {8, 25088},                              // rlc_g, last
};

static void test_load_table_golden()
{
    CHECK(R3_PSP_LOAD_COUNT == 17u, "17 entries");
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; i++) {
        CHECK(r3_psp_load_order[i].fw_type == kGolden[i].fw_type,
              "entry %u fw_type %u != golden %u", i, r3_psp_load_order[i].fw_type,
              kGolden[i].fw_type);
    }
    CHECK(r3_psp_load_order[16].fw_type == 8u, "RLC_G is last (D2)");
    // excluded by construction
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; i++) {
        const uint32_t t = r3_psp_load_order[i].fw_type;
        CHECK(t != 5u && t != 6u, "MEC JT %u must not be loaded (autoload skips it)", t);
        CHECK(t != 7u && t != 22u, "RLC %u has size 0 in the archived header", t);
        CHECK(t < 87u || t > 89u, "RS64 %u must never be loaded", t);
    }
}

// Stage plan: every piece's staged size equals its golden header size, every slot is
// 4 KiB aligned, and the pieces provably cannot overlap.
static void test_stage_plan_golden()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture layout");
    (void)f;
    const R3Grant *fw = r3_layout_grant(&f.rt.layout, "fw_buf");
    CHECK(fw != nullptr, "fw_buf grant");
    if (!fw)
        return;
    const uint64_t fw_mc = kFbMcBase + fw->start;
    amdgpu::PspStagePlan sp = amdgpu::psp_stage_plan(&f.rt.layout, kFbMcBase);
    CHECK(sp.ok, "stage plan ok (fail=%s)", sp.fail ? sp.fail : "-");
    CHECK(sp.fw_buf_mc == fw_mc, "stage plan MC base");
    CHECK(sp.fw_buf_bar0 == fw->start, "stage plan BAR0 base");
    CHECK(sp.total_bytes <= fw->size, "staged %llu <= grant %llu",
          (unsigned long long)sp.total_bytes, (unsigned long long)fw->size);
    CHECK(sp.fw_count == 17u, "17 rows, got %u", sp.fw_count);
    for (uint32_t i = 0; i < sp.fw_count && i < 17u; i++) {
        CHECK(sp.fw[i].fw_type == kGolden[i].fw_type, "row %u fw_type", i);
        CHECK(sp.fw[i].size == kGolden[i].size, "row %u (%u) size %u != golden %u", i,
              sp.fw[i].fw_type, sp.fw[i].size, kGolden[i].size);
        CHECK((sp.fw[i].mc & (R3_FW_STAGE_ALIGN - 1u)) == 0u,
              "row %u MC 0x%llx not 4 KiB aligned", i, (unsigned long long)sp.fw[i].mc);
        CHECK(sp.fw[i].mc >= fw_mc && sp.fw[i].mc + sp.fw[i].size <= fw_mc + fw->size,
              "row %u outside the fw_buf grant", i);
        CHECK((sp.fw[i].bar0 & (R3_FW_STAGE_ALIGN - 1u)) == 0u, "row %u BAR0 aligned", i);
    }
    // TOC: 2304 B payload at fw_buf offset 0 (tech plan §3.1 S4)
    CHECK(sp.toc.size == 2304u, "TOC payload %u != 2304", sp.toc.size);
    CHECK(sp.toc.mc == fw_mc, "TOC must be staged at fw_buf offset 0 (0x%llx != 0x%llx)",
          (unsigned long long)sp.toc.mc, (unsigned long long)fw_mc);
    CHECK(sp.toc.bar0 == 0u, "TOC BAR0 offset must be 0");

    // Failure paths of the same pure function: each has its own static reason.
    {
        R3LayoutPlan bad = f.rt.layout;
        bad.ok = false;
        bad.fail_reason = "test";
        amdgpu::PspStagePlan sp2 = amdgpu::psp_stage_plan(&bad, kFbMcBase);
        CHECK(!sp2.ok && strcmp(sp2.fail, "stage:grant") == 0, "invalid plan: %s", sp2.fail);
    }
    {
        R3LayoutPlan bad = f.rt.layout;
        bad.grant_count = 0;   // no fw_buf
        amdgpu::PspStagePlan sp2 = amdgpu::psp_stage_plan(&bad, kFbMcBase);
        CHECK(!sp2.ok && strcmp(sp2.fail, "stage:grant") == 0, "no grant: %s", sp2.fail);
    }
    {
        R3LayoutPlan tight = f.rt.layout;
        for (uint32_t i = 0; i < tight.grant_count; i++)
            if (strcmp(tight.grants[i].tag, "fw_buf") == 0) {
                tight.grants[i].size = 0x1000u;      // far too small for ~1.6 MiB
                tight.grants[i].end = tight.grants[i].start + 0x1000u;
            }
        amdgpu::PspStagePlan sp2 = amdgpu::psp_stage_plan(&tight, kFbMcBase);
        CHECK(!sp2.ok && strcmp(sp2.fail, "stage:space") == 0, "too small: %s", sp2.fail);
    }
    {
        amdgpu::PspStagePlan sp2 = amdgpu::psp_stage_plan(&f.rt.layout, 0u);   // no MC base
        CHECK(!sp2.ok && strcmp(sp2.fail, "stage:grant") == 0, "no MC base: %s", sp2.fail);
    }
}

// =========================================================================================
// the fake PSP (driven only from the two seams the substep offers)
// =========================================================================================
static uint32_t       g_autoload_pos;   // load_order index where AUTOLOAD_RLC appeared
static bool           g_autoload_seen;
static size_t         g_cmd_count;      // command buffers seen

// The command hook: answer the command buffer the substep has just staged. `status`/`tmr_size`
// go where the PSP would write them; the firmware "comes up" (IFWI ready + version registers)
// from the first command on.
static void seq_cmd_hook(void *cookie, uint64_t cmd_bar0_off)
{
    Dev &d = *static_cast<Dev *>(cookie);
    const R3Grant *cmd = r3_layout_grant(&g_plan, "psp_cmd");
    if (!cmd || cmd_bar0_off != cmd->start)
        return;
    uint32_t cmd_id = 0;
    bar0_get(d, cmd->start + amdgpu::kCmdRespCmdId, &cmd_id);
    g_cmd_count++;
    // B-04: every command frame carries buf_size/buf_version 0 (the zero-fill is the only
    // writer; Linux never populates them).
    uint32_t buf_size = 1u, buf_version = 1u;
    bar0_get(d, cmd->start + amdgpu::kCmdRespBufSize, &buf_size);
    bar0_get(d, cmd->start + amdgpu::kCmdRespBufVersion, &buf_version);
    if (buf_size != 0u || buf_version != 0u)
        d.cmd_buf_fields_nonzero = true;

    uint32_t status = 0;
    if (d.fail_cmd_id && cmd_id == d.fail_cmd_id)
        status = d.fail_status ? d.fail_status : 0x100u;
    if (cmd_id == amdgpu::kGfxCmdLoadIpFw) {
        uint32_t fw_type = 0, fw_size = 0;
        bar0_get(d, cmd->start + amdgpu::kCmdRespCmd + amdgpu::kLoadIpFwType, &fw_type);
        bar0_get(d, cmd->start + amdgpu::kCmdRespCmd + amdgpu::kLoadIpFwSize, &fw_size);
        d.load_order.push_back(fw_type);
        d.load_sizes.push_back(fw_size);
        if (d.fail_fw_type && fw_type == d.fail_fw_type)
            status = 0x100u;
    }
    if (cmd_id == amdgpu::kGfxCmdSetupTmr) {
        for (uint32_t w = 0; w < 6u; w++) {
            uint32_t v = 0;
            bar0_get(d, cmd->start + amdgpu::kCmdRespCmd + w * 4u, &v);
            d.tmr_body[w] = v;
        }
    }
    if (cmd_id == amdgpu::kGfxCmdAutoloadRlc) {
        g_autoload_seen = true;
        g_autoload_pos = static_cast<uint32_t>(d.load_order.size());
    }
    bar0_put(d, cmd->start + amdgpu::kCmdRespResp, status);
    const uint32_t tmr = (cmd_id == amdgpu::kGfxCmdLoadToc) ? d.tmr_size : 0u;
    bar0_put(d, cmd->start + amdgpu::kCmdRespResp + amdgpu::kCmdRespTmrSize, tmr);

    if (d.c2p33_ready_after)
        g_dev->bar5[g_dev->mp0(r3gold::kMp0SmnC2pmsg33.offset)] = d.c2p33;
    g_dev->bar5[g_dev->mp0(r3gold::kMp0SmnC2pmsg58.offset)] = d.c2p58;
}

// The ring hook: the rb frame is in the ring and the doorbell has not been rung yet. The
// fake records the frame; the PSP only reacts to the doorbell (C2PMSG_67), which io_mmio sees
// after the clock starts - so the response latency is modelled there, where the hardware
// puts it. Copying the fence here would make the measured latency always 0.
static void seq_ring_hook(void *cookie, uint64_t ring_bar0_off)
{
    (void)cookie;
    (void)ring_bar0_off;
}

// =========================================================================================
// sequence + failure branches
// =========================================================================================
/* Step2: register PSP rows from the declaration table. Replaces kAuxRegs[][2]. */
static void register_psp_rows(uint32_t mp0_base)
{
    const uint32_t rc = amdgpu::getPspRowCount();
    const R3WriteRowReg *rows = amdgpu::getPspRows();
    for (uint32_t i = 0; i < rc; i++) {
        const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
        const uint32_t addr = mp0_base + g->offset;
        const WClass cls = rows[i].cls;
        r3_wg_register_reg(addr, 1u, cls, WSubstep::Psp, "reg:psp");
    }
}

struct RunResult {
    R3SubstepResult r;
    bool ready_bit;
    uint64_t deny_before, deny_after;   // whitelist DENY counter around the substep
};

static RunResult run(Fixture &f, Dev &d)
{
    g_dev = &d;
    g_plan = f.rt.layout;
    d.load_order.clear();
    d.load_sizes.clear();
    d.cmd_buf_fields_nonzero = false;
    memset(d.tmr_body, 0, sizeof d.tmr_body);
    g_autoload_seen = false;
    g_autoload_pos = 0xFFFFFFFFu;
    g_cmd_count = 0;
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.env.cmd_hook = seq_cmd_hook;
    f.env.ring_hook = seq_ring_hook;
    f.env.hook_cookie = &d;

    // Integration wiring (§6.1): the plan's ② grants under the PSP owner, then the ③④ MP0
    // registers the sequence uses.
    r3_writeguard_install_mm();
    for (uint32_t i = 0; i < f.rt.layout.grant_count; i++) {
        const R3Grant &g = f.rt.layout.grants[i];
        const uint32_t dw_start = static_cast<uint32_t>(g.start >> 2);
        const uint32_t dw = static_cast<uint32_t>(g.size / 4u);
        r3_wg_register_layout(dw_start, dw, WSubstep::Psp, "layout:psp");
    }
    register_psp_rows(kMp0Base);
    r3_wg_register_shared_reg(kHdpDword, 1u, WClass::Protocol, WSubstep::Psp, "reg:hdp-flush");

    RunResult out;
    out.deny_before = r3_writeguard_deny_count();
    out.r = amdgpu::psp_v13_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    out.ready_bit = (f.rt.ready_bits & R3_IP_BIT(R3Ip::Psp)) != 0u;
    return out;
}

// Evidence lines live in the n48log ring on host too (n48log_stub); search its text.
static bool ev_has(const Fixture &f, const char *needle)
{
    (void)f;
    return strstr(n48log_host_text(), needle) != nullptr;
}

// --- happy path + wire order + the three planted-break guards ------------------------------
static void test_happy_path()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fence_delay_us = 800;   // inside the V4 1 ms budget
    RunResult rr = run(f, d);

    CHECK(rr.r.status == kR3SsecOk, "happy path succeeded: %s", rr.r.name);
    CHECK(rr.ready_bit, "PSP ready bit set on success");
    CHECK(rr.deny_after == rr.deny_before, "no whitelist DENY during the substep (%llu)",
          (unsigned long long)(rr.deny_after - rr.deny_before));
    CHECK(d.fence_calls == d.doorbells && d.doorbells == 20u,
          "B-01 fence callback ran once per submit (%u calls / %u doorbells)", d.fence_calls,
          d.doorbells);
    CHECK(!d.doorbell_without_fence, "B-01 callback precedes every C2PMSG_67 doorbell");
    CHECK(d.wptr_reset_seen, "ring-create WPTR reset before the first doorbell");

    // S2: the ring (16 KiB), cmd and fence (4 KiB each) were explicitly zero-filled. The fake
    // pre-fills BAR0 with 0xA5 so "still zero" proves a real clear happened.
    const R3Grant *ring_gr = r3_layout_grant(&f.rt.layout, "psp_ring");
    const R3Grant *cmd_gr = r3_layout_grant(&f.rt.layout, "psp_cmd");
    const R3Grant *fence_gr = r3_layout_grant(&f.rt.layout, "psp_fence");
    CHECK(ring_gr && cmd_gr && fence_gr, "ring/cmd/fence grants");
    if (ring_gr && cmd_gr && fence_gr) {
        CHECK(d.bar0[ring_gr->start + R3_PSP_RING_ZERO - 1u] == 0u,
              "ring tail not zero-filled (S2)");
        CHECK(d.bar0[cmd_gr->start + R3_PSP_CMD_ZERO - 1u] == 0u,
              "cmd tail not zero-filled (S2)");
        CHECK(d.bar0[fence_gr->start + R3_PSP_FENCE_ZERO - 1u] == 0u,
              "fence tail not zero-filled (S2)");
    }

    // 17 pieces, table order (D2), RLC_G last, AUTOLOAD immediately after it.
    CHECK(d.load_order.size() == 17u, "17 LOAD_IP_FW commands, got %zu", d.load_order.size());
    for (uint32_t i = 0; i < d.load_order.size() && i < 17u; i++)
        CHECK(d.load_order[i] == kGolden[i].fw_type, "wire order %u: %u != %u", i,
              d.load_order[i], kGolden[i].fw_type);
    CHECK(g_autoload_seen, "AUTOLOAD_RLC issued");
    CHECK(g_autoload_pos == 17u, "AUTOLOAD_RLC right after RLC_G (position %u)", g_autoload_pos);

    // C1.2: every LOAD_IP_FW fw_size is the header-parsed payload length (never a constant);
    // MEC is ucode_size_bytes - jt_size*4 = 267904 - 224*4 = 267008. The fw_type on the wire
    // is the table's, never RS64 (87/88/89) and never one of the excluded types (MEC JT 5/6,
    // RLC 22/7).
    for (uint32_t i = 0; i < d.load_order.size(); i++) {
        const uint32_t t = d.load_order[i];
        const bool excluded = (t == 5u || t == 6u || t == 7u || t == 22u ||
                               (t >= 87u && t <= 89u));
        CHECK(!excluded, "excluded fw_type %u reached the wire (entry %u)", t, i);
    }
    CHECK(d.load_sizes.size() == 17u, "17 fw_size values, got %zu", d.load_sizes.size());
    for (uint32_t i = 0; i < d.load_sizes.size() && i < 17u; i++)
        CHECK(d.load_sizes[i] == kGolden[i].size, "wire fw_size %u (%u) != golden %u", i,
              d.load_sizes[i], kGolden[i].size);
    CHECK(d.load_sizes.size() > 4u && d.load_sizes[4] == 267008u,
          "MEC fw_size excludes the 896 B jump table (got %u)",
          d.load_sizes.size() > 4u ? d.load_sizes[4] : 0u);
    CHECK(!d.cmd_buf_fields_nonzero, "buf_size/buf_version stay 0 in every command frame (B-04)");

    // S7 SETUP_TMR body: TMR MC, the PSP-returned size, virt_phy flag, local system PA.
    const R3Grant *tmr_gr = r3_layout_grant(&f.rt.layout, "tmr");
    const uint32_t tmr_mc = static_cast<uint32_t>(kFbMcBase + tmr_gr->start);
    const uint32_t sys_phy = static_cast<uint32_t>(kFbPhysBase + tmr_gr->start);
    CHECK(d.tmr_body[0] == tmr_mc, "SETUP_TMR buf_addr_lo 0x%x != 0x%x", d.tmr_body[0], tmr_mc);
    CHECK(d.tmr_body[2] == 0x4000000u, "SETUP_TMR buf_size 0x%x != PSP tmr_size", d.tmr_body[2]);
    CHECK(d.tmr_body[3] == 0x2u, "SETUP_TMR flags 0x%x != virt_phy_addr", d.tmr_body[3]);
    CHECK(d.tmr_body[4] == sys_phy, "SETUP_TMR system_phy_lo 0x%x != 0x%x (local FB_OFFSET)",
          d.tmr_body[4], sys_phy);
    // The nullarc constant 0x6A0C000000 is a 64-bit PA; its low dword is 0 and its high dword
    // 0x6A. Neither may appear here (tech plan §4.4 / R4 T8).
    CHECK(!(d.tmr_body[4] == 0u && d.tmr_body[5] == 0x6Au),
          "system_phy must not be the nullarc constant 0x6A0C000000");

    // V4 evidence tokens.
    CHECK(ev_has(f, "S3 ring create ok"), "ring evidence");
    CHECK(ev_has(f, "S11 fence_ctl=800 us cmd=0x20"),
          "S11 must report the measured 800 us control fence and its command");
    CHECK(ev_has(f, "S5 LOAD_TOC ok"), "toc evidence");
    CHECK(ev_has(f, "S7 SETUP_TMR ok"), "tmr evidence");
    CHECK(ev_has(f, "S9 AUTOLOAD_RLC ok"), "autoload evidence");
    CHECK(ev_has(f, "ready: toc loaded"), "ready evidence");
    // E2 completion marker, published as its own property (stage-1 n48r3-Done equivalent).
    {
        bool found = false;
        for (int i = 0; i < f.prop_count; i++)
            if (strcmp(f.props[i].key, "N48R3EVID-psp-done") == 0 &&
                strcmp(f.props[i].value, "Yes") == 0)
                found = true;
        CHECK(found, "N48R3EVID-psp-done=Yes must be published on success");
    }
    // The evidence channel is the n48log ring (n48log_stub on host); the retired
    // stage2-log assembly path no longer fills a per-instance arena.
    {
        const char *text = n48log_host_text();
        CHECK(strstr(text, "psp: ready: toc loaded") != nullptr ||
                  strstr(text, "ready: toc loaded") != nullptr,
              "ring text must carry the ready line");
        CHECK(strstr(text, "S3 ring create ok") != nullptr, "ring text carries S3");
    }
    CHECK(ev_has(f, "tmr_size=0x4000000"), "PSP-returned TMR size in the evidence");
    // system_phy is this machine's PA, not the nullarc constant (tech plan §4.4)
    CHECK(!ev_has(f, "0x6a0c000000") && !ev_has(f, "0x6A0C000000"),
          "system_phy must not be the nullarc constant");

    // the SETUP_TMR body: TMR MC, size, flags, system_phy recomputed from FB_OFFSET
    const R3Grant *tmr = r3_layout_grant(&f.rt.layout, "tmr");
    const uint64_t expect_mc = kFbMcBase + tmr->start;
    const uint64_t expect_pa = kFbPhysBase + tmr->start;
    char needle[128];
    snprintf(needle, sizeof needle,
             "S6 TMR ok start=0x%llx size=0x%llx slot=0x%llx mc=0x%llx sys_phy=0x%llx",
             (unsigned long long)tmr->start, (unsigned long long)d.tmr_size,
             (unsigned long long)tmr->size, (unsigned long long)expect_mc,
             (unsigned long long)expect_pa);
}

// --- SOS alive: the requirement C0.4 gate ------------------------------------------------
static void test_sos_not_alive()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p81 = 0;   // SOS not alive
    d.bar5[kMp0Base + r3gold::kMp0SmnC2pmsg81.offset] = 0u;
    RunResult rr = run(f, d);

    CHECK(rr.r.status == kR3SsecFail, "SOS not alive must FAIL");
    CHECK(rr.r.section == kR3SecPre, "failure is in the precondition section");
    CHECK(strcmp(rr.r.name, "pre:sos-not-alive") == 0, "reason %s", rr.r.name);
    CHECK(!rr.ready_bit, "no ready bit on a failed substep");
    CHECK(d.writes.empty(), "NO PSP state write before the SOS check passes (%zu writes)",
          d.writes.size());
    CHECK(d.bar0_writes.empty(), "NO BAR0 write before the SOS check passes");
    CHECK(d.load_order.empty(), "no firmware loaded");
    CHECK(g_cmd_count == 0u, "no GPCOM command issued");
    // evidence must carry both SOS readings and the reason
    CHECK(ev_has(f, "pre:sos-not-alive") || ev_has(f, "SOS not alive"), "SOS failure evidence");
    CHECK(ev_has(f, "C2PMSG_81=0x00000000"), "C2PMSG_81 reading exported");
    CHECK(rr.r.reading_count >= 2u, "both SOS readings attached to the result");
}

// --- LOAD_TOC status != 0: recorded, tmr_size still read, sequence continues (B-03) --------
static void test_load_toc_status_continues()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fail_cmd_id = amdgpu::kGfxCmdLoadToc;
    d.fail_status = 0x100u;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "LOAD_TOC nonzero status must continue: %s", rr.r.name);
    CHECK(rr.ready_bit, "ready bit set after a warned TOC status");
    CHECK(d.load_order.size() == 17u, "all 17 pieces after a warned TOC, got %zu",
          d.load_order.size());
    CHECK(g_autoload_seen, "AUTOLOAD_RLC still issued");
    CHECK(d.tmr_body[2] == 0x4000000u, "tmr_size read normally after a warned TOC (0x%x)",
          d.tmr_body[2]);
    CHECK(ev_has(f, "WARN s5-load-toc: PSP status=0x00000100 (cmd=0x20)"),
          "warning carries status + command number");
}

// --- tmr_size 0 ---------------------------------------------------------------------------
static void test_tmr_size_zero()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.tmr_size = 0;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "tmr_size 0 must FAIL");
    CHECK(strcmp(rr.r.name, "seq:tmr-size") == 0, "reason %s", rr.r.name);
    CHECK(!ev_has(f, "S7 SETUP_TMR ok"), "no SETUP_TMR after a zero size");
}

// --- one of the 17 pieces answers status != 0: warn and keep loading (B-03) ---------------
static void test_piece_status_continues()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fail_fw_type = 4;   // CP_MEC
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "piece nonzero status must continue: %s", rr.r.name);
    CHECK(d.load_order.size() == 17u, "all 17 pieces loaded, got %zu", d.load_order.size());
    CHECK(g_autoload_seen, "AUTOLOAD_RLC after the warned piece");
    CHECK(ev_has(f, "WARN s8-load-ip-fw: PSP status=0x00000100 (cmd=0x06)"),
          "piece warning carries status + command number");
    CHECK(ev_has(f, "mec") && ev_has(f, "status=0x100"), "S8 evidence names MEC and its status");
}

// --- S7 SETUP_TMR status != 0: recorded, loading continues (B-03) -------------------------
static void test_setup_tmr_status_continues()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fail_cmd_id = amdgpu::kGfxCmdSetupTmr;
    d.fail_status = 0x100u;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "SETUP_TMR nonzero status must continue: %s", rr.r.name);
    CHECK(d.load_order.size() == 17u, "all 17 pieces after a warned SETUP_TMR");
    char needle[128];
    snprintf(needle, sizeof needle,
             "WARN s7-setup-tmr: PSP status=0x00000100 (cmd=0x%02x)",
             amdgpu::kGfxCmdSetupTmr);
    CHECK(ev_has(f, needle), "SETUP_TMR warning carries status + command number");
}

// --- S9 AUTOLOAD_RLC status != 0: recorded, substep still succeeds (B-03) ------------------
static void test_autoload_status_continues()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fail_cmd_id = amdgpu::kGfxCmdAutoloadRlc;
    d.fail_status = 0x100u;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "AUTOLOAD_RLC nonzero status must continue: %s",
          rr.r.name);
    CHECK(g_autoload_seen, "AUTOLOAD_RLC was still issued");
    char needle[128];
    snprintf(needle, sizeof needle,
             "WARN s9-autoload-rlc: PSP status=0x00000100 (cmd=0x%02x)",
             amdgpu::kGfxCmdAutoloadRlc);
    CHECK(ev_has(f, needle), "AUTOLOAD warning carries status + command number");
}

// --- D-08: a returned size below the 64 MiB slot uses a sub-region -------------------------
static void test_tmr_slot_subregion()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.tmr_size = 0x2000000u;   // 32 MiB inside the 64 MiB reserved slot
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "sub-region placement must succeed: %s", rr.r.name);
    CHECK(d.tmr_body[2] == 0x2000000u, "SETUP_TMR uses the returned size 0x%x", d.tmr_body[2]);
    CHECK(ev_has(f, "size=0x2000000 slot=0x4000000"), "evidence shows sub-region + idle slot");
}

// --- D-08: a returned size above the 64 MiB slot fails closed with full evidence -----------
static void test_tmr_over_slot()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.tmr_size = 0x5000000u;   // 80 MiB > 64 MiB slot
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "oversize TMR must FAIL");
    CHECK(strcmp(rr.r.name, "seq:tmr-placement") == 0, "reason %s", rr.r.name);
    CHECK(ev_has(f, "FAIL S6 TMR placement: returned=0x5000000"), "full placement evidence");
    CHECK(!ev_has(f, "S7 SETUP_TMR ok"), "no SETUP_TMR for an unplaceable size");
}

// --- S6: the PSP-returned TMR size must be a 4 KB multiple (psp_gfx_if.h) ------------------
static void test_tmr_size_not_4k()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.tmr_size = 0x4000001u;   // not a 4 KB multiple
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "non-4K TMR size must FAIL");
    CHECK(strcmp(rr.r.name, "seq:tmr-placement") == 0, "reason %s", rr.r.name);
    CHECK(ev_has(f, "FAIL S6 TMR placement: returned=0x4000001"), "full placement evidence");
}

// --- B-06: C2PMSG_33 bit31 is an observation, never a ready gate ---------------------------
static void test_ready_33_observation()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.c2p33_ready_after = false;   // firmware does not set bit31 after the ring command
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "33 bit31 clear must NOT fail: %s", rr.r.name);
    CHECK(rr.ready_bit, "ready via 81 + fence; PSP version is recorded");
    CHECK(ev_has(f, "S10 C2PMSG_33=0x00000000"), "33 reading recorded as observation");
}

// --- S3: ring-create response status != 0 is a FAILURE (Linux full-mask wait) -------------
static void test_ring_status_fails()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.c2p64_resp = 0x80000100u;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "ring-create nonzero status must fail: %s", rr.r.name);
    CHECK(strcmp(rr.r.name, "seq:ring-response") == 0, "reason %s", rr.r.name);
}

// --- R1: no HDP window means no submit (fail closed before any PSP write) -----------------
static void test_missing_hdp_window()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    f.env.hdp_flush_valid = false;
    const uint64_t t0 = d.now;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "missing HDP window must FAIL: %s", rr.r.name);
    CHECK(strcmp(rr.r.name, "pre:no-hdp-window") == 0, "reason %s", rr.r.name);
    CHECK(d.doorbells == 0u, "no doorbell without a flush window");
    CHECK(d.now == t0, "the PRE gate stops before any wait");
}

// --- fence timeout ------------------------------------------------------------------------
static void test_fence_timeout()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.fence_ok = false;
    const uint64_t t0 = d.now;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "fence timeout must FAIL");
    CHECK(strcmp(rr.r.name, "seq:toc-fence") == 0 || strcmp(rr.r.name, "seq:toc-cmd") == 0,
          "reason %s", rr.r.name);
    // The wait is BOTH bounded by the fence budget (2 s) and actually spent: a poll loop that
    // gives up immediately (or never sleeps) is not the ceiling-bounded wait §3.1 specifies.
    const uint64_t spent = d.now - t0;
    CHECK(spent <= 2000000u + 100000u, "fence wait bounded (%llu us)",
          (unsigned long long)spent);
    CHECK(spent >= 2000000u - 100000u, "fence wait reached the 2 s ceiling (%llu us)",
          (unsigned long long)spent);
}

// --- S11: a fence slower than the 1 ms KPI is RECORDED, not a substep failure -------------
static void test_fence_over_budget_recorded()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    d.fence_delay_us = 1500;   // over the V4 KPI, far inside the 2 s timeout
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "over-KPI fence must NOT fail: %s", rr.r.name);
    CHECK(rr.ready_bit, "ready bit set; readiness is 81 + fence; PSP version is recorded");
    CHECK(ev_has(f, "S11 fence_ctl="), "S11 control-submit observation recorded");
    CHECK(ev_has(f, "fence_fw="), "S11 firmware-load observation recorded");
    CHECK(ev_has(f, "cmd=0x"), "the max's command id is recorded (scope anchor)");
}

// --- S12: the SDMA0 version is an observation; an unresolved engine version must NOT
// gate the PSP substep (D3/§8.2). The PSP's own version is recorded pre/post.
static void test_version_unresolved_not_gated()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    f.ip.base[(int)amdgpu::IPBlock::SDMA0][0] = 0u;   // unresolved SDMA0 block
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "unresolved SDMA0 version must NOT gate PSP: %s",
          rr.r.name);
    CHECK(rr.ready_bit, "PSP ready is 81 + fence; version is recorded");
    CHECK(ev_has(f, "S12 psp_version"), "S12 records the PSP version");
    CHECK(ev_has(f, "record, not a gate"), "S12 is marked as a record");
}

// --- §15.1 item 4: S12 must display the pre value from env (set by preflight -> sync).
static void test_version_pre_from_env()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p58 = 0x8000001u;
    // Simulate preflight having set the probe result.
    f.env.psp_version_pre = 0x002d0074u;
    f.env.psp_version_pre_ok = true;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "pre from env does not block: %s", rr.r.name);
    CHECK(ev_has(f, "pre=0x002d0074"),
          "S12 must show the preflight probe value");
    CHECK(!ev_has(f, "pre=----"),
          "S12 must NOT show ---- when env carries the preflight reading");
}

// --- ring create never becomes ready ------------------------------------------------------
static void test_ring_not_ready()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    d.c2p64_ready = 0;
    d.bar5[kMp0Base + r3gold::kMp0SmnC2pmsg64.offset] = 0u;
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "ring not ready must FAIL");
    CHECK(strcmp(rr.r.name, "seq:ring-not-ready") == 0, "reason %s", rr.r.name);
    CHECK(d.load_order.empty(), "no firmware loaded");
}

// --- preconditions: no plan, no MC base ---------------------------------------------------
static void test_preconditions()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d, 0x4000000u), "fixture");
    // Each case mutates the source fixture and re-binds env->rt / env.ip to that fixture's own
    // members (a plain copy would keep the pointers aimed at `f`).
    {   // invalid plan
        Fixture g;
        CHECK(build_fixture(g, d, 0x4000000u), "fixture");
        g.rt.layout.ok = false;
        g.rt.layout.fail_reason = "test";
        RunResult rr = run(g, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:layout-invalid") == 0,
              "invalid plan: %s", rr.r.name);
    }
    {   // missing grant
        Fixture g;
        CHECK(build_fixture(g, d, 0x4000000u), "fixture");
        g.rt.layout.grant_count = 1;   // only fw_buf
        RunResult rr = run(g, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:layout-grants") == 0,
              "missing grant: %s", rr.r.name);
    }
    {   // MC base unknown
        Fixture g;
        CHECK(build_fixture(g, d, 0x4000000u), "fixture");
        g.env.fb_mc_base = 0;
        RunResult rr = run(g, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:no-mc-base") == 0,
              "no MC base: %s", rr.r.name);
    }
    {   // incomplete I/O
        Fixture g;
        CHECK(build_fixture(g, d, 0x4000000u), "fixture");
        g.io.bus = nullptr;
        RunResult rr = run(g, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:no-io") == 0,
              "no bus: %s", rr.r.name);
    }
}

int main()
{

    printf("test_psp_msg: T13 offsets + load table + stage plan\n");
    test_t13_offsets();
    test_load_table_golden();
    test_stage_plan_golden();
    printf("test_psp_msg: sequence state machine\n");
    test_happy_path();
    test_sos_not_alive();
    test_load_toc_status_continues();
    test_tmr_size_zero();
    test_piece_status_continues();
    test_setup_tmr_status_continues();
    test_autoload_status_continues();
    test_tmr_slot_subregion();
    test_tmr_over_slot();
    test_tmr_size_not_4k();
    test_ready_33_observation();
    test_ring_status_fails();
    test_missing_hdp_window();
    test_fence_timeout();
    test_fence_over_budget_recorded();
    test_version_unresolved_not_gated();
    test_version_pre_from_env();
    test_ring_not_ready();
    test_preconditions();
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
