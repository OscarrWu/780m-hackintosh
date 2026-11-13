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
    uint32_t             osssys0;

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
    uint32_t             gp3_sched;
    uint32_t             gp3_kiq;
    uint64_t             last_fence;
    uint64_t             last_status;
    uint64_t             cmp_fence;
    uint64_t             cmp_status;
    uint64_t             cmp_wptr_mc;
    char                 gfx_result[40];
    char                 cmp_result[40];
    uint32_t             frame[kMesApiFrameDwords];
    uint32_t             qframe[kMesApiFrameDwords]; /* D1: QUERY frame, 64 dw static */
    R3FwImage            fw_img;
    /* S-1 hub pre: MMHUB snapshot (方案 v3 §6.2.2/§6.6.1). */
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
    if (!reg_write(s, s.gc0 + kRegCpHqdPqControl, pq, kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqRptrReportAddr, report_lo, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqRptrReportAddrHi, report_hi, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqWptrPollAddr, poll_lo, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqWptrPollAddrHi, poll_hi, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqDoorbellControl, db, kR3WState, "mes-hqd"))
        return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPersistentState, kCpHqdPersistentDefault,
                   kR3WState, "mes-hqd")) return false;
    if (!reg_write(s, s.gc0 + kRegCpHqdPqRptr, 0u, kR3WState, "mes-hqd")) return false;
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
        !resolve_golden(s, r3gold::kCpHqdActive, &s.hqd_active)) {
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
    }
    evf(s.env, "mes-hub-pre",
        "IP: gc[0]=0x%x gc[1]=0x%x mmhub[0]=0x%x osssys[0]=0x%x nbio[2]=0x%x "
        "mmhub fb_off=0x%x fb_base=0x%x fb_top=0x%x "
        "sysa_low=0x%x sysa_high=0x%x default_pa=0x%llx resource1_pa=0x%llx "
        "PT win: 0x%x/0x%x 0x%x/0x%x 0x%x/0x%x",
        s.gc0, ip ? ip->getBase(IPBlock::GC, 1) : 0u,
        s.mmhub0, s.osssys0, s.nbio2,
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

    /* Row 27: CP_DEBUG RMW | (1<<15) (gc_11_0_0_offset.h:4224; Linux
     * gfxhub_v3_0_set_fault_enable_default, gfxhub_v3_0.c:415-417) */
    if (!rmw(s.gc0 + 0x1e1fu, 1u << 15, 1u << 15, "mes-hub-cp-debug"))
        return r3_substep_fail(kR3SecSeq, 1, kFailSetHw);
    /* Row 28: L2_PROTECTION_FAULT_CNTL RMW: bits2-12=1, bits30-31=0
     * (gc_11_0_0_offset.h:2746; Linux set_fault_enable_default, gfxhub_v3_0.c:409-414). */
    if (!rmw(s.gc0 + 0x15c4u, 0xc0001ffcu, 0x00001ffcu, "mes-hub-fault"))
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
        {gc_fb_off_addr,     0}, /* C */
        {s.gc0 + 0x1678u,    0}, /* FB_BASE */
        {s.gc0 + 0x1679u,    0}, /* FB_TOP */
        {s.gc0 + 0x167du,    0}, /* SYS_LOW */
        {s.gc0 + 0x167eu,    0}, /* SYS_HIGH */
        {s.gc0 + 0x167fu,    0}, /* MX_L1 */
        {s.gc0 + 0x15bcu,    0}, /* L2_CNTL */
        {s.gc0 + 0x15bdu,    0}, /* L2_CNTL2 */
        {s.gc0 + 0x15c4u,    0}, /* FAULT_CNTL */
        {s.gc0 + 0x15c5u,    0}, /* FAULT_CNTL2 */
        {s.gc0 + 0x1688u,    0}, /* CONTEXT0_CNTL */
        {s.gc0 + 0x1e1fu,    0}, /* CP_DEBUG */
        {s.nbio2 + 0xC0u,    0}, /* NBIF doorbell aper */
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
        write_c ? 29u : 28u, write_c ? "written" : "skipped");
    {
        const uint32_t nbif_val =
            post_regs[sizeof(post_regs) / sizeof(post_regs[0]) - 1u][1];
        char hub[128];
        snprintf(hub, sizeof(hub),
                 "fb_off_raw=0x%x\nc=%s\nnbif=0x%x/bit%u\nrows=%u",
                 raw_off, c_state, nbif_val, nbif_val & 1u, write_c ? 29u : 28u);
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
    p.gds_size = 0u;
    p.paging_vmid = 0u;
    for (uint32_t i = 0; i < 4u; i++)
        p.compute_hqd_mask[i] = 0xcu;
    p.gfx_hqd_mask[0] = 0xfffffffeu;
    p.sdma_hqd_mask[0] = 0xfcu;
    p.g_sch_ctx_gpu_mc_ptr = s.sch_wb_mc + kMesWbSchCtxOff;
    p.query_status_fence_gpu_mc_ptr = s.sch_wb_mc + kMesWbQueryFenceOff;
    for (uint32_t i = 0; i < 5u; i++) {
        p.gc_base[i] = ip_get(s, IPBlock::GC, i);
        p.mmhub_base[i] = ip_get(s, IPBlock::MMHUB, i);
        p.osssys_base[i] = ip_get(s, IPBlock::OSSSYS, i);
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
        if (sr != SubmitResult::IoFail) {
            s.cmp_fence = s.last_fence;
            s.cmp_status = s.last_status;
        }
    }
    s.cmp_wptr_mc = s.cmp_wb_mc + kMesWbWptrOff;
    copy_result(s.cmp_result, sizeof(s.cmp_result), result);
    evf(s.env, "mes-addq-compute",
        "S7 result=%s mqd_mc=0x%llx wptr_mc=0x%llx db=%u fence=0x%llx status=0x%llx",
        s.cmp_result, static_cast<unsigned long long>(s.cmp_mqd_mc),
        static_cast<unsigned long long>(s.cmp_wb_mc + kMesWbWptrOff),
        kMesDoorbellCompute, static_cast<unsigned long long>(s.last_fence),
        static_cast<unsigned long long>(s.last_status));
    /* C5.2 v1.5 (F7=c'): ADD_QUEUE is an attempt. Failure is recorded with its
     * response/address form/return code and is never fatal or latched. */
    return r3_substep_pass();
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
    /* C5.2 v1.5 (F7=c'): ADD_QUEUE is an attempt. Its failure is recorded, never masked,
     * never fatal and never latched. S9 still exports what the MES programmed. */
    return r3_substep_pass();
}

bool mc_prefix_ok(uint64_t addr)
{
    return (addr >> 32) == kMesMcPrefix;
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
    bool all_prefix = true;
    for (uint32_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        if (!mc_prefix_ok(addrs[i])) {
            all_prefix = false;
            evf(s.env, "mes-addr", "S9 address form bad @%u: 0x%llx", i,
                static_cast<unsigned long long>(addrs[i]));
        }
    }
    evf(s.env, "mes-queues",
        "S9 compute active=%u buf=%u rptr=%u gfx active=%u buf=%u rptr=%u",
        s.cmp_active, ca ? 1u : 0u, cr ? s.cmp_rptr : 0u,
        s.gfx_active, ga ? 1u : 0u, gr ? s.gfx_rptr : 0u);
    evf(s.env, "mes-addr",
        "S9 kiq_ring=0x%llx sch_ring=0x%llx gfx_ring=0x%llx cmp_ring=0x%llx prefix=%s",
        static_cast<unsigned long long>(s.kiq_ring_mc),
        static_cast<unsigned long long>(s.sch_ring_mc),
        static_cast<unsigned long long>(s.gfx_ring_mc),
        static_cast<unsigned long long>(s.cmp_ring_mc),
        all_prefix ? "0x80" : "bad");
    if (!all_prefix) {
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

} /* namespace */

R3SubstepResult mes_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    static Mes s;
    memset(&s, 0, sizeof(s));
    s.io = io;
    s.env = env;

    r3_wg_arm_substep(WSubstep::Mes);
    r3_wg_expect_writes(getMesRows(), getMesRowCount(), nullptr, 0, WSubstep::Mes);
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env ? env->ip : nullptr, getMesRows(), getMesRowCount(),
                            declared);
    r3_wg_expect_declared_addrs(declared, getMesRowCount());
    r3_wg_expect_abs_writes(getMesDoorbells(), getMesDoorbellCount(), WSubstep::Mes);

    R3SubstepResult r = pre(s);
    /* S-1: hub pre-initialisation runs BEFORE S0 (方案 v3 §6.1/§6.2). */
    if (r.status == kR3SsecOk)
        r = seq_hub_pre(s);
    if (r.status == kR3SsecOk)
        r = seq_cp_unhalt(s);
    if (r.status == kR3SsecOk)
        r = seq_start(s);
    if (r.status == kR3SsecOk)
        r = seq_version(s);
    if (r.status == kR3SsecOk)
        r = seq_kiq(s);
    if (r.status == kR3SsecOk)
        r = seq_sch(s);
    if (r.status == kR3SsecOk)
        r = seq_set_hw(s);
    /* S5.1: SET_HW_RESOURCES_1 after S5 (方案 v3 §6.5). */
    if (r.status == kR3SsecOk)
        r = seq_set_hw_1(s);
    if (r.status == kR3SsecOk)
        r = seq_query(s);
    if (r.status == kR3SsecOk)
        r = seq_addq_compute(s);
    if (r.status == kR3SsecOk)
        r = seq_addq_gfx(s);
    if (r.status == kR3SsecOk)
        r = chk_queues(s);
    if (r.status == kR3SsecOk) {
        r3_substep_note(&r, s.mes_cntl, s.cntl_readback, "CP_MES_CNTL");
        r3_substep_note(&r, s.mes_gp3, s.gp3_sched, "GP3 sched");
        r3_substep_note(&r, s.mes_gp3, s.gp3_kiq, "GP3 kiq");
        r3_substep_note(&r, s.hqd_active, s.cmp_active, "COMPUTE HQD");
        if (env && env->rt)
            env->rt->ready_bits |= R3_IP_BIT(R3Ip::Mes);
        set_prop(env, "N48R3EVID-mes-done", "Yes");
        set_prop(env, "N48R3EVID-mes-addq-compute", s.cmp_result);
        set_prop(env, "N48R3EVID-mes-addq-gfx", s.gfx_result);
        evf(env, "mes", "done: compute=%s, gfx=%s, sch_wptr=%u", s.cmp_result,
            s.gfx_result, s.sch_wptr);
    } else {
        set_prop(env, "N48R3EVID-mes-done", "No");
        set_prop(env, "N48R3EVID-mes-addq-compute",
                 s.cmp_result[0] ? s.cmp_result : "n/a");
        set_prop(env, "N48R3EVID-mes-addq-gfx",
                 s.gfx_result[0] ? s.gfx_result : "n/a");
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

/* Step2: declaration table (rdna3_writeguard.h R3WriteRowReg), the single source of truth
 * for the MES register write set. 53 rows: 29 hub-pre (S-1) + 24 existing (S0-S9).
 * Row order follows the write sequence: hub rows first, then the main sequence rows
 * (方案 v3 §6.2.3 table + §6.7). */
namespace {
constexpr R3WriteRowReg kMesRows[] = {
    /* S-1 rows 1-29: hub pre-initialisation (GCVM aperture/L1-L2/NBIF).
     * Row 1 (C) is WNeed::Optional; rows 2-29 are WNeed::Required. */
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
    /* Existing rows 30-53 (S0-S9): CP unhalt, MES start, KIQ/SCH/GFX queue init. */
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
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpRbDoorbellRangeLower, 1, WClass::State, WNeed::OnSuccessPath, "mes-gfx-dbrange"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpRbDoorbellRangeUpper, 1, WClass::State, WNeed::OnSuccessPath, "mes-gfx-dbrange"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdVmid, 1, WClass::State,         WNeed::Required,      "mes-hqd-vmid"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPersistentState, 1, WClass::State, WNeed::Required,   "mes-hqd-persistent"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqBase, 1, WClass::State,       WNeed::Required,      "mes-hqd-pq-base"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqBaseHi, 1, WClass::State,     WNeed::Required,      "mes-hqd-pq-base-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptr, 1, WClass::State,       WNeed::Required,      "mes-hqd-pq-rptr"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptrReportAddr, 1, WClass::State, WNeed::Required,  "mes-hqd-rptr-report"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqRptrReportAddrHi, 1, WClass::State, WNeed::Required, "mes-hqd-rptr-report-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqWptrPollAddr, 1, WClass::State, WNeed::Required,    "mes-hqd-wptr-poll"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqWptrPollAddrHi, 1, WClass::State, WNeed::Required,  "mes-hqd-wptr-poll-hi"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqDoorbellControl, 1, WClass::State, WNeed::Required, "mes-hqd-doorbell-ctl"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpHqdPqControl, 1, WClass::State,    WNeed::Required,      "mes-hqd-pq-ctl"},
    {nullptr, static_cast<uint8_t>(IPBlock::GC), 0, kRegCpMqdControl, 1, WClass::State,      WNeed::Required,      "mes-hqd-mqd-ctl"},
};
constexpr uint32_t kMesRowCount = sizeof(kMesRows) / sizeof(kMesRows[0]);
static_assert(kMesRowCount == 53, "mes declaration table must have 53 rows (29 hub + 24 seq)");
constexpr const char *kMesLayoutTag = "mes";

/* Step2: MES doorbell windows (BAR2 dword indices, 64-bit stores => 2 dwords). These are
 * registered in register_mes() as supplementary windows and are separately declared so the
 * expectation verification covers them (the R3WriteRowReg form cannot express absolute
 * indices). ctx matches the classify() labels used by doorbell_write() in the sequence. */
constexpr R3WriteRowAbs kMesDoorbells[] = {
    {amdgpu::kMesDoorbellKiq, 2u, WClass::Protocol, WNeed::Required, "mes-kiq-kick"},
    {amdgpu::kMesDoorbellSch, 2u, WClass::Protocol, WNeed::Required, "mes-sch-kick"},
};
constexpr uint32_t kMesDoorbellCount = sizeof(kMesDoorbells) / sizeof(kMesDoorbells[0]);
static_assert(kMesDoorbellCount == 2, "mes doorbell table must have 2 rows");
} // anonymous namespace

const R3WriteRowReg* getMesRows(void) { return kMesRows; }
uint32_t getMesRowCount(void) { return kMesRowCount; }
const char* getMesLayoutTag(void) { return kMesLayoutTag; }
const R3WriteRowAbs* getMesDoorbells(void) { return kMesDoorbells; }
uint32_t getMesDoorbellCount(void) { return kMesDoorbellCount; }

} /* namespace amdgpu */
