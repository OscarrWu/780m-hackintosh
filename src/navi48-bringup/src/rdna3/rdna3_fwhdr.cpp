//
//  rdna3_fwhdr.cpp — RDNA3 firmware header parsing (stage-2 tech plan §7.3).
//
//  Pure functions: no I/O, no allocation, no STL, no libc calls (the host
//  build and the kext build compile the same source).  Multi-byte fields are
//  read byte-by-byte as little-endian, so no unaligned load is ever formed.
//
//  Field offsets come from the Linux struct definitions
//  (amdgpu_ucode.h:30-41, 181-186, 201-213, 226-286, 328-336, 432-438) and
//  the size semantics from their users (amdgpu_ucode.c PSP branch,
//  amdgpu_rlc.c v2_0..v2_3).  The two facts that matter and that a table of
//  numbers gets wrong (R1 §3.6):
//    * a segment's payload length is its own size field, or for a plain
//      common header "declared file length − array offset" — never
//      common.ucode_size_bytes, which on a container describes RLC_G;
//    * offsets are absolute byte positions inside the blob (Linux adds them
//      to the blob base, not to the array offset).
//
//
//  Origin: header field layouts and segment-size semantics are taken from
//          upstream Linux drivers/gpu/drm/amd/amdgpu/{amdgpu_ucode.h,
//          amdgpu_ucode.c, amdgpu_rlc.c, amdgpu_psp.c, psp_gfx_if.h}
//          (Linux v7.2 archive, evidence/src/linux-7.2.0).
//          No Linux source is copied verbatim; only the field offsets and
//          the meaning of each size field are reproduced.
//
//  Copyright 2012-2026 Advanced Micro Devices, Inc.
//  Copyright 2014 Advanced Micro Devices, Inc. (amdgpu_rlc.c parts).
//  MIT licensed — see NOTICE.
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
//  THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
//  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
//  OTHER DEALINGS IN THE SOFTWARE.
//
#include "rdna3_fwhdr.h"

namespace {

// Little-endian field readers (-fno-builtin, no memcpy: each byte is
// loaded individually, so no alignment assumption is made).
inline uint16_t rd16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// common_firmware_header is 32 bytes: size_bytes, header_size_bytes,
// version major/minor, ip major/minor, ucode_version, ucode_size_bytes,
// ucode_array_offset_bytes, crc32.
constexpr uint32_t kCommonHdrSize = 32;
// Smallest full header per class: common + the class' own extension.
constexpr uint32_t kSdmaV20HdrSize = 64;   // common + 8 u32
constexpr uint32_t kImuV10HdrSize  = 48;   // common + 4 u32
constexpr uint32_t kMesV10HdrSize  = 72;   // common + 10 u32
constexpr uint32_t kRlcV20HdrSize  = 104;  // common + 18 u32
constexpr uint32_t kRlcV21HdrSize  = 156;  // v2_0 + 13 u32
constexpr uint32_t kRlcV22HdrSize  = 172;  // v2_1 + 4 u32
constexpr uint32_t kRlcV23HdrSize  = 204;  // v2_2 + 8 u32

// Sanity floor for ucode_array_offset_bytes is header_size_bytes (checked
// below); 0x100 is what every archived blob uses but is NOT enforced —
// Linux reads the field and trusts it (amdgpu_ucode.c:894-1005), so pinning
// the value here would reject a legitimately repacked firmware for no gain.

// Zero the descriptor so a failed parse can never hand a caller a stale
// segment list from a previous use.
void reset_image(R3FwImage *out) {
    out->header_version_major = 0;
    out->header_version_minor = 0;
    out->ip_version_major     = 0;
    out->ip_version_minor     = 0;
    out->ucode_version        = 0;
    out->ucode_size_bytes     = 0;
    out->array_offset         = 0;
    out->declared_size_bytes  = 0;
    out->declared_header_bytes = 0;
    out->seg_count            = 0;
    for (uint32_t i = 0; i < R3_FW_MAX_SEGS; ++i) {
        out->segs[i].fw_type = 0;
        out->segs[i].offset  = 0;
        out->segs[i].size    = 0;
        out->segs[i].name    = nullptr;
    }
    out->mes_uc_start_addr   = 0;
    out->mes_data_start_addr = 0;
}

template <typename T>
inline T min_of(T a, T b) { return a < b ? a : b; }

// Common-header validation shared by every class.  On success the common
// fields of `out` are filled; the caller adds its own extension.
int parse_common_hdr(const uint8_t *data, size_t len, uint64_t stage_mc,
                     R3FwImage *out) {
    if (data == nullptr || out == nullptr) return R3_FW_HDR_ERR_NULL;
    reset_image(out);
    if ((stage_mc & (R3_FW_STAGE_ALIGN - 1ull)) != 0)
        return R3_FW_HDR_ERR_ALIGN;
    if (len < kCommonHdrSize) return R3_FW_HDR_ERR_SHORT;

    const uint32_t size_bytes  = rd32(data + 0);
    // header_size_bytes (common header field, offset +4) is the size of the
    // class' full header.  Each class re-checks the exact value it needs; a
    // value too small even for the common header is corruption.
    const uint32_t hdr_bytes = rd32(data + 4);
    const uint32_t array_off = rd32(data + 24);

    // The declared total must fit the buffer we hold.
    if (size_bytes > len) return R3_FW_HDR_ERR_RANGE;
    if (hdr_bytes < kCommonHdrSize || hdr_bytes > size_bytes)
        return R3_FW_HDR_ERR_RANGE;
    if (array_off < hdr_bytes || array_off > size_bytes)
        return R3_FW_HDR_ERR_RANGE;
    // The array offset itself is trusted once it is inside the declared
    // length (checked above) — same as Linux.  Alignment is a property of
    // the STAGING ADDRESS (stage_mc + offset, psp_gfx_if.h:317), not of the
    // offset inside the blob: sub-blobs sit at arbitrary offsets (R1 §3.6 —
    // 25344, 148336, …), so an offset-alignment rule here would reject
    // every real file.

    out->header_version_major = rd16(data + 8);
    out->header_version_minor = rd16(data + 10);
    out->ip_version_major     = rd16(data + 12);
    out->ip_version_minor     = rd16(data + 14);
    out->ucode_version        = rd32(data + 16);
    out->ucode_size_bytes     = rd32(data + 20);
    out->array_offset         = array_off;
    out->declared_size_bytes  = size_bytes;
    out->declared_header_bytes = hdr_bytes;
    return R3_FW_HDR_OK;
}

// The blob must declare a header at least as large as this class' full
// header struct: those fields are read directly, so a smaller declared header
// means reading past what the file says it contains (the buffer may be longer
// by luck -- bind it anyway).  header_size_bytes is a field of the common
// header (amdgpu_ucode.h:32), independent of ucode_size_bytes.
int require_header(const R3FwImage *img, uint32_t need) {
    if (img->declared_header_bytes < need) return R3_FW_HDR_ERR_SHORT;
    return R3_FW_HDR_OK;
}

// Append one segment; ERR_CAP instead of writing past the array.
int add_seg(R3FwImage *out, uint32_t fw_type, uint32_t offset, uint32_t size,
            const char *name) {
    if (out->seg_count >= R3_FW_MAX_SEGS) return R3_FW_HDR_ERR_CAP;
    R3FwSeg &s = out->segs[out->seg_count];
    s.fw_type = fw_type;
    s.offset  = offset;
    s.size    = size;
    s.name    = name;
    ++out->seg_count;
    return R3_FW_HDR_OK;
}

// A required sub-blob: size must be non-zero and offset+size inside the
// declared file (which parse_common_hdr already proved is inside `len`).
int add_required(R3FwImage *out, uint32_t file_size, uint32_t fw_type,
                 uint32_t offset, uint32_t size, const char *name) {
    if (size == 0) return R3_FW_HDR_ERR_ZERO_SIZE;
    if (offset > file_size || size > file_size - offset)
        return R3_FW_HDR_ERR_RANGE;
    return add_seg(out, fw_type, offset, size, name);
}

// An optional sub-blob: absent (size 0) means "not in the load list"
// (amdgpu_rlc.c guards each registration with `if (..._size_bytes)`).
int add_optional(R3FwImage *out, uint32_t file_size, uint32_t fw_type,
                 uint32_t offset, uint32_t size, const char *name) {
    if (size == 0) return R3_FW_HDR_OK;
    return add_required(out, file_size, fw_type, offset, size, name);
}

} // namespace

int r3_fw_parse_common(const uint8_t *data, size_t len, uint64_t stage_mc,
                       R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    // Payload = ucode_size_bytes from the array offset (pfp/me/toc/ta).
    // MEC is the exception: its jump table is appended inside ucode_size_bytes
    // and is not part of the PSP payload, so r3_fw_parse_mec subtracts it
    // (amdgpu_ucode.c:906-911, C1.2).
    return add_required(out, out->declared_size_bytes, 0,
                        out->array_offset, out->ucode_size_bytes, "payload");
}

// CP firmware header (gfx_firmware_header_v1_0, amdgpu_ucode.h:180-186):
// common + ucode_feature_version (+32), jt_offset (+36), jt_size (+40).
// Linux ships ucode_size_bytes - jt_size * 4 for CP_MEC1/2 because the jump
// table is appended after the code and is not PSP payload (amdgpu_ucode.c:
// 906-911). The archived gc_11_0_1_mec.bin is exactly that case: 267904 -
// 224*4 = 267008 (golden_values.h kFirmware cross-check).
static int parse_cp_mec(const uint8_t *data, size_t len, uint64_t stage_mc,
                        R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    if (require_header(out, 44) != R3_FW_HDR_OK) return R3_FW_HDR_ERR_SHORT;
    const uint32_t jt_size_dwords = rd32(data + 40);
    const uint64_t jt_bytes = static_cast<uint64_t>(jt_size_dwords) * 4u;
    if (jt_bytes > out->ucode_size_bytes) return R3_FW_HDR_ERR_RANGE;
    const uint32_t payload =
        out->ucode_size_bytes - static_cast<uint32_t>(jt_bytes);
    return add_required(out, out->declared_size_bytes, 0, out->array_offset,
                        payload, "payload");
}

int r3_fw_parse_sdma(const uint8_t *data, size_t len, uint64_t stage_mc,
                     R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    if (out->header_version_major != 2 || out->header_version_minor != 0)
        return R3_FW_HDR_ERR_VERSION;
    if (require_header(out, kSdmaV20HdrSize) != R3_FW_HDR_OK)
        return R3_FW_HDR_ERR_SHORT;

    // sdma_firmware_header_v2_0 (amdgpu_ucode.h:328-336):
    //   +32 ucode_feature_version, +36 ctx_ucode_size_bytes,
    //   +40 ctx_jt_offset, +44 ctx_jt_size, +48 ctl_ucode_offset,
    //   +52 ctl_ucode_size_bytes, +56 ctl_jt_offset, +60 ctl_jt_size.
    const uint32_t ctx_size = rd32(data + 36);
    const uint32_t ctl_off  = rd32(data + 48);
    const uint32_t ctl_size = rd32(data + 52);

    // Context thread payload starts at the array offset (amdgpu_ucode.c:891-895).
    rc = add_required(out, out->declared_size_bytes, 71, out->array_offset,
                      ctx_size, "sdma_th0");
    if (rc != R3_FW_HDR_OK) return rc;
    return add_required(out, out->declared_size_bytes, 72, ctl_off,
                        ctl_size, "sdma_th1");
}

int r3_fw_parse_imu(const uint8_t *data, size_t len, uint64_t stage_mc,
                    R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    if (out->header_version_major != 1 || out->header_version_minor != 0)
        return R3_FW_HDR_ERR_VERSION;
    if (require_header(out, kImuV10HdrSize) != R3_FW_HDR_OK)
        return R3_FW_HDR_ERR_SHORT;

    // imu_firmware_header_v1_0 (amdgpu_ucode.h:432-438):
    //   +32 iram_ucode_size_bytes, +36 iram_ucode_offset_bytes,
    //   +40 dram_ucode_size_bytes, +44 dram_ucode_offset_bytes.
    // Neither *_offset_bytes field is used for the staging address: Linux
    // puts IMU_I at the array offset and IMU_D right after IMU_I
    // (amdgpu_ucode.c:1026-1031), and in the archived blob both fields are 0
    // and 0x10200 — the sizes, not the offsets, place the payloads.
    const uint32_t iram_size = rd32(data + 32);
    const uint32_t dram_size = rd32(data + 40);

    rc = add_required(out, out->declared_size_bytes, 68, out->array_offset,
                      iram_size, "imu_i");
    if (rc != R3_FW_HDR_OK) return rc;
    // IMU_D starts right after IMU_I.  IMU_I has just been range-checked
    // (array_offset + iram_size <= declared_size_bytes), so this sum cannot
    // wrap — but do it in 64-bit anyway, so a later reordering of the two
    // checks cannot silently turn a wrap into a fabricated in-bounds offset.
    const uint64_t dram_off64 = static_cast<uint64_t>(out->array_offset)
                              + static_cast<uint64_t>(iram_size);
    if (dram_off64 > 0xFFFFFFFFull) return R3_FW_HDR_ERR_RANGE;
    return add_required(out, out->declared_size_bytes, 69,
                        static_cast<uint32_t>(dram_off64), dram_size, "imu_d");
}

int r3_fw_parse_rlc(const uint8_t *data, size_t len, uint64_t stage_mc,
                    R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;

    // Whitelist v2.0–v2.3.  v2.4/v2.5 carry tap-delay sub-blobs this parser
    // does not model; anything else is a packager change we have not
    // reviewed.  Header drift fails closed, never silently (V1).
    const uint32_t vmin = out->header_version_minor;
    if (out->header_version_major != 2 || vmin > 3)
        return R3_FW_HDR_ERR_VERSION;

    uint32_t need = kRlcV20HdrSize;
    if (vmin == 1) need = kRlcV21HdrSize;
    else if (vmin == 2) need = kRlcV22HdrSize;
    else if (vmin == 3) need = kRlcV23HdrSize;
    if (require_header(out, need) != R3_FW_HDR_OK)
        return R3_FW_HDR_ERR_SHORT;
    if (out->ucode_size_bytes < need) return R3_FW_HDR_ERR_SHORT;

    // Emission order follows the 17-piece load order (D2), not the header's
    // field order: 20, 21, 26, 48, 25, [22], [7], 8.
    if (vmin >= 1) {
        // rlc_firmware_header_v2_1 (amdgpu_ucode.h:249-264):
        //   +116 cntl_size, +120 cntl_off,
        //   +132 gpm_size,  +136 gpm_off,
        //   +148 srm_size,  +152 srm_off.
        const uint32_t gpm_size = rd32(data + 132);
        const uint32_t gpm_off  = rd32(data + 136);
        rc = add_required(out, out->declared_size_bytes, 20, gpm_off, gpm_size,
                         "rlc_gpm");
        if (rc != R3_FW_HDR_OK) return rc;

        const uint32_t srm_size = rd32(data + 148);
        const uint32_t srm_off  = rd32(data + 152);
        rc = add_required(out, out->declared_size_bytes, 21, srm_off, srm_size,
                         "rlc_srm");
        if (rc != R3_FW_HDR_OK) return rc;
    }

    if (vmin >= 2) {
        // rlc_firmware_header_v2_2 (amdgpu_ucode.h:267-273):
        //   +156 iram_size, +160 iram_off, +164 dram_size, +168 dram_off.
        const uint32_t iram_size = rd32(data + 156);
        const uint32_t iram_off  = rd32(data + 160);
        rc = add_required(out, out->declared_size_bytes, 26, iram_off, iram_size,
                         "rlc_iram");
        if (rc != R3_FW_HDR_OK) return rc;

        const uint32_t dram_size = rd32(data + 164);
        const uint32_t dram_off  = rd32(data + 168);
        rc = add_required(out, out->declared_size_bytes, 48, dram_off, dram_size,
                         "rlc_dram");
        if (rc != R3_FW_HDR_OK) return rc;
    }

    if (vmin >= 3) {
        // rlc_firmware_header_v2_3 (amdgpu_ucode.h:276-286):
        //   +180 rlcp_size, +184 rlcp_off, +196 rlcv_size, +200 rlcv_off.
        const uint32_t rlcp_size = rd32(data + 180);
        const uint32_t rlcp_off  = rd32(data + 184);
        rc = add_required(out, out->declared_size_bytes, 25, rlcp_off, rlcp_size,
                         "rlc_p");
        if (rc != R3_FW_HDR_OK) return rc;

        const uint32_t cntl_size = rd32(data + 116);
        const uint32_t cntl_off  = rd32(data + 120);
        rc = add_optional(out, out->declared_size_bytes, 22, cntl_off,
                         cntl_size, "rlc_cntl");
        if (rc != R3_FW_HDR_OK) return rc;

        const uint32_t rlcv_size = rd32(data + 196);
        const uint32_t rlcv_off  = rd32(data + 200);
        rc = add_optional(out, out->declared_size_bytes, 7, rlcv_off,
                         rlcv_size, "rlc_v");
        if (rc != R3_FW_HDR_OK) return rc;
    }

    // RLC_G: ucode_size_bytes at the array offset.  This is the one place
    // where ucode_size_bytes IS a payload length (amdgpu_rlc.c:351-357);
    // for every other segment it is the RLC_G length, not theirs.
    return add_required(out, out->declared_size_bytes, 8, out->array_offset,
                        out->ucode_size_bytes, "rlc_g");
}

int r3_fw_parse_mes(const uint8_t *data, size_t len, uint64_t stage_mc,
                    R3FwImage *out) {
    int rc = parse_common_hdr(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    if (out->header_version_major != 1 || out->header_version_minor != 0)
        return R3_FW_HDR_ERR_VERSION;
    if (require_header(out, kMesV10HdrSize) != R3_FW_HDR_OK)
        return R3_FW_HDR_ERR_SHORT;

    // mes_firmware_header_v1_0 (amdgpu_ucode.h:201-213):
    //   +32 version, +36 code_size, +40 code_off, +44 data_version,
    //   +48 data_size, +52 data_off, +56..+68 the four start-addr halves.
    const uint32_t code_size = rd32(data + 36);
    const uint32_t code_off  = rd32(data + 40);
    const uint32_t data_size = rd32(data + 48);
    const uint32_t data_off  = rd32(data + 52);

    rc = add_required(out, out->declared_size_bytes, 0, code_off, code_size,
                      "mes_code");
    if (rc != R3_FW_HDR_OK) return rc;
    rc = add_required(out, out->declared_size_bytes, 0, data_off, data_size,
                      "mes_data");
    if (rc != R3_FW_HDR_OK) return rc;

    out->mes_uc_start_addr =
        (static_cast<uint64_t>(rd32(data + 60)) << 32) | rd32(data + 56);
    out->mes_data_start_addr =
        (static_cast<uint64_t>(rd32(data + 68)) << 32) | rd32(data + 64);
    return R3_FW_HDR_OK;
}

int r3_fw_parse_toc(const uint8_t *data, size_t len, uint64_t stage_mc,
                    R3FwImage *out) {
    // The TOC uses the plain common header; only the fw_type stamp differs,
    // and 24 marks bytes that are NOT a LOAD_IP_FW payload so a caller
    // cannot hand them to the load order by accident.
    const int rc = r3_fw_parse_common(data, len, stage_mc, out);
    if (rc != R3_FW_HDR_OK) return rc;
    out->segs[0].fw_type = 24;
    out->segs[0].name    = "toc";
    return R3_FW_HDR_OK;
}

int r3_fw_parse_id(amdgpu::FwId id, const uint8_t *data, size_t len,
                   uint64_t stage_mc, R3FwImage *out) {
    if (out == nullptr) return R3_FW_HDR_ERR_NULL;

    int rc;
    switch (id) {
    case amdgpu::FwId::SDMA6:
        return r3_fw_parse_sdma(data, len, stage_mc, out);
    case amdgpu::FwId::GC11_IMU:
        return r3_fw_parse_imu(data, len, stage_mc, out);
    case amdgpu::FwId::GC11_RLC:
        return r3_fw_parse_rlc(data, len, stage_mc, out);
    case amdgpu::FwId::GC11_MES_2:
    case amdgpu::FwId::GC11_MES1:
        rc = r3_fw_parse_mes(data, len, stage_mc, out);
        if (rc != R3_FW_HDR_OK) return rc;
        // pipe0 (mes_2.bin) → 33/34, pipe1 KIQ (mes1.bin) → 81/82
        // (amdgpu_mes.c:753-780).
        out->segs[0].fw_type = (id == amdgpu::FwId::GC11_MES1) ? 81u : 33u;
        out->segs[1].fw_type = (id == amdgpu::FwId::GC11_MES1) ? 82u : 34u;
        return R3_FW_HDR_OK;
    case amdgpu::FwId::GC11_PFP:
    case amdgpu::FwId::GC11_ME:
    case amdgpu::FwId::PSP13_TOC:
    case amdgpu::FwId::PSP13_TA:
        rc = r3_fw_parse_common(data, len, stage_mc, out);
        if (rc != R3_FW_HDR_OK) return rc;
        switch (id) {
        case amdgpu::FwId::GC11_PFP:  out->segs[0].fw_type = 2;  break;
        case amdgpu::FwId::GC11_ME:   out->segs[0].fw_type = 1;  break;
        case amdgpu::FwId::PSP13_TOC: out->segs[0].fw_type = 24; break;
        default:                      out->segs[0].fw_type = 75; break;
        }
        return R3_FW_HDR_OK;
    case amdgpu::FwId::GC11_MEC:
        rc = parse_cp_mec(data, len, stage_mc, out);
        if (rc != R3_FW_HDR_OK) return rc;
        out->segs[0].fw_type = 4;
        return R3_FW_HDR_OK;
    default:
        // Not a blob this parser serves (zero-length fallback rows, and the
        // ids reserved for later stages).  Callers fail closed on this.
        return R3_FW_HDR_ERR_VERSION;
    }
}

uint64_t r3_fw_stage_addr(uint64_t stage_mc, const R3FwImage *img,
                          uint32_t index) {
    if (img == nullptr) return stage_mc;
    // Sequential 4 KiB-rounded packing, the same rule Linux uses for its
    // per-ucode fw_offset (amdgpu_ucode.c:1203-1214).
    uint64_t off = 0;
    const uint32_t n = min_of(img->seg_count,
                              static_cast<uint32_t>(R3_FW_MAX_SEGS));
    for (uint32_t i = 0; i < n && i < index; ++i) {
        off += (static_cast<uint64_t>(img->segs[i].size) +
                (R3_FW_STAGE_ALIGN - 1ull)) & ~(R3_FW_STAGE_ALIGN - 1ull);
    }
    return stage_mc + off;
}

uint64_t r3_fw_stage_size(const R3FwImage *img) {
    if (img == nullptr) return 0;
    const uint32_t n = min_of(img->seg_count,
                              static_cast<uint32_t>(R3_FW_MAX_SEGS));
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        total += (static_cast<uint64_t>(img->segs[i].size) +
                  (R3_FW_STAGE_ALIGN - 1ull)) & ~(R3_FW_STAGE_ALIGN - 1ull);
    }
    return total;
}

const R3FwSeg *r3_fw_find_seg(const R3FwImage *img, uint32_t fw_type) {
    if (img == nullptr) return nullptr;
    const uint32_t n = min_of(img->seg_count,
                              static_cast<uint32_t>(R3_FW_MAX_SEGS));
    for (uint32_t i = 0; i < n; ++i) {
        if (img->segs[i].fw_type == fw_type) return &img->segs[i];
    }
    return nullptr;
}
