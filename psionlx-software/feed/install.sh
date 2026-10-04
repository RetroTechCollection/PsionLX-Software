#!/bin/sh
# PsionLX Software -- the installer.
#
# Puts "Find new software" on a PsionLX netBook Pro, from the PsionLX-Software
# folder of the RetroTechCollection archive, through PsionNet on a Mac on the
# same network. On the netBook Pro, in Terminal:
#
#     su
#     wget -O - http://<the Mac's address>:8080/lx/install | sh
#
# PsionNet fills its own address in below when it hands this script over.
# Run some other way, give the address yourself:  sh install.sh 192.168.1.4:8080
# ipkg checks every package against the MD5 in the package list.
PSIONNET=${1:-@PSIONNET@}
unset http_proxy HTTP_PROXY      # the files come from the local network
fail() { echo "*** $1"; exit 1; }
[ "$(id -u)" = 0 ] || fail "This needs root: type su, then run it again."
case "$PSIONNET" in
  *@*|"") fail "Give PsionNet's address: sh install.sh 192.168.1.4:8080" ;;
  *[!A-Za-z0-9.:-]*) fail "That does not look like an address: $PSIONNET" ;;
esac
mkdir -p /etc/ipkg
cat > /etc/ipkg/psionlx-software.conf <<CONF
# PsionLX Software: the PsionLX-Software folder of the RetroTechCollection
# archive, through PsionNet at $PSIONNET. "Find new software" keeps this up to date.
src psionlx-software http://$PSIONNET/lx/software
dest root /
CONF
echo "Fetching the package list through PsionNet at $PSIONNET..."
ipkg-cl update || fail "Could not reach PsionNet at $PSIONNET. Is it running, with \"netBook Pro (PsionLX, network)\" chosen?"
echo "Installing Find new software..."
ipkg-cl install psionlx-software || fail "ipkg could not install it; see the messages above."
# Adoption: a card made from an earlier full image has these programs already,
# but ipkg has no record of them. Install each one that is present -- the same
# files, now on record -- so Find new software shows it installed.
echo "Looking for programs already on this card..."
if wget -q -O /tmp/psionlx-catalogue.txt "http://$PSIONNET/lx/software/catalogue.txt"; then
  TAB=$(printf '\t')
  sed 1d /tmp/psionlx-catalogue.txt | while IFS="$TAB" read -r NAME TITLE CAT VER SIZE ICON EXE SUM DESC SEEN; do
    [ -n "$SEEN" ] && [ -e "$SEEN" ] || continue
    HAVE=$(awk -v p="$NAME" '$0 == "Package: " p { f = 1 } f && /^Version: / { print $2; exit } /^$/ { f = 0 }' /usr/lib/ipkg/status)
    [ "$HAVE" = "$VER" ] && continue
    echo "  $TITLE is here already: putting it on record"
    ipkg-cl install "$NAME" > /dev/null 2>&1 || echo "  ($TITLE: ipkg could not; it still works)"
  done
  rm -f /tmp/psionlx-catalogue.txt
fi
# Psion's launcher was restarted to pick up its TASKS entry; make sure it is up.
if ! ps | grep -q "[g]pe-appmgr"; then
  su - lx -c "DISPLAY=:0 gpe-appmgr -d -r > /dev/null 2>&1 &"
fi
echo
echo "Done. Tap TASKS, then Find new software, to choose what else to install."
echo "Firefox uses PsionNet by itself from now on."
