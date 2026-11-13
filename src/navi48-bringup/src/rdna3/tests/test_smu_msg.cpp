/*
 * test_smu_msg.cpp - host tests for the SMU 13.0.4 substep (K5 / T14 + the D11 state
 * machine, tech plan §3.2/§10).
 *
 * Everything runs on a RAM-backed fake of BAR5 and the SMN indirect pair. The fake models
 * the SMU mailbox protocol (clear response -> argument -> message -> response), the
 * firmware-flags register and the GC/SDMA effect registers, so every branch - success,
 * async-success, both recheck outcomes, a stuck-zero C2PMSG_90, the explicit error codes,
 * the 0xFFFFFFFF read path and the precondition refusals - executes on Linux with no GPU.
 *
 * Golden values are independent of the implementation (H4): the offsets and response
 * codes below are transcribed from mp_13_0_4_offset.h / gc_11_0_0_offset.h /
 * smu_v13_0_4_ppsmc.h / smu_cmn.c, not read back from smu_v13_0.h.
 *
 * Planted breaks (H3: the failure is recorded before the fix) are listed in
 * tmp/programmer/K5/run_smu_planted_breaks.py; each mutation MUST make this suite fail:
 *   B1 recheck-twice        the R3_SUBSTEP_RECHECK_USED guard is removed.
 *   B2 stuck-zero-resend    the pre-poll timeout is ignored and 0x16 is resent anyway.
 *   B3 ffffffff-valid       valid(v) accepts 0xFFFFFFFF.
 *   B4 rlc-not-gated        the gate becomes GRBM_STATUS OR RLC_STAT.
 *   B5 one-sample           the gate needs one sample instead of two consecutive ones.
 */
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "amdgpu_ip.h"
#include "amd/smu_v13_0.h"
#include "rdna3/rdna3_runtime.h"
#include "rdna3/rdna3_substep.h"
#include "rdna3/rdna3_writeguard.h"
#include "rdna3/rdna3_wmanifest.h"
#include "rdna3/golden_values.h"
#include "n48log_stub.h"


static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// =========================================================================================
// The fake device
// =========================================================================================
static const uint32_t kBar5Dwords = 0x100000u;  // 4 MiB of BAR5
static const uint32_t kMp1Base1   = 0x16000u;   // MP1 BASE_IDX 1 (R3 §2.1: mailbox at 0x16282)
static const uint32_t kGcBase0    = 0x50000u;   // GC BASE_IDX 0
static const uint32_t kGcBase1    = 0x30000u;   // GC BASE_IDX 1 (RLC_STAT)
static const uint32_t kSdma0Base0 = 0x20000u;   // SDMA0 BASE_IDX 0

// Independent transcriptions (not the header under test).
static const uint32_t kMp1C2pmsg66 = 0x0282u;
static const uint32_t kMp1C2pmsg82 = 0x0292u;
static const uint32_t kMp1C2pmsg90 = 0x029au;
static const uint32_t kGrbmStatus  = 0x0da4u;
static const uint32_t kGrbmStatus2 = 0x0da2u;
static const uint32_t kGbAddrConfig = 0x13deu;
static const uint32_t kRlcStat     = 0x4c04u;
static const uint32_t kSdma0Status = 0x0025u;

struct Scenario {
    // Message responses (0 = poll timeout).
    uint32_t driver_if_resp = 0x01;
    uint32_t driver_if_ret  = 8u;
    uint32_t pmfw_resp      = 0x01;
    uint32_t pmfw_ret       = 0x004C6500u;  // Linux decode: 76.101.0
    uint32_t feature_resp   = 0x01;
    uint32_t feature_lo     = 0xf3fff7fdu;
    uint32_t feature_hi     = 0x07a5f3f3u;

    // EnableGfxImu: [0] first send, [1] recheck send.
    uint32_t imu_resp[2]    = {0x01, 0x01};
    uint64_t imu_late_us[2] = {0u, 0u};     // for a timeout: when C2PMSG_90 finally changes
    uint32_t imu_late_code[2] = {0x01, 0x01};

    // Effect: 0 = never, 1 = after the first 0x16, 2 = after the recheck 0x16.
    uint32_t effect_after_send = 1u;
    uint64_t effect_delay_us   = 0u;
    bool     gc_fault          = false;  // GC regs read 0xFFFFFFFF
    bool     rlc_zero          = false;  // GRBM valid, RLC_STAT 0 (idle, healthy)
    bool     rlc_fault         = false;  // RLC_STAT reads 0xFFFFFFFF (read-path fault)
    bool     effect_pulse_once = false;  // valid on the first sample only

    uint32_t firmware_flags = 1u;
    bool     smuio_fail     = false;
    bool     psp_ready      = true;
    bool     mp1_resolved   = true;
    bool     gc_resolved    = true;
};

struct Dev {
    std::vector<uint32_t> bar5;
    std::vector<uint32_t> reads;      // BAR5 dwords read, in order
    std::vector<uint32_t> writes;     // BAR5 dwords written, in order
    std::vector<uint32_t> smu_reads;  // SMN indexes read, in order
    amdgpu::IPBaseTable   ip;
    Scenario              sc;
    uint64_t              clock_us;
    uint32_t              resp;
    uint32_t              arg;
    uint32_t              ret;
    uint32_t              imu_sends;
    bool                  effect_enabled;
    uint64_t              effect_at;
    bool                  late_pending;
    uint64_t              late_at;
    uint32_t              late_code;
    uint32_t              grbm_reads;
    uint32_t              lock_depth;
    R3Io                  io;

    Dev()
    {
        bar5.assign(kBar5Dwords, 0);
        io.ctx = this;
        io.mmio = [](void *c, uint32_t d, uint32_t *v, bool w) {
            return static_cast<Dev *>(c)->mmio(d, v, w);
        };
        io.smuio = [](void *c, uint32_t index, uint32_t *v, bool w) {
            return static_cast<Dev *>(c)->smuio(index, v, w);
        };
        io.bus = nullptr;
        io.udelay = [](void *c, uint32_t us) {
            static_cast<Dev *>(c)->clock_us += us;
        };
        io.mdelay = [](void *c, uint32_t ms) {
            static_cast<Dev *>(c)->clock_us += (uint64_t)ms * 1000u;
        };
        io.now_us = [](void *c) -> uint64_t { return static_cast<Dev *>(c)->clock_us; };
        io.lock = [](void *c) { static_cast<Dev *>(c)->lock_depth++; };
        io.unlock = [](void *c) {
            Dev *d = static_cast<Dev *>(c);
            if (d->lock_depth)
                d->lock_depth--;
        };
    }

    void reset(const Scenario &s)
    {
        sc = s;
        std::fill(bar5.begin(), bar5.end(), 0u);
        reads.clear();
        writes.clear();
        smu_reads.clear();
        clock_us = 0;
        resp = 0;
        arg = 0;
        ret = 0;
        imu_sends = 0;
        effect_enabled = false;
        effect_at = 0;
        late_pending = false;
        late_at = 0;
        late_code = 0;
        grbm_reads = 0;
        lock_depth = 0;
        ip = amdgpu::IPBaseTable();
        ip.setBase(amdgpu::IPBlock::MP1, 1, sc.mp1_resolved ? kMp1Base1 : 0xFFFFFFFFu);
        ip.setBase(amdgpu::IPBlock::GC, 0, sc.gc_resolved ? kGcBase0 : 0xFFFFFFFFu);
        ip.setBase(amdgpu::IPBlock::GC, 1, sc.gc_resolved ? kGcBase1 : 0xFFFFFFFFu);
        ip.setBase(amdgpu::IPBlock::SDMA0, 0, sc.gc_resolved ? kSdma0Base0 : 0xFFFFFFFFu);
    }

    uint32_t resp_value()
    {
        if (late_pending && clock_us >= late_at) {
            resp = late_code;
            late_pending = false;
        }
        return resp;
    }

    uint32_t gc_value(uint32_t which)
    {
        if (sc.gc_fault)
            return 0xFFFFFFFFu;
        if (!effect_enabled || clock_us < effect_at)
            return 0u;
        if (sc.effect_pulse_once && grbm_reads != 1u)
            return 0u;
        if (which == 1u && sc.rlc_fault) return 0xFFFFFFFFu;  // read-path fault (fail closed)
        if (which == 1u && sc.rlc_zero)  return 0u;            // idle, healthy (not gated)
        switch (which) {
        case 0: return 0x00000001u;  // GRBM_STATUS
        case 1: return 0x00000001u;  // RLC_STAT
        case 2: return 0x00000000u;  // GRBM_STATUS2 companion (not gated)
        case 3: return 0x00000242u;  // GB_ADDR_CONFIG companion (J-3 is NOT used)
        case 4: return 0x00000001u;  // SDMA0_STATUS_REG companion
        }
        return 0u;
    }

    void on_msg(uint32_t msg)
    {
        switch (msg) {
        case 0x03u:
            resp = sc.driver_if_resp;
            ret = sc.driver_if_ret;
            break;
        case 0x02u:
            resp = sc.pmfw_resp;
            ret = sc.pmfw_ret;
            break;
        case 0x12u:
            resp = sc.feature_resp;
            ret = (arg == 0u) ? sc.feature_lo : sc.feature_hi;
            break;
        case 0x16u: {
            const uint32_t idx = imu_sends++;
            if (idx < 2u) {
                resp = sc.imu_resp[idx];
                if (resp == 0u && sc.imu_late_us[idx] != 0u) {
                    late_pending = true;
                    late_at = clock_us + sc.imu_late_us[idx];
                    late_code = sc.imu_late_code[idx];
                }
                if (sc.effect_after_send == idx + 1u) {
                    effect_enabled = true;
                    effect_at = clock_us + sc.effect_delay_us;
                }
            } else {
                resp = 0xFDu;
            }
            break;
        }
        default:
            resp = 0xFDu;
            break;
        }
    }

    bool mmio(uint32_t d, uint32_t *v, bool write)
    {
        if (d >= kBar5Dwords)
            return false;
        if (write) {
            writes.push_back(d);
            const uint32_t value = *v;
            if (d == kMp1Base1 + kMp1C2pmsg90) {
                resp = value;
                late_pending = false;
            } else if (d == kMp1Base1 + kMp1C2pmsg82) {
                arg = value;
            } else if (d == kMp1Base1 + kMp1C2pmsg66) {
                on_msg(value);
            } else {
                bar5[d] = value;
            }
            return true;
        }
        reads.push_back(d);
        if (d == kMp1Base1 + kMp1C2pmsg90) {
            *v = resp_value();
            return true;
        }
        if (d == kMp1Base1 + kMp1C2pmsg82) {
            *v = ret;
            return true;
        }
        if (d == kMp1Base1 + kMp1C2pmsg66) {
            *v = 0u;
            return true;
        }
        if (d == kGcBase0 + kGrbmStatus) {
            grbm_reads++;
            *v = gc_value(0u);
            return true;
        }
        if (d == kGcBase1 + kRlcStat) {
            *v = gc_value(1u);
            return true;
        }
        if (d == kGcBase0 + kGrbmStatus2) {
            *v = gc_value(2u);
            return true;
        }
        if (d == kGcBase0 + kGbAddrConfig) {
            *v = gc_value(3u);
            return true;
        }
        if (d == kSdma0Base0 + kSdma0Status) {
            *v = gc_value(4u);
            return true;
        }
        *v = bar5[d];
        return true;
    }

    bool smuio(uint32_t index, uint32_t *v, bool write)
    {
        if (write)
            return false;
        smu_reads.push_back(index);
        if (index != 0x03b10028u || sc.smuio_fail)
            return false;
        *v = sc.firmware_flags;
        return true;
    }

    bool wrote(uint32_t d) const
    {
        return std::find(writes.begin(), writes.end(), d) != writes.end();
    }
    bool read(uint32_t d) const
    {
        return std::find(reads.begin(), reads.end(), d) != reads.end();
    }
};

static Dev g_dev;
static R3Rt g_rt;
static R3EvidenceLines g_ev;

static std::string evidence_text()
{
    return std::string(n48log_host_text());
}

static bool evidence_has(const char *needle)
{
    return evidence_text().find(needle) != std::string::npos;
}

/* Step2: register SMU rows from the declaration table. Replaces manual C2PMSG66/82/90. */
static void register_smu_rows(uint32_t mp1_base)
{
    const uint32_t rc = amdgpu::getSmuRowCount();
    const R3WriteRowReg *rows = amdgpu::getSmuRows();

    for (uint32_t i = 0; i < rc; i++) {
        const auto *g = static_cast<const r3gold::GoldenReg *>(rows[i].golden);
        const uint32_t addr = mp1_base + g->offset;
        r3_wg_register_reg(addr, 1u, rows[i].cls, WSubstep::Smu, "reg:smu");
    }
}

static R3SubstepResult run(const Scenario &sc, bool register_msg_grant = true,
                           uint32_t preset_flags = 0u)
{
    g_dev.reset(sc);
    memset(&g_rt, 0, sizeof g_rt);
    g_rt.ready_bits = sc.psp_ready ? R3_IP_BIT(R3Ip::Psp) : 0u;
    g_rt.substep.flags = preset_flags;
    r3_ev_init(&g_ev);
    n48log_host_reset();

    r3_writeguard_install_mm();
    if (register_msg_grant)
        register_smu_rows(kMp1Base1);

    R3SubstepEnv env;
    memset(&env, 0, sizeof env);
    env.rt = &g_rt;
    env.ev = &g_ev;
    env.ip = &g_dev.ip;
    env.strict_expect = true;
    return amdgpu::smu_v13_0_substep(&g_dev.io, &env);
}

static bool ok(const R3SubstepResult &r)
{
    return r.status == kR3SsecOk;
}

static bool named(const R3SubstepResult &r, const char *name)
{
    return r.name && strcmp(r.name, name) == 0;
}

// =========================================================================================
// tests
// =========================================================================================
static void test_constants()
{
    // mp_13_0_4_offset.h:300-349 (MP1, BASE_IDX 1).
    CHECK(amdgpu::kSmuMp1BaseIdx == 1, "MP1 BASE_IDX must be 1");
    CHECK(amdgpu::kSmuC2pmsg66 == kMp1C2pmsg66, "C2PMSG_66 offset");
    CHECK(amdgpu::kSmuC2pmsg82 == kMp1C2pmsg82, "C2PMSG_82 offset");
    CHECK(amdgpu::kSmuC2pmsg90 == kMp1C2pmsg90, "C2PMSG_90 offset");
    CHECK(amdgpu::kSmuMp1BaseIdx == 1 && amdgpu::kSmuC2pmsg66 == 0x0282u,
          "using MP0's 0x0082 would address 0x16082, not 0x16282");
    // smu_v13_0.c:237-259 (MP1_Public | smnMP1_V13_0_4_FIRMWARE_FLAGS).
    CHECK(amdgpu::kSmuFirmwareFlagsIndex == 0x03b10028u, "FIRMWARE_FLAGS SMN address");
    CHECK(amdgpu::kSmuFirmwareFlagsInterruptsEnabled == 1u, "FIRMWARE_FLAGS bit0");
    // gc_11_0_0_offset.h.
    CHECK(amdgpu::kSmuGrbmStatus == kGrbmStatus, "GRBM_STATUS offset");
    CHECK(amdgpu::kSmuGrbmStatus2 == kGrbmStatus2, "GRBM_STATUS2 offset");
    CHECK(amdgpu::kSmuGbAddrConfig == kGbAddrConfig, "GB_ADDR_CONFIG offset");
    CHECK(amdgpu::kSmuRlcStat == kRlcStat, "RLC_STAT offset");
    CHECK(amdgpu::kSmuSdma0StatusReg == kSdma0Status, "SDMA0_STATUS_REG offset");
    // smu_v13_0_4_ppsmc.h:55,56,71,76; smu_v13_0.c:86.
    CHECK(amdgpu::kSmuMsgGetPmfwVersion == 0x02u, "0x2 message id");
    CHECK(amdgpu::kSmuMsgGetDriverIfVersion == 0x03u, "0x3 message id");
    CHECK(amdgpu::kSmuMsgGetEnabledSmuFeatures == 0x12u, "0x12 message id");
    CHECK(amdgpu::kSmuMsgEnableGfxImu == 0x16u, "0x16 message id");
    CHECK(amdgpu::kSmuEnableImuArgGfxoff == 1u, "EnableGfxImu argument");
    // smu_cmn.c:76-84.
    CHECK(amdgpu::kSmuRespOk == 0x01u && amdgpu::kSmuRespBusy == 0xFCu &&
          amdgpu::kSmuRespFailed == 0xFFu && amdgpu::kSmuRespUnknownCmd == 0xFEu &&
          amdgpu::kSmuRespBadPrereq == 0xFDu && amdgpu::kSmuRespDebugEnd == 0xFBu,
          "response codes");
    CHECK(amdgpu::kSmuBaselinePmfw == 0x004C6500u, "PMFW baseline raw 76.101.0");
    // T-2/T-3: 2 s fast + 10 s slow = 12 s.
    CHECK(amdgpu::kSmuEffectFastUs == 2000000u && amdgpu::kSmuEffectSlowUs == 10000000u &&
          amdgpu::kSmuEffectTotalUs == 12000000u,
          "effect budget");
    CHECK(amdgpu::kSmuMailboxTimeoutUs == 2000000u, "mailbox timeout");
}

static void test_pre_failures()
{
    R3SubstepEnv env;
    memset(&env, 0, sizeof env);
    R3Io io;
    memset(&io, 0, sizeof io);
    Scenario sc;
    g_dev.reset(sc);
    env.rt = &g_rt;
    env.ev = &g_ev;
    env.ip = &g_dev.ip;
    env.strict_expect = true;
    R3SubstepResult r = amdgpu::smu_v13_0_substep(&io, &env);
    CHECK(named(r, "pre:no-io"), "empty I/O must fail pre:no-io (got %s)", r.name);
    r = amdgpu::smu_v13_0_substep(&g_dev.io, nullptr);
    CHECK(named(r, "pre:no-env"), "null env must fail pre:no-env (got %s)", r.name);

    struct Case {
        const char *name;
        bool psp_ready;
        bool mp1_resolved;
        bool gc_resolved;
        bool smuio_fail;
        uint32_t flags;
        const char *want;
    } cases[] = {
        {"mp1", true, false, true, false, 1u, "pre:mp1-unresolved"},
        {"psp", false, true, true, false, 1u, "pre:psp-not-ready"},
        {"gc", true, true, false, false, 1u, "pre:gc-base-unresolved"},
        {"smn-ok-false", true, true, true, true, 1u, "pre:fwflags-unreadable"},
        {"smn-sentinel", true, true, true, false, 0xFFFFFFFFu, "pre:fwflags-unreadable"},
        // bit0=0 is a WARNING that continues the sequence (test_pmfw_flags_bit0_continues).
    };
    for (const Case &c : cases) {
        Scenario s;
        s.psp_ready = c.psp_ready;
        s.mp1_resolved = c.mp1_resolved;
        s.gc_resolved = c.gc_resolved;
        s.smuio_fail = c.smuio_fail;
        s.firmware_flags = c.flags;
        r = run(s);
        CHECK(named(r, c.want), "%s: want %s, got %s", c.name, c.want, r.name);
        CHECK(g_dev.writes.empty(), "%s: no mailbox write before a failed precondition", c.name);
        CHECK(g_dev.imu_sends == 0u, "%s: 0x16 must not be sent", c.name);
        if (strncmp(c.name, "smn", 3) == 0)
            CHECK(evidence_has("read-fault, stop") && !evidence_has("WARN: MP1_FIRMWARE_FLAGS"),
                  "%s: evidence must say read-fault stop, never a bit0 warning", c.name);
    }
}

static void test_success()
{
    Scenario sc;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "success: got %s", r.name);
    CHECK((g_rt.ready_bits & R3_IP_BIT(R3Ip::Smu)) != 0u, "SMU ready bit");
    CHECK(g_dev.imu_sends == 1u, "one 0x16 send (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) == 0u, "no recheck on success");
    CHECK(g_dev.wrote(kMp1Base1 + kMp1C2pmsg66), "message register written");
    CHECK(!evidence_has("async-success"), "sync success must not carry the async label");
    CHECK(evidence_has("ready: pmfw=76.101.0 driver-if=8"), "version evidence");
    CHECK(g_dev.lock_depth == 0u, "lock released");
    CHECK(r3_writeguard_deny_count() == 0u, "no DENY on the success path");
}

static void test_async_success()
{
    Scenario sc;
    sc.imu_resp[0] = 0u;          // mailbox timeout
    sc.effect_after_send = 1u;
    sc.effect_delay_us = 1000u;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "async-success: got %s", r.name);
    CHECK(g_dev.imu_sends == 1u, "async-success must not recheck (got %u)", g_dev.imu_sends);
    CHECK(evidence_has("async-success"), "async-success label");
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) == 0u, "no recheck latched");
}

static void test_recheck_success()
{
    Scenario sc;
    sc.imu_resp[0] = 0u;
    sc.imu_late_us[0] = 2000000u;  // C2PMSG_90 becomes non-zero at the 2 s boundary
    sc.imu_late_code[0] = 0x01u;
    sc.effect_after_send = 2u;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "recheck success: got %s", r.name);
    CHECK(g_dev.imu_sends == 2u, "two 0x16 sends (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) != 0u, "recheck latched");
    CHECK(evidence_has("resent"), "recheck evidence");
    CHECK(evidence_has("effect#2 pass=1"), "second effect poll passed");
}

static void test_recheck_still_fails()
{
    Scenario sc;
    sc.imu_resp[0] = 0u;
    sc.imu_late_us[0] = 2000000u;
    sc.effect_after_send = 0u;  // never established
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:effect-not-established"),
          "recheck still fails: got %s", r.name);
    CHECK(g_dev.imu_sends == 2u, "exactly two 0x16 sends (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) != 0u, "recheck latched");
}

static void test_stuck_zero_no_resend()
{
    Scenario sc;
    sc.imu_resp[0] = 0u;
    sc.imu_late_us[0] = 0u;   // C2PMSG_90 stays 0 forever
    sc.effect_after_send = 0u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:effect-not-established"), "stuck zero: got %s", r.name);
    CHECK(g_dev.imu_sends == 1u, "stuck zero must never resend (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) == 0u, "recheck not latched");
    CHECK(evidence_has("stayed 0"), "stuck-zero evidence");
}

static void test_busy_recheck()
{
    Scenario sc;
    sc.imu_resp[0] = 0xFCu;  // Busy: PMFW says retry
    sc.effect_after_send = 2u;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "busy recheck: got %s", r.name);
    CHECK(g_dev.imu_sends == 2u, "busy recheck sends twice (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) != 0u, "recheck latched");
}

static void test_explicit_errors_no_recheck()
{
    const uint32_t codes[] = {0xFFu, 0xFDu, 0xFEu, 0xFBu, 0xFFFFFFFFu};
    for (uint32_t code : codes) {
        Scenario sc;
        sc.imu_resp[0] = code;
        sc.effect_after_send = 0u;
        R3SubstepResult r = run(sc);
        CHECK(!ok(r), "0x%02x must fail", code);
        CHECK(g_dev.imu_sends == 1u, "0x%02x must not recheck (got %u)", code,
              g_dev.imu_sends);
        CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) == 0u,
              "0x%02x must not latch the recheck", code);
    }
}

static void test_late_explicit_error_no_recheck()
{
    // S-06: C2PMSG_90 answers 0xFF at the effect window's 2 s boundary. That is an
    // explicit failure, not a timeout, so the 0x16 must not be rechecked.
    Scenario sc;
    sc.imu_resp[0] = 0u;
    sc.imu_late_us[0] = 2000000u;
    sc.imu_late_code[0] = 0xFFu;
    sc.effect_after_send = 0u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r), "late explicit error must fail: %s", r.name);
    CHECK(g_dev.imu_sends == 1u, "late 0xFF must not recheck (got %u)", g_dev.imu_sends);
    CHECK((g_rt.substep.flags & R3_SUBSTEP_RECHECK_USED) == 0u,
          "late 0xFF must not latch the recheck");
    CHECK(evidence_has("recheck suppressed"), "suppression evidence");

    const uint32_t codes[] = {0xFDu, 0xFEu};
    for (uint32_t code : codes) {
        Scenario s2;
        s2.imu_resp[0] = 0u;
        s2.imu_late_us[0] = 2000000u;
        s2.imu_late_code[0] = code;
        s2.effect_after_send = 0u;
        r = run(s2);
        CHECK(!ok(r) && g_dev.imu_sends == 1u,
              "late 0x%02x must not recheck (got %u)", code, g_dev.imu_sends);
    }

    // S-06 also covers an answer that arrives only after the 2 s phase check: the
    // slow window must re-read C2PMSG_90 at its end, so 0xFF at 3 s still suppresses
    // the recheck.
    Scenario s3;
    s3.imu_resp[0] = 0u;
    s3.imu_late_us[0] = 3000000u;
    s3.imu_late_code[0] = 0xFFu;
    s3.effect_after_send = 0u;
    r = run(s3);
    CHECK(!ok(r) && g_dev.imu_sends == 1u,
          "3 s late 0xFF must not recheck (got %u sends)", g_dev.imu_sends);
    CHECK(evidence_has("recheck suppressed"), "slow-window suppression evidence");
}

static void test_effect_read_fault()
{
    Scenario sc;
    sc.imu_resp[0] = 0x01u;
    sc.gc_fault = true;  // GC regs read 0xFFFFFFFF
    sc.effect_after_send = 1u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:effect-read-fault"), "0xFFFFFFFF effect: got %s", r.name);
    CHECK(g_dev.imu_sends == 1u, "a read fault must not recheck (got %u)", g_dev.imu_sends);
    CHECK(r.reading_count >= 2u, "effect readings attached");
}

static void test_recheck_guard_preset()
{
    Scenario sc;
    sc.imu_resp[0] = 0u;
    sc.imu_late_us[0] = 2000000u;  // a recheck would otherwise be possible
    sc.effect_after_send = 0u;
    R3SubstepResult r = run(sc, true, R3_SUBSTEP_RECHECK_USED);
    CHECK(!ok(r), "pre-latched recheck must fail the effect gate");
    CHECK(g_dev.imu_sends == 1u, "pre-latched recheck must not resend (got %u)",
          g_dev.imu_sends);
    CHECK(evidence_has("suppressed"), "suppression evidence");
}

static void test_driver_if_audit_only()
{
    Scenario sc;
    sc.driver_if_resp = 0xFDu;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "driver-if error must not gate (got %s)", r.name);
    CHECK(g_dev.imu_sends == 1u, "0x16 still sent after 0x3 error (got %u)",
          g_dev.imu_sends);
    CHECK(evidence_has("S3 audit"), "driver-if audit evidence");
}

static void test_pmfw_audit_only()
{
    Scenario sc;
    sc.pmfw_resp = 0xFDu;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "pmfw error must not gate (got %s)", r.name);
    CHECK(g_dev.imu_sends == 1u, "0x16 still sent after 0x2 error (got %u)",
          g_dev.imu_sends);
    CHECK(evidence_has("S4 audit"), "pmfw audit evidence");
}

static void test_version_timeout_blocks_imu()
{
    // A version message that leaves C2PMSG_90 at 0 is not an async success: the
    // next transaction's pre-poll (mandatory, never skipped) cannot start, so
    // 0x16 is never sent and the effect gate cannot be met.
    Scenario sc;
    sc.driver_if_resp = 0u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:enable-gfx-imu"),
          "stuck mailbox fails at 0x16 send (got %s)", r.name);
    CHECK(g_dev.imu_sends == 0u, "0x16 was never sent (got %u)", g_dev.imu_sends);
}

static void test_version_zero_audit_only()
{
    Scenario sc;
    sc.pmfw_ret = 0u;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "pmfw zero must not gate (got %s)", r.name);
    CHECK(evidence_has("pmfw=unavailable"), "pmfw zero audit evidence");

    Scenario sd;
    sd.driver_if_ret = 0u;
    r = run(sd);
    CHECK(ok(r), "driver-if zero must not gate (got %s)", r.name);
    CHECK(evidence_has("driver-if=unavailable"), "driver-if zero audit evidence");
}

static void test_feature_audit_non_gating()
{
    Scenario sc;
    sc.feature_resp = 0xFDu;  // 0x12 fails: audit only, never gates
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "feature failure must not gate (got %s)", r.name);
    CHECK(evidence_has("feature unavailable"), "feature unavailable evidence");

    Scenario diff;
    diff.feature_lo = 0xdeadbeefu;
    diff.feature_hi = 0x12345678u;
    r = run(diff);
    CHECK(ok(r), "feature difference must not gate (got %s)", r.name);
    CHECK(evidence_has("D10 features"), "D10 feature note");
}

static void test_manifest_recheck_count()
{
    Scenario sc;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "manifest baseline: got %s", r.name);
    const R3WManifest *m = r3_writeguard_manifest();
    uint32_t msg_count = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const R3WMEntry &e = m->entries[i];
        if (e.addr == kMp1Base1 + kMp1C2pmsg66 && e.cls == WClass::Protocol &&
            e.owner == WSubstep::Smu && e.tag && strcmp(e.tag, "reg:smu") == 0)

            msg_count += e.count;
    }
    // 0x3 + 0x2 + 0x16 + 0x12 low + 0x12 high share the message register. The
    // manifest either merges them (same key, count++) or records them per-write
    // (③④ no-merge contract), so sum the counts of every matching entry.
    CHECK(msg_count == 5u, "0x3+0x2+0x16+0x12x2 message writes (got %u)", msg_count);

    Scenario rc;
    rc.imu_resp[0] = 0u;
    rc.imu_late_us[0] = 2000000u;
    rc.effect_after_send = 2u;
    r = run(rc);
    CHECK(ok(r), "manifest recheck: got %s", r.name);
    m = r3_writeguard_manifest();
    msg_count = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const R3WMEntry &e = m->entries[i];
        if (e.addr == kMp1Base1 + kMp1C2pmsg66 && e.cls == WClass::Protocol &&
            e.owner == WSubstep::Smu && e.tag && strcmp(e.tag, "reg:smu") == 0)
            msg_count += e.count;
    }
    CHECK(msg_count == 6u, "the recheck's 0x16 adds one message write (got %u)", msg_count);
}

static void test_grant_missing()
{
    Scenario sc;
    R3SubstepResult r = run(sc, false);
    CHECK(!ok(r) && named(r, "seq:driver-if"), "missing grant must DENY: got %s",
          r.name);
    CHECK(r3_writeguard_deny_count() > 0u, "DENY counted");
    CHECK(g_dev.imu_sends == 0u, "no 0x16 without grants");
}

static void test_read_manifest()
{
    Scenario sc;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "read manifest: got %s", r.name);
    CHECK(std::find(g_dev.smu_reads.begin(), g_dev.smu_reads.end(), 0x03b10028u) !=
              g_dev.smu_reads.end(),
          "FIRMWARE_FLAGS SMN read recorded");
    CHECK(!g_dev.read(kMp1Base1 + kMp1C2pmsg66), "C2PMSG_66 is write-only in this sequence");
    CHECK(g_dev.read(kMp1Base1 + kMp1C2pmsg90), "C2PMSG_90 read recorded");
    CHECK(g_dev.read(kMp1Base1 + kMp1C2pmsg82), "C2PMSG_82 read recorded");
    CHECK(g_dev.read(kGcBase0 + kGrbmStatus), "GRBM_STATUS read recorded");
    CHECK(g_dev.read(kGcBase1 + kRlcStat), "RLC_STAT read recorded");
    CHECK(g_dev.read(kGcBase0 + kGrbmStatus2), "GRBM_STATUS2 read recorded");
    CHECK(g_dev.read(kGcBase0 + kGbAddrConfig), "GB_ADDR_CONFIG read recorded");
    CHECK(g_dev.read(kSdma0Base0 + kSdma0Status), "SDMA0_STATUS_REG read recorded");
}

static void test_rlc_value_not_gated()
{
    Scenario sc;
    sc.rlc_zero = true;  // GRBM_STATUS valid, RLC_STAT 0 (idle, healthy)
    sc.effect_after_send = 1u;
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "RLC_STAT=0 must not gate the effect verdict (got %s)", r.name);
    CHECK(evidence_has("rlc=0x00000000"), "RLC_STAT=0 recorded as a companion");
    CHECK(evidence_has("fault=0"), "effect evidence marks no read-path fault");
}

static void test_rlc_read_fault()
{
    Scenario sc;
    sc.rlc_fault = true;  // GRBM_STATUS valid, RLC_STAT reads 0xFFFFFFFF
    sc.effect_after_send = 1u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:effect-read-fault"),
          "RLC_STAT read fault must fail closed even with GRBM valid (got %s)", r.name);
    CHECK(g_dev.imu_sends == 1u, "a read fault must not recheck (got %u)", g_dev.imu_sends);
    CHECK(evidence_has("rlc=0xffffffff") && evidence_has("fault=1"),
          "effect evidence records the fault value and the fault flag");
}

static void test_pmfw_flags_bit0_continues()
{
    Scenario sc;
    sc.firmware_flags = 0u;  // bit0=0: WARN and continue (C0.5 effect verdict is final)
    R3SubstepResult r = run(sc);
    CHECK(ok(r), "FIRMWARE_FLAGS bit0=0 must not stop the sequence (got %s)", r.name);
    CHECK(g_dev.imu_sends == 1u, "0x16 still sent after bit0=0 (got %u)", g_dev.imu_sends);
    CHECK(evidence_has("bit0=0") && evidence_has("WARN") && evidence_has("continue"),
          "bit0=0 is a WARN+continue in evidence");
    CHECK(!evidence_has("read-fault, stop"),
          "bit0=0 must not be reported as a read-path fault / stop");
}

static void test_streak_two()
{
    Scenario sc;
    sc.effect_pulse_once = true;  // one valid sample, then zero
    sc.effect_after_send = 1u;
    R3SubstepResult r = run(sc);
    CHECK(!ok(r) && named(r, "seq:effect-not-established"),
          "two consecutive samples are required (got %s)", r.name);
}

int main()
{
    test_constants();
    test_pre_failures();
    test_success();
    test_async_success();
    test_recheck_success();
    test_recheck_still_fails();
    test_stuck_zero_no_resend();
    test_busy_recheck();
    test_explicit_errors_no_recheck();
    test_late_explicit_error_no_recheck();
    test_effect_read_fault();
    test_recheck_guard_preset();
    test_driver_if_audit_only();
    test_pmfw_audit_only();
    test_version_timeout_blocks_imu();
    test_version_zero_audit_only();
    test_feature_audit_non_gating();
    test_manifest_recheck_count();
    test_grant_missing();
    test_read_manifest();
    test_rlc_value_not_gated();
    test_rlc_read_fault();
    test_pmfw_flags_bit0_continues();
    test_streak_two();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
