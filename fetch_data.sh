#!/bin/bash
# Download the FlightGear 0.9.10 base data (161 MB), check it, unpack it into
# fgdata_x/ and add the PS3 changes from port/fgdata: the PS3 controller
# bindings, the hangar's aircraft list and the Nasal additions.
set -e
R="$(cd "$(dirname "$0")" && pwd)"
SNAP=https://snapshot.debian.org/archive/debian/20070301T000000Z/pool/main
F="$R/dl/fgfs-base_0.9.10.orig.tar.gz"
mkdir -p "$R/dl" "$R/fgdata_x"
[ -f "$F" ] || curl -fsSL --retry 3 -o "$F" "$SNAP/f/fgfs-base/fgfs-base_0.9.10.orig.tar.gz"
echo "243fec3287a84f86d8f12c62931119fe4c58ad3a9cbb8688366073b359a7bd4f  $F" | sha256sum -c --quiet
tar xzf "$F" -C "$R/fgdata_x"
cp -r "$R/port/fgdata/." "$R/fgdata_x/fgfs-base-0.9.10.orig/"
echo "data ready in fgdata_x/fgfs-base-0.9.10.orig"
