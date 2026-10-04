#!/bin/bash
# Stop RPCS3 only if it is running one of this port's ELFs (never another session's game).
for pid in $(pgrep -x rpcs3); do
    if tr '\0' ' ' < /proc/$pid/cmdline | grep -qE "/fgps3/|/FGFS00910/"; then
        kill "$pid" && echo "stopped rpcs3 $pid"
    else
        echo "leaving rpcs3 $pid alone: $(tr '\0' ' ' < /proc/$pid/cmdline)"
    fi
done
for i in $(seq 1 20); do pgrep -x rpcs3 >/dev/null || break; sleep 0.5; done
