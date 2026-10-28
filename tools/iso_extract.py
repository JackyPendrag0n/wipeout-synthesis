#!/usr/bin/env python3
"""Extract every file of an ISO9660 image (a PS2 DVD/CD dump) into a directory.

  iso_extract.py IMAGE.iso OUTDIR

Names are taken from the ISO9660 directory records with the ";1" version suffix removed, so
they keep the disc's upper-case spelling that the game uses in its paths. Standard library only.
"""
import os
import struct
import sys

SECTOR = 2048


def read_sector(f, lba, count=1):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)


def records(data):
    """Yield (name, lba, size, is_dir) for the directory records in a directory extent."""
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:                        # records never cross a sector: skip to the next
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        pos += length
        lba = struct.unpack_from('<I', rec, 2)[0]
        size = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        raw = rec[33:33 + name_len]
        if raw in (b'\x00', b'\x01'):          # "." and ".."
            continue
        name = raw.decode('ascii', 'replace').split(';')[0]
        if name.endswith('.') and not flags & 2:
            name = name[:-1]
        yield name, lba, size, bool(flags & 2)


def extract(f, lba, size, out, seen):
    if lba in seen:
        return 0
    seen.add(lba)
    os.makedirs(out, exist_ok=True)
    count = 0
    for name, child, csize, is_dir in records(read_sector(f, lba, (size + SECTOR - 1) // SECTOR)):
        path = os.path.join(out, name)
        if is_dir:
            count += extract(f, child, csize, path, seen)
            continue
        f.seek(child * SECTOR)
        left = csize
        with open(path, 'wb') as o:
            while left:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                o.write(chunk)
                left -= len(chunk)
        count += 1
        print(f'  {os.path.relpath(path, sys.argv[2])}  ({csize >> 10} KB)')
    return count


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        pvd = read_sector(f, 16)
        if pvd[1:6] != b'CD001' or pvd[0] != 1:
            sys.exit('not an ISO9660 image (no primary volume descriptor at sector 16)')
        root = pvd[156:156 + 34]
        lba = struct.unpack_from('<I', root, 2)[0]
        size = struct.unpack_from('<I', root, 10)[0]
        n = extract(f, lba, size, sys.argv[2], set())
    print(f'{n} files extracted to {sys.argv[2]}')


if __name__ == '__main__':
    main()
