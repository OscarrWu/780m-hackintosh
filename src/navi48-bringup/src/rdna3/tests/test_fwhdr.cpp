/*
 * test_fwhdr.cpp — host tests for RDNA3 firmware header parsing (T5, T6).
 *
 * T6: the ten archived blobs are parsed as they are embedded (fw_get(id)
 *     returns the real array built from the firmware/ blobs), so every expected
 *     length has teeth.  The expected values come from
 *     src/rdna3/tests/fwhdr_golden.inc, which tools/gen-fwhdr-golden.py
 *     derives from the archived blobs with an independent decoder (H4: never
 *     from the parser under test); the first 256 bytes of each blob are in
 *     that same file, so the negative cases below can mutate real headers.
 * T5: the 17-piece LOAD_IP_FW table — count, order, exclusions, and that
 *     every entry resolves to a segment of its source blob with the golden
 *     offset/size.
 *
 * Negative cases: every rejection class the parsers owe (length past the
 * buffer, offset past the declared length, zero-length required payload,
 * unaligned staging address / payload start, RLC version outside v2.0–v2.3,
 * and the ucode_size_bytes-as-payload trap).
 *
 * Build (from src/navi48-bringup; run_host_tests.sh does this):
 *   for f in the firmware/ blobs; do python3 tools/embed-firmware.py ... ; done
 *   clang++ -std=c++17 -Wall -Wextra -Werror -I src -I src/rdna3/tests \
 *     src/rdna3/tests/test_fwhdr.cpp src/rdna3/rdna3_fwhdr.cpp \
 *     src/rdna3/fw_table_rdna3.cpp src/fw/fw_*.o -o /tmp/test_fwhdr
 *   /tmp/test_fwhdr
 */
#include <cstdio>
#include <cstring>

#include "fw_table.h"
#include "rdna3/rdna3_fwhdr.h"
#include "fwhdr_golden.inc"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

// A 64 KiB scratch buffer, well past the largest header+header-copy need.
static uint8_t g_buf[1 << 16];

static void put32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// ---------------------------------------------------------------------
//  T6 — real archived headers
// ---------------------------------------------------------------------

static const GoldenBlob *golden_row(amdgpu::FwId id) {
    for (size_t i = 0; i < GOLDEN_COUNT; ++i) {
        if (kGolden[i].id == id) return &kGolden[i];
    }
    return nullptr;
}

// Negative-case helper: a copy of a blob's real first 256 bytes on top of a
// zeroed buffer whose declared length is `total`.  Every header field lives
// in those 256 bytes and the parsers never read past a declared sub-blob, so
// this is a complete, self-consistent file of that size — mutate it to build
// the rejection cases.  `total` must not exceed the golden file size.
static const uint8_t *load_header(const GoldenBlob *row, uint32_t total) {
    std::memset(g_buf, 0, sizeof g_buf);
    std::memcpy(g_buf, row->head256, 256);

    put32(g_buf + 0, total);
    return g_buf;
}

static void test_t6_real_headers(void) {
    for (size_t i = 0; i < GOLDEN_COUNT; ++i) {
        const GoldenBlob &g = kGolden[i];
        const amdgpu::FwBlob *b = amdgpu::fw_get(g.id);
        CHECK(b != nullptr && b->data != nullptr, "%s: embedded blob present",
              g.name);
        if (b == nullptr || b->data == nullptr) continue;
        // The embedded array must be the archived file, byte for byte.
        CHECK(b->size == g.file_size, "%s: embedded size %zu != %u", g.name,
              b->size, g.file_size);
        const uint8_t *blob = b->data;

        R3FwImage img;
        const int rc = r3_fw_parse_id(g.id, blob, b->size, 0x4000000ull,
                                      &img);
        CHECK(rc == R3_FW_HDR_OK, "%s: parse rc=%d", g.name, rc);
        if (rc != R3_FW_HDR_OK) continue;

        CHECK(img.header_version_major == g.version_major,
              "%s: major %u != %u", g.name, img.header_version_major,
              g.version_major);
        CHECK(img.header_version_minor == g.version_minor,
              "%s: minor %u != %u", g.name, img.header_version_minor,
              g.version_minor);
        CHECK(img.ucode_version == g.ucode_version,
              "%s: ucode_version %u != %u", g.name, img.ucode_version,
              g.ucode_version);
        CHECK(img.ucode_size_bytes == g.ucode_size_bytes,
              "%s: ucode_size_bytes %u != %u", g.name, img.ucode_size_bytes,
              g.ucode_size_bytes);
        CHECK(img.array_offset == g.array_offset,
              "%s: array_offset %u != %u", g.name, img.array_offset,
              g.array_offset);
        CHECK(img.seg_count == g.seg_count,
              "%s: seg_count %u != %u", g.name, img.seg_count, g.seg_count);

        for (uint32_t s = 0; s < g.seg_count && s < img.seg_count; ++s) {
            const GoldenSeg &want = g.segs[s];
            const R3FwSeg &got = img.segs[s];
            // A plain common header leaves fw_type 0 for r3_fw_parse_common;
            // r3_fw_parse_id stamps it, so 0xFFFFFFFF here means "unset".
            const uint32_t want_type =
                want.fw_type == 0xFFFFFFFFu ? got.fw_type : want.fw_type;
            if (want.fw_type != 0xFFFFFFFFu) {
                CHECK(got.fw_type == want_type,
                      "%s seg%u: fw_type %u != %u", g.name, s, got.fw_type,
                      want_type);
            }
            CHECK(got.offset == want.offset,
                  "%s seg%u: offset %u != %u", g.name, s, got.offset,
                  want.offset);
            CHECK(got.size == want.size,
                  "%s seg%u: size %u != %u", g.name, s, got.size, want.size);
            // Every payload must sit inside the file and end inside it.
            CHECK(static_cast<uint64_t>(got.offset) + got.size <= g.file_size,
                  "%s seg%u: payload runs past EOF", g.name, s);
            // The LOAD_IP_FW address the stage-2 code will use is aligned by
            // construction (4 KiB-rounded sequential packing, as Linux does).
            const uint64_t addr = r3_fw_stage_addr(0x4000000ull, &img, s);
            CHECK((addr & (R3_FW_STAGE_ALIGN - 1ull)) == 0,
                  "%s seg%u: staging address 0x%llx not aligned", g.name, s,
                  static_cast<unsigned long long>(addr));
        }

        if (g.mes_uc_start_hi || g.mes_uc_start_lo) {
            const uint64_t want_uc =
                (static_cast<uint64_t>(g.mes_uc_start_hi) << 32) |
                g.mes_uc_start_lo;
            const uint64_t want_data =
                (static_cast<uint64_t>(g.mes_data_start_hi) << 32) |
                g.mes_data_start_lo;
            CHECK(img.mes_uc_start_addr == want_uc,
                  "%s: mes_uc_start 0x%08x%08x", g.name,
                  static_cast<uint32_t>(img.mes_uc_start_addr >> 32),
                  static_cast<uint32_t>(img.mes_uc_start_addr));
            CHECK(img.mes_data_start_addr == want_data,
                  "%s: mes_data_start 0x%08x%08x", g.name,
                  static_cast<uint32_t>(img.mes_data_start_addr >> 32),
                  static_cast<uint32_t>(img.mes_data_start_addr));
        }
    }

    // The semantic trap, stated as its own check: MEC's PSP payload is
    // ucode_size_bytes - jt_size*4 = 267904 - 896 = 267008 (amdgpu_ucode.c:906-911),
    // not the header's ucode_size_bytes and nothing to do with the whole file.
    const GoldenBlob *mec = golden_row(amdgpu::FwId::GC11_MEC);
    CHECK(mec != nullptr, "mec golden row present");
    if (mec) {
        const amdgpu::FwBlob *mb = amdgpu::fw_get(amdgpu::FwId::GC11_MEC);
        const uint8_t *blob = mb->data;
        R3FwImage img;
        const int rc = r3_fw_parse_id(amdgpu::FwId::GC11_MEC, blob,
                                      mb->size, 0x4000000ull, &img);
        CHECK(rc == R3_FW_HDR_OK, "mec parse rc=%d", rc);
        CHECK(img.segs[0].size == 267008u, "mec payload = 267008, got %u",
              img.segs[0].size);
        CHECK(img.segs[0].size != 267904u, "mec payload excludes the 896 B jump table");
        CHECK(img.segs[0].size != mec->file_size,
              "mec payload is not the whole file");
    }

    // RLC: the two zero-size optional sub-blobs must be absent (22 and 7).
    const GoldenBlob *rlc = golden_row(amdgpu::FwId::GC11_RLC);
    CHECK(rlc != nullptr, "rlc golden row present");
    if (rlc) {
        const amdgpu::FwBlob *rb = amdgpu::fw_get(amdgpu::FwId::GC11_RLC);
        const uint8_t *blob = rb->data;
        R3FwImage img;
        const int rc = r3_fw_parse_id(amdgpu::FwId::GC11_RLC, blob,
                                      rb->size, 0x4000000ull, &img);
        CHECK(rc == R3_FW_HDR_OK, "rlc parse rc=%d", rc);
        CHECK(r3_fw_find_seg(&img, 22) == nullptr, "rlc: 22 not registered");
        CHECK(r3_fw_find_seg(&img, 7) == nullptr, "rlc: 7 not registered");
        CHECK(r3_fw_find_seg(&img, 8) != nullptr, "rlc: 8 registered");
    }
}

// ---------------------------------------------------------------------
//  T6 — rejection classes
// ---------------------------------------------------------------------

static void test_t6_rejections(void) {
    const GoldenBlob *pfp = golden_row(amdgpu::FwId::GC11_PFP);
    const GoldenBlob *rlc = golden_row(amdgpu::FwId::GC11_RLC);
    const GoldenBlob *sdma = golden_row(amdgpu::FwId::SDMA6);
    const GoldenBlob *mes = golden_row(amdgpu::FwId::GC11_MES_2);
    CHECK(pfp && rlc && sdma && mes, "golden rows for the rejection set");
    if (!(pfp && rlc && sdma && mes)) return;

    R3FwImage img;

    // Argument handling.
    CHECK(r3_fw_parse_common(nullptr, 64, 0, &img) == R3_FW_HDR_ERR_NULL,
          "null data");
    CHECK(r3_fw_parse_common(g_buf, 64, 0, nullptr) == R3_FW_HDR_ERR_NULL,
          "null out");
    CHECK(r3_fw_parse_common(g_buf, 31, 0, &img) == R3_FW_HDR_ERR_SHORT,
          "short buffer");

    // Declared length past the buffer.
    const uint8_t *blob = load_header(pfp, pfp->file_size);
    uint8_t copy[512];
    std::memcpy(copy, blob, sizeof copy);
    put32(copy + 0, pfp->file_size + 4096);
    CHECK(r3_fw_parse_common(copy, sizeof copy, 0, &img) ==
              R3_FW_HDR_ERR_RANGE, "declared length past buffer");

    // A payload start that is not 4 KiB aligned: unaligned staging address.
    CHECK(r3_fw_parse_common(blob, pfp->file_size, 0x40000001ull, &img) ==
              R3_FW_HDR_ERR_ALIGN, "unaligned stage_mc");

    // array offset moved inside the header: out of range.
    std::memcpy(copy, blob, sizeof copy);
    put32(copy + 24, 0x800);
    CHECK(r3_fw_parse_common(copy, sizeof copy, 0, &img) ==
              R3_FW_HDR_ERR_RANGE, "array offset outside declared length");

    // An array offset inside the header area is corrupt (Linux trusts the
    // field otherwise, amdgpu_ucode.c:894-1005 — a repacked blob with a
    // larger-but-valid offset must still parse).
    // A repacked blob whose array offset differs from 0x100 is still legal:
    // the field is trusted (Linux does the same), and the payload is sized
    // from it.  Build a fully self-consistent 512-byte file to prove it.
    std::memcpy(copy, blob, sizeof copy);
    put32(copy + 0, sizeof copy);           // declared length
    put32(copy + 20, sizeof copy - 44u);    // ucode_size_bytes = payload
    put32(copy + 24, 44);                   // array offset = header size
    CHECK(r3_fw_parse_common(copy, sizeof copy, 0, &img) == R3_FW_HDR_OK,
          "array offset == header size is accepted (payload from there)");
    CHECK(img.segs[0].offset == 44u && img.segs[0].size == sizeof copy - 44u,
          "payload starts at the declared array offset");
    std::memcpy(copy, blob, sizeof copy);
    put32(copy + 0, sizeof copy);
    put32(copy + 20, sizeof copy - 16u);
    put32(copy + 24, 16);               // below the common header
    CHECK(r3_fw_parse_common(copy, sizeof copy, 0, &img) ==
              R3_FW_HDR_ERR_RANGE, "array offset below header rejected");

    // Zero-length payload: the declared length equals the array offset, so
    // "file length − array offset" is 0.
    std::memcpy(copy, blob, sizeof copy);
    put32(copy + 0, 0x100);
    put32(copy + 20, 0);                // ucode_size_bytes
    CHECK(r3_fw_parse_common(copy, sizeof copy, 0, &img) ==
              R3_FW_HDR_ERR_ZERO_SIZE, "zero-length payload");

    // SDMA: a required thread size of zero, and an offset past EOF.
    const uint8_t *s = load_header(sdma, sdma->file_size);
    std::memcpy(copy, s, sizeof copy);
    put32(copy + 36, 0);
    CHECK(r3_fw_parse_sdma(copy, sdma->file_size, 0, &img) ==
              R3_FW_HDR_ERR_ZERO_SIZE, "sdma zero ctx size");
    std::memcpy(copy, s, sizeof copy);
    put32(copy + 48, sdma->file_size);  // ctl offset at EOF: runs past it
    CHECK(r3_fw_parse_sdma(copy, sdma->file_size, 0, &img) ==
              R3_FW_HDR_ERR_RANGE, "sdma ctl offset past EOF");
    CHECK(r3_fw_parse_sdma(copy, sdma->file_size, 0x100, &img) ==
              R3_FW_HDR_ERR_ALIGN, "sdma unaligned stage");

    // header_size_bytes is a real bound: each class reads its own header
    // fields directly, so a blob declaring a smaller header must be refused
    // even when the buffer happens to be long enough.
    {
        const GoldenBlob *r = golden_row(amdgpu::FwId::GC11_RLC);
        const uint8_t *rb = load_header(r, r->file_size);
        // NOTE: the declared LENGTH must also be updated whenever `copy`
        // (a 512-byte buffer) stands in for the real file, or the length
        // check fires before the header check and the case proves nothing.
        std::memcpy(copy, rb, sizeof copy);
        put32(copy + 0, sizeof copy);    // self-consistent 512-byte file
        put32(copy + 4, 104);            // v2.0-sized header, version says 2.3
        CHECK(r3_fw_parse_rlc(copy, sizeof copy, 0, &img) ==
                  R3_FW_HDR_ERR_SHORT,
              "rlc: declared header smaller than the class header is refused");
        // Accepting side: the real blob (declared header 204 = v2.3 size)
        // must parse — a 512-byte stand-in could not hold RLC's segments.
        CHECK(r3_fw_parse_rlc(rb, r->file_size, 0, &img) == R3_FW_HDR_OK,
              "rlc: header exactly the class size is accepted");
        CHECK(img.declared_header_bytes == 204u,
              "rlc: declared header is the v2.3 struct size");
    }

    // IMU: a dram offset that would need 33 bits must be refused, never
    // wrapped.  (IMU_I is checked first, so this also pins that ordering.)
    const GoldenBlob *imu = golden_row(amdgpu::FwId::GC11_IMU);
    CHECK(imu != nullptr, "imu golden row");
    if (imu != nullptr) {
        const uint8_t *i = load_header(imu, imu->file_size);
        std::memcpy(copy, i, sizeof copy);
        put32(copy + 32, 0xFFFFFF00u);   // iram_size: 256 + this wraps to 0
        put32(copy + 40, 16);            // dram_size
        CHECK(r3_fw_parse_imu(copy, sizeof copy, 0, &img) != R3_FW_HDR_OK,
              "imu: wrapping iram_size is refused");
        // iram exactly filling the file: IMU_D then has no room.
        std::memcpy(copy, i, sizeof copy);
        put32(copy + 0, 0x200);          // declared length = 512
        put32(copy + 32, 0x100);         // iram = 256 => ends at 512
        put32(copy + 40, 16);
        CHECK(r3_fw_parse_imu(copy, 512, 0, &img) == R3_FW_HDR_ERR_RANGE,
              "imu: dram past the declared end is refused");
    }

    // MES: code size zero, and the version gate.
    const uint8_t *m = load_header(mes, mes->file_size);
    std::memcpy(copy, m, sizeof copy);
    put32(copy + 36, 0);
    CHECK(r3_fw_parse_mes(copy, mes->file_size, 0, &img) ==
              R3_FW_HDR_ERR_ZERO_SIZE, "mes zero code size");
    std::memcpy(copy, m, sizeof copy);
    put16(copy + 10, 1);
    CHECK(r3_fw_parse_mes(copy, mes->file_size, 0, &img) ==
              R3_FW_HDR_ERR_VERSION, "mes v1.1 rejected");

    // RLC: version whitelist v2.0–v2.3 only.
    const uint8_t *r = load_header(rlc, rlc->file_size);
    std::memcpy(copy, r, sizeof copy);
    put16(copy + 10, 4);
    CHECK(r3_fw_parse_rlc(copy, rlc->file_size, 0, &img) ==
              R3_FW_HDR_ERR_VERSION, "rlc v2.4 rejected");
    std::memcpy(copy, r, sizeof copy);
    put16(copy + 8, 1);
    put16(copy + 10, 0);
    CHECK(r3_fw_parse_rlc(copy, rlc->file_size, 0, &img) ==
              R3_FW_HDR_ERR_VERSION, "rlc v1.0 rejected");

    // RLC: a required sub-blob that runs past the file.
    std::memcpy(copy, r, sizeof copy);
    put32(copy + 156, 0x7FFFFF00);      // iram size
    CHECK(r3_fw_parse_rlc(copy, rlc->file_size, 0, &img) ==
              R3_FW_HDR_ERR_RANGE, "rlc iram past EOF");

    // RLC: a required sub-blob with size 0.
    std::memcpy(copy, r, sizeof copy);
    put32(copy + 132, 0);                // gpm size
    CHECK(r3_fw_parse_rlc(copy, rlc->file_size, 0, &img) ==
              R3_FW_HDR_ERR_ZERO_SIZE, "rlc gpm zero size");

    // Ids without a stage-2 parser fail closed rather than guessing.
    CHECK(r3_fw_parse_id(amdgpu::FwId::SMU13, g_buf, 64, 0, &img) ==
              R3_FW_HDR_ERR_VERSION, "SMU13 has no parser");
    CHECK(r3_fw_parse_id(amdgpu::FwId::GC11_MES, g_buf, 64, 0, &img) ==
              R3_FW_HDR_ERR_VERSION, "fallback MES row has no blob");
    CHECK(r3_fw_parse_id(static_cast<amdgpu::FwId>(0xFFFF), g_buf, 64, 0,
                         &img) == R3_FW_HDR_ERR_VERSION, "out-of-range id");

    // A parse failure must not leave a stale segment list behind.
    const uint8_t *p2 = load_header(pfp, pfp->file_size);
    CHECK(r3_fw_parse_id(amdgpu::FwId::GC11_PFP, p2, pfp->file_size,
                         0x4000000ull, &img) == R3_FW_HDR_OK, "pfp ok");
    CHECK(img.seg_count == 1u, "pfp one segment");
    CHECK(r3_fw_parse_common(copy, 16, 0, &img) == R3_FW_HDR_ERR_SHORT,
          "short after ok");
    CHECK(img.seg_count == 0u, "failed parse clears seg_count");

    // fw_find_seg on a hand-built image.
    R3FwImage tiny;
    std::memset(&tiny, 0, sizeof tiny);
    CHECK(r3_fw_find_seg(&tiny, 8) == nullptr, "empty image has no segment");
    CHECK(r3_fw_find_seg(nullptr, 8) == nullptr, "null image");
}

// ---------------------------------------------------------------------
//  Embedding cross-check helpers (used by T4 and by use sites)
// ---------------------------------------------------------------------

// Staging layout: 4 KiB-rounded sequential packing over the segment list,
// exactly the fw_buf shape the tech plan §4.2 fixes (每件 4 KiB 对齐固定偏移，
// 合计 ≈1.7 MiB).  Verified against an independently summed expectation.
// The descriptor is caller-owned and sits on a kernel stack in stage 2.
static void test_descriptor_size(void) {
    CHECK(sizeof(R3FwImage) <= 256u, "R3FwImage within its 256-byte budget (%zu)",
          sizeof(R3FwImage));
    CHECK(sizeof(R3FwImage) >= 248u, "R3FwImage still holds 8 segments + mes addrs");
}

static void test_stage_packing(void) {
    CHECK(r3_fw_stage_size(nullptr) == 0u, "null image needs no staging");
    CHECK(r3_fw_stage_addr(0x4000000ull, nullptr, 3) == 0x4000000ull,
          "null image stages at the base");

    const GoldenBlob *mec = golden_row(amdgpu::FwId::GC11_MEC);
    CHECK(mec != nullptr, "mec golden row");
    if (mec == nullptr) return;
    const amdgpu::FwBlob *b = amdgpu::fw_get(amdgpu::FwId::GC11_MEC);
    R3FwImage img;
    CHECK(r3_fw_parse_id(amdgpu::FwId::GC11_MEC, b->data, b->size,
                         0x4000000ull, &img) == R3_FW_HDR_OK, "mec parses");
    // One 267008 B payload rounds to 270336 B (66 pages).
    CHECK(r3_fw_stage_size(&img) == 270336ull, "mec staging size %llu",
          static_cast<unsigned long long>(r3_fw_stage_size(&img)));
    CHECK(r3_fw_stage_addr(0x4000000ull, &img, 0) == 0x4000000ull,
          "segment 0 sits at the base");

    // Multi-segment image: cumulative 4 KiB-rounded offsets, checked with a
    // hand-summed expectation (2 pages + 4 pages = segment 2 at 6 pages).
    R3FwImage multi;
    multi.seg_count = 3;
    multi.segs[0].size = 4096;   // 1 page
    multi.segs[1].size = 5000;   // -> 2 pages
    multi.segs[2].size = 1;      // -> 1 page
    const uint64_t base = 0x8000000ull;
    CHECK(r3_fw_stage_addr(base, &multi, 0) == base, "multi seg0 at base");
    CHECK(r3_fw_stage_addr(base, &multi, 1) == base + 0x1000u, "multi seg1");
    CHECK(r3_fw_stage_addr(base, &multi, 2) == base + 0x3000u, "multi seg2");
    CHECK(r3_fw_stage_size(&multi) == 0x4000u, "multi total = 4 pages");
}

// ---------------------------------------------------------------------
//  T5 — the 17-piece LOAD_IP_FW table
// ---------------------------------------------------------------------

static void test_t5_load_order(void) {
    static const uint32_t kExpect[17] = {
        71, 72, 2, 1, 4, 33, 34, 81, 82, 68, 69, 20, 21, 26, 48, 25, 8
    };
    CHECK(R3_PSP_LOAD_COUNT == 17u, "count is 17");
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; ++i) {
        CHECK(r3_psp_load_order[i].fw_type == kExpect[i],
              "order[%u]: fw_type %u != %u", i, r3_psp_load_order[i].fw_type,
              kExpect[i]);
        CHECK(r3_psp_load_order[i].name != nullptr, "order[%u] has a label", i);
        CHECK(r3_psp_load_order[i].source != amdgpu::FwId::PSP13_TOC &&
                  r3_psp_load_order[i].source != amdgpu::FwId::PSP13_TA,
              "order[%u]: TOC/TA never in the load list", i);
    }

    // Excluded by construction: MEC JT, RLC 22/7, and the RS64 set.
    static const uint32_t kForbidden[] = { 5, 6, 7, 22, 87, 88, 89 };
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; ++i) {
        uint32_t t = r3_psp_load_order[i].fw_type;
        for (size_t f = 0; f < sizeof kForbidden / sizeof kForbidden[0]; ++f) {
            CHECK(t != kForbidden[f], "order[%u]: forbidden fw_type %u", i, t);
        }
    }

    // AUTOLOAD_RLC must follow RLC_G (fw_type 8) immediately: 8 is last.
    CHECK(r3_psp_load_order[R3_PSP_LOAD_COUNT - 1].fw_type == 8u,
          "RLC_G (8) is the last entry");

    // Every entry resolves: its source blob parses and contains the
    // segment with the golden offset/size (no zero-size or absent row).
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; ++i) {
        const R3PspLoadEntry &e = r3_psp_load_order[i];
        const GoldenBlob *g = golden_row(e.source);
        CHECK(g != nullptr, "order[%u]: source has a golden row", i);
        if (!g) continue;
        const amdgpu::FwBlob *b = amdgpu::fw_get(e.source);
        R3FwImage img;
        const int rc = r3_fw_parse_id(e.source, b->data, b->size,
                                      0x4000000ull, &img);
        CHECK(rc == R3_FW_HDR_OK, "order[%u]: %s parses (rc=%d)", i, g->name,
              rc);
        if (rc != R3_FW_HDR_OK) continue;
        const R3FwSeg *seg = r3_fw_find_seg(&img, e.fw_type);
        CHECK(seg != nullptr, "order[%u]: fw_type %u present in %s", i,
              e.fw_type, g->name);
        if (seg == nullptr) continue;
        CHECK(seg->size > 0u, "order[%u]: payload size > 0", i);
        bool matched = false;
        for (uint32_t s = 0; s < g->seg_count; ++s) {
            const uint32_t gt = g->segs[s].fw_type;
            const uint32_t want = (gt == 0xFFFFFFFFu) ? e.fw_type : gt;
            if (want == e.fw_type) {
                matched = true;
                CHECK(seg->offset == g->segs[s].offset,
                      "order[%u]: offset %u != %u", i, seg->offset,
                      g->segs[s].offset);
                CHECK(seg->size == g->segs[s].size,
                      "order[%u]: size %u != %u", i, seg->size,
                      g->segs[s].size);
            }
        }
        CHECK(matched, "order[%u]: fw_type %u is a golden segment", i,
              e.fw_type);
    }
}

int main() {
    test_t6_real_headers();
    test_t6_rejections();
    test_descriptor_size();
    test_stage_packing();
    test_t5_load_order();

    if (g_failures != 0) {
        std::printf("FAILED %d/%d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
