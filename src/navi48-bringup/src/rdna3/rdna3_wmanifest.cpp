// rdna3_wmanifest.cpp - see rdna3_wmanifest.h for the contract.
//
// The manifest is recorded by rdna3_writeguard (header-inline merge); this file owns the
// textual export consumed by the N48R3EVID-write-manifest property.
#include "rdna3_wmanifest.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#endif

size_t r3_wm_format(const R3WManifest *m, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!m)
        return 0;

    size_t used = 0;
    uint32_t i = 0;
    for (; i < m->count; i++) {
        /* Once real text is on the buffer keep the tail free so the truncation
         * markers can always be appended. An empty buffer still tries the first
         * line and falls into the fill-to-cap-1 path below, preserving the
         * "as much as fits, NUL-terminated" contract for tiny buffers. */
        if (used > 0 && cap - used <= 40u)
            break;
        const R3WMEntry &e = m->entries[i];
        const uint32_t len = e.end_off - e.addr + 1u;
        const int n = snprintf(out + used, cap - used,
                               "%s %s 0x%08x+%u x%u %s %s %s\n",
                               r3_wg_substep_name(e.owner), r3_wg_class_name(e.cls),
                               e.addr, len, e.count, r3_wm_status_name(e.status),
                               e.tag ? e.tag : "-", e.ctx ? e.ctx : "-");
        if (n <= 0)
            break;
        if ((size_t)n >= cap - used) {
            used = cap - 1;
            break;
        }
        used += (size_t)n;
    }
    /* The table may be intact while the TEXT buffer ran out: that loss is
     * reported separately from m->truncated (table-full dropped count). */
    if (i < m->count) {
        const int n = snprintf(out + used, cap - used, "text-truncated=%u\n",
                               m->count - i);
        if (n > 0 && (size_t)n < cap - used)
            used += (size_t)n;
    }

    if (m->truncated && used + 1 < cap) {
        const int n = snprintf(out + used, cap - used, "truncated=%u\n", m->dropped);
        if (n > 0 && (size_t)n < cap - used)
            used += (size_t)n;
    }
    return used;
}
