#!/bin/sh
# webOS25 DTS + TrueHD restore. Runs at boot via /var/lib/webosbrew/init.d/restore_dts25.
#
# THREE BYTE-IDENTICAL COPIES (CLAUDE.md rule 3): this file, the base64 INIT_B64
# blob in restore/install.sh, and w25InitScriptBody() in app/service/service.js.
# The block between the W25-COMPAT markers is additionally shared verbatim with
# the app's read-only DETECT_PROBE, so the verdict the UI shows is the verdict
# this script enforces. Change one copy -> change all three.
#
# MODES
#   (default)     apply: self-heal check (unowned -> revert+unlink; owned but the
#                 core payload is incomplete -> refuse, delete nothing) -> version
#                 guard -> identity gate -> loader gate -> binds -> post-bind
#                 proof -> registry commit.
#   W25_CHECK=1   read-only preflight for install.sh / the app's Enable: prints
#                 VERDICT/REASON/LABEL/CANFORCE/LOADER and exits. Binds nothing,
#                 writes nothing, toasts nothing.
#   W25_STAND_DOWN=1
#                 put this TV back on stock and exit: drop our binds and rebuild
#                 LG's registry if one of ours is live. No gate, no binds, no
#                 toast, and it can never apply anything. This is what the two
#                 installers call when their preflight refuses AFTER they have
#                 already detached the binds, so "nothing is applied" is true
#                 rather than merely intended.
#   FORCE=1       explicit opt-in on a TV that is not in the verified table.
#
# Always exits 0: a non-zero webosbrew init script trips the failsafe that
# disables ALL root customisations on the next boot. Every refusal path binds
# nothing -- the overrides are system-wide (they propagate into every app jail),
# so "not sure" must mean "do not touch this TV". The two container demuxers are
# OPTIONAL: without them DTS still works in MKV, which is how this shipped before
# the gate existed, so each is bound only when staged.
#
# A refusal also REPAIRS: it drops the binds and, if a media registry we wrote is
# still live, regenerates LG's. That second half is not optional -- the registry
# is committed with `cp -f` rather than bind-mounted, so an unrepaired one keeps
# our dtsdec registered by absolute path even with every bind gone.
#
# STAGED FILES ARE LEFT IN PLACE ON A REFUSAL, DELIBERATELY. A refused install
# links no boot hook, so nothing of ours ever runs again on that TV; the files
# under /var/lib/webosbrew are inert without the binds. They are kept because
# stock.fp is among them, and that recorded baseline is the only way a later boot
# can tell "the firmware changed" from "this TV was never verified" -- deleting it
# would downgrade every future drift into a bare "unverified". The app's Uninstall
# removes them on demand.
set -u
LOG=/tmp/dts25.log
W25_LOG=$LOG
toast() { luna-send -n 1 luna://com.webos.notification/createToast "{\"sourceId\":\"io.github.josippapez.dtsenabler\",\"message\":\"$1\"}" >/dev/null 2>&1; }
echo "--- dts25+truehd $(date) ---" >> $LOG 2>&1
# >>> W25-COMPAT-BEGIN
# Compatibility gate + reversibility helpers -- the SINGLE AUTHORED COPY.
# Everything here is either a pure measurement or an explicitly-called action;
# merely sourcing this block mounts, deletes and writes nothing, which is what
# lets the app's read-only probe reuse it as-is.
EXPECT_GST=1.24
# Stamp identifying the GATE this script enforces. The installed copy on a TV is
# only rewritten by Enable / install.sh, so a TV enabled under an older app keeps
# running that older script -- while the app's own probe judges with the compat
# block embedded in the NEW build. The app compares this value (which it reads
# from its own embedded copy of this block) against the one it finds in the
# installed script, and reports the hook as stale instead of silently rewriting a
# privileged file behind the user's back.
#
# BUMP THIS whenever the behaviour of this script changes. It is deliberately
# independent of the app version: a cosmetic app release must not invalidate a
# perfectly current hook, and a gate change must not hide behind an unchanged app
# version. check-init-sync.sh proves the three IN-REPO copies match; this proves
# the ON-TV copy matches the app that is asking, and sync-init.sh refuses to
# regenerate the derived copies if the body moved while this did not.
#   1  first gate: verified-sets table, loader gate, post-bind proof, self-heal,
#      stand-down on every refusal, /etc fingerprints, hook stamp.
#   2  w25_stock_registry refuses while a plugin bind of ours survives; the two
#      installers stand the TV down on a refusal instead of leaving it half-way.
#   3  the verified-sets table also keys on a product-id glob. Plugin hashes are
#      not model-unique -- a G5/M5 ships byte-identical copies of all three -- so
#      a hash-only match used to report `verified` with the C5's label on hardware
#      nobody had tested. A hash match on a different model is now `unverified`
#      with CANFORCE=1 and a reason that says the artifacts are identical but the
#      model is untested, which routes it through the existing explicit opt-in
#      instead of auto-applying.
W25_GATE_VERSION=3
FP=/var/lib/webosbrew/dts25/stock.fp
# Where the installed copy of THIS script lives, and the boot hook that symlinks
# to it. Named here, in the shared block, so the read-only probe can fingerprint
# the script and see whether the hook is linked without duplicating either path.
INIT_SELF=/var/lib/webosbrew/dts25/init_dts25.sh
HOOK=/var/lib/webosbrew/init.d/restore_dts25
REG=/mnt/flash/data/gst_1_0_registry.arm.bin
CFG=/etc/umediaserver/device_codec_capability_config.json
GC=/etc/gst/gstcool.conf
LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so
LGISO=/usr/lib/gstreamer-1.0/libgstisomp4.so
LGTSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so
# CORE payload -- without either of these there is no DTS and no TrueHD, so a
# missing one means "do not bind anything".
MYDTS=/var/lib/webosbrew/dts25/libgstdtsdec.so
MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so
# OPTIONAL payload: the patched container demuxers. Absent, DTS still works in
# MKV -- which is exactly how this shipped before the gate existed -- so each is
# bound only when staged and is never a reason to refuse or to delete anything.
MYISO=/var/lib/webosbrew/demux25/libgstisomp4.so
MYTSD=/var/lib/webosbrew/demux25/libgstmpegtsdemux.so
MYCFG=/var/lib/webosbrew/truehd/codec_capability.json
MYGC=/var/lib/webosbrew/truehd/gstcool.conf
MYLIBS=/var/lib/webosbrew/truehd/libs:/var/lib/webosbrew/dts25/libs
w25_log() { [ -n "${W25_LOG:-}" ] || return 0; echo "[dts25-gate $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "${W25_LOG:-}" 2>&1; }
# Unmount ONE bind target, falling back to a LAZY detach when it is busy.
# Measured on a real C5: umount of /usr/lib/gstreamer-1.0/libgstlibav.so fails
# with "target is busy" because WebAppMgr has the .so mapped, while `umount -l`
# succeeds. Without this fallback Disable only logs a WARN and silently leaves
# the override applied.
w25_umount() {
  grep -q " $1 " /proc/mounts 2>/dev/null || { w25_log "no bind over $1"; return 0; }
  if umount "$1" 2>/dev/null; then w25_log "unmounted bind over $1"; return 0; fi
  if umount -l "$1" 2>/dev/null; then w25_log "lazy-detached busy bind over $1 (a live mapping held it)"; return 0; fi
  w25_log "WARN could not unmount $1, even lazily"
  return 1
}
# Drop every override WE applied -- rootfs paths only. The same binds propagate
# into each app jail (27 jail-side copies per library on a real C5); those are
# left alone deliberately, because detaching a jail's own view would break that
# jail. $REG is included because older builds bind-mounted it.
#
# A target that survives even a lazy detach is REPORTED, not swallowed: it means
# the revert did not actually happen, and silently claiming success is precisely
# the class of failure this whole change exists to remove. Returns 1 and prints
# WARN_UNMOUNT=<targets> so every caller -- boot script and app alike -- carries
# the warning up.
w25_drop_binds() {
  UNMOUNT_FAILED=
  for t in "$CFG" "$GC" "$LGLIBAV" "$LGISO" "$LGTSD" "$REG"; do
    w25_umount "$t" || UNMOUNT_FAILED="${UNMOUNT_FAILED:+$UNMOUNT_FAILED }$t"
  done
  [ -z "$UNMOUNT_FAILED" ] && return 0
  w25_log "WARN revert incomplete -- still mounted: $UNMOUNT_FAILED"
  echo "WARN_UNMOUNT=$UNMOUNT_FAILED"
  return 1
}
# THE refusal action. Standing down is not "skip the binds" -- it is "leave this
# TV stock". The binds can be undone, but the media registry cannot: it is
# committed with `cp -f`, and any registry WE generated was scanned with
# /var/lib/webosbrew/dts25 on the plugin path, so it registers OUR dtsdec by
# absolute path and keeps doing so system-wide (every one of the 27 jail views)
# long after the binds are gone. Dropping binds while leaving that registry live
# is exactly the state the gate exists to prevent -- and it is the state an OTA
# that changes the stock plugins while GStreamer stays 1.24 would leave behind.
#
# Idempotent and self-limiting: w25_stock_registry scans LG's plugin directories
# ONLY, so the registry it writes does not name /var/lib/webosbrew and the next
# w25_reg_is_ours is false -- no repeated 60s regen on every boot. It also works
# when our libraries are already gone, for the same reason.
w25_stand_down() {
  w25_drop_binds
  if w25_reg_is_ours; then
    w25_log "a registry of ours is still live; regenerating the stock registry so nothing of ours stays registered"
    w25_stock_registry
  fi
  return 0
}
w25_md5() { [ -f "$1" ] || return 0; md5sum "$1" 2>/dev/null | cut -d" " -f1; }
w25_bound() { if grep -q " $1 " /proc/mounts 2>/dev/null; then echo 1; else echo 0; fi; }
w25_fp_get() { [ -f "$FP" ] || return 0; sed -n "s/^$1=//p" "$FP" 2>/dev/null | head -n1; }
w25_gst_mm() { /usr/bin/gst-inspect-1.0 --version 2>/dev/null | sed -n 's/^GStreamer \([0-9]*\.[0-9]*\).*/\1/p' | head -n1; }
w25_product_id() { command -v nyx-cmd >/dev/null 2>&1 || { echo unknown; return 0; }; v=$(nyx-cmd DeviceInfo query product_id 2>/dev/null | head -n1); [ -n "$v" ] || v=unknown; echo "$v"; }
w25_webos_release() { command -v nyx-cmd >/dev/null 2>&1 || { echo unknown; return 0; }; v=$(nyx-cmd OSInfo query webos_release 2>/dev/null | head -n1); [ -n "$v" ] || v=unknown; echo "$v"; }
# Measure the STOCK fingerprints of everything we shadow, plus the live GStreamer
# version. While we are ENABLED a bind of ours shadows the target, so its live
# hash is OURS and the stock hash is unmeasurable -- fall back to what stock.fp
# recorded when it still was pristine. S_* = measured live (empty when bound),
# M_* = effective stock fingerprint, B_* = 1 when our bind is present.
#
# The two /etc files matter as much as the plugins. $MYCFG and $MYGC are
# SNAPSHOTS, awk-derived at install time from the TV's own live
# device_codec_capability_config.json and gstcool.conf, and we bind them over the
# originals indefinitely. Those originals only ever change via an OTA -- exactly
# the event this gate exists to catch -- so an update that rewrites either one
# while leaving the three plugins alone must not read as "verified": the hook
# would silently revert LG's own config change, system-wide, forever.
#
# Residual we are NOT engineering around: libgstmatroska.so is neither shadowed
# nor fingerprinted, so an OTA that changes its A_DTS retag would silently lose
# MKV DTS. That fails in the acceptable direction -- it costs our codec and harms
# nothing else -- and the five-element proof still passes, because it checks that
# matroskademux REGISTERS, not what caps it emits.
w25_measure() {
  B_LIBAV=$(w25_bound "$LGLIBAV")
  B_ISOMP4=$(w25_bound "$LGISO")
  B_MPEGTS=$(w25_bound "$LGTSD")
  B_CFG=$(w25_bound "$CFG")
  B_GC=$(w25_bound "$GC")
  S_LIBAV=
  S_ISOMP4=
  S_MPEGTS=
  S_CFG=
  S_GC=
  [ "$B_LIBAV" = 0 ] && S_LIBAV=$(w25_md5 "$LGLIBAV")
  [ "$B_ISOMP4" = 0 ] && S_ISOMP4=$(w25_md5 "$LGISO")
  [ "$B_MPEGTS" = 0 ] && S_MPEGTS=$(w25_md5 "$LGTSD")
  [ "$B_CFG" = 0 ] && S_CFG=$(w25_md5 "$CFG")
  [ "$B_GC" = 0 ] && S_GC=$(w25_md5 "$GC")
  M_LIBAV=$S_LIBAV
  M_ISOMP4=$S_ISOMP4
  M_MPEGTS=$S_MPEGTS
  M_CFG=$S_CFG
  M_GC=$S_GC
  [ -n "$M_LIBAV" ] || M_LIBAV=$(w25_fp_get libgstlibav)
  [ -n "$M_ISOMP4" ] || M_ISOMP4=$(w25_fp_get libgstisomp4)
  [ -n "$M_MPEGTS" ] || M_MPEGTS=$(w25_fp_get libgstmpegtsdemux)
  [ -n "$M_CFG" ] || M_CFG=$(w25_fp_get device_codec_capability_config)
  [ -n "$M_GC" ] || M_GC=$(w25_fp_get gstcool)
  GST_MM_NOW=$(w25_gst_mm)
  [ -n "$GST_MM_NOW" ] || GST_MM_NOW=unknown
  return 0
}
# Recorded-vs-measured comparison that treats "we have no recorded value" as
# absence of evidence rather than as drift. Needed for the two /etc keys, which a
# stock.fp written by an earlier build simply does not contain -- comparing an
# empty recording against a real hash would report drift on every upgrade.
w25_fp_differs() { [ -n "$1" ] && [ -n "$2" ] && [ "$1" != "$2" ]; }
# Guard layer 0: our bind-over libs are armel GStreamer-1.24 builds. Binding
# them over a different-ABI LG lib after an OTA would break ALL mp4/ts/mkv
# playback, and no opt-in may override that.
w25_gst_ok() { [ "$GST_MM_NOW" = "$EXPECT_GST" ]; }
# Gate layer 1 -- identity. Sets VERDICT REASON LABEL CANFORCE from the measured
# stock fingerprints:
#   verified   the md5s are in the verified-sets table below
#   forced     no table match, but the user opted in and nothing changed since
#   drift      stock.fp recorded DIFFERENT md5s -> firmware update, stand down
#              UNCONDITIONALLY (FORCE cannot override it, and neither can the
#              ABI-change drift w25_gate reports)
#   unverified no match and no opt-in -> bind nothing
# The md5 of the stock libs is the key, not the model name: identical hashes
# mean these are literally the libraries the payload was verified against.
w25_verdict() {
  VERDICT=unverified
  REASON=
  LABEL=
  CANFORCE=0
  if [ -z "$M_LIBAV" ] || [ -z "$M_ISOMP4" ] || [ -z "$M_MPEGTS" ]; then
    REASON="the stock GStreamer plugin fingerprints could not be read on this TV"
    return 0
  fi
  # DRIFT IS CHECKED BEFORE THE TABLE, and that ordering is load-bearing. The
  # table can only key on gst_mm + the three plugin md5s, so it cannot express the
  # state of the two /etc files -- which means a table match would return
  # `verified` and mask an OTA that rewrote only gstcool.conf or the codec
  # capability JSON. "Has this TV changed since we recorded it" therefore outranks
  # "does this TV look like a known-good one".
  FP_AV=$(w25_fp_get libgstlibav)
  FP_ISO=$(w25_fp_get libgstisomp4)
  FP_TSD=$(w25_fp_get libgstmpegtsdemux)
  FP_CFG=$(w25_fp_get device_codec_capability_config)
  FP_GC=$(w25_fp_get gstcool)
  # The three plugin hashes compare strictly (they have always been recorded).
  # The two /etc hashes go through w25_fp_differs so an older stock.fp that never
  # recorded them does not read as drift.
  DRIFT_WHAT=
  DRIFT_PLUGINS=0
  DRIFT_CONFIG=0
  [ "$FP_AV" != "$M_LIBAV" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstlibav.so"; DRIFT_PLUGINS=1; }
  [ "$FP_ISO" != "$M_ISOMP4" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstisomp4.so"; DRIFT_PLUGINS=1; }
  [ "$FP_TSD" != "$M_MPEGTS" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstmpegtsdemux.so"; DRIFT_PLUGINS=1; }
  w25_fp_differs "$FP_CFG" "$M_CFG" && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }device_codec_capability_config.json"; DRIFT_CONFIG=1; }
  w25_fp_differs "$FP_GC" "$M_GC" && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }gstcool.conf"; DRIFT_CONFIG=1; }
  if [ -n "$FP_AV$FP_ISO$FP_TSD" ] && [ -n "$DRIFT_WHAT" ]; then
    # UNCONDITIONAL refusal -- FORCE is deliberately not consulted here, and
    # CANFORCE=0 so nothing advertises an escape hatch that does not exist.
    # A drift-specific override would be redundant: uninstall removes stock.fp,
    # so uninstall-then-enable puts this TV back into the "unverified" flow, which
    # already has an explicit, consented opt-in. Refusing outright keeps the state
    # machine smaller and the rule absolute: a firmware change that touches the
    # plugins we shadow stands us down, full stop.
    VERDICT=drift
    CANFORCE=0
    # The remedy depends on WHAT drifted. A plugin change is something a verified
    # set can describe, so the fingerprints are worth reporting. A change to the
    # two /etc files is NOT: no table row can ever clear it, because the table
    # keys on plugins only -- the snapshots simply have to be retaken from the
    # TV's new config, which is what Enable does.
    if [ "$DRIFT_PLUGINS" = 1 ] && [ "$DRIFT_CONFIG" = 1 ]; then
      DRIFT_FIX="uninstall then enable again to re-snapshot this TV's configuration, and report the new plugin fingerprints so this TV can be added to the verified table"
    elif [ "$DRIFT_PLUGINS" = 1 ]; then
      DRIFT_FIX="report the new fingerprints so this TV can be added to the verified table, or uninstall then enable again to opt in explicitly"
    else
      DRIFT_FIX="uninstall then enable again to re-snapshot this TV's configuration (no verified-set entry can cover a config change)"
    fi
    REASON="this TV changed since it was last verified ($DRIFT_WHAT), so nothing was applied; $DRIFT_FIX"
    return 0
  fi
  # The table keys on plugin hashes, which are NOT model-unique: a G5/M5 ships
  # byte-identical copies of all three, so a hash-only match reported `verified`
  # with the C5's label on hardware nobody had ever tested. That is a false
  # hardware-verification claim, so the row now also carries a product-id glob and
  # the two cases are kept apart:
  #   hashes match AND product matches -> verified   (this model really was tested)
  #   hashes match, product does not   -> unverified + CANFORCE (binary-set match
  #                                      only; strongest possible non-hardware
  #                                      evidence, so the existing explicit opt-in
  #                                      is the right gate rather than auto-apply)
  PRODUCT_NOW=$(w25_product_id)
  while IFS='|' read -r t_mm t_av t_iso t_tsd t_product t_label; do
    case "$t_mm" in ''|\#*) continue ;; esac
    if [ "$t_mm" = "$GST_MM_NOW" ] && [ "$t_av" = "$M_LIBAV" ] && [ "$t_iso" = "$M_ISOMP4" ] && [ "$t_tsd" = "$M_MPEGTS" ]; then
      LABEL=$t_label
      case "$PRODUCT_NOW" in
        $t_product)
          VERDICT=verified
          REASON="stock plugin fingerprints match a TV this payload was verified on"
          ;;
        *)
          VERDICT=unverified
          CANFORCE=1
          REASON="this TV's stock plugins are byte-identical to $t_label, so the payload is very likely compatible -- but THIS model ($PRODUCT_NOW) has never been tested on hardware, so it is not reported as verified. Opt in explicitly to try it, and please report the result so it can be added properly"
          ;;
      esac
      break
    fi
  done <<'W25_SETS'
# gst_mm|libgstlibav|libgstisomp4|libgstmpegtsdemux|product_id_glob|label
# product_id_glob must be a shell glob matched against nyx-cmd product_id. Only add
# a row with a real product glob after DTS and TrueHD have actually played on that
# model -- a byte-identical artifact set is NOT hardware verification.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|57fe57060774f248c05af5a411fc9a8f|9b84a95cf29bc025553c7dee829b7cc1|OLED*C5*|LG C5 OLED77C51LA (webOS 10.3.1, GStreamer 1.24.0)
W25_SETS
  [ "$VERDICT" = verified ] && return 0
  [ "$VERDICT" = unverified ] && [ "$CANFORCE" = 1 ] && return 0
  if [ "$(w25_fp_get forced)" = 1 ] && [ -n "$FP_AV" ]; then
    VERDICT=forced
    CANFORCE=1
    REASON="not a verified TV, but you opted in and nothing has changed since"
    return 0
  fi
  if [ "${FORCE:-0}" = 1 ]; then
    VERDICT=forced
    CANFORCE=1
    REASON="not a verified TV; applying because you explicitly opted in"
    return 0
  fi
  CANFORCE=1
  REASON="this TV's stock GStreamer plugins match no TV this payload was verified on"
  return 0
}
# Version guard + identity gate as one call, for the read-only callers (the
# app's probe and W25_CHECK=1). The apply path below runs the two as separate
# ordered steps so each gets its own log line and toast.
w25_gate() {
  w25_gst_ok && { w25_verdict; return 0; }
  VERDICT=drift
  CANFORCE=0
  LABEL=
  REASON="GStreamer $GST_MM_NOW is not the $EXPECT_GST this payload was built for; a firmware update changed the plugin ABI"
  return 0
}
w25_loader() {
  for f in /lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3 /lib/ld-linux.so.2 /lib/ld-linux*.so.* /lib/ld-*.so.*; do
    [ -x "$f" ] && { printf '%s\n' "$f"; return 0; }
  done
  return 1
}
# Gate layer 2a -- loader resolution. Every payload object we are about to bind
# or register must have ALL its dynamic dependencies resolvable on THIS TV.
# The assertion is that OUR deps resolve, NOT that our sonames match LG's:
# stock libav on the verified C5 is ffmpeg 5.x (libavcodec.so.59,
# libavformat.so.59, libavutil.so.57, libavfilter.so.8) while ours is ffmpeg 4.4
# (.58/.58/.56/.7), so a soname-equality check against stock would refuse a TV
# where the payload demonstrably works. Our objects carry
# RUNPATH=/var/lib/webosbrew/truehd/libs; $MYLIBS is kept for the dts25/libs case.
# LOADER_STAGED distinguishes "these libraries cannot load on this TV" (a real
# refusal, and forcing would not help) from "the core payload is not staged" (the
# state before the first Enable, where the answer is simply not known). The app
# uses that to decide whether offering "Try anyway" would be honest.
#
# Only the CORE objects are required. The optional demuxers are checked when they
# are staged and skipped when they are not, so a core-only install is a first
# class configuration rather than a failure.
w25_core_staged() { [ -f "$MYDTS" ] && [ -f "$MYLIBAV" ]; }
w25_loader_ok() {
  LOADER_MISS=
  LOADER_STAGED=1
  LD_SO=$(w25_loader)
  if [ -z "${LD_SO:-}" ]; then LOADER_MISS="no dynamic loader found on this TV"; return 1; fi
  if ! w25_core_staged; then
    LOADER_STAGED=0
    LOADER_MISS="the core payload is not staged ($MYDTS / $MYLIBAV)"
    return 1
  fi
  for so in "$MYDTS" "$MYLIBAV" "$MYISO" "$MYTSD"; do
    [ -f "$so" ] || continue
    n=$(LD_LIBRARY_PATH="$MYLIBS" LD_TRACE_LOADED_OBJECTS=1 "$LD_SO" "$so" 2>&1 | grep -c "not found")
    if [ "$n" != 0 ]; then LOADER_MISS="$so has $n unresolved dependencies on this TV"; return 1; fi
  done
  return 0
}
# Is the live media registry one WE wrote? It is committed with `cp -f`, not
# bind-mounted, so there is no mount to look for -- but a registry we generated
# names our plugin directories, and a stock one never does. This is the signal
# for "a registry of ours is live", which matters when our libraries have gone
# missing underneath it.
w25_reg_is_ours() { [ -f "$REG" ] && grep -q "/var/lib/webosbrew" "$REG" 2>/dev/null; }
# Gate layer 2b -- post-bind pipeline proof. The regenerated registry must carry
# ALL FIVE elements the DTS/TrueHD path needs: our two decoders AND the three
# demuxers we shadow (all three are present in the media registry today).
# A missing demuxer means our override produced a plugin the registry cannot
# use, i.e. a broken mp4/ts/mkv pipeline -- so the caller refuses the commit and
# drops the binds, which turns "the override didn't match" into a plain no-op.
w25_reg_has_all() {
  REG_MISS=
  for e in dtsdec avdec_truehd qtdemux tsdemux matroskademux; do
    GST_REGISTRY_1_0="$1" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 "$e" >/dev/null 2>&1 || { REG_MISS=$e; return 1; }
  done
  return 0
}
# Regenerate a clean STOCK registry from the pristine on-disk plugins and write
# it over the media registry. The registry is committed with `cp -f` (a
# PERSISTENT overwrite), not a bind-mount, so no umount can revert it; left
# alone it keeps referencing removed /var/lib/webosbrew plugins and breaks
# media-pipeline app audio (root-caused on a real C5, 2026-07-23). Call only
# AFTER the binds are dropped. Same routine as uninstall.sh step 2b.
#
# RETURNS 0 ONLY IF LG'S REGISTRY IS ACTUALLY BACK IN PLACE. This is a cold-cache
# full plugin scan under `timeout`, and at boot it runs at the busiest moment on
# the box, so it genuinely can fail -- callers that go on to DELETE our plugins
# must branch on this, never assume it worked. The scan's own exit status is not
# sufficient evidence either: a truncated registry would be worse than none, so
# the file has to be non-empty before it is committed.
# Are any of the plugin binds we manage still mounted? w25_stock_registry has to
# refuse while one is. The scan would then load OUR libgstlibav.so from the stock
# path (its RUNPATH resolves fine), produce a syntactically valid registry that
# happens to name no /var/lib/webosbrew path at all -- which CLEARS
# w25_reg_is_ours, the very signal callers use to decide a repair is needed -- and
# report success. A caller would take that as proof the TV is back on stock and
# delete the payload out from under the surviving bind. Refusing here makes this
# function's postcondition ("$REG is a stock registry") true by construction, and
# every caller already handles a return of 1 by deferring instead of deleting.
w25_plugin_binds_present() {
  grep -q " $LGLIBAV " /proc/mounts 2>/dev/null && return 0
  grep -q " $LGISO " /proc/mounts 2>/dev/null && return 0
  grep -q " $LGTSD " /proc/mounts 2>/dev/null && return 0
  return 1
}
w25_stock_registry() {
  if w25_plugin_binds_present; then
    w25_log "WARN refusing to rebuild the stock registry: a plugin bind of ours is still mounted, so the scan would not be reading stock plugins"
    return 1
  fi
  CLEAN_REG=/tmp/gst_clean_reg.bin
  rm -f "$CLEAN_REG" 2>/dev/null
  if GST_REGISTRY_1_0="$CLEAN_REG" GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0 GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 60 /usr/bin/gst-inspect-1.0 >/dev/null 2>&1 && [ -s "$CLEAN_REG" ]; then
    if cp -f "$CLEAN_REG" "$REG" 2>/dev/null; then
      w25_log "regenerated clean stock registry over $REG"
      rm -f "$CLEAN_REG" 2>/dev/null
      return 0
    fi
    w25_log "WARN could not write the clean stock registry to $REG"
  else
    w25_log "WARN clean stock registry regen failed (scan timed out, errored, or produced nothing); leaving $REG untouched"
  fi
  rm -f "$CLEAN_REG" 2>/dev/null
  return 1
}
# Record the PRISTINE stock fingerprints + TV identity. $1=verified, $2=forced.
# Written only when the gate allows applying, and only from hashes taken while
# no bind of ours shadowed the plugins -- that is what later lets a boot tell
# "the firmware changed" apart from "we are simply enabled".
w25_fp_write() {
  mkdir -p /var/lib/webosbrew/dts25 2>/dev/null
  { echo "gst_mm=$GST_MM_NOW"
    echo "product_id=$(w25_product_id)"
    echo "webos_release=$(w25_webos_release)"
    echo "libgstlibav=$M_LIBAV"
    echo "libgstisomp4=$M_ISOMP4"
    echo "libgstmpegtsdemux=$M_MPEGTS"
    echo "device_codec_capability_config=$M_CFG"
    echo "gstcool=$M_GC"
    echo "verified=$1"
    echo "forced=$2"
    echo "written=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$FP.tmp" 2>/dev/null && mv -f "$FP.tmp" "$FP" 2>/dev/null \
    && w25_log "wrote $FP (verified=$1 forced=$2)" || w25_log "WARN could not write $FP"
  return 0
}
# <<< W25-COMPAT-END
# W25_CHECK=1: read-only preflight used by install.sh and the app's Enable to
# refuse with a readable reason BEFORE they link the boot hook. Prints only.
if [ "${W25_CHECK:-0}" = 1 ]; then
  w25_measure
  w25_gate
  echo "GST_MM=$GST_MM_NOW"
  echo "VERDICT=$VERDICT"
  echo "REASON=$REASON"
  echo "LABEL=$LABEL"
  echo "CANFORCE=$CANFORCE"
  if w25_loader_ok; then echo "LOADER=ok"; else echo "LOADER=$LOADER_MISS"; fi
  echo "LOADER_STAGED=${LOADER_STAGED:-1}"
  # The six values a maintainer needs to add this TV to the verified-sets table.
  # Empty md5s mean the plugin is currently shadowed by one of our binds, so the
  # STOCK hash is unmeasurable right now -- run this with the overrides dropped.
  echo "PRODUCT_ID=$(w25_product_id)"
  echo "WEBOS_RELEASE=$(w25_webos_release)"
  echo "MD5_LIBGSTLIBAV=$S_LIBAV"
  echo "MD5_LIBGSTISOMP4=$S_ISOMP4"
  echo "MD5_LIBGSTMPEGTSDEMUX=$S_MPEGTS"
  echo "MD5_DEVICE_CODEC_CAPABILITY_CONFIG=$S_CFG"
  echo "MD5_GSTCOOL=$S_GC"
  # Which gate this script enforces, and its own fingerprint. "$0" is the
  # installed path, so a caller can tell whether the script it just wrote is the
  # one that ran.
  SELF_MD5=$(w25_md5 "$0")
  echo "GATE_VERSION=$W25_GATE_VERSION"
  echo "SCRIPT_MD5=$SELF_MD5"
  exit 0
fi
# W25_STAND_DOWN=1: revert to stock and exit. Deliberately a mode of its own
# rather than "just run the script and let it refuse": a mode cannot accidentally
# APPLY if the gate's inputs changed between the caller's preflight and this call.
if [ "${W25_STAND_DOWN:-0}" = 1 ]; then
  if w25_reg_is_ours; then WAS_OURS=1; else WAS_OURS=0; fi
  w25_stand_down
  w25_log "stand-down requested by the installer (a registry of ours was live: $WAS_OURS)"
  echo "STOOD_DOWN=$WAS_OURS"
  exit 0
fi
# --- G) self-heal / self-unlink, evaluated FIRST -----------------------------
# TWO DIFFERENT FAULTS, TWO DIFFERENT ANSWERS. Deleting is only ever right when
# nobody owns this install any more; a merely incomplete install is recoverable,
# and destroying it would be worse than the hazard the guard exists for.
#
#   UNOWNED  the app dir is gone from BOTH install trees AND there is no CLI
#            marker -> nobody can manage these system-wide overrides any more.
#            Full heal: drop the binds, put a clean stock registry back, remove
#            our state, unlink ourselves.
#   INCOMPLETE (handled in the next block, not here) the install is still owned
#            but a CORE object is missing -> bind nothing, delete nothing, keep
#            the hook, and repair the registry if a stale one of ours is live.
#
# The hook stays a symlink to THIS file under /var/lib/webosbrew/dts25 rather
# than into the app dir: a symlink into a removed app dir is dangling and
# executes nothing, so it could never heal anything.
APPDIR_DEV=/media/developer/apps/usr/palm/applications/io.github.josippapez.dtsenabler
APPDIR_SYS=/usr/palm/applications/io.github.josippapez.dtsenabler
CLI_MARKER=/var/lib/webosbrew/dts25/.cli-install
w25_unowned() {
  HEAL_WHY=
  if [ ! -d "$APPDIR_DEV" ] && [ ! -d "$APPDIR_SYS" ] && [ ! -f "$CLI_MARKER" ]; then
    HEAL_WHY="the DTS Enabler app is gone and this was not a CLI install"
    return 0
  fi
  return 1
}
# ORDER IS THE WHOLE POINT HERE: regenerate LG's registry FIRST and delete our
# plugins only once that has actually succeeded.
#
# Deleting first would mean that a regen which times out -- a cold-cache full
# plugin scan, at boot, on the busiest moment the box has -- leaves the live
# registry naming plugins that no longer exist AND removes the hook that could
# retry. That is the stale-registry state that broke other apps' audio on a real
# C5 (2026-07-23), and LG's stack does not recover from it by itself.
#
# So on failure: keep the binds dropped, keep the state dirs, keep the hook, and
# let the next boot try again. A heal that retries forever is strictly better
# than one that breaks audio once. Unconditional regen (not w25_stand_down's
# conditional one) because this path removes our plugins outright, so any
# registry naming them must be replaced whether or not it looks like ours.
w25_self_heal() {
  w25_log "SELF-HEAL: $HEAL_WHY -- reverting every override"
  w25_drop_binds
  if ! w25_stock_registry; then
    w25_log "SELF-HEAL DEFERRED: LG's registry could not be rebuilt, so our plugins and the boot hook are KEPT (deleting them now would leave the live registry pointing at missing plugins). Retrying at the next boot."
    toast "DTS Enabler cleanup deferred: the TV was too busy to rebuild its plugin list. It will finish at the next restart."
    return 1
  fi
  for d in /var/lib/webosbrew/dts25 /var/lib/webosbrew/truehd /var/lib/webosbrew/demux25; do
    [ -d "$d" ] && rm -rf "$d"
  done
  rm -f "$HOOK"
  toast "DTS Enabler is gone: DTS/TrueHD overrides reverted and the boot hook removed."
  return 0
}
# One compound command on purpose: the shell has finished parsing the whole
# `if` (and both function bodies) before w25_self_heal deletes the directory
# this very script is running from, so there is nothing left to read afterwards.
if w25_unowned; then
  if w25_self_heal; then echo "HEALED=$HEAL_WHY"; else echo "HEAL_DEFERRED=$HEAL_WHY"; fi
  exit 0
fi
# --- G2) owned, but the CORE payload is incomplete ---------------------------
# Recoverable, so nothing is deleted and the hook stays: re-opening the app or
# re-running install.sh restages the payload and the next boot applies normally.
# What DOES need repairing is the media registry -- it is committed with `cp -f`,
# so a registry we wrote earlier survives independently of our files, and if it
# is still live while our libraries are gone it names plugins that no longer
# exist (that is the failure that broke media-pipeline app audio on a real C5,
# 2026-07-23). Only the demuxers are optional; missing those is a normal
# MKV-only install, not a fault.
if ! w25_core_staged; then
  w25_log "REFUSED: core payload incomplete (dtsdec present=$([ -f "$MYDTS" ] && echo 1 || echo 0), libav present=$([ -f "$MYLIBAV" ] && echo 1 || echo 0)); binding nothing, install left intact"
  w25_stand_down
  toast "DTS Enabler: the installed files are incomplete, so nothing was applied. Re-open DTS Enabler (or re-run install.sh) to repair."
  echo "REFUSED=payload"
  echo "REASON=the staged DTS/TrueHD core payload is incomplete; nothing was bound and nothing was removed"
  exit 0
fi
# --- measure the pristine stock fingerprints (read-only) --------------------
w25_measure
# --- GATE 0) firmware-update / ABI guard ------------------------------------
if ! w25_gst_ok; then
  w25_log "ABORT: GStreamer '$GST_MM_NOW' != expected $EXPECT_GST (firmware update?); standing down"
  # The likeliest refusal in the wild, and the one where leaving a registry of
  # ours live would be worst: our plugins may not even exist any more after a
  # major firmware change. w25_stand_down handles that -- it rebuilds from LG's
  # plugin directories only.
  w25_stand_down
  toast "DTS/TrueHD paused: TV firmware changed (GStreamer $GST_MM_NOW). Re-open DTS Enabler to update."
  echo "REFUSED=drift"
  echo "REASON=GStreamer $GST_MM_NOW is not the $EXPECT_GST this payload was built for; a firmware update changed the plugin ABI"
  exit 0
fi
# --- GATE 1) identity -------------------------------------------------------
w25_verdict
case "$VERDICT" in
  verified|forced) w25_log "gate: $VERDICT -- $REASON${LABEL:+ [$LABEL]}" ;;
  drift)
    w25_log "REFUSED: drift -- $REASON"
    w25_stand_down
    toast "DTS/TrueHD stopped: this TV changed since it was last verified, so nothing was applied. Uninstall then Enable in DTS Enabler to opt in again."
    echo "REFUSED=drift"
    echo "REASON=$REASON"
    exit 0 ;;
  *)
    w25_log "REFUSED: $VERDICT -- $REASON"
    w25_stand_down
    toast "DTS Enabler: this TV is not on the verified list, so nothing was changed. Open DTS Enabler to try anyway."
    echo "REFUSED=$VERDICT"
    echo "REASON=$REASON"
    exit 0 ;;
esac
# --- GATE 2a) loader resolution ---------------------------------------------
if ! w25_loader_ok; then
  w25_log "REFUSED: loader -- $LOADER_MISS"
  w25_stand_down
  toast "DTS Enabler: the payload cannot load on this TV ($LOADER_MISS); nothing was changed."
  echo "REFUSED=loader"
  echo "REASON=$LOADER_MISS"
  exit 0
fi
# --- record the pristine fingerprints before anything of ours is bound ------
case "$VERDICT" in
  verified) FP_FORCED=$(w25_fp_get forced); [ -n "$FP_FORCED" ] || FP_FORCED=0; w25_fp_write 1 "$FP_FORCED" ;;
  *)        w25_fp_write 0 1 ;;
esac
# --- APPLY 1) codec-capability override (adds TRUEHD/MLP so umediaserver allocates a decoder resource)
[ -f "$MYCFG" ] && ! grep -q " $CFG " /proc/mounts 2>/dev/null && mount -n --bind "$MYCFG" "$CFG" 2>>$LOG
# --- APPLY 2) replace LG.s truehd-less libav with ours (has avdec_truehd/avdec_mlp)
[ -f "$MYLIBAV" ] && ! grep -q " $LGLIBAV " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYLIBAV" "$LGLIBAV" 2>>$LOG
# --- APPLY 2b) gstcool.conf: give avdec_truehd a high SW rank so LG autoplugs it (not the HW path)
[ -f "$MYGC" ] && ! grep -q " $GC " /proc/mounts 2>/dev/null && mount -n --bind "$MYGC" "$GC" 2>>$LOG
# --- APPLY 2c) container demuxers with DTS re-enabled (mp4/ts/m2ts DTS -> audio/x-dts).
#         Patched isomp4/mpegtsdemux default dts_support=TRUE. Bound BEFORE the
#         regen below so the registry picks them up at their normal path.
[ -f "$MYISO" ] && ! grep -q " $LGISO " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYISO" "$LGISO" 2>>$LOG
[ -f "$MYTSD" ] && ! grep -q " $LGTSD " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYTSD" "$LGTSD" 2>>$LOG
# --- APPLY 3) regenerate the media registry (fresh) with dtsdec + our libav, then write it to the media path.
#    Bounded by `timeout` and scanned in-process (GST_REGISTRY_FORK=no) so a hang can't trip HBC
#    failsafe and no gst-plugin-scanner child lingers past the timeout.
REG_TMP=/tmp/gst_dts_reg.bin
rm -f "$REG_TMP"
LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs \
GST_REGISTRY_1_0="$REG_TMP" \
GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \
GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 30 /usr/bin/gst-inspect-1.0 >/dev/null 2>>$LOG
# --- GATE 2b) post-bind pipeline proof, then commit: overwrite the media
#         registry only if the regen survived the binds with all five elements
#         intact. Otherwise drop our own binds -- that is what turns "the
#         override did not match" from a broken media pipeline into a no-op.
if w25_reg_has_all "$REG_TMP"; then
  cp -f "$REG_TMP" "$REG" 2>>$LOG && echo "registry updated (dtsdec+avdec_truehd+qtdemux+tsdemux+matroskademux)" >>$LOG
  echo "APPLIED=$VERDICT"
else
  w25_log "REFUSED: regen is missing $REG_MISS; not committing it, and standing down"
  w25_stand_down
  toast "DTS Enabler: the plugin registry did not survive the override ($REG_MISS missing), so nothing was changed."
  echo "REFUSED=pipeline"
  echo "REASON=the regenerated plugin registry is missing $REG_MISS"
fi
exit 0
