//
//  psp_v13_0.h - PSP 13.0.4 (Phoenix / 780M) power-up substep for the RDNA3 bringup.
//
//  Structure port of Linux drivers/gpu/drm/amd/amdgpu/{psp_v13_0_4.c, amdgpu_psp.c,
//  psp_gfx_if.h, amdgpu_psp.h} onto the mac-amdgpu (Cobalt64) IP-module conventions. Only
//  the register/protocol semantics are reproduced, no Linux code is copied verbatim; all
//  offsets come from the pinned headers (mp_13_0_4_offset.h / psp_gfx_if.h) and are cited
//  at every use site.
//
//  Copyright 2020 Advanced Micro Devices, Inc.
//  MIT licensed - see NOTICE.
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this
//  software and associated documentation files (the "Software"), to deal in the Software
//  without restriction, including without limitation the rights to use, copy, modify,
//  merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to the following
//  conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies
//  or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//  INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//  PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
//  THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//  What this module does (tech plan §3.1, requirement C1): the complete PSP substep of the
//  five-substep ladder:
//
//    PRE  P0 MP0 mailbox resolved through the golden table (group 1), P2 layout plan is
//         valid and has the PSP grants, P3 SOS-alive self-check (C2PMSG_33 then C2PMSG_81,
//         read-only, before any write). A failed precondition stops the substep and exports
//         the readings; the driver NEVER tries to load SOS itself (requirement C0.4).
//    SEQ  S2 ring/cmd/fence zero-fill -> S3 ring create -> S4 TOC stage -> S5 LOAD_TOC and
//         read resp.tmr_size -> S6 TMR placement (1 MiB aligned, slot sub-region) -> S7
//         SETUP_TMR (system_phy recomputed from this machine's FB_OFFSET) -> S8 17x
//         LOAD_IP_FW in Linux enum order -> S9 AUTOLOAD_RLC immediately after RLC_G -> S11
//         GPCOM fence.
//    CHK  S10 C2PMSG_33 bit31 (observation only), S11 fence measured, S12 firmware
//         version registers (against the preflight-rebuilt baseline).
//
//  Two things are deliberately NOT here:
//    * bootloader kdb/spl/sysdrv/soc/intf/dbg/ras/ipkeymgr/spdm/sos: 13.0.4 initialises
//      only toc+ta, so every one of those descriptors is empty and Linux skips them
//      (amdgpu_psp.c:2913-3001; psp_v13_0_4.c:35-56). They are not implemented, and SOS is
//      never loaded (C0.4/C1.1).
//    * ASD / RL / P2S / TMR destroy: empty or functional-init-only steps in both sources
//      (R1 §3.10). teardown does not reverse PSP either (tech plan §9).
//
#ifndef NAVI48_PSP_V13_0_H
#define NAVI48_PSP_V13_0_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"

#include "../rdna3/rdna3_fwhdr.h"
#include "../rdna3/rdna3_ip_entries.h"

namespace amdgpu {

// MP0_SMN register offsets live in rdna3/golden_values.h (P1, A-01/D-03): each entry
// carries the Linux _BASE_IDX, so the production code resolves every mailbox address
// through IPBaseTable::resolve() rather than adding a segment-0 base.  The group here is
// BASE_IDX 1 (0x16000 on this machine); see r3gold::kMp0SmnC2pmsg*.
//
// MP0 mailbox response/ready masks (amdgpu_psp.h:57-65 via psp_gfx_if.h:29-32,84).
constexpr uint32_t kPspMboxRespFlag  = 0x80000000u;  // GFX_FLAG_RESPONSE
constexpr uint32_t kPspMboxStatusMask = 0x0000FFFFu; // GFX_CMD_STATUS_MASK
constexpr uint32_t kPspMboxReadyFlag = 0x80000000u;  // MBOX_TOS_READY_FLAG (bit31 only)
// MBOX_TOS_READY_MASK is (GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK): bit31 set and
// status 15:0 == 0. Using bit31 alone would accept a non-zero status.
constexpr uint32_t kPspMboxReadyMask = 0x8000FFFFu;  // MBOX_TOS_READY_MASK
// C2PMSG_33 bit31: IFWI/PSP boot finished (amdgpu_discovery.c:281-284). Observation only:
// C1.3 v1.4 makes ready = 81 + non-zero versions + fence, never this bit.
constexpr uint32_t kPspC2pmsg33ReadyBit = 0x80000000u;
constexpr uint32_t kPspC2pmsg64RingInitKm = 0x00020000u;  // PSP_RING_TYPE__KM(2) << 16

// Ring constants (amdgpu_psp.c:75-90, amdgpu_psp.h:35-36). The ring is 4 KiB on the wire;
// this driver reserves and clears 16 KiB (nullarc's more conservative form, D-5).
constexpr uint32_t kPspRingBytes     = 0x1000;
constexpr uint32_t kPspCmdBufBytes   = 0x1000;
constexpr uint32_t kPspFenceBufBytes = 0x1000;
constexpr uint32_t kPspFrameBytes    = 64;    // sizeof(psp_gfx_rb_frame), psp_gfx_if.h:497-512
constexpr uint32_t kPspFrameDwords   = kPspFrameBytes / 4;

// GPCOM command ids (psp_gfx_if.h:88-111).
constexpr uint32_t kGfxCmdSetupTmr    = 0x00000005u;
constexpr uint32_t kGfxCmdLoadIpFw    = 0x00000006u;
constexpr uint32_t kGfxCmdLoadToc     = 0x00000020u;
constexpr uint32_t kGfxCmdAutoloadRlc = 0x00000021u;

// GFX_FW_TYPE_RLC_G (psp_gfx_if.h:227): AUTOLOAD_RLC follows this piece and no other.
constexpr uint32_t kFwTypeRlcG = 8u;

// psp_gfx_cmd_resp / psp_gfx_rb_frame field offsets (psp_gfx_if.h:466-512).
constexpr uint32_t kCmdRespBufSize    = 0;   // u32 total buffer size
constexpr uint32_t kCmdRespBufVersion = 4;   // u32 PSP_GFX_CMD_BUF_VERSION
constexpr uint32_t kCmdRespCmdId      = 8;   // u32 command id
constexpr uint32_t kCmdRespCmd        = 28;  // +0 command-specific structure
constexpr uint32_t kCmdRespResp       = 864; // +0 struct psp_gfx_resp
constexpr uint32_t kCmdRespTmrSize    = 16;  // resp.tmr_size (inside psp_gfx_resp)

constexpr uint32_t kRbFrameCmdLo    = 0 + 0;   // [0], [4]
constexpr uint32_t kRbFrameCmdHi    = 4;
constexpr uint32_t kRbFrameCmdSize  = 8;
constexpr uint32_t kRbFrameFenceLo  = 12;
constexpr uint32_t kRbFrameFenceHi  = 16;
constexpr uint32_t kRbFrameFenceVal = 20;

// Command bodies (psp_gfx_if.h:191-226, 300-346).
//   SETUP_TMR: buf_phy_addr_lo/hi, buf_size, tmr_flags, system_phy_addr_lo/hi
constexpr uint32_t kSetupTmrAddrLo     = 0;
constexpr uint32_t kSetupTmrAddrHi     = 4;
constexpr uint32_t kSetupTmrSize       = 8;
constexpr uint32_t kSetupTmrFlags      = 12;  // bitfield: sriov_enabled:1, virt_phy_addr:1
constexpr uint32_t kSetupTmrSysPhyLo   = 16;
constexpr uint32_t kSetupTmrSysPhyHi   = 20;
constexpr uint32_t kSetupTmrFlagVirtPhy = 0x2u;  // virt_phy_addr = 1 (bit 1, psp_gfx_if.h:197-201)
//   LOAD_IP_FW: fw_phy_addr_lo/hi, fw_size, fw_type
constexpr uint32_t kLoadIpFwAddrLo = 0;
constexpr uint32_t kLoadIpFwAddrHi = 4;
constexpr uint32_t kLoadIpFwSize   = 8;
constexpr uint32_t kLoadIpFwType   = 12;
//   LOAD_TOC: toc_phy_addr_lo/hi, toc_size
constexpr uint32_t kLoadTocAddrLo = 0;
constexpr uint32_t kLoadTocAddrHi = 4;
constexpr uint32_t kLoadTocSize   = 8;
//   AUTOLOAD_RLC: no body.

// Timeout budget (tech plan §3.1: Linux magnitude as the default, nullarc magnitude as the
// ceiling; a slow response is a wait, not a failure - F6 allows no retry, so this is the
// only tolerance window). Linux: psp_wait_for = usec_timeout(100 ms) x 1 us polls
// (amdgpu_device.c:3775, amdgpu_psp.c:603-634); fence = psp_timeout 20000 x 60..100 us
// (amdgpu_psp.c:292,744-760); ring-create mdelay(20) (psp_v13_0_4.c:277). nullarc: ready
// 2 s, fence 5 s (R1 §2 #15). Waits that Linux gives 100 ms and nullarc 2 s are polled
// twice: the Linux budget first, then the remainder up to the nullarc ceiling - every
// wait therefore has one deterministic poll bound (polls_for_us at kPspPollUs).
constexpr uint32_t kPspPollUs            = 10;      // poll period for every register wait
constexpr uint32_t kPspReadyBudgetUs     = 100000;  // Linux: 100 ms for C2PMSG_64 ready
constexpr uint32_t kPspReadyCeilingUs    = 2000000; // nullarc: 2 s ceiling -> polls_for_us
constexpr uint32_t kPspRingCmdDelayMs    = 20;      // mdelay(20) before the response wait
// S11 / V4: the GPCOM submit fence latency is RECORDED (observation/KPI), never a substep
// verdict. The hard failures are the fence timeout (submit()) and a non-zero response
// status (post()). A firmware-load submit (LOAD_TOC/LOAD_IP_FW) must not fail the PSP on
// this threshold: the base allows 20 s (Cobalt64) and Linux uses psp_timeout; the measured
// 4.14 ms LOAD_TOC on this machine is normal (2026-11-05 review).
constexpr uint32_t kPspFenceMaxUs        = 1000;
constexpr uint32_t kPspFencePollUs       = 60;      // Linux usleep_range(60,100) lower bound
constexpr uint32_t kPspFenceBudgetUs     = 2000000; // 2 s ceiling
constexpr uint32_t kPspFencePolls        = kPspFenceBudgetUs / kPspFencePollUs;

// D-08 TMR placement: the layout reserves a 64 MiB slot; resp.tmr_size occupies a sub-region
// of it (the rest stays idle) and is only 1 MiB aligned (Linux PSP_TMR_ALIGNMENT). A returned
// size above the slot cannot be placed and fails closed with full evidence.
constexpr uint64_t kPspTmrSlotBytes  = 64ull << 20;
constexpr uint64_t kPspTmrAlignBytes = 1ull << 20;
constexpr uint64_t kPspTmrSizeAlign  = 4096ull;  // psp_gfx_if.h: buf_size must be a 4 KB multiple

// The one named refusal budget: a poll count derived at compile time from the ceiling.

// S12/D3: the PSP itself records its version (C2PMSG_58; pre from the existing preflight
// probe, post here). RLC_F32/SDMA0 are observations in their own substeps (golden entries),
// not PSP gates; the stage-1 "all zero" baseline is invalid (wrong segment).



// Staged firmware structures — local to psp_v13_0, not in contract.
struct PspFwStage {
    uint32_t fw_type;     // psp_gfx_fw_type of this row
    const char *name;     // static label (r3_psp_load_order)
    uint32_t size;        // payload length from the parsed header (MEC excludes its jump table)
    uint32_t offset;      // payload offset inside its blob
    const uint8_t *blob;  // blob bytes (payload source)
    uint32_t blob_size;
    uint64_t mc;          // MC address of this row's 4 KiB staging slot
    uint64_t bar0;        // BAR0 byte offset of the same slot
};

struct PspStagePlan {
    PspFwStage toc;                    // psp_13_0_4_toc payload (LOAD_TOC)
    PspFwStage fw[R3_PSP_LOAD_COUNT];  // the 17 LOAD_IP_FW rows, table order (D2)
    uint32_t   fw_count;
    uint64_t   total_bytes;            // staged bytes, TOC included
    uint64_t   fw_buf_mc;              // MC base of the staging grant
    uint64_t   fw_buf_bar0;            // BAR0 byte offset of the same grant
    uint64_t   fw_buf_capacity;        // grant size in bytes
    bool       ok;
    const char *fail;                  // "stage:grant" | "stage:space" | "stage:parse"
    int        err;                    // the underlying code (0 when ok)
};

// Lay out the staging area. Local to psp_v13_0, not in contract.
PspStagePlan psp_stage_plan(const struct R3LayoutPlan *plan, uint64_t fb_mc_base);
} // namespace amdgpu

#endif // NAVI48_PSP_V13_0_H
