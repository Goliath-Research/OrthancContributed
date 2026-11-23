#!/bin/sh
# Builds the VPI storage plugin with the series-label tests and runs them.
# AddressSanitizer is required: the copy into the 100-byte static buffer
# is out of bounds while the bug is present, and the series files are
# wrong if that copy is only truncated.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
OUT=${TMPDIR:-/tmp}/series_label_overflow_test

g++ -std=c++11 -fsanitize=address -fno-omit-frame-pointer -g -O1 \
  -I "$ROOT/Plugins/orthancVPIRevealPlugin/Include" \
  -I "$ROOT/Plugins/orthancVPIRevealPlugin/tests/shim" \
  "$ROOT/Plugins/orthancVPIRevealPlugin/Plugin/Plugin.cpp" \
  "$ROOT/Plugins/orthancVPIRevealPlugin/tests/series_label_overflow_test.cpp" \
  -o "$OUT"

export ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0

"$OUT" fitting
"$OUT" max-accepted
"$OUT" one-past
