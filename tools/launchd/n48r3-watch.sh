#!/bin/sh
# n48r3-watch.sh - persist the kext's per-step progress to disk so a freeze/power-off leaves
# the last completed step behind. Managed by launchd (com.n48r3.watch); never run via nohup.
last=""
while :; do
  line=$(/usr/sbin/ioreg -r -c NaviR3Bringup -l -w 0 2>/dev/null \
         | /usr/bin/grep -F '"N48R3EVID-progress"' | /usr/bin/head -1)
  if [ -n "$line" ]; then
    hex=$(printf '%s\n' "$line" | /usr/bin/sed -n 's/.*<\([0-9a-fA-F]*\)>.*/\1/p')
    if [ -n "$hex" ]; then
      v=$(printf '%s' "$hex" | /usr/bin/xxd -r -p)
    else
      v=$(printf '%s\n' "$line" | /usr/bin/sed -n 's/.*<"\(.*\)">.*/\1/p')
      [ -z "$v" ] && v="$line"
    fi
    if [ "$v" != "$last" ]; then
      printf '%s %s\n' "$(/bin/date +%H:%M:%S)" "$v" >> /var/tmp/n48r3-progress.log
      /bin/sync
      last="$v"
    fi
  fi
  /bin/sleep 0.2
done
