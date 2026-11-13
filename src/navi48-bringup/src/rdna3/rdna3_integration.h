/*
 * rdna3_integration.h - the stage-2 integration layer (K10): production writeguard
 * registration, the per-IP enable predicate fed by K0's bit-map gate, and the evidence
 * summary assembly. Pure functions only: this file never performs I/O and never includes
 * IOKit, so the host test can drive the exact same registration/enable logic the kext uses
 * (the K7 R1 lesson: a green host test with an unregistered production path is worthless).
 *
 * Layer split:
 *   - THIS header/translation unit: registration rows, enable predicate, summary text.
 *   - NaviR3Bringup.cpp: the R3Io adapter over amdgpu::DeviceContext (BAR5/BAR0/BAR2/SMN/
 *     delay/lock) and the IOKit property shell. Nothing here knows about IOKit.
 *
 * Registration is fail closed: every r3_wg_register_* return code is checked. A rejected
 * row means the whitelist table is full or malformed; the ladder must stop before any
 * power-up write (otherwise the first write is a DENY on real hardware).
 */
#ifndef N48_RDNA3_INTEGRATION_H
#define N48_RDNA3_INTEGRATION_H

#include <stddef.h>
#include <stdint.h>

#include "rdna3_layout.h"
#include "rdna3_runtime.h"
#include "rdna3_substep.h"

#include "golden_values.h"

/* --- gate inputs (F2/F3, §5.1-5.4) ------------------------------------------------------
 * The five leaves are resolved by BootArgFamily::r3_kill_state(); `absent` means "follow the
 * ladder" (the value is then ignored). `latch` is the R3Rt bit map.
 */
struct R3GateInputs {
    uint32_t stage;          /* n48r3-stage (clamped by the runner)                        */
    uint32_t kill_mask;      /* R3_IP_BIT(x) for every leaf explicitly = 0                 */
    uint32_t latch_mask;     /* rt->latch_bits (R3_IP_BIT(x))                              */
};

/* enabled(ip) = stage reached && !killed && !latched && dependency closure.
 * Returns the enabled bitmap in the R3Ip bit space (R3_IP_COUNT bits). This is a thin
 * adapter over common/StageRunner's r3_gate_enabled(): the K0 function owns the policy,
 * this one owns the R3Ip<->R3GateIp mapping (they share the same bit order by design). */
uint32_t r3_integration_enabled(const R3GateInputs *in);

/* True when the substep at `ip` may run. Stage 0 (survey) is never gated (R5). */
bool r3_integration_ip_enabled(const R3GateInputs *in, R3Ip ip);

/* --- D-01/F12 trigger admission (pure) -------------------------------------------------
 * The boot arms the read-only ladder (survey + preflight + preparation); the trigger only
 * fires the armed substeps. The decision is data-driven so the refusal reasons are host
 * tested, and it is the only place that maps "n48r3-stage N" to ladder indices:
 *   ok              -> first_stage = substep_stage, target_stage = min(N+1, table_count-1)
 *   already booted  -> D-04 refusal (one initialization per boot)
 *   not armed       -> the boot gate/preflight, the preparation, or the R3Io adapter set is
 *                      incomplete
 *   nothing to fire -> stage 0 (read-only) or a table without a substep row
 * `substep_stage` is the ladder index of the first power-up substep (the psp row). */
enum class R3TriggerVerdict : uint8_t {
    Ok = 0,
    AlreadyProvisioned,
    NotArmed,
    NoSubsteps,
};

struct R3TriggerDecision {
    R3TriggerVerdict verdict;
    const char      *reason;       /* static, always set (evidence/log friendly) */
    uint32_t         first_stage;  /* valid when verdict == Ok                   */
    uint32_t         target_stage; /* valid when verdict == Ok                   */
};

R3TriggerDecision r3_trigger_admission(bool provisioned, bool gate_ok, bool prep_ok,
                                       bool io_ok, uint32_t stage, uint32_t substep_stage,
                                       uint32_t table_count);

/* --- production writeguard registration (§6.1, K4-K8 transfer notes) -------------------
 * Registers every row the five substeps write, plus one layout grant per plan grant:
 *   psp  : all PSP-owned layout grants + MP0 C2PMSG 64/69/70/71 (State) + 67 (Protocol)
 *   smu  : MP1 C2PMSG 66/82/90 (Protocol)
 *   gfx  : none (read-only substep, write count 0)
 *   sdma : layout:sdma + the 14 SDMA0 queue registers (K7 R1: without these the real
 *          hardware DENYs every write)
 *   mes  : layout:mes + 25 CP/MES registers + doorbells 0x16/0x18 as 2 dwords (K8 note)
 * Also registers the PSP fw staging, TMR, ring/cmd/fence grants (layout:psp).
 *
 * Returns 0 on success; -1 on the first rejected row (the caller stops the ladder; no
 * power-up write may follow). `ip` is the discovery base table (MP0/MP1/GC/SDMA0 bases).
 */
int r3_integration_register_production(const R3LayoutPlan *plan,
                                       const void *ip_base_table);

/* Number of rows this layer attempts (test pin: the table must not silently shrink). */
uint32_t r3_integration_register_row_count(void);

/* --- scanout read-only collection (tech plan §4.2 L1 inputs) --------------------------
 * The layout allocator consumes the active scanout end so its guard never covers a live
 * GOP surface. Stage 1's survey does not retain it, so the integration layer performs the
 * small read-only collection here (DCN BASE_IDX 2; offsets from dcn_3_1_4_offset.h /
 * dcn_3_1_4_sh_mask.h):
 *   HUBP0..3 DCSURF_PRI_VIEWPORT_DIMENSION (height), HUBPREQ0..3 SURFACE_PITCH,
 *   HUBPREQ0..3 PRIMARY_SURFACE_ADDRESS(_HIGH), HUBPx_DCHUBP_CNTL (surface enable),
 *   CURSOR0..3 CURSOR_CONTROL (enable), CURSOR_SURFACE_ADDRESS(_HIGH), CURSOR_SIZE.
 * Surface/cursor addresses are read in the MC form and NORMALIZED to carveout byte offsets
 * (mc - fb_mc_base) before they feed L1; a surface below the carveout base or ending past
 * real_vram_size fails the collection (fail closed, "scanout:outside-carveout" in the prep
 * log). `read` returns false on an inaccessible register; the collector then returns false
 * and the caller must treat the guard as unmeasured (L8), never substitute a default.
 * The DCN segment base is IPBlock-agnostic here: the caller passes the DCN BASE_IDX 2 base.
 */
struct R3ScanoutReading {
    uint64_t gop_end;     /* max surface end (bytes into the carveout) */
    uint32_t surfaces;    /* number of enabled surfaces contributing   */
    uint32_t cursors;     /* number of enabled cursors contributing   */
};
typedef bool (*R3Read32Fn)(void *user, uint32_t dword, uint32_t *value);

bool r3_integration_collect_scanout(uint32_t dcn_seg2_base, R3Read32Fn read, void *user,
                                    uint64_t fb_mc_base, uint64_t real_vram_size,
                                    R3ScanoutReading *out);

/* --- enabled map evidence (E2) --------------------------------------------------------- */
/* "psp=1 smu=1 gfx=1 sdma=0 mes=0 kill=0x0 latch=0x0 stage=5\n" (snprintf-style). */
size_t r3_integration_format_enabled(const R3GateInputs *in, char *out, size_t cap);

/* --- R3Io addressing (pure, host-testable) ---------------------------------------------
 * The kext adapter in NaviR3Bringup.cpp maps BAR5/BAR0/BAR2 and delegates the address
 * arithmetic here so the host test can verify the exact dword/byte translation the
 * hardware sees (the K4/K7 "two-space trap"):
 *   mmio   : `dword` is an absolute BAR5 dword index (bounds = rmmioSize/4).
 *   bus    : `bar` is R3_IO_BAR_VRAM (0) or the doorbell BAR (2); `byte_off` is a BYTE
 *            offset inside that BAR; `dwords` is 1 or 2 (2 requires 8-byte alignment).
 *   smuio  : `index` is the SMN BYTE address (Linux RREG32_PCIE argument); the adapter
 *            writes index>>2 into PCIE_INDEX2.
 * Fails closed on a bad width, misalignment, or a range past the mapped window. */
struct R3IoWindow {
    uint64_t bar5_bytes;   /* BAR5 map length (0 = unmapped)   */
    uint64_t bar0_bytes;   /* BAR0 carveout map length         */
    uint64_t bar2_bytes;   /* BAR2 doorbell map length         */
};

bool r3_io_mmio_in_range(const R3IoWindow *w, uint32_t dword);
bool r3_io_bus_in_range(const R3IoWindow *w, uint32_t bar, uint64_t byte_off,
                        uint32_t dwords);

/* HDP flush window discovery (v1.7/R1). The NBIO remap register holds a BAR5 byte offset;
 * writing 0 to that dword flushes the host data path (Linux amdgpu_device_flush_hdp,
 * nullarc hdpFlush). Returns true and the BAR5 dword only for a plausible, in-range value.
 * Pure: the prepare path and the host tests share this exact rule. */
static inline bool r3_hdp_flush_window(uint32_t remap, uint64_t bar5_bytes, uint32_t *dword)
{
    if (!dword || remap == 0u || remap == 0xFFFFFFFFu || (remap & 3u) != 0u)
        return false;
    const uint32_t d = remap >> 2;
    if (bar5_bytes < 4u || (uint64_t)d * 4u + 4u > bar5_bytes)
        return false;
    *dword = d;
    return true;
}

/* --- evidence summary (E2/E3) ---------------------------------------------------------- */
/* One "N48R3EVID-stage2-summary" payload:
 *   "stage=%u effective=%u ready=0x%x latch=0x%x deny=%llu\n"
 * plus one "ip=<name> done=<yes|no> name=<substep-name> section=<s> step=<n>\n" line per
 * substep result the caller supplies (may be null). Pure formatting; the caller owns the
 * buffer. Returns the snprintf-style would-be length. */
struct R3IpOutcome {
    R3Ip ip;
    bool  ran;                    /* false => label "not-run"                          */
    R3SubstepResult result;       /* valid when ran                                   */
    uint32_t ready;               /* R3_IP_BIT(ip) present in rt->ready_bits           */
};

size_t r3_integration_format_summary(uint32_t stage, uint32_t effective_stage,
                                     uint32_t ready_bits, uint32_t latch_bits,
                                     uint64_t deny_count,
                                     const R3IpOutcome *outcomes, uint32_t count,
                                     char *out, size_t cap);

/* --- the stage-2 integration context (the object behind StageCtx::substep) -------------
 * One instance lives in the driver (never on the stack). The entry class fills the I/O
 * window and the IP table, runs the layout before stage 1, then hands the embedded
 * R3SubstepEnv to each substep. The host test fills the same struct with a RAM stub.
 */
#define R3_INTEG_MAX_OUTCOMES R3_IP_COUNT

struct R3IntegrationCtx {
    void             *owner;          /* the driver instance (host test: stub owner) */
    R3Rt              rt;              /* layout + ready/latch; first member          */
    R3SubstepEnv      env;             /* handed to every substep                     */
    R3EvidenceLines   ev;              /* substep/dump/teardown line arena            */
    R3EvSink          sink;            /* N48R3EVID-* publisher (may be null on host) */
    R3Io              io;              /* adapter in rdna3_substep.h (contract 6 fence) */
    R3IoWindow        window;          /* mapped window sizes for the pure checks     */

    bool              prepared;        /* layout + whitelist arm + registration done  */
    bool              prep_ok;         /* false => stage 1..5 must not run            */
    const char       *prep_fail;       /* static reason when prep_ok == false         */
    char              prep_chain[192]; /* D-11: "first cause -> consequence" (never reset) */

    uint32_t          stage;           /* requested n48r3-stage                       */
    uint32_t          effective_stage; /* clamped by the runner                       */
    uint32_t          kill_mask;       /* R3_IP_BIT(x) for leaves explicitly = 0      */
    uint32_t          psp_version_pre; /* D3: from the existing C2PMSG_58 preflight    */
    bool              psp_version_pre_ok;

    R3IpOutcome       outcomes[R3_INTEG_MAX_OUTCOMES];
    uint32_t          outcome_count;
    R3Ip              failed_ip;       /* substep that stopped the ladder             */
    bool              failed;
};

/* Build the substep environment from the context (rt/ev/sink/ip/fb_mc_base). Pure. */
void r3_integration_bind_env(R3IntegrationCtx *ctx, const void *ip_base_table,
                             uint64_t fb_mc_base);

/* Sync preflight probe results into env after preflight has populated ctx->psp_version_pre.
 * Called at the end of stage_preflight (which runs after bind_env). */
static inline void r3_integration_sync_preflight_env(R3IntegrationCtx *ctx)
{
    if (!ctx)
        return;
    ctx->env.psp_version_pre = ctx->psp_version_pre;
    ctx->env.psp_version_pre_ok = ctx->psp_version_pre_ok;
}


/* Record one substep outcome (called by the stage wrappers after run). */
void r3_integration_note_outcome(R3IntegrationCtx *ctx, R3Ip ip, bool ran,
                                 const R3SubstepResult *result);

/* Gate inputs view over the context. */
R3GateInputs r3_integration_gate_inputs(const R3IntegrationCtx *ctx);

/* --- D-02 read-only preflight gate (pure) ----------------------------------------------
 * The ladder's "preflight" stage samples one reading per register in the probe set below
 * through R3Io (so every address enters the stage-2 read manifest, B5) and hands the table
 * to r3_preflight_evaluate(). The decision is a pure function of the readings and the
 * golden table: layer 1 compares value-gated registers with their registered baseline,
 * layer 2 requires the read to complete and rejects the 0xFFFFFFFF "all ones" sentinel.
 * Gate/mask/baseline come only from GoldenReg; this layer invents no judgement.
 *
 * The probe set is one canary per segment the five substeps and the layout use, plus the
 * two MMHUB FB value gates and the MP0 C2PMSG_81 SOS-alive gate. C2PMSG_58 is the PSP
 * version register recorded to rebuild the D3 baseline with the corrected segment (v1.4).
 */
struct R3PreflightReading {
    const r3gold::GoldenReg *reg; /* golden entry: gate + baseline                   */
    bool     read_ok;             /* R3Io::mmio completed the read                   */
    uint32_t addr;                /* BAR5 dword actually addressed (evidence)        */
    uint32_t value;               /* only meaningful when read_ok                    */
};

enum class R3PreflightVerdict : uint8_t {
    Ok = 0,
    ReadFail,      /* the read did not complete                                   */
    AllOnes,       /* 0xFFFFFFFF unreadable sentinel: abort (upstream probe)      */
    ValueMismatch, /* a value-gated register missed its golden baseline           */
};

struct R3PreflightItem {
    const char *name;
    R3PreflightVerdict verdict;
    uint32_t addr;
    uint32_t value;
};

#define R3_PREFLIGHT_MAX 16u

struct R3PreflightResult {
    bool ok;
    uint32_t count;
    R3PreflightItem items[R3_PREFLIGHT_MAX];
};

void r3_preflight_evaluate(const R3PreflightReading *readings, uint32_t count,
                           R3PreflightResult *out);

/* §15.1 item 4 (S12): extract psp_version_pre from preflight probe readings and sync into
 * env. Pure entry point shared by stage_preflight and host tests, so the stage call site
 * is verifiable without linking the kext-only stage table. */
void r3_stages_preflight_sync_env(R3IntegrationCtx *ctx,
                                  const R3PreflightReading *readings, uint32_t probe_count,
                                  const R3PreflightResult *res);

/* The fixed D-02 read set (see the comment above). */
uint32_t r3_preflight_probe_count(void);
const r3gold::GoldenReg *r3_preflight_probe(uint32_t index);
/* Match two GoldenReg pointers by their ref() fields instead of pointer equality.
 * constexpr GoldenReg variables have internal linkage (each TU its own copy), so
 * pointer comparison across TUs always fails. This field match is constexpr-safe. */
static inline bool r3_preflight_match(const r3gold::GoldenReg *a,
                                      const r3gold::GoldenReg *b)
{
    if (!a || !b)
        return false;
    const amdgpu::RegRef ra = a->ref();
    const amdgpu::RegRef rb = b->ref();
    return ra.block == rb.block && ra.offset == rb.offset && ra.base_idx == rb.base_idx;
}

/* D-05: preparation is skipped entirely when the survey did not complete (rc != 0): no
 * whitelist re-arm, no counter/manifest reset. The ladder's stage 0 calls this. */
bool r3_integration_should_prepare(bool survey_ok, uint32_t stage);

/* D-11: record a preparation failure without overwriting the first cause; later causes
 * are appended to prep_chain as "first -> next". */
void r3_integration_note_prep_failure(R3IntegrationCtx *ctx, const char *reason);

#endif /* N48_RDNA3_INTEGRATION_H */
