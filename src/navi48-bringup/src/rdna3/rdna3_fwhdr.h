//
//  rdna3_fwhdr.h — RDNA3 firmware header parsing (pure functions) and the
//  17-piece PSP LOAD_IP_FW sequence table (stage-2 tech plan §3.1.4/§7.3).
//
//  Why this exists: every LOAD_IP_FW command needs a segment PAYLOAD length
//  and a payload FILE OFFSET, and those live in the firmware file's own
//  header — never in a hand-written table.  nullarc's bringup script passed
//  267008 B for gc_11_0_1_mec.bin while the archived header's
//  ucode_size_bytes says 267904 B; the 896 B gap is the jump table, which
//  Linux subtracts for CP_MEC (amdgpu_ucode.c:906-911) because it is not PSP
//  payload.  The parser below applies the same rule (C1.2), so a size that
//  ignores jt_size still walks off the payload on a firmware revision
//  change.  The parsers read the archived blob with the same field semantics
//  Linux uses (amdgpu_ucode.c:851-1091 for the segment sizes,
//  amdgpu_rlc.c:281-492 for the RLC sub-blobs) and are pure host-testable
//  functions: no I/O, no allocation, no STL.
//
//  Header classes (tech plan §7.3, D5):
//    common        — every blob; one payload at ucode_array_offset_bytes
//    rlc v2.0–v2.3 — the RLC container plus its save/restore sub-blobs
//    mes v1.0      — code + data segment pair, plus the two start addresses
//
//  Segment sizes are ALWAYS taken from the sub-blob offset/size field family
//  (or "file length − array offset" for a plain common header), never from
//  common.ucode_size_bytes: for a container that field describes RLC_G, not
//  the file (R1 §3.6 semantic trap).
//
//  Every parser rejects: a declared length past the buffer, an
//  array/sub-blob offset past the declared length, a zero-length required
//  payload, a staging base that is not 4 KiB aligned (psp_gfx_if.h:317
//  requires the LOAD_IP_FW address itself to be aligned, and each segment is
//  staged in its own aligned buffer — see r3_fw_stage_addr), and (for RLC)
//  any header version outside v2.0–v2.3.
//
//  Provenance metadata (sha256, commit, source path) for the ten embedded
//  RDNA3 blobs lives next to the table it describes: fw_table_rdna3.cpp.
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
#ifndef N48_RDNA3_FWHDR_H_
#define N48_RDNA3_FWHDR_H_

#include <stdint.h>
#include <stddef.h>

#include "fw_table.h"

// 4 KiB: the granularity PSP LOAD_IP_FW requires of the staged source
// address (psp_gfx_if.h:317).
#define R3_FW_STAGE_ALIGN  4096ull
// Largest segment list any supported class produces: RLC v2.3 yields 6
// (GPM/SRM/IRAM/DRAM/RLC_P/RLC_G); the optional 22 and 7 rows fit inside
// the same bound, so a table that turns them on cannot overflow silently.
#define R3_FW_MAX_SEGS     8u

// Error codes (0 = OK).  Distinct codes rather than a bool so a failure log
// can name the reason (F6: every failure carries its reason).
enum R3FwHdrStatus {
    R3_FW_HDR_OK            =  0,
    R3_FW_HDR_ERR_NULL      = -1,   // null argument
    R3_FW_HDR_ERR_SHORT     = -2,   // buffer shorter than the header needs
    R3_FW_HDR_ERR_RANGE     = -3,   // a declared length/offset runs past the buffer
    R3_FW_HDR_ERR_ZERO_SIZE = -4,   // a required payload is zero length
    R3_FW_HDR_ERR_ALIGN     = -5,   // staging address not 4 KiB aligned
    R3_FW_HDR_ERR_VERSION   = -6,   // header version outside the supported set
    R3_FW_HDR_ERR_CAP       = -7,   // segment list would exceed R3_FW_MAX_SEGS
};

// One firmware segment = one LOAD_IP_FW payload:
//   {fw_mc = stage_mc + offset, size = size, fw_type}
struct R3FwSeg {
    uint32_t    fw_type;    // psp_gfx_fw_type (psp_gfx_if.h:191-205)
    uint32_t    offset;     // byte offset of the payload inside the blob
    uint32_t    size;       // payload length in bytes (never the file size)
    const char *name;       // static label for logs; never null on success
};

// Parsed image: common-header fields plus the segment list.
struct R3FwImage {
    uint32_t header_version_major;
    uint32_t header_version_minor;
    uint32_t ip_version_major;
    uint32_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;  // raw common field; RLC_G's length for a container
    uint32_t array_offset;      // ucode_array_offset_bytes
    uint32_t declared_size_bytes; // common size_bytes: the file length the header declares
    uint32_t declared_header_bytes; // common header_size_bytes: the class header's size
    uint32_t seg_count;
    R3FwSeg  segs[R3_FW_MAX_SEGS];
    // mes v1.0 only (zero for every other class).
    uint64_t mes_uc_start_addr;
    uint64_t mes_data_start_addr;
};

// R3FwImage is caller-owned and lives on a kernel stack in stage 2; keep the
// size pinned so a future field added here is a conscious choice, not a
// silent stack-budget regression.  (T6 asserts this value.)
static_assert(sizeof(R3FwImage) <= 256,
              "R3FwImage grew past its 256-byte stack budget");

//
// Class parsers.  `data`/`len` is the whole blob (len = fw_get(id)->size);
// `stage_mc` is the 4 KiB-aligned MC base of the caller's staging area.
// Each segment is copied to its own aligned slot and its LOAD_IP_FW address
// is r3_fw_stage_addr(stage_mc, img, i) — never stage_mc + seg.offset, which
// would not be aligned (psp_gfx_if.h:317; Linux does the same,
// amdgpu_ucode.c:1203-1214).  seg.offset says where the bytes are INSIDE the
// blob, which is all the parser can decide; where each slot lands is the
// carveout allocator's business (tech plan §4.2: fw_buf is one 16 MiB region
// with 4 KiB-aligned fixed offsets totalling ≈1.7 MiB).
//

// Plain common header (any version): exactly one segment at
// ucode_array_offset_bytes.  pfp/me/toc/ta ship ucode_size_bytes bytes
// (R1 §3.6; amdgpu_ucode.c:906-911).  fw_type stays 0 — a common header
// does not say what it holds; r3_fw_parse_id stamps it.  MEC has its own
// path: its jump table is appended inside ucode_size_bytes and is NOT PSP
// payload, so r3_fw_parse_id ships ucode_size_bytes − jt_size*4 (C1.2).
int r3_fw_parse_common(const uint8_t *data, size_t len,
                       uint64_t stage_mc, R3FwImage *out);

// sdma v2.0: TH0 at array offset (ctx_ucode_size_bytes) and TH1 at
// ctl_ucode_offset (ctl_ucode_size_bytes).  amdgpu_ucode.c:891-901.
int r3_fw_parse_sdma(const uint8_t *data, size_t len,
                     uint64_t stage_mc, R3FwImage *out);

// imu v1.0: IMU_I at array offset (iram size), IMU_D at +iram size
// (dram size).  amdgpu_ucode.c:1026-1031.
int r3_fw_parse_imu(const uint8_t *data, size_t len,
                    uint64_t stage_mc, R3FwImage *out);

// rlc v2.0–v2.3 container.  RLC_G is ucode_size_bytes at the array
// offset; the sub-blobs come from the v2_1/v2_2/v2_3 field families
// (amdgpu_rlc.c:281-492) and are emitted in the 17-piece order
// 20, 21, 26, 48, 25, 8.  22 (save_restore_list_cntl) and 7 (rlcv) are
// emitted only when their size is non-zero: in the archived v2.3 both are
// zero, so neither enters the load list (R1 §3.6).  Any other version is
// ERR_VERSION.
int r3_fw_parse_rlc(const uint8_t *data, size_t len,
                    uint64_t stage_mc, R3FwImage *out);

// mes v1.0: code segment (mes_ucode_offset/size) and data segment
// (mes_ucode_data_offset/size) — amdgpu_ucode.c:977-996, NOT ucode_size_bytes
// which here is the code length by coincidence of the packager.  Also
// yields mes_uc_start_addr / mes_data_start_addr (64-bit lo/hi pairs).
// fw_type is left for r3_fw_parse_id: 33/34 for mes_2.bin, 81/82 for
// mes1.bin (amdgpu_mes.c:753-780).
int r3_fw_parse_mes(const uint8_t *data, size_t len,
                    uint64_t stage_mc, R3FwImage *out);

// Dispatch by blob identity and stamp every segment's fw_type:
//   PSP13_TOC/TA, GC11_PFP/ME → common (24/75/2/1; TOC and TA are never in
//     the load order — TOC goes to LOAD_TOC, TA is not loaded in stage 2);
//   GC11_MEC → CP header minus the jump table (ucode_size_bytes -
//     jt_size*4, amdgpu_ucode.c:906-911); fw_type 4;
//   SDMA6, GC11_IMU, GC11_RLC, GC11_MES_2, GC11_MES1 → their containers.
// ERR_VERSION for any id that has no stage-2 parser.
int r3_fw_parse_id(amdgpu::FwId id, const uint8_t *data, size_t len,
                   uint64_t stage_mc, R3FwImage *out);

// The TOC's LOAD_TOC payload: the array-offset payload of psp_13_0_4_toc
// (amdgpu_psp.c:853-859 stages toc.size_bytes from the payload start and
// submits those bytes; the TOC is never a LOAD_IP_FW type, R1 §3.6).
int r3_fw_parse_toc(const uint8_t *data, size_t len, uint64_t stage_mc,
                    R3FwImage *out);

// LOAD_IP_FW source address of segment `index` of a parsed image: pack the
// segments in order, each rounded up to a 4 KiB page, exactly as Linux lays
// out its single firmware BO (amdgpu_ucode.c:1203-1214).  This reproduces
// the tech plan §4.2 fw_buf shape ("每件 4 KiB 对齐固定偏移，合计 ≈1.7 MiB")
// without inventing a constant: r3_fw_stage_size(img) is the area a caller
// must reserve, and it is the sum of the same rounded sizes.
// `index` must be < img->seg_count.
uint64_t r3_fw_stage_addr(uint64_t stage_mc, const R3FwImage *img,
                          uint32_t index);

// Bytes of staging area one parsed image needs (sum of 4 KiB-rounded
// payloads).  0 for an empty image.
uint64_t r3_fw_stage_size(const R3FwImage *img);

// Locate a segment by fw_type; nullptr when absent.
const R3FwSeg *r3_fw_find_seg(const R3FwImage *img, uint32_t fw_type);

// PSP LOAD_IP_FW sequence — 17 entries, Linux ucode_id ascending (D2).
// TH0 71 → TH1 72 → PFP 2 → ME 1 → MEC 4 → 33 → 34 → 81 → 82 →
// IMU_I 68 → IMU_D 69 → 20 → 21 → 26 → 48 → RLC_P 25 → RLC_G 8.
// Excluded by construction: MEC JT 5/6 (skipped when autoload is on,
// amdgpu_psp.c:3433-3446), RLC 22/7 (size 0 in the archived v2.3 header),
// RS64 87/88/89 (pfp header is v1.0 ⇒ rs64_enable=false).
// `source` is the blob an entry's segment comes from — load it with
// r3_fw_parse_id, then r3_fw_find_seg(img, fw_type).

#define R3_PSP_LOAD_COUNT 17u

struct R3PspLoadEntry {
    uint32_t      fw_type;   // psp_gfx_fw_type
    amdgpu::FwId  source;    // blob holding this segment
    const char   *name;      // static label for logs
};

extern const R3PspLoadEntry r3_psp_load_order[R3_PSP_LOAD_COUNT];

// Provenance of the ten embedded blobs (A2/A4): sha256 as registered in
// evidence/src/INDEX.md M7, the linux-firmware commit the blobs were taken
// from, and the path inside that checkout — the "M7 登记指针" A4 wants in the
// load evidence.  The digest is a BUILD-time gate (tech plan §7.2); the
// run-time check stays what §7.2 specifies: length self-consistency only.
//
struct R3FwProvenance {
    amdgpu::FwId id;
    const char  *name;             // bare stem, e.g. "gc_11_0_1_imu"
    const char  *sha256;           // 64 lowercase hex chars
    const char  *linux_fw_commit;  // "bda6eccc"
    const char  *source_path;      // "amdgpu/<name>.bin"
    uint32_t     size_bytes;       // file size, INDEX M7
};

// Provenance row of an embedded blob; nullptr for every other id.
const R3FwProvenance *r3_fw_provenance(amdgpu::FwId id);

#endif // N48_RDNA3_FWHDR_H_
