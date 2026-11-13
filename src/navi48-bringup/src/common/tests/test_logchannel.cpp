// test_logchannel.cpp — LogChannel pure core: N48R3LOG line shape and the registry
// chunk boundaries (count/span/key). No IORegistry calls: phase 1 only fixes TgtLogSink.
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 \
//       -I src/navi48-bringup/src/common \
//       src/navi48-bringup/src/common/tests/test_logchannel.cpp \
//       src/navi48-bringup/src/common/LogChannel.cpp -o /tmp/t1c_logchannel
//   /tmp/t1c_logchannel
//
// (paths relative to the fork repository root)
#include <cstdio>
#include <cstring>
#include <string>

#include "LogChannel.h"
#include "TargetConfig.h"

#if defined(TARGET_RDNA3)
static const char *kPrefix = "N48R3LOG";
#else
static const char *kPrefix = "N48LOG";
#endif

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got %#llx want %#llx\n", what,
                    (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
    }
}

static void expect_s(const char *what, const std::string &got, const std::string &want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
    } else {
        std::printf("ok    %-78s \"%s\"\n", what, got.c_str());
    }
}

static std::string line_of(const char *tag, const char *text)
{
    char buf[256] = { 0 };
    const size_t n = tgt_log_line(buf, sizeof buf, tag, text);
    if (n >= sizeof buf) return "<truncated>";
    return buf;
}

static std::string key_of(uint32_t index)
{
    char buf[64] = { 0 };
    const size_t n = tgt_log_chunk_key(buf, sizeof buf, index);
    if (n >= sizeof buf) return "<truncated>";
    return buf;
}

int main()
{
    expect_s("TGT_LOG_PREFIX is the documented prefix", TGT_LOG_PREFIX, kPrefix);
    expect_u("chunk size is 1 KiB", kTgtLogChunkBytes, 1024);

    // ---- line assembly -------------------------------------------------------------
    expect_s("tag and kv text are assembled under the prefix",
             line_of("stage0", "status=ok"), std::string(kPrefix) + " stage0: status=ok\n");
    expect_s("a null tag becomes '-'", line_of(nullptr, "x=1"),
             std::string(kPrefix) + " -: x=1\n");
    expect_s("a null text becomes empty", line_of("t", nullptr),
             std::string(kPrefix) + " t: \n");
    {
        char small[8];
        std::memset(small, 'Z', sizeof small);
        const size_t need = tgt_log_line(small, sizeof small, "stage0", "status=ok");
        expect_u("snprintf-style length is the untruncated size", need,
                 std::string(kPrefix).size() + std::string(" stage0: status=ok\n").size());
        expect_u("truncated output is still NUL-terminated", small[sizeof small - 1], 0);
    }

    // ---- chunk count: exact multiples, one over, one under -------------------------
    expect_u("0 bytes -> 0 chunks", tgt_log_chunk_count(0), 0);
    expect_u("1 byte -> 1 chunk", tgt_log_chunk_count(1), 1);
    expect_u("chunk-1 bytes -> 1 chunk", tgt_log_chunk_count(kTgtLogChunkBytes - 1), 1);
    expect_u("chunk bytes -> 1 chunk", tgt_log_chunk_count(kTgtLogChunkBytes), 1);
    expect_u("chunk+1 bytes -> 2 chunks", tgt_log_chunk_count(kTgtLogChunkBytes + 1), 2);
    expect_u("2*chunk bytes -> 2 chunks", tgt_log_chunk_count(2 * kTgtLogChunkBytes), 2);
    expect_u("2*chunk+1 bytes -> 3 chunks", tgt_log_chunk_count(2 * kTgtLogChunkBytes + 1), 3);

    // ---- chunk span ---------------------------------------------------------------
    {
        const size_t len = kTgtLogChunkBytes + 1;
        size_t off = 9999, n = 9999;
        expect_u("span 0 of chunk+1 exists", tgt_log_chunk_span(len, 0, &off, &n), 1);
        expect_u("  offset", off, 0);
        expect_u("  length (full chunk)", n, kTgtLogChunkBytes);
        expect_u("span 1 of chunk+1 exists", tgt_log_chunk_span(len, 1, &off, &n), 1);
        expect_u("  offset", off, kTgtLogChunkBytes);
        expect_u("  length (remainder)", n, 1);
        expect_u("span 2 of chunk+1 does not exist", tgt_log_chunk_span(len, 2, &off, &n), 0);
        expect_u("span of an empty log does not exist", tgt_log_chunk_span(0, 0, &off, &n), 0);
    }
    {
        const size_t len = 2 * kTgtLogChunkBytes;
        size_t off = 9999, n = 9999;
        expect_u("last span of an exact multiple", tgt_log_chunk_span(len, 1, &off, &n), 1);
        expect_u("  offset", off, kTgtLogChunkBytes);
        expect_u("  length (not over-read)", n, kTgtLogChunkBytes);
        expect_u("one past the multiple is refused", tgt_log_chunk_span(len, 2, &off, &n), 0);
    }

    // ---- property key -------------------------------------------------------------
    expect_s("chunk key is prefix + zero-padded index", key_of(7),
             std::string(kPrefix) + "-0007");
    expect_s("chunk key index 0", key_of(0), std::string(kPrefix) + "-0000");
    {
        char tiny[8];
        std::memset(tiny, 'Z', sizeof tiny);
        const size_t need = tgt_log_chunk_key(tiny, sizeof tiny, 7);
        expect_u("a short buffer reports the needed length",
                 need, std::string(kPrefix).size() + std::string("-0007").size());
        expect_u("  and stays NUL-terminated", tiny[sizeof tiny - 1], 0);
    }

    // ---- planted-break guards: off-by-one chunk arithmetic must be caught ----------
    {
        // floor(len/chunk) loses the final partial chunk.
        auto bad_floor = [](size_t len) -> uint32_t {
            return (uint32_t)(len / kTgtLogChunkBytes);
        };
        expect_u("guard: floor division would be caught at chunk+1",
                 (bad_floor(kTgtLogChunkBytes + 1) != tgt_log_chunk_count(kTgtLogChunkBytes + 1)) ? 1u : 0u, 1u);
        // len/chunk + 1 invents an empty chunk for exact multiples and zero.
        auto bad_plus_one = [](size_t len) -> uint32_t {
            return (uint32_t)(len / kTgtLogChunkBytes + 1);
        };
        expect_u("guard: always-plus-one would be caught at 0",
                 (bad_plus_one(0) != tgt_log_chunk_count(0)) ? 1u : 0u, 1u);
        expect_u("guard: always-plus-one would be caught at an exact multiple",
                 (bad_plus_one(kTgtLogChunkBytes) != tgt_log_chunk_count(kTgtLogChunkBytes)) ? 1u : 0u, 1u);
        // A span stride of chunk+1 skips a byte.
        const size_t len = 2 * kTgtLogChunkBytes + 5;
        size_t off = 0, n = 0;
        (void)tgt_log_chunk_span(len, 1, &off, &n);
        expect_u("guard: a chunk+1 stride would be caught", (off != kTgtLogChunkBytes + 1) ? 1u : 0u, 1u);
    }

    std::printf("\n%s: %d checks, %d failures\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
