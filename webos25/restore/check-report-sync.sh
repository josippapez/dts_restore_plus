#!/bin/sh
# check-report-sync.sh -- report-tv.sh must compute the SAME profile the app does.
#
# report-tv.sh duplicates the app's profile selection logic so it can run
# standalone. Verify that critical detection rules remain synchronized
# between service.js and report-tv.sh.
set -eu
here=$(dirname "$0")
SVC="$here/../app/service/service.js"
RPT="$here/report-tv.sh"
[ -f "$SVC" ] || { echo "FAIL: cannot find $SVC"; exit 1; }
[ -f "$RPT" ] || { echo "FAIL: cannot find $RPT"; exit 1; }

fails=0
check() {
  # $1 = human name, $2 = literal shell fragment that must be in BOTH files
  if ! grep -qF "$2" "$SVC"; then echo "FAIL: $1 not found in service.js"; fails=$((fails+1)); return; fi
  if ! grep -qF "$2" "$RPT"; then echo "FAIL: $1 not found in report-tv.sh"; fails=$((fails+1)); return; fi
  echo "ok - $1"
}

check "C2 model glob" 'OLED*C2*|OLED*G2*|OLED*CS*) C2_MODEL=1 ;; *) C2_MODEL=0 ;; esac'
check "C2 firmware triple 7.4.0" '[ "$WEBOS_MANUFACTURING_VERSION" = "04.40.93" ] || [ "$WEBOS_MANUFACTURING_VERSION" = "04.40.93.01" ]; } && [ "$WEBOS_RELEASE" = "7.4.0" ] && [ "$GST_VERSION" = "1.18.2" ]'
check "C2 firmware triple 9.2.2" '[ "$WEBOS_MANUFACTURING_VERSION" = "23.25.55" ] || [ "$WEBOS_MANUFACTURING_VERSION" = "23.25.55.01" ]; } && [ "$WEBOS_RELEASE" = "9.2.2" ] && [ "$GST_VERSION" = "1.18.5" ]'
check "C2 hardware ids" 'HE_DTV_W22O_AFABATAA|HE_DTV_W22O_AFABATPU)'
check "gate: model" '[ "$C2_MODEL" = 1 ] || C2_GATE_FAIL="$C2_GATE_FAIL model($PRODUCT_ID)"'
check "gate: board-type" '[ "$BOARD_TYPE" != unknown ] || C2_GATE_FAIL="$C2_GATE_FAIL board-type"'
check "gate: firmware" '[ "$C2_FWOK" = 1 ] || C2_GATE_FAIL="$C2_GATE_FAIL firmware($WEBOS_MANUFACTURING_VERSION/$WEBOS_RELEASE/$GST_VERSION)"'
check "gate: loader" '[ "$LOADER" = "ld-linux.so.3" ] || C2_GATE_FAIL="$C2_GATE_FAIL loader($LOADER)"'
check "gate: float-abi" '[ "$FLOAT_ABI" = "soft" ] || C2_GATE_FAIL="$C2_GATE_FAIL float-abi($FLOAT_ABI)"'
check "W25 loader/abi condition" '[ "$LOADER" = "ld-linux.so.3" ] && [ "$FLOAT_ABI" = "soft" ]'
check "float ABI hard bit" '[ "$((val & 0x400))" -ne 0 ]'
check "float ABI soft bit" '[ "$((val & 0x200))" -ne 0 ]'
check "native DTS protected profiles" 'webos22-o22-gst118|webos22-o22-c2-diagnostic|webos25-armel-gst124) ;;'
check "native DTS override guard" 'if [ "$HAS_DTS_AUDIODEC" = "yes" ]; then'
check "native DTS gated split" 'if [ "$DCA_RANK" = "0" ]; then PROFILE=native-dts-gated'
check "native DTS fallback" 'else PROFILE=native-dts; fi'
check "GST_MM derivation" 'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)'
# The loader must be chosen from the plugin's own ELF identity, not from which
# loader filename sorts first -- a TV carrying BOTH loaders (issue #5) picked the
# 64-bit one and was reported unsupported while its media stack was 32-bit ARM.
check "plugin ELF class/machine -> arch" '01/2800) PLUGIN_ARCH=arm32 ;;'
check "loader chosen from plugin arch" 'arm32) LD=$(first_glob "/lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3" 2>/dev/null) ;;'

if [ "$fails" -ne 0 ]; then
  echo "FAILED: $fails profile detection check(s) out of sync between service.js and report-tv.sh"
  exit 1
fi
echo "ALL CHECKS PASSED (report-tv.sh profile detection matches the app)"
