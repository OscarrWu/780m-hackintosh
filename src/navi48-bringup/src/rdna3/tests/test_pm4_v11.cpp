/*
 * test_pm4_v11.cpp - host tests for the PM4 packet builders + submit tail.
 *
 * Part 1: packet golden cases against the nullarc originals (R10 §8/§9):
 *   WRITE_DATA      0xC0033700,0x00100500,lo,hi,data   (mesboot.c:227 area)
 *   INDIRECT_BUFFER 0xC0023F00,lo,hi,VALID|len|vmid<<24 (PhxBringup.cpp:796-806)
 *   RELEASE_MEM     0xC0064900,...,lo,hi,seq,0,0 (:807-826 + Linux emit_fence :6094)
 *   ACQUIRE_MEM     0xC0065800,0,0xFFFFFFFF,0x00FFFFFF,0,0,0x0A,0xC000
 *   SET_UCONFIG_REG 0xC0017900,0x40,0xDEADBEEF
 * plus the canonical VA trap (upper-half IB with a bare 16-bit high word).
 *
 * Part 2: submit tail against a Dev stub (drain -> hdpFlush -> WB+16 ->
 * readback -> hdpFlush -> fence -> doorbell index*4, abort on any hdpFlush
 * failure).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "amd/pm4_v11.h"
#include "rdna3/rdna3_substep.h"

using namespace amdgpu;

namespace {

static uint32_t g_checks = 0;
static uint32_t g_fail = 0;

#define CHECK(cond, ...)                                                                  \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            g_fail++;                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            printf(__VA_ARGS__);                                                          \
            printf("\n");                                                                 \
        }                                                                                 \
    } while (0)

/* ---- Part 1: packet golden cases --------------------------------------- */

static void test_write_data(void)
{
    uint32_t out[8];
    memset(out, 0xEE, sizeof(out));
    const uint32_t n = pm4_write_data(out, 0x00007FFF00000100ull, 0x1234u);
    CHECK(n == 5, "len %u", n);
    CHECK(out[0] == 0xC0033700u, "word0 0x%08x", out[0]);
    CHECK(out[1] == 0x00100500u, "word1 0x%08x (DST_SEL(5)|WR_CONFIRM)",
          out[1]);
    CHECK(out[2] == 0x00000100u, "addr lo 0x%08x", out[2]);
    CHECK(out[3] == 0x00007FFFu, "addr hi 0x%08x", out[3]);
    CHECK(out[4] == 0x1234u, "data");

    /* Misaligned destination is rejected. */
    CHECK(pm4_write_data(out, 0x00007FFF00000102ull, 1u) == 0, "misaligned");
    CHECK(pm4_write_data(nullptr, 0x1000ull, 1u) == 0, "null out");
}

static void test_indirect_buffer(void)
{
    uint32_t out[8];
    /* Compute form: VALID | length | vmid<<24 (Linux gfx_v11_0.c:6036). */
    memset(out, 0xEE, sizeof(out));
    uint32_t n = pm4_indirect_buffer(out, 0x00007FFF00000200ull, 0x100u, 3u, true);
    CHECK(n == 4, "len %u", n);
    CHECK(out[0] == 0xC0023F00u, "word0 0x%08x", out[0]);
    CHECK(out[1] == 0x00000200u, "base lo 0x%08x", out[1]);
    CHECK(out[2] == 0x00007FFFu, "base hi 0x%08x", out[2]);
    CHECK(out[3] == (0x100u | (3u << 24) | (1u << 23)), "control 0x%08x",
          out[3]);

    /* Gfx form: no VALID (Linux gfx_v11_0.c:6006). */
    n = pm4_indirect_buffer(out, 0x00007FFF00000200ull, 0x100u, 3u, false);
    CHECK(n == 4 && out[3] == (0x100u | (3u << 24)), "gfx control 0x%08x",
          out[3]);

    /* Canonical trap: upper-half VA gets the high bits set
     * (nullarc PhxBringup.cpp:801; the card's 0xffff8001 case). */
    n = pm4_indirect_buffer(out, 0x0000800100000000ull, 0x10u, 0u, true);
    CHECK(n == 4, "len");
    CHECK(out[1] == 0x00000000u, "canonical lo 0x%08x", out[1]);
    CHECK(out[2] == 0xFFFF8001u, "canonical hi 0x%08x", out[2]);

    /* Already-canonical input passes through unchanged. */
    CHECK(pm4_va_canonical(0xFFFF800112345678ull) == 0xFFFF800112345678ull,
          "canonical passthrough");
    /* Lower-half input unchanged. */
    CHECK(pm4_va_canonical(0x000012345678ull) == 0x000012345678ull,
          "lower passthrough");

    /* Invalid inputs. */
    CHECK(pm4_indirect_buffer(out, 0x00007FFF00000202ull, 0x10u, 0u, true) == 0,
          "misaligned ib");
    CHECK(pm4_indirect_buffer(out, 0x1000ull, 0u, 0u, true) == 0, "zero len");
    CHECK(pm4_indirect_buffer(out, 0x1000ull, 0x100000u + 1u, 0u, true) == 0,
          "len overflow");
    CHECK(pm4_indirect_buffer(out, 0x1000ull, 0x10u, 16u, true) == 0,
          "vmid overflow");
    CHECK(pm4_indirect_buffer(nullptr, 0x1000ull, 0x10u, 0u, true) == 0,
          "null out");
}

static void test_release_mem(void)
{
    uint32_t out[8];
    memset(out, 0xEE, sizeof(out));
    /* fence_mc = MC aperture form, 8-aligned (64-bit fence write). */
    const uint64_t fence_mc = 0x8000000000ull + 0x1230ull;
    const uint32_t n = pm4_release_mem(out, fence_mc, 0x42u);
    CHECK(n == 8, "len %u", n);
    CHECK(out[0] == 0xC0064900u, "word0 0x%08x", out[0]);
    CHECK(out[1] == 0x06603514u, "word1 0x%08x (nullarc verbatim)", out[1]);
    CHECK(out[2] == 0x40000000u, "word2 0x%08x (DATA_SEL 2, no interrupt)",
          out[2]);
    CHECK(out[3] == 0x00001230u, "fence lo 0x%08x", out[3]);
    CHECK(out[4] == 0x00000080u, "fence hi 0x%08x", out[4]);
    CHECK(out[5] == 0x42u, "seq lo");
    CHECK(out[6] == 0u, "seq hi");
    CHECK(out[7] == 0u, "count=6 body pad (Linux emit_fence :6094)");

    /* 4-aligned but not 8-aligned is rejected (64-bit fence write). */
    CHECK(pm4_release_mem(out, 0x8000000000ull + 0x1234ull, 1u) == 0,
          "misaligned fence");
    CHECK(pm4_release_mem(nullptr, 0x8000000000ull, 1u) == 0, "null out");
}

static void test_acquire_mem(void)
{
    uint32_t out[8];
    memset(out, 0xEE, sizeof(out));
    const uint32_t n = pm4_acquire_mem(out);
    CHECK(n == 8, "len %u", n);
    CHECK(out[0] == 0xC0065800u, "word0 0x%08x", out[0]);
    CHECK(out[1] == 0u, "coher_cntl");
    CHECK(out[2] == 0xFFFFFFFFu, "coher_size");
    CHECK(out[3] == 0x00FFFFFFu, "coher_size_hi");
    CHECK(out[4] == 0u && out[5] == 0u, "coher_base");
    CHECK(out[6] == 0x0Au, "poll_interval");
    CHECK(out[7] == 0xC000u, "gcr_cntl 0x%08x (GL2_INV|GL2_WB)", out[7]);
    CHECK(pm4_acquire_mem(nullptr) == 0, "null out");
}

static void test_set_uconfig_reg(void)
{
    uint32_t out[8];
    memset(out, 0xEE, sizeof(out));
    const uint32_t n = pm4_set_uconfig_reg(out, 0x40u, 0xDEADBEEFu);
    CHECK(n == 3, "len %u", n);
    CHECK(out[0] == 0xC0017900u, "word0 0x%08x", out[0]);
    CHECK(out[1] == 0x40u, "reg off");
    CHECK(out[2] == 0xDEADBEEFu, "value");
    CHECK(pm4_set_uconfig_reg(nullptr, 0x40u, 0u) == 0, "null out");
}

/* ---- Part 2: submit tail ------------------------------------------------ */

struct Dev {
    std::vector<uint8_t> bar0;
    std::vector<char> kick_order;
    struct Db {
        uint32_t bar;
        uint64_t byte_off;
        uint64_t value;
        uint32_t dwords;
    };
    std::vector<Db> doorbells;
    int hdp_calls;
    bool hdp_ok;
    bool hdp_fail_second; /* fail only from the second flush on */
    bool wb_corrupt;      /* flip a bit in the WB write */

    Dev() : bar0(0x100000u, 0xa5u), hdp_calls(0), hdp_ok(true),
            hdp_fail_second(false), wb_corrupt(false) {}
};

static bool io_bus(void *ctx, uint32_t bar, uint64_t off, uint64_t *value,
                   bool write, uint32_t dwords)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value)
        return false;
    if (bar == 2u) {
        d.doorbells.push_back({bar, off, *value, dwords});
        return true;
    }
    if (bar != R3_IO_BAR_VRAM)
        return false;
    if ((off & 3ull) != 0ull ||
        off + static_cast<uint64_t>(dwords) * 4ull > static_cast<uint64_t>(d.bar0.size()))
        return false;
    if (dwords == 2u && (off & 7ull) != 0ull)
        return false;
    if (write) {
        uint64_t v = *value;
        if (d.wb_corrupt && dwords == 2u && off == 0x1000ull + 16ull)
            v ^= 0x100000000ull; /* corrupt the high dword */
        memcpy(&d.bar0[static_cast<size_t>(off)], &v,
               static_cast<size_t>(dwords) * 4u);
    } else {
        memcpy(value, &d.bar0[static_cast<size_t>(off)],
               static_cast<size_t>(dwords) * 4u);
    }
    return true;
}

static void io_fence(void *ctx)
{
    static_cast<Dev *>(ctx)->kick_order.push_back('f');
}

static bool io_hdp(void *ctx)
{
    Dev &d = *static_cast<Dev *>(ctx);
    d.hdp_calls++;
    if (d.hdp_fail_second && d.hdp_calls >= 2)
        return false;
    return d.hdp_ok;
}

static void test_submit_tail_happy(void)
{
    Dev d;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    io.fence = io_fence;
    io.hdp_flush = io_hdp;

    const uint64_t ring_off = 0x800ull;
    const uint64_t wb_off = 0x1000ull;
    const bool ok = pm4_submit_tail(&io, ring_off, wb_off, 6u, 0x1234ull);
    CHECK(ok, "submit ok");
    CHECK(d.doorbells.size() == 1, "doorbells %zu", d.doorbells.size());
    if (d.doorbells.size() == 1) {
        CHECK(d.doorbells[0].bar == 2u, "bar");
        CHECK(d.doorbells[0].byte_off == 6ull * 4ull, "byte off %llu",
              static_cast<unsigned long long>(d.doorbells[0].byte_off));
        CHECK(d.doorbells[0].value == 0x1234ull, "value");
        CHECK(d.doorbells[0].dwords == 2u, "dwords");
    }
    /* WB page at +16 carries the 64-bit wptr. */
    uint64_t wb = 0;
    memcpy(&wb, &d.bar0[static_cast<size_t>(wb_off + 16)], 8u);
    CHECK(wb == 0x1234ull, "wb 0x%llx", static_cast<unsigned long long>(wb));
    CHECK(d.kick_order.size() == 2, "fences %zu", d.kick_order.size());
    CHECK(d.hdp_calls == 2, "hdp calls %d", d.hdp_calls);
}

static void test_submit_tail_hdp_fail_first(void)
{
    Dev d;
    d.hdp_ok = false;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    io.fence = io_fence;
    io.hdp_flush = io_hdp;

    const bool ok = pm4_submit_tail(&io, 0x800ull, 0x1000ull, 6u, 0x1234ull);
    CHECK(!ok, "aborted at first hdpFlush");
    CHECK(d.doorbells.empty(), "no doorbell");
    CHECK(d.hdp_calls == 1, "hdp calls %d", d.hdp_calls);
}

static void test_submit_tail_hdp_fail_second(void)
{
    Dev d;
    d.hdp_fail_second = true;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    io.fence = io_fence;
    io.hdp_flush = io_hdp;

    const bool ok = pm4_submit_tail(&io, 0x800ull, 0x1000ull, 6u, 0x1234ull);
    CHECK(!ok, "aborted at second hdpFlush");
    CHECK(d.doorbells.empty(), "no doorbell after failed flush");
    CHECK(d.hdp_calls == 2, "hdp calls %d", d.hdp_calls);
}

static void test_submit_tail_wb_mismatch(void)
{
    Dev d;
    d.wb_corrupt = true;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    io.fence = io_fence;
    io.hdp_flush = io_hdp;

    const bool ok = pm4_submit_tail(&io, 0x800ull, 0x1000ull, 6u, 0x1234ull);
    CHECK(!ok, "aborted at WB readback");
    CHECK(d.doorbells.empty(), "no doorbell");
}

static void test_submit_tail_drain_fail(void)
{
    Dev d;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    io.fence = io_fence;
    io.hdp_flush = io_hdp;

    /* ring_off beyond the BAR0 window: the drain read fails. */
    const bool ok = pm4_submit_tail(&io, 0x200000ull, 0x1000ull, 6u, 0x1234ull);
    CHECK(!ok, "aborted at drain read");
    CHECK(d.doorbells.empty(), "no doorbell");
    CHECK(d.hdp_calls == 0, "no flush before drain");
}

static void test_submit_tail_missing_io(void)
{
    Dev d;
    R3Io io;
    memset(&io, 0, sizeof(io));
    io.ctx = &d;
    io.bus = io_bus;
    /* io->fence and io->hdp_flush missing: reject up front. */
    const bool ok = pm4_submit_tail(&io, 0x800ull, 0x1000ull, 6u, 0x1234ull);
    CHECK(!ok, "missing io callbacks rejected");
    CHECK(d.doorbells.empty(), "no doorbell");
}

} /* namespace */

int main(void)
{
    test_write_data();
    test_indirect_buffer();
    test_release_mem();
    test_acquire_mem();
    test_set_uconfig_reg();
    test_submit_tail_happy();
    test_submit_tail_hdp_fail_first();
    test_submit_tail_hdp_fail_second();
    test_submit_tail_wb_mismatch();
    test_submit_tail_drain_fail();
    test_submit_tail_missing_io();

    printf("test_pm4_v11: %u checks, %u failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
