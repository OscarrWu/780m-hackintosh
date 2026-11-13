//
//  test_fwtable_rdna3.cpp - host-side test for the RDNA3 FwId extension
//  (requirement B4, decision D2). Builds the real src/amd/fw_table.cpp and
//  checks:
//
//    R1  the gfx1201 (RDNA4) enum values are UNCHANGED (0..12) and the
//        in-range lookup contract is intact - pointer, id field, bare name;
//    R2  every RDNA3 id is in range, resolves to its own row, and is a
//        zero-length stub (data==nullptr, size==0) - the stage-1 fail-closed
//        convention, identical to the gfx1201 optional rows;
//    R3  fw_find_by_name resolves every RDNA3 stem AND still resolves the
//        gfx1201 stems (no name collision between gc_12_* and gc_11_*);
//    R4  out-of-range ids still return nullptr (unknown-id behaviour).
//
//  The gfx1201 blob symbols are stubbed here with size 0 because the host
//  build has no firmware/*.bin; R1 therefore asserts the lookup/enum
//  contract (the thing this change can regress), not blob byte counts.
//
//  Build (from src/navi48-bringup, same flags as tests/native_*_plant.sh):
//    clang++ -std=c++17 -Wall -Wextra -Werror -O1 \
//      -fsanitize=address,undefined -fno-sanitize-recover=all \
//      -I src -I src/amd src/rdna3/tests/test_fwtable_rdna3.cpp src/amd/fw_table.cpp \
//      -o /tmp/test_fwtable_rdna3 && /tmp/test_fwtable_rdna3
//

#include "../../fw/fw_table.h"

#include <cstdint>
#include <cstdio>

using namespace amdgpu;

// ---- stub definitions for the gfx1201 (RDNA4) generated blobs -----------
// fw_table.cpp declares these extern "C"; the host has no firmware/*.bin, so
// provide empty stand-ins (size 0) purely to satisfy the linker.
extern "C" {
extern const uint8_t fw_psp_14_0_3_sos[64];
extern const size_t  fw_psp_14_0_3_sos_size;
extern const uint8_t fw_psp_14_0_3_ta[64];
extern const size_t  fw_psp_14_0_3_ta_size;
extern const uint8_t fw_gc_12_0_1_imu[64];
extern const size_t  fw_gc_12_0_1_imu_size;
extern const uint8_t fw_gc_12_0_1_pfp[64];
extern const size_t  fw_gc_12_0_1_pfp_size;
extern const uint8_t fw_gc_12_0_1_me[64];
extern const size_t  fw_gc_12_0_1_me_size;
extern const uint8_t fw_gc_12_0_1_mec[64];
extern const size_t  fw_gc_12_0_1_mec_size;
extern const uint8_t fw_gc_12_0_1_uni_mes[64];
extern const size_t  fw_gc_12_0_1_uni_mes_size;
extern const uint8_t fw_gc_12_0_1_rlc[64];
extern const size_t  fw_gc_12_0_1_rlc_size;
extern const uint8_t fw_smu_14_0_3[64];
extern const size_t  fw_smu_14_0_3_size;
extern const uint8_t fw_sdma_7_0_1[64];
extern const size_t  fw_sdma_7_0_1_size;
}

const uint8_t fw_psp_14_0_3_sos[64] = {};
const size_t  fw_psp_14_0_3_sos_size = 0;
const uint8_t fw_psp_14_0_3_ta[64] = {};
const size_t  fw_psp_14_0_3_ta_size = 0;
const uint8_t fw_gc_12_0_1_imu[64] = {};
const size_t  fw_gc_12_0_1_imu_size = 0;
const uint8_t fw_gc_12_0_1_pfp[64] = {};
const size_t  fw_gc_12_0_1_pfp_size = 0;
const uint8_t fw_gc_12_0_1_me[64] = {};
const size_t  fw_gc_12_0_1_me_size = 0;
const uint8_t fw_gc_12_0_1_mec[64] = {};
const size_t  fw_gc_12_0_1_mec_size = 0;
const uint8_t fw_gc_12_0_1_uni_mes[64] = {};
const size_t  fw_gc_12_0_1_uni_mes_size = 0;
const uint8_t fw_gc_12_0_1_rlc[64] = {};
const size_t  fw_gc_12_0_1_rlc_size = 0;
const uint8_t fw_smu_14_0_3[64] = {};
const size_t  fw_smu_14_0_3_size = 0;
const uint8_t fw_sdma_7_0_1[64] = {};
const size_t  fw_sdma_7_0_1_size = 0;

namespace {

int g_fail = 0;
int g_run = 0;

void expect(bool ok, const char *what) {
    ++g_run;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

void expect_u(const char *what, uint32_t got, uint32_t want) {
    ++g_run;
    if (got != want) {
        ++g_fail;
        std::printf("FAIL: %s: got %u want %u\n", what, got, want);
    }
}

bool str_eq(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

struct Row { FwId id; const char *name; };

// R1: the gfx1201 rows and their numeric values (must not move when the
// RDNA3 ids are appended - the append-before-Count discipline is the
// regression guard).
const Row kRdna4[] = {
    { FwId::PSP_SOS,    "psp_14_0_3_sos" },
    { FwId::PSP_TA,     "psp_14_0_3_ta"  },
    { FwId::GC_IMU,     "gc_12_0_1_imu"  },
    { FwId::GC_PFP,     "gc_12_0_1_pfp"  },
    { FwId::GC_ME,      "gc_12_0_1_me"   },
    { FwId::GC_MEC,     "gc_12_0_1_mec"  },
    { FwId::GC_MES,     "gc_12_0_1_mes"  },
    { FwId::GC_MES1,    "gc_12_0_1_mes1" },
    { FwId::GC_UNI_MES, "gc_12_0_1_uni_mes" },
    { FwId::GC_RLC,     "gc_12_0_1_rlc"  },
    { FwId::GC_TOC,     "gc_12_0_1_toc"  },
    { FwId::SMU,        "smu_14_0_3"     },
    { FwId::SDMA,       "sdma_7_0_1"     },
};

// R2: the RDNA3 rows (zero-length stubs in the shared table; the embedded
// copies live in rdna3/fw_table_rdna3.cpp). Stage 2 appended the three MES
// rows, still as zero-length stubs here (A3: GC11_MES registered, not embedded).
const Row kRdna3[] = {
    { FwId::PSP13_SOS,   "psp_13_0_4_sos" },
    { FwId::PSP13_TA,    "psp_13_0_4_ta"  },
    { FwId::PSP13_TOC,   "psp_13_0_4_toc" },
    { FwId::GC11_IMU,    "gc_11_0_1_imu"  },
    { FwId::GC11_PFP,    "gc_11_0_1_pfp"  },
    { FwId::GC11_ME,     "gc_11_0_1_me"   },
    { FwId::GC11_MEC,    "gc_11_0_1_mec"  },
    { FwId::GC11_UNI_MES,"gc_11_0_1_uni_mes" },
    { FwId::GC11_RLC,    "gc_11_0_1_rlc"  },
    { FwId::SMU13,       "smu_13_0_4"     },
    { FwId::SDMA6,       "sdma_6_0_1"     },
    { FwId::VCN402,      "vcn_4_0_2"      },
    { FwId::DCN314_DMUB, "dcn_3_1_4_dmcub"},
    { FwId::GC11_MES_2,  "gc_11_0_1_mes_2" },
    { FwId::GC11_MES1,   "gc_11_0_1_mes1"  },
    { FwId::GC11_MES,    "gc_11_0_1_mes"   },
};

} // namespace

int main() {
    // R1a: the gfx1201 enum values are pinned. Appending RDNA3 ids must not
    // shift any of these (a break here is a silent ABI/semantic regression).
    expect_u("PSP_SOS == 0",    static_cast<uint32_t>(FwId::PSP_SOS), 0);
    expect_u("PSP_TA == 1",     static_cast<uint32_t>(FwId::PSP_TA), 1);
    expect_u("GC_IMU == 2",     static_cast<uint32_t>(FwId::GC_IMU), 2);
    expect_u("GC_PFP == 3",     static_cast<uint32_t>(FwId::GC_PFP), 3);
    expect_u("GC_ME == 4",      static_cast<uint32_t>(FwId::GC_ME), 4);
    expect_u("GC_MEC == 5",     static_cast<uint32_t>(FwId::GC_MEC), 5);
    expect_u("GC_MES == 6",     static_cast<uint32_t>(FwId::GC_MES), 6);
    expect_u("GC_MES1 == 7",    static_cast<uint32_t>(FwId::GC_MES1), 7);
    expect_u("GC_UNI_MES == 8", static_cast<uint32_t>(FwId::GC_UNI_MES), 8);
    expect_u("GC_RLC == 9",     static_cast<uint32_t>(FwId::GC_RLC), 9);
    expect_u("GC_TOC == 10",    static_cast<uint32_t>(FwId::GC_TOC), 10);
    expect_u("SMU == 11",       static_cast<uint32_t>(FwId::SMU), 11);
    expect_u("SDMA == 12",      static_cast<uint32_t>(FwId::SDMA), 12);
    expect_u("Count == 29",     static_cast<uint32_t>(FwId::Count), 29);

    // R1b: gfx1201 rows still resolve in range, with the same id and name.
    for (const Row &r : kRdna4) {
        const FwBlob *b = fw_get(r.id);
        expect(b != nullptr, r.name);
        if (b == nullptr) continue;
        expect_u("row id field", static_cast<uint32_t>(b->id), static_cast<uint32_t>(r.id));
        expect(str_eq(b->name, r.name), r.name);
        expect(b->data != nullptr || b->size == 0, "rdna4 row data/size consistency");
    }

    // R2: every RDNA3 id resolves to its own zero-length stub.
    for (const Row &r : kRdna3) {
        const FwBlob *b = fw_get(r.id);
        expect(b != nullptr, r.name);
        if (b == nullptr) continue;
        expect_u("rdna3 row id field", static_cast<uint32_t>(b->id), static_cast<uint32_t>(r.id));
        expect(str_eq(b->name, r.name), r.name);
        expect(b->data == nullptr, "rdna3 stub data == nullptr");
        expect(b->size == 0, "rdna3 stub size == 0");
    }

    // R2b: no row in the whole in-range table is left uninitialised (a
    // forgotten build_table() assignment leaves name==nullptr, which would
    // crash fw_find_by_name() - catch it here instead).
    for (uint32_t i = 0; i < static_cast<uint32_t>(FwId::Count); ++i) {
        const FwBlob *b = fw_get(static_cast<FwId>(i));
        expect(b != nullptr, "all in-range rows present");
        if (b == nullptr) continue;
        expect(b->name != nullptr, "all in-range rows named");
        expect_u("row index matches id", static_cast<uint32_t>(b->id), i);
    }

    // R3: name lookup resolves both generations, no gc_11_/gc_12_ collision.
    for (const Row &r : kRdna4) {
        const FwBlob *b = fw_find_by_name(r.name);
        expect(b != nullptr, r.name);
        if (b != nullptr) expect_u("find rdna4 id", static_cast<uint32_t>(b->id), static_cast<uint32_t>(r.id));
    }
    for (const Row &r : kRdna3) {
        const FwBlob *b = fw_find_by_name(r.name);
        expect(b != nullptr, r.name);
        if (b != nullptr) {
            expect_u("find rdna3 id", static_cast<uint32_t>(b->id), static_cast<uint32_t>(r.id));
            expect(b->size == 0, "rdna3 find size == 0");
        }
    }
    expect(fw_find_by_name("gc_11_0_1_imu") != fw_find_by_name("gc_12_0_1_imu"),
           "gc_11/gc_12 lookup rows differ");
    expect(fw_find_by_name("no_such_blob") == nullptr, "unknown name -> nullptr");
    expect(fw_find_by_name(nullptr) == nullptr, "null name -> nullptr");

    // R4: out-of-range ids still return nullptr (unknown-id behaviour).
    expect(fw_get(static_cast<FwId>(static_cast<uint32_t>(FwId::Count))) == nullptr,
           "id == Count -> nullptr");
    expect(fw_get(static_cast<FwId>(static_cast<uint32_t>(FwId::Count) + 17)) == nullptr,
           "id > Count -> nullptr");
    expect(fw_get(static_cast<FwId>(0xFFFFFFFFu)) == nullptr, "id == UINT32_MAX -> nullptr");

    if (g_fail != 0) {
        std::printf("FAILED %d/%d checks\n", g_fail, g_run);
        return 1;
    }
    std::printf("OK: %d checks passed\n", g_run);
    return 0;
}
