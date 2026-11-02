"""Reads RSDKv4 data packs (Data.rsdk of the 2013 Sonic 1 / Sonic 2 remasters).

Layout (see RSDKv4/Reader.cpp): 'RSDKvB', u16 file count, then per file: the MD5 of its lower case path as 4 big endian
u32 (the MD5 state words), u32 offset, u32 size (top bit: encrypted). The files only have hashes, no names.

Encryption: the two keys are MD5s of the file's size written in decimal (size and size/2 + 1), so a file can be
decrypted without knowing its name.
"""
import hashlib
import struct

ENC_KEY_1 = 0xAAAAAAAB
ENC_KEY_2 = 0x24924925


def path_hash(path):
    """The 16 bytes the pack stores for a path: the MD5 of the lower case path, each 4 byte word reversed."""
    d = hashlib.md5(path.lower().replace('\\', '/').encode('latin-1')).digest()
    return b''.join(d[i:i + 4][::-1] for i in range(0, 16, 4))


def _key(value):
    d = hashlib.md5(str(value).encode()).digest()
    return [b for i in range(0, 16, 4) for b in d[i:i + 4][::-1]]


def _mul_high(a, b):
    return (a * b) >> 32


def decrypt(data):
    """Decrypts one file's bytes (FileRead in Reader.cpp, from the start of the file)."""
    size = len(data)
    str_a = _key(size)
    str_b = _key((size >> 1) + 1)
    no = (size & 0x1FC) >> 2
    pos_a, pos_b, swap = 0, 8, 0
    out = bytearray(size)
    for i, c in enumerate(data):
        v = str_b[pos_b] ^ no ^ c
        if swap:
            v = ((v << 4) | (v >> 4)) & 0xFF
        out[i] = v ^ str_a[pos_a]

        pos_a += 1
        pos_b += 1
        if pos_a <= 0x0F:
            if pos_b > 0x0C:
                pos_b = 0
                swap ^= 1
        elif pos_b <= 0x08:
            pos_a = 0
            swap ^= 1
        else:
            no = (no + 2) & 0x7F
            key1 = _mul_high(ENC_KEY_1, no)
            key2 = _mul_high(ENC_KEY_2, no)
            temp1 = key2 + (no - key2) // 2
            temp2 = key1 // 8 * 3
            if swap:
                swap = 0
                pos_a = no - temp1 // 4 * 7
                pos_b = no - temp2 * 4 + 2
            else:
                swap = 1
                pos_b = no - temp1 // 4 * 7
                pos_a = no - temp2 * 4 + 3
    return bytes(out)


class Pack:
    """A Data.rsdk: .files is a list of (hash bytes, offset, size, encrypted)."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        if self.data[:6] != b'RSDKvB':
            raise ValueError(f'{path}: not an RSDKv4 data pack')
        count = struct.unpack_from('<H', self.data, 6)[0]
        self.files = []
        pos = 8
        for _ in range(count):
            h = self.data[pos:pos + 16]
            offset, size = struct.unpack_from('<II', self.data, pos + 16)
            self.files.append((h, offset, size & 0x7FFFFFFF, bool(size & 0x80000000)))
            pos += 24

    def read(self, entry):
        h, offset, size, encrypted = entry
        raw = self.data[offset:offset + size]
        return decrypt(raw) if encrypted else raw

    def find(self, path):
        h = path_hash(path)
        for e in self.files:
            if e[0] == h:
                return e
        return None
