#!/bin/bash
# Build and run the standalone native tests for filter instances and the core
# filter operations (test/filter_ops_test.cpp) on the null backend.
#
#   ./test/run_filter_ops_test.sh [--tsan] [scenario]
#
# Built with AddressSanitizer by default, so an out-of-bounds filter buffer
# access is reported deterministically; --tsan builds with ThreadSanitizer
# instead (for the `getter` race). A scenario name (eq, getter or pitch) runs
# only that scenario.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

SANITIZER=(-fsanitize=address -fno-omit-frame-pointer)
OUT="${TMPDIR:-/tmp}/filter_ops_test"
if [ "${1:-}" == "--tsan" ]; then
    SANITIZER=(-fsanitize=thread)
    OUT="${OUT}_tsan"
    shift
fi

cc -O1 -g "${SANITIZER[@]}" -c -o "$OUT-pffft.o" src/pffft/pffft.c

c++ -std=c++17 -O1 -g -w -pthread "${SANITIZER[@]}" \
    -DWITH_NULL \
    -I src/soloud/include \
    -o "$OUT" \
    test/filter_ops_test.cpp \
    src/filters/*.cpp \
    src/soloud/src/core/*.cpp \
    src/soloud/src/filter/*.cpp \
    src/soloud/src/backend/null/soloud_null.cpp \
    "$OUT-pffft.o"

"$OUT" "$@"
