/*
 * mes_v11_0.h - MES 11.0 scheduler power-up substep (stage 2, K8).
 *
 * Scope: unhalt the CP (S0), start the two MES pipes out of the firmware headers PSP
 * already loaded (K4), bring up the KIQ and SCH queues with the direct-HQD form, and
 * run the scheduler API sequence SET_HW_RESOURCES -> QUERY_SCHEDULER_STATUS ->
 * ADD_QUEUE(COMPUTE) -> ADD_QUEUE(GFX). Both ADD_QUEUE forms are attempt-and-record.
 *
 * Address spaces (same two-space trap as K4/K7):
 *   - R3Grant::start/size are BAR0 byte offsets; CPU copies use only BAR0 (B4).
 *   - Register addresses are BAR5 dword offsets (r3_io_read32/write32).
 *   - MES API addresses (MQD, ring, writeback, fences) are MC:
 *     fb_mc_base + grant.start + sub-offset.
 *   - Doorbells are BAR2 dword indices (index * 4 bytes).
 *
 * Register source: gc_11_0_0_offset.h / gc_11_0_0_sh_mask.h.
 * Wire format: mes_v11_api_def.h field order (Linux amdgpu_mes.h / nullarc).
 * Firmware start address: r3_fw_parse_mes() over GC11_MES_2 (pipe 0) and
 * GC11_MES1 (pipe 1) - never a constant (mes_uc_start_addr lives in the header and
 * is consumed raw, already an MC address: Linux mes_v11_0.c:1136-1143).
 * Linux reference: mes_v11_0_enable(), mes_v11_0_queue_init_register(),
 * mes_v11_0_set_hw_resources(), mes_v11_0_submit_pkt_and_poll_completion().
 *
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/mes_v11_0.c (Copyright 2019 Advanced Micro Devices, Inc.,
 * MIT licensed - see NOTICE). Only the register/protocol semantics are reproduced onto
 * the mac-amdgpu (Cobalt64) IP-module conventions; no Linux code is copied verbatim,
 * and every offset is cited from the pinned headers at its use site.
 */
#ifndef NAVI48_MES_V11_0_H
#define NAVI48_MES_V11_0_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"


namespace amdgpu {

/* GC register offsets (base 0 unless noted), gc_11_0_0_offset.h. */

constexpr uint32_t kRegMesPrgrmCntrStart       = 0x2800;
constexpr uint32_t kRegMesPrgrmCntrStartHi     = 0x289d;


/* CP HQD (compute/MEC queue) registers, base 0. */
constexpr uint32_t kRegCpMqdBaseAddr           = 0x1fa9;
constexpr uint32_t kRegCpMqdBaseAddrHi         = 0x1faa;

constexpr uint32_t kRegCpHqdVmid               = 0x1fac;
constexpr uint32_t kRegCpHqdPersistentState    = 0x1fad;
constexpr uint32_t kRegCpHqdPqBase             = 0x1fb1;
constexpr uint32_t kRegCpHqdPqBaseHi           = 0x1fb2;
constexpr uint32_t kRegCpHqdPqRptr             = 0x1fb3;
constexpr uint32_t kRegCpHqdPqRptrReportAddr   = 0x1fb4;
constexpr uint32_t kRegCpHqdPqRptrReportAddrHi = 0x1fb5;
constexpr uint32_t kRegCpHqdPqWptrPollAddr     = 0x1fb6;
constexpr uint32_t kRegCpHqdPqWptrPollAddrHi   = 0x1fb7;
constexpr uint32_t kRegCpHqdPqDoorbellControl  = 0x1fb8;
constexpr uint32_t kRegCpHqdPqControl          = 0x1fba;
constexpr uint32_t kRegCpMqdControl            = 0x1fcb;
constexpr uint32_t kRegCpHqdEopBaseAddr        = 0x1fce;
constexpr uint32_t kRegCpHqdEopBaseAddrHi      = 0x1fcf;
constexpr uint32_t kRegCpHqdEopControl         = 0x1fd0;

/* CP GFX HQD registers, base 0 (S9 state export only). */

constexpr uint32_t kRegCpGfxHqdRptr            = 0x1e88;

/* GFX ring doorbell range (gc0). nullarc gfx_via_mes writes these before ADD_QUEUE. */
constexpr uint32_t kRegCpRbDoorbellRangeLower   = 0x1dfa;
constexpr uint32_t kRegCpRbDoorbellRangeUpper   = 0x1dfb;
constexpr uint32_t kCpHqdDoorbellRangeUpper     = 0x7f8;

/* Register field values used by the queue init (gc_11_0_0_sh_mask.h). */

constexpr uint32_t kGrbmVmidShift              = 4;
constexpr uint32_t kGrbmQueueIdShift           = 8;

constexpr uint32_t kCpHqdPqControlDefault      = 0x00308509u;
constexpr uint32_t kCpHqdPersistentDefault     = 0x0be05501u;
constexpr uint32_t kCpHqdPqDoorbellEn          = 1u << 30;
constexpr uint32_t kCpMqdControlDefault        = 0x00000100u;
constexpr uint32_t kCpHqdEopControlDefault     = 0x00000006u;
constexpr uint32_t kCpHqdEopSizeMask           = 0x0000003fu; /* shift 0 */
constexpr uint32_t kCpHqdEopSizeShift          = 0;
constexpr uint32_t kCpHqdActiveOn              = 1u;
/* CP_ME_CNTL.CE_HALT (bit24). S0 keeps it set; writing 0 hangs PFP on RCIU (nullarc). */
constexpr uint32_t kMesCpMeCntlCeHalt          = 1u << 24;
constexpr uint32_t kMesRlcSchedulerByte        = 0x80u;

/* MES carveout sub-offsets inside the layout's 8 MiB "mes" grant (C5). */
constexpr uint32_t kMesKiqRingOff              = 0x0000u;
constexpr uint32_t kMesKiqWbOff                = 0x1000u;
constexpr uint32_t kMesKiqMqdOff               = 0x2000u;
constexpr uint32_t kMesKiqEopOff               = 0x3000u;
constexpr uint32_t kMesSchRingOff              = 0x4000u;
constexpr uint32_t kMesSchWbOff                = 0x5000u;
constexpr uint32_t kMesSchMqdOff               = 0x6000u;
constexpr uint32_t kMesSchEopOff               = 0x7000u;
constexpr uint32_t kMesGfxRingOff              = 0x8000u;
constexpr uint32_t kMesGfxWbOff                = 0xa000u;
constexpr uint32_t kMesGfxMqdOff               = 0xb000u;
constexpr uint32_t kMesGfxEopOff               = 0xc000u;
constexpr uint32_t kMesCmpRingOff              = 0x10000u;
constexpr uint32_t kMesCmpWbOff                = 0x11000u;
constexpr uint32_t kMesCmpMqdOff               = 0x12000u;
constexpr uint32_t kMesCmpEopOff               = 0x13000u;
constexpr uint32_t kMesRegionBytes             = 0x14000u; /* all of the above + slack */
/* S-1 scratch slots inside the "mes" grant, past the queue area (方案 v3 §6.2.4/§6.5). */
constexpr uint32_t kMesDefaultPageOff          = kMesRegionBytes;       /* 0x14000 */
constexpr uint32_t kMesResource1Off            = kMesRegionBytes + 0x1000u; /* 0x15000 */
constexpr uint32_t kMesScratchBytes            = 0x2000u; /* default page + resource1 */

constexpr uint32_t kMesKiqRingDwords            = 1024u; /* 4 KiB */
constexpr uint32_t kMesSchRingDwords            = 1024u; /* 4 KiB */
constexpr uint32_t kMesGfxRingDwords            = 2048u; /* 8 KiB */
constexpr uint32_t kMesCmpRingDwords            = 1024u; /* 4 KiB */
constexpr uint32_t kMesMqdBytes                 = 0x1000u;
constexpr uint32_t kMesEopBytes                 = 0x800u;

/* MQE writeback sub-offsets (one WB block per queue). */
constexpr uint32_t kMesWbRptrOff                = 0u;
constexpr uint32_t kMesWbWptrOff                = 16u;
constexpr uint32_t kMesWbFenceOff               = 32u;
constexpr uint32_t kMesWbStatusOff              = 64u;
constexpr uint32_t kMesWbSchCtxOff              = 128u;
constexpr uint32_t kMesWbQueryFenceOff          = 136u;

/* Doorbell indices (BAR2 dword index). */
constexpr uint32_t kMesBarDoorbell              = 2u;
constexpr uint32_t kMesDoorbellKiq              = 0x18u;
constexpr uint32_t kMesDoorbellSch              = 0x16u;
constexpr uint32_t kMesDoorbellCompute          = 6u;
constexpr uint32_t kMesDoorbellGfx              = 0x116u;

/* MES API wire constants (mes_v11_api_def.h). */
constexpr uint32_t kMesApiFrameDwords           = 64u;
constexpr uint32_t kMesApiTypeScheduler         = 1u;
constexpr uint32_t kMesApiSetHwRsrc             = 0u;
constexpr uint32_t kMesApiSetHwRsrc1            = 19u;
constexpr uint32_t kMesApiQuerySchedulerStatus  = 11u;
constexpr uint32_t kMesApiAddQueue              = 2u;

constexpr uint32_t kMesQueueTypeGfx             = 0u;
constexpr uint32_t kMesQueueTypeCompute         = 1u;
constexpr uint32_t kMesFlagsDisableReset        = 1u << 0;
constexpr uint32_t kMesFlagsUseDiffVmidCompute  = 1u << 1;
constexpr uint32_t kMesFlagsDisableMesLog       = 1u << 2;
constexpr uint32_t kMesFlagsLevelProcessQuantum = 1u << 6;
constexpr uint32_t kMesFlagsRegActivePoll       = 1u << 10;
/* ADD_QUEUE.flags bit13 (mes_v11_api_def.h enum, after is_aql_queue=bit11). */
constexpr uint32_t kMesAddQueueFlagMapLegacyKq = 1u << 13;

constexpr uint32_t kMesTimeoutUs                = 2100000u; /* 2100 ms, Linux */
constexpr uint32_t kMesPollUs                   = 100u;
constexpr uint32_t kMesMcPrefix                 = 0x80u; /* high dword of the MC form */

/* Wire header: type bits 0-3, opcode 4-11, dwsize 12-19 (little endian). */
inline uint32_t mes_api_header(uint32_t type, uint32_t opcode, uint32_t dwsize)
{
    return (type & 0xfu) | ((opcode & 0xffu) << 4) | ((dwsize & 0xffu) << 12);
}

#pragma pack(push, 4)

struct MesApiStatus {
    uint64_t api_completion_fence_addr;
    uint64_t api_completion_fence_value;
};

struct MesApiSetHwResources {
    uint32_t header;
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint32_t gds_size;
    uint32_t paging_vmid;
    uint32_t compute_hqd_mask[8];
    uint32_t gfx_hqd_mask[2];
    uint32_t sdma_hqd_mask[2];
    uint32_t aggregated_doorbells[5];
    uint64_t g_sch_ctx_gpu_mc_ptr;
    uint64_t query_status_fence_gpu_mc_ptr;
    uint32_t gc_base[8];
    uint32_t mmhub_base[8];
    uint32_t osssys_base[8];
    MesApiStatus api_status;
    uint32_t flags;
    uint32_t oversubscription_timer;
    uint64_t doorbell_info;
    uint64_t event_intr_history_gpu_mc_ptr;
    uint64_t timestamp;
    uint32_t os_tdr_timeout_in_sec;
};

static_assert(sizeof(MesApiSetHwResources) <= kMesApiFrameDwords * 4,
              "SET_HW_RESOURCES frame must fit in ring slot (252 < 256)");

struct MesApiAddQueue {
    uint32_t header;
    uint32_t process_id;
    uint64_t page_table_base_addr;
    uint64_t process_va_start;
    uint64_t process_va_end;
    uint64_t process_quantum;
    uint64_t process_context_addr;
    uint64_t gang_quantum;
    uint64_t gang_context_addr;
    uint32_t inprocess_gang_priority;
    uint32_t gang_global_priority_level;
    uint32_t doorbell_offset;
    uint64_t mqd_addr;
    uint64_t wptr_addr;
    uint64_t h_context;
    uint64_t h_queue;
    uint32_t queue_type;
    uint32_t gds_base;
    uint32_t gds_size;
    uint32_t gws_base;
    uint32_t gws_size;
    uint32_t oa_mask;
    uint64_t trap_handler_addr;
    uint32_t vm_context_cntl;
    uint32_t flags;
    MesApiStatus api_status;
    uint64_t tma_addr;
    uint32_t sch_id;
    uint64_t timestamp;
    uint32_t process_context_array_index;
    uint32_t gang_context_array_index;
    uint32_t pipe_id;
    uint32_t queue_id;
    uint32_t alignment_mode_setting;
    uint64_t unmap_flag_addr;
};

struct MesApiQuerySchedulerStatus {
    uint32_t header;
    uint8_t  mes_healthy;
    uint8_t  pad[3];
    MesApiStatus api_status;
};

/* S5.1: MESAPI_SET_HW_RESOURCES_1 (opcode 19), mirrored on Linux
 * include/mes_v11_api_def.h:256-275 (#pragma pack(4)). api_status sits at 4 here,
 * unlike MesApiQuerySchedulerStatus (8) - the two must not be confused (方案 v3 §6.5). */
struct MesApiSetHwResources1 {
    uint32_t    header;
    MesApiStatus api_status;
    uint64_t    timestamp;
    uint32_t    enable_mes_info_ctx;
    uint64_t    mes_info_ctx_mc_addr;
    uint32_t    mes_info_ctx_size;
    uint64_t    reserved1;
    uint64_t    cleaner_shader_fence_mc_addr;
};

#pragma pack(pop)

static_assert(offsetof(MesApiSetHwResources, api_status) == 200u,
              "MesApiSetHwResources.api_status offset drift");
static_assert(offsetof(MesApiSetHwResources, flags) == 216u,
              "MesApiSetHwResources.flags offset drift");
static_assert(offsetof(MesApiAddQueue, api_status) == 148u,
              "MesApiAddQueue.api_status offset drift");
static_assert(offsetof(MesApiAddQueue, queue_type) == 108u,
              "MesApiAddQueue.queue_type offset drift");
static_assert(offsetof(MesApiQuerySchedulerStatus, api_status) == 8u,
              "MesApiQuerySchedulerStatus.api_status offset drift");
static_assert(sizeof(MesApiSetHwResources1) == 60u,
              "MesApiSetHwResources1 struct size drift");
static_assert(offsetof(MesApiSetHwResources1, api_status) == 4u,
              "SET_HW_RESOURCES_1 api_status offset drift");
static_assert(offsetof(MesApiSetHwResources1, timestamp) == 20u,
              "SET_HW_RESOURCES_1 timestamp offset drift");
static_assert(offsetof(MesApiSetHwResources1, enable_mes_info_ctx) == 28u,
              "SET_HW_RESOURCES_1 enable offset drift");
static_assert(offsetof(MesApiSetHwResources1, mes_info_ctx_mc_addr) == 32u,
              "SET_HW_RESOURCES_1 mc_addr offset drift");
static_assert(offsetof(MesApiSetHwResources1, mes_info_ctx_size) == 40u,
              "SET_HW_RESOURCES_1 size offset drift");
static_assert(offsetof(MesApiSetHwResources1, reserved1) == 44u,
              "SET_HW_RESOURCES_1 reserved1 offset drift");
static_assert(offsetof(MesApiSetHwResources1, cleaner_shader_fence_mc_addr) == 52u,
              "SET_HW_RESOURCES_1 cleaner offset drift");
static_assert(sizeof(MesApiSetHwResources1) <= kMesApiFrameDwords * 4u,
              "SET_HW_RESOURCES_1 must fit the 64-dword API frame");


} /* namespace amdgpu */

#endif /* NAVI48_MES_V11_0_H */
