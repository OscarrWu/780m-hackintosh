/*
 * pm4_v11.h - PM4 TYPE-3 packet builders + submit tail (stage 3, §8).
 *
 * Pure functions: every builder writes a dword array and returns the packet
 * length in dwords (0 on impossible input). No I/O, no global state.
 *
 * Sources:
 *   - Packet opcodes/fields: nullarc evidence nvd.h (R9) + Linux
 *     drivers/gpu/drm/amd/amdgpu/nvd.h; packet usage verified against
 *     nullarc PhxBringup.cpp/mesboot.c (R10 §6/§8/§9) and Linux
 *     gfx_v11_0.c.
 *   - Submit tail: nullarc PhxBringup.cpp:766-838 gpuSubmit (R10 §8.3).
 *
 * Canonical VA trap (R10 §10.4): an upper-half IB address (bit 47 set)
 * with a bare 16-bit high word silently wedges the queue. INDIRECT_BUFFER
 * sign-extends through pm4_va_canonical (nullarc :801); WRITE_DATA
 * destinations are VM addresses the caller must pass canonical
 * (pm4_va_canonical is public for that); RELEASE_MEM fence addresses are
 * MC-aperture forms passed raw (nullarc :809-814 - no extension).
 */

#ifndef NAVI48_PM4_V11_H
#define NAVI48_PM4_V11_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"

namespace amdgpu {

/* ---- opcodes (nvd.h; R9 table) ---------------------------------------- */

constexpr uint32_t kPm4Type3 = 3u;
constexpr uint32_t kPm4OpWriteData      = 0x37;
constexpr uint32_t kPm4OpIndirectBuffer = 0x3F;
constexpr uint32_t kPm4OpReleaseMem     = 0x49;
constexpr uint32_t kPm4OpAcquireMem     = 0x58;
constexpr uint32_t kPm4OpSetUconfigReg  = 0x79;

/* PACKET3(op, n): TYPE3<<30 | op<<8 | count<<16 (nvd.h:48). The count field
 * is the number of body dwords after word0 minus one (total = count + 2). */
constexpr uint32_t kPm4PacketTypeShift  = 30;
constexpr uint32_t kPm4PacketOpShift    = 8;
constexpr uint32_t kPm4PacketCountShift = 16;

/* ---- field masks (nvd.h) ---------------------------------------------- */

/* WRITE_DATA (nvd.h:106-157). */
constexpr uint32_t kPm4WdDstSelShift  = 8;
constexpr uint32_t kPm4WdDstSelMemory = 5; /* memory async direct */
constexpr uint32_t kPm4WdWrConfirm    = 1u << 20;

/* INDIRECT_BUFFER (nvd.h:224-250). */
constexpr uint32_t kPm4IbValid     = 1u << 23;
constexpr uint32_t kPm4IbVmidShift = 24;
constexpr uint32_t kPm4IbVmidMask  = 0xFu << 24;

/* RELEASE_MEM (nvd.h:354-390). */
constexpr uint32_t kPm4RmEventIndexShift  = 8;
constexpr uint32_t kPm4RmGlmWb            = 1u << 12;
constexpr uint32_t kPm4RmGlmInv           = 1u << 13;
constexpr uint32_t kPm4RmGl2Inv           = 1u << 20;
constexpr uint32_t kPm4RmGl2Wb            = 1u << 21;
constexpr uint32_t kPm4RmSeq              = 1u << 22;
constexpr uint32_t kPm4RmCachePolicyShift = 25;
constexpr uint32_t kPm4RmDataSelShift     = 29;
constexpr uint32_t kPm4RmDataSel64        = 2u; /* 64-bit fence value */

/* ACQUIRE_MEM GCR_CNTL (nvd.h:447-501). */
constexpr uint32_t kPm4GcrGl2Inv = 1u << 14;
constexpr uint32_t kPm4GcrGl2Wb  = 1u << 15;

/* ---- canonical VA ------------------------------------------------------ */

/* Sign-extend a 48-bit VM address into canonical form: bit 47 set ->
 * | 0xFFFF000000000000 (nullarc PhxBringup.cpp:64-67/:801; Linux
 * amdgpu_gmc_sign_extend, amdgpu_gmc.h:36-39/:408-409, ORs HOLE_END
 * instead - see the report; the nullarc form is the same-machine
 * evidence). */
uint64_t pm4_va_canonical(uint64_t va);

/* ---- packet builders ---------------------------------------------------
 * Each returns the packet length in dwords (words written), 0 on invalid
 * input. out must hold at least the stated capacity. */

/* WRITE_DATA: 5 dwords (0xC0033700 form, nullarc mesboot.c:227 area).
 * dst_va is a VM address; pass it canonical (see pm4_va_canonical). */
uint32_t pm4_write_data(uint32_t out[5], uint64_t dst_va, uint32_t data);

/* INDIRECT_BUFFER: 4 dwords. compute selects VALID=1<<23 (Linux
 * gfx_v11_0.c:6036); the gfx path omits it (:6006). va is sign-extended
 * here. */
uint32_t pm4_indirect_buffer(uint32_t out[4], uint64_t ib_va,
                             uint32_t length_dw, uint32_t vmid, bool compute);

/* RELEASE_MEM (fence): 8 dwords, the nullarc form (PhxBringup.cpp:807-826) —
 * TYPE3 count=body-1, packet3(op,6) carries 8 dwords; the trailing 0 pads the
 * body to the count (Linux gfx_v11_0.c emit_fence :6094 writes it explicitly).
 * out[0]=0xC0064900,1=0x06603514,2=0x40000000(DATA_SEL64),3/4=fence lo/hi,
 * 5=seq,6=0,7=0. Fails (0) when fence_mc is not 8B-aligned.  The fence is an
 * MC-aperture address passed raw. */
uint32_t pm4_release_mem(uint32_t out[8], uint64_t fence_mc, uint32_t seq);

/* ACQUIRE_MEM (GL2 writeback+invalidate), the nullarc form (R10 §9):
 * 0xC0065800,0,0xFFFFFFFF,0x00FFFFFF,0,0,0x0A,0xC000. */
uint32_t pm4_acquire_mem(uint32_t out[8]);

/* SET_UCONFIG_REG: 2 body dwords - window-relative register offset + value.
 * nullarc: 0xC0017900,0x40,0xDEADBEEF (SCRATCH_REG0, R10 §9). */
uint32_t pm4_set_uconfig_reg(uint32_t out[3], uint32_t reg_off, uint32_t value);

/* ---- submit tail (R10 §8.3, nullarc PhxBringup.cpp:827-838) ------------
 *
 * Order after the packets are in the ring and the caller has advanced its
 * wptr and recorded fseq/fend:
 *   sfence -> read the ring through the carveout window (drain the WC
 *   buffer) -> hdpFlush (abort on false) -> write the 64-bit wptr to the
 *   WB page at +16 -> sfence -> read it back -> hdpFlush (abort on false)
 *   -> fence -> ring the doorbell: byte offset = index*4, 64-bit value =
 *   wptr (dword count).
 *
 * No writeguard classify here: this is the runtime submit path; window
 * registration is the stage's/caller's responsibility.
 * Returns true when the doorbell was rung; false = aborted at an hdpFlush
 * (or a failed drain read) - the caller must not treat the submit as
 * delivered.
 */
bool pm4_submit_tail(const R3Io *io, uint64_t ring_off, uint64_t wb_off,
                     uint32_t doorbell_index, uint64_t wptr);

} /* namespace amdgpu */

#endif /* NAVI48_PM4_V11_H */
