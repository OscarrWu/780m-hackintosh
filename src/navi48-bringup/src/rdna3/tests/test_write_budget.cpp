/*
 * test_write_budget.cpp - 写守卫全链预算契约的 host 回归（蓝本 §7.3 的 L3 层）。
 *
 * 背景：boot5 真机失败根因是写守卫 grant 表容量（当时 128）小于全链登记总量
 * （162），S7 第 4 行表满返回 -2 被折叠成 pre:register。修复把"数量"上提为
 * 编译期契约（rdna3_ip_entries.h 预算常量 + static_assert 闸门，W1），运行期
 * 以 r3_wg_reserve 预检（W2）与 expect 集合硬化（W3）为回归。
 *
 * 本文件在 R3_WG_GRANT_MAX / 预算常量已知的前提下直接驱动 writeguard 单测：
 *   - test_budget_arithmetic   预算常量恒等式（LAYOUT/REG/STAGE2/TOTAL 与分项和）；
 *   - test_fill_to_capacity    表满边界：预算全部可注册、表满后第 MAX+1 条 -2 且
 *                              不改变 grant_count（planted break >= -> > 必被抓）；
 *   - test_capacity_log        表满单点日志含 GRANT-FULL 与 used/need/cap 三要素；
 *   - test_reserve_boundary    预检边界：0 -> -1、预留成功、reserve 只读不注册；
 *   - test_expect_cap_reject   （W3）expect 超 R3_WG_EXPECT_MAX 拒绝且不留半初始化；
 *   - test_expect_fill_boundary（W3）恰好 80 条期望可注册、可命中、可 verify。
 *
 * 合成 grant 的注意事项（蓝本 §7.3）：tag 前缀与 class 会被校验——layout 必须
 * Data+"layout:"、reg 必须非 Data+"reg:"；用非法组合测容量会先得到 -1 掩盖 -2。
 */
#include <stdio.h>
#include <string.h>

#include "rdna3_ip_entries.h"
#include "rdna3_writeguard.h"
#include "rdna3_substep.h"
#include "n48log_stub.h"

using namespace amdgpu;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 日志捕获：把 writeguard 的 log sink 行收进静态缓冲，供断言检索。 */
static char g_logbuf[512];
static size_t g_logused;
static void capture_log(void * /*user*/, const char *line)
{
    if (!line)
        return;
    size_t n = strlen(line);
    if (n + 1u > sizeof g_logbuf - g_logused)
        n = sizeof g_logbuf - g_logused - 1u;
    memcpy(g_logbuf + g_logused, line, n);
    g_logused += n;
    g_logbuf[g_logused] = '\0';
}

/* 合成一条 reg grant（合法形态：非 Data + "reg:" 前缀）。 */
static int syn_reg(uint32_t addr)
{
    return r3_wg_register_reg(addr, 1u, WClass::State, WSubstep::Psp, "reg:syn");
}

static void test_budget_arithmetic(void)
{
    CHECK(R3_WG_BUDGET_LAYOUT_TOTAL == 14u,
          "layout total == 14 (got %u)", (unsigned)R3_WG_BUDGET_LAYOUT_TOTAL);
    CHECK(R3_WG_BUDGET_REG_TOTAL == 162u,
          "reg total == 162 (got %u)", (unsigned)R3_WG_BUDGET_REG_TOTAL);
    CHECK(R3_WG_BUDGET_STAGE2 == 121u,
          "stage2 == 121 (got %u)", (unsigned)R3_WG_BUDGET_STAGE2);
    CHECK(R3_WG_BUDGET_TOTAL == 176u,
          "total == 176 (got %u)", (unsigned)R3_WG_BUDGET_TOTAL);
    CHECK(R3_WG_BUDGET_TOTAL <= R3_WG_GRANT_MAX,
          "contract total fits grant table (%u <= %u)",
          (unsigned)R3_WG_BUDGET_TOTAL, (unsigned)R3_WG_GRANT_MAX);
    /* 附录 A 账目的收尾恒等式：阶段2 + 阶段2后各分项 == 全链总量。 */
    CHECK(R3_WG_BUDGET_STAGE2 + kHdpGrantCount + kGmcSeqLayoutCount + kGartRowCount +
              kVmid8FirstInvalidateRow + kMesQueueLayoutCount + kGfxTeardownRowCount +
              kIhTeardownRowCount + kIhInputRowCount == R3_WG_BUDGET_TOTAL,
          "stage2 + hdp + seq + gart + vmid8 + queue-layout + gfx-teardown + "
          "ih-teardown + ih-input == total (%u+%u+%u+%u+%u+%u+%u+%u+%u = %u)",
          (unsigned)R3_WG_BUDGET_STAGE2, (unsigned)kHdpGrantCount,
          (unsigned)kGmcSeqLayoutCount, (unsigned)kGartRowCount,
          (unsigned)kVmid8FirstInvalidateRow, (unsigned)kMesQueueLayoutCount,
          (unsigned)kGfxTeardownRowCount, (unsigned)kIhTeardownRowCount,
          (unsigned)kIhInputRowCount, (unsigned)R3_WG_BUDGET_TOTAL);
}

static void test_fill_to_capacity(void)
{
    r3_writeguard_install_mm();

    /* 先按全链预算 176 注册合成 reg grant：必须全部成功。 */
    for (uint32_t i = 0; i < R3_WG_BUDGET_TOTAL; i++) {
        CHECK(syn_reg(0x1000u + i * 4u) == 0, "contract row %u registers", i);
    }
    CHECK(r3_wg_grant_count() == R3_WG_BUDGET_TOTAL,
          "grant_count == 176 (got %u)", r3_wg_grant_count());

    /* 继续注册到物理容量 R3_WG_GRANT_MAX（200 = 176 预算 + 24 预留）。 */
    for (uint32_t i = R3_WG_BUDGET_TOTAL; i < R3_WG_GRANT_MAX; i++) {
        CHECK(syn_reg(0x1000u + i * 4u) == 0, "row %u fills spare slots", i);
    }
    CHECK(r3_wg_grant_count() == R3_WG_GRANT_MAX,
          "grant_count == 200 (got %u)", r3_wg_grant_count());

    /* 第 MAX+1 条必须 -2 且不改变计数（planted break >= -> > 会在此处失败）。 */
    CHECK(syn_reg(0x1000u + R3_WG_GRANT_MAX * 4u) == -2,
          "row %u rejected (table full)", (unsigned)(R3_WG_GRANT_MAX + 1u));
    CHECK(r3_wg_grant_count() == R3_WG_GRANT_MAX,
          "rejected row left grant_count at 200 (got %u)", r3_wg_grant_count());
}

static void test_capacity_log(void)
{
    g_logbuf[0] = '\0';
    g_logused = 0;
    r3_writeguard_install_mm();
    r3_wg_set_log(capture_log, nullptr);   /* install_mm 会重置 State（含 log_fn），先装再挂 sink */

    for (uint32_t i = 0; i < R3_WG_GRANT_MAX; i++) {
        CHECK(syn_reg(0x2000u + i * 4u) == 0, "fill row %u", i);
    }
    CHECK(syn_reg(0x2000u + R3_WG_GRANT_MAX * 4u) == -2, "overflow row rejected");

    CHECK(strstr(g_logbuf, "GRANT-FULL") != nullptr, "log carries GRANT-FULL");
    if (strstr(g_logbuf, "GRANT-FULL") != nullptr) {
        char want[64];
        snprintf(want, sizeof(want), "used=%u", (unsigned)R3_WG_GRANT_MAX);
        CHECK(strstr(g_logbuf, want) != nullptr, "log carries %s", want);
        snprintf(want, sizeof(want), "cap=%u", (unsigned)R3_WG_GRANT_MAX);
        CHECK(strstr(g_logbuf, want) != nullptr, "log carries %s", want);
    }
    r3_wg_set_log(nullptr, nullptr);
}

static void test_reserve_boundary(void)
{
    r3_writeguard_install_mm();

    CHECK(r3_wg_reserve(0u) == -1, "reserve(0) rejected");
    CHECK(r3_wg_reserve(R3_WG_BUDGET_TOTAL) == 0, "reserve(full contract) fits empty table");
    CHECK(r3_wg_grant_count() == 0u, "reserve registers nothing");

    /* 填到 200-5=195：reserve(4) 成功（195+4 <= 200）、reserve(6) 拒绝（195+6 > 200）。
     * reserve 只读：成功/失败都不改变 grant_count。 */
    for (uint32_t i = 0; i < R3_WG_GRANT_MAX - 5u; i++) {
        CHECK(syn_reg(0x3000u + i * 4u) == 0, "fill row %u", i);
    }
    CHECK(r3_wg_grant_count() == R3_WG_GRANT_MAX - 5u,
          "grant_count == 195 (got %u)", r3_wg_grant_count());

    CHECK(r3_wg_reserve(4u) == 0, "reserve(4) fits in last 5 slots");
    CHECK(r3_wg_grant_count() == R3_WG_GRANT_MAX - 5u,
          "successful reserve did not register (count %u)", r3_wg_grant_count());

    CHECK(r3_wg_reserve(6u) == -2, "reserve(6) overflows");
    CHECK(r3_wg_grant_count() == R3_WG_GRANT_MAX - 5u,
          "rejected reserve did not register (count %u)", r3_wg_grant_count());
}

/* 超限期望必须被拒绝（fail closed），且拒绝路径不留半初始化状态：拒绝之后再以
 * 合法参数调用必须成功（expect_armed/计数已被显式复位）。 */
static void test_expect_cap_reject(void)
{
    R3WriteRowReg rows[R3_WG_EXPECT_MAX + 1u];
    R3WriteRowAbs abs[R3_WG_EXPECT_MAX + 1u];
    uint32_t addrs[R3_WG_EXPECT_MAX + 1u];
    for (uint32_t i = 0; i <= R3_WG_EXPECT_MAX; i++) {
        rows[i] = {nullptr, 0, 0, 0x4000u + i * 4u, 1u, WClass::State, WNeed::Required, "syn"};
        abs[i] = {0x5000u + i * 4u, 1u, WClass::Protocol, WNeed::OnSuccessPath, "syn-abs"};
        addrs[i] = rows[i].off;
    }

    r3_writeguard_install_mm();
    r3_wg_arm_substep(WSubstep::Psp);

    /* 81 行期望：expect_writes 拒绝（原实现静默截断为 80，是漏检根源）。 */
    CHECK(r3_wg_expect_writes(rows, R3_WG_EXPECT_MAX + 1u, nullptr, 0,
                              WSubstep::Psp) == -1,
          "expect_writes(81) rejected");
    /* 拒绝后状态已复位：合法 80 行必须成功。 */
    CHECK(r3_wg_expect_writes(rows, R3_WG_EXPECT_MAX, nullptr, 0,
                              WSubstep::Psp) == 0,
          "expect_writes(80) accepted after reject");

    /* abs 超限同样拒绝。 */
    CHECK(r3_wg_expect_abs_writes(abs, R3_WG_EXPECT_MAX + 1u, WSubstep::Psp) == -1,
          "expect_abs_writes(81) rejected");
    CHECK(r3_wg_expect_abs_writes(abs, R3_WG_EXPECT_MAX, WSubstep::Psp) == 0,
          "expect_abs_writes(80) accepted after reject");

    /* declared 超限拒绝（count != expect_row_count 与 count > MAX 双条件）。 */
    CHECK(r3_wg_expect_declared_addrs(addrs, R3_WG_EXPECT_MAX + 1u) == -1,
          "expect_declared_addrs(81) rejected");
    CHECK(r3_wg_expect_declared_addrs(addrs, R3_WG_EXPECT_MAX) == 0,
          "expect_declared_addrs(80) accepted after reject");

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

/* 恰好 80 行的边界不是"接受但不可用"：逐条命中、verify 全中、first_unmet == -1。 */
static void test_expect_fill_boundary(void)
{
    R3WriteRowReg rows[R3_WG_EXPECT_MAX];
    for (uint32_t i = 0; i < R3_WG_EXPECT_MAX; i++) {
        rows[i] = {nullptr, 0, 0, 0x6000u + i * 4u, 1u, WClass::State, WNeed::Required, "syn"};
    }

    r3_writeguard_install_mm();
    r3_wg_arm_substep(WSubstep::Psp);

    uint32_t addrs[R3_WG_EXPECT_MAX];
    for (uint32_t i = 0; i < R3_WG_EXPECT_MAX; i++)
        addrs[i] = rows[i].off;
    CHECK(r3_wg_expect_writes(rows, R3_WG_EXPECT_MAX, nullptr, 0,
                              WSubstep::Psp) == 0,
          "expect_writes(80) accepted at boundary");
    CHECK(r3_wg_expect_declared_addrs(addrs, R3_WG_EXPECT_MAX) == 0,
          "expect_declared_addrs(80) accepted at boundary");

    /* 80 条对应 reg grant + 逐条命中。 */
    for (uint32_t i = 0; i < R3_WG_EXPECT_MAX; i++) {
        CHECK(r3_wg_register_reg(rows[i].off, 1u, WClass::State, WSubstep::Psp,
                                 "reg:syn") == 0,
              "grant row %u", i);
        CHECK(r3_wg_classify(rows[i].off, 1u, WClass::State, "syn") == WVerdict::TxnRead,
              "row %u classified", i);
    }

    CHECK(r3_wg_verify_expectations() == static_cast<int>(R3_WG_EXPECT_MAX),
          "all 80 rows hit (got %d)", r3_wg_verify_expectations());
    CHECK(r3_wg_first_unmet() == -1, "no unmet (got %d)", r3_wg_first_unmet());

    r3_wg_expect_clear();
    r3_wg_disarm_substep();
}

int main(void)
{
    test_budget_arithmetic();
    test_fill_to_capacity();
    test_capacity_log();
    test_reserve_boundary();
    test_expect_cap_reject();
    test_expect_fill_boundary();

    printf("test_write_budget: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
