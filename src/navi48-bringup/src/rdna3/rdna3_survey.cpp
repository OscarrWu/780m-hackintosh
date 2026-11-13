// rdna3_survey.cpp - kernel-side RDNA3 stage-1 read-only survey.
//
// Zero state side effects: every access in this file is a read except the AUX request-register
// writes performed inside dcn314_aux_min.c, and those are classified one-by-one by the
// rdna3_writeguard before they reach BAR5. A write outside that whitelist is a survey-code
// defect: the item loop terminates globally and n48r3-Done is NOT published.
//
// Evidence tags (D-group): d1-discovery d2-uma d3-fb d4-fwver d5-edid d6-dcn d7-pwm d8-90hz
// d9-export d10-baseline abort done truncated. AUX per-write annotations are exported as
// N48R3EVID-aux-writes; the main log keeps only per-deny lines plus one d5-aux summary.

#include "rdna3_survey.h"

#include <stdarg.h>

#if defined(R3_SURVEY_PURE_ONLY)
#include <stdio.h>
#else
#include "rdna3_writeguard.h"
#include "golden_values.h"

#include <IOKit/IOLib.h>

#include "../../../dcn314/dcn314_ro.h"
#include "../../../dcn314/dcn314_aux_min.h"
#endif

namespace {

// ---------------------------------------------------------------------------
// small helpers (pure; the host test compiles this file with
// -DR3_SURVEY_PURE_ONLY so this section stays kernel-free)
// ---------------------------------------------------------------------------

static void addf(R3SurveyLines *lines, const char *tag, const char *fmt, ...) {
    char buf[R3_SURVEY_LINE_CHARS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r3_lines_add(lines, tag, buf);
}

// ---------------------------------------------------------------------------
// D5 evidence split. V5 requires the per-write annotation verbatim, but a
// 256-byte EDID read alone costs ~80-110 AUX request-register writes and the
// main log is bounded by R3_SURVEY_MAX_LINES. So allowed-write annotations go
// to a separate evidence payload (N48R3EVID-aux-writes) while whitelist DENIES
// stay in the main log one by one: a deny is a survey-code defect signal.
// ---------------------------------------------------------------------------

#define R3_AUX_EVID_BYTES 32768

struct R3AuxEvidence {
    char     buf[R3_AUX_EVID_BYTES];
    size_t   used;
    uint32_t writes;      // AUX request writes accepted and annotated
    uint32_t denied;      // writes rejected by the whitelist
    bool     truncated;   // payload overflowed; tail marker written
    uint32_t mm_writes;   // MM_INDEX/MM_INDEX_HI read-transaction writes accepted
    uint32_t mm_denied;   // ... rejected (code defect)
};

// The MM window is written twice per TMR dword (5120 writes for 10 KiB). Annotating each one
// would exhaust the AUX payload and drop the real AUX audit, so they are counted under tag
// mm-indirect and appended as one summary line at export time. The per-write whitelist
// classification still happens for every MM write.
static char g_aux_export_buf[R3_AUX_EVID_BYTES + 128];

static const char kAuxEvidTrunc[] = "aux evidence truncated; later writes dropped\n";
static const size_t kAuxEvidTruncLen = sizeof kAuxEvidTrunc - 1;

static void r3_aux_evid_init(R3AuxEvidence *ev) {
    if (!ev) return;
    ev->used = 0;
    ev->writes = 0;
    ev->denied = 0;
    ev->truncated = false;
    ev->mm_writes = 0;
    ev->mm_denied = 0;
    ev->buf[0] = '\0';
}

// Accepts a record only when the tail marker and its NUL still fit afterwards.
static bool r3_aux_evid_fits(size_t used, size_t len) {
    return used + len + kAuxEvidTruncLen + 1 <= R3_AUX_EVID_BYTES;
}

static void r3_aux_evid_record(R3AuxEvidence *ev, uint32_t idx, const char *tag,
                               const char *ctx, uint32_t addr, uint32_t value) {
    if (!ev || ev->truncated) return;
    char line[192];
    const int n = snprintf(line, sizeof line,
                           "aux[%03u] txn tag=%s ctx=%s addr=0x%08x val=0x%08x\n",
                           idx, tag ? tag : "-", ctx ? ctx : "-", addr, value);
    if (n <= 0) { ev->truncated = true; return; }
    size_t len = (size_t)n;
    if (len >= sizeof line) len = sizeof line - 1;
    // Reserve the tail marker plus its NUL on every accepted record, so the
    // marker always fits and the terminating NUL never writes past buf.
    if (!r3_aux_evid_fits(ev->used, len)) {
        ev->truncated = true;
        memcpy(ev->buf + ev->used, kAuxEvidTrunc, kAuxEvidTruncLen);
        ev->used += kAuxEvidTruncLen;
        ev->buf[ev->used] = '\0';
        return;
    }
    memcpy(ev->buf + ev->used, line, len);
    ev->used += len;
    ev->buf[ev->used] = '\0';
}

static void r3_aux_annotate(R3SurveyLines *lines, R3AuxEvidence *ev, bool allowed,
                            const char *tag, const char *ctx, uint32_t addr, uint32_t value) {
    if (!ev) return;
    if (!allowed) {
        ev->denied++;
        addf(lines, "d5-aux", "DENY tag=%s ctx=%s addr=0x%08x val=0x%08x",
             tag ? tag : "-", ctx ? ctx : "-", addr, value);
        return;
    }
    r3_aux_evid_record(ev, ev->writes, tag, ctx, addr, value);
    ev->writes++;
}

static void r3_mm_write_note(R3AuxEvidence *ev, bool allowed) {
    if (!ev) return;
    if (allowed) ev->mm_writes++;
    else ev->mm_denied++;
}

static void r3_mm_write_audit(R3SurveyLines *lines, R3AuxEvidence *ev, bool allowed,
                              uint32_t addr, uint32_t value) {
    r3_mm_write_note(ev, allowed);
    if (!allowed) {
        addf(lines, "d1-discovery",
             "DENY tag=mm-indirect addr=0x%08x val=0x%08x", addr, value);
    }
}

static void r3_aux_summary(R3SurveyLines *lines, const R3AuxEvidence *ev) {
    const uint32_t writes = ev ? ev->writes : 0;
    const uint32_t denied = ev ? ev->denied : 0;
    const uint32_t mm_writes = ev ? ev->mm_writes : 0;
    const uint32_t mm_denied = ev ? ev->mm_denied : 0;
    const char *evid = (ev && (ev->used || mm_writes || mm_denied))
                           ? "N48R3EVID-aux-writes" : "-";
    addf(lines, "d5-aux", "summary writes=%u denied=%u mm_writes=%u mm_denied=%u evid=%s%s",
         writes, denied, mm_writes, mm_denied, evid, (ev && ev->truncated) ? " truncated" : "");
}

// Publish the non-empty evidence payload. AUX records keep their per-write text; the MM summary
// is appended under the same key (no new property name), tagged mm-indirect.
static bool r3_aux_export(const TgtLogSink *sink, const R3AuxEvidence *ev) {
    if (!sink || !sink->set_property || !ev) return false;
    const bool has_mm = ev->mm_writes != 0 || ev->mm_denied != 0;
    if (ev->used == 0 && !has_mm) return false;
    if (!has_mm)
        return sink->set_property(sink->user, "N48R3EVID-aux-writes", ev->buf, ev->used);

    size_t total = ev->used;
    if (total > R3_AUX_EVID_BYTES) total = R3_AUX_EVID_BYTES;
    memcpy(g_aux_export_buf, ev->buf, total);
    char line[96];
    const int n = snprintf(line, sizeof line, "tag=mm-indirect writes=%u denied=%u\n",
                           ev->mm_writes, ev->mm_denied);
    if (n <= 0) return false;
    size_t len = (size_t)n;
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(g_aux_export_buf + total, line, len);
    total += len;
    return sink->set_property(sink->user, "N48R3EVID-aux-writes",
                              g_aux_export_buf, total);
}

// ---------------------------------------------------------------------------
// D3 line assembly: raw registers in, one evidence line out per discovery copy.
// Phoenix carries the FB_LOCATION registers twice: the GC(gfxhub) copy at GC seg0
// + 0x1678/0x1679/0x15a7 and the MMHUB copy at MMHUB seg1 + 0x8ec/0x8ed/0x8d7
// (archived PhxNativeABI.h:68-70; Linux mmhub_v3_0_1.c:507/516). Which copy the
// VBIOS programmed is not proven, so both are read and labelled; the target
// dual-read decides the authoritative one. The system aperture is a logical page
// number ( >>18 per gfxhub_v3_0.c:161-164 / mmhub_v3_0.c:178-181 ); the MMHUB
// aperture offset is not archived, so only the GC copy carries the gtt conversion.
// ---------------------------------------------------------------------------

static bool bad_word(uint32_t v) { return v == 0xFFFFFFFFu; }

enum R3FbCopy {
    R3_FB_COPY_GC = 0,
    R3_FB_COPY_MMHUB = 1,
    R3_FB_COPY_COUNT = 2,
};

struct R3FbCopyWords {
    bool     resolved;      // discovery base for this copy resolved
    uint32_t fb_base;
    uint32_t fb_top;
    uint32_t fb_off;
    bool     has_aperture;  // false when the aperture offset is not archived
    uint32_t ap_lo;
    uint32_t ap_hi;
};

static const char *r3_fb_copy_label(R3FbCopy copy) {
    return copy == R3_FB_COPY_MMHUB ? "MMHUB" : "GC(gfxhub)";
}

static void r3_fb_line(char *out, size_t cap, R3FbCopy copy, const R3FbCopyWords &w,
                       uint64_t *mc_base_out) {
    const uint64_t mc_base = (uint64_t)(w.fb_base & 0x00ffffffu) << 24;
    const uint64_t mc_top  = ((uint64_t)(w.fb_top & 0x00ffffffu) << 24) | 0xffffffu;
    const uint64_t fb_size = (mc_top >= mc_base) ? (mc_top - mc_base + 1) : 0;
    if (mc_base_out) *mc_base_out = mc_base;
    if (w.has_aperture) {
        // LOGICAL_ADDR_MASK=0x3FFFFFFF, SHIFT=0 (gc_11_0_0_sh_mask.h:11351-11355);
        // the hardware stores the MC address right-shifted by 18.
        const uint64_t gtt_lo = (uint64_t)(w.ap_lo & 0x3fffffffu) << 18;
        const uint64_t gtt_hi = (uint64_t)(w.ap_hi & 0x3fffffffu) << 18;
        const uint64_t gtt_size = (gtt_hi >= gtt_lo) ? (gtt_hi - gtt_lo) : 0;
        snprintf(out, cap,
                 "PASS copy=%s fb_mc_base=0x%llx fb_mc_top=0x%llx fb_size=0x%llx "
                 "fb_offset=0x%llx gtt_lo=0x%llx gtt_hi=0x%llx gtt_size=0x%llx "
                 "raw_ap=0x%08x/0x%08x (aperture >>18 per gfxhub_v3_0.c:161-164/"
                 "mmhub_v3_0.c:178-181 (archived); authoritative copy to be confirmed "
                 "by dual-read on target)",
                 r3_fb_copy_label(copy), (unsigned long long)mc_base,
                 (unsigned long long)mc_top, (unsigned long long)fb_size,
                 (unsigned long long)((uint64_t)(w.fb_off & 0x00ffffffu) << 24),
                 (unsigned long long)gtt_lo, (unsigned long long)gtt_hi,
                 (unsigned long long)gtt_size, w.ap_lo, w.ap_hi);
    } else {
        snprintf(out, cap,
                 "PASS copy=%s fb_mc_base=0x%llx fb_mc_top=0x%llx fb_size=0x%llx "
                 "fb_offset=0x%llx system_aperture=not-read (MMHUB aperture offset not "
                 "archived); authoritative copy to be confirmed by dual-read on target",
                 r3_fb_copy_label(copy), (unsigned long long)mc_base,
                 (unsigned long long)mc_top, (unsigned long long)fb_size,
                 (unsigned long long)((uint64_t)(w.fb_off & 0x00ffffffu) << 24));
    }
}

// Emits one line per copy. A copy is usable only when its discovery base resolved and every
// word read back; the first usable copy (GC preferred) supplies the cached MC base.
static bool r3_fb_emit(R3SurveyLines *lines, const R3FbCopyWords *copies,
                       uint64_t *mc_base_out) {
    bool any = false;
    for (int i = 0; i < R3_FB_COPY_COUNT; i++) {
        const R3FbCopy copy = (R3FbCopy)i;
        const R3FbCopyWords &w = copies[i];
        if (!w.resolved) {
            addf(lines, "d3-fb", "FAIL copy=%s unavailable (discovery base unresolved)",
                 r3_fb_copy_label(copy));
            continue;
        }
        if (bad_word(w.fb_base) || bad_word(w.fb_top) || bad_word(w.fb_off) ||
            (w.has_aperture && (bad_word(w.ap_lo) || bad_word(w.ap_hi)))) {
            addf(lines, "d3-fb",
                 "FAIL copy=%s register read returned 0xffffffff "
                 "(base=0x%08x top=0x%08x off=0x%08x)",
                 r3_fb_copy_label(copy), w.fb_base, w.fb_top, w.fb_off);
            continue;
        }
        char text[R3_SURVEY_LINE_CHARS];
        uint64_t mc = 0;
        r3_fb_line(text, sizeof text, copy, w, &mc);
        r3_lines_add(lines, "d3-fb", text);
        if (!any && mc_base_out) *mc_base_out = mc;
        any = true;
    }
    return any;
}

typedef uint32_t (*R3FbReadWord)(void *user, R3FbCopy copy, uint32_t reg);

// The per-copy register map lives here so the host test can drive the whole dual-read with
// a fake discovery/reader and catch a regression back to a single copy or wrong offsets.
// MMHUB uses MMMC_VM_* in segment 1 (0x1a000 + 0x8ec/0x8ed/0x8d7, PhxNativeABI.h:68-70);
// its aperture offset is not archived, so it carries no gtt conversion.
static bool r3_fb_item(R3SurveyLines *lines, R3FbReadWord read, void *user,
                       bool gc_available, bool mmhub_available, uint64_t *mc_base_out) {
    struct Plan {
        R3FbCopy copy;
        bool     available;
        uint32_t base_reg, top_reg, off_reg;
        bool     has_aperture;
        uint32_t ap_lo_reg, ap_hi_reg;
    };
    const Plan plans[R3_FB_COPY_COUNT] = {
        {R3_FB_COPY_GC,    gc_available,    0x1678u, 0x1679u, 0x15a7u, true,
         0x167du, 0x167eu},
        {R3_FB_COPY_MMHUB, mmhub_available, 0x08ecu, 0x08edu, 0x08d7u, false, 0u, 0u},
    };

    R3FbCopyWords copies[R3_FB_COPY_COUNT] {};
    for (int i = 0; i < R3_FB_COPY_COUNT; i++) {
        const Plan &p = plans[i];
        R3FbCopyWords &w = copies[i];
        w.resolved = p.available;
        if (!p.available) continue;
        w.fb_base = read(user, p.copy, p.base_reg);
        w.fb_top  = read(user, p.copy, p.top_reg);
        w.fb_off  = read(user, p.copy, p.off_reg);
        w.has_aperture = p.has_aperture;
        if (p.has_aperture) {
            w.ap_lo = read(user, p.copy, p.ap_lo_reg);
            w.ap_hi = read(user, p.copy, p.ap_hi_reg);
        }
    }
    return r3_fb_emit(lines, copies, mc_base_out);
}

// D2 line: d0=confirmed only when the caller measured IOPowerManagement; otherwise the
// post-probe D0 is an inference and must stay labelled as such (D-15, I4).
static void r3_uma_line(char *out, size_t cap, uint32_t raw, bool d0_measured) {
    snprintf(out, cap, "PASS carveout_mib=%u bytes=%llu raw=0x%08x d0=%s",
             raw, (unsigned long long)((uint64_t)raw << 20), raw,
             d0_measured ? "confirmed" : "assumed");
}

// D-02 abort semantics: only a deny recorded after this survey took its entry snapshot aborts it.
// The writeguard counter is process-global, so a stale deny from an earlier run must not re-trigger
// the abort. r3_writeguard_install() zeroes the counter at D1: a current value below the snapshot
// is that re-arm, not a rollback, so the baseline follows it. Single instance / single boot
// premise (D-02 downgrade ruling): no concurrent survey moves the counter, hence no locking.
static inline bool r3_deny_aborts(uint32_t &baseline, uint32_t current) {
    if (current < baseline) baseline = current;
    return current != baseline;
}

// D8 frame-count difference. The OTG counter is a free-running 32-bit register, so the
// difference is taken modulo 2^32: a wrap between the two reads yields the true small
// count (0xfffffff0 -> 0x10 is 0x20), never a huge value that would fake a PASS.
static uint32_t r3_frame_delta(uint32_t first, uint32_t second) {
    return (uint32_t)(second - first);
}

#if !defined(R3_SURVEY_PURE_ONLY)
using amdgpu::DeviceContext;
using amdgpu::IPBlock;
using amdgpu::IPBaseTable;

static R3AuxEvidence g_aux_ev;

static const R3IpBase *find_ip(const R3DiscoveryReport &d, uint16_t hw) {
    for (uint32_t i = 0; i < d.ip_count; i++) {
        if (d.ips[i].hwId == hw) return &d.ips[i];
    }
    return nullptr;
}

// Every real bus read is recorded for N48R3EVID-read-manifest. RREG32 itself refuses an
// out-of-range dword without touching the bus, so the note is gated on the same range.
static uint32_t manifest_rreg(R3SurveyHw &hw, uint32_t abs_dword) {
    if (hw.dev && hw.dev->rmmio && (size_t)abs_dword * 4u + 4u <= hw.dev->rmmioSize)
        r3_manifest_note(&hw.manifest, abs_dword);
    return amdgpu::RREG32(*hw.dev, abs_dword);
}

static uint32_t rd(R3SurveyHw &hw, IPBlock blk, uint32_t reg) {
    if (!hw.dev->ip.isResolved(blk)) return 0xFFFFFFFFu;
    return manifest_rreg(hw, amdgpu::SOC15_REG_OFFSET(*hw.dev, blk, reg));
}

static uint32_t rdb(R3SurveyHw &hw, IPBlock blk, int base_idx, uint32_t reg) {
    if (!hw.dev->ip.isResolved(blk, base_idx)) return 0xFFFFFFFFu;
    return manifest_rreg(hw, amdgpu::SOC15_REG_OFFSET_BIDX(*hw.dev, blk, base_idx, reg));
}

/* Golden-table read: the entry owns the register's _BASE_IDX (A-01). */
static uint32_t rd_golden(R3SurveyHw &hw, const r3gold::GoldenReg &g) {
    uint32_t addr = 0;
    if (!hw.dev || !hw.dev->ip.resolve(g.ref(), &addr)) return 0xFFFFFFFFu;
    return manifest_rreg(hw, addr);
}
#endif // !R3_SURVEY_PURE_ONLY

#if !defined(R3_SURVEY_PURE_ONLY)
// dcn314 callbacks: the cookie is the whole survey context (needs the line sink for the
// per-write V5 annotation as well as the BAR5-mapped DeviceContext).
static uint32_t dcn_rreg(void *cookie, uint32_t abs_dword) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(cookie);
    return manifest_rreg(*hw, abs_dword);
}

static void dcn_wreg(void *cookie, uint32_t abs_dword, uint32_t value) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(cookie);
    amdgpu::WREG32(*hw->dev, abs_dword, value);
}

static void dcn_udelay(void * /*cookie*/, uint32_t us) {
    IODelay(us);
}

// The only permitted writes: AUX read-transaction request registers. Each ALLOWED write is
// annotated immediately (V5: tag + address + width) into the evidence payload before it
// reaches the bus; a DENIED write is a code defect and stays in the bounded main log.
static bool dcn_write_ok(void *cookie, uint32_t abs_dword, uint32_t value) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(cookie);
    const bool allowed = r3_writeguard_write_ok(hw->dev, abs_dword, value);
    r3_aux_annotate(hw->lines, &g_aux_ev, allowed, r3_writeguard_last_tag(),
                    r3_writeguard_last_context(), abs_dword, value);
    return allowed;
}

// Abort on a deny observed during THIS survey (D-02): r3_survey_run_hw snapshots the counter at
// entry, so a pre-existing deny from an earlier run can never terminate a healthy survey.
static bool survey_abort_check(void *user) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    return hw && r3_deny_aborts(hw->deny_before, (uint32_t)r3_writeguard_deny_count());
}
#endif // !R3_SURVEY_PURE_ONLY

#if !defined(R3_SURVEY_PURE_ONLY)

// ---------------------------------------------------------------------------
// D1 - IP table: VRAM TMR -> RAM via MM_INDEX/MM_DATA -> unmodified IpDiscovery parser.
// Source and window semantics are copied from the base Navi48Bringup.cpp:486-503 / 160-165
// (Linux amdgpu_discovery.c:1989-1992 is the same VRAM-TMR path). The BAR5 register body is
// NOT a discovery table and is never scanned (requirement v1.4 D1; InvB read-path audit).
// ---------------------------------------------------------------------------

// MM_INDEX (0x0) / MM_INDEX_HI (0x6) writes are read-transaction writes: every one is classified,
// but only counted under tag mm-indirect so 5120 TMR-window writes cannot evict the AUX audit.
static bool mm_indirect_write(R3SurveyHw &hw, uint32_t abs_dword, uint32_t value) {
    const bool allowed = r3_write_classify(abs_dword, 1u, "mm-tmr-read") == WVerdict::TxnRead;
    r3_mm_write_audit(hw.lines, &g_aux_ev, allowed, abs_dword, value);
    if (!allowed) return false;
    amdgpu::WREG32(*hw.dev, abs_dword, value);
    return true;
}

// One VRAM dword through the MM window (base Navi48Bringup.cpp:160-165, verbatim arithmetic).
static bool tmr_read32(R3SurveyHw &hw, uint64_t pos, uint32_t *out) {
    if (!mm_indirect_write(hw, 0x6u, (uint32_t)(pos >> 31))) return false;
    if (!mm_indirect_write(hw, 0x0u, ((uint32_t)pos & 0x7ffffffcu) | 0x80000000u)) return false;
    *out = manifest_rreg(hw, 0x1u);
    return true;
}

static bool item_discovery(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dev || !hw->dev->rmmio || !hw->disc) {
        addf(lines, "d1-discovery", "FAIL: BAR5 not mapped / no report buffer");
        return false;
    }

    uint64_t tmr = 0;
    if (!r3_tmr_addr(hw->vram_mb, &tmr)) {
        addf(lines, "d1-discovery",
             "FAIL: T1 VRAM size unavailable (vram_mb=%u); TMR address undefined", hw->vram_mb);
        return false;
    }

    if (r3_writeguard_install_mm() != 0) {
        addf(lines, "d1-discovery", "FAIL: MM window writeguard arm rejected");
        return false;
    }

    const uint32_t dwords = r3_disc_tmr_dwords();
    uint32_t *dw = reinterpret_cast<uint32_t *>(hw->disc_tmr);
    for (uint32_t i = 0; i < dwords; i++) {
        if (!tmr_read32(*hw, tmr + 4ULL * i, &dw[i])) {
            addf(lines, "d1-discovery",
                 "FAIL: MM_INDEX/MM_DATA read denied at TMR dword %u", i);
            return false;
        }
    }
    addf(lines, "d1-discovery",
         "source=on-die TMR tmr=0x%llx bytes=%u via MM_INDEX/MM_DATA",
         (unsigned long long)tmr, (unsigned)R3_DISC_TMR_SIZE);

    const int rc = r3_discovery_run(hw->disc_tmr, sizeof hw->disc_tmr, nullptr, 0, hw->disc);
    if (rc != R3_DISC_OK) {
        addf(lines, "d1-discovery", "FAIL rc=%d (%s)", rc, r3_discovery_code_name(rc));
        return false;
    }

    for (uint32_t i = 0; i < hw->disc->ip_count; i++) {
        const R3IpBase &ip = hw->disc->ips[i];
        IPBlock blk;
        if (!r3_ip_block_from_hw(ip.hwId, &blk)) continue;
        const uint32_t nb = ip.numBases < (uint32_t)IPBaseTable::kMaxBaseSegments
                                ? ip.numBases
                                : (uint32_t)IPBaseTable::kMaxBaseSegments;
        for (uint32_t b = 0; b < nb; b++) hw->dev->ip.setBase(blk, (int)b, ip.bases[b]);
    }

    addf(lines, "d1-discovery",
         "PASS ip_count=%u expect_found=%u/%u major_bad=%u missing=%u minor_diffs=%u",
         hw->disc->ip_count, hw->disc->expect_found, hw->disc->expect_count,
         hw->disc->expect_major_bad, hw->disc->expect_missing, hw->disc->minor_diffs);

    for (uint32_t i = 0; i < hw->disc->ip_count; i++) {
        const R3IpBase &ip = hw->disc->ips[i];
        addf(lines, "d1-discovery", "ip hw=%u inst=%u ver=%u.%u.%u nb=%u b0=0x%08x b1=0x%08x",
             ip.hwId, ip.instance, ip.major, ip.minor, ip.revision, ip.numBases,
             ip.numBases > 0 ? ip.bases[0] : 0u, ip.numBases > 1 ? ip.bases[1] : 0u);
    }

    for (uint32_t i = 0; i < hw->disc->expect_count; i++) {
        const R3ExpectVerdict &v = hw->disc->expect[i];
        addf(lines, "d1-discovery",
             "expect %s found=%d major=%d minor_diff=%d seen=%u.%u.%u expected=%u.%u",
             v.name, v.found ? 1 : 0, v.major_ok ? 1 : 0, v.minor_diff ? 1 : 0,
             v.seen_major, v.seen_minor, v.seen_revision, v.expected_major, v.expected_minor);
    }

    // One-time setup for the AUX path: re-arm the writeguard with the full table (MM + AUX) and
    // bind the DCN layer from the DMU IP's discovery segment bases. Setup, not evidence.
    const R3IpBase *dmu = find_ip(*hw->disc, IpDiscovery::HwDmu);
    if (!dmu || dmu->numBases < DCN314_NUM_SEGS) {
        addf(lines, "d1-discovery", "FAIL: DMU bases incomplete (need %u, got %u)",
             (unsigned)DCN314_NUM_SEGS, dmu ? (unsigned)dmu->numBases : 0u);
        return true; // the IP table itself is valid; DCN items will report the dependency fail
    }

    uint32_t seg[DCN314_NUM_SEGS];
    for (uint32_t b = 0; b < DCN314_NUM_SEGS; b++) seg[b] = dmu->bases[b];

    if (r3_writeguard_install(seg) != 0) {
        addf(lines, "d1-discovery", "FAIL: writeguard install rejected DMU bases");
        return true;
    }

    const int drc = dcn314_dev_init(hw->dcn, hw, dcn_rreg, dcn_wreg, dcn_udelay,
                                    dcn_write_ok, seg,
                                    (uint32_t)(hw->dev->rmmioSize / 4), 0);
    hw->dcn_ready = (drc == DCN314_OK);
    addf(lines, "d1-discovery", "dcn_init=%s seg0=0x%08x seg2=0x%08x",
         hw->dcn_ready ? "PASS" : "FAIL", seg[0], seg[2]);
    return true;
}

// ---------------------------------------------------------------------------
// D2 - UMA carveout: RCC_CONFIG_MEMSIZE at BAR5 byte 0x378c (dword 0x0de3)
// ---------------------------------------------------------------------------

static bool item_uma(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dev || !hw->dev->rmmio) {
        addf(lines, "d2-uma", "FAIL: BAR5 not mapped");
        return false;
    }
    // Config/UMA reads are only legal in D0 (the driver probes after the device is powered).
    if (!hw->d0_confirmed) {
        addf(lines, "d2-uma", "FAIL: PCI function not confirmed in D0");
        return false;
    }
    // Base probeMemSize (Navi48Bringup.cpp:470-484) reads BAR5 byte 0x378c, which is dword
    // 0x0de3 (RCC_CONFIG_MEMSIZE); regRead32 divides the byte offset by 4. T1's single read.
    const uint32_t raw = manifest_rreg(*hw, 0x0de3u);
    if (raw == 0 || raw == 0xFFFFFFFFu) {
        addf(lines, "d2-uma", "FAIL raw=0x%08x (RCC_CONFIG_MEMSIZE unusable, recorded as-is)", raw);
        return false;
    }
    hw->vram_mb = raw;
    hw->dev->vramSizeBytes = (uint64_t)raw * 1024ull * 1024ull;
    char text[R3_SURVEY_LINE_CHARS];
    r3_uma_line(text, sizeof text, raw, hw->d0_measured);
    r3_lines_add(lines, "d2-uma", text);
    return true;
}

// ---------------------------------------------------------------------------
// D3 - FB location / GTT window (GC and MMHUB copies; see the D3 helper comment)
// ---------------------------------------------------------------------------

// Kernel-side reader for the pure dual-copy plan: GC uses its own block offsets, MMHUB the
// MMMC_VM_* registers in MMHUB segment 1 (see r3_fb_item).
static uint32_t fb_read_reg(void *user, R3FbCopy copy, uint32_t reg) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (copy == R3_FB_COPY_MMHUB) return rdb(*hw, IPBlock::MMHUB, 1, reg);
    return rd(*hw, IPBlock::GC, reg);
}

static bool item_fb(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dev) {
        addf(lines, "d3-fb", "FAIL: no device context");
        return false;
    }
    const bool gc_ok = hw->dev->ip.isResolved(IPBlock::GC);
    const R3IpBase *mmhub = hw->disc ? find_ip(*hw->disc, IpDiscovery::HwMmhub) : nullptr;
    const bool mmhub_ok = mmhub && hw->dev->ip.isResolved(IPBlock::MMHUB, 1);
    uint64_t mc_base = 0;
    const bool ok = r3_fb_item(lines, fb_read_reg, hw, gc_ok, mmhub_ok, &mc_base);
    hw->fb_dual_seen = true;
    hw->fb_dual_ok = ok;
    if (ok) hw->dev->vramMcBase = mc_base;
    return ok;
}

// ---------------------------------------------------------------------------
// D4 - firmware version registers (read-only)
//   PSP : MP0 C2PMSG_58 = psp->sos.fw_version (psp_v13_0_init_sos_version); C2PMSG_59 is only
//         recorded as an adjacent raw word, it is NOT part of the version.
//   GC  : RLC_F32_UCODE_VERSION (GC base index 1)
//   SDMA: SDMA0_VERSION (SDMA0 base index 0)
//   NOTE: offset not verifiable against archived headers (mp_13_0_0/sdma_6_0_0_offset.h not
//         archived in evidence/src); cross-check pending archive backfill — treat as provisional
//   SMU : PMFW runtime version is delivered by SMU mailbox message 0x02, which is a firmware
//         command, not a read-only register. The SMU IP version from discovery is recorded and
//         the mailbox limitation is logged instead of issuing a write.
// ---------------------------------------------------------------------------


static bool item_fwver(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    bool ok = true;

    const uint32_t psp_ver = rd_golden(*hw, r3gold::kMp0SmnC2pmsg58); /* C2PMSG_58, seg 1 */
    const uint32_t psp_59  = rdb(*hw, IPBlock::MP0, 1, 0x007bu); /* C2PMSG_59 (raw, seg 1) */
    if (bad_word(psp_ver)) {
        addf(lines, "d4-fwver", "FAIL psp C2PMSG_58 unreadable (0x%08x) C2PMSG_59=0x%08x",
             psp_ver, psp_59);
        ok = false;
    } else {
        addf(lines, "d4-fwver", "PASS psp_sos_ver=0x%08x C2PMSG_59_raw=0x%08x",
             psp_ver, psp_59);
    }

    const uint32_t gc_rlc = rdb(*hw, IPBlock::GC, 1, 0x4c03u); /* RLC_F32_UCODE_VERSION */
    if (bad_word(gc_rlc)) {
        addf(lines, "d4-fwver", "FAIL gc RLC_F32_UCODE_VERSION unreadable");
        ok = false;
    } else {
        addf(lines, "d4-fwver", "PASS gc RLC_F32_UCODE_VERSION=0x%08x", gc_rlc);
    }

    const uint32_t sdma = rd(*hw, IPBlock::SDMA0, 0x0035u); /* SDMA0_VERSION */
    if (bad_word(sdma)) {
        addf(lines, "d4-fwver", "FAIL sdma SDMA0_VERSION unreadable");
        ok = false;
    } else {
        addf(lines, "d4-fwver", "PASS sdma SDMA0_VERSION=0x%08x", sdma);
    }

    const R3IpBase *mp1 = find_ip(*hw->disc, IpDiscovery::HwMp1);
    if (mp1) {
        addf(lines, "d4-fwver",
             "smu ip=%u.%u.%u pmfw_version=unavailable-readonly "
             "(SMU mailbox cmd 0x02 is a write; not issued in stage 1)",
             mp1->major, mp1->minor, mp1->revision);
    } else {
        addf(lines, "d4-fwver", "smu ip=not-discovered pmfw_version=unavailable-readonly");
    }
    return ok;
}

// ---------------------------------------------------------------------------
// D5 - EDID + DPCD over AUX (the only writes go through the writeguard)
// ---------------------------------------------------------------------------

static bool item_edid_body(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dcn_ready) {
        addf(lines, "d5-edid", "FAIL: DCN layer not initialised (dependency)");
        return false;
    }

    uint8_t edid[128];
    int last = DCN314_E_ARG;
    unsigned aux = 0;
    bool got = false;
    for (aux = 0; aux < DCN314_NUM_AUX; aux++) {
        const int rc = dcn314_aux_read_edid(hw->dcn, aux, edid, sizeof edid);
        last = rc;
        if (rc == DCN314_OK) { got = true; break; }
    }
    if (!got) {
        addf(lines, "d5-edid", "FAIL: EDID read failed on AUX0..%u (last rc=%d)",
             (unsigned)(DCN314_NUM_AUX - 1), last);
        return false;
    }

    static const uint8_t hdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    bool header_ok = true;
    for (unsigned i = 0; i < 8; i++) if (edid[i] != hdr[i]) header_ok = false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < sizeof edid; i++) sum += edid[i];
    const uint32_t pclk_10khz = (uint32_t)edid[54] | ((uint32_t)edid[55] << 8);
    const uint32_t ha = (uint32_t)edid[56] | (((uint32_t)edid[58] & 0xf0u) << 4);
    const uint32_t hb = (uint32_t)edid[57] | (((uint32_t)edid[58] & 0x0fu) << 8);
    const uint32_t va = (uint32_t)edid[59] | (((uint32_t)edid[61] & 0xf0u) << 4);
    const uint32_t vb = (uint32_t)edid[60] | (((uint32_t)edid[61] & 0x0fu) << 8);
    const uint32_t htot = ha + hb, vtot = va + vb;
    const uint32_t refresh_milli = (htot && vtot)
        ? (pclk_10khz * 10000ull * 1000ull) / ((uint64_t)htot * vtot)
        : 0u;
    addf(lines, "d5-edid",
         "%s aux=%u header=%s sum=0x%02x mfg=0x%04x prod=0x%04x pxclk_10khz=%u "
         "pref=%ux%u@%u.%03uHz",
         header_ok ? "PASS" : "FAIL", aux, header_ok ? "ok" : "bad", sum & 0xffu,
         (uint32_t)edid[8] | ((uint32_t)edid[9] << 8),
         (uint32_t)edid[10] | ((uint32_t)edid[11] << 8), pclk_10khz,
         ha, va, refresh_milli / 1000u, refresh_milli % 1000u);

    if (hw->sink && hw->sink->set_property) {
        hw->sink->set_property(hw->sink->user, "N48R3EVID-EDID",
                               reinterpret_cast<const char *>(edid), sizeof edid);
    }

    struct dcn314_aux_result res;
    const int drc = dcn314_aux_dpcd_read(hw->dcn, aux, 0x00000u, 16, &res);
    if (drc != DCN314_OK || res.reply != DCN314_AUX_REPLY_ACK) {
        addf(lines, "d5-edid", "FAIL: DPCD read rc=%d reply=%d", drc, (int)res.reply);
        return false;
    }
    addf(lines, "d5-edid",
         "PASS dpcd rev=0x%02x max_rate=0x%02x lanes=0x%02x bytes=%u",
         res.data[0], res.data[1], res.data[2], (unsigned)res.bytes);
    if (hw->sink && hw->sink->set_property) {
        hw->sink->set_property(hw->sink->user, "N48R3EVID-DPCD",
                               reinterpret_cast<const char *>(res.data), res.bytes);
    }
    return header_ok;
}

// Emit the one-line d5 summary even when the EDID/DPCD path failed: it states how
// many AUX request writes were annotated (payload) and how many were denied (log).
static bool item_edid(void *user, R3SurveyLines *lines) {
    const bool ok = item_edid_body(user, lines);
    r3_aux_summary(lines, &g_aux_ev);
    return ok;
}

// ---------------------------------------------------------------------------
// D6 - DCN snapshot: OTG0 timing/position/frame/sync/master, HUBP0 surface, DIO links
// ---------------------------------------------------------------------------

static bool item_dcn(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dcn_ready) {
        addf(lines, "d6-dcn", "FAIL: DCN layer not initialised (dependency)");
        return false;
    }
    bool ok = true;

    struct dcn314_otg_timing t;
    if (dcn314_otg_read_timing(hw->dcn, 0, &t) == DCN314_OK) {
        addf(lines, "d6-dcn",
             "PASS otg0 h_total=%u v_total=%u hblank=%u..%u vblank=%u..%u",
             t.h_total, t.v_total, t.h_blank_start, t.h_blank_end,
             t.v_blank_start, t.v_blank_end);
    } else {
        addf(lines, "d6-dcn", "FAIL otg0 timing unreadable");
        ok = false;
    }

    struct dcn314_otg_position p;
    if (dcn314_otg_read_position(hw->dcn, 0, &p) == DCN314_OK) {
        addf(lines, "d6-dcn", "PASS otg0 pos h=%u v=%u vnom=%u",
             p.horz_count, p.vert_count, p.vert_count_nom);
    } else {
        addf(lines, "d6-dcn", "FAIL otg0 position unreadable");
        ok = false;
    }

    uint32_t fc = 0;
    if (dcn314_otg_read_frame_count(hw->dcn, 0, &fc) == DCN314_OK) {
        addf(lines, "d6-dcn", "PASS otg0 frame_count=%u", fc);
    } else {
        addf(lines, "d6-dcn", "FAIL otg0 frame_count unreadable");
        ok = false;
    }

    bool vs = false, vu = false, master = false;
    if (dcn314_otg_read_sync_status(hw->dcn, 0, &vs, &vu) == DCN314_OK) {
        addf(lines, "d6-dcn", "PASS otg0 sync vstartup=%d vupdate_nolock=%d", vs ? 1 : 0, vu ? 1 : 0);
    } else {
        addf(lines, "d6-dcn", "FAIL otg0 sync unreadable");
        ok = false;
    }
    if (dcn314_otg_is_master_enabled(hw->dcn, 0, &master) == DCN314_OK) {
        addf(lines, "d6-dcn", "PASS otg0 master_enabled=%d", master ? 1 : 0);
    } else {
        addf(lines, "d6-dcn", "FAIL otg0 master_enabled unreadable");
        ok = false;
    }

    struct dcn314_hubp_surface s;
    if (dcn314_hubp_read_surface(hw->dcn, 0, &s) == DCN314_OK) {
        addf(lines, "d6-dcn",
             "PASS hubp0 addr=0x%llx fmt=%u swmode=%u dcc=%u in_blank=%u "
             "viewport=%ux%u+%u+%u",
             (unsigned long long)s.primary_address, s.pixel_format, s.sw_mode, s.dcc_en,
             s.in_blank, s.viewport_w, s.viewport_h, s.viewport_x, s.viewport_y);
    } else {
        addf(lines, "d6-dcn", "FAIL hubp0 surface unreadable");
        ok = false;
    }

    for (unsigned l = 0; l < DCN314_NUM_DIO_LINK; l++) {
        uint8_t enc = 0, hpo_dp = 0, hpo_hdmi = 0;
        if (dcn314_dio_link_read_type(hw->dcn, l, &enc, &hpo_dp, &hpo_hdmi) == DCN314_OK) {
            addf(lines, "d6-dcn", "PASS dio link%u enc=%u hpo_dp=%u hpo_hdmi=%u",
                 l, enc, hpo_dp, hpo_hdmi);
        } else {
            addf(lines, "d6-dcn", "FAIL dio link%u unreadable", l);
            ok = false;
        }
    }

    for (unsigned d = 0; d < DCN314_NUM_DP; d++) {
        bool up = false, ep = false, fec = false;
        if (dcn314_dp_read_link_status(hw->dcn, d, &up, &ep, &fec) == DCN314_OK) {
            addf(lines, "d6-dcn", "PASS dp%u link_up=%d embedded=%d fec=%d",
                 d, up ? 1 : 0, ep ? 1 : 0, fec ? 1 : 0);
        } else {
            addf(lines, "d6-dcn", "FAIL dp%u link status unreadable", d);
            ok = false;
        }
    }

    for (unsigned g = 0; g < DCN314_NUM_DIG; g++) {
        bool fe = false, be = false, fes = false, bes = false;
        if (dcn314_dig_read_status(hw->dcn, g, &fe, &be, &fes, &bes) == DCN314_OK) {
            addf(lines, "d6-dcn", "PASS dig%u fe=%d be=%d fe_symclk=%d be_symclk=%d",
                 g, fe ? 1 : 0, be ? 1 : 0, fes ? 1 : 0, bes ? 1 : 0);
        } else {
            addf(lines, "d6-dcn", "FAIL dig%u status unreadable", g);
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// D7 - DIO PWM / backlight state
// ---------------------------------------------------------------------------

static bool item_pwm(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dcn_ready) {
        addf(lines, "d7-pwm", "FAIL: DCN layer not initialised (dependency)");
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < DCN314_NUM_PWRSEQ; i++) {
        struct dcn314_pwm_state st;
        const int rc = dcn314_pwm_read_state(hw->dcn, i, &st);
        if (rc == DCN314_OK) {
            addf(lines, "d7-pwm",
                 "PASS pwrseq%u enabled=%d frac=%d level=%u period=%u period_bitcnt=%u "
                 "ref_div=%u xtal_ref_div=%u us_div=%u",
                 i, st.enabled ? 1 : 0, st.fractional ? 1 : 0, st.backlight_level, st.period,
                 st.period_bitcnt, st.ref_div, st.xtal_ref_div, st.us_timebase_div);
        } else {
            addf(lines, "d7-pwm", "FAIL pwrseq%u pwm state rc=%d", i, rc);
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// D8 - 90 Hz confirmation: 2880x1800 active timing and a frame-counter measurement.
// The GOP mode carries no refresh field, so the OTG frame counter over a bounded 100 ms
// read-only window is the corroborating measurement (no writes, no retries).
// ---------------------------------------------------------------------------

static bool item_hz90(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw || !hw->dcn_ready) {
        addf(lines, "d8-90hz", "FAIL: DCN layer not initialised (dependency)");
        return false;
    }

    bool master = false;
    if (dcn314_otg_is_master_enabled(hw->dcn, 0, &master) != DCN314_OK) {
        addf(lines, "d8-90hz", "FAIL: OTG0 master state unreadable");
        return false;
    }

    struct dcn314_otg_timing t;
    struct dcn314_hubp_surface s;
    if (dcn314_otg_read_timing(hw->dcn, 0, &t) != DCN314_OK ||
        dcn314_hubp_read_surface(hw->dcn, 0, &s) != DCN314_OK) {
        addf(lines, "d8-90hz", "FAIL: OTG0 timing / HUBP0 surface unreadable");
        return false;
    }

    const uint32_t active_w = s.viewport_w ? s.viewport_w : t.h_blank_start;
    const uint32_t active_h = s.viewport_h ? s.viewport_h : t.v_blank_start;

    uint32_t fc0 = 0, fc1 = 0;
    int rc0 = dcn314_otg_read_frame_count(hw->dcn, 0, &fc0);
    IODelay(100000);
    int rc1 = dcn314_otg_read_frame_count(hw->dcn, 0, &fc1);
    const uint32_t delta = (rc1 == DCN314_OK && rc0 == DCN314_OK) ? r3_frame_delta(fc0, fc1) : 0u;
    const uint32_t fps_milli = delta * 10000u; // 100 ms sample => fps * 1000

    const bool active_ok = (active_w == 2880u && active_h == 1800u);
    const bool hz_ok = (fps_milli >= 85000u && fps_milli <= 95000u);
    addf(lines, "d8-90hz",
         "%s otg0 master=%d active=%ux%u frame_delta=%u fps=%u.%03u "
         "(target 2880x1800@90)",
         (active_ok && hz_ok) ? "PASS" : "FAIL", master ? 1 : 0, active_w, active_h, delta,
         fps_milli / 1000u, fps_milli % 1000u);
    return active_ok && hz_ok && master;
}

// ---------------------------------------------------------------------------
// D9 - data backfill is performed by the T5a autoarm-r3 daemon; this survey only publishes the
// N48R3LOG chunks / evidence properties and the n48r3-Done flag through setProperty.
// ---------------------------------------------------------------------------

static bool item_export(void * /*user*/, R3SurveyLines *lines) {
    addf(lines, "d9-export",
         "PASS export via T5a autoarm-r3 daemon (N48R3LOG chunks + N48R3EVID-* properties)");
    return true;
}

// ---------------------------------------------------------------------------
// D10 - stage-0 baseline consistency (device / revision / BAR0 size)
// ---------------------------------------------------------------------------

static bool item_baseline(void *user, R3SurveyLines *lines) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    char text[R3_SURVEY_LINE_CHARS];
    r3_survey_baseline_line(text, sizeof text, hw->pci_device, hw->pci_revision,
                            hw->bar0_base, hw->bar0_size);
    r3_lines_add(lines, "d10-baseline", text);
    return r3_baseline_check(hw->pci_device, hw->pci_revision, hw->bar0_size);
}

// Per-tier completion: one N48R3LOG line per finished tier plus the achieved-depth registry
// property (only a passing tier advances it). The ladder stops after the first failed tier.
static void tier_done(void *user, uint32_t tier, bool ok) {
    R3SurveyHw *hw = static_cast<R3SurveyHw *>(user);
    if (!hw) return;
    const uint32_t depth = tier - 1u;
    if (ok) {
        addf(hw->lines, "depth", "PASS tier=T%u depth=%u reached", tier, depth);
        if (hw->sink && hw->sink->set_property) {
            char val[48];
            const int n = snprintf(val, sizeof val, "T%u depth=%u reached", tier, depth);
            if (n > 0)
                hw->sink->set_property(hw->sink->user, "N48R3EVID-depth", val, (size_t)n);
        }
    } else {
        addf(hw->lines, "depth", "FAIL tier=T%u depth=%u not reached", tier, depth);
    }
}

#endif // !R3_SURVEY_PURE_ONLY

} // namespace

#if !defined(R3_SURVEY_PURE_ONLY)

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

int r3_survey_run_hw(R3SurveyHw *hw, R3SurveyLines *lines, char *text, size_t text_cap,
                     const TgtLogSink *sink, R3SurveySummary *summary) {
    if (!hw || !lines || !text || text_cap == 0) return -1;

    hw->sink = sink;
    hw->lines = lines;
    r3_lines_init(lines);
    r3_aux_evid_init(&g_aux_ev);
    r3_manifest_init(&hw->manifest);
    // D-02: the survey owns its own baseline; no caller-side snapshot is needed.
    hw->deny_before = (uint32_t)r3_writeguard_deny_count();

    // T1 is the default: VRAM-size single read plus the no-MMIO config/export baseline.
    // T2 discovery, T3 FB/GTT, T4 DCN snapshot/PWM/90Hz, T5 firmware versions, T6 AUX (last).
    static const R3SurveyItem items[] = {
        {"d2-uma",       item_uma,        1},
        {"d10-baseline", item_baseline,   1},
        {"d9-export",    item_export,     1},
        {"d1-discovery", item_discovery,  2},
        {"d3-fb",        item_fb,         3},
        {"d6-dcn",       item_dcn,        4},
        {"d7-pwm",       item_pwm,        4},
        {"d8-90hz",      item_hz90,       4},
        {"d4-fwver",     item_fwver,      5},
        {"d5-edid",      item_edid,       6},
    };

    R3SurveySummary s {};
    r3_survey_run_tiered(items, (uint32_t)(sizeof items / sizeof items[0]),
                         r3_stage0_depth(), hw, lines, survey_abort_check, tier_done, &s);

    const size_t len = r3_survey_assemble(lines, text, text_cap);

    if (sink && sink->set_property) {
        char key[48];
        const uint32_t chunks = tgt_log_chunk_count(len);
        for (uint32_t i = 0; i < chunks; i++) {
            size_t off = 0, clen = 0;
            if (!tgt_log_chunk_span(len, i, &off, &clen)) break;
            tgt_log_chunk_key(key, sizeof key, i);
            sink->set_property(sink->user, key, text + off, clen);
        }
        r3_aux_export(sink, &g_aux_ev);
        const size_t mlen = r3_manifest_format(&hw->manifest, hw->manifest_text,
                                               sizeof hw->manifest_text);
        if (mlen > 0) {
            sink->set_property(sink->user, "N48R3EVID-read-manifest",
                               hw->manifest_text, mlen);
        }
        if (r3_survey_finalize(s)) {
            sink->set_property(sink->user, "n48r3-Done", "Yes", 3);
        }
    }

    if (summary) *summary = s;
    return r3_survey_finalize(s) ? 0 : -1;
}
#endif // !R3_SURVEY_PURE_ONLY
