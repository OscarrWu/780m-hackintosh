# 15-阶段5-Metal

## 0. 册头

- **编写意图**：本册承载阶段 5（Metal）的全部规划内容，并包含归属阶段 5 的验证条目与任务卡。阶段 5 尚未开工，本册写的是当前事实与计划基准，供开工时直接作为任务依据使用。
- **读者**：项目经理、负责阶段 5 实施的程序员与技术员、需要了解后续路线的取证人员。
- **与其他册的关系**：
  - 前置册：`13-阶段3-内存与队列.md`（用户态 ABI 与 compute 闭环）、`14-阶段4-显示与加速.md`（4a 显示宿主与 4b Apple 集成层两篇均需完成）。
  - 后续册：`16-阶段6-日常化.md`（承接本册的首期验收成果）。
  - 全局路线、硬件基线表、寄存器与固件速查、失败模式库见 `00-总体路线.md`（附录 A/B/C 归 00 册，本册只引用不复制）。
- **引用约定**：正文方括号编号（如 [37]）沿用项目统一文献编号，文献表见 `00-总体路线.md`；本项目文档按仓库根相对路径引用。
- **流程词说明**：取证（从实机与归档材料中提取事实证据的过程）、探针（临时部署到目标机采集运行时数据的观测工具）、回退（退回上一稳定状态）为本册使用的流程词，含义全项目一致。

## 1. 目录

- 1. 册头
- 2. 阶段定位与目标
  - 2.1 一段话定位
  - 2.2 入口条件（进入本阶段的前提）
  - 2.3 完成标志
- 3. 主体工作
  - 3.1 RADV Darwin 后端的 APU 适配
  - 3.2 Metal 插件 navir3metal
  - 3.3 WindowServer 与应用渲染接入
  - 3.4 发布门控、白名单与运行时安全网
- 4. 工作项与任务卡
  - 4.1 TC-17 RADV APU 适配
  - 4.2 TC-18 Metal 插件
  - 4.3 TC-19 WindowServer 接入
  - 4.4 TC-27 Metal 发布安全网（新编）
- 5. 验证与通过标准
  - 5.1 验证层次
  - 5.2 V 项清单
  - 5.3 稳定性考核（soak）与性能守门指标
  - 5.4 证据要求
- 6. 风险与回退
  - 6.1 相关风险条目
  - 6.2 回退路径
- 7. 结案状态
- 8. 本章术语

## 2. 阶段定位与目标

### 2.1 一段话定位

阶段 5 要解决的问题是：让**应用**的 3D 渲染也落到 780M 上。阶段 4 完成后，桌面合成已经走 GPU（"看起来正常的 Mac"），但应用自己的 Metal 渲染仍走 CPU 或尚未接管。本阶段通过两条用户态链路补齐：一是给 Mesa 的 RADV（AMD Vulkan 驱动 [28]）打 APU 适配补丁，使 Vulkan 能看到 780M；二是把 Cobalt64 的 Metal 插件复制改造为 navir3metal，让应用的 Metal 调用经 AIR→SPIR-V 翻译进入 RADV，最终在 780M 上执行。两条链路打通后，App 渲染与桌面合成落在同一块 GPU 上，达成首期验收。

**两条路径的定义**（此处集中定义）：

| 路径 | 含义 | 打通阶段 |
|---|---|---|
| 路径 B | 桌面合成路径：WindowServer 经 Apple 加速器路径（IOGraphicsAccelerator2 + xlat 翻译）在 GPU 上合成桌面 | 阶段 4（4a/4b） |
| 路径 A | 应用渲染路径：应用的 Metal 调用经 navir3metal→RADV→780M 执行 | 阶段 5（本册） |

### 2.2 入口条件（进入本阶段的前提）

| 前提 | 出处 |
|---|---|
| 阶段 4a 显示宿主通过：内屏点亮、背光可调、WindowServer 已在 GPU 上合成桌面（"看起来正常的 Mac"里程碑达成） | 14 册 4a 篇 |
| 阶段 4b Apple 集成层通过：TTL 对象创建、xlat11 翻译、加速器激活（IOGraphicsAccelerator2 路径生效） | 14 册 4b 篇 |
| 阶段 3 用户态 ABI 与 compute 闭环通过（compute magic 正确） | 13 册 |
| RADV 的 APU 补丁与 Metal 插件已通过 host 测试（纯函数、ASan/UBSan、golden） | 本册 4.1/4.2 的 host 侧前置 |

### 2.3 完成标志

**首期验收达成**——App 的 3D 渲染也走 780M（路径 A 打通），两条路径（App 渲染 + 桌面合成）都落在同一块 GPU 上。Safari/Chromium/办公 App 正常渲染，桌面 90Hz 流畅。这是"日常可用"里程碑：浏览器、文档、终端、轻度开发全部可用（硬解/睡眠/外接显示除外，这三项归阶段 6）。

## 3. 主体工作

### 3.1 RADV Darwin 后端的 APU 适配（TC-17）

Cobalt64 的 9 个补丁（10,554 行）建立了 RADV 的 Darwin 后端：canned device（预置设备表，RDNA4 机型为 gfx1201）、N48N 连接、scanout API、BO 导入、VRAM budget、GB_ADDR_CONFIG、service class、selector 22.26。本阶段的 RDNA3 适配有三项：

1. **canned device 换 gfx1103**。nullarc 的 `mesa-patches-phoenix/` 7 个补丁已完成同类工作 [28][37]：`RADV_DARWIN_CANNED` 环境变量选 phoenix 表、GB_ADDR_CONFIG 0x242、780M 身份与 PCI ID。**注意**：GB_ADDR_CONFIG 0x242 是 nullarc 同 IP 版本（GC 11.0.1）实测机的对照值，**不作本机真值，引用处需本机复核**（本机为 ThinkBook 14+ / 0x15BF，nullarc 实测机为 Firebat U6C / 0x1900）。
2. **堆/内存类型**：APU 的 DEVICE_LOCAL 与 GTT（Linux 对 GART 的叫法，图形地址翻译表）是同一块系统内存，`radv_physical_device` 要识别 UMA（统一内存架构：核显与 CPU 共用系统内存的一段 carveout）。UMA carveout 本机实测 4096 MiB（见 00 册附录 A）。
3. **scanout ABI 对接**阶段 4 的 N48N scanout selectors（显示扫描输出接口）。

**通过标准**：`vulkaninfo` 显示 780M；`vkprobe` 创建 device/queue 并跑通 compute（nullarc 实证 256/256 可作基准 [37]）。

### 3.2 Metal 插件 navir3metal（TC-18）

复制 Cobalt64 的 `navi48metal` 为 `navir3metal`，改造点：

- 设备描述改 780M / gfx1103 / 对应 feature set；
- AIR→SPIR-V 翻译复用 `metal2vulkan` [31] + `n48xlate`（AIR 是 Metal 着色器格式，SPIR-V 是 Vulkan 着色器中间格式）；
- 纹理/格式按 gfx1103 能力表；
- 扫描输出接 DCN 3.1.4 present。

**先例参照** [37][40]：NullMoth 插件（NVMTLDriver.bundle）在 AMD 模式下可用，Metal 3 报告、白名单分发、shader 首译 1–3 秒后磁盘缓存。这些是 nullarc/NullMoth 的事实参照；**本阶段 Metal 测试对象是本驱动经 navir3metal 暴露的 Metal 设备**，合成机制走正统 Apple 加速器路径（IOGraphicsAccelerator2 + xlat 翻译），不走 nullarc 的 Metal 插件机制。

**通过标准**：`mtlprobe` 返回 AMD Radeon 780M；command buffer 提交；清屏/三角形正确。

### 3.3 WindowServer 与应用渲染接入（TC-19）

阶段 4（4a/4b）已让桌面合成走加速器路径。本节工作是把**应用渲染路径**接入同一设备，并补齐运行时配套：

- WindowServer 识别 Metal 设备：GPUWrangler 学到 accelerator 后发布（nullarc 实证 [37]：`--windowserver` 两次 boot——第一次 GPUWrangler 学 accelerator，第二次合成生效）；
- surface 表 / flip / fence 对接；
- CoreAnimation present；
- GPU hang 监控与回退（见 3.4）。

**验收观测走 Apple 路径判据**（nullarc 的 `sysctl debug.nvaccel_iop_ok`、`CompositorMetal::CompositeLayersToDestination` 是其 Metal 插件机制的度量，不适用本路线）：

- ioreg 中 IOGraphicsAccelerator2（或其子类）服务出现并绑定 780M；
- 本驱动 xlat11 翻译计数随桌面/应用活动增长；
- `sample WindowServer` 栈中可见合成调用经本驱动加速器（而非 CPU 软件合成路径）。

上述观测通道的具体接口（如本驱动暴露的 sysctl/日志计数器名）**待取证**，在 TC-19 实施时定稿。

**通过标准**：WindowServer GPU 合成；桌面 90Hz；Safari 3 标签不崩；拖窗流畅。

### 3.4 发布门控、白名单与运行时安全网（TC-27）

| 机制 | 内容 | 来源 |
|---|---|---|
| 发布门控（accel-gate） | compute 自检（阶段 3 的 magic 测试）通过才允许发布 Metal 设备（gate=1 默认）；自检未过则设备不出现——宁可不发布，不发布坏的 | 吸收 Cobalt64 accel-gate |
| 白名单分发 | Metal 设备按进程白名单分发：`/Library/GPUBundles/n48r3-allow.txt`，`*`=全部、`-name`=拒绝、`!name`=armed 后允许；拒绝列表含 mediaanalysisd/photoanalysisd/Spotlight（Espresso 卷积内核会挂 GPU） | nullarc 实证 [37] |
| hang 自动恢复 | 会话中 GPU hang → WindowServer 反复挂 → 5 分钟内 2 次 hang 报告重启、1 小时内 3 次重启 → 回固件帧缓冲重启（`m2_fail` 机制） | 吸收 nullarc phxboot [37] |
| kill switch | `touch /Users/Shared/gpu/n48r3-off` 一条命令停掉一切自动行为 | 吸收 Cobalt64 kill file 通道 |

白名单起步取保守集合（上述拒绝列表）；进一步收敛归阶段 6（见 16 册）。

## 4. 工作项与任务卡

任务卡模板（目标/输入/输出/通过标准/禁止/人工上机点/回退）。TC-27 为新编条目（阶段 4 册新编至 TC-26，本册从 TC-27 起编，新编条目已在交付摘要中说明）。

### 4.1 TC-17 RADV APU 适配

- **目标**：RADV 在真机枚举出 780M，且 vkprobe compute 闭环通过。
- **输入（上下文包）**：nullarc `mesa-patches-phoenix/` 7 补丁 [28][37]；Cobalt64 Darwin 后端 9 补丁（canned device/N48N 连接/scanout API/BO 导入/VRAM budget/GB_ADDR_CONFIG/service class/selector 22.26）；阶段 3 UMA 取证（carveout 4096 MiB）；阶段 4 N48N scanout selectors；上游 Mesa RADV [28]。
- **输出**：RADV gfx1103 补丁集（canned phoenix 表、UMA 堆识别、scanout 对接）；host 测试。
- **通过标准**：`vulkaninfo` 显示 AMD Radeon 780M（gfx1103）；`vkprobe` 创建 device/queue 并 compute 通过（对照 nullarc 256/256 基准 [37]）。
- **禁止**：不猜 GB_ADDR_CONFIG——0x242 为 nullarc 对照值，须以本机 IP discovery 取证复核；不跨阶段改内核侧；不默认开 boot-arg。
- **人工上机点**：真机运行 vulkaninfo/vkprobe 并收集输出。
- **回退**：用户态组件，不装补丁即回原状，不影响内核与显示。

### 4.2 TC-18 Metal 插件

- **目标**：mtlprobe 返回 780M，基本渲染（清屏/三角形）正确。
- **输入**：Cobalt64 `navi48metal/`；`metal2vulkan` [31] + `n48xlate`；gfx1103 纹理/格式能力表（以 Mesa gfx11 寄存器/格式库为准 [28]，具体能力清单待取证）；NullMoth 先例 [40]；DCN 3.1.4 present（阶段 4 产物）。
- **输出**：`navir3metal` bundle；格式能力表；host 测试。
- **通过标准**：`mtlprobe` 返回 `AMD Radeon 780M`；command buffer 提交；清屏/三角形正确。
- **禁止**：不绕过 accel-gate 直接发布设备；不引入 nullarc Metal 插件机制。
- **人工上机点**：真机安装 bundle 后运行 mtlprobe。
- **回退**：不安装 bundle 即回原状。

### 4.3 TC-19 WindowServer 接入

- **目标**：App 渲染与桌面合成落在同一块 GPU 上（路径 A + 路径 B 同卡）。
- **输入**：`ws_*`（Cobalt64 WindowServer 对接层）；nullarc M2 实证 [37]；阶段 4b 的 IOGraphicsAccelerator2/xlat11 产物；白名单文件。
- **输出**：WindowServer 识别 Metal 设备的对接代码；surface 表/flip/fence 对接；Apple 路径观测通道（计数器/日志，接口待取证后定稿）。
- **通过标准**：V5-7/V5-8（见 5.2）。
- **禁止**：不以 `debug.nvaccel_iop_ok`/CompositorMetal 等 nullarc 插件度量作验收；不自动重启。
- **人工上机点**：两次 boot 观察（第一次 GPUWrangler 学 accelerator，第二次合成生效）；Safari/Chromium 实测。
- **回退**：`n48r3-metal=0` 回 CPU 合成（桌面仍可用，只是 App 渲染走 CPU）。

### 4.4 TC-27 Metal 发布安全网（新编）

- **目标**：发布门控、白名单分发、hang 自动恢复、kill switch 四项安全机制在真机生效并可演练。
- **输入**：Cobalt64 accel-gate 与 kill file 通道；nullarc phxboot fail-safe 与 allow.txt 机制 [37]；阶段 3 compute 自检（magic 测试）。
- **输出**：accel-gate 接线（compute 自检→设备发布）；`/Library/GPUBundles/n48r3-allow.txt` 及加载逻辑；hang 计数与 `m2_fail` 回固件帧缓冲重启逻辑；kill switch 文件监听。
- **通过标准**：V5-5/V5-6/V5-11/V5-12（见 5.2）。
- **禁止**：不默认关闭 gate；白名单不写空（空文件行为必须定义并有测试）；hang 恢复阈值不随意放宽。
- **人工上机点**：hang 演练时的人工恢复（长按电源键）；kill switch 验证。
- **回退**：kill switch（`touch /Users/Shared/gpu/n48r3-off`）+ `n48r3-metal=0`。

## 5. 验证与通过标准

### 5.1 验证层次

沿用项目统一五层验证（host 测试/构建检查/真机 smoke/回归/长稳），本阶段真机 smoke 即下表 V 项；长稳层在本阶段启动（8 小时档，完整基准归 16 册）。每项明确 PASS/FAIL/SKIPPED；失败必须带证据（命令输出/寄存器值/日志行）。PASS/FAIL 记录方式借鉴 Raphaeli [30] 与 RDNA4FB real-card-plan；失败处理流程（记录→latch off→回退→回顾→不掩盖）见 00 册统一验证章。

### 5.2 V 项清单

| V 项 | 内容 | 命令/方法 → 期望 |
|---|---|---|
| V5-1 | RADV 设备枚举 | `vulkaninfo` → 显示 AMD Radeon 780M（gfx1103） |
| V5-2 | compute 闭环 | `vkprobe` 创建 device/queue 并 compute 通过（对照 nullarc 256/256 基准 [37]） |
| V5-3 | Metal 枚举 | `mtlprobe` → 返回 `AMD Radeon 780M` |
| V5-4 | 基本渲染 | command buffer 提交，清屏/三角形输出正确 |
| V5-5 | 发布门控生效 | accel-gate=1 默认；compute 自检未过时 Metal 设备不出现（负路径用 planted break 验证） |
| V5-6 | 白名单分发 | `*`/`-name`/`!name` 三种规则行为正确；mediaanalysisd/photoanalysisd/Spotlight 被拒 |
| V5-7 | WindowServer 合成（Apple 路径判据） | ioreg 见 IOGraphicsAccelerator2 服务绑定 780M；xlat11 翻译计数随活动增长；`sample WindowServer` 栈中合成经本驱动加速器 |
| V5-8 | 应用渲染 | Safari 打开 3 个标签不崩；Chromium/Electron 正常；办公 App 正常；拖窗流畅 |
| V5-9 | 性能守门指标 | 见 5.3 四项 |
| V5-10 | 长稳 | 8 小时长稳无 hang |
| V5-11 | hang 自动恢复 | 会话中 hang → 5min 内 2 次 / 1h 内 3 次 → 回固件帧缓冲重启（`m2_fail`）；人为触发 hang 的安全演练方法待取证 |
| V5-12 | kill switch | `touch /Users/Shared/gpu/n48r3-off` 后一切自动行为停止 |
| V5-13 | 回退路径 | `n48r3-metal=0` 回 CPU 合成，桌面仍可用 |

### 5.3 稳定性考核（soak）与性能守门指标

soak（稳定性考核，逐阶段加长时间的安排）阶梯中本阶段为 **8 小时长稳**：单次开机连续 8 小时混合使用（浏览器/文档/终端/轻度开发）无 hang、无 panic。

首期性能守门指标（完整性能基准归 16 册/阶段 6）：

| 指标 | 门槛 |
|---|---|
| fence 完成延迟 | <1ms |
| vkprobe | 256/256 通过 |
| 窗口拖动 | 无可感知卡顿 |
| 90Hz 实际扫描 | 无降档（承接阶段 4 的扫描证据通道） |

### 5.4 证据要求

- 每项 V 项记录 PASS/FAIL/SKIPPED；FAIL 必须附命令输出/寄存器值/日志行。
- 证据归档；证据键沿用 `boot-<日期>-<主题>` 命名（先例：boot-20261029-ssh）。
- 长稳与 hang 演练的日志（含 hang 前最后一条日志）必须归档，供失败模式库（00 册附录 C）收录。

## 6. 风险与回退

### 6.1 相关风险条目

| ID | 风险 | 在本阶段的表现 | 缓解 |
|---|---|---|---|
| R2 | APU 的 UMA 无法呈现为可用 VRAM/GART | RADV 堆识别失败（DEVICE_LOCAL/GTT 同源判断错误） | 阶段 3 UMA 取证在手；堆识别有 host 测试；不成则不发布 RADV 设备 |
| R5 | GPU hang 整机无响应 | App 渲染引入后 hang 面变大（Espresso 等进程） | 发布门控 + 白名单拒绝高危进程 + hang 自动恢复 + kill switch |
| R8 | 上游演进 merge 冲突 | RADV 补丁集与 Mesa 上游漂移 | 按需 merge；RDNA4 回归必跑 |
| R10 | 面板实际不是 90Hz/需 DSC | 90Hz 实扫守门指标失真 | 以 EDID 为准（本机实测 2880×1800@90.006Hz）；DSC 路径预留 |
| R11 | Phoenix（GC 11.0.1）与对照基准的寄存器差异 | GB_ADDR_CONFIG 等 nullarc 对照值不适用本机 | nullarc 为同 IP 版本（GC 11.0.1）对照基准，其数值一律本机复核 |
| R13 | 许可证冲突 | navir3metal 复制 Cobalt64 代码的分发限制 | 个人自用；公开前替换/重写 |

### 6.2 回退路径

| 场景 | 动作 |
|---|---|
| App 渲染异常 | `n48r3-metal=0` 回 CPU 合成（桌面仍可用，只是 App 渲染走 CPU） |
| 会话中 GPU hang | 自动：5min 内 2 次 / 1h 内 3 次 → `m2_fail` 回固件帧缓冲重启 |
| 一切自动行为急停 | `touch /Users/Shared/gpu/n48r3-off` |
| 阶段深度控制 | `n48r3-stage=N` 阶段阶梯；每阶失败即停 + 诊断转储 + 逆序 teardown |

## 7. 结案状态

**未开工。** 前置阶段 3（内存与队列）、阶段 4（显示与加速）尚未完成。本册为规划口径；开工时以本册任务卡为依据，实测数据回填后更新。

## 8. 本章术语

本册首现、且 00 册全局术语表未覆盖的术语：

| 术语 | 解释 |
|---|---|
| canned device | RADV Darwin 后端的预置设备表：不依赖运行时枚举，直接按表声明一台设备（gfx1201/gfx1103）。 |
| navir3metal | 本项目的 Metal 插件，由 Cobalt64 的 navi48metal 复制改造，面向 gfx1103。 |
| accel-gate / 发布门控 | compute 自检通过才允许向系统发布 Metal 设备的机制（源自 Cobalt64）。 |
| vkprobe / mtlprobe | 用户态探测工具：前者验证 Vulkan 设备与 compute，后者验证 Metal 设备枚举与渲染。 |
| GTT | Graphics Translation Table，Linux amdgpu 对 GPU 地址翻译表（GART 一族）的叫法；APU 上与系统内存同源。 |
| Espresso | Apple 的图像分析/卷积框架（mediaanalysisd 等进程使用），其卷积内核在本驱动上会挂 GPU，故白名单拒绝。 |
| NVMTLDriver | NullMoth 的 Metal 驱动 bundle，作为 Metal 插件在 AMD 模式下可用的事实先例 [40]。 |
| m2_fail | nullarc phxboot 的 fail-safe 机制名：达到 hang 重启阈值后回固件帧缓冲重启 [37]。 |
| kill switch | 运行时急停通道：创建约定文件（`/Users/Shared/gpu/n48r3-off`）即停掉一切自动行为。 |
| WindowServer | macOS 的窗口合成与显示管理进程，桌面合成的执行者。 |
| CoreAnimation | macOS 的图层动画与合成框架，present（上屏提交）经它进入 WindowServer。 |
| surface 表 / flip / fence | 应用渲染上屏的三要素：表面地址表 / 翻页提交 / 完成信号。 |
