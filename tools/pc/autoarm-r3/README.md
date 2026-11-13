# autoarm-r3 — 阶段 1 导出通道（com.n48r3.export）

本目录交付阶段 1（Cobalt64-RDNA3）的**数据落盘导出链**（需求 E1/E2/E3、V5/V11；决策 D7）。
形态沿用阶段 0 `com.n48r3.probe-export` 的实证模式：kext 把日志/取证件 setProperty 到自身
IORegistry 节点 → LaunchDaemon（RunAtLoad）轮询完成标志 → 拼装分块 → 落盘 → 构建机 scp 拉回。
**本卡只交付代码与部署文档；真机部署由人工执行，脚本不连接目标机。**

## 契约（与 T1c 固化 / T2a 侧一致）

| 项 | 值 |
|---|---|
| 节点 IOClass | `NaviR3Bringup`（可用 `N48R3_IOCLASS` 覆盖） |
| 日志分块属性 | `N48R3LOG-%04u`（`0001` 起，最大 `9999`），每块 1024B |
| 完成标志属性 | `n48r3-Done` |
| 导出根目录 | `/var/tmp/n48r3-export/<UTC 时间戳>/` |
| 脚本日志 | `/Library/Logs/N48R3/export.log` |
| LaunchDaemon Label | `com.n48r3.export`（`RunAtLoad=true`） |

分块值兼容两种 ioreg 表现：**OSData**（`<hex...>`，含 ioreg 折行）与 **OSString**（`"..."`）。
日志块是文本（无 NUL），由脚本内置 awk 解码拼装；取证件可能是二进制（如 EDID 含 NUL），
保留原始 hex 后用 perl/xxd 做 NUL 安全解码。

## 文件

| 文件 | 作用 |
|---|---|
| `export.sh` | 主脚本：轮询节点/完成标志 → 按序拼装分块 → 落盘 → 置本 boot 幂等标记 |
| `com.n48r3.export.plist` | LaunchDaemon 定义（`/bin/bash /usr/local/bin/n48r3-export.sh`，RunAtLoad） |
| `uninstall.sh` | 卸载 daemon + 删除安装文件；默认保留导出件/日志，`--purge` 才删 |
| `test/run-tests.sh` | 本地 fake 测试（fake ioreg/sysctl/launchctl 经 PATH 注入） |
| `test/fakebin/` | fake 命令桩 |

## 导出物

```
/var/tmp/n48r3-export/<UTC 时间戳>/
├─ n48r3-log.txt      主通道：按块序号拼装后的全文（验收：grep N48R3LOG n48r3-log.txt）
├─ registry.txt       节点全属性 dump（人读取证；含分块原始值）
├─ n48r3-export-notes.txt  导出说明：complete / 块数 / 断号序号 / 证据件数
└─ evidence/<KEY>     取证件（若存在；属性名以 N48R3EVID- 开头，原样落盘）
```

`export.log` 记录节点发现/完成标志/超时/断号/证据件数量等过程信息。

### 健壮性行为

- 节点 60s（12×5s）宽限内不出现 → 跳过并记录（kext 未加载，**不是失败**）。
- 完成标志轮询上限 180s（36×5s）；超时仍导出（部分数据也是证据），日志标 `INCOMPLETE`。
- 分块缺失 / 块序号断号 → 拼装已存在的块，`MISSING chunk N48R3LOG-NNNN` 写入 export.log，断号也记入 `n48r3-export-notes.txt`。
- 半截块（数据块未闭合）→ 尽力保留已收字节，不崩溃。
- 同一 boot 幂等：导出完成后写 `/var/tmp/n48r3-export-done-<boottime.sec>`，再次运行直接跳过；
  换 boot（新 boottime）自动重新导出。

## 人工部署（阶段 1 真机，人工执行）

> 以下动作**由最终用户在 PC 上执行**；构建机只负责把文件传过去。

### 1. 放文件

```bash
# 在 PC 上（root）：
sudo mkdir -p /usr/local/bin /Library/Logs/N48R3
sudo install -m 755 export.sh            /usr/local/bin/n48r3-export.sh
sudo install -m 644 com.n48r3.export.plist /Library/LaunchDaemons/com.n48r3.export.plist
sudo chown root:wheel /Library/LaunchDaemons/com.n48r3.export.plist
```

### 2. 加载 daemon（Ventura：system 域 bootstrap）

```bash
sudo launchctl bootstrap system /Library/LaunchDaemons/com.n48r3.export.plist
# 立即（重新）运行一次：
sudo launchctl kickstart -k system/com.n48r3.export
# 查看进程/日志：
sudo launchctl print system/com.n48r3.export | head -20
tail -n 50 /Library/Logs/N48R3/export.log
```

RunAtLoad 会在每次开机自动执行一次。kext 未加载时脚本会跳过（不产生导出目录）。

### 3. 人工兜底重跑

```bash
# 同一 boot 已有导出时，默认跳过；加 N48R3_FORCE=1 强制重导：
sudo N48R3_FORCE=1 /usr/local/bin/n48r3-export.sh
```

### 4. 构建机拉回

```bash
scp -r root@<mac-ip>:/var/tmp/n48r3-export/<时间戳>/ ./n48r3-export-<时间戳>/
# 验收：
grep -c N48R3LOG n48r3-export-<时间戳>/n48r3-log.txt
```

### 5. 卸载

```bash
sudo ./uninstall.sh            # 卸载 daemon + 删除脚本/plist/幂等标记，保留导出件与日志
sudo ./uninstall.sh --purge    # 额外删除 /var/tmp/n48r3-export 与 /Library/Logs/N48R3
```

## 本地测试（Linux 宿主可直接跑）

```bash
bash test/run-tests.sh
```

fake 测试通过 PATH 注入 `test/fakebin/{ioreg,sysctl,launchctl}`，不接触真机、不读真实注册表。
覆盖：OSData/OSString 拼块、ioreg 折行、0/1 基序号、断号/半截容错、Done 超时、节点缺失跳过、
同 boot 幂等 + FORCE 重导 + 跨 boot、取证件（含 NUL）、awk 失败不置完成标记、uninstall、plist 结构、`bash -n`。

## 环境变量（测试/诊断用；默认值即部署值）

| 变量 | 默认 |
|---|---|
| `N48R3_IOCLASS` | `NaviR3Bringup` |
| `N48R3_IOREG` / `N48R3_SYSCTL` | `ioreg` / `sysctl` |
| `N48R3_OUTDIR` | `/var/tmp/n48r3-export` |
| `N48R3_LOGDIR` | `/Library/Logs/N48R3` |
| `N48R3_MARKERDIR` | `/var/tmp` |
| `N48R3_POLL_S` / `N48R3_NODE_GRACE` / `N48R3_DONE_POLL` | `5` / `12` / `36` |
| `N48R3_GAP_LIMIT` | `64` |
| `N48R3_EVID_PREFIX` | `N48R3EVID-` |
| `N48R3_FORCE` | 未设（设 `1` 忽略本 boot 幂等标记） |

若要把覆盖值固化进 daemon（而不是手工 `export`），在 plist 中追加：

```xml
<key>EnvironmentVariables</key>
<dict>
    <key>N48R3_EVID_PREFIX</key><string>N48R3EVID-</string>
</dict>
```

## 假设 / 待确认（与 T2a 侧接）

1. **IOClass** 默认按技术方案 §4.10 取 `NaviR3Bringup`；若最终类名不同，用 `N48R3_IOCLASS` 覆盖。
2. **取证件属性名前缀** 文档未固化，暂定 `N48R3EVID-`（如 `N48R3EVID-edid`）；最终命名由 T2a/kext
   决定，若不同请用 `N48R3_EVID_PREFIX` 覆盖（或告知本卡调整默认值）。
3. 分块序号**兼容 0 基与 1 基**：从最小已出现序号拼到最大，区间内断号记 `GAP`；块值按字节原样拼接
   （块内换行由 kext 保证），脚本不做行级重组。
