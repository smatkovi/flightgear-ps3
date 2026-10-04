#!/bin/bash
# Fetch FlightGear's log files from a real PS3 (webMAN FTP) into logs/ps3/.
# Usage: PS3=192.168.1.6 ./fetch_logs.sh
R="$(cd "$(dirname "$0")" && pwd)"
: "${PS3:?set PS3=<console ip>}"
mkdir -p "$R/logs/ps3"
for f in fgfs.log ps3gl.log fgfs.args; do
    curl -s --max-time 60 -o "$R/logs/ps3/$f" "ftp://$PS3/dev_hdd0/game/FGFS00910/USRDIR/$f" \
        && echo "logs/ps3/$f" || echo "$f: not there"
done
