#!/bin/sh
# run.sh -- host test of hdmi-watch.sh against a fake TV tree.
#
#   sh test/watch/run.sh [shell ...]     (default: sh, and dash when present)
#
# Uses test/session/fixtures (real C5 status files). Each case builds a fake
# tree under DTS25_ROOT and a timeline: the sleep stub moves the input to the
# timeline's next state on every "sleep 1" and, once the timeline is used up,
# sends TERM to its parent, which is the watcher (sleep 1 & wait). Stubs for
# id, amixer, systemctl, lsmod, rmmod, dmesg, tee and sleep come first in PATH
# and append one line per action to an event log; a state change appends
# "-- <state>". The tee stub applies "name value" to the fake prop file. The
# sleep stub also records each length in $DTS25_ROOT/sleeps, and cat and sed
# count the reads of the HDMI status file in $DTS25_ROOT/hdmi-reads.
# shellcheck disable=SC2016 # the stub bodies expand when the stubs run
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../../hdmi-watch.sh
FIX=$HERE/../session/fixtures
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
shells=${*:-sh $(command -v dash || true)}

# --- stubs -----------------------------------------------------------------
BIN=$WORK/bin
mkdir -p "$BIN"
stub() { printf '#!/bin/sh\n%s\n' "$2" > "$BIN/$1"; chmod +x "$BIN/$1"; }
stub id 'echo "${FAKE_UID:-0}"'
stub amixer 'R=$DTS25_ROOT
[ "$3" = numid=92 ] || { echo "amixer: unexpected $*" >&2; exit 1; }
if [ "$2" = cget ]; then
  sed "s/values=2,1,0,0,1/values=${FAKE_EARC:-2,1,0,0,1}/" "$FAKE_FIX/amixer-cget-numid92.txt"
  exit 0
fi
echo "cset numid=92 $4" >> "$FAKE_EVENTS"
# The route of the renderer whose decoder plays the current state (fake-state).
[ "${FAKE_ROUTE:-yes}" = no ] || { cat "$FAKE_FIX/dot_block_connection"
  printf "%s\n" " renderer_$(cat "$R/renderer")_out100 -> mixer_0_in100 [label=\"\"]" " mixer_0_out100 -> output_3_in0 [label=\"\"]"; } > "$R/proc/aaudio/dot_block_connection"'
# Only logged: the watcher must never call it (expect_clean).
stub systemctl 'echo "systemctl $*" >> "$FAKE_EVENTS"'
stub lsmod 'echo "Module Size Used by"; [ ! -e "$DTS25_ROOT/sys/module/earc_route" ] || echo "earc_route 16384 0"'
stub rmmod 'echo "rmmod $*" >> "$FAKE_EVENTS"
[ "${FAKE_RMMOD:-ok}" = ok ] || { echo "rmmod: ERROR: Module $1 is in use" >&2; exit 1; }
rm -rf "$DTS25_ROOT/sys/module/$1"'
stub dmesg 'echo "[ 1.0] earc_route: fake kernel line"'
for tool in cat sed; do
    stub "$tool" 'for a; do [ "$a" != "$DTS25_ROOT/proc/lgtv-driver/audio/hdmi" ] || echo x >> "$DTS25_ROOT/hdmi-reads"; done
exec '"$(command -v "$tool")"' "$@"'
done
stub tee 'line=$(cat); f=$1; echo "write ${f#$DTS25_ROOT} $line" >> "$FAKE_EVENTS"
name=${line%% *}; value=${line#* }
sed -i.bak "s/^$name=\(([a-z]*)\).*/$name=\1$value/" "$f"'
# fake-state STATE: rewrites the input and both decoders. A DTS stream starts
# adec.p1 on its own (the module's DTS check is open), unless FAKE_ADEC is
# "refused" (the TV refused the first stream: only DTS after a non-DTS state
# starts) or "never".
# FAKE_LG=1: a non-DTS state rewrites the DSP instance's codec, as LG does.
# p0dts, p0hd: the stream on adec.p0, adec.p1 disconnected. c5: the stream on
# adec.p0 and a stale adec.p1 on port 2, as on the C5. stalep0 and both: the
# stream on adec.p1 with adec.p0 on port 2, stopped on MAT or started on DTS.
# $R/renderer is the renderer of the decoder that plays: 0 for p0dts, p0hd and c5.
stub fake-state 'set -f; R=$DTS25_ROOT; A=$R/proc/lgtv-driver/audio
[ "$(cat "$R/state" 2>/dev/null)" = "$1" ] || echo "-- $1" >> "$FAKE_EVENTS"
echo "$1" > "$R/state"
case "$1" in
  dts|p0dts|c5|stalep0|both) c=UNKNOWN_DTS r=48000 u=DTS ;;
  hd|p0hd) c=DTS_HD r=192000 u=DTS_HD ;;
  pcm) c=PCM r=48000 u=PCM ;;
  mat) c=MAT r=192000 u=MAT ;;
  noaudio) c=NO_AUDIO r=48000 u=UNKNOWN ;;
  *) echo "fake-state: unknown $1" >&2; exit 1 ;;
esac
s=1
case "$u" in
  DTS*) case "${FAKE_ADEC:-auto}" in never) s=0 ;; refused) [ -e "$R/dropped" ] || s=0 ;; esac ;;
  *) touch "$R/dropped"
     [ "$u" != UNKNOWN ] || s=0
     [ -z "${FAKE_LG:-}" ] || sed -i.bak "s/^codec=(int).*/codec=(int)4/" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop" ;;
esac
sed -e "s/^Codec=.*/Codec=$c/" -e "s/^SamplingRate=.*/SamplingRate=$r/" "$FAKE_FIX/hdmi" > "$A/hdmi"
p1="-e s/^Connect=.*/Connect=HDMI_PORT0/ -e s/^Open=.*/Open=1/ -e s/^UserCodec=.*/UserCodec=$u/ -e s/^Start=.*/Start=$s/"
p0="" renderer=1
case "$1" in
  p0dts|p0hd) renderer=0 p0=$p1; p1="-e s/^Connect=.*/Connect=UNKNOWN/ -e s/^Open=.*/Open=0/ -e s/^Start=.*/Start=0/" ;;
  c5) renderer=0 p0=$p1; p1="-e s/^Connect=.*/Connect=HDMI_PORT2/ -e s/^Open=.*/Open=1/ -e s/^UserCodec=.*/UserCodec=UNKNOWN/ -e s/^Start=.*/Start=0/" ;;
  stalep0) p0="-e s/^Connect=.*/Connect=HDMI_PORT2/ -e s/^Open=.*/Open=1/ -e s/^UserCodec=.*/UserCodec=MAT/ -e s/^Start=.*/Start=0/" ;;
  both) p0="-e s/^Connect=.*/Connect=HDMI_PORT2/ -e s/^Open=.*/Open=1/ -e s/^UserCodec=.*/UserCodec=DTS/ -e s/^Start=.*/Start=1/" ;;
esac
sed $p0 -e "" "$FAKE_FIX/adec.p0" > "$A/adec.p0"
sed $p1 -e "" "$FAKE_FIX/adec.p1" > "$A/adec.p1"
echo "$renderer" > "$R/renderer"'
# "sleep 2" is the DTS-HD muted wait: logged, and earc_nonpcm becomes the next
# word of FAKE_NONPCM (default 1). Any other sleep takes the next timeline state.
stub sleep 'R=$DTS25_ROOT
if [ "$1" = 2 ]; then
  echo "sleep 2" >> "$FAKE_EVENTS"
  [ -f "$R/nonpcm" ] || echo "${FAKE_NONPCM:-1}" > "$R/nonpcm"
  set -- $(cat "$R/nonpcm"); v=${1:-1}; [ "$#" = 0 ] || shift; echo "$*" > "$R/nonpcm"
  echo "$v" > "$R/sys/module/earc_route/parameters/earc_nonpcm"
  exit 0
fi
echo "$1" >> "$R/sleeps"
if [ -s "$R/timeline" ]; then
  next=$(head -n 1 "$R/timeline"); sed -i.bak 1d "$R/timeline"; fake-state "$next"
  exit 0
fi
n=$(( $(cat "$R/after-end" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$R/after-end"
[ "$n" != 1 ] || kill -TERM "$PPID"
[ "$n" -lt 50 ] || kill -KILL "$PPID"'
LOADER='echo "load $*" >> "$FAKE_EVENTS"
if [ "${FAKE_LOADER:-ok}" = refuse ]; then
  echo "earc-route: REFUSED: another DTS tool is loaded: dts_gate (unload it first)"; exit 1
fi
P=$DTS25_ROOT/sys/module/earc_route/parameters
mkdir -p "$P"
for v in gate_open=1 selftest_passed=1 window_open=1 calls_seen=3 calls_matched=1 overrides=1 earc_nonpcm=0 \
         hd_probes_registered=2 hd_status_seen=4 hd_status_accepts=1 hd_name_seen=4 hd_name_fixes=1; do echo "${v#*=}" > "$P/${v%=*}"; done
echo "earc-route: PASSTHROUGH ACTIVE (mode=7 $*)"'

# --- one case --------------------------------------------------------------
P=/proc/aaudio/processors/dsp0/hdmi1/021c0001/prop
P0=/proc/aaudio/processors/dsp0/hdmi0/021d0001/prop
G=/proc/aaudio/processors/dsp2/gain/041b0001/prop
fail=0
cases=0
# setup FIRST [NEXT...]: eARC on, HDMI port 0, the input in state FIRST; the
# timeline is NEXT..., where "dts*3" is three dts states.
# FAKE_* settings are exported before setup and cleared by run.
setup() {
    R=$WORK/root
    rm -rf "$R" "$WORK/app"
    A=$R/proc/lgtv-driver/audio
    mkdir -p "$A" "$R/proc/aaudio/processors/dsp0/hdmi1/021c0001" "$R/proc/aaudio/processors/dsp0/hdmi0/021d0001" \
        "$R/proc/aaudio/processors/dsp2/gain/041b0001" "$R/tmp" "$WORK/app"
    sed 's/^EArcOnOff=.*/EArcOnOff=On/' "$FIX/sndout" > "$A/sndout"
    cp "$FIX/hdmi1.prop" "$R$P"
    # hdmi0 pairs with adec.p0: an idle instance on port 0 unless a case edits it.
    sed 's/^start=(int).*/start=(int)0/' "$FIX/hdmi1.prop" > "$R$P0"
    cp "$FIX/gain.prop" "$R/proc/aaudio/processors/dsp2/gain/041b0001/prop"
    cp "$FIX/dot_module_connection" "$FIX/dot_block_connection" "$R/proc/aaudio/"
    cp "$SCRIPT" "$WORK/app/hdmi-watch.sh"
    printf '#!/bin/sh\n%s\n' "$LOADER" > "$WORK/app/earc-route-load.sh"
    DTS25_ROOT=$R FAKE_EVENTS=/dev/null FAKE_FIX=$FIX "$BIN/fake-state" "$1"
    shift
    : > "$R/timeline"
    for s in "$@"; do
        case "$s" in *'*'*) n=${s#*\*} s=${s%\**} ;; *) n=1 ;; esac
        while [ "$n" -gt 0 ]; do echo "$s" >> "$R/timeline"; n=$((n-1)); done
    done
    : > "$WORK/events"
}
run() {  # sets status, out
    cp "$R$P" "$WORK/hdmi1.found"; cp "$R$P0" "$WORK/hdmi0.found"; cp "$R$G" "$WORK/gain.found"
    if out=$(DTS25_ROOT=$R FAKE_EVENTS=$WORK/events FAKE_FIX=$FIX PATH="$BIN:$PATH" \
             $SH "$WORK/app/hdmi-watch.sh" 2>&1); then status=0; else status=$?; fi
    unset FAKE_UID FAKE_EARC FAKE_ROUTE FAKE_ADEC FAKE_LOADER FAKE_NONPCM FAKE_LG FAKE_RMMOD
}
bad() { echo "FAIL [$SH] $name: $*"; echo "$out" | sed 's/^/    | /'; sed 's/^/    e /' "$WORK/events"; fail=1; }
expect_status() { [ "$status" = "$1" ] || bad "exit $status, expected $1"; }
expect_out() { case "$out" in *"$1"*) ;; *) bad "output lacks: $1" ;; esac; }
expect_events() { [ "$(cat "$WORK/events")" = "$1" ] || bad "events differ from expected:
$1"; }
count() { grep -c "$1" "$WORK/events" || true; }
# After every run: everything back as found, the module and the lock gone.
expect_clean() {
    cmp -s "$WORK/hdmi1.found" "$R$P" || bad "hdmi1 DSP prop not as found"
    cmp -s "$WORK/hdmi0.found" "$R$P0" || bad "hdmi0 DSP prop not as found"
    cmp -s "$WORK/gain.found" "$R$G" || bad "gain prop not as found"
    [ ! -e "$R/sys/module/earc_route" ] || bad "module still loaded"
    [ ! -e "$R/tmp/dts25-hdmi-watch.lock" ] || bad "lock left behind"
    case "$out" in *'Rollback incomplete'*) bad "rollback incomplete" ;; esac
    # The input service is never restarted; the stop says so when DTS is live.
    ! grep -q '^systemctl' "$WORK/events" || bad "input service restarted"
    if grep -q '^rmmod' "$WORK/events"; then
        case "$(cat "$R/state")" in
            dts|hd|p0dts|p0hd|c5|stalep0|both) expect_out "$LIVE" ;;
            *) case "$out" in *"$LIVE"*) bad "live-DTS notice without a live stream" ;; esac ;;
        esac
    fi
}
HINT='DTS is arriving but no decoder plays it (the TV refused it before the module loaded); skip, pause and play, or switch inputs and back to restart the stream.'
LIVE='DTS is still playing on HDMI; switch to another input and back, or the TV keeps the passthrough decoder state until the stream changes.'

LOAD='load --dts-hd window_seconds=0 supervised=1'
CSET='cset numid=92 2,1,0,0,1'
CORE="$CSET
write $G mute-input false
write $G mute-output false
write $P codec 5
write $P mode 1
write $P data-drop-mode 1
write $P drop-output-data false
write $P start 1
write $P drop 0"
MUTE="write $G mute-input true
write $G mute-output true"
UNMUTE="write $G mute-input false
write $G mute-output false"
CORE_RESTORE="write $P drop 1
write $P start 0
write $P codec 4
write $P mode 4
write $P data-drop-mode 2
write $P drop-output-data false
write $P start 1
write $P drop false
$UNMUTE"
HD="$CSET
$MUTE
write $P codec 6
write $P mode 4
write $P sampling-freq 192000
write $P channel-num 8
write $P data-drop-mode 1
write $P drop-output-data false
write $P start 1
write $P drop 0
sleep 2"
HD_RESTORE="write $P drop 1
write $P start 0
write $P codec 4
write $P mode 4
write $P sampling-freq 192000
write $P channel-num 2
write $P data-drop-mode 2
write $P drop-output-data false
write $P start 1
write $P drop false
$UNMUTE"
STOP="rmmod earc_route
$CSET"
# The same writes on hdmi0's instance, which starts a case stopped
# (start=(int)0) unless the case edits it.
CORE0=$(echo "$CORE" | sed "s|$P|$P0|")
CORE_RESTORE0=$(echo "$CORE_RESTORE" | sed -e "s|$P|$P0|" -e 's/start 1$/start 0/')
HD0=$(echo "$HD" | sed "s|$P|$P0|")
HD_RESTORE0=$(echo "$HD_RESTORE" | sed -e "s|$P|$P0|" -e 's/start 1$/start 0/')

for SH in $shells; do
    command -v "$SH" > /dev/null || { echo "SKIP shell $SH: not found"; continue; }

    name='steady DTS core, stopped while DTS is live'; cases=$((cases+1)); setup dts dts dts
    run; expect_status 0; expect_clean
    cmp -s "$FIX/hdmi1.prop" "$R$P" || bad "DSP prop is not the fixture"
    cmp -s "$FIX/gain.prop" "$R$G" || bad "gain prop is not the fixture"
    expect_events "$LOAD
$CORE
$MUTE
$CORE_RESTORE
$STOP"
    expect_out 'DTS core applied; overrides did not rise (1)'
    expect_out 'Watcher stopped: 1 applied, 1 reverted; logs: '
    logdir=$(ls -d "$R"/tmp/dts25-hdmi-watch-*)
    [ "$(ls -ld "$logdir" | cut -c1-10)" = drwx------ ] || bad "log dir is not mode 700"
    for f in watch.log loader.txt module.log original.prop original.gain route.dot; do
        [ -f "$logdir/$f" ] || bad "log lacks $f"
    done
    grep -q 'counters after apply: calls_seen=3 calls_matched=1 overrides=1 ' "$logdir/watch.log" || bad "no counters after apply"
    grep -q 'counters after revert: ' "$logdir/watch.log" || bad "no counters after revert"

    name='steady DTS core, stopped after the stream became PCM'; cases=$((cases+1)); setup dts dts pcm
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
-- pcm
$MUTE
$CORE_RESTORE
$CSET
$STOP"

    name='a one-poll DTS blip is not applied'; cases=$((cases+1)); setup pcm dts pcm pcm
    run; expect_status 0; expect_clean
    expect_events "$LOAD
-- dts
-- pcm
$STOP"

    name='seek: reverted on NO_AUDIO, applied again when DTS returns'; cases=$((cases+1))
    setup dts dts noaudio mat mat dts dts dts
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
-- noaudio
$MUTE
$CORE_RESTORE
$CSET
-- mat
-- dts
$CORE
$MUTE
$CORE_RESTORE
$STOP"

    name='DTS core on decoder 0: applied to hdmi0 and renderer 0'; cases=$((cases+1)); setup p0dts p0dts p0dts
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE0
$MUTE
$CORE_RESTORE0
$STOP"
    expect_out 'port=0 decoder=0'
    expect_out 'applying DTS core (port 0, decoder 0)'
    expect_out 'DTS core applied'
    [ -z "$(ls -d "$R"/tmp/dts25-hdmi-watch-*/decoder0-* 2>/dev/null)" ] || bad "decoder 0 capture written"

    name='DTS-HD on decoder 0: applied to hdmi0 and renderer 0'; cases=$((cases+1)); setup p0hd p0hd p0hd
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$HD0
$UNMUTE
$MUTE
$HD_RESTORE0
$STOP"
    expect_out 'DTS-HD applied'

    name='stream moves from decoder 1 to decoder 0 while applied: reverted, then applied to decoder 0'; cases=$((cases+1))
    setup dts dts p0dts p0dts
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
-- p0dts
$MUTE
$CORE_RESTORE
$CSET
$CORE0
$MUTE
$CORE_RESTORE0
$STOP"
    expect_out 'port=0 decoder=0); reverting'

    name='stream moves from decoder 0 to decoder 1 mid-stream: reverted on hdmi0, applied to hdmi1'; cases=$((cases+1))
    setup p0dts p0dts dts dts
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE0
-- dts
$MUTE
$CORE_RESTORE0
$CSET
$CORE
$MUTE
$CORE_RESTORE
$STOP"
    expect_out 'port=0 decoder=1); reverting'
    expect_out 'Watcher stopped: 2 applied, 2 reverted'

    name='core to DTS-HD: reverted, then the DTS-HD apply'; cases=$((cases+1)); setup dts dts hd hd hd
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
-- hd
$MUTE
$CORE_RESTORE
$CSET
$HD
$UNMUTE
$MUTE
$HD_RESTORE
$STOP"
    expect_out 'DTS-HD applied'

    name='DTS-HD with earc_nonpcm 0: reverted muted, retried only in a new stream'; cases=$((cases+1))
    FAKE_NONPCM='0 1'; export FAKE_NONPCM
    setup hd 'hd*3' noaudio 'hd*3'
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$HD
$HD_RESTORE
$CSET
-- noaudio
-- hd
$HD
$UNMUTE
$MUTE
$HD_RESTORE
$STOP"
    expect_out 'attempt failed (1 of 3): eARC is not non-PCM (earc_nonpcm=0); not retried until this stream ends'

    name='adec.p1 never starts on DTS: one failed attempt, no writes'; cases=$((cases+1))
    FAKE_ADEC=never; export FAKE_ADEC
    setup dts 'dts*20'
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$STOP"
    expect_out 'attempt failed (1 of 3): adec.p1 is not on UserCodec=DTS with Start=1; not retried until this stream ends'
    [ "$(printf '%s\n' "$out" | grep -c 'attempt failed')" = 1 ] || bad "more than one failed attempt"

    name='originals are saved once: LG rewrote the codec during the seek'; cases=$((cases+1))
    FAKE_LG=1; export FAKE_LG
    setup dts dts noaudio mat dts dts dts
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
-- noaudio
$MUTE
$CSET
-- mat
-- dts
$CORE
$MUTE
$CORE_RESTORE
$STOP"
    expect_out 'the DSP instance is no longer ours (LG rewrote it); left as it is'

    name='the loader refuses: nothing written, no lock'; cases=$((cases+1))
    FAKE_LOADER=refuse; export FAKE_LOADER
    setup dts dts
    run; expect_status 1; expect_clean
    expect_events "$LOAD"
    expect_out 'REFUSED: another DTS tool is loaded: dts_gate'

    name='earc_route already loaded: refused, left loaded'; cases=$((cases+1)); setup dts dts
    mkdir -p "$R/sys/module/earc_route"
    run; expect_status 1; expect_events ''
    expect_out 'REFUSED: earc_route is already loaded'
    [ -e "$R/sys/module/earc_route" ] || bad "unloaded a module it did not load"
    [ ! -e "$R/tmp/dts25-hdmi-watch.lock" ] || bad "lock left behind"

    name='not root: refused'; cases=$((cases+1))
    FAKE_UID=1000; export FAKE_UID
    setup dts dts
    run; expect_status 1; expect_events ''; expect_clean
    expect_out 'REFUSED: must run as root (uid 1000)'

    name='a second watcher is refused while the lock holder lives'; cases=$((cases+1)); setup dts dts
    mkdir "$R/tmp/dts25-hdmi-watch.lock"; echo "$$" > "$R/tmp/dts25-hdmi-watch.lock/pid"
    run; expect_status 1; expect_events ''
    expect_out "REFUSED: another hdmi-watch.sh is running (pid $$)"
    [ "$(cat "$R/tmp/dts25-hdmi-watch.lock/pid")" = "$$" ] || bad "took over a live lock"

    name='a stale lock is taken over'; cases=$((cases+1)); setup pcm pcm
    dead=$(sh -c 'echo $$'); mkdir "$R/tmp/dts25-hdmi-watch.lock"; echo "$dead" > "$R/tmp/dts25-hdmi-watch.lock/pid"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$STOP"

    name='three failed applies in a row: stopped, exit 1'; cases=$((cases+1))
    FAKE_ROUTE=no; export FAKE_ROUTE
    setup dts dts noaudio dts dts noaudio dts dts 'dts*5'
    run; expect_status 1; expect_clean
    expect_events "$LOAD
$CSET
-- noaudio
-- dts
$CSET
-- noaudio
-- dts
$CSET
$STOP"
    expect_out 'Three failed attempts in a row (last: the compressed route was not created (route.dot)); stopping.'

    name='C5: stream on adec.p0/hdmi0, stale adec.p1 on hdmi1 port 2: applied to decoder 0'; cases=$((cases+1))
    setup c5 c5 c5
    sed -i.bak -e 's/^hdmi-port=(int).*/hdmi-port=(int)2/' -e 's/^start=(int).*/start=(int)0/' -e 's/^codec=(int).*/codec=(int)0/' "$R$P"
    sed -i.bak 's/^start=(int).*/start=(int)1/' "$R$P0"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE0
$MUTE
$(echo "$CORE_RESTORE" | sed "s|$P|$P0|")
$STOP"
    expect_out 'port=0 decoder=0'

    name='stale adec.p0/hdmi0 while the stream plays on adec.p1: applied to decoder 1'; cases=$((cases+1))
    setup stalep0 stalep0 stalep0
    sed -i.bak -e 's/^hdmi-port=(int).*/hdmi-port=(int)2/' -e 's/^start=(int).*/start=(int)1/' "$R$P0"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$CORE
$MUTE
$CORE_RESTORE
$STOP"
    expect_out 'port=0 decoder=1'

    name='both decoders play DTS: not handled, nothing written'; cases=$((cases+1))
    setup both both both
    sed -i.bak 's/^hdmi-port=(int).*/hdmi-port=(int)2/' "$R$P0"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
$STOP"
    expect_out 'decoder=; decoders 0 and 1 both play an HDMI input; not handled'

    name='DTS refused before the load: the hint once, no failure; applied when the stream returns'; cases=$((cases+1))
    FAKE_ADEC=refused; export FAKE_ADEC
    setup dts 'dts*12' noaudio 'dts*3'
    sed -i.bak 's/^start=(int).*/start=(int)0/' "$R$P"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
-- noaudio
-- dts
$CORE
$MUTE
$(echo "$CORE_RESTORE" | sed 's/start 1$/start 0/')
$STOP"
    [ "$(printf '%s\n' "$out" | grep -c "$HINT")" = 1 ] || bad "the hint is not printed exactly once"
    case "$out" in *'attempt failed'*) bad "counted as a failed attempt" ;; esac

    name='no decoder playing in two streams: the hint once per stream'; cases=$((cases+1))
    FAKE_ADEC=never; export FAKE_ADEC
    setup dts 'dts*2' noaudio 'dts*3'
    sed -i.bak 's/^start=(int).*/start=(int)0/' "$R$P"
    run; expect_status 0; expect_clean
    expect_events "$LOAD
-- noaudio
-- dts
$STOP"
    [ "$(printf '%s\n' "$out" | grep -c "$HINT")" = 2 ] || bad "the hint is not printed once per stream"
    case "$out" in *'attempt failed'*) bad "counted as a failed attempt" ;; esac

    name='one read of the HDMI status file per poll'; cases=$((cases+1)); setup pcm 'pcm*5'
    run; expect_status 0; expect_clean
    # Six polls and the read at stop.
    [ "$(wc -l < "$R/hdmi-reads" | tr -d ' ')" = 7 ] || bad "$(wc -l < "$R/hdmi-reads" | tr -d ' ') reads of the HDMI status file, expected 7"

    name='idle after 60 polls without DTS: polled every 3 s, every second again on DTS'; cases=$((cases+1))
    setup pcm 'pcm*61' 'dts*3'
    run; expect_status 0; expect_clean
    expect_out 'DTS core applied'
    want=$(i=0; while [ "$i" -lt 59 ]; do echo 1; i=$((i+1)); done; echo 3; echo 3; echo 3; echo 1; echo 1; echo 1)
    [ "$(cat "$R/sleeps")" = "$want" ] || bad "poll intervals: $(tr '\n' ' ' < "$R/sleeps")"

    name='rmmod fails: rollback incomplete, exit 20'; cases=$((cases+1))
    FAKE_RMMOD=fail; export FAKE_RMMOD
    setup pcm pcm
    run; expect_status 20
    expect_out 'Rollback incomplete; restart the TV.'
    [ ! -e "$R/tmp/dts25-hdmi-watch.lock" ] || bad "lock left behind"
done

[ "$fail" = 0 ] || { echo "hdmi-watch test FAILED"; exit 1; }
echo "hdmi-watch test passed: $cases cases ($shells)"
