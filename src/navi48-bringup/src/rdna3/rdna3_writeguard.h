/*
 * rdna3_writeguard.h - the runtime write whitelist for the RDNA3 bringup (D5).
 *
 * DENY BY DEFAULT. Stage 1 may only write the AUX request registers needed to drive a DDC/DPCD
 * READ transaction (src/dcn314/dcn314_aux_min.c) plus the absolute MM_INDEX/MM_DATA/MM_INDEX_HI
 * read-transaction window. Stage 2 adds the B2 four-class model below. Every write is classified
 * here before it reaches the bus; everything else is refused. A refusal is a bug, not a fallback:
 * the caller must treat it as a failure and log it (B1: every DENY is its own manifest line).
 *
 * This layer performs NO register I/O. It is a pure address/width/owner/class classifier with
 * counters and a write manifest, so it cannot itself introduce a hang (no MMIO, no lock, no
 * allocation). It never touches SMN. Timing prerequisites (e.g. reading RB_WPTR before the WPTR
 * window write) are the caller's job; this classifier never decides ordering.
 *
 * Address space: `pa` is a dword address in the caller's aperture (BAR5-relative for the stage-1
 * MMIO windows, exactly the value dcn314 passes to its write_ok callback). `size` is the access
 * width in dwords. Grant intervals use the same dword space.
 *
 * Stage 2 extension (tech plan §6.1):
 *   WClass   - B2 side-effect class: Access / Data / Protocol / State.
 *   WSubstep - power-up substep owner: None / Psp / Smu / Gfx / Sdma / Mes.
 *   Grants   - ② Data layout intervals (tag "layout:...") allow any sub-range inside the grant;
 *              ③ Protocol and ④ State register windows (tag "reg:...") match exactly one
 *              address+width. ① Access keeps the legacy stage-1 window table below.
 *
 * Structural constraints (fail closed, a violation fails the substep):
 *   1. a window whose owner differs from the armed substep is DENY;
 *   2. only Data layout grants allow sub-ranges; register windows are exact;
 *   3. this classifier does no I/O and makes no timing decision;
 *   4. every DENY is a whitelist violation; r3_writeguard_deny_count() must be 0 in a clean run;
 *   5. after r3_writeguard_disarm() every classify() is DENY.
 *
 * Every classify() is appended to the writeguard-owned write manifest
 * (rdna3_wmanifest.h). The integration publishes it under N48R3EVID-write-manifest with
 * r3_wm_format(r3_writeguard_manifest(), ...).
 *
 * Wiring for stage 1 (T2b):
 *   r3_writeguard_install(dmu.bases);              // once, from the DMU IP discovery entry
 *   dev.write_ok = r3_writeguard_write_ok;         // dcn314_write_ok_fn adapter
 *   // on an allowed write (V5 "逐条标注"):
 *   r3_writeguard_last_tag() + r3_writeguard_last_context() + address + size
 *
 * Wiring for stage 2:
 *   r3_wg_register_layout(...);                    // ② per layout grant
 *   r3_wg_register_reg(...);                       // ③④ per explicit register
 *   r3_wg_arm_substep(WSubstep::Psp);              // enter a substep
 *   WVerdict v = r3_wg_classify(addr, dwords, WClass::Protocol, "psp-ring-wptr");
 */
#ifndef N48_RDNA3_WRITEGUARD_H
#define N48_RDNA3_WRITEGUARD_H

#include <stdint.h>

#define R3_WG_SEGS 5u
#define R3_WG_MM_WINDOW_COUNT 3u
#define R3_WG_AUX_WINDOW_COUNT 25u
#define R3_WG_WINDOW_COUNT (R3_WG_MM_WINDOW_COUNT + R3_WG_AUX_WINDOW_COUNT)
#define R3_WG_GRANT_MAX 128u


/* B2.1: write necessity level for expectation verification (tech plan v3 §B.2.1). */
enum class WNeed : uint8_t {
    Required = 0,       /* must write regardless of path */
    OnSuccessPath = 1,  /* written on the success path; may be skipped on failure */
    OnRetry = 2,        /* written only during a retry branch */
    OnFailure = 3,      /* written only on the failure path (e.g. teardown) */
    Optional = 4,       /* conditional write, no clear path affinity */
};

enum class WVerdict { Deny, TxnRead };

/* B2 side-effect class. Each write belongs to exactly one class. */
enum class WClass : uint8_t {
    Access = 0,   /* ① access enable (stage-1 semantics)         */
    Data = 1,     /* ② data movement (firmware/table/buffer)     */
    Protocol = 2, /* ③ protocol interaction (mailbox/wptr/doorbell) */
    State = 3     /* ④ state configuration (enable/clock/reset)  */
};

/* Power-up substep owner. None is the stage-1 / unowned context. */
enum class WSubstep : uint8_t {
    None = 0,
    Psp = 1,
    Smu = 2,
    Gfx = 3,
    Sdma = 4,
    Mes = 5
};

/* Register write declaration: one row per unique address (P5). Used by substep
 * modules to declare their register write set (the single source of truth). */
struct R3WriteRowReg {
    const void *golden;  /* nullptr = fallback form below; else cast to GoldenReg* */
    uint8_t  block;      /* IPBlock index (fallback) */
    uint8_t  base_idx;   /* segment index (fallback) */
    uint32_t off;        /* register offset */
    uint32_t dwords;     /* access width (1=register, 2=doorbell) */
    WClass   cls;        /* B2 side-effect class */
    WNeed    need;       /* necessity level */
    const char *ctx;     /* operation label */
};

/* Layout grant declaration. */
struct R3WriteRowLayout {
    const char *tag;
    WSubstep    owner;
    WNeed       need;
};


/* Absolute-dword write declaration (e.g. MES BAR2 doorbell indices). These addresses are
 * NOT IP-base+offset, so they cannot be expressed by R3WriteRowReg. Kept separate so the
 * register declaration tables stay untouched (B.2.1). Matching is by address (P5: one row
 * per unique address); ctx is a declaration tag for reporting, not a match key. */
struct R3WriteRowAbs {
    uint32_t    addr;   /* absolute dword address / BAR2 index */
    uint32_t    dwords; /* access width (2 for a 64-bit doorbell store) */
    WClass      cls;
    WNeed       need;
    const char *ctx;    /* declaration tag (reporting only, not a match key) */
};

/* One registered stage-2 window. A Data grant has range=true and allows any sub-range inside
 * [start, start+dwords); a Protocol/State/Access register window has range=false and matches
 * exactly (addr, dwords). `tag` must be "layout:..." for range grants and "reg:..." otherwise,
 * and must outlive the guard (string literal). */
struct R3WgGrant {
    uint32_t start;   /* first dword address                */
    uint32_t dwords;  /* width in dwords (>= 1)             */
    WClass cls;       /* B2 class                           */
    WSubstep owner;   /* substep that may use this window   */
    const char *tag;  /* "layout:..." or "reg:..."          */
    bool range;       /* true: layout grant (Data only)     */
    bool shared;      /* true: any armed substep may use it (platform resource) */
};

/* Arm the guard from the DMU (DCN) IP's five discovery segment bases. Returns 0 on success,
 * -1 on a null argument. Until armed, every classify() is Deny (fail closed). Re-installing
 * re-arms, clears stage-2 grants and resets the counters and the write manifest. */
int r3_writeguard_install(const uint32_t seg[R3_WG_SEGS]);

/* Arm only the absolute MM_INDEX/MM_DATA/MM_INDEX_HI read-transaction windows (BAR5 dwords
 * 0x0/0x1/0x6, base Navi48Bringup.cpp:160-165). T2 needs these BEFORE discovery has supplied
 * the DCN segment bases, so this is a separate arm step; r3_writeguard_install() later re-arms
 * the full table (MM + AUX) from the DMU bases. Always returns 0. Clears grants + manifest. */
int r3_writeguard_install_mm(void);

/* Disarm: every classify() becomes Deny. Counters and the manifest are kept. */
void r3_writeguard_disarm(void);

/* Register one stage-2 window. Returns 0 on success, -1 on an invalid argument (null, zero
 * width, 32-bit overflow, owner None, Data via the register form, wrong tag prefix), -2 when
 * the table is full. Registration is fail closed: a rejected window is never partially added. */
int r3_wg_register_grant(const R3WgGrant *grant);

/* ② data-movement layout grant: cls=Data, range=true, tag must start "layout:". */
int r3_wg_register_layout(uint32_t start, uint32_t dwords, WSubstep owner, const char *tag);

/* ③④ explicit register window: cls must be Access/Protocol/State, range=false, tag must start
 * "reg:". Data must use r3_wg_register_layout(). */
int r3_wg_register_reg(uint32_t addr, uint32_t dwords, WClass cls, WSubstep owner,
                       const char *tag);

/* Register one platform-shared row (e.g. the HDP memory-flush window): the address/class
 * checks still apply, but the row is usable by any armed substep. A shared resource is not
 * owned by a single IP; making it per-IP is how visibility fixes go missing. */
int r3_wg_register_shared_reg(uint32_t addr, uint32_t dwords, WClass cls, WSubstep owner,
                              const char *tag);

uint32_t r3_wg_grant_count(void);
const R3WgGrant *r3_wg_grant(uint32_t index);

/* Enter a substep (sets the owner checked against every grant) or leave it (owner None). */
void r3_wg_arm_substep(WSubstep s);
void r3_wg_disarm_substep(void);
WSubstep r3_wg_substep(void);


/* D-06/B1: set a log sink so every DENY is also logged to the main log (N48R3LOG).
 * NULL (the default) suppresses the log line; the manifest always records the DENY. */
typedef void (*R3WgLogFn)(void *user, const char *line);
void r3_wg_set_log(R3WgLogFn fn, void *user);
/* Classify one write with its declared B2 class. TxnRead only when the whole [pa, pa+size)
 * range matches a window owned by the current substep and of the declared class. For a Data
 * layout grant the manifest records the grant interval (A5 aggregation); every other write is
 * recorded at its actual address/width. `ctx` is the caller's operation label.
 *
 * A window is claimed by address first: if the range lies inside a registered window no other
 * window is consulted. Two windows may cover one address (e.g. a ② layout grant and a ④ state
 * register at its start): they are ONE virtual table and each page is a row, and the row whose
 * owner is the armed substep and whose class matches the declared class is the one written
 * (several substeps may also register the same address and tag: the armed owner's row wins).
 * If some row fits address/width but none fits owner + class, the write is DENIED with that
 * row's tag. A row that only contains part of the range was not written and claims nothing:
 * a write that starts inside a layout grant and ends past its last dword is checked against
 * the remaining windows and normally DENIED. */
WVerdict r3_wg_classify(uint64_t pa, uint32_t size, WClass cls, const char *ctx);

/* Stage-1 compatibility wrapper: classifies as WClass::Access. */
WVerdict r3_write_classify(uint64_t pa, uint32_t size, const char *tag);

/* dcn314_write_ok_fn adapter: classifies a 1-dword AUX request-register write. */
extern "C" bool r3_writeguard_write_ok(void *cookie, uint32_t abs_dword, uint32_t value);

/* Survey counters. deny_count is the whitelist-outside write count V10 requires to be 0;
 * any DENY (off-list, owner mismatch, class mismatch, disarmed) increments it. */
uint64_t r3_writeguard_deny_count(void);
uint64_t r3_writeguard_txn_count(void);

/* Tags of the last ALLOWED write: the matched window's tag and the caller's label. Both
 * NULL before the first allow and after a deny. */
const char *r3_writeguard_last_tag(void);
const char *r3_writeguard_last_context(void);

/* The hand-written stage-1 window table (exposed for host tests and survey logging). */
struct R3WgWindow {
    uint8_t base_idx;   /* DCN segment index           */
    uint32_t offset;    /* dword offset in that segment */
    uint8_t dwords;     /* window width in dwords       */
    const char *tag;
    bool absolute;      /* true: offset is a BAR5 dword, not seg[base_idx]-relative */
};


/* -----------------------------------------------------------------------------------------
 * Step3: 期望校验（方案 v3 §B.2.4）
 *
 * 生命周期：expect → classify (自动追踪) → verify (+ clear)。每子步独立一次，不嵌套。
 * 容量固定：R3_WG_EXPECT_MAX，超量静默截断（只记录计数）。
 *
 * 正向校验（miss）：need 匹配当前路径时，声明行的地址必须被写过（P5：声明表按唯一
 *   地址一行；同一地址的多次写、不同 ctx 是同一声明写，不重复计数）。
 * 反向校验（extra write）：地址不在本子步期望时刻的已注册 reg 窗口快照内、且未被
 *   P2 排除规则豁免的 allow 写。P2 判定顺序（方案 v3 §B.2.4）：
 *   1. addr 在已注册 layout grant 区间内（range=true）→ ② Data 类写，不记 extra
 *   2. addr 匹配 shared reg 窗口（HDP，shared=true）→ 不记 extra
 *   3. addr 匹配其他 owner 的已注册窗口 → 不记 extra
 *   4. 其余不匹配任何已注册窗口的写 → 记 extra write（ctx=当前 classify 的标签）
 *   （快照含声明表行 + 声明表之外的注册补充项，如 MES doorbell 的 2 个窗口；
 *     快照之外 = 期望之后才出现的注册/写 → extra 的正确语义。任何白名单外写仍
 *     会被 classify DENY，声明表/注册表一致性由 test_integration 断言保障。）
 *
 * 布局期望（R3WriteRowLayout）：owner 粒度标记——该 owner 的任一 Data 写发生即标记
 *   seen（writeguard 注册层只保留 IP 级 tag"layout:psp"，无法细分到 plan grant tag，
 *   故按 owner 归属；first_unmet 可报告未命中的 layout tag）。
 *
 * 失败路径：子步失败（r3_wg_set_path("failure")）后期望校验只记录、不阻断——失败原因
 *   必须可回退、不被 expect-write-mismatch 覆盖。成功路径 strict=true 时阻断。
 * ----------------------------------------------------------------------------------------- */
#define R3_WG_EXPECT_MAX 64u

/* 注册期望：遍历 rows/layouts 存入内部表。每次调用先清空之前状态（P4）。 */
int r3_wg_expect_writes(const R3WriteRowReg *rows, uint32_t count,
                        const R3WriteRowLayout *layouts, uint32_t layout_count,
                        WSubstep owner);

/* 追加绝对地址期望行（R3WriteRowAbs，如 MES doorbell）。在 r3_wg_expect_writes 之后调用；
 * 与既有期望行共享同一生命周期（同一 expect_armed/clear）。返回 0 成功，-1 参数非法。 */
int r3_wg_expect_abs_writes(const R3WriteRowAbs *rows, uint32_t count, WSubstep owner);

/* 声明表行的运行时地址（按声明序号，与 rows 一一对应），由子步注册期解析注入。
 * 这是 expect 校验的静态权威地址集合：正向 met 与反向 extra 判定都基于它，与
 * 注册窗口的注册顺序无关（方案 v3 §B.2.1 P5：声明表按唯一地址一行）。*/
int r3_wg_expect_declared_addrs(const uint32_t *addrs, uint32_t count);

/* 解析声明表行地址（golden 或 fallback），供 expect 时把 rows 投影成静态权威地址集合。
 * out 需容纳 count 个 uint32_t。*ip 为 amdgpu::IPBaseTable*（行内仅用其 resolve/getBase）。
 * 返回解析成功的行数；某行解析失败则 out[i] 置 0 并继续（该行无法判定、报 miss）。*/
int r3_wg_resolve_row_addrs(const void *ip, const R3WriteRowReg *rows, uint32_t count,
                            uint32_t *out);

/* 设置当前执行路径标签。默认 "success"。 */
void r3_wg_set_path(const char *label);

/* 校验期望：返回命中行数（need 匹配当前路径且已被写的行数，含 layout 行）。
 * 内部会重置 met 状态并从 classify 历史重建。 */
int r3_wg_verify_expectations(void);

/* 清空期望表（释放生命周期）。 */
void r3_wg_expect_clear(void);

/* 第一个未命中行索引（need 匹配当前路径但未被写；reg 行在前、layout 行在后）。
 * -1 = 全部命中。 */
int r3_wg_first_unmet(void);

/* 额外写操作（不被期望且未被 P2 排除规则豁免的写）。 */
uint32_t r3_wg_extra_write_count(void);
const char* r3_wg_extra_write_ctx(uint32_t index);


/* Forward declaration needed by r3_wg_verify_and_clear (rdna3_substep.h has the full struct). */
struct R3SubstepEnv;

/* Helper: run verify+clear with the strict_expect gate. 实现见 rdna3_writeguard.cpp。
 * `expected` 保留以兼容所有调用点；miss 判定使用 first_unmet()（按 need/路径过滤），
 * 不再用固定总行数。
 * Returns 0 on verify pass (or strict_expect=false record-only / failure-path record),
 * 1 on verify fail (strict_expect=true + unmet/extra, success path only). */
int r3_wg_verify_and_clear(const R3SubstepEnv *env, uint32_t expected);

const R3WgWindow *r3_writeguard_window(uint32_t index);


/* Human-readable names used by the manifest exporter. */
const char *r3_wg_class_name(WClass cls);
const char *r3_wg_substep_name(WSubstep s);

/* The writeguard-owned write manifest (B1). Full definition in rdna3_wmanifest.h. */
struct R3WManifest;
const R3WManifest *r3_writeguard_manifest(void);

#endif /* N48_RDNA3_WRITEGUARD_H */
