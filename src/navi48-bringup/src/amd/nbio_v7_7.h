//
//  nbio_v7_7.h — NBIO 7.7 CSDMA doorbell range helper.
//
//  Linux 7.2.0 nbio_v7_7.c:64-85 programs GDC0_BIF_CSDMA_DOORBELL_RANGE
//  through RREG32_PCIE_PORT/WREG32_PCIE_PORT, i.e. the NBIO PF0 RSMU
//  index/data pair (nbio_v7_7.c:211-219, BASE_IDX 1), not the BAR5 direct
//  map.  The SDMA substep (S4, inside the MINOR_PTR_UPDATE window) and the
//  GART substep (re-program after the PSP/MES hand-off) share this sequence.
//
#ifndef NAVI48_NBIO_V7_7_H
#define NAVI48_NBIO_V7_7_H

#include <stdint.h>

#include "amdgpu_ip.h"

struct R3Io;

namespace amdgpu {
namespace nbio_v7_7 {

/* NBIO 7.7 register segments and offsets.
 * nbio_7_7_0_offset.h:3358-3361 (RSMU, BASE_IDX 1),
 * :4008-4009 (GDC0_BIF_CSDMA_DOORBELL_RANGE, BASE_IDX 3). */
constexpr int      kRsmuBaseIdx           = 1;
constexpr int      kCsdmaBaseIdx          = 3;
constexpr uint32_t kRsmuIndexOff          = 0x0000u;
constexpr uint32_t kRsmuDataOff           = 0x0001u;
constexpr uint32_t kCsdmaDoorbellRangeOff = 0x4f0afbu;

/* Field layout: nbio_7_7_0_sh_mask.h:31644-31648. */
constexpr uint32_t kCsdmaOffsetMask = 0x00000ffcu;
constexpr uint32_t kCsdmaOffsetShift = 2u;
constexpr uint32_t kCsdmaSizeMask   = 0x00ff0000u;
constexpr uint32_t kCsdmaSizeShift  = 16u;
constexpr uint32_t kCsdmaMaxIndex   = 0x3ffu;
constexpr uint32_t kCsdmaMaxSize    = 0xffu;

enum class RangeResult : uint8_t {
    Ok = 0,
    NoBase,
    BadParam,
    Denied,
    ReadFail,
    WriteFail,
    VerifyFail,
};

/* Pure encoding: set/clear OFFSET/SIZE on top of cur.
 * enable=false clears SIZE only and preserves OFFSET (PC-7: no upstream
 * counterpart; ladder harness construct, Linux disables the SDMA0 range
 * instead of the CSDMA one). */
uint32_t csdma_range_value(uint32_t cur, uint32_t index, uint32_t size, bool enable,
                           bool *ok);

/* Resolve the CSDMA target absolute address; returns 0 and *ok=false when
 * either NBIO segment is unresolved. */
uint32_t csdma_range_addr(const IPBaseTable &ip, bool *ok);

/* Program the range through the NBIO PF0 RSMU index/data pair and read it
 * back, asserting the OFFSET|SIZE fields. before/after may be null; after is
 * the full readback (reserved bits included), only OFFSET|SIZE are compared. */
RangeResult program_csdma_doorbell_range(const R3Io *io, const IPBaseTable &ip,
                                         uint32_t index, uint32_t size, bool enable,
                                         uint32_t *before, uint32_t *after);

} /* namespace nbio_v7_7 */
} /* namespace amdgpu */

#endif /* NAVI48_NBIO_V7_7_H */
