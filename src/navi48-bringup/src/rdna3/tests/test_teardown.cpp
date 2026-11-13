/*
 * test_teardown.cpp - host test for the RDNA3 failure dump and reverse teardown (K9).
 *
 * Covers tech plan §8.4/§9 and the T12 six scenarios:
 *   mes fail  -> mes action, sdma action, gfx none, smu none, psp untouched;
 *   sdma fail -> sdma action, gfx none, smu none;
 *   gfx fail  -> gfx none, smu none, zero writes;
 *   smu fail  -> smu none, zero writes;
 *   psp fail  -> no teardown action at all (PSP stays alive);
 *   stop      -> reverse order over the ready set.
 * Plus the failure-dump availability sets: psp (C2PMSG + fence + GC/MMHUB L2 fault),
 * smu (mailbox + GC L2 fault), gfx/sdma/mes (domain state + GC/MMHUB L2 fault),
 * readability skips (dump:skip:<obj>) and the hard rule that the dump never drains IH,
 * never writes, and never polls.
 *
 * Golden values are the Linux header offsets cited in rdna3_dump.cpp and the IP headers;
 * nothing here is taken from the implementation under test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "amdgpu_ip.h"
#include "amd/gfx_v11_0.h"
#include "amd/mes_v11_0.h"
#include "amd/psp_v13_0.h"
#include "amd/sdma_v6_0.h"
#include "amd/smu_v13_0.h"
#include "n48log_stub.h"
#include "rdna3/rdna3_dump.h"
#include "rdna3/golden_values.h"
#include "rdna3/rdna3_layout.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_teardown.h"
#include "rdna3/rdna3_integration.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/rdna3_wmanifest.h"

using namespace amdgpu;

namespace {

constexpr uint32_t kBar5Dwords = 0x100000u;
constexpr uint32_t kBar0Bytes = 0x00200000u; /* 2 MiB: enough for the fence grant */

/* Discovery stand-in bases (never register offsets: those come from the headers). */
constexpr uint32_t kGc0     = 0x10000u;
constexpr uint32_t kGc1     = 0x20000u;
constexpr uint32_t kMmhub1  = 0x30000u;
constexpr uint32_t kSdma0   = 0x40000u;
constexpr uint32_t kSdma1   = 0x50000u;
constexpr uint32_t kMp0     = 0x60000u;
constexpr uint32_t kMp1     = 0x70000u;
constexpr uint32_t kOsssys0 = 0x80000u;

/* GCVM/MMVM L2 fault offsets (gc_11_0_0_offset.h:2754-2759,
 * mmhub_3_0_1_offset.h:1058-1063). BASE_IDX 0 for GC, 1 for MMHUB. */
constexpr uint32_t kGcVmStatus = 0x15c8u;
constexpr uint32_t kGcVmLo     = 0x15c9u;
constexpr uint32_t kGcVmHi     = 0x15cau;

constexpr uint64_t kFenceOff = 0x00100000ull;

constexpr uint32_t kPspBit  = R3_IP_BIT(R3Ip::Psp);
constexpr uint32_t kSmuBit  = R3_IP_BIT(R3Ip::Smu);
constexpr uint32_t kGfxBit  = R3_IP_BIT(R3Ip::Gfx);
constexpr uint32_t kSdmaBit = R3_IP_BIT(R3Ip::Sdma);
constexpr uint32_t kMesBit  = R3_IP_BIT(R3Ip::Mes);

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

struct Dev {
    std::vector<uint32_t> bar5;
    std::vector<uint8_t> bar0;
    std::vector<uint32_t> writes;
    std::vector<uint32_t> reads;
    uint32_t cur_pipe;
    uint32_t cur_me;
    uint32_t force_ff_addr;   /* read returns 0xFFFFFFFF for this address */
    uint32_t fail_addr;       /* read fails at the bus level for this address */
    bool fail_mp0;            /* MP0 reads fail at the bus level */
};

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= d.bar5.size())
        return false;

    if (write) {
        d.writes.push_back(dword);
        d.bar5[dword] = *value;
        if (dword == kGc1 + kRegGrbmGfxCntl) {
            d.cur_pipe = *value & 0x3u;
            d.cur_me = (*value >> kGrbmMeIdShift) & 0x3u;
        }
        return true;
    }

    d.reads.push_back(dword);
    if (d.fail_addr != 0u && dword == d.fail_addr)
        return false;
    if (d.fail_mp0 && dword >= kMp0 && dword < kMp0 + 0x100u)
        return false;
    if (d.force_ff_addr != 0u && dword == d.force_ff_addr) {
        *value = 0xFFFFFFFFu;
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
    if (!value || dwords == 0u || dwords > 2u || bar != R3_IO_BAR_VRAM)
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

static void io_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }
static void io_mdelay(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }
static uint64_t io_now(void *ctx) { (void)ctx; return 0ull; }
static void io_lock(void *ctx) { (void)ctx; }
static void io_unlock(void *ctx) { (void)ctx; }

struct Prop {
    std::string key;
    std::string value;
};

struct Store {
    std::vector<Prop> props;
};

static bool prop_set(void *user, const char *key, const char *bytes, size_t len)
{
    Store &s = *static_cast<Store *>(user);
    if (!key || !bytes)
        return false;
    Prop p;
    p.key.assign(key);
    p.value.assign(bytes, len);
    s.props.push_back(p);
    return true;
}

struct Fixture {
    Dev d;
    R3Rt rt;
    R3EvidenceLines ev;
    R3EvSink sink;
    Store store;
    R3SubstepEnv env;
    R3Io io;
};

static void build_fixture(Fixture &f)
{
    f.d.bar5.assign(kBar5Dwords, 0u);
    f.d.bar0.assign(kBar0Bytes, 0u);
    f.d.cur_pipe = 0u;
    f.d.cur_me = 0u;
    f.d.force_ff_addr = 0u;
    f.d.fail_addr = 0u;
    f.d.fail_mp0 = false;

    memset(&f.rt, 0, sizeof f.rt);
    f.rt.layout.ok = true;
    f.rt.layout.bar0_limit = kBar0Bytes;
    f.rt.layout.limit = kBar0Bytes;
    f.rt.layout.grant_count = 1u;
    R3Grant &g = f.rt.layout.grants[0];
    g.start = kFenceOff;
    g.end = kFenceOff + 0x1000ull;
    g.size = 0x1000ull;
    g.owner = R3Owner::Psp;
    g.cls = R3B2Class::DataMove;
    g.needs_cpu = true;
    snprintf(g.tag, sizeof g.tag, "psp_fence");

    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.props.clear();
    f.sink.set_property = prop_set;
    f.sink.user = &f.store;

    memset(&f.env, 0, sizeof f.env);
    f.env.rt = &f.rt;
    f.env.ev = &f.ev;
    f.env.sink = &f.sink;
    f.env.fb_mc_base = 0x8000000000ull;
    f.env.strict_expect = true;

    static IPBaseTable ip;
    memset(&ip, 0, sizeof ip);
    /* memset loses the 0xFFFFFFFF sentinel; rebuild it explicitly. */
    for (int i = 0; i < static_cast<int>(IPBlock::Count); i++)
        for (int j = 0; j < IPBaseTable::kMaxBaseSegments; j++)
            ip.base[i][j] = 0xFFFFFFFFu;
    ip.set(IPBlock::GC, kGc0);
    ip.setBase(IPBlock::GC, 1, kGc1);
    ip.setBase(IPBlock::MMHUB, 1, kMmhub1);
    ip.set(IPBlock::SDMA0, kSdma0);
    ip.setBase(IPBlock::SDMA0, 1, kSdma1);
    ip.set(IPBlock::MP0, kMp0);
    ip.setBase(IPBlock::MP0, 1, kMp0);
    ip.setBase(IPBlock::MP1, 1, kMp1);
    ip.set(IPBlock::OSSSYS, kOsssys0);
    f.env.ip = &ip;

    memset(&f.io, 0, sizeof f.io);
    f.io.ctx = &f.d;
    f.io.mmio = io_mmio;
    f.io.smuio = io_smuio;
    f.io.bus = io_bus;
    f.io.udelay = io_udelay;
    f.io.mdelay = io_mdelay;
    f.io.now_us = io_now;
    f.io.lock = io_lock;
    f.io.unlock = io_unlock;
}

static bool ev_has(const Fixture &f, const char *needle)
{
    (void)f;
    return strstr(n48log_host_text(), needle) != nullptr;
}

static const std::string *prop_get(const Fixture &f, const char *key)
{
    for (const Prop &p : f.store.props) {
        if (p.key == key)
            return &p.value;
    }
    return nullptr;
}

static bool prop_has(const Fixture &f, const char *key)
{
    return prop_get(f, key) != nullptr;
}

static bool prop_contains(const Fixture &f, const char *key, const char *needle)
{
    const std::string *v = prop_get(f, key);
    return v != nullptr && v->find(needle) != std::string::npos;
}

static size_t write_index(const Dev &d, uint32_t addr)
{
    for (size_t i = 0; i < d.writes.size(); i++) {
        if (d.writes[i] == addr)
            return i;
    }
    return static_cast<size_t>(-1);
}

static bool write_has_readback(const Dev &d)
{
    /* Every teardown write must be followed by at least one read of the same register
     * (the "readback boundary" §9 requires). */
    for (size_t i = 0; i < d.writes.size(); i++) {
        bool found = false;
        for (size_t j = 0; j < d.reads.size(); j++) {
            if (d.reads[j] == d.writes[i]) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool no_smu_gfx_writes(const Dev &d)
{
    for (uint32_t a : d.writes) {
        /* GFX read-only registers and SMU mailbox/effect registers must never be written. */
        if (a == kGc1 + kGfx11ImuScratch0 || a == kGc1 + kGfx11RlcStat ||
            a == kGc0 + kGfx11GrbmStatus || a == kGc0 + kGfx11CpStat ||
            a == kMp1 + kSmuC2pmsg66 || a == kMp1 + kSmuC2pmsg82 ||
            a == kMp1 + kSmuC2pmsg90)
            return false;
    }
    return true;
}

static R3TeardownReport run_teardown(Fixture &f, uint32_t ready, bool failed, R3Ip ip)
{
    f.rt.ready_bits = ready;
    f.rt.latch_bits = 0u;
    f.d.writes.clear();
    f.d.reads.clear();
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.props.clear();

    /* The integration arms the whitelist before any substep; teardown must reuse the
     * same table. D-09: power-up rows must already exist — teardown only reuses them. */
    r3_writeguard_install_mm();
    r3_wg_register_reg(kGc1 + kRegGrbmGfxCntl, 1u, WClass::State, WSubstep::Mes,
                       "reg:mes");
    r3_wg_register_reg(kGc0 + kRegCpHqdActive, 1u, WClass::State, WSubstep::Mes,
                       "reg:mes");
    r3_wg_register_reg(kGc1 + kRegMesCntl, 1u, WClass::State, WSubstep::Mes,
                       "reg:mes");
    r3_wg_register_reg(kSdma0 + kRegSdma0Queue0RbCntl, 1u, WClass::State,
                       WSubstep::Sdma, "reg:sdma");
    r3_wg_register_reg(kSdma1 + kRegSdma0F32Cntl, 1u, WClass::State,
                       WSubstep::Sdma, "reg:sdma");

    R3TeardownEnv te;
    te.io = &f.io;
    te.sub = &f.env;
    te.failed = failed;
    te.failed_ip = ip;
    return r3_teardown_run(&te);
}

/* Same as run_teardown, but the integration has already registered the power-up rows
 * (the K10 wiring). Teardown must REUSE them, not append duplicates, and must still
 * classify with the teardown ctx while the manifest keeps the power-up tag. */
static R3TeardownReport run_teardown_pre_registered(Fixture &f, uint32_t ready)
{
    f.rt.ready_bits = ready;
    f.rt.latch_bits = 0u;
    f.d.writes.clear();
    f.d.reads.clear();
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.props.clear();

    r3_writeguard_install_mm();
    /* The rows mes_v11_0.cpp / sdma_v6_0.cpp write during power-up. */
    CHECK(r3_wg_register_reg(kGc1 + kRegGrbmGfxCntl, 1u, WClass::State, WSubstep::Mes,
                             "reg:mes") == 0, "register power-up grbm");
    CHECK(r3_wg_register_reg(kGc0 + kRegCpHqdActive, 1u, WClass::State, WSubstep::Mes,
                             "reg:mes") == 0, "register power-up hqd");
    CHECK(r3_wg_register_reg(kGc1 + kRegMesCntl, 1u, WClass::State, WSubstep::Mes,
                             "reg:mes") == 0, "register power-up mes cntl");
    CHECK(r3_wg_register_reg(kSdma0 + kRegSdma0Queue0RbCntl, 1u, WClass::State,
                             WSubstep::Sdma, "reg:sdma") == 0, "register power-up rb");
    CHECK(r3_wg_register_reg(kSdma1 + kRegSdma0F32Cntl, 1u, WClass::State,
                             WSubstep::Sdma, "reg:sdma") == 0, "register power-up f32");
    const uint64_t deny_before = r3_writeguard_deny_count();

    R3TeardownEnv te;
    te.io = &f.io;
    te.sub = &f.env;
    te.failed = true;
    te.failed_ip = R3Ip::Mes;
    const R3TeardownReport r = r3_teardown_run(&te);

    CHECK(r3_writeguard_deny_count() == deny_before, "teardown must not add DENY rows");
    return r;
}

static void run_dump(Fixture &f, R3Ip ip, bool has_failed)
{
    f.d.writes.clear();
    f.d.reads.clear();
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.props.clear();

    R3DumpEnv de;
    de.io = &f.io;
    de.sub = &f.env;
    de.failed_ip = ip;
    de.has_failed_ip = has_failed;
    r3_dump_failure_state(&de);
}

static bool no_ih_in_dump(const Fixture &f)
{
    for (const Prop &p : f.store.props) {
        if (p.value.find("IH") != std::string::npos ||
            p.value.find("drain") != std::string::npos ||
            p.value.find("ih_") != std::string::npos)
            return false;
    }
    const char *ev_text = n48log_host_text();
    if (strstr(ev_text, "IH") != nullptr || strstr(ev_text, "drain") != nullptr ||
        strstr(ev_text, "ih_") != nullptr)
        return false;
    /* IH ring registers are OSSSYS + 0x0080..0x0082 (stage-1 dump offsets). */
    for (uint32_t a : f.d.reads) {
        if (a >= kOsssys0 + 0x0080u && a <= kOsssys0 + 0x0082u)
            return false;
    }
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* T12: teardown order and writes                                                        */
/* -------------------------------------------------------------------------------------- */

static void scenario_mes_failure()
{
    Fixture f;
    build_fixture(f);
    const uint32_t ready = kPspBit | kSmuBit | kGfxBit | kSdmaBit;
    const R3TeardownReport r = run_teardown(f, ready, true, R3Ip::Mes);
    const Dev &d = f.d;

    CHECK(r.triggered, "mes failure must trigger teardown");
    CHECK(r.acted == (kMesBit | kSdmaBit), "acted=0x%x", r.acted);
    CHECK(r.none == (kGfxBit | kSmuBit), "none=0x%x", r.none);
    CHECK(r.skipped == kPspBit, "psp must not be acted on or logged none (skipped=0x%x)",
          r.skipped);
    CHECK(r.write_fail == 0u, "write_fail=0x%x", r.write_fail);
    CHECK((r.started & kMesBit) != 0u, "the failed MES counts as started");

    /* Reverse order: every MES write precedes every SDMA write. */
    const size_t mes_reset = write_index(d, kGc1 + kRegMesCntl);
    const size_t sdma_rb = write_index(d, kSdma0 + kRegSdma0Queue0RbCntl);
    const size_t sdma_f32 = write_index(d, kSdma1 + kRegSdma0F32Cntl);
    CHECK(mes_reset != static_cast<size_t>(-1), "CP_MES_CNTL reset write missing");
    CHECK(sdma_rb != static_cast<size_t>(-1), "RB_CNTL write missing");
    CHECK(sdma_f32 != static_cast<size_t>(-1), "F32_CNTL write missing");
    CHECK(mes_reset < sdma_rb, "teardown order is not mes->sdma (mes=%zu sdma=%zu)",
          mes_reset, sdma_rb);
    CHECK(sdma_rb < sdma_f32, "SDMA order is not RB_CNTL->F32_CNTL");

    /* MES action: both HQD selections deactivate, then the reset. */
    const size_t sch_sel = write_index(d, kGc1 + kRegGrbmGfxCntl);
    const size_t hqd0 = write_index(d, kGc0 + kRegCpHqdActive);
    CHECK(sch_sel < hqd0, "GRBM select must precede CP_HQD_ACTIVE");
    const uint32_t rb_val = d.bar5[kSdma0 + kRegSdma0Queue0RbCntl];
    const uint32_t f32_val = d.bar5[kSdma1 + kRegSdma0F32Cntl];
    CHECK((rb_val & kRbCntlRbEnableMask) == 0u, "RB_ENABLE not cleared (0x%08x)", rb_val);
    CHECK((f32_val & kF32CntlHaltMask) != 0u, "F32 HALT not set (0x%08x)", f32_val);
    const uint32_t cntl = d.bar5[kGc1 + kRegMesCntl];
    CHECK((cntl & (kMesCntlPipe0Reset | kMesCntlPipe1Reset)) ==
              (kMesCntlPipe0Reset | kMesCntlPipe1Reset),
          "CP_MES_CNTL reset bits not set (0x%08x)", cntl);
    CHECK((cntl & (kMesCntlPipe0Active | kMesCntlPipe1Active)) == 0u,
          "CP_MES_CNTL still active (0x%08x)", cntl);

    CHECK(no_smu_gfx_writes(d), "gfx/smu must have no writes");
    CHECK(write_has_readback(d), "every teardown write needs a readback");

    /* Log order: mes -> sdma -> gfx none -> smu none; no psp line. */
    CHECK(ev_has(f, "teardown: mes"), "missing teardown: mes");
    CHECK(ev_has(f, "teardown: sdma"), "missing teardown: sdma");
    CHECK(ev_has(f, "teardown: gfx none"), "missing teardown: gfx none");
    CHECK(ev_has(f, "teardown: smu none (no inverse in minimal message set)"),
          "missing smu none line");
    CHECK(!ev_has(f, "teardown: psp"), "PSP must not get a teardown action");
    CHECK(prop_has(f, "N48R3EVID-teardown"), "teardown evidence property missing");
}

static void scenario_sdma_failure()
{
    Fixture f;
    build_fixture(f);
    const R3TeardownReport r = run_teardown(f, kPspBit | kSmuBit | kGfxBit, true,
                                            R3Ip::Sdma);
    const Dev &d = f.d;

    CHECK(r.acted == kSdmaBit, "acted=0x%x", r.acted);
    CHECK(r.none == (kGfxBit | kSmuBit), "none=0x%x", r.none);
    CHECK((r.started & kSdmaBit) != 0u, "the failed SDMA counts as started");
    CHECK(write_index(d, kGc1 + kRegMesCntl) == static_cast<size_t>(-1),
          "MES was never started and must not be reset");
    CHECK(write_index(d, kSdma0 + kRegSdma0Queue0RbCntl) != static_cast<size_t>(-1),
          "RB_CNTL write missing");
    CHECK(no_smu_gfx_writes(d), "gfx/smu must have no writes");
    CHECK(write_has_readback(d), "every teardown write needs a readback");
    CHECK(ev_has(f, "teardown: sdma"), "missing teardown: sdma");
    CHECK(ev_has(f, "teardown: gfx none"), "missing teardown: gfx none");
    CHECK(ev_has(f, "teardown: smu none (no inverse in minimal message set)"),
          "missing smu none line");
}

static void scenario_gfx_failure()
{
    Fixture f;
    build_fixture(f);
    const R3TeardownReport r = run_teardown(f, kPspBit | kSmuBit, true, R3Ip::Gfx);

    CHECK(r.acted == 0u, "gfx has no action (acted=0x%x)", r.acted);
    CHECK(r.none == (kGfxBit | kSmuBit), "none=0x%x", r.none);
    CHECK(f.d.writes.empty(), "gfx/smu teardown must write nothing");
    CHECK(ev_has(f, "teardown: gfx none"), "missing teardown: gfx none");
    CHECK(ev_has(f, "teardown: smu none (no inverse in minimal message set)"),
          "missing smu none line");
}

static void scenario_smu_failure()
{
    Fixture f;
    build_fixture(f);
    const R3TeardownReport r = run_teardown(f, kPspBit, true, R3Ip::Smu);

    CHECK(r.acted == 0u, "smu has no action (acted=0x%x)", r.acted);
    CHECK(r.none == kSmuBit, "none=0x%x", r.none);
    CHECK(f.d.writes.empty(), "smu teardown must write nothing");
    CHECK(ev_has(f, "teardown: smu none (no inverse in minimal message set)"),
          "missing smu none line");
}

static void scenario_psp_failure()
{
    Fixture f;
    build_fixture(f);
    const R3TeardownReport r = run_teardown(f, 0u, true, R3Ip::Psp);

    CHECK(r.psp_failed, "psp_failed must be reported");
    CHECK(r.acted == 0u, "PSP failure must not act (acted=0x%x)", r.acted);
    CHECK(r.none == 0u, "PSP failure must not register per-IP none (none=0x%x)", r.none);
    CHECK(f.d.writes.empty(), "PSP failure must perform no teardown write");
    CHECK(!ev_has(f, "teardown: mes"), "PSP failure ran a MES teardown");
    CHECK(!ev_has(f, "teardown: sdma"), "PSP failure ran an SDMA teardown");
    CHECK(!ev_has(f, "teardown: gfx"), "PSP failure ran a GFX teardown");
    CHECK(!ev_has(f, "teardown: smu"), "PSP failure ran an SMU teardown");
    CHECK(ev_has(f, "psp left alive"), "PSP alive must be logged");

    /* A PSP failure wins even if stale ready bits claim later IPs started: the ladder
     * never reached them in this boot, so no teardown action may run. */
    build_fixture(f);
    const uint32_t all = kPspBit | kSmuBit | kGfxBit | kSdmaBit | kMesBit;
    const R3TeardownReport r2 = run_teardown(f, all, true, R3Ip::Psp);
    CHECK(r2.psp_failed, "psp_failed must win over stale ready bits");
    CHECK(r2.acted == 0u, "stale ready bits must not trigger PSP teardown (acted=0x%x)",
          r2.acted);
    CHECK(f.d.writes.empty(), "stale ready bits must not produce teardown writes");
}

static void scenario_stop_after_success()
{
    Fixture f;
    build_fixture(f);
    const uint32_t all = kPspBit | kSmuBit | kGfxBit | kSdmaBit | kMesBit;
    const R3TeardownReport r = run_teardown(f, all, false, R3Ip::Psp);
    const Dev &d = f.d;

    CHECK(r.triggered, "stop with ready IPs must trigger teardown");
    CHECK(r.acted == (kMesBit | kSdmaBit), "acted=0x%x", r.acted);
    CHECK(r.none == (kGfxBit | kSmuBit), "none=0x%x", r.none);
    const size_t mes = write_index(d, kGc1 + kRegMesCntl);
    const size_t sdma = write_index(d, kSdma0 + kRegSdma0Queue0RbCntl);
    CHECK(mes != static_cast<size_t>(-1) && sdma != static_cast<size_t>(-1),
          "both actions must run");
    CHECK(mes < sdma, "stop teardown order is not mes->sdma");
}

/* K10 wiring: the power-up rows already exist in the whitelist. Teardown must reuse
 * them (no duplicate registration) and still write under the teardown ctx. */
static void scenario_power_up_rows_reused()
{
    Fixture f;
    build_fixture(f);
    const uint32_t ready = kPspBit | kSmuBit | kGfxBit | kSdmaBit;
    const R3TeardownReport r = run_teardown_pre_registered(f, ready);

    CHECK(r.acted == (kMesBit | kSdmaBit), "acted=0x%x", r.acted);
    CHECK(r.write_fail == 0u, "reused rows must still classify (write_fail=0x%x)",
          r.write_fail);
    CHECK(write_index(f.d, kGc1 + kRegMesCntl) != static_cast<size_t>(-1),
          "MES reset write missing on the reuse path");
    CHECK(write_index(f.d, kSdma0 + kRegSdma0Queue0RbCntl) != static_cast<size_t>(-1),
          "SDMA RB write missing on the reuse path");

    /* The manifest line must carry the teardown ctx even though the tag row was the
     * power-up one (tech plan §9: same table, different ctx). */
    const R3WManifest *m = r3_writeguard_manifest();
    bool ctx_seen = false;
    for (uint32_t i = 0; m && i < m->count; i++) {
        if (m->entries[i].ctx &&
            (strcmp(m->entries[i].ctx, "teardown:mes") == 0 ||
             strcmp(m->entries[i].ctx, "teardown:sdma") == 0))
            ctx_seen = true;
    }
    CHECK(ctx_seen, "write manifest must record the teardown:<ip> ctx");
}

/* D-09 planted break: teardown must NOT add new whitelist rows. When the power-up rows
 * are absent (the substep did not write those registers), teardown skips the action and
 * logs "no reversible object" — it never registers new rows to widen the whitelist. */
static void scenario_d09_teardown_no_new_rows()
{
    Fixture f;
    build_fixture(f);
    const uint32_t ready = kPspBit | kSmuBit | kGfxBit | kSdmaBit | kMesBit;

    f.rt.ready_bits = ready;
    f.rt.latch_bits = 0u;
    f.d.writes.clear();
    f.d.reads.clear();
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.store.props.clear();

    /* Arm ONLY the MM windows — no power-up production rows registered. */
    r3_writeguard_install_mm();
    const uint32_t grants_before = r3_wg_grant_count();

    R3TeardownEnv te;
    te.io = &f.io;
    te.sub = &f.env;
    te.failed = true;
    te.failed_ip = R3Ip::Mes;
    const R3TeardownReport r = r3_teardown_run(&te);

    /* D-09: no new rows were added (grant count unchanged). */
    CHECK(r3_wg_grant_count() == grants_before,
          "teardown must not add new rows (before=%u after=%u)",
          grants_before, r3_wg_grant_count());
    /* MES and SDMA skipped (no reversible object), not acted. */
    CHECK(r.acted == 0u, "no IP acted without power-up rows (acted=0x%x)", r.acted);
    CHECK((r.none & (kMesBit | kSdmaBit)) != 0u,
          "MES and SDMA must be 'none' when rows absent (none=0x%x)", r.none);
    CHECK(r.write_fail == 0u, "skip is not a failure (write_fail=0x%x)", r.write_fail);
    CHECK(ev_has(f, "no reversible object"), "missing 'no reversible object' log");
    CHECK(f.d.writes.empty(), "no writes without power-up rows");
}

/* -------------------------------------------------------------------------------------- */
/* §8.4: dump availability sets                                                          */
/* -------------------------------------------------------------------------------------- */

static void scenario_dump_psp()
{
    Fixture f;
    build_fixture(f);
    f.d.bar5[kMp0 + r3gold::kMp0SmnC2pmsg33.offset] = 0x80000000u;
    f.d.bar5[kGc0 + kGcVmStatus] = 0x00000001u;
    f.d.bar5[kGc0 + kGcVmLo] = 0x00001000u;
    f.d.bar5[kGc0 + kGcVmHi] = 0x00000080u;
    f.d.bar0[kFenceOff] = 0x5Au;
    run_dump(f, R3Ip::Psp, true);

    CHECK(prop_has(f, "N48R3EVID-dump-psp"), "dump-psp missing");
    CHECK(prop_has(f, "N48R3EVID-dump-gcvm"), "dump-gcvm missing");
    CHECK(prop_has(f, "N48R3EVID-dump-mmvm"), "dump-mmvm missing");
    CHECK(!prop_has(f, "N48R3EVID-dump-smu"), "psp dump must not probe SMU");
    CHECK(!prop_has(f, "N48R3EVID-dump-gfx"), "psp dump must not probe GFX");
    CHECK(!prop_has(f, "N48R3EVID-dump-sdma"), "psp dump must not probe SDMA");
    CHECK(!prop_has(f, "N48R3EVID-dump-mes"), "psp dump must not probe MES");
    CHECK(prop_contains(f, "N48R3EVID-dump-psp", "c2pmsg33=0x80000000"),
          "C2PMSG_33 reading missing");
    CHECK(prop_has(f, "N48R3EVID-dump-psp-fence"), "dump-psp-fence missing");
    CHECK(prop_contains(f, "N48R3EVID-dump-psp-fence", "fence[0..3]"),
          "fence reading missing");
    CHECK(prop_contains(f, "N48R3EVID-dump-gcvm", "status=0x00000001"),
          "GCVM status missing");
    CHECK(no_ih_in_dump(f), "dump must never touch IH");
    CHECK(f.d.writes.empty(), "dump must not write");
}

static void scenario_dump_smu()
{
    Fixture f;
    build_fixture(f);
    f.d.bar5[kMp1 + kSmuC2pmsg66] = 0x00000016u;
    f.d.bar5[kMp1 + kSmuC2pmsg90] = 0x00000001u;
    run_dump(f, R3Ip::Smu, true);

    CHECK(prop_has(f, "N48R3EVID-dump-smu"), "dump-smu missing");
    CHECK(prop_has(f, "N48R3EVID-dump-gcvm"), "dump-gcvm missing");
    CHECK(!prop_has(f, "N48R3EVID-dump-mmvm"), "smu dump must not probe MMHUB");
    CHECK(!prop_has(f, "N48R3EVID-dump-psp"), "smu dump must not probe PSP");
    CHECK(prop_contains(f, "N48R3EVID-dump-smu", "c2pmsg66=0x00000016"),
          "SMU mailbox reading missing");
    CHECK(no_ih_in_dump(f), "dump must never touch IH");
    CHECK(f.d.writes.empty(), "dump must not write");
}

static void scenario_dump_gfx()
{
    Fixture f;
    build_fixture(f);
    f.d.bar5[kGc1 + kGfx11RlcStat] = 0x00000025u;
    f.d.bar5[kGc0 + kGfx11GrbmStatus] = 0x00000001u;
    run_dump(f, R3Ip::Gfx, true);

    CHECK(prop_has(f, "N48R3EVID-dump-gfx"), "dump-gfx missing");
    CHECK(prop_has(f, "N48R3EVID-dump-gcvm"), "dump-gcvm missing");
    CHECK(prop_has(f, "N48R3EVID-dump-mmvm"), "dump-mmvm missing");
    CHECK(prop_contains(f, "N48R3EVID-dump-gfx", "rlc_stat=0x00000025"),
          "RLC_STAT reading missing");
    CHECK(no_ih_in_dump(f), "dump must never touch IH");
    CHECK(f.d.writes.empty(), "dump must not write");
}

static void scenario_dump_sdma()
{
    Fixture f;
    build_fixture(f);
    f.d.bar5[kSdma0 + kRegSdma0StatusReg] = 0x00000003u;
    run_dump(f, R3Ip::Sdma, true);

    CHECK(prop_has(f, "N48R3EVID-dump-sdma"), "dump-sdma missing");
    CHECK(prop_has(f, "N48R3EVID-dump-gcvm"), "dump-gcvm missing");
    CHECK(prop_has(f, "N48R3EVID-dump-mmvm"), "dump-mmvm missing");
    CHECK(prop_contains(f, "N48R3EVID-dump-sdma", "status=0x00000003"),
          "SDMA0_STATUS_REG reading missing");
    CHECK(no_ih_in_dump(f), "dump must never touch IH");
    CHECK(f.d.writes.empty(), "dump must not write");
}

static void scenario_dump_mes()
{
    Fixture f;
    build_fixture(f);
    f.d.bar5[kGc1 + kRegMesCntl] = 0x0C000000u;
    run_dump(f, R3Ip::Mes, true);

    CHECK(prop_has(f, "N48R3EVID-dump-mes"), "dump-mes missing");
    CHECK(prop_has(f, "N48R3EVID-dump-gcvm"), "dump-gcvm missing");
    CHECK(prop_has(f, "N48R3EVID-dump-mmvm"), "dump-mmvm missing");
    CHECK(prop_contains(f, "N48R3EVID-dump-mes", "cp_mes_cntl=0x0c000000"),
          "CP_MES_CNTL reading missing");
    CHECK(no_ih_in_dump(f), "dump must never touch IH");
    CHECK(f.d.writes.empty(), "dump must not write");
}

static void scenario_dump_skips()
{
    Fixture f;
    build_fixture(f);
    /* Unresolved MP0 base: the C2PMSG group is skipped, not guessed. */
    static_cast<IPBaseTable *>(const_cast<void *>(f.env.ip))->setBase(IPBlock::MP0, 1,
                                                                     0xFFFFFFFFu);
    run_dump(f, R3Ip::Psp, true);
    CHECK(prop_contains(f, "N48R3EVID-dump-psp", "dump:skip:psp-c2pmsg"),
          "unresolved MP0 must record dump:skip:psp-c2pmsg");

    /* A 0xFFFFFFFF readability probe drops the whole GC fault group. */
    build_fixture(f);
    f.d.force_ff_addr = kGc0 + kGcVmStatus;
    run_dump(f, R3Ip::Gfx, true);
    CHECK(prop_contains(f, "N48R3EVID-dump-gcvm", "dump:skip:gcvm"),
          "0xFFFFFFFF probe must record dump:skip:gcvm");
    CHECK(f.d.writes.empty(), "dump must not write");

    /* A read failure after a successful probe aborts only that group. */
    build_fixture(f);
    f.d.fail_addr = kGc0 + kGcVmLo;
    run_dump(f, R3Ip::Gfx, true);
    CHECK(prop_contains(f, "N48R3EVID-dump-gcvm", "dump:abort:gcvm"),
          "mid-group failure must record dump:abort:gcvm");
    CHECK(prop_contains(f, "N48R3EVID-dump-gcvm", "status="),
          "the abort must keep the status already read");
    CHECK(prop_has(f, "N48R3EVID-dump-mmvm"),
          "aborting GCVM must not stop the independent MMVM group");
    CHECK(f.d.writes.empty(), "dump must not write");
}

} /* namespace */

/* F2 (review P2-1): a latched IP must be gated off for any later enabled() evaluation on
 * the same boot -- the consumption side of the runtime latch the stage wrappers now set
 * before the dump. */
static void scenario_latch_gates_disabled_ip()
{
    R3GateInputs in;
    in.stage = 5;
    in.kill_mask = 0u;
    in.latch_mask = kMesBit | kSdmaBit; /* what run_substep sets on their failure */
    CHECK(r3_integration_enabled(&in) == (kPspBit | kSmuBit | kGfxBit),
          "latched mes/sdma must drop out of the enabled bitmap (got 0x%x)",
          r3_integration_enabled(&in));
    in.latch_mask = kSmuBit;
    /* Latching smu closes gfx (gfx->smu) and mes (mes->gfx); psp and sdma survive. */
    CHECK(r3_integration_enabled(&in) == (kPspBit | kSdmaBit),
          "latching smu must close gfx and mes (got 0x%x)",
          r3_integration_enabled(&in));
}

int main()
{
    scenario_mes_failure();
    scenario_sdma_failure();
    scenario_gfx_failure();
    scenario_smu_failure();
    scenario_psp_failure();
    scenario_stop_after_success();
    scenario_latch_gates_disabled_ip();
    scenario_power_up_rows_reused();
    scenario_d09_teardown_no_new_rows();

    scenario_dump_psp();
    scenario_dump_smu();
    scenario_dump_gfx();
    scenario_dump_sdma();
    scenario_dump_mes();
    scenario_dump_skips();

    printf("%u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
