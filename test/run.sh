#!/bin/sh
# Builds and runs the tests for tt_text.h on this computer (needs a C++17 compiler).
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/tiny-tunes-tests"
c++ -std=c++17 -Wall -Wno-unused-function -I . test_text.cpp -o "$OUT"
"$OUT"
