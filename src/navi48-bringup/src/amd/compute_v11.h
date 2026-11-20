/*
 * compute_v11.h - Compute 11.0 bring-up substep (S13, stage 3 T-E).
 *
 * Scope: submit a compute indirect-buffer chain to the MES-managed compute queue
 * established by mes_queue_v11_0_substep:
 *   SET_UCONFIG_REG → WRITE_DATA (minimal form) → INDIRECT_BUFFER → RELEASE_MEM.
 * The IB chain stores a magic value and the substep verifies it via the fence
 * (poll < 1 ms).  This exercises the full PM4 submission path through the MES
 * scheduler: CP uconfig, WRITE_DATA to the test-buf page (D7), IB jump into the
 * IB preamble at D7 test_buf+0x800 (kComputeIbPreambleOff), and RELEASE_MEM
 * with EOP interrupt.  The D4 "csib" grant stays untouched for Gfx/RLC.
 *
 * Dependencies: mes_queue_v11_0_substep (S12) must have completed, making the
 * compute queue live and the MQD/ring/fence structures initialised.
 *
 * Register source: gc_11_0_0_offset.h, pm4_v11.h, mes_v11_api_def.h.
 * PM4 packet format: pm4_v11.h (cp_ucode_ucode_ucode_ucode.h in the Linux header).
 */
#ifndef NAVI48_COMPUTE_V11_H
#define NAVI48_COMPUTE_V11_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"

namespace amdgpu {

/* S13 compute substep: submit the minimal+full compute chain through the MES
 * compute queue.  The queue was set up by mes_queue_v11_0_substep (S12). */
R3SubstepResult compute_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);

} /* namespace amdgpu */

#endif /* NAVI48_COMPUTE_V11_H */
