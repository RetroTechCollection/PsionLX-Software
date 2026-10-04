"""Build PsionLX-Software: the archive folder "Find new software" installs from.

    python3 scripts/mkfeed.py        # -> PsionLX-Software/ beside PsionLX/

The packages are the programs, games and fonts the full image adds to Psion's
image, built from the same files merge-soho.py puts there, so a stock PsionLX
card that installs them all ends up with the same programs. merge-soho.py
imports this module as well, to install our programs and to register every
package in the full image's ipkg database: the image and the feed share one
definition and cannot drift apart.

Each package's Depends names Psion's own packages, worked out from what its
programs link (DT_NEEDED) and the stock image's ipkg file lists. The extras'
original control files name community-image versions (gtk+ >= 2.6.3) that
Psion's image cannot satisfy, though linkcheck.py shows they run on its
libraries.

The format is Psion's ipkg-build's: an ar archive holding debian-binary,
data.tar.gz and control.tar.gz, with ./-relative members owned by root.
Every timestamp is fixed, so an unchanged package rebuilds byte for byte.
"""
import gzip
import hashlib
import io
import os
import shutil
import stat
import struct
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
W = HERE.parent                                   # the workspace (4-BUILD-SYSTEM)
SOHO = W / 'card-soho' / 'root'                   # Psion's image, for Depends
OLD = W / 'card-reconstruction' / 'root'          # where the extras come from
BUILT = next((d for d in (W / 'built', W / 'built-binaries') if d.is_dir()), W / 'built') / 'psion-apps'
SPOTIFY = W / 'psionnet-spotify'
SOFTWARE = W / 'psionlx-software'
FEEDDOCS = SOFTWARE / 'feed'                      # README.txt, install.sh, icons/README.txt
OUT = W.parent / 'PsionLX-Software'
FEED_URL = 'https://archive.retrotechcollection.com/PsionLX-Software'
MAINTAINER = f'PsionLX Software <{FEED_URL}>'
EPOCH = 1791072000                                # 4 October 2026, 00:00 UTC

HUP = 'killall -HUP gpe-appmgr 2>/dev/null   # Psion\'s launcher re-reads its programs\n'
RXVT = 'rxvt-unicode'                             # Psion's terminal, which Telnet opens

# The fonts make "Sans" ambiguous, which pushed the desktop's rows under the
# dock (the font bug of 2026); this pins the generic families to Bitstream
# Vera, as Psion's shipped image effectively had them. The same block goes
# into the full image's local.conf (merge-soho.py), between the same markers.
FONT_PIN_BEGIN = '<!-- PsionLX Agfa fonts: begin -->'
FONT_PIN_END = '<!-- PsionLX Agfa fonts: end -->'
FONT_PIN = (
    FONT_PIN_BEGIN + '\n'
    '<!-- The Agfa fonts make "Sans" ambiguous. Psion\'s shipped image had only\n'
    '     Bitstream Vera, so pin the generic families to it and leave Psion\'s\n'
    '     gtkrc as it was. Added by the agfa-fonts package (PsionLX, 2026). -->\n'
    '<alias><family>sans-serif</family><prefer><family>Bitstream Vera Sans</family></prefer></alias>\n'
    '<alias><family>Sans</family><prefer><family>Bitstream Vera Sans</family></prefer></alias>\n'
    '<alias><family>serif</family><prefer><family>Bitstream Vera Serif</family></prefer></alias>\n'
    '<alias><family>monospace</family><prefer><family>Bitstream Vera Sans Mono</family></prefer></alias>\n'
    + FONT_PIN_END + '\n')

# Psion's launcher, gpe-appmgr-psion 56-r0. TASKS is a table of {name, icon,
# command} pointers (file offset 0xa7e0, eleven entries); "Find new software"
# has a NULL command, so the launcher shows "This task is not yet implemented"
# -- that message's only use, and the table's only NULL (docs/09 \u00a712). The
# command goes into the message's bytes and the entry points at it: 34 bytes.
APPMGR_MD5 = 'bc5ce737df02f6856ec1795e75867240'
APPMGR_MSG_OFF, APPMGR_MSG_VA = 0xa120, 0x12120
APPMGR_TASKS, APPMGR_COUNT, APPMGR_FIND_NEW = 0xa7e0, 11, 6


def patch_appmgr(data: bytes) -> bytes:
    """Psion's gpe-appmgr, with TASKS > Find new software running psionlx-software."""
    if hashlib.md5(data).hexdigest() != APPMGR_MD5:
        raise SystemExit('gpe-appmgr is not Psion\'s 56-r0 binary: not patching it')
    d = bytearray(data)
    msg = b'This task is not yet implemented\0'
    cmds = [struct.unpack('<I', d[APPMGR_TASKS + 12 * i + 8:APPMGR_TASKS + 12 * i + 12])[0]
            for i in range(APPMGR_COUNT)]
    if d[APPMGR_MSG_OFF:APPMGR_MSG_OFF + len(msg)] != msg or \
            [i for i, c in enumerate(cmds) if c == 0] != [APPMGR_FIND_NEW]:
        raise SystemExit('gpe-appmgr: the task table is not as disassembled')
    cmd = b'psionlx-software\0'
    d[APPMGR_MSG_OFF:APPMGR_MSG_OFF + len(msg)] = cmd + b'\0' * (len(msg) - len(cmd))
    struct.pack_into('<I', d, APPMGR_TASKS + 12 * APPMGR_FIND_NEW + 8, APPMGR_MSG_VA)
    return bytes(d)


def patched_appmgr() -> Path:
    """The patched launcher, as a file the psionlx-software package can carry."""
    out = BUILT / 'gpe-appmgr.find-new-software'
    data = patch_appmgr((SOHO / 'usr/bin/gpe-appmgr').read_bytes())
    if not out.exists() or out.read_bytes() != data:
        out.write_bytes(data)
    os.chmod(out, 0o755)
    return out


# Swap Psion's launcher for the patched one -- only over Psion's exact binary,
# keeping Psion's -- and restart it, so TASKS > Find new software works at
# once. Psion's session starts it in the background (/etc/matchbox/session:
# "gpe-appmgr -d -r &"); the window manager holds the session, and the launcher
# survives the installing terminal closing because it catches SIGHUP.
# Wait for it to come up: until it has installed that handler, the SIGHUP the
# next package's install sends it is fatal (seen on the hardware, 4 Oct 2026).
_LAUNCHER = """A=/usr/bin/gpe-appmgr
D=/usr/lib/psionlx-software
sum_of() { set -- $(md5sum "$1"); echo "$1"; }
restart_launcher() {
  killall gpe-appmgr 2>/dev/null || return 0
  sleep 1
  XA=; [ -f /home/lx/.Xauthority ] && XA=XAUTHORITY=/home/lx/.Xauthority
  su - lx -c "DISPLAY=:0 $XA gpe-appmgr -d -r > /dev/null 2>&1 &"
  sleep 10
}
"""


def _swap_in():
    return _LAUNCHER + f"""# TASKS > Find new software: Psion's launcher has no command for it.
if [ "$(sum_of $A)" = "{APPMGR_MD5}" ]; then
  cp $A $D/gpe-appmgr.psion && cp $D/gpe-appmgr $A.new && chmod 755 $A.new \\
    && mv $A.new $A && restart_launcher
fi
"""


def _swap_out():
    patched = hashlib.md5(patched_appmgr().read_bytes()).hexdigest()
    return _LAUNCHER + f"""# Put Psion's launcher back, if ours is the one in place.
if [ "$(sum_of $A)" = "{patched}" ] && [ -f $D/gpe-appmgr.psion ]; then
  cp $D/gpe-appmgr.psion $A.new && chmod 755 $A.new && mv $A.new $A \\
    && rm -f $D/gpe-appmgr.psion && restart_launcher
fi
"""


AGFA = ['ALBW.TTF', 'ALBWB.TTF', 'ALBWBI.TTF', 'ALBWI.TTF', 'ARIALN.TTF', 'ARIALNB.TTF',
        'ARIALNBI.TTF', 'ARIALNI.TTF', 'CumberlandAMT-Bold.otf', 'CumberlandAMT-BoldItalic.otf',
        'CumberlandAMT-Italic.otf', 'CumberlandAMT-Regular.otf', 'THOWBI__.TTF', 'THOWB___.TTF',
        'THOWI___.TTF', 'THOWR___.TTF']


class Pkg:
    def __init__(self, name, version, title, category, summary, description,
                 files, arch='armv5te', extra_depends=(), postinst=HUP, prerm=None, postrm=HUP,
                 exec_='', icon=None, section='x11/utils', source=''):
        self.name, self.version, self.title, self.category = name, version, title, category
        self.summary, self.description = summary, description
        self.files = files                  # [(source path, path in the image, mode or None)]
        self.arch, self.extra_depends = arch, list(extra_depends)
        self.postinst, self.prerm, self.postrm = postinst, prerm, postrm
        self.exec, self.icon, self.section, self.source = exec_, icon, section, source
        self.depends = []


def _reconstruction(pkg, also=()):
    """A package's files, by its list in the previous reconstruction, as merge-soho takes them."""
    out = []
    for line in (OLD / 'usr/lib/ipkg/info' / f'{pkg}.list').read_text().split('\n'):
        rel = line.strip().lstrip('/')
        src = OLD / rel
        if rel and os.path.lexists(src) and not (src.is_dir() and not src.is_symlink()):
            out.append((src, rel, None))
    return out + [(OLD / rel, rel, None) for rel in also]


def _version(pkg):
    for line in (OLD / 'usr/lib/ipkg/info' / f'{pkg}.control').read_text().split('\n'):
        if line.startswith('Version:'):
            return line.split(':', 1)[1].strip()
    raise SystemExit(f'{pkg}: no version in the reconstruction')


def _extra(name, title, category, summary, description, also=(), section='x11/utils', **kw):
    return Pkg(name, _version(name), title, category, summary, description,
               _reconstruction(name, also), section=section,
               source='the PsionLX full image (built from the community GPE image of 2005)', **kw)


def packages():
    """Every package in the feed, in catalogue order."""
    sw = SOFTWARE
    return [
        Pkg('psionlx-software', '1.0', 'Find new software', 'System',
            'This program, and the helper that points Firefox at PsionNet.',
            'The catalogue you are looking at. It installs and removes software with '
            'Psion\'s own package manager, through PsionNet on a Mac on the same network. '
            'It also sets Firefox up for PsionNet each time you log in: the web goes '
            'through PsionNet when it is there, and straight out when it is not.',
            [(BUILT / 'psionlx-software', 'usr/bin/psionlx-software', 0o755),
             (BUILT / 'psionnet-find', 'usr/bin/psionnet-find', 0o755),
             (sw / 'psionnet-connect', 'usr/bin/psionnet-connect', 0o755),
             (sw / 'ipkg-run', 'usr/lib/psionlx-software/ipkg-run', 0o755),
             (sw / '75psionnet', 'etc/X11/Xsession.d/75psionnet', 0o755),
             (sw / 'psionnet.js', 'usr/lib/firefox-1.0/defaults/pref/psionnet.js', 0o644),
             (sw / 'psionlx-software.png', 'usr/share/pixmaps/psionlx-software.png', 0o644),
             (patched_appmgr(), 'usr/lib/psionlx-software/gpe-appmgr', 0o755)],
            extra_depends=['ipkg', 'gpe-su', 'busybox', 'gpe-appmgr-psion'], exec_='psionlx-software',
            postinst=_swap_in() + '# Set Firefox up for PsionNet now, rather than at the next login.\n'
                                  '[ -d /home/lx ] && su lx -c "HOME=/home/lx /usr/bin/psionnet-connect" '
                                  '> /dev/null 2>&1 &\n',
            prerm=_swap_out(), postrm=None,
            source='PsionLX (2026), https://github.com/RetroTechCollection'),
        Pkg('psionnet-spotify', '1.0', 'Spotify', 'Sound',
            'Your Spotify, played through PsionNet on a Mac.',
            'Browse your Liked Songs and playlists, search, and play, on the netBook Pro\'s '
            'own speaker. PsionNet does the talking to Spotify: run it on a Mac on the same '
            'network with "netBook Pro (PsionLX, network)" chosen and log in there. Needs '
            'Spotify Premium.',
            [(BUILT / 'psionnet-spotify', 'usr/bin/psionnet-spotify', 0o755),
             (SPOTIFY / 'psionnet-spotify.desktop', 'usr/share/applications/psionnet-spotify.desktop', 0o644),
             (SPOTIFY / 'psionnet-spotify.png', 'usr/share/pixmaps/psionnet-spotify.png', 0o644)],
            extra_depends=['gstreamer', 'gst-plugin-gnomevfs', 'gst-plugin-mad',
                           'gst-plugin-audioconvert', 'gst-plugin-esd', 'esd', 'gnome-vfs'],
            exec_='psionnet-spotify', section='x11/multimedia',
            source='PsionNet (2026), https://github.com/JYewman/PsionNet'),
        _extra('xdemineur', 'Bombs', 'Games', 'Minesweeper: clear the field without a bang.',
               'Uncover every square that hides no bomb, using the numbers to work out where '
               'the bombs are. Named Bombs, after the game on Psion\'s EPOC machines.',
               also=['usr/share/gpe/overrides/xdemineur.desktop'], section='x11/games'),
        _extra('gpe-tetris', 'Tetris', 'Games', 'The falling-blocks game.',
               'Turn and drop the falling blocks to fill whole lines; filled lines vanish, and '
               'the blocks fall faster as you go.', section='x11/games'),
        _extra('gpe-othello', 'Othello', 'Games', 'The board game of flipping counters.',
               'Outflank your opponent\'s counters to turn them to your colour. Whoever has '
               'more counters when the board is full wins.', section='x11/games'),
        _extra('gpe-go', 'Go', 'Games', 'A Go board, for two players.',
               'The ancient board game of surrounding territory, on a board for two people '
               'sharing the machine.', section='x11/games'),
        _extra('gpe-lights', 'Lights Out', 'Games', 'Switch every light off.',
               'Pressing a light switches it and its neighbours. Find the presses that leave '
               'the whole board dark.', section='x11/games'),
        _extra('gsoko', 'Sokoban', 'Games', 'Push the crates onto their spots.',
               'A warehouse puzzle: push every crate onto a target square. Crates can be '
               'pushed but never pulled, so one wrong push can trap a crate for good.',
               section='x11/games'),
        _extra('figment', 'Jotter', 'Tools', 'Notes and lists, as an outline.',
               'An outliner for notes, lists and plans, with entries you can fold away. Named '
               'Jotter, after the program on Psion\'s EPOC machines.',
               also=['usr/share/gpe/overrides/figment.desktop']),
        _extra('gpe-plucker', 'Plucker', 'Tools', 'Reads Plucker e-books.',
               'Reads books and web pages converted to the Plucker format for handhelds.'),
        _extra('gpe-watch', 'Watch', 'Tools', 'A watch face.',
               'A clock face to keep on the screen.'),
        _extra('gpe-timesheet', 'Time Tracker', 'Tools', 'Records the time you spend on things.',
               'Start and stop a timer for each job or task, and see where the time went.'),
        _extra('gpe-taskmanager', 'Program manager', 'Tools', 'See and close the running programs.',
               'Lists the programs that are running, so you can switch to one or close it.'),
        _extra('gpe-soundbite', 'Play and Record', 'Sound', 'Voice notes: record and play back.',
               'Record notes with the microphone and play them back. Appears as Play and '
               'Record, the names Psion\'s EPOC machines used.',
               also=['usr/share/gpe/overrides/gpe-soundbite-play.desktop',
                     'usr/share/gpe/overrides/gpe-soundbite-record.desktop'],
               section='x11/multimedia'),
        _extra('gpe-mixer', 'Audio mixer', 'Sound', 'Volume and microphone levels.',
               'Sliders for the playback, recording and microphone levels.',
               section='x11/multimedia'),
        _extra('telnet-session', 'Telnet', 'Internet', 'Log in to other computers by telnet.',
               'Asks for a machine\'s address and connects to it by telnet, in a terminal '
               'window, as a VT100.', section='x11/network',
               extra_depends=['busybox', RXVT]),     # scripts: rxvt -e telnet
        Pkg('gpe-wlancfg', '0.2.7-r4', 'WLAN Config', 'Internet',
            'Set up a wireless network card.',
            'Network name, mode and encryption key for a wireless card. Built from Psion\'s '
            'patched version, which their image does not include. Appears in the Control Panel.',
            [(OLD / rel, rel, None) for rel in ('usr/bin/gpe-wlancfg',
                                                'usr/share/pixmaps/gpe-wlancfg.png',
                                                'usr/share/applications/gpe-wlancfg.desktop')],
            section='x11/network', source='Psion\'s gpe-wlancfg_0.2.7.oe (r4), built in 2026'),
        Pkg('agfa-fonts', '1.0-r6psionlx1', 'Psion\'s Agfa fonts', 'Fonts',
            'Albany, Arial Narrow, Cumberland and Thorndale.',
            'The sixteen Agfa Monotype fonts Psion\'s build recipe installs: Albany AMT, '
            'Arial Narrow, Cumberland AMT and Thorndale AMT, each in four styles. Psion\'s '
            'shipped package of them was empty, so this replaces it. The desktop keeps its '
            'own font: installing these also pins it to Bitstream Vera, as in Psion\'s image.',
            [(OLD / 'usr/share/fonts/ttf' / f, f'usr/share/fonts/ttf/{f}', 0o644) for f in AGFA],
            arch='all', section='fonts', source='Psion\'s agfa-fonts recipe, r5',
            icon=SOFTWARE / 'agfa-fonts.png',      # catalogue only: fonts have no launcher
            postinst=('F=/etc/fonts/local.conf\n'
                      '# Cards built before these packages pinned Sans already, unmarked.\n'
                      f'if [ -f $F ] && ! grep -q "{FONT_PIN_BEGIN}" $F && ! grep -q '
                      '"<family>Sans</family><prefer><family>Bitstream Vera Sans" $F; then\n'
                      '  awk \'/<\\/fontconfig>/ { while ((getline l < "/usr/share/fonts/ttf/'
                      '.psionlx-pin") > 0) print l } { print }\' $F > $F.new && mv $F.new $F\n'
                      'fi\n') + HUP,
            prerm=('F=/etc/fonts/local.conf\n'
                   f'if [ -f $F ] && grep -q "{FONT_PIN_BEGIN}" $F; then\n'
                   f'  awk \'/{FONT_PIN_BEGIN}/ {{ s = 1 }} !s {{ print }} /{FONT_PIN_END}/ '
                   '{ s = 0 }\' $F > $F.new && mv $F.new $F\n'
                   'fi\n')),
    ]


# --- metadata the catalogue needs --------------------------------------------------

def _desktop(pkg):
    """The launcher the user sees: Psion's EPOC-name override wins over the program's own."""
    found = [(rel, src) for src, rel, _ in pkg.files if rel.endswith('.desktop')]
    found.sort(key=lambda x: ('overrides' not in x[0], x[0]))
    fields = {}
    if found:
        for line in Path(found[0][1]).read_text(errors='replace').split('\n'):
            if '=' in line and not line.startswith('#'):
                k, v = line.split('=', 1)
                fields.setdefault(k.strip(), v.strip())
    return fields


def _icon_source(pkg, fields):
    name = fields.get('Icon', '')
    if not name:
        return None
    for src, rel, _ in pkg.files:
        if rel.endswith('/' + name) or rel == name:
            return src
    for root in (SOHO, OLD):
        p = root / 'usr/share/pixmaps' / name
        if p.exists():
            return p
    return None


# --- ELF dependencies -------------------------------------------------------------

def _needed(path):
    d = Path(path).read_bytes()
    if d[:4] != b'\x7fELF' or len(d) < 52:
        return []
    sh = struct.unpack('<I', d[0x20:0x24])[0]
    es, n, si = struct.unpack('<HHH', d[0x2e:0x34])
    secs = [struct.unpack('<10I', d[sh + i * es:sh + i * es + 40]) for i in range(n)]
    names = secs[si]
    nm = lambda s: d[names[4] + s[0]:d.index(b'\0', names[4] + s[0])].decode()
    dyn = next((s for s in secs if nm(s) == '.dynamic'), None)
    strt = next((s for s in secs if nm(s) == '.dynstr'), None)
    out = []
    if dyn and strt:
        for i in range(0, dyn[5], 8):
            tag, val = struct.unpack('<iI', d[dyn[4] + i:dyn[4] + i + 8])
            if tag == 0:
                break
            if tag == 1:
                s = strt[4] + val
                out.append(d[s:d.index(b'\0', s)].decode())
    return out


def _psion_owners():
    owners = {}
    for lst in sorted((SOHO / 'usr/lib/ipkg/info').glob('*.list')):
        for line in lst.read_text(errors='replace').split('\n'):
            line = line.strip()
            if line:
                owners.setdefault(os.path.basename(line), lst.stem)
    return owners


def _psion_installed():
    names = set()
    for block in (SOHO / 'usr/lib/ipkg/status').read_text(errors='replace').split('\n\n'):
        for line in block.split('\n'):
            if line.startswith('Package: '):
                names.add(line[9:].strip())
    return names


def resolve(pkgs):
    """Fill in each package's Depends, and refuse anything a stock card could not run."""
    owners, have = _psion_owners(), _psion_installed()
    for p in pkgs:
        own = {os.path.basename(rel) for _, rel, _ in p.files}
        deps = set()
        for src, rel, _ in p.files:
            if os.path.islink(src) or not os.path.isfile(src):
                continue
            for so in _needed(src):
                if so in own:
                    continue
                if so not in owners:
                    raise SystemExit(f'{p.name}: {rel} needs {so}, which Psion\'s image lacks')
                deps.add(owners[so])
        for dep in p.extra_depends:
            if dep not in have:
                raise SystemExit(f'{p.name}: depends on {dep}, which Psion\'s image lacks')
            deps.add(dep)
        deps.discard(p.name)
        p.depends = sorted(deps)
        for _, rel, _ in p.files:
            q = SOHO / rel
            if os.path.lexists(q) and not q.is_dir():
                raise SystemExit(f'{p.name}: {rel} would overwrite a file of Psion\'s')


# --- building the .ipk -------------------------------------------------------------

def control_text(p):
    lines = [f'Package: {p.name}', f'Version: {p.version}']
    if p.depends:
        lines.append('Depends: ' + ', '.join(p.depends))
    lines += [f'Section: {p.section}', 'Priority: optional', f'Maintainer: {MAINTAINER}',
              f'Architecture: {p.arch}', f'Source: {p.source}',
              f'Description: {p.title}: {p.summary}']
    return '\n'.join(lines) + '\n'


def _tar(entries):
    """entries: [(name, kind, data_or_target, mode)]; dirs are implied."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as t:
        dirs = set()
        def add_dir(path):
            if path in dirs or path in ('', '.'):
                return
            parent = os.path.dirname(path)
            if parent:
                add_dir(parent)
            dirs.add(path)
            ti = tarfile.TarInfo('./' + path)
            ti.type, ti.mode, ti.mtime, ti.uname, ti.gname = tarfile.DIRTYPE, 0o755, EPOCH, 'root', 'root'
            t.addfile(ti)
        ti = tarfile.TarInfo('./')
        ti.type, ti.mode, ti.mtime, ti.uname, ti.gname = tarfile.DIRTYPE, 0o755, EPOCH, 'root', 'root'
        t.addfile(ti)
        for name, kind, data, mode in sorted(entries):
            if len(name) > 96:
                raise SystemExit(f'{name}: too long for a plain tar header')
            add_dir(os.path.dirname(name))
            ti = tarfile.TarInfo('./' + name)
            ti.mtime, ti.uname, ti.gname = EPOCH, 'root', 'root'
            if kind == 'link':
                ti.type, ti.linkname, ti.mode = tarfile.SYMTYPE, data, 0o777
                t.addfile(ti)
            else:
                ti.mode, ti.size = mode, len(data)
                t.addfile(ti, io.BytesIO(data))
    return gzip.compress(buf.getvalue(), compresslevel=9, mtime=0)


def _ar(members):
    out = b'!<arch>\n'
    for name, data in members:
        out += f'{name + "/":<16}{EPOCH:<12}{0:<6}{0:<6}{"100644":<8}{len(data):<10}`\n'.encode()
        out += data + (b'\n' if len(data) % 2 else b'')
    return out


def build_ipk(p):
    entries = []
    for src, rel, mode in p.files:
        if os.path.islink(src):
            entries.append((rel, 'link', os.readlink(src), 0o777))
        else:
            m = mode if mode is not None else (0o755 if os.stat(src).st_mode & 0o111 else 0o644)
            entries.append((rel, 'file', Path(src).read_bytes(), m))
    if p.name == 'agfa-fonts':          # the block postinst adds to local.conf
        entries.append(('usr/share/fonts/ttf/.psionlx-pin', 'file', FONT_PIN.encode(), 0o644))
    control = [('control', 'file', control_text(p).encode(), 0o644)]
    for script in ('postinst', 'prerm', 'postrm'):
        body = getattr(p, script)
        if body:
            control.append((script, 'file', ('#!/bin/sh\n' + body + 'exit 0\n').encode(), 0o755))
    return _ar([('debian-binary', b'2.0\n'), ('data.tar.gz', _tar(entries)),
                ('control.tar.gz', _tar(control))])


def witness(p):
    """A file whose presence shows the package is on a card already: its program,
    else its first file. The installer registers such packages (adoption)."""
    rels = [rel for _, rel, _ in p.files]
    return '/' + next((r for r in rels if r.startswith('usr/bin/')), rels[0])


def _installed_size(p):
    return sum(os.path.getsize(s) for s, _, _ in p.files if not os.path.islink(s))


# --- the folder ---------------------------------------------------------------------

def _icon_png(src, dest):
    from PIL import Image
    im = Image.open(src).convert('RGBA')
    if im.size != (48, 48):                 # square it up, then scale to 48
        side = max(im.size)
        sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
        sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
        im = sq.resize((48, 48), Image.LANCZOS)
    im.save(dest, optimize=True)


def check_built():
    """Refuse a binary older than any of its sources: rebuild it in the chroot first."""
    for binary, sources in ((BUILT / 'psionlx-software', ['psionlx-software.c', 'psionnet-discover.h']),
                            (BUILT / 'psionnet-find', ['psionnet-find.c', 'psionnet-discover.h']),
                            (BUILT / 'psionnet-spotify', [SPOTIFY / 'psionnet-spotify.c', SPOTIFY / 'icons.h'])):
        for src in sources:
            src = SOFTWARE / src if isinstance(src, str) else src
            if os.path.getmtime(src) > os.path.getmtime(binary):
                raise SystemExit(f'{binary} is older than {src}: rebuild it (build/job-*.sh)')


def build(out=OUT):
    pkgs = packages()
    for p in pkgs:
        for src, rel, _ in p.files:
            if not os.path.lexists(src):
                raise SystemExit(f'{p.name}: missing {src}')
    check_built()
    resolve(pkgs)
    if out.exists():
        shutil.rmtree(out)
    (out / 'icons').mkdir(parents=True)
    index, cat = [], ['PSIONLX-SOFTWARE 1']
    for p in pkgs:
        data = build_ipk(p)
        fn = f'{p.name}_{p.version}_{p.arch}.ipk'
        (out / fn).write_bytes(data)
        index.append(control_text(p) + f'Filename: {fn}\nSize: {len(data)}\n'
                     f'MD5Sum: {hashlib.md5(data).hexdigest()}\n')
        fields = _desktop(p)
        icon = p.icon or _icon_source(p, fields)
        if icon:
            _icon_png(icon, out / 'icons' / f'{p.name}.png')
        execute = p.exec or fields.get('Exec', '')
        cat.append('\t'.join([p.name, p.title, p.category, p.version, str(len(data)),
                              f'icons/{p.name}.png' if icon else '', execute, p.summary,
                              p.description.replace('\n', '\\n'), witness(p)]))
        print(f'  {fn:52} {len(data):>9,}  needs: {", ".join(p.depends) or "-"}')
    (out / 'Packages').write_text('\n'.join(index))
    (out / 'catalogue.txt').write_text('\n'.join(cat) + '\n', encoding='utf-8')
    for doc in ('README.txt', 'install.sh'):
        shutil.copy2(FEEDDOCS / doc, out / doc)
    shutil.copy2(FEEDDOCS / 'icons-README.txt', out / 'icons' / 'README.txt')
    sums = []
    for f in sorted(out.rglob('*')):
        if f.is_file() and f.name != 'SHA256SUMS.txt':
            sums.append(f'{hashlib.sha256(f.read_bytes()).hexdigest()}  ./{f.relative_to(out)}')
    (out / 'SHA256SUMS.txt').write_text('\n'.join(sums) + '\n')
    print(f'{len(pkgs)} packages -> {out}')
    return pkgs


# --- for merge-soho.py: install and register in a tree ------------------------------

def install_files(p, root):
    for src, rel, mode in p.files:
        dest = Path(root) / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if os.path.lexists(dest):
            dest.unlink()
        if os.path.islink(src):
            os.symlink(os.readlink(src), dest)
        else:
            shutil.copy2(src, dest)
            if mode is not None:
                os.chmod(dest, mode)


def register(pkgs, root):
    """Tell the tree's ipkg that these packages are installed, as ipkg itself would."""
    info = Path(root) / 'usr/lib/ipkg/info'
    status = Path(root) / 'usr/lib/ipkg/status'
    text = status.read_text()
    blocks = [b for b in text.split('\n\n') if b.strip()]
    names = {p.name for p in pkgs}
    kept = [b for b in blocks if not any(l == f'Package: {n}' for n in names for l in b.split('\n'))]
    for p in pkgs:
        (info / f'{p.name}.control').write_text(control_text(p))
        listing = [f'/{rel}' for _, rel, _ in p.files]
        if p.name == 'agfa-fonts':
            listing.append('/usr/share/fonts/ttf/.psionlx-pin')
            (Path(root) / 'usr/share/fonts/ttf/.psionlx-pin').write_text(FONT_PIN)
        (info / f'{p.name}.list').write_text('\n'.join(listing) + '\n')
        for script in ('postinst', 'prerm', 'postrm'):
            body = getattr(p, script)
            f = info / f'{p.name}.{script}'
            if body:
                f.write_text('#!/bin/sh\n' + body + 'exit 0\n')
                os.chmod(f, 0o755)
        block = [f'Package: {p.name}', f'Version: {p.version}']
        if p.depends:
            block.append('Depends: ' + ', '.join(p.depends))
        block += ['Status: install ok installed', f'Architecture: {p.arch}']
        kept.append('\n'.join(block))
    status.write_text('\n\n'.join(kept) + '\n\n')


if __name__ == '__main__':
    build()
