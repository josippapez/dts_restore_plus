#!/usr/bin/env python3
"""usage: lpunpack.py SUPER OUTDIR -- extracts the partitions of an Android super (LP metadata v10) image."""
import os, struct, sys

src, outdir = sys.argv[1:3]
f = open(src, 'rb')
f.seek(4096)
geo = f.read(52)
if geo[:4] != b'gDla':
    sys.exit('no LP geometry')
f.seek(4096 * 3)
hdr = f.read(256)
if hdr[:4] != b'0PLA':
    sys.exit('no LP metadata header')
major, minor, hsize = struct.unpack_from('<HHI', hdr, 4)
tables_size, = struct.unpack_from('<I', hdr, 44)
# Table descriptors: partitions, extents, groups, block devices (offset, num, entry size).
desc = [struct.unpack_from('<III', hdr, 80 + 12 * i) for i in range(4)]
f.seek(4096 * 3 + hsize)
tables = f.read(tables_size)
(po, pn, ps), (eo, en, es) = desc[0], desc[1]
extents = [struct.unpack_from('<QIQI', tables, eo + i * es) for i in range(en)]
os.makedirs(outdir, exist_ok=True)
for i in range(pn):
    entry = tables[po + i * ps: po + (i + 1) * ps]
    name = entry[:36].split(b'\0')[0].decode()
    first, count = struct.unpack_from('<II', entry, 40)
    if not count:
        continue
    with open(os.path.join(outdir, name + '.img'), 'wb') as out:
        for sectors, kind, data, _ in extents[first:first + count]:
            if kind == 0:
                f.seek(data * 512)
                left = sectors * 512
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    out.write(chunk)
                    left -= len(chunk)
            else:
                out.seek(sectors * 512, 1)
                out.truncate()
    print(name, os.path.getsize(os.path.join(outdir, name + '.img')))
