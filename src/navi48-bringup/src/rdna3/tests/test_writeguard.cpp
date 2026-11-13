/*
 * test_writeguard.cpp - host tests for the deny-by-default write whitelist (T9) and the B1
 * write manifest it owns (T10).
 *
 * Stage-1 families: the absolute MM_INDEX/MM_DATA/MM_INDEX_HI window T2 needs to pull the VRAM
 * TMR block into RAM, and the 25 AUX request registers dcn314_aux_min.c writes. Every window is
 * one dword; widening any of them by one dword MUST be caught here (the planted-break harness
 * mutates the MM_INDEX_HI entry).
 *
 * Stage-2 families (tech plan §6.1/§6.2): the B2 four classes and the five substep owners.
 * A Data layout grant ("layout:...") admits any sub-range but never crosses the grant edge; a
 * Protocol/State register window ("reg:...") matches one address+width exactly and belongs to
 * exactly one substep. Owner mismatch, an unarmed substep and a disarmed guard all DENY.
 *
 * Manifest (T10): same (owner, cls, start, len, tag) merges its count, never across substeps;
 * every DENY is its own line; the R3_WM_MAX cap sets truncated=true and is never silent.
 *
 * Address space used below is stand-in dword addresses; the classifier only compares values.
 */
#include <stdio.h>
#include <string.h>

#include "rdna3_wmanifest.h"
#include "rdna3_writeguard.h"
#include "rdna3_substep.h"
#include "n48log_stub.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Minimal N48R3EVID-* property capture for the P12 sink-channel assertion. */
struct PropCapture {
    char      key[48];
    char      value[R3_EVID_TEXT_BYTES + 32u];
    uint32_t  value_len;
    uint32_t  calls;
};
static bool prop_capture(void *user, const char *key, const char *bytes, size_t len)
{
    PropCapture *p = static_cast<PropCapture *>(user);
    if (!p || !key || !bytes)
        return false;
    p->calls++;
    snprintf(p->key, sizeof p->key, "%s", key);
    size_t n = len < sizeof p->value - 1u ? len : sizeof p->value - 1u;
    memcpy(p->value, bytes, n);
    p->value[n] = '\0';
    p->value_len = static_cast<uint32_t>(n);
    return true;
}
static void prop_capture_init(PropCapture *p)
{
    memset(p, 0, sizeof *p);
    p->key[0] = '\0';
    p->value[0] = '\0';
}

static const uint32_t SEG[R3_WG_SEGS] = {
    0x00000000u, 0x00001000u, 0x00010000u, 0x00020000u, 0x00030000u
};
#define AUX(a) (SEG[2] + (uint32_t)(a))

/* Absolute BAR5 dwords from the base window implementation (Navi48Bringup.cpp:160-165). */
static const uint32_t kMmOffsets[R3_WG_MM_WINDOW_COUNT] = {
    0x0u,   /* MM_INDEX */
    0x1u,   /* MM_DATA  */
    0x6u,   /* MM_INDEX_HI */
};

/* Exactly the registers dcn314_aux_min.c writes, in table order. */
static const uint32_t kAuxOffsets[R3_WG_AUX_WINDOW_COUNT] = {
    0x1f50u, 0x1f51u, 0x1f52u, 0x1f53u, 0x1f56u,   /* AUX0 */
    0x1f6cu, 0x1f6du, 0x1f6eu, 0x1f6fu, 0x1f72u,   /* AUX1 */
    0x1f88u, 0x1f89u, 0x1f8au, 0x1f8bu, 0x1f8eu,   /* AUX2 */
    0x1fa4u, 0x1fa5u, 0x1fa6u, 0x1fa7u, 0x1faau,   /* AUX3 */
    0x1fc0u, 0x1fc1u, 0x1fc2u, 0x1fc3u, 0x1fc6u,   /* AUX4 */
};

/* Addresses that must be refused: gaps around the MM window, the AUX read-only status
 * register, gaps between/around AUX windows, and neighbours in other segments. */
static const uint32_t kDeny[] = {
    0x00000002u,                       /* first dword past MM_INDEX */
    0x00000005u,                       /* gap below MM_INDEX_HI */
    0x00000007u,                       /* first dword past MM_INDEX_HI */
    0x00000040u,
    AUX(0x1f54u),                      /* AUX0_SW_STATUS (read-only) */
    AUX(0x1f55u),                      /* gap below AUX0_SW_DATA */
    AUX(0x1f57u),                      /* first dword past AUX0_SW_DATA */
    AUX(0x1f5bu),
    AUX(0x1f6bu),                      /* gap before AUX1 */
    AUX(0x1fb0u),                      /* gap between AUX3 and AUX4 */
    AUX(0x1fbfu),
    AUX(0x1fc7u),                      /* first dword past AUX4_SW_DATA */
    AUX(0x1f4fu),                      /* first dword before AUX0 */
    0x00002f50u,                       /* SEG1 + 0x1f50 */
    0x00021f50u,                       /* SEG3 + 0x1f50 */
};

/* --------------------------------------------------------------------- stage 1 (unchanged) */

static void test_install(void)
{
    CHECK(r3_writeguard_install(nullptr) == -1, "null seg refused");
    CHECK(r3_writeguard_install(SEG) == 0, "install ok");
    CHECK(r3_writeguard_deny_count() == 0 && r3_writeguard_txn_count() == 0,
          "install resets counters");
}

static void test_mm_only_arm(void)
{
    CHECK(r3_writeguard_install_mm() == 0, "MM arm ok");
    for (unsigned i = 0; i < R3_WG_MM_WINDOW_COUNT; i++) {
        CHECK(r3_write_classify(kMmOffsets[i], 1u, "tmr") == WVerdict::TxnRead,
              "MM dword 0x%x allowed before discovery", kMmOffsets[i]);
        CHECK(r3_writeguard_last_tag() && strcmp(r3_writeguard_last_tag(), "mm-indirect") == 0,
              "MM window tag recorded");
    }
    CHECK(r3_write_classify(0x7u, 1u, "tmr") == WVerdict::Deny, "past MM window denied");
    CHECK(r3_write_classify(AUX(0x1f50u), 1u, "edid") == WVerdict::Deny,
          "AUX not armed by the MM-only step");
    CHECK(r3_writeguard_txn_count() == R3_WG_MM_WINDOW_COUNT, "MM txn count");
    CHECK(r3_writeguard_deny_count() == 2, "MM-only denies");
}

static void test_table(void)
{
    for (uint32_t i = 0; i < R3_WG_WINDOW_COUNT; i++) {
        const R3WgWindow* w = r3_writeguard_window(i);
        CHECK(w != nullptr, "window %u present", i);
        if (!w)
            continue;
        CHECK(w->dwords == 1u, "window %u is one dword", i);
        if (i < R3_WG_MM_WINDOW_COUNT) {
            CHECK(w->absolute, "window %u is absolute", i);
            CHECK(w->offset == kMmOffsets[i], "window %u offset 0x%x, want 0x%x", i,
                  w->offset, kMmOffsets[i]);
            CHECK(w->tag && strcmp(w->tag, "mm-indirect") == 0, "window %u tag", i);
        } else {
            const uint32_t a = i - R3_WG_MM_WINDOW_COUNT;
            CHECK(!w->absolute, "window %u is segment-relative", i);
            CHECK(w->base_idx == 2u, "window %u in DCN segment 2", i);
            CHECK(w->offset == kAuxOffsets[a], "window %u offset 0x%x, want 0x%x", i,
                  w->offset, kAuxOffsets[a]);
            CHECK(w->tag && strcmp(w->tag, "aux-ddc-read") == 0, "window %u tag", i);
        }
    }
    CHECK(r3_writeguard_window(R3_WG_WINDOW_COUNT) == nullptr, "past-the-end is null");
}

static void test_allow(void)
{
    r3_writeguard_install(SEG);
    for (uint32_t i = 0; i < R3_WG_MM_WINDOW_COUNT; i++) {
        const WVerdict v = r3_write_classify(kMmOffsets[i], 1u, "tmr-read");
        CHECK(v == WVerdict::TxnRead, "MM dword 0x%x allowed", kMmOffsets[i]);
        CHECK(r3_writeguard_last_tag() &&
                  strcmp(r3_writeguard_last_tag(), "mm-indirect") == 0,
              "MM dword 0x%x tag recorded", kMmOffsets[i]);
        CHECK(r3_writeguard_last_context() &&
                  strcmp(r3_writeguard_last_context(), "tmr-read") == 0,
              "MM dword 0x%x context recorded", kMmOffsets[i]);
    }
    for (uint32_t i = 0; i < R3_WG_AUX_WINDOW_COUNT; i++) {
        const WVerdict v = r3_write_classify(AUX(kAuxOffsets[i]), 1u, "edid-aux0");
        CHECK(v == WVerdict::TxnRead, "offset 0x%x allowed", kAuxOffsets[i]);
        CHECK(r3_writeguard_last_tag() &&
                  strcmp(r3_writeguard_last_tag(), "aux-ddc-read") == 0,
              "offset 0x%x window tag recorded", kAuxOffsets[i]);
        CHECK(r3_writeguard_last_context() &&
                  strcmp(r3_writeguard_last_context(), "edid-aux0") == 0,
              "offset 0x%x context tag recorded", kAuxOffsets[i]);
    }
    CHECK(r3_writeguard_txn_count() == R3_WG_WINDOW_COUNT, "txn count");
    CHECK(r3_writeguard_deny_count() == 0, "no denies");
}

static void test_deny(void)
{
    r3_writeguard_install(SEG);
    for (unsigned i = 0; i < sizeof(kDeny) / sizeof(kDeny[0]); i++) {
        CHECK(r3_write_classify(kDeny[i], 1u, "probe") == WVerdict::Deny,
              "addr 0x%x denied", kDeny[i]);
        CHECK(r3_writeguard_last_tag() == nullptr, "deny clears tag (0x%x)", kDeny[i]);
    }
    CHECK(r3_writeguard_deny_count() == sizeof(kDeny) / sizeof(kDeny[0]), "deny count");
    CHECK(r3_writeguard_txn_count() == 0, "no txns");
}

static void test_cross_window(void)
{
    r3_writeguard_install(SEG);
    /* A two-dword write starting at a one-dword window crosses out of it. */
    CHECK(r3_write_classify(AUX(0x1f56u), 2u, "wide") == WVerdict::Deny, "wide write denied");
    CHECK(r3_write_classify(AUX(0x1f50u), 2u, "wide") == WVerdict::Deny, "wide write denied 2");
    CHECK(r3_write_classify(0x6u, 2u, "wide") == WVerdict::Deny, "wide MM window denied");
    CHECK(r3_write_classify(0x0u, 2u, "wide") == WVerdict::Deny, "wide MM_INDEX denied");
    CHECK(r3_write_classify(AUX(0x1f56u), 1u, "narrow") == WVerdict::TxnRead, "size 1 ok");
    CHECK(r3_write_classify(0x6u, 1u, "narrow") == WVerdict::TxnRead, "MM size 1 ok");
    CHECK(r3_write_classify(AUX(0x1f56u), 0u, "zero") == WVerdict::Deny, "size 0 denied");
}

static void test_unarmed(void)
{
    r3_writeguard_install(SEG);
    r3_write_classify(AUX(0x1f50u), 1u, "ok");
    r3_writeguard_disarm();
    CHECK(r3_write_classify(AUX(0x1f50u), 1u, "ok") == WVerdict::Deny,
          "disarmed denies a former window");
    CHECK(r3_write_classify(0x0u, 1u, "ok") == WVerdict::Deny,
          "disarmed denies the MM window");
    CHECK(r3_writeguard_txn_count() == 1, "txn count kept across disarm");
    CHECK(r3_writeguard_deny_count() == 2, "deny count after disarm");
}

static void test_adapter(void)
{
    r3_writeguard_install(SEG);
    CHECK(r3_writeguard_write_ok(nullptr, AUX(0x1f56u), 0xdeadbeefu) == true,
          "adapter allows AUX SW_DATA");
    CHECK(r3_writeguard_write_ok(nullptr, AUX(0x1f54u), 0u) == false,
          "adapter refuses AUX SW_STATUS");
    CHECK(r3_writeguard_write_ok(nullptr, 0x2u, 0u) == false, "adapter refuses MM gap");
    CHECK(r3_writeguard_txn_count() == 1, "adapter txn");
    CHECK(r3_writeguard_deny_count() == 2, "adapter denies");
}

static void test_overflow(void)
{
    r3_writeguard_install(SEG);
    CHECK(r3_write_classify(0xFFFFFFFFull, 1u, "x") == WVerdict::Deny, "top dword denied");
    CHECK(r3_write_classify(0xFFFFFFFFull, 2u, "x") == WVerdict::Deny, "top+1 denied");
    CHECK(r3_write_classify(0x100000000ull, 1u, "x") == WVerdict::Deny, ">32-bit denied");
}

/* ------------------------------------------------------- stage 2: class / substep / windows */

/* Layout grant: 8 dwords of the PSP stage-2 buffer, owned by the PSP substep. */
#define FW_START 0x00800000u
#define FW_LEN   8u
#define FW_TAG   "layout:psp-stage2"
/* Protocol register: PSP GPCOM C2PMSG_67, one dword, PSP only. */
#define GPCOM   0x00010040u
/* Protocol regs owned by two different substeps at different addresses. */
#define SMU_MAIL 0x00100080u
#define SDMA_WPT 0x00300010u
/* State registers. */
#define PSP_RING 0x00010060u
#define CP_MES   0x00400090u

static void arm_stage2(void)
{
    r3_writeguard_install(SEG);
    CHECK(r3_wg_register_layout(FW_START, FW_LEN, WSubstep::Psp, FW_TAG) == 0,
          "layout grant registered");
    CHECK(r3_wg_register_reg(GPCOM, 1u, WClass::Protocol, WSubstep::Psp, "reg:psp-gpcom") == 0,
          "psp reg registered");
    CHECK(r3_wg_register_reg(SMU_MAIL, 1u, WClass::Protocol, WSubstep::Smu,
                             "reg:smu-mailbox") == 0,
          "smu reg registered");
    CHECK(r3_wg_register_reg(SDMA_WPT, 1u, WClass::Protocol, WSubstep::Sdma,
                             "reg:sdma-wptr") == 0,
          "sdma reg registered");
    CHECK(r3_wg_register_reg(PSP_RING, 4u, WClass::State, WSubstep::Psp, "reg:psp-ring") == 0,
          "psp state reg registered");
    CHECK(r3_wg_register_reg(CP_MES, 1u, WClass::State, WSubstep::Mes, "reg:cp-mes-cntl") == 0,
          "mes state reg registered");
}

static void test_stage2_register_reject(void)
{
    arm_stage2();
    const uint32_t n = r3_wg_grant_count();
    CHECK(r3_wg_register_grant(nullptr) == -1, "null grant refused");
    CHECK(r3_wg_register_layout(FW_START, 0u, WSubstep::Psp, FW_TAG) == -1, "zero width");
    CHECK(r3_wg_register_layout(0xFFFFFFFFu, 2u, WSubstep::Psp, FW_TAG) == -1, "overflow");
    CHECK(r3_wg_register_layout(FW_START, 1u, WSubstep::None, FW_TAG) == -1, "owner None");
    CHECK(r3_wg_register_layout(FW_START, 1u, WSubstep::Psp, "reg:wrong") == -1,
          "layout without layout: prefix");
    CHECK(r3_wg_register_reg(GPCOM, 1u, WClass::Data, WSubstep::Psp, "reg:x") == -1,
          "Data via the register form");
    CHECK(r3_wg_register_reg(GPCOM, 1u, WClass::Protocol, WSubstep::Psp, "layout:x") == -1,
          "reg without reg: prefix");
    CHECK(r3_wg_register_grant(nullptr) == -1, "null grant refused again");
    CHECK(r3_wg_grant_count() == n, "rejected registrations add nothing");
    const R3WgGrant *g = r3_wg_grant(0);
    CHECK(g && g->cls == WClass::Data && g->range && g->dwords == FW_LEN &&
              g->owner == WSubstep::Psp && strcmp(g->tag, FW_TAG) == 0,
          "grant 0 keeps layout shape");
    CHECK(r3_wg_grant(n) == nullptr, "past-the-end grant is null");

    r3_writeguard_install(SEG);
    CHECK(r3_wg_grant_count() == 0, "install clears grants");
    CHECK(r3_wg_substep() == WSubstep::None, "install resets the substep owner");
}

static void test_stage2_allow_four_classes(void)
{
    arm_stage2();

    /* ① Access is owner-less: the stage-1 window belongs to WSubstep::None, so inside a substep
     * it is claimed by that window and DENIED (owner mismatch) - never re-tried. */
    r3_wg_arm_substep(WSubstep::Psp);
    CHECK(r3_wg_classify(0x6u, 1u, WClass::Access, "mm") == WVerdict::Deny,
          "access window denied inside a substep (owner mismatch)");
    CHECK(r3_writeguard_last_tag() == nullptr, "deny clears the tag");
    r3_wg_disarm_substep();
    CHECK(r3_wg_classify(0x6u, 1u, WClass::Access, "mm") == WVerdict::TxnRead,
          "access window allowed with no substep armed");
    CHECK(r3_writeguard_last_tag() && strcmp(r3_writeguard_last_tag(), "mm-indirect") == 0,
          "access window tag");
    r3_wg_arm_substep(WSubstep::Psp);

    /* ③ Protocol: exact register, PSP owner. */
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "psp-load-ip-fw") == WVerdict::TxnRead,
          "psp gpcom allowed");
    CHECK(r3_writeguard_last_tag() && strcmp(r3_writeguard_last_tag(), "reg:psp-gpcom") == 0,
          "gpcom tag");
    CHECK(r3_writeguard_last_context() &&
              strcmp(r3_writeguard_last_context(), "psp-load-ip-fw") == 0,
          "gpcom context");

    /* ④ State: a multi-dword register group replaces all its dwords. */
    CHECK(r3_wg_classify(PSP_RING, 4u, WClass::State, "psp-ring") == WVerdict::TxnRead,
          "psp ring group allowed");
    CHECK(r3_wg_classify(PSP_RING + 3u, 1u, WClass::State, "psp-ring") == WVerdict::Deny,
          "a sub-range of a register group is not the group");

    /* ② Data: any sub-range of the layout interval. */
    CHECK(r3_wg_classify(FW_START, FW_LEN, WClass::Data, "fw-stage2") == WVerdict::TxnRead,
          "whole layout interval allowed");
    CHECK(r3_wg_classify(FW_START + 3u, 2u, WClass::Data, "fw-stage2") == WVerdict::TxnRead,
          "layout sub-range allowed");
    CHECK(r3_wg_classify(FW_START + FW_LEN - 1u, 1u, WClass::Data, "fw-stage2") ==
              WVerdict::TxnRead,
          "last dword allowed");
    CHECK(r3_writeguard_last_tag() && strcmp(r3_writeguard_last_tag(), FW_TAG) == 0,
          "layout tag");
    CHECK(r3_writeguard_deny_count() == 2, "one access-owner deny + one group sub-range deny");
}

static void test_stage2_layout_edge(void)
{
    arm_stage2();
    r3_wg_arm_substep(WSubstep::Psp);
    /* One dword past each edge, and a write that starts inside but ends outside. */
    CHECK(r3_wg_classify(FW_START + FW_LEN, 1u, WClass::Data, "x") == WVerdict::Deny,
          "first dword past the grant denied");
    CHECK(r3_wg_classify(FW_START - 1u, 1u, WClass::Data, "x") == WVerdict::Deny,
          "last dword before the grant denied");
    CHECK(r3_wg_classify(FW_START + FW_LEN - 1u, 2u, WClass::Data, "x") == WVerdict::Deny,
          "write crossing the grant end denied");
    CHECK(r3_wg_classify(FW_START, FW_LEN + 1u, WClass::Data, "x") == WVerdict::Deny,
          "grant widened by one dword denied");
    CHECK(r3_wg_classify(FW_START - 1u, FW_LEN + 2u, WClass::Data, "x") == WVerdict::Deny,
          "oversized write denied");
    /* A Data grant never admits a non-Data class, nor an Access window a substep owns. */
    CHECK(r3_wg_classify(FW_START, 1u, WClass::State, "x") == WVerdict::Deny,
          "layout in the wrong class denied");
    CHECK(r3_wg_classify(FW_START, 1u, WClass::Access, "x") == WVerdict::Deny,
          "layout as access denied");
    CHECK(r3_wg_classify(0x6u, 1u, WClass::Data, "x") == WVerdict::Deny,
          "access window in the wrong class denied");
    CHECK(r3_writeguard_deny_count() == 8, "eight edges denied");
}

static void test_stage2_owner(void)
{
    arm_stage2();

    r3_wg_arm_substep(WSubstep::Smu);
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "psp register denied in the smu substep");
    CHECK(r3_wg_classify(FW_START, 2u, WClass::Data, "x") == WVerdict::Deny,
          "psp layout denied in the smu substep");
    CHECK(r3_wg_classify(PSP_RING, 4u, WClass::State, "x") == WVerdict::Deny,
          "psp state denied in the smu substep");
    CHECK(r3_wg_classify(SMU_MAIL, 1u, WClass::Protocol, "x") == WVerdict::TxnRead,
          "smu register allowed in the smu substep");

    r3_wg_arm_substep(WSubstep::Sdma);
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "psp register denied in the sdma substep");
    CHECK(r3_wg_classify(SDMA_WPT, 1u, WClass::Protocol, "x") == WVerdict::TxnRead,
          "sdma register allowed in the sdma substep");

    r3_wg_arm_substep(WSubstep::Mes);
    CHECK(r3_wg_classify(CP_MES, 1u, WClass::State, "x") == WVerdict::TxnRead,
          "mes register allowed in the mes substep");
    CHECK(r3_wg_classify(SDMA_WPT, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "sdma register denied in the mes substep");

    /* Substeps without a registered window never inherit another substep's. */
    r3_wg_arm_substep(WSubstep::Gfx);
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "gfx owns no psp register");

    /* No substep (None) and no registration: a parser and nothing more. */
    r3_wg_disarm_substep();
    CHECK(r3_wg_substep() == WSubstep::None, "disarm_substep clears the owner");
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "unowned register denied");
    CHECK(r3_wg_classify(FW_START, 1u, WClass::Data, "x") == WVerdict::Deny,
          "unowned layout denied");
    /* 3 smu + 1 later sdma + 1 later mes + 1 gfx + 2 unowned = 8 owner mismatches. */
    CHECK(r3_writeguard_deny_count() == 8, "eight owner violations denied");
    CHECK(r3_writeguard_txn_count() == 3, "three owner-matched writes allowed");
}

static void test_stage2_disarm(void)
{
    arm_stage2();
    r3_wg_arm_substep(WSubstep::Psp);
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::TxnRead, "armed");
    r3_writeguard_disarm();
    CHECK(r3_wg_classify(GPCOM, 1u, WClass::Protocol, "x") == WVerdict::Deny,
          "disarmed denies a register window");
    CHECK(r3_wg_classify(FW_START, 1u, WClass::Data, "x") == WVerdict::Deny,
          "disarmed denies a layout grant");
    CHECK(r3_wg_classify(0x6u, 1u, WClass::Access, "x") == WVerdict::Deny,
          "disarmed denies the access window");
    CHECK(r3_writeguard_deny_count() == 3, "three disarmed denies");
    CHECK(r3_writeguard_txn_count() == 1, "txn count kept across disarm");
}

/* ------------------------------------------------------------------------- manifest (T10) */

static const R3WMEntry *find_entry(const R3WManifest *m, WSubstep owner, WClass cls,
                                   uint32_t addr, uint32_t len, WMStatus st, const char *tag)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const R3WMEntry &e = m->entries[i];
        if (e.owner == owner && e.cls == cls && e.addr == addr &&
            (e.end_off - e.addr + 1u) == len && e.status == st && r3_wm_streq(e.tag, tag))
            return &e;
    }
    return nullptr;
}

static uint32_t deny_lines(const R3WManifest *m)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->entries[i].status == WMStatus::Deny)
            n++;
    return n;
}

static void test_manifest_merge(void)
{
    char text[R3_WM_TEXT_BYTES];
    arm_stage2();
    r3_wg_arm_substep(WSubstep::Psp);
    CHECK(r3_wg_substep() == WSubstep::Psp, "psp substep armed for the merge test");

    const R3WManifest *m = r3_writeguard_manifest();
    CHECK(m != nullptr, "writeguard owns a write manifest");
    if (!m)
        return;
    const uint32_t before = m->count;

    /* A5: three sub-range writes into one Data grant aggregate into one grant-interval line. */
    r3_wg_classify(FW_START + 1u, 2u, WClass::Data, "fw-a");
    r3_wg_classify(FW_START, FW_LEN, WClass::Data, "fw-b");
    r3_wg_classify(FW_START + 6u, 1u, WClass::Data, "fw-b");
    CHECK(m->count == before + 1u, "three layout writes merge into one entry (was %u now %u)",
          before, m->count);
    const R3WMEntry *d = find_entry(m, WSubstep::Psp, WClass::Data, FW_START, FW_LEN,
                                    WMStatus::Allow, FW_TAG);
    CHECK(d != nullptr, "aggregated layout entry present");
    CHECK(d && d->count == 3u, "aggregated count 3 (got %u)", d ? d->count : 0u);
    CHECK(d && d->ctx && strcmp(d->ctx, "fw-a") == 0, "ctx is the first-seen label");

    /* ③④ are per-write lines: repeats do not merge (§6.2). */
    const uint32_t n2 = m->count;
    r3_wg_classify(GPCOM, 1u, WClass::Protocol, "c2p-b");
    CHECK(m->count == n2 + 1u, "first gpcom write is a new entry");
    const R3WMEntry *p = find_entry(m, WSubstep::Psp, WClass::Protocol, GPCOM, 1u,
                                    WMStatus::Allow, "reg:psp-gpcom");
    CHECK(p && p->count == 1u, "first gpcom line count 1 (got %u)", p ? p->count : 0u);

    /* A second protocol write to the same address creates a new entry (count=1). */
    const uint32_t n3 = m->count;
    r3_wg_classify(GPCOM, 1u, WClass::Protocol, "c2p-c");
    CHECK(m->count == n3 + 1u, "second gpcom write is also a new entry");

    /* The merge rule excludes owner: the same address under ANOTHER substep is a new line. */
    CHECK(r3_wg_register_reg(SMU_MAIL, 1u, WClass::Protocol, WSubstep::Smu, "reg:psp-gpcom") == 0,
          "same tag registered for the smu substep");
    r3_wg_arm_substep(WSubstep::Smu);
    { const WVerdict v = r3_wg_classify(SMU_MAIL, 1u, WClass::Protocol, "smu-gpcom");
      CHECK(v == WVerdict::TxnRead, "smu write allowed by its own window"); }
    CHECK(find_entry(m, WSubstep::Smu, WClass::Protocol, SMU_MAIL, 1u, WMStatus::Allow,
                     "reg:smu-mailbox") != nullptr,
          "the smu line is its own entry");
    CHECK(p && p->count == 1u, "the psp line was not merged across substeps (got %u)",
          p ? p->count : 0u);

    /* ② and ④ at the same address, owner and tag: cls keeps them separate. */
    { const int rc = r3_wg_register_reg(FW_START, 1u, WClass::State, WSubstep::Psp,
                                       "reg:state-fw");
      CHECK(rc == 0, "state window registered at the layout start (rc=%d)", rc); }
    r3_wg_arm_substep(WSubstep::Psp);
    { const WVerdict v = r3_wg_classify(FW_START, 1u, WClass::State, "reg-hit");
      CHECK(v == WVerdict::TxnRead, "state write allowed by its own window"); }
    CHECK(find_entry(m, WSubstep::Psp, WClass::State, FW_START, 1u, WMStatus::Allow,
                     "reg:state-fw") != nullptr,
          "state register with the layout tag is its own line");
    CHECK(d && d->count == 3u, "layout line not merged with the state line (got %u)",
          d ? d->count : 0u);

    r3_wg_arm_substep(WSubstep::Smu);
    r3_wg_classify(SMU_MAIL, 1u, WClass::Protocol, "smu-msg");
    CHECK(find_entry(m, WSubstep::Smu, WClass::Protocol, SMU_MAIL, 1u, WMStatus::Allow,
                     "reg:smu-mailbox") != nullptr,
          "second smu protocol write is its own line");
    r3_wg_arm_substep(WSubstep::Psp);
    r3_wg_classify(PSP_RING, 4u, WClass::State, "ring");
    r3_wg_classify(PSP_RING, 4u, WClass::State, "ring");
    /* Two identical state writes produce two entries (count=1 each). */
    {
        uint32_t state_matches = 0;
        for (uint32_t i = 0; i < m->count; i++) {
            const R3WMEntry &e = m->entries[i];
            if (e.owner == WSubstep::Psp && e.cls == WClass::State &&
                e.addr == PSP_RING && (e.end_off - e.addr + 1u) == 4u &&
                e.status == WMStatus::Allow && r3_wm_streq(e.tag, "reg:psp-ring")) {
                CHECK(e.count == 1u, "state ring entry count 1 (got %u)", e.count);
                state_matches++;
            }
        }
        CHECK(state_matches == 2u, "two separate state ring entries (got %u)", state_matches);
    }
    CHECK(m->truncated == false && m->dropped == 0u, "not truncated");

    /* Every B2 class is present in the manifest (V10). ③④ are each count=1. */
    const size_t n = r3_wm_format(m, text, sizeof text);
    CHECK(n > 0 && n == strlen(text), "format length matches the C string");
    CHECK(strstr(text, "psp data 0x00800000+8 x3 allow layout:psp-stage2 fw-a\n") != nullptr,
          "data line (② merged)\n---\n%s---", text);
    CHECK(strstr(text, "psp protocol 0x00010040+1 x1 allow reg:psp-gpcom c2p-b\n") != nullptr,
          "protocol line (③ per-write, count=1)");
    CHECK(strstr(text, "psp protocol 0x00010040+1 x1 allow reg:psp-gpcom c2p-c\n") != nullptr,
          "second protocol line (③ per-write, separate entry)");
    CHECK(strstr(text, "psp state 0x00010060+4 x1 allow reg:psp-ring ring\n") != nullptr,
          "state line (④ per-write)");
    CHECK(strstr(text, "smu protocol 0x00100080+1 x1 allow reg:smu-mailbox smu-gpcom\n") != nullptr,
          "smu protocol line");
    CHECK(strstr(text, "smu protocol 0x00100080+1 x1 allow reg:smu-mailbox smu-msg\n") != nullptr,
          "second smu protocol line");

    CHECK(strstr(text, "truncated=") == nullptr, "no truncated marker");

    char small[32];
    CHECK(r3_wm_format(m, small, sizeof small) == sizeof small - 1u,
          "small buffer is filled and NUL-terminated");
    CHECK(small[sizeof small - 1u] == '\0', "small buffer NUL");
    CHECK(r3_wm_format(nullptr, text, sizeof text) == 0u, "null manifest formats empty");
    CHECK(r3_wm_format(m, text, 0u) == 0u, "zero cap formats nothing");
    CHECK(r3_wm_format(m, nullptr, 16u) == 0u, "null buffer formats nothing");
}
static void test_manifest_deny_each(void)
{
    char text[R3_WM_TEXT_BYTES];
    arm_stage2();
    r3_wg_arm_substep(WSubstep::Smu);
    /* Four identical refusals of a PSP register in the SMU substep: four lines, no merging. */
    for (int i = 0; i < 4; i++)
        CHECK(r3_wg_classify(GPCOM, 2u, WClass::Protocol, "stray") == WVerdict::Deny,
              "refusal %d", i);
    CHECK(r3_wg_classify(0x00ABCDE0u, 1u, WClass::Protocol, "off-list") == WVerdict::Deny,
          "off-list refused");

    const R3WManifest *m = r3_writeguard_manifest();
    CHECK(m != nullptr, "writeguard owns a write manifest");
    if (!m)
        return;
    CHECK(deny_lines(m) == 5u, "five DENY lines (got %u)", deny_lines(m));
    CHECK(m->count == 5u, "DENY lines are not merged (got %u)", m->count);
    CHECK(r3_writeguard_deny_count() == 5u, "deny counter matches");

    const size_t n = r3_wm_format(m, text, sizeof text);
    CHECK(n > 0 && n == strlen(text), "format length");
    unsigned hits = 0;
    /* The tag names the claiming window: the exact register is a 1-dword window, so a 2-dword
     * write does not fit it and is refused off-list (never merged, four separate lines). */
    for (const char *p = text; (p = strstr(p, "smu protocol 0x00010040+2 x1 DENY "
                                           "off-list stray\n")) != nullptr; p++)
        hits++;
    CHECK(hits == 4u, "four identical DENY lines (got %u)\n---\n%s---", hits, text);
    CHECK(strstr(text, "smu protocol 0x00010040+1 x1 DENY reg:psp-gpcom stray\n") == nullptr,
          "a refused write never borrows the register it does not fit");
    CHECK(strstr(text, "smu protocol 0x00abcde0+1 x1 DENY off-list off-list\n") != nullptr,
          "off-list line names the tag");
}

static void test_manifest_truncated(void)
{
    char text[R3_WM_TEXT_BYTES];
    r3_writeguard_install(SEG);
    r3_wg_arm_substep(WSubstep::Psp);
    const R3WManifest *m = r3_writeguard_manifest();
    CHECK(m != nullptr, "writeguard owns a write manifest");
    if (!m)
        return;
    for (uint32_t i = 0; i <= R3_WM_MAX; i++)
        r3_wg_classify(0x00F00000u + i, 1u, WClass::Protocol, "flood");
    CHECK(m->count == R3_WM_MAX, "table capped at R3_WM_MAX (got %u)", m->count);
    CHECK(m->truncated, "cap sets truncated");
    CHECK(m->dropped == 1u, "one dropped entry counted (got %u)", m->dropped);
    const size_t n = r3_wm_format(m, text, sizeof text);
    CHECK(n > 0 && n == strlen(text), "format length");
    const char *t = strstr(text, "truncated=1\n");
    CHECK(t != nullptr, "truncated marker in the export");
    CHECK(t && t[12] == '\0', "truncated marker ends the export");

    /* The cap is independent of the merge: an allowed repeat still merges once full, so a
     * known (owner, cls, addr, len, tag) is not counted as dropped. */
    r3_writeguard_install(SEG);
    r3_wg_register_reg(GPCOM, 1u, WClass::Protocol, WSubstep::Psp, "reg:psp-gpcom");
    r3_wg_arm_substep(WSubstep::Psp);
    for (uint32_t i = 0; i < R3_WM_MAX; i++)
        r3_wg_classify(0x00F00000u + i, 1u, WClass::Protocol, "flood");
    r3_wg_classify(GPCOM, 1u, WClass::Protocol, "c2p");
    CHECK(m->count == R3_WM_MAX, "full table drops a new address");
    CHECK(m->dropped == 1u, "dropped count (got %u)", m->dropped);
    r3_wg_classify(GPCOM, 1u, WClass::Protocol, "c2p");
    CHECK(m->dropped == 2u, "a refused repeat is not a merge (got %u)", m->dropped);
    CHECK(m->truncated, "still truncated");
}

/* Capacity: the worst-case full-chain success path must fit without truncation.
 * Estimate (tmp/programmer/S2-EVID/容量核算.md): PSP 49 + SMU 18 + SDMA 21 + MES 41 = 129
 * entries plus a DENY/boundary margin. The table must hold at least 160 and the text buffer
 * must hold 160 lines of the longest realistic line (~124 bytes). */
static void test_manifest_capacity(void)
{
    char text[R3_WM_TEXT_BYTES];
    CHECK(R3_WM_MAX >= 160u, "table cap covers the worst-case chain (R3_WM_MAX=%u)",
          R3_WM_MAX);
    CHECK(R3_WM_TEXT_BYTES >= 160u * 124u,
          "text buffer covers 160 worst-case lines (R3_WM_TEXT_BYTES=%u)", R3_WM_TEXT_BYTES);

    /* Fill with the worst-case count of distinct ③ entries and confirm no truncation and a
     * fully formatted export. */
    r3_writeguard_install(SEG);
    r3_wg_arm_substep(WSubstep::Psp);
    const R3WManifest *m = r3_writeguard_manifest();
    CHECK(m != nullptr, "writeguard owns a write manifest");
    if (!m)
        return;
    for (uint32_t i = 0; i < 160u; i++)
        r3_wg_classify(0x00F00000u + i, 1u, WClass::Protocol, "worst-case-chain");
    CHECK(m->count == 160u, "160 distinct entries stored (got %u)", m->count);
    CHECK(!m->truncated && m->dropped == 0u, "no truncation at the worst-case count");
    const size_t n = r3_wm_format(m, text, sizeof text);
    CHECK(n > 0 && n == strlen(text), "format length matches the C string");
    CHECK(strstr(text, "truncated=") == nullptr, "no truncated marker");
    /* Every entry must be present: count the newlines. */
    uint32_t lines = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n')
            lines++;
    CHECK(lines == 160u, "160 formatted lines (got %u)", lines);
}
// v1.7: a shared row (e.g. the HDP window) is usable by any armed substep, still class-checked.
static void test_shared_row(void)
{
    r3_writeguard_install_mm();
    CHECK(r3_wg_register_shared_reg(0x80000u, 1u, WClass::Protocol, WSubstep::Psp,
                                    "reg:hdp-flush") == 0,
          "shared row register");
    r3_wg_arm_substep(WSubstep::Sdma);
    CHECK(r3_wg_classify(0x80000u, 1u, WClass::Protocol, "shared-other-owner") ==
              WVerdict::TxnRead,
          "shared row allows another armed owner");
    CHECK(r3_wg_classify(0x80000u, 1u, WClass::State, "shared-wrong-class") !=
              WVerdict::TxnRead,
          "shared row is still class-checked");
    r3_wg_disarm_substep();
}

/* --- Step3: 期望校验测试 --------------------------------------------------------------- */

/* expect_writes + expect_declared_addrs in one call. These tests declare their rows in the
 * fallback form ({nullptr, 0, 0, <dword addr>, ...}) — the file header notes the stand-in
 * dword address space — so rows[i].off IS the absolute address the guard should treat as
 * the declared set. Production substeps resolve golden/fallback through the IP base table
 * instead (r3_wg_resolve_row_addrs); both feed the same static declared-address contract. */
static int expect_row_set(const R3WriteRowReg *rows, uint32_t count,
                          const R3WriteRowLayout *layouts, uint32_t layout_count,
                          WSubstep owner)
{
    uint32_t addrs[R3_WG_EXPECT_MAX] = {0};
    if (rows) {
        for (uint32_t i = 0; i < count && i < R3_WG_EXPECT_MAX; i++)
            addrs[i] = rows[i].off;
    }
    const int rc = r3_wg_expect_writes(rows, count, layouts, layout_count, owner);
    if (rc != 0)
        return rc;
    return r3_wg_expect_declared_addrs(addrs, count);
}

static void arm_stage2_basic(void)
{
    r3_writeguard_install_mm();
    r3_wg_register_reg(0x100u, 1u, WClass::State, WSubstep::Sdma, "reg:sdma");
    r3_wg_register_reg(0x104u, 1u, WClass::Protocol, WSubstep::Sdma, "reg:sdma");
    r3_wg_register_reg(0x200u, 1u, WClass::State, WSubstep::Mes, "reg:mes");
    r3_wg_register_reg(0x300u, 1u, WClass::Protocol, WSubstep::Smu, "reg:smu");
}

/* Test: expectation hit tracking and verify. */
static void test_expect_hit(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State,    WNeed::Required,      "s1-init"},
        {nullptr, 0, 0, 0x104u, 1u, WClass::Protocol, WNeed::OnSuccessPath, "s1-doorbell"},
    };
    constexpr uint32_t count = sizeof(rows) / sizeof(rows[0]);

    r3_writeguard_install_mm();
    arm_stage2_basic();
    r3_wg_arm_substep(WSubstep::Sdma);

    CHECK(expect_row_set(rows, count, nullptr, 0, WSubstep::Sdma) == 0, "expect ok");

    /* Write both expected rows */
    CHECK(r3_wg_classify(0x100u, 1u, WClass::State, "s1-init") == WVerdict::TxnRead, "hit s1-init");
    CHECK(r3_wg_classify(0x104u, 1u, WClass::Protocol, "s1-doorbell") == WVerdict::TxnRead, "hit doorbell");

    const int hit = r3_wg_verify_expectations();
    CHECK(hit == 2, "both rows hit (got %d)", hit);
    CHECK(r3_wg_first_unmet() == -1, "no unmet (got %d)", r3_wg_first_unmet());
    CHECK(r3_wg_extra_write_count() == 0u, "no extra writes (got %u)", r3_wg_extra_write_count());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: extra writes detected (P2 step 4). The known set is the STATIC declared set
 * (r3_wg_expect_declared_addrs), not a registration-time snapshot: any allow whose
 * address is not a declared row and not layout/shared/other-owner is an extra,
 * regardless of when its window was registered. */
static void test_expect_extra(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);

    /* Declared row hits. 0x104 is registered but NOT declared: it is an extra. */
    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");
    r3_wg_classify(0x104u, 1u, WClass::Protocol, "s1-doorbell");
    CHECK(r3_wg_extra_write_count() == 1u, "undeclared registered window is extra (got %u)",
          r3_wg_extra_write_count());

    /* A window registered after expect is likewise undeclared -> extra. */
    CHECK(r3_wg_register_reg(0x108u, 1u, WClass::State, WSubstep::Sdma, "reg:sdma") == 0,
          "register post-expect window");
    r3_wg_classify(0x108u, 1u, WClass::State, "s1-post-expect");
    CHECK(r3_wg_extra_write_count() == 2u, "post-expect undeclared window is extra (got %u)",
          r3_wg_extra_write_count());

    CHECK(r3_wg_verify_expectations() == 1, "one hit");

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: WNeed path filtering. */
static void test_expect_path(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State,    WNeed::Required,      "s1-init"},
        {nullptr, 0, 0, 0x104u, 1u, WClass::Protocol, WNeed::OnFailure,    "s1-fail"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 2, nullptr, 0, WSubstep::Sdma);

    r3_wg_set_path("failure");
    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");       // Required always counts
    // s1-fail is OnFailure but NOT written

    CHECK(r3_wg_verify_expectations() == 1, "Required hit, OnFailure not written -> 1 (got %d)",
          r3_wg_verify_expectations());
    CHECK(r3_wg_first_unmet() == 1, "unmet is row 1 (OnFailure) (got %d)", r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}


/* Test: ② Data write inside a registered layout grant is never extra (P2 rule 1). */
static void test_expect_layout_no_extra(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
    };
    const R3WriteRowLayout layouts[] = {
        {"ring", WSubstep::Sdma, WNeed::Required},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    CHECK(r3_wg_register_layout(0x8000u, 0x20u, WSubstep::Sdma, "layout:sdma") == 0,
          "register layout grant");
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 1, layouts, 1, WSubstep::Sdma);

    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");           // declared reg row
    r3_wg_classify(0x8004u, 2u, WClass::Data, "s1-ring-stage");     // Data inside layout

    CHECK(r3_wg_verify_expectations() == 2, "reg + layout rows hit (got %d)",
          r3_wg_verify_expectations());
    CHECK(r3_wg_extra_write_count() == 0u, "layout Data write is not extra (got %u)",
          r3_wg_extra_write_count());
    CHECK(r3_wg_first_unmet() == -1, "all hit (got %d)", r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: the declared-address set decouples met/extra from registration ORDER. The
 * substep feeds addresses via r3_wg_expect_declared_addrs (B), so writing 0x100/0x104
 * after registering them in ANY order must hit both rows and never extra. */
static void test_expect_registration_order_independent(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State,    WNeed::Required,      "s1-init"},
        {nullptr, 0, 0, 0x104u, 1u, WClass::Protocol, WNeed::OnSuccessPath, "s1-doorbell"},
    };
    constexpr uint32_t count = sizeof(rows) / sizeof(rows[0]);
    r3_writeguard_install_mm();
    /* Deliberately register in REVERSE order first. */
    CHECK(r3_wg_register_reg(0x104u, 1u, WClass::Protocol, WSubstep::Sdma, "reg:sdma") == 0,
          "register row 1 first");
    CHECK(r3_wg_register_reg(0x100u, 1u, WClass::State, WSubstep::Sdma, "reg:sdma") == 0,
          "register row 0 second");
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, count, nullptr, 0, WSubstep::Sdma);

    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");
    r3_wg_classify(0x104u, 1u, WClass::Protocol, "s1-doorbell");

    CHECK(r3_wg_verify_expectations() == 2, "both rows hit regardless of register order (got %d)",
          r3_wg_verify_expectations());
    CHECK(r3_wg_extra_write_count() == 0u, "no extra despite reverse registration (got %u)",
          r3_wg_extra_write_count());
    CHECK(r3_wg_first_unmet() == -1, "all hit (got %d)", r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: shared reg write is never extra (P2 rule 2). */
static void test_expect_shared_no_extra(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    CHECK(r3_wg_register_shared_reg(0x40000u, 1u, WClass::Protocol, WSubstep::Psp,
                                    "reg:hdp-flush") == 0,
          "register shared HDP row");
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);

    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");       // declared reg row
    r3_wg_classify(0x40000u, 1u, WClass::Protocol, "s1-hdp");   // shared, other owner

    CHECK(r3_wg_verify_expectations() == 1, "declared row hit (got %d)",
          r3_wg_verify_expectations());
    CHECK(r3_wg_extra_write_count() == 0u, "shared write is not extra (got %u)",
          r3_wg_extra_write_count());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: a write hitting another owner's registered window is never extra (P2 rule 3).
 * The window must be shared for classify() to allow it while Sdma is armed. */
static void test_expect_other_owner_no_extra(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    CHECK(r3_wg_register_shared_reg(0x50000u, 1u, WClass::State, WSubstep::Mes,
                                    "reg:mes-shared") == 0,
          "register other-owner shared window");
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);

    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");       // declared reg row
    r3_wg_classify(0x50000u, 1u, WClass::State, "s1-mes-shared"); // other owner

    CHECK(r3_wg_extra_write_count() == 0u, "other-owner write is not extra (got %u)",
          r3_wg_extra_write_count());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: declared reg row not written reports a miss (first_unmet). */
static void test_expect_miss(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
        {nullptr, 0, 0, 0x104u, 1u, WClass::Protocol, WNeed::Required, "s1-doorbell"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(rows, 2, nullptr, 0, WSubstep::Sdma);

    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");       // only row 0 written

    CHECK(r3_wg_verify_expectations() == 1, "one hit (got %d)", r3_wg_verify_expectations());
    CHECK(r3_wg_first_unmet() == 1, "unmet is row 1 (got %d)", r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: declared layout row not written reports a layout-index miss via first_unmet.
 * Layout tracking is owner-granular (a Data write marks the owner's layout rows seen),
 * so the miss case is "no Data write at all" -> the first layout row (index row_count). */
static void test_expect_layout_miss(void)
{
    const R3WriteRowLayout layouts[] = {
        {"ring", WSubstep::Sdma, WNeed::Required},
        {"fence", WSubstep::Sdma, WNeed::Required},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    CHECK(r3_wg_register_layout(0x8000u, 0x20u, WSubstep::Sdma, "layout:sdma-ring") == 0,
          "register ring layout");
    CHECK(r3_wg_register_layout(0x9000u, 0x10u, WSubstep::Sdma, "layout:sdma-fence") == 0,
          "register fence layout");
    r3_wg_arm_substep(WSubstep::Sdma);
    expect_row_set(nullptr, 0, layouts, 2, WSubstep::Sdma);

    /* No Data write at all: both layout rows unmet; first_unmet lands on the layout
     * segment (index == row_count), which the caller can map back to the layout tag. */
    CHECK(r3_wg_verify_expectations() == 0, "no layout hit (got %d)",
          r3_wg_verify_expectations());
    CHECK(r3_wg_first_unmet() == 0, "unmet is layout row 0 ('ring') (got %d)",
          r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* Test: r3_wg_verify_and_clear strict gate - block on strict=true, record on false.
 * Failure path must never block even under strict. */
static void test_expect_strict_gate(void)
{
    const R3WriteRowReg rows[] = {
        {nullptr, 0, 0, 0x100u, 1u, WClass::State, WNeed::Required, "s1-init"},
    };
    r3_writeguard_install_mm();
    arm_stage2_basic();
    r3_wg_arm_substep(WSubstep::Sdma);
    R3EvidenceLines ev;
    r3_ev_init(&ev);
    n48log_host_reset();
    PropCapture cap;
    prop_capture_init(&cap);
    R3EvSink sink;
    sink.set_property = prop_capture;
    sink.user = &cap;
    R3SubstepEnv env;
    memset(&env, 0, sizeof env);
    env.ev = &ev;
    env.sink = &sink;
    /* strict=false: miss records only, verify_and_clear returns 0. */
    env.strict_expect = false;
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);
    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");   // declared row hit
    const int rc_false = r3_wg_verify_and_clear(&env, 1);
    CHECK(rc_false == 0, "strict=false must not block (got %d)", rc_false);
    /* Real hardware runs with strict_expect=false and MUST still record the verdict
     * (observations are never gated by the strict gate). This is that exact branch. */
    CHECK(n48log_host_has_tag("expect-verify"),
          "strict=false verify must publish expect-verify evidence");

    /* strict=true success path: a miss (declared row NOT written at all) blocks. */
    env.strict_expect = true;
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);
    /* no classify() at all -> declared row 0 stays unmet */
    const int rc_block = r3_wg_verify_and_clear(&env, 1);
    CHECK(rc_block == 1, "strict=true must block on miss (got %d)", rc_block);

    /* strict=true failure path: expected rows skipped, never block. */
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);
    r3_wg_set_path("failure");
    (void)r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");
    const int rc_fail = r3_wg_verify_and_clear(&env, 1);
    CHECK(rc_fail == 0, "failure path must not block (got %d)", rc_fail);

    /* Clean pass under strict: declared row written, no extra. */
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);
    r3_wg_classify(0x100u, 1u, WClass::State, "s1-init");
    const int rc_clean = r3_wg_verify_and_clear(&env, 1);
    CHECK(rc_clean == 0, "clean pass under strict (got %d)", rc_clean);

    /* One more pass that writes a post-expect window: that IS an extra (P2 step 4) and
     * the evidence must carry it. */
    expect_row_set(rows, 1, nullptr, 0, WSubstep::Sdma);
    CHECK(r3_wg_register_reg(0x108u, 1u, WClass::State, WSubstep::Sdma,
                             "reg:sdma") == 0, "register post-expect window");
    (void)r3_wg_classify(0x108u, 1u, WClass::State, "s1-post-expect");
    const int rc_extra = r3_wg_verify_and_clear(&env, 1);
    CHECK(rc_extra == 1, "post-expect extra must block (got %d)", rc_extra);

    /* P12: every verify publishes the verdict line into the n48log ring (n48log_stub on
     * host), where the runner can audit it (record-only on real hardware). */
    {
        bool meta = false, miss = false, extra_line = false;
        const char *p = n48log_host_text();
        while (p != nullptr && *p != '\0') {
            const char *eol = strchr(p, '\n');
            const size_t len = eol != nullptr ? (size_t)(eol - p) : strlen(p);
            if (len > strlen("expect-verify: ") &&
                strncmp(p, "expect-verify: ", strlen("expect-verify: ")) == 0) {
                char line[512];
                const size_t copy = len < sizeof line - 1 ? len : sizeof line - 1;
                memcpy(line, p, copy);
                line[copy] = '\0';
                if (strstr(line, "declared=") && strstr(line, "extra="))
                    meta = true;
                if (strstr(line, "miss: s1-init"))
                    miss = true;
                if (strstr(line, "extra: s1-post-expect"))
                    extra_line = true;
            }
            p = eol != nullptr ? eol + 1 : nullptr;
        }
        CHECK(meta, "expect-verify metadata line missing from evidence");
        CHECK(miss, "expect-verify miss line missing");
        CHECK(extra_line, "expect-verify extra line missing");
        /* P12 dedicated property (方案 v3 B.4), published through env->sink. The final
         * verify is the post-expect-extra case, so it carries both a miss and an extra. */
        CHECK(cap.calls > 0, "N48R3EVID-expect-verify property never published");
        CHECK(strcmp(cap.key, "N48R3EVID-expect-verify") == 0,
              "property key is '%s'", cap.key);
        CHECK(strstr(cap.value, "declared=") && strstr(cap.value, "path=success"),
              "property missing metadata line: '%s'", cap.value);
        CHECK(strstr(cap.value, "miss: s1-init"), "property missing miss line");
        CHECK(strstr(cap.value, "extra: s1-post-expect"), "property missing extra line");
    }

    r3_wg_disarm_substep();
}
int main(void)
{
    test_install();
    test_mm_only_arm();
    test_table();
    test_allow();
    test_deny();
    test_cross_window();
    test_unarmed();
    test_adapter();
    test_overflow();
    test_shared_row();

    test_stage2_register_reject();
    test_stage2_allow_four_classes();
    test_stage2_layout_edge();
    test_stage2_owner();
    test_stage2_disarm();
    test_manifest_merge();
    test_manifest_deny_each();
    test_manifest_truncated();
    test_manifest_capacity();

    test_expect_hit();
    test_expect_extra();

    test_expect_path();
    test_expect_layout_no_extra();
    test_expect_registration_order_independent();
    test_expect_shared_no_extra();
    test_expect_other_owner_no_extra();
    test_expect_miss();
    test_expect_layout_miss();
    test_expect_strict_gate();

    if (failures) {
        printf("test_writeguard: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_writeguard: %d checks passed\n", checks);
    return 0;
}
