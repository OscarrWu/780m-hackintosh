/*
 * rdna3_ip_expect.h - the compile-time Phoenix (780M) IP expectation table (D3).
 *
 * This is the REFERENCE the IP-discovery table is checked against, not the source of truth
 * for bases: the discovered base addresses remain authoritative (D3, "以实机为准"). The
 * comparison rule (implemented in rdna3_discovery.cpp, fail-closed per INV-D3 since
 * batch 1c) is:
 *
 *   - MAJOR must be equal            -> otherwise fail closed (F group)
 *   - MINOR must be equal            -> otherwise fail closed (F group, INV-D3)
 *   - REVISION is never compared     -> accepted and recorded
 *
 * IDs come from the base parser's IpDiscovery::HwId enum (src/ipdiscovery.hpp) so we never
 * guess a hardware id. Two facts from the Linux v7.2 tree shape the table:
 *   - VCN/UVD is hw id 12 (soc15_hw_ip.h: VCN_HWID = UVD_HWID = 12). The base enum does not
 *     name it, so it is spelled out below.
 *   - MES and GMC have no standalone IP-discovery block on RDNA3; amdgpu selects gmc_v11_0
 *     and mes_v11_0 from the GC block's version (amdgpu_discovery.c:
 *     amdgpu_discovery_set_gmc_ip_blocks switches on GC_HWIP 11.0.x; mes_v11_0 major=11).
 *     Their 11.0 expectation is therefore validated by the GC row, not by a separate hw id.
 *
 * IH is OSSSYS (hw 40). Phoenix reports OSSSYS 6.0.x and Linux selects ih_v6_0 for it
 * (amdgpu_discovery.c: amdgpu_discovery_set_ih_ip_blocks, OSSSYS 6.0.x -> ih_v6_0). The
 * 6.0 expectation is confirmed by the real 780M discovery dump at
 * evidence/src/nullarc/hardware/dumps/firebat_u6c/ip_discovery.bin (hw=40 OSSSYS v6.0.1 on
 * a GC 11.0.1 / DCN 3.1.4 board). It is NOT the 7.0 an earlier draft of the plan/card carried.
 */

#ifndef N48_RDNA3_IP_EXPECT_H
#define N48_RDNA3_IP_EXPECT_H

#include <stdint.h>

#include "ipdiscovery.hpp"

struct IpExpect {
    IpDiscovery::HwId id;
    uint8_t major;
    uint8_t minor;
    const char* name;
};

/* VCN/UVD discovery id (soc15_hw_ip.h). */
constexpr IpDiscovery::HwId kR3HwVcn = static_cast<IpDiscovery::HwId>(12);

/* Discovery-discoverable Phoenix IPs. GMC 11.0 and MES 11.0 are implied by the GC row. */
static constexpr IpExpect kPhoenixExpect[] = {
    { IpDiscovery::HwGc,    11, 0, "GC"   },   /* also GMC 11.0 / MES 11.0 */
    { IpDiscovery::HwDmu,    3, 1, "DCN"  },
    { IpDiscovery::HwMp0,   13, 0, "PSP"  },
    { IpDiscovery::HwMp1,   13, 0, "SMU"  },
    { IpDiscovery::HwSdma0,  6, 0, "SDMA" },
    { kR3HwVcn,              4, 0, "VCN"  },
    { IpDiscovery::HwNbif,   7, 7, "NBIO" },
    { IpDiscovery::HwOsssys, 6, 0, "IH"   },
};

constexpr uint32_t kPhoenixExpectCount =
    sizeof(kPhoenixExpect) / sizeof(kPhoenixExpect[0]);

#endif /* N48_RDNA3_IP_EXPECT_H */
