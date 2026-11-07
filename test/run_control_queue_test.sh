#!/bin/bash
# Build and run the standalone native tests for non-blocking voice control:
# setVolume/getVolume, getStreamPosition, seek, isValidVoiceHandle,
# setPause/getPause and setPan/getPan must not wait for a mix block that is
# stalled inside a source's getAudio() (see test/control_queue_test.cpp).
# Uses the null backend, so no audio hardware is needed.
#
# Pass --tsan to build under ThreadSanitizer.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SANITIZER=(-O2)
OUT="${TMPDIR:-/tmp}/control_queue_test"
if [ "$1" == "--tsan" ]; then
    SANITIZER=(-fsanitize=thread -g -O1)
    OUT="${OUT}_tsan"
fi

c++ -std=c++17 -Wall -Wextra -pthread "${SANITIZER[@]}" \
    -DWITH_NULL \
    -I src/soloud/include \
    -o "$OUT" \
    test/control_queue_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/backend/null/soloud_null.cpp

"$OUT"
