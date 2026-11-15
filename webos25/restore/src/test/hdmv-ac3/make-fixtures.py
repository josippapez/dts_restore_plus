#!/usr/bin/env python3
"""Generate short M2TS fixtures and their original elementary audio streams."""

import argparse
from collections import defaultdict
from pathlib import Path
import subprocess


PMT_PID = 0x0100
TRUEHD_PID = 0x1100
AC3_PIDS = (0x1101, 0x1102)
START_PTS = 90_000


def encode(ffmpeg, codec, frequency):
    return subprocess.check_output([
        ffmpeg, '-v', 'error', '-nostdin', '-f', 'lavfi', '-i',
        f'sine=frequency={frequency}:sample_rate=48000:duration=0.320',
        '-ac', '2', '-c:a', codec, '-strict', 'experimental',
        '-flags:a', '+bitexact', '-f', codec, 'pipe:1',
    ])


def truehd_chunks(audio):
    """Group 24 TrueHD access units per PES (20 ms at 48 kHz)."""
    offset = 0
    chunks = []
    for _ in range(16):
        start = offset
        for _ in range(24):
            size = 2 * (int.from_bytes(audio[offset:offset + 2], 'big') & 0x0fff)
            if size < 4 or offset + size > len(audio):
                raise ValueError('invalid TrueHD access unit')
            offset += size
        chunks.append(audio[start:offset])
    if offset != len(audio):
        raise ValueError('unexpected TrueHD duration')
    return chunks


def ac3_chunks(audio):
    """One AC-3 frame per PES (32 ms at 48 kHz)."""
    size, remainder = divmod(len(audio), 10)
    frames = [audio[i * size:(i + 1) * size] for i in range(10)]
    if remainder or not all(frame.startswith(b'\x0b\x77') for frame in frames):
        raise ValueError('unexpected AC-3 frame layout')
    return frames


def psi_section(table_id, body):
    section = bytes([table_id]) + (0xb000 | (len(body) + 4)).to_bytes(2, 'big') + body
    crc = 0xffffffff
    for byte in section:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return b'\x00' + section + crc.to_bytes(4, 'big')  # PSI pointer field


def pmt(registration):
    def entry(stream_type, pid, descriptors):
        return (bytes([stream_type]) + (0xe000 | pid).to_bytes(2, 'big')
                + (0xf000 | len(descriptors)).to_bytes(2, 'big') + descriptors)

    ac3_descriptors = b'\x05\x04AC-3\x81\x04\x06\x2d\x04\x00'
    body = (b'\x00\x01\xc1\x00\x00' + (0xe000 | AC3_PIDS[0]).to_bytes(2, 'big')
            + b'\xf0\x06\x05\x04' + registration)
    # Type 0x83 deliberately omits the ATSC AC-3 descriptor. The companion's
    # AC-3 caps must come from its PES substream, independent of the PMT.
    body += entry(0x83, TRUEHD_PID, b'')
    body += b''.join(entry(0x81, pid, ac3_descriptors) for pid in AC3_PIDS)
    return psi_section(0x02, body)


def pes(audio, pts, extension):
    timestamp = bytes([
        0x21 | ((pts >> 29) & 0x0e), (pts >> 22) & 0xff,
        1 | ((pts >> 14) & 0xfe), (pts >> 7) & 0xff, 1 | ((pts << 1) & 0xfe),
    ])
    if extension is None:
        stream_id, flags, fields = 0xbd, 0x80, timestamp
    else:
        stream_id, flags = 0xfd, 0x81
        fields = timestamp + bytes([0x01, 0x81, extension])
    body = bytes([0x84, flags, len(fields)]) + fields + audio
    return b'\x00\x00\x01' + bytes([stream_id]) + len(body).to_bytes(2, 'big') + body


class M2ts:
    def __init__(self):
        self.packets = []
        self.continuity = defaultdict(int)

    def write(self, pid, data, pcr=None):
        first = True
        while data:
            flags = b''
            capacity = 184
            if pcr is not None:
                flags = b'\x10' + ((pcr << 15) | 0x7e00).to_bytes(6, 'big')
                capacity -= 1 + len(flags)
                pcr = None
            payload, data = data[:capacity], data[capacity:]
            unused = 184 - len(payload)
            adaptation = b''
            if unused:
                fields = (flags or b'\x00')[:unused - 1]
                adaptation = bytes([unused - 1]) + fields + b'\xff' * (unused - 1 - len(fields))
            header = bytes([
                0x47, (0x40 if first else 0) | (pid >> 8), pid & 0xff,
                (0x30 if adaptation else 0x10) | self.continuity[pid],
            ])
            arrival = (len(self.packets) * 300).to_bytes(4, 'big')
            self.packets.append(arrival + header + adaptation + payload)
            self.continuity[pid] = (self.continuity[pid] + 1) % 16
            first = False

    def data(self):
        return b''.join(self.packets)


def mux(truehd, companion, later_tracks, registration=b'HDMV', extension=0x72):
    muxer = M2ts()
    pat = psi_section(0x00, b'\x00\x01\xc1\x00\x00\x00\x01\xe1\x00')
    program_map = pmt(registration)
    events = []
    for i, chunk in enumerate(truehd):
        events.append((i * 1800, TRUEHD_PID, extension, chunk))
    for i, frame in enumerate(companion):
        events.append((i * 2880, TRUEHD_PID, 0x76, frame))
    for pid, frames in zip(AC3_PIDS, later_tracks):
        for i, frame in enumerate(frames):
            events.append((i * 2880, pid, 0x71, frame))

    next_tables = 0
    for offset, pid, stream_extension, audio in sorted(events, key=lambda event: event[0]):
        if offset >= next_tables:
            muxer.write(0, pat)
            muxer.write(PMT_PID, program_map)
            next_tables += 9000
        pts = START_PTS + offset
        pcr = pts - 9000 if pid == AC3_PIDS[0] else None
        muxer.write(pid, pes(audio, pts, stream_extension), pcr)
    return muxer.data()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?',
                        default=Path(__file__).with_name('fixtures'))
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    truehd = encode(args.ffmpeg, 'truehd', 440)
    companion, later_1101, later_1102 = [
        encode(args.ffmpeg, 'ac3', frequency) for frequency in (550, 660, 770)
    ]
    truehd_pes = truehd_chunks(truehd)
    ac3_pes = ac3_chunks(companion)
    later_pes = [ac3_chunks(audio) for audio in (later_1101, later_1102)]
    files = {
        'expected.truehd': truehd,
        'expected.ac3': companion,
        'expected-1101.ac3': later_1101,
        'expected-1102.ac3': later_1102,
        'with-ac3.m2ts': mux(truehd_pes, ac3_pes, later_pes),
        'no-ac3.m2ts': mux(truehd_pes, [], later_pes),
        'extensionless-truehd.m2ts': mux(truehd_pes, [], later_pes, extension=None),
        'non-hdmv.m2ts': mux(truehd_pes, ac3_pes, later_pes, registration=b'TEST'),
    }
    for name, data in files.items():
        (args.output / name).write_bytes(data)


if __name__ == '__main__':
    main()
