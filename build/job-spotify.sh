# psionnet-spotify: the PsionLX half of PsionNet's Spotify bridge.
#
# The source lives in workspace/psionnet-spotify/ and is copied into the
# chroot first:
#
#   rm -rf build/buildroot/build/psionnet-spotify
#   cp -R psionnet-spotify build/buildroot/build/
#   scripts/qbuild.sh build/job-spotify.sh
#
# The chroot carries GTK 2.6 / GLib 2.6 headers; Psion's image has GTK 2.4.13
# and GLib 2.4.7. The program uses only the 2.4 API, and scripts/linkcheck.py
# against the card's root afterwards proves no newer symbol slipped in.
set -x
rm -rf /build/out; mkdir -p /build/out/bin
cd /build/psionnet-spotify || { echo "MISSING source: copy psionnet-spotify into the chroot"; exit 1; }
rm -f psionnet-spotify
gcc-3.4 -O2 -Wall -o psionnet-spotify psionnet-spotify.c \
    `pkg-config --cflags --libs gtk+-2.0 gthread-2.0`
echo "RC=$?"
strip psionnet-spotify
[ -f psionnet-spotify ] && { cp psionnet-spotify /build/out/bin/; echo GOT_spotify; } || echo MISSING_spotify
ls -l /build/out/bin
