#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds and runs the MidiQueue test three ways, then checks that the test can
# actually fail by breaking the queue on purpose.  Nothing is written inside
# the repository.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../src
out=${TMPDIR:-/tmp}/sc55d-midi-queue-test
rm -rf "$out"
mkdir -p "$out"

CXX=${CXX:-g++}
common="-std=c++20 -Wall -Wextra -I$src -pthread"

build() { # name, extra flags, queue source
    $CXX $common $2 -o "$out/$1" "$here/midi_queue_test.cpp" "$3" 2>&1
}

status=0
note() { printf '%-28s %s\n' "$1" "$2"; }

echo "== MidiQueue =="

# --wrap takes the 32-bit counters all the way round, which is half a minute
# of pure arithmetic: worth it once, in the build that can afford it.
build plain "-O2" "$src/midi_queue.cpp"
if "$out/plain" --wrap >"$out/plain.log" 2>&1; then note "optimised" "ok"; else note "optimised" "FAIL"; status=1; fi
sed 's/^/    /' "$out/plain.log"

# ThreadSanitizer is the only thing here that can see the bug this queue
# exists to prevent: on x86 a missing release/acquire pair is invisible at
# runtime.  Fewer bytes, because TSan is roughly 20x slower.
build tsan "-O1 -g -fsanitize=thread" "$src/midi_queue.cpp"
if "$out/tsan" 2000000 >"$out/tsan.log" 2>&1; then note "ThreadSanitizer" "ok"; else note "ThreadSanitizer" "FAIL"; status=1; cat "$out/tsan.log"; fi

build asan "-O1 -g -fsanitize=address,undefined" "$src/midi_queue.cpp"
if "$out/asan" 4000000 >"$out/asan.log" 2>&1; then note "ASan/UBSan" "ok"; else note "ASan/UBSan" "FAIL"; status=1; cat "$out/asan.log"; fi

echo
echo "== mutants =="
echo "(each breaks the queue on purpose; the test must notice)"

# Each mutant is: name | program | build flags to detect it under | tool
# (sed, the default, or perl for the ones that span lines) | runtime args.
mutate() {
    name=$1; program=$2; flags=$3; tool=${4:-sed}; args=${5:-2000000}
    copy=$out/mutant-$name.cpp
    if [ "$tool" = perl ]; then
        perl -0pe "$program" "$src/midi_queue.cpp" > "$copy"
    else
        sed "$program" "$src/midi_queue.cpp" > "$copy"
    fi
    if cmp -s "$copy" "$src/midi_queue.cpp"; then
        note "$name" "BROKEN TEST -- mutation did not apply"
        status=1
        return
    fi
    if ! build "m-$name" "$flags" "$copy" > "$out/m-$name.build" 2>&1; then
        note "$name" "BROKEN TEST -- mutant does not compile"
        cat "$out/m-$name.build"
        status=1
        return
    fi
    # A missing wake-up cannot hang this test -- there are no condition
    # variables -- but a mutant can spin, so cap it.
    if timeout 300 "$out/m-$name" $args > "$out/m-$name.log" 2>&1; then
        note "$name" "SURVIVED -- the test is too weak"
        status=1
    else
        note "$name" "killed"
    fi
}

# Publishing the bytes without release: invisible on x86 at runtime, so this
# one is only meaningful under TSan -- which is the point of listing it.
mutate relaxed-publish \
    's/head_.store((uint32_t)(head + length), std::memory_order_release)/head_.store((uint32_t)(head + length), std::memory_order_relaxed)/' \
    "-O1 -g -fsanitize=thread"

# Same on the reader side: downgrade the acquire load that publishes the bytes.
mutate relaxed-consume \
    's/const uint32_t head = head_.load(std::memory_order_acquire)/const uint32_t head = head_.load(std::memory_order_relaxed)/' \
    "-O1 -g -fsanitize=thread"

# One byte more capacity than the ring has: the last byte of a full ring lands
# on the oldest unread one, and Drain then reads kSize + 1 bytes out of a
# kSize buffer.  (The mutation the other way -- kMask, one byte *less* -- is
# not a corruption bug at all, which is why it is not in this list; it is a
# capacity regression, and capacity-under-by-one below is what pins it.)
mutate capacity-over-by-one \
    's/const uint32_t free_bytes = kSize - (head - tail)/const uint32_t free_bytes = kSize + 1 - (head - tail)/' \
    "-O2"

# One byte less: nothing is corrupted, but a maximal 8192-byte sysex out of
# midi_in.cpp's decode buffer no longer fits and is silently refused.
mutate capacity-under-by-one \
    's/const uint32_t free_bytes = kSize - (head - tail)/const uint32_t free_bytes = kMask - (head - tail)/' \
    "-O2"

# Let a message too big for the ring past the guard: it wraps onto itself and
# leaves head - tail larger than the ring, after which free_bytes underflows
# and the queue accepts anything.
mutate oversize-accepted \
    's/if (length > free_bytes)/if (length > free_bytes \&\& length <= kSize)/' \
    "-O2"

# Forget to count what was refused: Dropped() is what main.cpp shows the user.
mutate no-drop-count \
    's/        dropped_.fetch_add((unsigned long)length, std::memory_order_relaxed);//' \
    "-O2"

# Forget it wraps: after 2^32 bytes head is numerically below tail and a
# relational test says "empty" forever.  Only --wrap can see this one.
mutate counters-compared-relationally \
    's/    if (head == tail)/    if (head <= tail)/' \
    "-O2" sed "2000000 --wrap"

# Drain() lies about how much it handed over.
mutate drain-miscounts \
    's/    return count;/    return count + 1;/' \
    "-O2"

# Hand the core an empty block.
mutate post-empty-block \
    's/Core::PostMidi(&byte, 1)/Core::PostMidi(\&byte, 0)/' \
    "-O2"

# Hand the core one byte too many -- reads off the end of the local.
mutate post-two-bytes \
    's/Core::PostMidi(&byte, 1)/Core::PostMidi(\&byte, 2)/' \
    "-O1 -g -fsanitize=address"

# The obvious optimisation, and it is wrong: one call for the whole run reads
# straight off the end of the buffer whenever the queued bytes wrap.
mutate post-whole-block \
    's/    for \(uint32_t i = 0; i < count; i\+\+\)\n    \{\n.*?\n    \}\n/    Core::PostMidi(&data_[tail & kMask], count);\n/s' \
    "-O1 -g -fsanitize=address" perl

# Consumer does not publish how far it got: the producer never sees space and
# every byte after the first lap is dropped.
mutate no-tail-publish \
    's/    tail_.store(head, std::memory_order_release);//' \
    "-O2"

# Consumer rewinds one byte: every drain repeats its last byte.
mutate tail-off-by-one \
    's/tail_.store(head, std::memory_order_release)/tail_.store(head - 1, std::memory_order_release)/' \
    "-O2"

# Reads the byte before the one it should: stale data, right count.
mutate stale-read \
    's/const uint8_t byte = data_\[(tail + i) & kMask\]/const uint8_t byte = data_[(tail + i - 1) \& kMask]/' \
    "-O2"

echo
if [ "$status" -eq 0 ]; then echo "all good"; else echo "FAILURES ABOVE"; fi
exit $status
