#!/usr/bin/env python3
"""VU1 micro-memory snapshots, rebuilt from the game's own executable.

vurecomp.py translates VU1 programs that the game assembles at run time from several overlays
(RT_VU1MISS dumps: u32 entry + 16 KB micro memory). The dumps themselves are game code, so the
repository only keeps a manifest that says which bytes of SLUS_204.62 go where; the build makes
the dumps from the user's copy and checks each one against its SHA-256.

  vu1dumps.py build SLUS_204.62 MANIFEST.json OUTDIR     make the dumps (used by build.py)
  vu1dumps.py make  SLUS_204.62 DUMPDIR MANIFEST.json    write a manifest (developers, after
                                                         capturing new RT_VU1MISS dumps)
Standard library only.
"""
import collections
import hashlib
import json
import os
import struct
import sys

MICRO = 16384


def make(elf_path, dumpdir, manifest_path):
    elf = open(elf_path, 'rb').read()
    index = collections.defaultdict(list)
    for o in range(0, len(elf) - 8, 4):
        w = elf[o:o + 8]
        if w != bytes(8):
            index[w].append(o)
    out = []
    for fn in sorted(os.listdir(dumpdir)):
        d = open(os.path.join(dumpdir, fn), 'rb').read()
        if len(d) != 4 + MICRO:
            continue
        code = d[4:]
        runs, pos, missing = [], 0, 0
        while pos < MICRO:
            w = code[pos:pos + 8]
            if w == bytes(8):
                pos += 8
                continue
            best = (0, None)
            for o in index.get(w, [])[:4000]:
                n = 8
                while pos + n < MICRO and o + n < len(elf) and code[pos + n] == elf[o + n]:
                    n += 1
                if n > best[0]:
                    best = (n, o)
            if not best[0]:
                missing += 1
                pos += 8
                continue
            n = max(8, best[0] // 8 * 8)
            runs.append([pos, best[1], n])
            pos += n
        if missing:
            print(f'{fn}: {missing} words are not in the executable; left out')
            continue
        out.append({'name': fn, 'entry': struct.unpack_from('<I', d)[0],
                    'sha256': hashlib.sha256(d).hexdigest(), 'runs': runs})
    with open(manifest_path, 'w') as f:
        json.dump({'elf_sha256': hashlib.sha256(elf).hexdigest(), 'dumps': out}, f, indent=1)
    print(f'{len(out)} dumps -> {manifest_path}')


def build(elf_path, manifest_path, outdir):
    elf = open(elf_path, 'rb').read()
    m = json.load(open(manifest_path))
    if hashlib.sha256(elf).hexdigest() != m['elf_sha256']:
        sys.exit(f'{elf_path} is not the NTSC-U SLUS_204.62 this port was made for (SHA-256 differs)')
    os.makedirs(outdir, exist_ok=True)
    for e in m['dumps']:
        code = bytearray(MICRO)
        for dst, src, n in e['runs']:
            code[dst:dst + n] = elf[src:src + n]
        d = struct.pack('<I', e['entry']) + bytes(code)
        if hashlib.sha256(d).hexdigest() != e['sha256']:
            sys.exit(f'{e["name"]}: rebuilt dump does not match')
        with open(os.path.join(outdir, e['name']), 'wb') as f:
            f.write(d)
    print(f'{len(m["dumps"])} VU1 dumps rebuilt in {outdir}')


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == 'make':
        make(*sys.argv[2:])
    elif len(sys.argv) == 5 and sys.argv[1] == 'build':
        build(*sys.argv[2:])
    else:
        sys.exit(__doc__)
