#!/bin/bash
# Put FlightGear on a real PS3 running webMAN MOD (FTP + HTTP), CFW or HEN:
#   1. upload the FlightGear data to /dev_hdd0/game/FGFS00910/USRDIR/fgdata
#      (resumable; only changed files are sent again)
#   2. upload fgfs-ps3.pkg to /dev_hdd0/packages and ask webMAN to install it
#      (if that does not work: XMB > Game > Package Manager > Install Package Files)
# Usage: PS3=192.168.1.6 ./deploy_ps3.sh [--no-hires]   (--no-hires skips Textures.high, 73 MB)
set -e
R="$(cd "$(dirname "$0")" && pwd)"
: "${PS3:?set PS3=<console ip>}"
DATA="$R/fgdata_x/fgfs-base-0.9.10.orig"
DEST=/dev_hdd0/game/FGFS00910/USRDIR/fgdata
EXCL="--exclude Aircraft/UIUC/ --exclude Docs/"   # UIUC aircraft need 118 MB more than there is
[ "$1" = --no-hires ] && EXCL="$EXCL --exclude Textures.high/"

curl -s --max-time 5 "http://$PS3/cpursx.ps3" > /dev/null || { echo "PS3 at $PS3 does not answer (webMAN)"; exit 1; }
[ -f "$R/fgfs-ps3.pkg" ] || { echo "build the package first: ./pkg.sh"; exit 1; }
cp -r "$R/port/fgdata/." "$DATA/"     # regional databases, controller bindings

# webMAN's FTP has no TLS; lftp would try it first
lftp -c "set ftp:ssl-allow no; set ftp:passive-mode on; set net:timeout 30;
  set net:max-retries 5; set net:reconnect-interval-base 3;
  open -u anonymous, $PS3;
  mkdir -p -f '$DEST';
  mirror -R --only-newer --parallel=2 $EXCL --verbose=1 '$DATA' '$DEST';
  mkdir -p /dev_hdd0/packages; put -O /dev_hdd0/packages '$R/fgfs-ps3.pkg'"

curl -s --max-time 120 "http://$PS3/install.ps3/dev_hdd0/packages/fgfs-ps3.pkg" > /dev/null \
  && echo "install requested via webMAN" \
  || echo "webMAN install did not answer: install /dev_hdd0/packages/fgfs-ps3.pkg from the XMB"
echo "done"
