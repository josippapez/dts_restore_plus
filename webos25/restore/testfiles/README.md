# DTS container test files

Ready-to-play samples to verify the webOS 25 container-DTS patch (mp4/ts/m2ts).
Copy them to a USB stick, plug it into the TV, and open each in the **Media Player**.
You should hear audio; before the patch these containers played silent.

| File | Container | Content | Exercises |
|------|-----------|---------|-----------|
| `DTS-HD-MA-5.1.ts`   | MPEG-TS  | H.262 video + **DTS-HD MA 5.1** | `tsdemux` (.ts) |
| `DTS-HD-MA-5.1.m2ts` | MPEG-TS  | same stream, `.m2ts` extension  | `tsdemux` (.m2ts) |
| `DTS-in-mp4.mp4`     | ISO-BMFF | H.264 video + **DTS 5.1** (dtsc)| `qtdemux` (.mp4) |

Notes:
- The `.ts`/`.m2ts` audio is DTS-HD MA; the open decoder plays its DTS **core**
  (5.1), not the lossless MA extension — expected.
- Provenance: audio/`.ts` from samples.ffmpeg.org (A-codecs/DTS); the `.mp4` is
  that DTS core re-muxed with an H.264 video track via GPAC/MP4Box.
- Decode is verified accurate on a real C5: native discrete 5.1 (6 distinct
  channels), matching a reference DTS decoder within ~0.1–0.2 dB per channel.
  No stereo downmix (unlike the CX/upstream tool).
