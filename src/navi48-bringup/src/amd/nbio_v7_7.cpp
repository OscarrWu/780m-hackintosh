//
//  nbio_v7_7.cpp — NBIO 7.7 CSDMA doorbell range helper.
//
//  The write sequence mirrors Linux amdgpu_reg_pciep_wr32/rd32
//  (amdgpu_reg_access.c:905-937): write INDEX = reg << 2, read INDEX as a
//  barrier, read DATA, write DATA, read DATA back.
//
#include "nbio_v7_7.h"

#include "../rdna3/rdna3_ip_entries.h"
#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"

namespace amdgpu {
namespace nbio_v7_7 {

static_assert(kCsdmaRangeGrantCount == 3u,
              "CSDMA range grant count must match the budget contract");

uint32_t csdma_range_value(uint32_t cur, uint32_t index, uint32_t size, bool enable,
                           bool *ok)
{
    if (ok)
        *ok = false;
    if (index > kCsdmaMaxIndex || size > kCsdmaMaxSize)
        return 0u;
    uint32_t v = cur;
    if (enable) {
        v = (v & ~kCsdmaOffsetMask) |
            ((index << kCsdmaOffsetShift) & kCsdmaOffsetMask);
        v = (v & ~kCsdmaSizeMask) |
            ((size << kCsdmaSizeShift) & kCsdmaSizeMask);
    } else {
        v &= ~kCsdmaSizeMask;
    }
    if (ok)
        *ok = true;
    return v;
}

uint32_t csdma_range_addr(const IPBaseTable &ip, bool *ok)
{
    if (ok)
        *ok = false;
    if (!ip.isResolved(IPBlock::NBIO, kRsmuBaseIdx) ||
        !ip.isResolved(IPBlock::NBIO, kCsdmaBaseIdx))
        return 0u;
    if (ok)
        *ok = true;
    return ip.getBase(IPBlock::NBIO, kCsdmaBaseIdx) + kCsdmaDoorbellRangeOff;
}

RangeResult program_csdma_doorbell_range(const R3Io *io, const IPBaseTable &ip,
                                         uint32_t index, uint32_t size, bool enable,
                                         uint32_t *before, uint32_t *after)
{
    if (!io || !io->mmio)
        return RangeResult::NoBase;
    bool have_addr = false;
    const uint32_t reg = csdma_range_addr(ip, &have_addr);
    if (!have_addr)
        return RangeResult::NoBase;
    if (index > kCsdmaMaxIndex || size > kCsdmaMaxSize)
        return RangeResult::BadParam;

    const uint32_t rsmu = ip.getBase(IPBlock::NBIO, kRsmuBaseIdx);
    const uint32_t index_port = rsmu + kRsmuIndexOff;
    const uint32_t data_port = rsmu + kRsmuDataOff;

    /* IO-3: the semantic target and both ports must pass the classifier; the
     * target classify also matches the R3WriteRowAbs declaration and sets its
     * met bit. */
    if (r3_wg_classify(reg, 1u, WClass::Protocol, "sdma:csdma-range") !=
            WVerdict::TxnRead ||
        r3_wg_classify(index_port, 1u, WClass::Protocol, "sdma:csdma-index") !=
            WVerdict::TxnRead ||
        r3_wg_classify(data_port, 1u, WClass::Protocol, "sdma:csdma-data") !=
            WVerdict::TxnRead)
        return RangeResult::Denied;

    if (!r3_io_write32(io, index_port, reg << 2))
        return RangeResult::WriteFail;
    (void)r3_io_read32(io, index_port, nullptr);
    bool ok = false;
    const uint32_t cur = r3_io_read32(io, data_port, &ok);
    if (!ok)
        return RangeResult::ReadFail;
    if (before)
        *before = cur;

    bool enc_ok = false;
    const uint32_t want = csdma_range_value(cur, index, size, enable, &enc_ok);
    if (!enc_ok)
        return RangeResult::BadParam;
    if (!r3_io_write32(io, data_port, want))
        return RangeResult::WriteFail;

    const uint32_t back = r3_io_read32(io, data_port, &ok);
    if (!ok)
        return RangeResult::ReadFail;
    if (after)
        *after = back;

    const uint32_t fields = kCsdmaOffsetMask | kCsdmaSizeMask;
    if ((back & fields) != (want & fields))
        return RangeResult::VerifyFail;
    return RangeResult::Ok;
}

} /* namespace nbio_v7_7 */
} /* namespace amdgpu */
