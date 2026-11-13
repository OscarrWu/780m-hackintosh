// StageRunner.cpp — see StageRunner.h. Failure semantics mirror the base ladder:
// stop at the first non-kStageOk, dump diagnostics, never run later stages.
#include "StageRunner.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#endif

static void log_success(StageCtx &ctx, uint32_t index, const char *name)
{
    if (!ctx.log) return;
    char line[128];
    snprintf(line, sizeof line, "stage %u (%s) ok", index, name ? name : "?");
    ctx.log(ctx.user, line);
}

static void log_latched(StageCtx &ctx, uint32_t index, const char *name, StageResult result)
{
    if (!ctx.log) return;
    char line[160];
    // D-06/contract ⑤: FAIL format with reason, distinguishable from skip.
    snprintf(line, sizeof line, "stage %u (%s) FAIL result=%d; ladder latched",
             index, name ? name : "?", (int)result);
    ctx.log(ctx.user, line);
}

static void log_skipped(StageCtx &ctx, uint32_t index, const char *name)
{
    if (!ctx.log) return;
    char line[128];
    // D-06/contract ⑤: skip format with reason, distinguishable from FAIL.
    snprintf(line, sizeof line, "stage %u (%s) skip: disabled(gated)",
             index, name ? name : "?");
    ctx.log(ctx.user, line);
}

static void log_clamped(StageCtx &ctx, uint32_t requested, uint32_t effective)
{
    if (!ctx.log) return;
    char line[128];
    snprintf(line, sizeof line, "stage ladder: target %u past table end; using %u",
             requested, effective);
    ctx.log(ctx.user, line);
}

StageResult bringup_to(uint32_t target_stage, const StageTable &table, StageCtx &ctx)
{
    if (!table.stages || table.count == 0) {
        if (ctx.log) ctx.log(ctx.user, "stage ladder: empty table (nothing to do)");
        return kStageOk;
    }

    const uint32_t last = target_stage < table.count ? target_stage : table.count - 1;
    for (uint32_t i = 0; i <= last; i++) {
        const StageDef &def = table.stages[i];

        StageResult r = kStageFail;
        if (def.run) r = def.run(ctx);

        if (r != kStageOk) {
            if (ctx.dump_failure_state) ctx.dump_failure_state(ctx);
            log_latched(ctx, i, def.name, r);
            return r;
        }

        log_success(ctx, i, def.name);
    }

    return kStageOk;
}

StageResult bringup_r3(uint32_t target_stage, const StageTable &table, StageCtx &ctx,
                       const StageHooks *hooks)
{
    return bringup_r3_from(0u, target_stage, table, ctx, hooks);
}

StageResult bringup_r3_from(uint32_t first_stage, uint32_t target_stage,
                            const StageTable &table, StageCtx &ctx,
                            const StageHooks *hooks)
{
    if (!table.stages || table.count == 0) {
        if (ctx.log) ctx.log(ctx.user, "stage ladder: empty table (nothing to do)");
        return kStageOk;
    }

    const uint32_t last = target_stage < table.count ? target_stage : table.count - 1;
    if (target_stage > last) log_clamped(ctx, target_stage, last);
    // D-01/F12: an explicit first index skips the read-only prefix (survey/preflight).
    // A target below it is a no-op, never a backwards walk.
    if (first_stage > last) {
        if (ctx.log) {
            char line[128];
            snprintf(line, sizeof line,
                     "stage ladder: start %u past target %u (nothing to do)",
                     first_stage, last);
            ctx.log(ctx.user, line);
        }
        return kStageOk;
    }

    for (uint32_t i = first_stage; i <= last; i++) {
        const StageDef &def = table.stages[i];

        // R5: stage 0 (survey) is never gated by kill/latch predicates.
        if (i != 0 && def.enabled && !def.enabled(ctx)) {
            log_skipped(ctx, i, def.name);
            continue;
        }

        if (hooks && hooks->on_stage_start) hooks->on_stage_start(ctx, i);

        StageResult r = kStageFail;
        if (def.run) r = def.run(ctx);

        if (r != kStageOk) {
            if (ctx.dump_failure_state) ctx.dump_failure_state(ctx);
            log_latched(ctx, i, def.name, r);
            if (hooks && hooks->on_stage_stop) hooks->on_stage_stop(ctx, i, r);
            return r;
        }

        log_success(ctx, i, def.name);
    }

    return kStageOk;
}

uint32_t r3_gate_stage_of(R3GateIp ip)
{
    const uint32_t v = static_cast<uint32_t>(ip);
    return v < static_cast<uint32_t>(kR3GateIpCount) ? v + 1 : 0;
}

static uint32_t gate_dep_mask(uint32_t ip)
{
    switch (ip) {
    case kR3GateSmu:
        return r3_gate_bit(kR3GatePsp);
    case kR3GateGfx:
        return r3_gate_bit(kR3GateSmu);
    case kR3GateSdma:
        return r3_gate_bit(kR3GatePsp);
    case kR3GateMes:
        return r3_gate_bit(kR3GatePsp) | r3_gate_bit(kR3GateGfx);
    default:
        return 0;
    }
}

uint32_t r3_gate_enabled(uint32_t stage, uint32_t killed, uint32_t latched)
{
    const uint32_t s = stage < static_cast<uint32_t>(kR3GateIpCount)
        ? stage
        : static_cast<uint32_t>(kR3GateIpCount);
    uint32_t enabled = 0;

    // Topological order: every dependency bit is already final when an IP is evaluated.
    for (uint32_t ip = 0; ip < static_cast<uint32_t>(kR3GateIpCount); ip++) {
        const uint32_t bit = 1u << ip;
        if (s < r3_gate_stage_of(static_cast<R3GateIp>(ip))) continue;
        if (killed & bit) continue;
        if (latched & bit) continue;
        if ((enabled & gate_dep_mask(ip)) != gate_dep_mask(ip)) continue;
        enabled |= bit;
    }

    return enabled;
}
