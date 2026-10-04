#!/bin/bash
# Install FlightGear into RPCS3's virtual hard disk as a game folder:
#   dev_hdd0/game/FGFS00910/PARAM.SFO, USRDIR/EBOOT.BIN, USRDIR/fgdata, USRDIR/fgfs.args
# The base data is linked, not copied; the PS3 additions (controller bindings)
# are copied into it. Does not start or stop RPCS3 (see run_rpcs3.sh).
set -e
R="$(cd "$(dirname "$0")" && pwd)"
HDD="${RPCS3_HDD0:-$HOME/.config/rpcs3/dev_hdd0}"
GAME="$HDD/game/FGFS00910"
USRDIR="$GAME/USRDIR"
DATA="$R/fgdata_x/fgfs-base-0.9.10.orig"

mkdir -p "$USRDIR"
cp -r --preserve=timestamps "$R/port/fgdata/." "$DATA/"
ln -sfn "$DATA" "$USRDIR/fgdata"
[ -f "$USRDIR/fgfs.args" ] || cp "$R/port/fgfs.args" "$USRDIR/fgfs.args"
python3 "$HOME/ps3dev/bin/sfo.py" --title "FlightGear 0.9.10" --appid "FGFS00910" \
    -f "$HOME/ps3dev/bin/sfo.xml" "$GAME/PARAM.SFO"
cp "$R/build/fgfs.run.elf" "$USRDIR/EBOOT.BIN"
cp "$R/build/fgfs.run.elf" "$USRDIR/RELOAD.SELF"      # what "back to the hangar" starts
echo "installed to $GAME"
