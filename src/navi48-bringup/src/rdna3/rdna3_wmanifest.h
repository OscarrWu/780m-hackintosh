/*
 * rdna3_wmanifest.h - the B1 write manifest for the RDNA3 bringup.
 *
 * Every write classified by rdna3_writeguard is recorded here as
 *   {addr, end_off, count, owner, cls, status, tag, ctx}
 * where end_off is the INCLUSIVE last dword (addr + dwords - 1). The integration exports the
 * table under the chunked IORegistry property N48R3EVID-write-manifest:
 *
 *   r3_wm_format(r3_writeguard_manifest(), buf, sizeof buf)
 *
 * Line format (tech plan §6.2):
 *   <owner> <cls> <start>+<len> x<count> <allow|DENY> <tag> <ctx>
 *
 * Merge rule: an ALLOW entry of class Data (②) merges with an existing entry with the same
 * (owner, cls, addr, end_off, tag). owner is part of the key, so ③ Protocol and ④ State
 * writes never merge across substeps, and within one substep they are per-write lines too:
 * every ③④ write is its own entry (§6.2 "③④ 类逐条记录、不合并"). ctx is the first-seen
 * operation label for a Data entry (the plan's key does not include it). A DENY entry is
 * never merged: B1 requires every refused write to be its own line.
 *
 * R3_WM_MAX is a hard cap. When it is reached, truncated=true and dropped is incremented;
 * the entry is never silently discarded (V10 treats truncated=true as incomplete evidence).
 *
 * record/merge is header-inline so the writeguard can own its manifest without a second
 * translation-unit dependency; formatting lives in rdna3_wmanifest.cpp.
 */
#ifndef N48_RDNA3_WMANIFEST_H
#define N48_RDNA3_WMANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "rdna3_writeguard.h"

#define R3_WM_MAX 256u
#define R3_WM_TEXT_BYTES 32768u
#define R3_WM_EVID "N48R3EVID-write-manifest"

enum class WMStatus : uint8_t { Allow = 0, Deny = 1 };

struct R3WMEntry {
    uint32_t addr;     /* first dword address                    */
    uint32_t end_off;  /* inclusive last dword (addr + len - 1)  */
    uint32_t count;    /* occurrences aggregated into this entry */
    WSubstep owner;    /* substep owner                          */
    WClass cls;        /* B2 side-effect class                   */
    WMStatus status;   /* allow / DENY                           */
    const char *tag;   /* window tag: layout:... / reg:... / ... */
    const char *ctx;   /* caller's operation label               */
};

struct R3WManifest {
    R3WMEntry entries[R3_WM_MAX];
    uint32_t count;
    uint32_t dropped;  /* entries refused after the cap was hit  */
    bool truncated;    /* true => evidence incomplete (V10 fails) */
};

static inline void r3_wm_init(R3WManifest *m)
{
    if (!m)
        return;
    m->count = 0;
    m->dropped = 0;
    m->truncated = false;
}

static inline const char *r3_wm_status_name(WMStatus s)
{
    return s == WMStatus::Allow ? "allow" : "DENY";
}

static inline bool r3_wm_streq(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Record one classified write.
 * Returns 0 when a new entry was stored, 1 when an ALLOW entry was merged,
 * -1 on an invalid argument (null manifest, zero width, 32-bit end overflow),
 * -2 when the table is full (truncated=true, dropped++). */
static inline int r3_wm_record(R3WManifest *m, uint32_t addr, uint32_t dwords,
                               WSubstep owner, WClass cls, WMStatus status,
                               const char *tag, const char *ctx)
{
    if (!m || dwords == 0u)
        return -1;
    if ((uint64_t)addr + (uint64_t)dwords - 1u > 0xFFFFFFFFull)
        return -1;
    const uint32_t end_off = addr + dwords - 1u;

    /* ② Data merges on the grant interval; ③④ are per-write (§6.2). */
    if (status == WMStatus::Allow && cls == WClass::Data) {
        for (uint32_t i = 0; i < m->count; i++) {
            R3WMEntry &e = m->entries[i];
            if (e.status != WMStatus::Allow)
                continue;
            if (e.addr == addr && e.end_off == end_off && e.owner == owner &&
                e.cls == cls && r3_wm_streq(e.tag, tag)) {
                e.count++;
                return 1;
            }
        }
    }

    if (m->count >= R3_WM_MAX) {
        m->truncated = true;
        m->dropped++;
        return -2;
    }

    R3WMEntry &e = m->entries[m->count++];
    e.addr = addr;
    e.end_off = end_off;
    e.count = 1;
    e.owner = owner;
    e.cls = cls;
    e.status = status;
    e.tag = tag;
    e.ctx = ctx;
    return 0;
}

/* Format the manifest. snprintf-style: returns the number of bytes written excluding the NUL.
 * When cap is too small the output is truncated and still NUL-terminated. A truncated table
 * appends "truncated=<dropped>\n". */
size_t r3_wm_format(const R3WManifest *m, char *out, size_t cap);

#endif /* N48_RDNA3_WMANIFEST_H */
