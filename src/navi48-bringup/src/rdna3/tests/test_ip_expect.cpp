/*
 * test_ip_expect.cpp - pins the Phoenix expectation table.
 *
 * A wrong constant here rejects the real 780M (fail closed) or, worse, lets a foreign part
 * through. The table is the contract between the survey and the discovery check, so every row
 * is checked against the documented Phoenix values. The IH row is 6.0 (OSSSYS 6.0.1 on the
 * real 780M); GMC/MES are derived from the GC row, not separate rows.
 */
#include <stdio.h>
#include <string.h>

#include "rdna3_discovery.h"
#include "rdna3_ip_expect.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct {
    IpDiscovery::HwId id;
    uint8_t major, minor;
    const char* name;
} want[] = {
    { IpDiscovery::HwGc,    11, 0, "GC"   },
    { IpDiscovery::HwDmu,    3, 1, "DCN"  },
    { IpDiscovery::HwMp0,   13, 0, "PSP"  },
    { IpDiscovery::HwMp1,   13, 0, "SMU"  },
    { IpDiscovery::HwSdma0,  6, 0, "SDMA" },
    { kR3HwVcn,              4, 0, "VCN"  },
    { IpDiscovery::HwNbif,   7, 7, "NBIO" },
    { IpDiscovery::HwOsssys, 6, 0, "IH"   },
};

static void test_rows(void)
{
    const unsigned n = (unsigned)(sizeof(want) / sizeof(want[0]));

    CHECK(kPhoenixExpectCount == n, "row count %u, want %u", kPhoenixExpectCount, n);
    CHECK(kPhoenixExpectCount <= R3_DISC_MAX_EXPECT, "table fits report");

    for (unsigned i = 0; i < n && i < kPhoenixExpectCount; i++) {
        const IpExpect& e = kPhoenixExpect[i];
        CHECK(e.id == want[i].id, "row %u id %u, want %u", i, (unsigned)e.id,
              (unsigned)want[i].id);
        CHECK(e.major == want[i].major, "row %u (%s) major %u, want %u", i, e.name,
              e.major, want[i].major);
        CHECK(e.minor == want[i].minor, "row %u (%s) minor %u, want %u", i, e.name,
              e.minor, want[i].minor);
        CHECK(e.name && strcmp(e.name, want[i].name) == 0, "row %u name %s, want %s", i,
              e.name ? e.name : "(null)", want[i].name);
    }
}

static void test_derivations(void)
{
    CHECK(static_cast<uint16_t>(kR3HwVcn) == 12u, "VCN is hw id 12");

    for (uint32_t i = 0; i < kPhoenixExpectCount; i++) {
        /* GMC and MES have no standalone discovery block; they are implied by GC 11.0. Do
         * not let a stale MMHUB row sneak in (MMHUB is 3.0.1 on Phoenix and would fail). */
        CHECK(kPhoenixExpect[i].id != IpDiscovery::HwMmhub, "row %u is not MMHUB", i);
        /* Unique ids: a duplicate would silently validate one block twice. */
        for (uint32_t j = i + 1; j < kPhoenixExpectCount; j++)
            CHECK(kPhoenixExpect[i].id != kPhoenixExpect[j].id, "ids %u and %u unique", i, j);
    }

    /* The GC row pins GMC 11.0 and MES 11.0. */
    bool saw_gc = false;
    for (uint32_t i = 0; i < kPhoenixExpectCount; i++) {
        if (kPhoenixExpect[i].id == IpDiscovery::HwGc) {
            saw_gc = true;
            CHECK(kPhoenixExpect[i].major == 11, "GC major pins GMC/MES 11.0");
        }
    }
    CHECK(saw_gc, "GC row present");
}

int main(void)
{
    test_rows();
    test_derivations();

    if (failures) {
        printf("test_ip_expect: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_ip_expect: %d checks passed\n", checks);
    return 0;
}
