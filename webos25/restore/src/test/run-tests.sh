#!/usr/bin/env sh
#
# run-tests.sh — host unit test for the webOS 25 dtsdec DRC core.
#
# Covers BOTH decoders: gstdtsdec.c (DTS) and the mlpdec.c patch embedded in
# ../build-truehd.sh (TrueHD). Needs nothing but a C compiler, libc and libm:
# no GStreamer, no libdca, no cross-toolchain, no TV, no network. It:
#
#   1. Extracts the DRC CORE block from ../gstdtsdec.c (everything between the
#      DRC-CORE-BEGIN and DRC-CORE-END markers) into drc-core.inc, so the
#      assertions run against the code that actually ships rather than a copy.
#   2. Builds and runs test-drc.c (warnings are errors).
#   3. Checks structural invariants that unit tests cannot express: the
#      per-sample apply loop stays free of transcendentals, the
#      inert/make-up-gain output loop is still token-for-token what HEAD ships,
#      no silence gate has crept back, and the make-up-gain clamps go through
#      the NaN-safe path.
#   4. DRIFT GUARD: diffs the DTS DRC core against the copy embedded in
#      build-truehd.sh's patch heredoc. The two decoders share this block
#      byte-for-byte; this is the single mechanism that stops them silently
#      diverging, so it FAILS the run rather than living in a comment.
#   5. Extracts the TrueHD HOST BINDING out of the same heredoc and builds two
#      more tests against it (test-mlp-drc.c, test-window.c).
#   6. Checks the patch heredoc is internally well-formed (every @@ hunk header
#      matches the body it heads). `git apply --check` against a pristine
#      ffmpeg n4.4.4 tree is the real gate, but that needs the network and a
#      checkout; this catches the failure mode that actually happens — editing
#      the patch body and forgetting the line counts — with neither.
#
# Usage: sh run-tests.sh          (exit 0 = everything passed)

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../gstdtsdec.c"
TRUEHD="$HERE/../../build-truehd.sh"
[ -f "$TRUEHD" ] || { echo "ERROR: $TRUEHD not found" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-cc}"

# Body of the inlined mlpdec patch (everything between the heredoc delimiters).
extract_patch () {
  awk '/^cat > \/tmp\/mlpdec-webos25-loudness\.patch <<.PATCH_EOF.$/{f=1;next}
       /^PATCH_EOF$/{f=0} f' "$TRUEHD"
}

echo "=== 0. extract the DRC core from $SRC ==="
sed -n '/<<<DRC-CORE-BEGIN>>>/,/<<<DRC-CORE-END>>>/p' "$SRC" > "$WORK/drc-core.inc"
if [ ! -s "$WORK/drc-core.inc" ]; then
  echo "ERROR: DRC-CORE markers not found in $SRC" >&2
  exit 1
fi
echo "extracted $(wc -l < "$WORK/drc-core.inc" | tr -d ' ') lines -> drc-core.inc"

echo
echo "=== 1. compile + run the unit test ==="
$CC -std=c99 -O2 -Wall -Wextra -Werror -I"$WORK" \
    -o "$WORK/test-drc" "$HERE/test-drc.c" -lm
"$WORK/test-drc" "$WORK"

echo
echo "=== 2. structural check: no transcendental in the per-sample loop ==="
sed -n '/BEGIN DRC per-sample apply/,/END DRC per-sample apply/p' "$SRC" \
  > "$WORK/apply.txt"
if [ ! -s "$WORK/apply.txt" ]; then
  echo "ERROR: per-sample apply markers not found in $SRC" >&2
  exit 1
fi
if grep -nE '\b(powf?|log10f?|logf?|expf?|sqrtf?)[[:space:]]*\(' "$WORK/apply.txt"; then
  echo "FAIL: transcendental call inside the per-sample loop" >&2
  exit 1
fi
echo "ok   no powf/logf/expf/sqrtf in the per-sample apply loop"
if grep -nE 'gdouble[[:space:]]+(g|step|chan_scale)\b' "$WORK/apply.txt"; then
  echo "FAIL: gain arithmetic promoted to double in the per-sample loop" >&2
  exit 1
fi
echo "ok   the gain ramp stays float (armel soft-float budget)"

echo
echo "=== 3. structural check: the make-up-gain output loop is unchanged ==="
extract_convert () {
  # The whole nested output loop of the drc-off path -- both `for` headers, the
  # comment, the make-up-gain multiply and the S32 scale/clamp/store -- so loop
  # ORDER and nesting are compared, not just the conversion statement.
  # Indentation is stripped because the loop is now nested one level deeper
  # under the `else if (gain_only)` branch; that re-indent is the intended
  # change and must not read as a difference.
  # `q` after the terminator keeps this to the FIRST range: the worktree now
  # has a second `for (n = 0; ...)` in the DRC branch, and without the quit sed
  # would restart the range there and run to EOF.
  sed -n "/for (n = 0; n < 256; n++) {/,/reorder_map\[c\]\] = (gint32) s;/{p;/reorder_map\[c\]\] = (gint32) s;/q;}" \
    | sed 's/^[[:space:]]*//'
}
if git -C "$HERE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  REL="$(git -C "$HERE" ls-files --full-name "$SRC" 2>/dev/null || true)"
  [ -n "$REL" ] || REL="webos25/restore/src/gstdtsdec.c"
  git -C "$HERE" show "HEAD:$REL" | extract_convert > "$WORK/head.txt"
  extract_convert < "$SRC" > "$WORK/now.txt"
  if [ ! -s "$WORK/head.txt" ]; then
    echo "skip  HEAD has no make-up-gain loop to compare against"
  elif diff -u "$WORK/head.txt" "$WORK/now.txt"; then
    echo "ok   drc-off output loop (both for-headers + gain + convert) is"
    echo "     token-identical to HEAD, modulo the one extra indent level"
    echo "     => at gain 0 dB it multiplies by an exact 1.0f, so output is"
    echo "        bit-identical to the shipped build (unit test [8] proves the"
    echo "        multiply itself is lossless)."
  else
    echo "FAIL: the drc-off output loop changed; bit-exactness is not proven" >&2
    exit 1
  fi
else
  echo "skip  not a git work tree"
fi

echo
echo "=== 4. structural check: no silence gate reintroduced ==="
# Amendment F retired the silence gate: it made the worst case WORSE (a hard cut
# from an established high gain froze that gain, so the next cue peaked at
# -0.42 dBFS vs -12.01 dBFS with no gate). Amendment D's boost decay is what
# handles silence now. The unit tests exercise the EXTRACTED core, so a hold
# re-added to the HOST BINDING would slip past them entirely -- hence this grep
# over the whole file, not just the core markers.
if grep -nE 'GATE_DB|gate_holds|drc_gate' "$SRC" >&2; then
  echo "FAIL: a silence gate was reintroduced (amendment F retired it)" >&2
  exit 1
fi
echo "ok   no silence gate in gstdtsdec.c (amendment F holds)"

echo
echo "=== 5. structural check: make-up gain uses the NaN-safe clamp ==="
# The DTS make-up-gain setter used to hand-roll `if (g > MAX) ... else if
# (g < MIN)`, which is exactly the pair that lets a NaN through -- and a NaN
# there makes every output sample (gint32) NaN, i.e. undefined behaviour. It
# now delegates to dts_drc_clampf(). Unit tests cannot see this function (it
# takes a GstDtsDec), so assert it structurally.
sed -n '/^gst_dtsdec_apply_makeup_gain_db (/,/^}/p' "$SRC" > "$WORK/makeup.txt"
if [ ! -s "$WORK/makeup.txt" ]; then
  echo "ERROR: gst_dtsdec_apply_makeup_gain_db not found in $SRC" >&2
  exit 1
fi
if ! grep -q 'dts_drc_clampf' "$WORK/makeup.txt"; then
  echo "FAIL: the make-up-gain setter no longer uses the NaN-safe clamp" >&2
  exit 1
fi
if grep -qE '<[[:space:]]*DTS_MAKEUP_GAIN_DB_MIN' "$WORK/makeup.txt"; then
  echo "FAIL: the NaN-permeable '< MIN' clamp is back in the make-up-gain setter" >&2
  exit 1
fi
echo "ok   gst_dtsdec_apply_makeup_gain_db() clamps via dts_drc_clampf()"

echo
echo "=== 6. DRIFT GUARD: the DTS and TrueHD DRC cores are byte-identical ==="
# The two decoders ship the SAME DSP block. build-truehd.sh carries its copy
# inside a patch heredoc, so nothing but this check couples them: without it,
# a fix to one decoder silently leaves the other wrong (which is precisely how
# this file's own NaN bug would have half-shipped).
sed -n '/<<<DRC-CORE-BEGIN>>>/,/<<<DRC-CORE-END>>>/p' "$SRC" > "$WORK/dts-core.inc"
extract_patch \
  | sed -n 's/^+//p' \
  | sed -n '/<<<DRC-CORE-BEGIN>>>/,/<<<DRC-CORE-END>>>/p' > "$WORK/thd-core.inc"
# Both sides must be non-empty AND non-trivial: an extraction that silently
# matched nothing would otherwise diff clean and read as a pass.
for f in dts-core thd-core; do
  n="$(wc -l < "$WORK/$f.inc" | tr -d ' ')"
  if [ "$n" -lt 100 ]; then
    echo "ERROR: $f.inc extracted only $n lines — markers moved or renamed" >&2
    exit 1
  fi
done
if diff -u "$WORK/dts-core.inc" "$WORK/thd-core.inc"; then
  echo "ok   both cores are $(wc -l < "$WORK/dts-core.inc" | tr -d ' ') lines /" \
       "$(wc -c < "$WORK/dts-core.inc" | tr -d ' ') bytes, byte-for-byte equal"
else
  echo "FAIL: the DTS and TrueHD DRC cores have DRIFTED (diff above)." >&2
  echo "      Re-port the change into build-truehd.sh's patch heredoc." >&2
  exit 1
fi

echo
echo "=== 7. patch heredoc: every @@ hunk header matches its body ==="
# Editing the patch body without fixing the hunk line counts produces a patch
# that `git apply` rejects -- and that failure only surfaces inside the podman
# cross-build, minutes later. Check the arithmetic here instead.
extract_patch > "$WORK/mlp.patch"
if [ ! -s "$WORK/mlp.patch" ]; then
  echo "ERROR: could not extract the mlpdec patch from $TRUEHD" >&2
  exit 1
fi
if ! awk '
  /^@@ / {
    if (h && (o != oc || n != nc)) {
      printf "hunk %s: header says -%d,%d +%d,%d but body has %d/%d\n",
             hdr, os, oc, ns, nc, o, n; bad++
    }
    hdr = $0; h = 1; o = n = 0
    split($2, a, ","); os = -a[1]; oc = a[2]
    split($3, b, ","); ns =  b[1]; nc = b[2]
    next
  }
  h && /^\+/ { n++; next }
  h && /^-/  { o++; next }
  h          { o++; n++ }
  END {
    if (h && (o != oc || n != nc)) {
      printf "hunk %s: header says -%d,%d +%d,%d but body has %d/%d\n",
             hdr, os, oc, ns, nc, o, n; bad++
    }
    if (!h) { print "no @@ hunk headers found in the patch"; bad++ }
    exit bad ? 1 : 0
  }' "$WORK/mlp.patch"; then
  echo "FAIL: patch hunk headers are inconsistent with the patch body" >&2
  exit 1
fi
echo "ok   $(grep -c '^@@ ' "$WORK/mlp.patch") hunk headers agree with the body"

echo
echo "=== 8. TrueHD host binding: extract + unit test ==="
# The DSP core is shared (check 6 proves it); what is NOT shared is the
# interleaved-integer detector, the interleaved apply loop and the legacy
# double-precision make-up-gain reader. Test those against the code that ships,
# extracted from the patch rather than copied.
sed -n 's/^+//p' "$WORK/mlp.patch" \
  | sed -n '/^static double mlp_read_makeup_gain_db(void)$/,/END DRC per-sample apply/p' \
  > "$WORK/mlp-binding.inc"
for sym in mlp_read_makeup_gain_db mlp_makeup_gain_db_to_linear \
           mlp_apply_makeup_gain mlp_drc_sum_squares_ilv mlp_drc_apply; do
  if ! grep -q "$sym" "$WORK/mlp-binding.inc"; then
    echo "ERROR: $sym missing from the extracted binding (extraction broke)" >&2
    exit 1
  fi
done
echo "extracted $(wc -l < "$WORK/mlp-binding.inc" | tr -d ' ') lines -> mlp-binding.inc"

$CC -std=c99 -O2 -Wall -Wextra -Werror -I"$WORK" \
    -DMLP_MAKEUP_GAIN_CONF_PATH="\"$WORK/truehd-gain.conf\"" \
    -o "$WORK/test-mlp-drc" "$HERE/test-mlp-drc.c" -lm
"$WORK/test-mlp-drc"

echo
echo "=== 9. TrueHD detector window vs the DTS reference ==="
$CC -std=c99 -O2 -Wall -Wextra -Werror -I"$WORK" \
    -DMLP_MAKEUP_GAIN_CONF_PATH="\"$WORK/truehd-gain.conf\"" \
    -o "$WORK/test-window" "$HERE/test-window.c" -lm
"$WORK/test-window"

echo
echo "=== ALL CHECKS PASSED ==="
exit 0
