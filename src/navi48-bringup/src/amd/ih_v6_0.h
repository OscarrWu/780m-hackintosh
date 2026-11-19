/*
 * ih_v6_0.h - IH 6.0.1 (OSSSYS) ring bring-up constants + IV entry decoder
 *              (stage 3, S10 "doorbell + IH").
 *
 * Scope:
 *   - D8 IH ring (64 KiB, 8-dword/32-byte entries) + rptr writeback page
 *     (4 KiB). Addresses come from the runtime layout grants ("ih_ring",
 *     "ih_rptr_wb") - never hardcoded.
 *   - Seven-step ring bring-up transcribed from Linux
 *     drivers/gpu/drm/amd/amdgpu/ih_v6_0.c:
 *       RB_CNTL -> RB_BASE/HI -> RB_RPTR -> RB_WPTR ->
 *       RB_WPTR_ADDR_LO/HI -> DOORBELL_RPTR -> enable.
 *   - Pure IV entry decoder, field-for-field from
 *     amdgpu_ih.c:263-296 (amdgpu_ih_decode_iv_helper).
 *
 * Address form: the ring and the writeback page live in the UMA carveout,
 * so the GPU-visible base is the carveout SYSTEM PHYSICAL address
 * (fb_phys_base + offset, "GPA"). This is the Linux use_bus_addr=true path
 * (ih_v6_0.c:339-342 IH_CHICKEN.MC_SPACE_GPA_ENABLE=1, :220 MC_SPACE=2,
 * :274-275 RB_BASE = gpu_addr >> 8). The MC-aperture form has no proven
 * MC_SPACE value in any amdgpu IH driver (INV-A2: never guess a register
 * encoding), and the GPUVA form would need GPUVM mappings this stage does
 * not own.
 *
 * Risk note (task card): nullarc has NO IH implementation anywhere in its
 * tree (R10) - Linux ih_v6_0.c is the sole source for this substep. The
 * doorbell index 0x2F0 is derived (AMDGPU_NAVI10_DOORBELL_IH 0x178 << 1,
 * soc21.c:494-528 + amdgpu_doorbell.h:186-237 + ih_v6_0.c:596) and marked
 * 【推测】: it must be confirmed on real hardware before it is trusted.
 *
 * Polling discipline: this stage runs the IH ring in polling mode
 * (ENABLE_INTR=0, RPTR_REARM=0) - the fence path (B3) never depends on IH.
 * Ring bring-up failure = Fail; a ring that comes up but produces no
 * entries is a gated extra (recorded, not fatal).
 *
 * Provenance (PC-8): stage-3 implementation doc §7; static evidence
 * .trellis/research/阶段3-静态取证.md §2. Register offsets cross-checked
 * against golden_values.h (OSSSYS 6.0.1, BASE_IDX 0).
 */
#ifndef NAVI48_IH_V6_0_H
#define NAVI48_IH_V6_0_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"

namespace amdgpu {

/* ---- ring geometry (D8, rdna3_layout.h) ------------------------------- */

constexpr uint64_t kIhRingBytes   = 64ull * 1024;  /* R3_D8_IH_RING_SIZE */
constexpr uint64_t kIhRptrWbBytes = 4ull * 1024;   /* R3_D8_IH_RPTR_SIZE */
constexpr uint32_t kIhEntryDwords = 8;             /* 32-byte IV entry */
constexpr uint32_t kIhEntryBytes  = kIhEntryDwords * 4u;
constexpr uint32_t kIhRingEntries =
    (uint32_t)(kIhRingBytes / kIhEntryBytes);      /* 2048 */

/* ih_v6_0.c:229 RB_SIZE = order_base_2(ring_size / 4) = order_base_2(16384) */
constexpr uint32_t kIhRbSizeOrder = 14;

/* Writeback page layout (amdgpu_ih.c:74-77: wptr shadow, then rptr shadow;
 * project amdgpu_ih.h IHContext carries the same two shadows). */
constexpr uint64_t kIhWbWptrShadowOff = 0;
constexpr uint64_t kIhWbRptrShadowOff = 4;

/* ---- IH_RB_CNTL fields (osssys_6_0_0_sh_mask.h) ----------------------- */

constexpr uint32_t kIhRbCntlEnable          = 1u << 0;     /* RB_ENABLE */
constexpr uint32_t kIhRbCntlSizeMask        = 0x3Eu;       /* RB_SIZE @1 */
constexpr uint32_t kIhRbCntlSizeShift       = 1;
constexpr uint32_t kIhRbCntlWptrWbEnable    = 1u << 8;     /* WPTR_WRITEBACK_ENABLE */
constexpr uint32_t kIhRbCntlWptrOverflowEn  = 1u << 16;    /* WPTR_OVERFLOW_ENABLE */
constexpr uint32_t kIhRbCntlEnableIntr      = 1u << 17;    /* ENABLE_INTR */
constexpr uint32_t kIhRbCntlMcSnoop         = 1u << 20;    /* MC_SNOOP */
constexpr uint32_t kIhRbCntlRptrRearm       = 1u << 21;    /* RPTR_REARM */
constexpr uint32_t kIhRbCntlMcRo            = 1u << 22;    /* MC_RO */
constexpr uint32_t kIhRbCntlMcVmidMask      = 0xF000000u;  /* MC_VMID @24 */
constexpr uint32_t kIhRbCntlMcVmidShift     = 24;
constexpr uint32_t kIhRbCntlMcSpaceMask     = 0x70000000u; /* MC_SPACE @28 */
constexpr uint32_t kIhRbCntlMcSpaceShift    = 28;
constexpr uint32_t kIhRbCntlWptrOverflowClr = 1u << 31;    /* WPTR_OVERFLOW_CLEAR */

/* MC_SPACE values: 2 = GPA (system physical), 4 = GPUVA (ih_v6_0.c:220).
 * No proven value exists for the MC-aperture form (INV-A2). */
constexpr uint32_t kIhMcSpaceGpa   = 2;
constexpr uint32_t kIhMcSpaceGpuva = 4;

/* IH_CHICKEN (osssys_6_0_0_offset.h:625, BASE_IDX 0; not in the golden
 * table - fallback row form). */
constexpr uint32_t kRegIhChicken = 0x18d;
constexpr uint32_t kIhChickenMcSpaceGpaEnable = 1u << 4; /* MC_SPACE_GPA_ENABLE */

/* Doorbell index: AMDGPU_NAVI10_DOORBELL_IH (0x178) << 1 = 0x2F0.
 * 【推测】derived from soc21.c:494-528 + amdgpu_doorbell.h:186-237 +
 * ih_v6_0.c:596 (doorbell_index = adev->doorbell_index.ih << 1); confirm
 * on real hardware. */
constexpr uint32_t kIhDoorbellIndex = 0x2F0;

/* IH_DOORBELL_RPTR fields (osssys_6_0_0_sh_mask.h). */
constexpr uint32_t kIhDoorbellRptrOffsetMask = 0x03FFFFFFu; /* OFFSET @0 */
constexpr uint32_t kIhDoorbellRptrEnable     = 1u << 28;   /* ENABLE */

/* ---- IV entry decoder (amdgpu_ih.c:263-296) --------------------------- */

struct IhEntry {
    uint32_t client_id;
    uint32_t src_id;
    uint32_t ring_id;
    uint32_t vmid;
    uint32_t vmid_src;
    uint64_t timestamp;
    uint32_t timestamp_src;
    uint32_t pasid;
    uint32_t node_id;
    uint32_t src_data[4];
};

/* Decode one 8-dword IV entry. Pure function; mirrors
 * amdgpu_ih_decode_iv_helper (amdgpu_ih.c:263-296) field for field.
 * rptr/wptr step in BYTES: one entry advances the byte counter by 32. */
void ih_decode_iv(const uint32_t iv[8], IhEntry *entry);

} /* namespace amdgpu */

#endif /* NAVI48_IH_V6_0_H */
