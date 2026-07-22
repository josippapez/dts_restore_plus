#!/bin/sh
# detect-target.sh - identify the DTS-restore target profile of an LG webOS TV.
#
# POSIX sh. Meant to run ON THE TV, as root (some probes read system libs and the
# GStreamer registry). READ-ONLY: it inspects, it never mounts, copies, or modifies
# anything. It prints a machine-readable KEY=VALUE report on stdout and a suggested
# PROFILE name that the installer maps to payload/<profile>/.
#
# The three axes that decide binary compatibility + which fix to apply (see
# MULTI-MODEL.md): (1) CPU arch / float ABI of the GStreamer *userspace*, (2)
# GStreamer version, (3) how LG disabled DTS. Each probe below is commented with
# what it establishes and whether it is authoritative or a heuristic.
#
# Exit status: 0 if a known profile was determined, 1 if the profile is unknown
# (so a caller can `detect-target.sh || refuse`).

# Do not abort the whole script if one probe fails - each probe is defensive and
# prints "unknown" on failure, because a partial report is still useful.
set -u

# ---------------------------------------------------------------------------
# Helper: print "KEY=VALUE".
emit() { printf '%s=%s\n' "$1" "$2"; }

# Helper: first existing file matching a glob (POSIX-safe, no arrays).
first_glob() {
  # shellcheck disable=SC2048,SC2086
  for f in $1; do [ -e "$f" ] && { printf '%s\n' "$f"; return 0; }; done
  return 1
}

echo "# dts_restore target detection (read-only)"
echo "# ------------------------------------------------------------"

# ===========================================================================
# PROBE 1a. Dynamic loader name -> coarse arch + float-ABI hint.
#   ld-linux.so.3          => 32-bit ARM, OABI/EABI soft-float (webOS-25 C5 case)
#   ld-linux-armhf.so.3    => 32-bit ARM hard-float (expected CX case, UNVERIFIED)
#   ld-linux-aarch64.so.1  => 64-bit ARM userspace
#   Authoritative for "which loader userspace binaries use". The C5 has an
#   aarch64 *kernel* but a 32-bit soft-float *userspace*, so we must look at the
#   loader, not `uname -m` alone.
# ===========================================================================
LOADER="unknown"
LD=$(first_glob "/lib/ld-linux*.so.* /lib/ld-linux-*.so.* /lib/ld-*.so.*" 2>/dev/null)
[ -n "${LD:-}" ] && LOADER=$(basename "$LD")
emit LOADER "$LOADER"
emit LOADER_PATH "${LD:-unknown}"

# ===========================================================================
# PROBE 1b. ELF e_flags of a real GStreamer system .so -> definitive float ABI.
#   The ARM EABI float ABI lives in the ELF header e_flags field (32-bit LE at
#   file offset 0x24 = 36). webOS-25 C5 shows 0x05000200:
#     - EABI version 5           (0x05000000)
#     - EF_ARM_ABI_FLOAT_SOFT    (0x00000200)   <- soft float
#   Hard-float would set EF_ARM_ABI_FLOAT_HARD (0x00000400) instead.
#   We read a gstreamer .so (not the loader) because it is the plugin ABI we must
#   match. Uses od (present on busybox). This is the authoritative float-ABI check
#   and is the probe MULTI-MODEL.md flags as "still owed for CX".
# ===========================================================================
EFLAGS="unknown"
FLOAT_ABI="unknown"
GSTSO=$(first_glob "/usr/lib/gstreamer-1.0/libgstcoreelements.so /usr/lib/gstreamer-1.0/libgsttypefindfunctions.so /usr/lib/gstreamer-1.0/*.so" 2>/dev/null)
if [ -n "${GSTSO:-}" ] && command -v od >/dev/null 2>&1; then
  # Read 4 bytes at offset 36 as a little-endian 32-bit value.
  # od -An (no address) -t x1 (hex bytes) -j 36 (skip) -N 4 (count).
  bytes=$(od -An -t x1 -j 36 -N 4 "$GSTSO" 2>/dev/null | tr -d ' \n')
  if [ -n "$bytes" ] && [ "${#bytes}" -eq 8 ]; then
    # bytes are b0 b1 b2 b3 (little-endian); recompose as 0xb3b2b1b0.
    b0=$(printf '%s' "$bytes" | cut -c1-2)
    b1=$(printf '%s' "$bytes" | cut -c3-4)
    b2=$(printf '%s' "$bytes" | cut -c5-6)
    b3=$(printf '%s' "$bytes" | cut -c7-8)
    EFLAGS="0x${b3}${b2}${b1}${b0}"
    # Decode the float-ABI nibble. 0x200 => soft, 0x400 => hard.
    val=$(printf '%d' "$EFLAGS" 2>/dev/null || echo 0)
    if [ "$((val & 0x400))" -ne 0 ]; then
      FLOAT_ABI="hard"
    elif [ "$((val & 0x200))" -ne 0 ]; then
      FLOAT_ABI="soft"
    else
      FLOAT_ABI="unspecified"
    fi
  fi
fi
emit GST_PROBE_SO "${GSTSO:-unknown}"
emit ELF_EFLAGS "$EFLAGS"
emit FLOAT_ABI "$FLOAT_ABI"

# ===========================================================================
# PROBE 1c. Kernel/machine arch (context only - NOT the userspace ABI).
#   On the C5 this is aarch64 while userspace is 32-bit soft-float ARM, so this
#   value must never be used alone to pick a plugin build.
# ===========================================================================
emit UNAME_M "$(uname -m 2>/dev/null || echo unknown)"

# ===========================================================================
# PROBE 2. GStreamer version -> plugin ABI + build system.
#   CX-class = 1.14.4 (autotools); webOS 25 = 1.24.0 (meson). Authoritative.
# ===========================================================================
GST_VERSION="unknown"
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  GST_VERSION=$(gst-inspect-1.0 --version 2>/dev/null | grep -i 'GStreamer' | head -n1 | awk '{print $2}')
  [ -n "$GST_VERSION" ] || GST_VERSION="unknown"
fi
emit GST_VERSION "$GST_VERSION"
# Coarse major.minor for profile selection.
GST_MM=$(printf '%s' "$GST_VERSION" | cut -d. -f1-2)
emit GST_MAJMIN "${GST_MM:-unknown}"

# ===========================================================================
# PROBE 3. webOS release + product_id (model/chassis family).
#   nyx-cmd is LG's device-info CLI (same source install.sh uses at L82-83).
#   webos_release major: 5=webOS5 (CX), 6=webOS6 (C1/C2), 7/8/9=webOS22/23/24,
#   10=webOS25 (C5). product_id carries the chassis code (e.g. OLED77C51LA).
# ===========================================================================
WEBOS_RELEASE="unknown"
PRODUCT_ID="unknown"
if command -v nyx-cmd >/dev/null 2>&1; then
  WEBOS_RELEASE=$(nyx-cmd OSInfo query webos_release 2>/dev/null | head -n1)
  PRODUCT_ID=$(nyx-cmd DeviceInfo query product_id 2>/dev/null | head -n1)
  [ -n "$WEBOS_RELEASE" ] || WEBOS_RELEASE="unknown"
  [ -n "$PRODUCT_ID" ] || PRODUCT_ID="unknown"
fi
emit WEBOS_RELEASE "$WEBOS_RELEASE"
emit PRODUCT_ID "$PRODUCT_ID"
WEBOS_MAJOR=$(printf '%s' "$WEBOS_RELEASE" | cut -d. -f1)
emit WEBOS_MAJOR "${WEBOS_MAJOR:-unknown}"

# ===========================================================================
# PROBE 4. Which DTS decoder(s), if any, are already registered.
#   webOS 25 ships NONE (that's the whole problem); CX ships avdec_dca but ranked
#   0 until gstcool.conf bumps it. Presence steers the fix mechanism.
# ===========================================================================
HAS_AVDEC_DCA="no"
HAS_DTSDEC="no"
HAS_DTS_AUDIODEC="no"   # LG's proprietary decoder name
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  gst-inspect-1.0 avdec_dca    >/dev/null 2>&1 && HAS_AVDEC_DCA="yes"
  gst-inspect-1.0 dtsdec       >/dev/null 2>&1 && HAS_DTSDEC="yes"
  gst-inspect-1.0 dts_audiodec >/dev/null 2>&1 && HAS_DTS_AUDIODEC="yes"
fi
emit HAS_AVDEC_DCA "$HAS_AVDEC_DCA"
emit HAS_DTSDEC "$HAS_DTSDEC"
emit HAS_DTS_AUDIODEC "$HAS_DTS_AUDIODEC"

# ===========================================================================
# PROBE 5. matroskademux DTS caps behaviour: audio/x-dts vs audio/x-unknown.
#   THE mechanism discriminator (MULTI-MODEL.md sec.1):
#     - CX-nerf     => demuxer has NO DTS pad at all (A_DTS strings stripped)
#     - webOS25     => demuxer emits audio/x-unknown, codec-id=A_DTS (re-tag)
#     - DTS-enabled => demuxer emits audio/x-dts
#   Definitive classification needs a real DTS MKV pushed through matroskademux
#   (typefind on a sample) - which this read-only detector cannot synthesize.
#   So we do a STATIC heuristic: grep the demuxer .so for the A_DTS codec-id and
#   the audio/x-dts caps string, and DOCUMENT the on-device probe to run manually.
# ===========================================================================
MKV_SO=$(first_glob "/usr/lib/gstreamer-1.0/libgstmatroska.so" 2>/dev/null)
MKV_HAS_ADTS="unknown"     # is the "A_DTS" Matroska codec-id string present?
MKV_HAS_XDTS="unknown"     # is the "audio/x-dts" caps string present?
if [ -n "${MKV_SO:-}" ] && command -v strings >/dev/null 2>&1; then
  if strings "$MKV_SO" 2>/dev/null | grep -q 'A_DTS'; then MKV_HAS_ADTS="yes"; else MKV_HAS_ADTS="no"; fi
  if strings "$MKV_SO" 2>/dev/null | grep -q 'audio/x-dts'; then MKV_HAS_XDTS="yes"; else MKV_HAS_XDTS="no"; fi
fi
emit MKV_DEMUX_SO "${MKV_SO:-unknown}"
emit MKV_HAS_A_DTS_STRING "$MKV_HAS_ADTS"
emit MKV_HAS_XDTS_CAPS_STRING "$MKV_HAS_XDTS"
# Static inference of the disable mechanism (heuristic - confirm with the probe below):
#   A_DTS present + x-dts present  => demuxer likely emits real DTS caps (enabled or webOS25-style)
#   A_DTS absent                  => CX-style nerf (codec-id mapping compiled out)
MECH="unknown"
if [ "$MKV_HAS_ADTS" = "no" ]; then
  MECH="cx-demuxer-nerf"
elif [ "$MKV_HAS_ADTS" = "yes" ] && [ "$HAS_AVDEC_DCA" = "no" ] && [ "$HAS_DTSDEC" = "no" ] && [ "$HAS_DTS_AUDIODEC" = "no" ]; then
  MECH="webos25-retag-no-decoder"
elif [ "$MKV_HAS_ADTS" = "yes" ]; then
  MECH="demuxer-emits-dts"
fi
emit DTS_DISABLE_MECHANISM_GUESS "$MECH"

cat <<'PROBE_DOC'
# ------------------------------------------------------------
# MANUAL RUNTIME PROBE (needs a DTS sample - documented, not run here):
#   The static grep above cannot tell audio/x-dts from audio/x-unknown at
#   runtime. To confirm the re-tag, push a known DTS MKV through the demuxer and
#   read the audio pad caps, e.g.:
#
#     gst-launch-1.0 -v filesrc location=/tmp/dts_sample.mkv ! matroskademux \
#        ! fakesink silent=false 2>&1 | grep -iE 'audio/x-(dts|unknown)'
#
#   - "audio/x-unknown, codec-id=(string)A_DTS"  => webos25-retag-no-decoder
#       -> fix = inject widened-caps dtsdec (profile webos25-armel-gst124)
#   - "audio/x-dts"                              => decoder-absent only
#       -> fix = rank an existing/normal-caps decoder
#   - no DTS audio pad at all                    => cx-demuxer-nerf
#       -> fix = override demuxer libs + bump avdec_dca rank (profile cx-armv7-gst114)
# ------------------------------------------------------------
PROBE_DOC

# ===========================================================================
# PROFILE SELECTION. Combine the axes into a suggested profile name.
#   Known:
#     cx-armv7-gst114        GStreamer 1.14.x, armv7 (float ABI to be confirmed)
#     webos25-armel-gst124   GStreamer 1.24.x, 32-bit soft-float, ld-linux.so.3,
#                            re-tag + no decoder
#   Anything else -> unknown-<gstmajmin>-<arch> (installer should refuse).
# ===========================================================================
PROFILE="unknown"
case "$GST_MM" in
  1.14)
    # CX-class. armv7; float ABI reported separately so the build can be picked.
    PROFILE="cx-armv7-gst114"
    ;;
  1.24)
    if [ "$LOADER" = "ld-linux.so.3" ] && [ "$FLOAT_ABI" = "soft" ]; then
      PROFILE="webos25-armel-gst124"
    else
      PROFILE="webos25-${LOADER}-${FLOAT_ABI}"
    fi
    ;;
  *)
    arch_tag="${LOADER}"
    [ "$arch_tag" = "unknown" ] && arch_tag=$(uname -m 2>/dev/null || echo arch)
    PROFILE="unknown-gst${GST_MM}-${arch_tag}"
    ;;
esac
echo "# ------------------------------------------------------------"
emit PROFILE "$PROFILE"

# Exit 1 on an unknown profile so `detect-target.sh || refuse` works in install.sh.
case "$PROFILE" in
  cx-armv7-gst114|webos25-armel-gst124) exit 0 ;;
  *) exit 1 ;;
esac
