#!/bin/sh
# report-tv.sh -- collect everything needed to answer "why was my TV refused?"
#
# READ-ONLY. Mounts nothing, copies nothing, writes nothing but a scratch file in
# /tmp that it removes (see bounded() below). Run as root and paste
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

# LG's audio driver can lock up, and a tester's report then stopped dead at the
# first amixer read with everything after it lost. A task stuck inside the kernel
# ignores even SIGKILL, and busybox `timeout` runs the program in its own pid, so
# waiting on `timeout` would hang just the same. Instead run each read that talks
# to LG's driver or a service in the background, into a scratch file in /tmp (the
# only thing this script writes, removed at once), and stop waiting after LIMIT
# seconds. The scratch file keeps a stuck read from holding our stdout open.
LIMIT=5
if sleep 0.1 2>/dev/null; then TICK=0.1 TICKS=$((LIMIT * 10)); else TICK=1 TICKS=$LIMIT; fi
alive() { # $1=pid; an exited child is a zombie until reaped, and kill -0 still finds it
  kill -0 "$1" 2>/dev/null || return 1
  st=; { read -r st < "/proc/$1/stat"; } 2>/dev/null
  case "$st" in *") Z "*) return 1 ;; esac
}
bounded() { # $@=command or function; prints its output, or what it printed plus a marker
  o=$(mktemp /tmp/dts25-report.XXXXXX) || { echo "(no scratch file in /tmp)"; return 1; }
  "$@" >"$o" 2>/dev/null </dev/null &
  p=$! n=0
  while alive "$p"; do
    if [ "$n" -ge "$TICKS" ]; then
      pkill -KILL -P "$p" 2>/dev/null; kill -KILL "$p" 2>/dev/null
      cat "$o" 2>/dev/null; rm -f "$o"
      echo "(timed out after $LIMIT s)"
      return 1
    fi
    sleep "$TICK"; n=$((n + 1))
  done
  wait "$p"
  cat "$o" 2>/dev/null; rm -f "$o"
}

echo "=== dts_restore_plus TV report ==="
echo "report_version=6"

echo
echo "--- loader / ABI ---"
echo "od_present=$(command -v od >/dev/null 2>&1 && echo yes || echo no)"
# The loader we report must follow the ABI of the GStreamer plugins we bind
# alongside, NOT whichever loader filename happens to sort first. A G5 on
# firmware 43.21.73 ships BOTH /lib/ld-linux.so.3 and /lib/ld-linux-aarch64.so.1;
# glob results are sorted and "-" sorts before ".", so the 64-bit one won and the
# TV was reported unsupported while its media stack is plain 32-bit ARM
# (issue #5 -- the owner's plugin reads class=01 machine=2800, same as a C5).
# So read the plugin's ELF identity first and pick the matching loader.
GSTSO=$(first_glob "/usr/lib/gstreamer-1.0/libgstcoreelements.so /usr/lib/gstreamer-1.0/libgsttypefindfunctions.so /usr/lib/gstreamer-1.0/*.so" 2>/dev/null)
echo "eflag_probe_file=${GSTSO:-none}"
ELF_CLASS=unknown; ELF_MACHINE=unknown; PLUGIN_ARCH=unknown
if [ -n "${GSTSO:-}" ] && command -v od >/dev/null 2>&1; then
  ELF_CLASS=$(od -An -t x1 -j 4 -N 1 "$GSTSO" 2>/dev/null | tr -d " \n")
  ELF_MACHINE=$(od -An -t x1 -j 18 -N 2 "$GSTSO" 2>/dev/null | tr -d " \n")
  case "${ELF_CLASS}/${ELF_MACHINE}" in
    01/2800) PLUGIN_ARCH=arm32 ;;
    02/b700) PLUGIN_ARCH=arm64 ;;
  esac
fi
echo "ELF_CLASS=$ELF_CLASS"
echo "ELF_MACHINE=$ELF_MACHINE"
echo "PLUGIN_ARCH=$PLUGIN_ARCH"
case "$PLUGIN_ARCH" in
  arm32) LD=$(first_glob "/lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3" 2>/dev/null) ;;
  arm64) LD=$(first_glob "/lib/ld-linux-aarch64.so.1 /lib64/ld-linux-aarch64.so.1" 2>/dev/null) ;;
  *)     LD= ;;
esac
# Fall back to the old wildcard only when the plugin's ELF identity could not be
# read at all, so a TV without `od` behaves exactly as it did before.
[ -n "${LD:-}" ] || LD=$(first_glob "/lib/ld-linux*.so.* /lib/ld-linux-*.so.* /lib/ld-*.so.*" 2>/dev/null)
LOADER=unknown
[ -n "${LD:-}" ] && LOADER=$(basename "$LD")
echo "LOADER=$LOADER"
echo "LOADER_PATH=${LD:-none}"
EFLAGS=unknown; FLOAT_ABI=unknown
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
# The DTS-HD kernel module is built per kernel release, not per firmware.
echo "KERNEL_RELEASE=$(uname -r 2>/dev/null || echo unknown)"
echo "KERNEL_BUILD=$(uname -v 2>/dev/null || echo unknown)"

echo
echo "--- stock plugin hashes ---"
G=/usr/lib/gstreamer-1.0
FP=/var/lib/webosbrew/dts25/stock.fp
for f in libgstlibav libgstisomp4 libgstmatroska libgstmpegtsdemux; do
  if grep -q " $G/$f.so " /proc/mounts 2>/dev/null; then
    # A bind mount means this file is ours, so the stock hash comes from the
    # fingerprint recorded before the patch was applied.
    echo "$f.so=patched"
    echo "$f.so.stock_md5=$(sed -n "s/^$f=//p" "$FP" 2>/dev/null | head -n1)"
  elif [ -e "$G/$f.so" ]; then
    echo "$f.so.md5=$(hash_of md5 "$G/$f.so")"
    echo "$f.so.sha256=$(hash_of sha256 "$G/$f.so")"
  else
    echo "$f.so=absent"
  fi
done
[ -e /etc/gst/gstcool.conf ] && echo "gstcool.conf.sha256=$(hash_of sha256 /etc/gst/gstcool.conf)"
# LG's own sink; DTS-HD/TrueHD to eARC only arms for builds on the boot script's list.
[ -e "$G/libgstlgaudiosink.so" ] && echo "libgstlgaudiosink.so.sha256=$(hash_of sha256 "$G/libgstlgaudiosink.so")"
# webOS 6 (C1/G1): the same list, for LG's older sink.
[ -e "$G/libgstlxaudiosink.so" ] && echo "libgstlxaudiosink.so.sha256=$(hash_of sha256 "$G/libgstlxaudiosink.so")"
# Dolby Vision profile 7 FEL decoding depends on LG's build of this library.
[ -e /usr/lib/librpu_m.so ] && echo "librpu_m.so.sha256=$(hash_of sha256 /usr/lib/librpu_m.so)"
# LG's media stack offers only the codecs named here.
C=/etc/umediaserver/device_codec_capability_config.json
[ -e "$C" ] && echo "codec_capability_names=$(grep -o '"name"[[:space:]]*:[[:space:]]*"[^"]*"' "$C" 2>/dev/null | sed 's/.*"\([^"]*\)"$/\1/' | sort -u | tr '\n' ' ')"

echo
echo "--- existing state (is something already installed?) ---"
for h in /var/lib/webosbrew/init.d/restore_dts25 \
         /var/lib/webosbrew/init.d/restore_dts \
         /var/lib/webosbrew/init.d/restore_dts_c2; do
  [ -e "$h" ] && echo "hook_present=$h" || echo "hook_absent=$h"
done
echo "our_mounts=$(grep -c 'gstreamer-1.0\|gst_1_0_registry\|gstcool.conf\|device_codec_capability' /proc/self/mountinfo 2>/dev/null || echo 0)"
# Mount point, source root and propagation: a CS on the C2 profile needed all three.
# Each one repeats in every app sandbox (/var/palm/jail), so those are only counted.
grep 'gstreamer-1.0\|gst_1_0_registry\|gstcool.conf\|device_codec_capability' /proc/self/mountinfo 2>/dev/null |
  awk '$5 ~ /\/var\/palm\/jail\// { jail++; next } { print "mount " $5 " <- " $4 " " $7 }
       END { print "mounts in app sandboxes: " jail + 0 }'
# What the media pipeline will load: the live registry, read without updating it.
REG=/mnt/flash/data/gst_1_0_registry.arm.bin
live_registry() {
  for e in dtsdec adecswitch truehdmat avdec_truehd; do
    r=$(GST_REGISTRY_1_0=$REG GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no gst-inspect-1.0 "$e" 2>/dev/null |
      grep -E '^  (Filename|Rank)' | tr -s ' ' | tr '\n' ' ')
    echo "registry $e: ${r:-absent}"
  done
}
[ -e "$REG" ] && command -v gst-inspect-1.0 >/dev/null 2>&1 && bounded live_registry
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

# How LG disabled DTS on this set. This is the value that decides which mechanism
# applies on a generation we do not cover yet, and it was the one missing from the
# first reports off unsupported TVs (issue #3, a 55C11 on GStreamer 1.16).
HAS_AVDEC_DCA=no; HAS_DTSDEC=no
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  GST_REGISTRY_FORK=no gst-inspect-1.0 avdec_dca >/dev/null 2>&1 && HAS_AVDEC_DCA=yes
  GST_REGISTRY_FORK=no gst-inspect-1.0 dtsdec    >/dev/null 2>&1 && HAS_DTSDEC=yes
fi
MKV_SO=/usr/lib/gstreamer-1.0/libgstmatroska.so
MKV_HAS_ADTS=unknown; MKV_HAS_XDTS=unknown
if [ -e "$MKV_SO" ] && command -v strings >/dev/null 2>&1; then
  if strings "$MKV_SO" 2>/dev/null | grep -q "A_DTS"; then MKV_HAS_ADTS=yes; else MKV_HAS_ADTS=no; fi
  if strings "$MKV_SO" 2>/dev/null | grep -q "audio/x-dts"; then MKV_HAS_XDTS=yes; else MKV_HAS_XDTS=no; fi
fi
echo "MKV_HAS_A_DTS_STRING=$MKV_HAS_ADTS"
echo "MKV_HAS_XDTS_CAPS_STRING=$MKV_HAS_XDTS"
MECH=unknown
if [ "$MKV_HAS_ADTS" = "no" ]; then MECH=cx-demuxer-nerf
elif [ "$MKV_HAS_ADTS" = "yes" ] && [ "$HAS_AVDEC_DCA" = "no" ] && [ "$HAS_DTSDEC" = "no" ] && [ "$HAS_DTS_AUDIODEC" = "no" ]; then MECH=webos25-retag-no-decoder
elif [ "$MKV_HAS_ADTS" = "yes" ]; then MECH=demuxer-emits-dts; fi
echo "DTS_DISABLE_MECHANISM_GUESS=$MECH"


# What is installed and switched on: issues kept needing a follow-up for these.
A=/media/developer/apps/usr/palm/applications/io.github.josippapez.dtsenabler/appinfo.json
echo "app_version=$(sed -n 's/.*"version"[^"]*"\([^"]*\)".*/\1/p' "$A" 2>/dev/null | head -n1)"
D=/var/lib/webosbrew/dts25
echo "hook_gate_version=$(sed -n 's/^W25_GATE_VERSION=//p' "$D/init_dts25.sh" 2>/dev/null | head -n1)"
for f in init_dts25.sh libgstadecswitch.so libgstdtsdec.so; do
  [ -f "$D/$f" ] && echo "$f.md5=$(md5sum < "$D/$f" | cut -d' ' -f1)"
done
grep -hE '^(verified|forced)=' "$D/stock.fp" 2>/dev/null
for f in passthrough passthrough-hd multich appdts.enabled; do
  [ -e "$D/$f" ] && echo "optin_$f=on" || echo "optin_$f=off"
done
sound_output() {
  luna-send -n 1 -w 3000 luna://com.webos.settingsservice/getSystemSettings \
    '{"category":"sound","keys":["soundOutput","soundOutputDigital"]}' </dev/null 2>/dev/null \
    | grep -o '"soundOutput[A-Za-z]*" *: *"[^"]*"' | tr -d ' "' | tr '\n' ' '
}
echo "sound_output=$(bounded sound_output)"
echo "watcher_sound_output=$(cat /run/dts25/sound-output 2>/dev/null)"
# Apps and HDMI inputs offer DTS only if this mentions dts (Show DTS tracks).
# The live value: the app sets it with setConfigs, which is not saved to disk.
edid_type() {
  script -q -e -c "luna-send -n 1 luna://com.webos.service.config/getConfigs '{\"configNames\":[\"tv.model.edidType\"]}'" /dev/null </dev/null 2>/dev/null | sed -n 's/.*"tv.model.edidType":"\([^"]*\)".*/\1/p' | head -n1
}
echo "edidType=$(bounded edid_type)"
echo "appdts_last=$(head -n1 "$D/appdts.last" 2>/dev/null)"
echo "Show DTS tracks, one line per boot:"
tail -n 10 "$D/appdts.log" 2>/dev/null
echo "eim_env=$(cat /var/systemd/system/env/eim.env 2>/dev/null)"
# pidof and maps take the target's memory lock, which a stuck task can hold.
eim_hooked() { grep -c libedidaudio "/proc/$(pidof eim | cut -d' ' -f1)/maps" 2>/dev/null; }
echo "eim_hooked=$(bounded eim_hooked)"
# A late start means something restarted LG's HDMI input service after boot.
eim_age() { awk -v up="$(cut -d' ' -f1 /proc/uptime)" '{ printf "started %.0f s after boot, %.0f s ago\n", $22 / 100, up - $22 / 100 }' "/proc/$(pidof eim | cut -d' ' -f1)/stat"; }
echo "eim: $(bounded eim_age)"
# The app reaches Homebrew Channel's exec service only through this group.
P=/var/luna-service2-dev/client-permissions.d/io.github.josippapez.dtsenabler.service.service.json
echo "hb_permission=$(grep -c org.webosbrew.hbchannel.service.group "$P" 2>/dev/null || echo missing)"
echo "edid_hook=$(cat /run/dts25/edidaudio 2>/dev/null)"

echo
echo "--- computed profile (what the app will pick) ---"
PROFILE=unknown
C2_GATE_FAIL=
case "$GST_MM" in
  1.14) PROFILE=cx-armv7-gst114 ;;
  1.16)
    if [ "$LOADER" = "ld-linux.so.3" ] && [ "$FLOAT_ABI" = "soft" ]; then PROFILE=webos6-armel-gst116
    else PROFILE="unknown-gst${GST_MM}-${LOADER}"; fi ;;
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
        [ "$WEBOS_MANUFACTURING_VERSION" = "13.30.56" ] && [ "$WEBOS_RELEASE" = "8.3.0" ] && [ "$GST_VERSION" = "1.18.5" ] && C2_FWOK=1
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
# Preserve the webOS 25 restore and C2 profiles even when the stock DTS element
# is registered. Use the native DTS heuristic for the remaining profiles.
# Keep this fallback in sync with the app's DETECT_PROBE. The app then reports a
# webOS 25 set with dts_audiodec as native DTS unless its compatibility check
# matches the TV; this report has no verified table, so it stops here.
case "$PROFILE" in
  webos22-o22-gst118|webos22-o22-c2-diagnostic|webos25-armel-gst124) ;;
  *)
    if [ "$HAS_DTS_AUDIODEC" = "yes" ]; then
      if [ "$DCA_RANK" = "0" ]; then PROFILE=native-dts-gated
      else PROFILE=native-dts; fi
    fi ;;
esac
echo "PROFILE=$PROFILE"
echo "C2_GATE_FAIL=$C2_GATE_FAIL"

echo
echo "--- app log (if the app has run) ---"
L=/var/lib/webosbrew/dtsenabler/dtsenabler.log
if [ -f "$L" ]; then
  # Status entries are long, so a plain tail pushed Enable's outcome out of the
  # report (#18). Show what each button press did, then the latest status.
  echo "last actions:"
  awk '/^[^ ]/ { keep = /\[action\]/; if (keep) n++ }
       keep { b[n] = b[n] $0 "\n" }
       END { for (i = (n > 8 ? n - 7 : 1); i <= n; i++) printf "%s", b[i] }' "$L"
  echo "latest status:"
  awk '/^[^ ]/ { keep = !/\[action\]/; if (keep) cur = "" }
       keep { cur = cur $0 "\n"; last = cur }
       END { printf "%s", last }' "$L"
else
  echo "no app log at $L"
fi
# The service's own log, cleared at restart: what each shell step printed.
echo "service log (last 40):"
tail -n 40 /tmp/dtsenabler.log 2>/dev/null

echo
echo "--- boot hook log (since the last restart) ---"
if [ -f /tmp/dts25.log ]; then tail -n 30 /tmp/dts25.log; else echo "no boot hook log"; fi
# Tagged lines from earlier in the log, which the tail misses.
echo "tagged lines:"
grep -E "app-dts|multich:|hb-permission:|earc_route" /tmp/dts25.log 2>/dev/null | tail -n 30
# webOS 3-6 (CX, C1) boot hook.
[ -f /tmp/dts_restore.log ] && { echo "CX/webOS 6 boot hook log:"; tail -n 30 /tmp/dts_restore.log; }

echo
echo "--- DTS-HD/TrueHD eARC route log (since the last restart) ---"
if [ -f /run/dts25/lgroute.log ]; then tail -n 200 /run/dts25/lgroute.log; else echo "no route log"; fi
# Markers and watcher state; a .pid file says whether its watcher still runs.
for f in /run/dts25/* /run/dtsc1/*; do
  [ -f "$f" ] || continue
  case "$f" in
    *.log|*.out) ;;
    *.pid) p=$(cat "$f" 2>/dev/null); [ -n "$p" ] && [ -d "/proc/$p" ] && echo "$f: $p alive" || echo "$f: $p gone" ;;
    *) echo "$f: $(head -c 200 "$f" 2>/dev/null | tr '\n' ' ')" ;;
  esac
done

# DTS-HD needs this module, and every refusal so far needed these by hand.
echo
echo "--- DTS-HD kernel module (earc_route) ---"
K=$D/hdmi
echo "sig_enforce=$(cat /sys/module/module/parameters/sig_enforce 2>/dev/null || echo absent)"
echo "modules_disabled=$(cat /proc/sys/kernel/modules_disabled 2>/dev/null || echo absent)"
for f in try-unverified loading; do [ -e "$K/$f" ] && echo "kmod_$f=yes" || echo "kmod_$f=no"; done
loader_check() { sh "$K/earc-route-load.sh" --check 2>&1 | tail -n 1; }
if [ -f "$K/earc-route-load.sh" ]; then echo "loader_check=$(bounded loader_check)"; else echo "loader_check=no loader installed"; fi
echo "unit=$(systemctl is-active dts25-earc.service 2>/dev/null)"
if [ -d /sys/module/earc_route ]; then
  echo "loaded, parameters:"
  for p in /sys/module/earc_route/parameters/*; do printf '%s=%s ' "${p##*/}" "$(cat "$p" 2>/dev/null)"; done
  echo
else
  echo "not loaded"
fi
echo "last load output:"
tail -n 20 /run/dts25/hdmi-watch.out 2>/dev/null
# Also the kernel's own refusals: signing, missing symbols, and LG's audio
# driver turning down DTS from HDMI ("suppported" is LG's spelling).
echo "kernel log:"
dmesg 2>/dev/null | grep -a -E "earc_route|Key was rejected|Unknown symbol|module verification|[Ll]ockdown|is not suppported" | tail -n 40
# LG's audio functions the module needs; a new kernel generation can lack them.
echo "kernel audio symbols: $(grep -E "_earc|earc_|bm_connect_block|aud_clock_update_output|output_set_earc_on|renderer_get_pcm_bypass|_cb_output_status|aud_hal_dts_supported" /proc/kallsyms 2>/dev/null | awk '{ print $3 }' | sort -u | head -n 60 | tr '\n' ' ')"

# webOS 6 (C1/G1) DTS-HD keeps its own log and files.
if [ -d "$D/4.4" ] || [ -f /var/lib/webosbrew/dtsenabler/hd-4.4.log ]; then
  echo
  echo "--- webOS 6 DTS-HD ---"
  echo "files: $(ls "$D/4.4" 2>/dev/null | tr '\n' ' ')"
  tail -n 40 /var/lib/webosbrew/dtsenabler/hd-4.4.log 2>/dev/null
fi

# What LG's side did with the route. Every receiver report so far needed a
# follow-up asking for these by hand.
echo
echo "--- LG audio sink: renderer restarts and what it told the DSP (last 40) ---"
grep -a -E "lgao_push_first_event|Caps is first or updated|same caps for normal|Connect ES Renderer|Restart renderer|Set pcm" \
  /var/log/legacy-log 2>/dev/null | sed 's/^[^T]*T\([0-9:.]*\)Z.*) /\1 /' | cut -c1-600 | tail -n 40

# How LG set up its outputs and what it reported to the sound settings. Testers
# were asked for these after silence, noise or a freeze on resume and seek.
echo
echo "--- LG output setup and errors (last 60) ---"
echo "legacy-log starts: $(head -n 1 /var/log/legacy-log 2>/dev/null | cut -c1-40)"
grep -a -E "gst_lg_audio_render_set_caps|BYPASS|setArcOnOff|setEArcOutputType|outputType Error|CONTROL_ELEMENT_SET_ERROR|sndout_connect|sndout_disconnect|NL_SOUND|NL_SETTING_CHANGE|is not suppported" \
  /var/log/legacy-log 2>/dev/null | sed 's/^[^T]*T\([0-9:.]*\)Z.*) /\1 /' | cut -c1-400 | tail -n 60

echo
echo "--- audio DSP crashes ---"
{ zcat /var/log/audio_dspdump_dmesg0.gz 2>/dev/null; cat /var/log/legacy-log 2>/dev/null; } \
  | grep -a -E "Exception: cause|eform_flush|is DEAD" | cut -c1-300 | tail -n 10

# Printed before the driver reads below, so it survives one of them hanging: the
# kernel stacks name the lock a stuck task waits for. LG's kernel threads idle in
# state D by design (twelve on a healthy C5), so stacks are for user processes.
echo
echo "--- tasks stuck in uninterruptible sleep (state D) ---"
stuck_tasks() {
  n=0 u=0
  for s in /proc/[0-9]*/status; do
    nm= st= pp=
    while read -r k v; do
      case "$k" in Name:) nm=$v ;; State:) st=$v ;; PPid:) pp=$v; break ;; esac
    done 2>/dev/null < "$s"
    case "$st" in D*) ;; *) continue ;; esac
    n=$((n + 1)); [ "$n" -le 40 ] || continue
    d=${s%/status}; w=; { read -r w < "$d/wchan"; } 2>/dev/null
    if [ "$pp" = 2 ]; then echo "${d#/proc/} $nm wchan=${w:-?} (kernel thread)"; continue; fi
    echo "${d#/proc/} $nm wchan=${w:-?}"
    u=$((u + 1)); [ "$u" -le 10 ] && head -n 16 "$d/stack" 2>/dev/null | sed 's/^/  /'
  done
  [ "$n" -gt 0 ] || echo "no task is in state D"
  [ "$n" -le 40 ] || echo "($((n - 40)) more not shown)"
}
bounded stuck_tasks

echo
echo "--- eARC state ---"
earc_ctl() { amixer -c0 cget name="$1" 2>/dev/null | sed -n 's/.*: values=//p'; }
for c in "Sndout EARC OnOff" "Sndout EARC Outputtype"; do
  echo "$c=$(bounded earc_ctl "$c")"
done
# LG's own account of the output: PCM or bitstream mode, channels, underflows.
sndout() { sed -n '/^\[ARC\]/,/^$/p; /^\[EARC\]/,/^$/p' /proc/lgtv-driver/audio/sndout 2>/dev/null | tr '\n' ' '; }
echo "$(bounded sndout)"
# LG picks the eARC output from this. A tester's DD+ Atmos track played as stereo
# PCM while LG's output word asked for plain Dolby Digital.
input_property() { luna-send -n 1 -w 3000 luna://com.webos.service.audio/tv/getAudioInputProperty '{}' </dev/null 2>/dev/null | grep -o '"audioInputProperty":.*' | cut -c1-400; }
echo "LG says is playing: $(bounded input_property)"
# A soundbar on eARC that lacks DTS-HD or TrueHD cannot play the lossless carrier.
echo "receiver says it takes:"
arc_state() {
  luna-send -n 1 -w 3000 luna://com.webos.service.arccontroller/getARCState '{}' </dev/null 2>/dev/null \
    | grep -o -E '"(arcActivation|audioDeviceType|earcMatProfile|earcATMOS)":[^,}]*|"(earcCapability|advancedEarcCapability)":\[[^]]*\]'
}
bounded arc_state
echo "bypass chains:"
bypass_chains() { grep -o -E "(rts|srcbin)_[0-9a-f]+_[0-9a-f]+_out0 -> (srcbin|lipsync)_[0-9a-f]+" /proc/aaudio/dot_module_connection 2>/dev/null; }
bounded bypass_chains
echo "media pipelines loaded (another app holding audio can block the route):"
# ps reads every process's command line, which takes that process's memory lock.
media_pipelines() { ps w 2>/dev/null | grep "[p]ipeline_process" | sed 's/.*pipeline_process//' | cut -c1-120; }
bounded media_pipelines
# Running with nothing playing, it makes install refuse with "media player is running".
starfish_media() { ps w 2>/dev/null | grep -c "[s]tarfish-media"; }
echo "starfish-media processes: $(bounded starfish_media)"
# What an HDMI source sends and which decoder took it, for "audio not supported"
# banners. Each read of LG's hdmi file adds lines to the kernel log, so it comes
# after the kernel log above.
echo "HDMI input:"
hdmi_input() {
  head -n 4 /proc/lgtv-driver/audio/hdmi 2>/dev/null | tr '\n' ' '; echo
  grep -H -E '^(Connect|Start|UserCodec)=' /proc/lgtv-driver/audio/adec.p0 /proc/lgtv-driver/audio/adec.p1 2>/dev/null | tr '\n' ' '; echo
}
bounded hdmi_input
# Failed system services: a tester's dead Home and remote keys showed up here.
echo "failed services: $(bounded systemctl --failed --no-legend --plain 2>/dev/null | awk '{ print $1 }' | tr '\n' ' ')"

# A tester's TV had Glasshouse's DTS forwarding switched on next to this tool:
# it re-ranks the decoders and takes the eARC output itself.
echo
echo "--- other audio tools on this TV ---"
echo "boot hooks: $(ls /var/lib/webosbrew/init.d/ 2>/dev/null | tr '\n' ' ')"
for p in /var/lib/lg-dts-native /var/lib/lg-dts-core /sys/module/gh_dts_native /sys/module/dts_core_route /sys/module/dts_gate \
    /run/systemd/system/umediaserver.service.d; do
  [ -e "$p" ] && echo "present=$p $(ls "$p" 2>/dev/null | tr '\n' ' ')"
done
# A tester's helper bind-mounts another model's DSP firmware over LG's and reloads it.
fw=$(grep -o ' /lib/firmware/audio_[^ ]*' /proc/mounts 2>/dev/null | tr -d '\n')
[ -n "$fw" ] && echo "audio DSP firmware replaced:$fw" || echo "audio DSP firmware: LG's own"
dsp_decoders() { ls /proc/aaudio/processors/dsp0/ 2>/dev/null | grep '^dec_' | tr '\n' ' '; }
echo "DSP decoders: $(bounded dsp_decoders)"
echo "=== end of report ==="
