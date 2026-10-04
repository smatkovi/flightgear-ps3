#!/bin/bash
# Download FlightGear 0.9.10, SimGear 0.3.10 and PLIB 1.8.4, check them, unpack
# them into src/ and apply the PS3 patches from port/patches. Running it again
# starts over from clean sources (the downloads are kept in dl/).
set -e
R="$(cd "$(dirname "$0")" && pwd)"
SNAP=https://snapshot.debian.org/archive/debian/20070301T000000Z/pool/main

get() {     # url sha256
    local f="$R/dl/$(basename "$1")"
    mkdir -p "$R/dl"
    [ -f "$f" ] || curl -fsSL --retry 3 -o "$f" "$1"
    echo "$2  $f" | sha256sum -c --quiet
}
get $SNAP/f/flightgear/flightgear_0.9.10.orig.tar.gz 6307ad26e4141d27dcd66962f9c8e117206197a468b4f8bc99a6a31deb2e4992
get $SNAP/s/simgear/simgear_0.3.10.orig.tar.gz fc14e986d11212a5435d684c4f7cd4245e5afa942db5a8e48b82cbd57ac84720
get http://plib.sourceforge.net/dist/plib-1.8.4.tar.gz 79e71d02fc2d7c984a4341239ed1d89ced743db2d6d4f83c30c422edaa6020e1

mkdir -p "$R/src" && cd "$R/src"
rm -rf FlightGear-0.9.10 SimGear-0.3.10 plib-1.8.4
for t in flightgear_0.9.10.orig simgear_0.3.10.orig plib-1.8.4; do tar xzf "$R/dl/$t.tar.gz"; done
patch -s -p1 -d FlightGear-0.9.10 < "$R/port/patches/flightgear-ps3.diff"
patch -s -p1 -d SimGear-0.3.10 < "$R/port/patches/simgear-ps3.diff"
patch -s -p1 -d plib-1.8.4 < "$R/port/patches/plib-ps3.diff"
rm -rf "$R/build/include" "$R/build/sg_srcs.mk" "$R/build/fg_srcs.mk"
echo "sources ready in src/ - next: ./dk make -j8"
