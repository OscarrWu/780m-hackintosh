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
// 写守卫全链预算契约 — 数量编译期已知（boot5 根因修复，实施蓝本 W-fixplan-2）
// =========================================================================
// 设计意图：每张 IP 声明表的行数是编译期常量（amd/*.cpp 内由 sizeof 反证），
// 全链登记量的"布局+行"预算集中于此作为唯一数字来源：R3_WG_GRANT_MAX 必须
// 不小于全链总预算，否则编译期断言直接失败——而不是像 boot5 那样真机运行到
// S7 第 4 行才表满返回 -2（被折叠成 pre:register）。每一常量由 amd/*.cpp 的
// 反证 static_assert 机械核对（删本地 sizeof 常量处），总和与 R3_WG_GRANT_MAX
// 在此比较。IH 的输入行与拆除行同址共用：拆除行（3 条）覆盖 RB_CNTL/RPTR/WPTR，
// 另 6 条输入行（CHICKEN/RB_BASE/HI/WPTR_ADDR_LO/HI/DOORBELL_RPTR）由
// ih_v6_0.cpp pre() 注册——每条 reg_write 都经 r3_wg_classify（ih_v6_0.cpp:189）
// 需要 grant，故 9 行全部计入 grant 预算。

// ---- 阶段2（prep 生产注册，S2..S11）常量 ----
constexpr uint32_t kPspLayoutCount   = 5u;   // psp_v13_0.cpp kPspLayouts[]（5 项 layout）
constexpr uint32_t kPspRowCount      = 5u;   // psp_v13_0.cpp kPspRows[]（MP0 C2PMSG 5 行）
constexpr uint32_t kSmuRowCount      = 3u;   // smu_v13_0.cpp kSmuRows[]（MP1 C2PMSG 3 行）
constexpr uint32_t kSdmaLayoutCount  = 1u;   // sdma_v6_0.cpp layout:sdma 计 1 条
constexpr uint32_t kSdmaRowCount     = 20u;  // sdma_v6_0.cpp kSdmaRows[]（20 行）
constexpr uint32_t kMesLayoutCount   = 1u;   // mes_v11_0.cpp layout:mes 计 1 条
constexpr uint32_t kMesRowCount      = 74u;  // mes_v11_0.cpp kMesRows[]（52 hub + 10 S0-S5 + 12 HQD）
constexpr uint32_t kMesDoorbellCount = 4u;   // mes_v11_0.cpp kMesDoorbells[]（kiq+sch+cmp+gfx）
constexpr uint32_t kMesQueueRowCount = 11u;  // mes_v11_0.cpp kMesQueueRows[]（2 dbrange+GRBM+5 S11+3 拆除行）

// ---- 阶段2之后（S6..S11 增量）常量 ----
constexpr uint32_t kHdpGrantCount           = 1u;   // stages.cpp reg:hdp-flush 共享窗口 1 条
constexpr uint32_t kGmcSeqLayoutCount       = 6u;   // gmc_v11_0.cpp S6 uma 6 条 layout
constexpr uint32_t kGartRowCount            = 20u;  // gmc_v11_0.cpp kGartRows[]（17 vbind + 2 inv-arm + 1 fault-record）
constexpr uint32_t kGartDoorbellCount       = 1u;   // gmc_v11_0.cpp "reg:gmc-db"（SDMA0 QUEUE0 运行期门铃 1 条）
constexpr uint32_t kVmid8FirstInvalidateRow = 15u;  // S9 只注册前 15 行（15..17 由 S7 已注册）
constexpr uint32_t kVmid8RowCount           = 18u;  // 声明全集（expect 用；15..17 非 S9 注册）
constexpr uint32_t kMesQueueLayoutCount     = 1u;   // mes S11 test_buf layout 计 1 条

// ---- GFX 阶段2/阶段3 声明行（gfx_v11_0.cpp 内由 sizeof 反证） ----
constexpr uint32_t kGfxStage2RowCount = 14u;  // 4 W1.5 + 10 golden (G01-G10)
constexpr uint32_t kGfxStage3RowCount = 6u;   // 计划行（G13 5 + G17 1）；GDS 60 个 per-VMID 实例地址由 stage3 函数按 stride 动态登记，属 deferred 路径，不并入生产预算

// ---- W1 拆除行（teardown-only，不属于阶段2 生产注册；GFX 行由 gfx_v11_0.cpp、IH 行由 ih_v6_0.cpp 注册） ----
constexpr uint32_t kGfxTeardownRowCount = 2u;   // CP_ME_CNTL + CP_MEC_CNTL，gfx_v11_0.cpp "reg:gfx-teardown"

// IH 拆除行由 ih_v6_0.cpp pre() 注册（reg:teardown:ih-*）：关断只写 CNTL/RPTR/WPTR 三个。
constexpr uint32_t kIhTeardownRowCount  = 3u;   // IH_RB_CNTL + IH_RB_RPTR + IH_RB_WPTR，ih_v6_0.cpp "reg:teardown:ih-*"

// IH 输入行由 ih_v6_0.cpp pre() 注册（reg:ih-*）：S10.1/S10.2 真实写点。
constexpr uint32_t kIhInputRowCount     = 6u;   // CHICKEN + RB_BASE/HI + WPTR_ADDR_LO/HI + DOORBELL_RPTR，ih_v6_0.cpp "reg:ih-*"

// ---- 派生合计（与实施蓝本附录 A 账目一一对应） ----
constexpr uint32_t R3_WG_BUDGET_LAYOUT_TOTAL =
    kPspLayoutCount + kSdmaLayoutCount + kMesLayoutCount +
    kGmcSeqLayoutCount + kMesQueueLayoutCount;                       // = 14
constexpr uint32_t R3_WG_BUDGET_REG_TOTAL =
    kPspRowCount + kSmuRowCount + kSdmaRowCount + kMesRowCount +
    kMesDoorbellCount + kMesQueueRowCount + kHdpGrantCount +
    kGartRowCount + kGartDoorbellCount + kVmid8FirstInvalidateRow + kGfxTeardownRowCount +
    kIhTeardownRowCount + kIhInputRowCount +
    kGfxStage2RowCount + kGfxStage3RowCount;                         // = 185
constexpr uint32_t R3_WG_BUDGET_STAGE2 =
    kPspLayoutCount + kPspRowCount + kSmuRowCount + kSdmaLayoutCount +
    kSdmaRowCount + kMesLayoutCount + kMesRowCount + kMesDoorbellCount +
    kMesQueueRowCount;                                               // = 124
constexpr uint32_t R3_WG_BUDGET_TOTAL =
    R3_WG_BUDGET_LAYOUT_TOTAL + R3_WG_BUDGET_REG_TOTAL;              // = 199

static_assert(R3_WG_BUDGET_STAGE2 == 124u, "阶段2 生产注册预算应为 124 项");
static_assert(R3_WG_BUDGET_LAYOUT_TOTAL == 14u, "全链 layout 预算应为 14 项");
static_assert(R3_WG_BUDGET_REG_TOTAL == 185u, "全链 reg 预算应为 185 项");
static_assert(R3_WG_BUDGET_TOTAL == 199u, "全链写守卫预算应为 199 项");
static_assert(R3_WG_BUDGET_TOTAL <= R3_WG_GRANT_MAX,
              "写守卫 grant 容量小于全链预算：请同步 R3_WG_GRANT_MAX");

// expect 集容量契约：单个 IP 的期望集合不得超过 R3_WG_EXPECT_MAX(80)。
static_assert(kMesRowCount <= R3_WG_EXPECT_MAX, "MES 声明行数超过 R3_WG_EXPECT_MAX");
static_assert(kMesDoorbellCount <= R3_WG_EXPECT_MAX, "MES 门铃数超过 R3_WG_EXPECT_MAX");
static_assert(kMesQueueRowCount <= R3_WG_EXPECT_MAX, "MES 队列行数超过 R3_WG_EXPECT_MAX");
static_assert(kGartRowCount <= R3_WG_EXPECT_MAX, "GART 行数超过 R3_WG_EXPECT_MAX");
static_assert(kGartDoorbellCount <= R3_WG_EXPECT_MAX, "GART 门铃数超过 R3_WG_EXPECT_MAX");
static_assert(kVmid8RowCount <= R3_WG_EXPECT_MAX, "VMID8 行数超过 R3_WG_EXPECT_MAX");
static_assert(kPspRowCount <= R3_WG_EXPECT_MAX, "PSP 行数超过 R3_WG_EXPECT_MAX");
static_assert(kPspLayoutCount <= R3_WG_EXPECT_MAX, "PSP layout 数超过 R3_WG_EXPECT_MAX");
static_assert(kSmuRowCount <= R3_WG_EXPECT_MAX, "SMU 行数超过 R3_WG_EXPECT_MAX");
static_assert(kGfxStage2RowCount <= R3_WG_EXPECT_MAX, "GFX 阶段2 行数超过 R3_WG_EXPECT_MAX");
static_assert(kGfxStage3RowCount <= R3_WG_EXPECT_MAX, "GFX 阶段3 行数超过 R3_WG_EXPECT_MAX");

// =========================================================================
// GFX 11.0.1 — substep entry (read-only)
// =========================================================================
R3SubstepResult gfx_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
constexpr uint32_t kGfxRowCount = kGfxStage2RowCount + kGfxStage3RowCount;

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
// CP_MEC_CNTL (S11 T-E.6 inverse of the MEC power-up). MEC_ME2_HALT bit28,
// MEC_ME1_HALT bit30 (gc_11_0_0_sh_mask.h:23976-24001).
constexpr uint32_t kGfx11CpMecCntl         = 0x0802;
constexpr int      kGfx11CpMecCntlBase     = 1;
constexpr uint32_t kGfx11CpMecCntlMe2Halt  = 0x10000000u;
constexpr uint32_t kGfx11CpMecCntlMe1Halt  = 0x40000000u;

// =========================================================================
// MES 11.0 — substep entry & step-2 getters
// =========================================================================
R3SubstepResult mes_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
const R3WriteRowReg*  getMesRows(void);
uint32_t              getMesRowCount(void);
const R3WriteRowReg*  getMesQueueRows(void);
uint32_t              getMesQueueRowCount(void);
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

// Teardown-only MES control fields (tech plan §9 T-E.3/T-E.4; gc_11_0_0_sh_mask.h
// CP_MES_CNTL). The HQD offsets live in mes_v11_0.h; only the field bits the inverse
// sequence needs are declared here.
constexpr uint32_t kCpHqdPqDoorbellHit          = 1u << 31;
constexpr uint32_t kMesCntlInvalidateIcache     = 1u << 4;
constexpr uint32_t kMesCntlHalt                 = 1u << 30;

// Teardown-only IH bits (tech plan §9 T-E.7). Linux amdgpu_ih_toggle_ring_interrupts(..., false)
// clears exactly RB_ENABLE (ih_v6_0.c:137) and ENABLE_INTR (ih_v6_0.c:168, ring0 only) in one
// RMW of IH_RB_CNTL, then zeroes RPTR/WPTR (ih_v6_0.c:181-182). Bit positions:
// oss_3_0_sh_mask.h:737 (RB_ENABLE__SHIFT=0) and :67-68 (ENABLE_INTR_MASK=0x20000, SHIFT=17).
constexpr uint32_t kIhRbCntlRbEnable            = 1u << 0;
constexpr uint32_t kIhRbCntlEnableIntr          = 1u << 17;

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
constexpr uint32_t kRegSdma0RbRptrFetch            = 0x0020;  // gc_11_0_0_offset.h:48，BASE_IDX 0
constexpr uint32_t kRegSdma0RbRptrFetchHi          = 0x0021;  // gc_11_0_0_offset.h:50，BASE_IDX 0
constexpr uint32_t kRbCntlRbEnableMask             = 0x00000001u;
constexpr uint32_t kF32CntlHaltMask                = 0x00000001u;
// =========================================================================
// IH 6.0 — substep entry (doorbell + interrupt handler init)
// =========================================================================
R3SubstepResult ih_v6_0_substep(const R3Io *io, const R3SubstepEnv *env);

// =========================================================================
// GMC 11.0 — substep entries (uma/gart/gpuvm0/vmid8)
// =========================================================================
R3SubstepResult uma_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult gart_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult gpuvm0_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult vmid8_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);

// =========================================================================
// MES 11.0 queue — substep entry (queue setup portion, split from mes_v11_0)
// =========================================================================
R3SubstepResult mes_queue_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);

// =========================================================================
// Compute 11.0 — substep entry (S13 compute magic)
// =========================================================================
R3SubstepResult compute_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);

} // namespace amdgpu
