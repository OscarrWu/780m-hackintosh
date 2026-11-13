#!/bin/sh
# n48r3-logstream.sh — 持久化 kext n48log 环到磁盘，由 launchd (com.n48r3.logstream) 托管。
# 不得通过 nohup/sudo 常驻。
#
# 调用形态（参数名为暂定名，以工具实现为准）：
#   n48r3test logstream <OUTFILE> <INTERVAL_MS> <MAX_S> <STOPFILE>
#
# 正常退出（rc=0）：stopfile 出现或 max_s 已到 → launchd 不重启。
# 异常退出（rc≠0）：工具/kext 未就绪 → 脚本内有限重试 → 仍失败则交 launchd 重启。

set -u

LOGFILE="/var/tmp/n48r3-driver.log"
STOPFILE="/var/tmp/n48r3-logstream.stop"
N48R3TEST="/var/tmp/n48r3test"
INTERVAL_MS="200"
MAX_S="86400"

MAX_RETRIES=6          # 重试次数上限
BASE_SLEEP=2           # 指数退避基秒数

# ---------- 1. 清理陈旧 stopfile（跨重启残留）----------
rm -f "$STOPFILE"

echo "n48r3-logstream.sh: starting (pid $$), logfile=$LOGFILE stopfile=$STOPFILE"

# ---------- 2. 带指数退避的重试循环 ----------
# 统一退避：无论工具不存在还是执行失败，都按 retry 次数递增等待
retry=0
while [ $retry -lt $MAX_RETRIES ]; do

    # 退避等待（第 0 次不等待；第 1 次起 2→4→8→16→32s，上限 60s）
    if [ $retry -gt 0 ]; then
        wait_s=$(( BASE_SLEEP * (1 << (retry - 1)) ))
        [ $wait_s -gt 60 ] && wait_s=60
        sleep $wait_s
    fi

    # 检查工具是否存在
    if [ ! -x "$N48R3TEST" ]; then
        echo "n48r3-logstream.sh: tool not ready ($N48R3TEST), retry $((retry+1))/$MAX_RETRIES"
        retry=$((retry + 1))
        continue
    fi

    # 执行 logstream（长驻进程，内部按 INTERVAL_MS 轮询）
    "$N48R3TEST" logstream "$LOGFILE" $INTERVAL_MS $MAX_S "$STOPFILE"
    rc=$?

    # rc=0：stopfile/max_s 正常退出 → 干净收尾
    if [ $rc -eq 0 ]; then
        exit 0
    fi

    # rc≠0：工具执行失败（kext 未就绪/版本不匹配等）→ 退避重试
    echo "n48r3-logstream.sh: tool exited rc=$rc, retry $((retry+1))/$MAX_RETRIES"
    retry=$((retry + 1))
done

# 所有重试耗尽 → 交 launchd 重启（exit 1 触发 SuccessfulExit=false）
echo "n48r3-logstream.sh: all $MAX_RETRIES retries exhausted, exiting rc=1"
exit 1
