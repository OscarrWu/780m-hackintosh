// BootArgFamily.cpp — see BootArgFamily.h.
#include "BootArgFamily.h"
#include "TargetConfig.h"

#include <pexpert/pexpert.h>
#include <string.h>

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#endif

static TgtDefaultPolicy gPolicy = { 0, 0, 0 };

TgtDefaultPolicy &tgt_default_policy()
{
    return gPolicy;
}

uint32_t tgt_boot_arg_default(const char *leaf)
{
    if (!leaf) return gPolicy.other;
    if (strcmp(leaf, "bringup") == 0) return gPolicy.bringup;
    if (strcmp(leaf, "stage") == 0) return gPolicy.stage;
    return gPolicy.other;
}

bool tgt_boot_arg_name(const char *leaf, char *out, size_t cap)
{
    if (!leaf || !*leaf || !out || cap == 0) return false;

    // The load gate keeps the upstream spelling "<family>bringup" (no hyphen,
    // Navi48Bringup.cpp:7329); every other leaf is "<family>-<leaf>".
    const int n = (strcmp(leaf, "bringup") == 0)
        ? snprintf(out, cap, "%s%s", TGT_BOOTARG_FAMILY, leaf)
        : snprintf(out, cap, "%s-%s", TGT_BOOTARG_FAMILY, leaf);
    return n > 0 && (size_t)n < cap;
}

bool tgt_boot_arg_u32(const char *leaf, uint32_t *out)
{
    if (!out) return false;

    char name[48];
    if (!tgt_boot_arg_name(leaf, name, sizeof name)) {
        *out = tgt_boot_arg_default(leaf);
        return false;
    }

    uint32_t parsed = 0;
    if (PE_parse_boot_argn(name, &parsed, sizeof parsed)) {
        *out = parsed;
        return true;
    }

    *out = tgt_boot_arg_default(leaf);
    return false;
}

bool tgt_boot_arg_present(const char *leaf)
{
    char name[48];
    if (!tgt_boot_arg_name(leaf, name, sizeof name)) return false;

    uint32_t unused = 0;
    return PE_parse_boot_argn(name, &unused, sizeof unused);
}

static bool is_r3_kill_leaf(const char *leaf)
{
    static const char *const kLeaves[] = { "psp", "smu", "gfx", "sdma", "mes" };
    if (!leaf) return false;
    for (size_t i = 0; i < sizeof kLeaves / sizeof kLeaves[0]; i++) {
        if (strcmp(leaf, kLeaves[i]) == 0) return true;
    }
    return false;
}

R3KillState r3_kill_state(const char *leaf)
{
    if (!is_r3_kill_leaf(leaf)) return kR3KillFollowStage;

    uint32_t value = 0;
    if (!tgt_boot_arg_u32(leaf, &value)) return kR3KillFollowStage;
    return value == 0 ? kR3KillOff : kR3KillOn;
}
