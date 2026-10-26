"""Copy the DLLs a Windows build needs next to it, so the executable starts without MSYS2 on PATH.

usage: python tools/bundle_dlls.py <dll dir> <out dir> <exe or dll>...

Follows the import tables (and delay-load imports) of the given files recursively. A DLL found in <dll dir> (MSYS2's
ucrt64/bin) is copied to <out dir> when missing or older; anything not found there is the system's (kernel32,
ucrtbase...) and is left alone. vulkan-1.dll is never copied: the GPU driver's own loader (System32) must be used.
"""
import os
import shutil
import struct
import sys

NEVER = {'vulkan-1.dll'}


def imports(path):
    """Names of the DLLs a PE file imports (normal and delay-load)."""
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, opt_size = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)  # PE32+ / PE32 data directories
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        return None

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b'\0', o)].decode('ascii', 'replace') if o is not None else None

    names = []
    for index, entry, name_at in ((1, 20, 12), (13, 32, 4)):  # import table, delay-load import table
        rva = struct.unpack_from('<I', d, dirs + 8 * index)[0]
        o = off(rva) if rva else None
        while o is not None and o + entry <= len(d):
            name_rva = struct.unpack_from('<I', d, o + name_at)[0]
            if not name_rva:
                break
            n = cstr(name_rva)
            if n:
                names.append(n)
            o += entry
    return names


def main():
    src, out, roots = sys.argv[1], sys.argv[2], sys.argv[3:]
    have = {n.lower(): n for n in os.listdir(src)}
    seen, todo, copied = set(), list(roots), 0
    while todo:
        for name in imports(todo.pop()):
            k = name.lower()
            if k in seen or k in NEVER or k not in have:
                continue
            seen.add(k)
            s, t = os.path.join(src, have[k]), os.path.join(out, have[k])
            if not os.path.exists(t) or os.path.getmtime(t) < os.path.getmtime(s):
                shutil.copy2(s, t)
                copied += 1
            todo.append(s)
    print(f'bundle_dlls: {len(seen)} DLLs needed, {copied} copied to {out}')


if __name__ == '__main__':
    main()
