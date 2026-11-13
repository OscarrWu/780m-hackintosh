# 阶段 1：fork 与骨架

## 0. 册头

### 0.1 编写意图

本册回答一个问题：**如何把 Cobalt64 这个"RDNA4 独显 + Tahoe"的驱动基座，改造成能在 780M 上加载、且只读不写的骨架，并让它在目标机上完成第一次全量硬件取证。**

阶段 1 已于 2026-11-01 完成，本册记录当前事实与结案结论。

**术语约定（本册首现解释，后文直接使用）**：

- **阶梯（stage ladder）**：指用 boot-arg 控制驱动执行到第 N 步、每步失败即停并转储诊断的机制（继承 Cobalt64 的 `navi48-stage` 设计）；本项目的 boot-arg 为 `n48r3-stage=N`。
- **boot-arg 门控**：指用 NVRAM boot-args 开关控制驱动是否加载、执行到哪一步的机制；默认关闭即系统回到纯原生状态。
- **回退**：指通过 boot-arg 开关或删除注入物把系统恢复到前一状态；本阶段每个步骤失败可回退、不 panic。
- **planted break（预埋故障）**：指故意在代码中埋入错误，验证测试确实能失败——测试不能失败就等于测试无效。
- **survey（只读巡检）**：指驱动加载后只读不写地打印硬件全量信息（PCI/BAR/IP 表/固件版本/UMA/EDID/DCN 快照）。

### 0.2 读者与阅读方式

- **接手阶段 1 相关工作的新 AI 会话**：通读本册；结论权威源为本册第 10 章。
- **项目决策者**：读第 1、8、10 章。

### 0.3 与其他册的关系

| 关系 | 册 | 说明 |
|---|---|---|
| 前置 | 10-阶段0-取证与基线.md | 阶段 0 三问有答案、GO 决策签字、移交取证项清单 |
| 后续 | 12-阶段2-上电与固件.md | 阶段 1 的 survey 结论与遗留项是阶段 2 的输入 |
| 引用 | 00-总体路线.md | 硬件基线实测表见其附录 A；寄存器与固件速查见其附录 B；失败模式库见其附录 C；文献表见其参考文献章 |

**参考文献说明**：本册引用编号（如 [35]）与文献表（见 00-总体路线.md）一致。

## 目录

- 1 阶段定位与目标
  - 1.1 目标
  - 1.2 入口条件
  - 1.3 完成标志
- 2 fork 与分支模型
- 3 构建体系
  - 3.1 双目标构建
  - 3.2 Linux 交叉编译链
- 4 IP discovery 与寄存器基址
- 5 只读 survey 与日志
- 6 渐进分档（T1–T6）与首测挂死
  - 6.1 首测挂死与两处根因
  - 6.2 渐进分档设计
- 7 工作项与任务卡
- 8 验证与通过标准
- 9 风险与回退
- 10 结案状态
  - 10.1 最终结论
  - 10.2 T1 轮真机实测
  - 10.3 已知失败模式表
- 11 本章术语

---

## 1. 阶段定位与目标

### 1.1 目标

780M 上只读跑通；双目标构建；IP discovery；日志与回退机制。**不写上电序列**。

### 1.2 入口条件

- 阶段 0 的三份取证报告已签字；
- go/no-go 决策为 TTL 路线（或备选路线已批准）；
- 构建机环境就绪。

### 1.3 完成标志

`TARGET=rdna3` 的 kext 已在目标机加载并完成只读 survey——`ioreg` 能看到自写驱动节点与完整 IP 表，**但 GPU 仍处于固件 GOP 点亮的状态，驱动没有写任何硬件寄存器**。系统此时是"原生桌面 + 一个只读观察者"。

---

## 2. fork 与分支模型

| 分支 | 作用 | 规则 |
|---|---|---|
| `upstream/main` | Cobalt64 上游镜像 | 只读；定期 fetch |
| `main` | 主线（RDNA3） | 所有 RDNA3 工作在此 |
| `rdna4` | RDNA4 保留分支 | 上游快照；回归对照 |
| `feature/*` | 短期特性 | 每任务卡一个 |

完整 fork（保留全部历史、host 测试、planted break、构建脚本——这些是资产）。merge 节奏**按需**（原为每两周，调整为按需：个人自用单机项目，固定短周期 merge 的维护成本大于收益。触发条件：上游有 TTL/HWLibs hook、安全网或构建相关的关键修复，或季度例行检查一次）；冲突热点：Makefile、`src/apple/`、`tools/`；每次 merge 后跑 RDNA4 构建 + host 测试。

---

## 3. 构建体系

### 3.1 双目标构建

**源码现状（校对）**：Cobalt64 没有任何多目标机制——Makefile 无 TARGET 变量，类名/文件名/日志前缀/boot-arg 族（约 48 个 `navi48-*`）/N48N ABI 名/fw_table 的 FwId 枚举全部硬编码 Navi48；两个最大单文件（`AppleHardwareHook.cpp` 约 3.5 万行、`xlat12_ib.c` 约 39 万字节）深度耦合 GFX12 代际。因此"共享层解耦与命名路由"是双目标构建的隐含前提，作为阶段 1 的显式任务卡（TC-03b）交付。

方案：Makefile 增加 `TARGET ?= rdna4`；`TARGET=rdna3` 选 `*_v11_0.cpp`/`*_v13_0.cpp`/`*_v6_0.cpp` 与 `xlat11`、`dcn314`。源文件按 IP 版本命名，不搞 `#ifdef` 大杂烩；共享代码进 `common/`。boot-arg 族按前缀路由（`navi48-*` 与 `n48r3-*` 共用同一解析层）；日志前缀、产物目录随之分离。CI（本地脚本）：两目标构建 + host 测试。

**通过标准**：两命令产出 kext；`nm -u` 只依赖 kernel/IOKit/KPI；无 SSE/x87（沿用 kextcheck）。

### 3.2 Linux 交叉编译链

编译不必须 macOS。已实测的 Linux（Debian 12）交叉编译配方：

- 宿主 clang（`-target x86_64-apple-macos11.0 -mkernel -fapple-kext`）；
- cctools-port 的 Apple ld64（`ld -kext`；LLVM lld 未实现 `-kext`，不可用）；
- MacKernelSDK（纯头文件 + libkmod，无需 macOS SDK）。

Cobalt64 的 Navi48Bringup.kext 已在该环境完整编译并通过离线自检。

**分工**：日常迭代用 Linux 工具链（秒级反馈）；发布产物用 GitHub Actions `macos-latest` runner（生态标准，与 Apple clang 对齐）；加载测试必须真机。

**已知限制**：非 Apple clang 不保证位对齐、LTO 不可用。Makefile 的 Linux 适配见 `Makefile.linux`。

---

## 4. IP discovery 与寄存器基址

复用 `ipdiscovery.cpp` 解析器（从 RDNA4FB 逐字借用的 BSD-3 代码，出处见 [35] 与 Cobalt64 的 NOTICE；签名 0x28211407）。**数据源的正统路径**：IP discovery 表不在 BAR5 寄存器本体，而在 VRAM 顶部的 TMR（10KiB）——须经 MM_INDEX/MM_DATA 间接窗口读入内存再解析（基座项目与 Linux `amdgpu_discovery.c` 均如此；首测挂死的根因之一即此处错位，见 6.1）。

新增 Phoenix IP 表校验：GC 11.0.1、DCN 3.1.4、PSP 13.0.4、SMU 13.0.4、SDMA 6.0.1、VCN 4.0.2、NBIO 7.7、IH 6.0.1（勘误：原写 IH 7.0 系 Navi48 值误移；780M 实机 OSSSYS=6.0.1，nullarc ip_discovery.bin 与 Linux `amdgpu_discovery_set_ih_ip_blocks` 双重实证）、GMC 11.0（以实机表为准，允许 revision 差异；GMC/MES 由 GC 主版本派生校验，Linux `amdgpu_discovery_set_gmc/mes_ip_blocks` 逻辑，无独立 discovery hw id）。每个 IP 的 base 来自 discovery；寄存器偏移来自 Linux 头文件。discovery 不可用则回退 ATOM/VBIOS 解析；再失败拒绝启动（fail closed）。

**通过标准**：`n48r3test info` 打印完整 IP 表，与 Linux 预期一致（允许 revision 差异）。

> 说明：nullarc 为同 IP 版本（GC 11.0.1）对照基准，其平台相关数值不作本机真值，引用处标注需本机复核（全局统一说法，见 00-总体路线.md）。

---

## 5. 只读 survey 与日志

`n48r3bringup=0`（默认）不加载；`n48r3-stage=0`（默认）只读 survey：打印 PCI/BAR/IP/固件版本/UMA/EDID/DCN 快照。日志前缀 `N48R3LOG`，失败带 reason 与寄存器快照。survey 阶段 MMIO 写路径编译期排除或白名单拒绝。

> 说明：survey 承接阶段 0 探针移交的取证项（IP discovery 全表/固件版本/EDID/DPCD/DCN 快照/DIO PWM 状态——探针按精炼原则未做这些，见 10 册 2.3），在自写驱动框架内完成首次全量硬件读取；UMA 已由探针读出，survey 复核一致性即可。

**通过标准**：导出通道落盘的日志可见完整 survey（`grep N48R3LOG <导出文件>`；同步：OC 注入独立 kext 的 IOLog 不进统一日志，阶段 0 实证）；`ioreg` 可见 IP 版本；无 MMIO 写。

> 说明：`n48r3test` 是规划中的诊断工具，阶段 1 实现；此前用 `log show`/`ioreg`/`kmutil showloaded` 代替。

---

## 6. 渐进分档（T1–T6）与首测挂死

### 6.1 首测挂死与两处根因

2026-11-01 首次真机部署挂死（BAR 读总线挂起，整机不可回退）。经调研确证两处实现级偏差：

1. **缺 `setMemoryEnable`**——旧需求"零写红线"把"配置空间"一刀切禁写（禁令清单无意图句），实现按字面跳过了 MMIO 读的这一安全前提。MMIO 读能工作的前提是配置空间的 Memory Space Enable 位已置位；这是读路径的安全前提，不是写操作。
2. **discovery 数据源错位**——把 BAR5 寄存器本体当表扫描，而基座与 Linux 均经 MM_INDEX/MM_DATA 间接窗口读 VRAM 顶部 TMR 10KiB 入内存再解析（见第 4 章）。

修复后架构升级为**渐进分档**（见 6.2）。

### 6.2 渐进分档设计

探测深度由 `n48r3-depth` 控制，分 **T1–T6** 六档，默认 **T1 最小安全验证**；每档带安全前提论证与读副作用审计。运行级前置的深度下限在阶段 2 进一步收紧（阶段 ≥1 巡检下限 T3，见 12 册）。

---

## 7. 工作项与任务卡

任务卡模板沿用全局统一模板（见 00-总体路线.md）。本阶段任务卡：

| 卡号 | 模块 | 目标 | 主要输入 | 通过标准 | 状态 |
|---|---|---|---|---|---|
| TC-03 | fork/构建 | 双目标 | Makefile | 两目标构建 | ✅ 完成 |
| TC-03b | 共享层解耦 | 共享层抽取与命名路由：boot-arg 族（`n48r3-*`）共用解析层、日志前缀、N48N ABI 命名、fw_table FwId 枚举扩展、Makefile TARGET 文件集与产物目录分离（源码现状见 3.1） | Makefile、Navi48Bringup.cpp、N48N ABI 头、fw_table | 双目标构建全绿 + RDNA4 回归通过 | ✅ 完成 |
| TC-04 | IP discovery | 运行时基址 | discovery 表 | info 输出 | ✅ 完成 |
| TC-20 | 加载/签名 | AuxKC（跨阶段 0/1） | OpenCore | 加载 | ✅ 完成 |
| TC-21 | 安全网 | boot-arg/latch/NVRAM 标记 | 两项目实践 | 回退可用 | ✅ 完成 |

---

## 8. 验证与通过标准

### 8.1 通过标准（具体到命令与输出）

| 通过标准 | 具体证据（命令 → 期望输出） |
|---|---|
| 双目标构建 | `make TARGET=rdna4 all` 与 `make TARGET=rdna3 all` 都产出 kext；`dwarfdump --uuid` 可读 |
| 符号纯净 | `nm -u -arch x86_64 <binary>` 只含 kernel/IOKit/KPI 符号；kextcheck 无 SSE/x87 |
| IP discovery | `n48r3test info` 打印 IP 表：GC 11.0.1、DCN 3.1.4、PSP 13.0.4、SMU 13.0.4、SDMA 6.0.1、VCN 4.0.2，各 IP base 与阶段 0 取证一致 |
| 只读 survey | 导出通道落盘的日志可见 PCI/BAR/IP/UMA/EDID/DCN 快照全项（`grep N48R3LOG <导出文件>`，同步）；无任何状态类寄存器写 |
| 默认不加载 | `n48r3bringup=0`（默认）时 `kmutil showloaded` 无本驱动；`n48r3bringup=1` 才加载 |
| 上游不破坏 | `TARGET=rdna4` 构建 + host 测试全绿（`make -C src/xlat12 test` 等） |
| host 测试 | xlat11/dcn314 的单测 + planted break 全部通过（planted break 必须先失败后修复） |

> 说明：阶段 1 的验收编号为 V1–V12；其中 V10 为回退演练，V 项定义在阶段结案时经评审定稿。上表为命令级通过标准，与 V 项一一对应。

### 8.2 证据要求

- 证据批次按 `boot-<日期>-<主题>` 命名：T1 轮（批次含甄别声明与结论摘要）与回退演练（V10）各一批；
- 机器配置基线已用实测值覆盖 carveout"待取证"项；
- 取证形态变更：证据由主 agent SSH 经 ioreg 直接提取（职责分离——用户只负责改 boot-args 与重启，不安装守护、不执行脚本）；原规划的导出守护（LaunchDaemon 自动落盘）废止，其职责由 SSH 提取替代。

**soak（长稳）要求**：本阶段无 soak 要求；soak 阶梯自阶段 3 起（见 00-总体路线.md）。

---

## 9. 风险与回退

**回退路径**：`n48r3bringup=0`（驱动不加载，系统回到纯原生）；`n48r3-stage=0`（加载但只读）；删除 `build/rdna3/` 不影响 RDNA4。

**吸收 Cobalt64 的阶段阶梯**：`n48r3-stage=N` 控制执行到第 N 阶（对应 Cobalt64 的 `navi48-stage`），每阶失败即停并 dump VM fault/IH 诊断（`dump_failure_state`），后续阶段不执行。

**风险条目**：

1. MMIO 读的前提（配置空间 Memory Space Enable）曾被"零写红线"误禁——已修正为"读路径安全前提"单独论证（D18）；
2. discovery 数据源必须走 MM_INDEX/MM_DATA 间接窗口——已固化为实现约束；
3. 非 Apple clang 不保证位对齐、LTO 不可用——发布产物走 GitHub Actions `macos-latest` 规避；
4. 首测挂死形态（BAR 读总线挂起、整机不可回退）——由渐进分档 T1–T6 把探测深度切成可回退的小步（D19/D20）。

---

## 10. 结案状态

### 10.1 最终结论

**阶段 1 已完成并归档（2026-11-01）**：**V1–V12 全 PASS**（V1/V2/V3/V8/V9 离线；V4–V7/V10–V12 真机）；回退演练（V10）PASS——boot-args 回原生原值、无 IOService 节点、取证属性计数 0、桌面正常。系统处于"原生桌面 + 一个只读观察者"完成状态。遗留项（阶段 2 输入）已移交阶段 2。

### 10.2 T1 轮真机实测（修复后）

| 项 | 实测 |
|---|---|
| 驱动加载 | com.navi48.bringup 0.1.0 已加载（kmutil 确认） |
| T1 档完成 | `N48R3EVID-depth="T1 depth=0 reached"`、`n48r3-Done="Yes"` |
| 访问使能（挂死根因修复实证） | read-manifest `0x00000de3=1`——首次 MMIO 读成功且全程仅此一次读 |
| UMA carveout（D2 取证目标） | **实测 4096 MiB**（raw=0x00001000，d0=assumed 保守口径） |
| 基线一致性（D10） | dev=0x15bf rev=0xc7 bar0=0x8d00000000/256MiB——与阶段 0 一致 |

### 10.3 已知失败模式表

| # | 现象 | 根因 | 处理 |
|---|---|---|---|
| 1 | 首次真机部署挂死：BAR 读总线挂起，整机不可回退 | ①缺 `setMemoryEnable`——"零写红线"把配置空间一刀切禁写，跳过了 MMIO 读的安全前提；②discovery 数据源错位——把 BAR5 寄存器本体当表扫描，正统路径是经 MM_INDEX/MM_DATA 间接窗口读 VRAM 顶部 TMR 10KiB 入内存再解析 | 修复两处偏差；架构升级为渐进分档 T1–T6（`n48r3-depth`，默认 T1 最小安全验证，每档带安全前提论证与读副作用审计）；禁令清单补意图句 |
| 2 | 阶段 0 移交的固件版本寄存器读数经错误段址 | 段基址定位错误（详见 10 册 4.2 归属表） | 阶段 2 以 PSP 版本基线重建、RLC/SDMA 在各自子步读取（D3） |

---

## 11. 本章术语

本册首现、且未收入全局术语表（见 00-总体路线.md）的术语：

| 术语 | 一句话解释 |
|---|---|
| cctools-port | 把 Apple 工具链（含 ld64 链接器）移植到 Linux 的项目；本项目的内核态链接依赖其 `ld -kext` |
| MacKernelSDK | 纯头文件 + libkmod 的 macOS 内核开发 SDK，无需完整 macOS SDK 即可编译 kext |
| `-fapple-kext` / `-mkernel` | clang 的内核态编译开关（禁用部分 ABI 特性以适配 XNU 内核环境） |
| `ld -kext` | Apple ld64 链接器的 kext 链接模式；LLVM lld 未实现该模式 |
| AuxKC | OpenCore 注入非 Apple kext 后生成的辅助内核集合（阶段 0 册已释，此处为加载路径语境） |
| host 测试 | 在构建机（宿主 Linux）上运行的纯函数测试（含 ASan/UBSan、golden、fuzz、planted break），不需要真机 |
| planted break | 预埋故障：故意埋错验证测试能失败；测试不能失败即测试无效 |
| TMR（10KiB 语境） | VRAM 顶部的 Task Management Region 存根区，IP discovery 表所在处，经 MM_INDEX/MM_DATA 间接窗口读取（阶段 2 的 64 MiB TMR 是另一同名词，见 12 册） |
| MM_INDEX / MM_DATA | GPU 的间接寄存器访问窗口：向 MM_INDEX 写地址、从 MM_DATA 读写数据 |
| fail closed | 取证/校验失败时拒绝启动，而不是带病继续 |
| ATOM/VBIOS | GPU 固件中的 BIOS 表解析路径，作为 IP discovery 不可用时的回退数据源 |
| read-manifest | 驱动登记的"读过的地址清单"属性，用于审计读路径（如 `0x00000de3=1` 即 Memory Space Enable 位） |
| d0=assumed | T1 轮读 UMA 时电源态按 D0 假定的保守口径标注 |

