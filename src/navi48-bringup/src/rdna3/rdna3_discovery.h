/*
 * rdna3_discovery.h - IP discovery for the RDNA3 survey (D3/D3b).
 *
 * Reuses the base parser (src/ipdiscovery.{hpp,cpp}, BSD-3, unmodified) to read the discovery
 * binary from BAR5, then checks every expected Phoenix IP in rdna3_ip_expect.h. The check is
 * fail-closed (INV-D3, batch 1c): any MAJOR-version mismatch, MINOR-version mismatch or
 * missing expected IP returns an error and produces NO base table. Only a revision
 * difference is accepted and recorded in the report (the survey logs it). Base addresses,
 * when the check passes, come from the real table (D3: 以实机为准).
 *
 * The parser only reads the caller's buffer. r3_discovery_run() validates the discovery table
 * against the expected IP list (rdna3_ip_expect.h) and fails closed: any validation failure
 * returns an error and leaves NO base table, so the driver must reject startup. The ATOM/VBIOS
 * fallback hook (D3b) is a provisional stub in stage 1 and cannot succeed; the fail-closed path
 * is therefore the only one exercised. The rdna3 host tests cover that behavior.
 */
#ifndef N48_RDNA3_DISCOVERY_H
#define N48_RDNA3_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "ipdiscovery.hpp"

#define R3_DISC_MAX_IPS 64
#define R3_DISC_MAX_EXPECT 12

/* One IP block's base table, copied out of the parser so the caller does not keep the BAR5
 * buffer alive. `bases[]` are dword register segment bases (D3: use the real table). */
struct R3IpBase {
    uint16_t hwId;
    uint8_t instance;
    uint8_t major, minor, revision;
    uint8_t numBases;
    uint32_t bases[IpDiscovery::MaxBases];
};

/* One expected-IP verdict. */
struct R3ExpectVerdict {
    IpDiscovery::HwId id;
    const char* name;
    uint8_t expected_major, expected_minor;
    uint8_t seen_major, seen_minor, seen_revision;
    bool found;
    bool major_ok;    /* seen_major == expected_major */
    bool minor_diff;  /* seen_minor != expected_minor (recorded AND fail-closed, INV-D3) */
};

/* The full outcome. `ips[]` is populated ONLY on R3_DISC_OK; on any failure `ip_count` is 0
 * and every entry in `ips[]` is zeroed (no base table is produced). `expect[]` is filled for
 * diagnostics even on failure. This is a caller-owned buffer (~4.8 KiB / 4920 B); keep it off a
 * kernel stack (store it in the driver instance). */
struct R3DiscoveryReport {
    uint32_t ip_count;
    uint32_t expect_count;
    uint32_t expect_found;
    uint32_t expect_major_bad;
    uint32_t expect_missing;
    uint32_t minor_diffs;
    R3ExpectVerdict expect[R3_DISC_MAX_EXPECT];
    R3IpBase ips[R3_DISC_MAX_IPS];
};

enum {
    R3_DISC_OK = 0,
    R3_DISC_E_ARG = -1,      /* null/undersized buffer or null report */
    R3_DISC_E_PARSE = -2,    /* signature/checksum/table invalid */
    R3_DISC_E_EXPECT = -3,   /* a MAJOR or MINOR mismatch, or a missing expected IP */
    R3_DISC_E_FALLBACK = -4  /* the ATOM/VBIOS fallback declined or is unimplemented */
};

/* Parse `bar5`, check every expected IP, and on success fill the base table. If parsing or the
 * check fails, try the ATOM/VBIOS hook on `rom` (D3b; pass NULL,0 when no ROM image exists).
 * If the hook also fails, return the original error with no base table. Never touches hardware.
 */
int r3_discovery_run(const uint8_t* bar5, size_t bar5_size,
                     const uint8_t* rom, size_t rom_size,
                     R3DiscoveryReport* out);

/* Validate an already-parsed table against kPhoenixExpect. Zeroes `out` first. */
int r3_ip_expect_check(const IpDiscovery& disc, R3DiscoveryReport* out);

/* ATOM/VBIOS fallback hook (D3b). Stage 1 ships the interface only: TODO(survey 后补) - locate
 * the IP table in the injected ROM image, read the GC version, and fill a minimal base table.
 * Until then it always returns R3_DISC_E_FALLBACK and leaves `out` without a base table. */
int r3_discovery_atom_fallback(const uint8_t* rom, size_t rom_size, R3DiscoveryReport* out);

/* Human-readable name for a return code ("OK", "ARG", ...). */
const char* r3_discovery_code_name(int rc);

#endif /* N48_RDNA3_DISCOVERY_H */
