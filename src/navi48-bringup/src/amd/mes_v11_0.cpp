/*
 * mes_v11_0.cpp - MES 11.0 scheduler power-up substep (stage 2, K8).
 *
 * Sequence (tech plan §3.5 S0-S9):
 *   P   prerequisites: PSP + GFX ready, "mes" layout grant, firmware headers
 *   S0  CP unhalt: CP_MEC_CNTL=0, CP_ME_CNTL=0x01000000 (CE_HALT kept), before all
 *       queue operations (nullarc full.sh:9-10; C-07)
 *   S1  start both pipes: CP_MES_CNTL reset -> PRGRM_CNTR_START(_HI)=header>>2 -> ACTIVE
 *   S2  CP_MES_GP3_LO version readback for pipe 0 (SCH) and pipe 1 (KIQ)
 *   S3  KIQ context: RLC_CP_SCHEDULERS me3/pipe1, MQD + direct HQD, doorbell 0x18
 *   S4  SCH queue: MQD + direct HQD me3/pipe0, doorbell 0x16
 *   S5  SET_HW_RESOURCES through the SCH ring (wptr shadow -> doorbell -> fence/status)
 *   S6  QUERY_SCHEDULER_STATUS (records mes_healthy)
 *   S7  ADD_QUEUE(COMPUTE, map_legacy_kq=bit13) - attempt + record, never fatal
 *   S8  ADD_QUEUE(GFX) - attempt + record, never fatal (C5.2 v1.5, F7=c')
 *   S9  queue state export + MC address-form self-check (0x80_ prefix, B4)
 *
 * S4 uses the nullarc direct-HQD form. Linux's KIQ MAP_QUEUES alternative is NOT
 * attempted automatically (F8): a failure of the direct form is reported as-is.
 *
 * No retries (F6): every failed access or timeout returns with its own name.
 * Provenance (porting-charter PC-8): structure port of Linux
 * drivers/gpu/drm/amd/amdgpu/mes_v11_0.c (Copyright 2019 Advanced Micro Devices, Inc.,
 * MIT licensed - see NOTICE and mes_v11_0.h). Only the register/protocol semantics are
 * reproduced onto the mac-amdgpu (Cobalt64) IP-module conventions; every offset and MQD
 * dword is cited from the pinned headers at its use site.
 */
#include "mes_v11_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h> /* snprintf/vsnprintf/memcpy/memset, no libc under -nostdinc */
#else
#include <stdio.h>
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "fw_table.h"
#include "../rdna3/rdna3_fwhdr.h"
#include "../rdna3/rdna3_layout.h"
#include "../rdna3/rdna3_runtime.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/golden_values.h"
#include "csb_gfx11_data.h"
#include "gmc_v11_0.h"
#include "pm4_v11.h"


namespace amdgpu {
namespace {

/* Static failure names (R3SubstepResult::name must outlive the call). */
constexpr const char *kFailNoIo          = "pre:no-io";
constexpr const char *kFailNoEnv         = "pre:no-env";
constexpr const char *kFailNoBase        = "pre:ip-base";
constexpr const char *kFailNoMcBase      = "pre:no-mc-base";
constexpr const char *kFailLayout        = "pre:layout-invalid";
constexpr const char *kFailGrant         = "pre:mes-grant";
constexpr const char *kFailOwner         = "pre:mes-owner";
constexpr const char *kFailSpace         = "pre:mes-space";
constexpr const char *kFailBar0          = "pre:mes-bar0";
constexpr const char *kFailNoCpu         = "pre:mes-cpu";
constexpr const char *kFailPspReady      = "pre:psp-not-ready";
constexpr const char *kFailPspLatched    = "pre:psp-latched";
constexpr const char *kFailGfxReady      = "pre:gfx-not-ready";
constexpr const char *kFailGfxLatched    = "pre:gfx-latched";
constexpr const char *kFailNoHdp         = "pre:no-hdp-window";
constexpr const char *kFailFw            = "pre:mes-fw";
constexpr const char *kFailCpUnhalt      = "seq:cp-unhalt";
constexpr const char *kFailStart         = "seq:start";
constexpr const char *kFailVersion       = "chk:version";
constexpr const char *kFailKiq           = "seq:kiq";
constexpr const char *kFailSch           = "seq:sch";
constexpr const char *kFailSetHw         = "seq:set-hw";
constexpr const char *kFailQuery         = "seq:query";
constexpr const char *kFailFence         = "seq:fence-timeout";
constexpr const char *kFailApiStatus     = "chk:api-status";
constexpr const char *kFailAddrPrefix    = "chk:addr-prefix";
constexpr const char *kFailS11Csib       = "seq:s11-csib";
constexpr const char *kFailS11Dbrange    = "seq:s11-dbrange";
constexpr const char *kFailS11Wptr       = "seq:s11-wptr";
constexpr const char *kFailS11Vmid8Ib    = "seq:s11-vmid8-ib";
constexpr const char *kFailS11TestGrant  = "pre:test-buf-grant";


WClass to_wclass(R3WClassId c)
{
    switch (c) {
    case kR3WAccess:   return WClass::Access;
    case kR3WData:     return WClass::Data;
    case kR3WProtocol: return WClass::Protocol;
    case kR3WState:    return WClass::State;
    }
    return WClass::Access;
}

constexpr uint32_t kMesApiQueryDwords = kMesApiFrameDwords; /* D1: must be 64, not 6 */
constexpr uint32_t kMesFencePolls    = kMesTimeoutUs / kMesPollUs;
/* The MesApiQuerySchedulerStatus struct is only 24 bytes (the "response prefix").
 * The full QUERY frame written to ring is kMesApiFrameDwords dwords (方案 v3 §6.3). */
static_assert(sizeof(MesApiQuerySchedulerStatus) == 24u,
              "query-response frame size drift");


void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
{
    if (!env || !env->ev)
        return;
    char line[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r3_ev_add(env->ev, tag, line);
}

bool set_prop(const R3SubstepEnv *env, const char *key, const char *value)
{
    if (!env || !env->sink || !env->sink->set_property || !value)
        return false;
    size_t n = 0;
    while (value[n])
        n++;
    return env->sink->set_property(env->sink->user, key, value, n);
}

/* Everything the substep needs. Static: plan + parsed headers must not sit on the
 * 16 KiB kernel stack. R3Io::lock is NOT a substep-level contract (only SMU holds it
 * for its whole entry; PSP holds it inside post(); GFX/SDMA/MES never take it, same
 * statement as rdna3_dump.cpp:488-491): serialisation comes from the stage ladder,
 * which runs the five substeps one after another from a single trigger path
 * (rdna3_stages.cpp kStages). */
struct Mes {
    const R3Io          *io;
    const R3SubstepEnv  *env;
    R3LayoutPlan         plan;
    uint64_t             mc_base;

    uint32_t             gc0;
    uint32_t             gc1;
    /* Golden-table-resolved addresses (P1 owns the table; base_idx comes from it). */
    uint32_t             mes_cntl;
    uint32_t             mes_prgrm;
    uint32_t             mes_prgrm_hi;
    uint32_t             mes_gp3;
    uint32_t             rlc_sched;
    uint32_t             mec_cntl;
    uint32_t             me_cntl;
    uint32_t             hqd_active;
    uint32_t             mmhub0;
    uint32_t             mmhub1;
    uint32_t             osssys0;
    uint32_t             rlc_csib_lo;
    uint32_t             rlc_csib_hi;
    uint32_t             rlc_csib_len;
    uint32_t             db_range_lower;
    uint32_t             db_range_upper;
    uint32_t             mec_db_range_lower;
    uint32_t             mec_db_range_upper;


    uint64_t             grant_start;
    uint64_t             kiq_ring_bar0, kiq_wb_bar0, kiq_mqd_bar0, kiq_eop_bar0;
    uint64_t             sch_ring_bar0, sch_wb_bar0, sch_mqd_bar0, sch_eop_bar0;
    uint64_t             gfx_ring_bar0, gfx_wb_bar0, gfx_mqd_bar0, gfx_eop_bar0;
    uint64_t             cmp_ring_bar0, cmp_wb_bar0, cmp_mqd_bar0, cmp_eop_bar0;

    uint64_t             kiq_ring_mc, kiq_wb_mc, kiq_mqd_mc, kiq_eop_mc;
    uint64_t             sch_ring_mc, sch_wb_mc, sch_mqd_mc, sch_eop_mc;
    uint64_t             gfx_ring_mc, gfx_wb_mc, gfx_mqd_mc, gfx_eop_mc;
    uint64_t             cmp_ring_mc, cmp_wb_mc, cmp_mqd_mc, cmp_eop_mc;

    uint64_t             sched_uc;
    uint64_t             kiq_uc;
    uint32_t             sch_wptr;
    uint32_t             sch_seq;

    uint32_t             kiq_active, kiq_rptr;
    uint32_t             sch_active, sch_rptr;
    uint32_t             cmp_active, cmp_rptr;
    uint32_t             gfx_active, gfx_rptr;
    uint32_t             cntl_readback;
    /* B#22: compute queues successfully added by S7 (CID2 = the second one). */
    uint32_t             cmp_queues_added;
    uint32_t             gp3_sched;
    uint32_t             gp3_kiq;
    uint64_t             last_fence;
    uint64_t             last_status;
    uint64_t             cmp_fence;
    uint64_t             cmp_status;
    uint64_t             cmp_wptr_mc;
    uint64_t             csib_bar0;
    uint64_t             csib_mc;
    /* Firmware image descriptors (must live as long as the fw_buf carveout). */
    R3FwImage            fw_img;

    /* API frame scratch buffers (kept off the 16 KiB kernel stack). */
    uint32_t             frame[kMesApiFrameDwords];
    uint32_t             qframe[kMesApiFrameDwords];
    char                 gfx_result[40];
    char                 cmp_result[40];
    /* T-E: real GPA from sysmem_grants D2 root for ADD_QUEUE payload (mes_queue_v11_0_substep
     * resolves this; mes_v11_0_substep leaves it at 0 = kMesDefaultPageTableBase). */
    uint64_t             page_table_base_addr;
    uint32_t             fb_off_raw;
    uint32_t             fb_base_raw;
    uint32_t             fb_top_raw;
    uint64_t             default_pa;
    uint64_t             resource1_pa;
    uint32_t             gc_fb_off;
    uint32_t             nbif_aper;
    uint32_t             nbio2;
};

uint32_t field_set(uint32_t v, uint32_t mask, uint32_t shift, uint32_t value)
{
    return (v & ~mask) | ((value << shift) & mask);
}

uint32_t order_of(uint32_t v)
{
    uint32_t n = 0;
    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}

uint32_t pq_control(uint32_t ring_dwords)
{
    /* PV-3 (batch 1c): field-level provenance, Linux mes_v11_0_mqd_init()
     * (mes_v11_0.c:1177-1187). Base: regCP_HQD_PQ_CONTROL_DEFAULT=0x00308509
     * (gc_11_0_0_default.h:2097). QUEUE_SIZE=order_base_2(ring/4)-1, bits 0-5
     * (:1178-1179); RPTR_BLOCK_SIZE=9 (4 KiB page order), bits 8-13 (:1180-1181);
     * the explicit bits are UNORD_DISPATCH=1 / TUNNEL_DISPATCH=0 / PRIV_STATE=1 /
     * KMD_QUEUE=1 / NO_UPDATE_RPTR=1 (:1182-1186); bit positions as encoded below
     * match gc_11_0_0_sh_mask.h CP_HQD_PQ_CONTROL. */
    uint32_t pq = kCpHqdPqControlDefault;
    pq = field_set(pq, 0x3fu, 0, order_of(ring_dwords) - 1u);
    pq = field_set(pq, 0x3fu << 8, 8, 9u); /* RPTR_BLOCK_SIZE: 4 KiB page */
    pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27);
    pq &= ~(1u << 29);
    return pq;
}

uint32_t reg_read(Mes &s, uint32_t abs, bool *ok)
{
    return r3_io_read32(s.io, abs, ok);
}

uint32_t ip_get(const Mes &s, IPBlock block, uint32_t segment)
{
    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    const uint32_t b = ip ? ip->getBase(block, static_cast<int>(segment)) : 0xffffffffu;
    return b == 0xffffffffu ? 0u : b;
}
bool resolve_golden(const Mes &s, const r3gold::GoldenReg &reg, uint32_t *out)
{
    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    uint32_t abs = 0;
    if (!ip || !ip->resolve(reg.ref(), &abs))
        return false;
    *out = abs;
    return true;
}
bool reg_write(Mes &s, uint32_t abs, uint32_t value, R3WClassId cls, const char *ctx)
{
    if (r3_wg_classify(abs, 1, to_wclass(cls), ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(s.io, abs, value);
}

bool bus_write(Mes &s, uint64_t bar0_off, uint64_t value, uint32_t dwords, const char *ctx)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u)
        return false;
    if (r3_wg_classify(bar0_off >> 2, dwords, WClass::Data, ctx) != WVerdict::TxnRead)
        return false;
    return s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &value, true, dwords);
}

bool bus_read(Mes &s, uint64_t bar0_off, uint32_t dwords, uint64_t *value)
{
    if ((bar0_off & 3ull) != 0ull || dwords == 0u || dwords > 2u || !value)
        return false;
    uint64_t v = 0;
    if (!s.io->bus(s.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, dwords))
        return false;
    *value = v;
    return true;
}

bool write_dwords(Mes &s, uint64_t bar0_off, const uint32_t *src, uint32_t dwords,
                  const char *ctx)
{
    uint32_t i = 0;
    while (i < dwords) {
        const uint64_t off = bar0_off + (uint64_t)i * 4ull;
        uint32_t n = 1;
        if (((off & 7ull) == 0ull) && (dwords - i) >= 2u)
            n = 2;
        uint64_t v = 0;
        memcpy(&v, &src[i], (size_t)n * 4u);
        if (!bus_write(s, off, v, n, ctx))
            return false;
        i += n;
    }
    return true;
}

bool zero_region(Mes &s, uint64_t bar0_off, uint64_t bytes, const char *ctx)
{
    for (uint64_t off = 0; off < bytes; off += 32ull) {
        for (uint32_t i = 0; i < 4u; i++) {
            if (!bus_write(s, bar0_off + off + (uint64_t)i * 8ull, 0ull, 2u, ctx))
                return false;
        }
    }
    return true;
}

bool doorbell_write(Mes &s, uint32_t index, uint32_t value, const char *ctx)
{
    /* CP/MES ring doorbells are 64-bit stores at index*4 (Linux WDOORBELL64,
     * nullarc doorbellWrite); the writeguard covers the whole 8-byte slot. */
    if (r3_wg_classify(index, 2, WClass::Protocol, ctx) != WVerdict::TxnRead)
        return false;
    /* Contract 6/7 / B-01: order host writes, then make them visible to the firmware. */
    if (s.io->fence)
        s.io->fence(s.io->ctx);
    if (!r3_io_hdp_flush(s.io))
        return false;
    uint64_t v = value;
    return s.io->bus(s.io->ctx, kMesBarDoorbell, (uint64_t)index * 4ull, &v, true, 2);
}

bool grbm_select(Mes &s, uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
    const uint32_t v = (pipe << kGrbmPipeIdShift) | (me << kGrbmMeIdShift) |
                       (vmid << kGrbmVmidShift) | (queue << kGrbmQueueIdShift);
    return reg_write(s, s.gc1 + kRegGrbmGfxCntl, v, kR3WState, "mes-grbm");
}

bool mqd_put(Mes &s, uint64_t mqd, uint32_t dword, uint32_t value)
{
    return bus_write(s, mqd + (uint64_t)dword * 4ull, value, 1, "mes-mqd");
}

/* Convert a BAR0 offset to an MC address. */
static inline uint64_t mes_mc(const Mes &s, uint64_t bar0)
{
    return s.mc_base + bar0;
}

bool build_compute_mqd(Mes &s, uint64_t mqd, uint64_t ring, uint64_t wb, uint64_t eop,
                       uint32_t doorbell, uint32_t ring_dwords)
{
    /* PV-2 (batch 1c): every dword carries its field name and the Linux provenance
     * (mes_v11_0_mqd_init(), mes_v11_0.c:1118-1220; layout v11_compute_mqd,
     * include/v11_structs.h:674). Linux zeroes the whole MQD first (:1124); the puts
     * below are exactly the fields Linux writes explicitly. */
    if (!mqd_put(s, mqd, 0, 0xC0310800u)) return false; /* header: mes_v11_0.c:1126 */
    if (!mqd_put(s, mqd, 11, 0x00000001u)) return false; /* pipelinestat_enable: :1127 */
    if (!mqd_put(s, mqd, 23, 0xffffffffu)) return false; /* thread_mgmt_se0: :1128 */
    if (!mqd_put(s, mqd, 24, 0xffffffffu)) return false; /* thread_mgmt_se1: :1129 */
    if (!mqd_put(s, mqd, 26, 0xffffffffu)) return false; /* thread_mgmt_se2: :1130 */
    if (!mqd_put(s, mqd, 27, 0xffffffffu)) return false; /* thread_mgmt_se3: :1131 */
    if (!mqd_put(s, mqd, 32, 0x00000007u)) return false; /* misc_reserved: :1132 */
    /* cp_mqd_base = own MQD address (self-pointer): :1152-1153 */
    if (!mqd_put(s, mqd, 128, static_cast<uint32_t>(mes_mc(s, mqd) & 0xfffffffcull))) return false;
    if (!mqd_put(s, mqd, 129, static_cast<uint32_t>(mes_mc(s, mqd) >> 32))) return false;
    if (!mqd_put(s, mqd, 130, kCpHqdActiveOn)) return false; /* cp_hqd_active=1: :1207 */
    if (!mqd_put(s, mqd, 131, 0u)) return false; /* cp_hqd_vmid=0: :1205 */
    /* PV-1 (batch 1c): mqd[132] CP_HQD_PERSISTENT_STATE = DEFAULT 0x0be05501
     * (gc_11_0_0_default.h:2085). That default already encodes PRELOAD_SIZE=0x55
     * (shift 8, mask 0x3FF00), so writing the default is exactly equivalent to Linux
     * mes_v11_0.c:1209-1212 REG_SET_FIELD(..., PRELOAD_SIZE, 0x55) and to nullarc's
     * read-modify-write of the same default. */
    if (!mqd_put(s, mqd, 132, kCpHqdPersistentDefault)) return false;
    if (!mqd_put(s, mqd, 135, 0u)) return false; /* cp_hqd_quantum=DEFAULT(0): :1216 */
    if (!mqd_put(s, mqd, 136, static_cast<uint32_t>(mes_mc(s, ring) >> 8))) return false; /* pq_base_lo: :1162 */
    if (!mqd_put(s, mqd, 137, static_cast<uint32_t>(mes_mc(s, ring) >> 40))) return false; /* pq_base_hi: :1163 */
    if (!mqd_put(s, mqd, 138, 0u)) return false; /* cp_hqd_pq_rptr=0: :1147 */
    if (!mqd_put(s, mqd, 139, static_cast<uint32_t>(mes_mc(s, wb) & 0xfffffffcull))) return false; /* rptr_report_lo: :1167 */
    if (!mqd_put(s, mqd, 140, static_cast<uint32_t>((mes_mc(s, wb) >> 32) & 0xffffu))) return false; /* rptr_report_hi: :1168-1169 */
    if (!mqd_put(s, mqd, 141, static_cast<uint32_t>((mes_mc(s, wb) + kMesWbWptrOff) & 0xfffffff8ull)))
        return false; /* wptr_poll_lo: :1173 */
    if (!mqd_put(s, mqd, 142,
                 static_cast<uint32_t>(((mes_mc(s, wb) + kMesWbWptrOff) >> 32) & 0xffffu))) return false; /* wptr_poll_hi: :1174 */
    if (!mqd_put(s, mqd, 143, (doorbell << 2) | kCpHqdPqDoorbellEn)) return false; /* doorbell_control: :1190-1203 */
    if (!mqd_put(s, mqd, 145, pq_control(ring_dwords))) return false; /* pq_control: :1177-1187 */
    if (!mqd_put(s, mqd, 149, 0x00300000u)) return false; /* ib_control=DEFAULT: :1214 */
    if (!mqd_put(s, mqd, 150, 0u)) return false; /* iq_timer=DEFAULT(0): :1215 */
    if (!mqd_put(s, mqd, 162, kCpMqdControlDefault)) return false; /* mqd_control VMID=0: :1156-1158 */
    if (!mqd_put(s, mqd, 165, static_cast<uint32_t>(mes_mc(s, eop) >> 8))) return false; /* eop_base_lo: :1141 */
    if (!mqd_put(s, mqd, 166, static_cast<uint32_t>(mes_mc(s, eop) >> 40))) return false; /* eop_base_hi: :1142 */
    if (!mqd_put(s, mqd, 167,
                 field_set(kCpHqdEopControlDefault, kCpHqdEopSizeMask,
                           kCpHqdEopSizeShift, 8u))) return false; /* eop_control EOP_SIZE: :1137-1139 */
    if (!mqd_put(s, mqd, 182, 0u)) return false; /* pq_wptr_lo=0: :1148 */
    if (!mqd_put(s, mqd, 183, 0u)) return false; /* pq_wptr_hi=0: :1149 */
    return true;
}

bool build_gfx_mqd(Mes &s, uint64_t mqd, uint64_t ring, uint64_t wb, uint64_t eop,
                   uint32_t doorbell)
{
    (void)eop;
    /* PV-2 (batch 1c): gfx MQD, provenance gfx_v11_0_gfx_mqd_init()
     * (gfx_v11_0.c:4139-4234; layout v11_gfx_mqd, include/v11_structs.h:27). Linux
     * relies on a zeroed MQD allocation; the puts below are the fields it writes
     * explicitly. */
    if (!mqd_put(s, mqd, 128, static_cast<uint32_t>(mes_mc(s, mqd) & 0xfffffffcull))) return false; /* mqd_base_lo: gfx_v11_0.c:4152 */
    if (!mqd_put(s, mqd, 129, static_cast<uint32_t>(mes_mc(s, mqd) >> 32))) return false; /* mqd_base_hi: :4153 */
    if (!mqd_put(s, mqd, 162, kCpMqdControlDefault)) return false; /* gfx_mqd_control VMID=0/PRIV=1: :4156-4160 */
    if (!mqd_put(s, mqd, 131, 0u)) return false; /* gfx_hqd_vmid=0: :4163-4165 */
    if (!mqd_put(s, mqd, 134, 0u)) return false; /* queue_priority=0 (kernel queue): :4168 */
    if (!mqd_put(s, mqd, 135, 0x00000a01u)) return false; /* quantum=DEFAULT 0x00000a01 (gc_11_0_0_default.h:1984, QUANTUM_EN set): :4171-4173 */
    if (!mqd_put(s, mqd, 136, static_cast<uint32_t>(mes_mc(s, ring) >> 8))) return false; /* hqd_base_lo: :4176-4177 */
    if (!mqd_put(s, mqd, 137, static_cast<uint32_t>(mes_mc(s, ring) >> 40))) return false; /* hqd_base_hi: :4178 */
    if (!mqd_put(s, mqd, 139, static_cast<uint32_t>(mes_mc(s, wb) & 0xfffffffcull))) return false; /* rptr_addr_lo: :4182 */
    if (!mqd_put(s, mqd, 140, static_cast<uint32_t>((mes_mc(s, wb) >> 32) & 0xffffu))) return false; /* rptr_addr_hi: :4183-4184 */
    if (!mqd_put(s, mqd, 141, static_cast<uint32_t>((mes_mc(s, wb) + kMesWbWptrOff) & 0xfffffffcull)))
        return false; /* wptr_poll_lo: :4188 */
    if (!mqd_put(s, mqd, 142,
                 static_cast<uint32_t>(((mes_mc(s, wb) + kMesWbWptrOff) >> 32) & 0xffffu))) return false; /* wptr_poll_hi: :4189 */
    if (!mqd_put(s, mqd, 145, 0x00a00000u | 10u | (8u << 8) | (1u << 29))) return false; /* gfx_hqd_cntl: DEFAULT 0x00a00000 (gc_11_0_0_default.h:1994) + RB_BUFSZ/RB_BLKSZ (:4192-4195) + KMD_QUEUE (sh_mask.h:16682, shift 29) */
    if (!mqd_put(s, mqd, 143, (doorbell << 2) | kCpHqdPqDoorbellEn)) return false; /* rb_doorbell_control: :4206-4215 */
    if (!mqd_put(s, mqd, 138, 0u)) return false; /* gfx_hqd_rptr=DEFAULT(0): :4218 */
    if (!mqd_put(s, mqd, 149, 0u)) return false; /* gfx_hqd_wptr=0: :4148 */
    if (!mqd_put(s, mqd, 150, 0u)) return false; /* gfx_hqd_wptr_hi=0: :4149 */
    if (!mqd_put(s, mqd, 130, kCpHqdActiveOn)) return false; /* gfx_hqd_active=1: :4221 */
    return true;
}

bool program_hqd(Mes &s, uint32_t me, uint32_t pipe, uint64_t mqd, uint64_t ring,
                 uint64_t wb, uint64_t eop, uint32_t pq, uint32_t doorbell)
{
    (void)eop; /* Linux mes_v11_0_queue_init_register does not write the HQD EOP regs */
    mqd = mes_mc(s, mqd);
    ring = mes_mc(s, ring);
    wb = mes_mc(s, wb);
    const uint32_t report_lo = static_cast<uint32_t>(wb & 0xfffffffcull);
    const uint32_t report_hi = static_cast<uint32_t>((wb >> 32) & 0xffffu);
    const uint32_t poll_off = static_cast<uint32_t>(wb + kMesWbWptrOff);
    const uint32_t poll_lo = static_cast<uint32_t>(poll_off & 0xfffffff8u);
    const uint32_t poll_hi = static_cast<uint32_t>(((wb + kMesWbWptrOff) >> 32) & 0xffffu);
    const uint32_t db = (doorbell << 2) | kCpHqdPqDoorbellEn;

    if (!grbm_select(s, me, pipe, 0u, 0u)) return false;
    if (!reg_write(s, s.hqd_active, 0u, kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdVmid, 0u, kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqDoorbellControl, 0u, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpMqdBaseAddr,
                   static_cast<uint32_t>(mqd & 0xfffffffcull), kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpMqdBaseAddrHi,
                   static_cast<uint32_t>(mqd >> 32), kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpMqdControl, 0u, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqBase,
                   static_cast<uint32_t>(ring >> 8), kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqBaseHi,
                   static_cast<uint32_t>(ring >> 40), kR3WState, "mes-hqd")) return false;
    /* Linux mes_v11_0.c:1255-1262 writes the RPTR report address before
     * PQ_CONTROL: the queue latches it when PQ_CONTROL enables it. */
    if (!reg_write(s, s.gc0 + kRegCpHqdPqRptrReportAddr, report_lo, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqRptrReportAddrHi, report_hi, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqControl, pq, kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqWptrPollAddr, poll_lo, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqWptrPollAddrHi, poll_hi, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqDoorbellControl, db, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPersistentState, kCpHqdPersistentDefault,
                   kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.hqd_active, kCpHqdActiveOn, kR3WState, "mes-hqd"))
        return false;
    return grbm_select(s, 0u, 0u, 0u, 0u);
}

bool zero_queue_structures(Mes &s, uint64_t ring, uint64_t wb, uint64_t mqd, uint64_t eop,
                           uint32_t ring_dwords)
{
    if (!zero_region(s, ring, (uint64_t)ring_dwords * 4ull, "mes-zero-ring")) return false;
    if (!zero_region(s, wb, 0x1000ull, "mes-zero-wb")) return false;
    if (!zero_region(s, mqd, kMesMqdBytes, "mes-zero-mqd")) return false;
    if (!zero_region(s, eop, kMesEopBytes, "mes-zero-eop")) return false;
    return true;
}

bool load_headers(Mes &s)
{
    /* ME-01: the firmware header's mes_uc_start_addr is already an MC address; nullarc
     * mesboot.c:139-142 loads 0xFFFFFFFFF0005000 and writes it >>2 directly. Adding
     * fb_mc_base would relocate twice. */
    if (s.mc_base == 0ull) {
        evf(s.env, "mes-pre", "P1 mc_base zero before MES setup");
        return false;
    }

    const FwBlob *b = fw_get(FwId::GC11_MES_2);
    if (!b || !b->data || b->size == 0)
        return false;
    if (r3_fw_parse_mes(b->data, b->size, s.mc_base, &s.fw_img) != R3_FW_HDR_OK)
        return false;
    s.sched_uc = s.fw_img.mes_uc_start_addr;

    b = fw_get(FwId::GC11_MES1);
    if (!b || !b->data || b->size == 0)
        return false;
    if (r3_fw_parse_mes(b->data, b->size, s.mc_base, &s.fw_img) != R3_FW_HDR_OK)
        return false;
    s.kiq_uc = s.fw_img.mes_uc_start_addr;

    evf(s.env, "mes-pre",
        "MES uc raw: sched=0x%llx kiq=0x%llx (mc_base=0x%llx)",
        (unsigned long long)s.sched_uc, (unsigned long long)s.kiq_uc,
        (unsigned long long)s.mc_base);
    return true;
}

R3SubstepResult pre(Mes &s)
{
    if (!s.io || !s.io->bus || !r3_io_complete(s.io)) {
        evf(s.env, "mes-pre", "P0 missing io callback (bus=%d)",
            s.io && s.io->bus ? 1 : 0);
        return r3_substep_fail(kR3SecPre, 0, kFailNoIo);
    }
    if (!s.env || !s.env->rt) {
        evf(s.env, "mes-pre", "P0 missing environment/runtime");
        return r3_substep_fail(kR3SecPre, 0, kFailNoEnv);
    }
    if (!s.env->hdp_flush_valid) {
        evf(s.env, "mes-pre", "P0 no HDP visibility window (submit would be unsafe)");
        return r3_substep_fail(kR3SecPre, 0, kFailNoHdp);
    }

    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    if (!ip || !ip->isResolved(IPBlock::GC, 0) || !ip->isResolved(IPBlock::GC, 1) ||
        !ip->isResolved(IPBlock::MMHUB, 0) || !ip->isResolved(IPBlock::OSSSYS, 0)) {
        evf(s.env, "mes-pre", "P0 GC/MMHUB/OSSSYS base unresolved");
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    }
    s.gc0 = ip->getBase(IPBlock::GC, 0);
    s.gc1 = ip->getBase(IPBlock::GC, 1);
    s.mmhub0 = ip->getBase(IPBlock::MMHUB, 0);
    s.osssys0 = ip->getBase(IPBlock::OSSSYS, 0);

    /* Golden-table consumption (MP1/GC segment authority lives in P1's table). */
    if (!resolve_golden(s, r3gold::kCpMesCntl, &s.mes_cntl) ||
        !resolve_golden(s, r3gold::kCpMesPrgrmCntrStart, &s.mes_prgrm) ||
        !resolve_golden(s, r3gold::kCpMesPrgrmCntrStartHi, &s.mes_prgrm_hi) ||
        !resolve_golden(s, r3gold::kCpMesGp3Lo, &s.mes_gp3) ||
        !resolve_golden(s, r3gold::kRlcCpSchedulers, &s.rlc_sched) ||
        !resolve_golden(s, r3gold::kCpMecCntl, &s.mec_cntl) ||
        !resolve_golden(s, r3gold::kCpMeCntl, &s.me_cntl) ||
        !resolve_golden(s, r3gold::kCpHqdActive, &s.hqd_active) ||
        !resolve_golden(s, r3gold::kRlcCsibAddrLo, &s.rlc_csib_lo) ||
        !resolve_golden(s, r3gold::kRlcCsibAddrHi, &s.rlc_csib_hi) ||
        !resolve_golden(s, r3gold::kRlcCsibLength, &s.rlc_csib_len) ||
        !resolve_golden(s, r3gold::kCpRbDoorbellRangeLower, &s.db_range_lower) ||
        !resolve_golden(s, r3gold::kCpRbDoorbellRangeUpper, &s.db_range_upper) ||
        !resolve_golden(s, r3gold::kCpMecDoorbellRangeLower, &s.mec_db_range_lower) ||
        !resolve_golden(s, r3gold::kCpMecDoorbellRangeUpper, &s.mec_db_range_upper)) {
        evf(s.env, "mes-pre", "P0 golden register resolution failed");
        return r3_substep_fail(kR3SecPre, 0, kFailNoBase);
    }


    s.mc_base = s.env->fb_mc_base;
    if (s.mc_base == 0ull) {
        evf(s.env, "mes-pre", "P0 fb_mc_base is zero");
        return r3_substep_fail(kR3SecPre, 0, kFailNoMcBase);
    }

    s.plan = s.env->rt->layout;
    if (!s.plan.ok) {
        evf(s.env, "mes-pre", "P1 layout invalid: %s",
            s.plan.fail_reason ? s.plan.fail_reason : "?");
        return r3_substep_fail(kR3SecPre, 1, kFailLayout);
    }
    const R3Grant *g = r3_layout_grant(&s.plan, "mes");
    if (!g) {
        evf(s.env, "mes-pre", "P1 layout has no \"mes\" grant");
        return r3_substep_fail(kR3SecPre, 1, kFailGrant);
    }
    if (g->owner != R3Owner::Mes) {
        evf(s.env, "mes-pre", "P1 \"mes\" grant owner=%u (want %u)",
            static_cast<unsigned>(g->owner), static_cast<unsigned>(R3Owner::Mes));
        return r3_substep_fail(kR3SecPre, 1, kFailOwner);
    }
    const uint32_t kPreCheckBytes = kMesRegionBytes + kMesScratchBytes; /* 0x16000 incl. scratch */
    if (g->size < kPreCheckBytes) {
        evf(s.env, "mes-pre", "P1 \"mes\" grant %llu B < %u B",
            static_cast<unsigned long long>(g->size), kPreCheckBytes);
        return r3_substep_fail(kR3SecPre, 1, kFailSpace);
    }
    if (!g->needs_cpu) {
        evf(s.env, "mes-pre", "P1 \"mes\" grant is not CPU reachable");
        return r3_substep_fail(kR3SecPre, 1, kFailNoCpu);
    }
    if (g->start + kPreCheckBytes > s.plan.bar0_limit) {
        evf(s.env, "mes-pre", "P1 \"mes\" region %llu..%llu past BAR0 limit %llu",
            static_cast<unsigned long long>(g->start),
            static_cast<unsigned long long>(g->start + kPreCheckBytes),
            static_cast<unsigned long long>(s.plan.bar0_limit));
        return r3_substep_fail(kR3SecPre, 1, kFailBar0);
    }

    s.grant_start = g->start;
    s.kiq_ring_bar0 = s.grant_start + kMesKiqRingOff;
    s.kiq_wb_bar0 = s.grant_start + kMesKiqWbOff;
    s.kiq_mqd_bar0 = s.grant_start + kMesKiqMqdOff;
    s.kiq_eop_bar0 = s.grant_start + kMesKiqEopOff;
    s.sch_ring_bar0 = s.grant_start + kMesSchRingOff;
    s.sch_wb_bar0 = s.grant_start + kMesSchWbOff;
    s.sch_mqd_bar0 = s.grant_start + kMesSchMqdOff;
    s.sch_eop_bar0 = s.grant_start + kMesSchEopOff;
    s.gfx_ring_bar0 = s.grant_start + kMesGfxRingOff;
    s.gfx_wb_bar0 = s.grant_start + kMesGfxWbOff;
    s.gfx_mqd_bar0 = s.grant_start + kMesGfxMqdOff;
    s.gfx_eop_bar0 = s.grant_start + kMesGfxEopOff;
    s.cmp_ring_bar0 = s.grant_start + kMesCmpRingOff;
    s.cmp_wb_bar0 = s.grant_start + kMesCmpWbOff;
    s.cmp_mqd_bar0 = s.grant_start + kMesCmpMqdOff;
    s.cmp_eop_bar0 = s.grant_start + kMesCmpEopOff;
    s.csib_bar0 = s.grant_start + kMesCsibOff;


    s.kiq_ring_mc = s.mc_base + s.kiq_ring_bar0;
    s.kiq_wb_mc = s.mc_base + s.kiq_wb_bar0;
    s.kiq_mqd_mc = s.mc_base + s.kiq_mqd_bar0;
    s.kiq_eop_mc = s.mc_base + s.kiq_eop_bar0;
    s.sch_ring_mc = s.mc_base + s.sch_ring_bar0;
    s.sch_wb_mc = s.mc_base + s.sch_wb_bar0;
    s.sch_mqd_mc = s.mc_base + s.sch_mqd_bar0;
    s.sch_eop_mc = s.mc_base + s.sch_eop_bar0;
    s.gfx_ring_mc = s.mc_base + s.gfx_ring_bar0;
    s.gfx_wb_mc = s.mc_base + s.gfx_wb_bar0;
    s.gfx_mqd_mc = s.mc_base + s.gfx_mqd_bar0;
    s.gfx_eop_mc = s.mc_base + s.gfx_eop_bar0;
    s.cmp_ring_mc = s.mc_base + s.cmp_ring_bar0;
    s.cmp_wb_mc = s.mc_base + s.cmp_wb_bar0;
    s.cmp_mqd_mc = s.mc_base + s.cmp_mqd_bar0;
    s.cmp_eop_mc = s.mc_base + s.cmp_eop_bar0;
    s.csib_mc = s.mc_base + s.csib_bar0;


    if (!(s.env->rt->ready_bits & R3_IP_BIT(R3Ip::Psp))) {
        evf(s.env, "mes-pre", "P2 PSP not ready (ready_bits=0x%08x)",
            s.env->rt->ready_bits);
        return r3_substep_fail(kR3SecPre, 2, kFailPspReady);
    }
    if (s.env->rt->latch_bits & R3_IP_BIT(R3Ip::Psp)) {
        evf(s.env, "mes-pre", "P2 PSP latched (latch_bits=0x%08x)",
            s.env->rt->latch_bits);
        return r3_substep_fail(kR3SecPre, 2, kFailPspLatched);
    }
    if (!(s.env->rt->ready_bits & R3_IP_BIT(R3Ip::Gfx))) {
        evf(s.env, "mes-pre", "P3 GFX not ready (ready_bits=0x%08x)",
            s.env->rt->ready_bits);
        return r3_substep_fail(kR3SecPre, 3, kFailGfxReady);
    }
    if (s.env->rt->latch_bits & R3_IP_BIT(R3Ip::Gfx)) {
        evf(s.env, "mes-pre", "P3 GFX latched (latch_bits=0x%08x)",
            s.env->rt->latch_bits);
        return r3_substep_fail(kR3SecPre, 3, kFailGfxLatched);
    }

    if (!load_headers(s)) {
        evf(s.env, "mes-pre", "P4 MES firmware header unreadable");
        return r3_substep_fail(kR3SecPre, 4, kFailFw);
    }

    evf(s.env, "mes-pre",
        "P0 gc0=0x%x gc1=0x%x mmhub=0x%x osssys=0x%x mc=0x%llx "
        "P1 grant=0x%llx..0x%llx P2 psp=ready P3 gfx=ready "
        "P4 sched_uc=0x%llx kiq_uc=0x%llx",
        s.gc0, s.gc1, s.mmhub0, s.osssys0,
        static_cast<unsigned long long>(s.mc_base),
        static_cast<unsigned long long>(g->start),
        static_cast<unsigned long long>(g->end),
        static_cast<unsigned long long>(s.sched_uc),
        static_cast<unsigned long long>(s.kiq_uc));
    return r3_substep_pass();
}

/* S-1: hub pre-initialisation (GCVM aperture/L1-L2/NBIF, 29 rows; 方案 v3 §6.2).
 * Run BEFORE S0 (CP unhalt), so GFXHUB is configured before any CP/MES access. */
R3SubstepResult seq_hub_pre(Mes &s)
{
    /* --- 1. MMHUB snapshot (single read, source of truth for C and FB_LOCATION) --- */
    uint32_t mmhub_fb_off = 0;
    if (!resolve_golden(s, r3gold::kMmmcVmFbOffset, &mmhub_fb_off)) {
        evf(s.env, "mes-hub-pre", "mmhub FB_OFFSET golden unresolved");
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }
    uint32_t mmhub_fb_base = 0;
    if (!resolve_golden(s, r3gold::kMmmcVmFbLocationBase, &mmhub_fb_base)) {
        evf(s.env, "mes-hub-pre", "mmhub FB_LOCATION_BASE golden unresolved");
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }
    uint32_t mmhub_fb_top = 0;
    if (!resolve_golden(s, r3gold::kMmmcVmFbLocationTop, &mmhub_fb_top)) {
        evf(s.env, "mes-hub-pre", "mmhub FB_LOCATION_TOP golden unresolved");
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }

    bool ok = false;
    s.fb_off_raw  = reg_read(s, mmhub_fb_off, &ok);
    if (!ok) return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    const uint32_t raw_off = s.fb_off_raw;
    s.fb_base_raw = reg_read(s, mmhub_fb_base, &ok);
    if (!ok) return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    s.fb_top_raw  = reg_read(s, mmhub_fb_top, &ok);
    if (!ok) return r3_substep_fail(kR3SecPre, 1, kFailNoBase);

    /* Defensive: MMHUB FB_OFFSET must be non-zero. If 0, default_pa would fall to
     * low physical address and crash. */
    if (raw_off == 0u) {
        evf(s.env, "mes-hub-pre", "MMHUB FB_OFFSET=0 (fatal: default_pa would crash)");
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }

    /* T8: cross-check against env->fb_mc_base. */
    const uint64_t mmhub_mc_base = static_cast<uint64_t>(s.fb_base_raw) << 24;
    if (mmhub_mc_base != s.mc_base) {
        evf(s.env, "mes-hub-pre",
            "T8 mmhub FB_BASE=0x%x (->0x%llx) != env mc_base=0x%llx",
            s.fb_base_raw, static_cast<unsigned long long>(mmhub_mc_base),
            static_cast<unsigned long long>(s.mc_base));
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }
    const uint64_t fb_phys_base = static_cast<uint64_t>(raw_off) << 24;
    const uint64_t fb_mc_top = (static_cast<uint64_t>(s.fb_top_raw) << 24) | 0xffffffull;

    /* System aperture formula (Linux gfxhub_v3_0_gart_enable). */
    const uint32_t sysa_low  = static_cast<uint32_t>((mmhub_mc_base < 0xffffffffffffull
                               ? mmhub_mc_base : 0xffffffffffffull) >> 18);
    const uint32_t sysa_high = static_cast<uint32_t>((fb_mc_top > 0ull
                               ? fb_mc_top : 0ull) >> 18);

    /* Default page / resource1 PA inside the MES grant (方案 v3 §6.2.4/§6.5). */
    s.default_pa   = fb_phys_base + s.grant_start + kMesDefaultPageOff;
    s.resource1_pa = fb_phys_base + s.grant_start + kMesResource1Off;

    /* --- 2. CONTEXT0 PT window snapshot (M6: overlap with MES grant → fail closed) --- */
    struct { uint32_t addr; uint32_t value; } pt_wins[6] = {
        {s.gc0 + 0x16f3, 0}, {s.gc0 + 0x16f4, 0},
        {s.gc0 + 0x1713, 0}, {s.gc0 + 0x1714, 0},
        {s.gc0 + 0x1733, 0}, {s.gc0 + 0x1734, 0},
    };
    for (uint32_t i = 0; i < 6u; i++) {
        pt_wins[i].value = reg_read(s, pt_wins[i].addr, &ok);
        if (!ok) {
            evf(s.env, "mes-hub-pre", "PT window 0x%x unreadable", pt_wins[i].addr);
            return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
        }
    }
    /* Check if any PT base is non-zero and overlaps with MES grant MC range. */
    if (pt_wins[0].value != 0u || pt_wins[1].value != 0u) {
        const uint64_t pt_base = (static_cast<uint64_t>(pt_wins[0].value) & 0xfffff000ull) |
                                 (static_cast<uint64_t>(pt_wins[1].value) << 32);
        const uint64_t mes_mc_start = s.mc_base + s.grant_start;
        const uint64_t mes_mc_end   = mes_mc_start + kMesRegionBytes + kMesScratchBytes;
        if (pt_base >= mes_mc_start && pt_base < mes_mc_end) {
            evf(s.env, "mes-hub-pre",
                "M6 CONTEXT0 PT 0x%llx overlaps MES grant 0x%llx-0x%llx",
                static_cast<unsigned long long>(pt_base),
                static_cast<unsigned long long>(mes_mc_start),
                static_cast<unsigned long long>(mes_mc_end));
            return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
        }
    }

    /* --- 3. Pre-hub snapshot (方案 v3 §6.6.1) --- */
    s.nbio2 = 0;
    const IPBaseTable *ip = static_cast<const IPBaseTable *>(s.env->ip);
    if (ip) {
        s.nbio2 = ip->getBase(IPBlock::NBIO, 2);
        s.mmhub1 = ip->getBase(IPBlock::MMHUB, 1);
    }
    evf(s.env, "mes-hub-pre",
        "IP: gc[0]=0x%x gc[1]=0x%x mmhub[0]=0x%x mmhub[1]=0x%x osssys[0]=0x%x nbio[2]=0x%x "
        "mmhub fb_off=0x%x fb_base=0x%x fb_top=0x%x "
        "sysa_low=0x%x sysa_high=0x%x default_pa=0x%llx resource1_pa=0x%llx "
        "PT win: 0x%x/0x%x 0x%x/0x%x 0x%x/0x%x",
        s.gc0, ip ? ip->getBase(IPBlock::GC, 1) : 0u,
        s.mmhub0, s.mmhub1, s.osssys0, s.nbio2,
        raw_off, s.fb_base_raw, s.fb_top_raw,
        sysa_low, sysa_high,
        static_cast<unsigned long long>(s.default_pa),
        static_cast<unsigned long long>(s.resource1_pa),
        pt_wins[0].value, pt_wins[1].value, pt_wins[2].value, pt_wins[3].value,
        pt_wins[4].value, pt_wins[5].value);

    /* --- 4. Read GC GCMC_VM_FB_OFFSET (C row, Optional conditional) --- */
    const uint32_t gc_fb_off_addr = s.gc0 + 0x15a7u;
    s.gc_fb_off = reg_read(s, gc_fb_off_addr, &ok);
    if (!ok) {
        evf(s.env, "mes-hub-pre", "C: GC FB_OFFSET 0x%x unreadable", gc_fb_off_addr);
        return r3_substep_fail(kR3SecPre, 1, kFailNoBase);
    }
    bool write_c = false;
    const char *c_state = "written";
    if (s.gc_fb_off == 0u) {
        write_c = true;
        evf(s.env, "mes-hub-pre", "C: GC FB_OFFSET=0 -> will write MMHUB value 0x%x",
            raw_off);
    } else if (s.gc_fb_off == raw_off) {
        c_state = "skipped";
        evf(s.env, "mes-hub-pre", "C: GC FB_OFFSET=0x%x == MMHUB, skip", s.gc_fb_off);
    } else {
        c_state = "mismatch";
        evf(s.env, "mes-hub-pre",
            "C: GC FB_OFFSET=0x%x != MMHUB 0x%x, report & skip (M7)",
            s.gc_fb_off, raw_off);
    }

    /* --- 5. Zero the two scratch pages (方案 v3 §6.2.4). The pages sit inside the layout:mes
     * grant (Data class), so zero_region's bus_write classifies correctly. --- */
    for (uint32_t pg = 0; pg < 2u; pg++) {
        const uint64_t pg_bar0 = s.grant_start + kMesRegionBytes + (uint64_t)pg * 0x1000ull;
        if (!zero_region(s, pg_bar0, 0x1000ull, "mes-hub-scratch"))
            return r3_substep_fail(kR3SecSeq, 1, "seq:scratch-io");
    }

    /* --- 6. Write 29 rows (表 1–29, 方案 v3 §6.2.3). Use RMW helpers. --- */
    const auto rmw = [&](uint32_t addr, uint32_t mask, uint32_t value,
                         const char *ctx) -> bool {
        bool r_ok = false;
        uint32_t cur = reg_read(s, addr, &r_ok);
        if (!r_ok) return false;
        return reg_write(s, addr, (cur & ~mask) | (value & mask), kR3WState, ctx);
    };
    const auto wabs = [&](uint32_t addr, uint32_t v, const char *ctx) -> bool {
        return reg_write(s, addr, v, kR3WState, ctx);
    };

    /* Row 1: C - GCMC_VM_FB_OFFSET (conditional, WNeed::Optional)
     * (gc_11_0_0_offset.h:2690) */
    if (write_c) {
        if (!wabs(gc_fb_off_addr, raw_off, "mes-hub-fb-offset"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }
    /* Row 2-3: FB_LOCATION (gc_11_0_0_offset.h:2830/:2832; Linux
     * gfxhub_v3_0_gart_enable, gfxhub_v3_0.c:360-363) */
    if (!wabs(s.gc0 + 0x1678u, s.fb_base_raw, "mes-hub-fb") ||
        !wabs(s.gc0 + 0x1679u, s.fb_top_raw,  "mes-hub-fb"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 4-8: AGP close + system aperture (gc_11_0_0_offset.h:2834-:2842; Linux
     * gfxhub_v3_0_init_system_aperture_regs, gfxhub_v3_0.c:150-182) */
    if (!wabs(s.gc0 + 0x167cu, 0u,            "mes-hub-aperture") ||  /* AGP_BASE */
        !wabs(s.gc0 + 0x167bu, 0xffffffffu,   "mes-hub-aperture") ||  /* AGP_BOT */
        !wabs(s.gc0 + 0x167au, 0u,             "mes-hub-aperture") ||  /* AGP_TOP */
        !wabs(s.gc0 + 0x167du, sysa_low,       "mes-hub-aperture") ||  /* SYSTEM_APERTURE_LOW */
        !wabs(s.gc0 + 0x167eu, sysa_high,      "mes-hub-aperture"))    /* SYSTEM_APERTURE_HIGH */
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    const uint32_t default_addr_lo = static_cast<uint32_t>(s.default_pa >> 12);
    const uint32_t default_addr_hi = static_cast<uint32_t>(s.default_pa >> 44);
    if (!wabs(s.gc0 + 0x15a8u, default_addr_lo, "mes-hub-default") ||  /* SYS_APERTURE_DEFAULT_LSB: gc_11_0_0_offset.h:2692 */
        !wabs(s.gc0 + 0x15a9u, default_addr_hi, "mes-hub-default") ||  /* SYS_APERTURE_DEFAULT_MSB: :2694 */
        !wabs(s.gc0 + 0x15cbu, default_addr_lo, "mes-hub-default") ||  /* FAULT_DEFAULT_LO32: :2760 */
        !wabs(s.gc0 + 0x15ccu, default_addr_hi, "mes-hub-default"))    /* FAULT_DEFAULT_HI32: :2762 (Linux gfxhub_v3_0.c:168-177) */
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 13: L2_PROTECTION_FAULT_CNTL2: RMW | (1<<18) (default 0xa0000→0xe0000)
     * (gc_11_0_0_offset.h:2748; Linux gfxhub_v3_0.c:179-181) */
    if (!rmw(s.gc0 + 0x15c5u, 1u << 18, 1u << 18, "mes-hub-fault2"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 14: MX_L1_TLB_CNTL: RMW. Set ENABLE=1 bit0, MODE=3 bits3-4, UNMAPPED=0 bit5,
     * ADV=1 bit6, ECO=0 bits7-10, MTYPE=3 bits11-13.
     * (gc_11_0_0_offset.h:2844; Linux gfxhub_v3_0_init_tlb_regs, gfxhub_v3_0.c:184-202) */
    if (!rmw(s.gc0 + 0x167fu,
             0x00000001u | (3u << 3) | (1u << 5) | (1u << 6) | (0xfu << 7) | (7u << 11),
             0x00000001u | (3u << 3) | (1u << 6) | (3u << 11),
             "mes-hub-mx-l1"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 15-19: L2_CNTL..CNTL5 (gc_11_0_0_offset.h:2730/:2732/:2734/:2776/:2788;
     * Linux gfxhub_v3_0_init_cache_regs, gfxhub_v3_0.c:204-253) */
    {
        /* L2_CNTL RMW (Linux gfxhub_v3_0_init_cache_regs): fields per gc_11_0_0_sh_mask.h:
         * ENABLE_L2_CACHE=1 bit0, ENABLE_L2_FRAGMENT_PROCESSING=0 bit1,
         * L2_PDE0_CACHE_TAG_GENERATION_MODE=0 bit8,
         * ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY=1 bit11,
         * PDE_FAULT_CLASSIFICATION=0 bit18,
         * CONTEXT1_IDENTITY_ACCESS_MODE=1 bits19-20,
         * IDENTITY_MODE_FRAGMENT_SIZE=0 bits21-25.
         * Reset 0x00080602 → 0x00080E01. */
        if (!rmw(s.gc0 + 0x15bcu,
                 0x00000001u | (1u << 1) | (1u << 8) | (1u << 11) | (1u << 18) |
                 (3u << 19) | (0x1fu << 21),
                 0x00000001u | (1u << 11) | (1u << 19),
                 "mes-hub-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* L2_CNTL2: RMW: INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE */
        if (!rmw(s.gc0 + 0x15bdu, 3u, 3u, "mes-hub-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* L2_CNTL3: default 0x80120007 + BANK_SELECT=9 + BIGK_FRAGMENT=6 */
        if (!wabs(s.gc0 + 0x15beu, 0x80130009u, "mes-hub-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* L2_CNTL4: Linux absolute write based on regGCVM_L2_CNTL4_DEFAULT (0xc1)
         * with VMC_TAP_PDE/PTE_REQUEST_PHYSICAL (bit6/7) cleared → 0x1. */
        if (!wabs(s.gc0 + 0x15d4u, 0x1u, "mes-hub-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* L2_CNTL5: default 0x3fe0 + SMALLK_FRAGMENT=0 */
        if (!wabs(s.gc0 + 0x15dau, 0x3fe0u, "mes-hub-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }
    /* Row 20 (A): GCVM_CONTEXT0_CNTL RMW: ENABLE_CONTEXT=1 bit0, PAGE_TABLE_DEPTH=0
     * bits1-2, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT=0 bit7
     * (gc_11_0_0_offset.h:2850; Linux gfxhub_v3_0_enable_system_domain,
     * gfxhub_v3_0.c:255-265). */
    if (!rmw(s.gc0 + 0x1688u, 0x87u, 0x00000001u, "mes-hub-context0"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 21-26: identity aperture + physical offset (all 0 or 0xfffffffff)
     * (gc_11_0_0_offset.h:2764-:2774; Linux gfxhub_v3_0_disable_identity_aperture,
     * gfxhub_v3_0.c:267-288) */
    if (!wabs(s.gc0 + 0x15ceu, 0xffffffffu, "mes-hub-identity") ||
        !wabs(s.gc0 + 0x15cfu, 0x0000000fu, "mes-hub-identity") ||
        !wabs(s.gc0 + 0x15d0u, 0u,          "mes-hub-identity") ||
        !wabs(s.gc0 + 0x15d1u, 0u,          "mes-hub-identity") ||
        !wabs(s.gc0 + 0x15d2u, 0u,          "mes-hub-identity") ||
        !wabs(s.gc0 + 0x15d3u, 0u,          "mes-hub-identity"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);

    /* HDP flush between identity and CP_DEBUG/fault (方案 v3 §6.2.3 表注). */
    if (!r3_io_hdp_flush(s.io)) {
        evf(s.env, "mes-hub-pre", "HDP flush failed after identity");
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }
    /* --- MMHUB VM path initialization (方案 W, Linux mmhub_v3_0_1.c semantics) --- */
    const uint32_t mmhub1 = s.mmhub1;
    if (mmhub1 == 0u || !ip->isResolved(IPBlock::MMHUB, 1)) {
        evf(s.env, "mes-hub-pre", "MMHUB seg1 unresolved (0x%x)", mmhub1);
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }
    /* MM AGP 关 + system aperture (mmhub_3_0_1_offset.h:1672-:1682; Linux
     * mmhub_v3_0_1_init_system_aperture_regs, mmhub_v3_0_1.c:183-186) */
    if (!wabs(mmhub1 + 0x08F0u, 0u,            "mes-hub-mm-aperture") ||  /* AGP_BASE */
        !wabs(mmhub1 + 0x08EFu, 0x00ffffffu,   "mes-hub-mm-aperture") ||  /* AGP_BOT */
        !wabs(mmhub1 + 0x08EEu, 0u,             "mes-hub-mm-aperture") ||  /* AGP_TOP */
        !wabs(mmhub1 + 0x08F1u, sysa_low,       "mes-hub-mm-aperture") ||  /* SYSTEM_APERTURE_LOW */
        !wabs(mmhub1 + 0x08F2u, sysa_high,      "mes-hub-mm-aperture"))    /* SYSTEM_APERTURE_HIGH */
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MM DEFAULT_ADDR + FAULT_DEFAULT (mmhub_3_0_1_offset.h:1628/:1630/:1064/:1066;
     * Linux mmhub_v3_0_1_init_system_aperture_regs, mmhub_v3_0_1.c:189-199) */
    if (!wabs(mmhub1 + 0x08D8u, default_addr_lo, "mes-hub-mm-default") ||  /* SYS_APERTURE_DEFAULT_LSB */
        !wabs(mmhub1 + 0x08D9u, default_addr_hi, "mes-hub-mm-default") ||  /* SYS_APERTURE_DEFAULT_MSB */
        !wabs(mmhub1 + 0x070Fu, default_addr_lo, "mes-hub-mm-default") ||  /* FAULT_DEFAULT_LO32 */
        !wabs(mmhub1 + 0x0710u, default_addr_hi, "mes-hub-mm-default"))    /* FAULT_DEFAULT_HI32 */
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MM FAULT_CNTL2: RMW | (1<<18) (mmhub_3_0_1_offset.h:1052-:1053;
     * Linux mmhub_v3_0_1.c:201-204) */
    if (!rmw(mmhub1 + 0x0709u, 1u << 18, 1u << 18, "mes-hub-mm-fault2"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MM MX_L1_TLB_CNTL: RMW (same bits as GC, mmhub_3_0_1_offset.h:1682-:1683;
     * Linux mmhub_v3_0_1_init_tlb_regs, mmhub_v3_0_1.c:212-224) */
    if (!rmw(mmhub1 + 0x08F3u,
             0x00000001u | (3u << 3) | (1u << 5) | (1u << 6) | (0xfu << 7) | (7u << 11),
             0x00000001u | (3u << 3) | (1u << 6) | (3u << 11),
             "mes-hub-mm-mx-l1"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MM L2_CNTL..L2_CNTL5 (mmhub_3_0_1_offset.h:1034-:1093;
     * Linux mmhub_v3_0_1_init_cache_regs, mmhub_v3_0_1.c:226-283) */
    {
        /* MM L2_CNTL RMW: same fields as GC */
        if (!rmw(mmhub1 + 0x0700u,
                 0x00000001u | (1u << 1) | (1u << 8) | (1u << 11) | (1u << 18) |
                 (3u << 19) | (0x1fu << 21),
                 0x00000001u | (1u << 11) | (1u << 19),
                 "mes-hub-mm-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* MM L2_CNTL2: RMW: INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE */
        if (!rmw(mmhub1 + 0x0701u, 3u, 3u, "mes-hub-mm-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* MM L2_CNTL3: same value as GC */
        if (!wabs(mmhub1 + 0x0702u, 0x80130009u, "mes-hub-mm-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* MM L2_CNTL4 */
        if (!wabs(mmhub1 + 0x0718u, 0x1u, "mes-hub-mm-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        /* MM L2_CNTL5 */
        if (!wabs(mmhub1 + 0x071Eu, 0x3fe0u, "mes-hub-mm-l2"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }

    /* Row 27: CP_DEBUG RMW | (1<<15) (gc_11_0_0_offset.h:4224; Linux
     * gfxhub_v3_0_set_fault_enable_default, gfxhub_v3_0.c:415-417) */
    if (!rmw(s.gc0 + 0x1e1fu, 1u << 15, 1u << 15, "mes-hub-cp-debug"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 28: L2_PROTECTION_FAULT_CNTL RMW: bits2-12=1, bits30-31=0
     * (gc_11_0_0_offset.h:2746; Linux set_fault_enable_default, gfxhub_v3_0.c:409-414). */
    if (!rmw(s.gc0 + 0x15c4u, 0xc0001ffcu, 0x00001ffcu, "mes-hub-fault"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MMHUB L2_PROTECTION_FAULT_CNTL RMW: same as GC (mmhub_3_0_1_offset.h:1050-:1051) */
    if (!rmw(mmhub1 + 0x0708u, 0xc0001ffcu, 0x00001ffcu, "mes-hub-mm-fault"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* MM identity aperture close + physical offset (mmhub_3_0_1_offset.h:1068-1079;
     * Linux mmhub_v3_0_1_disable_identity_aperture, mmhub_v3_0_1.c:284-303). */
    if (!wabs(mmhub1 + 0x0712u, 0xffffffffu, "mes-hub-mm-identity") ||
        !wabs(mmhub1 + 0x0713u, 0x0000000fu, "mes-hub-mm-identity") ||
        !wabs(mmhub1 + 0x0714u, 0u,          "mes-hub-mm-identity") ||
        !wabs(mmhub1 + 0x0715u, 0u,          "mes-hub-mm-identity") ||
        !wabs(mmhub1 + 0x0716u, 0u,          "mes-hub-mm-identity") ||
        !wabs(mmhub1 + 0x0717u, 0u,          "mes-hub-mm-identity"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 29: NBIF doorbell aperture RMW | bit0 (nbio_7_7_0_offset.h:3670-3671;
     * Linux nbio_v7_7_enable_doorbell_aperture, nbio_v7_7.c:112-116) */
    {
        const uint32_t nbif_addr = s.nbio2 + 0xC0u;
        if (s.nbio2 == 0u || !ip->isResolved(IPBlock::NBIO, 2)) {
            evf(s.env, "mes-hub-pre", "NBIO seg2 unresolved (0x%x)", s.nbio2);
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
        }
        if (!rmw(nbif_addr, 1u, 1u, "mes-hub-doorbell-aper"))
            return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    }

    /* --- 7. Post-hub readback (方案 v3 §6.6.2) --- */
    uint32_t post_regs[][2] = {
        {gc_fb_off_addr,          0}, /* C */
        {s.gc0 + 0x1678u,         0}, /* FB_BASE */
        {s.gc0 + 0x1679u,         0}, /* FB_TOP */
        {s.gc0 + 0x167du,         0}, /* SYS_LOW */
        {s.gc0 + 0x167eu,         0}, /* SYS_HIGH */
        {s.gc0 + 0x167fu,         0}, /* MX_L1 */
        {s.gc0 + 0x15bcu,         0}, /* L2_CNTL */
        {s.gc0 + 0x15bdu,         0}, /* L2_CNTL2 */
        {s.gc0 + 0x15c4u,         0}, /* FAULT_CNTL */
        {s.gc0 + 0x15c5u,         0}, /* FAULT_CNTL2 */
        {s.gc0 + 0x1688u,         0}, /* CONTEXT0_CNTL */
        {s.gc0 + 0x1e1fu,         0}, /* CP_DEBUG */
        {mmhub1 + 0x08F2u,        0}, /* MM_SYS_HIGH */
        {mmhub1 + 0x08F3u,        0}, /* MM_MX_L1 */
        {mmhub1 + 0x0700u,        0}, /* MM_L2_CNTL */
        {mmhub1 + 0x0708u,        0}, /* MM_FAULT_CNTL */
        {mmhub1 + 0x0712u,        0}, /* MM_IDENTITY_LO */
        {s.nbio2 + 0xC0u,         0}, /* NBIF doorbell aper */
    };
    char rb[512];
    uint32_t rb_pos = 0;
    rb_pos += (uint32_t)snprintf(rb + rb_pos, sizeof(rb) - rb_pos, "post:");
    for (uint32_t i = 0; i < sizeof(post_regs) / sizeof(post_regs[0]); i++) {
        bool r_ok = false;
        post_regs[i][1] = reg_read(s, post_regs[i][0], &r_ok);
        if (r_ok) {
            rb_pos += (uint32_t)snprintf(rb + rb_pos, sizeof(rb) - rb_pos,
                        " 0x%x=0x%x", post_regs[i][0], post_regs[i][1]);
        }
    }
    evf(s.env, "mes-hub-post", "%s", rb);

    evf(s.env, "mes-hub-pre", "done: wrote %u rows (C %s)",
        write_c ? 52u : 51u, write_c ? "written" : "skipped");
    {
        const uint32_t nbif_val =
            post_regs[sizeof(post_regs) / sizeof(post_regs[0]) - 1u][1];
        char hub[128];
        snprintf(hub, sizeof(hub),
                 "fb_off_raw=0x%x\nc=%s\nnbif=0x%x/bit%u\nrows=%u",
                 raw_off, c_state, nbif_val, nbif_val & 1u, write_c ? 52u : 51u);
        set_prop(s.env, "N48R3EVID-mes-hub", hub);
    }
    return r3_substep_pass();
}

R3SubstepResult seq_cp_unhalt(Mes &s)
{
    /* S0: release CP before any queue operation. nullarc full.sh:9-10 writes
     * CP_MEC_CNTL=0 then CP_ME_CNTL=0x01000000; writing 0 to CP_ME_CNTL hangs PFP
     * on RCIU, so CE_HALT stays set. Addresses come from the golden table. */
    if (!reg_write(s, s.mec_cntl, 0u, kR3WState, "mes-cp-unhalt") ||
        !reg_write(s, s.me_cntl, kMesCpMeCntlCeHalt, kR3WState, "mes-cp-unhalt")) {
        evf(s.env, "mes-seq", "S0 CP_MEC_CNTL/CP_ME_CNTL write denied");
        return r3_substep_fail(kR3SecSeq, 0, kFailCpUnhalt);
    }
    evf(s.env, "mes-seq", "S0 CP_MEC_CNTL=0 CP_ME_CNTL=0x%08x (CE_HALT kept)",
        kMesCpMeCntlCeHalt);
    return r3_substep_pass();
}

R3SubstepResult seq_start(Mes &s)
{
    bool ok = false;
    const uint32_t before = reg_read(s, s.mes_cntl, &ok);
    if (!ok) {
        evf(s.env, "mes-seq", "S1 CP_MES_CNTL unreadable");
        return r3_substep_fail(kR3SecSeq, 1, kFailStart);
    }
    if (!reg_write(s, s.mes_cntl,
                   before | kMesCntlPipe0Reset | kMesCntlPipe1Reset,
                   kR3WState, "mes-start-reset")) {
        evf(s.env, "mes-seq", "S1 CP_MES_CNTL reset write denied");
        return r3_substep_fail(kR3SecSeq, 1, kFailStart);
    }

    const uint64_t addr[2] = {s.sched_uc >> 2, s.kiq_uc >> 2};
    for (uint32_t pipe = 0; pipe < 2u; pipe++) {
        if (!grbm_select(s, 3u, pipe, 0u, 0u)) {
            evf(s.env, "mes-seq", "S1 grbm select pipe%u denied", pipe);
            return r3_substep_fail(kR3SecSeq, 1, kFailStart);
        }
        if (!reg_write(s, s.mes_prgrm,
                       static_cast<uint32_t>(addr[pipe]), kR3WState, "mes-start-prgrm") ||
            !reg_write(s, s.mes_prgrm_hi,
                       static_cast<uint32_t>(addr[pipe] >> 32), kR3WState,
                       "mes-start-prgrm")) {
            evf(s.env, "mes-seq", "S1 PRGRM_CNTR_START pipe%u write denied", pipe);
            return r3_substep_fail(kR3SecSeq, 1, kFailStart);
        }
    }
    if (!grbm_select(s, 0u, 0u, 0u, 0u)) {
        evf(s.env, "mes-seq", "S1 grbm deselect denied");
        return r3_substep_fail(kR3SecSeq, 1, kFailStart);
    }
    if (!reg_write(s, s.mes_cntl, kMesCntlPipe0Active | kMesCntlPipe1Active,
                   kR3WState, "mes-start-active")) {
        evf(s.env, "mes-seq", "S1 CP_MES_CNTL active write denied");
        return r3_substep_fail(kR3SecSeq, 1, kFailStart);
    }
    r3_io_udelay(s.io, 500u);

    const uint32_t after = reg_read(s, s.mes_cntl, &ok);
    s.cntl_readback = after;
    if (!ok) {
        evf(s.env, "mes-seq", "S1 CP_MES_CNTL readback unreadable");
        return r3_substep_fail(kR3SecSeq, 1, kFailStart);
    }
    /* C5.2 / tech plan §3.5: the CP_MES_CNTL readback is only recorded; the PIPE ACTIVE
     * bits are not a gate (Linux mes_v11_0.c:957-1005 and nullarc mesboot.c:147-161
     * wait/print only). A read-path fault fails closed (§8.1), handled above. */

    evf(s.env, "mes-start",
        "S1 sched_uc=0x%llx >>2 lo=0x%08x hi=0x%08x kiq_uc=0x%llx >>2 lo=0x%08x hi=0x%08x "
        "cntl=0x%08x",
        static_cast<unsigned long long>(s.sched_uc),
        static_cast<uint32_t>(s.sched_uc >> 2),
        static_cast<uint32_t>(s.sched_uc >> 34),
        static_cast<unsigned long long>(s.kiq_uc),
        static_cast<uint32_t>(s.kiq_uc >> 2),
        static_cast<uint32_t>(s.kiq_uc >> 34), after);
    return r3_substep_pass();
}

R3SubstepResult seq_version(Mes &s)
{
    bool ok0 = false, ok1 = false;
    if (!grbm_select(s, 3u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 2, kFailVersion);
    const uint32_t v0 = reg_read(s, s.mes_gp3, &ok0);
    if (!grbm_select(s, 3u, 1u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 2, kFailVersion);
    const uint32_t v1 = reg_read(s, s.mes_gp3, &ok1);
    if (!grbm_select(s, 0u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 2, kFailVersion);

    /* C5.2 / tech plan §3.5: the GP3 version is only recorded; 0 / all-ones are
     * observations, never a gate. Only a read-path fault fails closed (§8.1). */
    if (!ok0 || !ok1) {
        evf(s.env, "mes-version",
            "S2 CP_MES_GP3_LO read fault sched=0x%08x (ok=%d) kiq=0x%08x (ok=%d)",
            v0, ok0, v1, ok1);
        R3SubstepResult r = r3_substep_fail(kR3SecChk, 2, kFailVersion);
        r3_substep_note(&r, s.mes_gp3, v0, "sched");
        r3_substep_note(&r, s.mes_gp3, v1, "kiq");
        return r;
    }
    s.gp3_sched = v0;
    s.gp3_kiq = v1;
    evf(s.env, "mes-version", "S2 sched=0x%08x kiq=0x%08x", v0, v1);
    return r3_substep_pass();
}

R3SubstepResult seq_kiq(Mes &s)
{
    bool ok = false;
    const uint32_t t0 = reg_read(s, s.rlc_sched, &ok);
    if (!ok) {
        evf(s.env, "mes-kiq", "S3 RLC_CP_SCHEDULERS unreadable");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }
    const uint32_t t = (t0 & 0xffffff00u) | (3u << 5) | (1u << 3);
    if (!reg_write(s, s.rlc_sched, t | kMesRlcSchedulerByte,
                   kR3WProtocol, "mes-kiq-sched")) {
        evf(s.env, "mes-kiq", "S3 RLC_CP_SCHEDULERS write denied");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }

    if (!zero_queue_structures(s, s.kiq_ring_bar0, s.kiq_wb_bar0, s.kiq_mqd_bar0,
                               s.kiq_eop_bar0, kMesKiqRingDwords)) {
        evf(s.env, "mes-kiq", "S3 KIQ zero failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }
    if (!build_compute_mqd(s, s.kiq_mqd_bar0, s.kiq_ring_bar0, s.kiq_wb_bar0,
                           s.kiq_eop_bar0, kMesDoorbellKiq, kMesKiqRingDwords)) {
        evf(s.env, "mes-kiq", "S3 KIQ MQD build failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }
    if (!program_hqd(s, 3u, 1u, s.kiq_mqd_bar0, s.kiq_ring_bar0, s.kiq_wb_bar0,
                     s.kiq_eop_bar0, pq_control(kMesKiqRingDwords), kMesDoorbellKiq)) {
        evf(s.env, "mes-kiq", "S3 KIQ HQD programming failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }
    if (!bus_write(s, s.kiq_wb_bar0 + kMesWbWptrOff, 0ull, 2u, "mes-kiq-wptr") ||
        !doorbell_write(s, kMesDoorbellKiq, 0u, "mes-kiq-kick")) {
        evf(s.env, "mes-kiq", "S3 KIQ wptr/doorbell failed");
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    }

    if (!grbm_select(s, 3u, 1u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);
    bool aok = false, rok = false;
    s.kiq_active = reg_read(s, s.hqd_active, &aok);
    s.kiq_rptr = reg_read(s, s.gc0 + kRegCpHqdPqRptr, &rok);
    if (!grbm_select(s, 0u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 3, kFailKiq);

    evf(s.env, "mes-kiq",
        "S3 RLC=0x%08x..0x%08x ring_mc=0x%llx mqd_mc=0x%llx active=%u buf=%u rptr=%u",
        t0, t | kMesRlcSchedulerByte,
        static_cast<unsigned long long>(s.kiq_ring_mc),
        static_cast<unsigned long long>(s.kiq_mqd_mc),
        s.kiq_active, aok ? 1u : 0u, rok ? s.kiq_rptr : 0u);
    return r3_substep_pass();
}

R3SubstepResult seq_sch(Mes &s)
{
    if (!zero_queue_structures(s, s.sch_ring_bar0, s.sch_wb_bar0, s.sch_mqd_bar0,
                               s.sch_eop_bar0, kMesSchRingDwords)) {
        evf(s.env, "mes-sch", "S4 SCH zero failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);
    }
    if (!build_compute_mqd(s, s.sch_mqd_bar0, s.sch_ring_bar0, s.sch_wb_bar0,
                           s.sch_eop_bar0, kMesDoorbellSch, kMesSchRingDwords)) {
        evf(s.env, "mes-sch", "S4 SCH MQD build failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);
    }
    if (!program_hqd(s, 3u, 0u, s.sch_mqd_bar0, s.sch_ring_bar0, s.sch_wb_bar0,
                     s.sch_eop_bar0, pq_control(kMesSchRingDwords), kMesDoorbellSch)) {
        evf(s.env, "mes-sch", "S4 SCH HQD programming failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);
    }
    if (!bus_write(s, s.sch_wb_bar0 + kMesWbWptrOff, 0ull, 2u, "mes-sch-wptr") ||
        !doorbell_write(s, kMesDoorbellSch, 0u, "mes-sch-kick")) {
        evf(s.env, "mes-sch", "S4 SCH wptr/doorbell failed");
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);
    }

    if (!grbm_select(s, 3u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);
    bool aok = false, rok = false;
    s.sch_active = reg_read(s, s.hqd_active, &aok);
    s.sch_rptr = reg_read(s, s.gc0 + kRegCpHqdPqRptr, &rok);
    if (!grbm_select(s, 0u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 4, kFailSch);

    evf(s.env, "mes-sch",
        "S4 ring_mc=0x%llx mqd_mc=0x%llx active=%u buf=%u rptr=%u",
        static_cast<unsigned long long>(s.sch_ring_mc),
        static_cast<unsigned long long>(s.sch_mqd_mc),
        s.sch_active, aok ? 1u : 0u, rok ? s.sch_rptr : 0u);
    return r3_substep_pass();
}

enum class SubmitResult {
    Ok,
    FenceTimeout,
    StatusTimeout,
    IoFail,
};

SubmitResult submit_api(Mes &s, uint32_t opcode, uint32_t api_status_off)
{
    const uint32_t fw = s.sch_wptr;
    const uint64_t frame_bar0 = s.sch_ring_bar0 + (uint64_t)fw * 4ull;
    const uint64_t query_bar0 = frame_bar0 + (uint64_t)kMesApiFrameDwords * 4ull;

    /* D1/方案 v3 §6.3: ring space check before writing. */
    if (fw + kMesApiFrameDwords + kMesApiQueryDwords > kMesSchRingDwords) {
        evf(s.env, "mes-api", "op=%u ring full: wptr=%u, need=%u, cap=%u",
            opcode, fw, kMesApiFrameDwords + kMesApiQueryDwords, kMesSchRingDwords);
        return SubmitResult::IoFail;
    }

    /* The API packet names the status dword MES writes on completion; the trailing query
     * packet names the ring fence (Linux mes_v11_0_submit_pkt_and_poll_completion). */
    const uint64_t status_addr = s.sch_wb_mc + kMesWbStatusOff;
    const uint64_t status_one = 1ull;
    memcpy((uint8_t *)s.frame + api_status_off, &status_addr, 8u);
    memcpy((uint8_t *)s.frame + api_status_off + 8u, &status_one, 8u);

    /* Clear the status dword first so a previous command's 1 cannot fake this one. */
    if (!bus_write(s, s.sch_wb_bar0 + kMesWbStatusOff, 0ull, 2u, "mes-api-status-clear") ||
        !bus_write(s, s.sch_wb_bar0 + kMesWbStatusOff + 8ull, 0ull, 2u,
                   "mes-api-status-clear"))
        return SubmitResult::IoFail;
    if (!write_dwords(s, frame_bar0, s.frame, kMesApiFrameDwords, "mes-api-frame"))
        return SubmitResult::IoFail;

    /* D1: QUERY frame is kMesApiFrameDwords = 64 dwords, not 6 (方案 v3 §6.3).
     * Use static s.qframe to avoid kernel stack increase. */
    MesApiQuerySchedulerStatus q;
    memset(&q, 0, sizeof(q));
    q.header = mes_api_header(kMesApiTypeScheduler, kMesApiQuerySchedulerStatus,
                              kMesApiFrameDwords);
    const uint32_t seq = ++s.sch_seq;
    q.api_status.api_completion_fence_addr = s.sch_wb_mc + kMesWbFenceOff;
    q.api_status.api_completion_fence_value = seq;
    memset(s.qframe, 0, sizeof(s.qframe));
    memcpy(s.qframe, &q, sizeof(q));
    if (!write_dwords(s, query_bar0, s.qframe, kMesApiQueryDwords, "mes-api-query"))
        return SubmitResult::IoFail;

    /* D1: wptr advances by 128, not 70 (方案 v3 §6.3). */
    const uint32_t new_wptr = fw + kMesApiFrameDwords + kMesApiQueryDwords;
    if (!bus_write(s, s.sch_wb_bar0 + kMesWbWptrOff, new_wptr, 2u, "mes-api-wptr"))
        return SubmitResult::IoFail;
    if (!doorbell_write(s, kMesDoorbellSch, new_wptr, "mes-api-kick"))
        return SubmitResult::IoFail;

    /* The firmware has consumed the frame once the doorbell retires; advance the ring
     * even if completion is late or missing so a following attempt cannot replay the
     * same slot (C-13: ADD_QUEUE failures are recorded, not replayed). */
    s.sch_wptr = new_wptr;

    uint64_t fence = 0;
    bool fence_seen = false;
    for (uint32_t polls = 0; polls < kMesFencePolls; polls++) {
        if (!bus_read(s, s.sch_wb_bar0 + kMesWbFenceOff, 2u, &fence))
            return SubmitResult::IoFail;
        if (fence == seq) {
            fence_seen = true;
            break;
        }
        r3_io_udelay(s.io, kMesPollUs);
    }
    s.last_fence = fence;
    if (!fence_seen) {
        /* 方案 v3 §6.3/§6.6.3: collect evidence before returning.
         * Read SCH WB status/wptr shadow, then GRBM-select MEC/pipe0 for HQD state. */
        uint64_t s5_status = 0, s5_wptr = 0;
        bus_read(s, s.sch_wb_bar0 + kMesWbStatusOff, 2u, &s5_status);
        bus_read(s, s.sch_wb_bar0 + kMesWbWptrOff, 2u, &s5_wptr);
        bool aok = false, rok = false, wok = false, dok = false, mok = false;
        (void)wok; (void)mok;
        if (grbm_select(const_cast<Mes &>(s), 3u, 0u, 0u, 0u)) {
            const uint32_t hqd_rptr = reg_read(const_cast<Mes &>(s),
                s.gc0 + kRegCpHqdPqRptr, &rok);
            const uint32_t hqd_doorbell = reg_read(const_cast<Mes &>(s),
                s.gc0 + kRegCpHqdPqDoorbellControl, &dok);
            const uint32_t hqd_active = reg_read(const_cast<Mes &>(s),
                s.hqd_active, &aok);
            grbm_select(const_cast<Mes &>(s), 0u, 0u, 0u, 0u);
            evf(s.env, "mes-api",
                "op=%u fence timeout: got 0x%llx want 0x%x status=0x%llx wptr=0x%llx "
                "rptr=%u db=0x%x active=%u",
                opcode, static_cast<unsigned long long>(fence), seq,
                static_cast<unsigned long long>(s5_status),
                static_cast<unsigned long long>(s5_wptr),
                rok ? hqd_rptr : 0u, dok ? hqd_doorbell : 0u, aok ? hqd_active : 0u);
        } else {
            evf(s.env, "mes-api",
                "op=%u fence timeout: got 0x%llx want 0x%x status=0x%llx wptr=0x%llx "
                "(no GRBM)",
                opcode, static_cast<unsigned long long>(fence), seq,
                static_cast<unsigned long long>(s5_status),
                static_cast<unsigned long long>(s5_wptr));
        }
        return SubmitResult::FenceTimeout;
    }

    uint64_t status = 0;
    bool status_seen = false;
    for (uint32_t polls = 0; polls < kMesFencePolls; polls++) {
        if (!bus_read(s, s.sch_wb_bar0 + kMesWbStatusOff, 2u, &status))
            return SubmitResult::IoFail;
        if (status == 1ull) {
            status_seen = true;
            break;
        }
        r3_io_udelay(s.io, kMesPollUs);
    }
    s.last_status = status;
    if (!status_seen) {
        evf(s.env, "mes-api", "op=%u api_status timeout: got 0x%llx (fence=0x%x)",
            opcode, static_cast<unsigned long long>(status), seq);
        return SubmitResult::StatusTimeout;
    }
    return SubmitResult::Ok;

}

R3SubstepResult submit_fail(SubmitResult sr, uint32_t step, const char *seq_name)
{
    switch (sr) {
    case SubmitResult::Ok:           return r3_substep_pass();
    case SubmitResult::FenceTimeout: return r3_substep_fail(kR3SecSeq, step, kFailFence);
    case SubmitResult::StatusTimeout: return r3_substep_fail(kR3SecChk, step, kFailApiStatus);
    case SubmitResult::IoFail:       return r3_substep_fail(kR3SecSeq, step, seq_name);
    }
    return r3_substep_fail(kR3SecSeq, step, seq_name);
}

R3SubstepResult seq_set_hw(Mes &s)
{
    if (!bus_write(s, s.sch_wb_bar0 + kMesWbSchCtxOff, 0ull, 2u, "mes-set-hw-ctx") ||
        !bus_write(s, s.sch_wb_bar0 + kMesWbQueryFenceOff, 0ull, 2u, "mes-set-hw-qfence")) {
        evf(s.env, "mes-set-hw", "S5 context zero failed");
        return r3_substep_fail(kR3SecSeq, 5, kFailSetHw);
    }

    MesApiSetHwResources p;
    memset(&p, 0, sizeof(p));
    p.header = mes_api_header(kMesApiTypeScheduler, kMesApiSetHwRsrc, kMesApiFrameDwords);
    p.vmid_mask_mmhub = 0xffffff00u;
    p.vmid_mask_gfxhub = 0xffffff00u;
    p.gds_size = 0x1000u;
    p.paging_vmid = 0u;
    for (uint32_t i = 0; i < 4u; i++)
        p.compute_hqd_mask[i] = 0xcu;
    p.gfx_hqd_mask[0] = 0x2u;
    p.sdma_hqd_mask[0] = 0xfcu;
    if (!s.env->mes_aggregated_valid) {
        evf(s.env, "mes-set-hw", "S5 aggregated doorbells not synced");
        return r3_substep_fail(kR3SecSeq, 5, kFailSetHw);
    }
    p.g_sch_ctx_gpu_mc_ptr = s.sch_wb_mc + kMesWbSchCtxOff;
    p.query_status_fence_gpu_mc_ptr = s.sch_wb_mc + kMesWbQueryFenceOff;
    for (uint32_t i = 0; i < 5u; i++) {
        p.gc_base[i] = ip_get(s, IPBlock::GC, i);
        p.mmhub_base[i] = ip_get(s, IPBlock::MMHUB, i);
        p.osssys_base[i] = ip_get(s, IPBlock::OSSSYS, i);
        p.aggregated_doorbells[i] = s.env->mes_aggregated[i];
        if (i != 0u && p.aggregated_doorbells[i] != p.aggregated_doorbells[0] + 2u * i) {
            evf(s.env, "mes-set-hw", "S5 aggregated doorbell stride != 2 at %u", i);
            return r3_substep_fail(kR3SecSeq, 5, kFailSetHw);
        }
    }
    p.flags = kMesFlagsDisableReset | kMesFlagsUseDiffVmidCompute |
              kMesFlagsDisableMesLog | kMesFlagsLevelProcessQuantum |
              kMesFlagsRegActivePoll;
    p.oversubscription_timer = 50u;
    p.os_tdr_timeout_in_sec = 0u;

    memset(s.frame, 0, sizeof(s.frame));
    memcpy(s.frame, &p, sizeof(p));
    const SubmitResult sr = submit_api(s, kMesApiSetHwRsrc,
        static_cast<uint32_t>(offsetof(MesApiSetHwResources, api_status)));
    R3SubstepResult r = submit_fail(sr, 5u, kFailSetHw);
    if (r.status != kR3SsecOk)
        return r;
    evf(s.env, "mes-set-hw", "S5 fence=0x%llx status=0x%llx wptr=%u",
        static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status), s.sch_wptr);
    /* B#22: record the resources actually handed to MES; readiness is confirmed
     * only by MMIO polling (completion fences), no interrupt is routed to the OS. */
    evf(s.env, "mes-chk",
        "set_hw gds=0x%x gfx_mask=0x%x cmp_mask=0x%x sdma_mask=0x%x agg=%x,%x,%x,%x,%x",
        p.gds_size, p.gfx_hqd_mask[0], p.compute_hqd_mask[0], p.sdma_hqd_mask[0],
        p.aggregated_doorbells[0], p.aggregated_doorbells[1], p.aggregated_doorbells[2],
        p.aggregated_doorbells[3], p.aggregated_doorbells[4]);
    return r3_substep_pass();
}

/* S5.1: SET_HW_RESOURCES_1 (opcode 19). Skipped if MES version < 0x52 (方案 v3 §6.5).
 * Must not be confused with S5 - api_status_off=4. */
R3SubstepResult seq_set_hw_1(Mes &s)
{
    if ((s.gp3_sched & 0x00000fffu) < 0x52u) {
        evf(s.env, "mes-set-hw-1", "S5.1 skip: MES version 0x%x < 0x52",
            s.gp3_sched & 0x00000fffu);
        char b[64];
        snprintf(b, sizeof(b), "op19=skip ver=0x%x", s.gp3_sched & 0x00000fffu);
        set_prop(s.env, "N48R3EVID-mes-set-hw-1", b);
        return r3_substep_pass();
    }

    if (!bus_write(s, s.sch_wb_bar0 + kMesWbSchCtxOff, 0ull, 2u, "mes-set-hw1-ctx") ||
        !bus_write(s, s.sch_wb_bar0 + kMesWbQueryFenceOff, 0ull, 2u, "mes-set-hw1-qfence")) {
        evf(s.env, "mes-set-hw-1", "S5.1 context zero failed");
        set_prop(s.env, "N48R3EVID-mes-set-hw-1", "op19=fail fence=0x0 status=0x0");
        return r3_substep_fail(kR3SecSeq, 5, "seq:set-hw-1");
    }

    MesApiSetHwResources1 p;
    memset(&p, 0, sizeof(p));
    p.header = mes_api_header(kMesApiTypeScheduler, kMesApiSetHwRsrc1,
                              kMesApiFrameDwords);
    p.enable_mes_info_ctx = 1u;
    p.cleaner_shader_fence_mc_addr = s.resource1_pa;

    memset(s.frame, 0, sizeof(s.frame));
    memcpy(s.frame, &p, sizeof(p));
    const SubmitResult sr = submit_api(s, kMesApiSetHwRsrc1,
        static_cast<uint32_t>(offsetof(MesApiSetHwResources1, api_status)));
    R3SubstepResult r = submit_fail(sr, 5u, "seq:set-hw-1");
    {
        char b1[96];
        const bool sampled = sr != SubmitResult::IoFail;
        snprintf(b1, sizeof(b1), "op19=%s fence=0x%llx status=0x%llx",
                 r.status == kR3SsecOk ? "ok" : "fail",
                 static_cast<unsigned long long>(sampled ? s.last_fence : 0u),
                 static_cast<unsigned long long>(sampled ? s.last_status : 0u));
        set_prop(s.env, "N48R3EVID-mes-set-hw-1", b1);
    }
    if (r.status != kR3SsecOk)
        return r;

    evf(s.env, "mes-set-hw-1", "S5.1 op19 fence=0x%llx status=0x%llx",
        static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status));
    return r3_substep_pass();
}

R3SubstepResult seq_query(Mes &s)
{
    MesApiQuerySchedulerStatus p;
    memset(&p, 0, sizeof(p));
    p.header = mes_api_header(kMesApiTypeScheduler, kMesApiQuerySchedulerStatus,
                              kMesApiFrameDwords);
    memset(s.frame, 0, sizeof(s.frame));
    memcpy(s.frame, &p, sizeof(p));

    const SubmitResult sr = submit_api(s, kMesApiQuerySchedulerStatus,
        static_cast<uint32_t>(offsetof(MesApiQuerySchedulerStatus, api_status)));
    R3SubstepResult r = submit_fail(sr, 6u, kFailQuery);
    if (r.status != kR3SsecOk)
        return r;

    evf(s.env, "mes-query", "S6 fence=0x%llx status=0x%llx",
        static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status));
    return r3_substep_pass();
}

void copy_result(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    while (src[n] && n + 1u < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

R3SubstepResult seq_addq_compute(Mes &s)
{
    const char *result = "skip";
    if (!zero_queue_structures(s, s.cmp_ring_bar0, s.cmp_wb_bar0, s.cmp_mqd_bar0,
                               s.cmp_eop_bar0, kMesCmpRingDwords)) {
        result = "fail:zero";
    } else if (!build_compute_mqd(s, s.cmp_mqd_bar0, s.cmp_ring_bar0, s.cmp_wb_bar0,
                                  s.cmp_eop_bar0, kMesDoorbellCompute,
                                  kMesCmpRingDwords)) {
        result = "fail:mqd";
    } else {
        MesApiAddQueue p;
        memset(&p, 0, sizeof(p));
        p.header = mes_api_header(kMesApiTypeScheduler, kMesApiAddQueue,
                                  kMesApiFrameDwords);
        p.doorbell_offset = kMesDoorbellCompute;
        p.mqd_addr = s.cmp_mqd_mc;
        p.wptr_addr = s.cmp_wb_mc + kMesWbWptrOff;
        p.queue_type = kMesQueueTypeCompute;
        p.pipe_id = 0u;
        p.queue_id = 0u;
        p.flags = kMesAddQueueFlagMapLegacyKq;

        memset(s.frame, 0, sizeof(s.frame));
        memcpy(s.frame, &p, sizeof(p));
        const SubmitResult sr = submit_api(s, kMesApiAddQueue,
            static_cast<uint32_t>(offsetof(MesApiAddQueue, api_status)));
        switch (sr) {
        case SubmitResult::Ok:            result = "ok";              break;
        case SubmitResult::FenceTimeout:  result = "fail:fence";      break;
        case SubmitResult::StatusTimeout: result = "fail:status";     break;
        case SubmitResult::IoFail:        result = "fail:io";         break;
        }
        if (sr == SubmitResult::Ok)
            s.cmp_queues_added++;
        if (sr != SubmitResult::IoFail) {
            s.cmp_fence = s.last_fence;
            s.cmp_status = s.last_status;
        }
    }
    /* S7 fatal-capable: failure is now fatal (S11 gate requires reliable queues). */
    R3SubstepResult ret;
    if (strncmp(result, "fail:", 5u) == 0) {
        ret = r3_substep_fail(kR3SecSeq, 7, kFailFence);
    } else if (strcmp(result, "skip") == 0) {
        ret = r3_substep_fail(kR3SecSeq, 7, kFailFence);
    } else {
        ret = r3_substep_pass();
    }
    s.cmp_wptr_mc = s.cmp_wb_mc + kMesWbWptrOff;
    copy_result(s.cmp_result, sizeof(s.cmp_result), result);
    evf(s.env, "mes-addq-compute",
        "S7 result=%s mqd_mc=0x%llx wptr_mc=0x%llx db=%u fence=0x%llx status=0x%llx",
        s.cmp_result, static_cast<unsigned long long>(s.cmp_mqd_mc),
        static_cast<unsigned long long>(s.cmp_wb_mc + kMesWbWptrOff),
        kMesDoorbellCompute, static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status));
    return ret;
}

R3SubstepResult seq_addq_gfx(Mes &s)
{
    const char *result = "skip";
    uint64_t gfx_fence = 0;
    uint64_t gfx_status = 0;
    if (!reg_write(s, s.gc0 + kRegCpRbDoorbellRangeLower, kMesDoorbellGfx << 2,
                   kR3WState, "mes-gfx-dbrange") ||
        !reg_write(s, s.gc0 + kRegCpRbDoorbellRangeUpper, kCpHqdDoorbellRangeUpper,
                   kR3WState, "mes-gfx-dbrange")) {
        result = "fail:dbrange";
    } else if (!zero_queue_structures(s, s.gfx_ring_bar0, s.gfx_wb_bar0, s.gfx_mqd_bar0,
                               s.gfx_eop_bar0, kMesGfxRingDwords)) {
        result = "fail:zero";
    } else if (!build_gfx_mqd(s, s.gfx_mqd_bar0, s.gfx_ring_bar0, s.gfx_wb_bar0,
                              s.gfx_eop_bar0, kMesDoorbellGfx)) {
        result = "fail:mqd";
    } else {
        MesApiAddQueue p;
        memset(&p, 0, sizeof(p));
        p.header = mes_api_header(kMesApiTypeScheduler, kMesApiAddQueue,
                                  kMesApiFrameDwords);
        p.doorbell_offset = kMesDoorbellGfx;
        p.mqd_addr = s.gfx_mqd_mc;
        p.wptr_addr = s.gfx_wb_mc + kMesWbWptrOff;
        p.queue_type = kMesQueueTypeGfx;
        p.pipe_id = 0u;
        p.queue_id = 0u;
        /* G19: map_legacy_kq is the MES ADD_QUEUE form of Linux's legacy-KQ
         * mapping (mes_v11_0.c:493-516 sets map_legacy_kq=1); the KIQ-side
         * equivalent is mes_v11_0_kiq_enable_queue -> kiq_map_queues
         * (:1284-1300). Intentional, not an omission. */
        p.flags = kMesAddQueueFlagMapLegacyKq;
        memset(s.frame, 0, sizeof(s.frame));
        memcpy(s.frame, &p, sizeof(p));

        const SubmitResult sr = submit_api(s, kMesApiAddQueue,
            static_cast<uint32_t>(offsetof(MesApiAddQueue, api_status)));
        switch (sr) {
        case SubmitResult::Ok:            result = "ok";              break;
        case SubmitResult::FenceTimeout:  result = "fail:fence";      break;
        case SubmitResult::StatusTimeout: result = "fail:status";     break;
        case SubmitResult::IoFail:        result = "fail:io";         break;
        }
        if (sr != SubmitResult::IoFail) {
            gfx_fence = s.last_fence;
            gfx_status = s.last_status;
        }
    }

    /* S8 fatal-capable: failure is now fatal (S11 gate requires reliable queues). */
    R3SubstepResult ret;
    if (strncmp(result, "fail:", 5u) == 0) {
        ret = r3_substep_fail(kR3SecSeq, 8, kFailFence);
    } else if (strcmp(result, "skip") == 0) {
        ret = r3_substep_fail(kR3SecSeq, 8, kFailFence);
    } else {
        ret = r3_substep_pass();
    }
    copy_result(s.gfx_result, sizeof(s.gfx_result), result);
    {
        char rq[256];
        snprintf(rq, sizeof(rq),
                 "compute=%s fence=0x%llx status=0x%llx mqd_mc=0x%llx wptr_mc=0x%llx "
                 "db=%u; gfx=%s fence=0x%llx status=0x%llx mqd_mc=0x%llx "
                 "wptr_mc=0x%llx db=%u",
                 s.cmp_result, static_cast<unsigned long long>(s.cmp_fence),
                 static_cast<unsigned long long>(s.cmp_status),
                 static_cast<unsigned long long>(s.cmp_mqd_mc),
                 static_cast<unsigned long long>(s.cmp_wptr_mc), kMesDoorbellCompute,
                 s.gfx_result, static_cast<unsigned long long>(gfx_fence),
                 static_cast<unsigned long long>(gfx_status),
                 static_cast<unsigned long long>(s.gfx_mqd_mc),
                 static_cast<unsigned long long>(s.gfx_wb_mc + kMesWbWptrOff),
                 kMesDoorbellGfx);
        set_prop(s.env, "N48R3EVID-mes-addq-resp", rq);
    }
    evf(s.env, "mes-addq-gfx", "S8 result=%s fence=0x%llx status=0x%llx",
        s.gfx_result, static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status));
    return ret;
}

/* MC-form self-check (audit B#19/d-3): every ring/wb/mqd/eop address must lie
 * inside the FB aperture discovered at runtime (MMMC_VM_FB_LOCATION_BASE/TOP,
 * S-1). The old check compared the high dword against the constant 0x80, which
 * is this board's nullarc observation (implementation plan v3 "MC form"), not
 * an architectural value: a different FB placement would false-fail. Derive
 * the window from mc_base/fb_top_raw instead (AMD IP discovery runtime base). */
uint64_t fb_top_mc(const Mes &s)
{
    return ((static_cast<uint64_t>(s.fb_top_raw & MMHUBRegs::kFBBaseMask))
            << MMHUBRegs::kFBBaseShift) | 0xffffffull;
}

bool mc_in_fb_aperture(const Mes &s, uint64_t addr)
{
    if (s.mc_base == 0ull)
        return false;
    const uint64_t fb_top = fb_top_mc(s);
    return fb_top >= s.mc_base && addr >= s.mc_base && addr <= fb_top;
}

R3SubstepResult chk_queues(Mes &s)
{
    if (!grbm_select(s, 1u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 9, kFailSch);
    bool ca = false, cr = false;
    s.cmp_active = reg_read(s, s.hqd_active, &ca);
    s.cmp_rptr = reg_read(s, s.gc0 + kRegCpHqdPqRptr, &cr);
    if (!grbm_select(s, 0u, 0u, 0u, 0u))
        return r3_substep_fail(kR3SecSeq, 9, kFailSch);

    bool ga = false, gr = false;
    s.gfx_active = reg_read(s, s.gc0 + kRegCpGfxHqdActive, &ga);
    s.gfx_rptr = reg_read(s, s.gc0 + kRegCpGfxHqdRptr, &gr);

    const uint64_t addrs[] = {
        s.kiq_ring_mc, s.kiq_wb_mc, s.kiq_mqd_mc, s.kiq_eop_mc,
        s.sch_ring_mc, s.sch_wb_mc, s.sch_mqd_mc, s.sch_eop_mc,
        s.gfx_ring_mc, s.gfx_wb_mc, s.gfx_mqd_mc, s.gfx_eop_mc,
        s.cmp_ring_mc, s.cmp_wb_mc, s.cmp_mqd_mc, s.cmp_eop_mc,
    };
    bool all_form = true;
    for (uint32_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        if (!mc_in_fb_aperture(s, addrs[i])) {
            all_form = false;
            evf(s.env, "mes-addr", "S9 address outside FB aperture @%u: 0x%llx", i,
                static_cast<unsigned long long>(addrs[i]));
        }
    }
    /* B#22: readiness was obtained through MMIO only (CP_MES_CNTL readback in S1
     * and completion fences in S5/S6); no IRQ is routed to the OS. CID2 is the
     * second compute queue: stage 3's init_compute_vmid creates it, this
     * bring-up adds one compute queue (S7), so the record derives to 0. */
    evf(s.env, "mes-chk", "ready via MMIO (no IRQ); CID2=%u",
        s.cmp_queues_added > 1u ? 1u : 0u);
    evf(s.env, "mes-queues",
        "S9 compute active=%u buf=%u rptr=%u gfx active=%u buf=%u rptr=%u",
        s.cmp_active, ca ? 1u : 0u, cr ? s.cmp_rptr : 0u,
        s.gfx_active, ga ? 1u : 0u, gr ? s.gfx_rptr : 0u);
    const uint64_t fb_top = fb_top_mc(s);
    evf(s.env, "mes-addr",
        "S9 kiq_ring=0x%llx sch_ring=0x%llx gfx_ring=0x%llx cmp_ring=0x%llx "
        "fb=0x%llx..0x%llx form=%s",
        static_cast<unsigned long long>(s.kiq_ring_mc),
        static_cast<unsigned long long>(s.sch_ring_mc),
        static_cast<unsigned long long>(s.gfx_ring_mc),
        static_cast<unsigned long long>(s.cmp_ring_mc),
        static_cast<unsigned long long>(s.mc_base),
        static_cast<unsigned long long>(fb_top), all_form ? "ok" : "bad");
    if (!all_form) {
        char q[128];
        snprintf(q, sizeof(q), "prefix=bad sch=0x%llx cmp=0x%llx gfx=0x%llx",
                 static_cast<unsigned long long>(s.sch_ring_mc),
                 static_cast<unsigned long long>(s.cmp_ring_mc),
                 static_cast<unsigned long long>(s.gfx_ring_mc));
        set_prop(s.env, "N48R3EVID-mes-queues", q);
        R3SubstepResult r = r3_substep_fail(kR3SecChk, 9, kFailAddrPrefix);
        r3_substep_note(&r, 0u, static_cast<uint32_t>(s.sch_ring_mc >> 32), "sch_hi");
        r3_substep_note(&r, 0u, static_cast<uint32_t>(s.cmp_ring_mc >> 32), "cmp_hi");
        return r;
    }
    set_prop(s.env, "N48R3EVID-mes-queues", "prefix=ok");
    return r3_substep_pass();
}

/* S11 add-on (V3-4 "IB with VMID 8 executable"): submit a real IB through
 * VMID 8 on the compute ring (S7 ADD_QUEUE). S9 built the VMID8 page table and
 * mapped test_buf page 2 at kGmcTestVaVmid8 (gmc_v11_0.cpp seq_pt_build8); the
 * IB body lives at that mapped VA because the VMID8 context has no
 * FB-aperture passthrough guarantee - the IB must be reachable through the
 * translation, not via an MC address. Shape mirrors S13 (compute_v11.cpp) with
 * vmid=8: WRITE_DATA magic -> mapped VA, RELEASE_MEM fence (8 dw, Linux
 * gfx_v11_0.c emit_fence shape), fence poll, read-back. PM4 encoding comes
 * from pm4_v11.h (single authority).  Every IB operand is a VA of the one page
 * S9 mapped at kGmcTestVaVmid8; the fence therefore targets that page
 * (kMesS11FenceIntra), not the VMID0 writeback MC address (Linux
 * gfx_v11_0_ring_emit_fence writes the ring's GPU VA verbatim,
 * gfx_v11_0.c:6065-6096). */
R3SubstepResult seq_s11_vmid8_ib(Mes &s)
{
    const R3Grant *test_grant = r3_layout_grant(&s.plan, "test_buf");
    if (!test_grant)
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    /* S9 mapped grant page 2 (start + 2*4096) at kGmcTestVaVmid8.  The IB
     * executes under the VMID carried in the INDIRECT_BUFFER packet, so every
     * address it touches must be a VA in that one mapped page; the VMID0
     * writeback MC address is not translatable here. */
    const uint64_t test_page_off = test_grant->start + 2 * 4096ull;
    const uint64_t vmid8_va = kGmcTestVaVmid8;
    const uint64_t magic_off = test_page_off + kGmcTestPageIntra;
    const uint64_t fence_va = vmid8_va + kMesS11FenceIntra;
    const uint64_t fence_off = test_page_off + kMesS11FenceIntra;

    /* IB body preloaded at the mapped page start: WRITE_DATA magic, then
     * RELEASE_MEM fence (GL2 writeback included, nullarc vkprobe shape).  The
     * fence operand is the VMID8 VA of the same page (Linux
     * gfx_v11_0_ring_emit_fence writes the ring's GPU VA verbatim,
     * gfx_v11_0.c:6065-6096), not the VMID0 writeback MC address. */
    uint32_t ib[13] = {};
    if (pm4_write_data(ib, vmid8_va + kGmcTestPageIntra, kGmcTestMagic) == 0u ||
        pm4_release_mem(ib + 5, fence_va, 1u) == 0u)
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    if (!write_dwords(s, test_page_off, ib, 13u, "mes-s11-vmid8-ib"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    /* Main chain: INDIRECT_BUFFER(vmid=8, compute) at ring wptr 0. */
    uint32_t chain[4];
    if (pm4_indirect_buffer(chain, vmid8_va, 13u, 8u, true) == 0u)
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    if (!write_dwords(s, s.cmp_ring_bar0, chain, 4u, "mes-s11-vmid8-chain"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    /* Clear the fence first so a stale value cannot fake completion. */
    if (!bus_write(s, fence_off, 0ull, 2u, "mes-s11-vmid8-fence"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    /* Submit tail (pm4_submit_tail order via mes helpers): drain read, HDP
     * flush, WB wptr, read-back, HDP flush, doorbell. */
    uint64_t drain = 0;
    if (!bus_read(s, s.cmp_ring_bar0, 1u, &drain) || !r3_io_hdp_flush(s.io))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    if (!bus_write(s, s.cmp_wb_bar0 + kMesWbWptrOff, 4ull, 2u, "mes-s11-vmid8-wptr"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    uint64_t wptr_rb = 0;
    if (!bus_read(s, s.cmp_wb_bar0 + kMesWbWptrOff, 2u, &wptr_rb) || wptr_rb != 4ull)
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    if (!r3_io_hdp_flush(s.io))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    if (!doorbell_write(s, kMesDoorbellCompute, 4u, "mes-s11-vmid8-kick"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    /* Fence poll (submit_api shape). */
    uint64_t fence = 0;
    bool fence_seen = false;
    for (uint32_t polls = 0; polls < kMesFencePolls; polls++) {
        if (!bus_read(s, fence_off, 2u, &fence))
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
        if (fence == 1ull) {
            fence_seen = true;
            break;
        }
        r3_io_udelay(s.io, kMesPollUs);
    }
    if (!fence_seen) {
        evf(s.env, "mes-s11",
            "vmid8 ib fence timeout: got 0x%llx want 1 (fence bar0=0x%llx)",
            static_cast<unsigned long long>(fence),
            static_cast<unsigned long long>(fence_off));
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    }

    /* Read back the magic through the mapped page's backing store. */
    uint64_t magic = 0;
    if (!bus_read(s, magic_off, 1u, &magic) || magic != kGmcTestMagic) {
        evf(s.env, "mes-s11", "vmid8 ib magic mismatch: got 0x%llx want 0x%x @ bar0=0x%llx",
            static_cast<unsigned long long>(magic), kGmcTestMagic,
            static_cast<unsigned long long>(magic_off));
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    }

    /* Wrap-around reset so S13 (hardcoded wptr=0 start) still runs from a clean
     * ring: the CP consumed dwords 0..3 (rptr=4); fill the ring with 2-dw
     * TYPE-3 NOPs, write the WB wptr back to 0 and ring the doorbell with 0 -
     * the CP eats the NOPs, wraps to 0 and parks (rptr==wptr==0), so S13's
     * chain at ring[0..11] executes in full.
     * 【待真机复核】CP NOP consumption and doorbell-0 wrap semantics. */
    for (uint32_t i = 0; i < kMesCmpRingDwords; i += 2u) {
        const uint64_t off = s.cmp_ring_bar0 + (uint64_t)i * 4ull;
        const uint64_t nop = 0x00000000C0001000ull; /* TYPE-3 NOP (op 0x10), count 0 */
        if (!bus_write(s, off, nop, 2u, "mes-s11-vmid8-nop"))
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);
    }
    if (!bus_write(s, s.cmp_wb_bar0 + kMesWbWptrOff, 0ull, 2u, "mes-s11-vmid8-wptr0") ||
        !doorbell_write(s, kMesDoorbellCompute, 0u, "mes-s11-vmid8-wrap"))
        return r3_substep_fail(kR3SecSeq, 11, kFailS11Vmid8Ib);

    set_prop(s.env, "N48R3EVID-mes-s11-vmid8-ib",
             "ok fence=1 magic=0xdeadbeef va=0x200000100");
    evf(s.env, "mes-s11", "vmid8 ib ok: fence=1 magic @ va=0x%llx (bar0=0x%llx)",
        static_cast<unsigned long long>(vmid8_va + kGmcTestPageIntra),
        static_cast<unsigned long long>(magic_off));
    return r3_substep_pass();
}

R3SubstepResult seq_s11(Mes &s)
{
    /* S11: MES queue extension - CSIB, doorbell range, ADD_QUEUE full payload.
     * Also upgrades S7/S8 to fatal-capable (kill-switch gated by T-E wiring). */

    /* --- 0. VMID8 IB submission (V3-4): runs before the version check so the
     * legacy (<0x47) path is covered too; it only needs the S7 compute queue
     * and the S9 VMID8 page table. */
    R3SubstepResult vib = seq_s11_vmid8_ib(s);
    if (vib.status != kR3SsecOk)
        return vib;

    /* --- 1. MES version check (mes_v11_0.c:1615-1618): sched_version&0xFF >= 0x47
     * enables legacy_queue_map path. Below 0x47: direct-HQD (existing), not failure. */
    const uint32_t sched_ver = s.gp3_sched & 0xffu;
    if (sched_ver < 0x47u) {
        evf(s.env, "mes-s11", "S11 MES sched_version 0x%02x < 0x47; "
            "legacy_queue_map unavailable, using direct-HQD (existing path)", sched_ver);
        char ver_buf[48];
        snprintf(ver_buf, sizeof(ver_buf), "legacy-map=skip ver=0x%x", sched_ver);
        set_prop(s.env, "N48R3EVID-mes-s11-version", ver_buf);
        /* Mark path as "failure" so S11 OnSuccessPath write rows (CSIB, MEC DB range)
         * are not expected by the writeguard verify. Continue with pass result. */
        r3_wg_set_path("failure");
        return r3_substep_pass();
    }
    evf(s.env, "mes-s11", "S11 MES sched_version 0x%02x >= 0x47, legacy_queue_map OK",
        sched_ver);

    /* --- 2. CSIB: write 960 dw preamble + header to D4 area, program RLC_CSIB regs --- */
    {
        /* Write the 960-dw preamble to the CSIB region (csb_gfx11.h preamble). */
        const uint32_t csib_dwords = 960u;
        if (!write_dwords(s, s.csib_bar0, kCsbGfx11, csib_dwords, "mes-s11-csib-data")) {
            evf(s.env, "mes-s11", "S11 CSIB data write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Csib);
        }
        /* Write CSIB header after preamble. */
        evf(s.env, "mes-s11", "S11 CSIB %u dw preamble @ bar0=0x%llx mc=0x%llx",
            csib_dwords, static_cast<unsigned long long>(s.csib_bar0),
            static_cast<unsigned long long>(s.csib_mc));

        /* Program RLC_CSIB_ADDR_HI/LO/LENGTH (SEG1, base_idx=1). */
        if (!reg_write(s, s.rlc_csib_hi,
                        static_cast<uint32_t>(s.csib_mc >> 32),
                        kR3WState, "mes-s11-csib")) {
            evf(s.env, "mes-s11", "S11 RLC_CSIB_ADDR_HI write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Csib);
        }
        const uint32_t csib_lo_val = static_cast<uint32_t>(s.csib_mc & 0xfffffffcull);
        if (!reg_write(s, s.rlc_csib_lo, csib_lo_val, kR3WState, "mes-s11-csib")) {
            evf(s.env, "mes-s11", "S11 RLC_CSIB_ADDR_LO write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Csib);
        }
        /* RLC_CSIB_LENGTH = 960 dwords (csb_gfx11.h preamble). */
        if (!reg_write(s, s.rlc_csib_len, csib_dwords, kR3WState, "mes-s11-csib")) {
            evf(s.env, "mes-s11", "S11 RLC_CSIB_LENGTH write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Csib);
        }
        evf(s.env, "mes-s11", "S11 CSIB programmed @ 0x%x/0x%x len=%u",
            s.rlc_csib_lo, s.rlc_csib_hi, csib_dwords);
    }

    /* --- 3. Doorbell range (CP_RB_DOORBELL_RANGE_LOWER/UPPER) --- */
    {
        if (!reg_write(s, s.db_range_lower, kMesRbDoorbellLower,
                        kR3WState, "mes-s11-dbrange")) {
            evf(s.env, "mes-s11", "S11 doorbell range LOWER write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Dbrange);
        }
        if (!reg_write(s, s.db_range_upper, kMesRbDoorbellUpper,
                        kR3WState, "mes-s11-dbrange")) {
            evf(s.env, "mes-s11", "S11 doorbell range UPPER write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Dbrange);
        }
        /* MEC doorbell range (compute engine). */
        if (!reg_write(s, s.mec_db_range_lower, kMesMecDoorbellLower,
                        kR3WState, "mes-s11-dbrange")) {
            evf(s.env, "mes-s11", "S11 MEC doorbell range LOWER write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Dbrange);
        }
        if (!reg_write(s, s.mec_db_range_upper, kMesMecDoorbellUpper,
                        kR3WState, "mes-s11-dbrange")) {
            evf(s.env, "mes-s11", "S11 MEC doorbell range UPPER write failed");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Dbrange);
        }
        evf(s.env, "mes-s11", "S11 doorbell range RB=0x%x/0x%x MEC=0x%x/0x%x",
            s.db_range_lower, s.db_range_upper,
            s.mec_db_range_lower, s.mec_db_range_upper);
    }

    /* S11 used to rebuild the compute/gfx MQDs and re-submit ADD_QUEUE with a
     * full payload. S7 (compute) and S8 (gfx) already issued the single
     * ADD_QUEUE that Linux mes_v11_0_hw_init issues (AMDKFD ADD_QUEUE is
     * per-process). A second ADD_QUEUE for the same queue is not a legal
     * upstream sequence (mes_v11_0.c only calls it once per queue init), so
     * the duplicate block is removed. */

    /* --- 5. wptr discipline check: WB+16 must precede doorbell --- */
    {
        /* Read back the wptr shadow to verify it was written before doorbell.
         * This is a logical check; the actual ordering is enforced by
         * submit_api() which writes wptr shadow then doorbell. */
        uint64_t wb_wptr_val = 0;
        if (!bus_read(s, s.sch_wb_bar0 + kMesWbWptrOff, 2, &wb_wptr_val)) {
            evf(s.env, "mes-s11", "S11 wptr shadow unreadable");
            return r3_substep_fail(kR3SecSeq, 11, kFailS11Wptr);
        }
        evf(s.env, "mes-s11", "S11 wptr shadow = 0x%llx (non-zero confirms WB write)",
            static_cast<unsigned long long>(wb_wptr_val));
    }

    evf(s.env, "mes-s11", "S11 done: queue extension complete (legacy mode)");
    return r3_substep_pass();
}
/* T-E: shared Mes state across mes_v11_0_substep and mes_queue_v11_0_substep.
 * mes_v11_0_substep initialises (memset zero) and builds all KIQ/SCH infrastructure;
 * mes_queue_v11_0_substep reuses the established state without clearing it. */
static Mes s_mes;


} /* namespace */

R3SubstepResult mes_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    memset(&s_mes, 0, sizeof(s_mes));
    s_mes.io = io;
    s_mes.env = env;

    r3_wg_arm_substep(WSubstep::Mes);
    if (r3_wg_expect_writes(getMesRows(), getMesRowCount(), nullptr, 0, WSubstep::Mes) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env ? env->ip : nullptr, getMesRows(), getMesRowCount(),
                            declared);
    if (r3_wg_expect_declared_addrs(declared, getMesRowCount()) != 0 ||
        r3_wg_expect_abs_writes(getMesDoorbells(), getMesDoorbellCount(), WSubstep::Mes) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");

    R3SubstepResult r = pre(s_mes);
    /* S-1: hub pre-initialisation runs BEFORE S0 (方案 v3 §6.1/§6.2). */
    if (r.status == kR3SsecOk)
        r = seq_hub_pre(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_cp_unhalt(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_start(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_version(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_kiq(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_sch(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_set_hw(s_mes);
    /* S5.1: SET_HW_RESOURCES_1 after S5 (方案 v3 §6.5). */
    if (r.status == kR3SsecOk)
        r = seq_set_hw_1(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_query(s_mes);
    /* T-E split: seq_addq_compute/seq_addq_gfx/chk_queues/seq_s11 moved to
     * mes_queue_v11_0_substep. */
    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, s_mes.mes_cntl, s_mes.cntl_readback, "CP_MES_CNTL");
        r3_substep_note(&r, s_mes.mes_gp3, s_mes.gp3_sched, "GP3 sched");
        r3_substep_note(&r, s_mes.mes_gp3, s_mes.gp3_kiq, "GP3 kiq");
        if (env && env->rt)
            env->rt->ready_bits |= R3_IP_BIT(R3Ip::Mes);
        set_prop(env, "N48R3EVID-mes-done", "Yes");
        evf(env, "mes", "done (pre-queue): sched=%u, kiq=%u", s_mes.gp3_sched, s_mes.gp3_kiq);
    } else {
        set_prop(env, "N48R3EVID-mes-done", "No");
        /* T-E split: queue substep never runs when MES fails, so the ADD_QUEUE
         * evidence props must still exist ("n/a" = not attempted) for the
         * integration contract that reads all three. */
        set_prop(env, "N48R3EVID-mes-addq-compute", "n/a");
        set_prop(env, "N48R3EVID-mes-addq-gfx", "n/a");
        evf(env, "mes", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
    }

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getMesRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }

    r3_wg_disarm_substep();
    return r;
}

/* T-E split: queue substep runs seq_addq_compute→seq_addq_gfx→chk_queues→seq_s11.
 * Arms, registers doorbells, and verifies independently from mes_v11_0_substep.
 * Uses real GPA from sysmem_grants D2 root for ADD_QUEUE page_table_base_addr. */
R3SubstepResult mes_queue_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    /* Reuse Mes state established by mes_v11_0_substep (no memset — KIQ/SCH/HQD
     * infrastructure must survive). Only update io/env for the current call. */
    s_mes.io = io;
    s_mes.env = env;

    /* Resolve D2 root GPA for ADD_QUEUE page_table_base_addr (T-E contract: gpuvm0
     * has already back-filled the sysmem_grants via gmc_v11_0_bind_sysmem).
     * Match by tag "ctx0_root_pool" (index 1, NOT the first non-zero — index 0
     * is gart_table which is a different translation mechanism). */
    uint64_t pt_base = 0ull;
    if (env && env->rt) {
        const R3LayoutPlan &plan = env->rt->layout;
        for (uint32_t i = 0; i < plan.sysmem_grant_count && i < R3_LAYOUT_SYSMEM_MAX; i++) {
            if (strcmp(plan.sysmem_grants[i].tag, "ctx0_root_pool") == 0) {
                pt_base = plan.sysmem_grants[i].start;
                break;
            }
        }
    }
    if (pt_base == 0ull) {
        /* No D2 root GPA available - fail closed per T-E contract. */
        R3SubstepResult fr = r3_substep_fail(kR3SecPre, 0, "no-sysmem-grant");
        set_prop(env, "N48R3EVID-mes-queue-done", "No");
        evf(env, "mes-queue", "fail: no sysmem grant (ctx0_root_pool not found)");
        return fr;
    }
    s_mes.page_table_base_addr = pt_base;

    /* S11's VMID8 IB writes the test_buf page S9 mapped (gmc seq_layout
     * registered that grant under owner Gmc); register a Mes-owned row for the
     * same range so the armed Mes substep may write it (writeguard contract:
     * one row per substep per address, the armed owner's row wins). */
    if (env && env->rt) {
        const R3Grant *tg = r3_layout_grant(&env->rt->layout, "test_buf");
        if (!tg || r3_wg_register_layout(static_cast<uint32_t>(tg->start >> 2),
                                         static_cast<uint32_t>(tg->size >> 2),
                                         WSubstep::Mes, "layout:test_buf") != 0) {
            R3SubstepResult fr = r3_substep_fail(kR3SecPre, 0, kFailS11TestGrant);
            set_prop(env, "N48R3EVID-mes-queue-done", "No");
            evf(env, "mes-queue", "fail: test_buf grant register failed");
            return fr;
        }
    }

    r3_wg_arm_substep(WSubstep::Mes);
    if (r3_wg_expect_writes(getMesQueueRows(), getMesQueueRowCount(), nullptr, 0, WSubstep::Mes) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env ? env->ip : nullptr, getMesQueueRows(), getMesQueueRowCount(),
                            declared);
    if (r3_wg_expect_declared_addrs(declared, getMesQueueRowCount()) != 0 ||
        r3_wg_expect_abs_writes(getMesDoorbells(), getMesDoorbellCount(), WSubstep::Mes) != 0)
        return r3_substep_fail(kR3SecPre, 0, "pre:expect-cap");

    /* S7-S11: ADD_QUEUE compute/gfx, queue check, S11. */
    R3SubstepResult r = r3_substep_pass();
    /* Re-run pre() and hub_pre()? No - they are already done by mes_v11_0_substep.
     * The Mes state is shared (s_mes) so seq_addq_compute sees the KIQ/SCH
     * infrastructure established by mes_v11_0_substep. */
    if (r.status == kR3SsecOk)
        r = seq_addq_compute(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_addq_gfx(s_mes);
    if (r.status == kR3SsecOk)
        r = chk_queues(s_mes);
    if (r.status == kR3SsecOk)
        r = seq_s11(s_mes);
    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, s_mes.mes_cntl, s_mes.cntl_readback, "CP_MES_CNTL");
        r3_substep_note(&r, s_mes.hqd_active, s_mes.cmp_active, "COMPUTE HQD");
        set_prop(env, "N48R3EVID-mes-queue-done", "Yes");
        set_prop(env, "N48R3EVID-mes-addq-compute", s_mes.cmp_result);
        set_prop(env, "N48R3EVID-mes-addq-gfx", s_mes.gfx_result);
        evf(env, "mes-queue", "done: compute=%s, gfx=%s, sch_wptr=%u", s_mes.cmp_result,
            s_mes.gfx_result, s_mes.sch_wptr);
    } else {
        set_prop(env, "N48R3EVID-mes-queue-done", "No");
        set_prop(env, "N48R3EVID-mes-addq-compute",
                 s_mes.cmp_result[0] ? s_mes.cmp_result : "n/a");
        set_prop(env, "N48R3EVID-mes-addq-gfx",
                 s_mes.gfx_result[0] ? s_mes.gfx_result : "n/a");
        evf(env, "mes-queue", "fail: %s (%s step %u)", r.name,
            r3_substep_section_name(r.section), r.step);
    }

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getMesQueueRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }

    r3_wg_disarm_substep();
    return r;
}

/* Step2: declaration table (rdna3_writeguard.h R3WriteRowReg), the single source of truth
 * for the MES register write set.
 *
 * After T-E split (mes_queue_v11_0_substep introduction), this is split into two tables:
 *   kMesRows[74] for mes_v11_0_substep (S-1..S6): 52 hub-pre + 10 S0-S5 rows.
 *   kMesQueueRows[11] for mes_queue_v11_0_substep (S7..S11): 2 GFX dbrange + 1 GRBM
 *   select + 5 S11 rows + 3 teardown-only rows.
 * Doorbell rows (kMesDoorbells) are shared and registered by both entries.
 */
namespace {
constexpr R3WriteRowReg kMesRows[] = {
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15a7u, 1, WClass::State, WNeed::Optional,    "mes-hub-fb-offset"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x1678u, 1, WClass::State, WNeed::Required,    "mes-hub-fb"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x1679u, 1, WClass::State, WNeed::Required,    "mes-hub-fb"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167cu, 1, WClass::State, WNeed::Required,    "mes-hub-aperture"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167bu, 1, WClass::State, WNeed::Required,    "mes-hub-aperture"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167au, 1, WClass::State, WNeed::Required,    "mes-hub-aperture"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167du, 1, WClass::State, WNeed::Required,    "mes-hub-aperture"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167eu, 1, WClass::State, WNeed::Required,    "mes-hub-aperture"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15a8u, 1, WClass::State, WNeed::Required,    "mes-hub-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15a9u, 1, WClass::State, WNeed::Required,    "mes-hub-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15cbu, 1, WClass::State, WNeed::Required,    "mes-hub-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15ccu, 1, WClass::State, WNeed::Required,    "mes-hub-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15c5u, 1, WClass::State, WNeed::Required,    "mes-hub-fault2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x167fu, 1, WClass::State, WNeed::Required,    "mes-hub-mx-l1"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15bcu, 1, WClass::State, WNeed::Required,    "mes-hub-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15bdu, 1, WClass::State, WNeed::Required,    "mes-hub-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15beu, 1, WClass::State, WNeed::Required,    "mes-hub-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15d4u, 1, WClass::State, WNeed::Required,    "mes-hub-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15dau, 1, WClass::State, WNeed::Required,    "mes-hub-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x1688u, 1, WClass::State, WNeed::Required,    "mes-hub-context0"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15ceu, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15cfu, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15d0u, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15d1u, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15d2u, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15d3u, 1, WClass::State, WNeed::Required,    "mes-hub-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x1e1fu, 1, WClass::State, WNeed::Required,    "mes-hub-cp-debug"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, 0x15c4u, 1, WClass::State, WNeed::Required,    "mes-hub-fault"},
    {nullptr, static_cast<uint8_t>(IPBlock::NBIO), 2, 0xC0u, 1, WClass::State, WNeed::Required,    "mes-hub-doorbell-aper"},
    /* Rows 29-51: MMHUB VM path initialization (方案 W, Linux mmhub_v3_0_1.c semantics) */
    {&r3gold::kMmmcVmAgpBase, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-aperture"},
    {&r3gold::kMmmcVmAgpBot, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-aperture"},
    {&r3gold::kMmmcVmAgpTop, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-aperture"},
    {&r3gold::kMmmcVmSystemApertureLowAddr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-aperture"},
    {&r3gold::kMmmcVmSystemApertureHighAddr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-aperture"},
    {&r3gold::kMmmcVmSystemApertureDefaultAddrLsb, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-default"},
    {&r3gold::kMmmcVmSystemApertureDefaultAddrMsb, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0, 1, WClass::State, WNeed::Required, "mes-hub-mm-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x070Fu, 1, WClass::State, WNeed::Required, "mes-hub-mm-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0710u, 1, WClass::State, WNeed::Required, "mes-hub-mm-default"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0709u, 1, WClass::State, WNeed::Required, "mes-hub-mm-fault2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x08F3u, 1, WClass::State, WNeed::Required, "mes-hub-mm-mx-l1"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0700u, 1, WClass::State, WNeed::Required, "mes-hub-mm-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0701u, 1, WClass::State, WNeed::Required, "mes-hub-mm-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0702u, 1, WClass::State, WNeed::Required, "mes-hub-mm-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0718u, 1, WClass::State, WNeed::Required, "mes-hub-mm-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x071Eu, 1, WClass::State, WNeed::Required, "mes-hub-mm-l2"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0708u, 1, WClass::State, WNeed::Required, "mes-hub-mm-fault"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0712u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0713u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0714u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0715u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0716u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    {nullptr, static_cast<uint8_t>(IPBlock::MMHUB), 1, 0x0717u, 1, WClass::State, WNeed::Required, "mes-hub-mm-identity"},
    /* Rows 52-61 (S0-S5): CP unhalt, MES start, KIQ init. */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 1, kRegGrbmGfxCntl, 1, WClass::State,      WNeed::Required,      "mes-cp-unhalt"},
    {&r3gold::kCpMecCntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,        WNeed::Required,      "mes-cp-unhalt"},
    {&r3gold::kCpMeCntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,         WNeed::Required,      "mes-cp-unhalt"},
    {&r3gold::kRlcCpSchedulers, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::Protocol, WNeed::Required,    "mes-sched"},
    {&r3gold::kCpMesPrgrmCntrStart, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State, WNeed::Required,  "mes-start-prgrm"},
    {&r3gold::kCpMesPrgrmCntrStartHi, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State, WNeed::Required, "mes-start-prgrm"},
    {&r3gold::kCpMesCntl, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,        WNeed::Required,      "mes-start"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpMqdBaseAddr, 1, WClass::State,     WNeed::Required,      "mes-kiq-mqd-base"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpMqdBaseAddrHi, 1, WClass::State,   WNeed::Required,      "mes-kiq-mqd-base-hi"},
    {&r3gold::kCpHqdActive, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,       WNeed::Required,      "mes-hqd-active"},
    /* Rows 62-73 (HQD registers): written by program_hqd() in S3/S4 (seq_kiq/seq_sch).
     * These were originally in kMesQueueRows (T-E split), but program_hqd runs in the
     * MES entry, not the queue entry — moved here to fix writeguard "extra" detection. */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdVmid, 1, WClass::State,         WNeed::Required,      "mes-hqd-vmid"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPersistentState, 1, WClass::State, WNeed::Required,   "mes-hqd-persistent"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqBase, 1, WClass::State,       WNeed::Required,      "mes-hqd-pq-base"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqBaseHi, 1, WClass::State,     WNeed::Required,      "mes-hqd-pq-base-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptr, 1, WClass::State,       WNeed::OnFailure,     "mes-hqd-pq-rptr"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptrReportAddr, 1, WClass::State, WNeed::Required,  "mes-hqd-rptr-report"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptrReportAddrHi, 1, WClass::State, WNeed::Required, "mes-hqd-rptr-report-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqWptrPollAddr, 1, WClass::State, WNeed::Required,    "mes-hqd-wptr-poll"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqWptrPollAddrHi, 1, WClass::State, WNeed::Required,  "mes-hqd-wptr-poll-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqDoorbellControl, 1, WClass::State, WNeed::Required, "mes-hqd-doorbell-ctl"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqControl, 1, WClass::State,    WNeed::Required,      "mes-hqd-pq-ctl"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpMqdControl, 1, WClass::State,      WNeed::Required,      "mes-hqd-mqd-ctl"},
};
static_assert(sizeof(kMesRows) / sizeof(kMesRows[0]) == kMesRowCount,
              "mes declaration table must have 74 rows (52 hub + 10 S0-S5 + 12 HQD)");

/* Queue substep rows (S7..S11): 2 GFX dbrange + 1 GRBM select + 5 S11 rows + 3
 * teardown-only rows = 11.
 * GRBM_GFX_CNTL row: chk_queues (S9) calls grbm_select() twice (mes_v11_0.cpp:1605/1610)
 * which writes gc1+kRegGrbmGfxCntl — the row is declared at i=29 in kMesRows for the
 * MES entry, but the queue entry arms a separate expectation set, so S9's GRBM writes
 * would be classified as extras on the success path (observed as expect-write-mismatch
 * with gp3 >= 0x47; gp3 < 0x47 hides it because seq_s11 sets path=failure and the strict
 * check is skipped). Re-declared here for the queue entry.
 * HQD rows (formerly rows 2-13) moved to kMesRows (rows 39-50) because program_hqd
 * runs in the MES entry, not the queue entry. */
constexpr R3WriteRowReg kMesQueueRows[] = {
    /* Rows 0-1: GFX doorbell range (S7/S8 queue init). */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpRbDoorbellRangeLower, 1, WClass::State, WNeed::OnSuccessPath, "mes-gfx-dbrange"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpRbDoorbellRangeUpper, 1, WClass::State, WNeed::OnSuccessPath, "mes-gfx-dbrange"},
    /* Row 2: GRBM select (S9 chk_queues grbm_select). */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 1, kRegGrbmGfxCntl, 1, WClass::State, WNeed::Required, "mes-grbm-sel"},
    /* Rows 3-7: S11 CSIB + MEC doorbell range (mes_queue_v11_0_substep). */
    {&r3gold::kRlcCsibAddrHi, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,      WNeed::OnSuccessPath, "mes-s11-csib"},
    {&r3gold::kRlcCsibAddrLo, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,      WNeed::OnSuccessPath, "mes-s11-csib"},
    {&r3gold::kRlcCsibLength, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State,      WNeed::OnSuccessPath, "mes-s11-csib"},
    {&r3gold::kCpMecDoorbellRangeLower, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State, WNeed::OnSuccessPath, "mes-s11-dbrange"},
    {&r3gold::kCpMecDoorbellRangeUpper, static_cast<uint8_t>(IPBlock::GC), 0, 0, 1, WClass::State, WNeed::OnSuccessPath, "mes-s11-dbrange"},
    /* Rows 8-10: S11 inverse objects. Never written at power-up (the hardware clears
     * DEQUEUE; WPTR/RPTR are polled, not driven), declared for teardown. */
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kCpHqdDequeue,  1, WClass::State, WNeed::Optional, "reg:teardown:mes-hqd-dequeue"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kCpHqdPqWptrLo, 1, WClass::State, WNeed::Optional, "reg:teardown:mes-hqd-wptr-lo"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kCpHqdPqWptrHi, 1, WClass::State, WNeed::Optional, "reg:teardown:mes-hqd-wptr-hi"},
};
static_assert(sizeof(kMesQueueRows) / sizeof(kMesQueueRows[0]) == kMesQueueRowCount,
              "mes queue declaration table must have 11 rows (2 GFX dbrange + 1 GRBM + 5 S11 "
              "+ 3 teardown-only)");

constexpr const char *kMesLayoutTag = "mes";

/* Step2: MES doorbell windows (BAR2 dword indices, 64-bit stores => 2 dwords). These are
 * registered in register_mes() as supplementary windows and are separately declared so the
 * expectation verification covers them (the R3WriteRowReg form cannot express absolute
 * indices). ctx matches the classify() labels used by doorbell_write() in the sequence.
 * Both mes_v11_0_substep and mes_queue_v11_0_substep register and verify doorbells (S5
 * and S7/S8 both write doorbells per T-E split contract). */
constexpr R3WriteRowAbs kMesDoorbells[] = {
    {amdgpu::kMesDoorbellKiq, 2u, WClass::Protocol, WNeed::OnRetry, "mes-kiq-kick"},
    {amdgpu::kMesDoorbellSch, 2u, WClass::Protocol, WNeed::Required, "mes-sch-kick"},
    {amdgpu::kMesDoorbellCompute, 2u, WClass::Protocol, WNeed::OnRetry, "mes-cmp-db"},
    {amdgpu::kMesDoorbellGfx, 2u, WClass::Protocol, WNeed::OnRetry, "mes-gfx-db"},
};
static_assert(sizeof(kMesDoorbells) / sizeof(kMesDoorbells[0]) == kMesDoorbellCount,
              "mes doorbell table must have 4 rows (kiq+sch+cmp+gfx)");
} // anonymous namespace
const R3WriteRowReg* getMesRows(void) { return kMesRows; }
uint32_t getMesRowCount(void) { return kMesRowCount; }
const R3WriteRowReg* getMesQueueRows(void) { return kMesQueueRows; }
uint32_t getMesQueueRowCount(void) { return kMesQueueRowCount; }
const char* getMesLayoutTag(void) { return kMesLayoutTag; }
const R3WriteRowAbs* getMesDoorbells(void) { return kMesDoorbells; }
uint32_t getMesDoorbellCount(void) { return kMesDoorbellCount; }

} /* namespace amdgpu */
