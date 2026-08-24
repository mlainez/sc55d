/*
 * Correctness harness for PeriodRing (src/ring.h, src/ring.cpp).
 *
 * The ring is the only place in sc55d where two threads touch the same memory
 * without a lock held across the touch: the producer writes a slot's audio
 * outside the mutex and the consumer reads it outside the mutex, and the only
 * thing keeping them off each other is the head/tail arithmetic.  So the
 * property worth proving is not "it compiles and plays": it is that every
 * period the renderer commits reaches the output side exactly once, whole, in
 * order, and that neither side can be left asleep.
 *
 * To make a torn slot visible, every slot carries a sequence number, a payload
 * derived from that sequence number and a checksum over the lot.  Half of
 * period n followed by half of period n+1 fails the checksum; a slot handed
 * out twice fails the sequence check; a slot skipped leaves a gap.  The
 * consumer verifies each slot twice, so a producer scribbling on a slot that
 * is still being read has two windows to be caught in rather than one.
 *
 * Five phases, because the states that break a ring are the extremes:
 *   A  consumer held a whole ring back -- the ring saturated (steady state)
 *   B  producer slower than consumer   -- the ring starving
 *   C  matched, with jitter on both    -- the boundaries hit head-on, at speed
 *   E  rings of 1, 2 and 3 slots       -- nothing but boundary
 *   D  Shutdown() from a third thread  -- both sides must leave, not hang
 *   F  the counters across Shutdown()  -- they are what the user is shown
 *
 * It links only ring.cpp: no ALSA, no emulator core, no ROMs.
 *
 *   g++ -O2 -std=c++23 -pthread -I../../src -o ring_test ring_test.cpp ../../src/ring.cpp
 *   ./ring_test [scale]        scale < 1 shortens the run (sanitizers, mutants)
 */
#include "ring.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

constexpr unsigned kSlots = 8; /* --render-ahead 8 */
constexpr unsigned kPeriodFrames = 64;
constexpr unsigned kStride = kPeriodFrames * 2; /* int16_t per slot */

/* A lost wake-up shows up as nothing happening at all, so the harness has to
 * be able to fail rather than hang: every period bumps a heartbeat and a
 * watchdog thread gives up on the run if it stops moving. */
std::atomic<unsigned long> g_heartbeat{0};
std::atomic<const char *> g_phase{"startup"};
std::atomic<bool> g_watchdog_done{false};

std::atomic<int> g_failures{0};

void Fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL [%s]: ", g_phase.load(std::memory_order_relaxed));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    g_failures.fetch_add(1, std::memory_order_relaxed);
}

void Tick(uint64_t n)
{
    if ((n & 0xff) == 0)
        g_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

void Watchdog(double stall_seconds)
{
    unsigned long last = 0;
    double idle = 0;
    while (!g_watchdog_done.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const unsigned long now = g_heartbeat.load(std::memory_order_relaxed);
        if (now != last)
        {
            last = now;
            idle = 0;
            continue;
        }
        idle += 0.2;
        if (idle >= stall_seconds)
        {
            fprintf(stderr,
                    "FAIL [%s]: no progress for %.0fs -- a wake-up was lost or a "
                    "thread is deadlocked\n",
                    g_phase.load(std::memory_order_relaxed), stall_seconds);
            fflush(nullptr);
            _exit(1);
        }
    }
}

uint16_t Word(uint64_t seq, unsigned i)
{
    uint64_t x = seq * 0x9e3779b97f4a7c15ull + i * 0xbf58476d1ce4e5b9ull;
    x ^= x >> 31;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 29;
    return (uint16_t)x;
}

void FillSlot(int16_t *slot, uint64_t seq)
{
    slot[0] = (int16_t)(uint16_t)seq;
    slot[1] = (int16_t)(uint16_t)(seq >> 16);
    slot[2] = (int16_t)(uint16_t)(seq >> 32);
    for (unsigned i = 3; i < kStride - 1; i++)
        slot[i] = (int16_t)Word(seq, i);

    uint16_t sum = 0;
    for (unsigned i = 0; i < kStride - 1; i++)
        sum = (uint16_t)(sum * 31u + (uint16_t)slot[i]);
    slot[kStride - 1] = (int16_t)sum;
}

/* The sequence number the slot claims, or -1 if it does not hold one whole
 * period: a short read, a torn slot or a slot being overwritten right now. */
int64_t CheckSlot(const int16_t *slot)
{
    const uint64_t seq = (uint64_t)(uint16_t)slot[0] |
                         ((uint64_t)(uint16_t)slot[1] << 16) |
                         ((uint64_t)(uint16_t)slot[2] << 32);
    for (unsigned i = 3; i < kStride - 1; i++)
        if ((uint16_t)slot[i] != Word(seq, i))
            return -1;

    uint16_t sum = 0;
    for (unsigned i = 0; i < kStride - 1; i++)
        sum = (uint16_t)(sum * 31u + (uint16_t)slot[i]);
    if ((uint16_t)slot[kStride - 1] != sum)
        return -1;
    return (int64_t)seq;
}

uint32_t NextRandom(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void SpinFor(unsigned micros)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
    while (std::chrono::steady_clock::now() < until)
        ;
}

enum class Pace {
    Free,   /* as fast as the ring allows */
    Jitter, /* mostly free, occasionally stalls -- the interesting one */
};

void ApplyPace(Pace pace, uint32_t &rng)
{
    switch (pace)
    {
    case Pace::Free:
        break;
    case Pace::Jitter:
        if ((NextRandom(rng) & 0x3f) == 0)
            SpinFor(NextRandom(rng) & 0x1f);
        break;
    }
}

struct Pass {
    uint64_t produced = 0;
    uint64_t consumed = 0;
    unsigned min_observed = ~0u;
    unsigned long starves = 0;
    unsigned min_fill = 0;
    bool producer_saw_null = false;
    bool consumer_saw_null = false;
};

/* Produces and consumes exactly `count` periods, verifying every one.  No
 * shutdown: the run has to come out even on its own.
 *
 * `hold_full` holds the consumer back until the producer has committed a whole
 * ring ahead of it.  Pacing the consumer with a sleep would only make the ring
 * *probably* full, and a container that deschedules the producer for a
 * millisecond would then fail a run that is not testing scheduling; waiting on
 * the producer's own counter makes "the ring is saturated" true by
 * construction, so Starves() == 0 becomes a property rather than a hope. */
Pass RunPass(const char *name, uint64_t count, Pace produce, Pace consume, bool prefill,
             bool hold_full = false, unsigned slots = kSlots)
{
    g_phase.store(name, std::memory_order_relaxed);

    PeriodRing ring;
    ring.Init(slots, kPeriodFrames);
    Pass pass;

    std::atomic<uint64_t> committed{0};
    std::atomic<bool> aborted{false};

    std::thread producer([&] {
        uint32_t rng = 0x1234567u;
        for (uint64_t seq = 0; seq < count; seq++)
        {
            int16_t *slot = ring.BeginWrite();
            if (!slot)
            {
                pass.producer_saw_null = true;
                return;
            }
            FillSlot(slot, seq);
            ring.CommitWrite();
            committed.store(seq + 1, std::memory_order_relaxed);
            pass.produced = seq + 1;
            Tick(seq);
            ApplyPace(produce, rng);
        }
    });

    std::thread consumer([&] {
        uint32_t rng = 0x89abcdefu;

        if (prefill)
        {
            /* main.cpp starts playback from a full ring; phase A only means
             * anything if the consumer starts where the output thread does. */
            while (ring.WaitPrefilled(200) == PeriodRing::Fill::TimedOut)
                ;
        }

        for (uint64_t seq = 0; seq < count; seq++)
        {
            /* The last ring-full has no producer left to wait for. */
            while (hold_full && committed.load(std::memory_order_relaxed) < seq + slots &&
                   committed.load(std::memory_order_relaxed) < count)
                std::this_thread::yield();

            const int16_t *slot = ring.BeginRead();
            if (!slot)
            {
                pass.consumer_saw_null = true;
                return;
            }

            /* Twice: the second pass widens the window in which a producer
             * writing into a slot it does not own would be caught. */
            const int64_t first = CheckSlot(slot);
            const int64_t second = CheckSlot(slot);

            /* Sampled after the ring took its own measurement and before this
             * period is retired, so it can never be below the true fill. */
            const uint64_t fill = committed.load(std::memory_order_relaxed) - seq;
            if (fill < pass.min_observed)
                pass.min_observed = (unsigned)fill;

            if (first < 0 || second < 0 || first != second)
            {
                Fail("period %llu: slot is not a whole period (%lld then %lld) -- "
                     "torn or being written while read",
                     (unsigned long long)seq, (long long)first, (long long)second);
                aborted.store(true);
                ring.Shutdown();
                return;
            }
            if ((uint64_t)first != seq)
            {
                Fail("period %llu: got sequence %lld instead -- %s",
                     (unsigned long long)seq, (long long)first,
                     (uint64_t)first < seq ? "a slot was delivered twice"
                                           : "a period was skipped");
                aborted.store(true);
                ring.Shutdown();
                return;
            }

            ring.CommitRead();
            pass.consumed = seq + 1;
            Tick(seq);
            ApplyPace(consume, rng);
        }
    });

    producer.join();
    consumer.join();

    pass.starves = ring.Starves();
    pass.min_fill = ring.MinFill();

    if (!aborted.load())
    {
        if (pass.producer_saw_null || pass.consumer_saw_null)
            Fail("a side saw shutdown in a pass that never calls Shutdown()");
        if (pass.produced != count || pass.consumed != count)
            Fail("produced %llu and consumed %llu of %llu periods",
                 (unsigned long long)pass.produced, (unsigned long long)pass.consumed,
                 (unsigned long long)count);
        if (pass.consumed > pass.produced)
            Fail("consumed %llu periods but only %llu were committed",
                 (unsigned long long)pass.consumed, (unsigned long long)pass.produced);
        if (pass.min_fill > pass.min_observed)
            Fail("MinFill() reports %u but the consumer saw the ring down to %u",
                 pass.min_fill, pass.min_observed);
        if (ring.Slots() != slots)
            Fail("Slots() reports %u, not %u", ring.Slots(), slots);
    }

    printf("  %-28s %8llu periods  starves=%-8lu min_fill=%u (consumer saw %u)\n",
           name, (unsigned long long)count, pass.starves, pass.min_fill,
           pass.min_observed);
    fflush(stdout);
    return pass;
}

/* Consumer deliberately behind a free-running producer, started from a full
 * ring: the output thread's normal steady state, in which the renderer must
 * never be found missing. */
bool PhaseSaturated(double scale)
{
    const uint64_t count = (uint64_t)(100000 * scale) + 64;
    Pass pass = RunPass("A saturated", count, Pace::Free, Pace::Jitter, true, true);

    if (pass.starves != 0)
        Fail("Starves() is %lu on a ring the consumer never outran", pass.starves);
    if (pass.min_fill == 0)
        Fail("MinFill() is 0 on a ring that was never allowed to empty");
    return pass.consumed == count;
}

bool PhaseStarving(double scale)
{
    const uint64_t count = (uint64_t)(6000 * scale) + 64;

    g_phase.store("B starving", std::memory_order_relaxed);
    PeriodRing ring;
    ring.Init(kSlots, kPeriodFrames);

    std::atomic<uint64_t> committed{0};
    uint64_t consumed = 0;
    unsigned min_observed = ~0u;

    std::thread producer([&] {
        for (uint64_t seq = 0; seq < count; seq++)
        {
            int16_t *slot = ring.BeginWrite();
            if (!slot)
                return;
            FillSlot(slot, seq);
            ring.CommitWrite();
            committed.store(seq + 1, std::memory_order_relaxed);
            Tick(seq);
            /* Slower than any consumer: the ring should be empty almost every
             * time the consumer comes back for a period. */
            std::this_thread::sleep_for(std::chrono::microseconds(80));
        }
    });

    for (uint64_t seq = 0; seq < count; seq++)
    {
        const int16_t *slot = ring.BeginRead();
        if (!slot)
        {
            Fail("BeginRead() returned nullptr with no Shutdown()");
            break;
        }

        const uint64_t fill = committed.load(std::memory_order_relaxed) - seq;
        if (fill < min_observed)
            min_observed = (unsigned)fill;
        const int64_t got = CheckSlot(slot);
        if (got != (int64_t)seq)
        {
            Fail("period %llu: got %lld", (unsigned long long)seq, (long long)got);
            ring.Shutdown();
            break;
        }
        ring.CommitRead();
        consumed = seq + 1;
        Tick(seq);
    }
    producer.join();

    if (consumed != count)
        Fail("consumed %llu of %llu periods", (unsigned long long)consumed,
             (unsigned long long)count);
    /* The whole point of the counter: a consumer this far ahead has to have
     * found the ring empty, and MinFill() has to have bottomed out. */
    if (ring.Starves() == 0)
        Fail("Starves() is 0 after starving the consumer for %llu periods",
             (unsigned long long)count);
    if (ring.Starves() > (unsigned long)count)
        Fail("Starves() is %lu, more than the %llu periods of the run",
             ring.Starves(), (unsigned long long)count);
    if (ring.MinFill() != 0)
        Fail("MinFill() is %u after the consumer outran the producer", ring.MinFill());
    if (ring.MinFill() > min_observed)
        Fail("MinFill() reports %u but the consumer saw %u", ring.MinFill(), min_observed);

    printf("  %-28s %8llu periods  starves=%-8lu min_fill=%u (consumer saw %u)\n",
           "B starving", (unsigned long long)count, ring.Starves(), ring.MinFill(),
           min_observed);
    fflush(stdout);
    return consumed == count;
}

/* main.cpp prints Starves() and MinFill() after Shutdown() and tells the user
 * the core is not sustaining realtime if Starves() is non-zero, so the last
 * BeginRead() of a run must not invent one.  An empty ring on a closed ring is
 * the loop ending, not the renderer failing to keep up.
 *
 * The second half pins the other side of the same fix: a consumer that really
 * did wait, and was woken by Shutdown() rather than by a period, still has to
 * come back nullptr rather than hand out whatever is at tail_. */
bool PhaseShutdownCounters()
{
    g_phase.store("F counters at shutdown", std::memory_order_relaxed);

    PeriodRing ring;
    ring.Init(kSlots, kPeriodFrames);

    int16_t *slot = ring.BeginWrite();
    if (!slot)
    {
        Fail("BeginWrite() returned nullptr on a fresh ring");
        return false;
    }
    FillSlot(slot, 0);
    ring.CommitWrite();

    const int16_t *got = ring.BeginRead();
    if (!got || CheckSlot(got) != 0)
    {
        Fail("a one-period ring did not read back the period");
        return false;
    }
    ring.CommitRead();

    const unsigned long starves_before = ring.Starves();
    const unsigned min_fill_before = ring.MinFill();

    ring.Shutdown();
    if (ring.BeginRead() != nullptr)
        Fail("BeginRead() handed out a slot on a closed, empty ring");
    if (ring.Starves() != starves_before)
        Fail("Shutdown() was counted as a starve: Starves() went %lu -> %lu",
             starves_before, ring.Starves());
    if (ring.Starves() != 0)
        Fail("Starves() is %lu after a run that never waited for the renderer",
             ring.Starves());
    if (ring.MinFill() != min_fill_before)
        Fail("MinFill() moved from %u to %u across Shutdown()", min_fill_before,
             ring.MinFill());

    /* Now the waiting case: block a reader on an empty ring and close it. */
    PeriodRing waiting;
    waiting.Init(kSlots, kPeriodFrames);
    std::atomic<int> state{0};
    std::atomic<bool> handed_out{false};

    std::thread reader([&] {
        state.store(1);
        const int16_t *p = waiting.BeginRead();
        if (p)
            handed_out.store(true);
        state.store(2);
    });

    while (state.load() == 0)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_heartbeat.fetch_add(1, std::memory_order_relaxed);
    waiting.Shutdown();
    reader.join();

    if (handed_out.load())
        Fail("BeginRead() handed out a slot after being woken by Shutdown()");
    /* This one *is* a starve: the ring was empty while the renderer was still
     * supposed to be running, which is exactly what the counter is for. */
    if (waiting.Starves() != 1)
        Fail("a reader that really waited on an empty ring counted %lu starves, not 1",
             waiting.Starves());

    printf("  %-28s starves %lu -> %lu across Shutdown(), a real wait still counts 1\n",
           "F counters at shutdown", starves_before, ring.Starves());
    fflush(stdout);
    return true;
}

/* --render-ahead 1 is a ring of one slot: the producer cannot start a period
 * until the consumer has finished the previous one, so every write is on the
 * full boundary and every read is on the empty one. */
bool PhaseNarrow(double scale)
{
    bool ok = true;
    for (unsigned slots = 1; slots <= 3; slots++)
    {
        char name[32];
        snprintf(name, sizeof name, "E ring of %u slot%s", slots, slots == 1 ? "" : "s");
        Pass pass = RunPass(name, (uint64_t)(20000 * scale) + 64, Pace::Jitter, Pace::Jitter,
                            false, false, slots);
        ok = ok && pass.consumed == (uint64_t)(20000 * scale) + 64;
    }
    return ok;
}

/* Shutdown() from a third thread while both sides are mid-flight.  Both must
 * come back nullptr and both must return; the watchdog catches the case where
 * one of them does not. */
bool PhaseShutdown(double scale)
{
    g_phase.store("D shutdown", std::memory_order_relaxed);

    const int run_ms = (int)(300 * scale) + 30;
    PeriodRing ring;
    ring.Init(kSlots, kPeriodFrames);

    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> producer_saw_null{false};
    std::atomic<bool> consumer_saw_null{false};

    std::thread producer([&] {
        uint32_t rng = 0x2468aceu;
        for (uint64_t seq = 0;; seq++)
        {
            int16_t *slot = ring.BeginWrite();
            if (!slot)
            {
                producer_saw_null.store(true);
                return;
            }
            FillSlot(slot, seq);
            ring.CommitWrite();
            produced.store(seq + 1);
            Tick(seq);
            ApplyPace(Pace::Jitter, rng);
        }
    });

    std::thread consumer([&] {
        uint32_t rng = 0x13579bdu;
        for (uint64_t seq = 0;; seq++)
        {
            const int16_t *slot = ring.BeginRead();
            if (!slot)
            {
                consumer_saw_null.store(true);
                return;
            }
            const int64_t got = CheckSlot(slot);
            if (got != (int64_t)seq)
            {
                Fail("period %llu: got %lld after shutdown -- a closed ring "
                     "handed out a slot",
                     (unsigned long long)seq, (long long)got);
                return;
            }
            ring.CommitRead();
            consumed.store(seq + 1);
            Tick(seq);
            ApplyPace(Pace::Jitter, rng);
        }
    });

    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
        ring.Shutdown();
    });

    closer.join();
    producer.join();
    consumer.join();

    if (!producer_saw_null.load())
        Fail("the producer left its loop without BeginWrite() returning nullptr");
    if (!consumer_saw_null.load())
        Fail("the consumer left its loop without BeginRead() returning nullptr");
    if (ring.BeginWrite() != nullptr)
        Fail("BeginWrite() still hands out a slot after Shutdown()");
    if (ring.BeginRead() != nullptr)
        Fail("BeginRead() still hands out a slot after Shutdown()");
    if (ring.WaitPrefilled(10) != PeriodRing::Fill::Closed)
        Fail("WaitPrefilled() does not report Closed after Shutdown()");

    const uint64_t left = produced.load() - consumed.load();
    if (consumed.load() > produced.load())
        Fail("consumed %llu of %llu committed periods",
             (unsigned long long)consumed.load(), (unsigned long long)produced.load());
    else if (left > kSlots)
        Fail("%llu periods unaccounted for: a ring of %u slots cannot hold them",
             (unsigned long long)left, kSlots);

    printf("  %-28s %8llu produced, %llu consumed, %llu left queued\n",
           "D shutdown mid-flight", (unsigned long long)produced.load(),
           (unsigned long long)consumed.load(), (unsigned long long)left);
    fflush(stdout);
    return producer_saw_null.load() && consumer_saw_null.load();
}

} /* namespace */

int main(int argc, char **argv)
{
    const double scale = argc > 1 ? atof(argv[1]) : 1.0;
    if (scale <= 0)
    {
        fprintf(stderr, "usage: %s [scale]\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);
    printf("PeriodRing: %u frames per period, %u slots unless the phase says\n"
           "otherwise, scale %.2f\n",
           kPeriodFrames, kSlots, scale);

    std::thread watchdog([&] { Watchdog(10.0); });

    const auto start = std::chrono::steady_clock::now();

    PhaseSaturated(scale);
    /* Starves() must stay at 0 exactly when the ring never ran dry, so it is
     * checked on the phase that is not allowed to starve. */
    PhaseStarving(scale);
    RunPass("C matched with jitter", (uint64_t)(1000000 * scale) + 64, Pace::Jitter,
            Pace::Jitter, false);
    PhaseNarrow(scale);
    PhaseShutdown(scale);
    PhaseShutdownCounters();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    g_watchdog_done.store(true, std::memory_order_relaxed);
    watchdog.join();

    const int failures = g_failures.load();
    if (failures)
    {
        printf("ring_test: %d FAILURES in %.1fs\n", failures, secs);
        return 1;
    }
    printf("ring_test: all phases passed in %.1fs\n", secs);
    return 0;
}
