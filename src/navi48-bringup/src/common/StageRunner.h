// StageRunner.h — data-driven stage ladder shared by future targets (D4).
// The rdna4 ladder in amd/amdgpu_init.cpp is intentionally not migrated in phase 1;
// this is the one implementation the rdna3 target (and later targets) use.
#ifndef N48_COMMON_STAGERUNNER_H
#define N48_COMMON_STAGERUNNER_H

#include <stddef.h>
#include <stdint.h>

enum StageResult { kStageOk = 0, kStageFail = 1, kStageSkip = 2 };

struct StageCtx;

// The only injected state a stage may use: a log sink, the failure-dump hook, the
// target-owned user pointer, and the opaque per-substep context (appended for the
// phase-2 five-substep ladder; the rdna3 owner is wired by the integration card and
// deliberately NOT named here so common/ never includes rdna3_runtime.h).
struct StageCtx {
    void (*log)(void *user, const char *line);
    void (*dump_failure_state)(StageCtx &ctx);
    void *user;
    void *substep = nullptr;
};
static_assert(sizeof(StageCtx) == 4 * sizeof(void *), "shared layout drift: StageCtx");
static_assert(offsetof(StageCtx, substep) == 3 * sizeof(void *),
              "shared layout drift: StageCtx.substep");

struct StageDef {
    const char *name;
    StageResult (*run)(StageCtx &ctx);
    // Appended (R1): nullptr means "enabled", so every pre-extension table keeps its
    // exact behaviour. A false result logs "skip:", dumps nothing, latches nothing.
    bool (*enabled)(const StageCtx &ctx) = nullptr;
};

struct StageTable {
    const StageDef *stages;
    uint32_t count;
};

// Teardown/ownership hooks for bringup_r3(). on_stage_start runs immediately before an
// enabled stage body; on_stage_stop runs only when a stage stops the ladder (non-Ok),
// after dump + latch, matching the phase-2 R3 ordering. Neither is called for a skip.
struct StageHooks {
    void (*on_stage_start)(StageCtx &ctx, uint32_t index);
    void (*on_stage_stop)(StageCtx &ctx, uint32_t index, StageResult result);
};

// Run stages 0..target_stage in order (indices past target_stage are skipped; a target
// past the end clamps to the last stage). Any non-kStageOk result calls the dump hook,
// records the latch through the log hook, stops immediately and returns that result.
StageResult bringup_to(uint32_t target_stage, const StageTable &table, StageCtx &ctx);

// Phase-2 ladder entry (D9/R1-R5). Same traversal and clamp as bringup_to(), plus:
//   - a StageDef.enabled predicate returning false logs "skip:" and continues with Ok,
//     without dumping or latching (R2);
//   - a non-Ok run dumps, latches, calls hooks->on_stage_stop, then returns (R3);
//   - stage 0 is never gated by enabled() (R5);
//   - a clamped target is logged with both the requested and effective value (R4).
// hooks may be null. bringup_to() keeps its original semantics unchanged.
StageResult bringup_r3(uint32_t target_stage, const StageTable &table, StageCtx &ctx,
                       const StageHooks *hooks);

// D-01/F12: the trigger fires the already-armed substeps and must not re-run the read-only
// stages. bringup_r3_from() is bringup_r3() with an explicit first index; a target below
// first_stage is a logged no-op, so a stale trigger cannot walk the ladder backwards.
// bringup_r3() is exactly bringup_r3_from(0, ...); its semantics are unchanged.
StageResult bringup_r3_from(uint32_t first_stage, uint32_t target_stage,
                            const StageTable &table, StageCtx &ctx,
                            const StageHooks *hooks);

// ---- dependency gating (pure, no R3Rt / no I/O; the integration card feeds bitmaps) ----
// IP order is the topological evaluation order psp -> smu -> gfx -> sdma -> mes.
enum R3GateIp {
    kR3GatePsp = 0,
    kR3GateSmu = 1,
    kR3GateGfx = 2,
    kR3GateSdma = 3,
    kR3GateMes = 4,
    kR3GateIpCount = 5,
};

constexpr uint32_t r3_gate_bit(R3GateIp ip)
{
    return 1u << static_cast<uint32_t>(ip);
}

// stage_of(psp..mes) = 1..5 (D9 candidate A).
uint32_t r3_gate_stage_of(R3GateIp ip);

// enabled(ip) = (S >= stage_of(ip)) && !killed(ip) && !latched(ip)
//               && every dependency enabled, with S = min(stage, 5) and the DAG
//               smu->psp, gfx->smu, sdma->psp, mes->psp+gfx (C0.1 + D8).
// killed/latched are bitmaps indexed by R3GateIp; the result is the enabled bitmap.
uint32_t r3_gate_enabled(uint32_t stage, uint32_t killed, uint32_t latched);

#endif // N48_COMMON_STAGERUNNER_H
