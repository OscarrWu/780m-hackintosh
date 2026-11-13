/*
 * rdna3_layout.h - the RDNA3 carveout layout allocator (B3/B4/A5, D3/D4/D14/D15).
 *
 * PURE FUNCTIONS ONLY. No MMIO, no IOKit, no allocation, no floating point: the inputs are
 * the read-only observations a stage-0 survey already collected, and the output is a
 * LayoutPlan. Everything here is host-testable (tech plan §10 T7/T8) and cannot itself
 * introduce a hang. The allocator NEVER reads a register; the caller fills R3LayoutInputs.
 *
 * Address model (tech plan §4.1, B4):
 *   MC address    = fb_mc_base   + off          fb_mc_base = MMMC_VM_FB_LOCATION_BASE << 24
 *   system PA     = fb_phys_base + off          fb_phys_base = MMMC_VM_FB_OFFSET << 24
 *   CPU access    = bar0_base    + off          the ONLY CPU window into the carveout
 * The system-PA form exists only for the SETUP_TMR descriptor; it is never a CPU write path
 * (B4). system_phy MUST be recomputed at runtime from this machine's FB_OFFSET (tech plan
 * §4.4 / R4 T8): the nullarc constants 0x6A0C000000 and 0x6A0000000 are not valid here and
 * must not appear in code or expectations (r3_layout_system_phy_valid rejects them).
 *
 * Allocation rules (tech plan §4.3; host test T7 covers L1-L8 one group each):
 *   L1  gop_guard_end = max(64 MiB, ALIGN_UP(gop_end, 16 MiB)); gop_end is the folded max of
 *       every active HUBP/CURSOR surface end (base + pitch*height*4). Never hard-code
 *       nullarc's 17 MiB ring or 20 MiB TMR offsets: both land inside this machine's scanout.
 *   L2  TMR size comes ONLY from the PSP LOAD_TOC response; the slot start is aligned to
 *       1 MiB (Linux PSP_TMR_ALIGNMENT). A TMR larger than the 64 MiB reserved slot fails
 *       closed (D-08: R > slot → fail, never shrink or relocate the slot).
 *   L3  Degraded placement when the preferred slot is unusable: TMR re-placed inside
 *       [gop_guard_end, 0x10000000) with SDMA/MES moved ahead of it (<= 128 MiB), or the F6
 *       fallback区 above 0x10000000 for a TMR that cannot be CPU-checked via BAR0 (> 128 MiB
 *       or no BAR0 slot). An F6 TMR is marked tmr_in_bar0=false and needs_cpu=false.
 *   L4  top_guard = real_vram_size - max(vbios_reserve, 1 MiB); [top_guard, real_vram_size)
 *       is no-write, and every grant must end at or below min(BAR0 window, top_guard).
 *   L5  Any two grants are disjoint, including each grant's zero-fill area (the ring's
 *       16 KiB clear must be inside the ring grant, not just inside an interval).
 *   L6  Every grant carries a non-empty tag, an owner substep and a B2 class.
 *   L7  Every grant the CPU zeroes or writes (needs_cpu) ends inside the BAR0 window.
 *   L8  gop_end_known == false => ok=false, fail_reason="gop-range-unconfirmed" (fail closed;
 *       the default 64 MiB guard is never a substitute for the measured range).
 *
 * Region skeleton (tech plan §4.2, R4 §4.3; offsets are VRAM/BAR0 byte offsets):
 *   C0  [0, gop_guard_end)                     GOP guard, never allocated
 *   C1  fw_buf        gop_guard_end + 16 MiB   firmware staging (A5 manifest interval)
 *   C2  psp_ring/cmd/fence  back to back after fw_buf (16 KiB/4 KiB/4 KiB, zero-fill inside)
 *   C4  tmr           preferred 0x08000000     driver-allocated TMR
 *   C5  sdma/mes      0x0C000000 / 0x0C800000  one grant each (8 MiB), separate owners so
 *                                              the §6.1 owner check accepts each IP's own
 *                                              ring/MQD/fence/copy-buffer writes
 *   C6  [0x0D000000, 0x10000000)               deliberately unallocated (BAR0 margin)
 *   top [top_guard, real_vram_size)            no-write (PSP runtime DB / VBIOS reserve)
 * The BAR0 window is the entire CPU-reachable carveout, so the layout ceiling is
 * min(0x10000000, bar0_size, top_guard): 256 MiB BAR0 and a 1 MiB top guard give exactly
 * 0x10000000 (R4 §4.5).
 *
 * Typical use:
 *   R3LayoutInputs in = {...survey readings...};
 *   R3LayoutPlan plan;
 *   if (r3_layout_compute(&in, &plan) != 0) { log(plan.fail_reason); return kStageFail; }
 *   // register each grant with the write whitelist (integration card), then use
 *   // the derived MC / BAR0 addresses.
 */
#ifndef N48_RDNA3_LAYOUT_H
#define N48_RDNA3_LAYOUT_H

#include <stdint.h>

/* Byte offset constants of the region skeleton (tech plan §4.2). */
#define R3_L3_MIB        (1024ull * 1024ull)
#define R3_GOP_GUARD_MIN (64ull * R3_L3_MIB)   /* L1 floor                            */
#define R3_GOP_GUARD_ALIGN (16ull * R3_L3_MIB) /* L1 alignment granularity            */
#define R3_FW_BUF_SIZE   (16ull * R3_L3_MIB)   /* C1 (nullarc 16 MiB headroom)        */
/* C2: PSP ring / cmd / fence, packed back to back after fw_buf (R4 §4.3). The ring grant
 * spans the full 16 KiB the ring driver clears, and cmd/fence each own their 4 KiB clear, so
 * every zero-fill area is inside its own grant and L5's disjointness covers it. The packing
 * is tighter than the nullarc +0/+0x4000/+0x8000 offsets (PhxBringup.cpp:888-895) because
 * the 16 KiB ring clear would otherwise reach into the cmd buffer. */
#define R3_PSP_RING_ZERO (16ull * 1024ull)     /* ring grant span and zero-fill       */
#define R3_PSP_CMD_ZERO  (4ull * 1024ull)      /* cmd buffer grant span and zero-fill */
#define R3_PSP_FENCE_ZERO (4ull * 1024ull)     /* fence buffer span and zero-fill     */
#define R3_TMR_PREFERRED (0x08000000ull)       /* C4 preferred slot                   */
#define R3_TMR_SLOT_SIZE (64ull * R3_L3_MIB)   /* D-08: reserved TMR slot ceiling     */
#define R3_TMR_BAR0_MAX  (0x8000000ull)        /* L3: >128 MiB TMR cannot use BAR0    */
#define R3_C5_SDMA_OFF   (0x0c000000ull)       /* C5 first half: SDMA structures      */
#define R3_C5_MES_OFF    (0x0c800000ull)       /* C5 second half: MES structures      */
#define R3_C5_SIZE       (0x00800000ull)       /* 8 MiB per half                      */
#define R3_BAR0_LAYOUT_LIMIT (0x10000000ull)   /* C6 end / 256 MiB BAR0 window        */
#define R3_TOPGUARD_MIN  R3_L3_MIB             /* L4: at least the top 1 MiB          */

/* Grants the allocator can emit: fw_buf, psp_ring, psp_cmd, psp_fence, tmr, sdma, mes.
 * The +1 tail slot lets a caller add one follow-up grant (copy test buffer) and still run
 * r3_layout_validate() without growing the plan. */
#define R3_LAYOUT_MAX_GRANTS 8u

/* Static size ceiling (T16, PM ruling): the plan holds no variable-length data, so it may live
 * in the driver instance or on the kernel stack for a snapshot. The ceiling is 768 B: 8 grants
 * x 48 B + the 16 scalars/pointers of the plan, still far below one 4 KiB kernel-stack page. */
#define R3_LAYOUT_TAG_MAX   16u               /* grant tag storage (one cache line)   */
#define R3_LAYOUT_MAX_BYTES 768u

/* B2 side-effect class of a grant's writes (tech plan §6.1). Layout grants are data
 * movement by construction; the enum mirrors WClass so the integration card can map 1:1. */
enum class R3B2Class : uint8_t {
    AccessEnable = 0, /* ① access enable                     */
    DataMove     = 1, /* ② data movement (this allocator)    */
    Protocol     = 2, /* ③ protocol interaction              */
    StateConfig  = 3  /* ④ state configuration               */
};

/* Power-up substep that owns a grant (L6). Must match WSubstep/WClass ordering. */
enum class R3Owner : uint8_t {
    Psp = 0,
    Smu = 1,
    Gfx = 2,
    Sdma = 3,
    Mes = 4
};

/* One allocated carveout interval [start, end), byte offsets (not addresses; use the
 * helpers below for MC/PA/BAR0). `size` duplicates end-start for the manifest (A5). */
struct R3Grant {
    uint64_t start;
    uint64_t end;
    uint64_t size;
    R3Owner owner;
    R3B2Class cls;
    bool needs_cpu;        /* true: the CPU zeroes/writes through BAR0 (L7)   */
    char tag[R3_LAYOUT_TAG_MAX]; /* fixed storage: plans stay copyable/freezable */
};

/* The caller's read-only observations. Every field is a stage-0 (or L1 scanout) reading;
 * nothing here is derived from the layout itself. */
struct R3LayoutInputs {
    uint64_t fb_mc_base;     /* MMMC_VM_FB_LOCATION_BASE << 24 (MMHUB copy is authoritative) */
    uint64_t fb_phys_base;   /* MMMC_VM_FB_OFFSET << 24 (SETUP_TMR system_phy basis)         */
    uint64_t bar0_base;      /* BAR0 MMIO base (CPU carveout window)                          */
    uint64_t real_vram_size; /* carveout size in bytes (FB_LOCATION_TOP - BASE + 1) << 24     */
    uint64_t vbios_reserve;  /* VBIOS firmwareinfo reserve at the top of VRAM (L4)            */
    uint64_t bar0_size;      /* BAR0 window size in bytes                                     */
    uint64_t gop_end;        /* folded max HUBP/CURSOR surface end (L1)                        */
    uint64_t tmr_size;       /* PSP LOAD_TOC resp.tmr_size (L2; never a constant)             */
    bool gop_end_known;      /* L8: must be true, else fail closed                            */
};

/* The allocator's decision. `fail_reason` is a static string, NULL on success. */
struct R3LayoutPlan {
    bool ok;
    const char *fail_reason;
    uint64_t gop_guard_end;  /* L1 */
    uint64_t top_guard;      /* L4 */
    uint64_t bar0_limit;     /* min(bar0_size, top_guard) */
    uint64_t limit;          /* min(R3_BAR0_LAYOUT_LIMIT, bar0_limit) */
    uint64_t tmr_size;       /* L2: echoes the input */
    bool tmr_reordered;      /* L3: TMR is not at R3_TMR_PREFERRED */
    bool sdma_mes_moved;     /* L3: SDMA/MES squeezed below the re-placed TMR */
    bool tmr_in_bar0;        /* false only for the F6 fallback区 */
    uint64_t tmr_mc_addr;    /* MC form of the TMR start (SETUP_TMR buf_phy_addr) */
    // MC start = MMHUB FB_LOCATION_BASE<<24 + tmr.off; system PA = MMHUB FB_OFFSET<<24
    // + tmr.off (Linux vram_mc2pa on the bare-metal APU path; SR-IOV VF uses 0).
    uint64_t tmr_system_phy; /* system PA of the TMR start (runtime-computed)     */
    uint64_t tmr_bar0_addr;  /* 0 when the TMR is not in BAR0                     */
    uint32_t grant_count;
    R3Grant grants[R3_LAYOUT_MAX_GRANTS];
};

/* Compute the plan. Returns 0 and plan->ok=true on success; otherwise non-zero,
 * plan->ok=false, plan->fail_reason set and plan->grant_count==0 (nothing partial is ever
 * published). Fail reasons: "gop-range-unconfirmed" (L8), "tmr-size-invalid" (L2),
 * "top-guard-invalid" (L4), "no-room" (L3/L4: the region ceiling cannot hold the layout),
 * "invalid-input" (null pointer). */
int r3_layout_compute(const R3LayoutInputs *in, R3LayoutPlan *out);

/* Re-check a plan (L5/L6/L7, plus the L2 alignment assertion). Returns true when valid;
 * otherwise false and *why (when non-NULL) points to a static reason string containing
 * "overlap", "owner", "class", "tag", "bar0" or "align". The allocator runs this on its own
 * output before publishing, so a successful r3_layout_compute() already satisfies it. */
bool r3_layout_validate(const R3LayoutPlan *plan, const char **why);

/* Look up one grant by tag (NUL-terminated, compared with strcmp). NULL when absent. */
const R3Grant *r3_layout_grant(const R3LayoutPlan *plan, const char *tag);

/* Address forms (tech plan §4.1). Each is base + off with no rounding: the caller passes
 * the base it read and the grant offset. See the file header for the formulas. */
uint64_t r3_layout_mc_addr(uint64_t fb_mc_base, uint64_t off);
uint64_t r3_layout_system_phy(uint64_t fb_phys_base, uint64_t off);
uint64_t r3_layout_bar0_addr(uint64_t bar0_base, uint64_t off);

/* T8 guard: true only when `system_phy` equals the value recomputed from this machine's
 * FB_OFFSET reading. The hard-coded nullarc values are rejected before any arithmetic. */
bool r3_layout_system_phy_valid(uint64_t fb_phys_base, uint64_t off, uint64_t system_phy);

/* Manifest labels (tech plan §6.2 row format). */
const char *r3_b2_class_name(R3B2Class cls);
const char *r3_owner_name(R3Owner owner);

#endif /* N48_RDNA3_LAYOUT_H */
