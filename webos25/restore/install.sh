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
IyEvYmluL3NoCiMgd2ViT1MyNSBEVFMgKyBUcnVlSEQgcmVzdG9yZS4gUnVucyBhdCBib290IHZpYSAvdmFyL2xpYi93ZWJvc2JyZXcvaW5pdC5kL3Jlc3RvcmVfZHRzMjUuCnNldCAtdQpSRUc9L21udC9mbGFzaC9kYXRhL2dzdF8xXzBfcmVnaXN0cnkuYXJtLmJpbgpDRkc9L2V0Yy91bWVkaWFzZXJ2ZXIvZGV2aWNlX2NvZGVjX2NhcGFiaWxpdHlfY29uZmlnLmpzb24KTEdMSUJBVj0vdXNyL2xpYi9nc3RyZWFtZXItMS4wL2xpYmdzdGxpYmF2LnNvCk1ZTElCQVY9L3Zhci9saWIvd2Vib3NicmV3L3RydWVoZC9saWJnc3RsaWJhdi5zbwpMT0c9L3RtcC9kdHMyNS5sb2cKRVhQRUNUX0dTVD0xLjI0CnRvYXN0KCkgeyBsdW5hLXNlbmQgLW4gMSBsdW5hOi8vY29tLndlYm9zLm5vdGlmaWNhdGlvbi9jcmVhdGVUb2FzdCAie1wic291cmNlSWRcIjpcIm9yZy53ZWJvc2JyZXcuZHRzZW5hYmxlclwiLFwibWVzc2FnZVwiOlwiJDFcIn0iID4vZGV2L251bGwgMj4mMTsgfQplY2hvICItLS0gZHRzMjUrdHJ1ZWhkICQoZGF0ZSkgLS0tIiA+PiAkTE9HIDI+JjEKIyAwKSBmaXJtd2FyZS11cGRhdGUgLyBBQkkgZ3VhcmQuIE91ciBiaW5kLW92ZXIgbGlicyAobGliYXYvaXNvbXA0L21wZWd0c2RlbXV4KSBhcmUgYXJtZWwKIyAgICBHU3RyZWFtZXItMS4yNCBidWlsZHM7IGJpbmRpbmcgdGhlbSBvdmVyIGEgZGlmZmVyZW50LUFCSSBMRyBsaWIgYWZ0ZXIgYW4gT1RBIHdvdWxkIGJyZWFrCiMgICAgQUxMIG1wNC90cy9ta3YgcGxheWJhY2suIERldGVjdCB2aWEgdGhlIHVudG91Y2hlZCBjb3JlIHZlcnNpb247IGlmIGl0IGNoYW5nZWQsIGRvIE5PVEhJTkcKIyAgICBhbmQgbGV0IHN0b2NrIGZpcm13YXJlIHBsYXkgKGxvc2luZyBvbmx5IERUUy9UcnVlSEQpIC0tIGZhaWwgc2FmZSwgbmV2ZXIgYnJlYWsgcGxheWJhY2suCkdTVF9WRVI9JCgvdXNyL2Jpbi9nc3QtaW5zcGVjdC0xLjAgLS12ZXJzaW9uIDI+L2Rldi9udWxsIHwgc2VkIC1uICdzL15HU3RyZWFtZXIgXChbMC05XSpcLlswLTldKlwpLiovXDEvcCcgfCBoZWFkIC1uMSkKaWYgWyAiJEdTVF9WRVIiICE9ICIkRVhQRUNUX0dTVCIgXTsgdGhlbgogIGVjaG8gIkFCT1JUOiBHU3RyZWFtZXIgJyRHU1RfVkVSJyAhPSBleHBlY3RlZCAkRVhQRUNUX0dTVCAoZmlybXdhcmUgdXBkYXRlPyk7IG92ZXJyaWRlcyBza2lwcGVkIiA+PiAkTE9HCiAgdG9hc3QgIkRUUy9UcnVlSEQgcGF1c2VkOiBUViBmaXJtd2FyZSBjaGFuZ2VkIChHU3RyZWFtZXIgJEdTVF9WRVIpLiBSZS1vcGVuIERUUyBFbmFibGVyIHRvIHVwZGF0ZS4iCiAgZXhpdCAwCmZpCiMgMSkgY29kZWMtY2FwYWJpbGl0eSBvdmVycmlkZSAoYWRkcyBUUlVFSEQvTUxQIHNvIHVtZWRpYXNlcnZlciBhbGxvY2F0ZXMgYSBkZWNvZGVyIHJlc291cmNlKQpbIC1mIC92YXIvbGliL3dlYm9zYnJldy90cnVlaGQvY29kZWNfY2FwYWJpbGl0eS5qc29uIF0gJiYgISBncmVwIC1xICIgJENGRyAiIC9wcm9jL21vdW50cyAyPi9kZXYvbnVsbCAmJiBtb3VudCAtbiAtLWJpbmQgL3Zhci9saWIvd2Vib3NicmV3L3RydWVoZC9jb2RlY19jYXBhYmlsaXR5Lmpzb24gIiRDRkciIDI+PiRMT0cKIyAyKSByZXBsYWNlIExHLnMgdHJ1ZWhkLWxlc3MgbGliYXYgd2l0aCBvdXJzIChoYXMgYXZkZWNfdHJ1ZWhkL2F2ZGVjX21scCkKWyAtZiAiJE1ZTElCQVYiIF0gJiYgISBncmVwIC1xICIgJExHTElCQVYgIiAvcHJvYy9tb3VudHMgMj4vZGV2L251bGwgJiYgbW91bnQgLW4gLS1iaW5kIC1vIHJvICIkTVlMSUJBViIgIiRMR0xJQkFWIiAyPj4kTE9HCiMgMmIpIGdzdGNvb2wuY29uZjogZ2l2ZSBhdmRlY190cnVlaGQgYSBoaWdoIFNXIHJhbmsgc28gTEcgYXV0b3BsdWdzIGl0IChub3QgdGhlIEhXIHBhdGgpCkdDPS9ldGMvZ3N0L2dzdGNvb2wuY29uZgpbIC1mIC92YXIvbGliL3dlYm9zYnJldy90cnVlaGQvZ3N0Y29vbC5jb25mIF0gJiYgISBncmVwIC1xICIgJEdDICIgL3Byb2MvbW91bnRzIDI+L2Rldi9udWxsICYmIG1vdW50IC1uIC0tYmluZCAvdmFyL2xpYi93ZWJvc2JyZXcvdHJ1ZWhkL2dzdGNvb2wuY29uZiAiJEdDIiAyPj4kTE9HCiMgMmMpIGNvbnRhaW5lciBkZW11eGVycyB3aXRoIERUUyByZS1lbmFibGVkIChtcDQvdHMvbTJ0cyBEVFMgLT4gYXVkaW8veC1kdHMpLgojICAgICBQYXRjaGVkIGlzb21wNC9tcGVndHNkZW11eCBkZWZhdWx0IGR0c19zdXBwb3J0PVRSVUUuIEJvdW5kIEJFRk9SRSB0aGUgcmVnZW4KIyAgICAgYmVsb3cgc28gdGhlIHJlZ2lzdHJ5IHBpY2tzIHRoZW0gdXAgYXQgdGhlaXIgbm9ybWFsIHBhdGguCklTTz0vdXNyL2xpYi9nc3RyZWFtZXItMS4wL2xpYmdzdGlzb21wNC5zbwpUU0Q9L3Vzci9saWIvZ3N0cmVhbWVyLTEuMC9saWJnc3RtcGVndHNkZW11eC5zbwpbIC1mIC92YXIvbGliL3dlYm9zYnJldy9kZW11eDI1L2xpYmdzdGlzb21wNC5zbyBdICYmICEgZ3JlcCAtcSAiICRJU08gIiAvcHJvYy9tb3VudHMgMj4vZGV2L251bGwgJiYgbW91bnQgLW4gLS1iaW5kIC1vIHJvIC92YXIvbGliL3dlYm9zYnJldy9kZW11eDI1L2xpYmdzdGlzb21wNC5zbyAiJElTTyIgMj4+JExPRwpbIC1mIC92YXIvbGliL3dlYm9zYnJldy9kZW11eDI1L2xpYmdzdG1wZWd0c2RlbXV4LnNvIF0gJiYgISBncmVwIC1xICIgJFRTRCAiIC9wcm9jL21vdW50cyAyPi9kZXYvbnVsbCAmJiBtb3VudCAtbiAtLWJpbmQgLW8gcm8gL3Zhci9saWIvd2Vib3NicmV3L2RlbXV4MjUvbGliZ3N0bXBlZ3RzZGVtdXguc28gIiRUU0QiIDI+PiRMT0cKIyAzKSByZWdlbmVyYXRlIHRoZSBtZWRpYSByZWdpc3RyeSAoZnJlc2gpIHdpdGggZHRzZGVjICsgb3VyIGxpYmF2LCB0aGVuIHdyaXRlIGl0IHRvIHRoZSBtZWRpYSBwYXRoLgojICAgIEJvdW5kZWQgYnkgYHRpbWVvdXRgIGFuZCBzY2FubmVkIGluLXByb2Nlc3MgKEdTVF9SRUdJU1RSWV9GT1JLPW5vKSBzbyBhIGhhbmcgY2FuJ3QgdHJpcCBIQkMKIyAgICBmYWlsc2FmZSBhbmQgbm8gZ3N0LXBsdWdpbi1zY2FubmVyIGNoaWxkIGxpbmdlcnMgcGFzdCB0aGUgdGltZW91dC4Kcm0gLWYgL3RtcC9nc3RfZHRzX3JlZy5iaW4KTERfTElCUkFSWV9QQVRIPS92YXIvbGliL3dlYm9zYnJldy90cnVlaGQvbGlicyBcCkdTVF9SRUdJU1RSWV8xXzA9L3RtcC9nc3RfZHRzX3JlZy5iaW4gXApHU1RfUExVR0lOX1BBVEhfMV8wPS91c3IvbGliL2dzdHJlYW1lci0xLjA6L21udC9sZy9yZXMvbGdsaWIvZ3N0cmVhbWVyLTEuMDovdmFyL2xpYi93ZWJvc2JyZXcvZHRzMjUgXApHU1RfUkVHSVNUUllfRk9SSz1ubyBHU1RfUkVHSVNUUllfVVBEQVRFPXllcyB0aW1lb3V0IDMwIC91c3IvYmluL2dzdC1pbnNwZWN0LTEuMCA+L2Rldi9udWxsIDI+PiRMT0cKIyBvbmx5IG92ZXJ3cml0ZSB0aGUgbWVkaWEgcmVnaXN0cnkgaWYgb3VyIHJlZ2VuIGFjdHVhbGx5IGNvbnRhaW5zIEJPVEggZGVjb2RlcnMKaWYgR1NUX1JFR0lTVFJZXzFfMD0vdG1wL2dzdF9kdHNfcmVnLmJpbiBHU1RfUkVHSVNUUllfVVBEQVRFPW5vIEdTVF9SRUdJU1RSWV9GT1JLPW5vIC91c3IvYmluL2dzdC1pbnNwZWN0LTEuMCBkdHNkZWMgPi9kZXYvbnVsbCAyPiYxIFwKICAgJiYgR1NUX1JFR0lTVFJZXzFfMD0vdG1wL2dzdF9kdHNfcmVnLmJpbiBHU1RfUkVHSVNUUllfVVBEQVRFPW5vIEdTVF9SRUdJU1RSWV9GT1JLPW5vIC91c3IvYmluL2dzdC1pbnNwZWN0LTEuMCBhdmRlY190cnVlaGQgPi9kZXYvbnVsbCAyPiYxOyB0aGVuCiAgY3AgLWYgL3RtcC9nc3RfZHRzX3JlZy5iaW4gIiRSRUciIDI+PiRMT0cgJiYgZWNobyAicmVnaXN0cnkgdXBkYXRlZCAoZHRzZGVjK3RydWVoZCkiID4+JExPRwplbHNlCiAgZWNobyAiV0FSTjogcmVnZW4gbWlzc2luZyBkdHNkZWMgb3IgYXZkZWNfdHJ1ZWhkLCBsZWZ0IHJlZ2lzdHJ5IHVudG91Y2hlZCIgPj4kTE9HCiAgdG9hc3QgIkRUUyBFbmFibGVyOiBkZWNvZGVyIHJlZ2lzdHJ5IGluY29tcGxldGUgYWZ0ZXIgYm9vdDsgRFRTL1RydWVIRCBtYXkgbm90IHdvcmsuIgpmaQpleGl0IDAK
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
