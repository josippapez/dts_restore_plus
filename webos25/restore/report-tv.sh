#!/bin/sh
# report-tv.sh -- collect everything needed to answer "why was my TV refused?"
#
# READ-ONLY. Mounts nothing, copies nothing, writes nothing. Run as root and paste
# the whole output into a GitHub issue.
#
# Every value here is measured the SAME way the app's DETECT_PROBE measures it, so
# the PROFILE this prints is the profile the app will pick. If they ever disagree,
# that disagreement is itself the bug.
set -u

first_glob() { for f in $1; do [ -e "$f" ] && { printf "%s\n" "$f"; return 0; }; done; return 1; }
hash_of() { # $1=algo(md5|sha256) $2=file
  case "$1" in
    md5)    h=$(md5sum    "$2" 2>/dev/null | awk '{print $1}');;
    sha256) h=$(sha256sum "$2" 2>/dev/null | awk '{print $1}');;
  esac
  [ -n "${h:-}" ] || h=$(busybox "$1sum" "$2" 2>/dev/null | awk '{print $1}')
  printf "%s" "${h:-unavailable}"
}

echo "=== dts_restore_plus TV report ==="
echo "report_version=1"

echo
echo "--- loader / ABI ---"
LOADER=unknown
LD=$(first_glob "/lib/ld-linux*.so.* /lib/ld-linux-*.so.* /lib/ld-*.so.*" 2>/dev/null)
[ -n "${LD:-}" ] && LOADER=$(basename "$LD")
echo "LOADER=$LOADER"
echo "LOADER_PATH=${LD:-none}"
echo "od_present=$(command -v od >/dev/null 2>&1 && echo yes || echo no)"
EFLAGS=unknown; FLOAT_ABI=unknown
GSTSO=$(first_glob "/usr/lib/gstreamer-1.0/libgstcoreelements.so /usr/lib/gstreamer-1.0/libgsttypefindfunctions.so /usr/lib/gstreamer-1.0/*.so" 2>/dev/null)
echo "eflag_probe_file=${GSTSO:-none}"
if [ -n "${GSTSO:-}" ] && command -v od >/dev/null 2>&1; then
  bytes=$(od -An -t x1 -j 36 -N 4 "$GSTSO" 2>/dev/null | tr -d " \n")
  echo "eflag_bytes=${bytes:-none}"
  if [ -n "$bytes" ] && [ "${#bytes}" -eq 8 ]; then
    b0=$(printf "%s" "$bytes" | cut -c1-2); b1=$(printf "%s" "$bytes" | cut -c3-4)
    b2=$(printf "%s" "$bytes" | cut -c5-6); b3=$(printf "%s" "$bytes" | cut -c7-8)
    EFLAGS="0x${b3}${b2}${b1}${b0}"
    val=$(printf "%d" "$EFLAGS" 2>/dev/null || echo 0)
    if   [ "$((val & 0x400))" -ne 0 ]; then FLOAT_ABI=hard
    elif [ "$((val & 0x200))" -ne 0 ]; then FLOAT_ABI=soft
    else FLOAT_ABI=unspecified; fi
  fi
fi
echo "ELF_EFLAGS=$EFLAGS"
echo "FLOAT_ABI=$FLOAT_ABI"
echo "UNAME_M=$(uname -m 2>/dev/null || echo unknown)"

echo
echo "--- GStreamer ---"
GST_VERSION=unknown
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  GST_VERSION=$(GST_REGISTRY_FORK=no gst-inspect-1.0 --version 2>/dev/null | grep -i GStreamer | head -n1 | awk '{print $2}')
  [ -n "$GST_VERSION" ] || GST_VERSION=unknown
fi
echo "GST_VERSION=$GST_VERSION"
GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)
echo "GST_MM=$GST_MM"

echo
echo "--- identity ---"
WEBOS_RELEASE=unknown; PRODUCT_ID=unknown; HARDWARE_ID=unknown
BOARD_TYPE=unknown; WEBOS_MANUFACTURING_VERSION=unknown
if command -v nyx-cmd >/dev/null 2>&1; then
  WEBOS_RELEASE=$(nyx-cmd OSInfo query webos_release 2>/dev/null | head -n1)
  WEBOS_MANUFACTURING_VERSION=$(nyx-cmd OSInfo query webos_manufacturing_version 2>/dev/null | head -n1)
  PRODUCT_ID=$(nyx-cmd DeviceInfo query product_id 2>/dev/null | head -n1)
  HARDWARE_ID=$(nyx-cmd DeviceInfo query hardware_id 2>/dev/null | head -n1)
  BOARD_TYPE=$(nyx-cmd DeviceInfo query board_type 2>/dev/null | head -n1)
fi
for v in WEBOS_RELEASE WEBOS_MANUFACTURING_VERSION PRODUCT_ID HARDWARE_ID BOARD_TYPE; do
  eval "cur=\$$v"; [ -n "${cur:-}" ] || eval "$v=unknown"
  eval "echo \"$v=\$$v\""
done

echo
echo "--- stock plugin hashes ---"
G=/usr/lib/gstreamer-1.0
for f in libgstlibav libgstisomp4 libgstmatroska libgstmpegtsdemux; do
  if [ -e "$G/$f.so" ]; then
    echo "$f.so.md5=$(hash_of md5 "$G/$f.so")"
    echo "$f.so.sha256=$(hash_of sha256 "$G/$f.so")"
  else
    echo "$f.so=absent"
  fi
done
[ -e /etc/gst/gstcool.conf ] && echo "gstcool.conf.sha256=$(hash_of sha256 /etc/gst/gstcool.conf)"

echo
echo "--- existing state (is something already installed?) ---"
for h in /var/lib/webosbrew/init.d/restore_dts25 \
         /var/lib/webosbrew/init.d/restore_dts \
         /var/lib/webosbrew/init.d/restore_dts_c2; do
  [ -e "$h" ] && echo "hook_present=$h" || echo "hook_absent=$h"
done
echo "our_mounts=$(grep -c 'gstreamer-1.0\|gst_1_0_registry\|gstcool.conf\|device_codec_capability' /proc/self/mountinfo 2>/dev/null || echo 0)"
HAS_DTS_AUDIODEC=no
command -v gst-inspect-1.0 >/dev/null 2>&1 && GST_REGISTRY_FORK=no gst-inspect-1.0 dts_audiodec >/dev/null 2>&1 && HAS_DTS_AUDIODEC=yes
echo "HAS_DTS_AUDIODEC=$HAS_DTS_AUDIODEC"
echo "avdec_dca=$(GST_REGISTRY_FORK=no gst-inspect-1.0 avdec_dca >/dev/null 2>&1 && echo present || echo absent)"
DCA_RANK=unknown
if [ -r /etc/gst/gstcool.conf ]; then
  DCA_RANK=$(sed -n "s/^[[:space:]]*avdec_dca[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p" /etc/gst/gstcool.conf | head -n1)
  [ -n "$DCA_RANK" ] || DCA_RANK=absent
fi
echo "DCA_RANK=$DCA_RANK"

echo
echo "--- computed profile (what the app will pick) ---"
PROFILE=unknown
C2_GATE_FAIL=
case "$GST_MM" in
  1.14) PROFILE=cx-armv7-gst114 ;;
  1.24)
    if [ "$LOADER" = "ld-linux.so.3" ] && [ "$FLOAT_ABI" = "soft" ]; then PROFILE=webos25-armel-gst124
    else PROFILE="webos25-${LOADER}-${FLOAT_ABI}"; fi ;;
  1.18)
    case "$HARDWARE_ID" in
      HE_DTV_W22O_AFABATAA|HE_DTV_W22O_AFABATPU)
        case "$PRODUCT_ID" in OLED*C2*|OLED*G2*|OLED*CS*) C2_MODEL=1 ;; *) C2_MODEL=0 ;; esac
        C2_FWOK=0
        { [ "$WEBOS_MANUFACTURING_VERSION" = "04.40.93" ] || [ "$WEBOS_MANUFACTURING_VERSION" = "04.40.93.01" ]; } && [ "$WEBOS_RELEASE" = "7.4.0" ] && [ "$GST_VERSION" = "1.18.2" ] && C2_FWOK=1
        { [ "$WEBOS_MANUFACTURING_VERSION" = "23.25.55" ] || [ "$WEBOS_MANUFACTURING_VERSION" = "23.25.55.01" ]; } && [ "$WEBOS_RELEASE" = "9.2.2" ] && [ "$GST_VERSION" = "1.18.5" ] && C2_FWOK=1
        [ "$C2_MODEL" = 1 ] || C2_GATE_FAIL="$C2_GATE_FAIL model($PRODUCT_ID)"
        [ "$BOARD_TYPE" != unknown ] || C2_GATE_FAIL="$C2_GATE_FAIL board-type"
        [ "$C2_FWOK" = 1 ] || C2_GATE_FAIL="$C2_GATE_FAIL firmware($WEBOS_MANUFACTURING_VERSION/$WEBOS_RELEASE/$GST_VERSION)"
        [ "$LOADER" = "ld-linux.so.3" ] || C2_GATE_FAIL="$C2_GATE_FAIL loader($LOADER)"
        [ "$FLOAT_ABI" = "soft" ] || C2_GATE_FAIL="$C2_GATE_FAIL float-abi($FLOAT_ABI)"
        C2_GATE_FAIL=${C2_GATE_FAIL# }
        if [ -z "$C2_GATE_FAIL" ]; then PROFILE=webos22-o22-gst118; else PROFILE=webos22-o22-c2-diagnostic; fi ;;
      *W22H*) PROFILE=webos22-w22h-diagnostic ;;
      *W23O*) PROFILE=webos23-w23o-diagnostic ;;
      *W23H*) PROFILE=webos23-w23h-diagnostic ;;
      *) PROFILE="unknown-gst${GST_MM}-${LOADER}" ;;
    esac ;;
  *) PROFILE="unknown-gst${GST_MM}-${LOADER}" ;;
esac
# A registered dts_audiodec normally means LG ships a real decoder -- but not on the
# C2 family, where the element registers over a 128 KB stub. Must not displace an
# exact-matched C2 profile. Keep identical to the app's DETECT_PROBE.
case "$PROFILE" in webos22-o22-gst118|webos22-o22-c2-diagnostic) C2_KEEP=1 ;; *) C2_KEEP=0 ;; esac
if [ "$HAS_DTS_AUDIODEC" = "yes" ] && [ "$C2_KEEP" = 0 ]; then
  if [ "$DCA_RANK" = "0" ]; then PROFILE=native-dts-gated
  else PROFILE=native-dts; fi
fi
echo "PROFILE=$PROFILE"
echo "C2_GATE_FAIL=$C2_GATE_FAIL"

echo
echo "--- app log (if the app has run) ---"
L=/var/lib/webosbrew/dtsenabler/dtsenabler.log
if [ -f "$L" ]; then tail -n 40 "$L"; else echo "no app log at $L"; fi
echo "=== end of report ==="
