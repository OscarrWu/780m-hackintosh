#!/bin/bash
# export.sh - com.n48r3.export 的执行体（阶段 1 导出通道，D7 / E1-E3 / V5 / V11）。
# 在 PC 上由 LaunchDaemon（RunAtLoad）以 root 运行；人工重跑：sudo /usr/local/bin/n48r3-export.sh。
#
# 数据流（沿用阶段 0 com.n48r3.probe-export 的模式，D7）：
#   kext 节点（IOClass=NaviR3Bringup）出现 → 轮询 "n48r3-Done"（5s x 36 = 180s 上限）
#   → 一旦就绪（或超时）→ 按序拼装 N48R3LOG-0001..N 分块 → 落盘 /var/tmp/n48r3-export/<UTC 时间戳>/
#     ├─ n48r3-log.txt        主通道：按块序号拼装后的全文（验收：grep N48R3LOG <此文件>）
#     ├─ registry.txt         节点全属性 dump（人读取证；分块原始值也在其中）
#     └─ evidence/<KEY>       取证件（若存在：属性名以 N48R3EVID- 开头，原样落盘）
#   写完后置本 boot 的 done 标记，防止同一 boot 重复导出（幂等）。
#
# 健壮性（任务卡要求）：
#   * 节点在 60s 宽限内不出现 → 跳过并记录（kext 未加载，不是失败）。
#   * 分块属性缺失 / 半截块 / 块序号断号 → 不崩溃：拼装已存在的块，断号如实写入 export.log。
#   * Done 超时 → 仍然导出（部分数据也是证据），并标注 INCOMPLETE。
#   * 分块值兼容 OSData（ioreg 显示 <hex>，含 ioreg 折行）与 OSString（"..."）两种形态。
#
# 环境变量覆盖（仅测试/诊断用；默认值即部署值）：
#   N48R3_IOCLASS     默认 NaviR3Bringup
#   N48R3_IOREG       默认 ioreg
#   N48R3_SYSCTL      默认 sysctl
#   N48R3_OUTDIR      默认 /var/tmp/n48r3-export
#   N48R3_LOGDIR      默认 /Library/Logs/N48R3
#   N48R3_MARKERDIR   默认 /var/tmp
#   N48R3_POLL_S      默认 5
#   N48R3_NODE_GRACE  默认 12   （12 x 5s = 60s）
#   N48R3_DONE_POLL   默认 36   （36 x 5s = 180s）
#   N48R3_GAP_LIMIT   默认 64   （断号后最多连续扫描多少块就收尾）
#   N48R3_EVID_PREFIX 默认 N48R3EVID- （取证件属性名前缀；最终命名由 kext/T2a 定，可覆盖）
#   N48R3_FORCE=1     忽略本 boot 的 done 标记，强制重新导出
set -u

IOCLASS=${N48R3_IOCLASS:-NaviR3Bringup}
IOREG=${N48R3_IOREG:-ioreg}
SYSCTL=${N48R3_SYSCTL:-sysctl}
OUTBASE=${N48R3_OUTDIR:-/var/tmp/n48r3-export}
LOGDIR=${N48R3_LOGDIR:-/Library/Logs/N48R3}
MARKERDIR=${N48R3_MARKERDIR:-/var/tmp}
POLL_INTERVAL=${N48R3_POLL_S:-5}
NODE_GRACE_MAX=${N48R3_NODE_GRACE:-12}
DONE_POLL_MAX=${N48R3_DONE_POLL:-36}
GAP_SCAN_LIMIT=${N48R3_GAP_LIMIT:-64}
EVID_PREFIX=${N48R3_EVID_PREFIX:-N48R3EVID-}
CHUNK_PREFIX="N48R3LOG"
DONE_KEY="n48r3-Done"

umask 022
mkdir -p "$LOGDIR" 2>/dev/null || { echo "export.sh: cannot create $LOGDIR" >&2; exit 1; }
LOG="$LOGDIR/export.log"
exec >>"$LOG" 2>&1

say() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }
say "export.sh: start (pid $$, class=$IOCLASS)"

# ------------------------------------------------------------------ 本 boot 的 done 标记
BOOT_ID=$("$SYSCTL" kern.boottime 2>/dev/null \
    | sed -n 's/.*[^A-Za-z]sec[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$BOOT_ID" ] || BOOT_ID=0
MARKER="$MARKERDIR/n48r3-export-done-$BOOT_ID"
if [ "${N48R3_FORCE:-0}" != "1" ] && [ -e "$MARKER" ]; then
    say "already exported this boot (marker $MARKER) - skip (idempotent)"
    exit 0
fi

# ------------------------------------------------------------------ 等节点出现（60s 宽限）
node_seen=""
i=1
while [ "$i" -le "$NODE_GRACE_MAX" ]; do
    if "$IOREG" -w0 -c "$IOCLASS" 2>/dev/null | grep -q "$IOCLASS"; then
        node_seen="yes"
        say "node $IOCLASS found after $(( (i - 1) * POLL_INTERVAL ))s"
        break
    fi
    [ "$i" -lt "$NODE_GRACE_MAX" ] && sleep "$POLL_INTERVAL"
    i=$((i + 1))
done
if [ -z "$node_seen" ]; then
    say "node $IOCLASS not found within $(( NODE_GRACE_MAX * POLL_INTERVAL ))s - skip (kext not loaded)"
    exit 0
fi

# ------------------------------------------------------------------ 轮询完成标志（180s 上限）
complete="no"
i=1
while [ "$i" -le "$DONE_POLL_MAX" ]; do
    if "$IOREG" -w0 -c "$IOCLASS" 2>/dev/null | grep -q "\"$DONE_KEY\""; then
        complete="yes"
        say "\"$DONE_KEY\" observed after $(( (i - 1) * POLL_INTERVAL ))s"
        break
    fi
    [ "$i" -lt "$DONE_POLL_MAX" ] && sleep "$POLL_INTERVAL"
    i=$((i + 1))
done
if [ "$complete" = "no" ]; then
    say "TIMEOUT after $(( DONE_POLL_MAX * POLL_INTERVAL ))s - exporting anyway (INCOMPLETE; partial data is evidence)"
fi

# ------------------------------------------------------------------ 取一次节点全量 dump
REGDUMP=$("$IOREG" -w0 -l -c "$IOCLASS" 2>/dev/null)
if [ -z "$REGDUMP" ]; then
    say "node $IOCLASS vanished before export - skip"
    exit 0
fi

# ------------------------------------------------------------------ 落盘目录（UTC 时间戳）
TS=$(date -u '+%Y%m%dT%H%M%SZ')
OUTDIR="$OUTBASE/$TS"
n=1
while [ -e "$OUTDIR" ]; do OUTDIR="$OUTBASE/$TS-$n"; n=$((n + 1)); done
mkdir -p "$OUTDIR" || { say "cannot create $OUTDIR - abort"; exit 1; }
mkdir -p "$OUTDIR/evidence" 2>/dev/null
chmod 755 "$OUTBASE" "$OUTDIR" 2>/dev/null
LOGFILE="$OUTDIR/n48r3-log.txt"
META="$OUTDIR/.export-meta"
: > "$LOGFILE"; : > "$META"

# 单次 awk 扫描 REGDUMP：按 N48R3LOG-NNNN 块序号拼装 → n48r3-log.txt；
# 断号写入 META（GAP <idx>）；取证件原始值写入 evidence/<KEY>.raw（D=hex/S=文本）。
# 说明：日志块是文本（无 NUL），awk 解码安全；取证件可能是二进制（EDID 含 NUL），
#       故取证件保留原始 hex 交给下述 perl/xxd 解码（NUL 安全）。
say "assembling $CHUNK_PREFIX chunks in $OUTDIR"
printf '%s\n' "$REGDUMP" | awk \
    -v prefix="$CHUNK_PREFIX" -v evprefix="$EVID_PREFIX" \
    -v logfile="$LOGFILE" -v metafile="$META" -v evdir="$OUTDIR/evidence" \
    -v gaplimit="$GAP_SCAN_LIMIT" '
function hx(c,   v, p) {
    v = tolower(c)
    p = index("0123456789abcdef", v)
    return (p > 0) ? p - 1 : 0
}
function dechex(h,   i, out) {
    gsub(/[^0-9A-Fa-f]/, "", h)
    if (length(h) % 2 == 1) h = "0" h
    out = ""
    for (i = 1; i + 1 <= length(h); i += 2)
        out = out sprintf("%c", hx(substr(h, i, 1)) * 16 + hx(substr(h, i + 1, 1)))
    return out
}
function handle(k, typ, raw,   t, j, safe, ft, fr) {
    if (k ~ ("^" prefix "-[0-9]+$")) {
        t = k; sub("^" prefix "-", "", t); j = t + 0
        chunk[j] = (typ == "D") ? dechex(raw) : raw
        present[j] = 1
    } else if (evprefix != "" && k ~ ("^" evprefix)) {
        safe = k; gsub(/[^A-Za-z0-9._-]/, "_", safe)
        ft = evdir "/" safe ".type"
        fr = evdir "/" safe ".raw"
        printf "%s", typ > ft
        printf "%s", raw > fr
        close(ft); close(fr)
    }
}
BEGIN { inblk = 0 }
{
    line = $0
    if (inblk) {
        if (line ~ /^[ \t]*"[^"]*"[ \t]*=[ \t]*/) {
            handle(pkey, "D", buf); inblk = 0      # 半截块：下一条属性开始即收尾
        } else {
            buf = buf line
            if (buf ~ />/) {
                b = buf; sub(/^</, "", b); sub(/>.*$/, "", b)
                handle(pkey, "D", b); inblk = 0
            }
            next
        }
    }
    if (match(line, /^[ \t]*"[^"]*"[ \t]*=[ \t]*/) == 0) next
    k = line
    sub(/^[ \t]*"/, "", k); sub(/"[ \t]*=.*$/, "", k)
    rest = substr(line, RSTART + RLENGTH)
    if (rest ~ /^</) {
        if (rest ~ />/) {
            b = rest; sub(/^</, "", b); sub(/>.*$/, "", b)
            handle(k, "D", b)
        } else { pkey = k; buf = rest; inblk = 1 }
    } else if (rest ~ /^"/) {
        s = rest; sub(/^"/, "", s); sub(/"$/, "", s)
        gsub(/\\"/, "\"", s)
        handle(k, "S", s)
    } else {
        handle(k, "R", rest)
    }
}
END {
    if (inblk) handle(pkey, "D", buf)               # 文件尾仍有半截块：尽力保留
    np = 0; mini = 0; maxi = 0
    for (j in present) if (present[j]) {
        if (np == 0 || j < mini) mini = j
        if (np == 0 || j > maxi) maxi = j
        np++
    }
    n = 0; miss = 0; streak = 0
    if (np > 0) {
        out = ""
        for (j = mini; j <= maxi; j++) {
            if (j in chunk) { out = out chunk[j]; n++; streak = 0 }
            else {
                miss++; streak++
                print "GAP " j > metafile
                if (streak >= gaplimit) break
            }
        }
        printf "%s", out > logfile
    }
    print "STATS " n " " miss " " mini " " maxi > metafile
    close(logfile); close(metafile)
}
'
awk_rc=$?

# ------------------------------------------------------------------ 读回拼接统计 / 断号日志
chunks=0; missing=0; mini=0; maxi=0; gaps=""
while read -r tag a b c d; do
    case "$tag" in
        GAP)   gaps="$gaps $a"; say "MISSING chunk $(printf '%s-%04u' "$CHUNK_PREFIX" "$a") (gap) - continuing" ;;
        STATS) chunks=$a; missing=$b; mini=$c; maxi=$d ;;
    esac
done < "$META"
rm -f "$META"
if [ "$awk_rc" != 0 ]; then
    say "assembler failed (awk exit=$awk_rc) - abort without done marker (partial dir: $OUTDIR)"
    exit 1
fi
say "assembled $chunks chunks ($missing gap indices, index $mini..$maxi, $(wc -c < "$LOGFILE" | tr -d ' ') bytes)"
chmod 644 "$LOGFILE" 2>/dev/null

# ------------------------------------------------------------------ 节点全量属性（人读取证）
printf '%s\n' "$REGDUMP" > "$OUTDIR/registry.txt"
chmod 644 "$OUTDIR/registry.txt" 2>/dev/null
say "wrote registry.txt ($(wc -c < "$OUTDIR/registry.txt" | tr -d ' ') bytes)"

# ------------------------------------------------------------------ 取证件（若存在）：解码原始值
decode_hex_file() {  # <raw-hex-file> → stdout（NUL 安全）
    if command -v perl >/dev/null 2>&1; then
        perl -e 'local $/; $_=<>; s/\s//g; print pack("H*",$_)' < "$1"
    elif command -v xxd >/dev/null 2>&1; then
        xxd -r -p "$1"
    else
        local h i b
        h=$(tr -d ' \t\r\n' < "$1"); i=0
        while [ "$i" -lt "${#h}" ]; do
            b=${h:$i:2}
            printf "\\$(printf '%03o' $((16#$b)))"
            i=$((i + 2))
        done
    fi
}
evid_count=0
if [ -d "$OUTDIR/evidence" ]; then
    for tf in "$OUTDIR/evidence"/*.type; do
        [ -f "$tf" ] || continue
        base=${tf%.type}
        typ=$(cat "$tf" 2>/dev/null)
        case "$typ" in
            D) decode_hex_file "$base.raw" > "$base" ;;
            *) cat "$base.raw" > "$base" ;;
        esac
        rm -f "$base.raw" "$tf"
        chmod 644 "$base" 2>/dev/null
        evid_count=$((evid_count + 1))
    done
    rmdir "$OUTDIR/evidence" 2>/dev/null   # 只有在没有取证件时才会成功
fi
say "evidence artifacts exported: $evid_count"

# ------------------------------------------------------------------ 导出说明（随导出件一起拉回，含断号/超时如实记录）
NOTES="$OUTDIR/n48r3-export-notes.txt"
{
    printf 'generated_utc=%s\n' "$TS"
    printf 'io_class=%s\n' "$IOCLASS"
    printf 'boot_id=%s\n' "$BOOT_ID"
    printf 'complete=%s\n' "$complete"
    printf 'chunks=%s\n' "$chunks"
    printf 'gap_count=%s\n' "$missing"
    printf 'gap_indices=%s\n' "${gaps# }"
    printf 'min_index=%s\n' "$mini"
    printf 'max_index=%s\n' "$maxi"
    printf 'evidence=%s\n' "$evid_count"
} > "$NOTES"
chmod 644 "$NOTES" 2>/dev/null

# ------------------------------------------------------------------ 本 boot 幂等标记 + 收尾
: > "$MARKER"; chmod 644 "$MARKER" 2>/dev/null
say "export done (complete=$complete, dir=$OUTDIR) - marker $MARKER written"
exit 0
