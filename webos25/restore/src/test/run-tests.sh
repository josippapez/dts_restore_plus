#!/usr/bin/env sh
#
# run-tests.sh — host unit test for the webOS 25 dtsdec DRC core.
#
# Needs nothing but a C compiler, libc and libm: no GStreamer, no libdca, no
# cross-toolchain, no TV. It does three things:
#
#   1. Extracts the DRC CORE block from ../gstdtsdec.c (everything between the
#      DRC-CORE-BEGIN and DRC-CORE-END markers) into drc-core.inc, so the
#      assertions run against the code that actually ships rather than a copy.
#   2. Builds and runs test-drc.c (warnings are errors).
#   3. Checks two structural invariants that unit tests cannot express:
#      the per-sample apply loop stays free of transcendentals, and the
#      inert/make-up-gain output loop is still token-for-token what HEAD ships.
#
# Usage: sh run-tests.sh          (exit 0 = everything passed)

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../gstdtsdec.c"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CC="${CC:-cc}"

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
echo "=== ALL CHECKS PASSED ==="
exit 0
