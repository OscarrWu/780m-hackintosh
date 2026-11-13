/*
 * rdna3_runtime.h - the per-instance RDNA3 bringup runtime block (T16, D9).
 *
 * This is the state the five power-up substeps share: the layout plan computed before any
 * write, the per-IP ready/latch bits, and the current substep context. common/ must never
 * include this file (StageRunner.h carries only an opaque `void *substep` for that reason);
 * the integration card owns the pointer wiring.
 *
 * Size discipline (tech plan §10 T16): the block lives in the driver instance and may be
 * copied for a failure snapshot, so it is a fixed-size POD with no heap and no variable
 * length data. The static_asserts below are the contract the host test pins (test_layout.cpp:
 * offsetof(R3Rt, layout) == 0, sizeof(R3SubstepCtx) == 16).
 */
#ifndef N48_RDNA3_RUNTIME_H
#define N48_RDNA3_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "rdna3_layout.h"

/* Power-up IP ids. Bit positions of R3Rt::ready_bits / latch_bits. */
enum class R3Ip {
    Psp = 0,
    Smu = 1,
    Gfx = 2,
    Sdma = 3,
    Mes = 4
};

#define R3_IP_COUNT 5u
#define R3_IP_BIT(ip) (1u << static_cast<unsigned>(ip))

/* Substep context flags. */
#define R3_SUBSTEP_RECHECK_USED 0x1u /* the one permitted SMU re-check was used (D11) */

/* Static size ceiling of the runtime block (T16): plan (<= R3_LAYOUT_MAX_BYTES) plus the two
 * 32-bit bitmaps and the 16-byte substep context, with one word of slack. */
#define R3_RT_MAX_BYTES (R3_LAYOUT_MAX_BYTES + 64u)

/* One substep of the five-substep ladder (§2.3): which IP, which stage, which in-IP step. */
struct R3SubstepCtx {
    R3Ip ip;            /* R3Ip value                                  */
    uint16_t stage;     /* outer stage number (0..5)                   */
    uint16_t step;      /* step inside the substep                     */
    uint32_t reserved;  /* explicit pad: sizeof == 16, stage at offset 4 */
    uint32_t flags;     /* R3_SUBSTEP_*                                */
};

/* The shared runtime block. `layout` is first so a pointer to R3Rt is also a plan pointer. */
struct R3Rt {
    R3LayoutPlan layout;
    uint32_t ready_bits;  /* R3_IP_BIT(R3Ip::X): substep X completed                         */
    uint32_t latch_bits;  /* R3_IP_BIT(R3Ip::X): X (and successors) latched off this boot   */
    R3SubstepCtx substep;
};

static_assert(offsetof(R3Rt, layout) == 0u, "R3Rt.layout must be the first member");
static_assert(sizeof(R3SubstepCtx) == 16u, "R3SubstepCtx layout drift");
static_assert(offsetof(R3SubstepCtx, stage) == 4u, "R3SubstepCtx.stage offset drift");
static_assert(sizeof(R3Rt) <= R3_RT_MAX_BYTES, "R3Rt exceeds the kernel-stack ceiling (T16)");

#endif /* N48_RDNA3_RUNTIME_H */
