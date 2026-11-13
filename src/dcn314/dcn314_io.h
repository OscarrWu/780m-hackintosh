/*
 * dcn314_io.h - DCN 3.1.4 register I/O primitives (read/get/wait are live; write/update/set are stubs).
 *
 * Stage 1 is a read-only survey, so the display-state write primitives are deliberately NOT wired to
 * MMIO: dcn314_write / dcn314_update / dcn314_set return DCN314_E_NOWRITE and touch nothing. They
 * exist only to keep the interface the stage-4 layer will implement. The single exception is the
 * minimal AUX read transaction, whose request-register writes live in dcn314_aux_min.c and are gated
 * by the caller's runtime whitelist (dcn314_write_ok_fn); they do not come through this file.
 */
#ifndef N48_DCN314_IO_H
#define N48_DCN314_IO_H

#include "dcn314.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t dcn314_field(uint32_t value, uint32_t mask, uint32_t shift)
{
    return (value & mask) >> shift;
}

/* Read one dword. Returns 0 (never an error code) when the address is unusable; callers that must
 * distinguish validate the device/instance first. */
static inline uint32_t dcn314_read(const struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx)
{
    uint32_t a = dcn314_abs(dev, offset, base_idx);
    if (a == DCN314_BAD_ADDR)
        return 0u;
    return dev->rreg(dev->cookie, a);
}

/* Read a field. */
static inline uint32_t dcn314_get(const struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx,
                                  uint32_t mask, uint32_t shift)
{
    return dcn314_field(dcn314_read(dev, offset, base_idx), mask, shift);
}

/* Stage-1 write stubs. Never I/O. */
static inline int dcn314_write(struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx,
                               uint32_t value)
{
    (void)dev; (void)offset; (void)base_idx; (void)value;
    return DCN314_E_NOWRITE;
}

static inline int dcn314_update(struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx,
                                uint32_t mask, uint32_t shift, uint32_t value)
{
    (void)dev; (void)offset; (void)base_idx; (void)mask; (void)shift; (void)value;
    return DCN314_E_NOWRITE;
}

static inline int dcn314_set(struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx,
                             uint32_t mask, uint32_t value)
{
    (void)dev; (void)offset; (void)base_idx; (void)mask; (void)value;
    return DCN314_E_NOWRITE;
}

/* Bounded poll: read the field and wait until it equals `want` (or, when `any` is true, is non-zero),
 * at most max_try+1 reads with delay_us between them. Returns DCN314_OK or DCN314_E_TIMEOUT; *last
 * (optional) is the last observed field value. No software retry loop beyond this bound. */
static inline int dcn314_wait(const struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx,
                              uint32_t mask, uint32_t shift, uint32_t want, bool any,
                              uint32_t max_try, uint32_t delay_us, uint32_t *last)
{
    uint32_t v = 0u;
    uint32_t i;
    for (i = 0; i <= max_try; i++) {
        v = dcn314_field(dcn314_read(dev, offset, base_idx), mask, shift);
        if (any ? (v != 0u) : (v == want)) {
            if (last)
                *last = v;
            return DCN314_OK;
        }
        if (i < max_try && dev->udelay && delay_us)
            dev->udelay(dev->cookie, delay_us);
    }
    if (last)
        *last = v;
    return DCN314_E_TIMEOUT;
}

#ifdef __cplusplus
}
#endif
#endif /* N48_DCN314_IO_H */
