/*
 * rdna3_teardown.h - reverse-order teardown after a failed or stopped RDNA3 power-up
 * (tech plan §9, requirement F5).
 *
 * Rules (T1-T6):
 *   T1 triggered by a failure or a stop while at least one non-PSP IP was started;
 *   T2 strict reverse power-up order mes -> sdma -> gfx -> smu, started IPs only;
 *   T3 no GPU reset (the GOP display must survive);
 *   T4 PSP is left alive (no inverse sequence exists in either source - never invented);
 *   T5 an IP disabled by kill switch has no teardown (it never started);
 *   T6 teardown's own worst form is a quiet failure with a complete log: every write is
 *      bounded by a readback, no write is retried, and one failed action does not abort
 *      the remaining IPs.
 *
 * The "started" set is rt->ready_bits plus the failed substep itself: a substep that
 * failed after writing its first register was started even though its ready bit is clear
 * (§9: "failed IPs are torn down, never-started IPs are not").
 *
 * Writes use the SAME whitelist table as power-up, with ctx "teardown:<ip>". D-09: the
 * used-register registry ONLY REUSES rows the power-up substep already registered — it
 * never adds new rows. When a row is absent (the substep did not write that register),
 * the action is skipped and "no reversible object" is logged:
 *
 * Evidence: "<tag>: <text>" lines in the caller arena plus "N48R3EVID-teardown".
 */
#ifndef N48_RDNA3_TEARDOWN_H
#define N48_RDNA3_TEARDOWN_H

#include <stdint.h>

#include "rdna3_runtime.h"
#include "rdna3_substep.h"

struct R3TeardownEnv {
    const R3Io *io;            /* register access; null => quiet skip                    */
    const R3SubstepEnv *sub;   /* rt (ready bits), ev, sink, ip table                    */
    bool failed;               /* true: failed_ip stopped the ladder                     */
    R3Ip failed_ip;            /* only read when failed                                  */
};

struct R3TeardownReport {
    bool triggered;      /* teardown was entered (a non-PSP IP had started)             */
    bool psp_failed;     /* PSP failure: stop + snapshot, no inverse                        */
    uint32_t started;    /* mese bits: ready | failed                                   */
    uint32_t acted;      /* IPs whose teardown action ran (Mes/Sdma)                    */
    uint32_t none;       /* IPs recorded as no-action (Gfx/Smu)                         */
    uint32_t skipped;    /* IPs never started (including PSP)                           */
    uint32_t write_fail; /* IPs whose action reported a quiet failure                   */
    uint32_t writes;     /* allowed register writes performed                          */
};

R3TeardownReport r3_teardown_run(const R3TeardownEnv *env);

/* "psp" / "smu" / "gfx" / "sdma" / "mes" (static). */
const char *r3_teardown_ip_name(R3Ip ip);

#endif /* N48_RDNA3_TEARDOWN_H */
