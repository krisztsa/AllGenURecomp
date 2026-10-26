"""MT Framework ARC archives (Monster Hunter Generations Ultimate, Switch: version 17).

Layout: "ARC\\0", u16 version, u16 file count, u32 padding, then one 80-byte entry per file:
    char path[64]      (backslash-separated, no extension)
    u32  type_hash     (the resource class: picks the file extension)
    u32  comp_size
    u32  size_flags    (decompressed size in the low 29 bits, flags in the top bits)
    u32  offset
followed by the zlib-compressed file data.

Usage:
    mtarc.py list <file.arc>
    mtarc.py extract <file.arc> <outdir>
    mtarc.py pack <indir> <file.arc> [--like original.arc]   (rebuild from extracted files)
    mtarc.py index <romfs/nativeNX> <index.tsv>               (every entry of every archive)
"""
import json
import os
import struct
import sys
import zlib

MAGIC = b'ARC\0'
ENTRY = 80
SIZE_MASK = 0x1FFFFFFF


def load_types():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mt_types.json')
    try:
        return {int(k, 16): v for k, v in json.load(open(p)).items()}
    except (OSError, ValueError):
        return {}


TYPES = load_types()


def ext_for(h):
    return TYPES.get(h, f'{h:08X}')


def hash_for_ext(ext):
    for h, e in TYPES.items():
        if e == ext:
            return h
    return int(ext, 16)


class Arc:
    def __init__(self, data):
        if data[:4] != MAGIC:
            raise ValueError('not an ARC archive')
        self.version, n = struct.unpack_from('<HH', data, 4)
        self.data = data
        self.entries = []
        for i in range(n):
            o = 12 + i * ENTRY
            name = data[o:o + 64].split(b'\0')[0].decode('ascii', 'replace')
            th, csize, sf, off = struct.unpack_from('<IIII', data, o + 64)
            self.entries.append({'name': name, 'type': th, 'csize': csize, 'size': sf & SIZE_MASK,
                                 'flags': sf & ~SIZE_MASK, 'offset': off})

    @classmethod
    def open(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def read(self, e):
        raw = self.data[e['offset']:e['offset'] + e['csize']]
        if e['csize'] == e['size']:
            try:
                return zlib.decompress(raw)
            except zlib.error:
                return raw
        return zlib.decompress(raw)

    def path_of(self, e):
        return e['name'].replace('\\', '/') + '.' + ext_for(e['type'])


def build(files, version=17, flags=0x40000000, align=0x8000):
    """files: list of (name without extension, type_hash, data). Returns the archive bytes."""
    n = len(files)
    head = bytearray(MAGIC + struct.pack('<HHI', version, n, 0))
    blobs = []
    off = (12 + n * ENTRY + align - 1) // align * align if align else 12 + n * ENTRY
    for name, th, data in files:
        comp = zlib.compress(data, 9)
        nb = name.encode('ascii')[:63]
        head += nb + b'\0' * (64 - len(nb))
        head += struct.pack('<IIII', th, len(comp), (len(data) & SIZE_MASK) | flags, off)
        blobs.append(comp)
        off += len(comp)
    head += b'\0' * ((12 + n * ENTRY + align - 1) // align * align - len(head)) if align else b''
    return bytes(head) + b''.join(blobs)


def cmd_list(path):
    a = Arc.open(path)
    print(f'version {a.version}, {len(a.entries)} files')
    for e in a.entries:
        print(f'{e["size"]:9d} {e["type"]:08X} {a.path_of(e)}')


def cmd_extract(path, out):
    a = Arc.open(path)
    order = []
    for e in a.entries:
        p = os.path.join(out, a.path_of(e))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(a.read(e))
        order.append({'name': e['name'], 'type': f'{e["type"]:08X}', 'flags': f'{e["flags"]:08X}'})
    with open(os.path.join(out, '_arc.json'), 'w') as f:
        json.dump({'version': a.version, 'files': order}, f, indent=1)


def cmd_pack(indir, out):
    meta = json.load(open(os.path.join(indir, '_arc.json')))
    files = []
    for e in meta['files']:
        th = int(e['type'], 16)
        p = os.path.join(indir, e['name'].replace('\\', '/') + '.' + ext_for(th))
        files.append((e['name'], th, open(p, 'rb').read()))
    flags = int(meta['files'][0]['flags'], 16) if meta['files'] else 0x40000000
    with open(out, 'wb') as f:
        f.write(build(files, meta.get('version', 17), flags))


def cmd_index(root, out):
    rows = 0
    with open(out, 'w', encoding='utf-8') as f:
        for dp, dn, fn in os.walk(root):
            dn.sort()
            for name in sorted(fn):
                if not name.endswith('.arc'):
                    continue
                p = os.path.join(dp, name)
                try:
                    a = Arc.open(p)
                except ValueError:
                    continue
                rel = os.path.relpath(p, root).replace('\\', '/')
                for e in a.entries:
                    f.write(f'{rel}\t{e["name"]}\t{e["type"]:08X}\t{e["size"]}\n')
                    rows += 1
    print(f'{rows} entries')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    c = sys.argv[1]
    if c == 'list':
        cmd_list(sys.argv[2])
    elif c == 'extract':
        cmd_extract(sys.argv[2], sys.argv[3])
    elif c == 'pack':
        cmd_pack(sys.argv[2], sys.argv[3])
    elif c == 'index':
        cmd_index(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(1)
