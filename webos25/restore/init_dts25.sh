#!/bin/sh
# webOS25 DTS + TrueHD restore. Runs at boot via /var/lib/webosbrew/init.d/restore_dts25.
set -u
REG=/mnt/flash/data/gst_1_0_registry.arm.bin
CFG=/etc/umediaserver/device_codec_capability_config.json
LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so
MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so
LOG=/tmp/dts25.log
echo "--- dts25+truehd $(date) ---" >> $LOG 2>&1
# 1) codec-capability override (adds TRUEHD/MLP so umediaserver allocates a decoder resource)
[ -f /var/lib/webosbrew/truehd/codec_capability.json ] && ! grep -q " $CFG " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/codec_capability.json "$CFG" 2>>$LOG
# 2) replace LG.s truehd-less libav with ours (has avdec_truehd/avdec_mlp)
[ -f "$MYLIBAV" ] && ! grep -q " $LGLIBAV " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYLIBAV" "$LGLIBAV" 2>>$LOG
# 2b) gstcool.conf: give avdec_truehd a high SW rank so LG autoplugs it (not the HW path)
GC=/etc/gst/gstcool.conf
[ -f /var/lib/webosbrew/truehd/gstcool.conf ] && ! grep -q " $GC " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/gstcool.conf "$GC" 2>>$LOG
# 2c) container demuxers with DTS re-enabled (mp4/ts/m2ts DTS -> audio/x-dts).
#     Patched isomp4/mpegtsdemux default dts_support=TRUE. Bound BEFORE the regen
#     below so the registry picks them up at their normal path.
ISO=/usr/lib/gstreamer-1.0/libgstisomp4.so
TSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so
[ -f /var/lib/webosbrew/demux25/libgstisomp4.so ] && ! grep -q " $ISO " /proc/mounts 2>/dev/null && mount -n --bind -o ro /var/lib/webosbrew/demux25/libgstisomp4.so "$ISO" 2>>$LOG
[ -f /var/lib/webosbrew/demux25/libgstmpegtsdemux.so ] && ! grep -q " $TSD " /proc/mounts 2>/dev/null && mount -n --bind -o ro /var/lib/webosbrew/demux25/libgstmpegtsdemux.so "$TSD" 2>>$LOG
# 3) regenerate the media registry (fresh) with dtsdec + our libav, then write it to the media path
rm -f /tmp/gst_dts_reg.bin
LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs \
GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin \
GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \
GST_REGISTRY_UPDATE=yes /usr/bin/gst-inspect-1.0 >/dev/null 2>>$LOG
# only overwrite the media registry if our regen actually contains the decoders
if GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin GST_REGISTRY_UPDATE=no /usr/bin/gst-inspect-1.0 avdec_truehd >/dev/null 2>&1; then
  cp -f /tmp/gst_dts_reg.bin "$REG" 2>>$LOG && echo "registry updated (dtsdec+truehd)" >>$LOG
else
  echo "WARN: regen missing avdec_truehd, left registry untouched" >>$LOG
fi
exit 0
