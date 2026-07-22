#!/bin/sh
#
# install.sh - Install the unified DTS + TrueHD/MLP audio restore on a rooted
#              LG webOS 25 TV (C5 class). PROVEN working on a real LG C5.
#
# Run as root ON THE TV, from this restore/ directory (copied via scp), which
# must contain:
#     out/libgstdtsdec.so        patched dtsdec (S32LE, accepts A_DTS)   [build.sh]
#     out/libdca.so.0            DTS decode library (armel)              [build.sh]
#     truehd-out/libgstlibav.so  gst-libav with avdec_truehd/avdec_mlp   [build-truehd.sh]
#     truehd-out/libav*.so*      minimal ffmpeg n4.4.4 libs (+symlinks)  [build-truehd.sh]
#     truehd-out/libsw*.so*      libswresample (+symlinks)               [build-truehd.sh]
#     demux-out/libgstisomp4.so       mp4 demux, DTS re-enabled (OPTIONAL) [build-demux.sh]
#     demux-out/libgstmpegtsdemux.so  ts/m2ts demux, DTS re-enabled (OPT.) [build-demux.sh]
#   (the boot/apply script is EMBEDDED in this installer - no sibling file needed)
#
# All binaries are PREBUILT and bundled; you do NOT need Docker or to build
# anything. Just: copy this folder to the TV, then run (as root):
#
#   sh install.sh
#
# What it does (idempotent, guarded, logged to /tmp/dts25.log, always exit 0):
#   1. Stages the DTS payload  -> /var/lib/webosbrew/dts25/{,libs/}
#   2. Stages the TrueHD payload-> /var/lib/webosbrew/truehd/{,libs/}
#   2c. Stages the container demuxers (if demux-out/ present)
#        -> /var/lib/webosbrew/demux25/ (mp4/ts/m2ts DTS; boot hook binds them)
#   3. GENERATES two config overrides by EDITING the TV's own live /etc files
#      (no LG config file is shipped):
#        a. /etc/umediaserver/device_codec_capability_config.json
#           -> insert TRUEHD + MLP audio-codec objects after the DTSE entry
#        b. /etc/gst/gstcool.conf
#           -> insert avdec_truehd=310 + avdec_mlp=310 after [sw_decoder]
#      Both are written to /var/lib/webosbrew/truehd/ and bind-mounted by the
#      boot hook (originals are never modified in place - fully reversible).
#   4. Installs the canonical init_dts25.sh and symlinks the boot hook
#      /var/lib/webosbrew/init.d/restore_dts25 -> it.
#   5. Applies everything now (runs the init script) + restarts the media pipe.
#
set -u

LOG=/tmp/dts25.log
DTS_DEST=/var/lib/webosbrew/dts25
DTS_LIBS=$DTS_DEST/libs
THD_DEST=/var/lib/webosbrew/truehd
THD_LIBS=$THD_DEST/libs
INITD=/var/lib/webosbrew/init.d
HOOK=$INITD/restore_dts25
INIT_SCRIPT=$DTS_DEST/init_dts25.sh

# Live LG config files we derive our (bind-mounted) overrides from.
CFG_LIVE=/etc/umediaserver/device_codec_capability_config.json
CFG_OVR=$THD_DEST/codec_capability.json
GC_LIVE=/etc/gst/gstcool.conf
GC_OVR=$THD_DEST/gstcool.conf

# Bind targets the boot hook manages (unmount here so we read pristine originals).
LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so
REG_TARGET=/mnt/flash/data/gst_1_0_registry.arm.bin

SELF_DIR=$(cd "$(dirname "$0")" && pwd)

log() { echo "[dts25-install $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

log "=== unified DTS+TrueHD install start ==="

# --- 1. Stage DTS payload --------------------------------------------------
mkdir -p "$DTS_LIBS" || { log "FATAL: cannot create $DTS_LIBS"; exit 0; }

if [ -f "$SELF_DIR/out/libgstdtsdec.so" ]; then
  cp -f "$SELF_DIR/out/libgstdtsdec.so" "$DTS_DEST/libgstdtsdec.so" \
    && log "installed libgstdtsdec.so -> $DTS_DEST/" || log "WARN: copy libgstdtsdec.so failed"
else
  log "WARN: $SELF_DIR/out/libgstdtsdec.so not found (run build.sh first)"
fi
if [ -f "$SELF_DIR/out/libdca.so.0" ]; then
  cp -f "$SELF_DIR/out/libdca.so.0" "$DTS_LIBS/libdca.so.0" \
    && log "installed libdca.so.0 -> $DTS_LIBS/" || log "WARN: copy libdca.so.0 failed"
else
  log "WARN: $SELF_DIR/out/libdca.so.0 not found (run build.sh first)"
fi

# --- 2. Stage TrueHD payload (preserve the .so version symlinks) -----------
mkdir -p "$THD_LIBS" || { log "FATAL: cannot create $THD_LIBS"; exit 0; }

if [ -f "$SELF_DIR/truehd-out/libgstlibav.so" ]; then
  cp -f "$SELF_DIR/truehd-out/libgstlibav.so" "$THD_DEST/libgstlibav.so" \
    && log "installed libgstlibav.so -> $THD_DEST/" || log "WARN: copy libgstlibav.so failed"
else
  log "WARN: $SELF_DIR/truehd-out/libgstlibav.so not found (run build-truehd.sh first)"
fi

n=0
for f in "$SELF_DIR"/truehd-out/libav*.so* "$SELF_DIR"/truehd-out/libsw*.so*; do
  [ -e "$f" ] || continue
  # cp -P preserves the soname symlinks (libavcodec.so.58 -> libavcodec.so.58.x.y).
  cp -Pf "$f" "$THD_LIBS/" && n=$((n+1))
done
log "staged $n ffmpeg lib entries -> $THD_LIBS/"

# --- 2c. Stage container-demuxer payload (mp4/ts/m2ts DTS re-enabled) -------
# Patched isomp4/mpegtsdemux (dts_support default TRUE). Optional: if demux-out/
# is absent the boot hook simply skips the demuxer binds (MKV DTS still works).
DMX_DEST=/var/lib/webosbrew/demux25
DMX_ISO=/usr/lib/gstreamer-1.0/libgstisomp4.so
DMX_TSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so
mkdir -p "$DMX_DEST" || log "WARN: cannot create $DMX_DEST"
for so in libgstisomp4.so libgstmpegtsdemux.so; do
  if [ -f "$SELF_DIR/demux-out/$so" ]; then
    cp -f "$SELF_DIR/demux-out/$so" "$DMX_DEST/$so" \
      && log "installed $so -> $DMX_DEST/" || log "WARN: copy $so failed"
  else
    log "note: $SELF_DIR/demux-out/$so not found; container DTS (mp4/ts/m2ts) will be skipped"
  fi
done

# --- 3. Unmount any existing binds so we regenerate from PRISTINE originals -
for T in "$CFG_LIVE" "$GC_LIVE" "$LGLIBAV" "$DMX_ISO" "$DMX_TSD" "$REG_TARGET"; do
  if grep -q " $T " /proc/mounts 2>/dev/null; then
    umount "$T" 2>>"$LOG" && log "unmounted stale bind $T" || log "WARN: could not umount $T"
  fi
done

# --- 3a. Generate the codec-capability override (insert TRUEHD+MLP after DTSE)
# awk is idempotent: it only injects when the DTSE object is seen and, guarded
# below, only when TRUEHD is not already present in the source.
if [ -f "$CFG_LIVE" ]; then
  if grep -q '"TRUEHD"' "$CFG_LIVE"; then
    log "note: live capability config already has TRUEHD; copying as-is"
    cp -f "$CFG_LIVE" "$CFG_OVR"
  else
    awk '
      /"name" : "DTSE"/ { indts=1 }
      { print }
      indts && /^ *},/ {
        print "    {";
        print "      \"name\" : \"TRUEHD\",";
        print "      \"channels\" : 8";
        print "    },";
        print "";
        print "    {";
        print "      \"name\" : \"MLP\",";
        print "      \"channels\" : 8";
        print "    },";
        indts=0
      }
    ' "$CFG_LIVE" > "$CFG_OVR" && log "generated capability override $CFG_OVR (TRUEHD+MLP after DTSE)" \
      || log "WARN: capability override generation failed"
  fi
else
  log "WARN: $CFG_LIVE not present; cannot generate capability override"
fi

# --- 3b. Generate the gstcool.conf override (avdec_truehd/mlp=310 rank lever)
if [ -f "$GC_LIVE" ]; then
  if grep -q '^avdec_truehd=' "$GC_LIVE"; then
    log "note: live gstcool.conf already has avdec_truehd; copying as-is"
    cp -f "$GC_LIVE" "$GC_OVR"
  else
    awk '
      { print }
      /^\[sw_decoder\]/ { print "avdec_truehd=310"; print "avdec_mlp=310" }
    ' "$GC_LIVE" > "$GC_OVR" && log "generated gstcool override $GC_OVR (avdec_truehd/mlp=310)" \
      || log "WARN: gstcool override generation failed"
  fi
else
  log "WARN: $GC_LIVE not present; cannot generate gstcool override"
fi

# --- 4. Install the canonical boot/apply script (embedded) + hook ----------
# The boot/apply logic is embedded here as base64 so this installer is a single
# self-contained script (no sibling init file needed). Decodes verbatim to the
# proven init_dts25.sh.
base64 -d > "$INIT_SCRIPT" <<'INIT_B64'
IyEvYmluL3NoCiMgd2ViT1MyNSBEVFMgKyBUcnVlSEQgcmVzdG9yZS4gUnVucyBhdCBib290IHZpYSAvdmFyL2xpYi93ZWJvc2JyZXcvaW5pdC5kL3Jlc3RvcmVfZHRzMjUuCnNldCAtdQpSRUc9L21udC9mbGFzaC9kYXRhL2dzdF8xXzBfcmVnaXN0cnkuYXJtLmJpbgpDRkc9L2V0Yy91bWVkaWFzZXJ2ZXIvZGV2aWNlX2NvZGVjX2NhcGFiaWxpdHlfY29uZmlnLmpzb24KTEdMSUJBVj0vdXNyL2xpYi9nc3RyZWFtZXItMS4wL2xpYmdzdGxpYmF2LnNvCk1ZTElCQVY9L3Zhci9saWIvd2Vib3NicmV3L3RydWVoZC9saWJnc3RsaWJhdi5zbwpMT0c9L3RtcC9kdHMyNS5sb2cKRVhQRUNUX0dTVD0xLjI0CnRvYXN0KCkgeyBsdW5hLXNlbmQgLW4gMSBsdW5hOi8vY29tLndlYm9zLm5vdGlmaWNhdGlvbi9jcmVhdGVUb2FzdCAie1wic291cmNlSWRcIjpcImlvLmdpdGh1Yi5qb3NpcHBhcGV6LmR0c2VuYWJsZXJcIixcIm1lc3NhZ2VcIjpcIiQxXCJ9IiA+L2Rldi9udWxsIDI+JjE7IH0KZWNobyAiLS0tIGR0czI1K3RydWVoZCAkKGRhdGUpIC0tLSIgPj4gJExPRyAyPiYxCiMgMCkgZmlybXdhcmUtdXBkYXRlIC8gQUJJIGd1YXJkLiBPdXIgYmluZC1vdmVyIGxpYnMgKGxpYmF2L2lzb21wNC9tcGVndHNkZW11eCkgYXJlIGFybWVsCiMgICAgR1N0cmVhbWVyLTEuMjQgYnVpbGRzOyBiaW5kaW5nIHRoZW0gb3ZlciBhIGRpZmZlcmVudC1BQkkgTEcgbGliIGFmdGVyIGFuIE9UQSB3b3VsZCBicmVhawojICAgIEFMTCBtcDQvdHMvbWt2IHBsYXliYWNrLiBEZXRlY3QgdmlhIHRoZSB1bnRvdWNoZWQgY29yZSB2ZXJzaW9uOyBpZiBpdCBjaGFuZ2VkLCBkbyBOT1RISU5HCiMgICAgYW5kIGxldCBzdG9jayBmaXJtd2FyZSBwbGF5IChsb3Npbmcgb25seSBEVFMvVHJ1ZUhEKSAtLSBmYWlsIHNhZmUsIG5ldmVyIGJyZWFrIHBsYXliYWNrLgpHU1RfVkVSPSQoL3Vzci9iaW4vZ3N0LWluc3BlY3QtMS4wIC0tdmVyc2lvbiAyPi9kZXYvbnVsbCB8IHNlZCAtbiAncy9eR1N0cmVhbWVyIFwoWzAtOV0qXC5bMC05XSpcKS4qL1wxL3AnIHwgaGVhZCAtbjEpCmlmIFsgIiRHU1RfVkVSIiAhPSAiJEVYUEVDVF9HU1QiIF07IHRoZW4KICBlY2hvICJBQk9SVDogR1N0cmVhbWVyICckR1NUX1ZFUicgIT0gZXhwZWN0ZWQgJEVYUEVDVF9HU1QgKGZpcm13YXJlIHVwZGF0ZT8pOyBvdmVycmlkZXMgc2tpcHBlZCIgPj4gJExPRwogIHRvYXN0ICJEVFMvVHJ1ZUhEIHBhdXNlZDogVFYgZmlybXdhcmUgY2hhbmdlZCAoR1N0cmVhbWVyICRHU1RfVkVSKS4gUmUtb3BlbiBEVFMgRW5hYmxlciB0byB1cGRhdGUuIgogIGV4aXQgMApmaQojIDEpIGNvZGVjLWNhcGFiaWxpdHkgb3ZlcnJpZGUgKGFkZHMgVFJVRUhEL01MUCBzbyB1bWVkaWFzZXJ2ZXIgYWxsb2NhdGVzIGEgZGVjb2RlciByZXNvdXJjZSkKWyAtZiAvdmFyL2xpYi93ZWJvc2JyZXcvdHJ1ZWhkL2NvZGVjX2NhcGFiaWxpdHkuanNvbiBdICYmICEgZ3JlcCAtcSAiICRDRkcgIiAvcHJvYy9tb3VudHMgMj4vZGV2L251bGwgJiYgbW91bnQgLW4gLS1iaW5kIC92YXIvbGliL3dlYm9zYnJldy90cnVlaGQvY29kZWNfY2FwYWJpbGl0eS5qc29uICIkQ0ZHIiAyPj4kTE9HCiMgMikgcmVwbGFjZSBMRy5zIHRydWVoZC1sZXNzIGxpYmF2IHdpdGggb3VycyAoaGFzIGF2ZGVjX3RydWVoZC9hdmRlY19tbHApClsgLWYgIiRNWUxJQkFWIiBdICYmICEgZ3JlcCAtcSAiICRMR0xJQkFWICIgL3Byb2MvbW91bnRzIDI+L2Rldi9udWxsICYmIG1vdW50IC1uIC0tYmluZCAtbyBybyAiJE1ZTElCQVYiICIkTEdMSUJBViIgMj4+JExPRwojIDJiKSBnc3Rjb29sLmNvbmY6IGdpdmUgYXZkZWNfdHJ1ZWhkIGEgaGlnaCBTVyByYW5rIHNvIExHIGF1dG9wbHVncyBpdCAobm90IHRoZSBIVyBwYXRoKQpHQz0vZXRjL2dzdC9nc3Rjb29sLmNvbmYKWyAtZiAvdmFyL2xpYi93ZWJvc2JyZXcvdHJ1ZWhkL2dzdGNvb2wuY29uZiBdICYmICEgZ3JlcCAtcSAiICRHQyAiIC9wcm9jL21vdW50cyAyPi9kZXYvbnVsbCAmJiBtb3VudCAtbiAtLWJpbmQgL3Zhci9saWIvd2Vib3NicmV3L3RydWVoZC9nc3Rjb29sLmNvbmYgIiRHQyIgMj4+JExPRwojIDJjKSBjb250YWluZXIgZGVtdXhlcnMgd2l0aCBEVFMgcmUtZW5hYmxlZCAobXA0L3RzL20ydHMgRFRTIC0+IGF1ZGlvL3gtZHRzKS4KIyAgICAgUGF0Y2hlZCBpc29tcDQvbXBlZ3RzZGVtdXggZGVmYXVsdCBkdHNfc3VwcG9ydD1UUlVFLiBCb3VuZCBCRUZPUkUgdGhlIHJlZ2VuCiMgICAgIGJlbG93IHNvIHRoZSByZWdpc3RyeSBwaWNrcyB0aGVtIHVwIGF0IHRoZWlyIG5vcm1hbCBwYXRoLgpJU089L3Vzci9saWIvZ3N0cmVhbWVyLTEuMC9saWJnc3Rpc29tcDQuc28KVFNEPS91c3IvbGliL2dzdHJlYW1lci0xLjAvbGliZ3N0bXBlZ3RzZGVtdXguc28KWyAtZiAvdmFyL2xpYi93ZWJvc2JyZXcvZGVtdXgyNS9saWJnc3Rpc29tcDQuc28gXSAmJiAhIGdyZXAgLXEgIiAkSVNPICIgL3Byb2MvbW91bnRzIDI+L2Rldi9udWxsICYmIG1vdW50IC1uIC0tYmluZCAtbyBybyAvdmFyL2xpYi93ZWJvc2JyZXcvZGVtdXgyNS9saWJnc3Rpc29tcDQuc28gIiRJU08iIDI+PiRMT0cKWyAtZiAvdmFyL2xpYi93ZWJvc2JyZXcvZGVtdXgyNS9saWJnc3RtcGVndHNkZW11eC5zbyBdICYmICEgZ3JlcCAtcSAiICRUU0QgIiAvcHJvYy9tb3VudHMgMj4vZGV2L251bGwgJiYgbW91bnQgLW4gLS1iaW5kIC1vIHJvIC92YXIvbGliL3dlYm9zYnJldy9kZW11eDI1L2xpYmdzdG1wZWd0c2RlbXV4LnNvICIkVFNEIiAyPj4kTE9HCiMgMykgcmVnZW5lcmF0ZSB0aGUgbWVkaWEgcmVnaXN0cnkgKGZyZXNoKSB3aXRoIGR0c2RlYyArIG91ciBsaWJhdiwgdGhlbiB3cml0ZSBpdCB0byB0aGUgbWVkaWEgcGF0aC4KIyAgICBCb3VuZGVkIGJ5IGB0aW1lb3V0YCBhbmQgc2Nhbm5lZCBpbi1wcm9jZXNzIChHU1RfUkVHSVNUUllfRk9SSz1ubykgc28gYSBoYW5nIGNhbid0IHRyaXAgSEJDCiMgICAgZmFpbHNhZmUgYW5kIG5vIGdzdC1wbHVnaW4tc2Nhbm5lciBjaGlsZCBsaW5nZXJzIHBhc3QgdGhlIHRpbWVvdXQuCnJtIC1mIC90bXAvZ3N0X2R0c19yZWcuYmluCkxEX0xJQlJBUllfUEFUSD0vdmFyL2xpYi93ZWJvc2JyZXcvdHJ1ZWhkL2xpYnMgXApHU1RfUkVHSVNUUllfMV8wPS90bXAvZ3N0X2R0c19yZWcuYmluIFwKR1NUX1BMVUdJTl9QQVRIXzFfMD0vdXNyL2xpYi9nc3RyZWFtZXItMS4wOi9tbnQvbGcvcmVzL2xnbGliL2dzdHJlYW1lci0xLjA6L3Zhci9saWIvd2Vib3NicmV3L2R0czI1IFwKR1NUX1JFR0lTVFJZX0ZPUks9bm8gR1NUX1JFR0lTVFJZX1VQREFURT15ZXMgdGltZW91dCAzMCAvdXNyL2Jpbi9nc3QtaW5zcGVjdC0xLjAgPi9kZXYvbnVsbCAyPj4kTE9HCiMgb25seSBvdmVyd3JpdGUgdGhlIG1lZGlhIHJlZ2lzdHJ5IGlmIG91ciByZWdlbiBhY3R1YWxseSBjb250YWlucyBCT1RIIGRlY29kZXJzCmlmIEdTVF9SRUdJU1RSWV8xXzA9L3RtcC9nc3RfZHRzX3JlZy5iaW4gR1NUX1JFR0lTVFJZX1VQREFURT1ubyBHU1RfUkVHSVNUUllfRk9SSz1ubyAvdXNyL2Jpbi9nc3QtaW5zcGVjdC0xLjAgZHRzZGVjID4vZGV2L251bGwgMj4mMSBcCiAgICYmIEdTVF9SRUdJU1RSWV8xXzA9L3RtcC9nc3RfZHRzX3JlZy5iaW4gR1NUX1JFR0lTVFJZX1VQREFURT1ubyBHU1RfUkVHSVNUUllfRk9SSz1ubyAvdXNyL2Jpbi9nc3QtaW5zcGVjdC0xLjAgYXZkZWNfdHJ1ZWhkID4vZGV2L251bGwgMj4mMTsgdGhlbgogIGNwIC1mIC90bXAvZ3N0X2R0c19yZWcuYmluICIkUkVHIiAyPj4kTE9HICYmIGVjaG8gInJlZ2lzdHJ5IHVwZGF0ZWQgKGR0c2RlYyt0cnVlaGQpIiA+PiRMT0cKZWxzZQogIGVjaG8gIldBUk46IHJlZ2VuIG1pc3NpbmcgZHRzZGVjIG9yIGF2ZGVjX3RydWVoZCwgbGVmdCByZWdpc3RyeSB1bnRvdWNoZWQiID4+JExPRwogIHRvYXN0ICJEVFMgRW5hYmxlcjogZGVjb2RlciByZWdpc3RyeSBpbmNvbXBsZXRlIGFmdGVyIGJvb3Q7IERUUy9UcnVlSEQgbWF5IG5vdCB3b3JrLiIKZmkKZXhpdCAwCg==
INIT_B64
chmod 0755 "$INIT_SCRIPT" && log "installed embedded init_dts25.sh -> $INIT_SCRIPT" || log "WARN: writing init_dts25.sh failed"

mkdir -p "$INITD"
if [ -L "$HOOK" ] || [ -e "$HOOK" ]; then rm -f "$HOOK"; fi
ln -s "$INIT_SCRIPT" "$HOOK" && log "linked boot hook $HOOK -> $INIT_SCRIPT"

# --- 5. Apply now ----------------------------------------------------------
log "applying now (running $INIT_SCRIPT)"
sh "$INIT_SCRIPT"

if killall starfish-media-pipeline 2>>"$LOG"; then
  log "restarted starfish-media-pipeline"
else
  log "note: starfish-media-pipeline not running (starts fresh on next playback)"
fi

log "=== unified DTS+TrueHD install done ==="
echo "DTS + TrueHD install complete. See $LOG for details."
exit 0
