// test_survey.cpp - host tests for the pure RDNA3 survey model (rdna3_survey.h).
//
// Build with -DTARGET_RDNA3=1 and link src/common/LogChannel.cpp:
//   src/navi48-bringup/src/rdna3/tests/run_survey_tests.sh
#include "rdna3_survey.h"

#include <cstdio>
#include <cstring>

// Compile the kernel-side translation unit in pure mode: no IOKit, only the
// evidence-splitting / line-assembly helpers shared with the real survey.
#define R3_SURVEY_PURE_ONLY 1
#include "../rdna3_survey.cpp"

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static uint32_t count_lines_containing(const R3SurveyLines *lines, const char *needle) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < lines->count; i++) {
        if (std::strstr(lines->texts[i], needle)) n++;
    }
    return n;
}

struct FakeSink {
    char        key[64];
    const char *bytes;
    size_t      len;
    int         calls;
};

static bool fake_set_property(void *user, const char *key, const char *bytes, size_t len) {
    FakeSink *s = static_cast<FakeSink *>(user);
    s->calls++;
    std::snprintf(s->key, sizeof s->key, "%s", key);
    s->bytes = bytes;
    s->len = len;
    return true;
}

struct Ctx {
    int  ran[8];
    int  n;
    int  abort_at;   // abort once n >= abort_at (0 = never)
};

static bool item1(void *u, R3SurveyLines *l) {
    Ctx *c = static_cast<Ctx *>(u);
    c->ran[c->n++] = 1;
    return r3_lines_add(l, "t1", "item one ok");
}

static bool item2_fail(void *u, R3SurveyLines *l) {
    Ctx *c = static_cast<Ctx *>(u);
    c->ran[c->n++] = 2;
    r3_lines_add(l, "t2", "FAIL: planted single-item failure");
    return false;
}

static bool item3(void *u, R3SurveyLines *l) {
    Ctx *c = static_cast<Ctx *>(u);
    c->ran[c->n++] = 3;
    return r3_lines_add(l, "t3", "item three ok");
}

static bool abort_check(void *u) {
    Ctx *c = static_cast<Ctx *>(u);
    return c->abort_at != 0 && c->n >= c->abort_at;
}

static void test_assembly_and_chunking() {
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    CHECK(r3_lines_add(&lines, "d1-discovery", "ip_count=8"));
    CHECK(r3_lines_add(&lines, "d10-baseline", "PASS dev=0x15bf"));

    char text[512];
    const size_t len = r3_survey_assemble(&lines, text, sizeof text);
    CHECK(len > 0);
    CHECK(std::strstr(text, "N48R3LOG d1-discovery: ip_count=8\n") != nullptr);
    CHECK(std::strstr(text, "N48R3LOG d10-baseline: PASS dev=0x15bf\n") != nullptr);
    CHECK(text[len] == '\0');

    // Chunks partition the assembled text with the documented key format.
    const uint32_t chunks = tgt_log_chunk_count(len);
    CHECK(chunks == (uint32_t)((len + kTgtLogChunkBytes - 1) / kTgtLogChunkBytes));
    size_t covered = 0;
    for (uint32_t i = 0; i < chunks; i++) {
        size_t off = 0, clen = 0;
        CHECK(tgt_log_chunk_span(len, i, &off, &clen));
        CHECK(off == covered);
        CHECK(clen > 0 && clen <= kTgtLogChunkBytes);
        covered += clen;
    }
    CHECK(covered == len);

    char key[48];
    tgt_log_chunk_key(key, sizeof key, 0);
    CHECK(std::strcmp(key, "N48R3LOG-0000") == 0);
}

static void test_fail_continue() {
    Ctx c {};
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    const R3SurveyItem items[] = {
        {"t1", item1, 1}, {"t2", item2_fail, 1}, {"t3", item3, 1},
    };
    R3SurveySummary s {};
    const uint32_t run = r3_survey_run_items(items, 3, &c, &lines, abort_check, &s);

    CHECK(run == 3);
    CHECK(c.n == 3);
    CHECK(c.ran[0] == 1 && c.ran[1] == 2 && c.ran[2] == 3);  // later items still run
    CHECK(s.total == 3 && s.ok == 2 && s.failed == 1);
    CHECK(!s.global_abort);
    CHECK(r3_survey_finalize(s));
}

static void test_global_abort() {
    Ctx c {};
    c.abort_at = 1;   // abort right after the first item
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    const R3SurveyItem items[] = {
        {"t1", item1, 1}, {"t3", item3, 1},
    };
    R3SurveySummary s {};
    const uint32_t run = r3_survey_run_items(items, 2, &c, &lines, abort_check, &s);

    CHECK(run == 1);
    CHECK(c.n == 1);
    CHECK(s.global_abort);
    CHECK(!r3_survey_finalize(s));   // Done must NOT be advertised on abort

    char text[256];
    r3_survey_assemble(&lines, text, sizeof text);
    CHECK(std::strstr(text, "N48R3LOG abort:") != nullptr);
}

struct DenyCtx {
    uint32_t base;
    uint32_t deny;
    int      n;
    int      ran;
};

static bool deny_item(void *u, R3SurveyLines * /*l*/) {
    DenyCtx *c = static_cast<DenyCtx *>(u);
    c->ran++;
    c->n++;
    return true;
}

static bool deny_abort(void *u) {
    DenyCtx *c = static_cast<DenyCtx *>(u);
    return r3_deny_aborts(c->base, c->deny);
}

static void test_abort_deny_delta() {
    // D-02: the abort predicate is relative to the survey entry snapshot, not the global absolute
    // counter. A deny left behind by an earlier run must not abort a survey with no new deny.
    uint32_t base = 5;
    CHECK(!r3_deny_aborts(base, 5));
    CHECK(base == 5);
    CHECK(r3_deny_aborts(base, 6));

    // r3_writeguard_install() zeroes the counter at D1. That is a re-arm, not a rollback: the
    // baseline follows it and a post-install deny is still caught.
    base = 7;
    CHECK(!r3_deny_aborts(base, 0));
    CHECK(base == 0);
    CHECK(r3_deny_aborts(base, 1));

    R3SurveyItem items[] = {{"t1", deny_item, 1}, {"t2", deny_item, 1}};
    R3SurveyLines lines {};
    r3_lines_init(&lines);

    DenyCtx clean {3, 3, 0, 0};   // pre-existing deny stock, no new deny inside the survey
    R3SurveySummary s {};
    CHECK(r3_survey_run_items(items, 2, &clean, &lines, deny_abort, &s) == 2);
    CHECK(clean.ran == 2);
    CHECK(!s.global_abort);
    CHECK(r3_survey_finalize(s));

    DenyCtx fresh {3, 4, 0, 0};   // entry stock 3, first check sees a deny recorded inside
    r3_lines_init(&lines);
    s = {};
    CHECK(r3_survey_run_items(items, 2, &fresh, &lines, deny_abort, &s) == 1);
    CHECK(fresh.ran == 1);
    CHECK(s.global_abort);
    CHECK(!r3_survey_finalize(s));
}

static void test_baseline() {
    const uint64_t bar0_size = 256ull * 1024ull * 1024ull;
    CHECK(r3_baseline_check(R3_BASELINE_DEVICE, R3_BASELINE_REVISION, bar0_size));
    CHECK(!r3_baseline_check(R3_BASELINE_DEVICE, (uint8_t)(R3_BASELINE_REVISION - 1), bar0_size));
    CHECK(!r3_baseline_check((uint16_t)(R3_BASELINE_DEVICE - 1), R3_BASELINE_REVISION, bar0_size));
    CHECK(!r3_baseline_check(R3_BASELINE_DEVICE, R3_BASELINE_REVISION, bar0_size / 2));

    char ok[256], bad[256];
    r3_survey_baseline_line(ok, sizeof ok, R3_BASELINE_DEVICE, R3_BASELINE_REVISION,
                            0x8d000000ull, bar0_size);
    r3_survey_baseline_line(bad, sizeof bad, R3_BASELINE_DEVICE, 0x00, 0x8d000000ull, bar0_size);
    CHECK(std::strncmp(ok, "PASS ", 5) == 0);
    CHECK(std::strncmp(bad, "FAIL ", 5) == 0);
    CHECK(std::strstr(ok, "bar0=0x8d000000") != nullptr);   // base recorded, not compared
}

static void test_truncation() {
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    char text[64];
    for (int i = 0; i < R3_SURVEY_MAX_LINES + 8; i++) {
        std::snprintf(text, sizeof text, "line %d", i);
        (void)r3_lines_add(&lines, "t", text);
    }
    CHECK(lines.truncated);
    char out[13000];
    r3_survey_assemble(&lines, out, sizeof out);
    CHECK(std::strstr(out, "N48R3LOG truncated:") != nullptr);
}

static void test_aux_evidence_split() {
    R3AuxEvidence ev {};
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    r3_aux_evid_init(&ev);

    // A 256-byte EDID read is ~80-110 request-register writes; 200 models the
    // worst case plus headroom. None of them may consume a main-log line.
    for (uint32_t i = 0; i < 200; i++) {
        r3_aux_annotate(&lines, &ev, true, "DP_AUX1", "edid", 0x1000u + i, 0x80000000u | i);
    }
    CHECK(lines.count == 0);
    CHECK(ev.writes == 200);
    CHECK(ev.denied == 0);
    CHECK(!ev.truncated);

    uint32_t records = 0;
    for (const char *p = ev.buf; (p = std::strstr(p, "aux[")) != nullptr; p += 4) records++;
    CHECK(records == 200);
    CHECK(std::strstr(ev.buf, "aux[000] txn tag=DP_AUX1 ctx=edid addr=0x00001000") != nullptr);
    CHECK(std::strstr(ev.buf, "aux[199] txn tag=DP_AUX1") != nullptr);

    // The main log gets exactly one summary line, and it points at the payload.
    r3_aux_summary(&lines, &ev);
    CHECK(lines.count == 1);
    CHECK(count_lines_containing(&lines, "evid=N48R3EVID-aux-writes") == 1);
    CHECK(count_lines_containing(&lines, "writes=200 denied=0") == 1);

    // The payload is published under the contracted evidence key, unmodified.
    FakeSink fs {};
    TgtLogSink ts { fake_set_property, &fs };
    CHECK(r3_aux_export(&ts, &ev));
    CHECK(fs.calls == 1);
    CHECK(std::strcmp(fs.key, "N48R3EVID-aux-writes") == 0);
    CHECK(fs.bytes == ev.buf);
    CHECK(fs.len == ev.used);
    CHECK(r3_aux_export(&ts, nullptr) == false);
    CHECK(r3_aux_export(nullptr, &ev) == false);

    // Denies are a code-defect signal: one main-log line each, never evidence.
    R3AuxEvidence ev2 {};
    R3SurveyLines lines2 {};
    r3_lines_init(&lines2);
    r3_aux_evid_init(&ev2);
    r3_aux_annotate(&lines2, &ev2, false, "DP_AUX1", "edid", 0xdeadbeefu, 1u);
    r3_aux_annotate(&lines2, &ev2, false, "DP_AUX2", "dpcd", 0xdeadbee0u, 2u);
    CHECK(ev2.writes == 0);
    CHECK(ev2.denied == 2);
    CHECK(lines2.count == 2);
    CHECK(count_lines_containing(&lines2, "DENY tag=DP_AUX1") == 1);
    CHECK(count_lines_containing(&lines2, "DENY tag=DP_AUX2") == 1);
    CHECK(std::strstr(ev2.buf, "aux[") == nullptr);
}

static void test_aux_evidence_truncation() {
    R3AuxEvidence ev {};
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    r3_aux_evid_init(&ev);
    for (uint32_t i = 0; i < 1000; i++) {
        r3_aux_annotate(&lines, &ev, true, "DP_AUX1", "edid", i, i);
    }
    CHECK(ev.truncated);
    CHECK(ev.writes == 1000);   // every accepted write is counted, even past the payload edge
    uint32_t records = 0;
    for (const char *p = ev.buf; (p = std::strstr(p, "aux[")) != nullptr; p += 4) records++;
    CHECK(records > 0 && records < 1000);
    CHECK(std::strstr(ev.buf, "truncated; later writes dropped") != nullptr);
    CHECK(lines.count == 0);
}

static void test_aux_evidence_boundary() {
    R3AuxEvidence ev {};
    r3_aux_evid_init(&ev);
    // Largest fill an accepted record can leave behind; the next record must write
    // the marker and its NUL without stepping past buf.
    // The accept decision must reserve the marker AND its terminating NUL.
    CHECK(!r3_aux_evid_fits(sizeof ev.buf - kAuxEvidTruncLen - 1, 1));
    CHECK(r3_aux_evid_fits(sizeof ev.buf - kAuxEvidTruncLen - 2, 1));
    ev.used = sizeof ev.buf - kAuxEvidTruncLen - 1;
    r3_aux_evid_record(&ev, 0, "DP_AUX1", "edid", 0, 0);
    CHECK(ev.truncated);
    CHECK(ev.used == sizeof ev.buf - 1);
    CHECK(std::memcmp(ev.buf + ev.used - kAuxEvidTruncLen, kAuxEvidTrunc,
                      kAuxEvidTruncLen) == 0);
}

static void test_mm_write_audit() {
    R3AuxEvidence ev {};
    r3_aux_evid_init(&ev);
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    // 2560 TMR dwords = 5120 MM window writes; none may touch the AUX payload.
    for (uint32_t i = 0; i < 2560; i++) {
        r3_mm_write_audit(&lines, &ev, true, 0x6u, 0u);
        r3_mm_write_audit(&lines, &ev, true, 0x0u, 0x80000000u | i);
    }
    r3_mm_write_audit(&lines, &ev, false, 0x0u, 0xdeadbeefu);
    CHECK(ev.used == 0 && !ev.truncated);
    CHECK(ev.mm_writes == 5120 && ev.mm_denied == 1);
    CHECK(ev.writes == 0 && ev.denied == 0);
    CHECK(lines.count == 1);
    CHECK(std::strstr(lines.texts[0], "DENY tag=mm-indirect") != nullptr);

    // Real AUX writes keep per-write records; the MM summary is appended under the same key.
    for (uint32_t i = 0; i < 200; i++) {
        r3_aux_annotate(&lines, &ev, true, "DP_AUX1", "edid", 0x1000u + i, 0x80000000u | i);
    }
    FakeSink fs {};
    TgtLogSink ts { fake_set_property, &fs };
    CHECK(r3_aux_export(&ts, &ev));
    CHECK(fs.calls == 1);
    CHECK(std::strcmp(fs.key, "N48R3EVID-aux-writes") == 0);
    CHECK(std::strstr(fs.bytes, "aux[000] txn tag=DP_AUX1") != nullptr);
    CHECK(std::strstr(fs.bytes, "tag=mm-indirect writes=5120 denied=1") != nullptr);
    CHECK(fs.len == std::strlen(fs.bytes));

    // MM-only surveys (depth 2..5 without T6) still publish their audit summary.
    R3AuxEvidence mm {};
    r3_aux_evid_init(&mm);
    R3SurveyLines lines2 {};
    r3_lines_init(&lines2);
    r3_mm_write_audit(&lines2, &mm, true, 0x6u, 0u);
    FakeSink fs2 {};
    TgtLogSink ts2 { fake_set_property, &fs2 };
    CHECK(r3_aux_export(&ts2, &mm));
    CHECK(fs2.calls == 1);
    CHECK(std::strstr(fs2.bytes, "tag=mm-indirect writes=1 denied=0") != nullptr);
}

struct FakeFb {
    uint32_t gc[5];
    uint32_t mm[5];
    uint32_t seen_gc[5];
    uint32_t seen_mm[5];
    int      n_gc;
    int      n_mm;
};

static uint32_t fake_fb_read(void *user, R3FbCopy copy, uint32_t reg) {
    FakeFb *f = static_cast<FakeFb *>(user);
    static const uint32_t gc_off[5] = {0x1678u, 0x1679u, 0x15a7u, 0x167du, 0x167eu};
    static const uint32_t mm_off[5] = {0x08ecu, 0x08edu, 0x08d7u, 0u, 0u};
    const bool mm = copy == R3_FB_COPY_MMHUB;
    const uint32_t *off = mm ? mm_off : gc_off;
    const uint32_t *val = mm ? f->mm : f->gc;
    uint32_t *seen = mm ? f->seen_mm : f->seen_gc;
    int &n = mm ? f->n_mm : f->n_gc;
    for (int i = 0; i < 5; i++) {
        if (off[i] == reg) {
            if (n < 5) seen[n] = reg;
            n++;
            return val[i];
        }
    }
    return 0xFFFFFFFFu;
}

static void test_fb_dual_copy() {
    // Fake discovery: both copies resolved with distinct words. The survey must read the GC
    // copy at its five offsets and the MMHUB copy at 0x8ec/0x8ed/0x8d7, then emit one line
    // per copy (D-01). A regression to a single read leaves n_mm at 0 and fails here.
    FakeFb f {};
    f.gc[0] = 0x00123456u;
    f.gc[1] = 0x00123457u;
    f.gc[2] = 0x00000010u;
    f.gc[3] = 0x00200000u;   // mesboot.c writes this for MC 0x8000000000
    f.gc[4] = 0x00205fffu;
    f.mm[0] = 0x00120000u;
    f.mm[1] = 0x00120001u;
    f.mm[2] = 0x00000020u;

    R3SurveyLines lines {};
    r3_lines_init(&lines);
    uint64_t mc_base = 0;
    CHECK(r3_fb_item(&lines, fake_fb_read, &f, true, true, &mc_base));
    CHECK(f.n_gc == 5);
    CHECK(f.n_mm == 3);
    CHECK(f.seen_gc[0] == 0x1678u && f.seen_gc[1] == 0x1679u && f.seen_gc[2] == 0x15a7u &&
          f.seen_gc[3] == 0x167du && f.seen_gc[4] == 0x167eu);
    CHECK(f.seen_mm[0] == 0x08ecu && f.seen_mm[1] == 0x08edu && f.seen_mm[2] == 0x08d7u);
    CHECK(lines.count == 2);
    CHECK(count_lines_containing(&lines, "copy=GC(gfxhub)") == 1);
    CHECK(count_lines_containing(&lines, "copy=MMHUB") == 1);
    CHECK(count_lines_containing(&lines,
                                 "authoritative copy to be confirmed by dual-read on target") == 2);
    CHECK(mc_base == ((uint64_t)0x00123456u << 24));   // first usable copy (GC) wins
    CHECK(count_lines_containing(&lines, "fb_mc_base=0x120000000000") == 1);
    CHECK(count_lines_containing(&lines, "system_aperture=not-read") == 1);

    // An unresolved copy is reported, not silently dropped, and is never read.
    f.n_gc = f.n_mm = 0;
    r3_lines_init(&lines);
    CHECK(r3_fb_item(&lines, fake_fb_read, &f, true, false, &mc_base));
    CHECK(f.n_gc == 5);
    CHECK(f.n_mm == 0);
    CHECK(lines.count == 2);
    CHECK(count_lines_containing(&lines, "FAIL copy=MMHUB unavailable") == 1);

    f.n_gc = f.n_mm = 0;
    r3_lines_init(&lines);
    CHECK(!r3_fb_item(&lines, fake_fb_read, &f, false, false, &mc_base));
    CHECK(f.n_gc == 0);
    CHECK(f.n_mm == 0);
    CHECK(count_lines_containing(&lines, "FAIL copy=GC(gfxhub) unavailable") == 1);
    CHECK(count_lines_containing(&lines, "FAIL copy=MMHUB unavailable") == 1);
}

static void test_fb_aperture_conversion() {
    R3FbCopyWords w {};
    w.resolved = true;
    w.has_aperture = true;
    w.fb_base = 0x00123456u;
    w.fb_top  = 0x00123457u;
    w.fb_off  = 0x00000010u;
    w.ap_lo = 0x00200000u;   // >>18 == 0x8000000000 (the MC base mesboot programs)
    w.ap_hi = 0x00205fffu;

    char out[R3_SURVEY_LINE_CHARS];
    uint64_t mc_base = 0;
    r3_fb_line(out, sizeof out, R3_FB_COPY_GC, w, &mc_base);
    CHECK(mc_base == ((uint64_t)0x00123456u << 24));
    CHECK(std::strstr(out, "fb_mc_base=0x123456000000") != nullptr);
    CHECK(std::strstr(out, "fb_offset=0x10000000") != nullptr);
    CHECK(std::strstr(out, "gtt_lo=0x8000000000") != nullptr);
    CHECK(std::strstr(out, "gtt_hi=0x817ffc0000") != nullptr);
    CHECK(std::strstr(out, "gtt_size=0x17ffc0000") != nullptr);
    CHECK(std::strstr(out, "raw_ap=0x00200000/0x00205fff") != nullptr);
    CHECK(std::strstr(out, "aperture >>18 per gfxhub_v3_0.c:161-164/"
                           "mmhub_v3_0.c:178-181 (archived)") != nullptr);

    // LOGICAL_ADDR_MASK is 0x3FFFFFFF (gc_11_0_0_sh_mask.h:11351-11355): the upper two bits
    // are not part of the page number, and the conversion shifts left by 18.
    w.ap_lo = 0xFFFFFFFFu;
    w.ap_hi = 0u;
    r3_fb_line(out, sizeof out, R3_FB_COPY_GC, w, &mc_base);
    CHECK(std::strstr(out, "gtt_lo=0xfffffffc0000") != nullptr);
    CHECK(std::strstr(out, "gtt_hi=0x0") != nullptr);
    CHECK(std::strstr(out, "gtt_size=0x0") != nullptr);
}

static void test_frame_delta_wrap() {
    // 32-bit free-running counter: the unsigned difference handles the wrap naturally
    // instead of producing a huge value that would fake a 90 Hz PASS (D-13).
    CHECK(r3_frame_delta(100u, 700u) == 600u);
    CHECK(r3_frame_delta(0xFFFFFFF0u, 0x00000010u) == 0x00000020u);
    CHECK(r3_frame_delta(0xFFFFFFFFu, 0u) == 1u);
}

static void test_uma_d0_source() {
    char out[R3_SURVEY_LINE_CHARS];
    r3_uma_line(out, sizeof out, 0x00001000u, true);
    CHECK(std::strstr(out, "carveout_mib=4096") != nullptr);
    CHECK(std::strstr(out, "bytes=4294967296") != nullptr);
    CHECK(std::strstr(out, "raw=0x00001000") != nullptr);
    CHECK(std::strstr(out, "d0=confirmed") != nullptr);

    r3_uma_line(out, sizeof out, 0x00001000u, false);
    CHECK(std::strstr(out, "d0=assumed") != nullptr);
    CHECK(std::strstr(out, "d0=confirmed") == nullptr);
}

static void test_tmr_addr(void) {
    uint64_t a = 0;
    // VRAM size 0 is not usable -> refuse, do not underflow.
    CHECK(!r3_tmr_addr(0, &a));
    CHECK(a == 0);
    CHECK(!r3_tmr_addr(0, nullptr));
    // 1 MiB is the smallest size that can hold the 64 KiB reserve.
    CHECK(r3_tmr_addr(1, &a));
    CHECK(a == (1ull << 20) - R3_DISC_TMR_OFFSET);
    CHECK(a == 0xF0000ull);
    // The base's 16 GiB part: vramMB << 20 - 64 KiB.
    CHECK(r3_tmr_addr(16384, &a));
    CHECK(a == ((uint64_t)16384 << 20) - R3_DISC_TMR_OFFSET);
    // 32-bit maximum MiB: still a valid 64-bit byte address.
    CHECK(r3_tmr_addr(0xFFFFFFFFu, &a));
    CHECK(a == (((uint64_t)0xFFFFFFFFu << 20) - R3_DISC_TMR_OFFSET));
    // The TMR copy is exactly 10 KiB = 2560 dwords (base kDiscTmrSize/kDiscTmrOffset).
    CHECK(R3_DISC_TMR_SIZE == 10240u);
    CHECK(R3_DISC_TMR_OFFSET == 65536u);
    CHECK(r3_disc_tmr_dwords() == 2560u);
}

struct TierCtx {
    uint32_t ran[8];
    uint32_t n;
    uint32_t done[8];
    bool     done_ok[8];
    uint32_t nd;
    uint32_t fail_tier;
};

#define TIER_FN(N)                                                              \
    static bool tier_fn##N(void *u, R3SurveyLines * /*l*/) {                    \
        TierCtx *c = static_cast<TierCtx *>(u);                                 \
        c->ran[c->n++] = (N);                                                   \
        return c->fail_tier != (N);                                             \
    }
TIER_FN(1) TIER_FN(2) TIER_FN(3) TIER_FN(4) TIER_FN(5) TIER_FN(6)
#undef TIER_FN

static void tier_cb(void *u, uint32_t tier, bool ok) {
    TierCtx *c = static_cast<TierCtx *>(u);
    c->done[c->nd] = tier;
    c->done_ok[c->nd] = ok;
    c->nd++;
}

static void test_depth_gate_matrix(void) {
    const R3SurveyItem items[] = {
        {"t1", tier_fn1, 1}, {"t2", tier_fn2, 2}, {"t3", tier_fn3, 3},
        {"t4", tier_fn4, 4}, {"t5", tier_fn5, 5}, {"t6", tier_fn6, 6},
    };
    // depth N runs T1..T(N+1); out-of-range depth falls back to T1.
    for (uint32_t depth = 0; depth <= R3_STAGE0_MAX_DEPTH; depth++) {
        TierCtx c {};
        R3SurveyLines lines {};
        r3_lines_init(&lines);
        R3SurveySummary s {};
        r3_survey_run_tiered(items, 6, depth, &c, &lines, nullptr, tier_cb, &s);
        CHECK(c.n == depth + 1);
        for (uint32_t i = 0; i < c.n; i++) CHECK(c.ran[i] == i + 1);
        CHECK(c.nd == depth + 1);
        for (uint32_t i = 0; i < c.nd; i++) {
            CHECK(c.done[i] == i + 1);
            CHECK(c.done_ok[i]);
        }
        CHECK(s.ok == depth + 1 && s.failed == 0);
        CHECK(s.total == 6);
    }
    const uint32_t bad[] = {6u, 7u, 42u, 0xFFFFFFFFu};
    for (uint32_t depth : bad) {
        TierCtx c {};
        R3SurveyLines lines {};
        r3_lines_init(&lines);
        R3SurveySummary s {};
        r3_survey_run_tiered(items, 6, depth, &c, &lines, nullptr, tier_cb, &s);
        CHECK(c.n == 1 && c.ran[0] == 1);
    }
}

static void test_tier_failure_stops_ladder(void) {
    // T2 has two items; the first fails, the second still runs (same-tier evidence), then the
    // ladder stops: no T3+ item may run.
    const R3SurveyItem items[] = {
        {"t1", tier_fn1, 1}, {"t2a", tier_fn2, 2}, {"t2b", tier_fn6, 2},
        {"t3", tier_fn3, 3}, {"t4", tier_fn4, 4},
    };
    TierCtx c {};
    c.fail_tier = 2;
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    R3SurveySummary s {};
    r3_survey_run_tiered(items, 5, 5, &c, &lines, nullptr, tier_cb, &s);
    CHECK(c.n == 3);
    CHECK(c.ran[0] == 1 && c.ran[1] == 2 && (c.ran[2] == 6 || c.ran[2] == 2));
    CHECK(c.nd == 2);
    CHECK(c.done[0] == 1 && c.done_ok[0]);
    CHECK(c.done[1] == 2 && !c.done_ok[1]);
    CHECK(s.ok == 2 && s.failed == 1);
}

static bool abort_always(void * /*u*/) { return true; }

static void test_tier_abort_single_callback(void) {
    const R3SurveyItem items[] = {
        {"t1", tier_fn1, 1}, {"t2", tier_fn2, 2}, {"t3", tier_fn3, 3},
    };
    TierCtx c {};
    R3SurveyLines lines {};
    r3_lines_init(&lines);
    R3SurveySummary s {};
    r3_survey_run_tiered(items, 3, 5, &c, &lines, abort_always, tier_cb, &s);
    CHECK(c.n == 1 && c.nd == 1 && s.global_abort);
    CHECK(c.done[0] == 1 && c.done_ok[0]);
}

static void test_manifest_format(void) {
    R3ReadManifest m;
    r3_manifest_init(&m);
    r3_manifest_note(&m, 0x0de3u);
    r3_manifest_note(&m, 0x1u);
    r3_manifest_note(&m, 0x0de3u);
    r3_manifest_note(&m, 0x1u);
    r3_manifest_note(&m, 0x1u);
    CHECK(m.count == 2 && !m.truncated);
    CHECK(m.entries[0].addr == 0x0de3u && m.entries[0].count == 2);
    CHECK(m.entries[1].addr == 0x1u && m.entries[1].count == 3);

    char buf[128];
    const size_t n = r3_manifest_format(&m, buf, sizeof buf);
    CHECK(std::strcmp(buf, "0x00000de3=2\n0x00000001=3\n") == 0);
    CHECK(n == std::strlen(buf));

    // A short cap truncates cleanly and stays NUL-terminated.
    char small[24];
    const size_t sn = r3_manifest_format(&m, small, sizeof small);
    CHECK(sn == sizeof small - 1 && small[sn] == '\0');

    // Table overflow is flagged, and the first R3_MANIFEST_MAX addresses are kept.
    R3ReadManifest big;
    r3_manifest_init(&big);
    for (uint32_t i = 0; i < R3_MANIFEST_MAX; i++) r3_manifest_note(&big, i);
    CHECK(big.count == R3_MANIFEST_MAX && !big.truncated);
    r3_manifest_note(&big, R3_MANIFEST_MAX);
    CHECK(big.count == R3_MANIFEST_MAX && big.truncated);
}

int main() {
    test_assembly_and_chunking();
    test_fail_continue();
    test_global_abort();
    test_abort_deny_delta();
    test_aux_evidence_split();
    test_aux_evidence_truncation();
    test_aux_evidence_boundary();
    test_mm_write_audit();
    test_fb_dual_copy();
    test_fb_aperture_conversion();
    test_frame_delta_wrap();
    test_uma_d0_source();
    test_baseline();
    test_truncation();
    test_tmr_addr();
    test_depth_gate_matrix();
    test_tier_failure_stops_ladder();
    test_tier_abort_single_callback();
    test_manifest_format();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all survey host checks passed\n");
    return 0;
}
