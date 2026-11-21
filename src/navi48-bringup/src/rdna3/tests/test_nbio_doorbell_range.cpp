/*
 * test_nbio_doorbell_range — S7 CSDMA doorbell-range encoding and RSMU access.
 *
 * The doorbell range register is not directly MMIO reachable: Linux writes it
 * through the RSMU INDEX/DATA pair (nbio_v7_7.c sdma_doorbell_range).  These
 * checks pin the field encoding, the read-modify-write order, the writeguard
 * grant shape, and the fail-closed paths (missing base, bad parameters,
 * unverifiable readback).
 */
#include <stdio.h>
#include <string.h>

#include "amdgpu_ip.h"
#include "amd/nbio_v7_7.h"
#include "rdna3/rdna3_substep.h"
#include "rdna3/rdna3_writeguard.h"

using namespace amdgpu;

namespace {

static unsigned g_checks;
static unsigned g_fail;

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

struct Dev {
    uint32_t regs[64];
    uint32_t index;
    bool override_data;
    uint32_t override_value;
    Dev() : index(0u), override_data(false), override_value(0u) { memset(regs, 0, sizeof(regs)); }
};

static bool io_mmio(void *ctx, uint32_t dword, uint32_t *value, bool write)
{
    Dev &d = *static_cast<Dev *>(ctx);
    if (!value || dword >= 64u)
        return false;
    if (dword == 0x14u) {
        if (write)
            d.index = *value;
        else
            *value = d.index;
        return true;
    }
    if (dword == 0x15u) {
        if (write) {
            d.regs[0] = *value;
            return true;
        }
        *value = d.override_data ? d.override_value : d.regs[0];
        return true;
    }
    if (write)
        d.regs[dword] = *value;
    else
        *value = d.regs[dword];
    return true;
}

static void install_io(R3Io *io, Dev *d)
{
    memset(io, 0, sizeof(*io));
    io->ctx = d;
    io->mmio = io_mmio;
}

static void test_encoding(void)
{
    bool ok = false;
    CHECK(nbio_v7_7::csdma_range_value(0u, 0x200u, 20u, true, &ok) == 0x140800u && ok,
          "encode index 0x200 size 20");
    const uint32_t v = nbio_v7_7::csdma_range_value(0u, 0x200u, 20u, true, &ok);
    CHECK(((v & nbio_v7_7::kCsdmaOffsetMask) >> nbio_v7_7::kCsdmaOffsetShift) == 0x200u,
          "OFFSET decodes to 0x200");
    CHECK(((v & nbio_v7_7::kCsdmaSizeMask) >> nbio_v7_7::kCsdmaSizeShift) == 20u,
          "SIZE decodes to 20");

    /* Disable clears SIZE and preserves the OFFSET field. */
    CHECK(nbio_v7_7::csdma_range_value(0x140800u, 0u, 0u, false, &ok) == 0x800u && ok,
          "disable preserves OFFSET, clears SIZE");

    /* Re-encode replaces a stale OFFSET/SIZE pair. */
    CHECK(nbio_v7_7::csdma_range_value(0x00ff0ffcu, 0x200u, 20u, true, &ok) == 0x140800u,
          "re-encode overwrites stale fields");

    CHECK(nbio_v7_7::csdma_range_value(0u, 0x400u, 20u, true, &ok) == 0u && !ok,
          "index above 0x3ff rejected");
    CHECK(nbio_v7_7::csdma_range_value(0u, 0x200u, 0x100u, true, &ok) == 0u && !ok,
          "size above 0xff rejected");
}

static void test_no_base(void)
{
    IPBaseTable ip;
    bool ok = true;
    CHECK(nbio_v7_7::csdma_range_addr(ip, &ok) == 0u && !ok,
          "unresolved NBIO fails closed");

    ip.setBase(IPBlock::NBIO, 1, 0x14u);
    CHECK(nbio_v7_7::csdma_range_addr(ip, &ok) == 0u && !ok,
          "missing CSDMA segment fails closed");

    ip.setBase(IPBlock::NBIO, 3, 0x10400u);
    CHECK(nbio_v7_7::csdma_range_addr(ip, &ok) == 0x500efbu && ok,
          "csdma target = base3 + 0x4f0afb");
}

static void test_program(void)
{
    Dev d;
    R3Io io;
    install_io(&io, &d);
    IPBaseTable ip;
    ip.setBase(IPBlock::NBIO, 1, 0x14u);
    ip.setBase(IPBlock::NBIO, 3, 0x10400u);

    r3_writeguard_install_mm();
    CHECK(r3_wg_register_reg(0x500efbu, 1u, WClass::Protocol, WSubstep::Sdma,
                             "reg:sdma-csdma") == 0, "csdma grant");
    CHECK(r3_wg_register_shared_reg(0x14u, 1u, WClass::Protocol, WSubstep::Sdma,
                                    "reg:sdma-rsmu-index") == 0, "index grant");
    CHECK(r3_wg_register_shared_reg(0x15u, 1u, WClass::Protocol, WSubstep::Sdma,
                                    "reg:sdma-rsmu-data") == 0, "data grant");
    r3_wg_arm_substep(WSubstep::Sdma);

    uint32_t before = 0u, after = 0u;
    CHECK(nbio_v7_7::program_csdma_doorbell_range(&io, ip, 0x200u, 20u, true, &before,
                                                  &after) == nbio_v7_7::RangeResult::Ok,
          "program returns Ok");
    CHECK(d.index == (0x500efbu << 2), "INDEX carries the CSDMA target in dwords");
    CHECK(d.regs[0] == 0x140800u, "DATA carries the encoded range");
    CHECK(before == 0u && after == 0x140800u, "before/after observed");

    /* Readback divergence must fail closed. */
    d.override_data = true;
    d.override_value = 0u;
    CHECK(nbio_v7_7::program_csdma_doorbell_range(&io, ip, 0x200u, 20u, true, &before,
                                                  &after) == nbio_v7_7::RangeResult::VerifyFail,
          "divergent readback fails verification");
    r3_wg_disarm_substep();
}

} /* namespace */

int main(void)
{
    test_encoding();
    test_no_base();
    test_program();

    printf("test_nbio_doorbell_range: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail == 0u ? 0 : 1;
}
