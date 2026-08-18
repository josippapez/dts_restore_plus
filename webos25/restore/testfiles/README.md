# DTS container test files

Ready-to-play samples to verify the webOS 25 container-DTS patch (mp4/ts/m2ts).
Copy them to a USB stick, plug it into the TV, and open each in the **Media Player**.
You should hear audio; before the patch these containers played silent.

| File | Container | Content | Exercises |
|------|-----------|---------|-----------|
| `DTS-HD-MA-5.1.ts`   | MPEG-TS, 188-byte packets       | H.262 video + **DTS-HD MA 5.1** | `tsdemux`, 188-byte stride (.ts) |
| `DTS-HD-MA-5.1.m2ts` | **BDAV M2TS, 192-byte packets** | same elementary streams         | `tsdemux`, 192-byte stride (.m2ts) |
| `DTS-in-mp4.mp4`     | ISO-BMFF | H.264 video + **DTS 5.1** (dtsc)| `qtdemux` (.mp4) |

Notes:
- The `.ts`/`.m2ts` audio is DTS-HD MA; the open decoder plays its DTS **core**
  (5.1), not the lossless MA extension — expected.
- Provenance: audio/`.ts` from samples.ffmpeg.org (A-codecs/DTS); the `.mp4` is
  that DTS core re-muxed with an H.264 video track via GPAC/MP4Box.
- Decode is verified accurate on a real C5: native discrete 5.1 (6 distinct
  channels), matching a reference DTS decoder within ~0.1–0.2 dB per channel.
  No stereo downmix (unlike the CX/upstream tool).

## The `.m2ts` is a real BDAV stream, not a renamed `.ts`

Until 2026-08-18 `DTS-HD-MA-5.1.m2ts` was a byte-identical copy of the `.ts`
(same md5), so the `.m2ts` self-test case only proved that the extension routed
to `tsdemux` — it exercised no code the `.ts` case didn't. Blu-ray `.m2ts` is
genuinely different: each 188-byte transport packet carries a 4-byte
TP_extra_header (2-bit `copy_permission_indicator` + 30-bit 27 MHz
`arrival_time_stamp`), giving a **192-byte** stride that the GStreamer mpegts
packetizer has to auto-detect. The file is now that.

It is generated from the `.ts` by [`make-m2ts.py`](make-m2ts.py), which only
prepends the TP_extra_header — the 188-byte packets, PIDs and PMT (stream_type
`0x86`) are passed through untouched, so the BluRay/HDMV layout LG's demuxer
needs is preserved. Arrival timestamps are interpolated from the source stream's
own PCRs. Regenerate with:

```sh
./make-m2ts.py DTS-HD-MA-5.1.ts DTS-HD-MA-5.1.m2ts
```

Verified on the dev host after generation:
- `tsdemux` reports `have packetsize detected: 192 bytes` for the `.m2ts` and
  `188 bytes` for the `.ts` — the two cases now take different paths.
- `filesrc ! tsdemux ! dcaparse ! avdec_dca ! audioconvert ! wavenc` yields a
  **byte-identical** WAV from both files (sha256 `52cfe4b0…`), so the wrapping
  does not disturb the audio.

**Do NOT "dedupe" this back to a copy of the `.ts`.** And do not re-mux either
file with ffmpeg — see the warning in
[`../../app/payload/testfiles/README`](../../app/payload/testfiles/README): on the
C5, LG's demuxer exposes no audio stream at all from an ffmpeg-muxed TS.
