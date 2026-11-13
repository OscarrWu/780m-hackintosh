//
//  gfx_v11_0.h - GFX 11.0.1 (Phoenix / 780M) read-only readiness substep for the RDNA3
//  bringup (tech plan §3.3, requirement C3.1-C3.3).
//
//  Structure follows Linux drivers/gpu/drm/amd/amdgpu/{gfx_v11_0.c, imu_v11_0.c,
//  nbif_v6_3_1.c} onto the mac-amdgpu (Cobalt64) IP-module conventions. Only the
//  register/state semantics are reproduced, no Linux code is copied verbatim; all offsets
//  come from the pinned header gc_11_0_0_offset.h and are cited at every use site.
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
//  What this module does (tech plan §3.3; requirement C3):
//
//    PRE  PSP ready + SMU ready (the two dependency gates of §8.1), a complete R3Io
//         surface, and GC BASE_IDX 0/1 resolved. No layout grant is needed: the substep
//         issues ZERO writes (the plan's ruling: CP unhalt belongs to the MES substep).
//    SEQ  empty by design. There is no register/protocol write in this substep, so the
//         section exists only to keep the four-section shape identical to K4-K8.
//    CHK  S1 IMU version/status, S2 RLC autoload readiness (BOOTLOAD_COMPLETE and
//         CP_STAT==0, Linux gfx_v11_0.c:3081-3119), S3 CP_ME_CNTL readback. The
//         RLC_STAT/RLC_GPM_STAT/GRBM_STATUS readings are recorded for comparison only.
//    EV   the S1-S3 readings into env->ev, the GFX ready bit, and N48R3EVID-gfx-done.
//
//  Two things are deliberately NOT here:
//    * any write, including CP_ME_CNTL/CP_RB0_*: CP unhalt is the MES substep's job
//      (§3.5). Issuing it here would widen the R2 risk surface without adding a GFX
//      acceptance fact.
//    * the full Linux gfx_v11_0_hw_init write sequence (gfxhub, golden regs, constants,
//      gc_doorbell_init, rlc_resume, cp_resume): those are writes and belong to stage 3.
//      The two-source comparison is in tmp/programmer/K6/GFX两源对照补全.md.
//
//  Threshold provenance (the backfill of §3.3's limitation statement):
//    S1  valid(GFX_IMU_SCRATCH_0) || (GFX_IMU_GFX_RESET_CTRL & 0x1f) == 0x1f
//        Linux reads SCRATCH_0 as the IMU fw version (gfx_v11_0.c:4996-4998) and polls
//        the reset status in imu_v11_0_wait_for_reset_status (imu_v11_0.c:134-151).
//        The PSP load path does not call imu_v11_0_start, so either witness is accepted.
//    S2  (RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE == 1) && (CP_STAT == 0),
//        polled for up to kGfx11AutoloadTimeoutUs. This is Linux
//        gfx_v11_0_wait_for_rlc_autoload_complete (gfx_v11_0.c:3081-3119), with the
//        IP-version override regRLC_RLCS_BOOTLOAD_STATUS_gc_11_0_1 (gfx_v11_0.c:62).
//        RLC_STAT/RLC_GPM_STAT/GRBM_STATUS/GRBM_STATUS2 are recorded next to the
//        nullarc readings and never gate. The old "two equal samples" rule is gone.
//    S3  CP_ME_CNTL readable (halt bits recorded, not gated). CP unhalt belongs to
//        the MES substep (C-07).
//
#ifndef NAVI48_GFX_V11_0_H
#define NAVI48_GFX_V11_0_H

#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"


namespace amdgpu {

// ---------------------------------------------------------------------------------------
// GC register offsets (gc_11_0_0_offset.h; BASE_IDX is the SOC15 segment index)
// ---------------------------------------------------------------------------------------
//   GFX_IMU_SCRATCH_0        0x4068 BASE_IDX 1   gc_11_0_0_offset.h:11106-11107
//   GFX_IMU_GFX_RESET_CTRL   0x40bc BASE_IDX 1   gc_11_0_0_offset.h:11239-11240
//   GFX_IMU_CORE_CTRL        0x40b6 BASE_IDX 1   gc_11_0_0_offset.h:11227-11228 (evidence)
//   GFX_IMU_SCRATCH_10       0x4072 BASE_IDX 1   gc_11_0_0_offset.h:11126-11127 (evidence)
//   RLC_STAT                 0x4c04 BASE_IDX 1   gc_11_0_0_offset.h:9804-9805
//   RLC_GPM_STAT             0x4e6b BASE_IDX 1   gc_11_0_0_offset.h:10370-10371
//   RLC_F32_UCODE_VERSION    0x4c03 BASE_IDX 1   gc_11_0_0_offset.h:9802-9803 (evidence)
//   CP_ME_CNTL               0x0803 BASE_IDX 1   gc_11_0_0_offset.h:6198-6199
//   GRBM_STATUS              0x0da4 BASE_IDX 0   gc_11_0_0_offset.h:1846-1847
//   GRBM_STATUS2             0x0da2 BASE_IDX 0   gc_11_0_0_offset.h:1842-1843
//   CP_STAT                  0x0f40 BASE_IDX 0   gc_11_0_0_offset.h:1970-1971

constexpr uint32_t kGfx11ImuCoreCtrl       = 0x40b6;
constexpr int      kGfx11ImuCoreCtrlBase   = 1;
constexpr uint32_t kGfx11ImuScratch10      = 0x4072;
constexpr int      kGfx11ImuScratch10Base  = 1;

constexpr uint32_t kGfx11RlcF32UcodeVersion = 0x4c03;
constexpr int      kGfx11RlcF32UcodeVersionBase = 1;


// RLC_RLCS_BOOTLOAD_STATUS for GC 11.0.1 (gfx1103). Linux overrides the generic
// gc_11_0_0 register (0x4e82) with the IP-specific 0x4e7e in gfx_v11_0.c:62; the
// golden table already carries 0x4e7e for this target, so the constants agree.
constexpr uint32_t kGfx11RlcRlcsBootloadStatus = 0x4e7e;
constexpr int      kGfx11RlcRlcsBootloadStatusBase = 1;
constexpr uint32_t kGfx11BootloadComplete     = 0x80000000u; /* bit31 */

// ---------------------------------------------------------------------------------------
// Bit fields (gc_11_0_0_sh_mask.h)
// ---------------------------------------------------------------------------------------
//   CP_STAT.PFP_BUSY                     bit15 = 0x00008000  :6497
//   CP_STAT.ME_BUSY                      bit17 = 0x00020000  :6499
//   CP_STAT.CP_BUSY                      bit31 = 0x80000000  :6513
//   CP_ME_CNTL.CE_HALT                   bit24 = 0x01000000  :24035
//   CP_ME_CNTL.PFP_HALT                  bit26 = 0x04000000  :24037
//   CP_ME_CNTL.ME_HALT                   bit28 = 0x10000000  :24039
//   CP_STAT.CE_BUSY                      bit24 = 0x01000000  :6507
constexpr uint32_t kGfx11CpStatCeBusy  = 0x01000000u;
constexpr uint32_t kGfx11CpStatPfpBusy = 0x00008000u;
constexpr uint32_t kGfx11CpStatMeBusy  = 0x00020000u;
constexpr uint32_t kGfx11CpStatCpBusy  = 0x80000000u;
constexpr uint32_t kGfx11CpMeCntlCeHalt  = 0x01000000u;
constexpr uint32_t kGfx11CpMeCntlPfpHalt = 0x04000000u;
constexpr uint32_t kGfx11CpMeCntlMeHalt  = 0x10000000u;

// imu_v11_0_wait_for_reset_status: GFX_IMU_GFX_RESET_CTRL low 5 bits == 0x1f means the
// IMU has released every GFX reset domain (imu_v11_0.c:139-141).
constexpr uint32_t kGfx11ImuResetDoneMask = 0x0000001fu;
constexpr uint32_t kGfx11ImuResetDone     = 0x0000001fu;

// S2 readiness poll: Linux waits adev->usec_timeout (AMDGPU_MAX_USEC_TIMEOUT = 100 ms)
// while polling every 1 us. Readiness is sticky, so 1 ms steps keep the same 100 ms
// budget with far fewer MMIO reads; the final values are collected on timeout.
constexpr uint32_t kGfx11AutoloadTimeoutUs = 100000;
constexpr uint32_t kGfx11AutoloadStepUs    = 1000;


} // namespace amdgpu

#endif // NAVI48_GFX_V11_0_H
