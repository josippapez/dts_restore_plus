#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# hdmi-session.sh -- one DTS passthrough session (DTS core or DTS-HD, whichever
# the HDMI input carries) to an eARC receiver, on a rooted LG webOS 25 TV.
# --apply has run with a receiver on a CS (DTS core, DTS-HD, DTS:X).
#
#   sh hdmi-session.sh --check             read-only preflight, changes nothing
#   sh hdmi-session.sh --apply [seconds]   timed test, 15..60 seconds (default 30)
#   sh hdmi-session.sh --session           runs until the source changes (exit 10)
#
# Derived from tools/dts-core-route/run-on-tv.sh and
# tools/dts-hd-route/run-on-tv.sh of lg-g5-dts
# (https://github.com/dakenf/lg-g5-dts, GPL-2.0-only). Those scripts are pinned
# to one G5 image; this one reads the port, the decoder and the bypass gain
# block from the TV, loads earc_route (earc-route-load.sh --dts-hd, the stage
# that serves both kinds) in place of lg-g5-dts's modules, and reads the eARC
# non-PCM word from the module's earc_nonpcm parameter. Runs as root with
# busybox sh.
#
# DTS25_ROOT prefixes every absolute path; it is empty on the TV and names a
# fake tree in test/session/run.sh.
set -eu

usage() {
    echo 'DTS HDMI passthrough session (DTS core or DTS-HD). Nothing changed.'
    echo 'usage: sh hdmi-session.sh --check | --apply [15..60 seconds] | --session'
    echo '  --check    read-only: prints each precondition and a verdict'
    echo '  --apply    timed test, 30 seconds unless given, then restores'
    echo '  --session  runs until the source changes, then restores'
}
mode=${1:-}
case "$mode" in
    --check|--apply|--session) ;;
    '') usage; exit 0 ;;
    *) usage; exit 2 ;;
esac
seconds=${2:-30}
case "$seconds" in ''|*[!0-9]*) usage; exit 2 ;; esac
{ [ "$seconds" -ge 15 ] && [ "$seconds" -le 60 ]; } || { usage; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
R=${DTS25_ROOT:-}
AUDIO=$R/proc/lgtv-driver/audio
AAUDIO=$R/proc/aaudio
PARAMS=$R/sys/module/earc_route/parameters

value() {  # value FILE KEY: what follows the first "KEY=" line
    sed -n "s/^$2=//p" "$1" | head -n 1
}
# tee, not a redirection, only so that the host test can stub the write.
put() {  # put FILE 'name value'
    printf '%s\n' "$2" | tee "$1" > /dev/null
}
find_prop() { instance "$dec"; }
instance() {  # instance N: the one dsp0/hdmiN DSP instance; hdmiN pairs with adec.pN
    set -- "$AAUDIO"/processors/dsp0/hdmi"$1"/*/prop
    [ "$#" = 1 ] && [ -f "$1" ] || return 1
    printf '%s\n' "$1"
}
earc_control() {
    amixer -c0 cget numid=92 | sed -n 's/^  : values=//p'
}
# Ids of the gain modules whose output feeds a bypass module's input. On a C5
# the only such edge is renderer 1's compressed path (gain_041b0001 ->
# bypass_04160001, the block lg-g5-dts names); every other bypass module is fed
# by a queue or a glue module.
gain_ids() {
    sed -n 's/^ *gain_\([0-9a-f]*\)_[0-9a-f]*_out[0-9]* -> bypass_[0-9a-f]*_[0-9a-f]*_in[0-9]*;.*/\1/p' \
        "$AAUDIO/dot_module_connection" | sort -u
}
find_gain() {
    # shellcheck disable=SC2046 # one word per id
    set -- $(gain_ids)
    [ "$#" = 1 ] || return 1
    set -- "$AAUDIO"/processors/dsp*/gain/"$1"/prop
    [ "$#" = 1 ] && [ -f "$1" ] || return 1
    printf '%s\n' "$1"
}
stream_now() {
    grep -qx 'AudioData=1' "$AUDIO/hdmi" &&
    grep -Eqx "Codec=($codecs)" "$AUDIO/hdmi" &&
    grep -qx "SamplingRate=$rate" "$AUDIO/hdmi"
}
core_now() {
    stream_now && grep -qx "Connect=HDMI_PORT$port" "$AUDIO/adec.p$dec"
}

# Every precondition, each with the value read. The first failure is the reason.
refusal=''
failures=0
ok() { echo "  ok    $1"; }
no() { echo "  FAIL  $1"; failures=$((failures+1)); [ -n "$refusal" ] || refusal=$1; }
adec() {  # adec N: the decoder's fields, for the report only
    echo "adec.p$1 Connect=$(value "$AUDIO/adec.p$1" Connect) Open=$(value "$AUDIO/adec.p$1" Open) Start=$(value "$AUDIO/adec.p$1" Start) UserCodec=$(value "$AUDIO/adec.p$1" UserCodec)"
}

uid=$(id -u)
if [ "$uid" = 0 ]; then ok "running as root"; else no "must run as root (uid $uid)"; fi

# The stream decides the kind of session. LG's HDMI code reports a DTS-HD
# stream as type 202 or 1202, which its name table prints as DTS_HD or
# UNKNOWN_DTS_HD (read from the 5.4 kernel images; lg-g5-dts saw DTS_HD on a
# G5). Which one a C5 prints is unmeasured: its core stream reads UNKNOWN_DTS.
# The DTS_HD_HRA names (types 203, 1203) are a stream lg-g5-dts does not handle.
kind=core codecs='DTS|UNKNOWN_DTS' rate=48000 label='DTS core' adec_codec=DTS dsp_codec=5
restore='codec mode data-drop-mode drop-output-data start drop'
counters='gate_open selftest_passed window_open calls_seen calls_matched overrides hd_probes_registered hd_status_seen hd_status_accepts hd_name_seen hd_name_fixes'
seen="AudioData=$(value "$AUDIO/hdmi" AudioData) Codec=$(value "$AUDIO/hdmi" Codec) SamplingRate=$(value "$AUDIO/hdmi" SamplingRate)"
case "$seen" in
    'AudioData=1 Codec=DTS SamplingRate=48000'|'AudioData=1 Codec=UNKNOWN_DTS SamplingRate=48000')
        ok "live DTS core at 48 kHz on HDMI ($seen)" ;;
    'AudioData=1 Codec=DTS_HD SamplingRate=192000'|'AudioData=1 Codec=UNKNOWN_DTS_HD SamplingRate=192000')
        kind=hd codecs='DTS_HD|UNKNOWN_DTS_HD' rate=192000 label=DTS-HD adec_codec=DTS_HD dsp_codec=6
        restore='codec mode sampling-freq channel-num data-drop-mode drop-output-data start drop'
        ok "live DTS-HD at 192 kHz on HDMI ($seen)" ;;
    *) no "the HDMI input is not a live DTS stream this script handles ($seen)"
       echo '        accepted: Codec=DTS or UNKNOWN_DTS at 48000 (DTS core), Codec=DTS_HD or UNKNOWN_DTS_HD at 192000 (DTS-HD)' ;;
esac

seen=$(value "$AUDIO/sndout" EArcOnOff)
if [ "$seen" = On ]; then ok "eARC is on (EArcOnOff=$seen)"; else no "eARC is not on (EArcOnOff=$seen)"; fi

# Decoder N carries the input only if adec.pN is connected to the port of its
# own instance dsp0/hdmiN, and plays it if either of the two is started. A C5
# has shown adec.p1 open on port 2's idle hdmi1 while adec.p0 played port 0
# through hdmi0. A stock TV refuses a DTS stream, so nothing may play yet: then
# the one connected pair is used, which the input-service restart below
# starts. Sets connected, playing and found (the decoder, or empty).
detect() {
    connected='' playing='' found=''
    for n in 1 0; do
        p=$(instance "$n") || continue
        pt=$(sed -n 's/^hdmi-port=(int)//p' "$p")
        case "$pt" in ''|*[!0-9]*) continue ;; esac
        grep -qx "Connect=HDMI_PORT$pt" "$AUDIO/adec.p$n" || continue
        connected="$connected$n"
        if grep -qx 'Start=1' "$AUDIO/adec.p$n" || grep -qx 'start=(int)1' "$p"; then playing="$playing$n"; fi
    done
    if [ "$playing" = 10 ]; then
        if grep -qx "UserCodec=$adec_codec" "$AUDIO/adec.p1"; then
            grep -qx "UserCodec=$adec_codec" "$AUDIO/adec.p0" || playing=1
        elif grep -qx "UserCodec=$adec_codec" "$AUDIO/adec.p0"; then playing=0
        fi
    fi
    case "$playing" in 1|0) found=$playing ;; '') case "$connected" in 1|0) found=$connected ;; esac ;; esac
}
detect
dec=$found port='' prop=''
if [ -n "$dec" ]; then prop=$(instance "$dec"); port=$(sed -n 's/^hdmi-port=(int)//p' "$prop"); fi
seen="$(adec 0); $(adec 1)"
case "$dec$playing" in
    11|00) ok "one hdmi$dec DSP instance, hdmi-port $port ($prop)"; ok "HDMI port $port is on decoder $dec ($seen)" ;;
    1|0) ok "one hdmi$dec DSP instance, hdmi-port $port ($prop)"
         ok "HDMI port $port is connected to decoder $dec, not started yet; the input-service restart starts it ($seen)" ;;
    *) no "cannot tell which decoder carries the HDMI input (instances: $(echo "$AAUDIO"/processors/dsp0/hdmi*/*/prop); $seen)" ;;
esac

earc=$(earc_control)
# Only use the known ordinary pass-through setting; do not force a new codec.
if [ "$earc" = '2,1,0,0,1' ]; then ok "eARC control numid 92 is $earc"; else no "eARC control numid 92 is '$earc', expected 2,1,0,0,1"; fi

seen=$(gain_ids | tr '\n' ' ')
seen=${seen% }
case "$(gain_ids | wc -l | tr -d ' ')" in
    0) ok "no gain block feeds a bypass block yet; looked up again once the route exists" ;;
    1) ok "bypass gain block: gain_$seen (the compressed route itself is only checked during --apply or --session)" ;;
    *) no "more than one gain block feeds a bypass block ($seen); cannot tell which is the compressed path" ;;
esac

if [ "$failures" -gt 1 ]; then echo "REFUSED ($failures checks failed, first: $refusal). Nothing changed."; exit 1; fi
if [ -n "$refusal" ]; then echo "REFUSED: $refusal. Nothing changed."; exit 1; fi
if [ "$mode" = --check ]; then
    echo 'CHECK OK. Nothing changed. The loader runs its own checks (module ABI, derivation, other DTS tools) at --apply or --session.'
    exit 0
fi
session=0
[ "$mode" != --session ] || session=1

logdir="$R/tmp/dts25-hdmi-$(date +%s)-$$"
mkdir -m 700 "$logdir"
cp "$prop" "$logdir/original.prop"
cp "$AUDIO/sndout" "$logdir/original.sndout"
printf '%s\n' "$earc" > "$logdir/original.earc"
loaded=0
modified=0
gain_prop=''
gain_modified=0
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    set +e
    failed=0
    restore_input=0
    prop=$(find_prop) || prop=''
    # Source may disappear while our DSP instance still has its DTS override.
    # Quiesce that owned instance before removing the output-status hook.
    if [ "$modified" = 1 ] && [ -f "$prop" ] &&
       grep -qx "hdmi-port=(int)$port" "$prop" &&
       { core_now || grep -qx "codec=(int)$dsp_codec" "$prop"; }; then
        restore_input=1
        put "$prop" 'drop 1' || failed=1
        put "$prop" 'start 0' || failed=1
    fi
    if [ "$gain_modified" = 1 ] && [ "$restore_input" = 1 ] && [ -f "$gain_prop" ]; then
        for name in mute-input mute-output; do
            value=$(sed -n "s/^$name=([^)]*)//p" "$logdir/original.gain")
            put "$gain_prop" "$name $value" || failed=1
        done
    fi
    if [ "$restore_input" = 1 ]; then
        for name in $restore; do
            value=$(sed -n "s/^$name=([^)]*)//p" "$logdir/original.prop")
            [ -n "$value" ] && put "$prop" "$name $value"
        done
        sleep 1
    fi
    if [ "$loaded" = 1 ]; then
        for name in $counters; do
            echo "$name=$(cat "$PARAMS/$name")"
        done > "$logdir/counters.txt" 2>&1
        rmmod earc_route || failed=1
    fi
    if [ "$modified" = 1 ]; then
        # Let the driver calculate the new source's route from current controls.
        if ! core_now; then
            current_earc=$(earc_control)
            [ -z "$current_earc" ] || earc=$current_earc
        fi
        amixer -c0 cset numid=92 "$earc" > "$logdir/restore-earc.txt" 2>&1 || failed=1
        systemctl restart extinput-integration.service || failed=1
    fi
    if [ "$loaded" = 1 ] && [ -n "$(lsmod | awk '$1=="earc_route" {print $1}')" ]; then failed=1; fi
    dmesg | grep 'earc_route:' > "$logdir/module.log"
    cat "$AUDIO/sndout" > "$logdir/final.sndout"
    [ ! -f "$logdir/counters.txt" ] || echo "Module counters: $(tr '\n' ' ' < "$logdir/counters.txt")"
    echo "Test ended; logs: $logdir; cleanup_errors=$failed"
    [ "$failed" = 0 ] || { echo 'Rollback incomplete; inspect modules and restart TV before other tests.'; exit 1; }
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# One load opens the DTS check and arms the route hook, and the window starts
# now: it has to cover the ADEC wait below (20 s) as well as the test. A core
# session loads the DTS-HD stage too: it overrides both codecs and guards LG's
# codec-name load, which a source switching to a DTS-HD track would reach.
options="window_seconds=$((seconds+30))"
[ "$session" = 0 ] || options='window_seconds=0 supervised=1'
# shellcheck disable=SC2086 # options is a word list
if sh "$HERE/earc-route-load.sh" --dts-hd $options > "$logdir/loader.txt" 2>&1; then load_status=0; else load_status=$?; fi
cat "$logdir/loader.txt"
# The loader refuses when earc_route is already loaded, so a loaded module is
# ours, also when the loader failed after insmod (probe not registered).
[ ! -e "$R/sys/module/earc_route" ] || loaded=1
[ "$load_status" = 0 ] || { echo 'The loader did not start the session; aborting.'; exit 1; }
modified=1
systemctl restart extinput-integration.service
# Allow ordinary service startup to set the decoder's codec and create bypass
# nodes. The input reads as not DTS for a moment after the restart (a CS run
# aborted here on the first read), so only the last read decides. The decoder
# is detected again each time: the restart can move the input to the other
# decoder. The port must stay the same. On a CS the decoder started on DTS
# 9 s after the restart, so the wait is 20 s.
i=0
while [ "$i" -lt 20 ]; do
    detect
    if [ -n "$found" ] && stream_now && grep -qx "Connect=HDMI_PORT$port" "$AUDIO/adec.p$found" &&
       grep -qx "UserCodec=$adec_codec" "$AUDIO/adec.p$found" && grep -qx 'Start=1' "$AUDIO/adec.p$found"; then break; fi
    i=$((i+1)); sleep 1
done
if [ "$i" -ge 20 ]; then
    stream_now || { echo "$label input did not come back within 20 s of the input-service restart; aborting."; exit 1; }
    echo 'ADEC did not start; aborting.'; exit 1
fi
if [ "$found" != "$dec" ]; then
    # Nothing was written to the old decoder's instance; the new one's values
    # are what cleanup restores.
    echo "The input-service restart moved HDMI port $port from decoder $dec to decoder $found; using decoder $found."
    dec=$found
    cp "$(find_prop)" "$logdir/original.prop"
fi
prop=$(find_prop)
grep -qx "hdmi-port=(int)$port" "$prop"
# Create the downstream connection while the input is stopped.
amixer -c0 cset numid=92 "$earc" > "$logdir/trigger-earc.txt"
cat "$AAUDIO/dot_block_connection" > "$logdir/route-before-input.dot"
grep -q "renderer_${dec}_out100 -> mixer_0_in100" "$logdir/route-before-input.dot" || {
    echo 'Compressed route not created; aborting before input start.'; exit 1;
}
grep -q 'mixer_0_out100 -> output_3_in0' "$logdir/route-before-input.dot" || exit 1
# UNKNOWN decoder status otherwise leaves this gain muted despite a connected route.
cat "$AAUDIO/dot_module_connection" > "$logdir/modules-before-input.dot"
gain_prop=$(find_gain) || {
    echo "Expected exactly one gain block feeding a bypass block (found: $(gain_ids | tr '\n' ' ')); aborting before input start."; exit 1;
}
cp "$gain_prop" "$logdir/original.gain"
for name in mute-input mute-output; do
    value=$(sed -n "s/^$name=([^)]*)//p" "$logdir/original.gain")
    case "$value" in true|false) ;; *) echo 'Unexpected gain property'; exit 1;; esac
done
gain_modified=1
if [ "$kind" = core ]; then
    put "$gain_prop" 'mute-input false'
    put "$gain_prop" 'mute-output false'
    put "$prop" 'codec 5'
    put "$prop" 'mode 1'
    put "$prop" 'data-drop-mode 1'
    put "$prop" 'drop-output-data false'
    put "$prop" 'start 1'
    put "$prop" 'drop 0'
else
    put "$gain_prop" 'mute-input true'
    put "$gain_prop" 'mute-output true'
    put "$prop" 'codec 6'
    put "$prop" 'mode 4'
    put "$prop" 'sampling-freq 192000'
    put "$prop" 'channel-num 8'
    put "$prop" 'data-drop-mode 1'
    put "$prop" 'drop-output-data false'
    put "$prop" 'start 1'
    put "$prop" 'drop 0'
    # Stay muted until the asynchronous callback configures non-PCM transport.
    sleep 2
    core_now || exit 1
    transport=$(cat "$PARAMS/earc_nonpcm")
    printf '%s\n' "$transport" > "$logdir/earc-nonpcm.txt"
    [ "$transport" = 1 ] || { echo "eARC is not non-PCM (earc_nonpcm=$transport); aborting muted"; exit 1; }
    cat "$AAUDIO"/processors/dsp1/output_arc/*/prop > "$logdir/output-before-unmute.prop"
    put "$gain_prop" 'mute-input false'
    put "$gain_prop" 'mute-output false'
fi
if [ "$session" = 1 ]; then echo "$label supervised session active"; else
    echo "$label test running for $seconds seconds. Check the receiver's audio and format display."
fi
i=0
while { [ "$session" = 1 ] || [ "$i" -lt "$seconds" ]; }; do
    sleep 1
    core_now || { echo 'Input changed; ending test.'; [ "$session" = 0 ] && exit 1; exit 10; }
    i=$((i+1))
    sample=$i
    [ "$session" = 0 ] || sample=latest
    cat "${prop%/prop}/lstatus" > "$logdir/hdmi-$sample.txt"
    cat "$AUDIO/sndout" > "$logdir/sndout-$sample.txt"
    cat "$gain_prop" > "$logdir/gain-$sample.txt"
done
cat "$AAUDIO/dot_block_connection" > "$logdir/final.dot"
cat "$prop" > "$logdir/test.prop"
# Successful script execution is NOT proof of audible sound or DTS lock.
