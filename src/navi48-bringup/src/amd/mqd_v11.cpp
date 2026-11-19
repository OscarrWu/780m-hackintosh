/*
 * mqd_v11.cpp - GFX11 compute/gfx MQD field construction pure functions.
 *
 * Pure functions (no I/O) for building MQD dword arrays. See mqd_v11.h.
 *
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c (Copyright 2019 Advanced Micro Devices,
 * Inc., MIT licensed - see NOTICE). Every dword value cited from the line
 * ranges noted in the comments.
 */
#include "mqd_v11.h"

#include <string.h>

namespace amdgpu {

uint32_t mqd_compute_pq_control(uint32_t ring_dwords)
{
    /* gfx_v11_0_compute_mqd_init (gfx_v11_0.c:4378-4392):
     * QUEUE_SIZE = order_base_2(queue_size/4) - 1.
     * RPTR_BLOCK_SIZE = order_base_2(AMDGPU_GPU_PAGE_SIZE/4) - 1 = 9 (4 KiB). */
    uint32_t qs = 0;
    {
        uint32_t v = ring_dwords / 4u;
        while (v > 1u) { v >>= 1; qs++; }
    }
    uint32_t pq = kMqdPqControlBase;
    pq = (pq & ~kMqdPqControlQueueSizeMask) |
         ((qs & 0x3fu) << kMqdPqControlQueueSizeShift);
    pq = (pq & ~kMqdPqControlRptrBlockMask) |
         (9u << kMqdPqControlRptrBlockShift);
    /* Kernel queue fixed bits (mes_v11_0_mqd_init / nullarc mqd_build). */
    pq |= kMqdPqControlUnordDispatch | kMqdPqControlPrivState |
          kMqdPqControlKmdQueue | kMqdPqControlNoUpdateRptr;
    pq &= ~kMqdPqControlTunnelDispatch;
    return pq;
}

uint32_t mqd_compute_eop_control(uint32_t eop_bytes)
{
    /* gfx_v11_0_compute_mqd_init (gfx_v11_0.c:4332-4336):
     * EOP_SIZE = order_base_2(eop_size/4) - 1. */
    uint32_t es = 0;
    {
        uint32_t v = eop_bytes / 4u;
        while (v > 1u) { v >>= 1; es++; }
    }
    return (kMqdEopControlDefault & ~kMqdEopSizeMask) |
           ((es & 0x3fu) << kMqdEopSizeShift);
}

bool mqd_build_compute(uint32_t mqd[512],
                        uint64_t mqd_addr,
                        uint64_t ring_addr,
                        uint64_t wb_addr,
                        uint64_t eop_addr,
                        uint32_t doorbell_idx,
                        uint32_t ring_dwords)
{
    if (!mqd)
        return false;

    /* header (gfx_v11_0.c:4319) */
    mqd[kMqdCmpHeader] = kMqdHeader;
    mqd[kMqdCmpPipelineStatEnable] = kMqdPipelineStatEnable;  /* :4320 */
    mqd[kMqdCmpThreadMgmtSe0] = kMqdThreadMgmtAll;            /* :4321 */
    mqd[kMqdCmpThreadMgmtSe1] = kMqdThreadMgmtAll;            /* :4322 */
    mqd[kMqdCmpThreadMgmtSe2] = kMqdThreadMgmtAll;            /* :4323 */
    mqd[kMqdCmpThreadMgmtSe3] = kMqdThreadMgmtAll;            /* :4324 */
    mqd[kMqdCmpMiscReserved] = kMqdMiscReserved;              /* :4325 */

    /* eop base (gfx_v11_0.c:4327-4329) */
    mqd[kMqdCmpEopBaseLo] = static_cast<uint32_t>(eop_addr >> 8);
    mqd[kMqdCmpEopBaseHi] = static_cast<uint32_t>(eop_addr >> 40);

    /* eop control - EOP_SIZE=8 (order_base_2(2048/4)-1); gfx_v11_0.c:4332-4336 */
    mqd[kMqdCmpEopControl] = mqd_compute_eop_control(2048u);

    /* doorbell control (gfx_v11_0.c:4338-4355) */
    mqd[kMqdCmpDoorbellControl] = (doorbell_idx << 2) | kMqdDoorbellEnBit;

    /* clear rptr/wptr (gfx_v11_0.c:4358-4361) */
    mqd[kMqdCmpPqRptr] = 0;
    mqd[kMqdCmpPqWptrLo] = 0;
    mqd[kMqdCmpPqWptrHi] = 0;

    /* mqd base = self-pointer (gfx_v11_0.c:4364-4365) */
    mqd[kMqdCmpMqdBaseLo] = static_cast<uint32_t>(mqd_addr & 0xfffffffcull);
    mqd[kMqdCmpMqdBaseHi] = static_cast<uint32_t>(mqd_addr >> 32);

    /* mqd control VMID=0 (gfx_v11_0.c:4368-4370) */
    mqd[kMqdCmpMqdControl] = kMqdControlDefault;

    /* ring base >> 8 (gfx_v11_0.c:4373-4375) */
    mqd[kMqdCmpPqBaseLo] = static_cast<uint32_t>(ring_addr >> 8);
    mqd[kMqdCmpPqBaseHi] = static_cast<uint32_t>(ring_addr >> 40);

    /* pq control (gfx_v11_0.c:4378-4392) */
    mqd[kMqdCmpPqControl] = mqd_compute_pq_control(ring_dwords);

    /* rptr report addr (gfx_v11_0.c:4395-4398) */
    mqd[kMqdCmpRptrReportLo] = static_cast<uint32_t>(wb_addr & 0xfffffffcull);
    mqd[kMqdCmpRptrReportHi] = static_cast<uint32_t>((wb_addr >> 32) & 0xffffu);

    /* wptr poll addr = wb + 16 (gfx_v11_0.c:4401-4403) */
    mqd[kMqdCmpWptrPollLo] = static_cast<uint32_t>((wb_addr + 16) & 0xfffffffcull);
    mqd[kMqdCmpWptrPollHi] = static_cast<uint32_t>(((wb_addr + 16) >> 32) & 0xffffu);

    /* vmid (gfx_v11_0.c:4426) */
    mqd[kMqdCmpHqdVmid] = kMqdHqdVmid;

    /* persistent state with PRELOAD_SIZE=0x55 (gfx_v11_0.c:4428-4430) */
    mqd[kMqdCmpPersistentState] = kMqdPersistentDefault;

    /* ib_control MIN_IB_AVAIL_SIZE=3 (gfx_v11_0.c:4433-4435) */
    mqd[kMqdCmpIbControl] = kMqdIbControlDefault;

    /* iq_timer = 0 (default) */
    mqd[kMqdCmpIqTimer] = 0;

    /* quantum = 0 (gfx_v11_0.c:4441-4444; for kernel queue quantum=0 is default) */
    mqd[kMqdCmpQuantum] = 0;

    /* active (gfx_v11_0.c:4446) */
    mqd[kMqdCmpHqdActive] = kMqdHqdActive;

    return true;
}

bool mqd_build_gfx(uint32_t mqd[512],
                    uint64_t mqd_addr,
                    uint64_t ring_addr,
                    uint64_t wb_addr,
                    uint64_t eop_addr,
                    uint32_t doorbell_idx,
                    uint32_t ring_dwords)
{
    (void)eop_addr;

    if (!mqd)
        return false;

    /* mqd base = self-pointer (gfx_v11_0_gfx_mqd_init:4152-4153) */
    mqd[kMqdGfxCpMqdBaseLo] = static_cast<uint32_t>(mqd_addr & 0xfffffffcull);
    mqd[kMqdGfxCpMqdBaseHi] = static_cast<uint32_t>(mqd_addr >> 32);

    /* mqd control VMID=0 / PRIV=1 (gfx_v11_0.c:4156-4160) */
    mqd[kMqdGfxMqdControl] = kMqdControlDefault;

    /* vmid = 0 (gfx_v11_0.c:4163-4165) */
    mqd[kMqdGfxHqdVmid] = kMqdHqdVmid;

    /* queue priority = 0 (kernel queue, gfx_v11_0.c:4168) */
    mqd[kMqdGfxQueuePriority] = 0;

    /* quantum with QUANTUM_EN=1 (gfx_v11_0.c:4171-4173) */
    mqd[kMqdGfxQuantum] = kMqdGfxQuantumDefault;

    /* ring base >> 8 (gfx_v11_0.c:4176-4178) */
    mqd[kMqdGfxHqdBaseLo] = static_cast<uint32_t>(ring_addr >> 8);
    mqd[kMqdGfxHqdBaseHi] = static_cast<uint32_t>(ring_addr >> 40);

    /* rptr addr (gfx_v11_0.c:4182-4184) */
    mqd[kMqdGfxRptrAddrLo] = static_cast<uint32_t>(wb_addr & 0xfffffffcull);
    mqd[kMqdGfxRptrAddrHi] = static_cast<uint32_t>((wb_addr >> 32) & 0xffffu);

    /* wptr poll addr = wb + 16 (gfx_v11_0.c:4188-4189) */
    mqd[kMqdGfxWptrPollLo] = static_cast<uint32_t>((wb_addr + 16) & 0xfffffffcull);
    mqd[kMqdGfxWptrPollHi] = static_cast<uint32_t>(((wb_addr + 16) >> 32) & 0xffffu);

    /* hqd_cntl: RB_BUFSZ/RB_BLKSZ/KMD_QUEUE (gfx_v11_0.c:4192-4203) */
    {
        uint32_t rb_bufsz = 0;
        {
            uint32_t v = ring_dwords / 4u;
            while (v > 1u) { v >>= 1; rb_bufsz++; }
        }
        uint32_t rb_blksz = (rb_bufsz >= 2u) ? rb_bufsz - 2u : 0u;
        mqd[kMqdGfxHqdCntl] = kMqdGfxHqdCntlBase |
                               (rb_bufsz & 0x1fu) |
                               ((rb_blksz & 0x1fu) << 8) |
                               kMqdGfxHqdCntlKmdQueue;
    }

    /* doorbell control (gfx_v11_0.c:4206-4215) */
    mqd[kMqdGfxDoorbellControl] = (doorbell_idx << 2) | kMqdDoorbellEnBit;

    /* rptr = 0 (gfx_v11_0.c:4218) */
    mqd[kMqdGfxHqdRptr] = 0;

    /* wptr = 0 (gfx_v11_0.c:4148-4149) */
    mqd[kMqdGfxHqdWptrLo] = 0;
    mqd[kMqdGfxHqdWptrHi] = 0;

    /* active (gfx_v11_0.c:4221) */
    mqd[kMqdGfxHqdActive] = kMqdHqdActive;

    return true;
}

} /* namespace amdgpu */