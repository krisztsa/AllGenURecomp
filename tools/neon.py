"""Advanced SIMD (NEON) -> C.

Covers what the game and SDK execute (tools/gen.py --stats lists reachable unsupported encodings): bitwise ops and
moves, immediates, integer / float lane arithmetic and compares, VEXT, VDUP, VCNT and friends, VCVT, and every
element / structure load and store. Lanes are unrolled at generation time; each instruction reads its sources into
temporaries before writing, so overlapping registers behave as on hardware.
"""


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def bit(w, i):
    return (w >> i) & 1


def U():
    from lifter import Unsupported
    return Unsupported


def reg(v, x):
    return (x << 4) | v


LANE_T = {8: ('uint8_t', 'int8_t', 'b'), 16: ('uint16_t', 'int16_t', 'h'), 32: ('uint32_t', 'int32_t', 's'),
          64: ('uint64_t', 'int64_t', 'd')}


def lane(dreg, i, size):
    """C lvalue for lane i (of `size` bits) counted from register dreg (a q register is two d registers)."""
    k = LANE_T[size][2]
    per = 64 // size
    return f'c->v.{k}[{dreg * per + i}]'


def flane(dreg, i):
    return f'c->v.f[{dreg * 2 + i}]'


def fin(dreg, i):
    """A float lane as an input: Advanced SIMD flushes denormals to zero (standard FPSCR value)."""
    return f'nftz(c->v.f[{dreg * 2 + i}])'


class Neon:
    # ------------------------------------------------------------ data processing
    def dp(self, w, pc):
        Ubit = bit(w, 24)
        if bit(w, 23) == 0:
            return self.three_same(w, Ubit)
        if bits(w, 19, 3) == 0 and bit(w, 7) == 0 and bit(w, 4) == 1:
            return self.modified_imm(w)
        if bits(w, 20, 2) != 3 and bit(w, 4) == 0:
            if bit(w, 6) == 0:
                return self.three_diff(w, Ubit)
            return self.two_scalar(w, Ubit)
        if Ubit and bits(w, 20, 2) == 3 and bits(w, 16, 2) == 0 and bits(w, 18, 2) == 0 and bits(w, 8, 3) == 3 \
                and bit(w, 4) == 0:
            return self.aes(w)
        if bits(w, 20, 2) == 3 and not Ubit and bit(w, 4) == 0:
            return self.vext(w)
        if Ubit and bits(w, 20, 2) == 3 and bits(w, 10, 2) != 3 and bit(w, 4) == 0 and bits(w, 8, 4) <= 0xB:
            return self.two_misc(w)
        if Ubit and bits(w, 20, 2) == 3 and bits(w, 10, 2) == 2:
            return self.vtbl(w)
        if Ubit and bits(w, 20, 2) == 3 and bits(w, 7, 5) == 0b11000:
            return self.vdup_scalar(w)
        if bit(w, 4) == 1 and bit(w, 7) == 0 or bit(w, 7) == 1 and bit(w, 4) == 1:
            return self.shift_imm(w)
        raise U()(f'neon dp {w:08x}')

    def regs3(self, w):
        Q = bit(w, 6)
        d, n, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 16, 4), bit(w, 7)), reg(bits(w, 0, 4), bit(w, 5))
        return Q, d, n, m

    def three_same(self, w, Ubit):
        A, B, C = bits(w, 8, 4), bit(w, 4), bits(w, 20, 2)
        Q, d, n, m = self.regs3(w)
        nd = 2 if Q else 1
        if A == 1 and B == 1:  # bitwise
            out = []
            for i in range(nd):
                a, b, r = f'c->v.d[{n + i}]', f'c->v.d[{m + i}]', f'c->v.d[{d + i}]'
                e = {(0, 0): f'{a} & {b}', (0, 1): f'{a} & ~{b}', (0, 2): f'{a} | {b}', (0, 3): f'{a} | ~{b}',
                     (1, 0): f'{a} ^ {b}', (1, 1): f'({r} & {a}) | (~{r} & {b})',
                     (1, 2): f'({a} & {b}) | ({r} & ~{b})', (1, 3): f'({r} & {b}) | ({a} & ~{b})'}[(Ubit, C)]
                out.append(f'uint64_t t{i} = {e};')
            out += [f'c->v.d[{d + i}] = t{i};' for i in range(nd)]
            return '{ ' + ' '.join(out) + ' }'
        size = 8 << C
        if A == 0xD and not B and not Ubit:  # VADD / VSUB float
            if bit(w, 20):
                raise U()('f16')
            op = '-' if bit(w, 21) else '+'
            return self.lanes_f(d, n, m, Q, lambda a, b: f'{a} {op} {b}')
        if A == 0xD and B and Ubit and not bit(w, 21):  # VMUL float
            return self.lanes_f(d, n, m, Q, lambda a, b: f'{a} * {b}')
        if A == 0xD and B and not Ubit:  # VMLA / VMLS float
            op = '-' if bit(w, 21) else '+'
            return self.lanes_f(d, n, m, Q, lambda a, b, r=None: f'{{R}} {op} {a} * {b}', acc=True)
        if A == 0xD and not B and Ubit and not bit(w, 21):  # VPADD float
            return self.pairwise_f(d, n, m, lambda a, b: f'{a} + {b}')
        if A == 0xD and not B and Ubit and bit(w, 21):  # VABD float
            return self.lanes_f(d, n, m, Q, lambda a, b: f'fabsf({a} - {b})')
        if A == 0xF and not B:  # VMAX / VMIN float (Ubit=1: pairwise)
            fn = 'vmin_f' if bit(w, 21) else 'vmax_f'
            if Ubit:
                return self.pairwise_f(d, n, m, lambda a, b: f'{fn}({a}, {b})')
            return self.lanes_f(d, n, m, Q, lambda a, b: f'{fn}({a}, {b})')
        if A == 0xE and not B:  # VCEQ / VCGE / VCGT float
            if not Ubit and not bit(w, 21):
                cmp = '=='
            elif Ubit and not bit(w, 21):
                cmp = '>='
            elif Ubit and bit(w, 21):
                cmp = '>'
            else:
                raise U()('vc float')
            return self.lanes_fi(d, n, m, Q, lambda a, b: f'({a} {cmp} {b}) ? 0xFFFFFFFFu : 0u')
        if A == 0xF and B and not Ubit:  # VRECPS / VRSQRTS
            fn = 'vrsqrts_f' if bit(w, 21) else 'vrecps_f'
            return self.lanes_f(d, n, m, Q, lambda a, b: f'{fn}({a}, {b})')
        if A == 8 and not B:  # VADD / VSUB integer
            op = '-' if Ubit else '+'
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})({a} {op} {b})')
        if A == 9 and B and not Ubit:  # VMUL integer
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})({a} * {b})')
        if A == 9 and not B:  # VMLA / VMLS integer
            op = '-' if Ubit else '+'
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})({{R}} {op} {a} * {b})', acc=True)
        if A == 8 and B:  # VTST / VCEQ integer
            if Ubit:
                return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({a} == {b}) ? ({ut})~0u : 0')
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({a} & {b}) ? ({ut})~0u : 0')
        if A == 3:  # VCGT (B=0) / VCGE (B=1) integer
            cmp = '>=' if B else '>'
            if Ubit:
                return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({a} {cmp} {b}) ? ({ut})~0u : 0')
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'(({st}){a} {cmp} ({st}){b}) ? ({ut})~0u : 0')
        if A == 6:  # VMAX / VMIN integer
            op = '<' if B else '>'
            if Ubit:
                return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({a} {op} {b}) ? {a} : {b}')
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'(({st}){a} {op} ({st}){b}) ? {a} : {b}')
        if A == 0 and B:  # VQADD
            return self.lanes_sat(d, n, m, Q, size, Ubit, '+')
        if A == 2 and B:  # VQSUB
            return self.lanes_sat(d, n, m, Q, size, Ubit, '-')
        if A == 4 and not B:  # VSHL (register)
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: self.vshl_expr(b, a, ut, st, size, Ubit), swap=True)
        if A == 0xB and B and not Ubit:  # VPADD integer
            return self.pairwise_i(d, n, m, size)
        if A == 1 and not B:  # VRHADD
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})((({"uint64_t" if Ubit else "int64_t"})({"" if Ubit else "(" + st + ")"}{a}) + ({"" if Ubit else "(" + st + ")"}{b}) + 1) >> 1)')
        if A == 0 and not B:  # VHADD
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})((({"uint64_t" if Ubit else "int64_t"})({"" if Ubit else "(" + st + ")"}{a}) + ({"" if Ubit else "(" + st + ")"}{b})) >> 1)')
        if A == 7 and not B:  # VABD integer
            if Ubit:
                return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})({a} > {b} ? {a} - {b} : {b} - {a})')
            return self.lanes_i(d, n, m, Q, size, lambda a, b, ut, st: f'({ut})(({st}){a} > ({st}){b} ? {a} - {b} : {b} - {a})')
        raise U()(f'neon 3same {w:08x}')

    def vshl_expr(self, sh, v, ut, st, size, Ubit):
        s = f'(int8_t){sh}'
        if Ubit:
            return (f'({ut})(({s}) >= 0 ? (({s}) >= {size} ? 0 : (uint64_t){v} << ({s})) : '
                    f'(-({s}) >= {size} ? 0 : {v} >> -({s})))')
        return (f'({ut})(({s}) >= 0 ? (({s}) >= {size} ? 0 : (uint64_t){v} << ({s})) : '
                f'(({st}){v} >> (-({s}) >= {size} ? {size - 1} : -({s}))))')

    def lanes_f(self, d, n, m, Q, f, acc=False):
        k = 4 if Q else 2
        out = []
        for i in range(k):
            e = f(fin(n, i), fin(m, i))
            if acc:
                e = e.replace('{R}', fin(d, i))
            out.append(f'float t{i} = nftz({e});')
        out += [f'{flane(d, i)} = t{i};' for i in range(k)]
        return '{ ' + ' '.join(out) + ' }'

    def lanes_fi(self, d, n, m, Q, f):
        k = 4 if Q else 2
        out = [f'uint32_t t{i} = {f(fin(n, i), fin(m, i))};' for i in range(k)]
        out += [f'c->v.s[{d * 2 + i}] = t{i};' for i in range(k)]
        return '{ ' + ' '.join(out) + ' }'

    def pairwise_f(self, d, n, m, f):
        a0, a1, b0, b1 = fin(n, 0), fin(n, 1), fin(m, 0), fin(m, 1)
        return f'{{ float t0 = nftz({f(a0, a1)}), t1 = nftz({f(b0, b1)}); {flane(d, 0)} = t0; {flane(d, 1)} = t1; }}'

    def pairwise_i(self, d, n, m, size):
        ut, st, k = LANE_T[size]
        per = 64 // size
        vals = [lane(n, i, size) for i in range(per)] + [lane(m, i, size) for i in range(per)]
        out = [f'{ut} t{i} = ({ut})({vals[2 * i]} + {vals[2 * i + 1]});' for i in range(per)]
        out += [f'{lane(d, i, size)} = t{i};' for i in range(per)]
        return '{ ' + ' '.join(out) + ' }'

    def lanes_i(self, d, n, m, Q, size, f, acc=False, swap=False):
        ut, st, k = LANE_T[size]
        cnt = (128 if Q else 64) // size
        out = []
        for i in range(cnt):
            a, b = lane(n, i, size), lane(m, i, size)
            e = f(b, a, ut, st) if swap else f(a, b, ut, st)
            if acc:
                e = e.replace('{R}', lane(d, i, size))
            out.append(f'{ut} t{i} = {e};')
        out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def lanes_sat(self, d, n, m, Q, size, unsigned, op):
        ut, st, k = LANE_T[size]
        cnt = (128 if Q else 64) // size
        out = []
        if unsigned:
            hi, lo = (1 << size) - 1, 0
            wide = 'int64_t'
        else:
            hi, lo = (1 << (size - 1)) - 1, -(1 << (size - 1))
            wide = 'int64_t'
        for i in range(cnt):
            a, b = lane(n, i, size), lane(m, i, size)
            if size == 64:
                raise U()('vqadd 64')
            ca = f'({wide})({ut}){a}' if unsigned else f'({wide})({st}){a}'
            cb = f'({wide})({ut}){b}' if unsigned else f'({wide})({st}){b}'
            out.append(f'int64_t s{i} = {ca} {op} {cb}; if (s{i} > {hi}ll) {{ s{i} = {hi}ll; c->fpscr |= 1u << 27; }} '
                       f'if (s{i} < {lo}ll) {{ s{i} = {lo}ll; c->fpscr |= 1u << 27; }}')
        out += [f'{lane(d, i, size)} = ({ut})s{i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def modified_imm(self, w):
        Q = bit(w, 6)
        d = reg(bits(w, 12, 4), bit(w, 22))
        cmode, op = bits(w, 8, 4), bit(w, 5)
        imm8 = (bit(w, 24) << 7) | (bits(w, 16, 3) << 4) | bits(w, 0, 4)
        if cmode < 8:  # 32-bit lanes, shifted byte
            v = imm8 << (8 * (cmode >> 1))
            v64 = v | (v << 32)
            kind = 'orr' if (cmode & 1) else 'mov'
        elif cmode < 12:  # 16-bit lanes
            v = imm8 << (8 * ((cmode >> 1) & 1))
            v64 = v | (v << 16) | (v << 32) | (v << 48)
            kind = 'orr' if (cmode & 1) else 'mov'
        elif cmode < 14:  # 32-bit "ones" shift
            v = (imm8 << 8 | 0xFF) if cmode == 12 else (imm8 << 16 | 0xFFFF)
            v64 = v | (v << 32)
            kind = 'mov'
        elif cmode == 14:
            if not op:
                v64 = int.from_bytes(bytes([imm8] * 8), 'little')
            else:
                v64 = 0
                for i in range(8):
                    if (imm8 >> i) & 1:
                        v64 |= 0xFF << (8 * i)
                op = 0  # (this form is a plain move)
            kind = 'mov'
        else:  # cmode 15: float
            if op:
                raise U()('vmov f64 imm')
            b6 = (imm8 >> 6) & 1
            exp = ((b6 ^ 1) << 7) | ((0x1F if b6 else 0) << 2) | ((imm8 >> 4) & 3)
            v = ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 15) << 19)
            v64 = v | (v << 32)
            kind = 'mov'
        if op and kind == 'mov':
            v64 = ~v64 & 0xFFFFFFFFFFFFFFFF  # VMVN
        if op and kind == 'orr':
            kind = 'bic'
        nd = 2 if Q else 1
        out = []
        for i in range(nd):
            r = f'c->v.d[{d + i}]'
            if kind == 'mov':
                out.append(f'{r} = 0x{v64:x}ull;')
            elif kind == 'orr':
                out.append(f'{r} |= 0x{v64:x}ull;')
            else:
                out.append(f'{r} &= ~0x{v64:x}ull;')
        return ' '.join(out)

    def three_diff(self, w, Ubit):
        """VMULL / VMLAL / VMLSL (integer, long): Qd = Dn x Dm widened."""
        A = bits(w, 8, 4)
        size = 8 << bits(w, 20, 2)
        d, n, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 16, 4), bit(w, 7)), reg(bits(w, 0, 4), bit(w, 5))
        if A not in (0b1100, 0b1000, 0b1010):
            raise U()(f'neon 3diff {w:08x}')
        ut, st, _ = LANE_T[size]
        wide = size * 2
        wt, wst, _ = LANE_T[wide]
        cnt = 64 // size
        src = ut if Ubit else st
        out = []
        for i in range(cnt):
            p = f'({wt})(({wst})({src}){lane(n, i, size)} * ({wst})({src}){lane(m, i, size)})'
            if A == 0b1000:
                p = f'({wt})({lane(d, i, wide)} + {p})'
            elif A == 0b1010:
                p = f'({wt})({lane(d, i, wide)} - {p})'
            out.append(f'{wt} t{i} = {p};')
        out += [f'{lane(d, i, wide)} = t{i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def two_scalar(self, w, Q):
        """VMUL / VMLA / VMLS by scalar (integer and float), VMULL / VMLAL by scalar."""
        A = bits(w, 8, 4)
        size = 8 << bits(w, 20, 2)
        d, n = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 16, 4), bit(w, 7))
        if size == 16:
            m, idx = bits(w, 0, 3), (bit(w, 5) << 1) | bit(w, 3)
        else:
            m, idx = bits(w, 0, 4), bit(w, 5)
        F = A & 1
        cnt = (128 if Q else 64) // size
        op = A >> 2
        if A in (0, 1, 4, 5, 8, 9):  # same-width forms
            if F:
                s = f'nftz(c->v.f[{m * 2 + idx}])'
                out = []
                for i in range(cnt):
                    prod = f'{fin(n + i // 2, i % 2)} * {s}'
                    e = {0: f'{fin(d + i // 2, i % 2)} + {prod}', 1: f'{fin(d + i // 2, i % 2)} - {prod}', 2: prod}[op]
                    out.append(f'float t{i} = nftz({e});')
                out += [f'{flane(d + i // 2, i % 2)} = t{i};' for i in range(cnt)]
                return '{ ' + ' '.join(out) + ' }'
            ut = LANE_T[size][0]
            s = lane(m, idx, size)
            out = []
            for i in range(cnt):
                prod = f'{lane(n, i, size)} * {s}'
                e = {0: f'{lane(d, i, size)} + {prod}', 1: f'{lane(d, i, size)} - {prod}', 2: prod}[op]
                out.append(f'{ut} t{i} = ({ut})({e});')
            out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
            return '{ ' + ' '.join(out) + ' }'
        if A in (0b0010, 0b0110, 0b1010):  # VMLAL / VMLSL / VMULL by scalar (U = bit 24)
            ut, st, _ = LANE_T[size]
            src = ut if Q else st
            wide = size * 2
            wt, wst, _ = LANE_T[wide]
            out = []
            for i in range(64 // size):
                p = f'({wt})(({wst})({src}){lane(n, i, size)} * ({wst})({src}){lane(m, idx, size)})'
                if A == 0b0010:
                    p = f'({wt})({lane(d, i, wide)} + {p})'
                elif A == 0b0110:
                    p = f'({wt})({lane(d, i, wide)} - {p})'
                out.append(f'{wt} t{i} = {p};')
            out += [f'{lane(d, i, wide)} = t{i};' for i in range(64 // size)]
            return '{ ' + ' '.join(out) + ' }'
        raise U()(f'neon scalar {w:08x}')

    def aes(self, w):
        d, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 0, 4), bit(w, 5))
        op = bits(w, 6, 2) | (bit(w, 7) << 1)
        kind = bits(w, 6, 3) & 3  # bits 7:6: AESE 00, AESD 01, AESMC 10, AESIMC 11
        fn = ['neon_aese', 'neon_aesd', 'neon_aesmc', 'neon_aesimc'][bits(w, 6, 2)]
        return f'{fn}(c, {d}, {m});'

    def vext(self, w):
        Q, d, n, m = self.regs3(w)
        imm = bits(w, 8, 4)
        nb = 16 if Q else 8
        src = [f'c->v.b[{n * 8 + i}]' for i in range(nb)] + [f'c->v.b[{m * 8 + i}]' for i in range(nb)]
        out = [f'uint8_t t{i} = {src[imm + i]};' for i in range(nb)]
        out += [f'c->v.b[{d * 8 + i}] = t{i};' for i in range(nb)]
        return '{ ' + ' '.join(out) + ' }'

    def vtbl(self, w):
        d, n, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 16, 4), bit(w, 7)), reg(bits(w, 0, 4), bit(w, 5))
        length = bits(w, 8, 2) + 1
        tbx = bit(w, 6)
        out = ['uint8_t tab[32];']
        out += [f'memcpy(tab + {8 * i}, &c->v.d[{(n + i) % 32}], 8);' for i in range(length)]
        for i in range(8):
            idx = f'c->v.b[{m * 8 + i}]'
            keep = f'c->v.b[{d * 8 + i}]' if tbx else '0'
            out.append(f'uint8_t t{i} = {idx} < {8 * length} ? tab[{idx}] : {keep};')
        out += [f'c->v.b[{d * 8 + i}] = t{i};' for i in range(8)]
        return '{ ' + ' '.join(out) + ' }'

    def vdup_scalar(self, w):
        Q = bit(w, 6)
        d, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 0, 4), bit(w, 5))
        imm4 = bits(w, 16, 4)
        if imm4 & 1:
            size, idx = 8, imm4 >> 1
        elif imm4 & 2:
            size, idx = 16, imm4 >> 2
        elif imm4 & 4:
            size, idx = 32, imm4 >> 3
        else:
            raise U()('vdup scalar size')
        ut = LANE_T[size][0]
        cnt = (128 if Q else 64) // size
        out = [f'{ut} t = {lane(m, idx, size)};']
        out += [f'{lane(d, i, size)} = t;' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def vdup_core(self, w, pc, src):
        Q = bit(w, 21)
        d = reg(bits(w, 16, 4), bit(w, 7))
        be = (bit(w, 22) << 1) | bit(w, 5)
        size = {0: 32, 1: 16, 2: 8}.get(be)
        if size is None:
            raise U()('vdup size')
        ut = LANE_T[size][0]
        cnt = (128 if Q else 64) // size
        out = [f'{ut} t = ({ut}){src};']
        out += [f'{lane(d, i, size)} = t;' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def two_misc(self, w):
        size = 8 << bits(w, 18, 2)
        A, B = bits(w, 16, 2), bits(w, 6, 5)
        Q = bit(w, 6)
        d, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 0, 4), bit(w, 5))
        op = B >> 1  # bits 10:7
        nd = 2 if Q else 1
        if A == 0 and op == 0b1010 and size == 8:  # VCNT
            return self.map_lanes(d, m, Q, 8, lambda a, ut: f'({ut})__builtin_popcount({a})')
        if A == 0 and op == 0b1011:  # VMVN
            return '{ ' + ' '.join(f'uint64_t t{i} = ~c->v.d[{m + i}];' for i in range(nd)) + ' ' + \
                   ' '.join(f'c->v.d[{d + i}] = t{i};' for i in range(nd)) + ' }'
        if A == 0 and op in (0, 1, 2):  # VREV64 / VREV32 / VREV16
            group = {0: 64, 1: 32, 2: 16}[op]
            per = group // size
            cnt = (128 if Q else 64) // size
            src = [lane(m, i, size) for i in range(cnt)]
            out = [f'{LANE_T[size][0]} t{i} = {src[(i // per) * per + (per - 1 - i % per)]};' for i in range(cnt)]
            out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
            return '{ ' + ' '.join(out) + ' }'
        if A == 1 and op in (0b0110, 0b0111) and size == 32 and bit(w, 10):  # VABS / VNEG float
            if op == 0b0110:
                return self.map_lanes(d, m, Q, 32, lambda a, ut: f'{a} & 0x7FFFFFFFu')
            return self.map_lanes(d, m, Q, 32, lambda a, ut: f'{a} ^ 0x80000000u')
        if A == 1 and op in (0b0110, 0b0111) and not bit(w, 10):  # VABS / VNEG integer
            st = LANE_T[size][1]
            if op == 0b0110:
                return self.map_lanes(d, m, Q, size, lambda a, ut: f'({ut})(({st}){a} < 0 ? -({st}){a} : ({st}){a})')
            return self.map_lanes(d, m, Q, size, lambda a, ut: f'({ut})(-({st}){a})')
        if A == 3 and bits(w, 9, 2) == 3 and size == 32:  # VCVT float <-> integer
            opb = bits(w, 7, 2)
            k = 4 if Q else 2
            out = []
            for i in range(k):
                s = f'c->v.s[{m * 2 + i}]'
                if opb == 0:
                    e = f'f2u((float)(int32_t){s})'
                elif opb == 1:
                    e = f'f2u((float){s})'
                elif opb == 2:
                    e = f'vcvt_s32_f32(u2f({s}))'
                else:
                    e = f'vcvt_u32_f32(u2f({s}))'
                out.append(f'uint32_t t{i} = {e};')
            out += [f'c->v.s[{d * 2 + i}] = t{i};' for i in range(k)]
            return '{ ' + ' '.join(out) + ' }'
        if A == 3 and bits(w, 8, 3) == 0b101 and size == 32:  # VRECPE / VRSQRTE float
            fn = 'vrsqrte_f' if bit(w, 7) else 'vrecpe_f'
            if not bit(w, 8):
                raise U()('vrecpe int')
            return self.map_lanes(d, m, Q, 32, lambda a, ut: f'f2u({fn}(u2f({a})))')
        if A == 1 and op in (0, 1, 2, 3, 4) and bit(w, 10) and size == 32:  # VCGT / VCGE / VCEQ / VCLE / VCLT #0 (float)
            cmp = ['>', '>=', '==', '<=', '<'][op]
            return self.map_lanes(d, m, Q, 32, lambda a, ut: f'(nftz(u2f({a})) {cmp} 0.0f) ? 0xFFFFFFFFu : 0u')
        if A == 1 and op in (0, 1, 2, 3, 4) and not bit(w, 10):  # ... #0 (integer)
            cmp = ['>', '>=', '==', '<=', '<'][op]
            st = LANE_T[size][1]
            return self.map_lanes(d, m, Q, size, lambda a, ut: f'(({st}){a} {cmp} 0) ? ({ut})~0u : 0')
        if A == 2 and op in (0b0010, 0b0011):  # VUZP (op 0010) / VZIP (op 0011)
            return self.vuzp_zip(d, m, Q, size, op == 0b0011)
        if A == 2 and op in (0b0100, 0b0101) and not bit(w, 6):  # VMOVN
            return self.vmovn(d, m, size)
        if A == 2 and op == 0b0001:  # VTRN
            return self.vtrn(d, m, Q, size)
        raise U()(f'neon 2misc {w:08x}')

    def map_lanes(self, d, m, Q, size, f):
        ut = LANE_T[size][0]
        cnt = (128 if Q else 64) // size
        out = [f'{ut} t{i} = {f(lane(m, i, size), ut)};' for i in range(cnt)]
        out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def vmovn(self, d, m, size):
        ut = LANE_T[size][0]
        wt = LANE_T[size * 2][0]
        cnt = 64 // size
        out = [f'{ut} t{i} = ({ut}){lane(m, i, size * 2)};' for i in range(cnt)]
        out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def vuzp_zip(self, d, m, Q, size, zip_):
        ut = LANE_T[size][0]
        cnt = (128 if Q else 64) // size
        a = [lane(d, i, size) for i in range(cnt)]
        b = [lane(m, i, size) for i in range(cnt)]
        both = a + b
        if zip_:  # interleave: d gets the low half, m the high half
            res = []
            for i in range(cnt):
                res += [a[i], b[i]]
        else:  # de-interleave: d gets the even elements, m the odd ones
            res = [both[2 * i] for i in range(cnt)] + [both[2 * i + 1] for i in range(cnt)]
        out = [f'{ut} t{i} = {res[i]};' for i in range(2 * cnt)]
        out += [f'{a[i]} = t{i};' for i in range(cnt)] + [f'{b[i]} = t{cnt + i};' for i in range(cnt)]
        return '{ ' + ' '.join(out) + ' }'

    def vtrn(self, d, m, Q, size):
        ut = LANE_T[size][0]
        cnt = (128 if Q else 64) // size
        out = []
        for i in range(0, cnt, 2):
            out.append(f'{{ {ut} a = {lane(d, i + 1, size)}, b = {lane(m, i, size)}; '
                       f'{lane(d, i + 1, size)} = b; {lane(m, i, size)} = a; }}')
        return '{ ' + ' '.join(out) + ' }'

    def shift_imm(self, w):
        """VSHR / VSRA / VSHL / VSHLL / VSHRN / VMOVL by immediate (L:imm6 encodes size and amount)."""
        Ubit = bit(w, 24)
        L = bit(w, 7)
        imm6 = bits(w, 16, 6)
        A = bits(w, 8, 4)
        Q = bit(w, 6)
        d, m = reg(bits(w, 12, 4), bit(w, 22)), reg(bits(w, 0, 4), bit(w, 5))
        if L:
            size = 64
            shift = imm6
        else:
            if imm6 >= 32:
                size = 32
            elif imm6 >= 16:
                size = 16
            elif imm6 >= 8:
                size = 8
            else:
                raise U()('neon shift size')
            shift = imm6 - size
        ut, st, _ = LANE_T[size]
        if A == 0 or A == 1:  # VSHR / VSRA: amount = size - shift
            amt = size - shift if not L else 64 - imm6
            acc = A == 1
            if Ubit:
                f = (lambda a, ut_: f'({ut})({a} >> {amt})') if amt < size else (lambda a, ut_: '0')
            else:
                f = lambda a, ut_: f'({ut})(({st}){a} >> {min(amt, size - 1)})'
            if not acc:
                return self.map_lanes(d, m, Q, size, f)
            cnt = (128 if Q else 64) // size
            out = [f'{ut} t{i} = ({ut})({lane(d, i, size)} + {f(lane(m, i, size), ut)});' for i in range(cnt)]
            out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
            return '{ ' + ' '.join(out) + ' }'
        if A == 5 and not Ubit:  # VSHL immediate
            amt = shift if not L else imm6
            return self.map_lanes(d, m, Q, size, lambda a, ut_: f'({ut})((uint64_t){a} << {amt})')
        if A == 10 and not L and not Q:  # VSHLL / VMOVL: double width
            wide = size * 2
            wt = LANE_T[wide][0]
            src_t = LANE_T[size][0] if Ubit else LANE_T[size][1]
            cnt = 64 // size
            out = [f'{wt} t{i} = ({wt})(({LANE_T[wide][1]})({src_t}){lane(m, i, size)} << {shift});' for i in range(cnt)]
            out += [f'{lane(d, i, wide)} = t{i};' for i in range(cnt)]
            return '{ ' + ' '.join(out) + ' }'
        if A == 8 and not Ubit and not L and not bit(w, 6):  # VSHRN: imm6 encodes the narrow size here
            wide = size * 2
            amt = wide - imm6
            ut_n = LANE_T[size][0]
            cnt = 64 // size
            out = [f'{ut_n} t{i} = ({ut_n})({lane(m, i, wide)} >> {amt});' for i in range(cnt)]
            out += [f'{lane(d, i, size)} = t{i};' for i in range(cnt)]
            return '{ ' + ' '.join(out) + ' }'
        raise U()(f'neon shift {w:08x}')

    # ------------------------------------------------------------ loads / stores
    def ldst(self, w, pc):
        from lifter import R
        Abit, L = bit(w, 23), bit(w, 21)
        rn, rm = bits(w, 16, 4), bits(w, 0, 4)
        D = bit(w, 22)
        vd = reg(bits(w, 12, 4), D)
        base = R(rn, pc)
        lines = [f'uint32_t ea = {base};']
        if not Abit:
            typ = bits(w, 8, 4)
            size = 8 << bits(w, 6, 2)
            # (elements per structure, d registers per element, register step between elements)
            info = {0b0111: (1, 1, 1), 0b1010: (1, 2, 1), 0b0110: (1, 3, 1), 0b0010: (1, 4, 1),
                    0b1000: (2, 1, 1), 0b1001: (2, 1, 2), 0b0011: (2, 2, 2),
                    0b0100: (3, 1, 1), 0b0101: (3, 1, 2), 0b0000: (4, 1, 1), 0b0001: (4, 1, 2)}.get(typ)
            if info is None:
                raise U()('neon ldst type')
            nelem, per_elem, step = info
            if nelem == 1:  # VLD1 / VST1: consecutive registers, contiguous memory
                for i in range(per_elem):
                    r = (vd + i) % 32
                    lines.append(f'c->v.d[{r}] = LD64(ea + {8 * i}u);' if L else f'ST64(ea + {8 * i}u, c->v.d[{r}]);')
                nbytes = 8 * per_elem
            else:
                # element j of structure k lives at ea + (k * nelem + j) * esize, in register vd + j * step + k / per
                esz = size // 8
                per = 64 // size
                for k in range(per * per_elem):
                    for j in range(nelem):
                        r = (vd + j * step + k // per) % 32
                        off = (k * nelem + j) * esz
                        ln = lane(r, k % per, size)
                        lines.append(f'{ln} = LD{size}(ea + {off}u);' if L else f'ST{size}(ea + {off}u, {ln});')
                nbytes = 8 * nelem * per_elem
        else:
            size_bits = bits(w, 10, 2)
            nelem = bits(w, 8, 2) + 1
            if size_bits == 3:  # load to all lanes
                if not L:
                    raise U()('vst all lanes')
                size = 8 << bits(w, 6, 2)
                T = bit(w, 5)
                inc = 2 if T and nelem > 1 else 1
                nregs = (2 if T else 1) if nelem == 1 else 1
                ut = LANE_T[size][0]
                esz = size // 8
                for j in range(nelem):
                    regs = [(vd + j * inc) % 32] if nelem > 1 else [(vd + i) % 32 for i in range(nregs)]
                    lines.append(f'{{ {ut} v = LD{size}(ea + {j * esz}u);')
                    for r in regs:
                        for k in range(64 // size):
                            lines.append(f'{lane(r, k, size)} = v;')
                    lines.append('}')
                nbytes = nelem * esz
            else:
                size = 8 << size_bits
                idx_bits = bits(w, 4, 4)
                idx = idx_bits >> (size_bits + 1)
                inc = 1
                if size == 16 and (idx_bits & 2):
                    inc = 2
                if size == 32 and (idx_bits & 4):
                    inc = 2
                esz = size // 8
                for j in range(nelem):
                    r = (vd + j * inc) % 32
                    ln = lane(r, idx, size)
                    lines.append(f'{ln} = LD{size}(ea + {j * esz}u);' if L else f'ST{size}(ea + {j * esz}u, {ln});')
                nbytes = nelem * esz
        if rm == 13:
            lines.append(f'c->r[{rn}] = ea + {nbytes}u;')
        elif rm != 15:
            lines.append(f'c->r[{rn}] = ea + {R(rm, pc)};')
        return '{ ' + ' '.join(lines) + ' }'
