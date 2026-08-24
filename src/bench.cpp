/*
 * Benchmark mode: how much faster than realtime does this machine render?
 *
 * The MIDI sequence is generated here rather than loaded from a file, so the
 * measurement is reproducible and sc55d stays self-contained.  It is meant to
 * be hard on the core: sixteen parts, a note grid dense enough to hold the
 * SC-55's 24-voice pool saturated, plus continuous controller and pitch bend
 * traffic.
 */
#include "bench.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include "core.h"
#include "ring.h"
#include "rt.h"

namespace Bench {
namespace {

/* FNV-1a over the rendered samples.  Two builds that render the same audio
 * print the same digest, which is how a change to the emulation core gets
 * shown to be behaviour-preserving without needing a reference recording. */
struct Digest {
    uint64_t value = 1469598103934665603ull;
    bool silent = true;

    void Add(const int16_t *frames, int count)
    {
        const size_t samples = (size_t)count * 2;
        for (size_t i = 0; i < samples; i++)
        {
            if (frames[i] != 0)
                silent = false;
        }

        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(frames);
        const size_t length = samples * sizeof(int16_t);
        for (size_t i = 0; i < length; i++)
        {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
};

void Add(std::vector<Event> &events, uint64_t frame, uint8_t a, uint8_t b, uint8_t c)
{
    Event event;
    event.frame = frame;
    event.data[0] = a;
    event.data[1] = b;
    event.data[2] = c;
    event.length = 3;
    events.push_back(event);
}

void Add(std::vector<Event> &events, uint64_t frame, uint8_t a, uint8_t b)
{
    Event event;
    event.frame = frame;
    event.data[0] = a;
    event.data[1] = b;
    event.data[2] = 0;
    event.length = 2;
    events.push_back(event);
}

/* A GM patch per part, drums on channel 10 as usual. */
const uint8_t kPrograms[16] = {
    0,  48, 24, 33, 4,  56, 73, 19,
    11, 0,  40, 61, 81, 89, 16, 52,
};

std::vector<Event> BuildSequence(double seconds, int rate)
{
    const uint64_t total_frames = (uint64_t)(seconds * rate);
    const uint64_t step = (uint64_t)(rate * 0.06); // 60 ms grid
    const uint64_t hold = (uint64_t)(rate * 0.50); // notes ring for 500 ms

    std::vector<Event> events;

    for (uint8_t channel = 0; channel < 16; channel++)
    {
        Add(events, 0, (uint8_t)(0xc0 | channel), kPrograms[channel]);
        Add(events, 0, (uint8_t)(0xb0 | channel), 7, 100);   // volume
        Add(events, 0, (uint8_t)(0xb0 | channel), 11, 127);  // expression
        Add(events, 0, (uint8_t)(0xb0 | channel), 10, (uint8_t)(8 * channel));
        Add(events, 0, (uint8_t)(0xb0 | channel), 91, 64);   // reverb
        Add(events, 0, (uint8_t)(0xb0 | channel), 93, 40);   // chorus
    }

    int index = 0;
    for (uint64_t frame = 0; frame < total_frames; frame += step, index++)
    {
        /* Four parts fire on every grid step, so with a 500 ms hold there are
         * around 32 notes sounding at once -- past the SC-55's voice limit. */
        for (int voice = 0; voice < 4; voice++)
        {
            const uint8_t channel = (uint8_t)((index * 4 + voice) % 16);
            const bool drums = (channel == 9);
            const uint8_t note = drums
                ? (uint8_t)(35 + ((index * 3 + voice) % 16))
                : (uint8_t)(36 + ((index * 7 + voice * 5) % 48));
            const uint8_t velocity = (uint8_t)(80 + ((index + voice) % 40));

            Add(events, frame, (uint8_t)(0x90 | channel), note, velocity);
            if (frame + hold < total_frames)
                Add(events, frame + hold, (uint8_t)(0x80 | channel), note, 64);
        }

        /* Controller traffic keeps the MCU busy between notes. */
        const uint8_t mod_channel = (uint8_t)(index % 16);
        Add(events, frame, (uint8_t)(0xb0 | mod_channel), 1, (uint8_t)(index % 128));
        Add(events, frame, (uint8_t)(0xe0 | mod_channel), 0, (uint8_t)(56 + (index % 16)));
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const Event &a, const Event &b) { return a.frame < b.frame; });
    return events;
}

/* CLOCK_MONOTONIC rather than steady_clock's default: the histogram is about
 * scheduling behaviour, so it wants the same clock the kernel schedules on and
 * a call cheap enough (vDSO, no syscall) to sit inside the render loop. */
uint64_t NowNs()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* Nearest-rank, on an already sorted vector.  With tens of thousands of
 * samples the difference from an interpolating definition is far below the
 * run-to-run noise, and this cannot index past the end. */
uint64_t Percentile(const std::vector<uint64_t> &sorted, double fraction)
{
    if (sorted.empty())
        return 0;
    size_t rank = (size_t)(fraction * (double)sorted.size());
    if (rank >= sorted.size())
        rank = sorted.size() - 1;
    return sorted[rank];
}

/* `period_ns` must still be in render order: the burst and deficit numbers
 * below are about when the slow periods happened, not just how many. */
void PrintTimingHistogram(const std::vector<uint64_t> &period_ns, int page, int rate)
{
    if (period_ns.empty())
        return;

    const double budget_ns = 1e9 * (double)page / (double)rate;

    /* A single late period is harmless: the next early one pays it back out of
     * the ring.  What actually drains the ring is a run of them, so count the
     * longest unbroken run -- and, more directly, track the running deficit in
     * periods, floored at zero because a ring that is already full cannot get
     * any fuller.  The peak of that deficit is the depth the ring must have. */
    size_t over = 0;
    size_t run = 0;
    size_t worst_run = 0;
    size_t worst_index = 0;
    uint64_t worst_ns = 0;
    double backlog = 0.0;
    double worst_backlog = 0.0;
    double total_ns = 0.0;

    for (size_t i = 0; i < period_ns.size(); i++)
    {
        const uint64_t ns = period_ns[i];
        total_ns += (double)ns;

        /* Where the worst period fell separates a startup cost that the ring
         * only has to survive once from a stall that can happen again. */
        if (ns > worst_ns)
        {
            worst_ns = ns;
            worst_index = i;
        }

        if ((double)ns > budget_ns)
        {
            over++;
            run++;
            if (run > worst_run)
                worst_run = run;
        }
        else
        {
            run = 0;
        }

        backlog += ((double)ns - budget_ns) / budget_ns;
        if (backlog < 0.0)
            backlog = 0.0;
        if (backlog > worst_backlog)
            worst_backlog = backlog;
    }

    std::vector<uint64_t> sorted = period_ns;
    std::sort(sorted.begin(), sorted.end());

    printf("\n");
    printf("  period        %d frames = %.3f ms of audio\n", page, budget_ns / 1e6);
    printf("  timed         %zu periods\n", period_ns.size());
    printf("  render time per period, as a ratio of that budget:\n");

    auto line = [&](const char *name, double ns) {
        printf("    %-8s    %7.4fx  (%8.4f ms)\n", name, ns / budget_ns, ns / 1e6);
    };
    line("mean", total_ns / (double)period_ns.size());
    line("median", (double)Percentile(sorted, 0.50));
    line("p90", (double)Percentile(sorted, 0.90));
    line("p99", (double)Percentile(sorted, 0.99));
    line("p99.9", (double)Percentile(sorted, 0.999));
    line("max", (double)sorted.back());

    printf("  over budget   %zu of %zu periods (%.4f%%)\n", over, period_ns.size(),
           100.0 * (double)over / (double)period_ns.size());
    printf("  worst burst   %zu consecutive periods over budget\n", worst_run);
    printf("  worst period  #%zu of %zu (%.1f%% into the run)\n", worst_index,
           period_ns.size(), 100.0 * (double)worst_index / (double)period_ns.size());
    printf("  worst deficit %.2f periods behind realtime at the worst moment\n",
           worst_backlog);
}

/* Renders `frames` frames, posting `events` as their time arrives.  Returns the
 * number of frames actually rendered (short only if interrupted). */
uint64_t Render(uint64_t frames, const std::vector<Event> *events, Digest *digest)
{
    const int page = Core::PageFrames();
    size_t next_event = 0;
    uint64_t rendered = 0;

    while (rendered < frames && !g_quit.load(std::memory_order_relaxed))
    {
        while (events && next_event < events->size() && (*events)[next_event].frame <= rendered)
        {
            const Event &event = (*events)[next_event++];
            Core::PostMidi(event.data, event.length);
        }

        while (Core::FramesReady() < (size_t)page)
            Core::Step();

        if (digest)
            digest->Add(Core::Frames(), page);
        Core::Consume(page);
        rendered += (uint64_t)page;
    }

    return rendered;
}

} // namespace

std::vector<Event> Sequence(double seconds, int rate)
{
    return BuildSequence(seconds, rate);
}

int Run(const Options &options)
{
    const int rate = Core::SampleRate();
    const int page = Core::PageFrames();

    printf("sc55d: benchmark: %.0f s of audio at %d Hz "
           "(after %.0f s of warm-up), output discarded\n",
           options.seconds, rate, options.warmup_seconds);
    if (options.ring_slots > 0)
        printf("sc55d: benchmark: each period handed through a %d-slot ring to a "
               "consumer thread that discards it\n",
               options.ring_slots);
    fflush(stdout);

    /* Let the firmware boot and settle before the clock starts. A real SC-55
     * takes a second or two to come up, and MIDI sent before it is ready is
     * simply dropped -- which shows up as a silent run rather than an error,
     * so the default here is deliberately generous. */
    Core::PostReset(Core::Reset::GS);
    Render((uint64_t)(options.warmup_seconds * rate), nullptr, nullptr);

    const std::vector<Event> events = BuildSequence(options.seconds, rate);

    const uint64_t total_frames = (uint64_t)(options.seconds * rate);
    const uint64_t chunk_frames = (uint64_t)rate; // report the worst second
    const int page_count = std::max(1, (int)(chunk_frames / (uint64_t)page));

    /* Preallocated: a reallocation partway through would land inside a timed
     * period and be recorded as a spike that is ours, not the core's. */
    std::vector<uint64_t> period_ns;
    if (options.histogram)
        period_ns.reserve((size_t)(total_frames / (uint64_t)page) + 2);

    /* Same ring the daemon uses, driven the same way, so what it adds to a
     * period here is what it adds to a period there. */
    PeriodRing ring;
    std::thread consumer;
    if (options.ring_slots > 0)
    {
        ring.Init((unsigned)options.ring_slots, (unsigned)page);
        consumer = std::thread([&ring] {
            Rt::BlockSignals();
            Rt::NameThread("sc55d-sink");
            while (ring.BeginRead())
                ring.CommitRead();
        });
    }

    Digest digest;
    size_t next_event = 0;
    uint64_t rendered = 0;
    double worst_ratio = 0.0;
    bool have_worst = false;

    const auto started = std::chrono::steady_clock::now();
    auto chunk_started = started;
    uint64_t chunk_rendered = 0;

    while (rendered < total_frames && !g_quit.load(std::memory_order_relaxed))
    {
        while (next_event < events.size() && events[next_event].frame <= rendered)
        {
            const Event &event = events[next_event++];
            Core::PostMidi(event.data, event.length);
        }

        /* Posting MIDI and hashing the output are the benchmark's own work --
         * in the daemon the first belongs to another thread and the second
         * does not exist -- so the timed window skips over both and covers
         * only what the render thread actually does per period. */
        const uint64_t core_started = options.histogram ? NowNs() : 0;
        while (Core::FramesReady() < (size_t)page)
            Core::Step();
        const uint64_t core_ended = options.histogram ? NowNs() : 0;

        digest.Add(Core::Frames(), page);

        const uint64_t tail_started = options.histogram ? NowNs() : 0;
        if (options.ring_slots > 0)
        {
            int16_t *slot = ring.BeginWrite();
            if (!slot)
                break;
            memcpy(slot, Core::Frames(), (size_t)page * 2 * sizeof(int16_t));
            Core::Consume(page);
            ring.CommitWrite();
        }
        else
        {
            Core::Consume(page);
        }

        if (options.histogram)
            period_ns.push_back((core_ended - core_started) + (NowNs() - tail_started));

        rendered += (uint64_t)page;
        chunk_rendered += (uint64_t)page;

        if (chunk_rendered >= (uint64_t)page_count * (uint64_t)page)
        {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - chunk_started).count();
            const double ratio = wall > 0.0 ? ((double)chunk_rendered / rate) / wall : 0.0;
            if (!have_worst || ratio < worst_ratio)
            {
                worst_ratio = ratio;
                have_worst = true;
            }
            chunk_started = now;
            chunk_rendered = 0;
        }
    }

    const double wall = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - started).count();

    if (consumer.joinable())
    {
        ring.Shutdown();
        consumer.join();
    }

    const double rendered_seconds = (double)rendered / rate;
    const double ratio = wall > 0.0 ? rendered_seconds / wall : 0.0;

    printf("\n");
    printf("  rendered      %.2f s of audio (%llu frames at %d Hz)\n",
           rendered_seconds, (unsigned long long)rendered, rate);
    printf("  wall clock    %.2f s\n", wall);
    printf("  realtime      %.3fx  (rendered seconds per wall-clock second)\n", ratio);
    if (have_worst)
        printf("  worst second  %.3fx\n", worst_ratio);
    printf("  audio digest  %016llx%s\n", (unsigned long long)digest.value,
           digest.silent ? "  (SILENT)" : "");
    if (digest.silent)
        printf("\n  warning: the run produced no audio at all, so the digest is the\n"
               "           digest of silence and compares equal between any two builds.\n"
               "           Do not use it to check a change to the emulator.\n");

    if (options.histogram)
        PrintTimingHistogram(period_ns, page, rate);

    printf("\n");
    printf("  verdict       %s\n",
           (have_worst ? worst_ratio : ratio) >= 1.0
               ? "holds realtime"
               : "TOO SLOW for realtime on this machine");
    fflush(stdout);

    return (have_worst ? worst_ratio : ratio) >= 1.0 ? 0 : 1;
}

} // namespace Bench
