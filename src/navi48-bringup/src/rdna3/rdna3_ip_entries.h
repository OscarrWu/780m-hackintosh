//
//  rdna3_ip_entries.h — contract header for IP-module entry-point declarations.
//
//  Each IP module (PSP/SMU/GFX/SDMA/MES) declares its substep entry function
//  and register-table getters in its own amd/ header.  This contract header
//  forward-declares them so rdna3/ production code can call them without
//  depending on the amd/ implementation headers.  It also forwards the IP-
//  internal constants that rdna3 dump & teardown code needs for register
//  inspection (rdna3_dump.cpp, rdna3_teardown.cpp).
//
//  Design intent (Cobalt64 convention): platform contract in common/,
//  implementation in apple/ — here applied as contract in rdna3/,
//  implementation in amd/.  Each amd/ IP header reverse-includes this
//  header to guarantee the declarations match.
//
#pragma once

#include <stdint.h>

#include "rdna3_substep.h"
#include "rdna3_writeguard.h"


namespace amdgpu {

// =========================================================================
// GFX 11.0.1 — substep entry (read-only)
// =========================================================================
R3SubstepResult gfx_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
constexpr uint32_t kGfxRowCount = 0;
inline const R3WriteRowReg* getGfxRows(void) { return nullptr; }

// GFX register constants — for rdna3 dump & teardown inspection.
// Source: amd/gfx_v11_0.h (gc_11_0_0_offset.h / sh_mask.h)
constexpr uint32_t kGfx11ImuScratch0       = 0x4068;
constexpr int      kGfx11ImuScratch0Base   = 1;
constexpr uint32_t kGfx11ImuGfxResetCtrl   = 0x40bc;
constexpr int      kGfx11ImuGfxResetCtrlBase = 1;
constexpr uint32_t kGfx11RlcStat           = 0x4c04;
constexpr int      kGfx11RlcStatBase       = 1;
constexpr uint32_t kGfx11RlcGpmStat        = 0x4e6b;
constexpr int      kGfx11RlcGpmStatBase    = 1;
constexpr uint32_t kGfx11CpMeCntl          = 0x0803;
constexpr int      kGfx11CpMeCntlBase      = 1;
constexpr uint32_t kGfx11GrbmStatus        = 0x0da4;
constexpr int      kGfx11GrbmStatusBase    = 0;
constexpr uint32_t kGfx11GrbmStatus2       = 0x0da2;
constexpr int      kGfx11GrbmStatus2Base   = 0;
constexpr uint32_t kGfx11CpStat            = 0x0f40;
constexpr int      kGfx11CpStatBase        = 0;

// =========================================================================
// MES 11.0 — substep entry & step-2 getters
// =========================================================================
R3SubstepResult mes_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg*  getMesRows(void);
uint32_t              getMesRowCount(void);
const char*           getMesLayoutTag(void);
const R3WriteRowAbs*  getMesDoorbells(void);
uint32_t              getMesDoorbellCount(void);

// MES register / field constants — for rdna3 dump & teardown inspection.
// Source: amd/mes_v11_0.h (gc_11_0_0_offset.h / sh_mask.h)
constexpr int      kMesBaseIdx                 = 1;
constexpr uint32_t kRegGrbmGfxCntl             = 0x0900;
constexpr uint32_t kRegRlcCpSchedulers         = 0x098a;
constexpr uint32_t kRegMesCntl                 = 0x2807;
constexpr uint32_t kRegMesGp3Lo                = 0x2849;
constexpr uint32_t kRegCpHqdActive             = 0x1fab;
constexpr uint32_t kRegCpGfxHqdActive          = 0x1e80;
constexpr uint32_t kGrbmPipeIdShift            = 0;
constexpr uint32_t kGrbmMeIdShift              = 2;
constexpr uint32_t kMesCntlPipe0Active         = 1u << 26;
constexpr uint32_t kMesCntlPipe1Active         = 1u << 27;
constexpr uint32_t kMesCntlPipe0Reset          = 1u << 16;
constexpr uint32_t kMesCntlPipe1Reset          = 1u << 17;

// =========================================================================
// PSP 13.0 — substep entry & step-2 getters
// =========================================================================
R3SubstepResult psp_v13_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg*    getPspRows(void);
uint32_t                getPspRowCount(void);
const R3WriteRowLayout* getPspLayouts(void);
uint32_t                getPspLayoutCount(void);

// =========================================================================
// SMU 13.0 — substep entry & step-2 getters
// =========================================================================
R3SubstepResult smu_v13_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg*  getSmuRows(void);
uint32_t              getSmuRowCount(void);

// SMU register constants — for rdna3 dump inspection.
// Source: amd/smu_v13_0.h (mp_13_0_4_offset.h, mp_13_0_4_sh_mask.h).
//
// MP1 mailbox registers (mp_13_0_4_offset.h:300-349; BASE_IDX=1 on every one).
// WARNING: MP0 has same-named registers at 0x0082/0x0092/0x009a — using MP0 offsets
// with MP1 base[1] would write 0x16082 instead of 0x16282 (R3 report §2.1).
constexpr int      kSmuMp1BaseIdx    = 1;
constexpr uint32_t kSmuC2pmsg66      = 0x0282;
constexpr uint32_t kSmuC2pmsg82      = 0x0292;
constexpr uint32_t kSmuC2pmsg90      = 0x029a;

// =========================================================================
// SDMA 6.0 — substep entry & step-2 getters
// =========================================================================
R3SubstepResult sdma_v6_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg*  getSdmaRows(void);
uint32_t              getSdmaRowCount(void);
const char*           getSdmaLayoutTag(void);

// SDMA register / field constants — for rdna3 dump & teardown inspection.
// Source: amd/sdma_v6_0.h (gc_11_0_0_offset.h / sh_mask.h)
constexpr uint32_t kRegSdma0StatusReg              = 0x0025;
constexpr uint32_t kRegSdma0Queue0RbCntl           = 0x0080;
constexpr uint32_t kRegSdma0Queue0RbRptr           = 0x0083;
constexpr uint32_t kRegSdma0Queue0RbWptr           = 0x0085;
constexpr uint32_t kRegSdma0F32Cntl                = 0x589a;
constexpr uint32_t kRbCntlRbEnableMask             = 0x00000001u;
constexpr uint32_t kF32CntlHaltMask                = 0x00000001u;

} // namespace amdgpu
