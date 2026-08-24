#!/usr/bin/env bash
#
# Builds and runs the PeriodRing harness three ways and then mutation-tests it.
#
#   ./tests/ring/run.sh
#
#   plain -O2        the long run: this is the one that gets the period count up
#   ThreadSanitizer  the ring hands audio between threads outside the mutex, so
#                    "no race" is a claim that needs a race detector behind it
#   ASan + UBSan     slot arithmetic is index arithmetic; this catches a slot
#                    pointer that walks off the end of data_
#   mutants.sh       proof the above can fail at all
#
# Nothing is written inside the repo: everything lands in $OUT (default
# $TMPDIR/sc55d-ring-tests).  Needs no ROMs, no sound card and no ALSA.
set -u
cd "$(dirname "$0")"
HERE=$(pwd)
SRC=$(cd ../../src && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/sc55d-ring-tests}
mkdir -p "$OUT"
export OUT

CXX=${CXX:-g++}
BASE="-g -std=c++23 -pthread -I$SRC"
fail=0

stage() { # <name> <scale> <flags...>
    local name=$1 scale=$2
    shift 2
    printf '\n=== %s ===\n' "$name"
    if ! $CXX $BASE "$@" -o "$OUT/ring_test.$name" "$HERE/ring_test.cpp" "$SRC/ring.cpp" \
            > "$OUT/build.$name.log" 2>&1; then
        head -20 "$OUT/build.$name.log"
        echo "BUILD FAILED ($OUT/build.$name.log)"
        RESULTS+=("$(printf '%-24s BUILD FAILED' "$name")")
        fail=1
        return
    fi
    "$OUT/ring_test.$name" "$scale" > "$OUT/run.$name.log" 2>&1
    local status=$?
    cat "$OUT/run.$name.log"
    if [ $status -eq 0 ]; then
        RESULTS+=("$(printf '%-24s ok' "$name")")
    else
        RESULTS+=("$(printf '%-24s FAILED (exit %d)' "$name" $status)")
        fail=1
    fi
}

declare -a RESULTS

stage plain 1.0 -O2

export TSAN_OPTIONS="halt_on_error=1:exitcode=66"
stage tsan 0.15 -O1 -fsanitize=thread

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1"
stage asan-ubsan 0.3 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all

printf '\n=== mutants ===\n'
if ./mutants.sh; then
    RESULTS+=("$(printf '%-24s ok' mutants)")
else
    RESULTS+=("$(printf '%-24s FAILED' mutants)")
    fail=1
fi

printf '\n=== summary ===\n'
printf '%s\n' "${RESULTS[@]}"
echo "logs: $OUT"
[ $fail -eq 0 ] && echo "PeriodRing: all stages passed" || echo "PeriodRing: SOMETHING FAILED"
exit $fail
