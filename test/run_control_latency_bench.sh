#!/bin/bash
# Build and run the host benchmark that times voice-control calls (setVolume,
# getStreamPosition, seek, ...) while the engine mixes disk streams, and
# attributes the waits (see test/control_latency_bench.cpp). Uses the null
# backend, driven at real time by a mixer thread, so no audio hardware is
# needed.
#
# Ogg/Vorbis needs the Xiph libraries; they are linked from the system (brew:
# libogg libvorbis opus flac). Set NO_XIPH=1 to build without them, in which
# case the OGG voices fail to load.
#
# All arguments are passed to the benchmark, e.g.
#   ./test/run_control_latency_bench.sh --seconds 10 --render-ahead 4096
#   ./test/run_control_latency_bench.sh --disk-delay-ms 30 --disk-delay-prob 0.05
#   ./test/run_control_latency_bench.sh --m4a /path/to/file.m4a   (macOS: NativeStream)
#   ./test/run_control_latency_bench.sh --burst 10000 [--render-ahead 4096]
#       (bursts of control calls, then getActiveVoiceCount/stop/play)

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${TMPDIR:-/tmp}/control_latency_bench"

XIPH_FLAGS=()
XIPH_LIBS=()
if [ "${NO_XIPH:-0}" == "1" ]; then
    XIPH_FLAGS=(-DNO_XIPH_LIBS)
else
    for prefix in /opt/homebrew /usr/local /usr; do
        if [ -f "$prefix/include/vorbis/vorbisfile.h" ]; then
            XIPH_FLAGS=(-I "$prefix/include")
            XIPH_LIBS=(-L "$prefix/lib" -lvorbisfile -lvorbis -logg -lopus -lFLAC)
            break
        fi
    done
    if [ ${#XIPH_LIBS[@]} -eq 0 ]; then
        echo "Xiph libraries not found; building with NO_XIPH_LIBS" >&2
        XIPH_FLAGS=(-DNO_XIPH_LIBS)
    fi
fi

# Wav::load() links the OS decoder fallback (as src/CMakeLists.txt does).
PLATFORM_SOURCES=()
PLATFORM_LIBS=()
if [ "$(uname)" == "Darwin" ]; then
    PLATFORM_SOURCES=(src/native_decoder/os_decoder_apple.mm)
    PLATFORM_LIBS=(-framework AudioToolbox -framework CoreFoundation)
else
    PLATFORM_SOURCES=(src/native_decoder/os_decoder_linux.cpp)
fi

# Release-like build (-DNDEBUG), as shipped: the numbers are about timing.
c++ -std=c++17 -O2 -w -pthread \
    -DNDEBUG \
    -DWITH_NULL \
    "${XIPH_FLAGS[@]}" \
    -I src/soloud/include \
    -I src \
    -I src/native_decoder \
    -o "$OUT" \
    test/control_latency_bench.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/audiosource/wav/*.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/native_decoder/native_audio_decoder.cpp \
    src/native_stream/*.cpp \
    "${PLATFORM_SOURCES[@]}" \
    src/soloud/src/backend/null/soloud_null.cpp \
    "${XIPH_LIBS[@]}" \
    "${PLATFORM_LIBS[@]}"

"$OUT" "$@"
