#!/bin/bash
# Grab a screenshot of the RPCS3 game window (or the whole root window as fallback).
# Usage: shot.sh <output.png>
set -e
OUT="${1:?usage: shot.sh <out.png>}"
export DISPLAY="${DISPLAY:-:0}"
WID=""
for w in $(xdotool search --class rpcs3 2>/dev/null); do
    n=$(xdotool getwindowname "$w" 2>/dev/null || true)
    case "$n" in FPS:*) WID="$w" ;; esac
done
# import grabs the X server while it captures; if its target window disappears (emulator
# crash) it hangs and the whole desktop freezes - so every capture runs under a timeout.
if [ -n "$WID" ]; then
    xdotool windowraise "$WID" 2>/dev/null; sleep 0.3
    timeout 10 import -window "$WID" "$OUT" 2>/dev/null || timeout 10 import -window root "$OUT"
else
    timeout 10 import -window root "$OUT"
fi
echo "$OUT ${WID:-root}"
