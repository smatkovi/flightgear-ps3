#!/bin/bash
# Start one of this port's ELFs in RPCS3 (no GUI, game window only).
# Never stops an RPCS3 that is already running - other sessions use it too.
# Usage: run_rpcs3.sh build/gltest.run.elf
#        run_rpcs3.sh fgfs     (the installed game, see install_rpcs3.sh; RPCS3
#                               then applies custom_configs/config_FGFS00910.yml)
set -e
R="$(cd "$(dirname "$0")" && pwd)"
ELF="${1:?usage: run_rpcs3.sh <elf>|fgfs}"
if [ "$ELF" = fgfs ]; then
    ELF="$HOME/.config/rpcs3/dev_hdd0/game/FGFS00910/USRDIR/EBOOT.BIN"
else
    ELF="$(readlink -f "$ELF")"
fi
export DISPLAY="${DISPLAY:-:0}"
if pgrep -x rpcs3 >/dev/null; then
    echo "RPCS3 is already running, not starting:" >&2
    pgrep -a rpcs3 >&2
    exit 1
fi
mkdir -p "$R/logs"
sudo -n prlimit --pid $$ --memlock=2147483648:2147483648 2>/dev/null || echo "warning: could not raise memlock" >&2
: > "$HOME/.cache/rpcs3/RPCS3.log"
: > "$HOME/.cache/rpcs3/TTY.log" 2>/dev/null || true
U="$HOME/.config/rpcs3/dev_hdd0/game/FGFS00910/USRDIR"
rm -f "$U/ps3gl.log"
[ ! -f "$U/fgfs.log" ] || mv "$U/fgfs.log" "$U/fgfs.prev.log"   # as the game does; the hangar reads it
cd "$R"
nohup rpcs3 --no-gui "$ELF" > "$R/logs/rpcs3_stdout.log" 2>&1 &
echo "rpcs3 pid $!"
