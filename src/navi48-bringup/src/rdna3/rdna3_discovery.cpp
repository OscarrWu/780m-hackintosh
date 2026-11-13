// rdna3_discovery.cpp - see rdna3_discovery.h for the contract.
#include "rdna3_discovery.h"

#include "rdna3_ip_expect.h"

namespace {

void zero_bytes(void* p, size_t n)
{
    uint8_t* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

void zero_report(R3DiscoveryReport* out)
{
    zero_bytes(out, sizeof(*out));
}

// Copy the parsed per-IP base table into the report. Returns false (and leaves the report
// cleared) if the parser cannot produce an entry it claimed to have.
bool fill_bases(const IpDiscovery& disc, R3DiscoveryReport* out)
{
    uint32_t n = disc.ipCount();
    if (n > R3_DISC_MAX_IPS)
        n = R3_DISC_MAX_IPS;

    for (uint32_t i = 0; i < n; i++) {
        IpDiscovery::IpEntry e = {};
        if (!disc.ipAt(static_cast<uint16_t>(i), e))
            return false;

        R3IpBase& d = out->ips[i];
        d.hwId = e.hwId;
        d.instance = e.instance;
        d.major = e.major;
        d.minor = e.minor;
        d.revision = e.revision;
        d.numBases = e.numBases;
        for (uint8_t b = 0; b < e.numBases; b++)
            d.bases[b] = e.bases[b];
        out->ip_count = i + 1u;
    }
    return true;
}

} // namespace

int r3_ip_expect_check(const IpDiscovery& disc, R3DiscoveryReport* out)
{
    if (!out)
        return R3_DISC_E_ARG;

    zero_report(out);
    if (!disc.isValid())
        return R3_DISC_E_PARSE;

    out->expect_count = kPhoenixExpectCount;
    bool bad = false;

    for (uint32_t i = 0; i < kPhoenixExpectCount; i++) {
        const IpExpect& e = kPhoenixExpect[i];
        R3ExpectVerdict& v = out->expect[i];
        v.id = e.id;
        v.name = e.name;
        v.expected_major = e.major;
        v.expected_minor = e.minor;

        IpDiscovery::IpEntry ip = {};
        if (!disc.findIp(static_cast<uint16_t>(e.id), 0, ip)) {
            v.found = false;
            v.major_ok = false;
            out->expect_missing++;
            bad = true;
            continue;
        }

        v.found = true;
        v.seen_major = ip.major;
        v.seen_minor = ip.minor;
        v.seen_revision = ip.revision;
        v.major_ok = (ip.major == e.major);
        v.minor_diff = (ip.minor != e.minor);
        out->expect_found++;

        if (!v.major_ok) {
            out->expect_major_bad++;
            bad = true;
        }
        /* INV-D3 (batch 1c): the firmware-selection dimension is fail-closed on ANY
         * version mismatch - a minor difference is recorded AND fails the check
         * (R3_DISC_E_EXPECT) instead of degrading to a major-only match. The counter
         * stays for diagnostics. */
        if (v.minor_diff) {
            out->minor_diffs++;
            bad = true;
        }
    }

    if (bad) {
        out->ip_count = 0;
        return R3_DISC_E_EXPECT;
    }

    if (!fill_bases(disc, out)) {
        zero_report(out);
        return R3_DISC_E_PARSE;
    }
    return R3_DISC_OK;
}

int r3_discovery_run(const uint8_t* bar5, size_t bar5_size,
                     const uint8_t* rom, size_t rom_size,
                     R3DiscoveryReport* out)
{
    if (!out)
        return R3_DISC_E_ARG;

    zero_report(out);
    if (!bar5 || bar5_size < 512)
        return R3_DISC_E_ARG;

    IpDiscovery disc;
    int rc;

    if (!disc.init(bar5, bar5_size)) {
        rc = R3_DISC_E_PARSE;
    } else {
        rc = r3_ip_expect_check(disc, out);
        if (rc == R3_DISC_OK)
            return R3_DISC_OK;
    }

    // Fail-closed fallback (D3b): try the ATOM/VBIOS hook. In stage 1 it is a provisional stub,
    // so we return the original error and no base table (the fail-closed path the host tests
    // cover). When the hook is implemented it must fill `out` itself before returning R3_DISC_OK.
    if (rom && rom_size > 0 &&
        r3_discovery_atom_fallback(rom, rom_size, out) == R3_DISC_OK)
        return R3_DISC_OK;

    return rc;
}

int r3_discovery_atom_fallback(const uint8_t* rom, size_t rom_size, R3DiscoveryReport* out)
{
    (void)rom;
    (void)rom_size;
    (void)out;
    // TODO(survey 后补): parse the injected VBIOS image (ATOM ROM header -> master data table
    // -> "IP discovery" entry), read GC/PSP/SMU versions, and fill a minimal base table.
    return R3_DISC_E_FALLBACK;
}

const char* r3_discovery_code_name(int rc)
{
    switch (rc) {
    case R3_DISC_OK:
        return "OK";
    case R3_DISC_E_ARG:
        return "ARG";
    case R3_DISC_E_PARSE:
        return "PARSE";
    case R3_DISC_E_EXPECT:
        return "EXPECT";
    case R3_DISC_E_FALLBACK:
        return "FALLBACK";
    default:
        return "?";
    }
}
