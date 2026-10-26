"""AArch32 (A32 + VFP + Advanced SIMD) -> C static recompiler: one instruction at a time.

Each instruction becomes an Insn: C statements plus, for control flow, the kind of transfer and its target. The
generator (gen.py) strings them into C functions. Registers live in the Cpu struct (runtime/cpu.h); reads of the
pc are constants (instruction address + 8), and loads from the read-only text (literal pools) are folded into
constants at generation time.
"""
import struct

from neon import Neon

COND_NAMES = ['EQ', 'NE', 'CS', 'CC', 'MI', 'PL', 'VS', 'VC', 'HI', 'LS', 'GE', 'LT', 'GT', 'LE', 'AL', 'NV']


class Unsupported(Exception):
    pass


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def bit(w, i):
    return (w >> i) & 1


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def hx(v):
    return f'0x{v & 0xFFFFFFFF:x}u'


def ror32(v, r):
    r &= 31
    return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v


def cond_expr(cond):
    return None if cond >= 14 else f'COND_{COND_NAMES[cond]}'


class Insn:
    """One lifted instruction.

    kind: 'op' (straight-line code), 'b' (direct jump), 'bl' (direct call), 'blr' (indirect call), 'br' (indirect
    jump: c->pc set by code), 'ret' (return to caller), 'trap'.
    code: statements run first (for 'br' they set c->pc; for 'blr' too). cond: C condition or None. target: address
    for 'b' / 'bl'.
    """
    __slots__ = ('kind', 'code', 'target', 'cond', 'reads_pc_table')

    def __init__(self, kind, code='', target=None, cond=None):
        self.kind = kind
        self.code = code
        self.target = target
        self.cond = cond
        self.reads_pc_table = None


def R(n, pc):
    """Value of register n as read by the instruction at pc."""
    return hx(pc + 8) if n == 15 else f'c->r[{n}]'


def W(n, expr):
    return f'c->r[{n}] = {expr};'


class Lifter:
    def __init__(self, image, base, text_lo, text_hi):
        self.image = image          # module image (bytes-like), offsets from base
        self.base = base            # guest address of the module
        self.text_lo, self.text_hi = text_lo, text_hi  # absolute text range (read-only: literals fold)
        self.neon = Neon()
        self.prev_mov_lr_pc = False

    def word_at(self, a):
        return struct.unpack_from('<I', self.image, a - self.base)[0]

    def const_word(self, a):
        """Literal pool load: the word at a if it is in the read-only text, else None."""
        if self.text_lo <= a and a + 4 <= self.text_hi:
            return self.word_at(a)
        return None

    def lift(self, w, pc):
        cond = bits(w, 28, 4)
        if cond == 15:
            ins = self.uncond(w, pc)
        else:
            ins = self.conditional(w, pc)
            c = cond_expr(cond)
            if c:
                if ins.cond:
                    ins.cond = f'({c}) && ({ins.cond})'
                else:
                    ins.cond = c
        return ins

    # ================================================================ top level
    def conditional(self, w, pc):
        op1 = bits(w, 25, 3)
        if op1 in (0, 1):
            return self.dp_misc(w, pc)
        if op1 == 2 or (op1 == 3 and not bit(w, 4)):
            return self.ldst_word_byte(w, pc)
        if op1 == 3:
            return self.media(w, pc)
        if op1 == 4:
            return self.ldm_stm(w, pc)
        if op1 == 5:
            t = (pc + 8 + (sext(bits(w, 0, 24), 24) << 2)) & 0xFFFFFFFF
            return Insn('bl' if bit(w, 24) else 'b', target=t)
        return self.coproc_svc(w, pc)

    def uncond(self, w, pc):
        if bits(w, 25, 7) == 0b1111101:  # BLX imm: switches to Thumb
            return Insn('trap', f'rt_thumb(c, {hx(pc)});')
        if bits(w, 25, 7) == 0b1111001:  # Advanced SIMD data processing
            return Insn('op', self.neon.dp(w, pc))
        if bits(w, 24, 8) == 0b11110100 and not bit(w, 20):  # Advanced SIMD element / structure load/store
            return Insn('op', self.neon.ldst(w, pc))
        if (w & 0xFFFFFFF0) == 0xF57FF010 + 0x10 * 0 and bits(w, 4, 4) == 1:  # CLREX
            return Insn('op', 'c->excl_addr = ~0u;')
        if (w & 0xFFFFFF00) == 0xF57FF000:
            op = bits(w, 4, 4)
            if op == 1:
                return Insn('op', 'c->excl_addr = ~0u;')
            if op in (4, 5):  # DSB / DMB
                return Insn('op', '__atomic_thread_fence(__ATOMIC_SEQ_CST);')
            if op == 6:  # ISB
                return Insn('op', '/* isb */')
        if bits(w, 26, 6) == 0b111101 and bits(w, 20, 3) in (0b001, 0b101) and bits(w, 12, 4) == 15:  # PLD/PLI
            return Insn('op', '/* pld */')
        if bits(w, 24, 8) == 0b11111110 and bits(w, 9, 3) == 0b101 and not bit(w, 4):  # VFPv5 (ARMv8) extras
            return Insn('op', self.vfp_v8(w, pc))
        if bits(w, 20, 12) == 0xF10:  # CPS / SETEND
            return Insn('op', '/* cps/setend */')
        raise Unsupported(f'uncond {w:08x}')

    # ================================================================ shifter operands
    def imm_operand(self, w):
        rot = bits(w, 8, 4) * 2
        v = ror32(bits(w, 0, 8), rot)
        carry = str(v >> 31) if rot else None
        return hx(v), carry

    def reg_operand(self, w, pc, want_carry):
        """Register operand (immediate or register shift). Returns (setup, expr, carry) where setup holds
        statements that compute temporaries (carry must be captured before any register is written)."""
        rm = bits(w, 0, 4)
        typ = bits(w, 5, 2)
        m = R(rm, pc)
        if not bit(w, 4):  # immediate shift
            amt = bits(w, 7, 5)
            if typ == 0:
                if amt == 0:
                    return '', m, None
                return '', f'({m} << {amt})', f'(({m} >> {32 - amt}) & 1)'
            if typ == 1:
                if amt == 0:
                    return '', '0u', f'({m} >> 31)'
                return '', f'({m} >> {amt})', f'(({m} >> {amt - 1}) & 1)'
            if typ == 2:
                if amt == 0:
                    return '', f'(uint32_t)((int32_t){m} >> 31)', f'({m} >> 31)'
                return '', f'(uint32_t)((int32_t){m} >> {amt})', f'(({m} >> {amt - 1}) & 1)'
            if amt == 0:  # RRX
                return '', f'((c->cf << 31) | ({m} >> 1))', f'({m} & 1)'
            return '', f'ROR32({m}, {amt})', f'(({m} >> {amt - 1}) & 1)'
        # register-specified shift
        rs = bits(w, 8, 4)
        s = R(rs, pc)
        name = ['lsl', 'lsr', 'asr', 'ror'][typ]
        if want_carry:
            return '', f'shc_{name}(c, {m}, {s})', 'SHC'
        return '', f'sh_{name}({m}, {s})', None

    # ================================================================ data processing / misc
    def dp_misc(self, w, pc):
        I = bit(w, 25)
        op = bits(w, 20, 5)
        if not I:
            b4, b7 = bit(w, 4), bit(w, 7)
            if (op & 0b11001) == 0b10000:  # 10xx0: misc / halfword multiply
                if b7 and not b4:
                    return self.halfword_mul(w, pc)
                if not b7:
                    return self.misc(w, pc)
            if b4 and b7:
                if bits(w, 5, 2) == 0:
                    if bits(w, 24, 1) == 0:
                        return self.multiply(w, pc)
                    return self.sync(w, pc)
                return self.extra_ldst(w, pc)
            return self.data_proc(w, pc)
        if op == 0b10000:  # MOVW
            imm = (bits(w, 16, 4) << 12) | bits(w, 0, 12)
            return Insn('op', W(bits(w, 12, 4), hx(imm)))
        if op == 0b10100:  # MOVT
            imm = (bits(w, 16, 4) << 12) | bits(w, 0, 12)
            rd = bits(w, 12, 4)
            return Insn('op', W(rd, f'(c->r[{rd}] & 0xFFFFu) | {hx(imm << 16)}'))
        if (op & 0b11011) == 0b10010:  # MSR imm / hints
            if op == 0b10010 and bits(w, 16, 4) == 0:
                return Insn('op', '/* hint */')
            mask = bits(w, 18, 2)
            v, _ = self.imm_operand(w)
            return Insn('op', f'apsr_set(c, {v}, {mask >> 1}, {mask & 1});')
        return self.data_proc(w, pc)

    def data_proc(self, w, pc):
        I, opc, S = bit(w, 25), bits(w, 21, 4), bit(w, 20)
        rn, rd = bits(w, 16, 4), bits(w, 12, 4)
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        need_c = bool(S and logical)
        if I:
            setup, b, carry = '', *self.imm_operand(w)
        else:
            setup, b, carry = self.reg_operand(w, pc, need_c)
        a = R(rn, pc)
        if rd == 15 and not S and opc in (13, 4) and not I and not bit(w, 4):
            pass  # (handled by the generic pc-write path below)
        expr = {
            0: f'{a} & {b}', 1: f'{a} ^ {b}', 2: f'{a} - {b}', 3: f'{b} - {a}', 4: f'{a} + {b}',
            5: f'{a} + {b} + c->cf', 6: f'{a} - {b} - 1u + c->cf', 7: f'{b} - {a} - 1u + c->cf',
            8: f'{a} & {b}', 9: f'{a} ^ {b}', 10: None, 11: None,
            12: f'{a} | {b}', 13: b, 14: f'{a} & ~{b}', 15: f'~{b}',
        }[opc]
        lines = []
        if setup:
            lines.append(setup)
        if S and not logical:
            fn = {2: ('f_subs', a, b), 3: ('f_subs', b, a), 4: ('f_adds', a, b), 5: ('f_adcs', a, b),
                  6: ('f_sbcs', a, b), 7: ('f_sbcs', b, a), 10: ('f_subs', a, b), 11: ('f_adds', a, b)}[opc]
            val = f'{fn[0]}(c, {fn[1]}, {fn[2]})'
            if opc in (10, 11):
                return Insn('op', f'(void){val};')
            if rd == 15:
                raise Unsupported('flag-setting write to pc')
            return Insn('op', W(rd, val))
        if S:  # logical, flag-setting
            parts = []
            if carry == 'SHC':  # the shc_* helper sets c->cf itself
                parts.append(f'uint32_t r = {expr};')
            elif carry is not None:
                parts.append(f'uint32_t sc = {carry}; uint32_t r = {expr};')
                parts.append('c->cf = sc;')
            else:
                parts.append(f'uint32_t r = {expr};')
            parts.append('c->nf = r >> 31; c->zf = r == 0;')
            if opc not in (8, 9):
                if rd == 15:
                    raise Unsupported('flag-setting write to pc')
                parts.append(W(rd, 'r'))
            return Insn('op', '{ ' + ' '.join(parts) + ' }')
        if opc in (8, 9, 10, 11):
            raise Unsupported('test without S')
        if rd == 15:
            return self.write_pc(w, pc, expr, opc, rn, I)
        return Insn('op', W(rd, expr))

    def write_pc(self, w, pc, expr, opc, rn, I):
        """Data-processing write to the pc: return (mov pc, lr), or an indirect jump."""
        rm = bits(w, 0, 4)
        if opc == 13 and not I and rm == 14 and bits(w, 4, 8) == 0:
            return Insn('ret')
        if opc == 13 and not I and bits(w, 4, 8) == 0 and self.prev_mov_lr_pc:
            return Insn('blr', f'c->pc = {expr};')
        ins = Insn('br', f'c->pc = {expr};')
        return ins

    def misc(self, w, pc):
        op, op2 = bits(w, 21, 2), bits(w, 4, 3)
        rd, rm = bits(w, 12, 4), bits(w, 0, 4)
        if op2 == 0:
            if op & 1:  # MSR (register)
                mask = bits(w, 18, 2)
                return Insn('op', f'apsr_set(c, {R(rm, pc)}, {mask >> 1}, {mask & 1});')
            return Insn('op', W(rd, 'apsr_get(c)'))  # MRS
        if op2 == 1:
            if op == 1:  # BX
                if rm == 14:
                    return Insn('ret')
                if self.prev_mov_lr_pc:
                    return Insn('blr', f'c->pc = {R(rm, pc)};')
                return Insn('br', f'c->pc = {R(rm, pc)};')
            if op == 3:
                return Insn('op', W(rd, f'clz32({R(rm, pc)})'))
        if op2 == 3 and op == 1:  # BLX (register)
            return Insn('blr', f'c->pc = {R(rm, pc)};')
        if op2 == 5:  # QADD / QSUB / QDADD / QDSUB
            rn = bits(w, 16, 4)
            a, b = R(rm, pc), R(rn, pc)
            e = {0: f'qadd32(c, {a}, {b})', 1: f'qsub32(c, {a}, {b})',
                 2: f'qadd32(c, {a}, qadd32(c, {b}, {b}))', 3: f'qsub32(c, {a}, qadd32(c, {b}, {b}))'}[op]
            return Insn('op', W(rd, e))
        if op2 == 4:  # CRC32{B,H,W} / CRC32C{B,H,W} (ARMv8): sz = op
            rn = bits(w, 16, 4)
            tab = 'rt_crc32c_tab' if bit(w, 9) else 'rt_crc32_tab'
            return Insn('op', W(rd, f'crc32_n({R(rn, pc)}, {R(rm, pc)}, {1 << op}, {tab})'))
        if op2 == 7 and op == 1:  # BKPT
            return Insn('trap', f'rt_bkpt(c, {(bits(w, 8, 12) << 4) | bits(w, 0, 4)}, {hx(pc)});')
        raise Unsupported(f'misc {w:08x}')

    def halfword_mul(self, w, pc):
        op = bits(w, 21, 2)
        rd, ra, rm, rn = bits(w, 16, 4), bits(w, 12, 4), bits(w, 8, 4), bits(w, 0, 4)
        x, y = bit(w, 5), bit(w, 6)
        hn = f'(int32_t)(int16_t)({R(rn, pc)} >> {16 if x else 0})'
        hm = f'(int32_t)(int16_t)({R(rm, pc)} >> {16 if y else 0})'
        if op == 0:  # SMLAxy
            return Insn('op', '{ int64_t p = (int64_t)(' + hn + ' * ' + hm + ') + (int32_t)' + R(ra, pc) + '; '
                        'if (p != (int32_t)p) c->qf = 1; ' + W(rd, '(uint32_t)p') + ' }')
        if op == 1:
            hm_w = f'(int32_t)(int16_t)({R(rm, pc)} >> {16 if y else 0})'
            prod = f'(int32_t)(((int64_t)(int32_t){R(rn, pc)} * {hm_w}) >> 16)'
            if not x:  # SMLAWy
                return Insn('op', '{ int64_t p = (int64_t)' + prod + ' + (int32_t)' + R(ra, pc) + '; '
                            'if (p != (int32_t)p) c->qf = 1; ' + W(rd, '(uint32_t)p') + ' }')
            return Insn('op', W(rd, f'(uint32_t){prod}'))  # SMULWy
        if op == 2:  # SMLALxy: rdhi=rd, rdlo=ra
            return Insn('op', '{ uint64_t acc = ((uint64_t)' + R(rd, pc) + ' << 32) | ' + R(ra, pc) + '; '
                        'acc += (uint64_t)(int64_t)(' + hn + ' * ' + hm + '); ' + W(ra, '(uint32_t)acc') + ' '
                        + W(rd, '(uint32_t)(acc >> 32)') + ' }')
        return Insn('op', W(rd, f'(uint32_t)({hn} * {hm})'))  # SMULxy

    def multiply(self, w, pc):
        op, S = bits(w, 21, 3), bit(w, 20)
        rd, ra, rm, rn = bits(w, 16, 4), bits(w, 12, 4), bits(w, 8, 4), bits(w, 0, 4)
        a, b = R(rn, pc), R(rm, pc)
        nz = ' c->nf = r >> 31; c->zf = r == 0;' if S else ''
        if op == 0:
            return Insn('op', f'{{ uint32_t r = {a} * {b};{nz} {W(rd, "r")} }}')
        if op == 1:
            return Insn('op', f'{{ uint32_t r = {a} * {b} + {R(ra, pc)};{nz} {W(rd, "r")} }}')
        if op == 2:  # UMAAL
            return Insn('op', f'{{ uint64_t r = (uint64_t){a} * {b} + {R(ra, pc)} + {R(rd, pc)}; '
                              f'{W(ra, "(uint32_t)r")} {W(rd, "(uint32_t)(r >> 32)")} }}')
        if op == 3:  # MLS
            return Insn('op', W(rd, f'{R(ra, pc)} - {a} * {b}'))
        # long multiplies: rdhi = rd, rdlo = ra
        nz64 = ' c->nf = (uint32_t)(r >> 63); c->zf = r == 0;' if S else ''
        if op == 4:
            e = f'(uint64_t){a} * {b}'
        elif op == 5:
            e = f'(uint64_t){a} * {b} + (((uint64_t){R(rd, pc)} << 32) | {R(ra, pc)})'
        elif op == 6:
            e = f'(uint64_t)((int64_t)(int32_t){a} * (int32_t){b})'
        else:
            e = f'(uint64_t)((int64_t)(int32_t){a} * (int32_t){b}) + (((uint64_t){R(rd, pc)} << 32) | {R(ra, pc)})'
        return Insn('op', f'{{ uint64_t r = {e};{nz64} {W(ra, "(uint32_t)r")} {W(rd, "(uint32_t)(r >> 32)")} }}')

    def sync(self, w, pc):
        rn, rt, rt2 = bits(w, 16, 4), bits(w, 12, 4), bits(w, 0, 4)
        if bits(w, 23, 1) == 0:  # SWP / SWPB (deprecated)
            B = bit(w, 22)
            if B:
                return Insn('op', f'{{ uint32_t a = {R(rn, pc)}; uint32_t v = __atomic_exchange_n((uint8_t *)G2H(a), '
                                  f'(uint8_t){R(rt2, pc)}, __ATOMIC_SEQ_CST); {W(rt, "v")} }}')
            return Insn('op', f'{{ uint32_t a = {R(rn, pc)}; uint32_t v = __atomic_exchange_n((uint32_t *)G2H(a), '
                              f'{R(rt2, pc)}, __ATOMIC_SEQ_CST); {W(rt, "v")} }}')
        L, sz, kind = bit(w, 20), bits(w, 21, 2), bits(w, 8, 2)
        a = R(rn, pc)
        if kind == 0:  # LDA / STL (ARMv8)
            T = {0: ('32', 'uint32_t'), 2: ('8', 'uint8_t'), 3: ('16', 'uint16_t')}.get(sz)
            if not T:
                raise Unsupported('lda size')
            if L:
                return Insn('op', W(rt, f'LDA({T[0]}, {T[1]}, {a})'))
            return Insn('op', f'STL({T[0]}, {T[1]}, {a}, {R(rt2, pc)});')
        # exclusive (kind 3: LDREX/STREX, kind 2: LDAEX/STLEX: same with acquire/release, our helpers are seq_cst)
        if L:
            if sz == 1:  # LDREXD rt, rt+1
                return Insn('op', f'{{ uint64_t v = ldrexd(c, {a}); {W(rt, "(uint32_t)v")} {W(rt + 1, "(uint32_t)(v >> 32)")} }}')
            n = {0: 32, 2: 8, 3: 16}[sz]
            return Insn('op', W(rt, f'ldrex{n}(c, {a})'))
        rd = rt  # status register
        if sz == 1:
            return Insn('op', W(rd, f'strexd(c, {a}, ((uint64_t){R(rt2 + 1, pc)} << 32) | {R(rt2, pc)})'))
        n = {0: 32, 2: 8, 3: 16}[sz]
        return Insn('op', W(rd, f'strex{n}(c, {a}, {R(rt2, pc)})'))

    # ================================================================ loads / stores
    def addr_mode(self, w, pc, offset_expr, rn):
        """Effective address and writeback for P/U/W addressing. Returns (ea_expr, wb_expr or None)."""
        P, U, Wb = bit(w, 24), bit(w, 23), bit(w, 21)
        base = R(rn, pc)
        if offset_expr == '0u':
            off = base
        else:
            off = f'{base} {"+" if U else "-"} {offset_expr}'
        if P:
            return off, ('ea' if Wb else None)
        return base, off  # post-indexed always writes back

    def ldst_word_byte(self, w, pc):
        I, P, U, B, Wb, L = bit(w, 25), bit(w, 24), bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 20)
        rn, rt = bits(w, 16, 4), bits(w, 12, 4)
        if not I:
            imm = bits(w, 0, 12)
            off = hx(imm) if imm else '0u'
            if rn == 15 and P and not Wb:  # literal
                a = (pc + 8 + (imm if U else -imm)) & 0xFFFFFFFF
                if L:
                    v = self.const_word(a & ~3) if (a & 3) == 0 else None
                    if v is not None:
                        if B:
                            v = self.image[a - self.base]
                        if rt == 15:
                            return Insn('br', f'c->pc = {hx(v)};')
                        return Insn('op', W(rt, hx(v)))
        else:
            _, off, _ = self.reg_operand(w & ~0x10, pc, False)
        ea, wb = self.addr_mode(w, pc, off, rn)
        n = 8 if B else 32
        lines = [f'uint32_t ea = {ea};']
        if L:
            lines.append(f'uint32_t v = LD{n}(ea);')
            if wb and rn != 15:
                lines.append(W(rn, wb))
            if rt == 15:
                # ldr pc, [sp], #4 is a pop of the return address
                if rn == 13 and not P and U and not I and bits(w, 0, 12) == 4:
                    return Insn('ret', '{ ' + ' '.join(lines) + ' }')
                lines.append('c->pc = v;')
                kind = 'blr' if self.prev_mov_lr_pc else 'br'
                return Insn(kind, '{ ' + ' '.join(lines) + ' }')
            lines.append(W(rt, 'v'))
        else:
            v = R(rt, pc) if rt != 15 else hx(pc + 8)
            lines.append(f'ST{n}(ea, {v});')
            if wb and rn != 15:
                lines.append(W(rn, wb))
        return Insn('op', '{ ' + ' '.join(lines) + ' }')

    def extra_ldst(self, w, pc):
        P, U, I, Wb, L = bit(w, 24), bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 20)
        rn, rt = bits(w, 16, 4), bits(w, 12, 4)
        op2 = bits(w, 5, 2)
        if I:
            imm = (bits(w, 8, 4) << 4) | bits(w, 0, 4)
            off = hx(imm) if imm else '0u'
            if rn == 15 and P and not Wb and L:
                a = (pc + 8 + (imm if U else -imm)) & 0xFFFFFFFF
                if self.text_lo <= a and a + 8 <= self.text_hi:
                    raw = bytes(self.image[a - self.base:a - self.base + 8])
                    if op2 == 1:
                        return Insn('op', W(rt, hx(struct.unpack_from('<H', raw)[0])))
                    if op2 == 2:
                        return Insn('op', W(rt, hx(struct.unpack_from('<b', raw)[0])))
                    if op2 == 3:
                        return Insn('op', W(rt, hx(struct.unpack_from('<h', raw)[0])))
        else:
            off = R(bits(w, 0, 4), pc)
        ea, wb = self.addr_mode(w, pc, off, rn)
        lines = [f'uint32_t ea = {ea};']
        if op2 == 1:
            if L:
                lines.append('uint32_t v = LD16(ea);')
            else:
                lines.append(f'ST16(ea, {R(rt, pc)});')
        elif op2 == 2 and L:
            lines.append('uint32_t v = (uint32_t)(int32_t)(int8_t)LD8(ea);')
        elif op2 == 3 and L:
            lines.append('uint32_t v = (uint32_t)(int32_t)(int16_t)LD16(ea);')
        elif op2 == 2:  # LDRD
            lines.append('uint32_t v = LD32(ea), v2 = LD32(ea + 4);')
            if wb and rn != 15:
                lines.append(W(rn, wb))
            lines.append(W(rt, 'v'))
            lines.append(W(rt + 1, 'v2'))
            return Insn('op', '{ ' + ' '.join(lines) + ' }')
        else:  # STRD
            lines.append(f'ST32(ea, {R(rt, pc)}); ST32(ea + 4, {R(rt + 1, pc)});')
        if wb and rn != 15:
            lines.append(W(rn, wb))
        if L:
            lines.append(W(rt, 'v'))
        return Insn('op', '{ ' + ' '.join(lines) + ' }')

    def ldm_stm(self, w, pc):
        P, U, S, Wb, L = bit(w, 24), bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 20)
        rn = bits(w, 16, 4)
        regs = [i for i in range(16) if bit(w, i)]
        n = len(regs)
        if S:
            raise Unsupported('ldm/stm user registers')
        if not n:
            raise Unsupported('empty register list')
        base = R(rn, pc)
        if U:
            start = f'{base} + 4u' if P else base
            wbv = f'{base} + {4 * n}u'
        else:
            start = f'{base} - {4 * n}u' if P else f'{base} - {4 * n - 4}u'
            wbv = f'{base} - {4 * n}u'
        lines = [f'uint32_t ea = {start};']
        if L:
            loads = []
            for i, r in enumerate(regs):
                if r == 15:
                    loads.append(f'c->pc = LD32(ea + {4 * i}u);')
                else:
                    loads.append(W(r, f'LD32(ea + {4 * i}u)'))
            if Wb:
                lines.append(W(rn, wbv))
            lines += loads
            if 15 in regs:
                if rn == 13 and Wb and U and not P:  # pop {..., pc}
                    return Insn('ret', '{ ' + ' '.join(lines) + ' }')
                return Insn('br', '{ ' + ' '.join(lines) + ' }')
            return Insn('op', '{ ' + ' '.join(lines) + ' }')
        for i, r in enumerate(regs):
            lines.append(f'ST32(ea + {4 * i}u, {R(r, pc) if r != 15 else hx(pc + 8)});')
        if Wb:
            lines.append(W(rn, wbv))
        return Insn('op', '{ ' + ' '.join(lines) + ' }')

    # ================================================================ media
    def media(self, w, pc):
        op1, op2 = bits(w, 20, 5), bits(w, 5, 3)
        rd, rn, rm, ra = bits(w, 12, 4), bits(w, 16, 4), bits(w, 0, 4), bits(w, 8, 4)
        if (op1 & 0b11100) == 0b00000:  # parallel add/sub
            code = (bits(w, 20, 3) << 3) | op2
            return Insn('op', W(rd, f'rt_parallel(c, {code}, {R(rn, pc)}, {R(rm, pc)})'))
        if (op1 & 0b11000) == 0b01000:  # packing, unpacking, saturation, reversal
            return self.pack(w, pc)
        if (op1 & 0b11000) == 0b10000:  # signed multiplies, divide
            return self.signed_mul(w, pc)
        if op1 == 0b11000 and op2 == 0:
            rd2, ra2 = bits(w, 16, 4), bits(w, 12, 4)
            e = f'rt_usad8({R(bits(w, 0, 4), pc)}, {R(bits(w, 8, 4), pc)})'
            if ra2 == 15:
                return Insn('op', W(rd2, e))
            return Insn('op', W(rd2, f'{e} + {R(ra2, pc)}'))
        lsb, msb = bits(w, 7, 5), bits(w, 16, 5)
        if (op1 & 0b11110) == 0b11010 and (op2 & 3) == 2:  # SBFX
            width = msb + 1
            return Insn('op', W(rd, f'(uint32_t)(((int32_t)({R(rm, pc)} << {32 - lsb - width})) >> {32 - width})'))
        if (op1 & 0b11110) == 0b11110 and (op2 & 3) == 2:  # UBFX
            width = msb + 1
            mask = (1 << width) - 1
            return Insn('op', W(rd, f'({R(rm, pc)} >> {lsb}) & {hx(mask)}'))
        if (op1 & 0b11110) == 0b11100 and (op2 & 3) == 0:  # BFC / BFI
            if msb < lsb:
                raise Unsupported('bfi msb<lsb')
            mask = ((1 << (msb - lsb + 1)) - 1) << lsb
            if rm == 15:
                return Insn('op', W(rd, f'c->r[{rd}] & {hx(~mask)}'))
            return Insn('op', W(rd, f'(c->r[{rd}] & {hx(~mask)}) | (({R(rm, pc)} << {lsb}) & {hx(mask)})'))
        if op1 == 0b11111 and op2 == 7:
            return Insn('trap', f'rt_udf(c, {hx(pc)});')
        raise Unsupported(f'media {w:08x}')

    def pack(self, w, pc):
        op1, op2 = bits(w, 20, 3), bits(w, 5, 3)
        rd, rn, rm = bits(w, 12, 4), bits(w, 16, 4), bits(w, 0, 4)
        m = R(rm, pc)
        rot = bits(w, 10, 2) * 8
        mr = f'ROR32({m}, {rot})' if rot else m
        if op1 == 0 and (op2 & 1) == 0:  # PKHBT / PKHTB
            tb, imm = bit(w, 6), bits(w, 7, 5)
            if tb:
                sh = f'(uint32_t)((int32_t){m} >> {imm if imm else 31})'
                return Insn('op', W(rd, f'({R(rn, pc)} & 0xFFFF0000u) | ({sh} & 0xFFFFu)'))
            sh = f'({m} << {imm})' if imm else m
            return Insn('op', W(rd, f'({R(rn, pc)} & 0xFFFFu) | ({sh} & 0xFFFF0000u)'))
        if op1 == 0 and op2 == 3:  # SXTAB16 / SXTB16
            if rn == 15:
                return Insn('op', W(rd, f'sxtb16({mr})'))
            return Insn('op', f'{{ uint32_t x = sxtb16({mr}), n = {R(rn, pc)}; '
                              f'{W(rd, "((n + x) & 0xFFFFu) | (((n >> 16) + (x >> 16)) << 16)")} }}')
        if op1 == 0 and op2 == 5:
            return Insn('op', W(rd, f'rt_sel(c, {R(rn, pc)}, {m})'))
        if (op1 & 0b110) == 0b010 and (op2 & 1) == 0:  # SSAT
            sat, sh, imm = bits(w, 16, 5) + 1, bit(w, 6), bits(w, 7, 5)
            v = f'(int64_t)(int32_t){m}'
            if sh:
                v = f'(int64_t)((int32_t){m} >> {imm if imm else 31})'
            elif imm:
                v = f'(int64_t)(int32_t)({m} << {imm})'
            return Insn('op', W(rd, f'ssat32(c, {v}, {sat})'))
        if (op1 & 0b110) == 0b110 and (op2 & 1) == 0:  # USAT
            sat, sh, imm = bits(w, 16, 5), bit(w, 6), bits(w, 7, 5)
            v = f'(int64_t)(int32_t){m}'
            if sh:
                v = f'(int64_t)((int32_t){m} >> {imm if imm else 31})'
            elif imm:
                v = f'(int64_t)(int32_t)({m} << {imm})'
            return Insn('op', W(rd, f'usat32(c, {v}, {sat})'))
        if op1 == 0b010 and op2 == 1:  # SSAT16
            sat = bits(w, 16, 4) + 1
            return Insn('op', W(rd, f'(ssat32(c, (int16_t){m}, {sat}) & 0xFFFFu) | (ssat32(c, (int16_t)({m} >> 16), {sat}) << 16)'))
        if op1 == 0b110 and op2 == 1:  # USAT16
            sat = bits(w, 16, 4)
            return Insn('op', W(rd, f'(usat32(c, (int16_t){m}, {sat}) & 0xFFFFu) | (usat32(c, (int16_t)({m} >> 16), {sat}) << 16)'))
        ext = {(0b010, 3): '(uint32_t)(int32_t)(int8_t)', (0b011, 3): '(uint32_t)(int32_t)(int16_t)',
               (0b110, 3): '(uint32_t)(uint8_t)', (0b111, 3): '(uint32_t)(uint16_t)'}.get((op1, op2))
        if ext:
            if rn == 15:
                return Insn('op', W(rd, f'{ext}({mr})'))
            return Insn('op', W(rd, f'{R(rn, pc)} + {ext}({mr})'))
        if op1 == 0b100 and op2 == 3:  # UXTAB16 / UXTB16
            if rn == 15:
                return Insn('op', W(rd, f'uxtb16({mr})'))
            return Insn('op', f'{{ uint32_t x = uxtb16({mr}), n = {R(rn, pc)}; '
                              f'{W(rd, "((n + x) & 0xFFFFu) | (((n >> 16) + (x >> 16)) << 16)")} }}')
        if op1 == 0b011 and op2 == 1:
            return Insn('op', W(rd, f'__builtin_bswap32({m})'))
        if op1 == 0b011 and op2 == 5:
            return Insn('op', W(rd, f'rev16_32({m})'))
        if op1 == 0b111 and op2 == 1:
            return Insn('op', W(rd, f'rbit32({m})'))
        if op1 == 0b111 and op2 == 5:
            return Insn('op', W(rd, f'revsh32({m})'))
        raise Unsupported(f'pack {w:08x}')

    def signed_mul(self, w, pc):
        op1, op2 = bits(w, 20, 3), bits(w, 5, 3)
        rd, ra, rm, rn = bits(w, 16, 4), bits(w, 12, 4), bits(w, 8, 4), bits(w, 0, 4)
        n, m = R(rn, pc), R(rm, pc)
        if op1 == 1 and op2 == 0:
            return Insn('op', W(rd, f'sdiv32({n}, {m})'))
        if op1 == 3 and op2 == 0:
            return Insn('op', W(rd, f'udiv32({n}, {m})'))
        if op1 == 0:  # SMLAD / SMLSD / SMUAD / SMUSD (X: swap halves of m)
            mm = f'ROR32({m}, 16)' if bit(w, 5) else m
            lo = f'(int32_t)(int16_t){n} * (int32_t)(int16_t){mm}'
            hi = f'(int32_t)(int16_t)({n} >> 16) * (int32_t)(int16_t)({mm} >> 16)'
            sub = bit(w, 6)
            e = f'(int64_t)({lo}) {"-" if sub else "+"} (int64_t)({hi})'
            if ra != 15:
                e = f'{e} + (int32_t){R(ra, pc)}'
            return Insn('op', f'{{ int64_t p = {e}; if (p != (int32_t)p) c->qf = 1; {W(rd, "(uint32_t)p")} }}')
        if op1 == 4:  # SMLALD / SMLSLD: rdhi = rd, rdlo = ra
            mm = f'ROR32({m}, 16)' if bit(w, 5) else m
            lo = f'(int64_t)((int32_t)(int16_t){n} * (int32_t)(int16_t){mm})'
            hi = f'(int64_t)((int32_t)(int16_t)({n} >> 16) * (int32_t)(int16_t)({mm} >> 16))'
            e = f'{lo} {"-" if bit(w, 6) else "+"} {hi}'
            return Insn('op', f'{{ uint64_t acc = ((uint64_t){R(rd, pc)} << 32) | {R(ra, pc)}; acc += (uint64_t)({e}); '
                              f'{W(ra, "(uint32_t)acc")} {W(rd, "(uint32_t)(acc >> 32)")} }}')
        if op1 == 5:  # SMMUL / SMMLA / SMMLS (R: round)
            rnd = ' + 0x80000000ll' if bit(w, 5) else ''
            p = f'(int64_t)(int32_t){n} * (int32_t){m}'
            if op2 in (0, 1):
                if ra == 15:
                    return Insn('op', W(rd, f'(uint32_t)((uint64_t)({p}{rnd}) >> 32)'))
                return Insn('op', W(rd, f'(uint32_t)((uint64_t)(((int64_t)(int32_t){R(ra, pc)} << 32) + {p}{rnd}) >> 32)'))
            if op2 in (6, 7):
                return Insn('op', W(rd, f'(uint32_t)((uint64_t)(((int64_t)(int32_t){R(ra, pc)} << 32) - {p}{rnd}) >> 32)'))
        raise Unsupported(f'signed mul {w:08x}')

    # ================================================================ coprocessor / svc
    def coproc_svc(self, w, pc):
        if bits(w, 24, 4) == 0b1111:
            return Insn('op', f'rt_svc(c, {bits(w, 0, 24)}, {hx(pc)});')
        cp = bits(w, 8, 4)
        if cp in (10, 11):
            return Insn('op', self.vfp(w, pc))
        if cp == 15:
            return self.cp15(w, pc)
        raise Unsupported(f'coproc {cp} {w:08x}')

    def cp15(self, w, pc):
        rt = bits(w, 12, 4)
        if bits(w, 25, 3) == 0b111 and bit(w, 4):  # MRC / MCR
            L, opc1, crn, crm, opc2 = bit(w, 20), bits(w, 21, 3), bits(w, 16, 4), bits(w, 0, 4), bits(w, 5, 3)
            key = (opc1, crn, crm, opc2)
            if L:
                src = {(0, 13, 0, 3): 'c->tpidruro', (0, 13, 0, 2): 'c->tpidrurw',
                       (0, 14, 0, 0): '19200000u'}.get(key)
                if src is None:
                    raise Unsupported(f'mrc p15 {key}')
                return Insn('op', W(rt, src))
            if key == (0, 13, 0, 2):
                return Insn('op', f'c->tpidrurw = {R(rt, pc)};')
            if crn == 7:  # cache maintenance / legacy barriers
                return Insn('op', '__atomic_thread_fence(__ATOMIC_SEQ_CST);')
            raise Unsupported(f'mcr p15 {key}')
        if bits(w, 21, 7) == 0b1100010:  # MRRC / MCRR
            L, opc1, crm, rt2 = bit(w, 20), bits(w, 4, 4), bits(w, 0, 4), bits(w, 16, 4)
            if L and crm == 14 and opc1 in (0, 1):  # CNTPCT / CNTVCT
                return Insn('op', f'{{ uint64_t t = rt_ticks(); {W(rt, "(uint32_t)t")} {W(rt2, "(uint32_t)(t >> 32)")} }}')
            raise Unsupported(f'mrrc p15 {w:08x}')
        raise Unsupported(f'cp15 {w:08x}')

    # ================================================================ VFP
    @staticmethod
    def sreg(v, x):
        return (v << 1) | x

    @staticmethod
    def dreg(v, x):
        return (x << 4) | v

    def vfp(self, w, pc):
        dbl = bit(w, 8)
        op = bits(w, 25, 3)
        if op == 0b110:
            if bits(w, 21, 4) == 0b0010:  # 64-bit transfers
                return self.vfp_mov64(w, pc)
            return self.vfp_ldst(w, pc)
        if bits(w, 24, 4) == 0b1110:
            if bit(w, 4):
                return self.vfp_mov32(w, pc)
            return self.vfp_dp(w, pc)
        raise Unsupported(f'vfp {w:08x}')

    def S(self, n):
        return f'c->v.f[{n}]'

    def D(self, n):
        return f'c->v.df[{n}]'

    def vfp_mov64(self, w, pc):
        to_core, rt2, rt = bit(w, 20), bits(w, 16, 4), bits(w, 12, 4)
        m = self.sreg(bits(w, 0, 4), bit(w, 5)) if not bit(w, 8) else self.dreg(bits(w, 0, 4), bit(w, 5))
        if bit(w, 8):  # Dm <-> rt, rt2
            if to_core:
                return f'{{ uint64_t v = c->v.d[{m}]; {W(rt, "(uint32_t)v")} {W(rt2, "(uint32_t)(v >> 32)")} }}'
            return f'c->v.d[{m}] = ((uint64_t){R(rt2, pc)} << 32) | {R(rt, pc)};'
        if to_core:
            return f'{{ uint32_t a = c->v.s[{m}], b = c->v.s[{m + 1}]; {W(rt, "a")} {W(rt2, "b")} }}'
        return f'c->v.s[{m}] = {R(rt, pc)}; c->v.s[{m + 1}] = {R(rt2, pc)};'

    def vfp_ldst(self, w, pc):
        P, U, D, Wb, L = bit(w, 24), bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 20)
        rn, vd, imm8 = bits(w, 16, 4), bits(w, 12, 4), bits(w, 0, 8)
        dbl = bit(w, 8)
        first = self.dreg(vd, D) if dbl else self.sreg(vd, D)
        if P and not Wb:  # VLDR / VSTR
            off = imm8 * 4
            if rn == 15:
                a = ((pc + 8) & ~3) + (off if U else -off)
                if L and self.text_lo <= a and a + 8 <= self.text_hi:
                    if dbl:
                        v = struct.unpack_from('<Q', self.image, a - self.base)[0]
                        return f'c->v.d[{first}] = 0x{v:x}ull;'
                    return f'c->v.s[{first}] = {hx(self.word_at(a))};'
                ea = hx(a)
            else:
                ea = f'{R(rn, pc)} {"+" if U else "-"} {off}u' if off else R(rn, pc)
            if L:
                return f'c->v.d[{first}] = LD64({ea});' if dbl else f'c->v.s[{first}] = LD32({ea});'
            return f'ST64({ea}, c->v.d[{first}]);' if dbl else f'ST32({ea}, c->v.s[{first}]);'
        # VLDM / VSTM (IA with optional writeback, DB with writeback)
        if dbl:
            count = imm8 // 2
        else:
            count = imm8
        size = 8 if dbl else 4
        total = imm8 * 4
        base = R(rn, pc)
        lines = [f'uint32_t ea = {base}{" - " + str(total) + "u" if not U else ""};']
        for i in range(count):
            r = first + i
            if dbl:
                lines.append(f'c->v.d[{r}] = LD64(ea + {8 * i}u);' if L else f'ST64(ea + {8 * i}u, c->v.d[{r}]);')
            else:
                lines.append(f'c->v.s[{r}] = LD32(ea + {4 * i}u);' if L else f'ST32(ea + {4 * i}u, c->v.s[{r}]);')
        if Wb:
            lines.append(W(rn, f'{base} {"+" if U else "-"} {total}u'))
        return '{ ' + ' '.join(lines) + ' }'

    def vfp_mov32(self, w, pc):
        L, C, A, B = bit(w, 20), bit(w, 8), bits(w, 21, 3), bits(w, 5, 2)
        rt = bits(w, 12, 4)
        vn = bits(w, 16, 4)
        if not C:
            n = self.sreg(vn, bit(w, 7))
            if A == 0:
                if L:
                    return W(rt, f'c->v.s[{n}]')
                return f'c->v.s[{n}] = {R(rt, pc)};'
            if A == 7:
                if L:
                    if rt == 15:
                        return 'vfp_mrs_flags(c);'
                    return W(rt, 'c->fpscr')
                return f'c->fpscr = {R(rt, pc)};'
            raise Unsupported(f'vmov32 A={A}')
        d = self.dreg(vn, bit(w, 7))
        opc1, opc2 = bits(w, 21, 2), bits(w, 5, 2)
        if not L:
            if not bit(w, 23):  # VMOV Dd[x], Rt
                if opc1 & 2:
                    idx = ((opc1 & 1) << 2) | opc2
                    return f'c->v.b[{d * 8 + idx}] = (uint8_t){R(rt, pc)};'
                if opc2 & 1:
                    idx = ((opc1 & 1) << 1) | (opc2 >> 1)
                    return f'c->v.h[{d * 4 + idx}] = (uint16_t){R(rt, pc)};'
                if (opc2 & 3) == 0:
                    return f'c->v.s[{d * 2 + (opc1 & 1)}] = {R(rt, pc)};'
                raise Unsupported('vmov scalar')
            return self.neon.vdup_core(w, pc, R(rt, pc))
        U = bit(w, 23)
        if opc1 & 2:
            idx = ((opc1 & 1) << 2) | opc2
            t = 'uint8_t' if U else 'int8_t'
            return W(rt, f'(uint32_t)({t})c->v.b[{d * 8 + idx}]')
        if opc2 & 1:
            idx = ((opc1 & 1) << 1) | (opc2 >> 1)
            t = 'uint16_t' if U else 'int16_t'
            return W(rt, f'(uint32_t)({t})c->v.h[{d * 4 + idx}]')
        return W(rt, f'c->v.s[{d * 2 + (opc1 & 1)}]')

    def vfp_dp(self, w, pc):
        dbl = bit(w, 8)
        D, N, M = bit(w, 22), bit(w, 7), bit(w, 5)
        vd, vn, vm = bits(w, 12, 4), bits(w, 16, 4), bits(w, 0, 4)
        if dbl:
            d, n, m = self.dreg(vd, D), self.dreg(vn, N), self.dreg(vm, M)
            X = self.D
            T = 'double'
        else:
            d, n, m = self.sreg(vd, D), self.sreg(vn, N), self.sreg(vm, M)
            X = self.S
            T = 'float'
        opc1 = (bit(w, 23) << 2) | bits(w, 20, 2)
        o6 = bit(w, 6)
        a, b, r = X(n), X(m), X(d)
        if opc1 == 0:  # VMLA / VMLS
            return f'{r} = {r} {"-" if o6 else "+"} ({T})({a} * {b});'
        if opc1 == 1:  # VNMLS (o6=0) / VNMLA (o6=1)
            if o6:
                return f'{r} = -{r} - ({T})({a} * {b});'
            return f'{r} = -{r} + ({T})({a} * {b});'
        if opc1 == 2:
            return f'{r} = -({T})({a} * {b});' if o6 else f'{r} = {a} * {b};'
        if opc1 == 3:
            return f'{r} = {a} {"-" if o6 else "+"} {b};'
        if opc1 == 4 and not o6:
            return f'{r} = {a} / {b};'
        fma = 'fma' if dbl else 'fmaf'
        if opc1 == 5:  # VFNMS (o6=0): -d + n*m ; VFNMA (o6=1): -d - n*m
            if o6:
                return f'{r} = {fma}(-{a}, {b}, -{r});'
            return f'{r} = {fma}({a}, {b}, -{r});'
        if opc1 == 6:  # VFMA / VFMS
            if o6:
                return f'{r} = {fma}(-{a}, {b}, {r});'
            return f'{r} = {fma}({a}, {b}, {r});'
        if opc1 == 7:
            return self.vfp_other(w, pc, dbl, d, m, vd, vm, D, M)
        raise Unsupported(f'vfp dp {w:08x}')

    def vfp_other(self, w, pc, dbl, d, m, vd, vm, D, M):
        opc2, opc3 = bits(w, 16, 4), bits(w, 6, 2)
        X = self.D if dbl else self.S
        if not (opc3 & 1):  # VMOV immediate
            imm8 = (bits(w, 16, 4) << 4) | bits(w, 0, 4)
            sign = imm8 >> 7
            b6 = (imm8 >> 6) & 1
            if dbl:
                exp = ((b6 ^ 1) << 10) | ((0xFF if b6 else 0) << 2) | ((imm8 >> 4) & 3)
                v = (sign << 63) | (exp << 52) | ((imm8 & 15) << 48)
                return f'c->v.d[{d}] = 0x{v:x}ull;'
            exp = ((b6 ^ 1) << 7) | ((0x1F if b6 else 0) << 2) | ((imm8 >> 4) & 3)
            v = (sign << 31) | (exp << 23) | ((imm8 & 15) << 19)
            return f'c->v.s[{d}] = {hx(v)};'
        r, b = X(d), X(m)
        if opc2 == 0:
            if opc3 == 1:
                return f'c->v.d[{d}] = c->v.d[{m}];' if dbl else f'c->v.s[{d}] = c->v.s[{m}];'
            return f'{r} = {"fabs" if dbl else "fabsf"}({b});'
        if opc2 == 1:
            if opc3 == 1:
                return (f'c->v.d[{d}] = c->v.d[{m}] ^ 0x8000000000000000ull;' if dbl
                        else f'c->v.s[{d}] = c->v.s[{m}] ^ 0x80000000u;')
            return f'{r} = {"sqrt" if dbl else "sqrtf"}({b});'
        if opc2 in (2, 3):  # VCVTB / VCVTT (half <-> single); bit16: 0 = from half
            top = bit(w, 7)
            if dbl:
                raise Unsupported('vcvt half double')
            sd, sm = self.sreg(vd, D), self.sreg(vm, M)
            if opc2 == 2:
                return f'c->v.f[{sd}] = h2f((uint16_t)(c->v.s[{sm}] >> {16 if top else 0}));'
            if top:
                return f'c->v.s[{sd}] = (c->v.s[{sd}] & 0xFFFFu) | ((uint32_t)f2h(c->v.f[{sm}]) << 16);'
            return f'c->v.s[{sd}] = (c->v.s[{sd}] & 0xFFFF0000u) | f2h(c->v.f[{sm}]);'
        if opc2 in (4, 5):  # VCMP(E)
            other = ('0.0' if dbl else '0.0f') if opc2 == 5 else b
            return f'vfp_cmp_{"d" if dbl else "f"}(c, {r}, {other});'
        if opc2 == 6:  # VRINTR (opc3=1) / VRINTZ (opc3=3)
            if opc3 == 3:
                return f'{r} = {"trunc" if dbl else "truncf"}({b});'
            return f'{r} = ({"double" if dbl else "float"})vfp_round_mode(c, {b});'
        if opc2 == 7:
            if opc3 == 1:  # VRINTX
                return f'{r} = ({"double" if dbl else "float"})vfp_round_mode(c, {b});'
            # VCVT between double and single
            if dbl:
                sd = self.sreg(vd, D)
                return f'c->v.f[{sd}] = (float)c->v.df[{m}];'
            dd = self.dreg(vd, D)
            return f'c->v.df[{dd}] = (double)c->v.f[{m}];'
        if opc2 == 8:  # VCVT int -> float; source is a single register; bit7: signed
            sm = self.sreg(vm, M)
            src = f'(int32_t)c->v.s[{sm}]' if bit(w, 7) else f'c->v.s[{sm}]'
            return f'{r} = ({"double" if dbl else "float"}){src};'
        if opc2 in (10, 11, 14, 15):  # VCVT fixed point
            U = bit(w, 16)
            to_fixed = bit(w, 18)
            sx = bit(w, 7)
            size = 32 if sx else 16
            imm = (bits(w, 0, 4) << 1) | bit(w, 5)
            fbits = size - imm
            if dbl:
                reg = self.dreg(vd, D)
                if to_fixed:
                    fn = 'vcvt_fx_u' if U else 'vcvt_fx_s'
                    return f'c->v.d[{reg}] = {fn}(c->v.df[{reg}], {fbits}, {size});'
                src = f'(uint32_t)c->v.d[{reg}]'
                if size == 16:
                    src = f'(uint16_t)c->v.d[{reg}]' if U else f'(int16_t)c->v.d[{reg}]'
                elif not U:
                    src = f'(int32_t)(uint32_t)c->v.d[{reg}]'
                return f'c->v.df[{reg}] = ldexp((double){src}, -{fbits});'
            reg = self.sreg(vd, D)
            if to_fixed:
                fn = 'vcvt_fx_u' if U else 'vcvt_fx_s'
                v = f'{fn}((double)c->v.f[{reg}], {fbits}, {size})'
                if size == 16:
                    v = f'({v} & 0xFFFFu)' if U else f'(uint32_t)(int32_t)(int16_t){v}'
                return f'c->v.s[{reg}] = {v};'
            src = f'c->v.s[{reg}]'
            if size == 16:
                src = f'(uint16_t){src}' if U else f'(int16_t){src}'
            elif not U:
                src = f'(int32_t){src}'
            return f'c->v.f[{reg}] = (float)ldexp((double){src}, -{fbits});'
        if opc2 in (12, 13):  # VCVT(R) float -> int; result in a single register
            sd = self.sreg(vd, D)
            signed = opc2 == 13
            src = b if dbl else f'(double){b}'
            if not bit(w, 7):  # VCVTR: FPSCR rounding
                src = f'vfp_round_mode(c, {src})'
            fn = 'vcvt_s32_f64' if signed else 'vcvt_u32_f64'
            return f'c->v.s[{sd}] = {fn}({src});'
        raise Unsupported(f'vfp other {w:08x}')

    def vfp_v8(self, w, pc):
        """VSEL, VMAXNM / VMINNM, VRINTA/N/P/M, VCVTA/N/P/M (unconditional encodings)."""
        dbl = bit(w, 8)
        D, N, M = bit(w, 22), bit(w, 7), bit(w, 5)
        vd, vn, vm = bits(w, 12, 4), bits(w, 16, 4), bits(w, 0, 4)
        if dbl:
            d, n, m = self.dreg(vd, D), self.dreg(vn, N), self.dreg(vm, M)
            X, sfx = self.D, 'd'
        else:
            d, n, m = self.sreg(vd, D), self.sreg(vn, N), self.sreg(vm, M)
            X, sfx = self.S, 'f'
        if not bit(w, 23):  # VSEL
            cc = bits(w, 20, 2)
            cond = ['COND_EQ', 'COND_VS', 'COND_GE', 'COND_GT'][cc]
            if dbl:
                return f'c->v.d[{d}] = {cond} ? c->v.d[{n}] : c->v.d[{m}];'
            return f'c->v.s[{d}] = {cond} ? c->v.s[{n}] : c->v.s[{m}];'
        if bits(w, 20, 2) == 0:
            fn = 'vminnm' if bit(w, 6) else 'vmaxnm'
            return f'{X(d)} = {fn}_{sfx}({X(n)}, {X(m)});'
        if bits(w, 18, 4) == 0b1110 and bits(w, 6, 2) == 1:  # VRINTA/N/P/M
            rm = bits(w, 16, 2)
            f = ['round', 'nearbyint', 'ceil', 'floor'][rm] + ('' if dbl else 'f')
            return f'{X(d)} = {f}({X(m)});'
        if bits(w, 18, 4) == 0b1111 and bit(w, 6):  # VCVTA/N/P/M to integer
            rm = bits(w, 16, 2)
            f = ['round', 'nearbyint', 'ceil', 'floor'][rm]
            sd = self.sreg(vd, D)
            fn = 'vcvt_s32_f64' if bit(w, 7) else 'vcvt_u32_f64'
            src = X(m) if dbl else f'(double){X(m)}'
            return f'c->v.s[{sd}] = {fn}({f}({src}));'
        raise Unsupported(f'vfp v8 {w:08x}')
