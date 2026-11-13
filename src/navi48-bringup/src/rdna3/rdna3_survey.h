// rdna3_survey.h - RDNA3 stage-1 read-only survey: pure model, line assembly and the
// fail-continue/global-terminate orchestration. No IOKit here so the logic is host-testable;
// rdna3_survey.cpp supplies the register reads and the IORegistry setProperty shell.
#ifndef N48_RDNA3_SURVEY_H
#define N48_RDNA3_SURVEY_H

#include <stddef.h>
#include <stdint.h>

#include "common/LogChannel.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#include <cstring>
#endif

// Exported-line contract: "<prefix> <tag>: <text>\n"; the prefix is TGT_LOG_PREFIX
// (common/LogChannel.h), so this header does not define a second copy of it.

// Stage-0 baseline (machine-config.md / stage-0 probe report). Only the stable fields are
// compared: device id, revision and BAR0 size. BAR base addresses are firmware-assigned per
// boot and are recorded but not failed on.
#define R3_BASELINE_DEVICE    0x15BFu
#define R3_BASELINE_REVISION  0xC7u
#define R3_BASELINE_BAR0_SIZE (256ull * 1024ull * 1024ull)

// Line budget derivation (Q-08): the main log carries only bounded evidence. Worst-case d1
// is ip_count (39 on the Phoenix dump) + expect rows (8) + summary + dcn_init + TMR source
// = 50 lines; the fixed D2..D10 rows total about 38 including D3's two copies; the tier-done
// rows add 6 (one per T1..T6); abort/truncated add 1. 112 leaves a small margin over that ~95.
// AUX per-write annotations are exported separately (N48R3EVID-aux-writes), so they never
// consume a main-log line.
#define R3_SURVEY_MAX_LINES   112
// The arena stores the same <=112 lines: ~13 KiB is ~116 bytes per line, around the observed
// average (~110) while the longest D3/D1 lines stay inside R3_SURVEY_LINE_CHARS.
#define R3_SURVEY_ARENA_BYTES 13000
#define R3_SURVEY_TEXT_BYTES  16384
#define R3_SURVEY_LINE_CHARS  512

// ---------------------------------------------------------------------------
// T2 discovery source (requirement v1.4 D1; base Navi48Bringup.cpp:486-503, Linux
// amdgpu_discovery.c:1989-1992): the IP table lives in the 10 KiB TMR block at the top of VRAM,
// 64 KiB below VRAM end. It is copied into RAM through the BAR5 MM_INDEX/MM_DATA window (the TMR
// is far outside the BAR0 aperture) and only then handed to the unmodified IpDiscovery parser.
// ---------------------------------------------------------------------------
#define R3_DISC_TMR_SIZE   (10u << 10)   // kDiscTmrSize
#define R3_DISC_TMR_OFFSET (64u << 10)   // kDiscTmrOffset

static inline uint32_t r3_disc_tmr_dwords(void) { return R3_DISC_TMR_SIZE / 4u; }

// TMR byte address from the T1 VRAM-size read. 0 is not a usable VRAM size (the base returns
// false on vramMB==0) and a size that cannot contain the 64 KiB reserve is refused rather than
// allowed to underflow. Returns false and leaves *out untouched on refusal.
static inline bool r3_tmr_addr(uint32_t vram_mb, uint64_t *out) {
    if (!out || vram_mb == 0) return false;
    const uint64_t bytes = (uint64_t)vram_mb << 20;
    if (bytes <= R3_DISC_TMR_OFFSET) return false;
    *out = bytes - R3_DISC_TMR_OFFSET;
    return true;
}

// Stage-0 depth: T1..T6 = 0..5. Any other value falls back to T1 (the conservative default).
// The entry class owns its own boot-arg clamp; this one guards the ladder against a bad setter.
#define R3_STAGE0_MAX_DEPTH  5u
#define R3_STAGE0_TIER_COUNT 6u

static inline uint32_t r3_stage_depth_clamp(uint32_t depth) {
    return depth <= R3_STAGE0_MAX_DEPTH ? depth : 0u;
}

// Tiers admitted by depth: depth 0 (default) = T1 only, depth 5 = T1..T6.
static inline uint32_t r3_stage_depth_tiers(uint32_t depth) {
    return r3_stage_depth_clamp(depth) + 1u;
}

static inline bool r3_stage_depth_reaches(uint32_t depth, uint32_t tier) {
    return tier >= 1u && tier <= r3_stage_depth_tiers(depth);
}

// ---------------------------------------------------------------------------
// Run-level prerequisite model (v1.7, requirement C0.9). Pure so the host tests can
// exercise the exact strings the driver publishes.
//
// Depth index semantics stay 0..5 = T1..T6. A stage >= 1 consumes the discovery table
// (T2) and the FB/GTT dual read (T3), so its floor is depth 2. The knob stays a breadth
// preference: effective = max(requested, floor), and the plan records the raise.
// ---------------------------------------------------------------------------
enum R3DepthSource { kR3DepthAbsent = 0, kR3DepthExplicit = 1, kR3DepthInvalid = 2 };

struct R3DepthPlan {
    uint32_t    requested;
    bool        present;
    uint32_t    floor;
    uint32_t    effective;
    const char *reason;
};

static inline uint32_t r3_stage_survey_floor(uint32_t target_stage) {
    return target_stage == 0u ? 0u : 2u;
}

static inline R3DepthPlan r3_stage_depth_plan(uint32_t target_stage, uint32_t source,
                                              uint32_t requested) {
    R3DepthPlan p = { requested, source == kR3DepthExplicit, 0u, 0u, "default-floor" };
    p.floor = r3_stage_survey_floor(target_stage);
    p.effective = p.floor;
    if (source == kR3DepthExplicit) {
        if (requested >= p.floor) {
            p.effective = requested;
            p.reason = "explicit";
        } else {
            p.reason = "raised-to-floor";
        }
    } else if (source == kR3DepthInvalid) {
        p.reason = "invalid-as-default";
    }
    return p;
}

// Admission inputs are products, never the knob. fb_dual is an advisory acceptance item:
// it is reported but does not gate preparation (F12: the person decides at trigger time).
struct R3AdmissionInputs {
    bool survey_aborted;
    bool disc_present;
    bool mp0_1;
    bool mp1_1;
    bool gc_0;
    bool sdma_0;
    bool mmhub_1;
    bool dcn_2;
    bool fb_dual_seen;
    bool fb_dual_ok;
};

struct R3AdmissionResult {
    bool        ok;
    char        missing[96];
    const char *fb_dual;   // "ok" | "fail" | "missing"
};

static inline R3AdmissionResult r3_admission_eval(const R3AdmissionInputs &in) {
    R3AdmissionResult r = { false, "", "missing" };
    r.ok = !in.survey_aborted;
    r.missing[0] = '\0';
    const struct { bool ok; const char *name; } items[] = {
        { in.disc_present, "disc" },   { in.mp0_1, "mp0" },   { in.mp1_1, "mp1" },
        { in.gc_0, "gc" },             { in.sdma_0, "sdma" }, { in.mmhub_1, "mmhub" },
        { in.dcn_2, "dcn" },
    };
    size_t used = 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof items / sizeof items[0]); i++) {
        if (items[i].ok) continue;
        r.ok = false;
        const int n = snprintf(r.missing + used, sizeof r.missing - used, "%s%s",
                              used ? "," : "", items[i].name);
        if (n > 0) used += (size_t)n;
        if (used >= sizeof r.missing) break;
    }
    r.missing[used < sizeof r.missing ? used : sizeof r.missing - 1u] = '\0';
    r.fb_dual = !in.fb_dual_seen ? "missing" : (in.fb_dual_ok ? "ok" : "fail");
    return r;
}

static inline size_t r3_admission_fail_line(char *out, size_t cap, const R3AdmissionResult &r,
                                            const R3DepthPlan &p) {
    return (size_t)snprintf(out, cap,
                            "admission: FAIL missing=%s (requested=%u effective=%u floor=%u)",
                            r.missing[0] ? r.missing : "?", p.requested, p.effective, p.floor);
}

static inline size_t r3_admission_pass_line(char *out, size_t cap, uint32_t effective,
                                            const char *fb_dual) {
    return (size_t)snprintf(out, cap, "admission: PASS depth=%u fb-dual=%s", effective,
                            fb_dual ? fb_dual : "missing");
}

static inline size_t r3_admission_summary(char *out, size_t cap, uint32_t effective,
                                          const char *fb_dual) {
    return (size_t)snprintf(out, cap, "admission=PASS depth=%u fb-dual=%s", effective,
                            fb_dual ? fb_dual : "missing");
}

// E5: the stop reason is the first line of the stage-1 dump; the status field carries the
// real gate result instead of a constant "armed".
static inline size_t r3_stop_line(char *out, size_t cap, const char *reason) {
    return (size_t)snprintf(out, cap, "stop: %s",
                            (reason && reason[0]) ? reason : "(no reason emitted)");
}

static inline size_t r3_status_line(char *out, size_t cap, bool gate_ok, const char *summary,
                                    const char *reason, const R3DepthPlan *plan) {
    if (gate_ok) {
        if (plan) {
            if (summary && summary[0])
                return (size_t)snprintf(
                    out, cap, "armed, not fired; %s; depth requested=%u effective=%u floor=%u "
                              "reason=%s\n",
                    summary, plan->requested, plan->effective, plan->floor,
                    plan->reason ? plan->reason : "?");
            return (size_t)snprintf(out, cap,
                                    "armed, not fired; depth requested=%u effective=%u floor=%u "
                                    "reason=%s\n",
                                    plan->requested, plan->effective, plan->floor,
                                    plan->reason ? plan->reason : "?");
        }
        if (summary && summary[0])
            return (size_t)snprintf(out, cap, "armed, not fired; %s\n", summary);
        return (size_t)snprintf(out, cap, "armed, not fired\n");
    }
    if (plan)
        return (size_t)snprintf(out, cap,
                                "stopped: %s, not fired; depth requested=%u effective=%u floor=%u\n",
                                (reason && reason[0]) ? reason : "(no reason emitted)",
                                plan->requested, plan->effective, plan->floor);
    return (size_t)snprintf(out, cap, "stopped: %s, not fired\n",
                            (reason && reason[0]) ? reason : "(no reason emitted)");
}
// E5/C0.9⑥: status line after the power-up substeps have fired (or failed).
static inline size_t r3_status_fired_line(char *out, size_t cap, bool ok, const char *reason) {
    if (ok)
        return (size_t)snprintf(out, cap, "fired, done\n");
    return (size_t)snprintf(out, cap, "stopped: %s\n",
                            (reason && reason[0]) ? reason : "(no reason emitted)");
}

// ---------------------------------------------------------------------------
// Read manifest (tech plan 4.5 read-path audit, v1.4): every MMIO read the survey issues is
// recorded as (BAR5 dword address, count) and exported as text "0x%08x=%u\n" per address, in
// first-seen order. Addresses are the actual bus reads: one per rd()/rdb()/dcn314_read(), and
// one MM_DATA (dword 0x1) per TMR dword, not the VRAM offsets they target.
// ---------------------------------------------------------------------------
#define R3_MANIFEST_MAX   256u
#define R3_MANIFEST_BYTES 8192u

struct R3ReadManifestEntry {
    uint32_t addr;
    uint32_t count;
};

struct R3ReadManifest {
    R3ReadManifestEntry entries[R3_MANIFEST_MAX];
    uint32_t            count;
    bool                truncated;   // more distinct addresses than the table holds
};

static inline void r3_manifest_init(R3ReadManifest *m) {
    if (!m) return;
    m->count = 0;
    m->truncated = false;
}

static inline void r3_manifest_note(R3ReadManifest *m, uint32_t addr) {
    if (!m) return;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->entries[i].addr == addr) {
            m->entries[i].count++;
            return;
        }
    }
    if (m->count >= R3_MANIFEST_MAX) {
        m->truncated = true;
        return;
    }
    m->entries[m->count].addr = addr;
    m->entries[m->count].count = 1;
    m->count++;
}

// Format the manifest. Returns the number of bytes written excluding the NUL (snprintf-style);
// when cap is too small the output is truncated and still NUL-terminated.
static inline size_t r3_manifest_format(const R3ReadManifest *m, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!m) return 0;
    size_t used = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        if (used + 1 >= cap) break;
        const int n = snprintf(out + used, cap - used, "0x%08x=%u\n",
                               m->entries[i].addr, m->entries[i].count);
        if (n <= 0) break;
        if ((size_t)n >= cap - used) { used = cap - 1; break; }
        used += (size_t)n;
    }
    if (m->truncated && used + 1 < cap) {
        const int n = snprintf(out + used, cap - used, "truncated\n");
        if (n > 0 && (size_t)n < cap - used) used += (size_t)n;
    }
    return used;
}

// ---------------------------------------------------------------------------
// Read-side category register (requirement v1.4 D group; baseline
// tmp/tech/阶段1-read-to-clear排查.md sections 1.2-1.5). Confirmed read-to-clear: 0. The two
// field-level unknowns are accepted as "unknown but negligible" (no CLEAR/ACK pairing in the
// pinned headers and no Linux/base write-clear path). Default depth (T1) reads only
// RCC_CONFIG_MEMSIZE plus config-space baseline; no other tier read may run at T1.
//
//   T1  RCC_CONFIG_MEMSIZE 0x0de3                              RO   amdgpu_discovery.c:141
//       PCI config identity/revision/BAR0                      RO   firmware-assigned, no MMIO
//   T2  MM_INDEX/MM_DATA window (dwords 0x0/0x1/0x6)           RW   write index, read data;
//       VRAM TMR 10 KiB at vramMB<<20-64KiB                    RO   VRAM memory, not a register;
//                                                                   base Navi48Bringup.cpp:486-503
//   T3  GC GCMC_VM_FB_LOCATION_BASE/TOP, FB_OFFSET,            RO   gc_11_0_0_offset.h; base
//       SYSTEM_APERTURE_LOW/HIGH; MMHUB MMMC_VM_*                   gmc_v12_0 / mmhub_v3_0_1
//   T4  OTG timing/position/frame-count/sync/control          RO   dcn314_ro.c:23-103
//       HUBP surface/control/viewport                          W1C  status+independent CLEAR bits
//         HUBP_TIMEOUT/UNDERFLOW/SEG_ALLOC_ERR                W1C  cleared by write only
//                                                                   dcn10_hubp.c:107; read safe
//       DIO link, DP link/PHY, DIG FE/BE                       RO   dcn314_ro.c:192-232
//       BL_PWM_CNTL/PERIOD, PANEL_PWRSEQ_REF_DIV               RO   dcn314_ro.c:260-275
//         FRAME_START_EVENT_RECOGNIZED                          ?    unknown, negligible (no CLEAR)
//       DIG_FE_CNTL.DOLBY_VISION_METADATA_PACKET_MISSED        ?    unknown, negligible (no CLEAR)
//   T5  PSP C2PMSG_58/59, RLC_F32_UCODE_VERSION, SDMA0_VERSION RO   psp_v13_0.c:334;
//                                                                   gc_11_0_0_offset.h
//   T6  AUX_SW_STATUS/DATA/ARB_CONTROL/CONTROL                 RO   DONE cleared by W1C write
//                                                                   dce_aux.c:219; read safe
//   dump GCVM status/addr, IH RB cntl/rptr/wptr                 RO   cleared by write, not read
//                                                                   (rdna3_stages.cpp:71-88)
// ---------------------------------------------------------------------------

// Collected exported lines: an arena of text plus (tag, text) pairs. Lives in the driver
// instance (not on the 16 KiB kernel stack).
struct R3SurveyLines {
    char        arena[R3_SURVEY_ARENA_BYTES];
    size_t      arena_used;
    const char *tags[R3_SURVEY_MAX_LINES];
    const char *texts[R3_SURVEY_MAX_LINES];
    uint32_t    count;
    bool        truncated;
};

static inline void r3_lines_init(R3SurveyLines *lines) {
    if (!lines) return;
    lines->arena_used = 0;
    lines->count = 0;
    lines->truncated = false;
    lines->arena[0] = '\0';
}

// Append one (tag, text) line. Returns false when the arena or the line table is full; the
// survey records that as truncation rather than overwriting earlier evidence.
static inline bool r3_lines_add(R3SurveyLines *lines, const char *tag, const char *text) {
    if (!lines || !text) return false;
    const size_t n = strlen(text) + 1;
    if (lines->count >= R3_SURVEY_MAX_LINES || lines->arena_used + n > sizeof(lines->arena)) {
        lines->truncated = true;
        return false;
    }
    char *dst = lines->arena + lines->arena_used;
    memcpy(dst, text, n);
    lines->tags[lines->count] = tag;
    lines->texts[lines->count] = dst;
    lines->count++;
    lines->arena_used += n;
    return true;
}

// Assemble the exported text ("<prefix> <tag>: <text>\n" per line). snprintf-style: returns the
// would-be length excluding the NUL and NUL-terminates when cap > 0.
static inline size_t r3_survey_assemble(const R3SurveyLines *lines, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    if (!lines) return 0;
    for (uint32_t i = 0; i < lines->count; i++) {
        if (used + 1 >= cap) break;
        const size_t n = tgt_log_line(out + used, cap - used, lines->tags[i], lines->texts[i]);
        if (n >= cap - used) { used = cap - 1; out[used] = '\0'; break; }
        used += n;
    }
    if (lines->truncated && used + 1 < cap) {
        const size_t n = tgt_log_line(out + used, cap - used, "truncated",
                                      "survey line arena full; later evidence dropped");
        if (n < cap - used) used += n;
    }
    return used;
}

// One survey item. run() appends its evidence lines (PASS or FAIL) and returns true only when
// the item succeeded. A false return is recorded as FAIL but must NOT stop the remaining items.
struct R3SurveyItem {
    const char *tag;
    bool (*run)(void *user, R3SurveyLines *lines);
    uint8_t tier;   // T1..T6 = 1..6; 0 is treated as T1 (two-field legacy initializers)
};

struct R3SurveySummary {
    uint32_t total;
    uint32_t ok;
    uint32_t failed;
    bool     global_abort;   // write outside the AUX whitelist => survey-code defect
};

typedef bool (*R3SurveyAbortCheck)(void *user);

// Run the items in order. Per-item failure is recorded (FAIL line already emitted by the item)
// and the loop CONTINUES: stage 1 maximises evidence and never retries. If abort_check reports
// true after an item, the survey is a bug (whitelist violation): stop immediately, mark
// global_abort and append the abort line.
static inline uint32_t r3_survey_run_items(const R3SurveyItem *items, uint32_t n, void *user,
                                           R3SurveyLines *lines, R3SurveyAbortCheck abort_check,
                                           R3SurveySummary *out) {
    uint32_t run = 0, ok = 0, failed = 0;
    bool abort_now = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!items[i].run) {
            r3_lines_add(lines, items[i].tag ? items[i].tag : "item", "FAIL: null item runner");
            failed++;
            run++;
            continue;
        }
        const bool good = items[i].run(user, lines);
        run++;
        if (good) ok++;
        else failed++;
        if (abort_check && abort_check(user)) { abort_now = true; break; }
    }

    if (abort_now) {
        r3_lines_add(lines, "abort",
                     "FAIL: write outside AUX whitelist detected - survey terminated (code defect)");
    }
    if (out) {
        out->total = n;
        out->ok = ok;
        out->failed = failed;
        out->global_abort = abort_now;
    }
    return run;
}

// Per-tier completion hook: tier is 1..6, ok is false when any item of that tier failed.
typedef void (*R3SurveyTierDone)(void *user, uint32_t tier, bool ok);

// Tiered ladder (tech plan 4.5): items MUST be in ascending tier order. Every item whose tier is
// admitted by depth runs; failed items keep the fail-continue semantics WITHIN their tier (the
// risk is already taken), but the first failed tier stops all deeper tiers. This is the "档级
// 失败即停" rule layered on the existing runner contract. Skips do not count as failures.
static inline uint32_t r3_survey_run_tiered(const R3SurveyItem *items, uint32_t n, uint32_t depth,
                                            void *user, R3SurveyLines *lines,
                                            R3SurveyAbortCheck abort_check,
                                            R3SurveyTierDone tier_done,
                                            R3SurveySummary *out) {
    const uint32_t max_tier = r3_stage_depth_tiers(depth);
    uint32_t run = 0, ok = 0, failed = 0;
    uint32_t failed_tier = 0;
    uint32_t cur_tier = 0;
    bool cur_ok = true;
    bool abort_now = false;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t tier = items[i].tier ? items[i].tier : 1u;
        if (tier > max_tier) continue;
        if (failed_tier != 0 && tier > failed_tier) continue;

        if (cur_tier == 0) {
            cur_tier = tier;
            cur_ok = true;
        } else if (tier != cur_tier) {
            if (tier_done) tier_done(user, cur_tier, cur_ok);
            cur_tier = tier;
            cur_ok = true;
        }

        if (!items[i].run) {
            r3_lines_add(lines, items[i].tag ? items[i].tag : "item",
                         "FAIL: null item runner");
            run++;
            failed++;
            cur_ok = false;
            if (!failed_tier) failed_tier = tier;
            continue;
        }
        const bool good = items[i].run(user, lines);
        run++;
        if (good) {
            ok++;
        } else {
            failed++;
            cur_ok = false;
            if (!failed_tier) failed_tier = tier;
        }
        if (abort_check && abort_check(user)) {
            if (tier_done && cur_tier != 0) {
                tier_done(user, cur_tier, cur_ok);
                cur_tier = 0;
            }
            abort_now = true;
            break;
        }
    }

    if (cur_tier != 0 && tier_done) tier_done(user, cur_tier, cur_ok);
    if (abort_now) {
        r3_lines_add(lines, "abort",
                     "FAIL: write outside the whitelist detected - survey terminated (code defect)");
    }
    if (out) {
        out->total = n;
        out->ok = ok;
        out->failed = failed;
        out->global_abort = abort_now;
    }
    return run;
}

// Done is published only after a complete, non-terminated survey. An aborted survey is
// incomplete by definition and must not advertise n48r3-Done.
static inline bool r3_survey_finalize(const R3SurveySummary &s) {
    return !s.global_abort;
}

// D10 baseline consistency. Only the boot-invariant identity/size fields are compared.
static inline bool r3_baseline_check(uint16_t device, uint8_t revision, uint64_t bar0_size) {
    return device == R3_BASELINE_DEVICE && revision == R3_BASELINE_REVISION &&
           bar0_size == R3_BASELINE_BAR0_SIZE;
}

// Format the D10 line. BAR0 base is recorded for the trail but does not participate in the
// match (firmware assigns it per boot).
static inline int r3_survey_baseline_line(char *text, size_t cap, uint16_t device, uint8_t revision,
                                          uint64_t bar0_base, uint64_t bar0_size) {
    const bool match = r3_baseline_check(device, revision, bar0_size);
    return snprintf(text, cap,
                    "%s dev=0x%04x rev=0x%02x bar0=0x%llx bar0_size=0x%llx "
                    "(baseline dev=0x%04x rev=0x%02x bar0_size=0x%llx)",
                    match ? "PASS" : "FAIL", device, revision,
                    (unsigned long long)bar0_base, (unsigned long long)bar0_size,
                    R3_BASELINE_DEVICE, R3_BASELINE_REVISION,
                    (unsigned long long)R3_BASELINE_BAR0_SIZE);
}

// ---------------------------------------------------------------------------
// Kernel-facing survey glue (register reads + IORegistry shell). Guarded by KERNEL so the
// pure model above stays includable by host tests without IOKit.
// ---------------------------------------------------------------------------
#if defined(KERNEL)
#include "rdna3_discovery.h"
#include "amdgpu_regs.h"
#include "../../../dcn314/dcn314.h"
#include "common/StageRunner.h"

struct R3SurveyHw {
    amdgpu::DeviceContext *dev;      // BAR5-mapped context (probe/start owns it)
    R3DiscoveryReport     *disc;     // caller-owned base table
    struct dcn314_dev     *dcn;      // caller-owned DCN layer
    bool     dcn_ready;
    bool     d0_confirmed;           // PCI function confirmed in D0 (post-probe)
    // Provenance of the D0 gate (D-15): true only when confirmD0() actually read
    // IOPowerManagement/DevicePowerState. Default false keeps an inference from being
    // reported as a measurement; H1 sets this from IOPowerManagement.
    bool     d0_measured { false };
    uint16_t pci_device;             // PCI identity read by NaviR3Bringup (D10)
    uint8_t  pci_revision;
    uint64_t bar0_base;
    uint64_t bar0_size;
    uint32_t vram_mb { 0 };          // T1 RCC_CONFIG_MEMSIZE (MiB); T2 TMR address input
    bool     fb_dual_seen { false };  // d3-fb item ran (tier 3 reached)
    bool     fb_dual_ok { false };    // d3-fb concluded (GC + MMHUB copies read)
    uint32_t deny_before;            // writeguard deny counter snapshot at survey start
    const TgtLogSink      *sink;     // setProperty shell (N48R3EVID-* evidence)
    R3SurveyLines         *lines;    // evidence sink for per-write AUX annotations
    R3ReadManifest         manifest; // every MMIO read (T2..T6), exported as N48R3EVID-read-manifest
    uint8_t                disc_tmr[R3_DISC_TMR_SIZE] {}; // T2 RAM copy of the VRAM TMR block
    char                   manifest_text[R3_MANIFEST_BYTES] {};
};

// Everything stage 0 needs, owned by the driver instance.
struct R3SurveyStageState {
    R3SurveyHw        hw;
    R3SurveyLines     lines;
    char              text[R3_SURVEY_TEXT_BYTES];
    TgtLogSink        sink;
    R3SurveySummary   summary;
    // v1.7 run-level prerequisite model: the effective depth plan, the admission verdict,
    // and the most recent stage log line (E5 stop reason + truthful status).
    R3DepthPlan       plan {};
    bool              gate_ok { false };
    char              gate_summary[96] {};
    char              last_log[192] {};
    char              stop_reason[192] {};  // set by the failure dump before the latch line
};

const StageTable &r3_stage_table();
void r3_dump_failure_state(StageCtx &ctx);

/* D-01 ladder indices: 0 survey, 1 preflight, 2.. psp..mes. The trigger fires from the
 * first substep and never re-runs the read-only prefix (F12 + the kernel-stack budget). */
static constexpr uint32_t R3_LADDER_FIRST_SUBSTEP = 2u;

// Stage-0 depth selector (contract with the entry class): 0..5 = T1..T6, out of range -> 0.
// Implemented in rdna3_stages.cpp so the ladder owns its own selected depth.
void r3_stage0_set_depth(uint32_t depth);
uint32_t r3_stage0_depth(void);

// Discovery hwId -> amdgpu IPBlock. Only the blocks the survey reads are mapped.
static inline bool r3_ip_block_from_hw(uint16_t hw, amdgpu::IPBlock *out) {
    switch (hw) {
    case IpDiscovery::HwGc:     *out = amdgpu::IPBlock::GC;     return true;
    case IpDiscovery::HwHdp:    *out = amdgpu::IPBlock::HDP;    return true;
    case IpDiscovery::HwSdma0:  *out = amdgpu::IPBlock::SDMA0;  return true;
    case IpDiscovery::HwMp0:    *out = amdgpu::IPBlock::MP0;    return true;
    case IpDiscovery::HwMp1:    *out = amdgpu::IPBlock::MP1;    return true;
    case IpDiscovery::HwNbif:   *out = amdgpu::IPBlock::NBIO;   return true;
    case IpDiscovery::HwOsssys: *out = amdgpu::IPBlock::OSSSYS; return true;
    case IpDiscovery::HwMmhub:  *out = amdgpu::IPBlock::MMHUB;  return true;
    default: return false;
    }
}

bool r3_survey_item_discovery(void *user, R3SurveyLines *lines);
bool r3_survey_item_uma(void *user, R3SurveyLines *lines);
bool r3_survey_item_fb(void *user, R3SurveyLines *lines);
bool r3_survey_item_fwver(void *user, R3SurveyLines *lines);
bool r3_survey_item_edid(void *user, R3SurveyLines *lines);
bool r3_survey_item_dcn(void *user, R3SurveyLines *lines);
bool r3_survey_item_pwm(void *user, R3SurveyLines *lines);
bool r3_survey_item_hz90(void *user, R3SurveyLines *lines);
bool r3_survey_item_export(void *user, R3SurveyLines *lines);
bool r3_survey_item_baseline(void *user, R3SurveyLines *lines);

// Run all D-items, assemble the exported text, publish N48R3LOG chunks + n48r3-Done.
// Returns 0 on a complete survey, -1 when the survey was globally terminated.
int r3_survey_run_hw(R3SurveyHw *hw, R3SurveyLines *lines, char *text, size_t text_cap,
                     const TgtLogSink *sink, R3SurveySummary *summary);
#endif // KERNEL
#endif // N48_RDNA3_SURVEY_H
