/*
 * test_dcn314_allow.c - host unit tests for the dcn314 deny-by-default display write allowlist.
 *
 * The allowlist is the only thing standing between a wrong absolute address and SMN/indirect space, so
 * these tests pin the check order (window -> hard deny -> deny block -> allow ranges -> deny), the
 * placeholder-segment arming rule, and the counter/sample bookkeeping. The planted-defect runner
 * (tests/run_planted_breaks.py) proves this file actually fails when the table or the order is broken.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn314.h"
#include "dcn314_allow.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MMIO_DWORDS 0x40000u
static const uint32_t SEG[5] = {
    0x00000000u, 0x00001000u, 0x00010000u, 0x00020000u, 0x00030000u
};

static void test_init(void)
{
    struct dcn314_allow_state st;
    uint32_t bad[5];

    CHECK(dcn314_allow_init(&st, SEG, MMIO_DWORDS) == DCN314_ALLOW_OK, "init ok");
    CHECK(st.magic == DCN314_ALLOW_STATE_MAGIC && st.armed == 1u, "armed");
    CHECK(st.mmio_dwords == MMIO_DWORDS, "mmio");

    CHECK(dcn314_allow_init(NULL, SEG, MMIO_DWORDS) == DCN314_ALLOW_UNARMED, "null state");

    memcpy(bad, SEG, sizeof(bad));
    bad[2] ^= 0x40u;
    CHECK(dcn314_allow_init(&st, bad, MMIO_DWORDS) == DCN314_ALLOW_UNARMED, "wrong seg refused");
    CHECK(st.armed == 0u && st.magic == 0u, "wrong seg zeroed");

    CHECK(dcn314_allow_init(&st, SEG, 0u) == DCN314_ALLOW_UNARMED, "zero window");
    CHECK(dcn314_allow_init(&st, SEG, 0x33820u) == DCN314_ALLOW_UNARMED, "window not above highest");
    CHECK(dcn314_allow_init(&st, SEG, 0x33821u) == DCN314_ALLOW_OK, "window just above highest");
}

static void test_classify(void)
{
    const char *name = (const char *)1;
    const char *why = (const char *)1;

    /* AUX0_AUX_SW_DATA = 0x1f56 in BASE_IDX 2 => 0x10000 + 0x1f56 = 0x11f56. */
    CHECK(dcn314_allow_classify(0x11f56u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_OK, "aux allowed");
    CHECK(name != NULL && name != (const char *)1, "aux range name");
    CHECK(why == NULL, "aux why null");

    CHECK(dcn314_allow_classify(0x0u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_INDIRECT, "low page indirect");
    CHECK(dcn314_allow_classify(0xffu, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_INDIRECT, "low page top");
    CHECK(dcn314_allow_classify(0x100u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_RANGE, "above hard deny");

    CHECK(dcn314_allow_classify(0x34000u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_BLOCK, "deny block lo");
    CHECK(dcn314_allow_classify(0x34fffu, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_BLOCK, "deny block hi");
    CHECK(dcn314_allow_classify(0x34ffeu, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_BLOCK, "deny block in");

    CHECK(dcn314_allow_classify(MMIO_DWORDS, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_WINDOW, "at window");
    CHECK(dcn314_allow_classify(0x40001u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_WINDOW, "past window");
    CHECK(dcn314_allow_classify(0x11f56u, 0u, &name, &why) == DCN314_ALLOW_WINDOW, "zero window");

    CHECK(dcn314_allow_classify(0x30000u, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_RANGE, "gap denied");
    CHECK(dcn314_allow_classify(0x3ffffu, MMIO_DWORDS, &name, &why) == DCN314_ALLOW_RANGE, "tail denied");

    /* NULL out-params must not be dereferenced. */
    CHECK(dcn314_allow_classify(0x11f56u, MMIO_DWORDS, NULL, NULL) == DCN314_ALLOW_OK, "null outs");
}

static void test_boundaries(void)
{
    uint32_t n = dcn314_allow_range_count();
    uint32_t i, total = 0u;

    CHECK(n > 0u, "range count");
    for (i = 0; i < n; i++) {
        uint32_t lo = 0, hi = 0, base = 0xffu;
        const char *nm = NULL;
        CHECK(dcn314_allow_range_at(i, &lo, &hi, &base, &nm) == DCN314_OK, "range_at %u", i);
        CHECK(lo <= hi, "range %u lo<=hi", i);
        CHECK(nm != NULL, "range %u name", i);
        CHECK(base < DCN314_NUM_SEGS, "range %u base %u", i, base);
        total += hi - lo + 1u;
        CHECK(dcn314_allow_classify(lo, MMIO_DWORDS, NULL, NULL) == DCN314_ALLOW_OK, "range %u lo", i);
        CHECK(dcn314_allow_classify(hi, MMIO_DWORDS, NULL, NULL) == DCN314_ALLOW_OK, "range %u hi", i);
        if (lo > 0u)
            CHECK(dcn314_allow_classify(lo - 1u, MMIO_DWORDS, NULL, NULL) != DCN314_ALLOW_OK,
                  "range %u lo-1", i);
        CHECK(dcn314_allow_classify(hi + 1u, MMIO_DWORDS, NULL, NULL) != DCN314_ALLOW_OK,
              "range %u hi+1", i);
    }
    CHECK(total == dcn314_allow_total_dwords(), "total %u vs %u", total, dcn314_allow_total_dwords());
    CHECK(dcn314_allow_bar5_dwords() == MMIO_DWORDS, "bar5");
    CHECK(dcn314_allow_range_at(n, NULL, NULL, NULL, NULL) == DCN314_E_ARG, "range_at oob");
}

static void test_write_gate(void)
{
    struct dcn314_allow_state st;
    uint32_t i;

    CHECK(dcn314_allow_init(&st, SEG, MMIO_DWORDS) == DCN314_ALLOW_OK, "init");
    CHECK(dcn314_allow_write(&st, 0x11f56u, 0x12345678u, "aux") == true, "allow aux");
    CHECK(st.writes_seen == 1u && st.allowed == 1u && st.refused == 0u, "counters allow");
    CHECK(st.n_allow_samples == 1u && st.allow[0].abs_dword == 0x11f56u &&
          st.allow[0].value == 0x12345678u && st.allow[0].seq == 1u &&
          strcmp(st.allow[0].tag, "aux") == 0, "allow sample");

    CHECK(dcn314_allow_write(&st, 0x0u, 1u, "bad") == false, "deny low page");
    CHECK(st.writes_seen == 2u && st.refused == 1u, "counters deny");
    CHECK(st.last_refused_abs == 0u && st.last_refused_reason == DCN314_ALLOW_INDIRECT, "last refused");
    CHECK(st.by_reason[DCN314_ALLOW_INDIRECT] == 1u, "reason counter");

    CHECK(dcn314_allow_init(&st, SEG, MMIO_DWORDS) == DCN314_ALLOW_OK, "reinit for samples");
    /* Fill both sample arrays; further events only bump the dropped counters. */
    for (i = 0; i < DCN314_ALLOW_SAMPLES + 3u; i++)
        (void)dcn314_allow_write(&st, 0x11f56u, i, "fill-a");
    CHECK(st.n_allow_samples == DCN314_ALLOW_SAMPLES, "allow samples capped");
    for (i = 0; i < DCN314_ALLOW_SAMPLES + 3u; i++)
        (void)dcn314_allow_write(&st, 0x0u, i, "fill-r");
    CHECK(st.n_refuse_samples == DCN314_ALLOW_SAMPLES, "refuse samples capped");
    CHECK(st.refuse_dropped == 3u, "refuse dropped %u", st.refuse_dropped);

    CHECK(dcn314_allow_init(&st, SEG, MMIO_DWORDS) == DCN314_ALLOW_OK, "reinit for tag");
    /* A long tag is truncated, never overruns. */
    CHECK(dcn314_allow_write(&st, 0x11f56u, 0u,
                             "a-tag-that-is-much-longer-than-twenty") == true, "long tag");
    CHECK(strlen(st.allow[0].tag) < DCN314_ALLOW_TAG_LEN, "tag truncated");

    /* Unarmed state refuses everything. */
    {
        struct dcn314_allow_state u;
        memset(&u, 0, sizeof(u));
        CHECK(dcn314_allow_write(&u, 0x11f56u, 0u, "u") == false, "unarmed deny");
        CHECK(u.refused == 1u && u.by_reason[DCN314_ALLOW_UNARMED] == 1u, "unarmed counter");
    }
    CHECK(dcn314_allow_write(NULL, 0x11f56u, 0u, "n") == false, "null state deny");

    CHECK(dcn314_allow_write_cb(&st, 0x11f56u, 0u) == true, "cb allow");
    CHECK(dcn314_allow_write_cb(&st, 0x0u, 0u) == false, "cb deny");
}

static void test_note_read(void)
{
    struct dcn314_allow_state st;
    CHECK(dcn314_allow_init(&st, SEG, MMIO_DWORDS) == DCN314_ALLOW_OK, "init");
    dcn314_allow_note_read(&st);
    dcn314_allow_note_read(&st);
    CHECK(st.reads_seen == 2u, "reads seen");
    dcn314_allow_note_read(NULL);
}

static void test_mutation(void)
{
    /* Mutating the caller's window must re-classify an otherwise-allowed address as WINDOW. */
    CHECK(dcn314_allow_classify(0x11f56u, 0x10000u, NULL, NULL) == DCN314_ALLOW_WINDOW, "shrunk window");
    /* The value is irrelevant to classification; only the address is gated. */
    CHECK(dcn314_allow_classify(0x11f56u, MMIO_DWORDS, NULL, NULL) == DCN314_ALLOW_OK, "same addr");

    CHECK(strcmp(dcn314_allow_reason_name(DCN314_ALLOW_OK), "OK") == 0, "reason name OK");
    CHECK(strcmp(dcn314_allow_reason_name(DCN314_ALLOW_INDIRECT), "INDIRECT") == 0, "reason name INDIRECT");
    CHECK(strcmp(dcn314_allow_reason_name(DCN314_ALLOW_BLOCK), "BLOCK") == 0, "reason name BLOCK");
    CHECK(strcmp(dcn314_allow_reason_name(DCN314_ALLOW_RANGE), "RANGE") == 0, "reason name RANGE");
}

int main(void)
{
    test_init();
    test_classify();
    test_boundaries();
    test_write_gate();
    test_note_read();
    test_mutation();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
