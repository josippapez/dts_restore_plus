#!/bin/sh
# check-manifest-floor.sh -- the Homebrew Channel listing floor must not lock out a
# generation the service implements a profile for.
#
# This exists because it already happened, silently, and nothing caught it: the
# manifest required `webosRelease: '>=10.0'` (webOS 25 only) while service.js gated
# the experimental C2/G2 profile on webOS `7.4.0`. Those are mutually exclusive, so a
# whole shipped feature could never be reached through the normal install path. The
# two constraints live in different files -- packaging/homebrew/*.yml and
# app/service/service.js -- with nothing tying them together, which is exactly how a
# contradiction survives review.
#
# The check is deliberately narrow: it reads the floor from the manifest, reads the
# webOS releases the service actually gates profiles on, and fails if the floor
# excludes any of them. It does NOT try to model every profile's requirements -- it
# catches the one mistake that has actually been made.
#
# Usage: sh webos25/restore/check-manifest-floor.sh
# Exit 0 = consistent, 1 = the floor excludes an implemented profile.
set -u

here=$(dirname "$0")
root=$(cd "$here/../.." 2>/dev/null && pwd) || { echo "check-manifest-floor: cannot resolve repo root" >&2; exit 1; }

manifest="$root/packaging/homebrew/io.github.josippapez.dtsenabler.yml"
service="$root/webos25/app/service/service.js"

for f in "$manifest" "$service"; do
  [ -r "$f" ] || { echo "check-manifest-floor: cannot read $f" >&2; exit 1; }
done

# Floor: the number in `webosRelease: '>=X.Y'`. Only the >= form is understood; any
# other operator is reported rather than guessed at.
floor_raw=$(sed -n "s/^[[:space:]]*webosRelease:[[:space:]]*['\"]*>=\([0-9][0-9.]*\)['\"]*[[:space:]]*$/\1/p" "$manifest" | head -n1)
if [ -z "$floor_raw" ]; then
  if grep -q "webosRelease" "$manifest"; then
    echo "check-manifest-floor: webosRelease is present but not a plain '>=X.Y' -- review by hand:" >&2
    grep -n "webosRelease" "$manifest" >&2
    exit 1
  fi
  echo "check-manifest-floor: OK -- no webosRelease floor is declared, so no profile can be excluded."
  exit 0
fi

# Releases the service gates a profile on. Today that is the C2 gate's literal
# WEBOS_RELEASE comparison; grep is enough because the value is an author constant.
gated=$(grep -oE '\$WEBOS_RELEASE" = "[0-9][0-9.]*"' "$service" | grep -oE '[0-9][0-9.]+' | sort -u)

# Compare as dotted numbers: pad each component so 7.4 < 10.0 rather than sorting
# lexically, which is the mistake that makes "10.0" look smaller than "7.4".
norm() { awk -F. '{ printf "%05d%05d\n", $1, ($2 == "" ? 0 : $2) }'; }
floor_n=$(printf '%s\n' "$floor_raw" | norm)

status=0
if [ -z "$gated" ]; then
  echo "check-manifest-floor: note -- no literal WEBOS_RELEASE profile gate found in service.js; only the floor ($floor_raw) was checked."
else
  for rel in $gated; do
    rel_n=$(printf '%s\n' "$rel" | norm)
    if [ "$rel_n" -lt "$floor_n" ]; then
      echo "check-manifest-floor: FAIL -- service.js gates a profile on webOS $rel, but the manifest floor is >=$floor_raw." >&2
      echo "  That profile can never be selected through the Homebrew Channel. Either lower the floor" >&2
      echo "  to <=$rel, or drop the profile and say so in the docs." >&2
      status=1
    fi
  done
fi

[ "$status" = 0 ] && echo "check-manifest-floor: OK -- floor >=$floor_raw admits every webOS release the service gates a profile on (${gated:-none})."
exit "$status"
