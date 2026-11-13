/*
 * dcn314_core.c - device setup and address resolution for the dcn314 layer. See dcn314.h.
 *
 * Stage 1 is a read-only survey, so there is no scanout/flip/refresh-write arithmetic here: only the
 * device ready flag, dcn314_abs() and the write-refusal stub. dcn314_dev_init() accepts the caller's
 * discovery segment bases as-is (the offsets are real Linux v7.2 values; the bases are the survey's
 * job) and only checks that every base this layer uses, plus the largest offset it can form, stays
 * inside the caller's BAR5 window.
 */
#include "dcn314.h"

#pragma GCC poison float double   /* after the include: <stddef.h> itself names long double */

static bool dcn314_ready(const struct dcn314_dev *dev)
{
    return dev != NULL && dev->magic == DCN314_DEV_MAGIC && dev->rreg != NULL;
}

int dcn314_dev_init(struct dcn314_dev *dev, void *cookie, dcn314_rreg_fn rreg, dcn314_wreg_fn wreg,
                    dcn314_udelay_fn udelay, dcn314_write_ok_fn write_ok,
                    const uint32_t seg[DCN314_NUM_SEGS], uint32_t mmio_dwords, uint32_t flags)
{
    static const uint32_t max_off[DCN314_NUM_SEGS] = {
#ifdef DCN314_MAX_OFFSET_BASE0
        DCN314_MAX_OFFSET_BASE0,
#else
        0u,
#endif
#ifdef DCN314_MAX_OFFSET_BASE1
        DCN314_MAX_OFFSET_BASE1,
#else
        0u,
#endif
#ifdef DCN314_MAX_OFFSET_BASE2
        DCN314_MAX_OFFSET_BASE2,
#else
        0u,
#endif
#ifdef DCN314_MAX_OFFSET_BASE3
        DCN314_MAX_OFFSET_BASE3,
#else
        0u,
#endif
#ifdef DCN314_MAX_OFFSET_BASE4
        DCN314_MAX_OFFSET_BASE4,
#else
        0u,
#endif
    };
    uint32_t i;

    if (dev == NULL)
        return DCN314_E_ARG;
    for (i = 0; i < sizeof(*dev); i++)
        ((uint8_t *)dev)[i] = 0;
    if (rreg == NULL || wreg == NULL || udelay == NULL || seg == NULL || mmio_dwords == 0u)
        return DCN314_E_ARG;
    for (i = 0; i < DCN314_NUM_SEGS; i++) {
        if (!(DCN314_BASE_IDX_USED_MASK & (1u << i)))
            continue;
        if (seg[i] == 0u || (uint64_t)seg[i] + max_off[i] >= mmio_dwords)
            return DCN314_E_WINDOW;
    }

    dev->cookie = cookie;
    dev->rreg = rreg;
    dev->wreg = wreg;
    dev->udelay = udelay;
    dev->write_ok = write_ok;
    for (i = 0; i < DCN314_NUM_SEGS; i++)
        dev->seg[i] = seg[i];
    dev->mmio_dwords = mmio_dwords;
    dev->flags = flags;
    dev->magic = DCN314_DEV_MAGIC;
    return DCN314_OK;
}

uint32_t dcn314_abs(const struct dcn314_dev *dev, uint32_t offset, uint32_t base_idx)
{
    uint64_t a;

    if (!dcn314_ready(dev) || offset == DCN314_BAD_OFFSET || base_idx >= DCN314_NUM_SEGS ||
        !(DCN314_BASE_IDX_USED_MASK & (1u << base_idx)))
        return DCN314_BAD_ADDR;
    a = (uint64_t)dev->seg[base_idx] + offset;
    if (a >= dev->mmio_dwords)
        return DCN314_BAD_ADDR;
    return (uint32_t)a;
}

int dcn314_stage1_write_refused(void)
{
    return DCN314_E_NOWRITE;
}
