// NaviR3Bringup.hpp - the RDNA3 (Phoenix 780M) stage-2 kext entry class.
//
// probe() matches 0x15BF only when the load gate n48r3bringup=1 is present. start() maps BAR5,
// confirms D0, runs the read-only survey + preflight ladder (stages 0-1) and publishes the
// "armed, not fired" evidence. The five power-up substeps (stages 2-6) are triggered
// separately via the user client selector or n48r3-auto=1 (D-01/F12; the auto switch
// is compile-time gated off in the development phase, INV-D5 — see r3_auto_mode below).
#ifndef N48_RDNA3_BRINGUP_HPP
#define N48_RDNA3_BRINGUP_HPP

#include <stdint.h>

#include "common/BootArgFamily.h"

// n48r3-auto: when 1, start() would trigger the power-up substeps immediately after
// preflight (requirement F12; default off — the operator must confirm the machine is
// ready first). INV-D5: during the development phase the switch is hard-disabled at
// compile time — an unattended power-up failure would reboot-loop the machine, the one
// unrecoverable state. R3_AUTO_PHASE_OPEN is the phase gate (0 = closed); the parser is
// kept so an operator who sets n48r3-auto=1 gets a named refusal at the start() consumer
// (via `rejected`), not a silent ignore.
#ifndef R3_AUTO_PHASE_OPEN
#define R3_AUTO_PHASE_OPEN 0
#endif
static inline bool r3_auto_mode(bool *rejected = nullptr) {
    uint32_t val = 0;
    const bool requested = tgt_boot_arg_u32("auto", &val) && val != 0;
    if (rejected)
        *rejected = requested;
    return R3_AUTO_PHASE_OPEN != 0 && requested;
}

// n48r3-depth accepts 0..5 (T1..T6). Any other value falls back to 0 (T1, the
// conservative default). Pure, so the host test can exercise it without IOKit.
static inline uint32_t r3_depth_clamp(uint32_t raw) {
    return raw <= 5u ? raw : 0u;
}

// The resolution start() performs: parse n48r3-depth (BootArgFamily supplies the
// central default, 0 = T1) and clamp to the tier range. `present` separates an
// explicit boot-arg from the default so the log says which one was used.
struct R3DepthSel {
    uint32_t raw;
    uint32_t depth;
    bool     present;
};

static inline R3DepthSel r3_depth_select(void) {
    R3DepthSel s = { 0, 0, false };
    s.present = tgt_boot_arg_u32("depth", &s.raw);
    s.depth = r3_depth_clamp(s.raw);
    return s;
}

#if defined(KERNEL)
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>

#include "rdna3_survey.h"
#include "rdna3_wmanifest.h"
#include "rdna3_integration.h"

class IOPCIDevice;
class IOMemoryMap;
class IOUserClient;

class NaviR3Bringup : public IOService {
    OSDeclareDefaultStructors(NaviR3Bringup)
    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool       start(IOService *provider) override;
    void       stop(IOService *provider) override;
    void       free() override;
    IOReturn   newUserClient(task_t owningTask, void *securityID, UInt32 type,
                             OSDictionary *properties, IOUserClient **handler) override;

    const char *surveyText() const { return state_.text; }
    IOReturn   triggerPowerUp();  /* D-01: user-client trigger entry (selector 1) */

private:
    static bool setPropertyThunk(void *user, const char *key, const char *bytes, size_t len);
    static void wgLogThunk(void *user, const char *line);
    static void logThunk(void *user, const char *line);

    bool mapBar5();
    bool mapCarveout();          /* BAR0: the only CPU window into the carveout (B4)   */
    bool mapDoorbells();         /* BAR2: 64-bit doorbells for the MES substep          */
    void unmapBar0();
    void unmapBar2();
    void unmapBar5();
    bool confirmD0(bool *measured);

    static bool ioMmio(void *ctx, uint32_t dword, uint32_t *value, bool write);
    static bool ioSmuio(void *ctx, uint32_t index, uint32_t *value, bool write);
    static bool ioBus(void *ctx, uint32_t bar, uint64_t byte_off, uint64_t *value,
                      bool write, uint32_t dwords);
    static void ioUdelay(void *ctx, uint32_t us);
    static void ioMdelay(void *ctx, uint32_t ms);
    static uint64_t ioNowUs(void *ctx);
    static void ioFence(void *ctx);
    static bool ioHdpFlush(void *ctx);
    static void ioLock(void *ctx);
    static void ioUnlock(void *ctx);

    void bindIntegrationIo();
    void exportArmedEvidence();
    void exportStage2Evidence();
    /* StageHooks: the K9 dump -> latch -> teardown order (R3 in StageRunner does the latch). */
    static void onStageStop(StageCtx &ctx, uint32_t index, StageResult result);

    IOPCIDevice          *pci_ { nullptr };
    IOMemoryMap          *bar5Map_ { nullptr };
    IOMemoryMap          *bar0Map_ { nullptr };
    IOMemoryMap          *bar2Map_ { nullptr };
    IOLock               *ioLock_ { nullptr };
    R3IntegrationCtx      integ_ {};
    R3ReadManifest        rd_manifest_ {};   /* BAR5/BAR0 reads (B5)                  */
    R3ReadManifest        rd_smn_ {};        /* SMN reads, keyed by smn_byte_addr>>2  */
    amdgpu::DeviceContext dev_ {};
    R3DiscoveryReport     disc_ {};
    struct dcn314_dev     dcn_ {};
    R3SurveyStageState    state_ {};
    char                  wm_buf_[R3_WM_TEXT_BYTES] {};  /* D-08 item 6: member buffer (was static) */
    char                  rdm_buf_[R3_MANIFEST_BYTES] {}; /* B5/T16: read-manifest text (was stack) */
    bool                  fired_ { false };              /* D-01: power-up substeps ran this boot */
    bool                  started_ { false };
};

#endif // KERNEL

#endif // N48_RDNA3_BRINGUP_HPP
