/*
 * test_gfx_status.cpp - host tests for the GFX 11.0.1 read-only readiness substep
 * (K6 / requirement C3.1-C3.3, tech plan §3.3 + §8.1 + §10).
 *
 * Everything runs on a RAM-backed fake of BAR5: the substep issues no write, so the fake
 * only has to answer dword reads and prove that no write ever reaches it. Golden values
 * come from independent sources, never from the code under test (H4):
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
    std::vector<uint32_t> writes;       // BAR5 dwords written, in order (must stay empty)
    std::vector<RegSeq>   seqs;         // per-address read sequences
    std::vector<uint32_t> fail_reads;   // addresses whose read returns false
    uint64_t now;

    Dev()
    {
        bar5.assign(kBar5Dwords, 0);
        now = 1000;
    }

    void set(uint32_t base, uint32_t off, uint32_t v) { bar5[base + off] = v; }

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
    if (write) {
        g_dev->writes.push_back(dword);
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
    RunResult out;
    out.t0 = d.now;
    out.deny_before = r3_writeguard_deny_count();
    out.r = amdgpu::gfx_v11_0_substep(&f.io, &f.env);
    out.deny_after = r3_writeguard_deny_count();
    out.t1 = d.now;
    out.ready_bit = (f.rt.ready_bits & R3_IP_BIT(R3Ip::Gfx)) != 0u;
    return out;
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
    CHECK(amdgpu::kGfx11CpStatCeBusy == 0x01000000u, "CP_STAT.CE_BUSY");
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
    CHECK(d.writes.empty(), "GFX substep issued %zu writes (must be 0)", d.writes.size());
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
    CHECK(d.writes.empty(), "no writes on failure");
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
    CHECK(d.writes.empty(), "no writes");
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
    CHECK(d.writes.empty(), "no writes");
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
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
