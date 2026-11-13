//
//  psp_v13_0.cpp - the PSP 13.0.4 power-up substep (see psp_v13_0.h for the contract,
//  the license header and the Linux provenance of every offset).
//
//  Section map (tech plan §3.1; requirement C1):
//    pre()   P0 MP0 mailbox resolved through the golden table (group 1), P2 layout plan
//            valid + PSP grants present, P3 SOS alive (C2PMSG_33 recorded first, then 81 != 0).
//    seq()   S1 read 33/81 -> S2 zero ring/cmd/fence -> S3 ring create -> S4 stage TOC ->
//            S5 LOAD_TOC + read resp.tmr_size -> S6 TMR placement in the 64 MiB slot ->
//            S7 SETUP_TMR -> S8 17x LOAD_IP_FW -> S9 AUTOLOAD_RLC right after RLC_G.
//    chk()   S10 C2PMSG_33 bit31 (observation), S11 GPCOM fence measured, S12 versions.
//    ev()    the §3.1.5 fields into env->ev.
//
//  Every write goes through reg_write() (BAR5 dword, ③ Protocol or ④ State) or bus_write()
//  (BAR0, ② Data), and both classify it with the K3 whitelist under WSubstep::Psp BEFORE
//  issuing it. A DENY is a substep failure, never a "write it anyway" - that is the reason
//  the sequence helpers take the substep state instead of a bare context pointer.
//
//  Two ordering facts are load-bearing and are re-checked at run time as well as in the
//  host tests (planted breaks): AUTOLOAD_RLC must follow the RLC_G(8) LOAD_IP_FW, and the 17
//  pieces must follow the table order of r3_psp_load_order (Linux ucode-id ascending, D2).
//
//  Kernel build constraints: -nostdinc, no libc, no exceptions/RTTI, no allocation. Only
//  <stdint.h>/<stddef.h> + libkern (memcpy/memset/snprintf/vsnprintf) + the RDNA3 headers.
//
#include "psp_v13_0.h"

#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h>   /* memcpy/memset/snprintf/vsnprintf, no libc under -nostdinc */
#else
#include <stdio.h>             /* host runner: same names from libc */
#include <string.h>
#endif

#include "amdgpu_ip.h"
#include "../rdna3/golden_values.h"
#include "../rdna3/rdna3_layout.h"
#include "../rdna3/rdna3_runtime.h"
#include "../rdna3/rdna3_writeguard.h"

namespace amdgpu {

namespace {

// ---------------------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------------------
WClass to_wclass(R3WClassId c)
{
    switch (c) {
    case kR3WAccess:   return WClass::Access;
    case kR3WData:     return WClass::Data;
    case kR3WProtocol: return WClass::Protocol;
    case kR3WState:    return WClass::State;
    }
    return WClass::Data;
}

// Result names are static: R3SubstepResult::name outlives the substep (it is logged and
// exported afterwards), so every failure reason is a file-scope literal (E1).
constexpr const char *kPreNoIo      = "pre:no-io";
constexpr const char *kPreNoEnv     = "pre:no-env";
constexpr const char *kPreLayout    = "pre:layout-invalid";
constexpr const char *kPreGrants    = "pre:layout-grants";
constexpr const char *kPreSosAlive  = "pre:sos-not-alive";
constexpr const char *kPreNoMcBase  = "pre:no-mc-base";
constexpr const char *kPreNoHdp     = "pre:no-hdp-window";

constexpr const char *kSeqZero      = "seq:zero-fill";
constexpr const char *kSeqRingReady = "seq:ring-not-ready";
constexpr const char *kSeqRingWrite = "seq:ring-regs";
constexpr const char *kSeqRingResp  = "seq:ring-response";
constexpr const char *kSeqStage     = "seq:fw-stage";
constexpr const char *kSeqTocCmd    = "seq:toc-cmd";
constexpr const char *kSeqTocFence  = "seq:toc-fence";
constexpr const char *kSeqTmrsize   = "seq:tmr-size";
constexpr const char *kSeqTmrPlace  = "seq:tmr-placement";
constexpr const char *kSeqTmrCmd    = "seq:setup-tmr";
constexpr const char *kSeqIpFw      = "seq:load-ip-fw";
constexpr const char *kSeqAutoload  = "seq:autoload-rlc";


constexpr const char *kTagPre = "psp-pre";
constexpr const char *kTagSeq = "psp-seq";
constexpr const char *kTagChk = "psp-chk";

// Distinct blobs the 17-piece table draws from: sdma, imu, rlc, mes_2, mes1, pfp, me, mec.
// pfp/me/mec use the plain `common` header parser, so r3_fw_parse_id handles them too.
constexpr uint32_t kStageImgs = 8;
static_assert(kStageImgs == 8u, "load-order sources changed: update parse_stage slots");

// One row of the 17-piece load plan: where the entry's segment is staged.
struct StageRow {
    const R3PspLoadEntry *entry;   // r3_psp_load_order row (fw_type + blob + label)
    const R3FwImage      *img;     // parsed image of the entry's blob
    const R3FwSeg        *seg;     // the entry's segment inside `img`
    uint64_t              stage_mc;// MC address of the entry's 4 KiB staging slot
    const uint8_t        *blob;    // the blob bytes (payload source)
    uint32_t              blob_size;
};

// Everything the sequence needs from the firmware: parsed images (in place, no copies),
// the 17 staged rows, and the TOC payload.
struct Stage {
    R3FwImage imgs[kStageImgs];    // parsed once per blob
    StageRow  rows[R3_PSP_LOAD_COUNT];
    uint32_t  count;
    R3FwImage toc_img;
    const uint8_t *toc_blob;
    uint32_t  toc_off;
    uint64_t  toc_mc;
    uint64_t  toc_size;
    uint64_t  total_bytes;
    int       err;                 // 0 = ok, else kStageErr* or an R3FwHdrStatus value
};

// PSP staging errors outside the header parser's own range (which is negative -1..-7).
constexpr int kStageErrNoLoadOrderSlot = -100;  // a load-order entry named an unknown blob
constexpr int kStageErrRowCapacity    = -101;   // the plan array is shorter than the table
constexpr int kStageErrOverCapacity   = -102;   // the staged bytes do not fit fw_buf

struct Psp {
    const R3Io *io;
    const R3SubstepEnv *env;
    const amdgpu::IPBaseTable *ip;
    R3LayoutPlan plan;
    Stage        stage;
    uint32_t     frame;      // fence value of the next submit (1-based)
    uint64_t     fw_buf_bar0;// BAR0 byte offset of the fw_buf grant (staging target)
    uint32_t     fence_us;   // max observed submit fence latency (µs) - all submits
    uint32_t     fence_cmd;  // cmd_id that produced fence_us
    uint32_t     fence_ctl_us;   // max latency of non-LOAD_IP_FW submits (KPI scope)
    uint32_t     fence_ctl_cmd;
    uint32_t     fence_fw_us;    // max latency of cmd 0x06 (LOAD_IP_FW)
    uint32_t     fence_fw_cmd;
    bool         fence_seen;     // fence_us has a value
    bool         fence_ctl_seen;
    bool         fence_fw_seen;
    bool         fence_measured; // fence_us (the max) came from a real clock
    uint64_t     tmr_size;   // PSP-returned resp.tmr_size
    uint64_t     tmr_mc;     // MC address of the TMR (for the evidence line)
    uint64_t     tmr_sys_phy;
    // MC addresses of the ring/cmd/fence/fw_buf grants. Derived from the plan.ring/... MC
    // bases, never from the TMR: the allocator's MC-address fields are TMR-specific, so the
    // PSP substep derives the others from fb_mc_base (the stage-0 MMMC reading the plan used).
    uint64_t     mc_base;
    uint64_t     ring_mc;
    uint64_t     cmd_mc;
    uint64_t     fence_mc;
    uint64_t     fw_mc;
    // P3-5 (review): the post() command buffer lives in the driver instance instead of a
    // function-level static, so the "not re-entrant" invariant is structural (one Psp per
    // substep, serialised by R3Io::lock) rather than an implicit reliance on linkage.
    uint8_t      cmd_buf[kPspCmdBufBytes];
};

static_assert(sizeof(Stage) <= 5120u, "PSP stage table exceeds its budget (T16)");


void evf(R3EvidenceLines *ev, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void evf(R3EvidenceLines *ev, const char *tag, const char *fmt, ...)
{
    if (!ev)
        return;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r3_ev_add(ev, tag, buf);
}

void put_u32(void *b, uint32_t off, uint32_t v) { memcpy(static_cast<uint8_t *>(b) + off, &v, 4); }

// v1.7: per-step progress, published immediately so a user-space poller can persist the last
// completed step to disk. The final evidence keys are exported only at the end; a freeze would
// otherwise leave nothing behind.
void evp(const R3SubstepEnv *env, const char *text)
{
    if (env && env->sink && env->sink->set_property && text && text[0])
        env->sink->set_property(env->sink->user, "N48R3EVID-progress", text, strlen(text));
}

// ---------------------------------------------------------------------------------------
// whitelisted access
// ---------------------------------------------------------------------------------------
// Every PSP mailbox address is resolved through the discovery table and the golden entry's
// _BASE_IDX (BASE_IDX 1 = the 0x16000 segment on this machine). A local base + offset
// would reintroduce the segment-0 error that crashed the first hardware run (A-03/C0.8).
uint32_t reg_addr(const Psp &p, const r3gold::GoldenReg &g, bool *ok)
{
    uint32_t addr = 0;
    const bool good = p.ip && p.ip->resolve(g.ref(), &addr);
    if (ok)
        *ok = good;
    return good ? addr : 0xFFFFFFFFu;
}

bool reg_write(Psp &p, const r3gold::GoldenReg &g, uint32_t value, R3WClassId cls,
               const char *ctx)
{
    bool ok = false;
    const uint32_t abs = reg_addr(p, g, &ok);
    if (!ok || r3_wg_classify(abs, 1u, to_wclass(cls), ctx) != WVerdict::TxnRead)
        return false;
    return r3_io_write32(p.io, abs, value);
}

uint32_t reg_read(Psp &p, const r3gold::GoldenReg &g, bool *ok)
{
    bool resolved = false;
    const uint32_t abs = reg_addr(p, g, &resolved);
    if (!resolved) {
        if (ok)
            *ok = false;
        return 0xFFFFFFFFu;
    }
    return r3_io_read32(p.io, abs, ok);
}

int reg_poll(Psp &p, const r3gold::GoldenReg &g, uint32_t mask, uint32_t want, uint32_t polls,
             uint32_t us_per_poll, uint32_t *value)
{
    bool ok = false;
    const uint32_t abs = reg_addr(p, g, &ok);
    if (!ok) {
        if (value)
            *value = 0xFFFFFFFFu;
        return -1;
    }
    return r3_io_poll(p.io, abs, mask, want, polls, us_per_poll, value);
}

// The writeguard's address space is dword addresses (writeguard.h: "pa is a dword address").
// The layout plan carries byte offsets; bus_write converts them before classifying.
bool bus_write(Psp &p, uint64_t bar0_off, uint64_t value, uint32_t dwords, const char *ctx)
{
    if ((bar0_off & 3u) != 0u || dwords == 0u || dwords > 2u) {
        return false;
    }
    const uint32_t dword_addr = static_cast<uint32_t>(bar0_off >> 2);
    if (r3_wg_classify(dword_addr, dwords, WClass::Data, ctx) != WVerdict::TxnRead) {
        return false;
    }
    uint64_t v = value;
    return p.io->bus && p.io->bus(p.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, true, dwords);
}

bool bus_read(Psp &p, uint64_t bar0_off, uint32_t dwords, uint64_t *out)
{
    uint64_t v = 0;
    if (!p.io->bus || !p.io->bus(p.io->ctx, R3_IO_BAR_VRAM, bar0_off, &v, false, dwords))
        return false;
    *out = v;
    return true;
}

// Copy `len` bytes from `src` into grant `g` at `off` (both inside the grant). Doubles the
// write width to 4 dwords when the offset is 8-byte aligned so the ② manifest count stays
// small; every write is classified.
bool stage_copy(Psp &p, const R3Grant *g, uint64_t off, const uint8_t *src, uint64_t len,
                const char *ctx)
{
    if (!g || off > g->size || len > g->size - off)
        return false;
    // One 64-bit store is the widest access BAR0 gives (WBAR0_64); every write is 8 B when
    // aligned and 4 B at the tail, so the ② manifest count stays linear in the bytes.
    uint64_t i = 0;
    while (i < len) {
        const uint32_t dwords = (i + 8u <= len && ((off + i) % 8u) == 0u) ? 2u : 1u;
        uint64_t v = 0;
        memcpy(&v, src + i, dwords * 4u);
        if (!bus_write(p, g->start + off + i, v, dwords, ctx))
            return false;
        i += dwords * 4u;
    }
    return true;
}

// Zero [g->start, g->start+bytes) in 2-dword (8 B) ② writes (S2; nullarc's explicit clear).
// 8 B is the widest access R3Io::bus guarantees (the fake rejects wider; BAR0 writes are one
// 64-bit store each, which is also what the base driver's WBAR0_64 does).
bool zero_grant(Psp &p, const R3Grant *g, uint64_t bytes, const char *ctx)
{
    if (!g || bytes > g->size || (bytes % 32u) != 0u)
        return false;
    for (uint64_t off = 0; off < bytes; off += 32u) {
        for (uint32_t d = 0; d < 4u; d++) {
            if (!bus_write(p, g->start + off + d * 8u, 0, 2u, ctx))
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// GPCOM frame construction (T13 pins these offsets against psp_gfx_if.h)
// ---------------------------------------------------------------------------------------
// psp_gfx_if.h:497-512. Linux leaves cmd_buf_size 0 (amdgpu_psp.c:3908) and everything
// past +32 zero; the PSP only needs the two addresses and the fence value.
void build_rb_frame(uint8_t *frame, uint64_t cmd_mc_addr, uint64_t fence_mc_addr,
                    uint32_t fence_value)
{
    memset(frame, 0, kPspFrameBytes);
    put_u32(frame, kRbFrameCmdLo, static_cast<uint32_t>(cmd_mc_addr));
    put_u32(frame, kRbFrameCmdHi, static_cast<uint32_t>(cmd_mc_addr >> 32));
    put_u32(frame, kRbFrameFenceLo, static_cast<uint32_t>(fence_mc_addr));
    put_u32(frame, kRbFrameFenceHi, static_cast<uint32_t>(fence_mc_addr >> 32));
    put_u32(frame, kRbFrameFenceVal, fence_value);
}

// psp_gfx_if.h:466-487: buf_size, version, cmd_id, then the body at +28. resp (+864) and
// the tail stay zero: the PSP writes the response. buf_size/buf_version are part of the
// zero-fill and stay 0: Linux does not populate them (amdgpu_psp.c:3908, C1.1/B-04).
void build_cmd_resp(uint8_t *cmd, uint32_t cmd_id, const uint8_t *body, uint32_t body_bytes)
{
    memset(cmd, 0, kPspCmdBufBytes);
    put_u32(cmd, kCmdRespCmdId, cmd_id);
    if (body && body_bytes)
        memcpy(cmd + kCmdRespCmd, body, body_bytes);
}

// ---------------------------------------------------------------------------------------
// firmware staging
// ---------------------------------------------------------------------------------------
struct Blob {
    const uint8_t *data;
    uint32_t       size;
};

bool blob_of(FwId id, Blob *out)
{
    const FwBlob *b = fw_get(id);
    if (!b || !b->data || b->size == 0)
        return false;
    out->data = b->data;
    out->size = static_cast<uint32_t>(b->size);
    return true;
}

// Index of `seg` inside `img` (both always come from the same parsed image here).
uint32_t seg_index(const R3FwImage *img, const R3FwSeg *seg)
{
    for (uint32_t i = 0; i < img->seg_count; i++)
        if (&img->segs[i] == seg)
            return i;
    return 0;
}

// Parse the five sources and lay their 4 KiB-aligned slots out inside the fw_buf grant.
// Each image's area starts on a 4 KiB boundary and r3_fw_stage_addr keeps every in-image
// slot 4 KiB aligned, so every LOAD_IP_FW address satisfies psp_gfx_if.h:317 (C1.2).
bool parse_stage(Psp &p, uint64_t fw_mc, uint64_t fw_capacity)
{
    Stage &st = p.stage;
    memset(&st, 0, sizeof st);
    st.err = R3_FW_HDR_OK;

    static const FwId kSources[kStageImgs] = {
        FwId::SDMA6, FwId::GC11_IMU, FwId::GC11_RLC, FwId::GC11_MES_2, FwId::GC11_MES1,
        FwId::GC11_PFP, FwId::GC11_ME, FwId::GC11_MEC,
    };
    Blob blob[kStageImgs];
    uint64_t image_mc[kStageImgs];
    uint64_t off = 0;

    // S4: the TOC is staged at fw_buf offset 0 (tech plan §3.1 row S4; nullarc stages it the
    // same way, bringup5.sh:13-14). Everything else starts after its 4 KiB slot.
    Blob toc_blob;
    if (!blob_of(FwId::PSP13_TOC, &toc_blob)) {
        st.err = R3_FW_HDR_ERR_ZERO_SIZE;
        return false;
    }
    st.toc_mc = fw_mc;
    {
        const int trc = r3_fw_parse_toc(toc_blob.data, toc_blob.size, st.toc_mc, &st.toc_img);
        if (trc != R3_FW_HDR_OK) {
            st.err = trc;
            return false;
        }
    }
    st.toc_blob = toc_blob.data;
    st.toc_off = st.toc_img.segs[0].offset;
    st.toc_size = st.toc_img.segs[0].size;
    off = r3_fw_stage_size(&st.toc_img);

    for (uint32_t i = 0; i < kStageImgs; i++) {
        if (!blob_of(kSources[i], &blob[i])) {
            st.err = R3_FW_HDR_ERR_ZERO_SIZE;
            return false;
        }
        off = (off + (R3_FW_STAGE_ALIGN - 1ull)) & ~(R3_FW_STAGE_ALIGN - 1ull);
        image_mc[i] = fw_mc + off;
        const int rc = r3_fw_parse_id(kSources[i], blob[i].data, blob[i].size, image_mc[i],
                                      &st.imgs[i]);
        if (rc != R3_FW_HDR_OK) {
            st.err = rc;
            return false;
        }
        off += r3_fw_stage_size(&st.imgs[i]);
    }

    // One row per load-order entry, in table order (never sorted: the order is D2).
    for (uint32_t e = 0; e < R3_PSP_LOAD_COUNT; e++) {
        const R3PspLoadEntry &en = r3_psp_load_order[e];
        uint32_t slot = kStageImgs;
        for (uint32_t i = 0; i < kStageImgs; i++)
            if (kSources[i] == en.source) {
                slot = i;
                break;
            }
        if (slot == kStageImgs) {
            st.err = kStageErrNoLoadOrderSlot;
            return false;
        }
        const R3FwSeg *seg = r3_fw_find_seg(&st.imgs[slot], en.fw_type);
        if (!seg) {
            st.err = R3_FW_HDR_ERR_ZERO_SIZE;
            return false;
        }
        if (st.count >= R3_PSP_LOAD_COUNT) {
            st.err = kStageErrRowCapacity;
            return false;
        }
        StageRow &row = st.rows[st.count++];
        row.entry = &en;
        row.img = &st.imgs[slot];
        row.seg = seg;
        row.blob = blob[slot].data;
        row.blob_size = blob[slot].size;
        row.stage_mc = r3_fw_stage_addr(image_mc[slot], &st.imgs[slot],
                                        seg_index(&st.imgs[slot], seg));
    }

    st.total_bytes = off;
    if (off > fw_capacity) {
        st.err = kStageErrOverCapacity;
        return false;
    }
    return true;
}

// Stage one segment: copy its payload bytes to its slot through BAR0. `slot_mc` is the
// slot's MC address; the BAR0 byte offset is slot_mc minus the MC base of the carveout (the
// grant's `start` is a BAR0 byte offset, not an address - mixing the two is a units bug).
bool stage_segment(Psp &p, const R3Grant *g, uint64_t slot_mc, const uint8_t *blob,
                   uint32_t off, uint32_t size, const char *ctx)
{
    if (!g || !blob || slot_mc < p.mc_base)
        return false;
    // stage_copy's `off` is relative to the grant; slot_mc - mc_base is an absolute BAR0
    // offset, so subtract the grant start once more.
    const uint64_t abs_bar0 = slot_mc - p.mc_base;
    if (abs_bar0 < g->start)
        return false;
    return stage_copy(p, g, abs_bar0 - g->start, blob + off, size, ctx);
}

// ---------------------------------------------------------------------------------------
// one GPCOM submission: rb frame -> HDP flush -> wptr -> fence poll
// ---------------------------------------------------------------------------------------
// Linux's submit (amdgpu_psp.c:3869-3915): frame at wptr (written by the caller), flush HDP,
// advance wptr by 16 dwords, write C2PMSG_67, poll the fence buffer for `index`.
// The ring pointer is read from C2PMSG_67 on every submit (Linux's psp_v13_0_4_ring_get_wptr,
// psp_v13_0_4.c:309-317): the PSP owns that register, so it is the one authority on where the
// next frame goes. A cached copy would silently disagree the moment the PSP resets the ring.
//
// HDP flush (SF-1, batch 1c, keep-flush verdict): this project flushes on every
// submission - fence -> HDP flush -> wptr (below) - the same forced-refresh stance as
// nullarc's pspRunCmd (PhxBringup.cpp:985-988: sfence -> hdpFlush() -> C2PMSG_67;
// hdpFlush() itself at :404-408). Linux's amdgpu_device_flush_hdp early-return on an APU
// without passthrough (amdgpu_device.c:6677-6685) is NOT adopted: that exemption is an
// x86 APU coherence property (INV-C1), while this platform's write path goes through the
// BAR/uncached aperture, so the flush is required for the PSP to observe the ring frame.
bool submit(Psp &p, uint32_t cmd_id, const char *ctx)
{
    const R3Grant *fence = r3_layout_grant(&p.plan, "psp_fence");
    if (!fence || !p.io->bus)
        return false;

    // wptr in DWORDs, as the PSP maintains it (psp_v13_0_4.c:294-331: the register counts
    // rb_frame_dwords, i.e. 16, and the slot is derived mod ring_size_dw).
    bool ok = false;
    const uint32_t ring_dwords = kPspRingBytes / 4u;
    const uint32_t wptr = reg_read(p, r3gold::kMp0SmnC2pmsg67, &ok) % ring_dwords;
    if (!ok)
        return false;
    const uint32_t next = (wptr + kPspFrameDwords) % ring_dwords;
    const uint64_t t0 = p.io->now_us ? p.io->now_us(p.io->ctx) : 0;
    // Contract ⑥: the rb_frame is already in the ring; let the platform publish it before
    // the doorbell is written. The callback is nullable by contract, so call only if set.
    if (p.io->fence)
        p.io->fence(p.io->ctx);
    if (!r3_io_hdp_flush(p.io))
        return false;
    if (!reg_write(p, r3gold::kMp0SmnC2pmsg67, next, kR3WProtocol, ctx))
        return false;

    // Fence poll: the ceiling bounds it, a slow PSP is waited for (F6 has no retry, so this
    // window is the only tolerance). fence_us is the measured latency when a clock exists.
    for (uint32_t i = 0; i < kPspFencePolls; i++) {
        uint64_t v = 0;
        if (!bus_read(p, fence->start, 1u, &v))
            return false;
        if (static_cast<uint32_t>(v) == p.frame) {
            // S11 is an OBSERVATION: keep the max latency and the command that produced it,
            // so the KPI has an explicit scope anchor (never a verdict; 2026-11-05). The
            // control and firmware-load maxima are separate; the KPI applies only to the
            // control submits. Both clock and poll-budget paths update the same maxima.
            const bool clocked = p.io->now_us != nullptr;
            uint32_t us;
            if (clocked) {
                const uint64_t now = p.io->now_us(p.io->ctx);
                us = now >= t0 ? static_cast<uint32_t>(now - t0) : 0u;
            } else {
                us = i * kPspFencePollUs;   // delay budget actually spent
            }
            if (!p.fence_seen || us > p.fence_us) {
                p.fence_us = us;
                p.fence_cmd = cmd_id;
                p.fence_measured = clocked;
                p.fence_seen = true;
            }
            if (cmd_id == kGfxCmdLoadIpFw) {
                if (!p.fence_fw_seen || us > p.fence_fw_us) {
                    p.fence_fw_us = us;
                    p.fence_fw_cmd = cmd_id;
                    p.fence_fw_seen = true;
                }
            } else if (!p.fence_ctl_seen || us > p.fence_ctl_us) {
                p.fence_ctl_us = us;
                p.fence_ctl_cmd = cmd_id;
                p.fence_ctl_seen = true;
            }
            return true;
        }
        r3_io_udelay(p.io, kPspFencePollUs);
    }
    return false;
}

// Post one command: build cmd_resp at the front of the cmd grant, let the platform hook see
// it (null in the kext), submit, then read the response back through BAR0 (L7).
bool post(Psp &p, uint32_t cmd_id, const uint8_t *body, uint32_t body_bytes,
          uint32_t *resp_status, const char *ctx)
{
    const R3Grant *cmd = r3_layout_grant(&p.plan, "psp_cmd");
    const R3Grant *fence = r3_layout_grant(&p.plan, "psp_fence");
    if (!cmd || !fence || !p.io->bus || !p.io->lock || !p.io->unlock)
        return false;

    // nullarc clears the fence dword before every submit so a stale value can never satisfy
    // the poll; the fence grant is a registered Data window.
    if (!bus_write(p, fence->start, 0u, 1u, ctx))
        return false;

    uint8_t *buf = p.cmd_buf;
    build_cmd_resp(buf, cmd_id, body, body_bytes);
    if (!stage_copy(p, cmd, 0, buf, kPspCmdBufBytes, ctx)) {
        return false;
    }
#if !defined(KERNEL)
    if (p.env->cmd_hook)
        p.env->cmd_hook(p.env->hook_cookie, cmd->start);
#endif

    // The submit (frame, doorbell, fence poll, response read-back) is one critical section:
    // the ring, the cmd buffer and the fence are shared with every other MMIO user, and the
    // frame slot is the one submit() will compute from C2PMSG_67.
    p.frame++;
    p.io->lock(p.io->ctx);
    bool ok67 = false;
    const uint32_t slot = reg_read(p, r3gold::kMp0SmnC2pmsg67, &ok67) % (kPspRingBytes / 4u);
    uint8_t rb[kPspFrameBytes];
    build_rb_frame(rb, p.cmd_mc, p.fence_mc, p.frame);
    if (!ok67 ||
        !stage_copy(p, r3_layout_grant(&p.plan, "psp_ring"), slot * 4u, rb, sizeof rb, ctx)) {
        p.io->unlock(p.io->ctx);
        return false;
    }
#if !defined(KERNEL)
    if (p.env->ring_hook)
        p.env->ring_hook(p.env->hook_cookie, slot * 4u);
#endif
    const bool submitted = submit(p, cmd_id, ctx);
    p.io->unlock(p.io->ctx);
    if (!submitted)
        return false;

    // psp_gfx_resp.status is response dword 0 (psp_gfx_if.h:441-461). Linux records it and
    // keeps going: the real success status is not always zero for every firmware version
    // (amdgpu_psp.c:757-793). Only the fence timeout above fails the submit (B-03).
    uint64_t status = 0;
    if (!p.io->bus(p.io->ctx, R3_IO_BAR_VRAM, cmd->start + kCmdRespResp, &status, false, 1u))
        return false;
    const uint32_t st = static_cast<uint32_t>(status);
    if (resp_status)
        *resp_status = st;
    if (st != 0u)
        evf(p.env->ev, kTagSeq,
            "WARN %s: PSP status=0x%08x (cmd=0x%02x) - recorded, continuing", ctx, st,
            cmd_id);
    return true;
}

// ---------------------------------------------------------------------------------------
// PRE: preconditions (P0/P2/P3)
// ---------------------------------------------------------------------------------------
R3SubstepResult pre(Psp &p)
{
    R3SubstepResult r = r3_substep_pass();
    // The PSP substep stages firmware and polls the fence through BAR0, so `bus` is
    // mandatory here even though R3Io marks it optional for substeps that never touch VRAM.
    const bool have_io = r3_io_complete(p.io) && p.io->bus != nullptr;
    const bool have_rt = p.env && p.env->rt;
    if (!have_io) {
        r = r3_substep_fail(kR3SecPre, 0, kPreNoIo);
        evf(p.env ? p.env->ev : nullptr, kTagPre, "FAIL: I/O surface incomplete (mmio/smuio/delay/lock)");
        return r;
    }
    if (!have_rt) {
        r = r3_substep_fail(kR3SecPre, 1, kPreNoEnv);
        evf(p.env ? p.env->ev : nullptr, kTagPre, "FAIL: no runtime block (layout/ready bits)");
        return r;
    }

    // P0: MP0 is a discovery fact, never a constant. Every mailbox address below is
    // resolved through the golden table's _BASE_IDX (group 1 = 0x16000 here); a missing
    // segment makes resolve() fail, and the SOS-alive read then fails closed (B-05).
    p.plan = p.env->rt->layout;
    bool aok = false;
    const uint32_t canary = reg_addr(p, r3gold::kMp0SmnC2pmsg81, &aok);
    // MC base of the carveout: the stage-0 MMMC_VM_FB_LOCATION_BASE<<24 reading, retained by
    // the integration card in DeviceContext::vramMcBase (rdna3_survey.cpp D3). 0 = stage 0
    // never resolved it, and a fabricated base would put every firmware copy in the wrong
    // place, so this is fail closed.
    p.mc_base = p.env->fb_mc_base;
    if (p.mc_base == 0u) {
        r = r3_substep_fail(kR3SecPre, 2, kPreNoMcBase);
        evf(p.env->ev, kTagPre, "FAIL: FB MC base unknown (stage-0 D3 reading missing)");
        return r;
    }
    evf(p.env->ev, kTagPre, "P0 MP0 golden group %u C2PMSG_81 abs=0x%08x%s mc_base=0x%llx",
        static_cast<unsigned>(r3gold::kMp0SmnC2pmsg81.base_idx), canary,
        aok ? "" : " (unresolved)", (unsigned long long)p.mc_base);

    // P2: the layout plan is computed before ANY write (L8) and must carry the PSP grants.
    if (!p.plan.ok) {
        r = r3_substep_fail(kR3SecPre, 3, kPreLayout);
        evf(p.env->ev, kTagPre, "FAIL: layout plan invalid (%s)",
            p.plan.fail_reason ? p.plan.fail_reason : "?");
        return r;
    }
    static const char *const kNeed[] = { "fw_buf", "psp_ring", "psp_cmd", "psp_fence", "tmr" };
    for (const char *tag : kNeed) {
        if (!r3_layout_grant(&p.plan, tag)) {
            r = r3_substep_fail(kR3SecPre, 4, kPreGrants);
            evf(p.env->ev, kTagPre, "FAIL: layout grant %s missing", tag);
            return r;
        }
    }
    evf(p.env->ev, kTagPre, "P2 plan ok grants=%u tmr_in_bar0=%u",
        static_cast<unsigned>(p.plan.grant_count), p.plan.tmr_in_bar0 ? 1u : 0u);

    // R1: no submit without a host-path flush window. Linux and nullarc both flush before the
    // doorbell; skipping it lets the PSP read a stale/zero ring frame and act on address 0.
    if (!p.env->hdp_flush_valid) {
        r = r3_substep_fail(kR3SecPre, 6, kPreNoHdp);
        evf(p.env->ev, kTagPre, "FAIL: no HDP flush window (submit would be unsafe)");
        return r;
    }

    // P3: SOS alive. Read C2PMSG_33 FIRST so the evidence distinguishes "IFWI not finished"
    // from "SOS not alive"; C2PMSG_81 != 0 is the requirement C0.4 gate. Nothing has been
    // written yet, and a failure stops the substep - the driver never loads SOS itself.
    bool ok33 = false, ok81 = false;
    const uint32_t v33 = reg_read(p, r3gold::kMp0SmnC2pmsg33, &ok33);
    const uint32_t v81 = reg_read(p, r3gold::kMp0SmnC2pmsg81, &ok81);
    evf(p.env->ev, kTagPre, "P3 C2PMSG_33=0x%08x%s C2PMSG_81=0x%08x%s",
        v33, ok33 ? "" : "(unreadable)", v81, ok81 ? "" : "(unreadable)");
    if (!ok33 || !ok81 || v81 == 0u) {
        r = r3_substep_fail(kR3SecPre, 5, kPreSosAlive);
        bool nok = false;
        r3_substep_note(&r, reg_addr(p, r3gold::kMp0SmnC2pmsg33, &nok), v33, "C2PMSG_33");
        r3_substep_note(&r, reg_addr(p, r3gold::kMp0SmnC2pmsg81, &nok), v81, "C2PMSG_81");
        evf(p.env->ev, kTagPre,
            "FAIL: SOS not alive - %s; power-up STOPPED before any PSP write "
            "(loading SOS is not permitted)",
            (v33 & kPspC2pmsg33ReadyBit) ? "IFWI done but 81==0" : "IFWI not done (33 bit31=0)");
        return r;
    }
    evf(p.env->ev, kTagPre, "P3 PASS sos_alive (81 != 0), no write issued yet");
    return r;
}

// ---------------------------------------------------------------------------------------
// SEQ: S2..S9
// ---------------------------------------------------------------------------------------
R3SubstepResult r3_fail_seq(uint32_t step, const char *name)
{
    return r3_substep_fail(kR3SecSeq, step, name);
}

R3SubstepResult seq(Psp &p)
{
    R3EvidenceLines *ev = p.env->ev;
    const R3Grant *ring = r3_layout_grant(&p.plan, "psp_ring");
    const R3Grant *cmd = r3_layout_grant(&p.plan, "psp_cmd");
    const R3Grant *fence = r3_layout_grant(&p.plan, "psp_fence");
    const R3Grant *fw_buf = r3_layout_grant(&p.plan, "fw_buf");
    const R3Grant *tmr = r3_layout_grant(&p.plan, "tmr");

    p.fw_buf_bar0 = fw_buf->start;
    p.ring_mc = p.mc_base + ring->start;
    p.cmd_mc = p.mc_base + cmd->start;
    p.fence_mc = p.mc_base + fence->start;
    p.fw_mc = p.mc_base + fw_buf->start;

    evp(p.env, "psp-S2-zero");
    if (!zero_grant(p, ring, R3_PSP_RING_ZERO, "s2-ring-zero") ||
        !zero_grant(p, cmd, R3_PSP_CMD_ZERO, "s2-cmd-zero") ||
        !zero_grant(p, fence, R3_PSP_FENCE_ZERO, "s2-fence-zero")) {
        evf(ev, kTagSeq, "FAIL S2 zero-fill refused at +0x%llx/+0x%llx/+0x%llx",
            (unsigned long long)ring->start, (unsigned long long)cmd->start,
            (unsigned long long)fence->start);
        return r3_fail_seq(2, kSeqZero);
    }
    evf(ev, kTagSeq, "S2 zero ring=%llu cmd=%llu fence=%llu at +0x%llx/+0x%llx/+0x%llx",
        (unsigned long long)R3_PSP_RING_ZERO, (unsigned long long)R3_PSP_CMD_ZERO,
        (unsigned long long)R3_PSP_FENCE_ZERO, (unsigned long long)ring->start,
        (unsigned long long)cmd->start, (unsigned long long)fence->start);

    evp(p.env, "psp-S3-ring");
    // S3: ring create (psp_v13_0_4.c:255-286): wait ready, then 69/70/71/64.
    // Same two-stage budget as the response wait (Linux 100 ms, then the 2 s ceiling).
    uint32_t v = 0;
    int w = reg_poll(p, r3gold::kMp0SmnC2pmsg64, kPspMboxReadyMask, kPspMboxReadyFlag,
                     kPspReadyBudgetUs / kPspPollUs, kPspPollUs, &v);
    if (w == 0)
        w = reg_poll(p, r3gold::kMp0SmnC2pmsg64, kPspMboxReadyMask, kPspMboxReadyFlag,
                     (kPspReadyCeilingUs - kPspReadyBudgetUs) / kPspPollUs, kPspPollUs, &v);
    if (w != 1) {
        evf(ev, kTagSeq, "FAIL S3 C2PMSG_64 ready: %s (0x%08x)",
            w < 0 ? "read failed" : "timeout", v);
        R3SubstepResult f = r3_fail_seq(3, kSeqRingReady);
        bool nok = false;
        r3_substep_note(&f, reg_addr(p, r3gold::kMp0SmnC2pmsg64, &nok), v, "C2PMSG_64");
        return f;
    }
    const uint64_t ring_mc = p.ring_mc;
    const bool regs = reg_write(p, r3gold::kMp0SmnC2pmsg69, static_cast<uint32_t>(ring_mc),
                                kR3WState, "s3-ring-addr-lo") &&
                      reg_write(p, r3gold::kMp0SmnC2pmsg70,
                                static_cast<uint32_t>(ring_mc >> 32), kR3WState,
                                "s3-ring-addr-hi") &&
                      reg_write(p, r3gold::kMp0SmnC2pmsg71, kPspRingBytes, kR3WState,
                                "s3-ring-size");
    if (!regs) {
        evf(ev, kTagSeq, "FAIL S3 ring register write refused");
        return r3_fail_seq(3, kSeqRingWrite);
    }
    if (!reg_write(p, r3gold::kMp0SmnC2pmsg64, kPspC2pmsg64RingInitKm, kR3WState,
                   "s3-ring-init")) {
        evf(ev, kTagSeq, "FAIL S3 ring init write refused");
        return r3_fail_seq(3, kSeqRingWrite);
    }
    r3_io_mdelay(p.io, kPspRingCmdDelayMs);
    // Two-stage budget (tech plan §3.1): Linux's 100 ms first, then the remainder up to the
    // nullarc 2 s ceiling. The response check uses Linux's full mask
    // (MBOX_TOS_RESP_MASK = response bit | status bits), so a non-zero status is a timeout,
    // exactly as psp_wait_for() treats it; it is not recorded-and-continued like a firmware
    // load status (B-03 applies to LOAD_IP_FW, not to the ring handshake).
    w = reg_poll(p, r3gold::kMp0SmnC2pmsg64, kPspMboxReadyMask, kPspMboxReadyFlag,
                 kPspReadyBudgetUs / kPspPollUs, kPspPollUs, &v);
    if (w == 0)
        w = reg_poll(p, r3gold::kMp0SmnC2pmsg64, kPspMboxReadyMask, kPspMboxReadyFlag,
                     (kPspReadyCeilingUs - kPspReadyBudgetUs) / kPspPollUs, kPspPollUs, &v);
    if (w != 1) {
        evf(ev, kTagSeq, "FAIL S3 C2PMSG_64 response: %s (0x%08x)",
            w < 0 ? "read failed" : "timeout/status", v);
        R3SubstepResult f = r3_fail_seq(3, kSeqRingResp);
        bool nok = false;
        r3_substep_note(&f, reg_addr(p, r3gold::kMp0SmnC2pmsg64, &nok), v, "C2PMSG_64");
        return f;
    }
    // Defensive: the full-mask poll already rejects a non-zero status; keep the explicit
    // check so a future mask change cannot silently downgrade the handshake.
    const uint32_t ring_status = v & kPspMboxStatusMask;
    if (ring_status != 0u) {
        evf(ev, kTagSeq, "FAIL S3 ring create status=0x%08x (cmd=0x%05x)",
            ring_status, kPspC2pmsg64RingInitKm);
        R3SubstepResult f = r3_fail_seq(3, kSeqRingResp);
        bool nok = false;
        r3_substep_note(&f, reg_addr(p, r3gold::kMp0SmnC2pmsg64, &nok), v, "C2PMSG_64");
        return f;
    }
    // nullarc: after the ring is live, reset the doorbell so wptr agrees with slot 0. Without
    // this the first frame can land in a slot the PSP is not reading, and the PSP would consume
    // a zero frame (cmd address 0).
    if (!reg_write(p, r3gold::kMp0SmnC2pmsg67, 0u, kR3WProtocol, "s3-wptr-reset")) {
        evf(ev, kTagSeq, "FAIL S3 WPTR reset refused");
        return r3_fail_seq(3, kSeqRingWrite);
    }
    evf(ev, kTagSeq, "S3 ring create ok ring_mc=0x%llx size=0x%x C2PMSG_64=0x%08x",
        (unsigned long long)ring_mc, kPspRingBytes, v);
    evp(p.env, "psp-S3-ring-ok");

    // S4/S5: stage the TOC and the 17 pieces, then LOAD_TOC and read resp.tmr_size.
    evp(p.env, "psp-S4-stage");
    if (!parse_stage(p, p.fw_mc, fw_buf->size)) {
        evf(ev, kTagSeq, "FAIL S4 firmware staging: err=%d cap=%llu images=%llu",
            p.stage.err, (unsigned long long)fw_buf->size, (unsigned long long)p.mc_base);
        return r3_fail_seq(4, kSeqStage);
    }
    if (!stage_segment(p, fw_buf, p.stage.toc_mc, p.stage.toc_blob, p.stage.toc_off,
                       static_cast<uint32_t>(p.stage.toc_size), "s4-toc-stage")) {
        {
            const uint64_t b0 = p.stage.toc_mc - p.mc_base;
            const bool cls = r3_wg_classify(static_cast<uint32_t>(b0 >> 2), 2u, WClass::Data,
                                            "s4-toc-stage") == WVerdict::TxnRead;
            evf(ev, kTagSeq, "FAIL S4 TOC stage: bar0=0x%llx off=%u size=%u cls=%u",
                (unsigned long long)b0, p.stage.toc_off, (unsigned)p.stage.toc_size, cls ? 1u : 0u);
        }
        return r3_fail_seq(4, kSeqStage);
    }
    for (uint32_t i = 0; i < p.stage.count; i++) {
        const StageRow &row = p.stage.rows[i];
        if (!stage_segment(p, fw_buf, row.stage_mc, row.blob, row.seg->offset, row.seg->size,
                           "s4-fw-stage")) {
            evf(ev, kTagSeq, "FAIL S4 stage %s (fw_type %u) refused", row.entry->name,
                row.entry->fw_type);
            return r3_fail_seq(4, kSeqStage);
        }
    }
    evf(ev, kTagSeq, "S4 staged toc=%llu B + %u pieces = %llu B in fw_buf +0x%llx",
        (unsigned long long)p.stage.toc_size, p.stage.count,
        (unsigned long long)p.stage.total_bytes, (unsigned long long)fw_buf->start);

    // LOAD_TOC (0x20): {toc_phy_addr_lo, toc_phy_addr_hi, toc_size}
    uint8_t body[24] = {};
    put_u32(body, kLoadTocAddrLo, static_cast<uint32_t>(p.stage.toc_mc));
    put_u32(body, kLoadTocAddrHi, static_cast<uint32_t>(p.stage.toc_mc >> 32));
    put_u32(body, kLoadTocSize, static_cast<uint32_t>(p.stage.toc_size));
    uint32_t status = 0xFFFFFFFFu;
    evp(p.env, "psp-S5-toc");
    if (!post(p, kGfxCmdLoadToc, body, sizeof body, &status, "s5-load-toc")) {
        evf(ev, kTagSeq, "FAIL S5 LOAD_TOC submit/fence refused");
        return r3_fail_seq(5, kSeqTocCmd);
    }
    // B-03: a non-zero LOAD_TOC status is recorded by post() (status + command number) and
    // is NOT a substep failure; the firmware's real success status is not always zero
    // (amdgpu_psp.c:757-793). tmr_size below is read normally.
    // resp.tmr_size at response +16 (psp_gfx_if.h:453). `cmd` was already read back by post()
    // for the status word; only the size dword is needed here.
    uint64_t tmr_size_word = 0;
    if (!p.io->bus(p.io->ctx, R3_IO_BAR_VRAM, cmd->start + kCmdRespResp + kCmdRespTmrSize,
                   &tmr_size_word, false, 1u)) {
        evf(ev, kTagSeq, "FAIL S5 resp.tmr_size unreadable");
        return r3_fail_seq(5, kSeqTocFence);
    }
    p.tmr_size = static_cast<uint32_t>(tmr_size_word);
    evf(ev, kTagSeq, "S5 LOAD_TOC ok fence=%u us (measured=%u) tmr_size=0x%llx (%llu MiB)",
        p.fence_us, p.fence_measured ? 1u : 0u, (unsigned long long)p.tmr_size,
        (unsigned long long)(p.tmr_size >> 20));
    if (p.tmr_size == 0u) {
        evf(ev, kTagSeq, "FAIL S5 PSP returned tmr_size 0");
        return r3_fail_seq(5, kSeqTmrsize);
    }

    evp(p.env, "psp-S6-tmr");
    // S6 (D-08): the layout reserves a 64 MiB TMR slot; the PSP-returned size occupies a
    // sub-region of it (the remainder stays idle). R > 64 MiB cannot be placed and fails
    // closed with full evidence. Alignment is 1 MiB (Linux PSP_TMR_ALIGNMENT); the old
    // "natural alignment = tmr_size" and "plan.tmr_size == resp.tmr_size" rules are gone.
    if (tmr->start % kPspTmrAlignBytes != 0u || p.tmr_size > kPspTmrSlotBytes ||
        p.tmr_size > tmr->size || (p.tmr_size % kPspTmrSizeAlign) != 0u) {
        evf(ev, kTagSeq,
            "FAIL S6 TMR placement: returned=0x%llx slot=0x%llx start=0x%llx "
            "(limit=0x%llx align=0x%llx mc_base=0x%llx)",
            (unsigned long long)p.tmr_size, (unsigned long long)tmr->size,
            (unsigned long long)tmr->start, (unsigned long long)kPspTmrSlotBytes,
            (unsigned long long)kPspTmrAlignBytes, (unsigned long long)p.mc_base);
        R3SubstepResult f = r3_fail_seq(6, kSeqTmrPlace);
        r3_substep_note(&f, 0, static_cast<uint32_t>(p.tmr_size), "resp.tmr_size");
        r3_substep_note(&f, 0, static_cast<uint32_t>(tmr->size), "slot size");
        r3_substep_note(&f, 0, static_cast<uint32_t>(tmr->start), "tmr.start");
        return f;
    }
    p.tmr_mc = p.mc_base + tmr->start;
    // system_phy: recomputed here from THIS machine's FB_OFFSET (tech plan §4.4, T8). The
    // plan already holds it (r3_layout_system_phy(fb_phys_base, tmr.start)); it is never a
    // constant and never the nullarc value.
    p.tmr_sys_phy = p.plan.tmr_system_phy;
    evf(ev, kTagSeq,
        "S6 TMR ok start=0x%llx size=0x%llx slot=0x%llx mc=0x%llx sys_phy=0x%llx",
        (unsigned long long)tmr->start, (unsigned long long)p.tmr_size,
        (unsigned long long)tmr->size, (unsigned long long)p.tmr_mc,
        (unsigned long long)p.tmr_sys_phy);

    evp(p.env, "psp-S7-setup");
    // S7: SETUP_TMR (0x5).
    uint8_t tmr_body[24] = {};
    put_u32(tmr_body, kSetupTmrAddrLo, static_cast<uint32_t>(p.tmr_mc));
    put_u32(tmr_body, kSetupTmrAddrHi, static_cast<uint32_t>(p.tmr_mc >> 32));
    put_u32(tmr_body, kSetupTmrSize, static_cast<uint32_t>(p.tmr_size));
    put_u32(tmr_body, kSetupTmrFlags, kSetupTmrFlagVirtPhy);
    put_u32(tmr_body, kSetupTmrSysPhyLo, static_cast<uint32_t>(p.tmr_sys_phy));
    put_u32(tmr_body, kSetupTmrSysPhyHi, static_cast<uint32_t>(p.tmr_sys_phy >> 32));
    status = 0xFFFFFFFFu;
    if (!post(p, kGfxCmdSetupTmr, tmr_body, sizeof tmr_body, &status, "s7-setup-tmr")) {
        evf(ev, kTagSeq, "FAIL S7 SETUP_TMR submit/fence refused");
        return r3_fail_seq(7, kSeqTmrCmd);
    }
    // B-03: non-zero status is recorded in post() and continues; only a fence timeout fails.
    evf(ev, kTagSeq, "S7 SETUP_TMR ok size=0x%llx", (unsigned long long)p.tmr_size);

    // S8: 17x LOAD_IP_FW in table order, fw_size from the parsed segment (never a constant).
    for (uint32_t i = 0; i < p.stage.count; i++) {
        const StageRow &row = p.stage.rows[i];
        uint8_t fw_body[16] = {};
        put_u32(fw_body, kLoadIpFwAddrLo, static_cast<uint32_t>(row.stage_mc));
        put_u32(fw_body, kLoadIpFwAddrHi, static_cast<uint32_t>(row.stage_mc >> 32));
        put_u32(fw_body, kLoadIpFwSize, row.seg->size);
        put_u32(fw_body, kLoadIpFwType, row.entry->fw_type);
        status = 0xFFFFFFFFu;
        {
            char ptok[24];
            snprintf(ptok, sizeof ptok, "psp-S8-fw-%u", i);
            evp(p.env, ptok);
        }
        if (!post(p, kGfxCmdLoadIpFw, fw_body, sizeof fw_body, &status, "s8-load-ip-fw")) {
            evf(ev, kTagSeq, "FAIL S8 %s submit/fence refused", row.entry->name);
            return r3_fail_seq(8, kSeqIpFw);
        }
        evf(ev, kTagSeq, "S8 [%02u] fw_type=%3u %-10s size=%u @0x%llx status=0x%x", i,
            row.entry->fw_type, row.entry->name, row.seg->size,
            (unsigned long long)row.stage_mc, status);
        // B-03: a non-zero piece status is recorded by post() (status + command number) and
        // continues; the next piece still goes out in Linux order.
        // S9: AUTOLOAD_RLC IMMEDIATELY after RLC_G(8) (amdgpu_psp.c:3533-3541). Placed
        // inside the loop body, keyed on fw_type, exactly like Linux.
        if (row.entry->fw_type == kFwTypeRlcG) {
            status = 0xFFFFFFFFu;
            evp(p.env, "psp-S9-autoload");
            if (!post(p, kGfxCmdAutoloadRlc, nullptr, 0, &status, "s9-autoload-rlc")) {
                evf(ev, kTagSeq, "FAIL S9 AUTOLOAD_RLC submit/fence refused");
                return r3_fail_seq(9, kSeqAutoload);
            }
            // B-03: AUTOLOAD_RLC follows the same status rule as every other GPCOM command.
            evf(ev, kTagSeq, "S9 AUTOLOAD_RLC ok (after fw_type %u)", row.entry->fw_type);
        }
    }
    return r3_substep_pass();
}

// S12: only the PSP's own version is read here; RLC_F32/SDMA0 are observations in their
// own substeps (D3/§8.2).

// ---------------------------------------------------------------------------------------
// CHK: S10..S12
// ---------------------------------------------------------------------------------------
R3SubstepResult chk(Psp &p)
{
    R3EvidenceLines *ev = p.env->ev;

    // S10 (B-06): C2PMSG_33 bit31 is recorded as an observation, never a gate. Readiness is
    // carried by S3's preflight 81 != 0 (C0.4) and the submit/fence path; the PSP version is
    // recorded below (D3).
    bool ok = false;
    const uint32_t v33 = reg_read(p, r3gold::kMp0SmnC2pmsg33, &ok);
    evf(ev, kTagChk, "S10 C2PMSG_33=0x%08x%s (observation; not a ready gate)", v33,
        ok ? "" : " (unreadable)");

    // S11 (V4): the fence latency is an OBSERVATION (KPI), never a verdict. Hard failures
    // live in the submit path (fence timeout / non-zero response status). The control and
    // firmware-load maxima are separate; the KPI applies only to the control submits.
    evf(ev, kTagChk,
        "S11 fence_ctl=%u us cmd=0x%02x fence_fw=%u us cmd=0x%02x fence_all=%u us "
        "cmd=0x%02x measured=%u kpi=%u us (observation; control KPI only; "
        "timeout/status are the gates)",
        p.fence_ctl_us, p.fence_ctl_cmd, p.fence_fw_us, p.fence_fw_cmd, p.fence_us,
        p.fence_cmd, p.fence_measured ? 1u : 0u, kPspFenceMaxUs);

    // S12 (D3/§8.2): the PSP's own version is recorded pre/post; it is the SOS version,
    // so it is not a 0->nonzero load transition and it is NOT a gate. RLC_F32/SDMA0 are
    // observations in their own substeps.
    bool oks = false;
    const uint32_t psp_ver = reg_read(p, r3gold::kMp0SmnC2pmsg58, &oks);
    const uint32_t pre = p.env ? p.env->psp_version_pre : 0u;
    const bool pre_ok = p.env && p.env->psp_version_pre_ok;
    if (pre_ok) {
        evf(ev, kTagChk,
            "S12 psp_version pre=0x%08x post=0x%08x%s (record, not a gate)", pre,
            psp_ver, oks ? "" : " (unreadable)");
    } else {
        evf(ev, kTagChk,
            "S12 psp_version pre=---- post=0x%08x%s (record, not a gate)", psp_ver,
            oks ? "" : " (unreadable)");
    }
    return r3_substep_pass();
}

} // namespace

// ---------------------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------------------
R3SubstepResult psp_v13_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    // ~5 KiB of state (parsed images + 17 rows + the plan). It lives in static storage, not
    // on the 16 KiB kernel stack; the substep is serialised by the caller's lock (one power-up
    // at a time), so a static instance cannot be re-entered.
    static Psp p;
    memset(&p, 0, sizeof p);
    p.io = io;
    p.env = env;
    p.ip = env ? static_cast<const amdgpu::IPBaseTable *>(env->ip) : nullptr;
    p.frame = 1;

    if (!env || !env->rt) {
        R3SubstepResult r = r3_substep_fail(kR3SecPre, 0, kPreNoEnv);
        if (env && env->ev)
            evf(env->ev, kTagPre, "FAIL: no environment/runtime block");
        return r;
    }

    r3_wg_arm_substep(WSubstep::Psp);
    r3_wg_expect_writes(getPspRows(), getPspRowCount(), getPspLayouts(), getPspLayoutCount(), WSubstep::Psp);
    uint32_t declared[R3_WG_EXPECT_MAX];
    r3_wg_resolve_row_addrs(env->ip, getPspRows(), getPspRowCount(), declared);
    r3_wg_expect_declared_addrs(declared, getPspRowCount());
    R3SubstepResult r = pre(p);
    if (r.status == kR3SsecOk)
        r = seq(p);
    if (r.status == kR3SsecOk)
        r = chk(p);

    if (r.status != kR3SsecOk)
        r3_wg_set_path("failure");
    if (r3_wg_verify_and_clear(env, getPspRowCount())) {
        r = r3_substep_fail(kR3SecSeq, 0, "expect-write-mismatch");
    }
    r3_wg_disarm_substep();

    if (r.status == kR3SsecOk) {
        evp(env, "psp-ready");
        env->rt->ready_bits |= R3_IP_BIT(R3Ip::Psp);
        evf(env->ev, "psp", "ready: toc loaded, tmr=0x%llx (%llu MiB), 17 pieces loaded, "
                            "autoload done, fence_max=%u us (cmd=0x%02x), "
                            "psp_version recorded (pre/post in S12)",
            (unsigned long long)p.tmr_size, (unsigned long long)(p.tmr_size >> 20),
            p.fence_us, p.fence_cmd);
        // E2 "completion marker": the stage-1 equivalent of n48r3-Done, published as its own
        // property so a batch can be judged without parsing the log.
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-psp-done", "Yes", 3);
    } else {
        evp(env, "psp-fail");
        evf(env->ev, "psp", "FAIL %s (section %s step %u)", r.name,
            r3_substep_section_name(r.section), static_cast<unsigned>(r.step));
        if (env->sink && env->sink->set_property)
            env->sink->set_property(env->sink->user, "N48R3EVID-psp-done", "No", 2);
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// public staging plan (host-verifiable; the substep's own parse_stage fills the same rows)
// ---------------------------------------------------------------------------------------
PspStagePlan psp_stage_plan(const R3LayoutPlan *plan, uint64_t fb_mc_base)
{
    PspStagePlan out;
    memset(&out, 0, sizeof out);
    if (!plan || !plan->ok) {
        out.fail = "stage:grant";
        return out;
    }
    const R3Grant *g = r3_layout_grant(plan, "fw_buf");
    if (!g || g->owner != R3Owner::Psp || fb_mc_base == 0u || g->start + g->size > 0xFFFFFFFFull) {
        out.fail = "stage:grant";
        return out;
    }
    // T16/frame budget: Psp is ~6.5 KiB. This helper is host-test-only (no kernel caller)
    // and the host runners are single-threaded, so a static scratch keeps the 2 KiB frame
    // cap satisfiable without an exception list; the kernel substep has its own static Psp.
    static Psp p;
    memset(&p, 0, sizeof p);
    p.plan = *plan;
    if (!parse_stage(p, fb_mc_base + g->start, g->size)) {
        // Distinguish "the grant cannot hold the staged image" from "a blob would not parse":
        // the operator's next action differs (re-plan the layout vs. check the firmware).
        out.fail = p.stage.err == kStageErrOverCapacity ? "stage:space" : "stage:parse";
        out.err = p.stage.err;
        return out;
    }
    out.fw_buf_mc = fb_mc_base + g->start;
    out.fw_buf_bar0 = g->start;
    out.fw_buf_capacity = g->size;
    out.total_bytes = p.stage.total_bytes;
    out.toc.fw_type = 24u;   // GFX_FW_TYPE_TOC is NOT the load type; kept as a label only
    out.toc.name = p.stage.toc_img.segs[0].name;
    out.toc.size = p.stage.toc_size;
    out.toc.offset = p.stage.toc_off;
    out.toc.blob = p.stage.toc_blob;
    out.toc.blob_size = 0;   // not needed by consumers; the payload pointer is authoritative
    out.toc.mc = p.stage.toc_mc;
    out.toc.bar0 = p.stage.toc_mc - out.fw_buf_mc;
    out.fw_count = p.stage.count;
    for (uint32_t i = 0; i < p.stage.count; i++) {
        const StageRow &row = p.stage.rows[i];
        PspFwStage &e = out.fw[i];
        e.fw_type = row.entry->fw_type;
        e.name = row.entry->name;
        e.size = row.seg->size;
        e.offset = row.seg->offset;
        e.blob = row.blob;
        e.blob_size = row.blob_size;
        e.mc = row.stage_mc;
        e.bar0 = row.stage_mc - out.fw_buf_mc;
    }
    out.ok = true;
    return out;
}

/* Step2: declaration tables (rdna3_writeguard.h R3WriteRowReg/R3WriteRowLayout). */
namespace {
constexpr R3WriteRowLayout kPspLayouts[] = {
    {"psp_ring",  WSubstep::Psp, WNeed::Required},
    {"psp_cmd",   WSubstep::Psp, WNeed::Required},
    {"psp_fence", WSubstep::Psp, WNeed::Required},
    {"fw_buf",    WSubstep::Psp, WNeed::OnSuccessPath},
    {"tmr",       WSubstep::Psp, WNeed::OnSuccessPath},
};
constexpr R3WriteRowReg kPspRows[] = {
    {&r3gold::kMp0SmnC2pmsg64, static_cast<uint8_t>(IPBlock::MP0), 0, 0, 1, WClass::State,    WNeed::Required, "s3-ring-init"},
    {&r3gold::kMp0SmnC2pmsg69, static_cast<uint8_t>(IPBlock::MP0), 0, 0, 1, WClass::State,    WNeed::Required, "s3-ring-addr-lo"},
    {&r3gold::kMp0SmnC2pmsg70, static_cast<uint8_t>(IPBlock::MP0), 0, 0, 1, WClass::State,    WNeed::Required, "s3-ring-addr-hi"},
    {&r3gold::kMp0SmnC2pmsg71, static_cast<uint8_t>(IPBlock::MP0), 0, 0, 1, WClass::State,    WNeed::Required, "s3-ring-size"},
    {&r3gold::kMp0SmnC2pmsg67, static_cast<uint8_t>(IPBlock::MP0), 0, 0, 1, WClass::Protocol, WNeed::Required, "s3-wptr-reset"},
};
constexpr uint32_t kPspRowCount = sizeof(kPspRows) / sizeof(kPspRows[0]);
constexpr uint32_t kPspLayoutCount = sizeof(kPspLayouts) / sizeof(kPspLayouts[0]);
static_assert(kPspRowCount == 5, "psp declaration table must have 5 rows");
static_assert(kPspLayoutCount == 5, "psp layout declaration table must have 5 layouts");
} // anonymous namespace

const R3WriteRowReg* getPspRows(void) { return kPspRows; }
uint32_t getPspRowCount(void) { return kPspRowCount; }
const R3WriteRowLayout* getPspLayouts(void) { return kPspLayouts; }
uint32_t getPspLayoutCount(void) { return kPspLayoutCount; }

} // namespace amdgpu
