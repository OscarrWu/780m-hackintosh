/*
 * rdna3_dump.h - the RDNA3 stage-2 failure dump (tech plan §8.4, requirement F4).
 *
 * Stage 1's dump reads the always-on GCVM/MMVM fault registers and snapshots the IH ring.
 * Stage 2 must stay useful when the diagnosed block is NOT initialised: the available read
 * set is declared per failing substep, IH draining is REMOVED (stage 2 never initialises
 * IH, C0.6), and every group is guarded by a readability probe: a read that cannot be
 * trusted (bus failure or the 0xFFFFFFFF sentinel) abandons the whole group and records
 * "dump:skip:<obj>". No polling, no new protocol interaction, no write of any kind.
 *
 * Availability (tech plan §8.4 table):
 *   psp          C2PMSG group (MP0), PSP fence buffer (BAR0), GCVM + MMVM L2 fault
 *   smu          MP1 mailbox registers, GCVM L2 fault
 *   gfx          IMU/RLC/CP/GRBM state, GCVM + MMVM L2 fault
 *   sdma         SDMA0 status/EDC/RB state, GCVM + MMVM L2 fault
 *   mes          CP_MES/CP_HQD/RLC_CP_SCHEDULERS/GRBM state, GCVM + MMVM L2 fault
 *
 * Output: one chunked property per consulted group, "N48R3EVID-dump-<group>" (group =
 * psp|psp-fence|smu|gfx|sdma|mes|gcvm|mmvm), plus "N48R3EVID-dump-summary". An
 * unreadable probe writes "dump:skip:<obj>" into that group's property; a read failure
 * after a successful probe writes "dump:abort:<obj>" and stops only that group. Every
 * line is also appended to the caller-owned evidence arena (tag "dump") so the main log
 * carries the same material. The publisher copies the bytes (LogChannel convention).
 *
 * The dump runs after a substep has returned, so the R3Io lock is free; it takes the lock
 * once around the whole snapshot to serialise against any other MMIO user. It never waits.
 */
#ifndef N48_RDNA3_DUMP_H
#define N48_RDNA3_DUMP_H

#include <stdint.h>

#include "rdna3_runtime.h"
#include "rdna3_substep.h"

struct R3DumpEnv {
    const R3Io *io;            /* register/BAR0 access; may be null (records a skip)   */
    const R3SubstepEnv *sub;   /* rt (layout), ev (line arena), sink, ip table         */
    bool has_failed_ip;        /* true: failed_ip names the substep that stopped       */
    R3Ip failed_ip;            /* only read when has_failed_ip                         */
};

/* One snapshot. Never writes, never polls, never returns a failure code: its own worst
 * form is "quiet skip + complete log" (F4/F5). */
void r3_dump_failure_state(const R3DumpEnv *env);

/* "psp" / "smu" / "gfx" / "sdma" / "mes" (static). */
const char *r3_dump_ip_name(R3Ip ip);

#endif /* N48_RDNA3_DUMP_H */
