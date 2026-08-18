#!/usr/bin/env python3
"""Wrap a 188-byte MPEG-TS stream into a real BDAV M2TS (192-byte packets).

Blu-ray / BDAV `.m2ts` is not plain MPEG-TS with a different extension: every
188-byte transport packet is prefixed with a 4-byte TP_extra_header
(2-bit copy_permission_indicator + 30-bit arrival_time_stamp on the 27 MHz
clock), giving 192-byte packets. GStreamer's mpegts packetizer auto-detects the
192-byte stride, so a genuine .m2ts exercises a different code path in
`tsdemux` than the same stream as .ts -- which is the point of shipping both.

Arrival timestamps are derived from the source stream's own PCRs
(piecewise-linear between consecutive PCRs, extrapolated at both ends with the
adjacent segment's rate), so ATS is monotonic and consistent with the real
bitrate instead of invented.

Usage: ./make-m2ts.py <input.ts> <output.m2ts>
"""
import sys

PKT = 188
ATS_MOD = 1 << 30          # ATS is 30 bits at 27 MHz -> wraps every ~39.77 s


def read_pcrs(data, n):
    """[(packet_index, pcr_27mhz)] for every packet carrying a PCR."""
    out = []
    for i in range(n):
        p = data[i * PKT:(i + 1) * PKT]
        afc = (p[3] >> 4) & 0x3
        if afc in (2, 3) and p[4] > 0 and (p[5] & 0x10):
            base = (p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7)
            ext = ((p[10] & 0x01) << 8) | p[11]
            out.append((i, base * 300 + ext))
    return out


def arrival_times(pcrs, n):
    """One 27 MHz arrival time per packet, interpolated from the PCR track."""
    if len(pcrs) < 2:
        sys.exit("need at least two PCRs to derive arrival times")
    ats = [0] * n
    # rate (27 MHz ticks per packet) of the first and last PCR segments, used to
    # extrapolate the head and tail that sit outside the PCR-covered range.
    head_rate = (pcrs[1][1] - pcrs[0][1]) / (pcrs[1][0] - pcrs[0][0])
    tail_rate = (pcrs[-1][1] - pcrs[-2][1]) / (pcrs[-1][0] - pcrs[-2][0])
    for i in range(pcrs[0][0]):
        ats[i] = pcrs[0][1] - round((pcrs[0][0] - i) * head_rate)
    seg = 0
    for i in range(pcrs[0][0], pcrs[-1][0] + 1):
        while seg + 1 < len(pcrs) - 1 and i >= pcrs[seg + 1][0]:
            seg += 1
        (i0, t0), (i1, t1) = pcrs[seg], pcrs[seg + 1]
        ats[i] = t0 + round((i - i0) * (t1 - t0) / (i1 - i0))
    for i in range(pcrs[-1][0] + 1, n):
        ats[i] = pcrs[-1][1] + round((i - pcrs[-1][0]) * tail_rate)
    return ats


def main(src, dst):
    data = open(src, "rb").read()
    n = len(data) // PKT
    tail = len(data) % PKT
    if data[0] != 0x47:
        sys.exit(f"{src}: not 188-byte-aligned MPEG-TS (no sync byte at 0)")
    for i in range(n):
        if data[i * PKT] != 0x47:
            sys.exit(f"{src}: sync loss at packet {i}")
    pcrs = read_pcrs(data, n)
    ats = arrival_times(pcrs, n)
    with open(dst, "wb") as f:
        for i in range(n):
            # copy_permission_indicator = 0 (copy free); ATS in the low 30 bits.
            f.write((ats[i] % ATS_MOD).to_bytes(4, "big"))
            f.write(data[i * PKT:(i + 1) * PKT])
    note = f", dropped {tail}-byte partial trailing packet" if tail else ""
    print(f"{dst}: {n} packets x 192 bytes = {n * 192} bytes "
          f"(from {len(pcrs)} PCRs{note})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
