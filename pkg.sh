#!/bin/bash
# Build an installable package for a PS3 with CFW or HEN.
#   ./pkg.sh          fgfs-ps3.pkg: EBOOT.BIN (NPDRM-signed), PARAM.SFO, ICON0.PNG,
#                     USRDIR/fgfs.args - the data is uploaded separately (deploy_ps3.sh)
#   ./pkg.sh --full   FlightGear-0.9.10-PS3.pkg: the same plus the FlightGear data
#                     in USRDIR/fgdata (run ./fetch_data.sh first)
# Needs the PS3 toolchain's host tools (make_self_npdrm, sfo.py, pkg.py,
# package_finalize) in $PS3DEV/bin, default ~/ps3dev/bin. Run after ./dk make.
set -e
R="$(cd "$(dirname "$0")" && pwd)"
T="${PS3DEV:-$HOME/ps3dev}/bin"
CONTENTID=UP0001-FGFS00910_00-0000000000000000
DATA="$R/fgdata_x/fgfs-base-0.9.10.orig"
P="$R/build/pkg"
OUT="$R/fgfs-ps3.pkg"

rm -rf "$P" && mkdir -p "$P/USRDIR"
"$T/make_self_npdrm" "$R/build/fgfs.run.elf" "$P/USRDIR/EBOOT.BIN" "$CONTENTID" > /dev/null
# the same program as a plain SELF: what the hangar restarts into (exitspawn)
"$T/make_self" "$R/build/fgfs.run.elf" "$P/USRDIR/RELOAD.SELF" > /dev/null
python3 "$T/sfo.py" --title "FlightGear 0.9.10" --appid FGFS00910 -f "$T/sfo.xml" "$P/PARAM.SFO"
cp "$R/port/ICON0.PNG" "$P/ICON0.PNG"
cp "$R/port/fgfs.args" "$P/USRDIR/fgfs.args"
if [ "$1" = --full ]; then
    [ -d "$DATA" ] || { echo "no data: run ./fetch_data.sh first"; exit 1; }
    cp -r --preserve=timestamps "$R/port/fgdata/." "$DATA/"
    # UIUC aircraft need 118 MB more memory than there is; Docs are not needed;
    # aircraft that are not in the base package (installed through the hangar
    # in RPCS3) are downloads
    BASE=$(tar tzf "$R/dl/fgfs-base_0.9.10.orig.tar.gz" | awk -F/ '$2 == "Aircraft" && NF > 3 { print $3 }' | sort -u)
    EXCL="--exclude=./Aircraft/UIUC --exclude=./Docs"
    for d in $(ls "$DATA/Aircraft"); do echo "$BASE" | grep -qx "$d" || EXCL="$EXCL --exclude=./Aircraft/$d"; done
    (cd "$DATA" && tar cf - $EXCL .) | (mkdir -p "$P/USRDIR/fgdata" && cd "$P/USRDIR/fgdata" && tar xf -)
    OUT="$R/FlightGear-0.9.10-PS3.pkg"
fi
python3 "$T/pkg.py" --contentid "$CONTENTID" "$P/" "$OUT" > /dev/null
"$T/package_finalize" "$OUT" > /dev/null
ls -la "$OUT"
