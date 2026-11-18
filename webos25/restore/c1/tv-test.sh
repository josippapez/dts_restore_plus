#!/bin/sh
# tv-test.sh — try the webOS 6 dtsdec on an OLED C1/G1 WITHOUT installing it.
#
# Everything happens in /tmp/dtsc1 with a private plugin registry
# (GST_REGISTRY_1_0 points into /tmp). LG's registry, /etc, /usr and every
# LG service are untouched; a reboot removes all traces.
#
# Put these next to this script (same folder), e.g. /tmp/dtsc1:
#   libgstdtsdec.so  dtsc1-check.mkv (c1/testfiles/)  dtsc1-listen.mkv (optional)
#
# Usage:  sh tv-test.sh            steps 3+4 (silent: load + bit-exact check)
#         sh tv-test.sh listen     step 5: play to LG's audiosink as DTS
#                                  (turn the receiver volume DOWN first)
set -u
D=$(cd "$(dirname "$0")" && pwd)
W=/tmp/dtsc1
mkdir -p "$W"
export GST_REGISTRY_1_0=$W/registry.bin
export GST_PLUGIN_PATH_1_0=$D
export GST_REGISTRY_FORK=no
# dtsc1-check.mkv through `ffmpeg -c copy -f spdif` (the exact form the
# receiver already showed as DTS); also what qemu produced from this plugin.
REF_CHECK=5cdce24f2e4ca0cc72253786e40094e7cd9d5f23b6f6b2c42658d006bf4c3c33
REF_LISTEN=218fa6eed5b6b7ad2afd4977fa349d4e8bcd09f5aaa4d2cc8fa03660a569b74c

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; exit 1; }

for f in libgstdtsdec.so dtsc1-check.mkv; do
  [ -f "$D/$f" ] || fail "$f is missing in $D"
done
echo "plugin sha256: $(sha256sum "$D/libgstdtsdec.so" | cut -d' ' -f1)"

if [ "${1:-}" != listen ]; then
  echo "--- step 3: does the plugin load on this TV? (private registry in /tmp)"
  rm -f "$GST_REGISTRY_1_0"
  if gst-inspect-1.0 dtsdec > $W/inspect.txt 2> $W/inspect.err; then
    grep -E "Version|Rank|Filename" $W/inspect.txt
    pass "dtsdec loads with GStreamer $(gst-inspect-1.0 --version | sed -n 's/^GStreamer //p')"
  else
    cat $W/inspect.err | tail -n 5
    fail "dtsdec does not load"
  fi

  echo "--- step 4a: bit-exact IEC 61937 on this CPU (LG's matroskademux)"
  rm -f $W/pt.raw
  gst-launch-1.0 -q filesrc location="$D/dtsc1-check.mkv" ! matroskademux \
    ! dtsdec output=passthrough ! filesink location=$W/pt.raw 2> $W/pt.err \
    || { tail -n 5 $W/pt.err; fail "passthrough pipeline failed"; }
  got=$(sha256sum $W/pt.raw | cut -d' ' -f1)
  echo "$(wc -c < $W/pt.raw) bytes $got"
  [ "$got" = "$REF_CHECK" ] && pass "byte-identical to the reference" \
    || fail "output differs from the reference"

  echo "--- step 4b: decode fallback (TV speakers path) runs cleanly"
  gst-launch-1.0 -q filesrc location="$D/dtsc1-check.mkv" ! matroskademux \
    ! dtsdec output=decode ! fakesink 2> $W/dec.err \
    && pass "decoded to stereo PCM" || { tail -n 5 $W/dec.err; fail "decode failed"; }

  echo
  echo "Steps 3+4 passed. Next (audible): receiver volume DOWN, then"
  echo "  sh $0 listen"
  exit 0
fi

# --- step 5: through LG's own audio sink ------------------------------------
[ -f "$D/dtsc1-listen.mkv" ] || fail "dtsc1-listen.mkv is missing in $D"
echo "--- step 5: 60 s of DTS 5.1 to LG's audiosink as IEC 61937"
echo "    The receiver should switch to DTS. Ctrl+C stops it."
echo "    Sound output: $(luna-send -n 1 -w 3000 luna://com.webos.settingsservice/getSystemSettings \
  '{"category":"sound","keys":["soundOutput","soundOutputDigital"]}' </dev/null 2>/dev/null \
  | grep -o '"soundOutput[A-Za-z]*" *: *"[^"]*"' | tr -d ' "' | tr '\n' ' ')"
# In the background with a 20 s limit: outside LG's media service the sink
# may wait forever for its audio resource (seen on a C1). No busybox timeout:
# old busybox only knows `timeout -t`.
GST_DEBUG=3 gst-launch-1.0 filesrc location="$D/dtsc1-listen.mkv" ! matroskademux \
  ! dtsdec output=passthrough ! audiosink > $W/listen.log 2>&1 &
P=$!
i=0
while [ $i -lt 20 ] && kill -0 $P 2>/dev/null; do sleep 1; i=$((i + 1)); done
kill $P 2>/dev/null && echo "(stopped after 20 s)"
echo "--- last lines of $W/listen.log:"
grep -vE "^Setting|^Redistribute|^New clock" $W/listen.log | tail -n 15
echo
echo "No audioconvert on purpose: a format change would turn DTS into noise."
echo "If gst-launch could not open LG's audiosink outside the media service,"
echo "that is expected on some firmware: the real test is then step 6 (install)."
