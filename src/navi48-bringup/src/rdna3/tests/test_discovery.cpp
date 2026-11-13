/*
 * test_discovery.cpp - host tests for the fail-closed IP-discovery check.
 *
 * A syntactically valid discovery binary is built in memory (base parser format: binary
 * header -> IPDS table -> die -> variable IP records) so the real parser and the real
 * expectation check run end to end. Cases pin:
 *   - every expected IP present, exact versions            -> OK + base table
 *   - a MINOR difference                                   -> EXPECT, recorded, no base table
 *   - a REVISION difference                                -> OK, recorded
 *   - a HIGHER major (catches a ">=" regression)            -> EXPECT, no base table
 *   - a LOWER major                                        -> EXPECT, no base table
 *   - a missing expected IP                                -> EXPECT, no base table
 *   - a corrupted checksum / random bytes                  -> PARSE, no base table
 *   - the ATOM fallback stub and the fail-closed chain      -> FALLBACK / error
 */
#include <stdio.h>
#include <string.h>
#include "rdna3_ip_expect.h"

#include "rdna3_discovery.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FixIp {
    uint16_t hw;
    uint8_t inst, major, minor, rev;
    uint32_t base;
};

static void put16(uint8_t* b, size_t o, uint16_t v)
{
    b[o] = (uint8_t)v;
    b[o + 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* b, size_t o, uint32_t v)
{
    b[o] = (uint8_t)v;
    b[o + 1] = (uint8_t)(v >> 8);
    b[o + 2] = (uint8_t)(v >> 16);
    b[o + 3] = (uint8_t)(v >> 24);
}

/* Build a valid discovery binary (version 1 header, one die, one base per IP) into `buf`.
 * `corrupt` adds one to the stored checksum. Returns the total buffer size used. */
static size_t build(uint8_t* buf, size_t cap, const struct FixIp* ips, size_t n, int corrupt)
{
    const size_t ipd = 0x20;
    const size_t die = 0x100;
    const size_t end = die + 4 + n * 12;
    uint16_t binSize = 0x400;

    memset(buf, 0, cap);
    while ((size_t)binSize < end)
        binSize = (uint16_t)(binSize * 2);

    put32(buf, 0, 0x28211407u);   /* binary signature */
    put16(buf, 4, 1);             /* version_major 1 (table list at +12) */
    put16(buf, 6, 0);
    put16(buf, 8, 0);             /* checksum, filled last */
    put16(buf, 10, binSize);
    put16(buf, 12, (uint16_t)ipd);
    put16(buf, 14, 0);
    put16(buf, 16, 0x80);         /* ipdSize >= 80 */
    put16(buf, 18, 0);

    put32(buf, ipd + 0, 0x53445049u); /* "IPDS" */
    put16(buf, ipd + 4, 3);
    put16(buf, ipd + 6, 0x80);
    put32(buf, ipd + 8, 0);
    put16(buf, ipd + 12, 1);          /* num_dies */
    put16(buf, ipd + 14, 0);          /* die_id */
    put16(buf, ipd + 16, (uint16_t)die);

    put16(buf, die + 0, 0);
    put16(buf, die + 2, (uint16_t)n);
    size_t p = die + 4;
    for (size_t i = 0; i < n; i++) {
        put16(buf, p + 0, ips[i].hw);
        buf[p + 2] = ips[i].inst;
        buf[p + 3] = 1;               /* num_base */
        buf[p + 4] = ips[i].major;
        buf[p + 5] = ips[i].minor;
        buf[p + 6] = ips[i].rev;
        buf[p + 7] = 0;
        put32(buf, p + 8, ips[i].base);
        p += 12;
    }

    uint16_t sum = 0;
    for (size_t i = 10; i < (size_t)binSize; i++)
        sum = (uint16_t)(sum + buf[i]);
    put16(buf, 8, corrupt ? (uint16_t)(sum + 1u) : sum);

    return (size_t)binSize;
}

/* Phoenix values as discovered on a real 780M (GC 11.0.1 / DCN 3.1.4 / OSSSYS 6.0.1). */
static const struct FixIp kGood[] = {
    { 11,  0, 11, 0, 1, 0x1000 },   /* GC   11.0.1 */
    { 271, 0,  3, 1, 4, 0x2000 },   /* DCN   3.1.4 */
    { 255, 0, 13, 0, 4, 0x3000 },   /* PSP  13.0.4 */
    { 1,   0, 13, 0, 4, 0x4000 },   /* SMU  13.0.4 */
    { 42,  0,  6, 0, 1, 0x5000 },   /* SDMA  6.0.1 */
    { 12,  0,  4, 0, 2, 0x6000 },   /* VCN   4.0.2 */
    { 108, 0,  7, 7, 0, 0x7000 },   /* NBIO  7.7.0 */
    { 40,  0,  6, 0, 1, 0x8000 },   /* IH    6.0.1 */
};
#define GOOD_COUNT (sizeof(kGood) / sizeof(kGood[0]))

static const R3ExpectVerdict* verdict(const R3DiscoveryReport& rep, const char* name)
{
    for (uint32_t i = 0; i < rep.expect_count && i < R3_DISC_MAX_EXPECT; i++)
        if (rep.expect[i].name && strcmp(rep.expect[i].name, name) == 0)
            return &rep.expect[i];
    return nullptr;
}

static int ips_all_zero(const R3DiscoveryReport& rep)
{
    const uint8_t* p = (const uint8_t*)rep.ips;
    for (size_t i = 0; i < sizeof(rep.ips); i++)
        if (p[i] != 0)
            return 0;
    return 1;
}

static uint8_t g_buf[2048];

static void test_ok(void)
{
    R3DiscoveryReport rep;
    const size_t sz = build(g_buf, sizeof(g_buf), kGood, GOOD_COUNT, 0);

    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_OK, "good table accepted");
    CHECK(rep.ip_count == GOOD_COUNT, "ip_count %u, want %u", rep.ip_count,
          (unsigned)GOOD_COUNT);
    CHECK(rep.expect_found == kPhoenixExpectCount, "all expected found");
    CHECK(rep.expect_major_bad == 0 && rep.expect_missing == 0, "no major/missing");
    CHECK(rep.ips[0].hwId == 11 && rep.ips[0].bases[0] == 0x1000, "GC base copied");
    CHECK(rep.ips[4].hwId == 42 && rep.ips[4].bases[0] == 0x5000, "SDMA base copied");
    CHECK(rep.ips[7].major == 6 && rep.ips[7].minor == 0, "IH 6.0 copied");
}

static void test_minor(void)
{
    struct FixIp ips[GOOD_COUNT];
    R3DiscoveryReport rep;
    memcpy(ips, kGood, sizeof(ips));
    ips[0].minor = 3;              /* GC 11.3.1 vs expected 11.0 */

    const size_t sz = build(g_buf, sizeof(g_buf), ips, GOOD_COUNT, 0);
    /* INV-D3 (batch 1c): a minor mismatch now fails closed like a major mismatch. */
    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_E_EXPECT,
          "minor diff rejected");
    CHECK(rep.minor_diffs >= 1, "minor diff counted");
    CHECK(rep.ip_count == 0, "no base table on minor mismatch");
    CHECK(ips_all_zero(rep), "base table zeroed on minor mismatch");
    const R3ExpectVerdict* v = verdict(rep, "GC");
    CHECK(v && v->minor_diff && v->seen_minor == 3 && v->expected_minor == 0,
          "GC minor recorded (seen %u)", v ? v->seen_minor : 255u);
    CHECK(v && v->seen_revision == 1, "GC revision recorded");
}

static void test_revision(void)
{
    struct FixIp ips[GOOD_COUNT];
    R3DiscoveryReport rep;
    memcpy(ips, kGood, sizeof(ips));
    ips[0].rev = 9;                /* GC 11.0.9 vs 11.0.1 */

    const size_t sz = build(g_buf, sizeof(g_buf), ips, GOOD_COUNT, 0);
    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_OK, "revision diff accepted");
    const R3ExpectVerdict* v = verdict(rep, "GC");
    CHECK(v && v->seen_revision == 9, "revision recorded as 9");
}

static void test_major_high(void)
{
    struct FixIp ips[GOOD_COUNT];
    R3DiscoveryReport rep;
    memcpy(ips, kGood, sizeof(ips));
    ips[0].major = 12;             /* GC 12.x: higher major must still fail */

    const size_t sz = build(g_buf, sizeof(g_buf), ips, GOOD_COUNT, 0);
    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_E_EXPECT,
          "higher major rejected");
    CHECK(rep.expect_major_bad >= 1, "major mismatch counted");
    CHECK(rep.ip_count == 0, "no base table on mismatch");
    CHECK(ips_all_zero(rep), "base table zeroed on mismatch");
}

static void test_major_low(void)
{
    struct FixIp ips[GOOD_COUNT];
    R3DiscoveryReport rep;
    memcpy(ips, kGood, sizeof(ips));
    ips[0].major = 10;

    const size_t sz = build(g_buf, sizeof(g_buf), ips, GOOD_COUNT, 0);
    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_E_EXPECT, "lower major rejected");
    CHECK(rep.ip_count == 0 && ips_all_zero(rep), "no base table on lower major");
}

static void test_missing(void)
{
    R3DiscoveryReport rep;
    struct FixIp ips[GOOD_COUNT - 1];
    size_t n = 0;

    /* Drop NBIO (hw 108): still a valid binary, but an expected IP is absent. */
    for (size_t i = 0; i < GOOD_COUNT; i++)
        if (kGood[i].hw != 108u)
            ips[n++] = kGood[i];

    const size_t sz = build(g_buf, sizeof(g_buf), ips, n, 0);
    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_E_EXPECT, "missing IP rejected");
    CHECK(rep.expect_missing >= 1, "missing counted");
    CHECK(rep.ip_count == 0 && ips_all_zero(rep), "no base table when IP missing");
}

static void test_corrupt(void)
{
    R3DiscoveryReport rep;
    size_t sz = build(g_buf, sizeof(g_buf), kGood, GOOD_COUNT, 1);

    CHECK(r3_discovery_run(g_buf, sz, nullptr, 0, &rep) == R3_DISC_E_PARSE,
          "bad checksum rejected");
    CHECK(rep.ip_count == 0 && ips_all_zero(rep), "no base table on bad checksum");

    memset(g_buf, 0xAB, sizeof(g_buf));
    CHECK(r3_discovery_run(g_buf, sizeof(g_buf), nullptr, 0, &rep) == R3_DISC_E_PARSE,
          "random buffer rejected");
    CHECK(rep.ip_count == 0 && ips_all_zero(rep), "no base table on random buffer");
}

static void test_args(void)
{
    R3DiscoveryReport rep;

    CHECK(r3_discovery_run(g_buf, sizeof(g_buf), nullptr, 0, nullptr) == R3_DISC_E_ARG,
          "null report");
    CHECK(r3_discovery_run(nullptr, sizeof(g_buf), nullptr, 0, &rep) == R3_DISC_E_ARG,
          "null buffer");
    CHECK(r3_discovery_run(g_buf, 100, nullptr, 0, &rep) == R3_DISC_E_ARG, "short buffer");
    CHECK(rep.ip_count == 0, "no base table on bad args");
}

static void test_fallback(void)
{
    R3DiscoveryReport rep;
    memset(&rep, 0, sizeof(rep));

    CHECK(r3_discovery_atom_fallback(g_buf, sizeof(g_buf), &rep) == R3_DISC_E_FALLBACK,
          "fallback stub fails");
    CHECK(rep.ip_count == 0, "fallback produces no base table");

    /* Corrupt BAR5 + a ROM image: the stub declines, so the original error stands. */
    size_t sz = build(g_buf, sizeof(g_buf), kGood, GOOD_COUNT, 1);
    CHECK(r3_discovery_run(g_buf, sz, g_buf, 16, &rep) == R3_DISC_E_PARSE,
          "fail-closed chain ends in the original error");
    CHECK(rep.ip_count == 0 && ips_all_zero(rep), "chain leaves no base table");
}

/* D1 RAM path: T2 copies the 10 KiB VRAM TMR block into RAM and hands the whole copy to the
 * parser. Equivalence check: a binary placed at a non-zero offset in that 10 KiB buffer must
 * parse exactly like the same binary fed from a zero-based slice. */
static void test_ram_slice(void)
{
    static uint8_t tmr[10u << 10];
    memset(tmr, 0, sizeof(tmr));
    const size_t off = 2048;
    const size_t sz = build(tmr + off, sizeof(tmr) - off, kGood, GOOD_COUNT, 0);
    CHECK(sz > 0 && off + sz <= sizeof(tmr), "binary built inside the 10 KiB RAM copy");

    R3DiscoveryReport whole, slice;
    CHECK(r3_discovery_run(tmr, sizeof(tmr), nullptr, 0, &whole) == R3_DISC_OK,
          "TMR RAM copy (whole 10 KiB) accepted");
    CHECK(r3_discovery_run(tmr + off, sz, nullptr, 0, &slice) == R3_DISC_OK,
          "zero-based slice of the same bytes accepted");
    CHECK(whole.ip_count == slice.ip_count && whole.ip_count == GOOD_COUNT,
          "RAM copy and slice produce the same IP count");
    CHECK(memcmp(whole.ips, slice.ips, sizeof(whole.ips)) == 0,
          "RAM copy and slice produce the same base table");
}

static void test_codes(void)
{
    CHECK(strcmp(r3_discovery_code_name(R3_DISC_OK), "OK") == 0, "code OK");
    CHECK(strcmp(r3_discovery_code_name(R3_DISC_E_PARSE), "PARSE") == 0, "code PARSE");
    CHECK(strcmp(r3_discovery_code_name(R3_DISC_E_EXPECT), "EXPECT") == 0, "code EXPECT");
    CHECK(strcmp(r3_discovery_code_name(R3_DISC_E_FALLBACK), "FALLBACK") == 0, "code FALLBACK");
}

int main(void)
{
    test_ok();
    test_minor();
    test_revision();
    test_major_high();
    test_major_low();
    test_missing();
    test_corrupt();
    test_args();
    test_fallback();
    test_codes();
    test_ram_slice();

    if (failures) {
        printf("test_discovery: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_discovery: %d checks passed\n", checks);
    return 0;
}
