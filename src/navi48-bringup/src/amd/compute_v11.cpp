#include <stdarg.h>

#if defined(KERNEL)
#include <libkern/libkern.h> /* snprintf/vsnprintf/memcpy/memset, no libc under -nostdinc */
#else
#include <stdio.h>
#include <string.h>
#endif
/*
 * compute_v11.cpp - Compute 11.0 S13 substep (stage 3 T-E).
 *
 * Submits a PM4 chain to the MES-managed compute queue established by
 * mes_queue_v11_0_substep (S12).  The chain exercises:
 *
 *   SET_UCONFIG_REG   — minimal form: write SCRATCH_REG0 (3 dwords)
 *   WRITE_DATA        — write a magic dword to the test-buffer page (5 dwords)
 *   INDIRECT_BUFFER   — jump to a pre-loaded IB that contains RELEASE_MEM
 *   RELEASE_MEM       — (in IB) fence write, EOP seq=1
 *
 * After submission (doorbell), the substep polls the fence value
 * (< 1 ms, 100 µs stepping) and verifies:
 *   - SCRATCH_REG0 == magic        (SET_UCONFIG_REG worked)
 *   - fence == seq                 (RELEASE_MEM executed)
 *
 * Dependencies:
 *   - mes_queue_v11_0_substep (S12) makes the compute queue live.
 *   - gmc_v11_0_bind_sysmem (called in stage0 prep) back-fills the
 *     sysmem_grants so BAR0 CPU maps work.
 *   - Layout grant "mes" contains ring/WB/MQD/EOP (C5 layout).
- *   - Layout grant "test_buf" (D7) provides a 4096-byte page.
 *   - Layout grant "csib" (D4) holds the pre-loaded IB preamble.
 *
 * Register source: gc_11_0_0_offset.h, pm4_v11.h.
 * PM4 packet format: pm4_v11.h.
 */
#include "compute_v11.h"


#include "../rdna3/rdna3_integration.h"
#include "../rdna3/rdna3_layout.h"
#include "../rdna3/rdna3_substep.h"
#include "../rdna3/rdna3_writeguard.h"
#include "../rdna3/rdna3_ip_entries.h"
#include "mes_v11_0.h"
#include "pm4_v11.h"

/* ---- magic constants -------------------------------------------------- */

/* Magic value for SET_UCONFIG_REG / WRITE_DATA verification. */
constexpr uint32_t kComputeMagic    = 0xCAFEDEADu;

/* SCRATCH_REG0 window-relative offset (GC base[1]).
 * Full addr = GC_BASE[1] + 0x2040 = 0xA000 + 0x2040 = 0xC040.
 * SET_UCONFIG_REG_START = 0xC000 → window offset = 0x40. */
constexpr uint32_t kComputeScratchRegOff = 0x40u;

/* Test buffer sub-offset inside the "test_buf" grant (D7, 4096 bytes). */
constexpr uint32_t kComputeBufOff = 0u;

/* IB preamble (csib grant) holds exactly one RELEASE_MEM (8 dwords;
 * TYPE3 count=body-1 → packet3(op,6) carries 8 dwords, Linux emit_fence :6094). */
constexpr uint32_t kComputeIbDw = 8u;

/* Fence polling: 100 µs step, 2 s total. */
constexpr uint32_t kComputeFenceStepUs = 100u;
constexpr uint32_t kComputeFenceMaxUs  = 2u * 1000u * 1000u;
/* ---- logging helper (same pattern as mes_v11_0.cpp) ------------------- */

__attribute__((format(printf, 3, 4)))
static void evf(const R3SubstepEnv *env, const char *tag, const char *fmt, ...)
{
    if (!env || !env->ev)
        return;
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && static_cast<size_t>(n) >= sizeof(buf))
        n = static_cast<int>(sizeof(buf)) - 1;
    buf[sizeof(buf) - 1] = '\0';
    r3_ev_add(env->ev, tag, buf);
}

/* ---- helpers ---------------------------------------------------------- */

namespace {

/* Locate a layout grant by tag.  Returns null if not found. */
static const R3Grant* find_grant(const R3LayoutPlan &plan, const char *tag)
{
    for (uint32_t i = 0; i < plan.grant_count && i < R3_LAYOUT_MAX_GRANTS; i++) {
        if (strcmp(plan.grants[i].tag, tag) == 0) {
            return &plan.grants[i];
        }
    }
    return nullptr;
}

/* Write one dword to the carveout via the bus callback (BAR0 window). */
static bool bus_write32(const R3Io *io, uint64_t byte_off, uint32_t data)
{
    uint64_t v = data;
    return io->bus(io->ctx, R3_IO_BAR_VRAM, byte_off, &v, true, 1);
}

/* Write multiple dwords to the carveout via bus. */
static bool bus_write_multi(const R3Io *io, uint64_t byte_off,
                            const uint32_t *data, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!bus_write32(io, byte_off + i * 4u, data[i]))
            return false;
    }
    return true;
}

/* Read one dword from the carveout via the bus callback. */
static bool bus_read32(const R3Io *io, uint64_t byte_off, uint32_t *out)
{
    uint64_t v = 0;
    if (!io->bus(io->ctx, R3_IO_BAR_VRAM, byte_off, &v, false, 1))
        return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

/* Poll a 64-bit fence value at a carveout byte offset (WB page).
 * Returns true when the value matches `want` within the timeout. */
static bool poll_fence64(const R3Io *io, uint64_t byte_off,
                         uint64_t want, uint32_t timeout_us, uint32_t step_us)
{
    for (uint32_t elapsed = 0; elapsed < timeout_us; elapsed += step_us) {
        uint64_t v = 0;
        if (!io->bus(io->ctx, R3_IO_BAR_VRAM, byte_off, &v, false, 2))
            return false;
        if (v == want)
            return true;
        if (io->udelay)
            io->udelay(io->ctx, step_us);
        __sync_synchronize();
    }
    return false;
}

} /* namespace */

/* ---- substep entry ---------------------------------------------------- */

R3SubstepResult amdgpu::compute_v11_0_substep(const R3Io *io, const R3SubstepEnv *env)
{
    if (!io || !env || !env->rt) {
        return r3_substep_fail(kR3SecPre, 0, "null-io-or-env");
    }

    const R3LayoutPlan &plan = env->rt->layout;

    /* --- Locate grants --- */
    const R3Grant *mes_grant   = find_grant(plan, "mes");
    const R3Grant *test_grant  = find_grant(plan, "test_buf");
    const R3Grant *csib_grant  = find_grant(plan, "csib");

    if (!mes_grant || !test_grant || !csib_grant) {
        const char *missing = !mes_grant ? "mes" :
                              !test_grant ? "test_buf" : "csib";
        evf(env, "compute", "grant '%s' not found in layout", missing);
        return r3_substep_fail(kR3SecPre, 0, "missing-grant");
    }

    /* MC base of the framebuffer (for MC-aperture addresses). */
    const uint64_t fb_mc = env->fb_mc_base;

    /* Compute queue sub-offsets inside the mes grant (carveout byte offsets). */
    const uint64_t mes_off = mes_grant->start;
    const uint64_t cmp_ring_off = mes_off + amdgpu::kMesCmpRingOff;
    const uint64_t cmp_wb_off   = mes_off + amdgpu::kMesCmpWbOff;
    const uint64_t cmp_fence_off = mes_off + amdgpu::kMesCmpWbOff + amdgpu::kMesWbFenceOff;

    /* IB preamble (RELEASE_MEM) goes at csib grant start. */
    const uint64_t csib_off   = csib_grant->start;

    /* Test buffer offset (D7). */
    const uint64_t test_off  = test_grant->start + kComputeBufOff;

    /* Fence sequence number. */
    constexpr uint32_t kSeq = 1u;

    /* -- (A) Pre-load the IB preamble with RELEASE_MEM into csib grant ---
     * The IB contains exactly one RELEASE_MEM packet (8 dwords) that writes
     * the fence. We load it via the bus callback. */
    {
        uint32_t ib_pkt[8];
        const uint64_t fence_mc = fb_mc + cmp_fence_off;
        uint32_t n = amdgpu::pm4_release_mem(ib_pkt, fence_mc, kSeq);
        if (n != 8) {
            evf(env, "compute", "RELEASE_MEM builder failed (n=%u)", n);
            return r3_substep_fail(kR3SecSeq, 1, "ib-build-release-mem");
        }
        if (!bus_write_multi(io, csib_off, ib_pkt, n)) {
            evf(env, "compute", "csib write failed at off 0x%llx",
                static_cast<unsigned long long>(csib_off));
            return r3_substep_fail(kR3SecSeq, 1, "csib-bar0-write");
        }
    }

    /* -- (B) Build the compute ring PM4 chain --------------------------- */

    uint32_t wptr = 0;   /* dword offset into ring */
    constexpr uint32_t kMaxPkt = 8u;
    uint32_t pkt[kMaxPkt];

    /* (B1) SET_UCONFIG_REG: SCRATCH_REG0 ← magic.  3 dwords. */
    {
        uint32_t n = amdgpu::pm4_set_uconfig_reg(pkt, kComputeScratchRegOff, kComputeMagic);
        if (n != 3) {
            evf(env, "compute", "SET_UCONFIG_REG builder failed (n=%u)", n);
            return r3_substep_fail(kR3SecSeq, 2, "pkt-set-uconfig");
        }
        if (!bus_write_multi(io, cmp_ring_off + wptr * 4u, pkt, n)) {
            evf(env, "compute", "ring write failed at dw %u (SET_UCONFIG)", wptr);
            return r3_substep_fail(kR3SecSeq, 2, "ring-write");
        }
        wptr += n;
    }

    /* (B2) WRITE_DATA: write magic to test buffer.  5 dwords.
     * Uses the test buffer's VM address through the compute queue's page tables
     * (vmid0, page_table_base set by mes_queue_v11_0_substep from D2 root GPA). */
    {
        /* VM address of test buffer: fb_mc + test_off (through vmid0 aperture). */
        const uint64_t test_va = fb_mc + test_off;
        uint32_t n = amdgpu::pm4_write_data(pkt, test_va, kComputeMagic);
        if (n != 5) {
            evf(env, "compute", "WRITE_DATA builder failed (n=%u)", n);
            return r3_substep_fail(kR3SecSeq, 3, "pkt-write-data");
        }
        if (!bus_write_multi(io, cmp_ring_off + wptr * 4u, pkt, n)) {
            evf(env, "compute", "ring write failed at dw %u (WRITE_DATA)", wptr);
            return r3_substep_fail(kR3SecSeq, 3, "ring-write");
        }
        wptr += n;
    }

    /* (B3) INDIRECT_BUFFER: jump to csib preamble (RELEASE_MEM).  4 dwords.
     * Uses the csib grant's VM address.  vmid=0, compute=true. */
    {
        const uint64_t ib_va = fb_mc + csib_off;
        uint32_t n = amdgpu::pm4_indirect_buffer(pkt, ib_va, kComputeIbDw, 0, true);
        if (n != 4) {
            evf(env, "compute", "INDIRECT_BUFFER builder failed (n=%u)", n);
            return r3_substep_fail(kR3SecSeq, 4, "pkt-indirect-buffer");
        }
        if (!bus_write_multi(io, cmp_ring_off + wptr * 4u, pkt, n)) {
            evf(env, "compute", "ring write failed at dw %u (IB)", wptr);
            return r3_substep_fail(kR3SecSeq, 4, "ring-write");
        }
        wptr += n;
    }

    /* -- (C) Submit (doorbell) ------------------------------------------ */
    {
        bool submitted = amdgpu::pm4_submit_tail(io, cmp_ring_off, cmp_wb_off,
                                         amdgpu::kMesDoorbellCompute,
                                         static_cast<uint64_t>(wptr));
        if (!submitted) {
            evf(env, "compute", "pm4_submit_tail failed");
            return r3_substep_fail(kR3SecSeq, 5, "submit-tail-fail");
        }
        evf(env, "compute", "submitted %u dwords via doorbell[%u]",
            wptr, amdgpu::kMesDoorbellCompute);
    }

    /* -- (D) Poll fence (< 1 ms expected) ------------------------------- */
    {
        bool got = poll_fence64(io, cmp_fence_off,
                                static_cast<uint64_t>(kSeq),
                                kComputeFenceMaxUs, kComputeFenceStepUs);
        if (!got) {
            evf(env, "compute", "fence timeout (seq=%u, %u us)",
                kSeq, kComputeFenceMaxUs);
            return r3_substep_fail(kR3SecChk, 6, "fence-timeout");
        }
        evf(env, "compute", "fence matched seq=%u", kSeq);
    }

    /* -- (E) Verify SET_UCONFIG_REG: read SCRATCH_REG0 via MMIO --------- */
    {
        const uint32_t scratch_addr = 0x2040u;   /* GC base[0] */
        bool ok = false;
        const uint32_t val = r3_io_read32(io, scratch_addr, &ok);
        if (!ok) {
            evf(env, "compute", "SCRATCH_REG0 read failed");
            return r3_substep_fail(kR3SecChk, 7, "scratch-read-fail");
        }
        if (val != kComputeMagic) {
            evf(env, "compute", "SCRATCH_REG0 = 0x%x (want 0x%x)",
                val, kComputeMagic);
            return r3_substep_fail(kR3SecChk, 7, "scratch-mismatch");
        }
        evf(env, "compute", "SCRATCH_REG0 = 0x%x (SET_UCONFIG_REG OK)", val);
    }

    /* -- (F) Verify WRITE_DATA: read test buffer via bus ---------------- */
    {
        uint32_t val = 0;
        if (!bus_read32(io, test_off, &val)) {
            evf(env, "compute", "test buffer read failed (off 0x%llx)",
                static_cast<unsigned long long>(test_off));
            return r3_substep_fail(kR3SecChk, 8, "buf-read-fail");
        }
        if (val != kComputeMagic) {
            evf(env, "compute", "test buffer = 0x%x (want 0x%x)",
                val, kComputeMagic);
            return r3_substep_fail(kR3SecChk, 8, "buf-mismatch");
        }
        evf(env, "compute", "test buffer = 0x%x (WRITE_DATA + IB chain OK)", val);
    }

    evf(env, "compute", "S13 compute chain: PASS");
    return r3_substep_pass();
}
/* ---- ABI readiness substep (stage 12, runs before compute S13) --------- */

namespace amdgpu {
R3SubstepResult abi_v11_0_substep(const R3Io *io, const R3SubstepEnv *env);
} /* namespace amdgpu */

R3SubstepResult amdgpu::abi_v11_0_substep(const R3Io *io,
                                            const R3SubstepEnv *env)
{
    (void)io;
    if (!env || !env->rt)
        return r3_substep_fail(kR3SecPre, 0, "no-env");
    /* The ABI (user-client selectors BO/submit/fence) is always available
     * once the queue (S11) and its doorbell (S10) are operational.  This
     * substep only validates that the runtime layout is in place. */
    if (!env->rt->layout.ok)
        return r3_substep_fail(kR3SecPre, 0, "layout-not-ok");
    return r3_substep_pass();
}
