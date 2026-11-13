// test_gating.cpp — T1 (36-cell stage x kill matrix, tech plan §5.3) and T3
// (recursive dependency closure, C0.1 + D8) for the pure r3_gate_enabled() function.
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 \
//       -I src/navi48-bringup/src/common \
//       src/navi48-bringup/src/common/tests/test_gating.cpp \
//       src/navi48-bringup/src/common/StageRunner.cpp -o /tmp/t1c_gating
//   /tmp/t1c_gating
//
// Golden values are derived from the documented DAG (psp->smu->gfx, psp->sdma,
// psp+gfx->mes), not from the implementation.
#include <cstdint>
#include <cstdio>

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

static const uint32_t P = r3_gate_bit(kR3GatePsp);
static const uint32_t M = r3_gate_bit(kR3GateSmu);
static const uint32_t G = r3_gate_bit(kR3GateGfx);
static const uint32_t D = r3_gate_bit(kR3GateSdma);
static const uint32_t E = r3_gate_bit(kR3GateMes);

static uint32_t enabled_at(uint32_t stage, uint32_t killed, uint32_t latched = 0)
{
    return r3_gate_enabled(stage, killed, latched);
}

int main()
{
    // ---- stage_of mapping (D9 candidate A) ----------------------------------------
    expect_u("stage_of psp", r3_gate_stage_of(kR3GatePsp), 1);
    expect_u("stage_of smu", r3_gate_stage_of(kR3GateSmu), 2);
    expect_u("stage_of gfx", r3_gate_stage_of(kR3GateGfx), 3);
    expect_u("stage_of sdma", r3_gate_stage_of(kR3GateSdma), 4);
    expect_u("stage_of mes", r3_gate_stage_of(kR3GateMes), 5);

    // ---- T1: 36 cells, rows = stage 0..5, columns = no kill / one IP killed -------
    // Independent golden table from tech plan §5.3 (D8-corrected; see the K0 self-review
    // for the one §5.3 cell that contradicts the §5.2 formula).
    static const uint32_t golden[6][6] = {
        /* stage 0 */ { 0, 0, 0, 0, 0, 0 },
        /* stage 1 */ { P, 0, P, P, P, P },
        /* stage 2 */ { P | M, 0, P, P | M, P | M, P | M },
        /* stage 3 */ { P | M | G, 0, P, P | M, P | M | G, P | M | G },
        /* stage 4 */ { P | M | G | D, 0, P | D, P | M | D, P | M | G, P | M | G | D },
        /* stage 5 */ { P | M | G | D | E, 0, P | D, P | M | D, P | M | G | E, P | M | G | D },
    };
    static const uint32_t kill_col[6] = { 0, P, M, G, D, E };
    static const char *const col_name[6] = { "none", "psp=0", "smu=0", "gfx=0", "sdma=0", "mes=0" };

    for (uint32_t stage = 0; stage <= 5; stage++) {
        for (uint32_t col = 0; col < 6; col++) {
            char what[96];
            std::snprintf(what, sizeof what, "T1 matrix stage %u / %s", stage, col_name[col]);
            expect_u(what, enabled_at(stage, kill_col[col]), golden[stage][col]);
        }
    }

    // ---- T3: dependency closure, including the D8 mes->gfx edge -------------------
    expect_u("T3 psp killed closes every successor", enabled_at(5, P), 0);
    expect_u("T3 smu killed closes gfx (and mes via gfx)", enabled_at(5, M), P | D);
    expect_u("T3 gfx killed closes mes only", enabled_at(5, G), P | M | D);
    expect_u("T3 sdma killed closes sdma only", enabled_at(5, D), P | M | G | E);
    expect_u("T3 mes killed closes mes only", enabled_at(5, E), P | M | G | D);
    expect_u("T3 gfx kill keeps mes off (D8 edge)", enabled_at(5, G) & E, 0);
    expect_u("T3 smu kill keeps mes off through gfx (D8 edge)", enabled_at(5, M) & E, 0);

    // Latch uses the same closure rule (F6: latch is an enabled() input).
    expect_u("T3 latched psp closes every successor", enabled_at(5, 0, P), 0);
    expect_u("T3 latched smu closes gfx+mes", enabled_at(5, 0, M), P | D);
    expect_u("T3 latched gfx closes mes", enabled_at(5, 0, G), P | M | D);
    expect_u("T3 latched mes closes mes only", enabled_at(5, 0, E), P | M | G | D);

    // kill and latch compose (fail closed).
    expect_u("T3 psp killed + mes latched", enabled_at(5, P, E), 0);
    expect_u("T3 gfx killed + sdma latched", enabled_at(5, G, D), P | M);
    expect_u("T3 smu killed at stage 2 keeps only psp", enabled_at(2, M), P);

    // ---- clamp: S = min(stage, 5) -------------------------------------------------
    expect_u("S clamps above 5 (99 == 5)", enabled_at(99, 0), enabled_at(5, 0));
    expect_u("S=0 enables no IP", enabled_at(0, 0), 0);

    std::printf("\n%s: %d checks, %d failures\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
