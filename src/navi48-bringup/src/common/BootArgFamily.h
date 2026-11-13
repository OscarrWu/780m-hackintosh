// BootArgFamily.h — one boot-arg parser for both targets (D2/a2).
// rdna3 reads "<family>-<leaf>" (and the documented load gate "<family>bringup", no
// hyphen, mirroring the upstream "navi48bringup"). navi48 stock call sites are NOT
// migrated; they keep their direct PE_parse_boot_argn calls.
#ifndef N48_COMMON_BOOTARGFAMILY_H
#define N48_COMMON_BOOTARGFAMILY_H

#include "TargetConfig.h"

#include <stddef.h>
#include <stdint.h>

// TgtDefaultPolicy lives in TargetConfig.h next to its P3 layout guard. Defaults are
// centralized there so a host test can mutate one field and observe the resulting
// behaviour instead of re-deriving defaults from scattered call sites.

// Mutable so the host tests can flip a default (planted-break hook); the kext leaves
// it at its initialized value.
TgtDefaultPolicy &tgt_default_policy();

// The conservative default for this leaf. Unknown leaves use policy.other.
uint32_t tgt_boot_arg_default(const char *leaf);

// Exact name this layer reads for leaf (no allocation). leaf == "bringup" maps to
// "<family>bringup" (no hyphen); everything else maps to "<family>-<leaf>".
// Returns false when leaf is empty or the name does not fit.
bool tgt_boot_arg_name(const char *leaf, char *out, size_t cap);

// Parse "<family>-<leaf>" (or "<family>bringup"). On a miss *out receives the central
// default and the return value is false; on success the parsed value is written and
// the return value is true. *out must not be null.
bool tgt_boot_arg_u32(const char *leaf, uint32_t *out);

// True when the name exists in the boot-args, independent of its value.
bool tgt_boot_arg_present(const char *leaf);

// Three-state kill switch for the five phase-2 IP leaves (D10/F2): absent = follow the
// stage ladder, present 0 = off, present non-zero = on. Only psp/smu/gfx/sdma/mes are
// whitelisted; any other leaf (including null) returns FollowStage with no PE access.
// This is deliberately separate from tgt_boot_arg_u32()/TgtDefaultPolicy: their
// "absent = policy.other (0)" semantics stay untouched for the RDNA4 regression gate.
enum R3KillState {
    kR3KillFollowStage = 0,
    kR3KillOff = 1,
    kR3KillOn = 2,
};

R3KillState r3_kill_state(const char *leaf);

#endif // N48_COMMON_BOOTARGFAMILY_H
