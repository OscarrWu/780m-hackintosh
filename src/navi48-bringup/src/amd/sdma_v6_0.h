/*
 * sdma_v6_0.h - SDMA 6.0.1 queue0 power-up substep (stage 2, K7).
 *
 * Scope: the C0.6 minimum proof that the SDMA engine can copy memory. The substep
 * follows Linux sdma_v6_0_gfx_resume_instance() order: zero carveout, stage the ring,
 * write the wptr shadow copy, open the MINOR_PTR_UPDATE window and program RB_WPTR,
 * unhalt F32 + enable RB/IB (clearing doorbell ENABLE, zeroing SEM_WAIT_FAIL_TIMER
 * and arming WATCHDOG QUEUE_HANG_COUNT), poll the fence (2 s budget), compare the
 * target byte by byte (C4.2) and record UTCL1 defaults. SDMA0_STATUS_REG is the
 * cross evidence (S8).
 *
 * Address spaces (the two-space trap documented by K4):
 *   - R3Grant::start/size are BAR0 byte offsets; CPU copies use only BAR0 (B4).
 *   - SDMA register addresses are BAR5 dword offsets (r3_io_read32/write32).
 *   - SDMA packet addresses are MC: fb_mc_base + grant.start + sub-offset.
 *
 * Register source: gc_11_0_0_offset.h / gc_11_0_0_sh_mask.h (there is no separate
 * sdma_6_0_1 offset header for this ASIC). Packet source: sdma_v6_0_0_pkt_open.h.
 * Linux reference: sdma_v6_0_gfx_resume_instance() in sdma_v6_0.c.
 *
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c (Copyright 2020 Advanced Micro Devices, Inc.,
 * MIT licensed - see NOTICE). Only the register/protocol semantics are reproduced onto
 * the mac-amdgpu (Cobalt64) IP-module conventions; no Linux code is copied verbatim,
 * and every offset is cited from the pinned headers at its use site.
 */
#ifndef NAVI48_SDMA_V6_0_H
#define NAVI48_SDMA_V6_0_H

#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"


namespace amdgpu {

/* --- SDMA0 queue0 register offsets ------------------------------------------------
 * gc_11_0_0_offset.h. BASE_IDX 0 unless the comment says otherwise. The absolute
 * dword is IPBaseTable::getBase(IPBlock::SDMA0, base_idx) + offset.
 */

constexpr uint32_t kRegSdma0Version                = 0x0035; /* :84-85 (D3 observation) */

constexpr uint32_t kRegSdma0Queue0RbBase           = 0x0081; /* :192-193 */
constexpr uint32_t kRegSdma0Queue0RbBaseHi         = 0x0082; /* :194-195 */

constexpr uint32_t kRegSdma0Queue0RbRptrHi         = 0x0084; /* :198-199 */

constexpr uint32_t kRegSdma0Queue0RbWptrHi         = 0x0086; /* :202-203 */
constexpr uint32_t kRegSdma0Queue0RbRptrAddrHi     = 0x0088; /* :204-205 */
constexpr uint32_t kRegSdma0Queue0RbRptrAddrLo     = 0x0089; /* :206-207 */
constexpr uint32_t kRegSdma0Queue0IbCntl           = 0x008a; /* :208-209 */
constexpr uint32_t kRegSdma0Queue0RbWptrPollAddrHi = 0x00b2; /* :242-243 */
constexpr uint32_t kRegSdma0Queue0RbWptrPollAddrLo = 0x00b3; /* :244-245 */
constexpr uint32_t kRegSdma0Queue0MinorPtrUpdate   = 0x00b5; /* :248-249 */
constexpr uint32_t kRegSdma0SemWaitFailTimerCntl   = 0x0022; /* :52-53 */
constexpr uint32_t kRegSdma0WatchdogCntl           = 0x002e; /* :74-75 */
constexpr uint32_t kRegSdma0Utcl1Cntl              = 0x003c; /* :98-99 */
constexpr uint32_t kRegSdma0Utcl1Page              = 0x003f; /* :104-105 */
constexpr uint32_t kRegSdma0Queue0Doorbell         = 0x0092; /* :224-225 */


constexpr int kSdma0BaseIdx   = 0; /* regSDMA0_*_BASE_IDX */
constexpr int kSdmaF32BaseIdx = 1; /* regSDMA0_F32_CNTL_BASE_IDX */

/* --- RB_CNTL / IB_CNTL / F32_CNTL / MINOR_PTR_UPDATE / STATUS fields -------------
 * gc_11_0_0_sh_mask.h. Values are bit masks in the register, not shifted values.
 */

constexpr uint32_t kRbCntlRbSizeMask            = 0x0000003eu; /* shift 1  */
constexpr uint32_t kRbCntlRbSizeShift           = 1;
constexpr uint32_t kRbCntlWptrPollEnableMask    = 0x00000100u; /* shift 8  */
constexpr uint32_t kRbCntlF32WptrPollEnableMask = 0x00000800u; /* shift 11 */
constexpr uint32_t kRbCntlRptrWbEnableMask      = 0x00001000u; /* shift 12 */
constexpr uint32_t kRbCntlRbPrivMask            = 0x00800000u; /* shift 23 */
constexpr uint32_t kIbCntlIbEnableMask          = 0x00000001u; /* shift 0  */

constexpr uint32_t kF32CntlTh1ResetMask         = 0x00002000u; /* shift 13 */
constexpr uint32_t kMinorPtrUpdateMask          = 0x00000001u; /* shift 0  */
constexpr uint32_t kStatusIdleMask              = 0x00000001u; /* shift 0  */
constexpr uint32_t kStatusRbEmptyMask           = 0x00000004u; /* shift 2  */
constexpr uint32_t kSdmaQueue0DoorbellEnableMask   = 0x10000000u; /* shift 28 */
constexpr uint32_t kSdmaWatchdogHangCountMask      = 0x000000ffu; /* shift 0  */

/* --- WRITE_LINEAR packet (sdma_v6_0_0_pkt_open.h) --------------------------------
 * DW0 = header (op = SDMA_OP_WRITE, sub-op = SDMA_SUBOP_WRITE_LINEAR), DW1 = dst lo,
 * DW2 = dst hi, DW3 = count (data dwords - 1), DW4.. = data dwords.
 */
constexpr uint32_t kSdmaOpWrite            = 2; /* SDMA_OP_WRITE */
constexpr uint32_t kSdmaSubopWriteLinear   = 0; /* SDMA_SUBOP_WRITE_LINEAR */
constexpr uint32_t kPktHeaderOpMask        = 0x000000ffu;
constexpr uint32_t kPktHeaderSubOpShift    = 8;
constexpr uint32_t kWriteUntilDstLoOff     = 1;
constexpr uint32_t kWriteUntilDstHiOff     = 2;
constexpr uint32_t kWriteUntilCountOff     = 3;
constexpr uint32_t kWriteUntilCountMask    = 0x000fffffu;
constexpr uint32_t kWriteUntilData0Off     = 4;

/* --- carveout regions inside the layout "sdma" grant (BAR0 byte offsets) ----------
 * All five regions sit inside one layout grant, so L5's zero-fill area is inside the
 * grant and the writeguard aggregates the CPU writes to that one interval (A5).
 */
constexpr uint64_t kSdmaRingOff         = 0x000000ull;
constexpr uint64_t kSdmaRingBytes       = 0x001000ull; /* 4 KiB, RB_SIZE = 10 dwords */
constexpr uint64_t kSdmaWbOff           = 0x001000ull;
constexpr uint64_t kSdmaWbBytes         = 0x001000ull;
constexpr uint64_t kSdmaWptrShadowOff   = 0x002000ull;
constexpr uint64_t kSdmaWptrShadowBytes = 0x001000ull;
constexpr uint64_t kSdmaSrcOff          = 0x003000ull;
constexpr uint64_t kSdmaSrcBytes        = 0x001000ull;
constexpr uint64_t kSdmaDstOff          = 0x004000ull;
constexpr uint64_t kSdmaDstBytes        = 0x001000ull;
constexpr uint64_t kSdmaRegionBytes     = 0x005000ull;

constexpr uint64_t kSdmaRptrWbOff = kSdmaWbOff + 0x0000ull; /* 8 B writeback slot */
constexpr uint64_t kSdmaFenceOff  = kSdmaWbOff + 0x0020ull; /* 4 B fence dword */

constexpr uint32_t kSdmaCopyDwords = 16;                 /* 64 B copy proof */
constexpr uint32_t kSdmaCopyBytes  = kSdmaCopyDwords * 4;
constexpr uint32_t kSdmaFenceMagic = 0x53444d41u;        /* "SDMA", distinct from pattern */

/* --- ring geometry and budgets ---------------------------------------------------- */
constexpr uint32_t kSdmaRingDwords = static_cast<uint32_t>(kSdmaRingBytes / 4);
constexpr uint32_t kSdmaRbSize     = 10; /* order_base_2(kSdmaRingDwords) */
static_assert((1u << kSdmaRbSize) == kSdmaRingDwords, "RB_SIZE must describe the ring");

constexpr uint32_t kSdmaFencePollUs     = 100;
constexpr uint32_t kSdmaFenceBudgetUs   = 2000000; /* C4.2/S5: 2 s */
/* Linux sdma_v6_0.c:577-581: max(usec_timeout / 100000, 1) with usec_timeout = 100 ms
 * per unit here, i.e. 2 s / 100 ms = 20. */
constexpr uint32_t kSdmaWatchdogHangCount =
    (kSdmaFenceBudgetUs / 100000u) > 1u ? (kSdmaFenceBudgetUs / 100000u) : 1u;
constexpr uint32_t kSdmaFencePolls      = kSdmaFenceBudgetUs / kSdmaFencePollUs;



} /* namespace amdgpu */

#endif /* NAVI48_SDMA_V6_0_H */
