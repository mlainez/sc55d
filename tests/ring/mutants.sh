#!/usr/bin/env bash
#
# SPDX-License-Identifier: MIT
#
# Deliberately break src/ring.cpp one way at a time and confirm ring_test
# notices. A test that cannot fail is not evidence.
#
#   ./tests/ring/mutants.sh
#
# Every mutant is applied to a *copy* under the build dir -- src/ring.cpp is
# never touched -- and the copy is diffed against the original first, so a
# mutation whose pattern has drifted out of the source is reported as an error
# rather than quietly scoring a survivor.
#
# Each mutant is run plain and, if it survives that, under ThreadSanitizer:
# run.sh ships both, so a mutant either configuration catches is caught.  The
# lost-wake-up mutants do not fail, they stop; ring_test's watchdog turns that
# into an exit code, and `timeout` is the backstop if even that is broken.
set -u
cd "$(dirname "$0")"
HERE=$(pwd)
SRC=$(cd ../../src && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/sc55d-ring-tests}/mutants
rm -rf "$OUT" && mkdir -p "$OUT"

CXX=${CXX:-g++}
BASE="-g -std=c++23 -pthread"
SCALE=${MUTANT_SCALE:-0.25}
TSAN_SCALE=${MUTANT_TSAN_SCALE:-0.08}
export TSAN_OPTIONS="halt_on_error=1:exitcode=66"

fail=0
declare -a ROWS

build_and_run() { # <dir> <label> <extra-flags> <scale> <timeout>
    local dir=$1 label=$2 flags=$3 scale=$4 secs=$5
    if ! $CXX $BASE $flags -I"$dir" -o "$dir/ring_test.$label" \
            "$HERE/ring_test.cpp" "$dir/ring.cpp" > "$dir/build.$label.log" 2>&1; then
        echo "BUILD"
        return
    fi
    if timeout "$secs" "$dir/ring_test.$label" "$scale" > "$dir/run.$label.log" 2>&1; then
        echo "passed"
    elif [ $? = 124 ]; then
        echo "timeout"
    else
        echo "failed"
    fi
}

mutate() { # <name> <description> <command...>
    local name=$1 desc=$2
    shift 2
    local dir="$OUT/$name"
    mkdir -p "$dir"
    cp "$SRC/ring.h" "$SRC/ring.cpp" "$dir/"

    if ! "$@" "$dir/ring.cpp"; then
        ROWS+=("$(printf '%-46s %-9s %-9s %s' "$desc" "-" "-" "ERROR: mutation script failed")")
        fail=1
        return
    fi
    if cmp -s "$SRC/ring.cpp" "$dir/ring.cpp"; then
        ROWS+=("$(printf '%-46s %-9s %-9s %s' "$desc" "-" "-" "ERROR: MUTATION DID NOT APPLY")")
        fail=1
        return
    fi

    local plain tsan="-" verdict
    plain=$(build_and_run "$dir" plain "-O2" "$SCALE" 90)
    if [ "$plain" = BUILD ]; then
        ROWS+=("$(printf '%-46s %-9s %-9s %s' "$desc" "-" "-" "ERROR: mutant does not compile")")
        fail=1
        return
    fi
    if [ "$plain" = passed ]; then
        tsan=$(build_and_run "$dir" tsan "-O1 -fsanitize=thread" "$TSAN_SCALE" 300)
        [ "$tsan" = BUILD ] && tsan="build-err"
    fi

    if [ "$plain" = passed ] && { [ "$tsan" = passed ] || [ "$tsan" = "-" ]; }; then
        verdict="SURVIVED  <-- the test is too weak"
        fail=1
    else
        verdict="KILLED"
    fi
    ROWS+=("$(printf '%-46s %-9s %-9s %s' "$desc" "$plain" "$tsan" "$verdict")")
    printf '.' >&2
}

drop_commitwrite_mutex() {
    perl -0pi -e 's/void PeriodRing::CommitWrite\(\)\n\{\n.*?\n\}/void PeriodRing::CommitWrite()\n{\n    head_++;\n    readable_.notify_one();\n}/s' "$1"
}
# BeginRead() has two done_ checks -- one before the counters, one after the
# wait -- so rewriting the whole function is clearer than two fragile patterns.
rewrite_beginread() { # <replacement-body-on-stdin> <file>
    python3 -c 'import re, sys
body = sys.stdin.read().rstrip("\n")
path = sys.argv[1]
text = open(path).read()
out = re.sub(r"const int16_t \*PeriodRing::BeginRead\(\)\n\{.*?\n\}", lambda m: body, text,
             count=1, flags=re.S)
sys.exit(1) if out == text else open(path, "w").write(out)' "$1"
}

drop_beginread_done() {
    rewrite_beginread "$1" <<'BODY'
const int16_t *PeriodRing::BeginRead()
{
    std::unique_lock<std::mutex> lock(mutex_);

    const unsigned fill = (unsigned)(head_ - tail_);
    if (fill < min_fill_)
        min_fill_ = fill;

    if (fill == 0)
    {
        starves_++;
        readable_.wait(lock, [this] { return done_ || head_ != tail_; });
    }
    return data_.data() + (size_t)(tail_ % slots_) * stride_;
}
BODY
}

# The pre-fix ordering: the counters move before done_ is looked at, so a
# BeginRead() arriving on an already-closed ring reports a starve that never
# happened and drags MinFill() to 0 on a clean run.
counters_before_done() {
    rewrite_beginread "$1" <<'BODY'
const int16_t *PeriodRing::BeginRead()
{
    std::unique_lock<std::mutex> lock(mutex_);

    const unsigned fill = (unsigned)(head_ - tail_);
    if (fill < min_fill_)
        min_fill_ = fill;

    if (fill == 0)
    {
        starves_++;
        readable_.wait(lock, [this] { return done_ || head_ != tail_; });
    }
    if (done_)
        return nullptr;
    return data_.data() + (size_t)(tail_ % slots_) * stride_;
}
BODY
}
drop_min_fill() {
    perl -0pi -e 's/    if \(fill < min_fill_\)\n        min_fill_ = fill;\n//s' "$1"
}

echo "mutation testing PeriodRing (plain scale $SCALE, tsan scale $TSAN_SCALE):" >&2

mutate no-readable-notify "drop readable_.notify_one() in CommitWrite" \
    sed -i 's/^    readable_\.notify_one();$//'
mutate no-writable-notify "drop writable_.notify_one() in CommitRead" \
    sed -i 's/^    writable_\.notify_one();$//'
mutate unlocked-head "head_++ outside the mutex in CommitWrite" \
    drop_commitwrite_mutex
mutate write-off-by-one "BeginWrite hands out slot (head_ + 1) % slots_" \
    sed -i 's/(head_ % slots_)/((head_ + 1) % slots_)/'
mutate full-off-by-one "full check head_ - tail_ <= slots_" \
    sed -i 's/head_ - tail_ < slots_/head_ - tail_ <= slots_/'
mutate read-ignores-done "BeginRead does not check done_" \
    drop_beginread_done
mutate no-starve-count "starves_ never incremented" \
    sed -i 's/^        starves_++;$//'
mutate no-min-fill "min_fill_ never lowered" \
    drop_min_fill
mutate counters-before-done "counters moved before the done_ check" \
    counters_before_done

echo >&2
printf '\n%-46s %-9s %-9s %s\n' mutant plain tsan verdict
printf '%s\n' "$(printf '=%.0s' $(seq 1 92))"
printf '%s\n' "${ROWS[@]}"
echo

if [ $fail -ne 0 ]; then
    echo "SOME MUTANTS SURVIVED (logs under $OUT)"
    exit 1
fi
echo "all mutants killed (logs under $OUT)"
