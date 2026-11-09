#!/bin/bash
# Build and run the standalone native test for the ownership of the buffer
# passed to loadMem() (see test/load_mem_ownership_test.cpp) under
# AddressSanitizer: a read of the buffer after the loader freed it, or a
# delete[] of a buffer the caller allocated with calloc, aborts the run. The
# formats SoLoud parses run with alloc_dealloc_mismatch enabled (it is off by
# default on macOS); the ones that fall back to the platform decoder run
# without it, because Apple's AAC decoder has a mismatch of its own.
#
# Like the pull stream race test this links the plugin's own `bindings.cpp`,
# so loadMem() is called through the export Dart uses, on a real engine;
# miniaudio falls back to its null backend on a machine with no audio
# hardware. It is built without the Xiph libraries, like that test.
#
#   ./test/run_load_mem_ownership_test.sh

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SANITIZER=(-fsanitize=address -fno-omit-frame-pointer -g -O1)
OUT="${TMPDIR:-/tmp}/load_mem_ownership_test_asan"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# pffft.c is C99: the CMake build forces C for it, so do the same here rather
# than letting the C++ front end reject its VLAs.
cc -std=gnu99 -O2 -c "${SANITIZER[@]}" \
    -I src/pffft \
    -o "$WORK_DIR/pffft.o" \
    src/pffft/pffft.c

# The OS decoder backends are compiled per platform, like src/CMakeLists.txt.
PLATFORM_SOURCES=()
PLATFORM_LIBS=()
if [ "$(uname)" == "Darwin" ]; then
    PLATFORM_SOURCES=(
        src/native_decoder/os_decoder_apple.mm
        src/audiobuffer/aac_stream_decoder_apple.mm
    )
    PLATFORM_LIBS=(-framework AudioToolbox -framework CoreFoundation
                   -framework CoreAudio -framework AudioUnit -framework Foundation)
else
    PLATFORM_SOURCES=(src/native_decoder/os_decoder_linux.cpp)
fi

c++ -std=c++17 -O2 -w -pthread "${SANITIZER[@]}" \
    -DWITH_MINIAUDIO -DWITH_NULL \
    -DNO_XIPH_LIBS \
    -DSOLOUD_LIFECYCLE_TEST_HOOKS \
    -I src/soloud/include \
    -I src/soloud/src \
    -I src/pffft \
    -I src \
    -I src/native_decoder \
    -o "$OUT" \
    test/load_mem_ownership_test.cpp \
    "$WORK_DIR/pffft.o" \
    src/soloud_common.cpp \
    src/device_lifecycle_test_hooks.cpp \
    src/bindings.cpp \
    src/player.cpp \
    src/analyzer.cpp \
    src/synth/basic_wave.cpp \
    src/waveform/waveform.cpp \
    src/waveform/miniaudio_libvorbis.cpp \
    src/audiobuffer/*.cpp \
    src/native_stream/*.cpp \
    src/native_decoder/native_audio_decoder.cpp \
    "${PLATFORM_SOURCES[@]}" \
    src/filters/*.cpp \
    src/mixeroutput/*.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/filter/*.cpp \
    src/soloud/src/audiosource/ay/*.cpp \
    src/soloud/src/audiosource/monotone/*.cpp \
    src/soloud/src/audiosource/noise/*.cpp \
    src/soloud/src/audiosource/sfxr/*.cpp \
    src/soloud/src/audiosource/speech/*.cpp \
    src/soloud/src/audiosource/tedsid/*.cpp \
    src/soloud/src/audiosource/vic/*.cpp \
    src/soloud/src/audiosource/vizsn/*.cpp \
    src/soloud/src/audiosource/wav/*.cpp \
    src/soloud/src/backend/miniaudio/soloud_miniaudio.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    "${PLATFORM_LIBS[@]}" \
    -ldl -lm

ASAN_OPTIONS="alloc_dealloc_mismatch=1:${ASAN_OPTIONS:-}" "$OUT" soloud
ASAN_OPTIONS="alloc_dealloc_mismatch=0:${ASAN_OPTIONS:-}" "$OUT" platform
