#!/bin/sh
# run.sh -- every kmod test. Suites that need private captures (test/captures/) skip without them.
HERE=$(cd "$(dirname "$0")" && pwd)
for suite in loader/run.sh loader/captures.sh aud-hal/run.sh legacy-audio/run.sh; do
  echo "== $suite"
  sh "$HERE/$suite" || failed="${failed:-} $suite"
done
[ -z "${failed:-}" ] || { echo "FAILED:$failed"; exit 1; }
echo "ALL PASSED"
