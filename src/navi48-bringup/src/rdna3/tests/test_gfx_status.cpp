/*
 * test_gfx_status.cpp - host tests for the GFX 11.0.1 readiness + constants-init substep
 * (K6 / requirement C3.1-C3.3, tech plan §3.3 + §8.1 + §10).
 *
 * Everything runs on a RAM-backed fake of BAR5: the substep writes only the stage-2 declared
 * rows (four W1.5 + ten golden), so the fake records every write and proves nothing else
 * reaches it. Golden values come from independent sources, never from the code under test (H4):
 *
 *   Offsets  re-transcribed here from gc_11_0_0_offset.h / gc_11_0_0_sh_mask.h (the
 *            expected numbers are literals in this file, not references to the header).
 *   Values   healthy readings are nullarc E3d's (GRBM_STATUS=0x302c,
 *            GRBM_STATUS2=0x0400000c, RLC_STAT=0x25, RLC_GPM_STAT=0x00b40017,
 *            CP_ME_CNTL=0x15000000), the failure form is E4b's CP_STAT=0x80008000,
 *            and readiness is BOOTLOAD_COMPLETE=bit31 with CP_STAT=0.
 *   Time     the fake clock advances only through the R3Io delay callbacks, so the
 *            readiness poll budget is observable, not assumed.
 */
#include <stdio.h>
#include <string.h>

#include <vector>

#include "amdgpu_ip.h"
#include "amd/gfx_v11_0.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_substep.h"
#include "rdna3/rdna3_writeguard.h"
#include "n48log_stub.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// =========================================================================================
// The fake device
// =========================================================================================
static const uint32_t kBar5Dwords = 0x100000u;   // 4 MiB of BAR5
static const uint32_t kGcBase0    = 0x10000u;    // GC BASE_IDX 0 (GRBM/CP_STAT)
static const uint32_t kGcBase1    = 0x30000u;    // GC BASE_IDX 1 (IMU/RLC/CP_ME_CNTL)

struct RegSeq {
    uint32_t addr;
    uint32_t vals[4];
    uint32_t n;
    uint32_t i;
};

struct Dev {
    std::vector<uint32_t> bar5;
    std::vector<uint32_t> reads;        // BAR5 dwords read, in order
    std::vector<uint32_t> writes;       // BAR5 dwords written, in order (declared rows only)
    std::vector<RegSeq>   seqs;         // per-address read sequences
    std::vector<uint32_t> fail_reads;   // addresses whose read returns false
    uint64_t now;
    uint32_t vmid;                      // GRBM_GFX_CNTL.VMID selector (value >> 4)
    uint32_t shm_config[16];            // SH_MEM_CONFIG per VMID
    uint32_t shm_bases[16];             // SH_MEM_BASES per VMID

    Dev()
    {
        bar5.assign(kBar5Dwords, 0);
        now = 1000;
        vmid = 0;
        memset(shm_config, 0, sizeof shm_config);
        memset(shm_bases, 0, sizeof shm_bases);
    }

    void set(uint32_t base, uint32_t off, uint32_t v) { bar5[base + off] = v; }

    // SH_MEM_CONFIG/BASES are selected by GRBM_GFX_CNTL.VMID; preset all 16 slots.
    void set_shmem_all(uint32_t config, uint32_t bases)
    {
        for (int v = 0; v < 16; v++) {
            shm_config[v] = config;
            shm_bases[v] = bases;
        }
    }

    // Two-sample sequence for a stability register: first read -> a, second -> b.
    void seq2(uint32_t base, uint32_t off, uint32_t a, uint32_t b)
    {
        RegSeq s;
        s.addr = base + off;
        s.vals[0] = a;
        s.vals[1] = b;
        s.n = 2;
        s.i = 0;
        seqs.push_back(s);
    }
};

static Dev *g_dev;

static bool io_mmio(void *, uint32_t dword, uint32_t *value, bool write)
{
    if (dword >= kBar5Dwords)
        return false;
    const uint32_t gfx_cntl  = kGcBase1 + 0x0900u;   // GRBM_GFX_CNTL (VMID selector)
    const uint32_t shm_cfg   = kGcBase1 + 0x09E4u;   // SH_MEM_CONFIG
    const uint32_t shm_bases = kGcBase1 + 0x09E3u;   // SH_MEM_BASES
    if (write) {
        g_dev->writes.push_back(dword);
        if (dword == gfx_cntl) {
            g_dev->vmid = (*value >> 4) & 0xFu;
        } else if (dword == shm_cfg) {
            g_dev->shm_config[g_dev->vmid] = *value;
        } else if (dword == shm_bases) {
            g_dev->shm_bases[g_dev->vmid] = *value;
        }
        g_dev->bar5[dword] = *value;
        return true;
    }
    g_dev->reads.push_back(dword);
    for (RegSeq &s : g_dev->seqs) {
        if (s.addr == dword) {
            *value = s.vals[s.i < s.n ? s.i : s.n - 1u];
            if (s.i + 1u < s.n)
                s.i++;
            return true;
        }
    }
    for (uint32_t a : g_dev->fail_reads)
        if (a == dword)
            return false;
    if (dword == gfx_cntl) {
        *value = g_dev->vmid << 4;
        return true;
    }
    if (dword == shm_cfg) {
        *value = g_dev->shm_config[g_dev->vmid];
        return true;
    }
    if (dword == shm_bases) {
        *value = g_dev->shm_bases[g_dev->vmid];
        return true;
    }
    *value = g_dev->bar5[dword];
    return true;
}

static bool io_smuio(void *, uint32_t, uint32_t *, bool) { return false; }
static bool io_bus(void *, uint32_t, uint64_t, uint64_t *, bool, uint32_t) { return false; }
static void io_udelay(void *, uint32_t us) { g_dev->now += us; }
static void io_mdelay(void *, uint32_t ms) { g_dev->now += (uint64_t)ms * 1000u; }
static uint64_t io_now_us(void *) { return g_dev->now; }
static void io_lock(void *) {}
static void io_unlock(void *) {}

// =========================================================================================
// fixture
// =========================================================================================
struct PropSink {
    char key[64];
    char value[32];
};

struct Fixture {
    Fixture() { memset(this, 0, sizeof *this); }
    R3Rt rt;
    R3EvidenceLines ev;
    R3SubstepEnv env;
    R3Io io;
    amdgpu::IPBaseTable ip;
    R3EvSink sink;
    PropSink props[4];
    int prop_count;
};

static bool prop_set(void *user, const char *key, const char *bytes, size_t len)
{
    Fixture *f = static_cast<Fixture *>(user);
    if (!f || f->prop_count >= 4 || !key || !bytes)
        return false;
    PropSink &p = f->props[f->prop_count++];
    size_t kn = strlen(key) < sizeof p.key - 1 ? strlen(key) : sizeof p.key - 1;
    memcpy(p.key, key, kn);
    p.key[kn] = '\0';
    size_t vn = len < sizeof p.value - 1 ? len : sizeof p.value - 1;
    memcpy(p.value, bytes, vn);
    p.value[vn] = '\0';
    return true;
}

static bool build_fixture(Fixture &f, Dev &d)
{
    (void)d;
    memset(&f.rt, 0, sizeof f.rt);
    f.rt.ready_bits = R3_IP_BIT(R3Ip::Psp) | R3_IP_BIT(R3Ip::Smu);
    n48log_host_reset();
    r3_ev_init(&f.ev);
    memset(&f.env, 0, sizeof f.env);
    f.env.rt = &f.rt;
    f.env.ev = &f.ev;
    memset(&f.ip, 0, sizeof f.ip);
    f.ip.setBase(amdgpu::IPBlock::GC, 0, kGcBase0);
    f.ip.setBase(amdgpu::IPBlock::GC, 1, kGcBase1);
    f.env.ip = &f.ip;
    f.sink.set_property = prop_set;
    f.sink.user = &f;
    f.env.sink = &f.sink;
    f.env.strict_expect = true;
    f.prop_count = 0;
    memset(&f.io, 0, sizeof f.io);
    f.io.ctx = &d;
    f.io.mmio = io_mmio;
    f.io.smuio = io_smuio;
    f.io.bus = io_bus;
    f.io.udelay = io_udelay;
    f.io.mdelay = io_mdelay;
    f.io.now_us = io_now_us;
    f.io.lock = io_lock;
    f.io.unlock = io_unlock;
    return true;
}

// The nullarc E3d readings. Records are written once; CP_STAT/Bootload are polled.
static void healthy(Dev &d)
{
    d.set(kGcBase1, amdgpu::kGfx11ImuScratch0, 0x0e000000u);      // IMU fw version
    d.set(kGcBase1, amdgpu::kGfx11ImuGfxResetCtrl, 0x0000001fu);  // all reset domains out
    d.set(kGcBase1, amdgpu::kGfx11RlcStat, 0x25u);
    d.set(kGcBase1, amdgpu::kGfx11RlcGpmStat, 0x00b40017u);
    d.set(kGcBase1, amdgpu::kGfx11CpMeCntl, 0x15000000u);          // CE|PFP|ME halted
    d.set(kGcBase0, amdgpu::kGfx11GrbmStatus, 0x302cu);
    d.set(kGcBase0, amdgpu::kGfx11GrbmStatus2, 0x0400000cu);
    d.set(kGcBase0, amdgpu::kGfx11CpStat, 0x0u);
    d.set(kGcBase1, amdgpu::kGfx11RlcRlcsBootloadStatus,
          amdgpu::kGfx11BootloadComplete);
}

struct RunResult {
    R3SubstepResult r;
    bool ready_bit;
    uint64_t deny_before, deny_after;
    uint64_t t0, t1;
};

static RunResult run(Fixture &f, Dev &d)
{
    g_dev = &d;
    n48log_host_reset();
    r3_ev_init(&f.ev);
    f.prop_count = 0;
    r3_writeguard_install_mm();   // arms the MM windows; the substep declares its own rows
    RunResult out;
    out.t0 = d.now;
    out.deny_before = r3_writeguard_deny_count();
    out.r = amdgpu::gfx_v11_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    out.t1 = d.now;
    out.ready_bit = (f.rt.ready_bits & R3_IP_BIT(R3Ip::Gfx)) != 0u;
    return out;
}

// The only writes the substep may issue are the stage-2 declaration rows: four W1.5 rows and
// G01-G10 golden rows, each at its own BASE_IDX.
static bool only_declared_writes(const Dev &d)
{
    for (uint32_t a : d.writes) {
        if (a != kGcBase0 + 0x0DA0u && a != kGcBase1 + 0x0900u &&
            a != kGcBase1 + 0x09E4u && a != kGcBase1 + 0x09E3u &&
            a != kGcBase1 + 0x19A2u && a != kGcBase1 + 0x5087u &&
            a != kGcBase1 + 0x5086u && a != kGcBase0 + 0x1F53u &&
            a != kGcBase1 + 0x2E0Cu && a != kGcBase0 + 0x1025u &&
            a != kGcBase1 + 0x0944u && a != kGcBase1 + 0x0960u &&
            a != kGcBase0 + 0x12E2u && a != kGcBase1 + 0x19A3u)
            return false;
    }
    return true;
}

static uint32_t write_count(const Dev &d, uint32_t addr)
{
    uint32_t n = 0;
    for (uint32_t a : d.writes)
        if (a == addr)
            n++;
    return n;
}

static uint32_t read_count(const Dev &d, uint32_t addr)
{
    uint32_t n = 0;
    for (uint32_t a : d.reads)
        if (a == addr)
            n++;
    return n;
}

static bool ev_has(const Fixture &f, const char *needle)
{
    (void)f;
    return strstr(n48log_host_text(), needle) != nullptr;
}

static bool prop_is(const Fixture &f, const char *key, const char *value)
{
    for (int i = 0; i < f.prop_count; i++)
        if (strcmp(f.props[i].key, key) == 0 && strcmp(f.props[i].value, value) == 0)
            return true;
    return false;
}

// =========================================================================================
// golden: offsets and masks, re-transcribed from the pinned headers
// =========================================================================================
static void test_offsets()
{
    // gc_11_0_0_offset.h values, plus the gfx_v11_0.c:62 GC 11.0.1 override.
    CHECK(amdgpu::kGfx11ImuScratch0 == 0x4068u, "IMU_SCRATCH_0");
    CHECK(amdgpu::kGfx11ImuGfxResetCtrl == 0x40bcu, "IMU_GFX_RESET_CTRL");
    CHECK(amdgpu::kGfx11ImuCoreCtrl == 0x40b6u, "IMU_CORE_CTRL");
    CHECK(amdgpu::kGfx11ImuScratch10 == 0x4072u, "IMU_SCRATCH_10");
    CHECK(amdgpu::kGfx11RlcStat == 0x4c04u, "RLC_STAT");
    CHECK(amdgpu::kGfx11RlcGpmStat == 0x4e6bu, "RLC_GPM_STAT");
    CHECK(amdgpu::kGfx11RlcF32UcodeVersion == 0x4c03u, "RLC_F32_UCODE_VERSION");
    CHECK(amdgpu::kGfx11RlcRlcsBootloadStatus == 0x4e7eu,
          "RLC_RLCS_BOOTLOAD_STATUS gc_11_0_1 override");
    CHECK(amdgpu::kGfx11CpMeCntl == 0x0803u, "CP_ME_CNTL");
    CHECK(amdgpu::kGfx11GrbmStatus == 0x0da4u, "GRBM_STATUS");
    CHECK(amdgpu::kGfx11GrbmStatus2 == 0x0da2u, "GRBM_STATUS2");
    CHECK(amdgpu::kGfx11CpStat == 0x0f40u, "CP_STAT");

    // BASE_IDX: IMU/RLC/CP_ME_CNTL/BOOTLOAD are segment 1; GRBM/CP_STAT segment 0.
    CHECK(amdgpu::kGfx11ImuScratch0Base == 1 && amdgpu::kGfx11ImuGfxResetCtrlBase == 1,
          "IMU base idx");
    CHECK(amdgpu::kGfx11RlcStatBase == 1 && amdgpu::kGfx11RlcGpmStatBase == 1,
          "RLC base idx");
    CHECK(amdgpu::kGfx11RlcRlcsBootloadStatusBase == 1, "BOOTLOAD base idx");
    CHECK(amdgpu::kGfx11CpMeCntlBase == 1, "CP_ME_CNTL base idx");
    CHECK(amdgpu::kGfx11GrbmStatusBase == 0 && amdgpu::kGfx11GrbmStatus2Base == 0,
          "GRBM base idx");
    CHECK(amdgpu::kGfx11CpStatBase == 0, "CP_STAT base idx");

    // gc_11_0_0_sh_mask.h values.
    CHECK(amdgpu::kGfx11BootloadComplete == 0x80000000u, "BOOTLOAD_COMPLETE bit31");
    CHECK(amdgpu::kGfx11CpStatCeBusy == 0x04000000u, "CP_STAT.CE_BUSY bit26");
    CHECK(amdgpu::kGfx11CpStatPfpBusy == 0x00008000u, "CP_STAT.PFP_BUSY");
    CHECK(amdgpu::kGfx11CpStatMeBusy == 0x00020000u, "CP_STAT.ME_BUSY");
    CHECK(amdgpu::kGfx11CpStatCpBusy == 0x80000000u, "CP_STAT.CP_BUSY");
    CHECK(amdgpu::kGfx11CpMeCntlCeHalt == 0x01000000u, "CP_ME_CNTL.CE_HALT");
    CHECK(amdgpu::kGfx11CpMeCntlPfpHalt == 0x04000000u, "CP_ME_CNTL.PFP_HALT");
    CHECK(amdgpu::kGfx11CpMeCntlMeHalt == 0x10000000u, "CP_ME_CNTL.ME_HALT");
    CHECK(amdgpu::kGfx11ImuResetDoneMask == 0x1fu && amdgpu::kGfx11ImuResetDone == 0x1fu,
          "IMU reset done = low 5 bits 0x1f");
    CHECK(amdgpu::kGfx11AutoloadTimeoutUs >= 100000u, "autoload budget >= 100 ms");
}

// =========================================================================================
// S1-S3 happy path
// =========================================================================================
static void test_happy_path()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    RunResult rr = run(f, d);

    CHECK(rr.r.status == kR3SsecOk, "happy path succeeded: %s", rr.r.name);
    CHECK(rr.ready_bit, "GFX ready bit set on success");
    CHECK(d.bar5[kGcBase0 + 0x0DA0u] == 0xFFu, "GRBM_CNTL.READ_TIMEOUT=0xff");
    CHECK(d.bar5[kGcBase1 + 0x09E4u] == 0x0000C00Cu, "SH_MEM_CONFIG=0xC00C");
    CHECK(d.bar5[kGcBase1 + 0x09E3u] == 0x20001000u, "SH_MEM_BASES=0x20001000");
    CHECK(only_declared_writes(d), "only declared rows written (%zu)", d.writes.size());
    CHECK(d.writes.size() == 50u, "stage-2 writes: 40 W1.5 + 10 golden (got %zu)",
          d.writes.size());
    CHECK(d.bar5[kGcBase0 + 0x13DEu] == 0u, "G16 GB_ADDR_CONFIG never written");
    for (int v = 8; v < 16; v++) {
        CHECK(d.shm_config[v] == 0u && d.shm_bases[v] == 0u,
              "VMID%d left to init_compute_vmid (config=0x%08x bases=0x%08x)", v,
              d.shm_config[v], d.shm_bases[v]);
    }
    CHECK(rr.deny_after == rr.deny_before, "no whitelist DENY (%llu)",
          (unsigned long long)(rr.deny_after - rr.deny_before));

    // Each record register is read once; CP_STAT is polled until it is idle.
    uint32_t rlc_reads = 0, grbm_reads = 0, cpstat_reads = 0, boot_reads = 0;
    for (uint32_t a : d.reads) {
        if (a == kGcBase1 + amdgpu::kGfx11RlcStat) rlc_reads++;
        if (a == kGcBase0 + amdgpu::kGfx11GrbmStatus) grbm_reads++;
        if (a == kGcBase0 + amdgpu::kGfx11CpStat) cpstat_reads++;
        if (a == kGcBase1 + amdgpu::kGfx11RlcRlcsBootloadStatus) boot_reads++;
    }
    CHECK(rlc_reads == 1u, "RLC_STAT recorded once, got %u", rlc_reads);
    CHECK(grbm_reads == 1u, "GRBM_STATUS recorded once, got %u", grbm_reads);
    CHECK(cpstat_reads == 1u, "CP_STAT polled once (already idle), got %u", cpstat_reads);
    CHECK(boot_reads == 1u, "BOOTLOAD polled once (already complete), got %u", boot_reads);
    CHECK(rr.t1 == rr.t0, "no delay when readiness is immediate");

    // Evidence: each section left a line; the done marker is Yes.
    CHECK(ev_has(f, "S1"), "S1 evidence line");
    CHECK(ev_has(f, "S2"), "S2 evidence line");
    CHECK(ev_has(f, "S3"), "S3 evidence line");
    CHECK(ev_has(f, "imu"), "IMU evidence");
    CHECK(ev_has(f, "rlc"), "RLC evidence");
    CHECK(ev_has(f, "cp"), "CP evidence");
    CHECK(prop_is(f, "N48R3EVID-gfx-done", "Yes"), "gfx-done property Yes");
}

// =========================================================================================
// failure branches
// =========================================================================================
static void test_all_zero()
{
    Fixture f;
    Dev d;   // every GC register reads 0: the nullarc E2 "not powered" form
    CHECK(build_fixture(f, d), "fixture");
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail, "all-zero must fail");
    CHECK(strcmp(rr.r.name, "chk:imu-not-running") == 0, "all-zero reason: %s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 1u, "all-zero section/step");
    CHECK(!rr.ready_bit, "no ready bit on failure");
    CHECK(only_declared_writes(d), "no undeclared writes on failure");
    CHECK(prop_is(f, "N48R3EVID-gfx-done", "No"), "gfx-done property No");
}

static void test_imu_read_failure()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.fail_reads.push_back(kGcBase1 + amdgpu::kGfx11ImuScratch0);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "chk:imu-unreadable") == 0,
          "IMU read failure: %s", rr.r.name);
    CHECK(only_declared_writes(d), "no undeclared writes");
}

static void test_records_not_gated()
{
    // C-06/G-01: RLC_STAT/RLC_GPM_STAT/GRBM_STATUS are comparison records only. Neither
    // 0xFFFFFFFF nor a changing value may fail the substep.
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.set(kGcBase1, amdgpu::kGfx11RlcStat, 0xFFFFFFFFu);
    d.set(kGcBase1, amdgpu::kGfx11RlcGpmStat, 0xFFFFFFFFu);
    d.set(kGcBase0, amdgpu::kGfx11GrbmStatus, 0xFFFFFFFFu);
    d.set(kGcBase0, amdgpu::kGfx11GrbmStatus2, 0xFFFFFFFFu);
    d.seq2(kGcBase1, amdgpu::kGfx11RlcStat, 0x25u, 0x26u);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "record-only registers must not gate: %s", rr.r.name);
    CHECK(ev_has(f, "rlc_stat=0x00000025"), "RLC_STAT recorded");
}

static void test_bootload_absent()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.set(kGcBase1, amdgpu::kGfx11RlcRlcsBootloadStatus, 0u);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "chk:autoload-timeout") == 0,
          "missing BOOTLOAD_COMPLETE must time out: %s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 2u, "autoload section/step");
    CHECK(rr.t1 - rr.t0 >= amdgpu::kGfx11AutoloadTimeoutUs,
          "waited the full budget (%llu us)", (unsigned long long)(rr.t1 - rr.t0));
    CHECK(!rr.ready_bit, "no ready bit");
    CHECK(only_declared_writes(d), "no undeclared writes");
}

static void test_bootload_unreadable()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.fail_reads.push_back(kGcBase1 + amdgpu::kGfx11RlcRlcsBootloadStatus);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "chk:autoload-unreadable") == 0,
          "unreadable BOOTLOAD: %s", rr.r.name);
}

static void test_bootload_late()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.seqs.clear();
    d.seq2(kGcBase1, amdgpu::kGfx11RlcRlcsBootloadStatus, 0u,
           amdgpu::kGfx11BootloadComplete);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "late BOOTLOAD_COMPLETE must pass: %s", rr.r.name);
    CHECK(rr.t1 - rr.t0 >= amdgpu::kGfx11AutoloadStepUs, "one poll interval elapsed");
}

static void test_cp_busy_stuck()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.set(kGcBase0, amdgpu::kGfx11CpStat, 0x80008000u);  // nullarc E4b failure form
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "chk:autoload-timeout") == 0,
          "CP_STAT busy must time out: %s", rr.r.name);
    CHECK(rr.r.section == kR3SecChk && rr.r.step == 2u, "CP busy section/step");
    CHECK(rr.t1 - rr.t0 >= amdgpu::kGfx11AutoloadTimeoutUs,
          "waited the full budget (%llu us)", (unsigned long long)(rr.t1 - rr.t0));
}

static void test_cp_busy_transient()
{
    // Linux retries until CP_STAT==0; a transient busy reading is not a failure.
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.seq2(kGcBase0, amdgpu::kGfx11CpStat, 0x80008000u, 0u);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "transient busy must pass: %s", rr.r.name);
}

static void test_imu_reset_witness()
{
    // The PSP path does not call imu_v11_0_start; a zero version register is acceptable if
    // the reset-status witness (low 5 bits 0x1f) says the IMU released the GFX resets.
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    d.set(kGcBase1, amdgpu::kGfx11ImuScratch0, 0u);
    d.set(kGcBase1, amdgpu::kGfx11ImuGfxResetCtrl, 0x0000001fu);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "reset-status witness must pass: %s", rr.r.name);
}

static void test_preconditions()
{
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        f.io.mmio = nullptr;
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:no-io") == 0,
              "incomplete I/O: %s", rr.r.name);
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        f.rt.ready_bits &= ~R3_IP_BIT(R3Ip::Psp);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:psp-not-ready") == 0,
              "PSP not ready: %s", rr.r.name);
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        f.rt.ready_bits &= ~R3_IP_BIT(R3Ip::Smu);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:smu-not-ready") == 0,
              "SMU not ready: %s", rr.r.name);
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        f.ip.setBase(amdgpu::IPBlock::GC, 1, 0xFFFFFFFFu);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:gc-base") == 0,
              "missing GC base: %s", rr.r.name);
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        f.env.rt = nullptr;
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail && strcmp(rr.r.name, "pre:no-env") == 0,
              "no runtime: %s", rr.r.name);
    }
}

// =========================================================================================
// W1.5: read-back gate (补遗-2), snapshot (补遗-3) and the declared grants
// =========================================================================================
static void test_shmem_idempotent()
{
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.set_shmem_all(0x0000C00Cu, 0x20001000u);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecOk, "idempotent SH_MEM run: %s", rr.r.name);
        uint32_t cfg_w = 0, bases_w = 0;
        for (uint32_t a : d.writes) {
            if (a == kGcBase1 + 0x09E4u) cfg_w++;
            if (a == kGcBase1 + 0x09E3u) bases_w++;
        }
        CHECK(cfg_w == 0u, "equal SH_MEM_CONFIG not rewritten (got %u)", cfg_w);
        CHECK(bases_w == 0u, "equal SH_MEM_BASES not rewritten (got %u)", bases_w);
        CHECK(ev_has(f, "S0c shmem vmid15"), "补遗-3 readback snapshot logged");
        CHECK(ev_has(f, "readback, observe only"), "snapshot marked observation-only");
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.set_shmem_all(0x0000C00Cu, 0x0000C00Cu);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecOk, "mismatched bases run: %s", rr.r.name);
        uint32_t bases_w = 0;
        for (uint32_t a : d.writes)
            if (a == kGcBase1 + 0x09E3u) bases_w++;
        CHECK(bases_w == 7u, "VMID1..7 bases rewritten, got %u", bases_w);
        CHECK(d.bar5[kGcBase1 + 0x09E3u] == 0x20001000u, "bases corrected to 0x20001000");
        for (int v = 8; v < 16; v++)
            CHECK(d.shm_bases[v] == 0x0000C00Cu, "VMID%d untouched (0x%08x)", v,
                  d.shm_bases[v]);
    }
}

static void test_grants()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    CHECK(run(f, d).r.status == kR3SsecOk, "grant run succeeded");
    uint32_t td = 0, w15 = 0, golden = 0, bad = 0;
    for (uint32_t i = 0; i < r3_wg_grant_count(); i++) {
        const R3WgGrant *gr = r3_wg_grant(i);
        if (!gr)
            continue;
        if (gr->owner != WSubstep::Gfx || gr->cls != WClass::State) {
            bad++;
            continue;
        }
        if (strcmp(gr->tag, "reg:gfx-teardown") == 0)
            td++;
        else if (strncmp(gr->tag, "reg:gfx-w1.5-", 13) == 0)
            w15++;
        else if (strcmp(gr->tag, "reg:gfx-golden") == 0)
            golden++;
    }
    CHECK(td == 2u, "two teardown halt grants, got %u", td);
    CHECK(w15 == 4u, "four W1.5 grants, got %u", w15);
    CHECK(golden == 10u, "ten golden grants, got %u", golden);
    CHECK(r3_wg_grant_count() == 16u, "stage-2 grant count 16, got %u",
          r3_wg_grant_count());
    CHECK(bad == 0u, "no foreign grants in the Gfx run (%u)", bad);
}

// =========================================================================================
// G01-G10 golden writes, G16 read-only, B#25 CE_BUSY, stage-3 (deferred)
// =========================================================================================
static void test_golden_unconditional()
{
    // Linux applies the golden tables through an unconditional RMW. Preset every register to
    // all-ones: a read-back-skipping implementation would leave 0xFFFFFFFF in place.
    static const struct {
        int base;
        uint32_t off, mask, value;
    } kGolden[10] = {
        {1, 0x19A2u, 0x20000000u, 0x20000000u},
        {1, 0x5087u, 0x9fff8fffu, 0x00000010u},
        {1, 0x5086u, 0xffff8fffu, 0x00000010u},
        {0, 0x1F53u, 0x0007ffffu, 0x0000c200u},
        {1, 0x2E0Cu, 0xffff001bu, 0x00f01988u},
        {0, 0x1025u, 0xf0ffffffu, 0x00880007u},
        {1, 0x0944u, 0xfffffffdU, 0x00000008u},
        {1, 0x0960u, 0xfff891ffu, 0x55480100u},
        {0, 0x12E2u, 0xf7f7ffffu, 0x01030000u},
        {1, 0x19A3u, 0xfcffffffu, 0x0000000au},
    };
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    for (int i = 0; i < 10; i++)
        d.set(kGolden[i].base == 0 ? kGcBase0 : kGcBase1, kGolden[i].off, 0xFFFFFFFFu);
    RunResult rr = run(f, d);
    CHECK(rr.r.status == kR3SsecOk, "golden run succeeded: %s", rr.r.name);
    for (int i = 0; i < 10; i++) {
        const uint32_t addr =
            (kGolden[i].base == 0 ? kGcBase0 : kGcBase1) + kGolden[i].off;
        const uint32_t want = (0xFFFFFFFFu & ~kGolden[i].mask) | (kGolden[i].value & kGolden[i].mask);
        CHECK(d.bar5[addr] == want, "golden 0x%08x = 0x%08x (got 0x%08x)", addr, want,
              d.bar5[addr]);
        CHECK(write_count(d, addr) == 1u, "golden 0x%08x written once (got %u)", addr,
              write_count(d, addr));
    }
    CHECK(ev_has(f, "S0c golden G01-G10 applied"), "golden evidence line");
}

static void test_gb_addr_read_only()
{
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.set(kGcBase0, 0x13DEu, 0x12345678u);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecOk, "G16 read run: %s", rr.r.name);
        CHECK(d.bar5[kGcBase0 + 0x13DEu] == 0x12345678u, "GB_ADDR_CONFIG unchanged");
        CHECK(write_count(d, kGcBase0 + 0x13DEu) == 0u, "GB_ADDR_CONFIG never written");
        CHECK(read_count(d, kGcBase0 + 0x13DEu) == 1u, "GB_ADDR_CONFIG read once");
        CHECK(ev_has(f, "G16 GB_ADDR_CONFIG=0x12345678"), "G16 evidence line");
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.fail_reads.push_back(kGcBase0 + 0x13DEu);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecOk, "unreadable GB_ADDR_CONFIG is not fatal: %s",
              rr.r.name);
        CHECK(ev_has(f, "G16 GB_ADDR_CONFIG unreadable (recorded, not fatal)"),
              "G16 unreadable evidence line");
        CHECK(only_declared_writes(d), "no undeclared writes when G16 is unreadable");
    }
}

static void test_ce_busy_bit()
{
    // B#25: CE_BUSY is bit26 (0x04000000). 0x01000000 must decode as not-CE-busy.
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.set(kGcBase0, amdgpu::kGfx11CpStat, 0x04000000u);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail, "nonzero CP_STAT cannot be ready");
        CHECK(ev_has(f, "busy(ce=1 pfp=0 me=0 cp=0)"), "bit26 decodes as CE_BUSY");
    }
    {
        Fixture f;
        Dev d;
        CHECK(build_fixture(f, d), "fixture");
        healthy(d);
        d.set(kGcBase0, amdgpu::kGfx11CpStat, 0x01000000u);
        RunResult rr = run(f, d);
        CHECK(rr.r.status == kR3SsecFail, "nonzero CP_STAT cannot be ready");
        CHECK(ev_has(f, "busy(ce=0 pfp=0 me=0 cp=0)"), "0x01000000 is not CE_BUSY");
    }
}

static void test_stage3_deferred()
{
    Fixture f;
    Dev d;
    CHECK(build_fixture(f, d), "fixture");
    healthy(d);
    CHECK(run(f, d).r.status == kR3SsecOk, "stage-2 precondition run");
    d.writes.clear();
    d.reads.clear();
    d.set(kGcBase0, 0x1F72u, 0x10u);   // pre-set unrelated TRAP_EN bits
    d.set(kGcBase1, 0x4C80u, 0x10u);   // pre-set unrelated RLC_SRM_CNTL bits

    const uint64_t deny_before = r3_writeguard_deny_count();
    R3SubstepResult r = amdgpu::gfx_v11_0_stage3_substep(&f.io, &f.env);
    const uint64_t deny_after = r3_writeguard_deny_count();
    CHECK(r.status == kR3SsecOk, "stage-3 succeeded: %s", r.name);
    CHECK(deny_after == deny_before, "stage-3 caused no DENY (%llu)",
          (unsigned long long)(deny_after - deny_before));

    // G13 8 VMID x 8 writes + one final selector, G14 15 x 4, G17 one.
    CHECK(d.writes.size() == 126u, "stage-3 write count 126 (got %zu)", d.writes.size());
    CHECK(d.reads.size() == 9u, "stage-3 read count 9 (got %zu)", d.reads.size());
    CHECK(write_count(d, kGcBase1 + 0x0900u) == 9u, "selector writes");
    CHECK(write_count(d, kGcBase1 + 0x09E4u) == 8u, "G13 SH_MEM_CONFIG writes");
    CHECK(write_count(d, kGcBase1 + 0x09E3u) == 8u, "G13 SH_MEM_BASES writes");
    CHECK(write_count(d, kGcBase0 + 0x1F72u) == 8u, "G13 TRAP_EN writes");
    CHECK(write_count(d, kGcBase1 + 0x4C80u) == 1u, "G17 RLC_SRM_CNTL write");
    uint32_t gds_w = 0;
    for (uint32_t i = 2u; i <= 30u; i += 2u) {
        if (write_count(d, kGcBase0 + 0x20A0u + i) > 0u) gds_w++;
        if (write_count(d, kGcBase0 + 0x20A1u + i) > 0u) gds_w++;
    }
    for (uint32_t i = 1u; i <= 15u; i++) {
        if (write_count(d, kGcBase0 + 0x20C0u + i) > 0u) gds_w++;
        if (write_count(d, kGcBase0 + 0x20D0u + i) > 0u) gds_w++;
    }
    CHECK(gds_w == 60u, "all 60 GDS row/offset combinations written (got %u)", gds_w);
    CHECK(d.bar5[kGcBase0 + 0x1F72u] == 0x18u, "TRAP_EN ORed (0x10 -> 0x18)");
    CHECK(d.bar5[kGcBase1 + 0x4C80u] == 0x13u, "RLC_SRM_CNTL ORed (0x10 -> 0x13)");
    CHECK(d.bar5[kGcBase1 + 0x0900u] == 0u, "selector restored to VMID0");
    CHECK(d.bar5[kGcBase1 + 0x09E3u] == 0x00010002u, "compute bases 0x00010002");
    CHECK(d.shm_bases[8] == 0x00010002u, "VMID8 compute bases applied");
    CHECK(d.shm_config[8] == 0x0000C00Cu, "VMID8 config 0xC00C");

    uint32_t s3 = 0;
    for (uint32_t i = 0; i < r3_wg_grant_count(); i++) {
        const R3WgGrant *gr = r3_wg_grant(i);
        if (gr && strcmp(gr->tag, "reg:gfx-stage3") == 0)
            s3++;
    }
    CHECK(s3 == 62u, "62 stage-3 grants (60 GDS instances + SPI + RLC), got %u", s3);
    CHECK(r3_wg_grant_count() == 78u, "stage-2 + stage-3 grants 78, got %u",
          r3_wg_grant_count());
    CHECK(ev_has(f, "G13 compute vmid8..15"), "G13 evidence");
    CHECK(ev_has(f, "G14 GDS vmid1..15"), "G14 evidence");
    CHECK(ev_has(f, "G17 RLC_SRM_CNTL"), "G17 evidence");
}

int main()
{
    printf("test_gfx_status: offsets + S1-S3 state machine\n");
    test_offsets();
    test_happy_path();
    test_all_zero();
    test_imu_read_failure();
    test_records_not_gated();
    test_bootload_absent();
    test_bootload_unreadable();
    test_bootload_late();
    test_cp_busy_stuck();
    test_cp_busy_transient();
    test_imu_reset_witness();
    test_preconditions();
    test_shmem_idempotent();
    test_grants();
    test_golden_unconditional();
    test_gb_addr_read_only();
    test_ce_busy_bit();
    test_stage3_deferred();
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
