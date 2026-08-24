#!/bin/sh
# check-report-sync.sh -- report-tv.sh must compute the SAME profile the app does.
#
# report-tv.sh duplicates the C2 gate from the app's DETECT_PROBE (it has to: it
# runs standalone, with no app installed). Duplication drifts silently, and a
# report script that disagrees with the gate it is meant to explain is worse than
# no report at all -- it sends the maintainer after the wrong value. So assert the
# gate lines are byte-identical, modulo the JS string quoting around them.
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
check "C2 profiles kept from override" 'case "$PROFILE" in webos22-o22-gst118|webos22-o22-c2-diagnostic) C2_KEEP=1 ;; *) C2_KEEP=0 ;; esac'
check "native-dts override guard" 'if [ "$HAS_DTS_AUDIODEC" = "yes" ] && [ "$C2_KEEP" = 0 ]; then'
check "native-dts gated split" 'if [ "$DCA_RANK" = "0" ]; then PROFILE=native-dts-gated'
check "GST_MM derivation" 'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)'

if [ "$fails" -ne 0 ]; then
  echo "FAILED: $fails gate line(s) out of sync between service.js and report-tv.sh"
  exit 1
fi
echo "ALL CHECKS PASSED (report-tv.sh gate matches the app)"
