#!/usr/bin/env python3
"""Regenerate the fixtures of test-iec61937-hbr.c with the local ffmpeg.

  python3 make-iec61937-hbr-fixtures.py [path/to/ffmpeg]

Writes, next to this script:
  iec61937-dtshd.in    3 DTS-HD MA frames (core + extension substream) cut from
                       testfiles/DTS-HD-MA-5.1.ts, with one 0xAA byte inserted
                       after the second frame. FFmpeg's dca parser keeps that
                       byte in the second packet, so it is odd-sized and the
                       test covers the MSB-aligned lone last byte.
  iec61937-dtshd.ref   ffmpeg -f dts -i .in -c:a copy -f spdif -dtshd_rate 768000
  iec61937-truehd.in   77 access units from the first 100 of
                       testfiles/TrueHD-5.1.mkv: AUs 30-32 dropped (a gap the
                       timing pads over), AUs 50-69 dropped (a gap past half a
                       MAT frame: FFmpeg's "unusual frame timing", no padding),
                       and kept AUs 11 and 23 grown to 3200 and 2000 bytes so
                       their data straddles the MAT middle and end codes (AU 11
                       also makes the next AU's padding negative). A grown AU
                       is the original followed by filler, with the 12-bit
                       length and the parity nibble rewritten so FFmpeg's mlp
                       parser still takes it as one AU.
  iec61937-truehd.ref  ffmpeg -f truehd -i .in -c:a copy -f spdif
  iec61937-truehd96.in/.ref  the first 30 of those AUs as a 96 kHz stream:
                       input timing doubled and rate code 1 in the major syncs
                       (checksums fixed), so the AUs are 80 samples long.

The .ref files are whatever THIS ffmpeg writes. The checked-in ones come from
FFmpeg 9.0.2; FFmpeg master changed the TrueHD padding after that (commit
2db563fac2), so regenerating with a newer ffmpeg can legitimately change
iec61937-truehd.ref, and iec61937-hbr.c must then follow it.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTFILES = os.path.join(HERE, '..', '..', 'testfiles')
FFMPEG = sys.argv[1] if len(sys.argv) > 1 else 'ffmpeg'


def ffmpeg(*args, stdout=None):
    subprocess.run([FFMPEG, '-hide_banner', '-v', 'error', '-y', *args],
                   check=True, stdout=stdout)


def ffmpeg_bytes(*args):
    return subprocess.run([FFMPEG, '-hide_banner', '-v', 'error', *args, '-'],
                          check=True, stdout=subprocess.PIPE).stdout


def write(name, data):
    with open(os.path.join(HERE, name), 'wb') as f:
        f.write(data)
    print(f'{name}: {len(data)} bytes')


def core_frames(es):
    """Split at DTS core sync words, as FFmpeg's dca parser cut this stream."""
    sync = b'\x7f\xfe\x80\x01'
    starts = []
    i = es.find(sync)
    while i >= 0:
        starts.append(i)
        i = es.find(sync, i + 1)
    return [es[a:b] for a, b in zip(starts, starts[1:] + [len(es)])]


def access_units(es):
    aus, o = [], 0
    while o < len(es):
        n = ((es[o] << 8 | es[o + 1]) & 0xfff) * 2
        aus.append(bytearray(es[o:o + n]))
        o += n
    return aus


def fix_check(old, new):
    """Re-derive new's check nibble after edits to its AU header (bytes 0-3).

    The parser requires ((p >> 4) ^ p) & 0xf == 0xf for p = XOR of the AU
    header and substream directory bytes. That is linear in XOR, so flipping
    the check nibble by the edit's own contribution keeps it true."""
    d = (old[1] ^ new[1]) ^ (old[2] ^ new[2]) ^ (old[3] ^ new[3])
    c = (old[0] >> 4) ^ ((old[0] ^ new[0]) & 0x0f) ^ (((d >> 4) ^ d) & 0xf)
    new[0] = (c << 4) | (new[0] & 0x0f)


def grow(au, size):
    """AU padded to `size` bytes, with its 12-bit word count fixed."""
    assert au[4:7] != b'\xf8\x72\x6f' and size % 2 == 0 and size <= 8190
    words = size // 2
    out = au + bytearray((j * 7 + 3) & 0xff for j in range(size - len(au)))
    out[0] = (au[0] & 0xf0) | (words >> 8)
    out[1] = words & 0xff
    fix_check(au, out)
    return out


def crc16_2d(data):
    """FFmpeg's crc_2D (MSB first, poly 0x2D, init 0), byte-swapped as
    ff_mlp_checksum16() sees it."""
    c = 0
    for x in data:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x2d if c & 0x8000 else c << 1) & 0xffff
    return (c & 0xff) << 8 | c >> 8


def at_96k(au):
    """AU of a 96 kHz stream: twice the input timing, rate code 1."""
    out = bytearray(au)
    t = ((au[2] << 8 | au[3]) * 2) & 0xffff
    out[2], out[3] = t >> 8, t & 0xff
    fix_check(au, out)
    if out[4:8] == b'\xf8\x72\x6f\xba':
        assert out[29] & 1 == 0         # 28-byte major sync, no extension
        out[8] = (out[8] & 0x0f) | 1 << 4
        ms = out[4:32]
        crc = crc16_2d(ms[:24]) ^ (ms[24] | ms[25] << 8)
        out[30], out[31] = crc & 0xff, crc >> 8
    return out


def main():
    print(subprocess.run([FFMPEG, '-version'], check=True,
                         stdout=subprocess.PIPE, text=True).stdout.splitlines()[0])

    dts = ffmpeg_bytes('-i', os.path.join(TESTFILES, 'DTS-HD-MA-5.1.ts'),
                       '-map', '0:a:0', '-c:a', 'copy', '-frames:a', '3',
                       '-f', 'dts')
    f = core_frames(dts)
    assert len(f) == 3, len(f)
    write('iec61937-dtshd.in', f[0] + f[1] + b'\xaa' + f[2])
    ffmpeg('-f', 'dts', '-i', os.path.join(HERE, 'iec61937-dtshd.in'),
           '-c:a', 'copy', '-f', 'spdif', '-dtshd_rate', '768000',
           os.path.join(HERE, 'iec61937-dtshd.ref'))
    print('iec61937-dtshd.ref: %d bytes'
          % os.path.getsize(os.path.join(HERE, 'iec61937-dtshd.ref')))

    thd = ffmpeg_bytes('-i', os.path.join(TESTFILES, 'TrueHD-5.1.mkv'),
                       '-map', '0:a:0', '-c:a', 'copy', '-frames:a', '100',
                       '-f', 'truehd')
    aus = access_units(thd)
    assert len(aus) == 100, len(aus)
    kept = [aus[i] for i in range(100) if not (30 <= i <= 32 or 50 <= i <= 69)]
    kept[11] = grow(kept[11], 3200)
    kept[23] = grow(kept[23], 2000)
    write('iec61937-truehd.in', b''.join(kept))
    ffmpeg('-f', 'truehd', '-i', os.path.join(HERE, 'iec61937-truehd.in'),
           '-c:a', 'copy', '-f', 'spdif',
           os.path.join(HERE, 'iec61937-truehd.ref'))
    print('iec61937-truehd.ref: %d bytes'
          % os.path.getsize(os.path.join(HERE, 'iec61937-truehd.ref')))

    write('iec61937-truehd96.in', b''.join(at_96k(a) for a in aus[:30]))
    ffmpeg('-f', 'truehd', '-i', os.path.join(HERE, 'iec61937-truehd96.in'),
           '-c:a', 'copy', '-f', 'spdif',
           os.path.join(HERE, 'iec61937-truehd96.ref'))
    print('iec61937-truehd96.ref: %d bytes'
          % os.path.getsize(os.path.join(HERE, 'iec61937-truehd96.ref')))


if __name__ == '__main__':
    main()
