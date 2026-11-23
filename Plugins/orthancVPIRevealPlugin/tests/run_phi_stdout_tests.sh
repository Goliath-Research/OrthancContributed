#!/bin/sh
# Builds and runs the VPI Reveal patient-data disclosure checks.
# The checks fail while the store callback writes patient or study text
# to process output. They pass once that text is left out.
set -eu
cd "$(dirname "$0")/.."
g++ -std=c++11 -Wall -Wextra -I tests/support -I Include \
  -o tests/phi_stdout_test \
  tests/phi_stdout_test.cpp Plugin/Plugin.cpp
tests/phi_stdout_test
