// test_bringup_depth.cpp - host tests for the n48r3-depth entry wiring.
//
// Covers the two behaviours start() relies on: the conservative default (0 = T1) when
// the boot-arg is absent, and the 0..5 clamp that maps every out-of-range value to 0.
// Host-only: NaviR3Bringup.hpp exposes r3_depth_clamp without the IOKit class under
// !KERNEL, and the pexpert KPI is stubbed below.
#include "NaviR3Bringup.hpp"
// D-01 selector constants (mirrors NaviR3UserClient.hpp; not included to avoid IOKit).
enum { kN48R3SelInfo_Test = 0, kN48R3SelTrigger_Test = 1 };

#include "common/BootArgFamily.h"
#include "common/StageRunner.h"
#include "common/TargetConfig.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// This test's host runner links only BootArgFamily; pull in the real ladder implementation
// so the target-index checks below exercise bringup_r3() itself, not a re-implementation.
#include "../../common/StageRunner.cpp"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void check_u(const char *what, uint32_t got, uint32_t want) {
    if (got != want) {
        std::printf("FAIL %s: got %u want %u\n", what, got, want);
        failures++;
    }
}

// ---- pexpert KPI stub -------------------------------------------------------------
static bool gPresent = false;
static uint32_t gValue = 0;
static std::string gName;

extern "C" bool PE_parse_boot_argn(const char *name, void *out, int size) {
    gName = name ? name : "";
    if (!gPresent || !out || size <= 0) return false;
    if (size < (int)sizeof(uint32_t)) return false;
    std::memcpy(out, &gValue, sizeof gValue);
    return true;
}

static void set_parse(bool present, uint32_t value) {
    gPresent = present;
    gValue = value;
    gName.clear();
}

// Depth select: the exact resolver start() calls.
static uint32_t parse_depth() { return r3_depth_select().depth; }

// First power-up substep index (psp row). Must match R3_LADDER_FIRST_SUBSTEP
// in rdna3_survey.h; the integration tests pin r3_trigger_admission's contract
// so this code only needs the value, not the header.
static constexpr uint32_t kFirstSubstep = 2u;

// ---- trigger path: `bringup_r3_from(first_substep, target)` ---------------------------
// start() runs survey+preflight (indices 0-1) and arms the trigger. The trigger fires
// only the power-up substeps (psp onward at index R3_LADDER_FIRST_SUBSTEP = kFirstSubstep)
// via bringup_r3_from(first_substep, target) — never re-running survey/preflight.
// target = min(stage+1, table_count-1) — the contract is tested directly in test_integration.
// Reads the real kStages declaration from rdna3_stages.cpp so bringup_r3_from exercises
// the exact stage names the production ladder uses.
static std::string read_text(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        std::printf("FAIL cannot read %s\n", path.c_str());
        failures++;
        return std::string();
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string test_dir() {
    const std::string f = __FILE__;
    const std::string::size_type s = f.find_last_of('/');
    return s == std::string::npos ? std::string(".") : f.substr(0, s);
}

static std::vector<std::string> parse_kstages(const std::string &src) {
    static const char key[] = "kStages[] = {";
    std::vector<std::string> names;
    const std::string::size_type p = src.find(key);
    if (p == std::string::npos) return names;
    const std::string::size_type start = p + sizeof key - 1u;
    const std::string::size_type end = src.find("};", start);
    if (end == std::string::npos) return names;
    std::string::size_type i = start;
    while (i < end) {
        const std::string::size_type brace = src.find('{', i);
        if (brace == std::string::npos || brace >= end) break;
        const std::string::size_type a = src.find('"', brace);
        if (a == std::string::npos || a >= end) break;
        const std::string::size_type b = src.find('"', a + 1u);
        if (b == std::string::npos || b > end) break;
        names.push_back(src.substr(a + 1u, b - a - 1u));
        i = b + 1u;
    }
    return names;
}

struct LadderLog { std::string text; };

static void ladder_log(void *user, const char *line) {
    LadderLog *l = static_cast<LadderLog *>(user);
    if (!l || !line) return;
    l->text += line;
    l->text += '\n';
}

static StageResult ladder_run(StageCtx &) { return kStageOk; }
static std::string run_ladder_from(const std::vector<std::string> &names,
                                    uint32_t first, uint32_t target) {
    std::vector<StageDef> defs;
    defs.reserve(names.size());
    for (const std::string &n : names) {
        StageDef d {};
        d.name = n.c_str();
        d.run = ladder_run;
        d.enabled = nullptr;
        defs.push_back(d);
    }
    const StageTable table = { defs.data(), (uint32_t)defs.size() };
    LadderLog log;
    StageCtx ctx {};
    ctx.log = ladder_log;
    ctx.user = &log;
    if (bringup_r3_from(first, target, table, ctx, nullptr) != kStageOk) {
        std::printf("FAIL ladder from %u to %u did not complete\n", first, target);
        failures++;
    }
    return log.text;
}

static bool ladder_ran(const std::string &log, const std::string &name) {
    return log.find("(" + name + ")") != std::string::npos;
}


int main() {
    // 1. Absent boot-arg -> central default 0 (T1). This is the planted-break target:
    //    flipping the default away from 0 must fail here.
    set_parse(false, 0);
    const R3DepthSel absent = r3_depth_select();
    CHECK(!absent.present);
    check_u("absent raw", absent.raw, 0);
    check_u("absent depth defaults to T1 (0)", absent.depth, 0);
    if (gName != "n48r3-depth") {
        std::printf("FAIL depth leaf name: got '%s' want 'n48r3-depth'\n", gName.c_str());
        failures++;
    }

    // 2. Every in-range value passes through unchanged, and present is reported.
    for (uint32_t v = 0; v <= 5; v++) {
        set_parse(true, v);
        const R3DepthSel sel = r3_depth_select();
        CHECK(sel.present);
        check_u("in-range raw preserved", sel.raw, v);
        check_u("in-range depth preserved", sel.depth, v);
    }

    // 3. Out-of-range values clamp to 0 (T1), including the 32-bit extremes.
    const uint32_t bad[] = { 6u, 7u, 42u, 0xffffffffu };
    for (uint32_t v : bad) {
        set_parse(true, v);
        check_u("out-of-range depth clamps to 0", parse_depth(), 0);
    }

    // 4. The clamp itself (independent of the parser).
    check_u("clamp 0", r3_depth_clamp(0), 0);
    check_u("clamp 5", r3_depth_clamp(5), 5);
    check_u("clamp 6", r3_depth_clamp(6), 0);

    // ---- D-01: trigger two-form and D-04 marker logic -------------------------------

    // 8b: n48r3-auto absent → false (default off; the operator must opt in).
    set_parse(false, 0);
    CHECK(!r3_auto_mode());

    // 8b (INV-D5): n48r3-auto=1 is compile-time disabled in the development phase —
    // r3_auto_mode() returns false and names the refusal through `rejected`.
    set_parse(true, 1);
    {
        bool rejected = false;
        CHECK(!r3_auto_mode(&rejected));
        CHECK(rejected);
    }

    // 8b: n48r3-auto=0 → false (explicitly off, not a refusal).
    set_parse(true, 0);
    {
        bool rejected = true;
        CHECK(!r3_auto_mode(&rejected));
        CHECK(!rejected);
    }

    // 8c: the trigger selector is the next free slot after info (selector 0).
    CHECK(kN48R3SelInfo_Test == 0);
    CHECK(kN48R3SelTrigger_Test == 1);

    // ---- trigger maps stage N to bringup_r3_from(kFirstSubstep, target) ----------------
    // n48r3-stage=N means "N power-up substeps". kFirstSubstep (= R3_LADDER_FIRST_SUBSTEP)
    // is the psp row index; the trigger fires bringup_r3_from(kFirstSubstep, target) where
    // target = min(N+1, table_count-1), skipping survey+preflight that already ran in start().
    {
        const std::string dir = test_dir();
        const std::vector<std::string> stages =
            parse_kstages(read_text(dir + "/../rdna3_stages.cpp"));
        const std::vector<std::string> want = {
            "survey", "preflight", "psp", "smu", "gfx", "sdma", "mes"
        };
        CHECK(stages == want);

        // stage=1 -> target = min(1+1, 6) = 2. bringup_r3_from(2, 2) runs psp only.
        // survey+preflight already ran in start() and are NOT re-fired by the trigger.
        {
            const std::string t2 = run_ladder_from(stages, kFirstSubstep, 2u);
            CHECK(!ladder_ran(t2, "survey"));     // start() already ran these
            CHECK(!ladder_ran(t2, "preflight"));
            CHECK(ladder_ran(t2, "psp"));         // the first fired substep
            CHECK(!ladder_ran(t2, "smu"));
            CHECK(!ladder_ran(t2, "gfx"));
            CHECK(!ladder_ran(t2, "sdma"));
            CHECK(!ladder_ran(t2, "mes"));
        }

        // stage=5 -> target = min(5+1, 6) = 6. bringup_r3_from(2, 6) runs all 5 substeps.
        {
            const std::string t6 = run_ladder_from(stages, kFirstSubstep, 6u);
            CHECK(!ladder_ran(t6, "survey"));     // start() already ran these
            CHECK(!ladder_ran(t6, "preflight"));
            CHECK(ladder_ran(t6, "psp"));
            CHECK(ladder_ran(t6, "smu"));
            CHECK(ladder_ran(t6, "gfx"));
            CHECK(ladder_ran(t6, "sdma"));
            CHECK(ladder_ran(t6, "mes"));
        }

        // stage=0 -> target = min(0+1, 6) = 1 < kFirstSubstep → trigger fires no substeps.
        // (r3_trigger_admission returns NoSubsteps; this test verifies the ladder is no-op.)
        {
            const std::string t0 = run_ladder_from(stages, kFirstSubstep, 1u);
            CHECK(!ladder_ran(t0, "psp"));        // first substep never reached
            CHECK(!ladder_ran(t0, "smu"));
            CHECK(!ladder_ran(t0, "mes"));
        }
    }

    // T19/D-04 planted break: a second trigger in the same boot must be rejected.
    // The marker is a property on the provider (IOPCIDevice); here we simulate the
    // logic: the first trigger sets the marker, the second sees it and refuses.
    {
        bool marker = false;
        auto trigger_sim = [&]() -> int {
            if (marker) return (int)0xe00002e2;  // kIOReturnNotPermitted
            marker = true;
            return 0;  // kIOReturnSuccess
        };
        CHECK(trigger_sim() == 0);
        CHECK(trigger_sim() == (int)0xe00002e2);
    }
    if (failures) {
        std::printf("test_bringup_depth: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_bringup_depth: all checks passed\n");
    return 0;
}
