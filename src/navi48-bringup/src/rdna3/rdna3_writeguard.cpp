// rdna3_writeguard.cpp - see rdna3_writeguard.h for the contract.
//
// The stage-1 window table is the complete set of registers src/dcn314/dcn314_aux_min.c writes.
// Every register is a one-dword AUX request/data register in DCN segment 2 (BASE_IDX 2):
//
//   AUX_CONTROL(i)            +0x0   dcn314_aux_min.c aux_acquire (enable/reset)
//   AUX_SW_CONTROL(i)         +0x1   submit request / arm GO
//   AUX_ARB_CONTROL(i)        +0x2   acquire/release the AUX engine
//   AUX_INTERRUPT_CONTROL(i)  +0x3   acknowledge SW_DONE
//   AUX_SW_DATA(i)            +0x6   request byte / payload
//
// AUX_SW_STATUS (+0x4) is read-only and is deliberately absent. Instance stride is 0x1c.
// 5 instances x 5 registers = 25 one-dword windows, all tagged "aux-ddc-read".
//
// Stage 2 adds R3WgGrant windows: Data layout intervals (range=true, "layout:...") and explicit
// Protocol/State/Access registers (range=false, "reg:..."). Explicit windows match exactly;
// only Data grants admit sub-ranges. See the header for the five structural constraints.
#include "rdna3_writeguard.h"

#include "rdna3_wmanifest.h"
#include "rdna3_substep.h"
#include "golden_values.h"

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <cstdio>
#include <string.h>

#endif


namespace {

constexpr const char *kAuxTag = "aux-ddc-read";
constexpr const char *kMmTag = "mm-indirect";
constexpr const char *kOffListTag = "off-list";
constexpr const char *kDisarmedTag = "disarmed";
constexpr const char *kInvalidTag = "invalid";
constexpr const char *kLayoutPrefix = "layout:";
constexpr const char *kRegPrefix = "reg:";

// MM_INDEX/MM_DATA/MM_INDEX_HI are absolute BAR5 dwords (base Navi48Bringup.cpp:160-165:
// 0x0/0x1/0x6). They are the only write path T2 uses to pull the VRAM TMR block into RAM.
constexpr R3WgWindow kWindows[R3_WG_WINDOW_COUNT] = {
    { 0u, 0x0u, 1u, kMmTag, true },    /* MM_INDEX */
    { 0u, 0x1u, 1u, kMmTag, true },    /* MM_DATA */
    { 0u, 0x6u, 1u, kMmTag, true },    /* MM_INDEX_HI */
    /* AUX0 */
    { 2u, 0x1f50u, 1u, kAuxTag, false },   /* AUX0_AUX_CONTROL */
    { 2u, 0x1f51u, 1u, kAuxTag, false },   /* AUX0_AUX_SW_CONTROL */
    { 2u, 0x1f52u, 1u, kAuxTag, false },   /* AUX0_AUX_ARB_CONTROL */
    { 2u, 0x1f53u, 1u, kAuxTag, false },   /* AUX0_AUX_INTERRUPT_CONTROL */
    { 2u, 0x1f56u, 1u, kAuxTag, false },   /* AUX0_AUX_SW_DATA */
    /* AUX1 */
    { 2u, 0x1f6cu, 1u, kAuxTag, false },   /* AUX1_AUX_CONTROL */
    { 2u, 0x1f6du, 1u, kAuxTag, false },   /* AUX1_AUX_SW_CONTROL */
    { 2u, 0x1f6eu, 1u, kAuxTag, false },   /* AUX1_AUX_ARB_CONTROL */
    { 2u, 0x1f6fu, 1u, kAuxTag, false },   /* AUX1_AUX_INTERRUPT_CONTROL */
    { 2u, 0x1f72u, 1u, kAuxTag, false },   /* AUX1_AUX_SW_DATA */
    /* AUX2 */
    { 2u, 0x1f88u, 1u, kAuxTag, false },   /* AUX2_AUX_CONTROL */
    { 2u, 0x1f89u, 1u, kAuxTag, false },   /* AUX2_AUX_SW_CONTROL */
    { 2u, 0x1f8au, 1u, kAuxTag, false },   /* AUX2_AUX_ARB_CONTROL */
    { 2u, 0x1f8bu, 1u, kAuxTag, false },   /* AUX2_AUX_INTERRUPT_CONTROL */
    { 2u, 0x1f8eu, 1u, kAuxTag, false },   /* AUX2_AUX_SW_DATA */
    /* AUX3 */
    { 2u, 0x1fa4u, 1u, kAuxTag, false },   /* AUX3_AUX_CONTROL */
    { 2u, 0x1fa5u, 1u, kAuxTag, false },   /* AUX3_AUX_SW_CONTROL */
    { 2u, 0x1fa6u, 1u, kAuxTag, false },   /* AUX3_AUX_ARB_CONTROL */
    { 2u, 0x1fa7u, 1u, kAuxTag, false },   /* AUX3_AUX_INTERRUPT_CONTROL */
    { 2u, 0x1faau, 1u, kAuxTag, false },   /* AUX3_AUX_SW_DATA */
    /* AUX4 */
    { 2u, 0x1fc0u, 1u, kAuxTag, false },   /* AUX4_AUX_CONTROL */
    { 2u, 0x1fc1u, 1u, kAuxTag, false },   /* AUX4_AUX_SW_CONTROL */
    { 2u, 0x1fc2u, 1u, kAuxTag, false },   /* AUX4_AUX_ARB_CONTROL */
    { 2u, 0x1fc3u, 1u, kAuxTag, false },   /* AUX4_AUX_INTERRUPT_CONTROL */
    { 2u, 0x1fc6u, 1u, kAuxTag, false },   /* AUX4_AUX_SW_DATA */
};

/* Step3: expectation tracking entry. */
struct ExpectEntry {
    WNeed    need;
    const char *ctx;   /* matches classify() ctx */
    bool     met;
};

/* Extra write entry. */
struct ExtraEntry {
    const char *ctx;
};

struct State {
    bool armed;
    bool on[R3_WG_WINDOW_COUNT];
    uint32_t lo[R3_WG_WINDOW_COUNT];
    uint32_t hi[R3_WG_WINDOW_COUNT];
    uint64_t denies;
    uint64_t txns;
    const char *last_tag;
    const char *last_context;
    WSubstep owner;
    R3WgLogFn log_fn;       /* D-06/B1: optional DENY log sink                  */
    void *log_user;
    R3WgGrant grants[R3_WG_GRANT_MAX];
    uint32_t grant_count;
    R3WManifest manifest;
    /* Step3: expectation state. */
    const R3WriteRowReg *expect_rows;      /* pointer to the rows (not owned) */
    const R3WriteRowLayout *expect_layouts; /* pointer to the layouts (not owned) */
    const R3WriteRowAbs *expect_abs;       /* pointer to absolute-address rows (not owned) */
    uint32_t expect_row_count;
    uint32_t expect_layout_count;
    uint32_t expect_abs_count;
    WSubstep expect_owner;
    bool     expect_met[R3_WG_EXPECT_MAX]; /* one-bit per reg row: was its address written? */
    bool     expect_layout_met[R3_WG_EXPECT_MAX]; /* one-bit per layout row (owner granularity) */
    bool     expect_abs_met[R3_WG_EXPECT_MAX];    /* one-bit per abs row */
    uint32_t expect_addrs[R3_WG_EXPECT_MAX]; /* declared row addresses, order = declaration */
    uint32_t expect_addr_count;            /* declared row count (== expect_row_count) */
    char     expect_evid[R3_EVID_TEXT_BYTES]; /* P12: assembled N48R3EVID-expect-verify */
    uint32_t expect_evid_used;               /* bytes used in expect_evid (NUL excluded) */
    uint32_t expect_evid_dropped;            /* lines dropped when text is full */
    const char *expect_path;               /* current path label ("success"/"retry"/"failure") */
    bool     expect_armed;                 /* true between expect_writes and expect_clear */
    const char *expect_extra_ctx[R3_WG_EXPECT_MAX]; /* extra write ctx values */
    uint32_t expect_extra_count;
};
State g;

void zero_state(void)
{
    uint8_t *p = reinterpret_cast<uint8_t *>(&g);
    for (uintptr_t i = 0; i < sizeof(g); i++)
        p[i] = 0;
}

void arm_window(uint32_t i, uint32_t base)
{
    g.lo[i] = base;
    g.hi[i] = base + static_cast<uint32_t>(kWindows[i].dwords) - 1u;
    g.on[i] = true;
}

bool starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
        return false;
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

bool valid_class(WClass c)
{
    return c == WClass::Access || c == WClass::Data ||
           c == WClass::Protocol || c == WClass::State;
}

bool valid_owner(WSubstep s)
{
    return s == WSubstep::Psp || s == WSubstep::Smu || s == WSubstep::Gfx ||
           s == WSubstep::Sdma || s == WSubstep::Mes;
}


WVerdict note_deny(const char *ctx, uint32_t addr, uint32_t dwords, WClass cls,
                   const char *tag)
{
    g.denies++;
    g.last_tag = nullptr;
    g.last_context = nullptr;
    r3_wm_record(&g.manifest, addr, dwords ? dwords : 1u, g.owner, cls,
                 WMStatus::Deny, tag, ctx);
    if (g.log_fn) {
        char line[128];
        snprintf(line, sizeof line,
                 "N48R3LOG writeguard: DENY %s @0x%08x+%u ctx=%s owner=%s cls=%s",
                 tag ? tag : "?", addr, dwords, ctx ? ctx : "?",
                 r3_wg_substep_name(g.owner), r3_wg_class_name(cls));
        g.log_fn(g.log_user, line);
    }
    return WVerdict::Deny;
}

} // namespace

void r3_wg_set_log(R3WgLogFn fn, void *user)
{
    g.log_fn = fn;
    g.log_user = user;
}

int r3_writeguard_install(const uint32_t seg[R3_WG_SEGS])
{
    if (!seg)
        return -1;
    zero_state();
    r3_wm_init(&g.manifest);
    for (uint32_t i = 0; i < R3_WG_WINDOW_COUNT; i++) {
        const uint32_t base = kWindows[i].absolute ? kWindows[i].offset
                                                   : seg[kWindows[i].base_idx] + kWindows[i].offset;
        arm_window(i, base);
    }
    g.armed = true;
    return 0;
}

int r3_writeguard_install_mm(void)
{
    zero_state();
    r3_wm_init(&g.manifest);
    for (uint32_t i = 0; i < R3_WG_MM_WINDOW_COUNT; i++)
        arm_window(i, kWindows[i].offset);
    g.armed = true;
    return 0;
}


// `matched` is the grant window that authorised this write (nullptr for legacy stage-1 AUX).
// The Step3 expectation tracking lives here because classify() is the single choke point
// where a write becomes ALLOWED. Both checks use the DECLARED ADDRESS as the key: tech plan
// v3 §B.2.1 (P5) declares ONE row per unique address, while `ctx` is only the manifest's
// per-operation label and may differ for re-writes of the same address. P2 exclusion order
// (§B.2.4): layout-Data / shared / other-owner writes are never extras.
WVerdict note_allow(const R3WgGrant *matched, const char *ctx, uint32_t addr,
                    uint32_t dwords, WClass cls, const char *wm_tag)
{
    g.txns++;
    g.last_tag = wm_tag;
    g.last_context = ctx;
    r3_wm_record(&g.manifest, addr, dwords, g.owner, cls, WMStatus::Allow, wm_tag, ctx);

    if (g.expect_armed) {
        // Positive check: a write at a DECLARED address marks that declared row as met.
        // expect_addrs holds the substep-resolved declaration row addresses in declaration
        // order (r3_wg_expect_declared_addrs), so index == declared row index; no
        // registration-order inference remains.
        int row = -1;
        for (uint32_t i = 0; i < g.expect_addr_count; i++) {
            if (g.expect_addrs[i] == addr) {
                row = static_cast<int>(i);
                break;
            }
        }
        if (row >= 0)
            g.expect_met[row] = true;

        // Absolute-address expectations (e.g. MES doorbell declarations) are also part of
        // the declared set: a match marks them seen, and the reverse check below treats
        // them as known (not extra).
        int abs_row = -1;
        if (g.expect_abs) {
            for (uint32_t i = 0; i < g.expect_abs_count && i < R3_WG_EXPECT_MAX; i++) {
                if (g.expect_abs[i].addr == addr) {
                    g.expect_abs_met[i] = true;
                    abs_row = static_cast<int>(i);
                    break;
                }
            }
        }
        // Layout expectations: a Data write inside an expected layout grant marks the
        // owner's layout rows seen (registration carries only the IP-level layout tag,
        // not the plan grant tag, so tracking is owner-granular).
        if (matched && matched->range && cls == WClass::Data &&
            matched->owner == g.expect_owner) {
            for (uint32_t i = 0; i < g.expect_layout_count && i < R3_WG_EXPECT_MAX; i++)
                g.expect_layout_met[i] = true;
        }

        // Reverse check (P2 order):
        //   1. Data inside a registered layout grant  -> never extra
        //   2. shared reg (HDP)                       -> never extra
        //   3. another owner's window                 -> never extra
        //   4. legacy stage-1 AUX window              -> not a substep write
        //   5. otherwise, only an address OUTSIDE the declared set is an extra.
        const bool data_layout = matched && matched->range && cls == WClass::Data;
        const bool shared = matched && matched->shared;
        const bool other_owner = matched && matched->owner != g.expect_owner;
        const bool stage1 = !matched;
        if (row < 0 && abs_row < 0 && !data_layout && !shared && !other_owner &&
            !stage1 && g.expect_extra_count < R3_WG_EXPECT_MAX) {
            g.expect_extra_ctx[g.expect_extra_count++] = ctx;
        }
    }

    return WVerdict::TxnRead;
}


void r3_writeguard_disarm(void)
{
    g.armed = false;
}

int r3_wg_register_grant(const R3WgGrant *grant)
{
    if (!grant || !grant->tag || grant->dwords == 0u)
        return -1;
    if ((uint64_t)grant->start + (uint64_t)grant->dwords - 1u > 0xFFFFFFFFull)
        return -1;
    if (!valid_owner(grant->owner) || !valid_class(grant->cls))
        return -1;

    if (grant->range) {
        if (grant->cls != WClass::Data || !starts_with(grant->tag, kLayoutPrefix))
            return -1;
    } else {
        if (grant->cls == WClass::Data || !starts_with(grant->tag, kRegPrefix))
            return -1;
    }

    if (g.grant_count >= R3_WG_GRANT_MAX)
        return -2;
    g.grants[g.grant_count++] = *grant;
    return 0;
}

int r3_wg_register_layout(uint32_t start, uint32_t dwords, WSubstep owner, const char *tag)
{
    const R3WgGrant grant = { start, dwords, WClass::Data, owner, tag, true, false };
    return r3_wg_register_grant(&grant);
}

int r3_wg_register_reg(uint32_t addr, uint32_t dwords, WClass cls, WSubstep owner,
                       const char *tag)
{
    const R3WgGrant grant = { addr, dwords, cls, owner, tag, false, false };
    return r3_wg_register_grant(&grant);
}

int r3_wg_register_shared_reg(uint32_t addr, uint32_t dwords, WClass cls, WSubstep owner,
                              const char *tag)
{
    const R3WgGrant grant = { addr, dwords, cls, owner, tag, false, true };
    return r3_wg_register_grant(&grant);
}

uint32_t r3_wg_grant_count(void)
{
    return g.grant_count;
}

const R3WgGrant *r3_wg_grant(uint32_t index)
{
    return index < g.grant_count ? &g.grants[index] : nullptr;
}

void r3_wg_arm_substep(WSubstep s)
{
    g.owner = s;
}

void r3_wg_disarm_substep(void)
{
    g.owner = WSubstep::None;
}

WSubstep r3_wg_substep(void)
{
    return g.owner;
}

WVerdict r3_wg_classify(uint64_t pa, uint32_t size, WClass cls, const char *ctx)
{
    if (!g.armed)
        return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, kDisarmedTag);

    if (size == 0u || pa > 0xFFFFFFFFull ||
        pa + static_cast<uint64_t>(size) - 1u > 0xFFFFFFFFull)
        return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, kInvalidTag);

    const uint64_t hi = pa + static_cast<uint64_t>(size) - 1u;

    // One pass, address first: the window that fully contains [pa, hi] decides the verdict on
    // its own (owner + class must match), so a write is never re-tried against a second window.
    // A window that only partially contains the write was not written and claims nothing.
    // 1. Legacy stage-1 windows: ① Access, owner-less (WSubstep::None) only.
    for (uint32_t i = 0; i < R3_WG_WINDOW_COUNT; i++) {
        if (!g.on[i] || pa < g.lo[i] || hi > g.hi[i])
            continue;
        if (g.owner == WSubstep::None && cls == WClass::Access)
            return note_allow(nullptr, ctx, static_cast<uint32_t>(pa), size,
                              WClass::Access, kWindows[i].tag);
        return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, kWindows[i].tag);
    }

    // 2. ③④ explicit register windows (and an Access register window): exact addr+width, and
    //    only the substep that registered it. Several substeps may register the same address;
    //    the row whose owner is the armed substep and whose class matches is the one written.
    //    A row that fits address/width but not owner/class refuses the write; no row at all
    //    leaves it to the layout step below.
    const char *fits_tag = nullptr;
    for (uint32_t i = 0; i < g.grant_count; i++) {
        const R3WgGrant &gr = g.grants[i];
        if (gr.range || pa != gr.start || size != gr.dwords)
            continue;
        if (gr.cls == cls && (gr.shared || gr.owner == g.owner))
            return note_allow(&gr, ctx, static_cast<uint32_t>(pa), size, cls, gr.tag);
        fits_tag = gr.tag;
    }
    if (fits_tag)
        return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, fits_tag);

        // 3. ② Data layout grants: Data only, any sub-range inside the interval (either edge).
    for (uint32_t i = 0; i < g.grant_count; i++) {
        const R3WgGrant &gr = g.grants[i];
        if (!gr.range)
            continue;
        const uint64_t end = static_cast<uint64_t>(gr.start) + gr.dwords - 1u;
        if (pa < gr.start || hi > end)
            continue;
        if (gr.owner == g.owner && cls == WClass::Data)
            // A5: aggregate the whole grant interval, not each dword address.
            return note_allow(&gr, ctx, gr.start, gr.dwords, WClass::Data, gr.tag);
        return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, gr.tag);
    }    return note_deny(ctx, static_cast<uint32_t>(pa), size, cls, kOffListTag);
}

WVerdict r3_write_classify(uint64_t pa, uint32_t size, const char *tag)
{
    return r3_wg_classify(pa, size, WClass::Access, tag);
}

extern "C" bool r3_writeguard_write_ok(void *cookie, uint32_t abs_dword, uint32_t value)
{
    (void)cookie;
    (void)value;
    return r3_write_classify(static_cast<uint64_t>(abs_dword), 1u, kAuxTag) == WVerdict::TxnRead;
}

uint64_t r3_writeguard_deny_count(void)
{
    return g.denies;
}

uint64_t r3_writeguard_txn_count(void)
{
    return g.txns;
}

const char *r3_writeguard_last_tag(void)
{
    return g.last_tag;
}

const char *r3_writeguard_last_context(void)
{
    return g.last_context;
}

const R3WgWindow *r3_writeguard_window(uint32_t index)
{
    return index < R3_WG_WINDOW_COUNT ? &kWindows[index] : nullptr;
}

const char *r3_wg_class_name(WClass cls)
{
    switch (cls) {
    case WClass::Access: return "access";
    case WClass::Data: return "data";
    case WClass::Protocol: return "protocol";
    case WClass::State: return "state";
    }
    return "?";
}

const char *r3_wg_substep_name(WSubstep s)
{
    switch (s) {
    case WSubstep::None: return "none";
    case WSubstep::Psp: return "psp";
    case WSubstep::Smu: return "smu";
    case WSubstep::Gfx: return "gfx";
    case WSubstep::Sdma: return "sdma";
    case WSubstep::Mes: return "mes";
    }
    return "?";
}

const R3WManifest *r3_writeguard_manifest(void)
{
    return &g.manifest;
}

/* --- Step3: 期望校验 ------------------------------------------------------------------- */

int r3_wg_expect_writes(const R3WriteRowReg *rows, uint32_t count,
                        const R3WriteRowLayout *layouts, uint32_t layout_count,
                        WSubstep owner)
{
    if (!rows && count > 0)
        return -1;
    for (uint32_t i = 0; i < R3_WG_EXPECT_MAX; i++) {
        g.expect_met[i] = false;
        g.expect_layout_met[i] = false;
        g.expect_abs_met[i] = false;
        g.expect_extra_ctx[i] = nullptr;
    }
    g.expect_abs = nullptr;
    g.expect_abs_count = 0;
    g.expect_rows = rows;
    g.expect_row_count = count < R3_WG_EXPECT_MAX ? count : R3_WG_EXPECT_MAX;
    g.expect_layouts = layouts;
    g.expect_layout_count = layout_count < R3_WG_EXPECT_MAX ? layout_count : R3_WG_EXPECT_MAX;
    g.expect_owner = owner;
    /* The declared address set is supplied separately by r3_wg_expect_declared_addrs():
     * the substep resolves its declaration rows through the IP base table at expect time.
     * Nothing is inferred from the registration table here, so registration ORDER / a
     * post-expect registration can never shift which address belongs to which row. */
    g.expect_addr_count = 0;
    g.expect_armed = true;
    g.expect_extra_count = 0;
    g.expect_path = nullptr;
    return 0;
}

int r3_wg_expect_declared_addrs(const uint32_t *addrs, uint32_t count)
{
    if (!g.expect_armed || (!addrs && count > 0) || count != g.expect_row_count)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        g.expect_addrs[i] = addrs[i];
    g.expect_addr_count = count;
    return 0;
}

int r3_wg_resolve_row_addrs(const void *ip, const R3WriteRowReg *rows, uint32_t count,
                            uint32_t *out)
{
    if (!rows || !out || count > R3_WG_EXPECT_MAX)
        return 0;
    const amdgpu::IPBaseTable *table = static_cast<const amdgpu::IPBaseTable *>(ip);
    uint32_t resolved = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = 0u;
        if (!table) {
            out[i] = 0u;
            continue;
        }
        if (rows[i].golden) {
            if (table->resolve(static_cast<const r3gold::GoldenReg *>(rows[i].golden)->ref(),
                               &addr)) {
                out[i] = addr;
                resolved++;
            } else {
                out[i] = 0u;
            }
        } else {
            const amdgpu::IPBlock block = static_cast<amdgpu::IPBlock>(rows[i].block);
            if (table->isResolved(block, rows[i].base_idx)) {
                out[i] = table->getBase(block, rows[i].base_idx) + rows[i].off;
                resolved++;
            } else {
                out[i] = 0u;
            }
        }
    }
    return static_cast<int>(resolved);
}

int r3_wg_expect_abs_writes(const R3WriteRowAbs *rows, uint32_t count, WSubstep owner)
{
    if (!g.expect_armed || owner != g.expect_owner || (!rows && count > 0))
        return -1;
    g.expect_abs = rows;
    g.expect_abs_count = count < R3_WG_EXPECT_MAX ? count : R3_WG_EXPECT_MAX;
    return 0;
}

void r3_wg_set_path(const char *label)
{
    g.expect_path = label;
}

/* need 匹配当前路径（reg 行与 layout 行共用同一判定）。 */
static bool need_matches_path(WNeed need, const char *path)
{
    if (need == WNeed::Required)
        return true;
    if (need == WNeed::OnSuccessPath)
        return strcmp(path, "failure") != 0;
    if (need == WNeed::OnRetry)
        return strcmp(path, "retry") == 0;
    if (need == WNeed::OnFailure)
        return strcmp(path, "failure") == 0;
    return false;
}

int r3_wg_verify_expectations(void)
{
    if (!g.expect_armed)
        return 0;
    const char *path = g.expect_path ? g.expect_path : "success";
    uint32_t hit = 0;
    for (uint32_t i = 0; i < g.expect_row_count; i++) {
        if (need_matches_path(g.expect_rows[i].need, path) && g.expect_met[i])
            hit++;
    }
    for (uint32_t i = 0; i < g.expect_layout_count; i++) {
        if (need_matches_path(g.expect_layouts[i].need, path) && g.expect_layout_met[i])
            hit++;
    }
    for (uint32_t i = 0; i < g.expect_abs_count; i++) {
        if (need_matches_path(g.expect_abs[i].need, path) && g.expect_abs_met[i])
            hit++;
    }
    return static_cast<int>(hit);
}

void r3_wg_expect_clear(void)
{
    g.expect_armed = false;
    g.expect_rows = nullptr;
    g.expect_row_count = 0;
    g.expect_layouts = nullptr;
    g.expect_layout_count = 0;
    g.expect_abs = nullptr;
    g.expect_abs_count = 0;
    g.expect_addr_count = 0;
    g.expect_extra_count = 0;
    for (uint32_t i = 0; i < R3_WG_EXPECT_MAX; i++) {
        g.expect_met[i] = false;
        g.expect_layout_met[i] = false;
        g.expect_abs_met[i] = false;
        g.expect_extra_ctx[i] = nullptr;
    }
}

/* 第一个未命中行：reg 行 (0..row_count-1)、layout 行、abs 行依次排列。
 * -1 = 全部命中。 */
int r3_wg_first_unmet(void)
{
    if (!g.expect_armed)
        return -1;
    const char *path = g.expect_path ? g.expect_path : "success";
    for (uint32_t i = 0; i < g.expect_row_count; i++) {
        if (need_matches_path(g.expect_rows[i].need, path) && !g.expect_met[i])
            return static_cast<int>(i);
    }
    for (uint32_t i = 0; i < g.expect_layout_count; i++) {
        if (need_matches_path(g.expect_layouts[i].need, path) && !g.expect_layout_met[i])
            return static_cast<int>(g.expect_row_count + i);
    }
    for (uint32_t i = 0; i < g.expect_abs_count; i++) {
        if (need_matches_path(g.expect_abs[i].need, path) && !g.expect_abs_met[i])
            return static_cast<int>(g.expect_row_count + g.expect_layout_count + i);
    }
    return -1;
}

uint32_t r3_wg_extra_write_count(void)
{
    if (!g.expect_armed)
        return 0;
    return g.expect_extra_count;
}

const char* r3_wg_extra_write_ctx(uint32_t index)
{
    if (!g.expect_armed || index >= g.expect_extra_count)
        return nullptr;
    return g.expect_extra_ctx[index];
}

/* P12: append one line to the dedicated N48R3EVID-expect-verify text (方案 v3 §B.2.4/B.4).
 * Bounded by R3_EVID_TEXT_BYTES; a full buffer drops the line and increments dropped,
 * which is reported as "truncated=N" so a shortened payload is never silently taken as
 * complete. Global buffer (data segment), not on the 2 KiB kernel stack. */
static void expect_evid_line(const char *s)
{
    const size_t n = strlen(s);
    if (n + 2u > sizeof(g.expect_evid) - g.expect_evid_used) {
        g.expect_evid_dropped++;
        return;
    }
    memcpy(g.expect_evid + g.expect_evid_used, s, n);
    g.expect_evid_used += static_cast<uint32_t>(n);
    g.expect_evid[g.expect_evid_used++] = '\n';
    g.expect_evid[g.expect_evid_used] = '\0';
}

/* Publish the expectation verdict into BOTH channels:
 *   1. "expect-verify" lines via r3_ev_add into the shared n48log ring (the stage2-log
 *      property was retired with the 6 KiB arena, task 11-10-stage2-evidence-n48log
 *      design §7.3/§7.4),
 *   2. the dedicated N48R3EVID-expect-verify chunked property (方案 v3 P12).
 * Both run for real hardware (strict_expect=false, record-only) AND host tests.
 * The property carries the metadata line, one line per unmet/extra row, and a
 * "truncated=N" tail when the text bound was hit. A giant write set can never mask a
 * miss: each miss is its own line and the metadata is emitted first. */
static void expect_publish_verify(const R3SubstepEnv *env)
{
    if (!env)
        return;
    const char *path = g.expect_path ? g.expect_path : "success";
    const int hit = r3_wg_verify_expectations();
    const uint32_t extra = r3_wg_extra_write_count();
    const uint32_t declared = g.expect_row_count + g.expect_layout_count + g.expect_abs_count;
    /* missed counts only rows whose WNeed matches the path but were not written (same
     * filter as the "miss:" lines); declared-hit would over-count on the failure path. */
    uint32_t missed = 0;
    for (uint32_t i = 0; i < g.expect_row_count; i++)
        if (need_matches_path(g.expect_rows[i].need, path) && !g.expect_met[i])
            missed++;
    for (uint32_t i = 0; i < g.expect_layout_count; i++)
        if (need_matches_path(g.expect_layouts[i].need, path) && !g.expect_layout_met[i])
            missed++;
    for (uint32_t i = 0; i < g.expect_abs_count; i++)
        if (need_matches_path(g.expect_abs[i].need, path) && !g.expect_abs_met[i])
            missed++;

    g.expect_evid[0] = '\0';
    g.expect_evid_used = 0;
    g.expect_evid_dropped = 0;
    char line[160];   /* metadata ~110 B worst case */
    if (snprintf(line, sizeof line,
                 "%s: declared=%u hit=%d missed=%u extra=%u path=%s",
                 r3_wg_substep_name(g.expect_owner),
                 declared, hit, missed, extra, path) > 0) {
        expect_evid_line(line);
        if (env->ev)
            r3_ev_add(env->ev, "expect-verify", line);
    }
    /* One line per declared-but-unwritten expectation (reg, layout, abs). */
    for (uint32_t i = 0; i < g.expect_row_count; i++) {
        if (need_matches_path(g.expect_rows[i].need, path) && !g.expect_met[i]) {
            char b[96];
            if (snprintf(b, sizeof b, "miss: %s",
                         g.expect_rows[i].ctx ? g.expect_rows[i].ctx : "?") > 0) {
                expect_evid_line(b);
                if (env->ev)
                    r3_ev_add(env->ev, "expect-verify", b);
            }
        }
    }
    for (uint32_t i = 0; i < g.expect_layout_count; i++) {
        if (need_matches_path(g.expect_layouts[i].need, path) && !g.expect_layout_met[i]) {
            char b[96];
            if (snprintf(b, sizeof b, "miss: %s",
                         g.expect_layouts[i].tag ? g.expect_layouts[i].tag : "?") > 0) {
                expect_evid_line(b);
                if (env->ev)
                    r3_ev_add(env->ev, "expect-verify", b);
            }
        }
    }
    for (uint32_t i = 0; i < g.expect_abs_count; i++) {
        if (need_matches_path(g.expect_abs[i].need, path) && !g.expect_abs_met[i]) {
            char b[96];
            if (snprintf(b, sizeof b, "miss: %s (0x%x)",
                         g.expect_abs[i].ctx ? g.expect_abs[i].ctx : "?",
                         g.expect_abs[i].addr) > 0) {
                expect_evid_line(b);
                if (env->ev)
                    r3_ev_add(env->ev, "expect-verify", b);
            }
        }
    }
    /* One line per extra write (reverse check survivors). */
    for (uint32_t i = 0; i < extra; i++) {
        const char *cx = r3_wg_extra_write_ctx(i);
        char b[96];
        if (snprintf(b, sizeof b, "extra: %s", cx ? cx : "?") > 0) {
            expect_evid_line(b);
            if (env->ev)
                r3_ev_add(env->ev, "expect-verify", b);
        }
    }
    if (g.expect_evid_dropped > 0u) {
        char b[64];
        if (snprintf(b, sizeof b, "truncated=%u", g.expect_evid_dropped) > 0)
            expect_evid_line(b);
    }
    if (env->sink && env->sink->set_property)
        env->sink->set_property(env->sink->user, "N48R3EVID-expect-verify",
                                g.expect_evid, g.expect_evid_used);
}

/* Helper 实现（声明在 writeguard.h）：verify+clear 加 strict_expect 门控。
 * miss 判定用 first_unmet()（按 need/路径过滤），success 路径 strict=true 时阻断；
 * failure 路径（r3_wg_set_path("failure") 后）只记录、不阻断——子步失败原因必须
 * 能原样回退，不被 expect-write-mismatch 覆盖。 */
int r3_wg_verify_and_clear(const R3SubstepEnv *env, uint32_t expected)
{
    (void)expected;   /* 保留参数以兼容所有调用点；miss 判定改用 first_unmet() */
    const int unmet = r3_wg_first_unmet();
    const uint32_t extra = r3_wg_extra_write_count();
    const char *path = g.expect_path ? g.expect_path : "success";
    const bool failing = strcmp(path, "failure") == 0;
    const bool mismatched = unmet >= 0 || extra > 0u;
    const bool strict = env && env->strict_expect;
    const bool block = mismatched && strict && !failing;
    expect_publish_verify(env);   /* P12: record for real hardware AND host, before clear */
    r3_wg_expect_clear();
    return block ? 1 : 0;
}
