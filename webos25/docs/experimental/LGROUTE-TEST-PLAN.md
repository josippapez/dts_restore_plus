# LG eARC bitstream route, approach A — on-TV test plan

Branch `lg-route`. The test build is `webos25/restore/switch-out/libgstadecswitch.so`
(adecswitch + hdbitstream + lgroute). No kernel module, no boot-script change,
no gate/app bump: it is opted in by a marker file and reverted by restoring the
released `.so`.

What it does (decomp lines in `webos25/restore/src/lgroute-policy.h`): when the
marker exists and a receiver with Pass Through/Auto is reported, DTS / DTS-HD /
TrueHD get `hdbitstream` instead of a decoder. It packs IEC 61937 HBR bursts,
sets `pcm-multi-ch=TRUE` on LG's audiosink right before the carrier CAPS reach
it (so the kernel builds the PCM bypass head and LG's sound-output code wires
renderer → mixer → eARC itself), and sets it `FALSE` right before the first
buffer (so the first event on the bypass is `es=true, codec=mat, channels=8,
bits-per-sample=16, data-rate=768000` and the eARC output switches itself to
non-audio HBR). Every step is logged to `/tmp/dts25-lgroute.log`.

## 1. Install the test .so without disturbing the released install

The hook stages the plugin at `/var/lib/webosbrew/dts25/libgstadecswitch.so`
and regenerates the media registry from `/var/lib/webosbrew/dts25`
(`init_dts25.sh` APPLY 3). Keep the released file next to it.

```sh
# from the Mac, repo = ~/dts_restore_work/dts_restore-lgroute
scp webos25/restore/switch-out/libgstadecswitch.so c5:/tmp/libgstadecswitch.so.lgroute

# on the TV (root)
cd /var/lib/webosbrew/dts25
[ -f libgstadecswitch.so.release ] || cp -a libgstadecswitch.so libgstadecswitch.so.release
cp -f /tmp/libgstadecswitch.so.lgroute libgstadecswitch.so
sha256sum libgstadecswitch.so        # 6e6f043083fe1077adcbd1919b60e33a24909b86494ce8d4419f8ebd5b62d610

# regenerate the registry the way the boot hook does, prove both elements, commit
REG_TMP=/tmp/gst_dts_reg.bin; rm -f $REG_TMP
LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs GST_REGISTRY_1_0=$REG_TMP \
GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \
GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 30 /usr/bin/gst-inspect-1.0 >/dev/null
for e in dtsdec avdec_truehd adecswitch hdbitstream; do
  GST_REGISTRY_1_0=$REG_TMP GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 $e >/dev/null 2>&1 && echo "ok $e" || echo "MISSING $e"
done
cp -f $REG_TMP /mnt/flash/data/gst_1_0_registry.arm.bin
```

If any element is MISSING, do not copy the registry: restore the released
`.so` (section 5) and send the output of the gst-inspect line.

## 2. C5 run (OLED77C51LA, fw 33.31.68, no receiver)

Without a receiver `earc_on` is false, so LG will not connect the route and
nothing reaches an output. What this run settles: the head the kernel builds,
the flag values at each step, that the first bypass event goes out as ES, and
that playback and teardown are clean. The receiver check is skipped with the
word `force` in the marker.

```sh
echo force > /var/lib/webosbrew/dts25/lgroute-test
: > /tmp/dts25-lgroute.log
LL0=$(wc -l < /var/log/legacy-log); echo "legacy-log starts at line $LL0"
dmesg -c >/dev/null 2>&1 || true
```

Play `webos25/restore/testfiles/DTS-HD-MA-5.1.m2ts` (or any DTS-HD MA MKV) the
usual way, let it run 15 s, stop. Then the same with `TrueHD-5.1.mkv`.
Expected on the speakers: **silence** for these files (the bursts go to the
bypass appsrc only, never to the main path), video normal, and a Dolby or AAC
file afterwards plays normally through the speakers.

Collect after each file:

```sh
cat /tmp/dts25-lgroute.log
tail -n +$LL0 /var/log/legacy-log | grep -n \
  'Set pcm (multi channel) bypass\|pcm bypass status\|caps "\|Caps is first\|audio info\] codec\|codecType=\|bypass_head uid\|Event BYPASSS\|pcm_bypass or pcm_multi_ch\|acmod changed\|Lip-sync\|underflow\|renderer set pcm info\|pcm bypass uid\|es bypass uid\|PCM bypass pipeline\|bypass is not yet created\|rts, out channels\|bPcmBypass\|BYPASS'
dmesg | grep -i 'renderer set pcm info\|pcm bypass uid\|es bypass uid\|PCM bypass pipeline\|bypass is not yet\|rts, out channels\|bPcmBypass\|set pcm format\|param in\|param out' | tail -40
ls /proc/aaudio/
cat /proc/aaudio/dot_block_connection
cat /proc/aaudio/dot_module_connection 2>/dev/null | grep -i 'renderer_bypass\|rts\|srcbin\|lipsync' | head -40
```

What each line must say (file:line = decomp):

| expected | source of truth |
|---|---|
| lgroute: `armed on audiosink…`, then `CAPS at … (carrier): audio/x-raw, … es=(boolean)true, codec-str=(string)mat, data-rate=(int)768000 …` | our probe sits on the sink's own pad |
| lgroute: `pcm-multi-ch=TRUE at CAPS … (sink reads back TRUE)` | render_props.c:1218-1225 |
| legacy-log: `Set pcm (multi channel) bypass TRUE`, `pcm bypass status of renderer [n] changing 0 -> 1` | lgao.c:1698 |
| legacy-log: `caps "audio/x-raw, … rate=(int)192000, channels=(int)8 …"` then `Caps is first or updated` | render_element.c:323, :563 |
| kernel: `renderer set pcm info. bits per sample: 16, channels: 8, sampling rate: 192000, … pcm-bypass(google): 0, pcm-multi-ch(netflix): 1` | aapi_media.c:1710 |
| kernel: `PCM bypass pipeline is created. head uid : 0x…` and `pcm bypass uid = 0x…` | renderer.c:1651, aapi_media.c:1299 |
| lgroute: `first buffer at audiosink: N bytes, flags 0x…, ES bit set` then `FLIP: pcm-multi-ch=FALSE at BUFFER` | the flip, before lgao_feed |
| legacy-log: `Set pcm (multi channel) bypass FALSE`, `changing 1 -> 0` | lgao.c:1698 |
| legacy-log: `Event BYPASSS : … es=true … codec=mat … channels=8 … bits-per-sample=16 … data-rate=768000` and **no** `pcm_bypass or pcm_multi_ch` | lgao.c:1977 vs :1983 |
| kernel at start: `set pcm bypass rts, out channels: 8` | renderer.c:682 via aapi_media.c:1660 |
| dot_module_connection: a `renderer_bypass` chain `rts → srcbin → lipsync` | renderer.c:1569-1580 |
| lgroute: `sending DTS-HD bursts after 500 ms of silence` (MAT for TrueHD) | hdbitstream |
| lgroute on stop: `disarmed on audiosink after 1 flip(s)` | |

Failure signatures and what they mean:

- `pcm_bypass or pcm_multi_ch` instead of `Event BYPASSS`: the first events went
  out from inside the CAPS handler (render_element.c:586-593, acmod/downmix
  changed) while the flag was still TRUE. Send the `acmod changed` line; the
  fix is to match the previous stream's acmod.
- `es bypass uid` / `bypass is not yet created` instead of the PCM lines: the
  flag did not reach the kernel before GET_BYPASS_UID; send the ordering of the
  `Set pcm … TRUE` and `caps "` lines.
- `CAPS at … (not ours)` for our stream: something between adecswitch and the
  sink rewrote the caps (the `es`/`data-rate` fields are gone); send the line.
- `falling back to decoding: …` in the lgroute log: hdbitstream gave up and
  the decoder was plugged (the reason says why); speakers then play the file.
- A second `Caps is first or updated` + `renderer set pcm info` with
  `pcm-multi-ch(netflix): 0` later in the same playback: the sink re-opened
  the renderer (track change / `Restart renderer` line) after the flip.

Extra, if `/proc/aaudio` has per-module counters (list it and send): the bypass
`srcbin` input vs output bytes. Equal means it passed the 8 ch / 192 kHz frames
untouched; a 4:1 ratio means it resampled to 48 kHz and approach A needs the
format command Glasshouse's module sends (approach B territory).

## 3. Tester with an eARC receiver

Same install (section 1), then:

```sh
: > /var/lib/webosbrew/dts25/lgroute-test     # empty: the receiver check stays on
: > /tmp/dts25-lgroute.log
LL0=$(wc -l < /var/log/legacy-log)
cat /run/dts25/sound-output                     # must read "<pid> external_arc passThrough" or "... auto"
```

TV: Sound Out = HDMI ARC/eARC device, Digital Sound Out = Pass Through (Auto
also counts). Play a DTS-HD MA file, then a TrueHD (Atmos if available) file,
15 s each. **If the receiver plays loud noise, stop playback at once** and send
the logs anyway: that means the bursts reached it as PCM.

Expected: the receiver's display shows DTS-HD MA / DTS:X for the first file and
Dolby TrueHD / Atmos for the second, after about half a second of silence
(the lead-in). On the TV, in addition to the section 2 lines:

- kernel: `bPcmBypass 1 *bypass 1` and `BYPASS` (alsa_sndout.c:2286-2290,
  :2527): LG connected renderer → mixer → eARC;
- kernel: `output_arc mat 8 ch … 768000 Hz` or similar from `_cb_output_status`
  (output.c:756): dsp1 reported codec MAT at 768 kHz, so the eARC link went to
  HBR non-PCM;
- the receiver's display.

Send: `/tmp/dts25-lgroute.log`, the two greps and the two `/proc/aaudio` files
from section 2, `dmesg | grep -i 'earc\|arc\|bypass\|output' | tail -80`, a
photo of the receiver's display, the receiver model, and `sh report-tv.sh`.

If the display says PCM or Multi-PCM: the link stayed in L-PCM (output.c
mapped the codec to PCM, or dsp1's HBR setting was overridden); the bursts are
still bit-exact in the lanes, so a receiver that auto-detects may still lock.
Report exactly what the display shows and whether there is sound.

## 4. What approach A cannot settle on the C5 (needs the receiver tester)

- Whether the PCM head's `rts` and `srcbin` modules pass the 8 ch / 192 kHz
  frames bit-exact under an `es=true` first event (Glasshouse's module sent
  the bypass srcbin an explicit format command; here nothing does, see
  section 2's counter check).
- Whether the eARC link ends up in HBR non-PCM mode when the ARM side
  (`_set_format` with codec MAT) and dsp1 (`HAL_SetArcEsMode(768000)`) both
  configure it.
- Whether LG tears the route down and restores its own on stop and on the next
  Dolby stream (it should: the same code that connected it re-evaluates on
  every output change).

## 5. Uninstall

```sh
rm -f /var/lib/webosbrew/dts25/lgroute-test
cd /var/lib/webosbrew/dts25 && mv -f libgstadecswitch.so.release libgstadecswitch.so
# then the registry block from section 1 again (regenerate, prove dtsdec/avdec_truehd/adecswitch, commit)
```

A reboot after that leaves the TV exactly as the released install had it.
