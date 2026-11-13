// test_golden_values.cpp - T18 host test for the P1 mechanism layer.
//
// Covers:
//   1. every golden register's (offset, _BASE_IDX) and every C1.2 fw_type /
//      command number against the pinned Linux headers (H4);
//   2. the unified resolver (A-01): base[block][base_idx] + offset, including
//      PSP = 0x16000 segment, GC/SDMA segment 1, MMHUB, and NBIO base 0;
//   3. golden_values.h against machine-config.md (segment bases, IP versions,
//      FB window) and INDEX.md (MEC blob size);
//   4. planted break: an unregistered group must be rejected, and a
//      populated-but-wrong group (segment 0) must resolve to a different
//      address, proving the registered group is what keeps us right.
//
// Expectations are read from the evidence files, not from the code under test.

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "amdgpu_ip.h"
#include "golden_values.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         msg);                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static std::string slurp(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// __FILE__ is absolute when run_host_tests.sh invokes us; strip seven parents
// (tests/rdna3/src/navi48-bringup/src/src/780m-hackintosh).
// If the test was compiled through a relative path, walk up from the cwd until
// the evidence tree is visible.
static std::string repo_root() {
    std::string p = __FILE__;
    if (!p.empty() && p[0] == '/') {
        for (int i = 0; i < 7; ++i) {
            const std::string::size_type s = p.find_last_of('/');
            if (s == std::string::npos) return ".";
            p.erase(s);
        }
        return p;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return ".";
    std::string q = cwd;
    for (int i = 0; i < 12; ++i) {
        std::ifstream probe((q + "/evidence/src/INDEX.md").c_str());
        if (probe) return q;
        const std::string::size_type s = q.find_last_of('/');
        if (s == std::string::npos) break;
        q.erase(s);
        if (q.empty()) q = "/";
    }
    return ".";
}

static std::string trim(const std::string &s) {
    const std::string::size_type b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const std::string::size_type e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Parse a single-line "#define <macro> <value>" and return its line number.
static bool find_define(const std::string &text, const std::string &macro,
                        uint32_t *value, int *line) {
    std::istringstream in(text);
    std::string row;
    int ln = 0;
    const std::string key = "#define " + macro;
    while (std::getline(in, row)) {
        ++ln;
        if (row.compare(0, key.size(), key) != 0) continue;
        std::string::size_type p = key.size();
        if (p >= row.size() ||
            !std::isspace(static_cast<unsigned char>(row[p]))) continue;
        while (p < row.size() &&
               std::isspace(static_cast<unsigned char>(row[p]))) ++p;
        const char *start = row.c_str() + p;
        char *end = nullptr;
        const unsigned long v = std::strtoul(start, &end, 0);
        if (end == start) return false;
        *value = static_cast<uint32_t>(v);
        *line = ln;
        return true;
    }
    return false;
}

// Parse "NAME = 0x.." in a C enum (whitespace/newlines tolerated).
static bool find_enum_value(const std::string &text, const std::string &name,
                            uint32_t *value) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string::size_type p = line.find(name);
        if (p == std::string::npos) continue;
        const std::string::size_type e = p + name.size();
        if (e < line.size() &&
            (std::isalnum(static_cast<unsigned char>(line[e])) || line[e] == '_')) {
            continue;
        }
        const std::string::size_type eq = line.find('=', e);
        if (eq == std::string::npos) continue;
        char *end = nullptr;
        const unsigned long v = std::strtoul(line.c_str() + eq + 1, &end, 0);
        if (end == line.c_str() + eq + 1) continue;
        *value = static_cast<uint32_t>(v);
        return true;
    }
    return false;
}

static uint32_t message_value(const char *name) {
    for (const r3gold::GoldenMessage &m : r3gold::kMessages) {
        if (std::strcmp(m.name, name) == 0) return m.value;
    }
    return 0xFFFFFFFFu;
}

static const char *header_for(amdgpu::IPBlock block) {
    switch (block) {
        case amdgpu::IPBlock::MP0:
        case amdgpu::IPBlock::MP1:
            return "mp/mp_13_0_4_offset.h";
        case amdgpu::IPBlock::GC:
        case amdgpu::IPBlock::SDMA0:
            return "gc/gc_11_0_0_offset.h";
        case amdgpu::IPBlock::MMHUB:
            return "mmhub/mmhub_3_0_1_offset.h";
        case amdgpu::IPBlock::OSSSYS:
            return "oss/osssys_6_0_0_offset.h";
        case amdgpu::IPBlock::NBIO:
            return "nbio/nbio_7_7_0_offset.h";
        default:
            return nullptr;
    }
}

static bool segment_base(const amdgpu::IPBaseTable &ip, amdgpu::IPBlock block,
                         uint8_t idx, uint32_t *out) {
    return ip.resolve(amdgpu::RegRef{block, 0, idx}, out);
}

// ---------------------------------------------------------------------------
// 1. Register coordinates against the Linux asic_reg headers.
// ---------------------------------------------------------------------------
static void test_linux_pairing(const std::string &root) {
    const std::string asic =
        root + "/evidence/src/linux-7.2.0/drivers/gpu/drm/amd/include/"
               "asic_reg/";
    int per_block[static_cast<int>(amdgpu::IPBlock::Count)] = {0};
    const std::string gfx_text =
        slurp(root + "/evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/"
                      "gfx_v11_0.c");

    for (const r3gold::GoldenReg *g : r3gold::kRegs) {
        const char *hdr = header_for(g->block);
        CHECK(hdr != nullptr, "golden register has no Linux header mapping");
        if (!hdr) continue;
        const std::string text = slurp(asic + hdr);
        const std::string name = g->name;

        // GC 11.0.1 overrides this register in the Linux driver: the generic
        // gc_11_0_0 header (0x4E82) does not apply to gfx1103. Judge against
        // gfx_v11_0.c, which is still an independent Linux source (H4).
        if (name == "RLC_RLCS_BOOTLOAD_STATUS") {
            uint32_t ov = 0xFFFFFFFFu, ov_idx = 0xFFFFFFFFu;
            int ov_line = 0, ov_idx_line = 0;
            const bool have_ov = find_define(
                gfx_text, "regRLC_RLCS_BOOTLOAD_STATUS_gc_11_0_1",
                &ov, &ov_line);
            const bool have_ov_idx = find_define(
                gfx_text, "regRLC_RLCS_BOOTLOAD_STATUS_gc_11_0_1_BASE_IDX",
                &ov_idx, &ov_idx_line);
            CHECK(have_ov, "IP-version override missing from gfx_v11_0.c");
            CHECK(have_ov_idx, "IP-version override _BASE_IDX missing");
            if (have_ov) {
                CHECK(g->offset == ov,
                      "offset differs from the GC 11.0.1 override");
            }
            if (have_ov_idx) {
                CHECK(g->base_idx == ov_idx,
                      "group differs from the GC 11.0.1 override");
            }
            ++per_block[static_cast<int>(g->block)];
            continue;
        }

        uint32_t off = 0xFFFFFFFFu, idx = 0xFFFFFFFFu;
        int off_line = 0, idx_line = 0;
        const bool have_off =
            find_define(text, "reg" + name, &off, &off_line);
        const bool have_idx =
            find_define(text, "reg" + name + "_BASE_IDX", &idx, &idx_line);
        CHECK(have_off, "register missing from Linux header");
        CHECK(have_idx, "register _BASE_IDX missing from Linux header");
        if (have_off) {
            CHECK(g->offset == off, "offset differs from Linux header");
        }
        if (have_idx) {
            CHECK(g->base_idx == idx, "group (_BASE_IDX) differs from header");
        }
        ++per_block[static_cast<int>(g->block)];
    }

    // Requirement: at least two paired registers per domain.
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::MP0)] >= 2,
          "MP0 sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::MP1)] >= 2,
          "MP1 sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::GC)] >= 2,
          "GC sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::SDMA0)] >= 2,
          "SDMA sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::MMHUB)] >= 2,
          "MMHUB sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::NBIO)] >= 2,
          "NBIO sampling too small");
    CHECK(per_block[static_cast<int>(amdgpu::IPBlock::OSSSYS)] >= 2,
          "OSSSYS sampling too small");

    // D-03 also carries the firmware type and message-number set (C1.2/C0.3).
    const std::string psp_text =
        slurp(root + "/evidence/src/linux-7.2.0/drivers/gpu/drm/amd/amdgpu/"
                      "psp_gfx_if.h");
    CHECK(r3gold::kFwTypeCount == 17,
          "C1.2 registers exactly 17 firmware types");
    for (const r3gold::GoldenFwType &f : r3gold::kFwTypes) {
        uint32_t v = 0xFFFFFFFFu;
        const std::string enum_name = "GFX_FW_TYPE_" + std::string(f.name);
        CHECK(find_enum_value(psp_text, enum_name, &v),
              "fw_type missing from Linux enum");
        CHECK(f.type == v, "fw_type value differs from Linux enum");
    }

    uint32_t msg = 0;
    int line = 0;
    CHECK(find_enum_value(psp_text, "GFX_CMD_ID_LOAD_IP_FW", &msg) &&
              msg == 6u && message_value("pspGfxCmdLoadIpFw") == msg,
          "LOAD_IP_FW command number");
    CHECK(find_enum_value(psp_text, "GFX_CMD_ID_AUTOLOAD_RLC", &msg) &&
              msg == 0x21u && message_value("pspGfxCmdAutoloadRlc") == msg,
          "AUTOLOAD_RLC command number");
    const std::string ppsmc_text =
        slurp(root + "/evidence/src/linux-7.2.0/drivers/gpu/drm/amd/pm/swsmu/"
                      "inc/pmfw_if/smu_v13_0_4_ppsmc.h");
    CHECK(find_define(ppsmc_text, "PPSMC_MSG_EnableGfxImu", &msg, &line) &&
              msg == 0x16u && message_value("smuEnableGfxImu") == msg,
          "EnableGfxImu message number");
    CHECK(message_value("smuBootStep1") == 3u &&
              message_value("smuBootStep2") == 2u,
          "SMU boot sequence 0x3 -> 0x2 (requirement C0.3)");

    std::printf("  Linux pairing: %u registers checked\n", r3gold::kRegCount);
}

// ---------------------------------------------------------------------------
// 2. Resolver behaviour and golden gates.
// ---------------------------------------------------------------------------
static void test_resolver(void) {
    amdgpu::IPBaseTable ip;
    for (const r3gold::GoldenSegment &s : r3gold::kSegments) {
        ip.setBase(s.block, s.base_idx, s.base);
    }

    // Every golden entry resolves through its registered group.
    for (const r3gold::GoldenReg *g : r3gold::kRegs) {
        uint32_t base = 0, addr = 0xFFFFFFFFu;
        CHECK(segment_base(ip, g->block, g->base_idx, &base),
              "golden segment missing from simulated discovery table");
        CHECK(ip.resolve(g->ref(), &addr), "golden register did not resolve");
        CHECK(addr == base + g->offset,
              "resolved address != segment base + offset");
    }

    uint32_t a = 0;
    uint32_t psp1 = 0, gc1 = 0, sdma1 = 0, sdma0 = 0, mmhub1 = 0, nbio0 = 0;
    CHECK(segment_base(ip, amdgpu::IPBlock::MP0, 1, &psp1), "PSP seg missing");
    CHECK(segment_base(ip, amdgpu::IPBlock::GC, 1, &gc1), "GC seg1 missing");
    CHECK(segment_base(ip, amdgpu::IPBlock::SDMA0, 1, &sdma1), "SDMA seg1 missing");
    CHECK(segment_base(ip, amdgpu::IPBlock::SDMA0, 0, &sdma0), "SDMA seg0 missing");
    CHECK(segment_base(ip, amdgpu::IPBlock::MMHUB, 1, &mmhub1), "MMHUB seg missing");
    CHECK(segment_base(ip, amdgpu::IPBlock::NBIO, 0, &nbio0), "NBIO seg0 missing");

    // Expected addresses are taken from the golden table (segment + offset),
    // not from implementation literals (contract ② of design §5.2).
    CHECK(ip.resolve(r3gold::kMp0SmnC2pmsg81.ref(), &a) &&
              a == psp1 + r3gold::kMp0SmnC2pmsg81.offset,
          "PSP C2PMSG_81 = PSP segment + offset");
    CHECK(ip.resolve(r3gold::kCpMecCntl.ref(), &a) &&
              a == gc1 + r3gold::kCpMecCntl.offset,
          "CP_MEC_CNTL = GC segment 1 + offset");
    CHECK(ip.resolve(r3gold::kSdma0F32Cntl.ref(), &a) &&
              a == sdma1 + r3gold::kSdma0F32Cntl.offset,
          "SDMA0 F32_CNTL = SDMA segment 1 + offset");
    CHECK(ip.resolve(r3gold::kSdma0Queue0RbCntl.ref(), &a) &&
              a == sdma0 + r3gold::kSdma0Queue0RbCntl.offset,
          "SDMA0 queue0 RB_CNTL = SDMA segment 0 + offset");
    CHECK(ip.resolve(r3gold::kMmmcVmFbLocationBase.ref(), &a) &&
              a == mmhub1 + r3gold::kMmmcVmFbLocationBase.offset,
          "MMHUB FB_LOCATION_BASE = MMHUB segment 1 + offset");
    CHECK(ip.resolve(r3gold::kBifBx0PcieIndex2.ref(), &a) &&
              a == nbio0 + r3gold::kBifBx0PcieIndex2.offset,
          "NBIO BX0 INDEX2 = NBIO segment 0 (base 0) + offset");

    // Value-gate tiers (D-02): MMHUB FB base/top are static baselines, the
    // SOS sign-of-life is a non-zero gate; everything else is record-only.
    CHECK(r3gold::kMp0SmnC2pmsg81.gate == r3gold::Gate::NonZero,
          "C2PMSG_81 must be a NonZero value gate");
    CHECK(r3gold::kMmmcVmFbLocationBase.gate == r3gold::Gate::Equals,
          "MMHUB FB base must be an Equals value gate");
    CHECK(r3gold::kMmmcVmFbLocationTop.gate == r3gold::Gate::Equals,
          "MMHUB FB top must be an Equals value gate");
    CHECK((r3gold::kMmmcVmFbLocationBase.baseline &
           r3gold::kMmmcVmFbLocationBase.mask) == 0x00008000u,
          "FB base raw baseline must be 0x8000");
    CHECK((r3gold::kMmmcVmFbLocationTop.baseline &
           r3gold::kMmmcVmFbLocationTop.mask) == 0x000080FFu,
          "FB top raw baseline must be 0x80ff");
    CHECK((static_cast<uint64_t>(r3gold::kMmmcVmFbLocationBase.baseline) << 24)
              == 0x8000000000ull,
          "FB base raw << 24 must equal the measured MC base");
    CHECK((static_cast<uint64_t>(r3gold::kMmmcVmFbLocationTop.baseline) << 24)
              == 0x80FF000000ull,
          "FB top field must sit at MC 0x80ff000000");
    CHECK((((static_cast<uint64_t>(r3gold::kMmmcVmFbLocationTop.baseline) << 24) |
            0xFFFFFFull)) == 0x80FFFFFFFFull,
          "FB top raw<<24 | mask must equal the measured MC top");

    CHECK(r3gold::kSmn[0].byte_addr == 0x03B10028u,
          "MP1_FIRMWARE_FLAGS SMN byte address");
    CHECK(r3gold::kFirmware[0].file_size_bytes == 268160u,
          "MEC file size from the blob header");
    CHECK(r3gold::kFirmware[0].ucode_size_bytes == 267904u,
          "MEC ucode_size_bytes from the blob header");
    CHECK(r3gold::kFirmware[0].jt_size_dwords == 224u,
          "MEC jump-table dword count from the blob header");
    CHECK(r3gold::kFirmware[0].payload_bytes == 267008u,
          "MEC payload = ucode_size_bytes - jt_size*4");

    std::printf("  resolver: %u registers, %u segments\n", r3gold::kRegCount,
                r3gold::kSegmentCount);
}
// ---------------------------------------------------------------------------
// 2b. Registry integrity: metadata, duplicate coordinates, A-04 name drift.
// ---------------------------------------------------------------------------
static void test_registry_integrity(void) {
    unsigned bad_seg = 0, bad_meta = 0, dup_name = 0, dup_coord = 0;
    for (uint32_t i = 0; i < r3gold::kRegCount; ++i) {
        const r3gold::GoldenReg &a = *r3gold::kRegs[i];
        if (a.base_idx >= amdgpu::IPBaseTable::kMaxBaseSegments) ++bad_seg;
        if (!a.name || !a.name[0] || !a.source || !a.source[0]) ++bad_meta;
        for (uint32_t j = i + 1; j < r3gold::kRegCount; ++j) {
            const r3gold::GoldenReg &b = *r3gold::kRegs[j];
            if (std::strcmp(a.name, b.name) == 0) ++dup_name;
            if (a.block == b.block && a.offset == b.offset &&
                a.base_idx == b.base_idx)
                ++dup_coord;
        }
    }
    CHECK(bad_seg == 0, "every golden base_idx fits the resolver");
    CHECK(bad_meta == 0, "every golden register carries name and source");
    CHECK(dup_name == 0, "golden register names are unique");
    CHECK(dup_coord == 0, "golden register coordinates are unique");

    unsigned dup_seg = 0, sentinel = 0;
    for (uint32_t i = 0; i < r3gold::kSegmentCount; ++i) {
        const r3gold::GoldenSegment &a = r3gold::kSegments[i];
        if (a.base == 0xFFFFFFFFu) ++sentinel;
        if (a.base_idx >= amdgpu::IPBaseTable::kMaxBaseSegments) ++bad_seg;
        for (uint32_t j = i + 1; j < r3gold::kSegmentCount; ++j) {
            if (a.block == r3gold::kSegments[j].block &&
                a.base_idx == r3gold::kSegments[j].base_idx)
                ++dup_seg;
        }
    }
    CHECK(sentinel == 0, "no discovered segment is the 0xFFFFFFFF sentinel");
    CHECK(dup_seg == 0, "segment (block, group) pairs are unique");

    unsigned dup_fw = 0, dup_msg = 0;
    for (uint32_t i = 0; i < r3gold::kFwTypeCount; ++i) {
        for (uint32_t j = i + 1; j < r3gold::kFwTypeCount; ++j) {
            if (std::strcmp(r3gold::kFwTypes[i].name,
                            r3gold::kFwTypes[j].name) == 0 ||
                r3gold::kFwTypes[i].type == r3gold::kFwTypes[j].type)
                ++dup_fw;
        }
    }
    for (uint32_t i = 0; i < r3gold::kMessageCount; ++i) {
        for (uint32_t j = i + 1; j < r3gold::kMessageCount; ++j) {
            if (std::strcmp(r3gold::kMessages[i].name,
                            r3gold::kMessages[j].name) == 0)
                ++dup_msg;
        }
    }
    CHECK(dup_fw == 0, "fw_type names and values are unique");
    CHECK(dup_msg == 0, "message names are unique");

    // A-04: the renamed BX0 constants and the deprecated BX1 spellings must
    // agree with the golden table; drift would send P4b back to 0x800e.
    CHECK(amdgpu::NBIORegs::BIF_BX0_PCIE_INDEX2 ==
              r3gold::kBifBx0PcieIndex2.offset,
          "NBIORegs BX0 INDEX2 agrees with the golden table");
    CHECK(amdgpu::NBIORegs::BIF_BX0_PCIE_DATA2 ==
              r3gold::kBifBx0PcieData2.offset,
          "NBIORegs BX0 DATA2 agrees with the golden table");
    CHECK(amdgpu::NBIORegs::BIF_BX1_PCIE_INDEX2 ==
              amdgpu::NBIORegs::BIF_BX0_PCIE_INDEX2,
          "deprecated BX1 INDEX2 alias is the BX0 value");
    CHECK(amdgpu::NBIORegs::BIF_BX1_PCIE_DATA2 ==
              amdgpu::NBIORegs::BIF_BX0_PCIE_DATA2,
          "deprecated BX1 DATA2 alias is the BX0 value");

    const r3gold::GoldenFirmware &fw = r3gold::kFirmware[0];
    CHECK(static_cast<uint64_t>(fw.payload_bytes) +
              static_cast<uint64_t>(fw.jt_size_dwords) * 4u ==
              fw.ucode_size_bytes,
          "MEC payload + jump table covers the ucode size");
}
// ---------------------------------------------------------------------------
// 3. Planted break: unregistered group and wrong-group registration.
// ---------------------------------------------------------------------------
static void test_planted_break(void) {
    amdgpu::IPBaseTable ip;
    for (const r3gold::GoldenSegment &s : r3gold::kSegments) {
        ip.setBase(s.block, s.base_idx, s.base);
    }

    uint32_t psp_base = 0;
    CHECK(segment_base(ip, amdgpu::IPBlock::MP0, 1, &psp_base),
          "PSP segment missing");
    const uint32_t expected = psp_base + r3gold::kMp0SmnC2pmsg81.offset;

    uint32_t addr = 0xDEADBEEFu;
    amdgpu::RegRef good = r3gold::kMp0SmnC2pmsg81.ref();
    CHECK(ip.resolve(good, &addr) && addr == expected,
          "control: correct group resolves");

    // (a) The discovery walk reports only MP0 segments 0 and 1; segment 2 is a
    // hole (0xFFFFFFFF sentinel). A group outside the discovered set is
    // rejected by the entrance instead of resolving into the hole.
    amdgpu::RegRef wrong_group = good;
    wrong_group.base_idx = 2;
    addr = 0xDEADBEEFu;
    CHECK(!ip.resolve(wrong_group, &addr),
          "unregistered group must be rejected");
    CHECK(addr == 0xDEADBEEFu, "rejected resolve must not write the output");

    amdgpu::RegRef too_far = good;
    too_far.base_idx = amdgpu::IPBaseTable::kMaxBaseSegments;
    CHECK(!ip.resolve(too_far, &addr),
          "segment outside kMaxBaseSegments must be rejected");

    // (b) A populated-but-wrong group is arithmetically indistinguishable from
    // a correct one, so the entrance cannot reject it by inspecting the value:
    // the design's guarantee is that the group is registered data paired with
    // the Linux header (D-03), never guessed by the caller. Plant that break:
    // group 0 would resolve to a different address and must not be accepted as
    // the C2PMSG_81 coordinate.
    amdgpu::RegRef wrong_but_valid = good;
    wrong_but_valid.base_idx = 0;
    uint32_t wrong_addr = 0;
    CHECK(ip.resolve(wrong_but_valid, &wrong_addr),
          "populated group resolves (entrance follows the registered group)");
    CHECK(wrong_addr == ip.get(amdgpu::IPBlock::MP0) +
                             r3gold::kMp0SmnC2pmsg81.offset,
          "wrong group yields the segment-0 address");
    CHECK(wrong_addr != expected,
          "wrong-group address differs from the registered golden address");
    CHECK(r3gold::kMp0SmnC2pmsg81.base_idx == 1,
          "golden registration pins C2PMSG_81 to group 1");
    CHECK(ip.get(amdgpu::IPBlock::MP0) + r3gold::kMp0SmnC2pmsg81.offset ==
              wrong_addr,
          "legacy get(block) is the wrong-group trap");
}

// ---------------------------------------------------------------------------
// 4. Golden values vs machine-config.md / INDEX.md.
// ---------------------------------------------------------------------------
struct Row {
    std::string f2;
    std::string f3;
    bool found = false;
};

static Row find_md_row(const std::string &text, const std::string &label) {
    Row row;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        const std::vector<std::string> f = split(line, '|');
        if (f.size() < 4) continue;
        if (trim(f[1]) != label) continue;
        row.f2 = trim(f[2]);
        row.f3 = trim(f[3]);
        row.found = true;
        return row;
    }
    return row;
}

static uint32_t seg_of(amdgpu::IPBlock block, uint8_t idx) {
    for (const r3gold::GoldenSegment &s : r3gold::kSegments) {
        if (s.block == block && s.base_idx == idx) return s.base;
    }
    return 0xFFFFFFFFu;
}

static uint64_t scalar_of(const char *name) {
    for (const r3gold::GoldenScalar &s : r3gold::kScalars) {
        if (std::strcmp(s.name, name) == 0) return s.value;
    }
    return 0xFFFFFFFFFFFFFFFFull;
}

static void test_machine_config(const std::string &root) {
    const std::string text =
        slurp(root + "/evidence/target-machine/machine-config.md");

    struct Want {
        const char *label;
        amdgpu::IPBlock block;
        unsigned maj, min, rev;
    };
    const Want wants[] = {
        {"GC", amdgpu::IPBlock::GC, 11, 0, 1},
        {"PSP (MP0)", amdgpu::IPBlock::MP0, 13, 0, 4},
        {"SMU (MP1)", amdgpu::IPBlock::MP1, 13, 0, 4},
        {"SDMA0", amdgpu::IPBlock::SDMA0, 6, 0, 1},
        {"NBIO", amdgpu::IPBlock::NBIO, 7, 7, 0},
    };
    for (const Want &w : wants) {
        const Row row = find_md_row(text, w.label);
        CHECK(row.found, "machine-config row missing");
        if (!row.found) continue;
        unsigned b0 = 0, b1 = 0;
        CHECK(std::sscanf(row.f3.c_str(), "0x%x / 0x%x", &b0, &b1) == 2,
              "machine-config segment columns unparsable");
        CHECK(seg_of(w.block, 0) == b0, "golden seg0 differs from machine-config");
        CHECK(seg_of(w.block, 1) == b1, "golden seg1 differs from machine-config");

        unsigned maj = 0, min = 0, rev = 0;
        CHECK(std::sscanf(row.f2.c_str(), "%u.%u.%u", &maj, &min, &rev) == 3,
              "machine-config version unparsable");
        bool matched = false;
        for (const r3gold::GoldenIpVersion &v : r3gold::kIpVersions) {
            if (v.block == w.block) {
                matched = v.major == maj && v.minor == min && v.rev == rev;
            }
        }
        CHECK(matched, "golden IP version differs from machine-config");
    }

    // MMHUB is in the "remaining 31 IPs" sentence, not the table; the nullarc
    // 780M dump and the E13 cross-check give the same 3.0.1 / 0x13200 / 0x1a000.
    CHECK(seg_of(amdgpu::IPBlock::MMHUB, 0) == 0x00013200u,
          "MMHUB seg0 baseline");
    CHECK(seg_of(amdgpu::IPBlock::MMHUB, 1) == 0x0001A000u,
          "MMHUB seg1 baseline");

    const std::string::size_type fb = text.find("fb_mc_base=0x");
    CHECK(fb != std::string::npos, "machine-config FB baseline missing");
    if (fb != std::string::npos) {
        const unsigned long long base =
            std::strtoull(text.c_str() + fb + 13, nullptr, 16);
        const std::string::size_type tp = text.find("fb_mc_top=0x", fb);
        CHECK(tp != std::string::npos, "machine-config FB top missing");
        const unsigned long long top =
            tp == std::string::npos ? 0 : std::strtoull(text.c_str() + tp + 12,
                                                        nullptr, 16);
        CHECK(scalar_of("mmhubFbMcBase") == base,
              "golden FB MC base differs from machine-config");
        CHECK(scalar_of("mmhubFbMcTop") == top,
              "golden FB MC top differs from machine-config");
    }

    const std::string index = slurp(root + "/evidence/src/INDEX.md");
    std::istringstream in(index);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.find("gc_11_0_1_mec.bin") == std::string::npos) continue;
        const std::vector<std::string> f = split(line, '|');
        if (f.size() < 3) continue;
        const unsigned long size = std::strtoul(trim(f[2]).c_str(), nullptr, 10);
        CHECK(size == r3gold::kFirmware[0].file_size_bytes,
              "MEC size in INDEX.md differs from golden table");
        found = true;
        break;
    }
    CHECK(found, "MEC row missing from INDEX.md");

    std::printf("  machine-config/INDEX: bases, versions, FB window, MEC size OK\n");
}

static void test_version_observations()
{
    CHECK(r3gold::kRlcF32UcodeVersion.offset == 0x4c03u, "RLC_F32 offset");
    CHECK(r3gold::kRlcF32UcodeVersion.base_idx == 1u, "RLC_F32 base idx");
    CHECK(r3gold::kRlcF32UcodeVersion.gate == r3gold::Gate::RecordOnly,
          "RLC_F32 is a record, not a gate");
    CHECK(r3gold::kSdma0Version.offset == 0x0035u, "SDMA0 version offset");
    CHECK(r3gold::kSdma0Version.base_idx == 0u, "SDMA0 version base idx");
    CHECK(r3gold::kSdma0Version.gate == r3gold::Gate::RecordOnly,
          "SDMA0 version is a record, not a gate");
}

int main() {
    const std::string root = repo_root();
    std::printf("golden values (P1/T18), repo root %s\n", root.c_str());

    std::printf("[1] Linux header pairing\n");
    test_linux_pairing(root);
    std::printf("[2] resolver + gates\n");
    test_resolver();
    std::printf("[2b] registry integrity\n");
    test_registry_integrity();
    std::printf("[2c] version observations\n");
    test_version_observations();
    std::printf("[3] planted group mismatch\n");
    test_planted_break();
    std::printf("[4] machine-config / INDEX consistency\n");
    test_machine_config(root);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("test_golden_values: all checks passed\n");
    return 0;
}
