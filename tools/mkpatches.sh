#!/bin/bash
# Regenerate port/patches/*.diff from the changed sources in src/ against the
# original tarballs in dl/ (the inverse of fetch_sources.sh).
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cd "$T"
for t in flightgear_0.9.10.orig simgear_0.3.10.orig plib-1.8.4; do tar xzf "$R/dl/$t.tar.gz"; done
mk() {      # tree patch
    ln -s "$R/src/$1" "$1.ps3"
    diff -ruN --exclude='*.orig' --exclude='*.rej' "$1" "$1.ps3/" > "$R/port/patches/$2" || true
    echo "$2: $(grep -c '^+++ ' "$R/port/patches/$2") files"
}
mk FlightGear-0.9.10 flightgear-ps3.diff
mk SimGear-0.3.10 simgear-ps3.diff
mk plib-1.8.4 plib-ps3.diff
