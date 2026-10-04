# psionlx-software and psionnet-find: PsionLX's "Find new software", and the
# little tool that finds PsionNet on the network for shell scripts.
#
# The source lives in workspace/psionlx-software/ and is copied into the
# chroot first:
#
#   rm -rf build/buildroot/build/psionlx-software
#   cp -R psionlx-software build/buildroot/build/
#   scripts/qbuild.sh build/job-software.sh
#
# GTK 2.6 headers in the chroot, GTK 2.4 / GLib 2.4 on the card: only the 2.4
# API is used, and scripts/linkcheck.py on the card's tree proves it.
set -x
rm -rf /build/out; mkdir -p /build/out/bin
cd /build/psionlx-software || { echo "MISSING source: copy psionlx-software into the chroot"; exit 1; }
rm -f psionlx-software psionnet-find
gcc-3.4 -O2 -Wall -o psionlx-software psionlx-software.c \
    `pkg-config --cflags --libs gtk+-2.0 gthread-2.0`
echo "RC_APP=$?"
gcc-3.4 -O2 -Wall -o psionnet-find psionnet-find.c
echo "RC_FIND=$?"
strip psionlx-software psionnet-find
for f in psionlx-software psionnet-find; do
  [ -f $f ] && { cp $f /build/out/bin/; echo GOT_$f; } || echo MISSING_$f
done
ls -l /build/out/bin
