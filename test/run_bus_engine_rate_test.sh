#!/bin/bash
# Build and run the standalone native tests for a mixing bus in a 48 kHz engine:
# it mixes at the engine's rate, so a voice through it comes out sample for
# sample and a voice's delay counts the engine's samples. Built with
# AddressSanitizer, as the other bus tests are.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/bus_engine_rate_test"

c++ -std=c++17 -O1 -g -Wall -pthread \
    -fsanitize=address -fno-omit-frame-pointer \
    -DWITH_NULL \
    -I src/soloud/include \
    -o "$OUT" \
    test/bus_engine_rate_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/backend/null/soloud_null.cpp

"$OUT"
