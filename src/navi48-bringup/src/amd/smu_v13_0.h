//
//  smu_v13_0.h - SMU 13.0.4 (Phoenix / 780M) power-up substep for the RDNA3 bringup.
//
//  Structure port of Linux drivers/gpu/drm/amd/pm/swsmu/{smu_cmn.c, smu13/smu_v13_0.c,
//  smu13/smu_v13_0_4_ppt.c, inc/pmfw_if/smu_v13_0_4_ppsmc.h} onto the mac-amdgpu
//  (Cobalt64) IP-module conventions. Only the register/protocol semantics are reproduced;
//  every offset is cited at its use site (mp_13_0_4_offset.h / gc_11_0_0_offset.h).
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
//  What this module does (tech plan §3.2, requirement C0.3/C0.5/C0.7/C2.1-C2.3):
//
//    PRE  P0 I/O surface + runtime; P1 MP1 segment 1 resolved (mailbox BASE_IDX=1);
//         P2 PSP ready (dependency gate); P3 GC/SDMA bases resolved (effect readings);
//         P4 MP1_FIRMWARE_FLAGS (SMN, RO) bit0 recorded: bit0=0 is a WARN and the
//         sequence continues (C0.5 effect verdict is the final gate); a read failure
//         stops the substep before any mailbox write (read-path fault = fail closed).
//    SEQ  S1 FIRMWARE_FLAGS (read in PRE, restated) -> S2 mailbox configuration
//         (C2PMSG_66/82/90, BASE_IDX=1) -> S3 0x3 GetDriverIfVersion -> S4 0x2
//         GetPmfwVersion -> S5 0x16 EnableGfxImu(1) -> S6 effect verdict (J-2+J-5) with
//         the single recheck of D11 -> S7 0x12 GetEnabledSmuFeatures low/high (audit).
//         S3/S4/S7 are evidence/audit only: C0.5/C2.3 make the effect criterion the
//         final gate, so their timeout/error/value difference never latches the SMU off.
//    CHK  C0.4 evidence: PMFW/driver-if recorded (unavailable/zero audited, not gated),
//         effect verdict passed; D10 note on every value that differs from the baseline.
//    EV   the §3.2 fields: mailbox addresses, every response/return, the effect samples,
//         the recheck outcome, versions/features and the measured budgets.
//
//  Effect criterion (J-2+J-5): valid(v) = v != 0 && v != 0xFFFFFFFF; gate = GRBM_STATUS
//  valid on two consecutive samples >= 10 ms apart. RLC_STAT, GB_ADDR_CONFIG,
//  GRBM_STATUS2 and SDMA0_STATUS_REG are companions (recorded, never gated): RLC_STAT's
//  fields are all busy bits, 0 = idle, so RLC_STAT=0 is healthy, not a failure. A
//  read-path fault (MMIO fail or 0xFFFFFFFF) on any criterion/companion read is fail
//  closed (§8.1). J-3 (GB_ADDR_CONFIG == 0x242) is deliberately NOT used: that is a
//  Hawk Point value.
//  Recheck (D11, exactly one): only 0x16, only when the verdict has not passed and the
//  response was 0 (timeout) or 0xFC (busy). A timeout waits up to 2 s for C2PMSG_90 to
//  become non-zero through the standard transaction's pre-poll; a stuck 0 is never resent
//  (P-2/P-3 have no evidence). 0xFF/0xFE/0xFD/0xFB/0xFFFFFFFF never recheck. S-06: if
//  the effect window-end capture (2 s phase boundary, or the 10 s slow-window end)
//  saw one of the explicit errors 0xFF/0xFD/0xFE, the timeout is not rechecked either.
//  The single use is
//  latched in rt->substep.flags (R3_SUBSTEP_RECHECK_USED). C2PMSG_90 is never cleared
//  by hand and the pre-poll is never skipped.
//
//  SMN access contract: R3Io::smuio receives the SMN BYTE address exactly as Linux
//  RREG32_PCIE does (MP1_Public | smnMP1_V13_0_4_FIRMWARE_FLAGS = 0x03b10028). The
//  integration adapter writes it verbatim to BIF_BX1_PCIE_INDEX2 and reads DATA2 under the
//  SMN lock with a hard timeout; this substep holds R3Io::lock for its whole duration and
//  performs exactly one SMN read (the P4 precondition).
//
//  Read manifest (B5): the substep only calls R3Io::mmio/smuio, so the integration's
//  wrappers must note every successful read into the stage-1 R3ReadManifest. The host test
//  records the same calls and asserts the mailbox/effect addresses were read.
//
//  Integration grants (K3, owner WSubstep::Smu, class WClass::Protocol, 1 dword each):
//    MP1 base[1] + 0x282  "reg:smu-c2pmsg66"   (message id)
//    MP1 base[1] + 0x292  "reg:smu-c2pmsg82"   (parameter / return value)
//    MP1 base[1] + 0x29a  "reg:smu-c2pmsg90"   (response)
//  The test registers these itself; the production integration card owns the same rows.
//
#ifndef NAVI48_SMU_V13_0_H
#define NAVI48_SMU_V13_0_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"


namespace amdgpu {

// ---------------------------------------------------------------------------------------
// MP1 mailbox registers (mp_13_0_4_offset.h:300-349; BASE_IDX=1 on every one).
// The R3 report §2.1 warning: MP0's same-named registers are at 0x0082/0x0092/0x009a,
// so using the MP0 offsets with MP1 base[1] would write 0x16082 instead of 0x16282.
// ---------------------------------------------------------------------------------------


// MP1_FIRMWARE_FLAGS, read through the SMN indirect pair (smu_v13_0.c:237-259 uses
// RREG32_PCIE(MP1_Public | smnMP1_V13_0_4_FIRMWARE_FLAGS)). MP1_Public is 0x03b00000;
// the resulting byte address 0x03b10028 is also smu_v13_0_7's direct constant.
constexpr uint32_t kSmuMp1Public     = 0x03b00000u;
constexpr uint32_t kSmuSmnFirmwareFlags = 0x03010028u;
constexpr uint32_t kSmuFirmwareFlagsIndex = kSmuMp1Public | kSmuSmnFirmwareFlags;
constexpr uint32_t kSmuFirmwareFlagsInterruptsEnabled = 0x00000001u;  // bit0

// ---------------------------------------------------------------------------------------
// GC / SDMA registers used by the effect criterion (gc_11_0_0_offset.h).
// GRBM_STATUS and GRBM_STATUS2/GB_ADDR_CONFIG are GC BASE_IDX 0; RLC_STAT is BASE_IDX 1;
// SDMA0_STATUS_REG is SDMA0 BASE_IDX 0.
// ---------------------------------------------------------------------------------------
constexpr uint32_t kSmuGrbmStatus    = 0x0da4;  // gc_11_0_0_offset.h, BASE_IDX 0
constexpr uint32_t kSmuGrbmStatus2   = 0x0da2;  // gc_11_0_0_offset.h, BASE_IDX 0
constexpr uint32_t kSmuGbAddrConfig  = 0x13de;  // gc_11_0_0_offset.h, BASE_IDX 0
constexpr uint32_t kSmuRlcStat       = 0x4c04;  // gc_11_0_0_offset.h, BASE_IDX 1
constexpr uint32_t kSmuSdma0StatusReg = 0x0025; // gc_11_0_0_offset.h, SDMA0 BASE_IDX 0

// ---------------------------------------------------------------------------------------
// Message ids (smu_v13_0_4_ppsmc.h:55,56,71,76) and the EnableGfxImu argument
// (smu_v13_0.c:86 ENABLE_IMU_ARG_GFXOFF_ENABLE). 0x1A DisallowGfxOff is deliberately
// absent (C0.7 minimal message set; R7 is the review path if GFXOFF interferes).
// ---------------------------------------------------------------------------------------
constexpr uint32_t kSmuMsgGetPmfwVersion        = 0x02;
constexpr uint32_t kSmuMsgGetDriverIfVersion    = 0x03;
constexpr uint32_t kSmuMsgGetEnabledSmuFeatures = 0x12;
constexpr uint32_t kSmuMsgEnableGfxImu          = 0x16;
constexpr uint32_t kSmuEnableImuArgGfxoff       = 1u;

// ---------------------------------------------------------------------------------------
// Response codes (smu_cmn.c:76-84; smu_v13_0_4_ppsmc.h:46-53).
// ---------------------------------------------------------------------------------------
constexpr uint32_t kSmuRespNone         = 0x00;  // still executing / poll timeout
constexpr uint32_t kSmuRespOk           = 0x01;
constexpr uint32_t kSmuRespDebugEnd     = 0xFB;  // SMU_MSG_DebugEnd
constexpr uint32_t kSmuRespBusy         = 0xFC;  // SMU_MSG_Busy: sender should retry
constexpr uint32_t kSmuRespBadPrereq    = 0xFD;  // SMU_MSG_BadPrereq
constexpr uint32_t kSmuRespUnknownCmd   = 0xFE;  // SMU_MSG_UnknownCmd
constexpr uint32_t kSmuRespFailed       = 0xFF;  // SMU_MSG_Failed

// ---------------------------------------------------------------------------------------
// Budgets (tech plan §3.2; smu_cmn.c:459-578). The mailbox post/pre-poll is 2 s (Linux
// default_timeout = usec_timeout * 20); the effect poll is T-2 fast (2 s / 10 ms) plus the
// T-5 slow window (10 s / 100 ms) only while C2PMSG_90 is still 0, 12 s total.
// ---------------------------------------------------------------------------------------
constexpr uint32_t kSmuMailboxTimeoutUs   = 2000000u;
constexpr uint32_t kSmuMailboxPollUs      = 1000u;
constexpr uint32_t kSmuEffectFastUs       = 2000000u;
constexpr uint32_t kSmuEffectFastStepUs   = 10000u;
constexpr uint32_t kSmuEffectSlowUs       = 10000000u;
constexpr uint32_t kSmuEffectSlowStepUs   = 100000u;
constexpr uint32_t kSmuEffectTotalUs      = kSmuEffectFastUs + kSmuEffectSlowUs;
constexpr uint32_t kSmuEffectMinGapUs     = 10000u;

// nullarc / R3 baseline (R3 §4.1). Differences are recorded with a D10 note, never
// silently accepted and never a hard failure (D10: version mismatch is not fatal).
// Linux decode (smu_cmn.c:1084-1107): major = bits 16-23, minor = bits 8-15,
// debug = bits 0-7. 0x004c6500 therefore prints as 76.101.0.
constexpr uint32_t kSmuBaselinePmfw      = 0x004C6500u;  // 76.101.0
constexpr uint32_t kSmuBaselineDriverIf  = 8u;
constexpr uint32_t kSmuBaselineFeatureLo = 0xf3fff7fdu;
constexpr uint32_t kSmuBaselineFeatureHi = 0x07a5f3f3u;

} // namespace amdgpu

#endif // NAVI48_SMU_V13_0_H
