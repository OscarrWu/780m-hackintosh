/*
 * gmc_v11_0.h - GMC (Gfx/MM Hub) substeps for RDNA3 bring-up, gfx1103 (780M).
 *
 * Stage-3 batch T-B, four substeps (实施文档 §6):
 *   S6 uma     - MC aperture read-only recheck + layout D4-D8 registration +
 *                sysmem_grants backfill (three system-memory pools).
 *   S7 gart    - flat GART table in system memory (512K x 8B), VMID0 flat
 *                configuration on both hubs, TLB invalidate (engine 17,
 *                degraded Linux shape), SDMA WRITE_LINEAR through GART VA.
 *   S8 gpuvm0  - four-level page table for VMID0 (root 4KiB + 4MiB pool of
 *                4KiB pages), nullarc vmBind register sequence with absolute
 *                dword cross-check against R10, VRAM test page mapped, SDMA
 *                verification through GPUVM.
 *   S9 vmid8   - same shape for VMID 8 (D3 pool), PER_VMID invalidate,
 *                FAULT_DEFAULT=1 for the test window; register-level gate
 *                only (IB validation is an S11 add-on).
 *
 * Design intent (项目最高原则): the GPUVM/GART contract follows the Linux
 * amdgpu design (amdgpu_vm.h bit layout, gmc_v11_0.c flush shape) with the
 * nullarc on-silicon measurements (R10) as the concrete register sequence.
 * Every step fails closed (kR3SecChk + r3_substep_fail), never retries,
 * never widens.
 */
#ifndef NAVI48_GMC_V11_0_H
#define NAVI48_GMC_V11_0_H

#include <stddef.h>
#include <stdint.h>

#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"

namespace amdgpu {

/* ---- PTE/PDE bit definitions (Linux amdgpu_vm.h:57-136) ---- */
constexpr uint64_t kGmcPtValid       = 1ull << 0;
constexpr uint64_t kGmcPtSystem      = 1ull << 1;
constexpr uint64_t kGmcPtSnooped     = 1ull << 2;
constexpr uint64_t kGmcPtExecutable  = 1ull << 4;
constexpr uint64_t kGmcPtReadable    = 1ull << 5;
constexpr uint64_t kGmcPtWriteable   = 1ull << 6;
constexpr uint64_t kGmcPtMtypeShift  = 48;
constexpr uint64_t kGmcPtAddrMask    = 0x0000FFFFFFFFF000ull;

/* Leaf PTE flag sets (nullarc E6/E7 measured on this silicon). */
constexpr uint64_t kGmcPteVram   = kGmcPtValid | kGmcPtExecutable |
                                   kGmcPtReadable | kGmcPtWriteable; /* 0x71 */
constexpr uint64_t kGmcPteSystem = kGmcPtValid | kGmcPtSystem | kGmcPtSnooped |
                                   kGmcPtExecutable | kGmcPtReadable |
                                   kGmcPtWriteable | (4ull << kGmcPtMtypeShift);
/* PDE: next-level table pointer (nullarc: child | VALID|SYSTEM|SNOOPED; no
 * BFS/TF because gmc_v11 translate_further is off). */
constexpr uint64_t kGmcPdeFlags = kGmcPtValid | kGmcPtSystem | kGmcPtSnooped; /* 7 */
/* Root/table pointer flags for tables living in system memory (nullarc E6). */
constexpr uint64_t kGmcRootFlags = kGmcPtValid | kGmcPtSystem; /* 3 */

/* ---- GART (S7): flat first-level table, 512K entries x 8B = 4MiB ---- */
constexpr uint32_t kGmcGartEntries   = 512u * 1024u;
constexpr uint32_t kGmcGartBytes     = kGmcGartEntries * 8u;
constexpr uint32_t kGmcGartIndexMask = kGmcGartEntries - 1u;

/* ---- Page-table pool (S8/S9, nullarc PhxBringup.cpp:154-160 shape) ----
 * One contiguous grant: root 4KiB first, then a 4MiB pool of 4KiB pages the
 * walk hands out on demand (PDB2/PDB1/PDB0/leaf tables). */
constexpr uint32_t kGmcPtRootBytes  = 4096u;
constexpr uint32_t kGmcPtPoolPages  = 1024u; /* 4MiB / 4KiB */
constexpr uint32_t kGmcPtPoolBytes  = kGmcPtPoolPages * 4096u;
constexpr uint32_t kGmcPtGrantBytes = kGmcPtRootBytes + kGmcPtPoolBytes;

/* ---- vmBind register values (S8/S9 full-VA window, nullarc R10 §2.3) ----
 * START/END are page numbers (Linux gfxhub_v3_0.c:133-148).  This window is
 * the 16 TiB user-VM window used by the four-level paths (S8/S9); the flat
 * GART has its own window right below. */
constexpr uint32_t kGmcVbindCntl    = 0x01fffe07u;
constexpr uint32_t kGmcVbindStartLo = 0u;
constexpr uint32_t kGmcVbindStartHi = 0u;
constexpr uint32_t kGmcVbindEndLo   = 0xffffffffu;
constexpr uint32_t kGmcVbindEndHi   = 0x0fu;

/* ---- S7 flat-GART window (Linux gfxhub_v3_0.c:133-148) ----
 * The window must cover exactly the first-level table: one entry per 4 KiB
 * page, so END = (kGmcGartEntries * 4 KiB - 1) >> 12 = 0x0007ffff (2 GiB).
 * A wider window lets an address beyond the table walk past its end. */
constexpr uint32_t kGmcGartStartLo = 0u;
constexpr uint32_t kGmcGartStartHi = 0u;
constexpr uint32_t kGmcGartEndLo =
    static_cast<uint32_t>(((static_cast<uint64_t>(kGmcGartEntries) * 4096ull) - 1ull) >> 12);
constexpr uint32_t kGmcGartEndHi =
    static_cast<uint32_t>(((static_cast<uint64_t>(kGmcGartEntries) * 4096ull) - 1ull) >> 44);
static_assert(kGmcGartEndLo == 0x0007ffffu, "flat-GART window covers the 512K-entry table");
static_assert(kGmcGartEndHi == 0u, "flat-GART window page number fits in 44 bits");

/* ---- Invalidate (Linux gmc_v11_0.c:220-312 degraded, nullarc R10 §2.3) ----
 * req = base | (1 << vmid) | 1; ack poll = 1 << vmid. Engine 17 fixed. */
constexpr uint32_t kGmcInvBase   = 0x00F80000u; /* L2_PTES|PDE0|PDE1|PDE2|L1_PTES */
constexpr uint32_t kGmcInvEng17  = 17u;
constexpr uint32_t kGmcInvPolls  = 1000u;
constexpr uint32_t kGmcInvPollUs = 1u;

/* ---- fault control: mesboot env_on("FAULT_DEFAULT") measured value ---- */
constexpr uint32_t kGmcFaultDefault = 0x3ffffffcu;
/* mesboot gart() record mode: allow status updates, no default-page redirect,
 * no crash (tools/phxctl/mesboot.c:113). */
constexpr uint32_t kGmcFaultRecordMode = 0x00000002u;

/* ---- Test VAs / pages ---- */
constexpr uint64_t kGmcTestVaGart   = 0ull;            /* S7 flat GART */
constexpr uint64_t kGmcTestVaGpuvm  = 0x100000000ull;  /* S8 four-level */
constexpr uint64_t kGmcTestVaVmid8  = 0x200000000ull;  /* S9 four-level (S11 prep) */
constexpr uint32_t kGmcTestPageIntra = 0x100u;         /* intra-page offset */
constexpr uint32_t kGmcTestMagic     = 0xDEADBEEFu;

/* ---- R10 absolute dword cross-check (GC base 0x1260) ----
 * nullarc measured absolute dword indices for the VMID0 context registers;
 * the golden table must resolve to the same values or the substep stops and
 * records both sides. */
constexpr uint32_t kGmcR10Ctx0CntlAbs     = 0x28E8u;
constexpr uint32_t kGmcR10Ctx0BaseLoAbs   = 0x2953u;
constexpr uint32_t kGmcR10Ctx0StartLoAbs  = 0x2973u;
constexpr uint32_t kGmcR10Ctx0EndLoAbs    = 0x2993u;
constexpr uint32_t kGmcR10CtxDistance     = 1u; /* CNTL stride per vmid */
constexpr uint32_t kGmcR10CtxAddrDistance = 2u; /* address-pair stride per vmid */

/* ---- SDMA submission (nullarc mesboot sdma_write, E4a) ----
 * WRITE_LINEAR is a 5-dword packet: header, dst lo/hi, count/control, value.
 * count=0 means one dword. */
constexpr uint32_t kGmcSdmaWriteLinearHdr = 0x00000002u;
constexpr uint32_t kGmcSdmaRingDwords     = 1024u;
constexpr uint32_t kGmcSdmaPolls          = 1000u;
constexpr uint32_t kGmcSdmaPollUs         = 10u;
/* The queue's RB_RPTR/RB_WPTR are BYTE offsets: Linux sdma_v6_0.c shifts the
 * dword read right by 2 (ring_get_rptr) and the byte index left by 2
 * (ring_set_wptr), and the S5 path (sdma_v6_0.cpp:410-458) programs the queue
 * in bytes. Only the ring buffer itself is indexed in dwords. */
constexpr uint32_t kGmcSdmaPacketDwords = 5u;
constexpr uint32_t kGmcSdmaPacketBytes  = kGmcSdmaPacketDwords * 4u;
constexpr uint32_t kGmcSdmaRingBytes    = kGmcSdmaRingDwords * 4u;

/* ---- VMID 8..15 lending (nullarc R10 §2.3) ---- */
constexpr uint8_t kGmcFirstUserVmid        = 8u;
constexpr uint8_t kGmcNumUserVmids         = 8u;
constexpr uint8_t kGmcVmidNone             = 0xffu;
constexpr uint32_t kGmcVmidRetireTimeoutUs = 2000000u; /* 2s, caller-side */

/* ---- sysmem allocation contract ----
 * rdna3_substep.h has no allocation hook and is not writable in this batch,
 * so the integration layer (T-E) binds one allocator per driver instance
 * before the stage ladder runs; host tests bind a fake one. */
struct GmcSysmemAlloc {
    void *ctx;
    /* Physically contiguous, 4KiB-aligned system memory; returns the physical
     * address (0 on failure) and the CPU pointer via *cpu. */
    uint64_t (*alloc)(void *ctx, uint64_t bytes, void **cpu);
};
int gmc_v11_0_bind_sysmem(const GmcSysmemAlloc *alloc);

/* ---- cache-line flush (nullarc PhxBringup.cpp:466-468) ----
 * The GPU walker may not snoop CPU caches; every page-table/GART line written
 * by the CPU is flushed. No-op off x86. */
inline void gmc_pt_flush_line(const volatile void *p)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
#else
    (void)p;
#endif
}

/* ---- page-table pool (pure logic, host-testable) ---- */
struct GmcPtPool {
    uint64_t base_phys; /* grant start: root 4KiB + pool 4MiB contiguous */
    void *base_cpu;
    uint32_t used;      /* pool pages handed out (0..kGmcPtPoolPages) */
};

inline void gmc_pt_pool_init(GmcPtPool *pool, uint64_t base_phys, void *base_cpu)
{
    pool->base_phys = base_phys;
    pool->base_cpu = base_cpu;
    pool->used = 0;
}

/* Walks (and with alloc, constructs) the four-level path for va: root at
 * base+0, intermediate/leaf tables handed out from the pool. Returns the
 * leaf-table entry pointer, or null when the path is missing and alloc is
 * off / capacity is exhausted. */
inline uint64_t *gmc_pt_walk(GmcPtPool *pool, uint64_t va, int alloc)
{
    uint64_t tbl_phys = pool->base_phys;
    uint64_t *tbl = static_cast<uint64_t *>(pool->base_cpu);
    for (uint32_t shift = 39; shift >= 21; shift -= 9) {
        uint64_t *e = tbl + ((va >> shift) & 511u);
        if (!(*e & kGmcPtValid)) {
            if (!alloc || pool->used >= kGmcPtPoolPages)
                return nullptr;
            const uint64_t child = pool->base_phys + kGmcPtRootBytes +
                static_cast<uint64_t>(pool->used) * 4096ull;
            pool->used++;
            *e = child | kGmcPdeFlags;
            gmc_pt_flush_line(e);
        }
        tbl_phys = *e & kGmcPtAddrMask;
        tbl = reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(pool->base_cpu) +
            (tbl_phys - pool->base_phys));
    }
    return tbl + ((va >> 12) & 511u);
}

/* Writes the leaf PTE (page_phys | flags) and flushes the line. */
inline int gmc_pt_map(GmcPtPool *pool, uint64_t va, uint64_t page_phys,
                      uint64_t pte_flags)
{
    uint64_t *e = gmc_pt_walk(pool, va, 1);
    if (!e)
        return -1;
    *e = (page_phys & kGmcPtAddrMask) | pte_flags;
    gmc_pt_flush_line(e);
    return 0;
}

/* ---- GART entry builders (pure) ---- */
inline uint64_t gmc_gart_entry_vram(uint64_t page_phys)
{
    return (page_phys & kGmcPtAddrMask) | kGmcPteVram;
}
inline uint64_t gmc_gart_entry_system(uint64_t page_phys)
{
    return (page_phys & kGmcPtAddrMask) | kGmcPteSystem;
}
inline uint32_t gmc_gart_index(uint64_t va)
{
    return static_cast<uint32_t>((va >> 12) & kGmcGartIndexMask);
}

/* ---- VMID 8..15 LRU lending (pure; nullarc R10 §2.3) ----
 * Client contexts borrow VMIDs 8..15. A free slot is taken first; otherwise
 * the least-recently-used borrower is displaced and its owner tag returned so
 * the caller can wait (bounded, 2s) for that owner's GPU work to retire. */
struct GmcVmidLruSlot {
    uint8_t owner;   /* kGmcVmidNone = free */
    uint64_t stamp;
};
struct GmcVmidLru {
    GmcVmidLruSlot slot[kGmcNumUserVmids];
    uint64_t clock;
};

inline void gmc_vmid_lru_init(GmcVmidLru *lru)
{
    for (uint32_t i = 0; i < kGmcNumUserVmids; i++) {
        lru->slot[i].owner = kGmcVmidNone;
        lru->slot[i].stamp = 0;
    }
    lru->clock = 0;
}

inline uint8_t gmc_vmid_lru_acquire(GmcVmidLru *lru, uint8_t owner,
                                    uint8_t *evicted_owner)
{
    if (evicted_owner)
        *evicted_owner = kGmcVmidNone;
    for (uint32_t i = 0; i < kGmcNumUserVmids; i++) {
        if (lru->slot[i].owner == kGmcVmidNone) {
            lru->slot[i].owner = owner;
            lru->slot[i].stamp = ++lru->clock;
            return static_cast<uint8_t>(kGmcFirstUserVmid + i);
        }
    }
    uint32_t best = 0;
    for (uint32_t i = 1; i < kGmcNumUserVmids; i++)
        if (lru->slot[i].stamp < lru->slot[best].stamp)
            best = i;
    if (evicted_owner)
        *evicted_owner = lru->slot[best].owner;
    lru->slot[best].owner = owner;
    lru->slot[best].stamp = ++lru->clock;
    return static_cast<uint8_t>(kGmcFirstUserVmid + best);
}

inline void gmc_vmid_lru_release(GmcVmidLru *lru, uint8_t vmid)
{
    if (vmid < kGmcFirstUserVmid ||
        vmid >= static_cast<uint8_t>(kGmcFirstUserVmid + kGmcNumUserVmids))
        return;
    lru->slot[vmid - kGmcFirstUserVmid].owner = kGmcVmidNone;
}

inline void gmc_vmid_lru_touch(GmcVmidLru *lru, uint8_t vmid)
{
    if (vmid < kGmcFirstUserVmid ||
        vmid >= static_cast<uint8_t>(kGmcFirstUserVmid + kGmcNumUserVmids))
        return;
    lru->slot[vmid - kGmcFirstUserVmid].stamp = ++lru->clock;
}

/* ---- stage-3 GTT BO helpers (GART PTE-backed sysmem pages) ----------------
 * The stage ladder owns the GART table (S6/S7); these helpers let the ABI BO
 * allocator reuse the same table and the same sysmem allocator instead of
 * introducing a second address authority. CP consumption of GTT BOs is a
 * stage 4/5 item (E6/F5). */
uint64_t gmc_v11_0_sysmem_alloc_pages(uint32_t pages, void **cpu);
void     gmc_v11_0_sysmem_free_pages(uint64_t phys);
int      gmc_v11_0_gart_map_va(uint64_t gart_va, uint64_t page_phys, uint32_t pages);
void     gmc_v11_0_gart_unmap_va(uint64_t gart_va, uint32_t pages);

/* ---- substep entries (stage ladder, one per S6..S9) ---- */
R3SubstepResult uma_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult gart_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult gpuvm0_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
R3SubstepResult vmid8_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);

} /* namespace amdgpu */

#endif /* NAVI48_GMC_V11_0_H */
