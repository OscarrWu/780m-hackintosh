/*
 * rdna3_substep.cpp - the heap-free implementation half of rdna3_substep.h.
 *
 * One function: the evidence line append, which forwards every "<tag>: <text>" line to
 * the shared n48log ring (amd/n48log.h -- the 6 KiB per-instance arena was retired, task
 * 11-10-stage2-evidence-n48log design §7.4). It touches no register, allocates nothing,
 * and keeps no state of its own.
 *
 * Kernel build constraints honoured here (the same source compiles for the kext, which uses
 * -nostdinc): only <stdint.h>/<stddef.h> plus libkern's snprintf/vsnprintf. The host runners
 * add -include stdio.h -include string.h (see rdna3/tests/run_host_tests.sh).
 */
#include "rdna3_substep.h"

#include "n48log.h"
#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>   /* snprintf/vsnprintf/strlen/memcpy, no libc under -nostdinc */
#else
#include <stdio.h>             /* host runner: same names from libc */
#include <string.h>
#endif


const char *r3_substep_section_name(R3SubstepSection s)
{
    switch (s) {
    case kR3SecPre: return "pre";
    case kR3SecSeq: return "seq";
    case kR3SecChk: return "chk";
    case kR3SecEv:  return "ev";
    }
    return "?";
}

bool r3_ev_add(R3EvidenceLines *ev, const char *tag, const char *text)
{
    (void)ev;
    if (!text)
        return false;
    amdgpu::n48_logf("%s: %s", tag ? tag : "-", text);
    return true;
}

