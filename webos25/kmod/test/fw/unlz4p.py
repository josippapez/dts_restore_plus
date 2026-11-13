#!/usr/bin/env python3
"""usage: unlz4p.py IN OUT -- unpacks LG's LZ4P kernel container into a raw arm64 Image.

Layout inferred from a known pair (C5 33.31.68 Image.lz4 -> kernel.bin):
'LZ4P', u32 total, u32 packed-payload size, u32 block size (0x40000), u32 block count,
u32 flags, two zero words, then u32 compressed sizes per block, then the raw LZ4 blocks."""
import struct, sys
import lz4.block

src = open(sys.argv[1], 'rb').read()
if src[:4] != b'LZ4P':
    sys.exit('not an LZ4P container')
total, _, block, count = struct.unpack_from('<4I', src, 4)
sizes = struct.unpack_from('<%dI' % count, src, 0x20)
pos = 0x20 + 4 * count
out = bytearray()
for size in sizes:
    want = min(block, total - len(out))
    chunk = src[pos:pos + size]
    out += chunk if size == want else lz4.block.decompress(chunk, uncompressed_size=want)
    pos += size
if len(out) != total:
    sys.exit('unpacked %d bytes, header says %d' % (len(out), total))
open(sys.argv[2], 'wb').write(out)
print('ok', total, 'bytes')
