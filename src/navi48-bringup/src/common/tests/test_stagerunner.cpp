// test_stagerunner.cpp — StageRunner: depth control, fail-stop, dump-on-failure and
// the no-later-stages rule.
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 \
//       -I src/navi48-bringup/src/common \
//       src/navi48-bringup/src/common/tests/test_stagerunner.cpp \
//       src/navi48-bringup/src/common/StageRunner.cpp -o /tmp/t1c_stagerunner
//   /tmp/t1c_stagerunner
//
// (paths relative to the fork repository root)
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include "StageRunner.h"

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got %#llx want %#llx\n", what,
                    (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
    }
}

static void expect_has(const char *what, const std::string &hay, const char *needle, int want)
{
    const bool got = hay.find(needle) != std::string::npos;
    expect_u(what, got ? 1u : 0u, (uint64_t)want);
}

static constexpr unsigned kMaxStages = 5;

struct RunState {
    std::vector<uint32_t> ran;
    StageResult results[kMaxStages] = { kStageOk, kStageOk, kStageOk, kStageOk, kStageOk };
    bool enabled[kMaxStages] = { true, true, true, true, true };
    int dump_calls = 0;
    std::string log;
    std::vector<uint32_t> started;
    std::vector<uint32_t> stopped;
    std::vector<int> stop_results;
    std::string events;
};

static void log_hook(void *user, const char *line)
{
    if (!user) return;
    static_cast<RunState *>(user)->log += line;
    static_cast<RunState *>(user)->log += '\n';
}

static void dump_hook(StageCtx &ctx)
{
    RunState *s = static_cast<RunState *>(ctx.user);
    s->dump_calls++;
    s->events += "dump;";
}

static void start_hook(StageCtx &ctx, uint32_t index)
{
    RunState *s = static_cast<RunState *>(ctx.user);
    if (!s) return;
    s->started.push_back(index);
    s->events += "start;";
}

static void stop_hook(StageCtx &ctx, uint32_t index, StageResult result)
{
    RunState *s = static_cast<RunState *>(ctx.user);
    if (!s) return;
    s->stopped.push_back(index);
    s->stop_results.push_back((int)result);
    s->events += "stop;";
}

static const StageHooks kHooks = {
    .on_stage_start = start_hook,
    .on_stage_stop = stop_hook,
};

static StageResult record(StageCtx &ctx, uint32_t index)
{
    RunState *s = static_cast<RunState *>(ctx.user);
    if (!s) return kStageFail;
    s->ran.push_back(index);
    return index < kMaxStages ? s->results[index] : kStageFail;
}

static StageResult run0(StageCtx &c) { return record(c, 0); }
static StageResult run1(StageCtx &c) { return record(c, 1); }
static StageResult run2(StageCtx &c) { return record(c, 2); }
static StageResult run3(StageCtx &c) { return record(c, 3); }

static bool gate_at(const StageCtx &ctx, uint32_t index)
{
    const RunState *s = static_cast<const RunState *>(ctx.user);
    return s ? s->enabled[index] : true;
}

static bool gate0(const StageCtx &c) { return gate_at(c, 0); }
static bool gate1(const StageCtx &c) { return gate_at(c, 1); }
static bool gate2(const StageCtx &c) { return gate_at(c, 2); }
static bool gate3(const StageCtx &c) { return gate_at(c, 3); }

static const StageDef kStages[] = {
    { .name = "survey", .run = run0 },
    { .name = "psp",    .run = run1 },
    { .name = "smu",    .run = run2 },
    { .name = "gfx",    .run = run3 },
};
static const StageTable kTable = { kStages, 4 };

static const StageDef kGateStages[] = {
    { .name = "survey", .run = run0, .enabled = gate0 },
    { .name = "psp",    .run = run1, .enabled = gate1 },
    { .name = "smu",    .run = run2, .enabled = gate2 },
    { .name = "gfx",    .run = run3, .enabled = gate3 },
};
static const StageTable kGateTable = { kGateStages, 4 };

static StageCtx make_ctx(RunState &s)
{
    StageCtx c {};
    c.log = log_hook;
    c.dump_failure_state = dump_hook;
    c.user = &s;
    return c;
}

static uint32_t ran_at(const RunState &s, size_t i)
{
    return i < s.ran.size() ? s.ran[i] : 0xFFFFFFFFu;
}

static uint32_t ran_last(const RunState &s)
{
    return s.ran.empty() ? 0xFFFFFFFFu : s.ran.back();
}

// A plausible wrong runner: it records the failure and keeps walking the ladder.
static StageResult bad_continue(uint32_t target_stage, const StageTable &table, StageCtx &ctx)
{
    const uint32_t last = table.count - 1;
    const uint32_t limit = target_stage > last ? last : target_stage;
    for (uint32_t i = 0; i <= limit; i++) {
        if (table.stages[i].run) (void)table.stages[i].run(ctx);
    }
    return kStageOk;
}

int main()
{
    // ---- depth: stages up to and including target, later stages untouched ----------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("target 1 returns ok", bringup_to(1, kTable, c), kStageOk);
        expect_u("  stages executed", s.ran.size(), 2);
        expect_u("  stage 0 ran", ran_at(s, 0), 0);
        expect_u("  stage 1 ran", ran_at(s, 1), 1);
        expect_u("  no dump on success", s.dump_calls, 0);
        expect_has("  a success is logged", s.log, "stage 1 (psp)", 1);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("target 3 runs the whole table", bringup_to(3, kTable, c), kStageOk);
        expect_u("  stages executed", s.ran.size(), 4);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("an over-large target clamps to the last stage",
                 bringup_to(99, kTable, c), kStageOk);
        expect_u("  stages executed", s.ran.size(), 4);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("target 0 runs only the survey stage", bringup_to(0, kTable, c), kStageOk);
        expect_u("  stages executed", s.ran.size(), 1);
        expect_u("  stage 0 ran", ran_at(s, 0), 0);
    }

    // ---- failure: dump once, latch through the log, stop immediately ---------------
    {
        RunState s;
        s.results[2] = kStageFail;
        StageCtx c = make_ctx(s);
        expect_u("a failing stage returns its result", bringup_to(3, kTable, c), kStageFail);
        expect_u("  stage 2 ran", s.ran.size(), 3);
        expect_u("  stage 3 did NOT run", ran_last(s), 2);
        expect_u("  dump called exactly once", s.dump_calls, 1);
        expect_has("  the latch is recorded in the log", s.log, "latched", 1);
        expect_has("  the failing stage is named", s.log, "stage 2 (smu)", 1);
    }
    {
        RunState s;
        s.results[1] = kStageSkip;
        StageCtx c = make_ctx(s);
        expect_u("a skip is non-Ok and returns as-is", bringup_to(3, kTable, c), kStageSkip);
        expect_u("  it stops at the skip", s.ran.size(), 2);
        expect_u("  dump called exactly once", s.dump_calls, 1);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        StageDef broken[] = { { .name = "survey", .run = run0 },
                              { .name = "null", .run = nullptr },
                              { .name = "gfx", .run = run2 } };
        StageTable t = { broken, 3 };
        expect_u("a null stage body is a failure", bringup_to(2, t, c), kStageFail);
        expect_u("  the stage after it did NOT run", s.ran.size(), 1);
        expect_u("  dump called once", s.dump_calls, 1);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        c.dump_failure_state = nullptr;
        s.results[1] = kStageFail;
        expect_u("a missing dump hook does not crash", bringup_to(3, kTable, c), kStageFail);
        expect_u("  and still stops", s.ran.size(), 2);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        c.log = nullptr;
        s.results[1] = kStageFail;
        expect_u("a missing log hook does not crash", bringup_to(3, kTable, c), kStageFail);
        expect_u("  and still stops", s.ran.size(), 2);
    }

    // ---- empty/invalid tables ------------------------------------------------------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        StageTable empty = { nullptr, 0 };
        expect_u("an empty table is a successful no-op", bringup_to(3, empty, c), kStageOk);
        expect_u("  nothing ran", s.ran.size(), 0);
    }

    // ---- planted-break guard: continue-after-failure must be caught ----------------
    {
        RunState s;
        s.results[2] = kStageFail;
        StageCtx c = make_ctx(s);
        (void)bad_continue(3, kTable, c);
        expect_u("guard: a runner that continues after failure is detected",
                 (s.ran.size() == 4) ? 1u : 0u, 1u);
        expect_u("guard: ... and it reaches the stage the real runner must skip",
                 ran_last(s), 3);
    }

    // ---- T15 / §5.5: StageCtx ABI guard (substep pointer appended) ----------------
    {
        expect_u("StageCtx is 4 pointers after the substep append",
                 sizeof(StageCtx), 4 * sizeof(void *));
        expect_u("  log offset", offsetof(StageCtx, log), 0);
        expect_u("  dump offset", offsetof(StageCtx, dump_failure_state), sizeof(void *));
        expect_u("  user offset", offsetof(StageCtx, user), 2 * sizeof(void *));
        expect_u("  substep offset", offsetof(StageCtx, substep), 3 * sizeof(void *));
        expect_u("guard: the pre-extension 3-pointer layout is rejected",
                 (sizeof(StageCtx) == 3 * sizeof(void *)) ? 1u : 0u, 0u);
        expect_u("R1: StageDef.enabled defaults to nullptr", kStages[0].enabled == nullptr, 1);
    }

    // ---- R1: a table without enabled predicates keeps bringup_to semantics --------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("R1: no predicate means enabled", bringup_r3(3, kTable, c, &kHooks), kStageOk);
        expect_u("  stages executed", s.ran.size(), 4);
        expect_u("  no dump", s.dump_calls, 0);
    }

    // ---- R2: a disabled stage logs skip and continues without dump/latch ----------
    {
        RunState s;
        s.enabled[1] = false;
        StageCtx c = make_ctx(s);
        expect_u("R2: a skipped stage does not stop the ladder",
                 bringup_r3(3, kGateTable, c, &kHooks), kStageOk);
        expect_u("  stages executed", s.ran.size(), 3);
        expect_u("  stage 0 ran", ran_at(s, 0), 0);
        expect_u("  stage 2 ran after the skip", ran_at(s, 1), 2);
        expect_u("  stage 3 ran after the skip", ran_at(s, 2), 3);
        expect_u("  a skip never dumps", s.dump_calls, 0);
        expect_has("  skip is logged", s.log, "skip:", 1);
        expect_has("  skip is not a latch", s.log, "latched", 0);
        expect_u("  on_stage_start not called for the skip", s.started.size(), 3);
        expect_u("  started stage 0", s.started[0], 0);
        expect_u("  started stage 2", s.started[1], 2);
    }

    // ---- R5: stage 0 is never gated ------------------------------------------------
    {
        RunState s;
        s.enabled[0] = false;
        StageCtx c = make_ctx(s);
        expect_u("R5: stage 0 ignores a false predicate",
                 bringup_r3(0, kGateTable, c, &kHooks), kStageOk);
        expect_u("  stage 0 still ran", s.ran.size(), 1);
        expect_u("  and it is stage 0", ran_at(s, 0), 0);
        expect_has("  no skip log for stage 0", s.log, "skip:", 0);
    }
    {
        RunState s;
        s.enabled[0] = false;
        StageCtx c = make_ctx(s);
        expect_u("R5: stage 0 runs with later stages gated on",
                 bringup_r3(3, kGateTable, c, &kHooks), kStageOk);
        expect_u("  every stage ran", s.ran.size(), 4);
    }

    // ---- R3: failure dump -> latch -> on_stage_stop -> return ---------------------
    {
        RunState s;
        s.results[2] = kStageFail;
        StageCtx c = make_ctx(s);
        expect_u("R3: a failure returns its result",
                 bringup_r3(3, kGateTable, c, &kHooks), kStageFail);
        expect_u("  stops at the failing stage", s.ran.size(), 3);
        expect_u("  dump once", s.dump_calls, 1);
        expect_has("  latch is logged", s.log, "latched", 1);
        expect_u("  on_stage_stop once", s.stopped.size(), 1);
        expect_u("  stop index", s.stopped[0], 2);
        expect_u("  stop result", (uint64_t)s.stop_results[0], kStageFail);
        expect_u("  on_stage_start for 0..2", s.started.size(), 3);
        expect_u("  dump precedes stop",
                 s.events.find("dump;stop;") != std::string::npos, 1);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("R3: success does not call on_stage_stop",
                 bringup_r3(1, kGateTable, c, &kHooks), kStageOk);
        expect_u("  started 0..1", s.started.size(), 2);
        expect_u("  no stop on success", s.stopped.size(), 0);
    }

    // ---- R4: clamp logs requested and effective target ----------------------------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("R4: an over-large target runs the whole table",
                 bringup_r3(99, kGateTable, c, &kHooks), kStageOk);
        expect_u("  stages executed", s.ran.size(), 4);
        expect_has("  requested target logged", s.log, "target 99", 1);
        expect_has("  effective target logged", s.log, "using 3", 1);
    }

    // ---- null hooks / empty table -------------------------------------------------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("null hooks are safe", bringup_r3(2, kGateTable, c, nullptr), kStageOk);
        expect_u("  stages executed", s.ran.size(), 3);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        StageTable empty = { nullptr, 0 };
        expect_u("bringup_r3 on an empty table is a successful no-op",
                 bringup_r3(3, empty, c, &kHooks), kStageOk);
        expect_u("  nothing ran", s.ran.size(), 0);
    }

    // ---- D-01/F12: an explicit first index never re-runs the read-only prefix ------
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("D-01: from(2,3) returns ok",
                 bringup_r3_from(2, 3, kTable, c, &kHooks), kStageOk);
        expect_u("  only stages 2..3 ran", s.ran.size(), 2);
        expect_u("  first ran is stage 2", ran_at(s, 0), 2);
        expect_u("  second ran is stage 3", ran_at(s, 1), 3);
        expect_u("  no stage 0/1 ran", ran_last(s), 3);
        expect_u("  hooks started only 2..3", s.started.size(), 2);
        expect_u("  first start index", s.started[0], 2);
        expect_has("  skipped prefix is not logged as a skip", s.log, "skip:", 0);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("D-01: a target below the first index is a no-op",
                 bringup_r3_from(2, 1, kTable, c, &kHooks), kStageOk);
        expect_u("  nothing ran", s.ran.size(), 0);
        expect_u("  no start hook", s.started.size(), 0);
        expect_has("  the no-op is logged", s.log, "nothing to do", 1);
    }
    {
        RunState s;
        s.results[2] = kStageFail;
        StageCtx c = make_ctx(s);
        expect_u("D-01: a failure in the fired range still dumps/latches",
                 bringup_r3_from(2, 3, kTable, c, &kHooks), kStageFail);
        expect_u("  stopped at stage 2", s.ran.size(), 1);
        expect_u("  dump once", s.dump_calls, 1);
        expect_u("  stop hook fired", s.stopped.size(), 1);
        expect_u("  stop index", s.stopped.size() ? s.stopped[0] : 99u, 2);
    }
    {
        RunState s;
        StageCtx c = make_ctx(s);
        expect_u("D-01: bringup_r3 is from(0,...)",
                 bringup_r3(1, kTable, c, &kHooks), kStageOk);
        expect_u("  stage 0 still runs", ran_at(s, 0), 0);
        expect_u("  stage 1 runs", ran_at(s, 1), 1);
    }

    std::printf("\n%s: %d checks, %d failures\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
