#!/bin/bash
# Build and run the regression test for bounded WavStream seeks: a seek never
# decodes for long inside a mix, and playback still starts sample-exact at the
# target (see test/mp3_seek_bound_test.cpp). Uses the null backend, so no
# audio hardware is needed.
#
# The long MP3 is built by the test from a repo asset. When ffmpeg is
# available, long FLAC (with and, given metaflac, without a seek table), Opus
# and ADPCM WAV files are made from another repo asset and tested too.
#
# Ogg/Opus needs the Xiph libraries; they are linked from the system (brew:
# libogg libvorbis opus flac). Set NO_XIPH=1 to build without them.
#
# Pass --tsan to build under ThreadSanitizer (timing is then reported, not
# asserted).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SANITIZER=(-O2)
OUT="${TMPDIR:-/tmp}/mp3_seek_bound_test"
if [ "$1" == "--tsan" ]; then
    SANITIZER=(-fsanitize=thread -g -O1)
    OUT="${OUT}_tsan"
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
    -DWITH_NULL -DSOLOUD_LIFECYCLE_TEST_HOOKS \
    "${XIPH_FLAGS[@]}" \
    -I src/soloud/include \
    -I src/soloud/src/audiosource/wav \
    -o "$OUT" \
    test/mp3_seek_bound_test.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/audiosource/wav/soloud_wavstream.cpp \
    src/soloud/src/audiosource/wav/dr_impl.cpp \
    src/soloud/src/audiosource/wav/mb_ogg.cpp \
    src/audiobuffer/mp3_stream_decoder.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    "${XIPH_LIBS[@]}"

EXTRA=()
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
if command -v ffmpeg > /dev/null; then
    # Ten minutes (40 x 15 s) of the repo's WAV sample.
    LOOP=(-loglevel error -y -stream_loop 39 -i example/assets/audio/sample-WAV.wav)
    ffmpeg "${LOOP[@]}" -c:a flac "$WORK_DIR/long.flac" && EXTRA+=("$WORK_DIR/long.flac")
    # ffmpeg writes no FLAC seek table; metaflac (brew: flac) adds one, every
    # 25 s, so seeks go through the table and then decode on in steps.
    if command -v metaflac > /dev/null && [ -f "$WORK_DIR/long.flac" ]; then
        cp "$WORK_DIR/long.flac" "$WORK_DIR/long_seektable.flac"
        metaflac --add-seekpoint=25s "$WORK_DIR/long_seektable.flac" && EXTRA+=("$WORK_DIR/long_seektable.flac")
    fi
    ffmpeg "${LOOP[@]}" -c:a adpcm_ima_wav "$WORK_DIR/long_ima_adpcm.wav" && EXTRA+=("$WORK_DIR/long_ima_adpcm.wav")
    ffmpeg "${LOOP[@]}" -c:a adpcm_ms "$WORK_DIR/long_ms_adpcm.wav" && EXTRA+=("$WORK_DIR/long_ms_adpcm.wav")
    if [[ " ${XIPH_FLAGS[*]} " != *" -DNO_XIPH_LIBS "* ]]; then
        ffmpeg "${LOOP[@]}" -c:a libopus -b:a 96k "$WORK_DIR/long.opus" 2> /dev/null \
            && EXTRA+=("$WORK_DIR/long.opus") || echo "ffmpeg cannot encode Opus; skipping the long Opus file"
    fi
else
    echo "ffmpeg not found; testing only the repo's FLAC and Opus samples besides MP3"
fi

"$OUT" "${EXTRA[@]}"
