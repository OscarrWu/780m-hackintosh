// NaviR3Bringup.cpp - the RDNA3 (Phoenix 780M) stage-2 kext entry class.
//
// D-01: start() maps BAR5, confirms D0, runs the read-only survey + preflight ladder
// (stages 0-1) and publishes "armed, not fired" evidence. The five power-up substeps
// (stages 2-6) are triggered separately via the user client selector or n48r3-auto=1.
// All BAR5/SMN access goes through R3Io; all writes through rdna3_writeguard.
#include "NaviR3Bringup.hpp"
#include "NaviR3UserClient.hpp"
#include "rdna3_writeguard.h"

#include "common/BootArgFamily.h"
#include "common/StageRunner.h"
#include "rdna3_integration.h"
#include "rdna3_wmanifest.h"
#include "rdna3_teardown.h"
#include "rdna3_layout.h"
#include "amd/gmc_v11_0.h"
#include "amd/mes_v11_0.h"
#include "amd/pm4_v11.h"
#include "amd/amdgpu_sysmem.h"

#include <IOKit/IOLocks.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

// Stage-0 depth selector, implemented by the survey ladder (rdna3_stages.cpp). Declared
// here with the contract signature; the ladder header may also declare it.
void r3_stage0_set_depth(uint32_t depth);

// build 0.0.540 (amd/n48log.cpp:9-10): perf540 switch and note hook are global C++ symbols
// (declared before namespace amdgpu). The base defines them in src/Navi48Bringup.cpp:715/772,
// which is absent from the rdna3 set; GATE_OURS (Makefile.linux:247) flags the two as a
// regression guard (detection only; the stand-in below provides the real definition).
// Without stand-ins the linker permits undefined symbols (kext policy, see Makefile.linux:255-259)
// and the kext silently fails to load. Match the base OFF path: gN48Pf540On reads 0 once
// in n48_logf (n48log.cpp:36), so no clock is read and the hook is never called; rdna3 has
// no `gfxneuter 96` writer. Log lines still reach IOLog and the ring unconditionally
// (n48log.cpp:49,55-58). Never link src/Navi48Bringup.cpp into rdna3: it would double-define
// both symbols.
volatile uint32_t gN48Pf540On { 0u };
void navi48_pf540_log_note(uint64_t, uint64_t, uint64_t, uint64_t) {}

OSDefineMetaClassAndStructors(NaviR3Bringup, IOService)

IOService *NaviR3Bringup::probe(IOService *provider, SInt32 *score) {
    // Load gate: nothing happens unless the operator opted in (requirement F1 / V6).
    uint32_t gate = 0;
    if (!tgt_boot_arg_u32("bringup", &gate) || gate == 0) return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    const uint32_t id = pci->configRead32(kIOPCIConfigVendorID);
    if ((id & 0xffffu) != 0x1002u || (id >> 16) != 0x15bfu) {
        IOLog("N48R3LOG probe: %s id=0x%08x (want 0x1002:0x15bf)\n",
              id == 0xffffffffu ? "config read failed" : "device id mismatch", id);
        return nullptr;
    }

    IOService *svc = super::probe(provider, score);
    if (!svc) return nullptr;
    IOLog("N48R3LOG probe: matched 0x15bf (n48r3bringup=%u)\n", gate);
    return this;
}

bool NaviR3Bringup::mapBar5() {
    IODeviceMemory *bar = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (!bar) {
        IOLog("N48R3LOG mmio: BAR5 not present/assigned\n");
        return false;
    }
    bar5Map_ = bar->map();
    if (!bar5Map_) {
        IOLog("N48R3LOG mmio: BAR5 map failed\n");
        return false;
    }
    dev_.rmmio = reinterpret_cast<volatile uint32_t *>(bar5Map_->getVirtualAddress());
    dev_.rmmioSize = bar5Map_->getLength();
    if (!dev_.rmmio || dev_.rmmioSize < sizeof(uint32_t)) {
        IOLog("N48R3LOG mmio: BAR5 empty\n");
        return false;
    }
    IOLog("N48R3LOG mmio: BAR5 mapped %llu bytes\n", (unsigned long long)dev_.rmmioSize);
    return true;
}

bool NaviR3Bringup::mapCarveout() {
    IODeviceMemory *bar = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar) {
        IOLog("N48R3LOG mmio: BAR0 not present/assigned (stage-2 carveout unreachable)\n");
        return false;
    }
    bar0Map_ = bar->map();
    if (!bar0Map_) {
        IOLog("N48R3LOG mmio: BAR0 map failed\n");
        return false;
    }
    dev_.bar0 = reinterpret_cast<volatile uint8_t *>(bar0Map_->getVirtualAddress());
    dev_.bar0Size = bar0Map_->getLength();
    dev_.bar0Phys = bar->getPhysicalAddress();
    if (!dev_.bar0 || dev_.bar0Size == 0) {
        IOLog("N48R3LOG mmio: BAR0 empty\n");
        unmapBar0();
        return false;
    }
    IOLog("N48R3LOG mmio: BAR0 mapped %llu bytes (phys=0x%llx)\n",
          (unsigned long long)dev_.bar0Size, (unsigned long long)dev_.bar0Phys);
    return true;
}

bool NaviR3Bringup::mapDoorbells() {
    IODeviceMemory *bar = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!bar) {
        // A missing BAR2 is not fatal for a stage-1-only boot: the MES substep fails closed
        // on its own PRE check. Record it so the evidence says why.
        IOLog("N48R3LOG mmio: BAR2 (doorbells) not present; MES will fail closed\n");
        return false;
    }
    bar2Map_ = bar->map();
    if (!bar2Map_) {
        IOLog("N48R3LOG mmio: BAR2 map failed\n");
        return false;
    }
    dev_.bar2 = reinterpret_cast<volatile uint8_t *>(bar2Map_->getVirtualAddress());
    dev_.bar2Size = bar2Map_->getLength();
    dev_.bar2Phys = bar->getPhysicalAddress();
    IOLog("N48R3LOG mmio: BAR2 mapped %llu bytes (phys=0x%llx)\n",
          (unsigned long long)dev_.bar2Size, (unsigned long long)dev_.bar2Phys);
    return dev_.bar2 != nullptr && dev_.bar2Size != 0;
}

void NaviR3Bringup::unmapBar0() {
    dev_.bar0 = nullptr;
    dev_.bar0Size = 0;
    if (bar0Map_) {
        bar0Map_->release();
        bar0Map_ = nullptr;
    }
}

void NaviR3Bringup::unmapBar2() {
    dev_.bar2 = nullptr;
    dev_.bar2Size = 0;
    if (bar2Map_) {
        bar2Map_->release();
        bar2Map_ = nullptr;
    }
}

// --- R3Io adapter -----------------------------------------------------------------------
// This is the ONLY place a substep's I/O leaves the driver. Address arithmetic is delegated
// to rdna3_integration's pure range helpers so the host test pins the same translation.
bool NaviR3Bringup::ioMmio(void *ctx, uint32_t dword, uint32_t *value, bool write) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (!self || !value) return false;
    if (!r3_io_mmio_in_range(&self->integ_.window, dword)) return false;
    volatile uint32_t *p = self->dev_.rmmio + dword;
    if (write) {
        *p = *value;
    } else {
        *value = *p;
        // B5: every successful stage-2 read lands in the read manifest (the survey notes
        // its own reads through the same type; capacity is bounded by the fixed read set).
        r3_manifest_note(&self->rd_manifest_, dword);
    }
    return true;
}

bool NaviR3Bringup::ioSmuio(void *ctx, uint32_t index, uint32_t *value, bool write) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (!self || !value) return false;
    // The caller holds the lock (R3Io contract). The index/dat pair is the NBIO SMN window.
    const uint32_t idx = amdgpu::NBIORegs::BIF_BX0_PCIE_INDEX2;
    const uint32_t dat = amdgpu::NBIORegs::BIF_BX0_PCIE_DATA2;
    if (!r3_io_mmio_in_range(&self->integ_.window, idx) ||
        !r3_io_mmio_in_range(&self->integ_.window, dat))
        return false;
    // A-02 (requirement C0.8②): write the SMN BYTE address directly to PCIE_INDEX2.
    // Linux amdgpu_reg_access.c passes the byte address unchanged; the previous >>2
    // conversion accessed the wrong register. The readback below is the barrier.
    self->dev_.rmmio[idx] = index;
    (void)self->dev_.rmmio[idx];
    if (write) {
        self->dev_.rmmio[dat] = *value;
        (void)self->dev_.rmmio[dat];
    } else {
        *value = self->dev_.rmmio[dat];
        // A-02: the manifest key uses the SMN byte address (same space as the value
        // written to PCIE_INDEX2). The N48R3EVID-read-manifest-smn property is exported
        // separately from the BAR5 read-manifest; the two spaces are never confused.
        r3_manifest_note(&self->rd_smn_, index);
    }
    return true;
}

bool NaviR3Bringup::ioBus(void *ctx, uint32_t bar, uint64_t byte_off, uint64_t *value,
                          bool write, uint32_t dwords) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (!self || !value) return false;
    if (!r3_io_bus_in_range(&self->integ_.window, bar, byte_off, dwords)) return false;
    volatile uint8_t *base = bar == 2u ? self->dev_.bar2 : self->dev_.bar0;
    if (!base) return false;
    volatile uint8_t *p = base + byte_off;
    if (write) {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(value);
        for (uint32_t i = 0; i < dwords; i++)
            reinterpret_cast<volatile uint32_t *>(p)[i] = src[i];
    } else {
        uint32_t *dst = reinterpret_cast<uint32_t *>(value);
        for (uint32_t i = 0; i < dwords; i++)
            dst[i] = reinterpret_cast<volatile uint32_t *>(p)[i];
        // BAR0 carveout reads are noted in the same manifest with the dword index the
        // address maps to (byte offset >> 2); they are a distinct space, so they can never
        // collide with a BAR5 dword (the layout keeps BAR0 offsets in the low GiB).
        r3_manifest_note(&self->rd_manifest_,
                         static_cast<uint32_t>(byte_off >> 2));
    }
    return true;
}

void NaviR3Bringup::ioUdelay(void *, uint32_t us) { IODelay(us); }
// Other problem 3: ioMdelay must be a busy-wait (Linux mdelay semantics), not IOSleep
// (which yields the CPU). The name promises busy-wait; IOSleep was name-mismatched.
// Segmented IODelay(1000) per millisecond keeps the semantics without a single long spin.
void NaviR3Bringup::ioMdelay(void *ctx, uint32_t ms) {
    (void)ctx;
    for (uint32_t i = 0; i < ms; i++)
        IODelay(1000);
}
void NaviR3Bringup::ioFence(void *ctx) {
    (void)ctx;
    // Contract ⑥: flush the CPU write buffer so staged stores are globally visible
    // before a doorbell tells firmware to consume them. x86 sfence semantics.
    __builtin_ia32_sfence();
}

// Contract ⑦ (v1.7): device visibility. Linux amdgpu_device_flush_hdp and nullarc hdpFlush
// both write 0 to the NBIO-remapped HDP memory-flush window and read a register back. The
// window is discovered read-only in prepare; its writeguard row is shared, so any substep
// may use it. Returns false on no window / denied write / failed MMIO: no doorbell may ring.
bool NaviR3Bringup::ioHdpFlush(void *ctx) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (!self || !self->integ_.env.hdp_flush_valid) return false;
    const uint32_t d = self->integ_.env.hdp_flush_dword;
    if (r3_wg_classify(d, 1u, WClass::Protocol, "hdp-flush") != WVerdict::TxnRead)
        return false;
    uint32_t zero = 0;
    if (!NaviR3Bringup::ioMmio(ctx, d, &zero, true)) return false;
    uint32_t rb = 0;
    (void)NaviR3Bringup::ioMmio(ctx, d, &rb, false);  // read-back barrier
    return true;
}
uint64_t NaviR3Bringup::ioNowUs(void *) { return mach_absolute_time() / 1000ull; }

void NaviR3Bringup::ioLock(void *ctx) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (self && self->ioLock_) IOLockLock(self->ioLock_);
}
void NaviR3Bringup::ioUnlock(void *ctx) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(ctx);
    if (self && self->ioLock_) IOLockUnlock(self->ioLock_);
}

void NaviR3Bringup::bindIntegrationIo() {
    r3_manifest_init(&rd_manifest_);
    r3_manifest_init(&rd_smn_);
    // D-06/B1: every writeguard DENY gets its own N48R3LOG line.
    r3_wg_set_log(&NaviR3Bringup::wgLogThunk, this);
    integ_.io.ctx = this;
    integ_.io.mmio = &NaviR3Bringup::ioMmio;
    integ_.io.smuio = &NaviR3Bringup::ioSmuio;
    integ_.io.bus = &NaviR3Bringup::ioBus;
    integ_.io.udelay = &NaviR3Bringup::ioUdelay;
    integ_.io.mdelay = &NaviR3Bringup::ioMdelay;
    integ_.io.now_us = &NaviR3Bringup::ioNowUs;
    integ_.io.lock = &NaviR3Bringup::ioLock;
    integ_.io.unlock = &NaviR3Bringup::ioUnlock;
    integ_.io.fence = &NaviR3Bringup::ioFence;
    integ_.io.hdp_flush = &NaviR3Bringup::ioHdpFlush;
    integ_.window.bar5_bytes = dev_.rmmioSize;
    integ_.window.bar0_bytes = dev_.bar0Size;
    integ_.window.bar2_bytes = dev_.bar2Size;
}

void NaviR3Bringup::onStageStop(StageCtx &ctx, uint32_t index, StageResult result) {
    (void)index;
    (void)result;
    R3IntegrationCtx *ic = static_cast<R3IntegrationCtx *>(ctx.substep);
    NaviR3Bringup *self = ic ? static_cast<NaviR3Bringup *>(ic->owner) : nullptr;
    if (!self || !ic) return;
    // K9 order: dump already ran in the dump hook; latch is owned by StageRunner; here the
    // reverse teardown of the IPs that actually started.
    R3TeardownEnv te {};
    te.io = &ic->io;
    te.sub = &ic->env;
    te.failed = ic->failed;
    te.failed_ip_valid = ic->failed_ip_valid;
    te.failed_ip = ic->failed_ip;
    const R3TeardownReport rep = r3_teardown_run(&te);
    self->teardown_done_ = true;
    char line[192];
    snprintf(line, sizeof line,
             "N48R3LOG teardown: triggered=%d acted=0x%x none=0x%x writes=%u failed=0x%x "
             "ih_fail=%u",
             rep.triggered ? 1 : 0, rep.acted, rep.none, rep.writes, rep.write_fail,
             rep.ih_fail);
    IOLog("%s\n", line);
}

void NaviR3Bringup::unmapBar5() {
    dev_.rmmio = nullptr;
    dev_.rmmioSize = 0;
    if (bar5Map_) {
        bar5Map_->release();
        bar5Map_ = nullptr;
    }
}

bool NaviR3Bringup::confirmD0(bool *measured) {
    // Config/UMA reads are only valid in D0. The driver runs after probe, so the device should be
    // powered; verify through IOPowerManagement when it is published rather than writing the PCI
    // command register (stage-1 zero-write rule). *measured stays false when the power state was
    // not actually read, so an inference is never reported as a measurement (D-03 / I4).
    if (measured) *measured = false;
    OSDictionary *pm = OSDynamicCast(OSDictionary, pci_->getProperty("IOPowerManagement"));
    if (!pm) {
        IOLog("N48R3LOG d0: IOPowerManagement absent; assuming D0 (assumed, not measured)\n");
        return true;
    }
    OSNumber *state = OSDynamicCast(OSNumber, pm->getObject("DevicePowerState"));
    if (!state) {
        IOLog("N48R3LOG d0: DevicePowerState absent; assuming D0 (assumed, not measured)\n");
        return true;
    }
    const uint32_t v = state->unsigned32BitValue();
    if (measured) *measured = true;
    IOLog("N48R3LOG d0: DevicePowerState=%u (%s, measured)\n", v, v == 0 ? "D0" : "NOT D0");
    return v == 0;
}

// T-E: the GMC sysmem allocator (the gmc_v11_0.h:120-123 contract): the integration layer
// binds one allocator per driver instance before the stage ladder runs; host tests bind a
// fake. The callback's SysMem is a local: on return the IOBufferMemoryDescriptor pointer is
// dropped ON PURPOSE -- the bring-up page tables live for the whole kext lifetime, the same
// "never unbound and never freed" pattern as fastcopy's staging buffer
// (Navi48Bringup.cpp:2502/2915). SysMem has no destructor (amdgpu_sysmem.h:15-21) and the
// descriptor's refcount stays 1, so nothing releases it early; a failed sysmem_alloc
// releases internally (amdgpu_sysmem.cpp:18,22) and the callback reports 0 (fail closed).
static uint64_t r3_sysmem_alloc_cb(void *ctx, uint64_t bytes, void **cpu) {
    (void)ctx;
    amdgpu::SysMem m {};
    if (amdgpu::sysmem_alloc(m, bytes, 4096) != KERN_SUCCESS || !m.valid()) return 0;
    *cpu = m.cpu;
    return m.bus;
}
static const amdgpu::GmcSysmemAlloc g_r3_sysmem_alloc = { nullptr, r3_sysmem_alloc_cb };

bool NaviR3Bringup::start(IOService *provider) {
    if (!super::start(provider)) return false;

    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pci_) return false;

    // Access enable comes before the BAR5 map (base Navi48Bringup.cpp:7358 order): the
    // function must decode memory space before any MMIO read. Both command-register
    // snapshots go to the log so the transition is observable/evidenced on hardware.
    const uint32_t cmd_before = pci_->configRead32(kIOPCIConfigCommand);
    const bool mem_enabled = pci_->setMemoryEnable(true);
    const uint32_t cmd_after = pci_->configRead32(kIOPCIConfigCommand);
    IOLog("N48R3LOG access: PCI command before=0x%08x after=0x%08x "
          "(setMemoryEnable=%d, mem_space=%d)\n",
          cmd_before, cmd_after, mem_enabled ? 1 : 0, (cmd_after & 0x0002u) ? 1 : 0);

    if (!mapBar5()) return false;

    const uint32_t id = pci_->configRead32(kIOPCIConfigVendorID);
    const uint32_t rev = pci_->configRead32(kIOPCIConfigRevisionID);
    state_.hw.dev = &dev_;
    state_.hw.disc = &disc_;
    state_.hw.dcn = &dcn_;
    state_.hw.dcn_ready = false;
    state_.hw.pci_device = (uint16_t)(id >> 16);
    state_.hw.pci_revision = (uint8_t)(rev & 0xffu);
    state_.hw.d0_confirmed = confirmD0(&state_.hw.d0_measured);

    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    state_.hw.bar0_size = bar0 ? bar0->getLength() : 0;
    state_.hw.bar0_base = bar0 ? bar0->getPhysicalAddress() : 0;

    state_.sink.set_property = &NaviR3Bringup::setPropertyThunk;
    state_.sink.user = this;

    // Stage-2 maps: only when the operator asked for a ladder that can write. Stage 0 alone
    // stays the stage-1 read-only path (no BAR0/BAR2 map, no whitelist arm).
    uint32_t stage = 0;
    (void)tgt_boot_arg_u32("stage", &stage);
    if (stage >= 1u) {
        if (!mapCarveout()) {
            IOLog("N48R3LOG mmio: stage>=1 requires BAR0; ladder will fail closed\n");
        }
        (void)mapDoorbells();
        if (!ioLock_) ioLock_ = IOLockAlloc();
    }

    // The integration context behind StageCtx::substep. Stage 0 still sees state_ via
    // StageCtx::user; the substeps see integ_.env (rt/ev/sink/ip/fb_mc_base).
    integ_.failed_ip_valid = false;
    integ_.owner = this;
    integ_.stage = stage;
    integ_.sink.set_property = &NaviR3Bringup::setPropertyThunk;
    integ_.sink.user = this;
    r3_ev_init(&integ_.ev);
    integ_.env.rt = &integ_.rt;
    integ_.env.ev = &integ_.ev;
    integ_.env.sink = &integ_.sink;
    integ_.env.fb_mc_base = dev_.vramMcBase;
    integ_.env.ip = &dev_.ip;
    bindIntegrationIo();
    // The trigger may only fire when every substep callback exists. A missing callback used
    // to surface as "pre:no-io" on first use (2026-11-05 R1, missing unlock); fold the
    // completeness check into the armed gate so the boot status tells the truth.
    const bool io_ok = r3_io_complete(&integ_.io);

    // Kill switches: absent = follow the ladder (K0 three-state). Only the five leaves.
    integ_.kill_mask = 0u;
    for (uint32_t i = 0; i < R3_IP_COUNT; i++) {
        static const char *const leaves[R3_IP_COUNT] = {"psp", "smu", "gfx", "sdma", "mes"};
        if (r3_kill_state(leaves[i]) == kR3KillOff)
            integ_.kill_mask |= (1u << i);
    }

    StageCtx ctx;
    ctx.log = &NaviR3Bringup::logThunk;
    ctx.dump_failure_state = &r3_dump_failure_state;
    ctx.user = &state_;
    ctx.substep = &integ_;

    const R3DepthSel depth = r3_depth_select();
    uint32_t depth_source = kR3DepthAbsent;
    uint32_t depth_requested = 0u;
    if (depth.present) {
        if (depth.raw == depth.depth) {
            depth_source = kR3DepthExplicit;
            depth_requested = depth.depth;
        } else {
            depth_source = kR3DepthInvalid;
            depth_requested = depth.raw;  // keep what the operator wrote for the evidence
        }
    }
    // C0.9: the knob is a breadth preference; the stage floor is the requirement. The plan
    // records requested/effective/floor so a raise is never silent.
    state_.plan = r3_stage_depth_plan(stage, depth_source, depth_requested);
    if (depth_source == kR3DepthInvalid) {
        IOLog("N48R3LOG depth: n48r3-depth=%u out of range (0..5); using floor T%u\n",
              depth.raw, state_.plan.effective + 1u);
    } else {
        IOLog("N48R3LOG depth: requested=%u effective=%u floor=%u reason=%s\n",
              state_.plan.requested, state_.plan.effective, state_.plan.floor,
              state_.plan.reason);
    }
    r3_stage0_set_depth(state_.plan.effective);

    StageHooks hooks;
    hooks.on_stage_start = nullptr;
    hooks.on_stage_stop = &NaviR3Bringup::onStageStop;

    const StageTable &table = r3_stage_table();
    integ_.effective_stage = stage < table.count ? stage : table.count - 1u;
    // D-01: start() runs only survey + preflight (stages 0-1). The five power-up substeps
    // are triggered separately via the user client or n48r3-auto=1. This prevents an
    // automatic power-up that, if it crashes, would reboot on every boot.
    // T-E: bind the GMC sysmem allocator before the ladder runs (rdna3_survey.h:602).
    // Without it prepare_stage2 fails closed with prep:no-sysmem-alloc.
    r3_stages_set_sysmem_allocator(&g_r3_sysmem_alloc);
    const StageResult r = bringup_r3(1u, table, ctx, &hooks);
    state_.gate_ok = (r == kStageOk) && io_ok;
    if (!io_ok) {
        IOLog("N48R3LOG start: R3Io incomplete (power-up will be refused)\n");
    }
    if (r != kStageOk) {
        IOLog("N48R3LOG start: survey/preflight stopped (result %d); partial evidence kept\n", (int)r);
    }

    exportArmedEvidence();

    // D-01/F12: auto mode triggers the power-up immediately after preflight.
    // INV-D5: with the phase gate closed (R3_AUTO_PHASE_OPEN=0) a requested auto
    // trigger is refused by name, never silently ignored.
    bool auto_requested = false;
    const bool auto_open = r3_auto_mode(&auto_requested);
    if (r == kStageOk && auto_open) {
        IOLog("N48R3LOG start: n48r3-auto=1; triggering power-up substeps\n");
        (void)triggerPowerUp();
    } else if (auto_requested && !auto_open) {
        IOLog("N48R3LOG start: n48r3-auto=1 refused (R3_AUTO_PHASE_OPEN=0, development phase)\n");
    }

    started_ = true;
    registerService();
    return true;
}

void NaviR3Bringup::exportArmedEvidence() {
    // D-01: the "armed, not fired" status — exported after survey + preflight so the
    // operator can see the GPU is ready for a manual trigger.
    const R3GateInputs g = r3_integration_gate_inputs(&integ_);
    char buf[1024];
    const size_t en = r3_integration_format_enabled(&g, buf, sizeof buf);
    if (en > 0 && state_.sink.set_property)
        state_.sink.set_property(state_.sink.user, "N48R3EVID-stage2-enabled", buf, en);

    // E5/C0.9: the status must tell the truth — "armed" only when the functional admission
    // gate and preflight passed; otherwise publish the stop reason captured by logThunk.
    char status[288];
    const char *reason = state_.stop_reason[0] ? state_.stop_reason : state_.last_log;
    // R1 evidence: on the success path, show the discovered HDP flush dword so the boot
    // evidence proves the window exists (the prepare fails closed when it does not).
    char summary[128];
    const char *sum = state_.gate_summary;
    if (state_.gate_ok && state_.gate_summary[0] && integ_.env.hdp_flush_valid) {
        snprintf(summary, sizeof summary, "%s hdp=0x%x", state_.gate_summary,
                 integ_.env.hdp_flush_dword);
        sum = summary;
    }
    const int sn = (int)r3_status_line(status, sizeof status, state_.gate_ok, sum, reason,
                                       &state_.plan);
    if (sn > 0 && state_.sink.set_property)
        state_.sink.set_property(state_.sink.user, "N48R3EVID-status", status, (size_t)sn);
}

IOReturn NaviR3Bringup::triggerPowerUp() {
    // D-01/F12: start() already ran the read-only ladder (survey + preflight + preparation)
    // and published "armed, not fired". The trigger only fires the armed substeps: it does
    // not re-run stages 0-1, so the boot plan/env and its evidence stay authoritative and
    // the kernel stack stays bounded (the old export ran on a 16 KiB stack). D-04 still
    // makes this one attempt per boot.
    const StageTable &table = r3_stage_table();
    const bool provisioned = pci_ && pci_->getProperty("N48R3,ProvisionedThisBoot") != nullptr;
    const R3TriggerDecision d = r3_trigger_admission(
        provisioned, state_.gate_ok, integ_.prep_ok, r3_io_complete(&integ_.io),
        integ_.stage, R3_LADDER_FIRST_SUBSTEP, table.count);
    if (d.verdict != R3TriggerVerdict::Ok) {
        // E3: the reason must be observable through the evidence channel (log show is
        // unreachable). The boot-gate status keeps its semantics, so the refusal gets its
        // own short key; a pre-fire refusal touched no hardware, so D-04 is not set.
        char line[160];
        const int n = snprintf(line, sizeof line, "refused reason=%s\n",
                               d.reason ? d.reason : "?");
        const size_t len = n > 0 ? (static_cast<size_t>(n) < sizeof line
                                        ? static_cast<size_t>(n)
                                        : sizeof line - 1u)
                                 : 0u;
        if (len > 0u && state_.sink.set_property)
            state_.sink.set_property(state_.sink.user, "N48R3EVID-trigger-refused",
                                     line, len);
        IOLog("N48R3LOG trigger: %s", line);
        switch (d.verdict) {
        case R3TriggerVerdict::AlreadyProvisioned:
            return kIOReturnNotPermitted;
        case R3TriggerVerdict::NotArmed:
            return kIOReturnNotReady;
        case R3TriggerVerdict::NoSubsteps:
            return kIOReturnNotPermitted;
        case R3TriggerVerdict::Ok:
            break;
        }
        return kIOReturnError;
    }

    StageCtx ctx;
    ctx.log = &NaviR3Bringup::logThunk;
    ctx.dump_failure_state = &r3_dump_failure_state;
    ctx.user = &state_;
    ctx.substep = &integ_;

    StageHooks hooks;
    hooks.on_stage_start = nullptr;
    hooks.on_stage_stop = &NaviR3Bringup::onStageStop;

    const StageResult r = bringup_r3_from(d.first_stage, d.target_stage, table, ctx, &hooks);
    if (r != kStageOk) {
        IOLog("N48R3LOG trigger: power-up ladder stopped (result %d)\n", (int)r);
    }
    // D-01 lifecycle migration: full evidence export happens here, not in start().
    exportStage2Evidence();

    // E5/C0.9⑥: update the status from "armed, not fired" to the real result after firing.
    // The stop reason comes from the failure-dump field (set by the stage runner's logThunk),
    // falling back to the last stage log line when the dump never wrote a stop reason.
    {
        const char *reason = state_.stop_reason[0] ? state_.stop_reason : state_.last_log;
        char status[288];
        const int sn = (int)r3_status_fired_line(status, sizeof status, r == kStageOk, reason);
        if (sn > 0 && state_.sink.set_property)
            state_.sink.set_property(state_.sink.user, "N48R3EVID-status", status, (size_t)sn);
    }

    // D-04: set the marker regardless of success or failure — a failed power-up still
    // touched live hardware and must not be retried this boot.
    if (pci_)
        pci_->setProperty("N48R3,ProvisionedThisBoot", kOSBooleanTrue);
    fired_ = true;

    IOLog("N48R3LOG trigger: done result=%d ready=0x%x latch=0x%x deny=%llu\n",
          (int)r, integ_.rt.ready_bits, integ_.rt.latch_bits,
          (unsigned long long)r3_writeguard_deny_count());
    return r == kStageOk ? kIOReturnSuccess : kIOReturnInternalError;
}

// ---- BO tracking (UMA/GTT dual-domain pool, A-07/C-04/F-03/G-02) -------------
// UMA BOs are carved out of the D7 ("test_buf") grant page by page — the layout
// allocator stays the single authority over carveout addresses; this pool only
// *accounts* for pages of that grant (first pages 0..2 are reserved for the S7/S8/
// S9/S13 verification pages, so UMA allocation starts at page 3). The GPU address
// is the MC aperture address (fb_mc_base + D7 offset + page offset); CPU access is
// mapped by the user via IOConnectMapMemory64(handle) through a BAR0 window slice
// (see r3BoMemory). GTT BOs are backed by GART-table PTE-mapped sysmem pages (gmc
// mechanism via gmc_v11_0_gart_* helpers); their CP consumption path is deferred to
// stage 4/5 (E6/F5). All io->bus() calls take carveout byte offsets (C-08).
// ---- grant lookup helper (tag-based, same pattern as compute_v11.cpp) -------
static const R3Grant* find_grant(const R3LayoutPlan &plan, const char *tag) {
    for (uint32_t i = 0; i < plan.grant_count && i < R3_LAYOUT_MAX_GRANTS; i++) {
        if (strcmp(plan.grants[i].tag, tag) == 0) {
            return &plan.grants[i];
        }
    }
    return nullptr;
}


namespace {
struct BOEntry {
    uint32_t domain;       // kN48R3DomainUMA / kN48R3DomainGTT
    uint64_t size;         // bytes, page-aligned
    uint64_t gpu_va;       // canonical GPU address (UMA: fb_mc_base+D7 off; GTT: GART VA)
    uint64_t off;          // UMA: carveout byte offset inside D7 (BAR0 slice start)
    uint64_t gart_va;      // GTT: GART VA start (== gpu_va raw); 0 for UMA
    uint64_t sysmem_phys;  // GTT: sysmem physical page start; 0 for UMA
    void    *sysmem_cpu;   // GTT: sysmem CPU pointer; nullptr for UMA
    uint32_t pages;        // page count
    bool     in_use;
    IOMemoryDescriptor *map_md;  // P1: the ONE CPU-map descriptor of this BO (cache holds 1 ref)
};
static constexpr uint32_t kABIMaxBOs = 64u;
static BOEntry s_bo_pool[kABIMaxBOs] = {};
static IOLock *s_bo_lock = nullptr;

// UMA: page accounting bitmap over the D7 grant (1 MiB = 256 pages).
// Pages 0..2 are owned by the S7/S8/S9/S13 verification pages of the substeps.
static constexpr uint32_t kUmaMaxPages = 256u;
/* Page bitmap: bit i set = page i owned (incl. pages 0..2 pre-owned by the
 * S7/S8/S9/S13 verification pages of the substeps, bitmap starts 0x7). */
static uint32_t s_uma_bmp[8] = { 0x7u, 0, 0, 0, 0, 0, 0, 0 };

static bool uma_alloc_pages(uint32_t pages, uint64_t *page_off) {
    uint32_t run = 0;
    for (uint32_t i = 0; i < kUmaMaxPages; i++) {
        if (s_uma_bmp[i >> 5] & (1u << (i & 31u))) { run = 0; continue; }
        run++;
        if (run == pages) {
            const uint32_t start = i - pages + 1u;
            for (uint32_t k = start; k <= i; k++)
                s_uma_bmp[k >> 5] |= (1u << (k & 31u));
            *page_off = start;
            return true;
        }
    }
    return false;
}

static void uma_free_pages(uint64_t page_off, uint32_t pages) {
    for (uint32_t k = 0; k < pages; k++) {
        const uint32_t i = static_cast<uint32_t>(page_off) + k;
        if (i < kUmaMaxPages)
            s_uma_bmp[i >> 5] &= ~(1u << (i & 31u));
    }
}

// GTT: GART VA accounting. S7 occupies GART VA 0 .. 1 MiB (kGmcGartTestPages=256),
// so GTT BOs are allocated from VA 1 MiB upward; freed VAs are recycled on a stack.
static constexpr uint64_t kGartVaStart = 0x100000ull;
static uint64_t s_gart_next_va = kGartVaStart;
static uint64_t s_gart_free_va[kABIMaxBOs] = {};
static uint32_t s_gart_free_n = 0;

static uint64_t gart_alloc_va(uint32_t pages) {
    if (s_gart_free_n > 0) return s_gart_free_va[--s_gart_free_n];
    const uint64_t va = s_gart_next_va;
    s_gart_next_va += static_cast<uint64_t>(pages) * 4096ull;
    return va;
}

static void gart_free_va(uint64_t va) {
    if (s_gart_free_n < kABIMaxBOs) s_gart_free_va[s_gart_free_n++] = va;
}

static uint64_t bo_next_slot() {
    for (uint32_t i = 0; i < kABIMaxBOs; i++) {
        if (!s_bo_pool[i].in_use) {
            // Claim = clean record + visible to bo_by_slot/bo_by_va in one step (D1).
            s_bo_pool[i] = BOEntry{};
            s_bo_pool[i].in_use = true;
            return static_cast<uint64_t>(i) + 1ull;
        }
    }
    return 0ull;
}

static BOEntry *bo_by_slot(uint64_t slot) {
    if (slot == 0 || slot > kABIMaxBOs) return nullptr;
    const uint32_t i = static_cast<uint32_t>(slot - 1ull);
    if (!s_bo_pool[i].in_use) return nullptr;
    return &s_bo_pool[i];
}

// C-05: stage-3 minimal coverage check — the IB VA must fall inside an allocated BO.
static BOEntry *bo_by_va(uint64_t va) {
    const uint64_t s = n48r3_va_strip(va);
    for (uint32_t i = 0; i < kABIMaxBOs; i++) {
        BOEntry *e = &s_bo_pool[i];
        if (!e->in_use) continue;
        const uint64_t bs = n48r3_va_strip(e->gpu_va);
        if (s >= bs && s < bs + e->size) return e;
    }
    return nullptr;
}
} // namespace

IOReturn NaviR3Bringup::r3AllocBO(uint32_t domain, uint64_t size,
                                   uint64_t *gpu_va, uint64_t *handle) {
    if (!gpu_va || !handle) return kIOReturnBadArgument;
    if (n48r3_bo_alloc_args(size, domain) != 0) return kIOReturnBadArgument;
    if (!s_bo_lock) s_bo_lock = IOLockAlloc();
    if (!s_bo_lock) return kIOReturnNoMemory;

    const uint32_t pages = static_cast<uint32_t>((size + 4095ull) >> 12);
    const uint64_t aligned = static_cast<uint64_t>(pages) << 12;

    IOLockLock(s_bo_lock);
    const uint64_t slot = bo_next_slot();
    if (slot == 0) { IOLockUnlock(s_bo_lock); return kIOReturnNoMemory; }
    BOEntry *e = &s_bo_pool[slot - 1];
    *e = BOEntry{};

    IOReturn ret = kIOReturnSuccess;
    const R3LayoutPlan &plan = integ_.rt.layout;

    if (domain == kN48R3DomainUMA) {
        // UMA: carve D7 grant page-by-page; GPU VA = MC aperture address.
        const R3Grant *tb = find_grant(plan, "test_buf");
        if (!tb) { ret = kIOReturnInternalError; }
        else {
            uint64_t page_off = 0;
            if (!uma_alloc_pages(pages, &page_off)) {
                ret = kIOReturnNoMemory;
            } else {
                e->domain = kN48R3DomainUMA;
                e->size   = aligned;
                e->pages  = pages;
                e->off    = tb->start + (page_off << 12);
                e->gpu_va = n48r3_uma_gpu_va_canonical(integ_.env.fb_mc_base, e->off);
            }
        }
    } else {
        // GTT: sysmem pages via the gmc allocator + GART-table PTE programming.
        void *cpu = nullptr;
        const uint64_t phys = amdgpu::gmc_v11_0_sysmem_alloc_pages(pages, &cpu);
        const uint64_t gart_va = gart_alloc_va(pages);
        if (phys == 0) {
            ret = kIOReturnNoMemory;
        } else if (gart_va == 0) {
            amdgpu::gmc_v11_0_sysmem_free_pages(phys);
            ret = kIOReturnNoMemory;
        } else if (amdgpu::gmc_v11_0_gart_map_va(gart_va, phys, pages) < 0) {
            amdgpu::gmc_v11_0_sysmem_free_pages(phys);
            gart_free_va(gart_va);
            ret = kIOReturnInternalError;
        } else {
            e->domain     = kN48R3DomainGTT;
            e->size       = aligned;
            e->pages      = pages;
            e->gart_va    = gart_va;
            e->gpu_va     = gart_va;   // GART VA is already canonical (top 17 bits 0)
            e->sysmem_phys = phys;
            e->sysmem_cpu  = cpu;
        }
    }

    if (ret != kIOReturnSuccess) {
        e->in_use = false;
        IOLockUnlock(s_bo_lock);
        return ret;
    }
    *gpu_va  = e->gpu_va;
    *handle  = slot;
    IOLockUnlock(s_bo_lock);
    return kIOReturnSuccess;
}

IOReturn NaviR3Bringup::r3FreeBO(uint64_t gpu_va) {
    if (!s_bo_lock) return kIOReturnNotReady;
    IOLockLock(s_bo_lock);
    BOEntry *e = bo_by_va(gpu_va);
    if (!e) { IOLockUnlock(s_bo_lock); return kIOReturnBadArgument; }
    if (e->domain == kN48R3DomainUMA) {
        const R3Grant *tb = find_grant(integ_.rt.layout, "test_buf");
        if (tb) uma_free_pages((e->off - tb->start) >> 12, e->pages);
    } else {
        amdgpu::gmc_v11_0_gart_unmap_va(e->gart_va, e->pages);
        amdgpu::gmc_v11_0_sysmem_free_pages(e->sysmem_phys);
        gart_free_va(e->gart_va);
    }
    e->in_use = false;
    *e = BOEntry{};
    IOLockUnlock(s_bo_lock);
    return kIOReturnSuccess;
}

IOReturn NaviR3Bringup::r3SubmitIB(const struct N48R3CsIb *ibs, uint32_t num_ibs,
                                    uint64_t *fence) {
    if (!ibs || !fence) return kIOReturnBadArgument;
    if (num_ibs < 1u || num_ibs > N48R3_CS_IB_MAX) return kIOReturnBadArgument;
    if (!s_bo_lock) return kIOReturnNotReady;

    const R3LayoutPlan &plan = integ_.rt.layout;
    const uint64_t fb_mc_base = integ_.env.fb_mc_base;
    const R3Grant *mes_grant = find_grant(plan, "mes");
    const R3Grant *fence_ro  = find_grant(plan, "fence_ro");
    if (!mes_grant || !fence_ro) return kIOReturnInternalError;

    // C-05 stage-3 consensus: the ABI layer (NaviR3UserClient::doSubmit) already ran
    // n48r3_cs_in_validate; re-check IB validity and the BO coverage rule here so the
    // kext alone also refuses stray VAs. EXECUTABLE tracking is a stage-5 item (F8);
    // this stage's minimal rule: the IB VA must fall inside an allocated BO.
    IOLockLock(s_bo_lock);
    for (uint32_t i = 0; i < num_ibs; i++) {
        if (n48r3_cs_ib_valid(&ibs[i]) != 0) { IOLockUnlock(s_bo_lock); return kIOReturnBadArgument; }
        BOEntry *e = bo_by_va(ibs[i].va_start);
        if (!e) { IOLockUnlock(s_bo_lock); return kIOReturnBadArgument; }
        // GTT BOs are PTE-mapped but their CP consumption path is a stage 4/5 item (E6/F5);
        // the stage-3 ABI only submits IBs backed by UMA BOs (MC aperture VAs).
        if (e->domain == kN48R3DomainGTT) {
            IOLockUnlock(s_bo_lock);
            IOLog("N48R3LOG submit: GTT BO 0x%llx: CP consumption deferred to stage 4/5 (E6/F5)\n",
                  (unsigned long long)e->gpu_va);
            return kIOReturnUnsupported;
        }
    }
    IOLockUnlock(s_bo_lock);

    const uint64_t ring_off  = mes_grant->start + amdgpu::kMesCmpRingOff;  // carveout byte off (C-08)
    const uint64_t wb_off    = mes_grant->start + amdgpu::kMesCmpWbOff;
    const uint64_t fence_off = wb_off + amdgpu::kMesWbFenceOff;
    const uint64_t fence_mc  = fb_mc_base + fence_off;   // PM4-packet MC address only
    const uint32_t doorbell_idx = amdgpu::kMesDoorbellCompute;
    const uint32_t ring_dwords = amdgpu::kMesCmpRingDwords;
    const R3Io *io = &integ_.io;

    // C-09: persistent compute-ring wptr. Each submit appends one INDIRECT_BUFFER packet
    // per IB (4 dwords each) plus one RELEASE_MEM (8 dwords). When the tail would
    // straddle the ring boundary we pad to the end with NO_OP so no packet crosses the
    // wrap point (Linux amdgpu rings use the same NOP padding, gfx_v11 insert_nop).
    uint32_t wptr = cmp_wptr_;
    if (wptr == 0u) {
        // First ABI submit after the ladder: resume where the previous consumer
        // (S13, compute_v11.cpp) left the compute ring — read its wptr back from
        // the WB wptr-shadow page the MES ring contract uses (kMesWbWptrOff).
        // Restarting at 0 would make the MES re-execute the S13 packets that are
        // still in flight between rptr and the old wptr.
        uint64_t shadow = 0;
        if (!io->bus(io->ctx, R3_IO_BAR_VRAM, wb_off + amdgpu::kMesWbWptrOff,
                     &shadow, false, 2) ||
            shadow >= ring_dwords ||
            (shadow & 3u) != 0u)  /* 4-dword 对齐，保证回绕填充 NOP 对不越界 */
            return kIOReturnIOError;
        wptr = static_cast<uint32_t>(shadow);
    }
    uint32_t total_dw = 0;
    for (uint32_t i = 0; i < num_ibs; i++)
        total_dw += 4u;
    total_dw += 8u;
    if (total_dw > ring_dwords) return kIOReturnNoSpace;

    if (wptr + total_dw > ring_dwords) {
        // Fill with complete NO_OP packets (2 dwords: header + 0 body, see
        // N48R3_PM4_NOP_PAIR). wptr stays a multiple of 4 (each submit appends
        // 4-dword INDIRECT_BUFFERs and an 8-dword RELEASE_MEM, and the initial
        // shadow is 4-dword aligned), so pad is always even — write pairs.
        uint64_t nop_pair = N48R3_PM4_NOP_PAIR;
        const uint32_t pad = ring_dwords - wptr;
        for (uint32_t i = 0; i < pad; i += 2) {
            if (!io->bus(io->ctx, R3_IO_BAR_VRAM, ring_off + (wptr + i) * 4ull,
                         &nop_pair, true, 2)) return kIOReturnIOError;
        }
        wptr = 0;  // wrap: next packet starts at ring head
    }

    uint64_t seq = 0;
    { static uint64_t s_fence_seq = 1; seq = ++s_fence_seq; }

    // Each IB: INDIRECT_BUFFER pointing at the user BO (PM4 MC address inside the packet).
    for (uint32_t i = 0; i < num_ibs; i++) {
        uint32_t pkt[4];
        const uint32_t n = amdgpu::pm4_indirect_buffer(pkt, ibs[i].va_start,
                                                       ibs[i].ib_bytes / 4u, 0u, true);
        if (n == 0) return kIOReturnBadArgument;
        for (uint32_t k = 0; k < n; k++) {
            uint64_t v = pkt[k];
            if (!io->bus(io->ctx, R3_IO_BAR_VRAM, ring_off + (wptr + k) * 4ull, &v, true, 1))
                return kIOReturnIOError;
        }
        wptr += n;
    }

    // Fence: RELEASE_MEM targeting the WB fence slot (MC address inside the packet).
    uint32_t rm_pkt[8];
    const uint32_t rm_n = amdgpu::pm4_release_mem(rm_pkt, fence_mc, static_cast<uint32_t>(seq));
    for (uint32_t i = 0; i < rm_n; i++) {
        uint64_t v = rm_pkt[i];
        if (!io->bus(io->ctx, R3_IO_BAR_VRAM, ring_off + (wptr + i) * 4ull, &v, true, 1))
            return kIOReturnIOError;
    }
    wptr += rm_n;

    if (!amdgpu::pm4_submit_tail(io, ring_off, wb_off, doorbell_idx, wptr))
        return kIOReturnIOError;
    cmp_wptr_ = wptr;

    // Fence page (D6, fence_ro): publish the seq the user can map read-only and poll
    // (裁决 §2.4; grant is ReadOnly for the user side, the kext writes it per submit).
    uint64_t seq_val = seq;
    if (!io->bus(io->ctx, R3_IO_BAR_VRAM, fence_ro->start, &seq_val, true, 2))
        return kIOReturnIOError;

    *fence = seq;
    return kIOReturnSuccess;
}

IOReturn NaviR3Bringup::r3WaitFence(uint64_t expected_seq, uint32_t timeout_us,
                                     uint64_t *observed, bool *timed_out) {
    if (!observed || !timed_out) return kIOReturnBadArgument;
    *timed_out = false;

    const R3LayoutPlan &plan = integ_.rt.layout;
    const R3Grant *mes_grant = find_grant(plan, "mes");
    if (!mes_grant) return kIOReturnInternalError;

    const uint64_t fence_off = mes_grant->start + amdgpu::kMesCmpWbOff
                             + amdgpu::kMesWbFenceOff;   // carveout byte offset (C-08)
    const R3Io *io = &integ_.io;
    uint64_t val = 0;
    const uint64_t deadline = io->now_us(io->ctx) + static_cast<uint64_t>(timeout_us);

    while (io->now_us(io->ctx) < deadline) {
        if (!io->bus(io->ctx, R3_IO_BAR_VRAM, fence_off, &val, false, 1)) {
            *observed = val;
            return kIOReturnIOError;
        }
        if (val >= expected_seq) {
            *observed = val;
            return kIOReturnSuccess;
        }
        io->udelay(io->ctx, 100u);
    }

    io->bus(io->ctx, R3_IO_BAR_VRAM, fence_off, &val, false, 1);
    *observed = val;
    *timed_out = true;
    return kIOReturnSuccess;
}

IOMemoryDescriptor *NaviR3Bringup::r3BoMemory(uint64_t handle) {
    if (!s_bo_lock) return nullptr;
    IOLockLock(s_bo_lock);
    BOEntry *e = bo_by_slot(handle);
    IOMemoryDescriptor *md = nullptr;
    if (e) {
        if (e->domain == kN48R3DomainUMA) {
            // UMA: CPU access goes through the BAR0 window slice at the same carveout
            // offset the GPU sees (B4; grant->start is an offset into the BAR0 window).
            md = IOMemoryDescriptor::withPhysicalAddress(
                     dev_.bar0Phys + e->off, e->size, kIODirectionInOut);
        } else {
            md = IOMemoryDescriptor::withPhysicalAddress(
                     e->sysmem_phys, e->size, kIODirectionInOut);
        }
    }
    IOLockUnlock(s_bo_lock);
    return md;  // fresh descriptor; mapClientMemory64 consumes the reference
}

IOMemoryDescriptor *NaviR3Bringup::r3FenceRoMemory() {
    const R3Grant *fence_ro = find_grant(integ_.rt.layout, "fence_ro");
    if (!fence_ro) return nullptr;
    return IOMemoryDescriptor::withPhysicalAddress(
        dev_.bar0Phys + fence_ro->start, fence_ro->size, kIODirectionIn);
}


void NaviR3Bringup::exportStage2Evidence() {
    char buf[1024];

    // enabled bitmap + gate inputs (E2): exported even when preparation failed, so the
    // operator can see why stages 1..5 did not run.
    const R3GateInputs g = r3_integration_gate_inputs(&integ_);
    const size_t en = r3_integration_format_enabled(&g, buf, sizeof buf);
    if (en > 0 && state_.sink.set_property)
        state_.sink.set_property(state_.sink.user, "N48R3EVID-stage2-enabled", buf, en);

    // Per-substep outcome summary.
    const size_t sn = r3_integration_format_summary(
        integ_.stage, integ_.effective_stage, integ_.rt.ready_bits, integ_.rt.latch_bits,
        r3_writeguard_deny_count(), integ_.outcomes, integ_.outcome_count, buf, sizeof buf);
    if (sn > 0 && state_.sink.set_property)
        state_.sink.set_property(state_.sink.user, "N48R3EVID-stage2-summary", buf, sn);

    // B5: the stage-2 read manifest (every substep/dump read through the R3Io adapter).
    // T16: the text buffer is instance-owned, not a local. Two 8 KiB locals in one function
    // exceeded the 16 KiB kernel stack (R1 panic); the same member is reused sequentially.
    if (state_.sink.set_property) {
        const size_t rn = r3_manifest_format(&rd_manifest_, rdm_buf_, sizeof rdm_buf_);
        if (rn > 0)
            state_.sink.set_property(state_.sink.user, "N48R3EVID-read-manifest-stage2",
                                     rdm_buf_, rn);
        const size_t mn = r3_manifest_format(&rd_smn_, rdm_buf_, sizeof rdm_buf_);
        if (mn > 0)
            state_.sink.set_property(state_.sink.user, "N48R3EVID-read-manifest-smn",
                                     rdm_buf_, mn);
    }

    // Write manifest (B1) and the read manifest (B5) stay with their owners: the writeguard
    // owns the former, the survey the latter. Publish the write manifest here because the
    // stage-2 rows only exist once the ladder ran.
    if (state_.sink.set_property) {
        const size_t wn = r3_wm_format(r3_writeguard_manifest(), wm_buf_, sizeof wm_buf_);
        if (wn > 0)
            state_.sink.set_property(state_.sink.user, R3_WM_EVID, wm_buf_, wn);
        char mtext[128];
        const int mn = snprintf(mtext, sizeof mtext, "deny=%llu truncated=%d\n",
                                (unsigned long long)r3_writeguard_deny_count(),
                                r3_writeguard_manifest()->truncated ? 1 : 0);
        if (mn > 0)
            state_.sink.set_property(state_.sink.user, "N48R3EVID-write-manifest-status",
                                     mtext, (size_t)mn);
    }
}

void NaviR3Bringup::stop(IOService *provider) {
    // D-01/8e: a failed substep already ran the reverse chain in onStageStop. #18: a
    // power-up that completed successfully must also be reversed exactly once here;
    // teardown_done_ keeps the two paths single-shot. An armed-but-not-fired stop never
    // wrote the IPs, so the chain records "no reversible object" and writes nothing.
    if (started_ && !teardown_done_) {
        R3TeardownEnv te {};
        te.io = &integ_.io;
        te.sub = &integ_.env;
        te.failed = integ_.failed;
        te.failed_ip_valid = integ_.failed_ip_valid;
        te.failed_ip = integ_.failed_ip;
        const R3TeardownReport rep = r3_teardown_run(&te);
        IOLog("N48R3LOG teardown: stop triggered=%d acted=0x%x none=0x%x writes=%u failed=0x%x\n",
              rep.triggered ? 1 : 0, rep.acted, rep.none, rep.writes, rep.write_fail);
        teardown_done_ = true;
    }
    if (started_) {
        r3_writeguard_disarm();
        started_ = false;
    }
    if (ioLock_) {
        IOLockFree(ioLock_);
        ioLock_ = nullptr;
    }
    unmapBar2();
    unmapBar0();
    unmapBar5();
    super::stop(provider);
}

void NaviR3Bringup::free() {
    unmapBar2();
    unmapBar0();
    unmapBar5();
    super::free();
}

bool NaviR3Bringup::setPropertyThunk(void *user, const char *key, const char *bytes, size_t len) {
    NaviR3Bringup *self = static_cast<NaviR3Bringup *>(user);
    if (!self || !key || !bytes || len > 0xffffffffu) return false;
    return self->setProperty(key, const_cast<char *>(bytes), (unsigned int)len);
}

void NaviR3Bringup::logThunk(void *user, const char *line) {
    IOLog("%s\n", line ? line : "N48R3LOG (null line)");
    // E5 (v1.7): keep the most recent stage line so the failure dump can publish the stop
    // reason and the status field can tell the truth. Strip the shared prefix for readability.
    R3SurveyStageState *st = static_cast<R3SurveyStageState *>(user);
    if (!st || !line) return;
    const char *p = line;
    const size_t plen = sizeof(TGT_LOG_PREFIX) - 1u;
    if (plen > 0 && strncmp(p, TGT_LOG_PREFIX, plen) == 0 && p[plen] == ' ') p += plen + 1u;
    snprintf(st->last_log, sizeof st->last_log, "%s", p);
}

void NaviR3Bringup::wgLogThunk(void * /*user*/, const char *line) {
    IOLog("%s\n", line ? line : "N48R3LOG writeguard: (null)");
}

IOReturn NaviR3Bringup::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;

    NaviR3UserClient *uc = OSTypeAlloc(NaviR3UserClient);
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type, properties)) {
        uc->release();
        return kIOReturnNotPermitted;
    }
    if (!uc->attach(this)) {
        uc->release();
        return kIOReturnInternalError;
    }
    if (!uc->start(this)) {
        uc->detach(this);
        uc->release();
        return kIOReturnInternalError;
    }
    *handler = uc;
    return kIOReturnSuccess;
}
