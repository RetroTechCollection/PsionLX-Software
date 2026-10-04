===============================================================================
 PsionLX SOFTWARE
 Programs, games and fonts for PsionLX on the Psion Teklogix netBook Pro
===============================================================================

 Everything the full PsionLX image (../PsionLX/1-IMAGES/psionlx-cf-1gb.img)
 adds to Psion's own image, as packages for Psion's own package manager, so
 a netBook Pro running ANY PsionLX card can install them:

   Find new software    Psion's own TASKS entry, finished: the catalogue that
                        installs everything below, and a helper that sets
                        Firefox up for PsionNet
   Spotify              your Spotify, played through PsionNet (Premium)
   Games                Bombs, Tetris, Othello, Go, Lights Out, Sokoban
   Tools                Jotter, Plucker, Watch, Time Tracker, Program manager
   Sound                Play and Record, Audio mixer
   Internet             Telnet, WLAN Config
   Fonts                Psion's Agfa fonts: Albany, Arial Narrow, Cumberland
                        and Thorndale

 The full image has all of these already. On every card, "Find new
 software" lives where Psion put it, on the TASKS screen -- the task Psion
 listed in 2005 and never finished.


-------------------------------------------------------------------------------
 WHAT YOU NEED
-------------------------------------------------------------------------------

 * A netBook Pro running PsionLX from a CF card -- any of the three images
   in ../PsionLX/1-IMAGES.
 * A network connection for it: a wired or wireless network card.
 * PsionNet, free, on a Mac on the same network:
       https://github.com/JYewman/PsionNet
   with Device set to "netBook Pro (PsionLX, network)" and the proxy
   started. The netBook Pro cannot fetch from this archive itself -- its
   secure-connection (TLS) support is twenty years out of date -- so PsionNet
   fetches for it.


-------------------------------------------------------------------------------
 INSTALLING
-------------------------------------------------------------------------------

 1. On the Mac, start PsionNet with "netBook Pro (PsionLX, network)" and
    press Start. Note the address it shows, for example 192.168.1.4.

 2. On the netBook Pro, open Terminal (PROGRAMS > OTHER) and type, using
    the Mac's address:

        su
        wget -O - http://192.168.1.4:8080/lx/install | sh

    su asks for no password on PsionLX. The installer sets up Psion's
    package manager to fetch from this folder through PsionNet, installs
    "Find new software", and says "Done". The desktop blinks once as
    Psion's launcher restarts with the TASKS entry working.

 3. Tap TASKS, then Find new software. Pick something and press Install.
    PsionLX asks for the root password: there is none, so just press OK.
    New programs appear in PROGRAMS straight away.

 The installer is install.sh, in this folder. PsionNet puts its own address
 into it as it hands it over. Run any other way -- copied across on a card,
 say -- it needs the address given:

        sh install.sh 192.168.1.4:8080


-------------------------------------------------------------------------------
 WHAT IT CHANGES ON THE MACHINE
-------------------------------------------------------------------------------

 * Packages only add files; none replaces one of Psion's, and Remove (in
   "Find new software") takes a package's files away again.
 * The one exception is Psion's launcher, gpe-appmgr: its TASKS entry
   "Find new software" has no command, so installing Find new software
   swaps in a copy that runs it -- the same 34-byte change as in the full
   image, made only over Psion's exact program. Psion's copy is kept in
   /usr/lib/psionlx-software and put back if Find new software is removed.
 * /etc/ipkg/psionlx-software.conf says where packages come from.
 * At each login, psionnet-connect looks for PsionNet and writes
   ~/.psionnet/proxy.pac, which Firefox follows: the web goes through
   PsionNet when it is there, and straight out when it is not. To manage
   Firefox's proxy yourself instead, create the file ~/.psionnet/manual.
 * Psion's Agfa fonts replace Psion's own, empty, agfa-fonts package, and add
   a marked block to /etc/fonts/local.conf that keeps the desktop's lettering
   in Bitstream Vera, as Psion had it. Removing them takes the block out.
 * The internal flash (NAND) is never touched.


-------------------------------------------------------------------------------
 WITHOUT PsionNet
-------------------------------------------------------------------------------

 These are ordinary ipkg packages. Copy the ones you want onto an SD card or
 a USB stick, put it in the netBook Pro, and in Terminal:

        su
        mount /media/mmc                (a USB stick: /media/usb)
        ipkg-cl install /media/mmc/gpe-tetris_0.6-2-r0_armv5te.ipk

 Spotify and "Find new software" are no use without PsionNet; the games,
 tools and fonts are.


-------------------------------------------------------------------------------
 WHAT IS IN THIS FOLDER
-------------------------------------------------------------------------------

 *.ipk            the packages, in the format Psion's own build made them
 Packages         ipkg's list of them, with each one's size and MD5 checksum
 catalogue.txt    what "Find new software" shows: names, descriptions, icons
 icons/           its icons
 install.sh       the installer described above
 SHA256SUMS.txt   checksums of every file here

 Built by scripts/mkfeed.py in the PsionLX build system
 (../PsionLX/4-BUILD-SYSTEM), from the same files the full image is built
 from. Each package depends only on packages in Psion's own image, worked
 out from the libraries its programs use. The source of "Find new software"
 is at https://github.com/RetroTechCollection/PsionLX-Software, and of
 Spotify in PsionNet.

 Spotify is a trademark of Spotify AB; neither program is made or endorsed by
 Spotify.
