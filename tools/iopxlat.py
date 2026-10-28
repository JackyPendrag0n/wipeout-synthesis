#!/usr/bin/env python3
"""Translate IOP modules (IRX, MIPS-I) into native C++ for ps2xIOP (native/iop_aot.h).

usage: iopxlat.py OUT.cpp NAME=FILE.IRX [NAME=FILE.IRX ...]

Every FUNC symbol of each module becomes a C++ function that runs straight on the IOP register
file and memory. Execution enters at "labels" (function entries, branch and jump-table targets,
the instruction after every jump/branch and after anything left to the interpreter) and leaves
whenever control goes somewhere this function can't continue natively: calls (jal/jalr: the
dispatcher re-enters if the callee is translated, imports go to the interpreter), returns,
indirect jumps, syscalls, coprocessor instructions. Each label adds the exact number of guest
instructions its block executes, so IOP timing is unchanged. Relocated immediates (HI16/LO16)
are read from the loaded code at run time, so the output works at any load address.
"""
import struct
import sys
import zlib


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


class Irx:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        shoff, = struct.unpack_from('<I', d, 0x20)
        shnum, shstr = struct.unpack_from('<HH', d, 0x30)
        secs = []
        for i in range(shnum):
            secs.append(struct.unpack_from('<10I', d, shoff + 40 * i))
        stroff = secs[shstr][4]

        def name(o):
            return d[o:d.index(b'\0', o)].decode()
        self.secs = {name(stroff + s[0]): s for s in secs}
        t = self.secs['.text']
        self.text = d[t[4]:t[4] + t[5]]
        self.text_addr = t[3]
        assert self.text_addr == 0, 'expected .text at address 0'
        self.relocs = {}   # text offset -> reloc type
        if '.rel.text' in self.secs:
            r = self.secs['.rel.text']
            for k in range(r[5] // 8):
                off, info = struct.unpack_from('<II', d, r[4] + 8 * k)
                self.relocs[off] = info & 0xFF
        # jump tables: R_MIPS_32 words in .rodata that point into .text
        self.table_targets = set()
        if '.rel.rodata' in self.secs and '.rodata' in self.secs:
            ro = self.secs['.rodata']
            r = self.secs['.rel.rodata']
            for k in range(r[5] // 8):
                off, info = struct.unpack_from('<II', d, r[4] + 8 * k)
                if info & 0xFF == 2 and ro[3] <= off < ro[3] + ro[5]:
                    v = u32(d, ro[4] + off - ro[3])
                    if v < len(self.text):
                        self.table_targets.add(v)
        st = self.secs['.symtab']
        strtab = self.secs['.strtab'][4]
        self.funcs = []
        for k in range(st[5] // 16):
            nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, st[4] + 16 * k)
            if info & 0xF == 2 and size and val + size <= len(self.text):
                self.funcs.append((val, size, name(strtab + nm)))
        self.funcs.sort()
        self.find_tables()
        self.find_code_pointers()

    def find_tables(self):
        """Import (0x41E00000) and export (0x41C00000) tables sit in .text between functions: they
        are data, and an import stub (jr ra; li $0, n) must stay with the interpreter, which
        dispatches the import."""
        self.excluded = []
        n = len(self.text)
        off = 0
        while off < n:
            w = self.word(off)
            if w == 0x41E00000 and off + 20 <= n:
                end = off + 20
                while end + 8 <= n and not (self.word(end) == 0 and self.word(end + 4) == 0):
                    end += 8
                self.excluded.append((off, min(n, end + 8)))
                off = end + 8
                continue
            if w == 0x41C00000 and off + 20 <= n:
                end = off + 20
                while end + 4 <= n and self.word(end) != 0:
                    end += 4
                self.excluded.append((off, min(n, end + 4)))
                off = end + 4
                continue
            off += 4

    def is_excluded(self, off):
        return any(a <= off < b for a, b in self.excluded)

    def find_code_pointers(self):
        """Places execution can arrive at from elsewhere: call targets, exported functions and
        any HI16/LO16 address that points into .text (thread entries, callbacks)."""
        n = len(self.text)
        ptrs = set(self.table_targets)
        for off in range(0, n, 4):
            w = self.word(off)
            if w >> 26 == 3 and not self.is_excluded(off):
                ptrs.add((w & 0x3FFFFFF) << 2)
            if self.relocs.get(off) == 2:   # R_MIPS_32 in .text (export tables)
                ptrs.add(w)
        his = sorted(o for o, t in self.relocs.items() if t == 5)
        los = sorted(o for o, t in self.relocs.items() if t == 6)
        for h in his:
            lo = next((l for l in los if l > h), None)
            if lo is not None:
                v = ((self.word(h) & 0xFFFF) << 16) + s16(self.word(lo))
                ptrs.add(v & 0xFFFFFFFF)
        self.code_ptrs = {p for p in ptrs if p < n and not (p & 3) and not self.is_excluded(p)}

    def regions(self):
        """Code to translate: FUNC symbols, plus everything else outside the tables."""
        n = len(self.text)
        cuts = sorted(set([0, n] + [a for a, b in self.excluded] + [b for a, b in self.excluded] +
                          [a for a, size, nm in self.funcs] + [a + size for a, size, nm in self.funcs]))
        names = {a: nm for a, size, nm in self.funcs}
        out = []
        for a, b in zip(cuts, cuts[1:]):
            if b > a and not self.is_excluded(a):
                out.append((a, b - a, names.get(a, f'code_{a:x}')))
        return out

    def word(self, off):
        return u32(self.text, off)

    def masked_hash(self):
        """FNV-1a over .text with relocatable fields masked (must match iop_aot.cpp)."""
        h = 0x811C9DC5
        for i in range(0, len(self.text), 4):
            w = self.word(i)
            op = w >> 26
            m = 0xFFFFFFFF if op == 0 else 0xFC000000 if op in (2, 3) else 0xFFFF0000
            w &= m
            for b in w.to_bytes(4, 'little'):
                h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
        return h


BRANCH_OPS = {1, 2, 3, 4, 5, 6, 7}


def decode(w):
    return dict(op=w >> 26, rs=(w >> 21) & 31, rt=(w >> 16) & 31, rd=(w >> 11) & 31, sa=(w >> 6) & 31,
                fn=w & 63, imm=w & 0xFFFF, simm=s16(w), target=(w & 0x3FFFFFF) << 2)


def is_control(w):
    i = decode(w)
    if i['op'] == 0:
        return i['fn'] in (8, 9)
    if i['op'] == 1:
        return i['rt'] in (0, 1, 0x10, 0x11)
    return i['op'] in (2, 3, 4, 5, 6, 7)


class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, irx, start, size, name, prefix):
        self.x = irx
        self.prefix = prefix
        self.a, self.end, self.name = start, start + size, name

    def R(self, n):
        return '0u' if n == 0 else f'r[{n}]'

    def W(self, n, expr):
        return '' if n == 0 else f'r[{n}] = {expr};'

    def imm(self, off, i, signed):
        """The immediate, read from the loaded code when relocated."""
        if self.x.relocs.get(off) in (5, 6):   # HI16 / LO16
            k = off // 4
            return f'static_cast<uint32_t>(static_cast<int16_t>(c.code[{k}]))' if signed else f'(c.code[{k}] & 0xFFFFu)'
        return f'{i["simm"] & 0xFFFFFFFF:#x}u' if signed else f'{i["imm"]:#x}u'

    def simple(self, off, w):
        """C++ for a non-control instruction, or raise Unsupported."""
        i = decode(w)
        op, rs, rt, rd, sa, fn = i['op'], i['rs'], i['rt'], i['rd'], i['sa'], i['fn']
        R, W = self.R, self.W
        if w == 0:
            return ''
        if op == 0:
            if fn == 0: return W(rd, f'{R(rt)} << {sa}')
            if fn == 2: return W(rd, f'{R(rt)} >> {sa}')
            if fn == 3: return W(rd, f'static_cast<uint32_t>(static_cast<int32_t>({R(rt)}) >> {sa})')
            if fn == 4: return W(rd, f'{R(rt)} << ({R(rs)} & 31u)')
            if fn == 6: return W(rd, f'{R(rt)} >> ({R(rs)} & 31u)')
            if fn == 7: return W(rd, f'static_cast<uint32_t>(static_cast<int32_t>({R(rt)}) >> ({R(rs)} & 31u))')
            if fn == 0x10: return W(rd, 'c.cpu.hi')
            if fn == 0x11: return f'c.cpu.hi = {R(rs)};'
            if fn == 0x12: return W(rd, 'c.cpu.lo')
            if fn == 0x13: return f'c.cpu.lo = {R(rs)};'
            if fn == 0x18: return f'Mult(c, {R(rs)}, {R(rt)});'
            if fn == 0x19: return f'Multu(c, {R(rs)}, {R(rt)});'
            if fn == 0x1A: return f'Div(c, {R(rs)}, {R(rt)});'
            if fn == 0x1B: return f'Divu(c, {R(rs)}, {R(rt)});'
            if fn in (0x20, 0x21): return W(rd, f'{R(rs)} + {R(rt)}')
            if fn in (0x22, 0x23): return W(rd, f'{R(rs)} - {R(rt)}')
            if fn == 0x24: return W(rd, f'{R(rs)} & {R(rt)}')
            if fn == 0x25: return W(rd, f'{R(rs)} | {R(rt)}')
            if fn == 0x26: return W(rd, f'{R(rs)} ^ {R(rt)}')
            if fn == 0x27: return W(rd, f'~({R(rs)} | {R(rt)})')
            if fn == 0x2A: return W(rd, f'(static_cast<int32_t>({R(rs)}) < static_cast<int32_t>({R(rt)}) ? 1u : 0u)')
            if fn == 0x2B: return W(rd, f'({R(rs)} < {R(rt)} ? 1u : 0u)')
            raise Unsupported
        if op in (8, 9): return W(rt, f'{R(rs)} + {self.imm(off, i, True)}')
        if op == 0xA: return W(rt, f'(static_cast<int32_t>({R(rs)}) < static_cast<int32_t>({self.imm(off, i, True)}) ? 1u : 0u)')
        if op == 0xB: return W(rt, f'({R(rs)} < {self.imm(off, i, True)} ? 1u : 0u)')
        if op == 0xC: return W(rt, f'{R(rs)} & {self.imm(off, i, False)}')
        if op == 0xD: return W(rt, f'{R(rs)} | {self.imm(off, i, False)}')
        if op == 0xE: return W(rt, f'{R(rs)} ^ {self.imm(off, i, False)}')
        if op == 0xF: return W(rt, f'{self.imm(off, i, False)} << 16')
        addr = f'{R(rs)} + {self.imm(off, i, True)}'
        if op == 0x20: return W(rt, f'static_cast<uint32_t>(static_cast<int8_t>(c.mem.read8({addr})))') or f'(void)c.mem.read8({addr});'
        if op == 0x21: return W(rt, f'static_cast<uint32_t>(static_cast<int16_t>(c.mem.read16({addr})))') or f'(void)c.mem.read16({addr});'
        if op == 0x23: return W(rt, f'c.mem.read32({addr})') or f'(void)c.mem.read32({addr});'
        if op == 0x24: return W(rt, f'c.mem.read8({addr})') or f'(void)c.mem.read8({addr});'
        if op == 0x25: return W(rt, f'c.mem.read16({addr})') or f'(void)c.mem.read16({addr});'
        if op == 0x22: return W(rt, f'Lwl(c, {addr}, {R(rt)})') or f'(void)Lwl(c, {addr}, 0u);'
        if op == 0x26: return W(rt, f'Lwr(c, {addr}, {R(rt)})') or f'(void)Lwr(c, {addr}, 0u);'
        if op == 0x28: return f'c.mem.write8({addr}, static_cast<uint8_t>({R(rt)}));'
        if op == 0x29: return f'c.mem.write16({addr}, static_cast<uint16_t>({R(rt)}));'
        if op == 0x2B: return f'c.mem.write32({addr}, {R(rt)});'
        if op == 0x2A: return f'Swl(c, {addr}, {R(rt)});'
        if op == 0x2E: return f'Swr(c, {addr}, {R(rt)});'
        raise Unsupported

    def supported(self, off):
        w = self.x.word(off)
        if is_control(w):
            return True
        try:
            self.simple(off, w)
            return True
        except Unsupported:
            return False

    def analyse(self):
        a, end = self.a, self.end
        labels = {a}
        for t in self.x.code_ptrs:
            if a <= t < end:
                labels.add(t)
        for off in range(a, end, 4):
            w = self.x.word(off)
            i = decode(w)
            if is_control(w):
                labels.add(off + 8)
                if i['op'] in (1, 4, 5, 6, 7):
                    labels.add(off + 4 + (i['simm'] << 2))
                elif i['op'] == 2:
                    labels.add(i['target'])
            elif not self.supported(off):
                labels.add(off + 4)
        self.labels = sorted(l for l in labels if a <= l < end)
        self.labelset = set(self.labels)

    def count(self, off):
        """Guest instructions the block starting at label off executes before it ends or falls into
        the next label."""
        n = 0
        while off < self.end:
            if n and off in self.labelset:
                return n
            w = self.x.word(off)
            if is_control(w):
                if off + 4 >= self.end or not self.supported(off + 4) or is_control(self.x.word(off + 4)):
                    return n   # left to the interpreter
                return n + 2
            if not self.supported(off):
                return n
            n += 1
            off += 4
        return n

    def goto(self, target, off):
        """Jump to a guest offset: a label of this function, or leave."""
        back = target <= off
        if target in self.labelset:
            chk = f'if (c.n >= c.limit) {{ c.cpu.pc = c.base + {target:#x}u; return; }} ' if back else ''
            return f'{{ {chk}goto L_{target:x}; }}'
        return f'{{ c.cpu.pc = c.base + {target:#x}u; return; }}'

    def control(self, off, w, delay):
        i = decode(w)
        op, rs, rt = i['op'], i['rs'], i['rt']
        R = self.R
        nxt = off + 8
        if op == 0 and i['fn'] == 8:   # jr
            return [f'{{ const uint32_t t = {R(rs)};', delay, 'c.cpu.pc = t; return; }']
        if op == 0 and i['fn'] == 9:   # jalr
            return [f'{{ const uint32_t t = {R(rs)};', self.W(i['rd'], f'c.base + {nxt:#x}u'), delay, 'c.cpu.pc = t; return; }']
        if op == 2:
            return [delay, self.goto(i['target'], off)]
        if op == 3:
            return [f'r[31] = c.base + {nxt:#x}u;', delay, f'c.cpu.pc = c.base + {i["target"]:#x}u; return;']
        tgt = off + 4 + (i['simm'] << 2)
        if op == 1:
            cond = {0: f'static_cast<int32_t>({R(rs)}) < 0', 1: f'static_cast<int32_t>({R(rs)}) >= 0',
                    0x10: f'static_cast<int32_t>({R(rs)}) < 0', 0x11: f'static_cast<int32_t>({R(rs)}) >= 0'}[rt]
            link = f'r[31] = c.base + {nxt:#x}u;' if rt & 0x10 else ''
            return [f'{{ const bool t = {cond};', link, delay, f'if (t) {self.goto(tgt, off)} }}']
        cond = {4: f'{R(rs)} == {R(rt)}', 5: f'{R(rs)} != {R(rt)}',
                6: f'static_cast<int32_t>({R(rs)}) <= 0', 7: f'static_cast<int32_t>({R(rs)}) > 0'}[op]
        return [f'{{ const bool t = {cond};', delay, f'if (t) {self.goto(tgt, off)} }}']

    def emit(self, out, findex):
        self.analyse()
        nm = f'{self.prefix}_{self.a:x}'
        out.append(f'// {self.name} {self.a:#x}..{self.end:#x}')
        out.append(f'void {nm}(Ctx &c, uint32_t off)')
        out.append('{')
        out.append('    uint32_t *const r = c.cpu.gpr.data();')
        out.append('    switch (off)')
        out.append('    {')
        for l in self.labels:
            out.append(f'    case {l:#x}: goto L_{l:x};')
        out.append('    default: return;')
        out.append('    }')
        stubs = []
        off = self.a
        while off < self.end:
            if off in self.labelset:
                out.append(f'L_{off:x}: c.n += {self.count(off)};')
            w = self.x.word(off)
            if is_control(w):
                d = off + 4
                if d >= self.end or not self.supported(d) or is_control(self.x.word(d)):
                    out.append(f'    c.cpu.pc = c.base + {off:#x}u; return;')
                    off += 4
                    continue
                if d in self.labelset:   # a label in a delay slot: entered there it just falls through
                    stubs.append(f'L_{d:x}: c.n += 1; {self.simple(d, self.x.word(d))} goto L_{d + 4:x};')
                out.append('    ' + ' '.join(s for s in self.control(off, w, self.simple(d, self.x.word(d))) if s))
                off += 8
                continue
            if not self.supported(off):
                out.append(f'    c.cpu.pc = c.base + {off:#x}u; return;   // {w:08x} left to the interpreter')
                off += 4
                continue
            s = self.simple(off, w)
            if s:
                out.append('    ' + s)
            off += 4
        out.append(f'    c.cpu.pc = c.base + {self.end:#x}u; return;')
        out.extend('    ' + s for s in stubs)
        out.append('}')
        return nm, self.labels


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    outp = sys.argv[1]
    out = ['// Generated by iopxlat.py from the game\'s IRX modules. Do not edit.',
           '#include "iop_aot.h"', '', 'namespace ps2x::iop::detail::aot', '{', 'namespace', '{']
    mods = []
    for arg in sys.argv[2:]:
        name, path = arg.split('=', 1)
        try:
            x = Irx(path)
        except (OSError, KeyError, AssertionError) as e:
            print(f'{name}: skipped ({e})')
            continue
        words = len(x.text) // 4
        entry = [-1] * words
        fnames = []
        for (a, size, fname) in x.regions():
            g = FuncGen(x, a, size, fname, name)
            nm, labels = g.emit(out, len(fnames))
            idx = len(fnames)
            fnames.append(nm)
            for l in labels:
                entry[l // 4] = idx
        out.append(f'const Fn kFns_{name}[] = {{{", ".join(fnames)}}};')
        rows = []
        for k in range(0, words, 32):
            rows.append(', '.join(str(e) for e in entry[k:k + 32]))
        out.append(f'const int16_t kEntry_{name}[] = {{')
        out.extend(f'    {r},' for r in rows)
        out.append('};')
        mods.append((name, len(x.text), x.masked_hash(), len(fnames)))
        print(f'{name}: {len(fnames)} functions, {sum(1 for e in entry if e >= 0)} entry points, text {len(x.text)} bytes, hash {x.masked_hash():08x}')
    out.append('}')
    out.append('const Module kAotModules[] = {')
    for name, size, h, n in mods:
        out.append(f'    {{"{name}", {size}u, {h:#010x}u, kEntry_{name}, kFns_{name}}},')
    out.append('};')
    out.append(f'const size_t kAotModuleCount = {len(mods)};')
    out.append('}')
    open(outp, 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
