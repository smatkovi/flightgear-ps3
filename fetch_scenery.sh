#!/bin/bash
# Add Central Europe from FlightGear's World Scenery 2.12 to the data in
# fgdata_x/ (run ./fetch_data.sh first): the whole 137 MB archive, 10-20 E /
# 40-50 N - Vienna, Munich, Salzburg, Innsbruck, Budapest, Zagreb, Venice,
# Rome and more; see tools/ws2_install.py.
set -e
R="$(cd "$(dirname "$0")" && pwd)"
F="$R/dl/ws2-e010n40.tgz"
DATA="$R/fgdata_x/fgfs-base-0.9.10.orig"
TILES=""      # all 1x1 degree cells of the archive, filled in below
[ -d "$DATA" ] || { echo "no data: run ./fetch_data.sh first"; exit 1; }
mkdir -p "$R/dl"
[ -f "$F" ] || curl -fsSL --retry 3 -o "$F" https://mirrors.ibiblio.org/flightgear/ftp/Scenery-v2.12/e010n40.tgz
echo "13827dbdeb4be2865c46a12aaff08ca3d469762b1f4992bf878612e2a44cee88  $F" | sha256sum -c --quiet
W="$R/build/ws2" && rm -rf "$W" && mkdir -p "$W"
tar xzf "$F" -C "$W"
TILES=$(cd "$W/Terrain" && ls -d e010n40/e* | sort)
python3 "$R/tools/ws2_install.py" "$W" "$DATA" $TILES
rm -rf "$W"
