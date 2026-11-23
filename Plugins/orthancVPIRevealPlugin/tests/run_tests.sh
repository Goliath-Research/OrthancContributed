#!/bin/sh
# Build and run the VPI storage path tests against Plugin/Plugin.cpp.
set -eu
cd "$(dirname "$0")"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
g++ -std=c++11 -Wall -Wno-unused-function -Wno-unused-variable \
  -I../Include -I. -include hooks.h \
  -c ../Plugin/Plugin.cpp -o "$build/Plugin.o"
g++ -std=c++11 -Wall -Wno-unused-function \
  -I../Include -I. \
  -c test_path_traversal.cpp -o "$build/test_path_traversal.o"
g++ -std=c++11 -o "$build/test_path_traversal" \
  "$build/Plugin.o" "$build/test_path_traversal.o"
"$build/test_path_traversal"
