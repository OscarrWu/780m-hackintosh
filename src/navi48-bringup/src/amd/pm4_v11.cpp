/*
 * pm4_v11.cpp - PM4 TYPE-3 packet builders + submit tail (stage 3, §8).
 *
 * Every builder is a pure function: it fills the caller's dword array and
 * returns the packet length (0 on impossible input). The submit tail is the
 * only function with side effects; it follows the nullarc gpuSubmit tail
 * (PhxBringup.cpp:827-838, R10 §8.3) and aborts at any hdpFlush failure.
 *
 * Provenance (PC-8): packet encodings are verbatim nullarc evidence
 * (nvd.h field definitions + PhxBringup.cpp/mesboot.c usage, R9/R10) with
 * Linux gfx_v11_0.c cross-checks cited at each builder.
 */
#include "pm4_v11.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

namespace amdgpu {

namespace {

/* PACKET3(op, n): TYPE3<<30 | op<<8 | count<<16 (nvd.h:48). count = body
 * dwords after word0 minus one (total = count + 2). */
uint32_t packet3(uint32_t op, uint32_t count)
{
    return (kPm4Type3 << kPm4PacketTypeShift) | (op << kPm4PacketOpShift) |
           (count << kPm4PacketCountShift);
}

} /* namespace */

uint64_t pm4_va_canonical(uint64_t va)
{
    /* nullarc PhxBringup.cpp:64-67/:801: an upper-half IB address with a
     * bare 16-bit high word silently wedges the queue. Linux
     * amdgpu_gmc_sign_extend (amdgpu_gmc.h:408-409) ORs HOLE_END
     * (0xffff800000000000) instead; for already-canonical upper-half
     * addresses both forms agree. The nullarc form is the same-machine
     * evidence (R10). */
    return (va & (1ull << 47)) ? (va | 0xffff000000000000ull) : va;
}

uint32_t pm4_write_data(uint32_t out[5], uint64_t dst_va, uint32_t data)
{
    if (!out || (dst_va & 3ull) != 0ull)
        return 0;
    /* nullarc mesboot.c:227 area: 0xC0033700, 0x00100500
     * (DST_SEL(5)=memory async direct | WR_CONFIRM), VA lo, VA hi, data.
     * Fields: nvd.h:106-157; Linux usage gfx_v11_0.c:652-657. */
    out[0] = packet3(kPm4OpWriteData, 3);
    out[1] = (kPm4WdDstSelMemory << kPm4WdDstSelShift) | kPm4WdWrConfirm;
    out[2] = static_cast<uint32_t>(dst_va & 0xffffffffull);
    out[3] = static_cast<uint32_t>(dst_va >> 32);
    out[4] = data;
    return 5;
}

uint32_t pm4_indirect_buffer(uint32_t out[4], uint64_t ib_va,
                             uint32_t length_dw, uint32_t vmid, bool compute)
{
    if (!out || (ib_va & 3ull) != 0ull || length_dw == 0u ||
        (length_dw & ~0xfffffu) != 0u || (vmid & ~0xFu) != 0u)
        return 0;
    /* Linux gfx_v11_0.c:6030-6063 (compute): control = VALID | length_dw |
     * (vmid << 24); the gfx path (:5996-6028) omits VALID. The base must
     * be canonical (nullarc PhxBringup.cpp:801). */
    const uint64_t cva = pm4_va_canonical(ib_va);
    out[0] = packet3(kPm4OpIndirectBuffer, 2);
    out[1] = static_cast<uint32_t>(cva & ~3ull);
    out[2] = static_cast<uint32_t>(cva >> 32);
    out[3] = length_dw | (vmid << kPm4IbVmidShift) | (compute ? kPm4IbValid : 0u);
    return 4;
}

uint32_t pm4_release_mem(uint32_t out[8], uint64_t fence_mc, uint32_t seq)
{
    if (!out || (fence_mc & 7ull) != 0ull)
        return 0;
    /* nullarc PhxBringup.cpp:807-826 (verbatim):
     *   0xC0064900
     *   0x06603514 = SEQ|GL2_WB|GLM_INV|GLM_WB | CACHE_POLICY(3) |
     *                EVENT_TYPE(0x14=CACHE_FLUSH_AND_INV_TS_EVENT) |
     *                EVENT_INDEX(5)
     *   0x40000000 = DATA_SEL(2) 64-bit, INT_SEL(0) no interrupt
     *   fence lo / fence hi / seq lo / seq hi / 0
     * Linux gfx_v11_0.c:6065-6096 emit_fence matches field for field.
     * The trailing 0 pads the body to the TYPE3 count: packet3(op,6) means
     * count=body-1, so count=6 carries 8 dwords total — the terminating 0 is
     * written explicitly by Linux emit_fence (gfx_v11_0.c:6094).
     * fence_mc is an MC-aperture address passed raw (no canonical
     * extension; nullarc :809-814). */
    out[0] = packet3(kPm4OpReleaseMem, 6);
    out[1] = kPm4RmSeq | kPm4RmGl2Wb | kPm4RmGlmInv | kPm4RmGlmWb |
             (3u << kPm4RmCachePolicyShift) | (0x14u << 0) |
             (5u << kPm4RmEventIndexShift);
    out[2] = kPm4RmDataSel64 << kPm4RmDataSelShift;
    out[3] = static_cast<uint32_t>(fence_mc & 0xffffffffull);
    out[4] = static_cast<uint32_t>(fence_mc >> 32);
    out[5] = seq;
    out[6] = 0;
    out[7] = 0;   /* body pad to count=6 (Linux emit_fence :6094) */
    return 8;
}

uint32_t pm4_acquire_mem(uint32_t out[8])
{
    if (!out)
        return 0;
    /* nullarc mesboot.c (R10 §9), verbatim: GL2 writeback+invalidate
     * (GCR_CNTL = GL2_INV|GL2_WB), COHER_SIZE=0xFFFFFFFF,
     * COHER_SIZE_HI=0x00FFFFFF, no base, POLL_INTERVAL=0x0A. */
    out[0] = packet3(kPm4OpAcquireMem, 6);
    out[1] = 0;
    out[2] = 0xFFFFFFFFu;
    out[3] = 0x00FFFFFFu;
    out[4] = 0;
    out[5] = 0;
    out[6] = 0x0A;
    out[7] = kPm4GcrGl2Inv | kPm4GcrGl2Wb; /* 0xC000 */
    return 8;
}

uint32_t pm4_set_uconfig_reg(uint32_t out[3], uint32_t reg_off, uint32_t value)
{
    if (!out)
        return 0;
    /* nullarc mesboot.c (R10 §9): 0xC0017900, 0x40, 0xDEADBEEF
     * (SCRATCH_REG0). */
    out[0] = packet3(kPm4OpSetUconfigReg, 1);
    out[1] = reg_off;
    out[2] = value;
    return 3;
}

bool pm4_submit_tail(const R3Io *io, uint64_t ring_off, uint64_t wb_off,
                     uint32_t doorbell_index, uint64_t wptr)
{
    /* nullarc PhxBringup.cpp:827-838, verbatim order. The source performs
     * two hdpFlush calls; both are checked (the task card's "hdpFlush
     * false must abort" applies to each). */
    if (!io || !io->bus || !io->fence || !io->hdp_flush)
        return false;

    /* Drain the WC buffer: read the ring back through the carveout window
     * (source: (void)fbAper[ringOff]). */
    uint64_t drain = 0;
    if (!io->bus(io->ctx, R3_IO_BAR_VRAM, ring_off, &drain, false, 1))
        return false;

    if (!io->hdp_flush(io->ctx))
        return false;

    /* Write the 64-bit wptr to the WB page at +16 (source: wb[0]=wptr lo,
     * wb[1]=hi), fence, read it back, flush again. */
    if (!io->bus(io->ctx, R3_IO_BAR_VRAM, wb_off + 16, &wptr, true, 2))
        return false;
    io->fence(io->ctx);
    uint64_t back = 0;
    if (!io->bus(io->ctx, R3_IO_BAR_VRAM, wb_off + 16, &back, false, 2))
        return false;
    if (back != wptr)
        return false;
    if (!io->hdp_flush(io->ctx))
        return false;
    io->fence(io->ctx);

    /* Ring the doorbell: byte offset = index*4, 64-bit value = wptr (dword
     * count). Source: *(uint64*)(doorbellMap + dbByte) = wptr with
     * dbByte = db_index*4 (R10 §6). */
    const uint64_t db_byte = static_cast<uint64_t>(doorbell_index) * 4ull;
    uint64_t v = wptr;
    return io->bus(io->ctx, 2u, db_byte, &v, true, 2);
}

} /* namespace amdgpu */
