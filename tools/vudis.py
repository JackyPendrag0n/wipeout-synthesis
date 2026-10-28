#!/usr/bin/env python3
"""VU0/VU1 micro-mode disassembler.

  vudis.py FILE [start_byte [end_byte]]     disassemble raw microcode (8 bytes / instruction)
  vudis.py --cap CAP [which]                 disassemble VU1 code snapshot from an RTCAP001 file
                                             (which = 0 start snapshot, 1 end snapshot)
As a module: disasm(lo32, hi32) -> (upper_text, lower_text), opcode_names(lo, hi).
"""
import struct, sys

BC = 'xyzw'


def dest(i):
    d = (i >> 21) & 0xF
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)


def vf(n, d=''):
    return f'vf{n:02d}' + (f'{d}' if d else '')


UP6 = {}
for k, nm in enumerate(['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul']):
    for b in range(4):
        UP6[k * 4 + b] = (nm, 'bc', b)
UP6.update({0x1C: ('mul', 'q', 0), 0x1D: ('max', 'i', 0), 0x1E: ('mul', 'i', 0), 0x1F: ('mini', 'i', 0),
            0x20: ('add', 'q', 0), 0x21: ('madd', 'q', 0), 0x22: ('add', 'i', 0), 0x23: ('madd', 'i', 0),
            0x24: ('sub', 'q', 0), 0x25: ('msub', 'q', 0), 0x26: ('sub', 'i', 0), 0x27: ('msub', 'i', 0),
            0x28: ('add', '', 0), 0x29: ('madd', '', 0), 0x2A: ('mul', '', 0), 0x2B: ('max', '', 0),
            0x2C: ('sub', '', 0), 0x2D: ('msub', '', 0), 0x2E: ('opmsub', '', 0), 0x2F: ('mini', '', 0)})
UP11 = {}
for k, nm in enumerate(['adda', 'suba', 'madda', 'msuba']):
    for b in range(4):
        UP11[k * 4 + b] = (nm, 'bc', b)
for b, s in enumerate(['0', '4', '12', '15']):
    UP11[0x10 + b] = ('itof' + s, 'cvt', 0)
    UP11[0x14 + b] = ('ftoi' + s, 'cvt', 0)
for b in range(4):
    UP11[0x18 + b] = ('mula', 'bc', b)
UP11.update({0x1C: ('mula', 'q', 0), 0x1D: ('abs', 'cvt', 0), 0x1E: ('mula', 'i', 0), 0x1F: ('clip', 'clip', 0),
             0x20: ('adda', 'q', 0), 0x21: ('madda', 'q', 0), 0x22: ('adda', 'i', 0), 0x23: ('madda', 'i', 0),
             0x24: ('suba', 'q', 0), 0x25: ('msuba', 'q', 0), 0x26: ('suba', 'i', 0), 0x27: ('msuba', 'i', 0),
             0x28: ('adda', '', 0), 0x29: ('madda', '', 0), 0x2A: ('mula', '', 0), 0x2C: ('suba', '', 0),
             0x2D: ('msuba', '', 0), 0x2E: ('opmula', '', 0), 0x2F: ('nop', 'nop', 0)})


def upper(i):
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    d = dest(i)
    flags = ''.join(f for f, b in (('[I]', 31), ('[E]', 30), ('[M]', 29), ('[D]', 28), ('[T]', 27)) if (i >> b) & 1)
    op = i & 0x3F
    if op >= 0x3C:
        k = ((i >> 6) & 0x1F) << 2 | (i & 3)
        nm, kind, b = UP11.get(k, (f'?up11_{k:#x}', 'nop', 0))
        if kind == 'nop':
            t = nm
        elif kind == 'cvt':
            t = f'{nm}.{d} {vf(ft)}, {vf(fs)}'
        elif kind == 'clip':
            t = f'clipw.xyz {vf(fs)}, {vf(ft)}w'
        elif nm == 'opmula':
            t = f'opmula.xyz ACC, {vf(fs)}, {vf(ft)}'
        elif kind == 'bc':
            t = f'{nm}{BC[b]}.{d} ACC, {vf(fs)}, {vf(ft)}{BC[b]}'
        elif kind in ('q', 'i'):
            t = f'{nm}{kind}.{d} ACC, {vf(fs)}, {kind.upper()}'
        else:
            t = f'{nm}.{d} ACC, {vf(fs)}, {vf(ft)}'
    else:
        nm, kind, b = UP6.get(op, (f'?up6_{op:#x}', '', 0))
        if kind == 'bc':
            t = f'{nm}{BC[b]}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}{BC[b]}'
        elif kind in ('q', 'i'):
            t = f'{nm}{kind}.{d} {vf(fd)}, {vf(fs)}, {kind.upper()}'
        elif nm == 'opmsub':
            t = f'opmsub.xyz {vf(fd)}, {vf(fs)}, {vf(ft)}'
        else:
            t = f'{nm}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}'
    return (t + ' ' + flags).strip()


def s11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


LO7 = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
       0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
       0x18: 'fmeq', 0x1A: 'fmand', 0x1B: 'fmor', 0x1C: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr', 0x25: 'jalr',
       0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}
LO6 = {0x30: 'iadd', 0x31: 'isub', 0x32: 'iaddi', 0x34: 'iand', 0x35: 'ior'}
LO11 = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div', 0x39: 'sqrt',
        0x3A: 'rsqrt', 0x3B: 'waitq', 0x3C: 'mtir', 0x3D: 'mfir', 0x3E: 'ilwr', 0x3F: 'iswr', 0x40: 'rnext',
        0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop', 0x6C: 'xgkick',
        0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy', 0x75: 'eatanxz',
        0x76: 'esum', 0x78: 'esqrt', 0x79: 'ersqrt', 0x7A: 'ercpr', 0x7B: 'waitp', 0x7C: 'esin', 0x7D: 'eatan',
        0x7E: 'eexp'}


def lower_name(i):
    op = (i >> 25) & 0x7F
    if op == 0x40:
        sub = i & 0x3F
        if sub >= 0x3C:
            return LO11.get(((i >> 6) & 0x1F) << 2 | (i & 3), '?lo11')
        return LO6.get(sub, '?lo6')
    return LO7.get(op, f'?lo7_{op:#x}')


def lower(i, pc=0):
    it, is_, id_ = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    d = dest(i)
    nm = lower_name(i)
    fsf, ftf = (i >> 21) & 3, (i >> 23) & 3
    if nm in ('lq', 'sq'):
        return f'{nm}.{d} {vf(it) if nm == "lq" else vf(is_)}, {s11(i)}(vi{is_ if nm == "lq" else it:02d})'
    if nm in ('ilw', 'isw'):
        return f'{nm}.{d} vi{it:02d}, {s11(i)}(vi{is_:02d})'
    if nm in ('iaddiu', 'isubiu'):
        imm = ((i >> 10) & 0x7800) | (i & 0x7FF)
        return f'{nm} vi{it:02d}, vi{is_:02d}, {imm:#x}'
    if nm.startswith('fc') and nm != 'fcget':
        return f'{nm} vi01, {i & 0xFFFFFF:#08x}'
    if nm == 'fcget':
        return f'fcget vi{it:02d}'
    if nm in ('fseq', 'fsset', 'fsand', 'fsor'):
        imm = ((i >> 10) & 0x800) | (i & 0x7FF)
        return f'{nm} vi{it:02d}, {imm:#x}'
    if nm.startswith('fm'):
        return f'{nm} vi{it:02d}, vi{is_:02d}'
    if nm in ('b', 'bal'):
        tgt = pc + 8 + s11(i) * 8
        return f'{nm} {"vi%02d, " % it if nm == "bal" else ""}L{tgt:04x}'
    if nm in ('jr', 'jalr'):
        return f'{nm} {"vi%02d, " % it if nm == "jalr" else ""}vi{is_:02d}'
    if nm.startswith('ib'):
        tgt = pc + 8 + s11(i) * 8
        if nm in ('ibeq', 'ibne'):
            return f'{nm} vi{it:02d}, vi{is_:02d}, L{tgt:04x}'
        return f'{nm} vi{is_:02d}, L{tgt:04x}'
    if nm in ('iadd', 'isub', 'iand', 'ior'):
        return f'{nm} vi{id_:02d}, vi{is_:02d}, vi{it:02d}'
    if nm == 'iaddi':
        imm = (i >> 6) & 31
        imm = imm - 32 if imm & 16 else imm
        return f'iaddi vi{it:02d}, vi{is_:02d}, {imm}'
    if nm in ('move', 'mr32'):
        return f'{nm}.{d} {vf(it)}, {vf(is_)}'
    if nm in ('lqi', 'lqd'):
        return f'{nm}.{d} {vf(it)}, ({"vi%02d++" % is_ if nm == "lqi" else "--vi%02d" % is_})'
    if nm in ('sqi', 'sqd'):
        return f'{nm}.{d} {vf(is_)}, ({"vi%02d++" % it if nm == "sqi" else "--vi%02d" % it})'
    if nm in ('div',):
        return f'div Q, {vf(is_)}{BC[fsf]}, {vf(it)}{BC[ftf]}'
    if nm in ('sqrt', 'rsqrt'):
        return f'{nm} Q, ' + (f'{vf(is_)}{BC[fsf]}, ' if nm == 'rsqrt' else '') + f'{vf(it)}{BC[ftf]}'
    if nm in ('waitq', 'waitp', 'nop'):
        return nm
    if nm == 'mtir':
        return f'mtir vi{it:02d}, {vf(is_)}{BC[fsf]}'
    if nm == 'mfir':
        return f'mfir.{d} {vf(it)}, vi{is_:02d}'
    if nm in ('ilwr', 'iswr'):
        return f'{nm}.{d} vi{it:02d}, (vi{is_:02d})'
    if nm in ('rnext', 'rget'):
        return f'{nm}.{d} {vf(it)}, R'
    if nm in ('rinit', 'rxor'):
        return f'{nm} R, {vf(is_)}{BC[fsf]}'
    if nm == 'mfp':
        return f'mfp.{d} {vf(it)}, P'
    if nm in ('xtop', 'xitop'):
        return f'{nm} vi{it:02d}'
    if nm == 'xgkick':
        return f'xgkick vi{is_:02d}'
    if nm in ('esadd', 'ersadd', 'eleng', 'erleng', 'esum'):
        return f'{nm} P, {vf(is_)}'
    if nm.startswith('e'):
        return f'{nm} P, {vf(is_)}{BC[fsf]}'
    return nm


def disasm(lo, hi, pc=0):
    u = upper(hi)
    if (hi >> 31) & 1:
        f = struct.unpack('<f', struct.pack('<I', lo))[0]
        return u, f'loi {f!r}'
    if lo == 0x8000033C:
        return u, 'nop'
    return u, lower(lo, pc)


def opcode_names(lo, hi):
    up = upper(hi).split()[0]
    up = up.split('.')[0]
    if (hi >> 31) & 1:
        lw = 'loi'
    elif lo == 0x8000033C:
        lw = 'nop'
    else:
        lw = lower_name(lo)
    return up, lw


def listing(code, start=0, end=None, base=0):
    end = len(code) if end is None else end
    out = []
    for pc in range(start, end, 8):
        lo, hi = struct.unpack_from('<II', code, pc)
        u, l = disasm(lo, hi, base + pc)
        out.append(f'L{base + pc:04x}: {u:44s} {l}')
    return out


def cap_code(cap, which=0):
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    import gscap
    k = 0
    for typ, tick, data in gscap.read_cap(cap):
        if typ == 0x20:
            if k == which:
                return data
            k += 1


if __name__ == '__main__':
    if sys.argv[1] == '--cap':
        code = cap_code(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
        print('\n'.join(listing(code)))
    else:
        code = open(sys.argv[1], 'rb').read()
        s = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
        e = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(code)
        print('\n'.join(listing(code, s, e)))
