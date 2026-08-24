/*
 * SPDX-License-Identifier: MIT
 *
 * MidiQueue: does every byte the MIDI thread pushes reach the core exactly
 * once, in order, uncorrupted?
 *
 * That is the whole contract.  The queue exists so the core's UART FIFO is
 * only ever written by the render thread, and it is worth nothing if it
 * garbles the bytes on the way.
 *
 * The test drives it from two threads with a stream whose every byte is
 * predictable from its position, so a duplicate, a gap, a torn message or a
 * stale slot all show up as a mismatch at a known offset.  Run it under
 * ThreadSanitizer as well: the ordering bugs this queue exists to prevent are
 * invisible on x86 and TSan is the only thing here that can see them.
 */
#include "midi_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

/* Stands in for the emulator.  Drain() calls this, so it runs on the consumer
 * thread only -- exactly as Core::PostMidi() does in the real program.
 *
 * It checks its arguments as well as recording them.  A Drain() that handed
 * over the right bytes in the wrong-sized blocks would otherwise only show up
 * as a byte count at the very end, and one that handed over an empty block
 * would not show up at all. */
std::vector<uint8_t> g_received;
size_t g_posts = 0;

/* The wrap phase moves four gigabytes through the queue, far too much to keep
 * in a vector, so it verifies each byte against a rolling expectation as it
 * arrives instead of recording it. */
bool g_rolling = false;
uint8_t g_rolling_expect = 0;
unsigned long long g_rolling_bad = 0;
unsigned long long g_rolling_bytes = 0;

/* Byte n of the stream, chosen so that a slot left over from the previous lap
 * of the 8 KiB ring cannot pass for the right value: the period is 251, which
 * shares no factor with the ring size. */
uint8_t Expected(size_t n)
{
    return (uint8_t)(n % 251);
}

bool g_failed = false;

void Fail(const char *what, unsigned long long a, unsigned long long b)
{
    fprintf(stderr, "FAIL: %s (%llu vs %llu)\n", what, a, b);
    g_failed = true;
}

} // namespace

namespace Core {
void PostMidi(const uint8_t *data, size_t length)
{
    g_posts++;
    if (!data || length == 0)
    {
        /* Once: a Drain() that does this does it several million times. */
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            Fail("PostMidi() called with an empty block", (unsigned long long)length, 1);
        }
        return;
    }

    if (g_rolling)
    {
        for (size_t i = 0; i < length; i++)
        {
            if (data[i] != g_rolling_expect)
                g_rolling_bad++;
            g_rolling_expect = (uint8_t)(g_rolling_expect + 1 == 251 ? 0 : g_rolling_expect + 1);
        }
        g_rolling_bytes += length;
        return;
    }

    g_received.insert(g_received.end(), data, data + length);
}
} // namespace Core

/*
 * Capacity, and what happens to a message that cannot fit.
 *
 * The capacity is a contract rather than an implementation detail: midi_in.cpp
 * decodes into an 8192-byte buffer and pushes whatever comes out, so a maximal
 * sysex that the ALSA decoder accepts has to fit here too or it is refused
 * with nothing but a counter to show for it.  And a message that can never fit
 * has to be refused whole and leave the queue usable.
 */
void CapacityTest()
{
    const bool failed_before = g_failed;
    const size_t kRing = 8192; /* must match kSize in midi_queue.h */

    MidiQueue queue;
    uint8_t one = 0;
    size_t capacity = 0;
    while (capacity < kRing + 16)
    {
        const unsigned long before = queue.Dropped();
        one = Expected(capacity);
        queue.Push(&one, 1);
        if (queue.Dropped() != before)
            break;
        capacity++;
    }
    if (capacity != kRing)
        Fail("the ring accepts a different number of bytes than it holds",
             (unsigned long long)capacity, (unsigned long long)kRing);

    g_received.clear();
    if (queue.Drain() != capacity)
        Fail("drained a different number of bytes than the ring accepted",
             (unsigned long long)g_received.size(), (unsigned long long)capacity);
    for (size_t i = 0; i < g_received.size(); i++)
    {
        if (g_received[i] != Expected(i))
        {
            fprintf(stderr, "FAIL: capacity: byte %zu is %u, wanted %u\n", i,
                    g_received[i], Expected(i));
            g_failed = true;
            break;
        }
    }

    /* One byte more than the ring can ever hold, and then far more than that.
     * Both are impossible, not merely untimely: they must never be admitted
     * however empty the queue is. */
    MidiQueue fresh;
    const size_t oversize[2] = {kRing + 1, 20000};
    for (size_t which = 0; which < 2; which++)
    {
        std::vector<uint8_t> big(oversize[which], 0xa5);
        const unsigned long before = fresh.Dropped();
        fresh.Push(big.data(), big.size());
        if (fresh.Dropped() != before + big.size())
            Fail("an oversized message was not refused whole",
                 fresh.Dropped() - before, (unsigned long long)big.size());
    }

    /* And the refusals left nothing behind: the queue still works. */
    uint8_t small[3];
    for (size_t i = 0; i < 3; i++)
        small[i] = Expected(i);
    fresh.Push(small, 3);
    g_received.clear();
    if (fresh.Drain() != 3)
        Fail("the queue stopped working after refusing an oversized message",
             (unsigned long long)g_received.size(), 3);
    for (size_t i = 0; i < g_received.size() && i < 3; i++)
        if (g_received[i] != Expected(i))
            Fail("wrong byte after an oversized message was refused", g_received[i],
                 Expected(i));

    printf("size:    %s -- holds %zu bytes, refuses %zu and %zu whole\n",
           g_failed && !failed_before ? "FAIL" : "ok", capacity, oversize[0], oversize[1]);
}

/*
 * The 32-bit counters, taken all the way round.
 *
 * head_ and tail_ advance by the length of every message, so a synth left
 * running crosses 2^32 eventually.  Nothing else in this test gets within a
 * factor of 500 of that.  All the arithmetic is modular and 2^32 is a whole
 * number of rings, so the wrap should be invisible -- but a single relational
 * comparison on the counters anywhere would make it fatal, and that is a
 * one-character mistake that no amount of streaming at 8 MB would find.
 */
void WrapTest()
{
    const bool failed_before = g_failed;

    /* 251 * 32, so every block is the same bytes and the expectation the
     * consumer rolls is one counter rather than a stream position.  It is not
     * a divisor of 2^32, so the wrap lands in the middle of a block. */
    const size_t kBlock = 8032;
    std::vector<uint8_t> block(kBlock);
    for (size_t i = 0; i < kBlock; i++)
        block[i] = Expected(i);

    MidiQueue queue;
    g_rolling = true;
    g_rolling_expect = 0;
    g_rolling_bad = 0;
    g_rolling_bytes = 0;

    const unsigned long long target = (1ull << 32) + 65536;
    unsigned long long pushed = 0;
    unsigned long long drained = 0;
    while (pushed < target)
    {
        queue.Push(block.data(), kBlock);
        pushed += kBlock;
        drained += queue.Drain();
    }
    g_rolling = false;

    if (queue.Dropped() != 0)
        Fail("bytes were dropped from an always-emptied queue", queue.Dropped(), 0);
    if (drained != pushed)
        Fail("bytes went in and did not come out across the 2^32 wrap", drained, pushed);
    if (g_rolling_bytes != pushed)
        Fail("the core was handed a different number of bytes than were queued",
             g_rolling_bytes, pushed);
    if (g_rolling_bad != 0)
        Fail("bytes were corrupted across the 2^32 wrap", g_rolling_bad, 0);

    printf("wrap:    %s -- %llu bytes, counters round 2^32 once\n",
           g_failed && !failed_before ? "FAIL" : "ok", pushed);
}

/*
 * Drop accounting, single-threaded so the arithmetic is exact.
 *
 * Two contracts to hold: the queue counts every byte it refuses, and it
 * refuses a message whole.  Half a sysex reaching the emulated MCU would be
 * worse than none of it.
 */
void DropTest()
{
    const bool failed_before = g_failed;
    MidiQueue queue;

    /* Messages of 7, so the ring cannot end up exactly full and the last one
     * to fit leaves a remainder too small for the next. */
    const size_t length = 7;
    std::vector<uint8_t> message(length);

    size_t accepted = 0;
    size_t offered = 0;
    for (int i = 0; i < 4000; i++)
    {
        for (size_t j = 0; j < length; j++)
            message[j] = Expected(offered + j);

        const unsigned long before = queue.Dropped();
        queue.Push(message.data(), length);
        offered += length;
        if (queue.Dropped() == before)
            accepted += length;
    }

    if (queue.Dropped() != (unsigned long)(offered - accepted))
        Fail("Dropped() does not match the bytes refused", queue.Dropped(),
             (unsigned long long)(offered - accepted));

    g_received.clear();
    const size_t drained = queue.Drain();

    if (drained != accepted)
        Fail("drained a different number of bytes than were accepted",
             (unsigned long long)drained, (unsigned long long)accepted);
    if (drained % length != 0)
        Fail("a message was truncated -- drops must be whole messages",
             (unsigned long long)(drained % length), 0);

    /* Every accepted message was contiguous in the stream, so the bytes that
     * came out must be the first `accepted` of it. */
    for (size_t i = 0; i < g_received.size(); i++)
    {
        if (g_received[i] != Expected(i))
        {
            fprintf(stderr, "FAIL: drop test: byte %zu is %u, wanted %u\n",
                    i, g_received[i], Expected(i));
            g_failed = true;
            break;
        }
    }

    printf("drops:   %s -- %zu of %zu bytes accepted, %lu refused, all whole\n",
           g_failed && !failed_before ? "FAIL" : "ok", accepted, offered, queue.Dropped());
}

int main(int argc, char *argv[])
{
    /* Long enough to lap the 8 KiB ring many thousands of times.  --wrap adds
     * the four-gigabyte counter-wrap phase, which is too slow to run under a
     * sanitizer. */
    size_t total = 8u * 1000u * 1000u;
    bool wrap = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--wrap")
            wrap = true;
        else
            total = (size_t)strtoul(argv[i], nullptr, 10);
    }

    MidiQueue queue;
    std::atomic<bool> done(false);
    std::atomic<size_t> pushed(0);
    std::atomic<size_t> retried_bytes(0);
    /* The producer retries forever on a full queue, so a consumer that gives
     * up has to say so or the join below never returns. */
    std::atomic<bool> give_up(false);

    /* Messages of 1..12 bytes, the range a MIDI stream actually produces once
     * running status is expanded, with the occasional short sysex. */
    std::thread producer([&] {
        std::vector<uint8_t> message;
        size_t retried = 0;
        size_t n = 0;
        unsigned step = 0;
        while (n < total && !give_up.load(std::memory_order_relaxed))
        {
            const size_t length = 1 + (step++ % 12);
            message.clear();
            for (size_t i = 0; i < length && n + i < total; i++)
                message.push_back(Expected(n + i));

            /* The queue drops a message whole when it is full.  This phase is
             * about corruption, not capacity, so retry until it fits -- which
             * means Dropped() ends up counting retries, and DropTest() below
             * is what actually checks the drop accounting. */
            for (;;)
            {
                const unsigned long before = queue.Dropped();
                queue.Push(message.data(), message.size());
                if (queue.Dropped() == before)
                    break;
                if (give_up.load(std::memory_order_relaxed))
                    break;
                retried += message.size();
                std::this_thread::yield();
            }

            n += message.size();
            pushed.store(n, std::memory_order_relaxed);
        }
        retried_bytes.store(retried, std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    });

    size_t drains = 0;
    size_t empty_drains = 0;
    for (;;)
    {
        const size_t before = g_received.size();
        const size_t got = queue.Drain();
        drains++;
        /* Drain() documents its return value, and nothing else here would
         * notice if it were wrong. */
        if (g_received.size() - before != got)
            Fail("Drain() returned a count it did not hand to the core",
                 (unsigned long long)got, (unsigned long long)(g_received.size() - before));
        /* A queue that hands the same bytes over twice would otherwise be
         * caught only by running the machine out of memory. */
        if (g_received.size() > total)
            Fail("more bytes came out of the queue than went in",
                 (unsigned long long)g_received.size(), (unsigned long long)total);
        if (g_failed)
        {
            give_up.store(true, std::memory_order_relaxed);
            break;
        }
        if (got != 0)
            continue;

        empty_drains++;
        /* Empty and the producer says it is finished: drain once more, in case
         * it published between the drain above and the flag. */
        if (done.load(std::memory_order_acquire) && queue.Drain() == 0)
            break;
    }
    producer.join();
    queue.Drain();

    if (g_received.size() != total)
        Fail("byte count", (unsigned long long)g_received.size(), (unsigned long long)total);

    size_t mismatches = 0;
    size_t first_bad = 0;
    for (size_t i = 0; i < g_received.size(); i++)
    {
        if (g_received[i] != Expected(i))
        {
            if (!mismatches)
                first_bad = i;
            mismatches++;
        }
    }
    if (mismatches)
    {
        fprintf(stderr, "FAIL: %zu bytes wrong, first at offset %zu (got %u, wanted %u)\n",
                mismatches, first_bad, g_received[first_bad], Expected(first_bad));
        g_failed = true;
    }

    /* If the consumer never once found the queue empty, it was never actually
     * racing the producer and the test proved much less than it looks. */
    if (empty_drains == 0)
        Fail("consumer never caught up -- the threads did not interleave", 0, 1);

    printf("stream:  %s -- %zu bytes through %zu drains (%zu found it empty, "
           "%zu bytes retried on a full queue)\n",
           g_failed ? "FAIL" : "ok", total, drains, empty_drains,
           retried_bytes.load(std::memory_order_relaxed));

    DropTest();
    CapacityTest();
    if (wrap)
        WrapTest();

    return g_failed ? 1 : 0;
}
