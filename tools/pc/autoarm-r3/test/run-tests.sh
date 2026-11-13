#!/bin/bash
# run-tests.sh - autoarm-r3 本地 fake 测试（可在 Linux 宿主直接跑；ioreg/sysctl/launchctl 全部走 PATH stub）。
# 覆盖：拼块正确性（OSData hex + ioreg 折行 / OSString）、断号容错、半截块、Done 超时、
#       节点缺失跳过、同 boot 幂等 + 跨 boot 重复导出、取证件（含 NUL）、uninstall、plist 结构、bash -n。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FB="$HERE/fakebin"
W=$(mktemp -d "${TMPDIR:-/tmp}/n48r3-test.XXXXXX") || { echo "mktemp failed"; exit 1; }
trap 'rm -rf "$W"' EXIT INT TERM
PASS=0; FAIL=0; SCN=""
ok()  { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); echo "FAIL [$SCN] $*"; }
eq()  { [ "$2" = "$3" ] && ok || bad "$1: got [$2] want [$3]"; }

enc() { printf '%s' "$1" | od -A n -v -t x1 | tr -d ' \n'; }

setup() { # setup <name>
    SCN=$1; SD="$W/$1"; rm -rf "$SD"
    mkdir -p "$SD/out" "$SD/log" "$SD/mark"
    FAKE_REG="$SD/registry.txt"; : > "$FAKE_REG"
    : > "$SD/cmds.log"
    export FAKE_DIR="$SD" FAKE_REGISTRY="$FAKE_REG" FAKE_BOOTSEC=1791470000
    export N48R3_IOCLASS=NaviR3Bringup
    export N48R3_IOREG="$FB/ioreg" N48R3_SYSCTL="$FB/sysctl"
    export N48R3_OUTDIR="$SD/out" N48R3_LOGDIR="$SD/log" N48R3_MARKERDIR="$SD/mark"
    export N48R3_POLL_S=0 N48R3_NODE_GRACE=2 N48R3_DONE_POLL=2 N48R3_GAP_LIMIT=4
    unset N48R3_FORCE
}
reghead() { printf '%s\n' '+-o NaviR3Bringup  <class NaviR3Bringup, id 0x100000abc, registered, matched, active, busy 0 (0 ms), retain 8>' '  {' >> "$FAKE_REG"; }
regfoot() { printf '%s\n' '  }' >> "$FAKE_REG"; }
chunkD()  { printf '    "N48R3LOG-%04d" = <%s>\n' "$1" "$(enc "$2")" >> "$FAKE_REG"; }
chunkS()  { printf '    "N48R3LOG-%04d" = "%s"\n' "$1" "$2" >> "$FAKE_REG"; }
doneflag(){ printf '    "n48r3-Done" = Yes\n' >> "$FAKE_REG"; }
run()     { env "$@" /bin/bash "$SRC/export.sh" > "$SD/stdout" 2>&1; RC=$?; }
ndirs()   { ls -1 "$SD/out" 2>/dev/null | grep -c .; }
oneout()  { ls -1 "$SD/out" 2>/dev/null | head -1; }
logline() { grep -c -- "$1" "$SD/log/export.log" 2>/dev/null; }
logtext() { cat "$SD/log/export.log" 2>/dev/null; }

# ---------------------------------------------------------------- 1. happy：OSData hex + 折行 + 取证件
setup happy
reghead
t1=$'N48R3LOG stage0: status=ok\nN48R3LOG pci: 15bf\n'
t2=$'N48R3LOG bar: base=0x8d0000000\n'
t3=$'N48R3LOG wrapped: yes\n'
chunkD 1 "$t1"; chunkD 2 "$t2"
h3=$(enc "$t3"); printf '    "N48R3LOG-0003" = <%s\n%s>\n' "${h3:0:4}" "${h3:4}" >> "$FAKE_REG"
printf '    "N48R3EVID-edid" = <00ff10ab>\n' >> "$FAKE_REG"
doneflag; regfoot
run
eq "exit" "$RC" 0
eq "one export dir" "$(ndirs)" 1
D=$(oneout)
{ printf '%s' "$t1"; printf '%s' "$t2"; printf '%s' "$t3"; } > "$SD/expected"
cmp -s "$SD/expected" "$SD/out/$D/n48r3-log.txt" && ok || bad "assembled log mismatch"
eq "evidence NUL-safe" "$(od -A n -v -t x1 < "$SD/out/$D/evidence/N48R3EVID-edid" | tr -d ' \n')" "00ff10ab"
eq "registry present" "$([ -s "$SD/out/$D/registry.txt" ] && echo y)" y
eq "registry keeps raw chunk keys" "$(grep -c '"N48R3LOG-0001"' "$SD/out/$D/registry.txt")" 1
eq "marker" "$([ -f "$SD/mark/n48r3-export-done-1791470000" ] && echo y)" y
eq "complete=yes logged" "$(logline 'complete=yes')" 1
eq "no gap" "$(logline 'MISSING')" 0
eq "notes complete=yes" "$(grep -c '^complete=yes$' "$SD/out/$D/n48r3-export-notes.txt")" 1
eq "notes gap_count=0" "$(grep -c '^gap_count=0$' "$SD/out/$D/n48r3-export-notes.txt")" 1
eq "notes evidence=1" "$(grep -c '^evidence=1$' "$SD/out/$D/n48r3-export-notes.txt")" 1

# ---------------------------------------------------------------- 2. OSString 分块
setup str
reghead; chunkS 1 "N48R3LOG str1"; chunkS 2 " + str2"; doneflag; regfoot
run
eq "exit" "$RC" 0
eq "string assembly" "$(cat "$SD/out/$(oneout)/n48r3-log.txt")" "N48R3LOG str1 + str2"
eq "no evidence dir" "$([ -d "$SD/out/$(oneout)/evidence" ] && echo present || echo gone)" gone

# ---------------------------------------------------------------- 2b. 0 基序号（LogChannel 允许 index 0）
setup zero
reghead; chunkD 0 "A"; chunkD 1 "B"; doneflag; regfoot
run
eq "exit" "$RC" 0
eq "0-based assembly" "$(cat "$SD/out/$(oneout)/n48r3-log.txt")" "AB"
eq "notes min_index=0" "$(grep -c '^min_index=0$' "$SD/out/$(oneout)/n48r3-export-notes.txt")" 1
eq "notes max_index=1" "$(grep -c '^max_index=1$' "$SD/out/$(oneout)/n48r3-export-notes.txt")" 1

# ---------------------------------------------------------------- 3. 断号：缺 0003
setup gap
reghead; chunkD 1 "one"; chunkD 2 "two"; chunkD 4 "four"; doneflag; regfoot
run
eq "exit" "$RC" 0
eq "available chunks assembled" "$(cat "$SD/out/$(oneout)/n48r3-log.txt")" "onetwofour"
eq "gap recorded" "$(logline 'MISSING chunk N48R3LOG-0003')" 1
eq "notes gap_indices=3" "$(grep -c '^gap_indices=3$' "$SD/out/$(oneout)/n48r3-export-notes.txt")" 1

# ---------------------------------------------------------------- 4. 半截块：无闭合 >
setup trunc
reghead; chunkD 1 "good-"
printf '    "N48R3LOG-0002" = <414243\n' >> "$FAKE_REG"
doneflag; regfoot
run
eq "exit" "$RC" 0
eq "truncated block kept" "$(cat "$SD/out/$(oneout)/n48r3-log.txt")" "good-ABC"

# ---------------------------------------------------------------- 5. Done 超时：仍导出（INCOMPLETE）
setup timeout
reghead; chunkD 1 $'partial:\n'; chunkD 2 $'more\n'; regfoot
run
eq "exit" "$RC" 0
eq "exported despite timeout" "$(ndirs)" 1
eq "partial log" "$(cat "$SD/out/$(oneout)/n48r3-log.txt")" "$(printf 'partial:\nmore\n')"
eq "timeout logged" "$(logline 'TIMEOUT')" 1
eq "complete=no logged" "$(logline 'complete=no')" 1
eq "marker written" "$([ -f "$SD/mark/n48r3-export-done-1791470000" ] && echo y)" y
eq "notes complete=no" "$(grep -c '^complete=no$' "$SD/out/$(oneout)/n48r3-export-notes.txt")" 1

# ---------------------------------------------------------------- 6. 节点缺失：跳过
setup absent
run
eq "exit" "$RC" 0
eq "no export dir" "$(ndirs)" 0
eq "skip logged" "$(logline 'not found')" 1

# ---------------------------------------------------------------- 7. 幂等：同 boot 跳过；跨 boot 重新导出
setup idem
reghead; chunkD 1 "x"; doneflag; regfoot
run FAKE_BOOTSEC=100
eq "exit" "$RC" 0
eq "first boot export" "$(ndirs)" 1
sleep 1
run FAKE_BOOTSEC=100
eq "exit" "$RC" 0
eq "same boot skipped" "$(ndirs)" 1
eq "idempotent logged" "$(logline 'already exported')" 1
run N48R3_FORCE=1
eq "exit" "$RC" 0
eq "force re-export" "$(ndirs)" 2
run FAKE_BOOTSEC=200
eq "exit" "$RC" 0
eq "new boot re-exports" "$(ndirs)" 3

# ---------------------------------------------------------------- 8. uninstall（默认保留导出件；--purge 删除）
SCN=uninstall
unset N48R3_OUTDIR N48R3_LOGDIR N48R3_MARKERDIR N48R3_IOCLASS
UR="$W/uninst"
mkdir -p "$UR/Library/LaunchDaemons" "$UR/usr/local/bin" "$UR/var/tmp/n48r3-export/x" "$UR/Library/Logs/N48R3"
: > "$UR/Library/LaunchDaemons/com.n48r3.export.plist"
: > "$UR/usr/local/bin/n48r3-export.sh"
: > "$UR/var/tmp/n48r3-export-done-100"
: > "$UR/Library/Logs/N48R3/export.log"
: > "$UR/cmds.log"
N48R3_ROOT="$UR" N48R3_LAUNCHCTL="$FB/launchctl" FAKE_DIR="$UR" \
    /bin/bash "$SRC/uninstall.sh" > "$UR/out" 2>&1; urc=$?
eq "uninstall exit" "$urc" 0
eq "plist removed" "$([ -e "$UR/Library/LaunchDaemons/com.n48r3.export.plist" ] && echo present || echo gone)" gone
eq "script removed" "$([ -e "$UR/usr/local/bin/n48r3-export.sh" ] && echo present || echo gone)" gone
eq "marker removed" "$([ -e "$UR/var/tmp/n48r3-export-done-100" ] && echo present || echo gone)" gone
eq "exports kept" "$([ -d "$UR/var/tmp/n48r3-export/x" ] && echo y)" y
eq "launchctl bootout" "$(grep -c 'bootout system/com.n48r3.export' "$UR/cmds.log")" 1
N48R3_ROOT="$UR" N48R3_LAUNCHCTL="$FB/launchctl" FAKE_DIR="$UR" \
    /bin/bash "$SRC/uninstall.sh" --purge > "$UR/out2" 2>&1; prc=$?
eq "purge exit" "$prc" 0
eq "exports purged" "$([ -e "$UR/var/tmp/n48r3-export" ] && echo present || echo gone)" gone
eq "logs purged" "$([ -e "$UR/Library/Logs/N48R3" ] && echo present || echo gone)" gone
/bin/bash "$SRC/uninstall.sh" --help > "$UR/help" 2>&1; hrc=$?
eq "help exit" "$hrc" 0
eq "help lists env overrides" "$(grep -c 'N48R3_LAUNCHCTL' "$UR/help")" 1

# ---------------------------------------------------------------- 8b. awk 失败：不写完成标记，可重试
setup awkfail
reghead; chunkD 1 "x"; doneflag; regfoot
mkdir -p "$SD/bin"
printf '#!/bin/bash\nexit 2\n' > "$SD/bin/awk"; chmod +x "$SD/bin/awk"
run PATH="$SD/bin:$PATH"
eq "awk failure exit" "$RC" 1
eq "no done marker" "$([ -e "$SD/mark/n48r3-export-done-1791470000" ] && echo y || echo n)" n
eq "awk failure logged" "$(logline 'assembler failed')" 1

# ---------------------------------------------------------------- 9. plist 结构
SCN=plist
if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import plistlib,sys
d=plistlib.load(open(sys.argv[1],"rb"))
assert d["Label"]=="com.n48r3.export", d
assert d["RunAtLoad"] is True, d
assert d["ProgramArguments"]==["/bin/bash","/usr/local/bin/n48r3-export.sh"], d
assert d["StandardOutPath"]=="/Library/Logs/N48R3/export.log", d
assert d["StandardErrorPath"]=="/Library/Logs/N48R3/export.log", d' "$SRC/com.n48r3.export.plist" \
        && ok || bad "plist content"
else
    echo "  (plist parse skipped: no python3)"
fi

# ---------------------------------------------------------------- 10. bash 语法
SCN=syntax
for f in "$SRC/export.sh" "$SRC/uninstall.sh" "$FB/ioreg" "$FB/sysctl" "$FB/launchctl" "$HERE/run-tests.sh"; do
    /bin/bash -n "$f" && ok || bad "bash -n $f"
done

echo "== $PASS checks passed, $FAIL failed"
[ "$FAIL" = 0 ]
