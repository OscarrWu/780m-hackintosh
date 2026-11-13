/*
 * rdna3_substep.h - the shared shape of every stage-2 power-up substep (tech plan §2.2/§10).
 *
 * This is the template K5-K8 implement against; K4 (PSP 13.0.4, amd/psp_v13_0.cpp) is the
 * reference instance. A substep is always four sections, in this order, and never reordered:
 *
 *   1. PRE  - preconditions, read-only in the overwhelming majority. A precondition that
 *             fails stops the whole substep and exports evidence BEFORE any state write:
 *             the driver must not "try it and see" (requirement principle 1: 日常用无坑).
 *   2. SEQ  - the register/protocol sequence. Every write is classified by the K3 write
 *             whitelist (rdna3_writeguard.h) under this substep's owner and B2 class; every
 *             read goes into the caller's read manifest.
 *   3. CHK  - the intermediate self-checks of §8.1 (C0.4). A failed check fails the substep.
 *   4. EV   - the evidence fields of §8.1/E2 (ready state, check results, version values,
 *             sizes, measured times) and the §3.x per-IP fields.
 *
 * The substep returns exactly one R3SubstepResult. kOk means every section above ran to its
 * end; kFail means it did not, and R3SubstepResult::name says which section stopped
 * (`pre:<reason>`, `seq:<step>`, `chk:<check>`). There is no retry here (F6): the only retry
 * in stage 2 is inside the SMU substep's own verdict logic (D11), not in this runner.
 *
 * ---------------------------------------------------------------------------
 * R3Io - the injectable register/delay surface (tech plan §10)
 * ---------------------------------------------------------------------------
 * The kext does not gain a runtime indirection layer: it hands the substep a struct whose
 * function pointers are filled ONCE from amdgpu::DeviceContext accessors (amd/amdgpu_regs.h)
 * or from the AMDGPU-compatible WWREG32/WREG32/RREG32/mdelay surface (the other IP headers'
 * origin: mac-amdgpu). Host tests fill the same struct with a RAM-backed stub, so the whole
 * state machine - including failure branches and timeouts - runs on Linux.
 *
 *   mmio(bar, dword,  value)  dword-addressed BAR5 access; *value is in/out.
 *   smuio(index, data, value) SMN indirect access through the shared PCIE_INDEX2/DATA2 pair.
 *                             Contract: the caller (the owning substep) holds the SMN lock
 *                             and has a hard timeout; this header only fixes the shape.
 *   udelay(ctx, us), mdelay(ctx, ms)  delays; `now_us` when the caller can measure elapsed
 *                             time, else the substep accumulates the delays it requested.
 *   bus(bar, byte_off, value,  dwords)  CPU access into the carveout through BAR0 only (B4).
 *                             `bar` is the BAR index (0 for the VRAM aperture); reads/writes
 *                             are dword-or-wider; *value is in/out. Host tests point it at a
 *                             plain buffer, which is how the firmware staging is verified.
 *   lock/unlock               serialise the whole substep against every other MMIO user.
 *
 * Nothing in this header performs I/O; it is types + invariants + static_asserts only.
 */
#ifndef N48_RDNA3_SUBSTEP_H_
#define N48_RDNA3_SUBSTEP_H_

#include <stddef.h>
#include <stdint.h>

// --- B2 write classes (tech plan §6.1). Mirrors WClass so a substep can pass kR3WData
// straight to the whitelist register table without a switch. Kept as its own enum so the
// host state-machine tests do not need the whitelist translation unit.
enum R3WClassId : uint8_t {
    kR3WAccess   = 0,  // ① access enable
    kR3WData     = 1,  // ② data movement (firmware stage, buffer zero/fill)
    kR3WProtocol = 2,  // ③ protocol interaction (mailbox/wptr/doorbell)
    kR3WState    = 3,  // ④ state configuration (ring registers, enables)
};

// The four sections a substep runs, in order. Used for R3SubstepResult::section.
enum R3SubstepSection : uint8_t {
    kR3SecPre = 0,
    kR3SecSeq = 1,
    kR3SecChk = 2,
    kR3SecEv  = 3,
};

enum R3SubstepStatus : uint8_t {
    kR3SsecOk   = 0,
    kR3SsecFail = 1,
};

// The one result type. `name` is what the driver logs as the first token after the substep
// label (E1: every failure names its reason, the snapshot and the substep). `readings`
// carries up to `n` named values that belong to the failing section, so the evidence export
// can show them even on the failure path (§8.3: partial evidence still lands).
#define R3_SUBSTEP_READINGS 4u

struct R3SubstepReading {
    uint32_t    addr;   // BAR5 dword the value came from, or 0 when `what` is not a register
    uint32_t    value;
    const char *what;   // static label, never null
};

struct R3SubstepResult {
    R3SubstepStatus  status;
    R3SubstepSection section;   // the section that stopped (kR3SecEv on success)
    const char      *name;      // static: "ok", "pre:<reason>", "seq:<step>", "chk:<check>"
    uint32_t         step;      // step index inside `section` (0-based, as the substep numbers them)
    uint32_t         reading_count;
    R3SubstepReading readings[R3_SUBSTEP_READINGS];
};

static inline bool r3_substep_ok(const R3SubstepResult *r)
{
    return r && r->status == kR3SsecOk;
}

// Build a failure result. Kept inline so every substep fails identically.
static inline R3SubstepResult r3_substep_fail(R3SubstepSection section, uint32_t step,
                                             const char *name)
{
    R3SubstepResult r;
    r.status = kR3SsecFail;
    r.section = section;
    r.step = step;
    r.name = name ? name : "fail";
    r.reading_count = 0;
    return r;
}

static inline R3SubstepResult r3_substep_pass(void)
{
    R3SubstepResult r;
    r.status = kR3SsecOk;
    r.section = kR3SecEv;
    r.step = 0;
    r.name = "ok";
    r.reading_count = 0;
    return r;
}

// Attach one reading to a result. A full list keeps the first R3_SUBSTEP_READINGS entries;
// the extras are dropped silently on purpose - the labels are a fixed-size snapshot, and
// the caller that needs more must put it in its own evidence text.
static inline void r3_substep_note(R3SubstepResult *r, uint32_t addr, uint32_t value,
                                   const char *what)
{
    if (!r || r->reading_count >= R3_SUBSTEP_READINGS)
        return;
    R3SubstepReading &e = r->readings[r->reading_count++];
    e.addr = addr;
    e.value = value;
    e.what = what ? what : "-";
}

// Section names for logs/evidence ("pre", "seq", "chk", "ev").
const char *r3_substep_section_name(R3SubstepSection s);

// --- R3Io -------------------------------------------------------------------------------
// BAR index for the VRAM aperture in R3Io::bus. The carveout is reachable only through it (B4).
#define R3_IO_BAR_VRAM 0u

struct R3Io {
    void *ctx;   // opaque: owner of the mapped windows (DeviceContext*, host stub, ...)

    // dword-addressed BAR5 register access. Returns false when the address is out of range
    // (a false return is never a value: the caller must treat it as a failed access).
    bool (*mmio)(void *ctx, uint32_t dword, uint32_t *value, bool write);

    // SMN indirect access through the shared PCIE_INDEX2/DATA2 pair. The caller holds the lock.
    bool (*smuio)(void *ctx, uint32_t index, uint32_t *value, bool write);

    // Byte-addressed access into BAR `bar` (R3_IO_BAR_VRAM for the carveout), `dwords` wide.
    bool (*bus)(void *ctx, uint32_t bar, uint64_t byte_off, uint64_t *value, bool write,
                uint32_t dwords);

    void (*udelay)(void *ctx, uint32_t us);
    void (*mdelay)(void *ctx, uint32_t ms);

    // 64-bit microsecond clock; nullptr => the substep accumulates the delays it requested.
    uint64_t (*now_us)(void *ctx);

    // Contract 6 (P4a defines, P4b implements): flush the CPU write buffer so previously
    // staged stores are globally visible before a doorbell tells firmware to consume them.
    // x86 semantics = sfence. Callers MUST tolerate null: if (io->fence) io->fence(io->ctx);
    void (*fence)(void *ctx);

    // Contract 7 (v1.7): make staged host writes visible to the device that is about to be
    // rung (HDP memory flush). Linux amdgpu_device_flush_hdp and nullarc hdpFlush both do
    // this before the doorbell; sfence alone is CPU-only. Returns false when the window is
    // absent, the write is denied, or the MMIO access fails: callers MUST fail the submit.
    bool (*hdp_flush)(void *ctx);

    // Serialises one whole substep against every other MMIO user (the SMN pair and the
    // mailbox registers are shared). May be null on the host.
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
};

// Which callbacks a substep needs. `bus` is optional (a substep that touches no VRAM passes
// null and never calls it); the rest are mandatory. Filled from the device context once by
// the integration card, never re-bound per call.
static inline bool r3_io_complete(const R3Io *io)
{
    return io && io->mmio && io->smuio && io->udelay && io->mdelay && io->lock && io->unlock;
}

// --- Read/write helpers used by every substep -------------------------------------------
// A read failure yields the 0xFFFFFFFF "unreadable" sentinel (same convention as RREG32 and
// the stage-1 survey) and sets *ok=false when `ok` is given, so a caller can distinguish
// "the register says ~0" from "the register could not be read".
static inline uint32_t r3_io_read32(const R3Io *io, uint32_t dword, bool *ok)
{
    uint32_t v = 0xFFFFFFFFu;
    const bool good = io && io->mmio && io->mmio(io->ctx, dword, &v, false);
    if (ok)
        *ok = good;
    return v;
}

static inline bool r3_io_write32(const R3Io *io, uint32_t dword, uint32_t value)
{
    uint32_t v = value;
    return io && io->mmio && io->mmio(io->ctx, dword, &v, true);
}

// Contract 7: returns false when no visibility window exists or the flush failed. Callers
// must treat false as a failed submit (no doorbell).
static inline bool r3_io_hdp_flush(const R3Io *io)
{
    return io && io->hdp_flush && io->hdp_flush(io->ctx);
}

static inline void r3_io_udelay(const R3Io *io, uint32_t us)
{
    if (io && io->udelay)
        io->udelay(io->ctx, us);
}

static inline void r3_io_mdelay(const R3Io *io, uint32_t ms)
{
    if (io && io->mdelay)
        io->mdelay(io->ctx, ms);
}
// Poll `dword` until `(*value & mask) == want`, with `us_per_poll` between reads. Returns 1
// when the predicate held, 0 on timeout, -1 when a read itself failed; the final read is
// stored in *value in every case. `polls` bounds the number of reads, so the worst case is
// bounded by polls * us_per_poll regardless of how slow the bus is.
//
// Budget arithmetic is the caller's: the constants sit next to the protocol they serve (see
// amd/psp_v13_0.h), so a substep can express "Linux 100 ms first, nullarc 2 s ceiling" as two
// calls without this header inventing a shared unit.
static inline int r3_io_poll(const R3Io *io, uint32_t dword, uint32_t mask, uint32_t want,
                             uint32_t polls, uint32_t us_per_poll, uint32_t *value)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < polls; i++) {
        bool ok = false;
        v = r3_io_read32(io, dword, &ok);
        if (!ok) {
            if (value)
                *value = v;
            return -1;
        }
        if ((v & mask) == want) {
            if (value)
                *value = v;
            return 1;
        }
        if (us_per_poll)
            r3_io_udelay(io, us_per_poll);
    }
    if (value)
        *value = v;
    return 0;
}

// --- the substep itself ----------------------------------------------------------------
// Every stage-2 IP module exposes exactly this entry (K5-K8 follow it):
//   R3SubstepResult <ip>_r3_substep(const R3Io *io, const R3SubstepEnv *env);
// env->rt is the shared runtime (layout plan + ready/latch bits); the substep sets its own
// ready bit on success and touches nothing else outside its own evidence arena. Everything
// is caller-owned: a substep never allocates.
//
// The evidence channel. Process lines go to the shared n48log ring (amd/n48log.h):
// r3_ev_add forwards every "<tag>: <text>" line to n48_logf, and the ring is read back
// through the ReadLog selector / logstream (task 11-10-stage2-evidence-n48log, design
// §2/§7.4). The per-substep line table below is the retired stage2-log publish path's
// shell: nothing appends to it and nothing assembles it (design §7.3 retired the
// N48R3EVID-stage2-log property together with the 6 KiB arena). R3_EVID_TEXT_BYTES keeps
// the retired budget's value (6144 + 96 * 16) because rdna3_writeguard sizes its
// independent N48R3EVID-expect-verify buffer with it.
#define R3_EVID_MAX_LINES   96u
#define R3_EVID_TEXT_BYTES  7680u

struct R3EvidenceLines {
    const char *tags[R3_EVID_MAX_LINES];
    const char *texts[R3_EVID_MAX_LINES];
    uint32_t    count;
};

static_assert(sizeof(R3EvidenceLines) <= 8192u,
              "retired stage2-log line table outgrew its kernel-stack budget (T16)");

static inline void r3_ev_init(R3EvidenceLines *ev)
{
    if (!ev)
        return;
    ev->count = 0;
}

// Append one line to the driver log ring as "<tag>: <text>" (n48_logf adds the newline).
// The ring is global and capacity is the ring's own semantics (g_overflow + AT CAPACITY),
// so this always succeeds at this layer; `ev` stays in the signature because every IP
// module's evf() hands it the substep's (retired) line table.
bool r3_ev_add(R3EvidenceLines *ev, const char *tag, const char *text);

// Set one named chunk value: appends "<tag>: <text>\n" and re-publishes the whole chunk text
// to `sink` under `key` (the stage-1 N48R3EVID-* convention). `text_buf` must be the stable
// buffer the integration owns; returns false when the sink is absent or the assembly does not
// fit. This keeps one chunk per value family without a second buffer per family.
typedef bool (*R3EvSetProperty)(void *user, const char *key, const char *bytes, size_t len);

struct R3EvSink {
    R3EvSetProperty set_property;
    void *user;
};

// --- the per-substep context handed to the IP modules ----------------------------------
// `rt` is the shared runtime (layout + ready/latch + the one substep context). The fields
// below are the read-only inputs K4-K8 need that the stage-1 survey already collected; the
// integration card fills them once and never lets a substep derive them again (L8: the plan
// is the authority for every address).
struct R3LayoutPlan;   // rdna3_layout.h
struct R3Rt;           // rdna3_runtime.h

struct R3SubstepEnv {
    R3Rt              *rt;          // shared runtime block (plan + ready/latch)
    R3EvidenceLines   *ev;          // caller-owned evidence arena for this substep
    const R3EvSink    *sink;        // optional N48R3EVID-* publisher (may be null on host)
    uint64_t           fb_mc_base;  // MMMC_VM_FB_LOCATION_BASE<<24 (stage-0 D3 reading)
    const void        *ip;          // amdgpu::IPBaseTable (GC/SDMA reads)
    // D3/S12: PSP version pre from the existing preflight C2PMSG_58 probe. The preflight
    // runs before prep; bind_env copies this after the preflight. RLC/SDMA versions are
    // observations in their own substeps (no preflight probes).
    uint32_t           psp_version_pre;
    bool               psp_version_pre_ok;
    // R1 (v1.7): BAR5 dword that flushes the host data path after a frame write and before
    // the doorbell (Linux amdgpu_device_flush_hdp / nullarc hdpFlush). Discovered in prepare;
    // a missing window fails the substep before any submit.
    uint32_t           hdp_flush_dword;  // BAR5 dword; meaningful only when valid is true
    bool               hdp_flush_valid;
    bool               strict_expect;  // Step2: true=host test (expectation failure→substep fail)
                                      //        false=true hardware (record only, no gate)

#if !defined(KERNEL)
    // Host-test seams: they let the runner play the PSP (answer a staged command, bump the
    // fence) so the whole sequence runs on Linux. They are compiled out of the kext build -
    // a production contract should not carry fields that are always null.
    void (*cmd_hook)(void *cookie, uint64_t cmd_bar0_off);       // after staging, before doorbell
    void (*ring_hook)(void *cookie, uint64_t ring_bar0_off);     // rb frame written, pre-doorbell
    void *hook_cookie;
#endif
};


#endif  // N48_RDNA3_SUBSTEP_H_
