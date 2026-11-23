#!/usr/bin/env python3
"""make-v4-package.py: build an OpenLara package for the Apollo Vampire V4 from your own Tomb Raider (PC) game.

It copies the game's DATA and FMV folders, turns the CD music into the 22 kHz 16-bit AIFF tracks the V4
plays from RAM, adds OpenLara (OpenLaraHW, the 4:3 and 16:9 launchers, icons, a ReadMe) and writes:

    OUT/OpenLaraMain/     the drawer, ready to copy over as it is
    OUT/OpenLaraV4.lha    the same as an LHA archive (stored, readable by any LhA)
    OUT/OpenLaraV4.run    a self-extracting Amiga program: copy it to an SD card, run it on the Vampire

Sources (pick one):
    --cue GAME.CUE              a CD image (your own rip, or the GOG / Steam release's cue + bin)
    --cd DIR [--audio DIR]      a CD or game folder with DATA/ and FMV/, music as WAV/FLAC/Ogg/MP3 files
    --drive /dev/sr0            the CD in a drive (Linux; music read with cdparanoia)

Python 3.6+ only. ffmpeg (or numpy) is used for the music when present; FLAC/Ogg/MP3 music needs ffmpeg.
Only for your own copy of the game: the package holds the game's files.
"""
import argparse, array, calendar, glob, os, re, shutil, struct, subprocess, sys, tempfile, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, 'frozen', False)       # the Windows .exe (PyInstaller): the Amiga files are bundled
AMIGA = os.path.join(sys._MEIPASS, 'amiga') if FROZEN else os.path.join(HERE, '..', 'src', 'platform', 'amiga')
EXEDIR = os.path.dirname(os.path.abspath(sys.executable)) if FROZEN else HERE
RATE = 22050


def die(msg):
    sys.exit('error: ' + msg)


def say(msg):
    print(msg, flush=True)


def find_ci(folder, name):
    """name in folder, ignoring case (CD and GOG file names come in either)"""
    if not os.path.isdir(folder):
        return None
    for f in os.listdir(folder):
        if f.lower() == name.lower():
            return os.path.join(folder, f)
    return None


# ---------------------------------------------------------------- CD image: cue sheet and sectors

class Track:
    def __init__(self, number, mode, file, ftype):
        self.number, self.mode, self.file, self.ftype = number, mode, file, ftype
        self.index = {}                      # 0 / 1 -> frame within the file (75 a second)


def msf(s):
    m, sec, f = (int(x) for x in s.split(':'))
    return (m * 60 + sec) * 75 + f


def parse_cue(path):
    base = os.path.dirname(os.path.abspath(path))
    tracks, cur_file, cur_type = [], None, None
    with open(path, 'r', errors='replace') as f:
        for line in f:
            w = line.strip()
            m = re.match(r'FILE\s+"(.*)"\s+(\S+)', w, re.I) or re.match(r'FILE\s+(\S+)\s+(\S+)', w, re.I)
            if m:
                name = m.group(1)
                p = os.path.join(base, name)
                if not os.path.exists(p):
                    p = find_ci(base, os.path.basename(name)) or p
                if not os.path.exists(p):
                    die('the cue sheet names %s, which is not there' % name)
                cur_file, cur_type = p, m.group(2).upper()
                continue
            m = re.match(r'TRACK\s+(\d+)\s+(\S+)', w, re.I)
            if m:
                tracks.append(Track(int(m.group(1)), m.group(2).upper(), cur_file, cur_type))
                continue
            m = re.match(r'INDEX\s+(\d+)\s+(\d+:\d+:\d+)', w, re.I)
            if m and tracks:
                tracks[-1].index[int(m.group(1))] = msf(m.group(2))
    if not tracks:
        die('no tracks in %s' % path)
    return tracks


class SectorReader:
    """2048-byte user data sectors from a data track (MODE1/2048, MODE1/2352, MODE2/2352) or a device"""
    def __init__(self, path, mode='MODE1/2048', start=0):
        if os.name == 'nt' and re.match(r'^[A-Za-z]:?$', path):          # a drive letter: the raw CD
            path = r'\\.\%s:' % path[0]
        self.f = open(path, 'rb', buffering=0)
        self.size = 2352 if mode.endswith('2352') else 2048
        self.skip = 0 if self.size == 2048 else (24 if mode.startswith('MODE2') else 16)
        self.start = start                   # first sector of the track in the file

    def read(self, lba, count=1):
        out = bytearray()
        for i in range(count):
            self.f.seek((self.start + lba + i) * self.size + self.skip)
            out += self.f.read(2048)
        return bytes(out)


class Iso9660:
    def __init__(self, sectors):
        self.s = sectors
        pvd = sectors.read(16)
        if pvd[1:6] != b'CD001':
            die('the data track is not an ISO 9660 filesystem')
        self.root = self._record(pvd[156:156 + 34])

    @staticmethod
    def _record(r):
        n = r[32]
        name = r[33:33 + n].decode('latin1')
        return {'lba': struct.unpack('<I', r[2:6])[0], 'size': struct.unpack('<I', r[10:14])[0],
                'dir': bool(r[25] & 2), 'name': name.split(';')[0].rstrip('.')}

    def listdir(self, d):
        data = self.s.read(d['lba'], (d['size'] + 2047) // 2048)
        out, p = [], 0
        while p < d['size']:
            n = data[p]
            if n == 0:                       # records don't cross sector ends
                p = (p // 2048 + 1) * 2048
                continue
            r = self._record(data[p:p + n])
            if r['name'] not in ('\x00', '\x01'):
                out.append(r)
            p += n
        return out

    def find(self, name, d=None):
        for r in self.listdir(d or self.root):
            if r['name'].lower() == name.lower():
                return r
        return None

    def extract_dir(self, d, dest):
        os.makedirs(dest, exist_ok=True)
        n = 0
        for r in self.listdir(d):
            if r['dir']:
                continue
            with open(os.path.join(dest, r['name']), 'wb') as f:
                left, lba = r['size'], r['lba']
                while left > 0:
                    chunk = min(left, 2048 * 512)
                    f.write(self.s.read(lba, (chunk + 2047) // 2048)[:chunk])
                    left -= chunk
                    lba += chunk // 2048
            n += 1
        return n


# ---------------------------------------------------------------- music -> 22 kHz AIFF

def tool_path(tool):
    """a tool on the PATH, or next to the .exe / this script (ffmpeg.exe dropped beside it on Windows)"""
    for d in (EXEDIR, os.getcwd()):
        for name in (tool + '.exe', tool):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return p
    return shutil.which(tool)


def have(tool):
    return tool_path(tool) is not None


def ext80(rate):
    """IEEE 754 80-bit extended, as AIFF's COMM chunk stores the sample rate"""
    e, m = 16383 + 63, int(rate)
    while not m & (1 << 63):
        m <<= 1
        e -= 1
    return struct.pack('>HQ', e, m)


def write_aiff(path, pcm_be, channels=2, rate=RATE):
    frames = len(pcm_be) // (2 * channels)
    comm = struct.pack('>hIh', channels, frames, 16) + ext80(rate)
    ssnd = struct.pack('>II', 0, 0) + pcm_be
    body = b'AIFF' + b'COMM' + struct.pack('>I', len(comm)) + comm + b'SSND' + struct.pack('>I', len(ssnd)) + ssnd
    with open(path, 'wb') as f:
        f.write(b'FORM' + struct.pack('>I', len(body)) + body)


def to_22k_be(pcm_le_44k_stereo):
    """44.1 kHz 16-bit stereo little-endian -> 22.05 kHz big-endian"""
    try:
        import numpy as np
        a = np.frombuffer(pcm_le_44k_stereo[:len(pcm_le_44k_stereo) // 8 * 8], dtype='<i2').reshape(-1, 2, 2)
        b = ((a[:, 0, :].astype(np.int32) + a[:, 1, :]) >> 1).astype('>i2')     # average pairs: a simple low-pass
        return b.tobytes()
    except ImportError:
        a = array.array('h')
        a.frombytes(pcm_le_44k_stereo[:len(pcm_le_44k_stereo) // 8 * 8])
        if sys.byteorder == 'big':
            a.byteswap()
        b = array.array('h')
        b.extend(a[0::4]); c = array.array('h'); c.extend(a[1::4])   # every other frame, no filter
        out = array.array('h', bytes(len(b) * 4))
        out[0::2], out[1::2] = b, c
        if sys.byteorder == 'little':
            out.byteswap()
        return out.tobytes()


def ffmpeg_to_aiff(args_in, dest, stdin_bytes=None):
    cmd = [tool_path('ffmpeg'), '-nostdin', '-loglevel', 'error', '-y'] + args_in + ['-ar', str(RATE), '-ac', '2',
           '-c:a', 'pcm_s16be', '-f', 'aiff', dest]
    if stdin_bytes is not None:
        cmd.remove('-nostdin')
    subprocess.run(cmd, input=stdin_bytes, check=True)


def raw_cdda_to_aiff(pcm_le, dest):
    if have('ffmpeg'):
        ffmpeg_to_aiff(['-f', 's16le', '-ar', '44100', '-ac', '2', '-i', 'pipe:0'], dest, stdin_bytes=pcm_le)
    else:
        write_aiff(dest, to_22k_be(pcm_le))


def file_to_aiff(src, dest):
    if have('ffmpeg'):
        ffmpeg_to_aiff(['-i', src], dest)
        return
    if not src.lower().endswith('.wav'):
        die('%s: FLAC/Ogg/MP3 music needs ffmpeg (or convert it to WAV first)' % src)
    with wave.open(src, 'rb') as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 2 or w.getframerate() != 44100:
            die('%s: without ffmpeg only 44.1 kHz 16-bit stereo WAV can be converted' % src)
        write_aiff(dest, to_22k_be(w.readframes(w.getnframes())))


# ---------------------------------------------------------------- the sources

def from_cue(cue, out):
    tracks = parse_cue(cue)
    data = [t for t in tracks if t.mode != 'AUDIO']
    if not data:
        die('no data track in the cue sheet')
    t = data[0]
    say('data track %d (%s) in %s' % (t.number, t.mode, os.path.basename(t.file)))
    iso = Iso9660(SectorReader(t.file, t.mode, t.index.get(1, 0)))
    copy_game_dirs(iso, out)
    audio = [x for x in tracks if x.mode == 'AUDIO']
    say('%d music tracks' % len(audio))
    for i, a in enumerate(audio):
        dest = os.path.join(out, 'audio', '1', 'track_%02d.aiff' % a.number)
        if a.ftype in ('BINARY', 'MOTOROLA'):
            start = a.index.get(1, a.index.get(0, 0)) * 2352
            end = None
            nxt = tracks[tracks.index(a) + 1] if tracks.index(a) + 1 < len(tracks) else None
            if nxt and nxt.file == a.file:
                end = nxt.index.get(0, nxt.index.get(1)) * 2352
            with open(a.file, 'rb') as f:
                f.seek(start)
                pcm = f.read() if end is None else f.read(end - start)
            if a.ftype == 'MOTOROLA':
                b = array.array('h'); b.frombytes(pcm[:len(pcm) // 2 * 2]); b.byteswap(); pcm = b.tobytes()
            raw_cdda_to_aiff(pcm, dest)
        else:
            file_to_aiff(a.file, dest)
        say('  track %02d (%d/%d)' % (a.number, i + 1, len(audio)))


def copy_game_dirs(iso_or_dir, out):
    for name in ('DATA', 'FMV'):
        dest = os.path.join(out, name)
        if isinstance(iso_or_dir, Iso9660):
            d = iso_or_dir.find(name)
            if not d:
                if name == 'DATA':
                    die('no DATA folder on the CD: is this the PC (DOS/Windows) Tomb Raider?')
                say('no FMV folder: the game runs without its movies')
                continue
            n = iso_or_dir.extract_dir(d, dest)
        else:
            src = find_ci(iso_or_dir, name)
            if not src:
                if name == 'DATA':
                    die('no DATA folder in %s' % iso_or_dir)
                say('no FMV folder: the game runs without its movies')
                continue
            os.makedirs(dest, exist_ok=True)
            n = 0
            for f in sorted(os.listdir(src)):
                if os.path.isfile(os.path.join(src, f)):
                    shutil.copy2(os.path.join(src, f), os.path.join(dest, f.upper()))
                    n += 1
        say('%s: %d files' % (name, n))


def from_folder(cd, audio_dir, out):
    copy_game_dirs(cd, out)
    if not audio_dir:
        say('no --audio folder: the game runs without music')
        return
    files = []
    for f in sorted(os.listdir(audio_dir)):
        m = re.search(r'(\d+)', f)
        if m and f.lower().rsplit('.', 1)[-1] in ('wav', 'flac', 'ogg', 'mp3', 'aiff', 'aif'):
            files.append((int(m.group(1)), os.path.join(audio_dir, f)))
    if not files:
        die('no numbered music files (track02.wav, 02.flac, ...) in %s' % audio_dir)
    # rips number the music from 1 when they leave out the data track; the game counts CD tracks (2..)
    shift = 1 if min(n for n, _ in files) == 1 else 0
    if shift:
        say('music numbered from 1: taken as CD tracks from 2')
    for i, (n, src) in enumerate(files):
        file_to_aiff(src, os.path.join(out, 'audio', '1', 'track_%02d.aiff' % (n + shift)))
        say('  track %02d (%d/%d)' % (n + shift, i + 1, len(files)))


def from_drive(dev, out):
    copy_game_dirs(Iso9660(SectorReader(dev)), out)
    if not have('cdparanoia'):
        say('the music is not read from a drive without cdparanoia: rip the CD\'s audio tracks with any\n'
            'CD ripper (WAV or FLAC, named by track number) and use --cd %s --audio <that folder> instead,\n'
            'or make a cue/bin image of the CD and use --cue' % dev)
        return
    with tempfile.TemporaryDirectory() as tmp:
        say('reading the music with cdparanoia (this takes a while)')
        subprocess.run(['cdparanoia', '-d', dev, '-B'], cwd=tmp, check=True)
        wavs = sorted(glob.glob(os.path.join(tmp, 'track*.cdda.wav')))
        for i, w in enumerate(wavs):
            n = int(re.search(r'track(\d+)', os.path.basename(w)).group(1))
            file_to_aiff(w, os.path.join(out, 'audio', '1', 'track_%02d.aiff' % n))
            say('  track %02d (%d/%d)' % (n, i + 1, len(wavs)))


# ---------------------------------------------------------------- LHA (level 2, stored) and the self-extractor

def crc16(data, crc=0):
    tab = crc16.tab
    for b in data:
        crc = tab[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


crc16.tab = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (_c >> 1) ^ 0xA001 if _c & 1 else _c >> 1
    crc16.tab.append(_c)


def file_crc16(path):
    crc = 0
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                return crc
            crc = crc16(b, crc)


def lha_header(method, size, mtime, crc, dirparts, fname):
    ext = b''
    if fname is not None:
        n = fname.encode('latin1')
        ext += struct.pack('<H', len(n) + 3) + b'\x01' + n
    if dirparts:
        d = b''.join(p.encode('latin1') + b'\xff' for p in dirparts)
        ext += struct.pack('<H', len(d) + 3) + b'\x02' + d
    ext += struct.pack('<H', 0)
    # the first extended header's size field sits in the base header (bytes 24-25)
    # the Amiga's clock runs on local time (no time zone): store the PC's local wall-clock time
    local = calendar.timegm(time.localtime(mtime))
    base = method + struct.pack('<IIIBBHB', size, size, local, 0x20, 2, crc, ord('A'))
    h = base + ext
    total = 2 + len(h)
    if total & 0xFF == 0:                    # a size ending in 00 would read as the archive's end
        h += b'\x00'
        total += 1
    return struct.pack('<H', total) + h


def write_lha(root, top, dest):
    """root/top (a folder) and root/top.info into dest, directories first"""
    entries = []
    for d, subdirs, files in os.walk(os.path.join(root, top)):
        subdirs.sort()
        rel = os.path.relpath(d, root).split(os.sep)
        entries.append(('dir', rel, None, d))
        for f in sorted(files):
            entries.append(('file', rel, f, os.path.join(d, f)))
    info = os.path.join(root, top + '.info')
    if os.path.exists(info):
        entries.append(('file', [], top + '.info', info))
    with open(dest, 'wb') as out:
        for kind, rel, name, path in entries:
            st = os.stat(path)
            if kind == 'dir':
                out.write(lha_header(b'-lhd-', 0, st.st_mtime, 0, rel, None))
                continue
            crc = file_crc16(path)
            out.write(lha_header(b'-lh0-', st.st_size, st.st_mtime, crc, rel, name))
            with open(path, 'rb') as f:
                shutil.copyfileobj(f, out, 1 << 20)
        out.write(b'\x00')


def write_sfx(stub, lha, dest):
    with open(dest, 'wb') as out:
        with open(stub, 'rb') as f:
            shutil.copyfileobj(f, out)
        # An (empty) overlay table: the stub starts with the overlay header (installer/ovlhead.s), so
        # AmigaDOS's loader stops reading at HUNK_OVERLAY - the archive after it is neither loaded nor read
        # through when the program starts (as a skipped HUNK_DEBUG would be) - and closes the file again
        # in UnLoadSeg. Table: its size N, then N + 1 longwords.
        out.write(struct.pack('>III', 0x3F5, 0, 0))
        off = out.tell()
        with open(lha, 'rb') as f:
            shutil.copyfileobj(f, out, 1 << 20)
        length = out.tell() - off
        out.write(b'OLSFX001' + struct.pack('>II', off, length))
    os.chmod(dest, 0o755)


# ---------------------------------------------------------------- OpenLara's own files

README = """OpenLara for the Apollo Vampire V4
==================================

Double-click "OpenLara 4-3" (640x480) or "OpenLara 16-9" (640x360, widescreen).
F10 quits. Esc is the inventory; the V4's USB joypad works too.

From the Shell: SetEnv OLRES 640x480 (any 16-bit screen mode of that size), then OpenLaraHW.
More switches are listed in OpenLara's source: src/platform/amiga/README.md.

This drawer holds your own Tomb Raider files (DATA, FMV, audio). They are for your own use only.
OpenLara: https://github.com/XProger/OpenLara (BSD 2-clause), V4 port: https://github.com/slaapliedje/OpenLaraV4
"""


def add_openlara(out, bindir):
    def need(rel):
        p = os.path.join(bindir, rel)
        if not os.path.exists(p):
            die('%s is missing: build OpenLara first (src/platform/amiga/build.sh hw, launcher, installer)' % p)
        return p
    shutil.copy2(need('OpenLaraHW'), out)
    for name in ('OpenLara 4-3', 'OpenLara 16-9'):
        shutil.copy2(need(os.path.join('launcher', 'OpenLaraLaunch')), os.path.join(out, name))
        shutil.copy2(need(os.path.join('installer', 'icons', 'OpenLara.info')), os.path.join(out, name + '.info'))
    shutil.copy2(need(os.path.join('installer', 'icons', 'Drawer.info')), out + '.info')
    with open(os.path.join(out, 'ReadMe.txt'), 'w', newline='\n') as f:   # LF: Amiga text
        f.write(README)


def ask_gui():
    """double-clicked without arguments: file pickers for the source and the output folder"""
    try:
        import tkinter
        from tkinter import filedialog, messagebox
    except ImportError:
        return None
    root = tkinter.Tk()
    root.withdraw()
    messagebox.showinfo('OpenLara V4 package', 'Pick your Tomb Raider CD image (.cue).\n\n'
                        'No image? Cancel, then pick the game or CD folder that holds DATA and FMV.')
    args = []
    cue = filedialog.askopenfilename(title='Tomb Raider CD image (.cue)', filetypes=[('CD image', '*.cue *.CUE'), ('All files', '*.*')])
    if cue:
        args += ['--cue', cue]
    else:
        cd = filedialog.askdirectory(title='Folder with DATA and FMV (a CD or the installed game)')
        if not cd:
            return []
        args += ['--cd', cd]
        audio = filedialog.askdirectory(title='Music files folder (track02.wav, ...) - Cancel for none')
        if audio:
            args += ['--audio', audio]
    out = filedialog.askdirectory(title='Where to write the package')
    if not out:
        return []
    args += ['--out', os.path.join(out, 'OpenLaraV4-package')]
    root.destroy()
    return args


def main():
    if len(sys.argv) == 1 and FROZEN:
        gui = ask_gui()
        if gui == []:
            return
        if gui:
            sys.argv += gui
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--cue', help='CD image cue sheet (with its .bin / track files beside it)')
    src.add_argument('--cd', help='folder with DATA/ and FMV/ (a mounted CD or the installed game)')
    src.add_argument('--drive', help='CD drive device, e.g. /dev/sr0 (Linux, needs cdparanoia)')
    ap.add_argument('--audio', help='with --cd: folder of music files numbered by CD track')
    ap.add_argument('--out', default='OpenLaraV4-package', help='output folder (default %(default)s)')
    ap.add_argument('--bin', default=AMIGA, help='folder with the built Amiga files (default: the source tree)')
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    drawer = os.path.join(out, 'OpenLaraMain')
    if os.path.exists(drawer):
        die('%s already exists: remove it or pick another --out' % drawer)
    os.makedirs(os.path.join(drawer, 'audio', '1'))
    say('music conversion: %s' % ('ffmpeg' if have('ffmpeg') else 'built in (install ffmpeg for the best quality)'))
    t0 = time.time()
    if a.cue:
        from_cue(a.cue, drawer)
    elif a.cd:
        from_folder(a.cd, a.audio, drawer)
    else:
        from_drive(a.drive, drawer)
    for must in ('TITLE.PHD', 'GYM.PHD', 'LEVEL1.PHD'):
        if not find_ci(os.path.join(drawer, 'DATA'), must):
            say('warning: DATA/%s is missing: is this Tomb Raider 1 (PC)?' % must)
    add_openlara(drawer, a.bin)
    say('packing OpenLaraV4.lha')
    lha = os.path.join(out, 'OpenLaraV4.lha')
    write_lha(out, 'OpenLaraMain', lha)
    run = os.path.join(out, 'OpenLaraV4.run')
    write_sfx(os.path.join(a.bin, 'installer', 'sfxstub'), lha, run)
    say('done in %d s:\n  %s\n  %s (%d MB)\n  %s (%d MB): copy it to an SD card and run it on the Vampire'
        % (time.time() - t0, drawer, lha, os.path.getsize(lha) >> 20, run, os.path.getsize(run) >> 20))


if __name__ == '__main__':
    double_clicked = FROZEN and len(sys.argv) == 1
    try:
        main()
    except SystemExit as e:
        if e.code not in (None, 0):
            print(e.code if isinstance(e.code, str) else 'stopped', file=sys.stderr)
    except Exception as e:
        print('error: %s' % e, file=sys.stderr)
    if double_clicked:
        input('\nPress Enter to close this window.')
