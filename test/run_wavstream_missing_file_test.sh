#!/bin/bash
# Build and run the regression test for playing a disk-streamed WavStream
# whose file was deleted, moved or broken after it was loaded (see
# test/wavstream_missing_file_test.cpp). Uses the null backend, so no audio
# hardware is needed.
#
# Ogg Vorbis needs the Xiph libraries; they are linked from the system (brew:
# libogg libvorbis opus flac). Set NO_XIPH=1 to build without them.
#
# Pass --asan to build under AddressSanitizer.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SANITIZER=(-O1 -g)
OUT="${TMPDIR:-/tmp}/wavstream_missing_file_test"
if [ "$1" == "--asan" ]; then
    SANITIZER=(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    OUT="${OUT}_asan"
fi

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

c++ -std=c++17 -Wall -Wextra -pthread "${SANITIZER[@]}" \
    -DWITH_NULL \
    "${XIPH_FLAGS[@]}" \
    -I src/soloud/include \
    -I src/soloud/src/audiosource/wav \
    -o "$OUT" \
    test/wavstream_missing_file_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/audiosource/wav/soloud_wavstream.cpp \
    src/soloud/src/audiosource/wav/dr_impl.cpp \
    src/soloud/src/audiosource/wav/mb_ogg.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    "${XIPH_LIBS[@]}"

"$OUT"
