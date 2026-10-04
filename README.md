# PsionLX Software

**Find new software** for PsionLX, the Linux that Psion Teklogix built for the
netBook Pro in 2005 and never released. Psion's TASKS screen listed "Find new
software" and gave it no command: their launcher answered *"This task is not
yet implemented"*. This is that task, finished, along with the other programs
written for PsionLX in 2026.

| | |
|---|---|
| `psionlx-software/` | **Find new software**: a catalogue of programs, games and fonts, installed with Psion's own package manager (ipkg) |
| `psionnet-spotify/` | **Spotify** for the netBook Pro, through [PsionNet](https://github.com/JYewman/PsionNet) |
| `scripts/mkfeed.py` | builds the package library, `PsionLX-Software`, served from the RetroTechCollection archive |
| `build/` | the jobs that compile both programs in the PsionLX build system's ARM OABI chroot |

The packages live at
**<https://archive.retrotechcollection.com/PsionLX-Software>**, and PsionLX
itself, with its three CF card images and complete build system, at
<https://archive.retrotechcollection.com/?dir=PsionLX>.

## Installing on a netBook Pro

The netBook Pro cannot fetch from the archive itself: its TLS is twenty years
out of date. [PsionNet](https://github.com/JYewman/PsionNet), on a Mac on the
same network, fetches for it. Start PsionNet with *netBook Pro (PsionLX,
network)*, then on the netBook Pro, in Terminal:

    su
    wget -O - http://<the Mac's address>:8080/lx/install | sh

That installs Find new software -- on the TASKS screen, where Psion put it --
and a login helper that points Firefox at PsionNet. Programs already on the
card (from an earlier full image) are put on record, so they show as
installed. The full PsionLX image has all of it already except Spotify,
which is one Install away in Find new software.

## How it works

* The catalogue (`catalogue.txt`, with icons) and the packages come from the
  archive through PsionNet's `/lx/software/` route, which hands over the
  archive's bytes untouched. ipkg checks every package's MD5.
* Installing needs root. The app runs `ipkg-run`, a helper that does nothing
  but run ipkg, with plain `su`: PsionLX's root password is empty, so nothing
  is asked. Psion's own `gpe-su` cannot do it -- it waits for su to ask for a
  password, which with an empty one it never does -- so it is used only if a
  root password has been set. Afterwards `killall -HUP gpe-appmgr` makes
  Psion's launcher re-read its programs.
* `psionnet-connect` runs at each login: it finds PsionNet by UDP broadcast
  and writes a proxy auto-config file that Firefox follows -- through PsionNet
  when it is there, direct when it is not.
* Each package depends only on packages in Psion's own image, worked out from
  what its programs link. `mkfeed.py` refuses a package that would need a
  library Psion's image lacks, or would overwrite one of Psion's files.
* Psion's launcher gives the TASKS entry no command. Installing Find new
  software swaps in a copy that runs `psionlx-software` -- 34 bytes changed,
  only over Psion's exact binary, with Psion's kept to restore on removal --
  and restarts it. The full image has the same change built in.

Both programs use only GTK 2.4 and GLib 2.4, as Psion shipped them.

## Building

These folders are part of the PsionLX build system (`4-BUILD-SYSTEM` in the
PsionLX release on the archive), which has the ARM OABI chroot and the images
the packages are built from. Put them there, then:

    rm -rf arm-oabi-chroot/build/psionlx-software && cp -R psionlx-software arm-oabi-chroot/build/
    scripts/qbuild.sh build/job-software.sh
    rm -rf arm-oabi-chroot/build/psionnet-spotify && cp -R psionnet-spotify arm-oabi-chroot/build/
    scripts/qbuild.sh build/job-spotify.sh
    python3 scripts/mkfeed.py

## Licence

GPL-2.0-or-later. Find new software's icon is Psion's own, drawn in 2005 for
this task and taken from their PsionLX image.

Spotify is a trademark of Spotify AB; neither program is made or endorsed by
Spotify.
