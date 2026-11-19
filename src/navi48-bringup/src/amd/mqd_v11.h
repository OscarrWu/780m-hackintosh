/*
 * mqd_v11.h - GFX11 compute/gfx MQD field construction pure functions.
 *
 * These functions build MQD (Micro-Queue Descriptor) dword arrays for compute
 * and GFX queues. They are pure (no I/O), host-testable, and mirror the Linux
 * gfx_v11_0_compute_mqd_init (gfx_v11_0.c:4312-4455) and
 * gfx_v11_0_gfx_mqd_init (gfx_v11_0.c:4139-4234) field sets.
 *
 * Every dword index and value carries its Linux provenance.
 *
 * Address conventions:
 *   - All addresses (mqd, ring, wb, eop) are MC (Memory Controller) aperture
 *     addresses (0x80_xxxxxxxx for VRAM).
 *   - doorbell is a dword index (BAR2 byte offset = index * 4).
 *
 * Kernel queue semantics:
 *   PRELOAD_SIZE=0x55, EOP_SIZE=8, PQ_CONTROL bits KMD|PRIV|UNORD|NO_UPDATE_RPTR
 *   are kernel queue (KIQ/MES) semantics. User-queue ports must re-derive from
 *   Linux gfx_v11_0_compute_mqd_init with kernel_queue=false.
 *   (nullarc R10 §5 evidence, mesboot.c mqd_build)
 *
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c (Copyright 2019 Advanced Micro Devices,
 * Inc., MIT licensed - see NOTICE). Only the register definitions and dword
 * indices are reproduced; no Linux code is copied verbatim.
 */
#ifndef NAVI48_MQD_V11_H
#define NAVI48_MQD_V11_H

#include <stdint.h>

namespace amdgpu {

/* Compute MQD dword layout indices (v11_compute_mqd struct offsets). */
constexpr uint32_t kMqdCmpHeader              = 0u;
constexpr uint32_t kMqdCmpPipelineStatEnable  = 11u;
constexpr uint32_t kMqdCmpThreadMgmtSe0       = 23u;
constexpr uint32_t kMqdCmpThreadMgmtSe1       = 24u;
constexpr uint32_t kMqdCmpThreadMgmtSe2       = 26u;
constexpr uint32_t kMqdCmpThreadMgmtSe3       = 27u;
constexpr uint32_t kMqdCmpMiscReserved        = 32u;
constexpr uint32_t kMqdCmpMqdBaseLo           = 128u;
constexpr uint32_t kMqdCmpMqdBaseHi           = 129u;
constexpr uint32_t kMqdCmpHqdActive           = 130u;
constexpr uint32_t kMqdCmpHqdVmid             = 131u;
constexpr uint32_t kMqdCmpPersistentState     = 132u;
constexpr uint32_t kMqdCmpQuantum             = 135u;
constexpr uint32_t kMqdCmpPqBaseLo            = 136u;
constexpr uint32_t kMqdCmpPqBaseHi            = 137u;
constexpr uint32_t kMqdCmpPqRptr              = 138u;
constexpr uint32_t kMqdCmpRptrReportLo        = 139u;
constexpr uint32_t kMqdCmpRptrReportHi        = 140u;
constexpr uint32_t kMqdCmpWptrPollLo          = 141u;
constexpr uint32_t kMqdCmpWptrPollHi          = 142u;
constexpr uint32_t kMqdCmpDoorbellControl     = 143u;
constexpr uint32_t kMqdCmpPqControl           = 145u;
constexpr uint32_t kMqdCmpIbControl           = 149u;
constexpr uint32_t kMqdCmpIqTimer             = 150u;
constexpr uint32_t kMqdCmpMqdControl          = 162u;
constexpr uint32_t kMqdCmpEopBaseLo           = 165u;
constexpr uint32_t kMqdCmpEopBaseHi           = 166u;
constexpr uint32_t kMqdCmpEopControl          = 167u;
constexpr uint32_t kMqdCmpPqWptrLo            = 182u;
constexpr uint32_t kMqdCmpPqWptrHi            = 183u;

/* GFX MQD dword layout indices (v11_gfx_mqd struct offsets). */
constexpr uint32_t kMqdGfxCpMqdBaseLo         = 128u;
constexpr uint32_t kMqdGfxCpMqdBaseHi         = 129u;
constexpr uint32_t kMqdGfxHqdActive           = 130u;
constexpr uint32_t kMqdGfxHqdVmid             = 131u;
constexpr uint32_t kMqdGfxQueuePriority       = 134u;
constexpr uint32_t kMqdGfxQuantum             = 135u;
constexpr uint32_t kMqdGfxHqdBaseLo           = 136u;
constexpr uint32_t kMqdGfxHqdBaseHi           = 137u;
constexpr uint32_t kMqdGfxHqdRptr             = 138u;
constexpr uint32_t kMqdGfxRptrAddrLo          = 139u;
constexpr uint32_t kMqdGfxRptrAddrHi          = 140u;
constexpr uint32_t kMqdGfxWptrPollLo          = 141u;
constexpr uint32_t kMqdGfxWptrPollHi          = 142u;
constexpr uint32_t kMqdGfxDoorbellControl     = 143u;
constexpr uint32_t kMqdGfxHqdCntl             = 145u;
constexpr uint32_t kMqdGfxHqdWptrLo           = 149u;
constexpr uint32_t kMqdGfxHqdWptrHi           = 150u;
constexpr uint32_t kMqdGfxMqdControl          = 162u;

/* Compute MQD default/constant values. */
constexpr uint32_t kMqdHeader                 = 0xC0310800u;
constexpr uint32_t kMqdPipelineStatEnable     = 0x00000001u;
constexpr uint32_t kMqdThreadMgmtAll          = 0xFFFFFFFFu;
constexpr uint32_t kMqdMiscReserved           = 0x00000007u;
constexpr uint32_t kMqdHqdActive              = 1u;
constexpr uint32_t kMqdHqdVmid                = 0u;

/* CP_HQD_PERSISTENT_STATE default / PRELOAD_SIZE=0x55 */
constexpr uint32_t kMqdPersistentDefault      = 0x0be05501u;

/* CP_HQD_PQ_CONTROL default: 0x00308509 with KMD|PRIV|UNORD|NO_UPDATE_RPTR set.
 * QUEUE_SIZE and RPTR_BLOCK_SIZE are ring-size-dependent (see mqd_compute_pq_control). */
constexpr uint32_t kMqdPqControlBase          = 0x00308509u;

/* CP_HQD_IB_CONTROL default (MIN_IB_AVAIL_SIZE=3). */
constexpr uint32_t kMqdIbControlDefault       = 0x00300000u;

/* CP_MQD_CONTROL default: VMID=0. */
constexpr uint32_t kMqdControlDefault         = 0x00000100u;

/* CP_HQD_EOP_CONTROL default; EOP_SIZE is ring-dependent. */
constexpr uint32_t kMqdEopControlDefault      = 0x00000006u;
constexpr uint32_t kMqdEopSizeMask            = 0x0000003fu;
constexpr uint32_t kMqdEopSizeShift           = 0u;

/* GFX CP_GFX_HQD_QUANTUM default (QUANTUM_EN=1). */
constexpr uint32_t kMqdGfxQuantumDefault      = 0x00000a01u;

/* GFX CP_GFX_HQD_CNTL default: 0x00a00000 | RB_BUFSZ | (RB_BLKSZ<<8) | KMD_QUEUE_BIT. */
constexpr uint32_t kMqdGfxHqdCntlBase         = 0x00a00000u;
constexpr uint32_t kMqdGfxHqdCntlKmdQueue     = 1u << 29;

/* Doorbell control: bit30 = ENABLE, low bits = doorbell index<<2. */
constexpr uint32_t kMqdDoorbellEnBit          = 1u << 30;

/* Compute PQ_CONTROL fixed bits (kernel queue): UNORD_DISPATCH=1(bit28),
 * PRIV_STATE=1(bit30), KMD_QUEUE=1(bit31), NO_UPDATE_RPTR=1(bit27).
 * TUNNEL_DISPATCH=0(bit29). */
constexpr uint32_t kMqdPqControlUnordDispatch  = 1u << 28;
constexpr uint32_t kMqdPqControlPrivState      = 1u << 30;
constexpr uint32_t kMqdPqControlKmdQueue       = 1u << 31;
constexpr uint32_t kMqdPqControlNoUpdateRptr   = 1u << 27;
constexpr uint32_t kMqdPqControlTunnelDispatch = 1u << 29;

/* PQ_CONTROL field masks. */
constexpr uint32_t kMqdPqControlQueueSizeMask   = 0x3fu;
constexpr uint32_t kMqdPqControlQueueSizeShift  = 0u;
constexpr uint32_t kMqdPqControlRptrBlockMask   = 0x3fu << 8;
constexpr uint32_t kMqdPqControlRptrBlockShift  = 8u;

/* Compute MQD: build the PQ_CONTROL dword from ring size (in dwords).
 * Sets QUEUE_SIZE = order_base_2(ring_dwords/4)-1,
 * RPTR_BLOCK_SIZE = 9 (4 KiB page order),
 * and the kernel queue fixed bits. */
uint32_t mqd_compute_pq_control(uint32_t ring_dwords);

/* Compute MQD: build the EOP_CONTROL dword. EOP_SIZE = order_base_2(eop_bytes/4)-1.
 * Default eop_bytes = 2048 (kMesEopBytes = 0x800). */
uint32_t mqd_compute_eop_control(uint32_t eop_bytes);

/* Compute:
 *   mqd[128/129] = mqd_addr (self-pointer)
 *   mqd[130]     = active (=1)
 *   mqd[132]     = persistent_state with PRELOAD_SIZE=0x55
 *   mqd[136/137] = ring_addr >> 8
 *   mqd[139/140] = wb_addr (rptr report)
 *   mqd[141/142] = wb_addr + 16 (wptr poll)
 *   mqd[143]     = (doorbell_idx << 2) | ENABLE
 *   mqd[145]     = pq_control from ring_dwords
 *   mqd[162]     = mqd_control (VMID=0)
 *   mqd[165/166] = eop_addr >> 8
 *   mqd[167]     = eop_control with EOP_SIZE=8
 *
 * Populates a pre-zeroed 512-dword mqd buffer. Returns true on success. */
bool mqd_build_compute(uint32_t mqd[512],
                        uint64_t mqd_addr,
                        uint64_t ring_addr,
                        uint64_t wb_addr,
                        uint64_t eop_addr,
                        uint32_t doorbell_idx,
                        uint32_t ring_dwords);

/* GFX MQD build (v11_gfx_mqd layout).
 *   mqd[128/129] = mqd_addr (self-pointer)
 *   mqd[135]     = quantum with QUANTUM_EN=1
 *   mqd[136/137] = ring_addr >> 8
 *   mqd[139/140] = wb_addr (rptr addr)
 *   mqd[141/142] = wb_addr + 16 (wptr poll)
 *   mqd[145]     = hqd_cntl with RB_BUFSZ/RB_BLKSZ and KMD_QUEUE
 *   mqd[143]     = (doorbell_idx << 2) | ENABLE
 *   mqd[130]     = active
 *   mqd[162]     = mqd_control (VMID=0, PRIV=1)
 *
 * Populates a pre-zeroed 512-dword mqd buffer. Returns true on success. */
bool mqd_build_gfx(uint32_t mqd[512],
                    uint64_t mqd_addr,
                    uint64_t ring_addr,
                    uint64_t wb_addr,
                    uint64_t eop_addr,
                    uint32_t doorbell_idx,
                    uint32_t ring_dwords);

} /* namespace amdgpu */

#endif /* NAVI48_MQD_V11_H */