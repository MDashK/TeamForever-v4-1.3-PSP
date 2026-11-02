"""Builds Data.psp, the PSP port's data pack, from your own Data.rsdk and the Sonic 1 Forever / Sonic 2 Absolute files.

Data.psp is an RSDKv4 pack ('RSDKvB', the layout the engine already reads, see rsdkv4.py) holding everything the game
loads, prepared so the PSP does as little work as possible:
  - the Data.rsdk files and the mod's files (Data/, Scripts/, Bytecode/) merged into one pack, the mod's files
    replacing the game's: no mods folder to scan on the Memory Stick, nothing to decrypt;
  - music as ATRAC3 instead of Ogg Vorbis: the PSP's Media Engine decodes it while it streams from the pack (no Vorbis
    decoding on the main CPU, no whole track in RAM). Stored under the .ogg path, the engine tells them apart by content;
  - sound effects as mono 16-bit 44.1kHz WAV, the way the engine plays them (nothing to convert when a stage loads);
  - the mod's mod.ini as PSP/mod.ini (its settings: text scripts, skip the start menu...).

The output contains your game's data: it is for your own use, not for sharing.

usage: python build_psp_data.py <Data.rsdk> [--out DIR] [--mod DIR] [--eboot FILE] [--atracdenc PATH] [--ffmpeg PATH]
                                           [--cache DIR] [--jobs N]
  The game is told by the Data.rsdk's MD5 (Sonic 1 -> Sonic 1 Forever, Sonic 2 -> Sonic 2 Absolute), the mod's files
  come from patch/<mod>/, and out/PSP/GAME/<game>/ gets Data.psp and the EBOOT.PBP (from eboot/<S1F|S2A>/).
needs: ffmpeg, and atracdenc 0.2.3 (https://github.com/dcherednik/atracdenc) for the music
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsdkv4  # noqa: E402

ALIGN = 64            # file data starts on 64 byte boundaries (Memory Stick reads)
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
# the Data.rsdk each mod is made for (the 2013 Android remasters): the game, its files in patch/, its folder on the PSP
GAMES = {
    'ad10f658daf0f09b367ad7f23e35ff57': ('S1F', 'Sonic 1 (Android v3.0.0 - v3.3.0_57)', 'Sonic 1 Forever', 'Sonic1Forever'),
    '6f1244f8d6e3822778f69bce2d1e8558': ('S2A', 'Sonic 2 (Android)', 'Sonic 2 Absolute', 'Sonic2Absolute'),
}
MAX_FILES = 0x1000    # RETRO_PACKFILE_COUNT: the engine's file table
SFX_RATE = 44100      # the engine's output rate (Audio.cpp AUDIO_FREQUENCY)


def log(msg):
    print(msg, flush=True)


def tool(path, name):
    found = path or shutil.which(name)
    if not found or not os.path.exists(found):
        raise SystemExit(f'{name} not found (put it on the PATH or pass --{name})')
    return os.path.abspath(found)


def wav_format(data):
    """(channels, rate, bits, format tag) of a RIFF WAVE, or None."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return None
    pos = 12
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if cid == b'fmt ':
            tag, ch, rate, _, _, bits = struct.unpack_from('<HHIIHH', data, pos + 8)
            return ch, rate, bits, tag
        pos += 8 + size + (size & 1)
    return None


class Converter:
    def __init__(self, ffmpeg, atracdenc, cache):
        self.ffmpeg = ffmpeg
        self.atracdenc = atracdenc
        self.cache = cache
        if cache:
            os.makedirs(cache, exist_ok=True)

    def _cached(self, kind, data, make):
        key = hashlib.sha1(data).hexdigest()
        path = os.path.join(self.cache, f'{key}.{kind}') if self.cache else None
        if path and os.path.exists(path):
            with open(path, 'rb') as f:
                return f.read()
        out = make(data)
        if path and out:
            with open(path + '.tmp', 'wb') as f:
                f.write(out)
            os.replace(path + '.tmp', path)
        return out

    def _ffmpeg_wav(self, data, tmp, args):
        src = os.path.join(tmp, 'in')
        wav = os.path.join(tmp, 'out.wav')
        with open(src, 'wb') as f:
            f.write(data)
        subprocess.run([self.ffmpeg, '-v', 'error', '-y', '-i', src] + args + ['-c:a', 'pcm_s16le', wav], check=True)
        return wav

    def music(self, data):
        """An OGG track as ATRAC3 (44.1kHz stereo, what the PSP's decoder plays). atracdenc's file is kept as it is:
        rewriting its RIFF header made the PSP's decoder reject the files (Sonic Mania PSP)."""
        def make(d):
            with tempfile.TemporaryDirectory() as tmp:
                wav = self._ffmpeg_wav(d, tmp, ['-ar', '44100', '-ac', '2'])
                at3 = os.path.join(tmp, 'out.at3')
                subprocess.run([self.atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True,
                               stdout=subprocess.DEVNULL)
                with open(at3, 'rb') as f:
                    return f.read()
        return self._cached('at3', data, make)

    def sfx(self, data):
        """A sound effect as mono 16-bit WAV at the engine's output rate (kept as it is when it already is one)."""
        fmt = wav_format(data)
        if fmt and fmt[:4] == (1, SFX_RATE, 16, 1):
            return data

        def make(d):
            with tempfile.TemporaryDirectory() as tmp:
                wav = self._ffmpeg_wav(d, tmp, ['-ar', str(SFX_RATE), '-ac', '1'])
                with open(wav, 'rb') as f:
                    return f.read()
        return self._cached('wav', data, make)


def read_mod(folder):
    """The mod's files as {pack path: bytes}: Data/..., Scripts/..., Bytecode/..., PSP/... (the port's own: the scripts
    compiled ahead of time, PSP/ScriptCache), and mod.ini as PSP/mod.ini."""
    files = {}
    for top in ('Data', 'Scripts', 'Bytecode', 'PSP'):
        base = None
        for name in os.listdir(folder):
            if name.lower() == top.lower() and os.path.isdir(os.path.join(folder, name)):
                base = os.path.join(folder, name)
        if not base:
            continue
        for d, _, fs in os.walk(base):
            for f in fs:
                p = os.path.join(d, f)
                rel = os.path.relpath(p, base).replace(os.sep, '/')
                with open(p, 'rb') as h:
                    files[f'{top}/{rel}'] = h.read()
    ini = os.path.join(folder, 'mod.ini')
    if os.path.exists(ini):
        with open(ini, 'rb') as h:
            files['PSP/mod.ini'] = h.read()
    return files


def audio_names(all_data):
    """Every file name ending in .ogg/.wav mentioned in the scripts and configs: the candidates for Data/Music/<name>
    and Data/SoundFX/<name> (the pack only has hashes, so this is how tracks are told from sound effects)."""
    names = set()
    pattern = re.compile(rb'[A-Za-z0-9_\-./ ]+\.(?:ogg|wav)', re.I)
    for data in all_data:
        if len(data) > 4 * 1024 * 1024 or data[:4] in (b'OggS', b'RIFF') or data[:3] == b'GIF':
            continue
        for m in pattern.findall(data):
            names.add(m.decode('latin-1').strip().lstrip('/'))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rsdk')
    ap.add_argument('--mod', help="the mod's folder (default: its folder in patch/, by the game the Data.rsdk is)")
    ap.add_argument('--out', help='where PSP/GAME/<game>/ is made (default: out/ next to tools/), or a .psp file')
    ap.add_argument('--eboot', help='the EBOOT.PBP to put with it (default: eboot/<S1F|S2A>/EBOOT.PBP if there)')
    ap.add_argument('--atracdenc')
    ap.add_argument('--ffmpeg')
    ap.add_argument('--cache', help='keeps converted music/sfx here, so building again is quick')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    ffmpeg = tool(args.ffmpeg, 'ffmpeg')
    atracdenc = tool(args.atracdenc, 'atracdenc')
    conv = Converter(ffmpeg, atracdenc, args.cache)

    log(f'reading {args.rsdk}')
    pack = rsdkv4.Pack(args.rsdk)
    md5 = hashlib.md5(pack.data).hexdigest()
    game = GAMES.get(md5)
    if game:
        log(f'  {game[1]}: making {game[2]}')
    elif not args.mod:
        raise SystemExit(f'unknown Data.rsdk (MD5 {md5}). Needed: Sonic 1 or Sonic 2 from the 2013 Android versions:\n'
                         + '\n'.join(f'  {k}  {v[1]}' for k, v in GAMES.items()))
    else:
        log(f'  WARNING: unknown Data.rsdk (MD5 {md5}): the mods are made for the Android 2013 versions, it may not work')
    if not args.mod:
        args.mod = os.path.join(ROOT, 'patch', game[2])
        if not os.path.isdir(args.mod):
            raise SystemExit(f"the mod's files are missing: {os.path.abspath(args.mod)}")

    # where it goes: <out>/PSP/GAME/<folder>/Data.psp (copy <out>/PSP to the Memory Stick), or the .psp file asked for
    out_dir = None
    if args.out and args.out.lower().endswith('.psp'):
        out_file = args.out
    else:
        folder = game[3] if game else 'RSDKv4'
        out_dir = os.path.join(args.out or os.path.join(ROOT, 'out'), 'PSP', 'GAME', folder)
        os.makedirs(out_dir, exist_ok=True)
        out_file = os.path.join(out_dir, 'Data.psp')
    args.out = out_file
    entries = {}  # hash -> [path or None, bytes]
    for e in pack.files:
        entries[e[0]] = [None, pack.read(e)]
    log(f'  {len(entries)} files')

    log(f'reading the mod: {args.mod}')
    mod = read_mod(args.mod)
    replaced = 0
    for path, data in mod.items():
        h = rsdkv4.path_hash(path)
        replaced += h in entries
        entries[h] = [path, data]
    log(f'  {len(mod)} files ({replaced} replace the game\'s)')
    if 'PSP/mod.ini' not in mod:
        # (Sonic 2 Absolute's files come without one) what both mods' mod.ini set
        log('  no mod.ini in the mod folder: using text scripts and skipping the start menu, like the mods do')
        default_ini = b'TxtScripts=true\r\nSkipStartMenu=true\r\nDisableFocusPause=3\r\n'
        entries[rsdkv4.path_hash('PSP/mod.ini')] = ['PSP/mod.ini', default_ini]

    # tell music from sound effects by the names the scripts and configs use
    names = audio_names(d for _, d in entries.values())
    music_hashes = {rsdkv4.path_hash('Data/Music/' + n) for n in names}
    sfx_hashes = {rsdkv4.path_hash('Data/SoundFX/' + n) for n in names}

    music, sfx, kept_ogg = [], [], []
    for h, (path, data) in entries.items():
        # (a mod file's path says it outright: the menus' tracks are named in the engine's code, not in the scripts)
        folder = path.lower().split('/')[:2] if path else []
        if data[:4] == b'OggS' and (h in music_hashes or folder == ['data', 'music']):
            music.append(h)
        elif data[:4] == b'RIFF' or (data[:4] == b'OggS' and (h in sfx_hashes or folder == ['data', 'soundfx'])):
            sfx.append(h)
        elif data[:4] == b'OggS':
            kept_ogg.append(path or h.hex())
    log(f'music: {len(music)} tracks to ATRAC3, sound effects: {len(sfx)}')
    if kept_ogg:
        log(f'  WARNING: {len(kept_ogg)} OGG file(s) not named as music or sfx anywhere, kept as they are: '
            + ', '.join(kept_ogg[:6]))

    def do_music(h):
        entries[h][1] = conv.music(entries[h][1])
        return h

    def do_sfx(h):
        entries[h][1] = conv.sfx(entries[h][1])
        return h

    with ThreadPoolExecutor(args.jobs) as pool:
        for i, _ in enumerate(pool.map(do_music, music), 1):
            if i % 10 == 0 or i == len(music):
                log(f'  music {i}/{len(music)}')
        list(pool.map(do_sfx, sfx))
    log('  sound effects done')

    if len(entries) > MAX_FILES:
        raise SystemExit(f'too many files for the engine: {len(entries)} (most {MAX_FILES})')

    # write the pack: header, file table, then the files (aligned)
    order = sorted(entries)
    table_end = 8 + 24 * len(order)
    offset = (table_end + ALIGN - 1) // ALIGN * ALIGN
    table = bytearray(b'RSDKvB' + struct.pack('<H', len(order)))
    layout = []
    for h in order:
        data = entries[h][1]
        table += h + struct.pack('<II', offset, len(data))
        layout.append((offset, data))
        offset = (offset + len(data) + ALIGN - 1) // ALIGN * ALIGN

    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(table)
        for off, data in layout:
            f.write(b'\0' * (off - f.tell()))
            f.write(data)
    os.replace(tmp, args.out)
    log(f'wrote {args.out}: {len(order)} files, {os.path.getsize(args.out) / 1048576:.1f} MB')

    if out_dir:
        eboot = args.eboot or (os.path.join(ROOT, 'eboot', game[0], 'EBOOT.PBP') if game else None)
        if eboot and os.path.exists(eboot):
            shutil.copyfile(eboot, os.path.join(out_dir, 'EBOOT.PBP'))
            log(f'copied {eboot}')
        elif os.path.exists(os.path.join(out_dir, 'EBOOT.PBP')):
            log('EBOOT.PBP is there already')  # (the release package)
        else:
            log('(no EBOOT.PBP to copy: put it in the same folder as Data.psp)')
        log(f'copy {os.path.abspath(os.path.join(out_dir, "..", "..", ".."))}{os.sep}PSP to the root of the Memory Stick')


if __name__ == '__main__':
    main()
