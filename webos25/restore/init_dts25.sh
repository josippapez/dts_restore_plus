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
#   W25_PT=start|stop
#                 only start or stop the DTS passthrough sound-output watcher
#                 (the app's toggle), then exit.
#   W25_APPDTS=on|off
#                 only set or restore tv.model.edidType live (the app's "Show
#                 DTS tracks in apps" toggle), print EDIDTYPE=<value> and exit.
#   FORCE=1       explicit opt-in on a TV that is not in the verified table.
#
# Always exits 0: a non-zero webosbrew init script trips the failsafe that
# disables ALL root customisations on the next boot. Every refusal path binds
# nothing -- the overrides are system-wide (they propagate into every app jail),
# so "not sure" must mean "do not touch this TV". The container demuxers are
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
#   4  fixes a regression in 3: the near-match early-return skipped the FORCE=1
#      handling, so "Try anyway" reported the same refusal as Enable -- the message
#      told the user to opt in and then ignored them. Reported by a G5 owner. Also
#      adds the G5 row, which is now owner-verified on firmware 33.30.97.
#   5  adds adecswitch (GstBin, compiled rank 320) to the CORE payload: it fronts
#      Dolby (AC-3/E-AC-3) caps ahead of decproxy (rank 300) so a decodebin3 stream
#      switch on the same bin does not tear down and rebuild the HW decoder mid-
#      switch, while still handing Dolby off to decproxy for the actual decode.
#      Required like dtsdec/libav (w25_core_staged/w25_loader_ok) and added to the
#      post-bind registry proof (w25_reg_has_all), which now checks SIX elements.
#      A `[sw_decoder] adecswitch=320` line in the generated gstcool.conf remains a
#      config-level kill switch (rank 0 reverts to stock decodebin3 behaviour).
#   27 shadows libgstmatroska.so too (optional demuxer, Dolby Vision profile 7):
#      bound in APPLY 2c, unbound by w25_drop_binds, fingerprinted in stock.fp and
#      drift-checked like the /etc keys, so an older stock.fp without it is not drift.
#   41 opt-in DTS-HD/TrueHD over eARC Pass Through; LG's mixer is only read.
#   42 self-heal, stand-down and the boot apply continue when passthrough stop fails.
#   45 libgstmpegpsdemux.so (optional, DTS in .vob / MPEG-PS) is bound in APPLY 2c,
#      unbound by w25_drop_binds and counted by w25_plugin_binds_present. It is NOT
#      fingerprinted (no stock.fp key, no drift check), and the registry proof
#      requires mpegpsdemux only when our bind is live.
#   46 verified set for the G2 on firmware 33.22.95.
#   47 the app-DTS opt-in sets tv.model.edidType live through configd's setConfigs
#      (no bind, no configd restart); W25_APPDTS=on|off is the app's toggle.
#   48 with the app-DTS opt-in on, eim preloads libedidaudio.so so the HDMI-input
#      EDIDs keep LG's stock audio list (no DTS).
#   49 ...but not on a TV whose factory edidType already lists dts.
#   50 DTS-HD/TrueHD over eARC allowed on the G3's LG sink build (33.31.68).
#   51 verified stock plugin set for the G3 on firmware 33.31.68.
W25_GATE_VERSION=51
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
LGMKV=/usr/lib/gstreamer-1.0/libgstmatroska.so
LGPSD=/usr/lib/gstreamer-1.0/libgstmpegpsdemux.so
# CORE payload -- without any of these there is no DTS, no TrueHD, and no
# switching bin, so a missing one means "do not bind anything".
MYDTS=/var/lib/webosbrew/dts25/libgstdtsdec.so
MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so
# adecswitch (track A): a GstBin (compiled rank 320) fronting Dolby/TrueHD/DTS
# caps so decodebin3 keeps one bin across a same-family stream switch instead of
# tearing the HW decoder down mid-switch, while still handing Dolby off to
# decproxy (rank 300) for the actual decode. Staged alongside dtsdec -- the
# plugin path at APPLY 3 below already includes /var/lib/webosbrew/dts25.
MYSWITCH=/var/lib/webosbrew/dts25/libgstadecswitch.so
# OPTIONAL payload: the patched container demuxers. Absent, DTS still works in
# MKV -- which is exactly how this shipped before the gate existed -- so each is
# bound only when staged and is never a reason to refuse or to delete anything.
MYISO=/var/lib/webosbrew/demux25/libgstisomp4.so
MYTSD=/var/lib/webosbrew/demux25/libgstmpegtsdemux.so
MYMKV=/var/lib/webosbrew/demux25/libgstmatroska.so
MYPSD=/var/lib/webosbrew/demux25/libgstmpegpsdemux.so
MYCFG=/var/lib/webosbrew/truehd/codec_capability.json
MYGC=/var/lib/webosbrew/truehd/gstcool.conf
MYLIBS=/var/lib/webosbrew/truehd/libs:/var/lib/webosbrew/dts25/libs
# APP-DTS opt-in: makes apps that gate on the TV's declared capability (Stremio
# and Kodi both read tv.model.edidType) stop hiding DTS tracks. Off unless the
# marker exists -- see "App DTS opt-in" below.
APPDTS_FLAG=/var/lib/webosbrew/dts25/appdts.enabled
# Outcome of every apply attempt. Kept out of /tmp: the boot log there is gone
# by the next boot, which left no record of the boots where this did not apply.
# appdts.last is one line, "<epoch> <on|skipped|failed> <message>", for the app.
APPDTS_LAST=/var/lib/webosbrew/dts25/appdts.last
APPDTS_HIST=/var/lib/webosbrew/dts25/appdts.log
# NOTE the /tmp path, not /var/run: /var/run is a symlink to /tmp/var/run, so a
# bind made here is recorded in /proc/mounts under the RESOLVED path. Writing
# /var/run/... would make every "is it already mounted" guard miss -- stacking a
# fresh mount on every run and never unmounting on Disable.
LLS=/tmp/var/run/tvconfig/lls/factorydb.json
MYLLS=/var/lib/webosbrew/dts25/factorydb.override.json
# configd keeps values set through setConfigs here and applies them again at boot.
CFG_USER=/var/preferences/configd_factory_db.json
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
  for t in "$CFG" "$GC" "$LGLIBAV" "$LGISO" "$LGTSD" "$LGMKV" "$LGPSD" "$REG" "$LLS"; do
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
w25_pt_stop() {
  PT_RUN=/run/dts25
  PT_OUT=$PT_RUN/sound-output
  PT_PID=$PT_RUN/watcher.pid
  # Withdraw permission first; a live HD stream then falls back by itself.
  mkdir -p "$PT_RUN" || return 1
  : > "$PT_RUN/stopping" || return 1
  rm -f "$PT_OUT" || return 1
  P=$(cat "$PT_PID" 2>/dev/null)
  if [ -n "$P" ] && [ -d "/proc/$P" ]; then
    for child in /proc/[0-9]*; do
      [ "$(cut -d' ' -f4 "$child/stat" 2>/dev/null)" = "$P" ] && kill "${child#/proc/}" 2>/dev/null
    done
    kill "$P" 2>/dev/null
    attempts=0
    while [ "$(cat "$PT_PID" 2>/dev/null)" = "$P" ] && [ -r "/proc/$P/stat" ] && [ "$(cut -d' ' -f3 "/proc/$P/stat")" != Z ] && [ "$attempts" -lt 10 ]; do
      sleep 1
      attempts=$((attempts + 1))
    done
    if [ "$(cat "$PT_PID" 2>/dev/null)" = "$P" ] && [ -r "/proc/$P/stat" ] && [ "$(cut -d' ' -f3 "/proc/$P/stat")" != Z ]; then
      echo 'ERROR=The passthrough watcher did not stop; settings were kept.'
      return 1
    fi
  fi
  rm -f "$PT_OUT" "$PT_PID"
}
# Recovery paths must not stall on a watcher that will not stop.
w25_pt_stop_soft() {
  PT_STOP_OUT=$(w25_pt_stop) && return 0
  PT_STOP_OUT=$(printf '%s\n' "$PT_STOP_OUT" | sed -n 's/^ERROR=//p' | head -n1)
  w25_log "WARN passthrough stop failed; continuing: $PT_STOP_OUT"
  echo "WARN_PT_STOP=${PT_STOP_OUT:-passthrough did not stop cleanly}"
}
w25_stand_down() {
  w25_pt_stop_soft
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
# libgstmatroska.so (shadowed since gate 27, for Dolby Vision profile 7) is
# fingerprinted too, but it is NOT a verified-sets table key: the table predates
# it and its rows carry no matroska hash. Its drift check therefore goes through
# w25_fp_differs like the /etc keys, so a stock.fp written before gate 27 is not
# drift; protection engages from the first unbound measurement that records it.
w25_measure() {
  B_LIBAV=$(w25_bound "$LGLIBAV")
  B_ISOMP4=$(w25_bound "$LGISO")
  B_MPEGTS=$(w25_bound "$LGTSD")
  B_MKV=$(w25_bound "$LGMKV")
  B_CFG=$(w25_bound "$CFG")
  B_GC=$(w25_bound "$GC")
  S_LIBAV=
  S_ISOMP4=
  S_MPEGTS=
  S_MKV=
  S_CFG=
  S_GC=
  [ "$B_LIBAV" = 0 ] && S_LIBAV=$(w25_md5 "$LGLIBAV")
  [ "$B_ISOMP4" = 0 ] && S_ISOMP4=$(w25_md5 "$LGISO")
  [ "$B_MPEGTS" = 0 ] && S_MPEGTS=$(w25_md5 "$LGTSD")
  [ "$B_MKV" = 0 ] && S_MKV=$(w25_md5 "$LGMKV")
  [ "$B_CFG" = 0 ] && S_CFG=$(w25_md5 "$CFG")
  [ "$B_GC" = 0 ] && S_GC=$(w25_md5 "$GC")
  M_LIBAV=$S_LIBAV
  M_ISOMP4=$S_ISOMP4
  M_MPEGTS=$S_MPEGTS
  M_MKV=$S_MKV
  M_CFG=$S_CFG
  M_GC=$S_GC
  [ -n "$M_LIBAV" ] || M_LIBAV=$(w25_fp_get libgstlibav)
  [ -n "$M_ISOMP4" ] || M_ISOMP4=$(w25_fp_get libgstisomp4)
  [ -n "$M_MPEGTS" ] || M_MPEGTS=$(w25_fp_get libgstmpegtsdemux)
  [ -n "$M_MKV" ] || M_MKV=$(w25_fp_get libgstmatroska)
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
  FP_MKV=$(w25_fp_get libgstmatroska)
  FP_CFG=$(w25_fp_get device_codec_capability_config)
  FP_GC=$(w25_fp_get gstcool)
  # The three plugin hashes compare strictly (they have always been recorded).
  # The two /etc hashes and libgstmatroska go through w25_fp_differs so an older
  # stock.fp that never recorded them does not read as drift.
  DRIFT_WHAT=
  DRIFT_PLUGINS=0
  DRIFT_CONFIG=0
  [ "$FP_AV" != "$M_LIBAV" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstlibav.so"; DRIFT_PLUGINS=1; }
  [ "$FP_ISO" != "$M_ISOMP4" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstisomp4.so"; DRIFT_PLUGINS=1; }
  [ "$FP_TSD" != "$M_MPEGTS" ] && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstmpegtsdemux.so"; DRIFT_PLUGINS=1; }
  w25_fp_differs "$FP_MKV" "$M_MKV" && { DRIFT_WHAT="${DRIFT_WHAT:+$DRIFT_WHAT }libgstmatroska.so"; DRIFT_PLUGINS=1; }
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
      # Rows can share hashes -- a C5 and a G5 are byte-identical on all three -- so
      # do NOT stop at the first hash match. Keep scanning for a row whose product
      # glob also matches, and only remember the hash-only match as a fallback.
      # Breaking early here made the first matching row win, so a G5 could never
      # reach its own row and was reported untested despite being listed.
      case "$PRODUCT_NOW" in
        $t_product)
          LABEL=$t_label
          VERDICT=verified
          REASON="stock plugin fingerprints match a TV this payload was verified on"
          break
          ;;
        *)
          if [ "$VERDICT" != unverified ] || [ "$CANFORCE" != 1 ]; then
            LABEL=$t_label
            VERDICT=unverified
            CANFORCE=1
            REASON="this TV's stock plugins are byte-identical to $t_label, so the payload is very likely compatible -- but THIS model ($PRODUCT_NOW) has never been tested on hardware, so it is not reported as verified. Opt in explicitly to try it, and please report the result so it can be added properly"
          fi
          ;;
      esac
    fi
  done <<'W25_SETS'
# gst_mm|libgstlibav|libgstisomp4|libgstmpegtsdemux|product_id_glob|label
# product_id_glob must be a shell glob matched against nyx-cmd product_id. Only add
# a row with a real product glob after DTS and TrueHD have actually played on that
# model -- a byte-identical artifact set is NOT hardware verification.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|57fe57060774f248c05af5a411fc9a8f|9b84a95cf29bc025553c7dee829b7cc1|OLED*C5*|LG C5 OLED77C51LA (webOS 10.3.1, GStreamer 1.24.0)
# G5: owner-reported working on firmware 33.30.97 -- DTS played in MKV, MP4 and TS
# after enabling. Its 33.31.68 plugin hashes are byte-identical to the C5 row above,
# so the same row now covers both models. TrueHD on the G5 was reported as playing
# with interruptions; DTS was clean.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|57fe57060774f248c05af5a411fc9a8f|9b84a95cf29bc025553c7dee829b7cc1|OLED*G5*|LG G5 OLED77G55LW (webOS 10.3.1, GStreamer 1.24.0)
# G2 on webOS 25: a 2022 o22 panel upgraded to 10.3.1, so it lands on this same
# profile. Its libav is byte-identical to the C5/G5 build while BOTH demuxers
# differ, which is why it needs its own row rather than the one above. The three
# md5s were read from the 33.31.68 o22 image and match two independent owner
# reports. DTS played in MP4, TS and M2TS; TrueHD played from a TrueHD-only remux
# without stutter. The C2 shares this exact binary set but has no TrueHD report
# yet, so it stays on the opt-in path until one arrives.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|cf4d9bb9e3c3ad83f1a75a399d2f0b93|772fb3b29e224423035eec9e93615b23|OLED*G2*|LG G2 OLED77G26LA/OLED77G29LA (webOS 10.3.1, GStreamer 1.24.0)
# C2: the same o22 binary set as the G2 row above, so it was already matching by
# hashes only and forcing on every enable. TrueHD confirmed on an OLED55C21LA
# against two TrueHD-only Matroska samples -- single audio track, no AC-3
# alongside, so no compatibility track could decode in its place. DTS came from
# all three self-test containers.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|cf4d9bb9e3c3ad83f1a75a399d2f0b93|772fb3b29e224423035eec9e93615b23|OLED*C2*|LG C2 OLED55C21LA (webOS 10.3.1, GStreamer 1.24.0)
# CS: the same o22 binary set as the C2/G2 rows (firmware 33.31.68). An OLED55CS6LA
# passed all four self-tests and played the DTS and TrueHD test files with sound.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|cf4d9bb9e3c3ad83f1a75a399d2f0b93|772fb3b29e224423035eec9e93615b23|OLED*CS*|LG CS OLED55CS6LA (webOS 10.3.1, GStreamer 1.24.0)
# G3 on firmware 33.31.68: the same stock plugin hashes as the C2/G2 rows above.
# The owner of an OLED77G36LA passed all four self-tests and confirmed audio
# playback with the bundled DTS and TrueHD test files.
1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|cf4d9bb9e3c3ad83f1a75a399d2f0b93|772fb3b29e224423035eec9e93615b23|OLED*G3*|LG G3 OLED77G36LA (webOS 10.3.1, GStreamer 1.24.0)
# G2 on webOS 10.2.2 (firmware 33.22.95): its own builds of all three plugins
# (owner report). An OLED55G26LA played DTS-HD MA 5.1 and TrueHD 7.1 decoded and
# as bitstream to a Denon AVR-S770H after opting in.
1.24|ff330d3578ce83da402d686f4b5ad5c4|09d6385016be02c4552e719f3a01ef52|b0380d3c66d382c15504b81b09ff94bd|OLED*G2*|LG G2 OLED55G26LA (webOS 10.2.2, GStreamer 1.24.0)
# UR7500: owner-reported DTS and TrueHD playing (TrueHD with Plex direct play forced).
# Hashes read from the 33.31.61 K8LPN/W23P image: the demuxers match the o22 set,
# libav is its own build.
1.24|73e01f3d1ed9d9feb95f1260bfdda2f9|cf4d9bb9e3c3ad83f1a75a399d2f0b93|772fb3b29e224423035eec9e93615b23|*UR75*|LG UR7500 43UR7500PSC (webOS 10.3.1, GStreamer 1.24.0)
# G5 on webOS 11.2 (firmware 43.21.7x, GStreamer 1.24.10): new builds of all three
# plugins. An OLED77G54LW on 43.21.77 played DTS in Plex and TrueHD 7.1 through a
# soundbar after opting in, and passed all four self-tests. An OLED55G5WUA on 43.21.71
# reports the same three md5s.
1.24|cb40b8b3bcda13d2c5a662762f8ff1b2|8fa3f7b96a045765a28a62011ff02a66|fc717f70284deebecd4a1ea1254617e7|OLED*G5*|LG G5 OLED77G54LW (webOS 11.2.0, GStreamer 1.24.10)
# G5 on webOS 10.3.0 (firmware 33.31.28): its own builds of all three plugins
# (owner report). An OLED65G56LS passed the self-tests after opting in, and played
# DTS-HD MA and TrueHD in Plex with Direct Play forced.
1.24|3c8efc3a8af078f64d18fd2ce4f3ef71|18172fc834da68c7f31f36e46afbc5bd|2c7a44bc37a01a9f22adf93444467655|OLED*G5*|LG G5 OLED65G56LS (webOS 10.3.0, GStreamer 1.24.0)
# C5 on webOS 11.2: byte-identical to the G5 row above (read from the 43.21.77 image).
# An OLED65C5PUA on 43.21.77 played DTS and TrueHD audibly over eARC.
1.24|cb40b8b3bcda13d2c5a662762f8ff1b2|8fa3f7b96a045765a28a62011ff02a66|fc717f70284deebecd4a1ea1254617e7|OLED*C5*|LG C5 OLED65C5PUA (webOS 11.2.0, GStreamer 1.24.10)
# G2 on webOS 11.2 (43.21.74): same libav as the rows above, its own demuxer builds
# (read from the 43.21.74 image). An OLED77G23LA played a DTS:X MKV and a TrueHD 5.1
# MKV after opting in, and passed the self-tests.
1.24|cb40b8b3bcda13d2c5a662762f8ff1b2|d4e37523bcd2b00c16fc8792d75e1ae4|66ac51a0c81efbb8a705d0c60ab62cfa|OLED*G2*|LG G2 OLED77G23LA (webOS 11.2.0, GStreamer 1.24.10)
# UA73 (LCD, W25P) on webOS 10.3.2 (firmware 33.31.80): its own libav and isomp4
# builds, the TS demuxer matches the o22 set (owner report). A 55UA73006LA passed
# the self-tests after opting in, and played a DTS-HD MA M2TS and a TrueHD MKV.
1.24|3697edfd5fbb50309c54d28f76b6fc6b|d79a311c3783a7498576b7a88c0c8a97|772fb3b29e224423035eec9e93615b23|*UA73*|LG UA73 55UA73006LA (webOS 10.3.2, GStreamer 1.24.0)
W25_SETS
  [ "$VERDICT" = verified ] && return 0
  # A hash match on an untested model leaves VERDICT=unverified + CANFORCE=1 and must
  # still fall through to the opt-in handling below. Returning here instead made
  # FORCE=1 unreachable, so "Try anyway" reported the same refusal as Enable -- the
  # message told the user to opt in and then ignored them. Keep the reason/label from
  # the table (they are more specific than the generic no-match text) and only skip
  # the closing generic assignment.
  TABLE_NEAR_MATCH=0
  [ "$VERDICT" = unverified ] && [ "$CANFORCE" = 1 ] && TABLE_NEAR_MATCH=1
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
  [ "$TABLE_NEAR_MATCH" = 1 ] || REASON="this TV's stock GStreamer plugins match no TV this payload was verified on"
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
w25_core_staged() { [ -f "$MYDTS" ] && [ -f "$MYLIBAV" ] && [ -f "$MYSWITCH" ]; }
w25_loader_ok() {
  LOADER_MISS=
  LOADER_STAGED=1
  LD_SO=$(w25_loader)
  if [ -z "${LD_SO:-}" ]; then LOADER_MISS="no dynamic loader found on this TV"; return 1; fi
  if ! w25_core_staged; then
    LOADER_STAGED=0
    LOADER_MISS="the core payload is not staged ($MYDTS / $MYLIBAV / $MYSWITCH)"
    return 1
  fi
  for so in "$MYDTS" "$MYLIBAV" "$MYSWITCH" "$MYISO" "$MYTSD" "$MYMKV" "$MYPSD"; do
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
# ALL SIX elements the DTS/TrueHD/switch path needs: our three decoders (dtsdec,
# avdec_truehd, adecswitch) AND the three demuxers we shadow (all three are
# present in the media registry today).
# A missing demuxer means our override produced a plugin the registry cannot
# use, i.e. a broken mp4/ts/mkv pipeline -- so the caller refuses the commit and
# drops the binds, which turns "the override didn't match" into a plain no-op.
w25_reg_has_all() {
  REG_MISS=
  for e in dtsdec avdec_truehd adecswitch qtdemux tsdemux matroskademux; do
    GST_REGISTRY_1_0="$1" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 "$e" >/dev/null 2>&1 || { REG_MISS=$e; return 1; }
  done
  # The .vob demuxer is optional: held to the same proof only while our bind is live.
  if grep -q " $LGPSD " /proc/mounts 2>/dev/null; then
    GST_REGISTRY_1_0="$1" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 mpegpsdemux >/dev/null 2>&1 || { REG_MISS=mpegpsdemux; return 1; }
  fi
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
  grep -q " $LGMKV " /proc/mounts 2>/dev/null && return 0
  grep -q " $LGPSD " /proc/mounts 2>/dev/null && return 0
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
    echo "libgstmatroska=$M_MKV"
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
  echo "MD5_LIBGSTMATROSKA=$S_MKV"
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
# --- DTS passthrough opt-in: keep the sound output where dtsdec can read it.
# dtsdec sends the DTS core to an ARC/eARC receiver as IEC 61937 only while
# PT_OUT says so ("<watcher pid> <soundOutput> <soundOutputDigital>") and that
# pid is alive; on speakers the bitstream would be loud noise, so a missing
# file or a dead watcher means decode. The pid is read from /proc/self/stat
# because $$ in a subshell is the parent's, and the parent (this hook) exits.
# /run, not /tmp: only root can write there, so nothing else can plant a
# symlink for these root writes or a file that claims a receiver is attached.
PT_FLAG=/var/lib/webosbrew/dts25/passthrough
PT_RUN=/run/dts25
PT_OUT=$PT_RUN/sound-output
PT_PID=$PT_RUN/watcher.pid
w25_pt_watch() {
  read -r WPID _ < /proc/self/stat
  echo "$WPID" > "$PT_PID"
  trap 'rm -f "$PT_OUT" "$PT_PID"' EXIT
  trap 'exit 0' INT TERM HUP
  while [ -f "$PT_FLAG" ] && [ ! -e "$PT_RUN/stopping" ]; do
    luna-send -i luna://com.webos.settingsservice/getSystemSettings \
      '{"category":"sound","keys":["soundOutput","soundOutputDigital"],"subscribe":true}' \
      </dev/null 2>/dev/null | {
      out=
      digital=
      while read -r line; do
        [ -f "$PT_FLAG" ] && [ ! -e "$PT_RUN/stopping" ] || break
        v=$(echo "$line" | sed -n 's/.*"soundOutput" *: *"\([^"]*\)".*/\1/p')
        [ -n "$v" ] && out=$v
        v=$(echo "$line" | sed -n 's/.*"soundOutputDigital" *: *"\([^"]*\)".*/\1/p')
        [ -n "$v" ] && digital=$v
        [ -n "$out" ] && [ -n "$digital" ] || continue
        echo "$WPID $out $digital" > "$PT_OUT.tmp" && mv -f "$PT_OUT.tmp" "$PT_OUT"
        [ ! -e "$PT_RUN/stopping" ] || { rm -f "$PT_OUT"; break; }
      done
    }
    # the subscription ended: decode until it is back
    rm -f "$PT_OUT"
    sleep 5
  done
  rm -f "$PT_OUT" "$PT_PID"
}
w25_pt_sink_verified() {
  case "$1" in
    90dba3ef32704ce4096bf1e6045d2879d0c2192d507bc04d1e8e98437f420a43) return 0 ;;
    # G2 OLED55G26LA, firmware 33.22.95 (webOS 10.2.2), Denon AVR-S770H: tester
    # confirmed DTS-HD and TrueHD 7.1 bitstream on the receiver (discussion #17).
    792caa7e048a8550ec3c67e0fd4c9d94684d0633e0ff250a2bcbd0e29d538fe9) return 0 ;;
    # G3 OLED77G36LA, firmware 33.31.68 (webOS 10.3.1), Marantz receiver: the
    # tester's route log shows the carrier passing on every fresh play and the
    # tester reported TrueHD Atmos on the receiver (Discord, 2026-11-10).
    21ad477c010fc6cb2e6989ff263892981b9d26d28cd83d1736edc3331bf80745) return 0 ;;
  esac
  return 1
}
w25_pt_start() {
  [ -f "$PT_FLAG" ] || return 0
  mkdir -p "$PT_RUN"
  # dtsdec reads this marker instead of hashing LG's sink on every stream.
  rm -f "$PT_RUN/sink-ok"
  SINK=/usr/lib/gstreamer-1.0/libgstlgaudiosink.so
  SINK_SHA=$(sha256sum "$SINK" 2>/dev/null || busybox sha256sum "$SINK" 2>/dev/null)
  if w25_pt_sink_verified "${SINK_SHA%% *}"; then
    : > "$PT_RUN/sink-ok"
  else
    w25_log "HD refused: untested LG sink build (${SINK_SHA%% *}); DTS core/decode remains available"
  fi
  P=$(cat "$PT_PID" 2>/dev/null)
  [ -n "$P" ] && [ -d "/proc/$P" ] && return 0
  rm -f "$PT_RUN/stopping"
  ( w25_pt_watch ) </dev/null >/dev/null 2>&1 &
}
# --- App DTS opt-in (tv.model.edidType) -----------------------------------------
# Stremio and Kodi read tv.model.edidType and hide DTS tracks unless it mentions
# dts. It is a separate opt-in from Enable because edidType also feeds the EDID
# the TV advertises over eARC and gates DTS on its HDMI inputs.
#
# configd's setConfigs changes the live value. Root may call it (the app's own
# service identity may not) and nothing restarts: measured on a C5, configd,
# audiooutputd, arccontroller and audiod kept their pids. configd records the
# value as a user override in $CFG_USER and applies it again at every boot, so
# Off must put the factory value back and drop that override. Apps up to 2.7.64
# restarted configd instead, which could leave the TV without sound, volume or
# Home keys until the next restart.
#
# luna-send prints its reply only to a terminal; script(1) gives it one.
w25_luna() { script -q -e -c "luna-send -n 1 $1 '$2'" /dev/null 2>/dev/null; }
w25_edid_get() {
  w25_luna luna://com.webos.service.config/getConfigs '{"configNames":["tv.model.edidType"]}' |
    sed -n 's/.*"tv.model.edidType":"\([^"]*\)".*/\1/p' | head -n1
}
# Success is the value reading back, not the call's own returnValue.
w25_edid_set() {
  w25_luna luna://com.webos.service.config/setConfigs "{\"configs\":{\"tv.model.edidType\":\"$1\"}}" >/dev/null
  [ "$(w25_edid_get)" = "$1" ]
}
# The factory value, which Off restores.
w25_edid_stock() { grep -o '"edidType":"[^"]*"' "$LLS" 2>/dev/null | head -n1 | sed 's/.*:"//; s/"$//'; }
# Every outcome ends here, so none of them is silent: the boot log, a history
# that survives reboots (last 50 lines), the line the app shows, and a toast.
# $2 goes into the toast's JSON, so it must not contain a double quote.
w25_appdts_report() {
  w25_log "app-dts: $1: $2"
  echo "$(date +%s) $1 $2" > "$APPDTS_LAST" 2>/dev/null
  { tail -n 49 "$APPDTS_HIST" 2>/dev/null; echo "$(date '+%Y-%m-%d %H:%M:%S') $1 $2"; } > "$APPDTS_HIST.tmp" 2>/dev/null &&
    mv -f "$APPDTS_HIST.tmp" "$APPDTS_HIST" 2>/dev/null
  [ -n "${APPDTS_QUIET:-}" ] || toast "DTS Enabler: $2"
}

# eim builds every HDMI input's EDID from edidType too, so with the opt-in on,
# devices on those inputs were told the TV takes DTS, which it cannot play from
# HDMI (measured on a C5: a streamer sent DTS 5.1 and the TV stayed silent).
# libedidaudio.so makes eim build them from the edidType without "+dts". eim's
# unit reads $EIM_ENV, whose LD_PRELOAD replaces the unit's own, so LG's preload
# is listed again. The file persists, so eim has the hook from boot; changing it
# needs an eim restart, which restarts arccontroller too. Devices already plugged
# in re-read the EDID only on a replug or a TV restart.
EIM_ENV=/var/systemd/system/env/eim.env
EIM_UNIT=/etc/systemd/system/eim.service
MYEDID=/var/lib/webosbrew/dts25/libedidaudio.so
EDID_STATE=/run/dts25/edidaudio
w25_eim_hooked() { grep -q libedidaudio "/proc/$(pidof eim | cut -d' ' -f1)/maps" 2>/dev/null; }
w25_hdmi_hook_on() {
  [ -f "$MYEDID" ] || return 1
  if [ -f "$EIM_ENV" ] && ! grep -q libedidaudio "$EIM_ENV"; then
    w25_log "app-dts: $EIM_ENV is not ours, left alone"
    return 1
  fi
  LGPRE=$(sed -n 's/^Environment=LD_PRELOAD=\$LD_PRELOAD://p' "$EIM_UNIT" 2>/dev/null | head -n1)
  echo "LD_PRELOAD=$MYEDID${LGPRE:+:$LGPRE}" > "$EIM_ENV" || return 1
  if ! w25_eim_hooked; then
    rm -f "$EDID_STATE"
    systemctl restart eim
  fi
  # The hook writes its verdict the first time eim builds an EDID.
  i=0
  while [ ! -s "$EDID_STATE" ] && [ "$i" -lt 15 ]; do i=$((i + 1)); sleep 1; done
  w25_log "app-dts: HDMI EDID hook: $(cat "$EDID_STATE" 2>/dev/null || echo 'no verdict')"
  grep -q '^remap' "$EDID_STATE" 2>/dev/null
}
w25_hdmi_hook_off() {
  grep -q libedidaudio "$EIM_ENV" 2>/dev/null || return 0
  rm -f "$EIM_ENV"
  if w25_eim_hooked; then systemctl restart eim; fi
  return 0
}
# Appended to the "on" message when the HDMI inputs could not be kept DTS-free.
HDMI_NOTE=" HDMI devices are still told the TV takes DTS: if one goes silent, set its audio output to Dolby Digital Plus or PCM."

# The value to advertise for a stock edidType, or nothing when there is no safe one.
# Only strings LG's own code knows are used: libextinput-service lists TrueHD,
# TrueHD+dts and ac3+dts. A UR7500 reports ac3, not TrueHD.
w25_appdts_target() {
  case "$1" in
    TrueHD) echo "TrueHD+dts" ;;
    ac3) echo "ac3+dts" ;;
  esac
}
w25_appdts_apply() {
  [ -n "${W25_NO_APPDTS:-}" ] && return 0
  [ -f "$APPDTS_FLAG" ] || return 0
  CUR=$(w25_edid_get)
  case $CUR in
    *dts*)
      NOTE=
      # A factory value with dts means LG says this TV plays DTS from HDMI, so
      # its inputs keep listing it. Only DTS we added is kept off them.
      case $(w25_edid_stock) in
        *dts*) ;;
        *) w25_hdmi_hook_on || NOTE=$HDMI_NOTE ;;
      esac
      w25_appdts_report on "DTS tracks are on for apps.$NOTE"; return 0 ;;
  esac
  T=$(w25_appdts_target "$CUR")
  if [ -z "$T" ]; then
    w25_appdts_report failed "DTS tracks are off: this TV reports '${CUR:-nothing}', which has no known DTS form, so there was nothing to change."
    return 0
  fi
  # The hook goes in first, so the EDID eim rebuilds for "+dts" never lists DTS.
  NOTE=; w25_hdmi_hook_on || NOTE=$HDMI_NOTE
  if w25_edid_set "$T"; then
    w25_appdts_report on "DTS tracks are on for apps. Reopen Stremio or Kodi if it was already open.$NOTE"
  else
    w25_hdmi_hook_off
    w25_appdts_report failed "DTS tracks are off: the TV's settings service did not take the change."
  fi
}
w25_appdts_off() {
  # Apps up to 2.7.64 bound a patched copy over the factory file.
  w25_umount "$LLS"
  rm -f "$MYLLS" 2>/dev/null
  STOCK=$(w25_edid_stock)
  [ -n "$STOCK" ] || return 0
  [ "$(w25_edid_get)" = "$STOCK" ] || w25_edid_set "$STOCK" || return 0
  # Only once edidType is back, so the eim restart never builds a "+dts" EDID.
  w25_hdmi_hook_off
  # Setting it back leaves the factory value pinned as an override. Drop the file
  # when that is all it holds; configd reads it only at boot.
  [ "$(tr -d ' \t\n' < "$CFG_USER" 2>/dev/null)" = "{\"com.webos.service.config\":{\"usrDefined\":[\"tv.model.edidType\"]},\"tv.model\":{\"edidType\":\"$STOCK\"}}" ] &&
    rm -f "$CFG_USER"
  return 0
}
# W25_PT=start|stop: only start or stop the watcher, for the app's toggle, so
# turning passthrough on or off takes effect without a restart.
if [ -n "${W25_PT:-}" ]; then
  case $W25_PT in
    start) w25_pt_start ;;
    stop) w25_pt_stop || exit 1 ;;
  esac
  exit 0
fi
# W25_APPDTS=on|off: the app's "Show DTS tracks in apps" toggle, applied at once.
# On needs the marker the app has just written.
if [ -n "${W25_APPDTS:-}" ]; then
  APPDTS_QUIET=1
  case $W25_APPDTS in
    on) w25_appdts_apply ;;
    off) w25_appdts_off ;;
  esac
  echo "EDIDTYPE=$(w25_edid_get)"
  exit 0
fi
# W25_STAND_DOWN=1: revert to stock and exit. Deliberately a mode of its own
# rather than "just run the script and let it refuse": a mode cannot accidentally
# APPLY if the gate's inputs changed between the caller's preflight and this call.
if [ "${W25_STAND_DOWN:-0}" = 1 ]; then
  if w25_reg_is_ours; then WAS_OURS=1; else WAS_OURS=0; fi
  w25_stand_down || exit 1
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
  w25_pt_stop_soft
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
# --- G1) let the app's service call the Homebrew Channel ---------------------
# webOS 11 only lets a client call luna://org.webosbrew.hbchannel.service/exec
# with the org.webosbrew.hbchannel.service.group group; "public" no longer covers
# it (it still does on 10.3.1). The app installer writes our service's client
# permissions with "public" only, whatever the package asks for, and writes them
# again on every app install or update -- so every app call fails with "Message
# not processed" until the group is added back. Re-checked on every run for that
# reason. Only the one shape the installer is known to write is edited.
HB_GROUP=org.webosbrew.hbchannel.service.group
HB_PERM=/var/luna-service2-dev/client-permissions.d/io.github.josippapez.dtsenabler.service.service.json
w25_hb_permission() {
  [ -f "$HB_PERM" ] || return 0
  grep -q "$HB_GROUP" "$HB_PERM" 2>/dev/null && return 0
  if ! grep -Eqx '\{"io\.github\.josippapez\.dtsenabler\.service\*":\[[^]]*\]\}' "$HB_PERM" 2>/dev/null; then
    w25_log "hb-permission: unexpected format in $HB_PERM; left as is"
    return 0
  fi
  sed -i "s/\]}\$/,\"$HB_GROUP\"]}/" "$HB_PERM" 2>/dev/null
  if grep -q "$HB_GROUP" "$HB_PERM" 2>/dev/null; then
    /usr/sbin/ls-control scan-services >/dev/null 2>&1
    w25_log "hb-permission: added $HB_GROUP to the app service's client permissions"
  else
    w25_log "hb-permission: could not add $HB_GROUP to $HB_PERM"
  fi
}
w25_hb_permission
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
  w25_log "REFUSED: core payload incomplete (dtsdec present=$([ -f "$MYDTS" ] && echo 1 || echo 0), libav present=$([ -f "$MYLIBAV" ] && echo 1 || echo 0), adecswitch present=$([ -f "$MYSWITCH" ] && echo 1 || echo 0)); binding nothing, install left intact"
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
# --- APPLY 2d) OPT-IN: tell apps this TV accepts DTS (tv.model.edidType) -------
#     Detached, after boot has finished: this hook runs ~30s in, while
#     bootmode-normal-boot-done only goes active at ~45s (measured on a C5).
#     Bounded, and it applies anyway if the signal never arrives.
w25_appdts_start() {
  [ -n "${W25_NO_APPDTS:-}" ] && return 0
  [ -f "$APPDTS_FLAG" ] || return 0
  ( i=0
    while [ "$i" -lt 180 ]; do
      systemctl is-active bootmode-normal-boot-done.service >/dev/null 2>&1 && break
      i=$((i + 1)); sleep 1
    done
    w25_appdts_apply ) </dev/null >/dev/null 2>&1 &
}
w25_appdts_start
w25_pt_stop_soft
w25_pt_start
# --- APPLY 2c) container demuxers with DTS re-enabled (mp4/ts/m2ts DTS -> audio/x-dts).
#         Patched isomp4/mpegtsdemux/mpegpsdemux default dts_support=TRUE; patched matroska
#         signals Dolby Vision profile 7. Bound BEFORE the regen below so the
#         registry picks them up at their normal path.
[ -f "$MYISO" ] && ! grep -q " $LGISO " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYISO" "$LGISO" 2>>$LOG
[ -f "$MYTSD" ] && ! grep -q " $LGTSD " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYTSD" "$LGTSD" 2>>$LOG
[ -f "$MYMKV" ] && ! grep -q " $LGMKV " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYMKV" "$LGMKV" 2>>$LOG
[ -f "$MYPSD" ] && [ -f "$LGPSD" ] && ! grep -q " $LGPSD " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYPSD" "$LGPSD" 2>>$LOG
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
#         registry only if the regen survived the binds with all six elements
#         intact. Otherwise drop our own binds -- that is what turns "the
#         override did not match" from a broken media pipeline into a no-op.
if w25_reg_has_all "$REG_TMP"; then
  cp -f "$REG_TMP" "$REG" 2>>$LOG && echo "registry updated (dtsdec+avdec_truehd+adecswitch+qtdemux+tsdemux+matroskademux)" >>$LOG
  echo "APPLIED=$VERDICT"
else
  w25_log "REFUSED: regen is missing $REG_MISS; not committing it, and standing down"
  w25_stand_down
  toast "DTS Enabler: the plugin registry did not survive the override ($REG_MISS missing), so nothing was changed."
  echo "REFUSED=pipeline"
  echo "REASON=the regenerated plugin registry is missing $REG_MISS"
fi
exit 0
