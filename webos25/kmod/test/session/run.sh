#!/bin/sh
# run.sh -- host test of hdmi-session.sh against a fake TV tree.
#
#   sh test/session/run.sh [shell ...]     (default: sh, and dash when present)
#
# The fixtures are real status files from a C5 (see fixtures/README). Each case
# copies them into a fresh tree under DTS25_ROOT, edits them into the state it
# needs, and puts stubs for id, amixer, systemctl, lsmod, rmmod, dmesg, tee and
# sleep first in PATH, with a fake earc-route-load.sh next to a copy of the
# script. Every stub appends one line to an event log; the tee stub also applies
# "name value" to the fake prop file the way the TV's prop files do.
# shellcheck disable=SC2016 # the stub bodies expand when the stubs run
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../../hdmi-session.sh
FIX=$HERE/fixtures
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
shells=${*:-sh $(command -v dash || true)}

# --- stubs -----------------------------------------------------------------
BIN=$WORK/bin
mkdir -p "$BIN"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$BIN/$1"; chmod +x "$BIN/$1"; }
stub id 'echo "${FAKE_UID:-0}"'
stub amixer 'R=$DTS25_ROOT
case "$3" in
  numid=92) ;;
  *) echo "amixer: unexpected $*" >&2; exit 1 ;;
esac
if [ "$2" = cget ]; then
  sed "s/values=2,1,0,0,1/values=${FAKE_EARC:-2,1,0,0,1}/" "$FAKE_FIX/amixer-cget-numid92.txt"
  exit 0
fi
echo "cset numid=92 $4" >> "$FAKE_EVENTS"
[ "${FAKE_ROUTE:-yes}" = no ] || printf "%s\n" " renderer_${FAKE_RENDERER:-1}_out100 -> mixer_0_in100 [label=\"\"]" " mixer_0_out100 -> output_3_in0 [label=\"\"]" >> "$R/proc/aaudio/dot_block_connection"'
# FAKE_BLIP: the first restart turns the HDMI input to PCM; "once" brings it
# back at the next sleep (as on a CS), "forever" leaves it. FAKE_DEC: the
# decoder a restart starts (default 1). FAKE_FROM: at the first restart the
# input leaves that decoder (Connect=UNKNOWN, Start=0) for FAKE_DEC, on port 0.
# FAKE_LATE=N: the first restart turns the input to PCM and leaves the decoder
# stopped; N sleeps later the input is DTS again and the decoder starts.
stub systemctl 'echo "systemctl $*" >> "$FAKE_EVENTS"
A=$DTS25_ROOT/proc/lgtv-driver/audio h=$DTS25_ROOT/proc/lgtv-driver/audio/hdmi d=${FAKE_DEC:-1}
if [ -n "${FAKE_BLIP:-}" ] && [ ! -e "$DTS25_ROOT/blipped" ]; then
  touch "$DTS25_ROOT/blipped"; cp "$h" "$DTS25_ROOT/hdmi.real"; sed -i.bak "s/^Codec=.*/Codec=PCM/" "$h"
  [ "$FAKE_BLIP" = once ] || rm "$DTS25_ROOT/hdmi.real"
fi
if [ -n "${FAKE_FROM:-}" ] && [ ! -e "$DTS25_ROOT/moved" ]; then
  touch "$DTS25_ROOT/moved"
  sed -i.bak -e "s/^Connect=.*/Connect=UNKNOWN/" -e "s/^Start=.*/Start=0/" "$A/adec.p$FAKE_FROM"
  sed -i.bak "s/^Connect=.*/Connect=HDMI_PORT0/" "$A/adec.p$d"
fi
if [ -n "${FAKE_LATE:-}" ] && [ ! -e "$DTS25_ROOT/late-done" ]; then
  touch "$DTS25_ROOT/late-done"; cp "$h" "$DTS25_ROOT/hdmi.late"; sed -i.bak "s/^Codec=.*/Codec=PCM/" "$h"
  echo "$FAKE_LATE" > "$DTS25_ROOT/late-left"; exit 0
fi
[ "${FAKE_ADEC:-start}" = never ] || sed -i.bak -e "s/^UserCodec=.*/UserCodec=${FAKE_ADEC_CODEC:-DTS}/" -e "s/^Start=.*/Start=1/" "$A/adec.p$d"'
stub lsmod 'echo "Module Size Used by"; [ ! -e "$DTS25_ROOT/sys/module/earc_route" ] || echo "earc_route 16384 0"'
stub rmmod 'echo "rmmod $*" >> "$FAKE_EVENTS"; rm -rf "$DTS25_ROOT/sys/module/$1"'
stub dmesg 'echo "[ 1.0] earc_route: fake kernel line"'
stub tee 'line=$(cat); f=$1; echo "write ${f#$DTS25_ROOT} $line" >> "$FAKE_EVENTS"
name=${line%% *}; value=${line#* }
sed -i.bak "s/^$name=\(([a-z]*)\).*/$name=\1$value/" "$f"'
# Each sleep counts down lose-after; at zero the HDMI source becomes PCM. A
# script that keeps sleeping long after that (a broken loop) is sent TERM.
# "sleep 2" is the DTS-HD muted wait: it is logged, and the eARC non-PCM word
# only becomes FAKE_NONPCM (default 1) during it.
stub sleep '[ ! -f "$DTS25_ROOT/hdmi.real" ] || mv "$DTS25_ROOT/hdmi.real" "$DTS25_ROOT/proc/lgtv-driver/audio/hdmi"
if [ -f "$DTS25_ROOT/late-left" ]; then
  left=$(($(cat "$DTS25_ROOT/late-left") - 1)); echo "$left" > "$DTS25_ROOT/late-left"
  if [ "$left" -le 0 ]; then
    rm "$DTS25_ROOT/late-left"; mv "$DTS25_ROOT/hdmi.late" "$DTS25_ROOT/proc/lgtv-driver/audio/hdmi"
    sed -i.bak -e "s/^UserCodec=.*/UserCodec=DTS/" -e "s/^Start=.*/Start=1/" "$DTS25_ROOT/proc/lgtv-driver/audio/adec.p${FAKE_DEC:-1}"
  fi
fi
if [ "$1" = 2 ]; then
  echo "sleep 2" >> "$FAKE_EVENTS"
  echo "${FAKE_NONPCM:-1}" > "$DTS25_ROOT/sys/module/earc_route/parameters/earc_nonpcm"
fi
n=$DTS25_ROOT/lose-after
[ -f "$n" ] || exit 0
left=$(($(cat "$n") - 1)); echo "$left" > "$n"
[ "$left" -gt 0 ] || sed -i.bak "s/^Codec=.*/Codec=PCM/" "$DTS25_ROOT/proc/lgtv-driver/audio/hdmi"
[ "$left" -gt -100 ] || kill -TERM "$PPID"'
LOADER='echo "load $*" >> "$FAKE_EVENTS"
if [ "${FAKE_LOADER:-ok}" = refuse ]; then
  echo "earc-route: REFUSED: another DTS tool is loaded: dts_gate (unload it first)"; exit 1
fi
P=$DTS25_ROOT/sys/module/earc_route/parameters
mkdir -p "$P"
for v in gate_open=1 selftest_passed=1 window_open=1 calls_seen=3 calls_matched=1 overrides=1 earc_nonpcm=0 \
         hd_probes_registered=2 hd_status_seen=4 hd_status_accepts=1 hd_name_seen=4 hd_name_fixes=1; do echo "${v#*=}" > "$P/${v%=*}"; done
echo "earc-route: PASSTHROUGH ACTIVE (mode=5 $*)"'

# --- one case --------------------------------------------------------------
fail=0
cases=0
setup() {  # a DTS core input on HDMI port 0, decoder 1, eARC on, ADEC idle
    R=$WORK/root
    rm -rf "$R" "$WORK/app"
    A=$R/proc/lgtv-driver/audio
    mkdir -p "$A" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001" "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001" \
        "$R/proc/aaudio/processors/dsp2/gain/041b0001" "$R/tmp" "$WORK/app"
    cp "$FIX/adec.p0" "$FIX/adec.p1" "$FIX/sndout" "$A/"
    sed -e 's/^Codec=.*/Codec=UNKNOWN_DTS/' -e 's/^SamplingRate=.*/SamplingRate=48000/' "$FIX/hdmi" > "$A/hdmi"
    sed -i.bak 's/^EArcOnOff=.*/EArcOnOff=On/' "$A/sndout"
    sed -i.bak -e 's/^UserCodec=.*/UserCodec=UNKNOWN/' -e 's/^Start=.*/Start=0/' "$A/adec.p1"
    cp "$FIX/hdmi1.prop" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop"
    cp "$FIX/hdmi1.lstatus" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001/lstatus"
    # hdmi0 pairs with adec.p0 (not connected in the fixture): idle, on port 0.
    sed 's/^start=(int).*/start=(int)0/' "$FIX/hdmi1.prop" > "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001/prop"
    cp "$FIX/hdmi1.lstatus" "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001/lstatus"  # a session samples its instance's
    cp "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001/prop" "$WORK/hdmi0.found"
    cp "$FIX/hdmi1.prop" "$WORK/hdmi1.found"
    cp "$FIX/gain.prop" "$R/proc/aaudio/processors/dsp2/gain/041b0001/prop"
    cp "$FIX/dot_module_connection" "$FIX/dot_block_connection" "$R/proc/aaudio/"
    cp "$SCRIPT" "$WORK/app/hdmi-session.sh"
    printf '#!/bin/sh\n%s\n' "$LOADER" > "$WORK/app/earc-route-load.sh"
    : > "$WORK/events"
    unset FAKE_UID FAKE_EARC FAKE_ROUTE FAKE_ADEC FAKE_LOADER FAKE_ADEC_CODEC FAKE_NONPCM FAKE_BLIP FAKE_RENDERER FAKE_DEC FAKE_FROM FAKE_LATE
}
hd_setup() {  # hd_setup CODEC: the same TV with a DTS-HD stream named CODEC
    setup
    sed -i.bak -e "s/^Codec=.*/Codec=$1/" -e 's/^SamplingRate=.*/SamplingRate=192000/' "$A/hdmi"
    # No real sample: the C5 the fixtures come from has no eARC output instance.
    mkdir -p "$R/proc/aaudio/processors/dsp1/output_arc/03220001"
    echo 'synthetic=(int)1' > "$R/proc/aaudio/processors/dsp1/output_arc/03220001/prop"
    FAKE_ADEC_CODEC=DTS_HD; export FAKE_ADEC_CODEC
}
run() {  # run ARGS...: sets status, out
    if out=$(DTS25_ROOT=$R FAKE_EVENTS=$WORK/events FAKE_FIX=$FIX PATH="$BIN:$PATH" \
             $SH "$WORK/app/hdmi-session.sh" "$@" 2>&1); then status=0; else status=$?; fi
    no_core_load
}
# No kind ever loads --dts-core: --dts-hd is the session stage for both.
no_core_load() { case "$(cat "$WORK/events")" in *dts-core*) bad "loaded --dts-core" ;; esac; }
bad() { echo "FAIL [$SH] $name: $*"; echo "$out" | sed 's/^/    | /'; sed 's/^/    e /' "$WORK/events"; fail=1; }
expect_status() { [ "$status" = "$1" ] || bad "exit $status, expected $1"; }
expect_out() { case "$out" in *"$1"*) ;; *) bad "output lacks: $1" ;; esac; }
expect_events() { [ "$(cat "$WORK/events")" = "$1" ] || bad "events differ from expected:
$1"; }
# Refused before the first write: nothing loaded, written or restarted, no log dir.
expect_untouched() {
    [ ! -s "$WORK/events" ] || bad "events recorded"
    [ -z "$(ls "$R/tmp")" ] || bad "log dir created"
}
# Everything this run changed is back as it was and the module is gone. A case
# that edits a DSP prop before the run copies it to hdmi1.found or hdmi0.found.
expect_restored() {
    cmp -s "$WORK/hdmi1.found" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop" || bad "DSP prop not restored"
    cmp -s "$FIX/gain.prop" "$R/proc/aaudio/processors/dsp2/gain/041b0001/prop" || bad "gain prop not restored"
    cmp -s "$WORK/hdmi0.found" "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001/prop" || bad "hdmi0 prop changed"
    [ ! -e "$R/sys/module/earc_route" ] || bad "module still loaded"
    [ "$(tail -n 2 "$WORK/events")" = "cset numid=92 2,1,0,0,1
systemctl restart extinput-integration.service" ] || bad "numid 92 / service not restored last"
    case "$out" in *'Rollback incomplete'*) bad "rollback incomplete" ;; esac
}

P=/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop
P0=/proc/aaudio/processors/dsp0/hdmi0/021d0001/prop
G=/proc/aaudio/processors/dsp2/gain/041b0001/prop
HAPPY="load --dts-hd window_seconds=45
systemctl restart extinput-integration.service
cset numid=92 2,1,0,0,1
write $G mute-input false
write $G mute-output false
write $P codec 5
write $P mode 1
write $P data-drop-mode 1
write $P drop-output-data false
write $P start 1
write $P drop 0"
RESTORE="write $P drop 1
write $P start 0
write $G mute-input false
write $G mute-output false
write $P codec 4
write $P mode 4
write $P data-drop-mode 2
write $P drop-output-data false
write $P start 1
write $P drop false
rmmod earc_route
cset numid=92 2,1,0,0,1
systemctl restart extinput-integration.service"
HD_HAPPY="load --dts-hd window_seconds=45
systemctl restart extinput-integration.service
cset numid=92 2,1,0,0,1
write $G mute-input true
write $G mute-output true
write $P codec 6
write $P mode 4
write $P sampling-freq 192000
write $P channel-num 8
write $P data-drop-mode 1
write $P drop-output-data false
write $P start 1
write $P drop 0
sleep 2"
HD_UNMUTE="write $G mute-input false
write $G mute-output false"
HD_RESTORE="write $P drop 1
write $P start 0
write $G mute-input false
write $G mute-output false
write $P codec 4
write $P mode 4
write $P sampling-freq 192000
write $P channel-num 2
write $P data-drop-mode 2
write $P drop-output-data false
write $P start 1
write $P drop false
rmmod earc_route
cset numid=92 2,1,0,0,1
systemctl restart extinput-integration.service"

# The same writes on hdmi0's instance; hdmi0 starts the case stopped
# (start=(int)0), which is what its restore writes back.
HAPPY0=$(echo "$HAPPY" | sed "s|$P|$P0|")
RESTORE0=$(echo "$RESTORE" | sed -e "s|$P|$P0|" -e 's/start 1$/start 0/')
HD_HAPPY0=$(echo "$HD_HAPPY" | sed "s|$P|$P0|")
HD_RESTORE0=$(echo "$HD_RESTORE" | sed -e "s|$P|$P0|" -e 's/start 1$/start 0/')

for SH in $shells; do
    command -v "$SH" > /dev/null || { echo "SKIP shell $SH: not found"; continue; }

    for codec in DTS UNKNOWN_DTS; do
        name="check accepts Codec=$codec at 48000"; cases=$((cases+1)); setup
        sed -i.bak "s/^Codec=.*/Codec=$codec/" "$A/hdmi"
        run --check; expect_status 0; expect_out 'CHECK OK'; expect_untouched
        expect_out 'bypass gain block: gain_041b0001 (the compressed route'
    done
    for pair in MAT:192000 PCM:48000 NO_AUDIO:48000 DTS:44100 UNKNOWN_DTS:192000; do
        name="refuses Codec=${pair%:*} at ${pair#*:}"; cases=$((cases+1)); setup
        sed -i.bak -e "s/^Codec=.*/Codec=${pair%:*}/" -e "s/^SamplingRate=.*/SamplingRate=${pair#*:}/" "$A/hdmi"
        run --apply 15; expect_status 1; expect_out 'not a live DTS stream this script handles'; expect_untouched
    done

    name='refuses eARC off'; cases=$((cases+1)); setup
    sed -i.bak 's/^EArcOnOff=.*/EArcOnOff=Off/' "$A/sndout"
    run --apply; expect_status 1; expect_out 'REFUSED: eARC is not on (EArcOnOff=Off)'; expect_untouched

    name='decoder 0 plays the input: the session runs on hdmi0 and renderer 0'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Start=.*/Start=1/' "$A/adec.p0"
    sed -i.bak 's/^Connect=.*/Connect=HDMI_PORT3/' "$A/adec.p1"
    FAKE_DEC=0 FAKE_RENDERER=0; export FAKE_DEC FAKE_RENDERER
    run --apply 15; expect_status 0; expect_out 'ok    HDMI port 0 is on decoder 0 ('
    expect_out 'ok    one hdmi0 DSP instance, hdmi-port 0'
    expect_events "$HAPPY0
$RESTORE0"
    expect_restored

    name='decoder 0 beside a closed decoder 1 on the same port: check OK on decoder 0'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Open=.*/Open=1/' -e 's/^Start=.*/Start=1/' "$A/adec.p0"
    sed -i.bak 's/^Open=.*/Open=0/' "$A/adec.p1"
    sed -i.bak 's/^start=(int).*/start=(int)0/' "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop"
    run --check; expect_status 0; expect_out 'CHECK OK'; expect_untouched
    expect_out 'ok    HDMI port 0 is on decoder 0 (adec.p0 Connect=HDMI_PORT0 Open=1 Start=1 UserCodec=UNKNOWN; adec.p1 Connect=HDMI_PORT0 Open=0 Start=0 UserCodec=UNKNOWN)'

    name='C5: stale adec.p1 on hdmi1 port 2, the stream on adec.p0/hdmi0 port 0: decoder 0'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^hdmi-port=(int).*/hdmi-port=(int)2/' -e 's/^start=(int).*/start=(int)0/' -e 's/^codec=(int).*/codec=(int)0/' "$R$P"
    sed -i.bak 's/^start=(int).*/start=(int)1/' "$R$P0"
    cp "$R$P" "$WORK/hdmi1.found"; cp "$R$P0" "$WORK/hdmi0.found"
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Open=.*/Open=1/' -e 's/^Start=.*/Start=1/' -e 's/^UserCodec=.*/UserCodec=DTS/' "$A/adec.p0"
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT2/' "$A/adec.p1"
    FAKE_DEC=0 FAKE_RENDERER=0; export FAKE_DEC FAKE_RENDERER
    run --apply 15; expect_status 0; expect_out 'ok    HDMI port 0 is on decoder 0'
    expect_events "$HAPPY0
$(echo "$RESTORE" | sed "s|$P|$P0|")"
    expect_restored

    name='nothing started, only adec.p0/hdmi0 connected: proceeds on decoder 0'; cases=$((cases+1)); setup
    sed -i.bak 's/^Connect=.*/Connect=HDMI_PORT0/' "$A/adec.p0"
    sed -i.bak 's/^Connect=.*/Connect=UNKNOWN/' "$A/adec.p1"
    FAKE_DEC=0 FAKE_RENDERER=0; export FAKE_DEC FAKE_RENDERER
    run --apply 15; expect_status 0
    expect_out 'ok    HDMI port 0 is connected to decoder 0, not started yet; the input-service restart starts it'
    expect_events "$HAPPY0
$RESTORE0"
    expect_restored

    name='decoder 0, but only renderer 1 is routed: rolled back'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Start=.*/Start=1/' "$A/adec.p0"
    sed -i.bak 's/^Connect=.*/Connect=UNKNOWN/' "$A/adec.p1"
    FAKE_DEC=0; export FAKE_DEC
    run --apply 15; expect_status 1; expect_out 'Compressed route not created'
    expect_restored
    case "$(cat "$WORK/events")" in *'codec 5'*|*mute-*) bad "wrote to the DSP instance or gain" ;; esac

    name='the restart moves the input from decoder 1 to decoder 0: hdmi0 is written and restored'; cases=$((cases+1)); setup
    FAKE_FROM=1 FAKE_DEC=0 FAKE_RENDERER=0; export FAKE_FROM FAKE_DEC FAKE_RENDERER
    run --apply 15; expect_status 0; expect_out 'ok    HDMI port 0 is on decoder 1'
    expect_out 'The input-service restart moved HDMI port 0 from decoder 1 to decoder 0; using decoder 0.'
    expect_events "$HAPPY0
$RESTORE0"
    expect_restored

    name='the restart moves the input from decoder 0 to decoder 1: hdmi1 is written and restored'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Start=.*/Start=1/' "$A/adec.p0"
    sed -i.bak 's/^Connect=.*/Connect=UNKNOWN/' "$A/adec.p1"
    FAKE_FROM=0 FAKE_DEC=1; export FAKE_FROM FAKE_DEC
    run --apply 15; expect_status 0; expect_out 'ok    HDMI port 0 is on decoder 0'
    expect_out 'The input-service restart moved HDMI port 0 from decoder 0 to decoder 1; using decoder 1.'
    expect_events "$HAPPY
$RESTORE"
    expect_restored

    name='nothing started, only adec.p1/hdmi1 connected: proceeds'; cases=$((cases+1)); setup
    sed -i.bak 's/^start=(int).*/start=(int)0/' "$R$P"
    cp "$R$P" "$WORK/hdmi1.found"
    run --apply 15; expect_status 0
    expect_out 'ok    HDMI port 0 is connected to decoder 1, not started yet; the input-service restart starts it'
    expect_events "$HAPPY
$(echo "$RESTORE" | sed 's/start 1$/start 0/')"
    cmp -s "$WORK/hdmi1.found" "$R$P" || bad "DSP prop not restored"
    cmp -s "$WORK/hdmi0.found" "$R$P0" || bad "hdmi0 prop changed"
    [ ! -e "$R/sys/module/earc_route" ] || bad "module still loaded"

    name='both decoders started, adec.p1 on DTS: proceeds on decoder 1'; cases=$((cases+1)); setup
    sed -i.bak -e 's/^hdmi-port=(int).*/hdmi-port=(int)2/' -e 's/^start=(int).*/start=(int)1/' "$R$P0"
    cp "$R$P0" "$WORK/hdmi0.found"
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT2/' -e 's/^Start=.*/Start=1/' -e 's/^UserCodec=.*/UserCodec=MAT/' "$A/adec.p0"
    sed -i.bak 's/^UserCodec=.*/UserCodec=DTS/' "$A/adec.p1"
    run --apply 15; expect_status 0; expect_out 'ok    HDMI port 0 is on decoder 1'
    expect_events "$HAPPY
$RESTORE"
    expect_restored

    name='nothing started, both pairs connected: refused'; cases=$((cases+1)); setup
    sed -i.bak 's/^start=(int).*/start=(int)0/' "$R$P"
    sed -i.bak 's/^hdmi-port=(int).*/hdmi-port=(int)2/' "$R$P0"
    sed -i.bak 's/^Connect=.*/Connect=HDMI_PORT2/' "$A/adec.p0"
    run --apply 15; expect_status 1; expect_untouched
    expect_out 'REFUSED: cannot tell which decoder carries the HDMI input'

    name='two failed checks are counted'; cases=$((cases+1)); setup
    sed -i.bak 's/^Codec=.*/Codec=NO_AUDIO/' "$A/hdmi"
    sed -i.bak 's/^EArcOnOff=.*/EArcOnOff=Off/' "$A/sndout"
    run --check; expect_status 1; expect_untouched
    expect_out 'REFUSED (2 checks failed, first: the HDMI input is not a live DTS stream this script handles (AudioData=1 Codec=NO_AUDIO SamplingRate=48000)). Nothing changed.'

    name='refuses an unexpected numid 92'; cases=$((cases+1)); setup
    FAKE_EARC=2,1,0,0,0; export FAKE_EARC
    run --apply; expect_status 1; expect_out "REFUSED: eARC control numid 92 is '2,1,0,0,0'"; expect_untouched

    name='refuses two gain candidates'; cases=$((cases+1)); setup
    echo ' gain_041b0004_00000000d79692ad_out0 -> bypass_03160001_00000000cef69907_in0;' >> "$R/proc/aaudio/dot_module_connection"
    run --apply; expect_status 1; expect_out 'more than one gain block feeds a bypass block (041b0001 041b0004)'; expect_untouched

    name='refuses when not root'; cases=$((cases+1)); setup
    FAKE_UID=1000; export FAKE_UID
    run --apply; expect_status 1; expect_out 'REFUSED: must run as root'; expect_untouched

    name='no arguments changes nothing'; cases=$((cases+1)); setup
    run; expect_status 0; expect_out 'Nothing changed'; expect_untouched
    expect_out '--check    read-only'; expect_out '30 seconds unless given'
    name='rejects 61 seconds'; cases=$((cases+1)); setup
    run --apply 61; expect_status 2; expect_untouched

    name='stops when the loader refuses'; cases=$((cases+1)); setup
    FAKE_LOADER=refuse; export FAKE_LOADER
    run --apply 15; expect_status 1; expect_out 'REFUSED: another DTS tool is loaded: dts_gate'
    expect_events 'load --dts-hd window_seconds=45'
    cmp -s "$FIX/hdmi1.prop" "$R$P" || bad "prop changed"
    case "$out" in *'Rollback incomplete'*) bad "rollback incomplete" ;; esac

    name='happy path --apply 15'; cases=$((cases+1)); setup
    run --apply 15; expect_status 0; expect_out 'DTS core test running for 15 seconds'
    expect_events "$HAPPY
$RESTORE"
    expect_restored
    expect_out 'Module counters: gate_open=1 selftest_passed=1 window_open=1 calls_seen=3 calls_matched=1 overrides=1 hd_probes_registered=2 hd_status_seen=4 hd_status_accepts=1 hd_name_seen=4 hd_name_fixes=1'
    logdir=$(ls -d "$R"/tmp/dts25-hdmi-*)
    [ "$(ls -ld "$logdir" | cut -c1-10)" = drwx------ ] || bad "log dir is not mode 700"
    for f in original.prop original.sndout original.earc original.gain loader.txt route-before-input.dot \
             modules-before-input.dot hdmi-15.txt final.dot test.prop counters.txt module.log final.sndout; do
        [ -f "$logdir/$f" ] || bad "log lacks $f"
    done

    name='session ends with 10 on source loss'; cases=$((cases+1)); setup
    echo 3 > "$R/lose-after"
    run --session; expect_status 10; expect_out 'Input changed; ending test.'
    expect_events "$(echo "$HAPPY" | sed 's/window_seconds=45/window_seconds=0 supervised=1/')
$RESTORE"
    expect_restored

    name='ADEC never starts: rolled back'; cases=$((cases+1)); setup
    FAKE_ADEC=never; export FAKE_ADEC
    run --apply 15; expect_status 1; expect_out 'ADEC did not start; aborting.'
    expect_restored
    case "$(cat "$WORK/events")" in *'codec 5'*) bad "wrote codec 5" ;; esac

    name='input reads PCM for a moment after the service restart'; cases=$((cases+1)); setup
    FAKE_BLIP=once; export FAKE_BLIP
    run --apply 15; expect_status 0; expect_out 'DTS core test running for 15 seconds'
    expect_events "$HAPPY
$RESTORE"
    expect_restored

    name='the decoder starts on DTS 12 s after the service restart'; cases=$((cases+1)); setup
    FAKE_LATE=12; export FAKE_LATE
    run --apply 15; expect_status 0; expect_out 'DTS core test running for 15 seconds'
    expect_events "$HAPPY
$RESTORE"
    expect_restored

    name='input never comes back after the service restart: rolled back'; cases=$((cases+1)); setup
    FAKE_BLIP=forever; export FAKE_BLIP
    run --apply 15; expect_status 1; expect_out 'DTS core input did not come back within 20 s'
    case "$(cat "$WORK/events")" in *'codec 5'*) bad "wrote codec 5" ;; esac
    [ ! -e "$R/sys/module/earc_route" ] || bad "module still loaded"
    case "$out" in *'Rollback incomplete'*) bad "rollback incomplete" ;; esac

    name='no compressed route: rolled back'; cases=$((cases+1)); setup
    FAKE_ROUTE=no; export FAKE_ROUTE
    run --apply 15; expect_status 1; expect_out 'Compressed route not created'
    expect_restored

    name='no gain candidate after the trigger: rolled back'; cases=$((cases+1)); setup
    sed -i.bak '/gain_041b0001_[0-9a-f]*_out0 -> bypass_/d' "$R/proc/aaudio/dot_module_connection"
    run --apply 15; expect_status 1; expect_out 'Expected exactly one gain block'
    expect_restored
    case "$(cat "$WORK/events")" in *mute-*) bad "touched a gain block" ;; esac

    # --- DTS-HD kind -------------------------------------------------------
    for codec in DTS_HD UNKNOWN_DTS_HD; do
        name="check classifies Codec=$codec at 192000 as DTS-HD"; cases=$((cases+1)); hd_setup "$codec"
        run --check; expect_status 0; expect_out 'CHECK OK'; expect_untouched
        expect_out "ok    live DTS-HD at 192 kHz on HDMI (AudioData=1 Codec=$codec SamplingRate=192000)"
    done
    for pair in DTS_HD:48000 UNKNOWN_DTS_HD:96000 DTS_HD_HRA:192000 UNKNOWN_DTS_HD_HRA:192000 DTS_HD_MA:192000; do
        name="refuses Codec=${pair%:*} at ${pair#*:}"; cases=$((cases+1)); hd_setup "${pair%:*}"
        sed -i.bak "s/^SamplingRate=.*/SamplingRate=${pair#*:}/" "$A/hdmi"
        run --apply 15; expect_status 1; expect_untouched
        expect_out "REFUSED: the HDMI input is not a live DTS stream this script handles (AudioData=1 Codec=${pair%:*} SamplingRate=${pair#*:})"
        expect_out 'accepted: Codec=DTS or UNKNOWN_DTS at 48000 (DTS core), Codec=DTS_HD or UNKNOWN_DTS_HD at 192000 (DTS-HD)'
    done

    for codec in DTS_HD UNKNOWN_DTS_HD; do
        name="DTS-HD happy path --apply 15 (Codec=$codec)"; cases=$((cases+1)); hd_setup "$codec"
        run --apply 15; expect_status 0; expect_out 'DTS-HD test running for 15 seconds'
        expect_events "$HD_HAPPY
$HD_UNMUTE
$HD_RESTORE"
        expect_restored
        expect_out 'overrides=1 hd_probes_registered=2 hd_status_seen=4 hd_status_accepts=1 hd_name_seen=4 hd_name_fixes=1'
        logdir=$(ls -d "$R"/tmp/dts25-hdmi-*)
        [ "$(cat "$logdir/earc-nonpcm.txt")" = 1 ] || bad "earc-nonpcm.txt is not 1"
        [ "$(cat "$logdir/output-before-unmute.prop")" = 'synthetic=(int)1' ] || bad "output_arc prop not saved"
    done

    name='DTS-HD: earc_nonpcm 0 aborts muted, gain as found'; cases=$((cases+1)); hd_setup DTS_HD
    sed -i.bak 's/^\(mute-[a-z]*put\)=(bool)false/\1=(bool)true/' "$R$G"
    grep -c '=(bool)true' "$R$G" | grep -qx 2 || bad "fixture edit failed"
    cp "$R$G" "$WORK/gain.found"
    FAKE_NONPCM=0; export FAKE_NONPCM
    run --apply 15; expect_status 1; expect_out 'eARC is not non-PCM (earc_nonpcm=0); aborting muted'
    expect_events "$HD_HAPPY
$(echo "$HD_RESTORE" | sed 's/\(mute-[a-z]*put\) false/\1 true/')"
    cmp -s "$WORK/gain.found" "$R$G" || bad "gain not as found"
    cmp -s "$FIX/hdmi1.prop" "$R$P" || bad "DSP prop not restored"
    [ ! -e "$R/sys/module/earc_route" ] || bad "module still loaded"
    case "$out" in *'Rollback incomplete'*) bad "rollback incomplete" ;; esac

    name='DTS-HD session ends with 10 on source loss'; cases=$((cases+1)); hd_setup UNKNOWN_DTS_HD
    echo 4 > "$R/lose-after"
    run --session; expect_status 10; expect_out 'DTS-HD supervised session active'; expect_out 'Input changed; ending test.'
    expect_events "$(echo "$HD_HAPPY" | sed 's/window_seconds=45/window_seconds=0 supervised=1/')
$HD_UNMUTE
$HD_RESTORE"
    expect_restored

    name='DTS-HD on decoder 0: hdmi0 and renderer 0'; cases=$((cases+1)); hd_setup UNKNOWN_DTS_HD
    sed -i.bak -e 's/^Connect=.*/Connect=HDMI_PORT0/' -e 's/^Start=.*/Start=1/' "$A/adec.p0"
    sed -i.bak 's/^Connect=.*/Connect=UNKNOWN/' "$A/adec.p1"
    FAKE_DEC=0 FAKE_RENDERER=0; export FAKE_DEC FAKE_RENDERER
    run --apply 15; expect_status 0; expect_out 'DTS-HD test running for 15 seconds'
    expect_events "$HD_HAPPY0
$HD_UNMUTE
$HD_RESTORE0"
    expect_restored

    name='DTS-HD: ADEC stays on DTS: rolled back'; cases=$((cases+1)); hd_setup DTS_HD
    unset FAKE_ADEC_CODEC
    run --apply 15; expect_status 1; expect_out 'ADEC did not start; aborting.'
    expect_restored
    case "$(cat "$WORK/events")" in *'codec 6'*|*mute-*) bad "wrote to the DSP instance or gain" ;; esac

    name='core stream with the ADEC on DTS_HD: rolled back'; cases=$((cases+1)); setup
    FAKE_ADEC_CODEC=DTS_HD; export FAKE_ADEC_CODEC
    run --apply 15; expect_status 1; expect_out 'ADEC did not start; aborting.'
    expect_restored
done

[ "$fail" = 0 ] || { echo "hdmi-session test FAILED"; exit 1; }
echo "hdmi-session test passed: $cases cases ($shells)"
