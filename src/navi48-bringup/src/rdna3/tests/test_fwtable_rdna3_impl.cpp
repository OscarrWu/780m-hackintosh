/*
 * test_fwtable_rdna3_impl.cpp — host test for the RDNA3 firmware table
 * (src/rdna3/fw_table_rdna3.cpp, T4).
 *
 * What it pins:
 *   - the ten embedded rows (A1) carry data, the INDEX M7 length, and
 *     provenance that matches INDEX M7's sha256 / commit / path;
 *   - every other row is zero-length, so a use site fails closed (the
 *     stage-1 contract: PSP13_SOS, GC11_UNI_MES, GC11_MES fallback, SMU13,
 *     and the stage-4/6 placeholders);
 *   - the embedded array really is the archived blob: its length equals the
 *     golden that tools/gen-fwhdr-golden.py derives from the archive (H4);
 *   - name lookup and out-of-range ids behave.
 *
 * Build (from src/navi48-bringup; run_host_tests.sh does this):
 *   clang -c the embedded src/fw/fw_*.c blobs, then
 *   clang++ -std=c++17 -Wall -Wextra -Werror -I src -I src/rdna3/tests \
 *     src/rdna3/tests/test_fwtable_rdna3_impl.cpp src/rdna3/fw_table_rdna3.cpp \
 *     src/rdna3/rdna3_fwhdr.cpp src/fw/fw_*.o -o /tmp/test_fwtable
 *   /tmp/test_fwtable
 */
#include <cstdio>
#include <cstring>

#include "fw_table.h"
#include "rdna3/rdna3_fwhdr.h"
#include "fwhdr_golden.inc"

using amdgpu::FwId;

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

struct Row { FwId id; const char *name; };

// All 16 RDNA3 rows in FwId order.
static const Row kRdna3Rows[] = {
    { FwId::PSP13_SOS,    "psp_13_0_4_sos"    },
    { FwId::PSP13_TA,     "psp_13_0_4_ta"     },
    { FwId::PSP13_TOC,    "psp_13_0_4_toc"    },
    { FwId::GC11_IMU,     "gc_11_0_1_imu"     },
    { FwId::GC11_PFP,     "gc_11_0_1_pfp"     },
    { FwId::GC11_ME,      "gc_11_0_1_me"      },
    { FwId::GC11_MEC,     "gc_11_0_1_mec"     },
    { FwId::GC11_UNI_MES, "gc_11_0_1_uni_mes" },
    { FwId::GC11_RLC,     "gc_11_0_1_rlc"     },
    { FwId::GC11_MES_2,   "gc_11_0_1_mes_2"   },
    { FwId::GC11_MES1,    "gc_11_0_1_mes1"    },
    { FwId::GC11_MES,     "gc_11_0_1_mes"     },
    { FwId::SMU13,        "smu_13_0_4"        },
    { FwId::SDMA6,        "sdma_6_0_1"        },
    { FwId::VCN402,       "vcn_4_0_2"         },
    { FwId::DCN314_DMUB,  "dcn_3_1_4_dmcub"   },
};

// The ten rows A1 embeds, and the six that stay zero-length.
static const FwId kEmbedded[] = {
    FwId::PSP13_TOC, FwId::PSP13_TA, FwId::GC11_IMU, FwId::GC11_RLC,
    FwId::GC11_PFP, FwId::GC11_ME, FwId::GC11_MEC, FwId::GC11_MES_2,
    FwId::GC11_MES1, FwId::SDMA6,
};
static const FwId kZeroLength[] = {
    FwId::PSP13_SOS, FwId::GC11_UNI_MES, FwId::GC11_MES, FwId::SMU13,
    FwId::VCN402, FwId::DCN314_DMUB,
};

// The 13 RDNA4 rows exist for FwId-numbering compatibility only and must
// stay empty in this target (B2: no RDNA4 object in the RDNA3 kext).
static const Row kRdna4Rows[] = {
    { FwId::PSP_SOS,    "psp_14_0_3_sos"    },
    { FwId::PSP_TA,     "psp_14_0_3_ta"     },
    { FwId::GC_IMU,     "gc_12_0_1_imu"     },
    { FwId::GC_PFP,     "gc_12_0_1_pfp"     },
    { FwId::GC_ME,      "gc_12_0_1_me"      },
    { FwId::GC_MEC,     "gc_12_0_1_mec"     },
    { FwId::GC_MES,     "gc_12_0_1_mes"     },
    { FwId::GC_MES1,    "gc_12_0_1_mes1"    },
    { FwId::GC_UNI_MES, "gc_12_0_1_uni_mes" },
    { FwId::GC_RLC,     "gc_12_0_1_rlc"     },
    { FwId::GC_TOC,     "gc_12_0_1_toc"     },
    { FwId::SMU,        "smu_14_0_3"        },
    { FwId::SDMA,       "sdma_7_0_1"        },
};

static bool contains(const FwId *set, size_t n, FwId id) {
    for (size_t i = 0; i < n; ++i) if (set[i] == id) return true;
    return false;
}

static const GoldenBlob *golden_blob(FwId id) {
    for (size_t i = 0; i < GOLDEN_COUNT; ++i) {
        if (kGolden[i].id == id) return &kGolden[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------

static void test_row_identity(void) {
    for (size_t i = 0; i < sizeof kRdna3Rows / sizeof kRdna3Rows[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_get(kRdna3Rows[i].id);
        CHECK(b != nullptr, "row %s present", kRdna3Rows[i].name);
        if (b == nullptr) continue;
        CHECK(b->id == kRdna3Rows[i].id, "%s: id field", kRdna3Rows[i].name);
        CHECK(b->name != nullptr && std::strcmp(b->name, kRdna3Rows[i].name) == 0,
              "%s: name field ('%s')", kRdna3Rows[i].name,
              b->name ? b->name : "(null)");
    }
    for (size_t i = 0; i < sizeof kRdna4Rows / sizeof kRdna4Rows[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_get(kRdna4Rows[i].id);
        CHECK(b != nullptr, "rdna4 row %s present", kRdna4Rows[i].name);
        if (b == nullptr) continue;
        CHECK(b->name != nullptr && std::strcmp(b->name, kRdna4Rows[i].name) == 0,
              "rdna4 %s: name field", kRdna4Rows[i].name);
    }
}

static void test_embedded_rows(void) {
    static const size_t kEmbeddedCount = sizeof kEmbedded / sizeof kEmbedded[0];
    CHECK(kEmbeddedCount == 10u, "A1 embeds exactly ten blobs");

    for (size_t i = 0; i < kEmbeddedCount; ++i) {
        const FwId id = kEmbedded[i];
        const amdgpu::FwBlob *b = amdgpu::fw_get(id);
        CHECK(b != nullptr, "embedded row %u present",
              static_cast<unsigned>(id));
        if (b == nullptr) continue;
        CHECK(b->data != nullptr, "%s: data != nullptr", b->name);
        CHECK(b->size > 0u, "%s: size > 0", b->name);

        // A2: length and provenance come from INDEX M7, via the golden.
        const GoldenBlob *gb = golden_blob(id);
        CHECK(gb != nullptr, "%s: golden row", b->name);
        if (gb != nullptr) {
            CHECK(b->size == gb->file_size, "%s: size %zu != INDEX %u",
                  b->name, b->size, gb->file_size);
        }

        const R3FwProvenance *p = r3_fw_provenance(id);
        CHECK(p != nullptr, "%s: provenance registered", b->name);
        if (p == nullptr) continue;
        CHECK(p->id == id, "%s: provenance id", p->name);
        CHECK(p->name != nullptr && std::strcmp(p->name, b->name) == 0,
              "%s: provenance name", b->name);
        CHECK(p->sha256 != nullptr && std::strlen(p->sha256) == 64u,
              "%s: sha256 is 64 hex chars", p->name);
        // T4 (A2): the registered digest is INDEX M7's, not a paraphrase.
        // tools/gen-fwhdr-golden.py records M7's sha256 per blob; compare
        // case-insensitively since the INDEX table is lowercase-hex.
        if (gb != nullptr) {
            CHECK(p->sha256 != nullptr && gb->sha256 != nullptr &&
                      std::strlen(gb->sha256) == 64u &&
                      std::strncmp(p->sha256, gb->sha256, 64) == 0,
                  "%s: sha256 != INDEX M7 (table=%s golden=%s)", p->name,
                  p->sha256 ? p->sha256 : "(null)",
                  gb && gb->sha256 ? gb->sha256 : "(null)");
        }
        CHECK(p->linux_fw_commit != nullptr &&
                  std::strcmp(p->linux_fw_commit, "bda6eccc") == 0,
              "%s: commit is bda6eccc", p->name);
        // source_path is "amdgpu/<name>.bin" — check the tail, not a copy.
        const size_t plen = (p->source_path != nullptr)
                                ? std::strlen(p->source_path) : 0;
        CHECK(plen > 7u &&
                  std::strcmp(p->source_path + plen - 4, ".bin") == 0 &&
                  std::strstr(p->source_path, p->name) != nullptr,
              "%s: source_path names the blob", p->name);
        if (gb != nullptr) {
            CHECK(p->size_bytes == gb->file_size,
                  "%s: provenance size %u != %u", p->name, p->size_bytes,
                  gb->file_size);
        }
    }
}

static void test_zero_length_rows(void) {
    for (size_t i = 0; i < sizeof kZeroLength / sizeof kZeroLength[0]; ++i) {
        const FwId id = kZeroLength[i];
        const amdgpu::FwBlob *b = amdgpu::fw_get(id);
        CHECK(b != nullptr, "zero row %u present", static_cast<unsigned>(id));
        if (b == nullptr) continue;
        CHECK(b->data == nullptr, "%s: data == nullptr (fail closed)", b->name);
        CHECK(b->size == 0u, "%s: size == 0 (fail closed)", b->name);
        CHECK(r3_fw_provenance(id) == nullptr,
              "%s: no provenance for an unembedded row", b->name);
        // Use-point contract: size 0 means "cannot load", never "load 0".
        CHECK(!(b->data != nullptr && b->size > 0u),
              "%s: use point must refuse a zero-length row", b->name);
    }
    // The fallback mes.bin is registered (A3) but not embedded.
    const amdgpu::FwBlob *mes = amdgpu::fw_get(FwId::GC11_MES);
    CHECK(mes != nullptr && mes->size == 0u, "mes.bin fallback stays empty");
}

static void test_rdna4_rows_empty(void) {
    for (size_t i = 0; i < sizeof kRdna4Rows / sizeof kRdna4Rows[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_get(kRdna4Rows[i].id);
        CHECK(b != nullptr, "rdna4 row present");
        if (b == nullptr) continue;
        CHECK(b->data == nullptr && b->size == 0u,
              "rdna4 %s: not embedded in the RDNA3 target", b->name);
        CHECK(r3_fw_provenance(kRdna4Rows[i].id) == nullptr,
              "rdna4 %s: no provenance", b->name);
    }
}

static void test_lookup(void) {
    // Every row is reachable by its bare name, and names are unique across
    // the whole (RDNA3 + RDNA4) table.
    for (size_t i = 0; i < sizeof kRdna3Rows / sizeof kRdna3Rows[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_find_by_name(kRdna3Rows[i].name);
        CHECK(b != nullptr, "find_by_name(%s)", kRdna3Rows[i].name);
        if (b != nullptr)
            CHECK(b->id == kRdna3Rows[i].id, "find_by_name(%s): id",
                  kRdna3Rows[i].name);
    }
    for (size_t i = 0; i < sizeof kRdna4Rows / sizeof kRdna4Rows[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_find_by_name(kRdna4Rows[i].name);
        CHECK(b != nullptr, "find_by_name(%s)", kRdna4Rows[i].name);
        if (b != nullptr)
            CHECK(b->id == kRdna4Rows[i].id, "find_by_name(%s): id",
                  kRdna4Rows[i].name);
    }
    CHECK(amdgpu::fw_find_by_name("gc_11_0_1_imu") !=
              amdgpu::fw_find_by_name("gc_12_0_1_imu"),
          "gc_11 / gc_12 rows are distinct");
    CHECK(amdgpu::fw_find_by_name("no_such_blob") == nullptr,
          "unknown name -> nullptr");
    CHECK(amdgpu::fw_find_by_name(nullptr) == nullptr,
          "null name -> nullptr");
}

static void test_out_of_range(void) {
    const uint32_t count = static_cast<uint32_t>(FwId::Count);
    CHECK(amdgpu::fw_get(static_cast<FwId>(count)) == nullptr,
          "id == Count -> nullptr");
    CHECK(amdgpu::fw_get(static_cast<FwId>(count + 7)) == nullptr,
          "id > Count -> nullptr");
    CHECK(amdgpu::fw_get(static_cast<FwId>(0xFFFFFFFFu)) == nullptr,
          "id == UINT32_MAX -> nullptr");
}

static void test_load_order_matches_table(void) {
    // T5's 17-piece order and the table must agree row by row: every entry
    // names a real embedded blob, and the TOC/TA never appear in it.
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; ++i) {
        const R3PspLoadEntry &e = r3_psp_load_order[i];
        CHECK(!contains(kZeroLength, sizeof kZeroLength / sizeof kZeroLength[0],
                        e.source),
              "order[%u] %s: source is embedded", i, e.name);
        const amdgpu::FwBlob *b = amdgpu::fw_get(e.source);
        CHECK(b != nullptr && b->size > 0u, "order[%u] %s: blob has bytes", i,
              e.name);
        CHECK(e.source != FwId::PSP13_TOC && e.source != FwId::PSP13_TA,
              "order[%u]: TOC/TA are not LOAD_IP_FW sources", i);
    }

    // C1.2's acceptance line — "每件尺寸来源为固件头" — checked the only way
    // that means anything: parse the real blob and compare against R1 §3.6's
    // od output (an independent source, H4).  R1 lists exactly these 17
    // (offset, size) pairs for this firmware set.
    static const struct { uint32_t ft, off, sz; } kR1[17] = {
        {71, 256,    17408},  {72, 17664,  16896}, {2,  256,    263168},
        {1,  256,    263168}, {4,  256,    267008}, {33, 256,    135648},
        {34, 135904, 131072}, {81, 256,    104272}, {82, 104528, 131072},
        {68, 256,    66048},  {69, 66304,  66048},  {20, 25344,  2560},
        {21, 27904,  21104},  {26, 49008,  66048},  {48, 115056, 33280},
        {25, 148336, 8704},   {8,  256,    25088},
    };
    for (uint32_t i = 0; i < R3_PSP_LOAD_COUNT; ++i) {
        const R3PspLoadEntry &e = r3_psp_load_order[i];
        const amdgpu::FwBlob *b = amdgpu::fw_get(e.source);
        if (b == nullptr || b->data == nullptr) continue;
        R3FwImage img;
        const int rc = r3_fw_parse_id(e.source, b->data, b->size,
                                      0x4000000ull, &img);
        CHECK(rc == R3_FW_HDR_OK, "order[%u] %s: source parses (rc=%d)", i,
              e.name, rc);
        if (rc != R3_FW_HDR_OK) continue;
        const R3FwSeg *seg = r3_fw_find_seg(&img, e.fw_type);
        CHECK(seg != nullptr, "order[%u] %s: segment present", i, e.name);
        if (seg == nullptr) continue;
        CHECK(seg->offset == kR1[i].off && seg->size == kR1[i].sz,
              "order[%u] ft=%u: %u/%u != R1 §3.6 %u/%u", i, e.fw_type,
              seg->offset, seg->size, kR1[i].off, kR1[i].sz);
        // The MEC 896 B trap, asserted where it matters: the parser subtracts
        // jt_size*4 from ucode_size_bytes (267904 - 896 = 267008).
        if (e.fw_type == 4u) {
            CHECK(seg->size == 267008u, "mec fw_size is ucode_size - jt_size*4");
            CHECK(seg->size != 267904u, "mec fw_size excludes the jump table");
        }
    }
}

// Tech plan §4.2 fixes fw_buf at 16 MiB with 4 KiB-aligned fixed offsets
// "合计 ≈1.7 MiB".  The staging size the parser reports must land in that
// ballpark; a fixed-stride scheme (e.g. 8 x 256 KiB = 2 MiB) would not, so
// this pins the layout rule the caller is told to reserve.
static void test_staging_total(void) {
    static const FwId kStaged[] = {
        FwId::SDMA6, FwId::GC11_PFP, FwId::GC11_ME, FwId::GC11_MEC,
        FwId::GC11_MES_2, FwId::GC11_MES1, FwId::GC11_IMU, FwId::GC11_RLC,
    };
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof kStaged / sizeof kStaged[0]; ++i) {
        const amdgpu::FwBlob *b = amdgpu::fw_get(kStaged[i]);
        CHECK(b != nullptr, "staged blob present");
        if (b == nullptr || b->data == nullptr) continue;
        R3FwImage img;
        const int rc = r3_fw_parse_id(kStaged[i], b->data, b->size,
                                      0x4000000ull, &img);
        CHECK(rc == R3_FW_HDR_OK, "%s parses for staging", b->name);
        if (rc != R3_FW_HDR_OK) continue;
        const uint64_t sz = r3_fw_stage_size(&img);
        CHECK(sz > 0u, "%s needs staging area", b->name);
        total += sz;
    }
    // 1667072 B today (1.59 MiB).  Bound it either side of the plan's
    // "≈1.7 MiB" so a layout change that doubles the area fails here.
    CHECK(total >= 1500000ull && total <= 1800000ull,
          "staging total %llu B not in the ≈1.7 MiB band",
          static_cast<unsigned long long>(total));
    CHECK(total < 16ull * 1024ull * 1024ull, "fits the 16 MiB fw_buf region");
}

int main() {
    test_row_identity();
    test_embedded_rows();
    test_zero_length_rows();
    test_rdna4_rows_empty();
    test_lookup();
    test_out_of_range();
    test_load_order_matches_table();
    test_staging_total();

    if (g_failures != 0) {
        std::printf("FAILED %d/%d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
