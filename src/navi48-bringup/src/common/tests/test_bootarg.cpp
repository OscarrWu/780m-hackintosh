// test_bootarg.cpp — BootArgFamily: family routing, defaults, and the documented
// load-gate spelling. Run twice, with and without TARGET_RDNA3:
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 \
//       -I src/navi48-bringup/src/common -I src/navi48-bringup/tests/dalshim \
//       src/navi48-bringup/src/common/tests/test_bootarg.cpp \
//       src/navi48-bringup/src/common/BootArgFamily.cpp -o /tmp/t1c_bootarg_navi48
//   clang++ ... -DTARGET_RDNA3=1 ... -o /tmp/t1c_bootarg_rdna3
//   /tmp/t1c_bootarg_navi48 && /tmp/t1c_bootarg_rdna3
//
// (paths relative to the fork repository root; the pexpert shim provides the KPI
// declaration, the stub below provides its host behaviour.)
#include <cstdio>
#include <cstring>
#include <string>

#include "BootArgFamily.h"
#include "TargetConfig.h"

#if defined(TARGET_RDNA3)
static const char *kFamily = "n48r3";
static const char *kPrefix = "N48R3LOG";
#else
static const char *kFamily = "navi48";
static const char *kPrefix = "N48LOG";
#endif

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-74s got %#llx want %#llx\n", what,
                    (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-74s %#llx\n", what, (unsigned long long)got);
    }
}

static void expect_s(const char *what, const std::string &got, const std::string &want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-74s got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
    } else {
        std::printf("ok    %-74s \"%s\"\n", what, got.c_str());
    }
}

// ---- the KPI stub -----------------------------------------------------------------
static bool gPresent = false;
static uint32_t gValue = 0;
static std::string gName;
static int gCalls = 0;

extern "C" bool PE_parse_boot_argn(const char *name, void *out, int size)
{
    gCalls++;
    gName = name ? name : "";
    if (!gPresent) return false;
    if (out && size >= (int)sizeof(uint32_t)) std::memcpy(out, &gValue, sizeof gValue);
    return true;
}

static void reset_parse(bool present, uint32_t value)
{
    gPresent = present;
    gValue = value;
    gName.clear();
    gCalls = 0;
}

static void reset_policy()
{
    TgtDefaultPolicy &p = tgt_default_policy();
    p.bringup = 0;
    p.stage = 0;
    p.other = 0;
}

static std::string name_of(const char *leaf)
{
    char buf[96] = { 0 };
    if (!tgt_boot_arg_name(leaf, buf, sizeof buf)) return "<too long>";
    return buf;
}

int main()
{
    // Compile-time contract: the macros are the literals the rest of the tree greps for.
    expect_s("TGT_BOOTARG_FAMILY is the documented family", TGT_BOOTARG_FAMILY, kFamily);
    expect_s("TGT_LOG_PREFIX is the documented prefix", TGT_LOG_PREFIX, kPrefix);

    // The shipped defaults, before any test mutates the policy.
    TgtDefaultPolicy &initial = tgt_default_policy();
    expect_u("shipped bringup default is 0 (do not load)", initial.bringup, 0);
    expect_u("shipped stage default is 0 (read-only survey)", initial.stage, 0);
    expect_u("shipped other default is 0 (off)", initial.other, 0);

    // ---- routing: the load gate has no hyphen, everything else does -----------------
    expect_s("bringup leaf -> <family>bringup (upstream-compatible spelling)",
             name_of("bringup"), std::string(kFamily) + "bringup");
    expect_s("stage leaf -> <family>-stage",
             name_of("stage"), std::string(kFamily) + "-stage");
    expect_s("future psp leaf -> <family>-psp",
             name_of("psp"), std::string(kFamily) + "-psp");
    expect_s("case is preserved", name_of("PSP"), std::string(kFamily) + "-PSP");

    reset_parse(true, 7);
    uint32_t v = 0;
    expect_u("a present stage parses", tgt_boot_arg_u32("stage", &v), 1);
    expect_u("  parsed value", v, 7);
    expect_s("  PE saw the hyphenated name", gName, std::string(kFamily) + "-stage");

    reset_parse(true, 1);
    v = 0;
    expect_u("a present bringup parses", tgt_boot_arg_u32("bringup", &v), 1);
    expect_u("  parsed value", v, 1);
    expect_s("  PE saw the un-hyphenated load gate", gName, std::string(kFamily) + "bringup");

    reset_parse(true, 0);
    v = 99;
    expect_u("present with value 0 is still present", tgt_boot_arg_u32("stage", &v), 1);
    expect_u("  and the zero value is written", v, 0);

    reset_parse(true, 1);
    expect_u("present() sees the load gate", tgt_boot_arg_present("bringup"), 1);
    expect_s("  through the un-hyphenated name", gName, std::string(kFamily) + "bringup");
    reset_parse(false, 0);
    expect_u("present() is false when the arg is absent", tgt_boot_arg_present("bringup"), 0);

    // ---- the default matrix (F1/F3: everything conservative, load gate off) ---------
    reset_policy();
    reset_parse(false, 0);
    v = 1234;
    expect_u("absent bringup -> default policy (0 = do not load)", tgt_boot_arg_u32("bringup", &v), 0);
    expect_u("  bringup default value", v, 0);
    v = 1234;
    expect_u("absent stage -> default policy (0 = survey only)", tgt_boot_arg_u32("stage", &v), 0);
    expect_u("  stage default value", v, 0);
    v = 1234;
    expect_u("absent other leaf -> default policy (0 = off)", tgt_boot_arg_u32("psp", &v), 0);
    expect_u("  other default value", v, 0);

    // Each matrix cell is independent: flipping one must only move that cell.
    TgtDefaultPolicy &p = tgt_default_policy();
    p.bringup = 5; p.stage = 3; p.other = 9;
    reset_parse(false, 0);
    v = 0; expect_u("mutated bringup default is observed", tgt_boot_arg_u32("bringup", &v), 0); expect_u("  value", v, 5);
    v = 0; expect_u("mutated stage default is observed", tgt_boot_arg_u32("stage", &v), 0); expect_u("  value", v, 3);
    v = 0; expect_u("mutated other default is observed", tgt_boot_arg_u32("psp", &v), 0); expect_u("  value", v, 9);
    reset_policy();

    // ---- names and hostile inputs ---------------------------------------------------
    char tiny[8] = { 0 };
    expect_u("a name that does not fit is refused", tgt_boot_arg_name("stage", tiny, 4), 0);
    expect_u("null leaf is refused", tgt_boot_arg_name(nullptr, tiny, sizeof tiny), 0);
    expect_u("empty leaf is refused", tgt_boot_arg_name("", tiny, sizeof tiny), 0);
    expect_u("parse with null out is refused", tgt_boot_arg_u32("stage", nullptr), 0);
    expect_u("parse with null leaf is refused", tgt_boot_arg_u32(nullptr, &v), 0);
    expect_u("present() with null leaf is false", tgt_boot_arg_present(nullptr), 0);

    reset_policy();
    reset_parse(true, 7);
    v = 1234;
    const char *longleaf = "this-leaf-is-far-too-long-for-the-48-byte-name-buffer-0123456789";
    expect_u("an over-long leaf is refused", tgt_boot_arg_u32(longleaf, &v), 0);
    expect_u("  and falls back to its default", v, 0);
    expect_u("  PE is not called with a truncated name", gCalls, 0);

    // ---- planted-break guards: the checks above must reject these wrong shapes ------
    // Wrong spelling: hyphenating the load gate.
    expect_u("guard: a hyphenated load gate would be caught",
             (std::string(kFamily) + "-bringup" != name_of("bringup")) ? 1u : 0u, 1u);
    // Wrong default source: hardcoded zero instead of the policy.
    p.bringup = 1;
    reset_parse(false, 0);
    v = 0;
    (void)tgt_boot_arg_u32("bringup", &v);
    expect_u("guard: ignoring the default policy would be caught", (v != 0) ? 1u : 0u, 1u);
    reset_policy();

    // ---- T2: three-state kill switch (D10/F2) --------------------------------------
    {
        reset_policy();
        reset_parse(false, 0);
        expect_u("T2 absent psp -> FollowStage", r3_kill_state("psp"), kR3KillFollowStage);
        expect_u("  PE was consulted once for the whitelisted leaf", gCalls, 1);

        reset_parse(true, 0);
        expect_u("T2 present 0 psp -> Off", r3_kill_state("psp"), kR3KillOff);
        expect_s("  through <family>-psp", gName, std::string(kFamily) + "-psp");

        reset_parse(true, 7);
        expect_u("T2 present non-zero psp -> On", r3_kill_state("psp"), kR3KillOn);

        reset_parse(true, 0xffffffffu);
        expect_u("T2 present 0xffffffff -> On", r3_kill_state("psp"), kR3KillOn);

        // Every whitelisted leaf uses the same rule.
        static const char *const leaves[] = { "psp", "smu", "gfx", "sdma", "mes" };
        for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
            reset_parse(false, 0);
            expect_u("T2 absent leaf -> FollowStage", r3_kill_state(leaves[i]),
                     kR3KillFollowStage);
            reset_parse(true, 0);
            expect_u("T2 present 0 leaf -> Off", r3_kill_state(leaves[i]), kR3KillOff);
            reset_parse(true, 3);
            expect_u("T2 present 3 leaf -> On", r3_kill_state(leaves[i]), kR3KillOn);
        }

        // Non-whitelisted leaves are FollowStage with no PE side effect.
        static const char *const others[] = { "stage", "bringup", "PSP", "psp2", "depth", "" };
        for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
            reset_parse(true, 0);
            expect_u("T2 non-whitelist leaf -> FollowStage",
                     r3_kill_state(others[i]), kR3KillFollowStage);
            expect_u("  no PE access for a non-whitelist leaf", gCalls, 0);
        }
        reset_parse(true, 0);
        expect_u("T2 null leaf -> FollowStage", r3_kill_state(nullptr), kR3KillFollowStage);
        expect_u("  no PE access for null", gCalls, 0);

        // The generic default policy must not leak into the kill state (planted-break
        // guard: reusing tgt_boot_arg_u32's default would turn absent into Off/On).
        TgtDefaultPolicy &kp = tgt_default_policy();
        kp.other = 9;
        reset_parse(false, 0);
        expect_u("T2 absent ignores policy.other=9 (not On)",
                 r3_kill_state("psp"), kR3KillFollowStage);
        kp.other = 0;
        reset_parse(false, 0);
        expect_u("T2 absent ignores policy.other=0 (not Off)",
                 r3_kill_state("psp"), kR3KillFollowStage);
        reset_policy();

        // Existing u32 semantics are untouched (RDNA4 regression gate).
        reset_parse(false, 0);
        v = 1234;
        expect_u("T2 guard: tgt_boot_arg_u32 absent still reports the default",
                 tgt_boot_arg_u32("psp", &v), 0);
        expect_u("  and still writes policy.other", v, 0);
    }

    std::printf("\n%s: %d checks, %d failures\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
