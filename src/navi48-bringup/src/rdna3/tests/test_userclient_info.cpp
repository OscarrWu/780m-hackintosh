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

    if (g_fail)
        printf("test_userclient_info: %d check(s) FAILED\n", g_fail);
    else
        printf("test_userclient_info: all checks passed\n");
    return g_fail ? 1 : 0;
}
