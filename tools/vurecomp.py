#!/usr/bin/env python3
"""VU1 micro program static recompiler.

  vurecomp.py ELF OUTDIR          translate every DVP overlay group in the ELF to C++
  vurecomp.py ELF OUTDIR DUMPDIR  also translate VU1 programs saved at run time (RT_VU1MISS),
                                  for games that upload microcode from data files

The ELF's .DVP.ovlytab lists every microcode chunk (EE address in .vutext, VU load address);
the section names (.DVP.overlay..<vuaddr>.<id>.<n>.<chunk>) group chunks into programs.
Each group becomes `Exit prog_<id>(Ctx&, uint32_t pc)` in OUTDIR/vu1_<id>.cpp plus a
registry (OUTDIR/vu1_registry.cpp). Semantics follow src/vu1/vu1_fast.h.
"""
import re
import os, struct, sys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vudis

LANE = [8, 4, 2, 1]          # dest bit for x,y,z,w
XYZW = 'xyzw'


def lanes_of(dest):
    return [k for k in range(4) if dest & LANE[k]]


def s11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


def fnv(b):
    """FNV-1a over little-endian 64-bit words (matches vu1fast::fnv1a)."""
    h = 1469598103934665603
    for k in range(0, len(b) - 7, 8):
        h ^= int.from_bytes(b[k:k + 8], 'little')
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# --------------------------------------------------------------------------------- ELF
def read_overlays(elf):
    shoff = struct.unpack_from('<I', elf, 0x20)[0]
    shnum, shstr = struct.unpack_from('<HH', elf, 0x30)
    secs = [struct.unpack_from('<10I', elf, shoff + i * 40) for i in range(shnum)]
    st = secs[shstr]

    def nm(o):
        e = elf.index(b'\0', st[4] + o)
        return elf[st[4] + o:e].decode()
    by_name = {nm(s[0]): s for s in secs}
    vutext = by_name['.vutext']
    strtab = by_name['.DVP.ovlystrtab']
    tab = by_name['.DVP.ovlytab']
    groups = collections.OrderedDict()
    for k in range(tab[5] // 12):
        noff, ee, vu = struct.unpack_from('<3I', elf, tab[4] + 12 * k)
        if noff >= strtab[3] and strtab[3]:   # some linkers store the name as an address
            noff -= strtab[3]
        e = elf.index(b'\0', strtab[4] + noff)
        name = elf[strtab[4] + noff:e].decode()
        size = by_name[name][5]
        parts = name.split('..')[1].split('.')
        gid = int(parts[1])
        off = vutext[4] + (ee - vutext[3])
        groups.setdefault(gid, []).append((vu, elf[off:off + size]))
    return groups


# --------------------------------------------------------------------------------- decode
class Pair:
    def __init__(self, addr, lo, hi):
        self.addr, self.lo, self.hi = addr, lo, hi
        self.ibit = bool(hi >> 31 & 1)
        self.ebit = bool(hi >> 30 & 1)
        self.mbit = bool(hi >> 29 & 1)
        self.dbit = bool(hi >> 28 & 1)
        self.tbit = bool(hi >> 27 & 1)
        self.upper = decode_upper(hi)
        self.lower = None if self.ibit else decode_lower(lo, addr)
        self.text = vudis.disasm(lo, hi, addr)


def decode_upper(i):
    """Return dict: kind, reads [(reg,lanes)], accread lanes, write ('vf',reg,dest)/('acc',dest)/None,
    plus expression info."""
    op = i & 0x3F
    dest = (i >> 21) & 0xF
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    u = dict(op=op, dest=dest, ft=ft, fs=fs, fd=fd, reads=[], accread=0, write=None, kind=None, flags=False)
    madd_like = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}
    if op <= 0x2F:
        u['reads'].append((fs, dest))
        u['write'] = ('vf', fd, dest)
        if op <= 0x1B:
            u['reads'].append((ft, LANE[op & 3]))
        elif op >= 0x28:
            u['reads'].append((ft, 0xE if op == 0x2E else dest))
        if op in madd_like:
            u['accread'] = dest
        u['kind'] = ('std', op)
        return u
    if op >= 0x3C:
        sp = (i & 3) | ((i >> 4) & 0x7C)
        u['special'] = sp
        writes_acc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writes_acc:
            u['reads'].append((fs, dest))
            if sp <= 0x1B:
                u['reads'].append((ft, LANE[sp & 3]))
            elif 0x28 <= sp <= 0x2E:
                u['reads'].append((ft, 0xE if sp == 0x2E else dest))
            u['write'] = ('acc', dest)
            if 0x08 <= sp <= 0x0F or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u['accread'] = dest
            u['kind'] = ('acc', sp)
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            u['reads'].append((fs, dest))
            u['write'] = ('vf', ft, dest)
            u['kind'] = ('cvt', sp)
        elif sp == 0x1F:
            u['reads'].append((fs, 0xE))
            u['reads'].append((ft, 0x1))
            u['kind'] = ('clip', sp)
        elif sp in (0x2F, 0x30):
            u['kind'] = ('nop', sp)
        else:
            raise ValueError(f'reserved upper {i:08x}')
        return u
    raise ValueError(f'reserved upper {i:08x}')


def decode_lower(i, addr):
    if i in (0, 0x8000033C):
        return dict(name='nop', vireads=[], vfreads=[], viwrite=None, vfwrite=None, delay=False, pipe=None)
    op = (i >> 25) & 0x7F
    vft, vfs = (i >> 16) & 31, (i >> 11) & 31
    vit, vis, vid = (i >> 16) & 15, (i >> 11) & 15, (i >> 6) & 15
    dest = (i >> 21) & 0xF
    L = dict(raw=i, op=op, vft=vft, vfs=vfs, vit=vit, vis=vis, vid=vid, dest=dest, vireads=[], vfreads=[],
             viwrite=None, vfwrite=None, delay=False, pipe=None, addr=addr)

    def rvi(r):
        if r: L['vireads'].append(r)

    def wvi(r, lat):
        if r: L['viwrite'] = (r, lat)
    name = vudis.lower_name(i)
    L['name'] = name
    if op == 0x00:      # LQ
        rvi(vis); L['vfwrite'] = (vft, dest, 4)
    elif op == 0x01:    # SQ
        rvi(vit); L['vfreads'].append((vfs, dest))
    elif op == 0x04:    # ILW
        rvi(vis); wvi(vit, 4)
    elif op == 0x05:    # ISW
        rvi(vis); rvi(vit)
    elif op in (0x08, 0x09):
        L['delay'] = True; rvi(vis); wvi(vit, 1)
    elif op in (0x10, 0x12, 0x13):
        wvi(1, 1); L['clipread'] = True
    elif op == 0x11:
        pass
    elif op in (0x14, 0x16, 0x17):
        wvi(vit, 1); L['macread'] = True     # FSEQ/FSAND/FSOR read the status flag (kept with the MAC flag)
    elif op == 0x15:
        raise ValueError('FSSET is not supported')
    elif op in (0x18, 0x1A, 0x1B):
        rvi(vis); wvi(vit, 1); L['macread'] = True
    elif op == 0x1C:
        wvi(vit, 1); L['clipread'] = True
    elif op == 0x20:
        L['pipe'] = 'branch'
    elif op == 0x21:
        L['pipe'] = 'branch'; wvi(vit, 1)
    elif op == 0x24:
        L['pipe'] = 'branch'; rvi(vis)
    elif op == 0x25:
        L['pipe'] = 'branch'; rvi(vis); wvi(vit, 1)
    elif op in (0x28, 0x29):
        L['pipe'] = 'branch'; rvi(vis); rvi(vit)
    elif op in (0x2C, 0x2D, 0x2E, 0x2F):
        L['pipe'] = 'branch'; rvi(vis)
    elif op == 0x40:
        f = i & 0x3F
        if f in (0x30, 0x31, 0x34, 0x35):
            L['delay'] = True; rvi(vis); rvi(vit); wvi(vid, 1)
        elif f == 0x32:
            L['delay'] = True; rvi(vis); wvi(vit, 1)
        elif f >= 0x3C:
            sp = (i & 3) | ((i >> 4) & 0x7C)
            L['sp'] = sp
            fsf, ftf = (i >> 21) & 3, (i >> 23) & 3
            if sp in (0x30, 0x31):
                L['vfreads'].append((vfs, 0xF if sp == 0x31 else dest)); L['vfwrite'] = (vft, dest, 4)
            elif sp in (0x34, 0x36):
                L['delay'] = True; rvi(vis); wvi(vis, 1); L['vfwrite'] = (vft, dest, 4)
            elif sp in (0x35, 0x37):
                L['delay'] = True; rvi(vit); wvi(vit, 1); L['vfreads'].append((vfs, dest))
            elif sp == 0x38:
                L['pipe'] = 'fdiv'; L['vfreads'] += [(vfs, LANE[fsf]), (vft, LANE[ftf])]
            elif sp == 0x39:
                L['pipe'] = 'fdiv'; L['vfreads'].append((vft, LANE[ftf]))
            elif sp == 0x3A:
                L['pipe'] = 'fdiv'; L['vfreads'] += [(vfs, LANE[fsf]), (vft, LANE[ftf])]
            elif sp == 0x3B:
                L['pipe'] = 'waitq'
            elif sp == 0x3C:
                L['delay'] = True; L['vfreads'].append((vfs, LANE[fsf])); wvi(vit, 1)
            elif sp == 0x3D:
                rvi(vis); L['vfwrite'] = (vft, dest, 4)
            elif sp == 0x3E:
                rvi(vis); wvi(vit, 4)
            elif sp == 0x3F:
                rvi(vis); rvi(vit)
            elif sp in (0x40, 0x41):
                L['vfwrite'] = (vft, dest, 4)
            elif sp in (0x42, 0x43):
                L['vfreads'].append((vfs, LANE[fsf]))
            elif sp == 0x64:
                L['vfwrite'] = (vft, dest, 4)
            elif sp in (0x68, 0x69):
                wvi(vit, 1)
            elif sp == 0x6C:
                L['pipe'] = 'xgkick'; rvi(vis)
            elif 0x70 <= sp <= 0x7D and sp != 0x7B:
                L['pipe'] = 'efu'
                lanes = {0x70: 0xE, 0x71: 0xE, 0x72: 0xE, 0x73: 0xE, 0x74: 0xC, 0x75: 0xA, 0x76: 0xF}.get(sp, LANE[fsf])
                L['vfreads'].append((vfs, lanes))
            elif sp == 0x7B:
                L['pipe'] = 'waitp'
            else:
                raise ValueError(f'reserved lower {i:08x}')
        else:
            raise ValueError(f'reserved lower {i:08x}')
    else:
        raise ValueError(f'reserved lower {i:08x}')
    return L


# --------------------------------------------------------------------------------- emit
def vf(r, k):
    return f'c.vf[{r}][{k}]'


NOSTALL = os.environ.get('VURECOMP_NOSTALL') == '1'


def emit_stall(p, out):
    terms = []
    u, L = p.upper, p.lower
    for reg, ln in ([] if NOSTALL else u['reads']):
        if reg:
            terms += [f'c.vfR[{reg}][{k}]' for k in lanes_of(ln)]
    if not NOSTALL:
        terms += [f'c.accR[{k}]' for k in lanes_of(u['accread'])]
    if L:
        if not NOSTALL:
            for reg, ln in L['vfreads']:
                if reg:
                    terms += [f'c.vfR[{reg}][{k}]' for k in lanes_of(ln)]
            terms += [f'c.viR[{r}]' for r in L['vireads']]
        pipe = L['pipe']
        if pipe in ('fdiv', 'waitq'):
            terms.append('(c.qPending ? c.qReady : 0u)')
        elif pipe == 'efu':
            terms.append('c.efuFree')
        elif pipe == 'waitp':
            terms.append('c.maxPReady()')
        elif pipe == 'xgkick':
            terms.append('c.kickBusy')
    if terms:
        out.append('    { uint32_t s = c.cyc;')
        for t in dict.fromkeys(terms):
            out.append(f'      if ({t} > s) s = {t};')
        out.append('      c.cyc = s; }')


def upper_lane_expr(u, k, bc_var):
    """C expression (using fl{k}) computing lane k of an FMAC op, or None."""
    op = u['op']
    vs = f'vn({vf(u["fs"], k)})'
    vt = f'vn({vf(u["ft"], k)})'
    ac = f'vn(c.acc[{k}])'
    if u['kind'][0] == 'std':
        sel = op
        if op <= 0x1B:
            grp = op >> 2
            other = bc_var
            return {0: f'f_add({vs}, {other}, fl{k})', 1: f'f_sub({vs}, {other}, fl{k})',
                    2: f'f_madd({ac}, {vs}, {other}, fl{k})', 3: f'f_msub({ac}, {vs}, {other}, fl{k})',
                    4: f'(({vs}) > {other} ? ({vs}) : {other})', 5: f'(({vs}) < {other} ? ({vs}) : {other})',
                    6: f'f_mul({vs}, {other}, fl{k})'}[grp]
        m = {0x1C: f'f_mul({vs}, Qn, fl{k})', 0x1D: f'(({vs}) > In ? ({vs}) : In)', 0x1E: f'f_mul({vs}, In, fl{k})',
             0x1F: f'(({vs}) < In ? ({vs}) : In)', 0x20: f'f_add({vs}, Qn, fl{k})', 0x21: f'f_madd({ac}, {vs}, Qn, fl{k})',
             0x22: f'f_add({vs}, In, fl{k})', 0x23: f'f_madd({ac}, {vs}, In, fl{k})', 0x24: f'f_sub({vs}, Qn, fl{k})',
             0x25: f'f_msub({ac}, {vs}, Qn, fl{k})', 0x26: f'f_sub({vs}, In, fl{k})', 0x27: f'f_msub({ac}, {vs}, In, fl{k})',
             0x28: f'f_add({vs}, {vt}, fl{k})', 0x29: f'f_madd({ac}, {vs}, {vt}, fl{k})', 0x2A: f'f_mul({vs}, {vt}, fl{k})',
             0x2B: f'(({vs}) > ({vt}) ? ({vs}) : ({vt}))', 0x2C: f'f_sub({vs}, {vt}, fl{k})',
             0x2D: f'f_msub({ac}, {vs}, {vt}, fl{k})', 0x2F: f'(({vs}) < ({vt}) ? ({vs}) : ({vt}))'}
        if op == 0x2E:
            L_, R_ = [1, 2, 0, 3][k], [2, 0, 1, 3][k]
            if k == 3:
                return f'f_add(0.0f, 0.0f, fl{k})'
            return f'f_msub({ac}, vn({vf(u["fs"], L_)}), vn({vf(u["ft"], R_)}), fl{k})'
        return m[op]
    if u['kind'][0] == 'acc':
        sp = u['special']
        other = bc_var
        if sp <= 0x0F:
            grp = sp >> 2
            return {0: f'f_add({vs}, {other}, fl{k})', 1: f'f_sub({vs}, {other}, fl{k})',
                    2: f'f_madd({ac}, {vs}, {other}, fl{k})', 3: f'f_msub({ac}, {vs}, {other}, fl{k})'}[grp]
        if 0x18 <= sp <= 0x1B:
            return f'f_mul({vs}, {other}, fl{k})'
        if sp == 0x2E:
            L_, R_ = [1, 2, 0, 3][k], [2, 0, 1, 3][k]
            if k == 3:
                return f'f_add(0.0f, 0.0f, fl{k})'
            return f'f_mul(vn({vf(u["fs"], L_)}), vn({vf(u["ft"], R_)}), fl{k})'
        m = {0x1C: f'f_mul({vs}, Qn, fl{k})', 0x1E: f'f_mul({vs}, In, fl{k})', 0x20: f'f_add({vs}, Qn, fl{k})',
             0x21: f'f_madd({ac}, {vs}, Qn, fl{k})', 0x22: f'f_add({vs}, In, fl{k})', 0x23: f'f_madd({ac}, {vs}, In, fl{k})',
             0x24: f'f_sub({vs}, Qn, fl{k})', 0x25: f'f_msub({ac}, {vs}, Qn, fl{k})', 0x26: f'f_sub({vs}, In, fl{k})',
             0x27: f'f_msub({ac}, {vs}, In, fl{k})', 0x28: f'f_add({vs}, {vt}, fl{k})', 0x29: f'f_madd({ac}, {vs}, {vt}, fl{k})',
             0x2A: f'f_mul({vs}, {vt}, fl{k})', 0x2C: f'f_sub({vs}, {vt}, fl{k})', 0x2D: f'f_msub({ac}, {vs}, {vt}, fl{k})'}
        return m[sp]
    return None


SIMD = os.environ.get('VURECOMP_SIMD', '1') != '0'


def simd_form(u, lane0, bc_var):
    """(op, T vector expression) when the FMAC op is lane-parallel: lane k of the result only
    depends on lane k of fs (and acc) and on lane k of ft or a broadcast scalar."""
    m = re.match(r'f_(add|sub|mul|madd|msub)\((.*), fl0\)$', lane0)
    if not m:
        return None
    fn, args = m.group(1), m.group(2)
    fs = f'vn(c.vf[{u["fs"]}][0])'
    parts = [a.strip() for a in args.split(', ')]
    if fn in ('madd', 'msub'):
        if len(parts) != 3 or parts[0] != 'vn(c.acc[0])':
            return None
        parts = parts[1:]
    if len(parts) != 2 or parts[0] != fs:
        return None
    other = parts[1]
    if other in ('bc', 'Qn', 'In'):
        return fn, f'v4f{{{other}, {other}, {other}, {other}}}'
    if other == f'vn(c.vf[{u["ft"]}][0])':
        return fn, f'vn4(ld4(c.vf[{u["ft"]}]))'
    return None


def emit_upper(p, out, track_mac):
    """Emit computation of the upper op into temporaries; returns code to commit afterwards."""
    u = p.upper
    kind = u['kind'][0]
    commit = []
    if kind == 'nop':
        return commit
    dest = u['dest']
    ls = lanes_of(dest)
    if kind == 'clip':
        fs, ft = u['fs'], u['ft']
        out.append(f'    {{ const uint32_t wb = fbits({vf(ft, 3)}); const int32_t lim = (wb & 0x7F800000u) ? (int32_t)(wb & 0x7FFFFFFFu) : 0x007FFFFF;')
        out.append(f'      uint32_t cf = 0;')
        for k, nm in enumerate('xyz'):
            out.append(f'      if (clipExceeds(fbits({vf(fs, k)}), 0u, lim)) cf |= {1 << (2 * k)}u;')
            out.append(f'      if (clipExceeds(fbits({vf(fs, k)}), 0x80000000u, lim)) cf |= {2 << (2 * k)}u;')
        out.append('      c.queueClip(cf); }')
        return commit
    if not ls:
        return commit
    if kind == 'cvt':
        sp = u['special']
        fs, ft = u['fs'], u['ft']
        for k in ls:
            if sp in (0x10, 0x11, 0x12, 0x13):
                div = {0x10: '', 0x11: ' / 16.0f', 0x12: ' / 4096.0f', 0x13: ' / 32768.0f'}[sp]
                out.append(f'    int32_t ivx{k}; std::memcpy(&ivx{k}, &{vf(fs, k)}, 4); const float u{k} = static_cast<float>(ivx{k}){div};')
            elif sp == 0x1D:
                out.append(f'    const float u{k} = std::fabs(vn({vf(fs, k)}));')
            else:
                sc = {0x14: '1.0', 0x15: '16.0', 0x16: '4096.0', 0x17: '32768.0'}[sp]
                out.append(f'    const int32_t iu{k} = ftoi(vn({vf(fs, k)}), {sc}); float u{k}; std::memcpy(&u{k}, &iu{k}, 4);')
        if ft:
            for k in ls:
                commit.append(f'    {vf(ft, k)} = u{k}; c.vfR[{ft}][{k}] = c.cyc + 4u;')
        return commit
    # FMAC
    op = u['op']
    sel = u['special'] if kind == 'acc' else op
    needs_bc = (kind == 'std' and op <= 0x1B) or (kind == 'acc' and (sel <= 0x0F or 0x18 <= sel <= 0x1B))
    needs_q = sel in (0x1C, 0x20, 0x21, 0x24, 0x25)
    needs_i = sel in (0x1D, 0x1E, 0x1F, 0x22, 0x23, 0x26, 0x27)
    bc_var = None
    if needs_bc:
        b = sel & 3
        out.append(f'    const float bc = vn({vf(u["ft"], b)});')
        bc_var = 'bc'
    if needs_q:
        out.append('    const float Qn = vn(c.Q());')
    if needs_i:
        out.append('    const float In = vn(c.i);')
    is_minmax = kind == 'std' and (0x10 <= op <= 0x17 or op in (0x1D, 0x1F, 0x2B, 0x2F))
    simd = None if is_minmax else simd_form(u, upper_lane_expr(u, 0, bc_var), bc_var)
    if simd and SIMD:
        # All lanes at once; lanes whose result is not a normal float take the exact scalar path.
        fn, t_expr = simd
        # Without MAC tracking the operands need no normalization: the dispatcher runs with
        # DAZ/FTZ (denormal inputs read as zero) and Inf/NaN inputs give non-finite results,
        # which fall to the exact scalar path below.
        s_expr = f'vn4(ld4(c.vf[{u["fs"]}]))' if track_mac else f'ld4(c.vf[{u["fs"]}])'
        if not track_mac:
            t_expr = t_expr.replace('vn4(ld4(', '(ld4(')
        r = {'add': 'S_ + T_', 'sub': 'S_ - T_', 'mul': 'S_ * T_', 'madd': 'A_ + S_ * T_', 'msub': 'A_ - S_ * T_'}[fn]
        out.append(f'    float {", ".join(f"u{k}" for k in ls)}; uint32_t mac_;')
        # VU1 programs without MAC reads (track_mac off): ACC only ever holds FMAC results, which
        # are already normalized, and an exact zero result needs no flag work.
        acc_expr = 'ld4(c.acc)' if not track_mac else 'vn4(ld4(c.acc))'
        out.append(f'    {{ const v4f S_ = {s_expr}; const v4f T_ = {t_expr};' + (f' const v4f A_ = {acc_expr};' if fn in ('madd', 'msub') else ''))
        out.append(f'      const v4f R_ = {r}; const v4u RB_ = (v4u)R_; const v4u RM_ = RB_ & 0x7FFFFFFFu;')
        if track_mac:
            out.append('      const v4i OK_ = (v4i)((RM_ > 0x007FFFFFu) & (RM_ < 0x7F7FFFFFu));')
        else:
            out.append('      const v4i OK_ = (v4i)(((RM_ > 0x007FFFFFu) & (RM_ < 0x7F7FFFFFu)) | (RM_ == 0u));')
        out.append(f'      if (__builtin_expect(({" & ".join(f"OK_[{k}]" for k in ls)}) != 0, 1)) {{')
        out.append('        ' + ' '.join(f'u{k} = R_[{k}];' for k in ls))
        out.append('        mac_ = ' + ' | '.join(f'((RB_[{k}] >> 31) * {LANE[k] << 4}u)' for k in ls) + ';')
        out.append('      } else {')
        for k in ls:
            out.append(f'        uint32_t fl{k} = 0; u{k} = {upper_lane_expr(u, k, bc_var)};')
        out.append('        mac_ = ' + ' | '.join(f'macbits(fl{k}, {LANE[k]}u)' for k in ls) + ';')
        out.append('      } }')
        if track_mac:
            out.append('    c.queueMac(mac_);')
    else:
        for k in ls:
            if not is_minmax:
                out.append(f'    uint32_t fl{k} = 0;')
            out.append(f'    const float u{k} = {upper_lane_expr(u, k, bc_var)};')
        if not is_minmax and track_mac:
            out.append('    c.queueMac(' + ' | '.join(f'macbits(fl{k}, {LANE[k]}u)' for k in ls) + ');')
    if kind == 'acc':
        for k in ls:
            commit.append(f'    c.acc[{k}] = u{k}; c.accR[{k}] = c.cyc + 1u;')
    else:
        fd = u['fd']
        if fd:
            for k in ls:
                commit.append(f'    {vf(fd, k)} = u{k}; c.vfR[{fd}][{k}] = c.cyc + 4u;')
    return commit


def upper_vf_write(u):
    w = u['write']
    if w and w[0] == 'vf' and w[1] and w[2]:
        return w[1]
    return 0


def emit_lower(p, out, branch_sites):
    """Emit the lower op. Returns (code_after_upper_commit, sets_bk)."""
    L = p.lower
    nm = L['name']
    A = p.addr
    i = L['raw'] if 'raw' in L else 0
    after = []
    upper_reg = upper_vf_write(p.upper)
    suppressed = L.get('vfwrite') and upper_reg and L['vfwrite'][0] == upper_reg

    def vi_write(reg, expr, lat):
        if not reg:
            return
        out.append(f'    {{ const int32_t nv = (int16_t)({expr});')
        if L['delay']:
            out.append(f'      bk = {reg}; bkOld = c.vi[{reg}];')
        out.append(f'      c.vi[{reg}] = nv; c.viR[{reg}] = c.cyc + {lat}u; }}')

    def vf_write(reg, dest, vals, lat):
        if not reg or suppressed:
            return
        for k in lanes_of(dest):
            out.append(f'    {vf(reg, k)} = {vals[k]}; c.vfR[{reg}][{k}] = c.cyc + {lat}u;')

    def bvi(r):
        return '0' if r == 0 else f'(bk == {r} ? bkOld : c.vi[{r}])'

    if nm == 'nop':
        return after
    vit, vis, vid, vft, vfs, dest = L['vit'], L['vis'], L['vid'], L['vft'], L['vfs'], L['dest']
    op = L['op']
    if op == 0x00:  # LQ
        out.append(f'    {{ const uint32_t a = c.addrq(c.vi[{vis}] + ({s11(i)})); float t[4]; std::memcpy(t, mem + a, 16);')
        out.append('      (void)t;')
        if vft and not suppressed:
            for k in lanes_of(dest):
                out.append(f'      {vf(vft, k)} = t[{k}]; c.vfR[{vft}][{k}] = c.cyc + 4u;')
        out.append('    }')
    elif op == 0x01:  # SQ
        out.append('    if (c.kickPending) c.kickTick();')
        out.append(f'    {{ const uint32_t a = c.addrq(c.vi[{vit}] + ({s11(i)}));')
        for k in lanes_of(dest):
            out.append(f'      std::memcpy(mem + a + {4 * k}, &{vf(vfs, k)}, 4);')
        out.append('    }')
    elif op in (0x04,):  # ILW
        k = lanes_of(dest)[0] if dest else 3
        out.append(f'    {{ const uint32_t a = c.addrq(c.vi[{vis}] + ({s11(i)})); uint32_t w; std::memcpy(&w, mem + a + {4 * k}, 4);')
        out.append('    ')
        vi_write(vit, 'w & 0xFFFF', 4)
        out.append('    }')
    elif op == 0x05:  # ISW
        out.append('    if (c.kickPending) c.kickTick();')
        out.append(f'    {{ const uint32_t a = c.addrq(c.vi[{vis}] + ({s11(i)})); const uint32_t w = (uint16_t)c.vi[{vit}];')
        for k in lanes_of(dest):
            out.append(f'      std::memcpy(mem + a + {4 * k}, &w, 4);')
        out.append('    }')
    elif op in (0x08, 0x09):
        imm = (i & 0x7FF) | ((i >> 10) & 0x7800)
        vi_write(vit, f'c.vi[{vis}] {"+" if op == 8 else "-"} {imm}', 1)
    elif op == 0x10:
        vi_write(1, f'(c.CLIP() & 0xFFFFFFu) == {i & 0xFFFFFF}u ? 1 : 0', 1)
    elif op == 0x11:
        out.append(f'    c.queueFcset({i & 0xFFFFFF}u);')
    elif op == 0x12:
        vi_write(1, f'(c.CLIP() & {i & 0xFFFFFF}u) != 0 ? 1 : 0', 1)
    elif op == 0x13:
        vi_write(1, f'((c.CLIP() | {i & 0xFFFFFF}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1 : 0', 1)
    elif op in (0x14, 0x16, 0x17):
        imm12 = ((i >> 10) & 0x800) | (i & 0x7FF)
        expr = {0x14: f'(c.MAC(), (c.status & 0xFFFu) == {imm12}u ? 1 : 0)',
                0x16: f'(c.MAC(), (int32_t)(c.status & {imm12}u))',
                0x17: f'(c.MAC(), (int32_t)((c.status | {imm12}u) & 0xFFFu))'}[op]
        vi_write(vit, expr, 1)
    elif op == 0x18:
        vi_write(vit, f'(c.MAC() & 0xFFFFu) == (uint32_t)(uint16_t)c.vi[{vis}] ? 1 : 0', 1)
    elif op == 0x1A:
        vi_write(vit, f'(int32_t)(c.MAC() & (uint32_t)(uint16_t)c.vi[{vis}])', 1)
    elif op == 0x1B:
        vi_write(vit, f'(int32_t)(c.MAC() | (uint32_t)(uint16_t)c.vi[{vis}])', 1)
    elif op == 0x1C:
        vi_write(vit, 'c.CLIP() & 0x0FFFu', 1)
    elif L['pipe'] == 'branch':
        tgt = (A + 8 + s11(i) * 8) & CODE_MASK
        cond = {0x20: 'true', 0x21: 'true', 0x24: 'true', 0x25: 'true',
                0x28: f'(int16_t){bvi(vis)} == (int16_t){bvi(vit)}', 0x29: f'(int16_t){bvi(vis)} != (int16_t){bvi(vit)}',
                0x2C: f'(int16_t){bvi(vis)} < 0', 0x2D: f'(int16_t){bvi(vis)} > 0',
                0x2E: f'(int16_t){bvi(vis)} <= 0', 0x2F: f'(int16_t){bvi(vis)} >= 0'}[op]
        if op in (0x24, 0x25):
            out.append(f'    jt_{A:04x} = ((uint32_t)(uint16_t){bvi(vis)} * 8u) & {CODE_MASK:#x}u;')
        out.append(f'    tk_{A:04x} = {cond};')
        if op in (0x21, 0x25) and vit:
            out.append(f'    c.vi[{vit}] = (int16_t){(A + 16) // 8}; c.viR[{vit}] = c.cyc + 1u;')
        branch_sites[A] = ('jr' if op in (0x24, 0x25) else 'b', tgt)
    elif op == 0x40:
        f = i & 0x3F
        if f in (0x30, 0x31, 0x34, 0x35):
            o = {0x30: '+', 0x31: '-', 0x34: '&', 0x35: '|'}[f]
            vi_write(vid, f'c.vi[{vis}] {o} c.vi[{vit}]', 1)
        elif f == 0x32:
            imm = (i >> 6) & 0x1F
            imm = imm - 32 if imm & 16 else imm
            vi_write(vit, f'c.vi[{vis}] + ({imm})', 1)
        else:
            sp = L['sp']
            fsf, ftf = (i >> 21) & 3, (i >> 23) & 3
            if sp == 0x30:
                vf_write(vft, dest, [vf(vfs, k) for k in range(4)], 4)
            elif sp == 0x31:
                out.append(f'    {{ const float t0 = {vf(vfs, 1)}, t1 = {vf(vfs, 2)}, t2 = {vf(vfs, 3)}, t3 = {vf(vfs, 0)};')
                vf_write(vft, dest, ['t0', 't1', 't2', 't3'], 4)
                out.append('    }')
            elif sp in (0x34, 0x36):   # LQI / LQD
                if sp == 0x36:
                    vi_write(vis, f'c.vi[{vis}] - 1', 1)
                out.append(f'    {{ const uint32_t a = ((uint32_t)(uint16_t)c.vi[{vis}] * 16u) & c.memMask; float t[4]; std::memcpy(t, mem + a, 16);')
                vf_write(vft, dest, [f't[{k}]' for k in range(4)], 4)
                out.append('    }')
                if sp == 0x34:
                    vi_write(vis, f'c.vi[{vis}] + 1', 1)
            elif sp in (0x35, 0x37):   # SQI / SQD
                if sp == 0x37:
                    vi_write(vit, f'c.vi[{vit}] - 1', 1)
                out.append('    if (c.kickPending) c.kickTick();')
                out.append(f'    {{ const uint32_t a = ((uint32_t)(uint16_t)c.vi[{vit}] * 16u) & c.memMask;')
                for k in lanes_of(dest):
                    out.append(f'      std::memcpy(mem + a + {4 * k}, &{vf(vfs, k)}, 4);')
                out.append('    }')
                if sp == 0x35:
                    vi_write(vit, f'c.vi[{vit}] + 1', 1)
            elif sp == 0x38:
                out.append(f'    {{ const float num = vn({vf(vfs, fsf)}), den = vn({vf(vft, ftf)}); float r;')
                out.append('      if (den == 0.0f) r = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();')
                out.append('      else r = num / den;')
                out.append('      c.queueQ(r, 7u); }')
            elif sp == 0x39:
                out.append(f'    c.queueQ(std::sqrt(std::fabs(vn({vf(vft, ftf)}))), 7u);')
            elif sp == 0x3A:
                out.append(f'    {{ const float num = vn({vf(vfs, fsf)}); const float den = std::sqrt(std::fabs(vn({vf(vft, ftf)}))); float r;')
                out.append('      if (den != 0.0f) r = num / den; else r = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();')
                out.append('      c.queueQ(r, 13u); }')
            elif sp == 0x3B:
                pass
            elif sp == 0x3C:
                vi_write(vit, f'fbits({vf(vfs, fsf)}) & 0xFFFF', 1)
            elif sp == 0x3D:
                out.append(f'    {{ const int32_t iv = (int16_t)c.vi[{vis}]; float t; std::memcpy(&t, &iv, 4);')
                vf_write(vft, dest, ['t'] * 4, 4)
                out.append('    }')
            elif sp == 0x3E:
                k = lanes_of(dest)[0] if dest else 3
                out.append(f'    {{ const uint32_t a = ((uint32_t)(uint16_t)c.vi[{vis}] * 16u) & c.memMask; uint32_t w; std::memcpy(&w, mem + a + {4 * k}, 4);')
                vi_write(vit, 'w & 0xFFFF', 4)
                out.append('    }')
            elif sp == 0x3F:
                out.append('    if (c.kickPending) c.kickTick();')
                out.append(f'    {{ const uint32_t a = ((uint32_t)(uint16_t)c.vi[{vis}] * 16u) & c.memMask; const uint32_t w = (uint16_t)c.vi[{vit}];')
                for k in lanes_of(dest):
                    out.append(f'      std::memcpy(mem + a + {4 * k}, &w, 4);')
                out.append('    }')
            elif sp == 0x40:
                out.append('    { c.r = ((c.r << 1) ^ ((c.r >> 4) & 1u) ^ ((c.r >> 22) & 1u)) & 0x007FFFFFu; c.r |= 0x3F800000u; const float t = bfloat(c.r);')
                vf_write(vft, dest, ['t'] * 4, 4)
                out.append('    }')
            elif sp == 0x41:
                out.append('    { const float t = bfloat(c.r);')
                vf_write(vft, dest, ['t'] * 4, 4)
                out.append('    }')
            elif sp == 0x42:
                out.append(f'    c.r = 0x3F800000u | (fbits({vf(vfs, fsf)}) & 0x007FFFFFu);')
            elif sp == 0x43:
                out.append(f'    c.r = 0x3F800000u | ((c.r ^ fbits({vf(vfs, fsf)})) & 0x007FFFFFu);')
            elif sp == 0x64:
                out.append('    { const float t = c.P();')
                vf_write(vft, dest, ['t'] * 4, 4)
                out.append('    }')
            elif sp == 0x68:
                vi_write(vit, 'c.top & 0x3FFu', 1)
            elif sp == 0x69:
                vi_write(vit, 'c.itop & 0x3FFu', 1)
            elif sp == 0x6C:
                out.append(f'    c.xgkick((uint16_t)c.vi[{vis}]);')
            elif sp == 0x7B:
                pass
            elif 0x70 <= sp <= 0x7D:
                x, y, z = (f'vn({vf(vfs, k)})' for k in range(3))
                comp = f'vn({vf(vfs, fsf)})'
                e = {0x70: (f'{x} * {x} + {y} * {y} + {z} * {z}', 11),
                     0x71: (f'[&]{{ const float s = {x} * {x} + {y} * {y} + {z} * {z}; return s != 0.0f ? 1.0f / s : s; }}()', 18),
                     0x72: (f'std::sqrt({x} * {x} + {y} * {y} + {z} * {z})', 18),
                     0x73: (f'[&]{{ const float l = std::sqrt({x} * {x} + {y} * {y} + {z} * {z}); return l != 0.0f ? 1.0f / l : l; }}()', 24),
                     0x74: (f'[&]{{ const float xx = {x}, yy = {y}; return xx != 0.0f ? eatan(yy / xx) : 0.0f; }}()', 54),
                     0x75: (f'[&]{{ const float xx = {x}, zz = {z}; return xx != 0.0f ? eatan(zz / xx) : 0.0f; }}()', 54),
                     0x76: ('[&]{ float s = 0.0f; ' + ' '.join(f's += vn({vf(vfs, k)});' for k in range(4)) + ' return s; }()', 12),
                     0x77: (f'[&]{{ float v = {comp}; if (v >= 0.0f) {{ v = std::sqrt(v); if (v != 0.0f) v = 1.0f / v; }} return v; }}()', 18),
                     0x78: (f'[&]{{ const float v = {comp}; return v >= 0.0f ? std::sqrt(v) : v; }}()', 12),
                     0x79: (f'esin({comp})', 29),
                     0x7A: (f'[&]{{ const float v = {comp}; return v != 0.0f ? 1.0f / v : v; }}()', 12),
                     0x7C: (f'eatan({comp})', 54),
                     0x7D: (f'eexp({comp})', 44)}[sp]
                out.append(f'    c.queueP({e[0]}, {e[1]}u);')
            else:
                raise ValueError(f'unhandled lower special {sp:#x}')
    else:
        raise ValueError(f'unhandled lower {i:08x}')
    return after


CODE_MASK = 0x3FFF   # VU1: 16 KB micro memory; VU0: 4 KB
PREFIX = 'prog_'
RESTRICTED = False


# Entry points seen at run time (RT_VU1ENTRIES logs, "id pc" per line). A program with
# recorded entries keeps only those, its chunk starts, branch targets, call return points and
# E-bit resume points as switch cases; every other pair is reachable only by falling through,
# so the compiler can keep values in registers and drop redundant scoreboard loads across
# pairs. An unlisted entry falls back to the interpreter (Exit::Leave at the entry pc).
ENTRIES = collections.defaultdict(set)
if os.environ.get('VURECOMP_ENTRIES'):
    for line in open(os.environ['VURECOMP_ENTRIES']):
        f = line.split()
        if len(f) == 2:
            ENTRIES[int(f[0])].add(int(f[1], 0))


SB_READ = re.compile(r'^      if \((c\.(?:vfR\[\d+\]\[\d\]|accR\[\d\]|viR\[\d+\])) > s\) s = \1;$')
SB_WRITE = re.compile(r'c\.(vfR\[(\d+)\]\[(\d)\]|accR\[(\d)\]|viR\[(\d+)\]) = c\.cyc \+ (\d)u;')


def sb_name(ref):
    return 'r' + re.sub(r'\W+', '_', ref).strip('_')


def static_scoreboard(pairs, emitted, labels, exits):
    """Resolves scoreboard (ready-cycle) checks inside straight-line traces, i.e. runs of pairs
    entered only at their first pair. A pair t pairs after a write runs at least t cycles
    later (every pair advances the cycle by one, stalls only add), so a check against a write
    of latency L made >= L pairs earlier in the trace always passes and is dropped; a closer
    one reads the ready cycle from a local. The memory scoreboard store of a write is dropped
    when the trace overwrites that lane before control can leave, or when control can only
    leave >= 4 pairs after it: every ready cycle is at most issue + 4 (no latency exceeds 4),
    so by then the stored value, old or new, no longer stalls anything."""
    used = set()
    trace = []
    def flush_trace():
        if not trace:
            return
        writes = []   # (t, ref, A, line index)
        for t, A in enumerate(trace):
            for li, line in enumerate(emitted[A]):
                for m in SB_WRITE.finditer(line):
                    writes.append((t, 'c.' + m.group(1), A, li))
        exit_ts = [t for t, A in enumerate(trace) if A in exits] + [len(trace) - 1]
        drop = set()
        for w, (t, ref, A, li) in enumerate(writes):
            j = min(e for e in exit_ts if e >= t)
            t2 = next((x[0] for x in writes[w + 1:] if x[1] == ref), None)
            if (t2 is not None and t2 <= j) or j - t + 1 >= 4:
                drop.add((A, li, ref))
        last = {}   # ref -> (t, latency)
        for t, A in enumerate(trace):
            out = emitted[A]
            new = []
            for li, line in enumerate(out):
                m = SB_READ.match(line)
                if m and m.group(1) in last:
                    tw, lat = last[m.group(1)]
                    if t - tw >= lat:
                        continue
                    n = sb_name(m.group(1))
                    used.add(n)
                    new.append(f'      if ({n} > s) s = {n};')
                    continue
                def wr(mm):
                    ref = 'c.' + mm.group(1)
                    n = sb_name(ref)
                    last[ref] = (t, int(mm.group(6)))
                    if (A, li, ref) in drop:
                        return f'{n} = c.cyc + {mm.group(6)}u;'
                    return f'{n} = c.cyc + {mm.group(6)}u; {ref} = {n};'
                new.append(SB_WRITE.sub(wr, line))
            emitted[A] = new
        # locals are needed for every written ref (the assignments stay; unused ones fold away)
        for t, ref, A, li in writes:
            used.add(sb_name(ref))
        trace.clear()
    prev = None
    for p in pairs:
        A = p.addr
        if A in labels or prev is None or ((prev + 8) & CODE_MASK) != A:
            flush_trace()
        trace.append(A)
        prev = A
    flush_trace()
    return used


def translate_group(gid, chunks, name, vu1=False, starts=(), full=False):
    global RESTRICTED
    RESTRICTED = False
    code = {}
    for vu, data in chunks:
        for off in range(0, len(data) - 7, 8):
            lo, hi = struct.unpack_from('<II', data, off)
            code[vu + off] = Pair(vu + off, lo, hi)
    pairs = [code[a] for a in sorted(code)]
    # VU0 MAC flags can be read by the EE (CFC2), so VU0 code keeps them exact. A VU1 program
    # that never reads the MAC flag (FMAND/FMEQ/FMOR) skips the flag pipeline: nothing else
    # observes VU1's MAC (status-flag instructions are rejected above), and programs that do
    # read it set the flags they test themselves.
    track_mac = (not vu1) or any(p.lower and p.lower.get('macread') for p in pairs)
    branch_sites = {}
    body = []
    # first pass: emit pairs
    emitted = {}
    for p in pairs:
        out = []
        A = p.addr
        if p.upper['write'] and p.upper['write'][0] == 'vf' and p.upper['write'][1] == 0 and p.upper['write'][2] and p.lower:
            if any(r == 0 for r, _ in p.lower['vfreads']):
                raise ValueError(f'{A:#x}: lower reads vf00 written by upper')
        out.append(f'L{A:04x}: {{  // {p.text[0]} | {p.text[1]}')
        emit_stall(p, out)
        commit = emit_upper(p, out, track_mac)
        if p.lower:
            emit_lower(p, out, branch_sites)
        out += commit
        if p.ibit:
            imm = p.lo
            out.append(f'    c.i = vn(bfloat({imm:#010x}u));')
        if not (p.lower and p.lower['delay'] and p.lower.get('viwrite')):
            out.append('    bk = -1;')
        if p.ebit:
            out.append(f'    eb_{A:04x} = true;')
        out.append('    c.cyc++; c.pairs++;')
        emitted[A] = out
    # second pass: control flow after each pair
    ebit_sites = {p.addr for p in pairs if p.ebit}
    chunk_starts = {p.addr for p in pairs if ((p.addr - 8) & CODE_MASK) not in code}
    labels = set(code)
    if vu1 and ENTRIES.get(gid) and not full:
        RESTRICTED = True
        labels = set(ENTRIES[gid]) | set(starts) | set(chunk_starts)
        labels |= {t for k, t in branch_sites.values() if k == 'b'}
        labels |= {(B + 16) & CODE_MASK for B in branch_sites if code[B].lo >> 25 in (0x21, 0x25)}
        labels |= {(E + 16) & CODE_MASK for E in ebit_sites}
        labels &= set(code)
    exits = set()
    for p in pairs:
        A = p.addr
        out = emitted[A]
        nxt = (A + 8) & CODE_MASK
        brs = [B for B in branch_sites if B + 8 == A]
        es = [E for E in ebit_sites if E + 8 == A]
        for B in brs:
            if B in branch_sites and code.get(A) and code[A].lower and code[A].lower.get('pipe') == 'branch':
                raise ValueError(f'{A:#x}: branch in delay slot')
        if A in chunk_starts:
            # Delay slot of a branch (or E bit) that left another code block (g_pendTarget).
            out.append('    if (pend) { const uint32_t pt = pend; pend = 0;')
            out.append(f'      if (pt & 0x40000000u) {{ c.pc = {nxt}u; goto finish; }}')
            out.append('      const uint32_t np = pt & 0xFFFFu;')
            out.append('      if (c.cyc > budget) { c.pc = np; return Exit::Budget; }')
            out.append('      pc = np; goto dispatch; }')
        if brs or es:
            out.append(f'    {{ uint32_t np = {nxt}u; bool jumped = false;')
            for B in brs:
                kind, tgt = branch_sites[B]
                t = f'jt_{B:04x}' if kind == 'jr' else f'{tgt}u'
                out.append(f'      if (tk_{B:04x}) {{ tk_{B:04x} = false; np = {t}; jumped = true; }}')
            for E in es:
                out.append(f'      if (eb_{E:04x}) {{ eb_{E:04x} = false; c.pc = np; goto finish; }}')
            out.append('      if (jumped) {')
            out.append('        if (c.cyc > budget) { c.pc = np; return Exit::Budget; }')
            out.append('        pc = np; goto dispatch; }')
            out.append('    }')
        if nxt not in code:
            # A branch or E bit whose delay slot lies outside this block: hand the pending
            # transfer to the program that runs the delay slot.
            if A in branch_sites:
                kind, tgt = branch_sites[A]
                t = f'jt_{A:04x}' if kind == 'jr' else f'{tgt}u'
                out.append(f'    if (tk_{A:04x}) g_pendTarget = 0x80000000u | {t};')
            if A in ebit_sites:
                out.append('    g_pendTarget = 0x40000000u;')
            out.append(f'    c.pc = {nxt}u; return Exit::Leave;')
        out.append('  }')
        if A in chunk_starts or brs or es or nxt not in code or nxt in labels:
            exits.add(A)
    sb_locals = static_scoreboard(pairs, emitted, labels, exits)
    for p in pairs:
        body += emitted[p.addr]
    locals_ = [f'  uint32_t {n} = 0;' for n in sorted(sb_locals)]
    for B in sorted(branch_sites):
        locals_.append(f'  bool tk_{B:04x} = false;')
        if branch_sites[B][0] == 'jr':
            locals_.append(f'  uint32_t jt_{B:04x} = 0;')
    for E in sorted(ebit_sites):
        locals_.append(f'  bool eb_{E:04x} = false;')
    src = []
    src.append(f'// Generated by tools/vurecomp.py from {name}. Do not edit.')
    src.append('// Build with -ffp-contract=off: fused multiply-adds would change results vs. the VU.')
    src.append('#pragma GCC optimize("O2")')
    src.append('#include "vu1_fast.h"')
    src.append('')
    src.append('#pragma GCC diagnostic ignored "-Wunused-label"')
    src.append('#pragma GCC diagnostic ignored "-Wunused-variable"')
    src.append('#pragma GCC diagnostic ignored "-Wunused-but-set-variable"')
    src.append('namespace vu1fast {')
    src.append('extern uint32_t g_pendTarget;')
    src.append(f'Exit {PREFIX}{gid}{"_all" if full else ""}(Ctx &c, uint32_t pc)')
    src.append('{')
    src += locals_
    src.append('  uint8_t *__restrict const mem = c.mem;   // VU data never aliases the register file')
    src.append('  int bk = -1; int32_t bkOld = 0; (void)bkOld;')
    src.append('  uint32_t pend = g_pendTarget; g_pendTarget = 0;')
    starts = sorted(a for a in code if ((a - 8) & CODE_MASK) not in code)
    if starts:
        src.append('  if (pend && ' + ' && '.join(f'pc != {a:#x}u' for a in starts) + ') pend = 0;')
    else:
        src.append('  pend = 0;')
    src.append('  const uint32_t budget = c.cyc + 65536u;')
    src.append('dispatch:')
    src.append('  switch (pc) {')
    for p in pairs:
        if p.addr in labels:
            src.append(f'  case {p.addr:#x}: goto L{p.addr:04x};')
    src.append('  default: c.pc = pc; return Exit::Leave;')
    src.append('  }')
    src += body
    src.append('finish:')
    src.append('  c.flush();')
    src.append('  return Exit::End;')
    src.append('}')
    src.append('}')
    return '\n'.join(src) + '\n', track_mac


def add_full_variant(gid, chunks, name, outdir, reg, entries, pname, starts):
    """The same program with every pair as an entry, registered right after the one built
    from recorded entries: the dispatcher runs it for an entry the recording never saw."""
    src, _ = translate_group(gid, chunks, name, vu1=True, starts=starts, full=True)
    open(os.path.join(outdir, f'vu1_{gid}_all.cpp'), 'w').write(src)
    reg.append(f'Exit prog_{gid}_all(Ctx &, uint32_t);')
    entries.append(f'    {{"{pname}_all", {gid}u, k_{gid}, {len(chunks)}u, &prog_{gid}_all}},')


def main():
    elf = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    groups = read_overlays(elf)
    reg = ['// Generated by tools/vurecomp.py. Do not edit.', '#include "vu1_fast.h"', '', 'namespace vu1fast {']
    entries = []
    for gid, chunks in groups.items():
        name = f'g{gid}'
        try:
            src, mac = translate_group(gid, chunks, f'DVP overlay group {gid} ({name})', vu1=True)
        except ValueError as e:
            print(f'group {gid}: skipped ({e})')
            continue
        open(os.path.join(outdir, f'vu1_{gid}.cpp'), 'w').write(src)
        reg.append(f'Exit prog_{gid}(Ctx &, uint32_t);')
        ch = ', '.join(f'{{{vu:#x}u, {len(d)}u, {fnv(d):#x}ull}}' for vu, d in chunks)
        reg.append(f'static const Chunk k_{gid}[] = {{{ch}}};')
        entries.append(f'    {{"{name}", {gid}u, k_{gid}, {len(chunks)}u, &prog_{gid}}},')
        if RESTRICTED:
            add_full_variant(gid, chunks, f'DVP overlay group {gid} ({name}), every entry', outdir, reg, entries, name, ())
        npairs = sum(len(d) // 8 for _, d in chunks)
        print(f'group {gid}: {len(chunks)} chunks, {npairs} pairs{" (MAC)" if mac else ""}')
    if len(sys.argv) > 3:
        add_vu1_dumps(sys.argv[3], outdir, reg, entries, {g & 0x7FFFFFFF for g in groups})
    reg.append('static const Program k_programs[] = {')
    reg += entries
    reg.append('};')
    reg.append('const Program *programs(uint32_t &count) { count = sizeof(k_programs) / sizeof(k_programs[0]); return k_programs; }')
    reg.append('}')
    open(os.path.join(outdir, 'vu1_registry.cpp'), 'w').write('\n'.join(reg) + '\n')
    write_vu0(groups, outdir)


def write_vu0(groups, outdir):
    """The DVP overlay table also holds the game's VU0 micro programs (run with VCALLMS).
    Every group that fits in VU0's 4 KB micro memory and uses no VU1-only instruction is also
    compiled for VU0 (4 KB code/data wrap) as prog0_<id>, registered in programs0()."""
    global CODE_MASK, PREFIX
    saved = CODE_MASK, PREFIX
    CODE_MASK, PREFIX = 0xFFF, 'prog0_'
    reg = ['// Generated by tools/vurecomp.py. Do not edit.', '#include "vu1_fast.h"', '', 'namespace vu1fast {']
    entries = []
    for gid, chunks in groups.items():
        if any(vu + len(d) > 0x1000 for vu, d in chunks):
            continue
        try:
            src, _ = translate_group(gid, chunks, f'DVP overlay group {gid}, compiled for VU0')
        except ValueError:
            continue
        if 'c.xgkick' in src or 'c.top' in src or 'c.itop' in src:
            continue
        open(os.path.join(outdir, f'vu1_0_{gid}.cpp'), 'w').write(src)   # vu1_*: built with -ffp-contract=off
        reg.append(f'Exit prog0_{gid}(Ctx &, uint32_t);')
        ch = ', '.join(f'{{{vu:#x}u, {len(d)}u, {fnv(d):#x}ull}}' for vu, d in chunks)
        reg.append(f'static const Chunk k0_{gid}[] = {{{ch}}};')
        entries.append(f'    {{"vu0_{gid}", {gid}u, k0_{gid}, {len(chunks)}u, &prog0_{gid}}},')
        print(f'group {gid}: also compiled for VU0')
    reg.append('static const Program k_programs0[] = {')
    reg += entries or ['    {"none", 0u, nullptr, 0u, nullptr},']
    reg.append('};')
    n = 'sizeof(k_programs0) / sizeof(k_programs0[0])' if entries else '0'
    reg.append(f'const Program *programs0(uint32_t &count) {{ count = {n}; return k_programs0; }}')
    reg.append('}')
    open(os.path.join(outdir, 'vu1_registry0.cpp'), 'w').write('\n'.join(reg) + '\n')
    CODE_MASK, PREFIX = saved


def reachable(code, entry):
    """Pairs reachable from entry in a VU micro memory image (code: bytes)."""
    seen, work = set(), [entry]
    size = len(code)
    while work:
        pc = work.pop()
        while 0 <= pc < size and pc not in seen:
            seen.add(pc)
            lo, hi = struct.unpack_from('<II', code, pc)
            op = lo >> 25
            if hi & (1 << 30):          # E bit: the next pair is the delay slot
                seen.add((pc + 8) % size)
                break
            if hi & (1 << 31):          # I bit: lower word is an immediate
                pc += 8
                continue
            if op in (0x20, 0x21, 0x24, 0x25) or 0x28 <= op <= 0x2F:
                seen.add((pc + 8) % size)
                if op not in (0x24, 0x25):
                    work.append((pc + 8 + s11(lo & 0x7FF) * 8) & (size - 1))
                if op != 0x20 and op != 0x24:
                    work.append((pc + 16) % size)   # not taken / return point
                break
            pc += 8
    return seen


def runs_of(code, pcs):
    out, start, prev = [], None, None
    for a in sorted(pcs):
        if start is None:
            start = prev = a
        elif a == prev + 8:
            prev = a
        else:
            out.append((start, code[start:prev + 8]))
            start = prev = a
    if start is not None:
        out.append((start, code[start:prev + 8]))
    return out


def add_vu1_dumps(dumpdir, outdir, reg, entries, done):
    """VU1 programs from RT_VU1MISS dumps (u32 entry + 16 KB micro memory). Each distinct code
    image becomes one program covering the pairs reachable from its recorded entry points."""
    images = collections.OrderedDict()   # code hash -> (code, entries)
    for fn in sorted(os.listdir(dumpdir)):
        d = open(os.path.join(dumpdir, fn), 'rb').read()
        if len(d) != 4 + 0x4000:
            continue
        code = d[4:]
        images.setdefault(fnv(code), (code, set()))[1].add(struct.unpack_from('<I', d, 0)[0])
    # Entry points whose reachable code does not overlap become separate programs: another
    # image may hold the same code for one of them next to different code for the others.
    jobs = []
    for h, (code, starts) in images.items():
        comps = []   # [entries, pcs]
        for e in sorted(starts):
            r = reachable(code, e)
            ents = {e}
            for c in [c for c in comps if c[1] & r]:
                comps.remove(c)
                ents |= c[0]
                r = r | c[1]
            comps.append([ents, r])
        for ents, pcs in comps:
            jobs.append((h, code, ents, pcs))
    for h, code, starts, pcs in jobs:
        chunks = runs_of(code, pcs)
        gid = fnv(b''.join(struct.pack('<I', v) + c for v, c in chunks)) & 0x7FFFFFFF
        if gid in done:
            continue
        done.add(gid)
        try:
            src, mac = translate_group(gid, chunks, f'a VU1 dump (code {h:016x}, entries {sorted(starts)})', vu1=True, starts=starts)
        except ValueError as e:
            print(f'vu1 dump {h:016x}: skipped ({e})')
            continue
        open(os.path.join(outdir, f'vu1_{gid}.cpp'), 'w').write(src)
        reg.append(f'Exit prog_{gid}(Ctx &, uint32_t);')
        ch = ', '.join(f'{{{vu:#x}u, {len(d)}u, {fnv(d):#x}ull}}' for vu, d in chunks)
        reg.append(f'static const Chunk k_{gid}[] = {{{ch}}};')
        entries.append(f'    {{"d{gid}", {gid}u, k_{gid}, {len(chunks)}u, &prog_{gid}}},')
        if RESTRICTED:
            add_full_variant(gid, chunks, f'a VU1 dump (code {h:016x}), every entry', outdir, reg, entries, f'd{gid}', starts)
        print(f'vu1 dump program {gid}: entries {[hex(e) for e in sorted(starts)]}, {len(chunks)} chunks, '
              f'{len(pcs)} pairs{" (MAC)" if mac else ""}')


def main_vu0(dumpdir, outdir):
    """Recompile VU0 micro programs from runtime dumps (RT_VU0DUMP): u32 entry + 4 KB code.
    Not needed for Ruff Trigger (its VU0 programs are DVP overlays, see write_vu0); kept for
    code uploaded from data files."""
    global CODE_MASK, PREFIX
    CODE_MASK, PREFIX = 0xFFF, 'prog0_'
    os.makedirs(outdir, exist_ok=True)
    images = collections.OrderedDict()   # code hash -> (code, entries)
    for fn in sorted(os.listdir(dumpdir)):
        d = open(os.path.join(dumpdir, fn), 'rb').read()
        if len(d) != 4 + 0x1000:
            continue
        entry = struct.unpack_from('<I', d, 0)[0]
        code = d[4:]
        images.setdefault(fnv(code), (code, set()))[1].add(entry)
    reg = ['// Generated by tools/vurecomp.py --vu0. Do not edit.', '#include "vu1_fast.h"', '', 'namespace vu1fast {']
    entries = []
    done = set()
    for h, (code, starts) in images.items():
        pcs = set()
        for e in starts:
            pcs |= reachable(code, e)
        chunks = runs_of(code, pcs)
        gid = fnv(b''.join(struct.pack('<I', v) + c for v, c in chunks)) & 0x7FFFFFFF
        if gid in done:
            continue
        done.add(gid)
        try:
            src, _ = translate_group(gid, chunks, f'a VU0 dump (code {h:016x}, entries {sorted(starts)})')
        except ValueError as e:
            print(f'vu0 {h:016x}: skipped ({e})')
            continue
        open(os.path.join(outdir, f'vu1_0_{gid}.cpp'), 'w').write(src)   # vu1_*: built with -ffp-contract=off
        reg.append(f'Exit prog0_{gid}(Ctx &, uint32_t);')
        ch = ', '.join(f'{{{vu:#x}u, {len(d)}u, {fnv(d):#x}ull}}' for vu, d in chunks)
        reg.append(f'static const Chunk k0_{gid}[] = {{{ch}}};')
        entries.append(f'    {{"vu0_{gid}", {gid}u, k0_{gid}, {len(chunks)}u, &prog0_{gid}}},')
        print(f'vu0 program {gid}: entries {sorted(starts)}, {len(chunks)} chunks, {len(pcs)} pairs')
    reg.append('static const Program k_programs0[] = {')
    reg += entries or ['    {"none", 0u, nullptr, 0u, nullptr},']
    reg.append('};')
    n = 'sizeof(k_programs0) / sizeof(k_programs0[0])' if entries else '0'
    reg.append(f'const Program *programs0(uint32_t &count) {{ count = {n}; return k_programs0; }}')
    reg.append('}')
    open(os.path.join(outdir, 'vu1_registry0.cpp'), 'w').write('\n'.join(reg) + '\n')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--vu0':
        main_vu0(sys.argv[2], sys.argv[3])
    else:
        main()
