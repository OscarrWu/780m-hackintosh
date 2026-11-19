// test_userclient_info.cpp - host unit test for NaviR3UserClient selector 0
// offset-chunked read (the S3 info fix).
//
// The real IOKit is Apple-only, so ucmock/ provides the minimal IOUserClient
// surface and dalshim/ the real kIOReturn values. The call shapes modelled
// here follow tmp/main/技术方案-S3-info分块读取改造.md: selector 0 now
// expects scalar input (offset), caps struct output at NAVI48_UC_MAX_XFER,
// and returns total via scalar output — same shape as doReadLog.
//
#include "rdna3_survey.h"   // R3_SURVEY_TEXT_BYTES (the kernel TU gets it via
                            // NaviR3Bringup.hpp's KERNEL section)
// Build/run via run_host_tests.sh (suite "test_userclient_info"). The driver
// TU is compiled with -Wall -Werror but NOT -Wextra: the kernel build
// (Makefile.linux) also uses plain -Wall, and the driver's unused
// IOExternalMethodDispatch/OSObject parameters would otherwise fail here
// without any real defect.
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>   // dalshim: IOLog no-op

#include <cstdio>
#include <cstring>
#include <string>

// Linkage-only stubs for the n48log ring: the kernel links amd/n48log.cpp,
// host tests link these. Selector 0 never calls them.
namespace amdgpu {
uint32_t n48_log_read(uint32_t offset, void *dst, uint32_t max, uint32_t *total) {
    (void)offset; (void)dst; (void)max;
    if (total) *total = 0;
    return 0;
}
void n48_log_reset(void) {}
bool n48_log_overflowed(void) { return false; }
uint32_t n48_log_consume(uint32_t upTo, uint64_t *consumedTotal, uint64_t *droppedTotal) {
    (void)upTo;
    if (consumedTotal) *consumedTotal = 0;
    if (droppedTotal) *droppedTotal = 0;
    return 0;
}
} // namespace amdgpu

// Host stand-in for the provider. The real NaviR3Bringup class is declared
// only in the KERNEL section of NaviR3Bringup.hpp; the non-KERNEL section
// defines no class, so this definition does not conflict with the include
// inside NaviR3UserClient.cpp below.
class NaviR3Bringup : public IOService {
public:
    const char *surveyText() const { return text; }
    IOReturn triggerPowerUp() { return kIOReturnSuccess; }
    const char *text = nullptr;
};

#include "../NaviR3UserClient.cpp"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); g_fail++; } \
} while (0)

static const char kText[] = "R3SURVEY d1: ip table ok\nR3SURVEY d3: dcn init ok\n";

static void fillArgs(IOExternalMethodArguments *args, uint64_t scalarIn,
                     void *structOut, uint32_t structOutSize, uint64_t *scalarOut,
                     uint32_t scalarOutCount) {
    memset(args, 0, sizeof(*args));
    args->selector = kN48R3SelInfo;
    args->scalarInput = &scalarIn;
    args->scalarInputCount = 1;
    args->structureOutput = structOut;
    args->structureOutputSize = structOutSize;
    args->scalarOutput = scalarOut;
    args->scalarOutputCount = scalarOutCount;
}
// ---- X-02: ABI pure functions (contract header Navi48R3ABI.h is included via
// NaviR3UserClient.hpp above; these are the same functions n48r3test's
// N48R3_HOST_TEST exercises, kept here for the driver-side TU). ----
static void test_abi_pure(void) {
    // selector / domain numbers are part of the ABI contract (C-10)
    CHECK(kN48R3SelInfo == 0);
    CHECK(kN48R3SelTrigger == 1);
    CHECK(kN48R3SelBoAlloc == 2);
    CHECK(kN48R3SelBoFree == 3);
    CHECK(kN48R3SelSubmit == 4);
    CHECK(kN48R3SelFenceRead == 5);
    CHECK(kN48R3SelBoImportHost == 21);
    CHECK(kN48R3DomainUMA == 0);
    CHECK(kN48R3DomainGTT == 1);

    // address-shape helpers: gpu_va must be FB-MC based for UMA BOs (C-08;
    // io->bus only ever receives carveout byte offsets)
    const uint64_t fb = 0x8000000000ull;
    const uint64_t d7 = 0x6000ull;          // D7 test_buf grant + page 3
    const uint64_t gva = n48r3_uma_gpu_va_canonical(fb, d7);
    CHECK(n48r3_uma_gpu_va(fb, d7) == fb + d7);
    CHECK(n48r3_va_canonical(gva) == 1);   // canonical (upper 16 bits all 0/1)
    CHECK(n48r3_va_strip(gva) == fb + d7);
    CHECK(n48r3_carveout_offset(fb, gva, 4096) == d7);   // MC -> carveout
    CHECK(n48r3_carveout_offset(fb, 0x100ull, 4096) == 0);  // below FB base
    CHECK(n48r3_carveout_offset(0, gva, 4096) == 0);        // FB base unset
    const uint64_t lo = 0x89abcdefull, hi = 0x01234567ull;
    CHECK(n48r3_va_from_scalars(lo, hi) == ((hi << 32) | lo));

    // BOAlloc argument validation (selector 2)
    CHECK(n48r3_bo_alloc_args(4096, kN48R3DomainUMA) == 0);
    CHECK(n48r3_bo_alloc_args(0x100000, kN48R3DomainGTT) == 0);
    CHECK(n48r3_bo_alloc_args(0, kN48R3DomainUMA) != 0);
    CHECK(n48r3_bo_alloc_args(0x100001, kN48R3DomainUMA) != 0);
    CHECK(n48r3_bo_alloc_args(4096, 7) != 0);

    // cs_ib validation (C-05: 4-aligned, length cap, canonical VA, ip_type)
    struct N48R3CsIb good = {gva, 4096, kN48R3IpCompute};
    CHECK(n48r3_cs_ib_valid(&good) == 0);
    struct N48R3CsIb bad0 = {gva, 0, kN48R3IpCompute};
    CHECK(n48r3_cs_ib_valid(&bad0) != 0);
    struct N48R3CsIb bad1 = {gva, 18, kN48R3IpCompute};
    CHECK(n48r3_cs_ib_valid(&bad1) != 0);
    struct N48R3CsIb bad2 = {gva, N48R3_CS_IB_BYTES_MAX + 4, kN48R3IpCompute};
    CHECK(n48r3_cs_ib_valid(&bad2) != 0);
    struct N48R3CsIb bad3 = {gva, 4096, 7};
    CHECK(n48r3_cs_ib_valid(&bad3) != 0);
    struct N48R3CsIb bad4 = {0x0001000000000000ull, 4096, kN48R3IpCompute};
    CHECK(n48r3_cs_ib_valid(&bad4) != 0);   // non-canonical

    // PM4 NO_OP 常量与完整包形态（C-09 回绕填充；PACKET3(PACKET3_NOP,0)：
    // TYPE3<<30 | 0x10<<8 | 0<<16，nvd.h:48-55；包长 count+2 = 2 dwords）
    CHECK(N48R3_PM4_NOP == 0xC0001000u);
    CHECK(N48R3_PM4_NOP_PAIR == 0x00000000C0001000ull);

    // cs_in parsing/validation
    struct Payload { struct N48R3CsIn in; struct N48R3CsIb ibs[1]; } pl;
    memset(&pl, 0, sizeof(pl));
    pl.in.abi_version = N48R3_UC_ABI_VERSION;
    pl.in.num_ibs = 1;
    pl.ibs[0] = good;
    const struct N48R3CsIn *h = nullptr;
    const struct N48R3CsIb *list = nullptr;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), &h, &list) == 0);
    CHECK(h != nullptr && h->num_ibs == 1);
    CHECK(list != nullptr && list[0].va_start == gva);
    CHECK(n48r3_cs_in_validate(nullptr, 4, nullptr, nullptr) != 0);
    CHECK(n48r3_cs_in_validate(&pl, 8, nullptr, nullptr) != 0);
    pl.in.abi_version = 1;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) != 0);
    pl.in.abi_version = N48R3_UC_ABI_VERSION;
    pl.in.num_ibs = 0;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) != 0);
    pl.in.num_ibs = N48R3_CS_IB_MAX + 1;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) != 0);
    pl.in.num_ibs = 1;
    pl.in.flags = 1;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) != 0);
    pl.in.flags = 0;
    pl.ibs[0] = bad1;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) != 0);
    pl.ibs[0] = good;
    CHECK(n48r3_cs_in_validate(&pl, sizeof(pl), nullptr, nullptr) == 0);

    // WRITE_DATA packet golden shape (compute user-IB; pm4_v11.cpp:46-59)
    uint32_t pkt[5];
    const uint32_t magic = 0xCAFEDEADu;
    CHECK(n48r3_pm4_write_data(pkt, fb + 0x100, magic) == 5);
    CHECK(pkt[0] == 0xC0370003u && pkt[1] == 0x00100500u);
    CHECK(pkt[2] == (uint32_t)((fb + 0x100) & 0xFFFFFFFFull));
    CHECK(pkt[3] == (uint32_t)((fb + 0x100) >> 32));
    CHECK(pkt[4] == magic);
    CHECK(n48r3_pm4_write_data(pkt, 0x1001u, 0) != 5);  // unaligned rejected
}

int main() {
    NaviR3Bringup owner;
    owner.text = kText;
    const size_t textLen = strlen(kText);

    // 1. offset=0, max=4096: first chunk returns min(4096, textLen), total.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[4096];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnSuccess);
        CHECK(args.structureOutputSize == textLen);
        CHECK(scalarOut[0] == textLen);
        CHECK(memcmp(buf, kText, textLen) == 0);
    }

    // 2. offset=0, max=16: first chunk truncated by max.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[16];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnSuccess);
        CHECK(args.structureOutputSize == 16u);
        CHECK(scalarOut[0] == textLen);
        CHECK(memcmp(buf, kText, 16) == 0);
    }

    // 3. offset=40 (mid-chunk): content matches the corresponding section.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 40, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnSuccess);
        CHECK(args.structureOutputSize == textLen - 40);
        CHECK(scalarOut[0] == textLen);
        CHECK(memcmp(buf, kText + 40, textLen - 40) == 0);
    }

    // 4. offset==total: 0 bytes + total, rc=success (past-end clamp).
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, textLen, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnSuccess);
        CHECK(args.structureOutputSize == 0u);
        CHECK(scalarOut[0] == textLen);
    }

    // 5. offset > total: 0 bytes + total, rc=success (clamp).
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 999, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnSuccess);
        CHECK(args.structureOutputSize == 0u);
        CHECK(scalarOut[0] == textLen);
    }

    // 6. scalarInputCount=0: kIOReturnBadArgument.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, buf, sizeof(buf), scalarOut, 1);
        args.scalarInputCount = 0;
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnBadArgument);
    }

    // 7. no structureOutput: kIOReturnBadArgument.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, nullptr, 64, scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnBadArgument);
    }

    // 8. structureOutputSize=0: kIOReturnBadArgument.
    {
        NaviR3UserClient uc;
        CHECK(uc.start(&owner));
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, buf, 0, scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnBadArgument);
    }

    // 9. no owner (client never started): kIOReturnNotReady.
    {
        NaviR3UserClient uc;
        char buf[64];
        uint64_t scalarOut[1];
        IOExternalMethodArguments args;
        fillArgs(&args, 0, buf, sizeof(buf), scalarOut, 1);
        IOReturn r = uc.externalMethod(kN48R3SelInfo, &args, nullptr, nullptr, nullptr);
        CHECK(r == kIOReturnNotReady);
    }

    // X-02: ABI pure functions (see test_abi_pure above)
    test_abi_pure();


    if (g_fail)
        printf("test_userclient_info: %d check(s) FAILED\n", g_fail);
    else
        printf("test_userclient_info: all checks passed\n");
    return g_fail ? 1 : 0;
}
