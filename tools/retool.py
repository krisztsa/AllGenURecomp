"""Static reverse-engineering helpers for the game's main module (AArch32, position independent).

    python tools/retool.py strings <regex> [max]        printable strings matching (image offsets)
    python tools/retool.py range <lo> <hi>              every string between two image offsets
    python tools/retool.py xref <hex target>...         code loading that address (pc-relative literal + add pc, or a
                                                    GOT slot holding it); "xref str <exact string>" for a string
    python tools/retool.py callers <runtime addr>...    direct BL/B/BLX callers and data words (vtable slots) holding it
    python tools/retool.py dti [regex]                  MT Framework class tree: name, size, DTI, parent (MtDTI ctor calls)
    python tools/retool.py vtslot <runtime addr>        the vtables holding a function and its byte offset in them

Addresses: "image" offsets are positions in main's memory image; "runtime" addresses are what the recompiled game,
tools/disasm.py and thread dumps use (image + 0x200000). Caches go to build/re_cache/. Needs tools/venv (capstone).
"""
import bisect
import os
import pickle
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.environ.get('MHGU_MAIN', os.path.join(ROOT, 'build', 'data', 'exefs', 'main'))
CACHE = os.path.join(ROOT, 'obj', 're_cache')
BASE = 0x200000
MTDTI_CTOR = 0x7aedcc  # MtDTI::MtDTI(dti, name, parent, size, ...) (image offset)

n = Nso(MAIN)
img = bytes(n.image)
u32 = lambda a: struct.unpack_from('<I', img, a)[0]
T0, TL = n.text
TEXT = struct.unpack_from('<%dI' % (TL // 4), img, T0)


def cached(name, build):
    os.makedirs(CACHE, exist_ok=True)
    p = os.path.join(CACHE, name + '.pkl')
    if os.path.exists(p) and os.path.getmtime(p) > os.path.getmtime(MAIN):
        return pickle.load(open(p, 'rb'))
    v = build()
    pickle.dump(v, open(p, 'wb'))
    return v


def build_xrefs():
    refs = {}
    for i, w in enumerate(TEXT):
        if (w & 0x0F7F0000) != 0x051F0000:  # LDR rD, [pc, #imm]
            continue
        a = T0 + i * 4
        imm = w & 0xFFF
        lit = a + 8 + (imm if w & 0x00800000 else -imm)
        if lit < 0 or lit + 4 > len(img):
            continue
        v = u32(lit)
        rd = (w >> 12) & 15
        for k in range(1, 13):
            j = i + k
            if j >= len(TEXT):
                break
            x = TEXT[j]
            rn, rm = (x >> 16) & 15, x & 15
            if (x & 0x0FE00FF0) == 0x00800000 and ((rn == 15 and rm == rd) or (rn == rd and rm == 15)):  # add rX, pc, rD
                refs.setdefault((T0 + j * 4 + 8 + v) & 0xFFFFFFFF, []).append(a)
                break
            if (x & 0x0FFF0FF0) == 0x079F0000 and rm == rd:  # ldr rX, [pc, rD]: a GOT slot
                slot = T0 + j * 4 + 8 + v
                if 0 <= slot <= len(img) - 4:
                    refs.setdefault(u32(slot), []).append(a)
                break
    return refs


def cstr(a):
    try:
        e = img.index(b'\0', a)
    except ValueError:
        return None
    s = img[a:e]
    return s.decode() if s and re.fullmatch(rb'[\x20-\x7e]+', s) else None


def build_dti():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.skipdata = True
    calls = []
    for i, w in enumerate(TEXT):
        if (w & 0x0F000000) == 0x0B000000:
            off = w & 0xFFFFFF
            off -= 0x1000000 if off & 0x800000 else 0
            if T0 + i * 4 + 8 + off * 4 == MTDTI_CTOR:
                calls.append(T0 + i * 4)
    out = []
    for a in calls:
        regs = {}
        for ins in md.disasm(img[a - 640:a], a - 640):
            ops = ins.op_str.replace(' ', '').split(',')
            m = ins.mnemonic
            try:
                if m == 'ldr' and len(ops) == 3 and ops[1] == '[pc' and ops[2].startswith('#'):
                    regs[ops[0]] = u32(ins.address + 8 + int(ops[2].rstrip(']').lstrip('#'), 0))
                elif m == 'ldr' and len(ops) == 3 and ops[1] == '[pc' and ops[2].rstrip(']') in regs:
                    regs[ops[0]] = u32((ins.address + 8 + regs[ops[2].rstrip(']')]) & 0xFFFFFFFF)
                elif m == 'add' and len(ops) == 3 and 'pc' in ops[1:] and (ops[2] if ops[1] == 'pc' else ops[1]) in regs:
                    regs[ops[0]] = (ins.address + 8 + regs[ops[2] if ops[1] == 'pc' else ops[1]]) & 0xFFFFFFFF
                elif m == 'movw':
                    regs[ops[0]] = int(ops[1].lstrip('#'), 0)
                elif m == 'movt' and ops[0] in regs:
                    regs[ops[0]] = (regs[ops[0]] & 0xFFFF) | (int(ops[1].lstrip('#'), 0) << 16)
                elif m == 'mov' and ops[1].startswith('#'):
                    regs[ops[0]] = int(ops[1].lstrip('#'), 0)
                elif m == 'mov' and ops[1] in regs:
                    regs[ops[0]] = regs[ops[1]]
                elif m == 'bl':
                    for r in ('r0', 'r1', 'r2', 'r3', 'r12', 'lr'):
                        regs.pop(r, None)
            except (ValueError, struct.error):
                pass
        out.append((cstr(regs['r1']) if 'r1' in regs else None, regs.get('r3'), regs.get('r0'), regs.get('r2'), a))
    names = {d: nm for nm, _, d, _, _ in out if d is not None}
    return [(nm, sz, d, names.get(p, '?' if p is None else hex(p)), a) for nm, sz, d, p, a in out]


def main(argv):
    if not argv:
        print(__doc__)
        return
    cmd, args = argv[0], argv[1:]
    if cmd == 'strings':
        pat, mx = re.compile(args[0].encode(), re.I), int(args[1]) if len(args) > 1 else 100
        for m in re.finditer(rb'[\x20-\x7e]{4,}', img):
            if pat.search(m.group()):
                print(f'{m.start():#x} {m.group().decode()}')
                mx -= 1
                if not mx:
                    break
    elif cmd == 'range':
        a, b = int(args[0], 0), int(args[1], 0)
        for m in re.finditer(rb'[\x20-\x7e]{3,}', img[a:b]):
            print(f'{a + m.start():#x} {m.group().decode()}')
    elif cmd == 'xref':
        refs = cached('xrefs', build_xrefs)
        if args and args[0] == 'str':
            s = args[1].encode() + b'\0'
            for m in re.finditer(re.escape(s), img):
                if m.start() == 0 or img[m.start() - 1] == 0:
                    print(f'{args[1]!r} at {m.start():#x}: code at (runtime)',
                          ' '.join(f'{x + BASE:#x}' for x in refs.get(m.start(), [])))
        else:
            for t in args:
                print(f'{int(t, 16):#x}: code at (runtime)', ' '.join(f'{x + BASE:#x}' for x in refs.get(int(t, 16), [])))
    elif cmd == 'callers':
        allw = struct.unpack_from('<%dI' % (len(img) // 4), img, 0)
        for t in args:
            tgt = int(t, 0) - BASE
            direct = []
            for i, w in enumerate(TEXT):
                blx = (w & 0xFE000000) == 0xFA000000
                if (w & 0x0E000000) == 0x0A000000 or blx:
                    off = w & 0xFFFFFF
                    off -= 0x1000000 if off & 0x800000 else 0
                    dest = T0 + i * 4 + 8 + off * 4 + ((2 if w & 0x01000000 else 0) if blx else 0)
                    if dest == tgt:
                        kind = 'blx' if blx else ('bl' if w & 0x01000000 else 'b')
                        direct.append(f'{kind}@{T0 + i * 4 + BASE:#x}')
            data = [f'{i * 4:#x}' for i, w in enumerate(allw) if w == tgt and i * 4 >= n.rodata[0]]
            print(f'{int(t, 0):#x}: calls', ' '.join(direct) or '-', '| data words (image):', ' '.join(data[:30]) or '-')
    elif cmd == 'dti':
        pat = re.compile(args[0]) if args else None
        for nm, sz, d, p, a in cached('dti', build_dti):
            if nm and (not pat or pat.search(nm)):
                print(f'{nm}\t{sz:#x}\tdti {d:#x}\tparent {p}' if sz is not None and d is not None else f'{nm}\t?')
    elif cmd == 'vtslot':
        refs = cached('xrefs', build_xrefs)
        keys = sorted(k for k in refs if n.rodata[0] <= k < len(img))
        tgt = int(args[0], 0) - BASE
        allw = struct.unpack_from('<%dI' % (len(img) // 4), img, 0)
        for i, w in enumerate(allw):
            if w == tgt and i * 4 >= n.rodata[0]:
                k = keys[bisect.bisect_right(keys, i * 4) - 1]  # the vtable symbol constructors store (+8 = entries)
                print(f'vtable {k:#x}: byte offset {i * 4 - (k + 8):#x}')
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
