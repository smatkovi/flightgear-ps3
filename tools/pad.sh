#!/bin/bash
# Send pad input to the RPCS3 game window via the keyboard pad handler.
# Usage: pad.sh <key> [hold-ms] [key2 ...]      e.g. pad.sh X 120   or   pad.sh Left Left X
# Keys are X keysyms (X, Return, Left, Down, F2, KP_2, ...). Default hold 120 ms.
export DISPLAY="${DISPLAY:-:0}"
WID=""
for w in $(xdotool search --class rpcs3 2>/dev/null); do
    n=$(xdotool getwindowname "$w" 2>/dev/null || true)
    case "$n" in FPS:*) WID="$w" ;; esac
done
[ -n "$WID" ] || { echo "no RPCS3 game window" >&2; exit 1; }
HOLD=300
xdotool windowactivate --sync "$WID" 2>/dev/null
sleep 0.5
[ "$(xdotool getwindowfocus 2>/dev/null)" = "$WID" ] || { xdotool windowfocus --sync "$WID"; sleep 0.3; }
for k in "$@"; do
    case "$k" in
        [0-9]*) HOLD="$k"; continue ;;
    esac
    xdotool keydown "$k"
    sleep "$(awk "BEGIN{print $HOLD/1000}")"
    xdotool keyup "$k"
    sleep 0.35
done
