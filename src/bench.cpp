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
#include <vector>

#include "core.h"
#include "rt.h"

namespace Bench {
namespace {

struct Event {
    uint64_t frame;
    uint8_t data[3];
    uint8_t length;
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

/* Renders `frames` frames, posting `events` as their time arrives.  Returns the
 * number of frames actually rendered (short only if interrupted). */
uint64_t Render(uint64_t frames, const std::vector<Event> *events,
                std::vector<int16_t> &scratch)
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

        while (Core::FramesReady() < (uint64_t)page)
            Core::Step();

        Core::PullPage(scratch.data());
        rendered += (uint64_t)page;
    }

    return rendered;
}

} // namespace

int Run(double seconds, double warmup_seconds)
{
    const int rate = Core::SampleRate();
    const int page = Core::PageFrames();
    std::vector<int16_t> scratch((size_t)page * 2);

    printf("sc55d: benchmark: %.0f s of audio at %d Hz "
           "(after %.0f s of warm-up), output discarded\n",
           seconds, rate, warmup_seconds);
    fflush(stdout);

    /* Let the firmware boot and settle before the clock starts. */
    Core::PostReset(Core::Reset::GS);
    Render((uint64_t)(warmup_seconds * rate), nullptr, scratch);

    const std::vector<Event> events = BuildSequence(seconds, rate);

    const uint64_t total_frames = (uint64_t)(seconds * rate);
    const uint64_t chunk_frames = (uint64_t)rate; // report the worst second
    const int page_count = std::max(1, (int)(chunk_frames / (uint64_t)page));

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

        while (Core::FramesReady() < (uint64_t)page)
            Core::Step();

        Core::PullPage(scratch.data());
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
    const double rendered_seconds = (double)rendered / rate;
    const double ratio = wall > 0.0 ? rendered_seconds / wall : 0.0;

    printf("\n");
    printf("  rendered      %.2f s of audio (%llu frames at %d Hz)\n",
           rendered_seconds, (unsigned long long)rendered, rate);
    printf("  wall clock    %.2f s\n", wall);
    printf("  realtime      %.3fx  (rendered seconds per wall-clock second)\n", ratio);
    if (have_worst)
        printf("  worst second  %.3fx\n", worst_ratio);
    printf("\n");
    printf("  verdict       %s\n",
           (have_worst ? worst_ratio : ratio) >= 1.0
               ? "holds realtime"
               : "TOO SLOW for realtime on this machine");
    fflush(stdout);

    return (have_worst ? worst_ratio : ratio) >= 1.0 ? 0 : 1;
}

} // namespace Bench
