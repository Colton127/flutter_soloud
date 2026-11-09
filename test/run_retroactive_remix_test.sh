#!/bin/bash
# Build and run the standalone native retroactive re-mix tests (Phase 3a of
# the retroactive re-mixing feature). Uses the null backend, so no audio
# hardware is needed.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/retroactive_remix_test"

# Wav's native-format fallback is linked even when the fixture is PCM.
# Match the platform decoder sources used by the plugin's other runners.
PLATFORM_SOURCES=(src/native_decoder/native_audio_decoder.cpp)
PLATFORM_LIBS=()
if [ "$(uname)" == "Darwin" ]; then
    PLATFORM_SOURCES+=(src/native_decoder/os_decoder_apple.mm)
    PLATFORM_LIBS=(-framework AudioToolbox -framework CoreFoundation)
else
    PLATFORM_SOURCES+=(src/native_decoder/os_decoder_linux.cpp)
    PLATFORM_LIBS=(-ldl -lm)
fi

c++ -std=c++17 -O2 -Wall -Wextra -pthread \
    -DWITH_NULL \
    -DNO_XIPH_LIBS \
    -I src/soloud/include -I src/native_decoder \
    -o "$OUT" \
    test/retroactive_remix_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/audiosource/wav/*.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    "${PLATFORM_SOURCES[@]}" "${PLATFORM_LIBS[@]}"

"$OUT"
