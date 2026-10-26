"""Disassemble guest code at a runtime address (main at 0x200000, sdk at 0x2400000), or at a symbol of any module
(addresses then relative to that module; subsdk0, the movie library, is not loaded at runtime).

usage: python tools/disasm.py <game dir> <addr | module:symbol> [count] [before]
  count and before (instructions shown before the address) take hex or decimal; default 32 and 8 (0 for a symbol).
"""
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(__file__))
from nso import Nso

MODULES = [('main', 0x200000), ('sdk', 0x2400000)]


def main():
    game, where = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 32
    if ':' in where:
        name, sym = where.split(':', 1)
        nso, base = Nso(os.path.join(game, 'exefs', name)), dict(MODULES).get(name, 0)
        hits = [s for s in nso.exports() if s[0] == sym]
        if not hits:
            sys.exit(f'{sym} not found in {name}')
        addr = base + (hits[0][3] & ~1)
        before = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
        if len(sys.argv) <= 3 and hits[0][4]:
            count = hits[0][4] // 4
    else:
        addr = int(where, 0)
        before = int(sys.argv[4], 0) if len(sys.argv) > 4 else 8
        for name, base in reversed(MODULES):
            if addr >= base:
                break
        nso = Nso(os.path.join(game, 'exefs', name))
    names = {base + (s[3] & ~1): s[0] for s in nso.syms if s[0] and s[3]}
    imports = {}
    for off, typ, si, _ in nso.relocs:  # GOT / PLT slots of imported symbols
        if si and nso.syms[si][0]:
            imports[base + off] = nso.syms[si][0]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM | capstone.CS_MODE_V8)
    # PLT stubs: `add ip, pc, #a, #ra; add ip, ip, #b, #rb; ldr pc, [ip, #c]!` -> the GOT slot of an imported symbol
    got_sym = {base + off: nso.syms[si][0] for off, typ, si, _ in nso.relocs if typ == 22 and si}
    def rot(w):
        r = ((w >> 8) & 0xF) * 2
        v = w & 0xFF
        return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v
    for a in range(nso.text[0], nso.text[0] + nso.text[1] - 8, 4):
        w0, w1, w2 = nso.u32(a), nso.u32(a + 4), nso.u32(a + 8)
        if (w0 & 0xFFFFF000) == 0xE28FC000 and (w1 & 0xFFFFF000) == 0xE28CC000 and (w2 & 0xFFFFF000) == 0xE5BCF000:
            g = base + a + 8 + rot(w0) + rot(w1) + (w2 & 0xFFF)
            if g in got_sym:
                names.setdefault(base + a, got_sym[g])
    start = addr - 4 * before
    code = bytes(nso.image[start - base:start - base + 4 * (count + before)])
    for off in range(0, len(code), 4):
        a = start + off
        if a in names and off:
            print(f'   <{names[a]}>')
        ins = next(md.disasm(code[off:off + 4], a), None)
        text = f'{ins.mnemonic} {ins.op_str}' if ins else f'.word {int.from_bytes(code[off:off + 4], "little"):#x}'
        if ins and ins.mnemonic in ('bl', 'b', 'blx') and ins.op_str.startswith('#'):
            t = int(ins.op_str[1:], 0)
            if t in names:
                text += f'  ; {names[t]}'
        print(f'{"=>" if a == addr else "  "} {a:08x}: {text}')


if __name__ == '__main__':
    main()
