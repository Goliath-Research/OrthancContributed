#!/bin/bash
# Build and run the stored-instance missing-tag tests.
# The tests exit non-zero while the plugin dereferences a failed tag search.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${TESTDIR}/missing_tag_stored_instance_test"

g++ -std=c++11 -O0 -g \
  -I"${TESTDIR}/win32" \
  -I"${ROOT}/Include" \
  -I"${TESTDIR}/third_party/include" \
  -o "${OUT}" \
  "${TESTDIR}/missing_tag_stored_instance_test.cpp" \
  "${ROOT}/Plugin/Plugin.cpp" \
  "${TESTDIR}/third_party/src/lib_json/json_reader.cpp" \
  "${TESTDIR}/third_party/src/lib_json/json_value.cpp" \
  "${TESTDIR}/third_party/src/lib_json/json_writer.cpp"

set +e
"${OUT}"
status=$?
set -e
rm -f "${OUT}"
exit "${status}"
