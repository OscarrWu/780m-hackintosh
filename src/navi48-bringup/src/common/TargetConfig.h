// TargetConfig.h — compile-time target constants and the one shared config structure (D2).
// The Makefile supplies -DTARGET_RDNA3=1 for the rdna3 target; the rdna4 target leaves it
// undefined, so the navi48 values below are the upstream-compatible default.
#ifndef N48_COMMON_TARGETCONFIG_H
#define N48_COMMON_TARGETCONFIG_H

#include <stddef.h>
#include <stdint.h>

#if defined(TARGET_RDNA3)
#define TGT_BOOTARG_FAMILY "n48r3"
#define TGT_LOG_PREFIX     "N48R3LOG"
#else
#define TGT_BOOTARG_FAMILY "navi48"
#define TGT_LOG_PREFIX     "N48LOG"
#endif

// Shared boot-arg defaults (fields consumed by BootArgFamily; see that header). This is
// the one structure both targets compile into common/, so it carries the P3 layout guard:
// the Linux cross-build does not promise Apple's bit alignment, and a silent padding
// change would make the two targets disagree about the defaults.
struct TgtDefaultPolicy {
    uint32_t bringup;   // <family>bringup: 0 = driver does not probe
    uint32_t stage;     // <family>-stage:   0 = read-only survey only
    uint32_t other;     // every other <family>-<leaf>: 0 = off
};
static_assert(sizeof(TgtDefaultPolicy) == 12, "shared layout drift: TgtDefaultPolicy");
static_assert(offsetof(TgtDefaultPolicy, bringup) == 0, "shared layout drift: TgtDefaultPolicy.bringup");
static_assert(offsetof(TgtDefaultPolicy, stage) == 4, "shared layout drift: TgtDefaultPolicy.stage");
static_assert(offsetof(TgtDefaultPolicy, other) == 8, "shared layout drift: TgtDefaultPolicy.other");

// Note: the fw-table enum is a single shared `amdgpu::FwId` (fw/fw_table.h, T1d) whose
// RDNA3 ids are the appended sub-range; it needs no per-target TGT_* selection here.

#endif // N48_COMMON_TARGETCONFIG_H
