#!/bin/bash
# Try aircraft in RPCS3: for each name, start FlightGear with --aircraft=<name>,
# wait until it has drawn 600 frames in the main loop (or crashed/exited/timed out),
# then record the result, the errors from fgfs.log and a screenshot in logs/actest/.
# Usage: tools/test_aircraft.sh <set-name>...   (e.g. long-ez A320 me262)
R="$(cd "$(dirname "$0")/.." && pwd)"
U="$HOME/.config/rpcs3/dev_hdd0/game/FGFS00910/USRDIR"
O="$R/logs/actest"; mkdir -p "$O"
cp "$U/fgfs.args" "$O/fgfs.args.saved"
for a in "$@"; do
    { sed "s/^--aircraft=.*/--aircraft=$a/" "$R/port/fgfs.args"; echo --no-hangar; } > "$U/fgfs.args"
    "$R/run_rpcs3.sh" fgfs > /dev/null || { echo "$a: RPCS3 busy"; break; }
    res=TIMEOUT
    for i in $(seq 1 60); do
        sleep 4
        if grep -qE "^frame 600:" "$U/ps3gl.log" 2>/dev/null; then res=OK; break; fi
        if grep -qE "^·F " "$HOME/.cache/rpcs3/RPCS3.log" 2>/dev/null; then res=CRASH; break; fi
        pgrep -x rpcs3 > /dev/null || { res=EXIT; break; }
        grep -q "_sys_process_exit" "$HOME/.cache/rpcs3/RPCS3.log" 2>/dev/null && { res=EXIT; break; }
    done
    [ $res = OK ] && sleep 8
    "$R/tools/shot.sh" "$O/$a.png" > /dev/null 2>&1
    cp "$U/fgfs.log" "$O/$a.log" 2>/dev/null
    grep -a "^·F " "$HOME/.cache/rpcs3/RPCS3.log" > "$O/$a.fatal" 2>/dev/null || rm -f "$O/$a.fatal"
    heap=$(grep "^frame" "$U/ps3gl.log" 2>/dev/null | tail -1 | grep -oE "heap [0-9]+K|tex [0-9]+K" | tr '\n' ' ')
    echo "$a: $res $heap"
    grep -aiE "error|failed|exception|nasal (parse|runtime)|cannot|unknown|not found" "$O/$a.log" 2>/dev/null \
        | grep -viE "wav file|no audio|sound|Failed to create data socket|unknown.rgb" | sort | uniq -c | sort -rn | head -8 | sed 's/^/    /'
    "$R/stop_rpcs3.sh" > /dev/null
done
cp "$O/fgfs.args.saved" "$U/fgfs.args"
