#!/bin/bash
# uninstall.sh - 在 PC 上以 root 运行：卸载 com.n48r3.export 并清理它自己安装的文件。
# 默认保留导出件（/var/tmp/n48r3-export）与日志（/Library/Logs/N48R3）——它们是取证数据；
# 加 --purge 才一并删除。
#
# 用法：sudo ./uninstall.sh [--purge]
#
# 环境变量覆盖（仅测试/诊断用）：
#   N48R3_ROOT       非空时把所有默认路径挂到该前缀下（测试用；不再要求 root）
#   N48R3_PLIST      默认 $N48R3_ROOT/Library/LaunchDaemons/com.n48r3.export.plist
#   N48R3_SCRIPT     默认 $N48R3_ROOT/usr/local/bin/n48r3-export.sh
#   N48R3_LOGDIR     默认 $N48R3_ROOT/Library/Logs/N48R3
#   N48R3_OUTDIR     默认 $N48R3_ROOT/var/tmp/n48r3-export
#   N48R3_MARKERDIR  默认 $N48R3_ROOT/var/tmp
#   N48R3_LABEL      默认 com.n48r3.export
#   N48R3_LAUNCHCTL  默认 launchctl（测试注入 fake）
set -u

PURGE=0
for a in "$@"; do
    case "$a" in
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "uninstall.sh: unknown argument '$a' (try --help)" >&2; exit 2 ;;
    esac
done

ROOT=${N48R3_ROOT:-}
PLIST=${N48R3_PLIST:-$ROOT/Library/LaunchDaemons/com.n48r3.export.plist}
SCRIPT=${N48R3_SCRIPT:-$ROOT/usr/local/bin/n48r3-export.sh}
LOGDIR=${N48R3_LOGDIR:-$ROOT/Library/Logs/N48R3}
OUTDIR=${N48R3_OUTDIR:-$ROOT/var/tmp/n48r3-export}
MARKERDIR=${N48R3_MARKERDIR:-$ROOT/var/tmp}
LABEL=${N48R3_LABEL:-com.n48r3.export}
LAUNCHCTL=${N48R3_LAUNCHCTL:-launchctl}

if [ -z "$ROOT" ] && [ "$(id -u)" != 0 ]; then
    echo "uninstall.sh: run as root (sudo)" >&2
    exit 2
fi

# —— 卸载 daemon（launchd system 域；未加载也不算失败）
if "$LAUNCHCTL" bootout "system/$LABEL" 2>/dev/null; then
    echo "booted out system/$LABEL"
else
    echo "(daemon not loaded)"
fi

# —— 删除本工具安装的文件
for f in "$PLIST" "$SCRIPT"; do
    if [ -e "$f" ]; then rm -f -- "$f" && echo "removed $f"; else echo "not present: $f"; fi
done
# 本 boot 幂等标记也一并清掉，便于重装后重新导出
for m in "$MARKERDIR"/n48r3-export-done-*; do
    [ -e "$m" ] || continue
    rm -f -- "$m" && echo "removed $m"
done

# —— 导出件 / 日志：默认保留，--purge 才删
if [ "$PURGE" = 1 ]; then
    case "$OUTDIR" in
        ""|"/"|"/var"|"/var/tmp"|"/Library") echo "REFUSING to purge unsafe path '$OUTDIR'" >&2 ;;
        *) if [ -e "$OUTDIR" ]; then rm -rf -- "$OUTDIR" && echo "purged $OUTDIR"; fi ;;
    esac
    case "$LOGDIR" in
        ""|"/"|"/Library"|"/Library/Logs") echo "REFUSING to purge unsafe path '$LOGDIR'" >&2 ;;
        *) if [ -e "$LOGDIR" ]; then rm -rf -- "$LOGDIR" && echo "purged $LOGDIR"; fi ;;
    esac
else
    echo "kept export artifacts: $OUTDIR"
    echo "kept logs:             $LOGDIR"
    echo "(re-run with --purge to delete them)"
fi
exit 0
