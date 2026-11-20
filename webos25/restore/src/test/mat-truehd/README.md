# TrueHD MAT timing regression under QEMU

Run on Linux or WSL with Docker and `qemu-arm`. The test uses the
`dts25-dtsdec-build` image prepared by [`build.sh`](../../../build.sh),
including the release build's ARM soft-float toolchain and static FFmpeg
n4.4.4 libraries. It compiles the current [`gstdtsdec.c`](../../gstdtsdec.c)
into a test executable and runs it against the image's ARM GStreamer runtime.
The test itself runs offline and leaves the release artifacts untouched.

```sh
bash webos25/restore/src/test/mat-truehd/run-qemu.sh
```

Set `QEMU_ARM` to a different QEMU user-mode executable or
`DTSDEC_BUILD_IMAGE` to the name of an equivalent decoder build image.

The test gets its audio from the generated sine-wave
[TrueHD elementary stream](../hdmv-ac3/fixtures/expected.truehd) already in the
demux suite. It sets the 16-bit timing word and, in the 96 and 192 kHz cases,
the major-sync sample-rate code to construct synthetic timing cases. It
feeds the access units through the real `GstAudioDecoder` queue and FFmpeg MAT
muxer. It substitutes the LG audio controls and skips the receiver startup
mute in the test so that the first burst can be inspected.

| Case | Expected result |
| --- | --- |
| Nominal 16-bit wrap at major sync | The modular delta is 40 samples and the existing muxer continues. |
| Short gap at major sync | An 80-sample delta stays on the normal muxer path. |
| Major-sync threshold at 48 kHz | 639 samples keep the muxer. 640 samples start a new sequence. |
| Major-sync threshold at 96 kHz | 1,279 samples keep the muxer. 1,280 samples start a new sequence. |
| Major-sync threshold at 192 kHz | 2,559 samples keep the muxer. 2,560 samples start a new sequence. |
| 1,770-sample jump at major sync | Exactly one resync occurs without rearming the audio route. The resulting MAT bursts match a fresh decoder started on the same major sync byte for byte, carry the native ES flag and have matching input-derived timestamps. EOS generates no extra burst. |
| Flush then start on a minor unit | Previous timing is cleared. Playback waits for the next major sync, then produces the same MAT bytes and timestamps as a fresh decoder. |
| Truncated major sync or 44.1 kHz family | The existing decoder fallback disarms the route and emits no MAT burst. |

At the threshold, the tests also compare the MAT bytes and timestamps with a
fresh muxer and check that the audio route stays open. FFmpeg's
`Unusual frame timing` warnings are expected in the three cases just below
the conservative recovery threshold.

The QEMU run verifies software behavior. Audible recovery and receiver format
lock on hardware require the separate checks in [`RELEASING.md`](../../../../../RELEASING.md).
