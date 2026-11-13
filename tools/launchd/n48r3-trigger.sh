#!/bin/sh
# n48r3-trigger.sh - one-shot power-up trigger, managed by launchd (com.n48r3.trigger).
# The kernel call is long-running and may freeze the machine; do not run it under nohup or a
# foreground SSH session. The exit code is only a quick read; the evidence is in IORegistry.
/var/tmp/n48r3test trigger
echo "trigger rc=$?"
