#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# hdmi-watch.sh -- keeps DTS passthrough (DTS core or DTS-HD) from an HDMI
# input to an eARC receiver going across seeks and input switches, on a rooted
# LG webOS 25 TV. It has only run on a C5 without an eARC receiver. On a
# receiver TV, start with the receiver's volume low.
#
#   sh hdmi-watch.sh                                 foreground; Ctrl-C stops it
#   nohup sh hdmi-watch.sh > /dev/null 2>&1 &        background; kill <pid> stops it
#
# It loads earc_route once (earc-route-load.sh --dts-hd window_seconds=0
# supervised=1), so the DTS check is open before a stream arrives, and polls
# once a second. Decoder N plays the input only if its own dsp0/hdmiN instance
# is on the port adec.pN connects and one of the two is started. A DTS stream
# on decoder N that holds for two polls gets the route hdmi-session.sh builds
# (same writes, same order, on hdmiN and renderer N); a change of the stream,
# the port or the decoder reverts it at once. TERM, INT or HUP
# reverts, unloads the module and exits 0. Exit 1: refused, or three attempts
# in a row failed. After a minute with no DTS it polls every 3 s instead: each
# read of the TV's HDMI status file adds three lines to the kernel log.
#
# It never restarts the input service: on a C5 a restart left one HDMI input
# playing another's audio until a TV restart. With the module loaded first, the
# C5's decoder started on DTS by itself, also after a seek or an input switch.
# A stream already playing when the module loads was refused by the TV; it
# needs a skip, pause and play, or an input switch and back. A DTS stream still
# live at stop keeps its decoder state until the input changes. Exit 20: the rollback is incomplete. Logs go to
# /tmp/dts25-hdmi-watch-<epoch>-<pid>/.
#
# The helpers are copied from hdmi-session.sh. DTS25_ROOT prefixes every
# absolute path; it is empty on the TV and names a fake tree in
# test/watch/run.sh.
set -eu

[ "$#" = 0 ] || {
    echo 'usage: sh hdmi-watch.sh   (runs in the foreground until TERM, INT or HUP)'
    exit 2
}
HERE=$(cd "$(dirname "$0")" && pwd)
R=${DTS25_ROOT:-}
AUDIO=$R/proc/lgtv-driver/audio
AAUDIO=$R/proc/aaudio
PARAMS=$R/sys/module/earc_route/parameters
LOCK=$R/tmp/dts25-hdmi-watch.lock
COUNTERS='calls_seen calls_matched overrides hd_status_seen hd_status_accepts hd_name_seen hd_name_fixes earc_nonpcm'

value() {  # value FILE KEY: what follows the first "KEY=" line
    sed -n "s/^$2=//p" "$1" | head -n 1
}
field() {  # field TEXT KEY: the same, from text already read
    printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -n 1
}
# tee, not a redirection, only so that the host test can stub the write.
put() {  # put FILE 'name value'
    printf '%s\n' "$2" | tee "$1" > /dev/null
}
instance() {  # instance N: the one dsp0/hdmiN DSP instance; hdmiN pairs with adec.pN
    set -- "$AAUDIO"/processors/dsp0/hdmi"$1"/*/prop
    [ "$#" = 1 ] && [ -f "$1" ] || return 1
    printf '%s\n' "$1"
}
earc_control() {
    amixer -c0 cget numid=92 | sed -n 's/^  : values=//p'
}
gain_ids() {  # gain modules whose output feeds a bypass module (see hdmi-session.sh)
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
saved() {  # saved FILE NAME: the value of NAME in a saved prop file
    sed -n "s/^$2=([^)]*)//p" "$1"
}
logdir=''
say() {
    line="$(date +%H:%M:%S) $*"
    echo "$line"
    [ -z "$logdir" ] || echo "$line" >> "$logdir/watch.log"
}
counters() {
    for name in $COUNTERS; do printf '%s=%s ' "$name" "$(cat "$PARAMS/$name" 2>/dev/null)"; done
}
# An interruptible sleep: a TERM during it runs the trap at once.
nap() { sleep "$1" & wait $!; }

# Sets kind (core, hd, none), dec (1, 0 or empty, with decnote saying why when
# two decoders qualify), the chosen decoder's instance prop and port, and seen.
read_input() {
    h=$(cat "$AUDIO/hdmi") || h=''  # read once: every read adds three lines to the kernel log
    seen="AudioData=$(field "$h" AudioData) Codec=$(field "$h" Codec) SamplingRate=$(field "$h" SamplingRate)"
    case "$seen" in
        'AudioData=1 Codec=DTS SamplingRate=48000'|'AudioData=1 Codec=UNKNOWN_DTS SamplingRate=48000') kind=core ;;
        'AudioData=1 Codec=DTS_HD SamplingRate=192000'|'AudioData=1 Codec=UNKNOWN_DTS_HD SamplingRate=192000') kind=hd ;;
        *) kind=none ;;
    esac
    # Re-resolved on every poll: LG can recreate an instance. A C5 has shown
    # adec.p1 still connected and open on another port's idle instance while
    # adec.p0 played, so a decoder counts only if it or its instance is started.
    found='' prop1='' port1='' prop0='' port0=''
    for n in 1 0; do
        p=$(instance "$n") || continue
        pt=$(sed -n 's/^hdmi-port=(int)//p' "$p")
        case "$pt" in ''|*[!0-9]*) continue ;; esac
        grep -qx "Connect=HDMI_PORT$pt" "$AUDIO/adec.p$n" || continue
        grep -qx 'Start=1' "$AUDIO/adec.p$n" || grep -qx 'start=(int)1' "$p" || continue
        found="$found$n"
        if [ "$n" = 1 ]; then prop1=$p port1=$pt; else prop0=$p port0=$pt; fi
    done
    decnote=''
    case "$found" in
        1|0) dec=$found ;;
        10)
            want=none
            [ "$kind" != core ] || want=DTS
            [ "$kind" != hd ] || want=DTS_HD
            if grep -qx "UserCodec=$want" "$AUDIO/adec.p1"; then
                if grep -qx "UserCodec=$want" "$AUDIO/adec.p0"; then dec=''; else dec=1; fi
            elif grep -qx "UserCodec=$want" "$AUDIO/adec.p0"; then dec=0
            else dec=''
            fi
            [ -n "$dec" ] || decnote='decoders 0 and 1 both play an HDMI input; not handled' ;;
        *) dec='' ;;
    esac
    prop='' port=''
    if [ "$dec" = 1 ]; then prop=$prop1 port=$port1; elif [ "$dec" = 0 ]; then prop=$prop0 port=$port0; fi
    seen="$seen port=$port decoder=$dec"
}
kind_vars() {  # kind_vars KIND
    if [ "$1" = core ]; then
        label='DTS core' adec_codec=DTS dsp_codec=5
        restore='codec mode data-drop-mode drop-output-data start drop'
    else
        label=DTS-HD adec_codec=DTS_HD dsp_codec=6
        restore='codec mode sampling-freq channel-num data-drop-mode drop-output-data start drop'
    fi
}
adec_started() {  # adec.p$d, the decoder apply works on
    grep -qx "UserCodec=$adec_codec" "$AUDIO/adec.p$d" && grep -qx 'Start=1' "$AUDIO/adec.p$d"
}
started_within() {  # started_within SECONDS
    i=0
    until adec_started; do
        [ "$i" -lt "$1" ] || return 1
        i=$((i+1)); nap 1
    done
}

# State of the watcher. Applied state (a_*) is what revert undoes.
loaded=0 applied=0 touched=0 unmuted=0 incomplete=0
a_kind='' a_dec='' a_port='' a_prop='' a_dsp='' a_restore='' gain=''
saved_prop='' saved_gain=''
cand='' polls=0 fails=0 backoff=0 hinted=0 last='' quiet=0 interval=1
applies=0 reverts=0

# 0 applied; 1 failed (why says why; counts towards the three); 2 the stream
# changed under it (not counted). The caller reverts whatever was touched.
apply() {
    k=$kind d=$dec
    kind_vars "$k"
    say "applying $label (port $port, decoder $d)"
    started_within 3 || { why="adec.p$d is not on UserCodec=$adec_codec with Start=1"; return 1; }
    read_input
    { [ "$kind" = "$k" ] && [ "$dec" = "$d" ]; } || return 2

    # Originals are taken once per run, from the first instance we write. LG
    # itself sets the instance's codec to the input's (codec 4 for MAT in the
    # C5 fixture), so codec 5 or 6 does not mean the values are ours. A new
    # instance is one we never wrote, so its values are LG's.
    if [ "$prop" != "$saved_prop" ]; then
        cp "$prop" "$logdir/original.prop" || { why='could not save the DSP instance'; return 1; }
        saved_prop=$prop
        say "saved originals of $prop"
    fi

    before=$(cat "$PARAMS/overrides" 2>/dev/null || echo '?')
    n92=$(earc_control)
    amixer -c0 cset numid=92 "$n92" > /dev/null || { why='the numid 92 write failed'; return 1; }
    cat "$AAUDIO/dot_block_connection" > "$logdir/route.dot"
    { grep -q "renderer_${d}_out100 -> mixer_0_in100" "$logdir/route.dot" &&
      grep -q 'mixer_0_out100 -> output_3_in0' "$logdir/route.dot"; } || {
        why='the compressed route was not created (route.dot)'; return 1; }
    gain=$(find_gain) || {
        why="expected exactly one gain block feeding a bypass block (found: $(gain_ids | tr '\n' ' '))"; return 1; }
    if [ "$gain" != "$saved_gain" ]; then
        for name in mute-input mute-output; do
            case "$(saved "$gain" "$name")" in true|false) ;; *) why="unexpected $name in $gain"; return 1 ;; esac
        done
        cp "$gain" "$logdir/original.gain" || { why='could not save the gain block'; return 1; }
        saved_gain=$gain
    fi

    touched=1 a_kind=$k a_dec=$d a_port=$port a_prop=$prop a_dsp=$dsp_codec a_restore=$restore
    if [ "$k" = core ]; then
        unmuted=1
        put "$gain" 'mute-input false' && put "$gain" 'mute-output false' &&
        put "$prop" 'codec 5' && put "$prop" 'mode 1' && put "$prop" 'data-drop-mode 1' &&
        put "$prop" 'drop-output-data false' && put "$prop" 'start 1' && put "$prop" 'drop 0' ||
            { why='a write failed'; return 1; }
    else
        put "$gain" 'mute-input true' && put "$gain" 'mute-output true' &&
        put "$prop" 'codec 6' && put "$prop" 'mode 4' && put "$prop" 'sampling-freq 192000' &&
        put "$prop" 'channel-num 8' && put "$prop" 'data-drop-mode 1' &&
        put "$prop" 'drop-output-data false' && put "$prop" 'start 1' && put "$prop" 'drop 0' ||
            { why='a write failed'; return 1; }
        # Stay muted until the asynchronous callback configures non-PCM transport.
        nap 2
        read_input
        [ "$kind" = hd ] || return 2
        transport=$(cat "$PARAMS/earc_nonpcm")
        [ "$transport" = 1 ] || { why="eARC is not non-PCM (earc_nonpcm=$transport)"; return 1; }
        unmuted=1
        put "$gain" 'mute-input false' && put "$gain" 'mute-output false' || { why='a write failed'; return 1; }
    fi
    applied=1 applies=$((applies+1))
    after=$(cat "$PARAMS/overrides" 2>/dev/null || echo '?')
    echo "counters after apply: $(counters)" >> "$logdir/watch.log"
    if [ "$after" = "$before" ]; then
        say "$label applied; overrides did not rise ($after)"
    else
        say "$label applied; overrides $before -> $after"
    fi
}

# revert [stop]: undo what apply touched. With "stop" the caller writes
# numid 92 itself, after the module is gone.
revert() {
    [ "$touched" = 1 ] || return 0
    rfail=0
    if [ "$unmuted" = 1 ]; then
        put "$gain" 'mute-input true' || rfail=1
        put "$gain" 'mute-output true' || rfail=1
    fi
    p=$a_prop
    # As in hdmi-session.sh's cleanup: only the instance we wrote, still on our
    # port and still holding our codec, is ours to restore.
    if [ -f "$p" ] && grep -qx "hdmi-port=(int)$a_port" "$p" && grep -qx "codec=(int)$a_dsp" "$p"; then
        put "$p" 'drop 1' || rfail=1
        put "$p" 'start 0' || rfail=1
        for name in $a_restore; do
            v=$(saved "$logdir/original.prop" "$name")
            [ -z "$v" ] || put "$p" "$name $v" || rfail=1
        done
        for name in mute-input mute-output; do
            put "$gain" "$name $(saved "$logdir/original.gain" "$name")" || rfail=1
        done
    else
        say 'the DSP instance is no longer ours (LG rewrote it); left as it is'
    fi
    if [ "${1:-}" != stop ]; then
        n92=$(earc_control)
        amixer -c0 cset numid=92 "$n92" > /dev/null || rfail=1
    fi
    applied=0 touched=0 unmuted=0 reverts=$((reverts+1))
    echo "counters after revert: $(counters)" >> "$logdir/watch.log"
    [ "$rfail" = 0 ] || { incomplete=1; say 'revert: a write failed'; return 1; }
    say 'reverted'
}

poll() {
    read_input
    [ "$kind" != none ] || backoff=0 hinted=0
    # Idle after 60 polls with no DTS and nothing applied; any DTS polls every second again.
    if [ "$kind" = none ] && [ "$applied" = 0 ]; then
        [ "$quiet" -ge 60 ] || quiet=$((quiet+1))
    else
        quiet=0
    fi
    if [ "$quiet" -ge 60 ]; then interval=3; else interval=1; fi
    if [ "$applied" = 1 ] && { [ "$kind" != "$a_kind" ] || [ "$dec" != "$a_dec" ] || [ "$port" != "$a_port" ] || [ "$prop" != "$a_prop" ]; }; then
        say "input changed ($seen); reverting"
        revert || { say 'Revert failed; stopping.'; exit 20; }
    fi
    note=''
    if [ "$applied" = 0 ] && [ "$kind" != none ]; then
        if [ -n "$decnote" ]; then note=$decnote
        elif [ "$(value "$AUDIO/sndout" EArcOnOff)" != On ]; then note='eARC is off'
        elif [ "$backoff" = 1 ]; then note='not retried until this stream ends'
        else
            n92=$(earc_control)
            [ "$n92" = 2,1,0,0,1 ] || note="numid 92 is '$n92', expected 2,1,0,0,1"
        fi
        if [ -z "$note" ] && [ -z "$dec" ]; then
            note='no decoder is playing the HDMI input'
            [ "$hinted" = 1 ] || { hinted=1; say 'DTS is arriving but no decoder plays it (the TV refused it before the module loaded); skip, pause and play, or switch inputs and back to restart the stream.'; }
        fi
    fi
    [ "$seen${note:+; $note}" = "$last" ] || { last="$seen${note:+; $note}"; say "input: $last"; }
    if [ "$applied" = 1 ] || [ "$kind" = none ] || [ -n "$note" ]; then cand='' polls=0; return 0; fi
    if [ "$kind" = "$cand" ]; then polls=$((polls+1)); else cand=$kind polls=1; fi
    [ "$polls" -ge 2 ] || return 0
    cand='' polls=0
    if apply; then fails=0; return 0; else r=$?; fi
    revert || { say 'Revert failed; stopping.'; exit 20; }
    [ "$r" = 1 ] || { say 'the input changed during the apply'; return 0; }
    failed
}
failed() {  # counts the attempt that failed for the reason in why
    fails=$((fails+1)) backoff=1
    say "attempt failed ($fails of 3): $why; not retried until this stream ends"
    [ "$fails" -lt 3 ] || { say "Three failed attempts in a row (last: $why); stopping."; exit 1; }
}

cleanup() {
    status=$?
    trap '' HUP INT TERM
    trap - EXIT
    set +e
    if [ "$loaded" = 1 ]; then
        revert stop || incomplete=1
        echo "counters at stop: $(counters)" >> "$logdir/watch.log"
        read_input
        rmmod earc_route || incomplete=1
        [ -z "$(lsmod | awk '$1=="earc_route" {print $1}')" ] || incomplete=1
        n92=$(earc_control)
        [ -n "$n92" ] && amixer -c0 cset numid=92 "$n92" > /dev/null || incomplete=1
        [ "$kind" = none ] || say 'DTS is still playing on HDMI; switch to another input and back, or the TV keeps the passthrough decoder state until the stream changes.'
        dmesg | grep 'earc_route:' > "$logdir/module.log"
    fi
    rm -rf "$LOCK"
    say "Watcher stopped: $applies applied, $reverts reverted${logdir:+; logs: $logdir}"
    [ "$incomplete" = 0 ] || { echo 'Rollback incomplete; restart the TV.'; exit 20; }
    exit "$status"
}

uid=$(id -u)
[ "$uid" = 0 ] || { echo "REFUSED: must run as root (uid $uid). Nothing changed."; exit 1; }
if ! mkdir "$LOCK" 2>/dev/null; then
    pid=$(cat "$LOCK/pid" 2>/dev/null || true)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        echo "REFUSED: another hdmi-watch.sh is running (pid $pid). Nothing changed."; exit 1
    fi
    rm -rf "$LOCK"
    mkdir "$LOCK"
fi
echo "$$" > "$LOCK/pid"
trap cleanup EXIT
trap 'exit 0' HUP INT TERM

[ -z "$(lsmod | awk '$1=="earc_route" {print $1}')" ] || {
    echo 'REFUSED: earc_route is already loaded (another DTS tool or session). Nothing changed.'; exit 1; }
logdir="$R/tmp/dts25-hdmi-watch-$(date +%s)-$$"
mkdir -m 700 "$logdir"
if sh "$HERE/earc-route-load.sh" --dts-hd window_seconds=0 supervised=1 > "$logdir/loader.txt" 2>&1; then load_status=0; else load_status=$?; fi
cat "$logdir/loader.txt"
# The loader refuses when earc_route is already loaded, so a loaded module is ours.
[ ! -e "$R/sys/module/earc_route" ] || loaded=1
[ "$load_status" = 0 ] || { say 'The loader did not load the module; stopping.'; exit 1; }
say "watching the HDMI input; stop with TERM, INT or HUP (logs: $logdir)"
while :; do
    poll
    nap "$interval"
done
