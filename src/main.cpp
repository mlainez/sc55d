/*
 * SPDX-License-Identifier: MIT
 *
 * sc55d -- headless Roland SC-55 emulator daemon.
 *
 * MIDI comes in over the ALSA sequencer and audio goes out over ALSA.  There
 * are two ways to drive the core between them:
 *
 *   --render-ahead 0  one thread renders a period and writes it, in turn.  The
 *                     blocking write paces the emulation and latency is as low
 *                     as the ALSA buffer allows, but every scheduling hiccup
 *                     eats into the audio deadline.
 *
 *   --render-ahead N  a render thread fills a ring of N periods while the
 *                     output thread blocks in ALSA.  On a multi-core board the
 *                     two run at once and the queued periods absorb the
 *                     jitter, at the cost of N periods of MIDI latency.  This
 *                     is the default, and what makes a Pi 3 usable.
 *
 * MIDI input has its own thread either way.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "bench.h"
#include "core.h"
#include "midi_in.h"
#include "ring.h"
#include "rt.h"

namespace {

struct Options {
    std::string rom_dir = "roms";
    std::string model;
    std::string audio_device = "default";
    std::string client_name = "sc55d";
    int period_frames = 256;
    int periods = 3;
    int render_ahead = 4;
    bool render_ahead_set = false;
    int cpu = -1;
    int output_cpu = -1;
    int priority = 70;
    bool realtime = true;
    bool verify_roms = true;
    bool quiet_core = false;
    long core_log_limit = 100;
    bool bench = false;
    double selftest = 0.0;
    double bench_seconds = 30.0;
    double bench_warmup = 4.0;
    bool bench_histogram = false;
    bool bench_ring = false;
    Core::Reset reset = Core::Reset::None;
};

void PrintUsage()
{
    printf(
        "Usage: sc55d [options]\n"
        "\n"
        "A headless Nuked-SC55 frontend: ALSA sequencer MIDI in, ALSA audio out.\n"
        "\n"
        "ROMs:\n"
        "  --roms <dir>            Directory holding the ROM files (default: roms)\n"
        "  --model <name>          ROM set to use; default is autodetect\n"
        "  --list-models           List the ROM sets the core knows about\n"
        "  --no-verify-roms        Match ROMs by file name instead of SHA-256\n"
        "\n"
        "Logging:\n"
        "  --core-log-limit <n>    Cap the emulator's own messages (default: 100,\n"
        "                          0 = no cap) so a bad ROM cannot flood the journal\n"
        "  --quiet-core            Silence the emulator's messages entirely\n"
        "\n"
        "Audio:\n"
        "  --audio-device <name>   ALSA device (default: default)\n"
        "  --period-frames <n>     Frames per period (default: 256)\n"
        "  --periods <n>           Periods in the ALSA buffer (default: 3)\n"
        "  --render-ahead <n>      Periods the render thread may run ahead of\n"
        "                          playback, on its own core.  Absorbs\n"
        "                          scheduling jitter; costs n periods of MIDI\n"
        "                          latency.  0 renders and writes from one\n"
        "                          thread, for the lowest latency.\n"
        "                          Default is about 15 ms whatever the period\n"
        "                          size (8 at 128 frames, 4 at 256, 2 at 512),\n"
        "                          or 0 on a single-core machine.\n"
        "\n"
        "MIDI:\n"
        "  --client-name <name>    ALSA sequencer client name (default: sc55d)\n"
        "  --gm                    Send a GM reset at startup\n"
        "  --gs                    Send a GS reset at startup\n"
        "\n"
        "Realtime:\n"
        "  --cpu <n>               Pin the render thread to CPU n\n"
        "  --output-cpu <n>        Pin the output thread to CPU n\n"
        "  --priority <n>          SCHED_FIFO priority for the render thread\n"
        "                          (default: 70).  MIDI runs one step above it\n"
        "                          and the output thread two.\n"
        "  --no-realtime           Skip mlockall and SCHED_FIFO\n"
        "\n"
        "Other:\n"
        "  --bench                 Render a stress sequence as fast as possible\n"
        "                          and report the realtime ratio\n"
        "  --bench-seconds <n>     Seconds of audio to render (default: 30)\n"
        "  --bench-warmup <n>      Seconds rendered before timing starts, to let\n"
        "                          the firmware boot (default: 4). Too short and\n"
        "                          the run measures silence.\n"
        "  --bench-histogram       Time every period and print the distribution\n"
        "                          against its realtime budget: this is what says\n"
        "                          how deep --render-ahead has to be\n"
        "  --bench-ring            Hand every period through the render-ahead ring\n"
        "                          to a consumer that discards it, so the cost of\n"
        "                          the hand-off itself shows up in the numbers\n"
        "  --selftest <seconds>    Play the benchmark's stress sequence to the\n"
        "                          audio device in realtime, through the same\n"
        "                          MIDI path as the sequencer. Answers \"does\n"
        "                          this box make a sound\" with no aconnect.\n"
        "  -h, --help              This message\n");
}

bool ParseInt(const char *text, int *out)
{
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (!end || *end != '\0' || end == text)
        return false;
    *out = (int)value;
    return true;
}

bool ParseDouble(const char *text, double *out)
{
    char *end = nullptr;
    const double value = strtod(text, &end);
    if (!end || *end != '\0' || end == text)
        return false;
    *out = value;
    return true;
}

/* Returns false on a bad argument; sets *done when we already printed output. */
bool ParseOptions(int argc, char *argv[], Options *options, bool *done)
{
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];

        auto take = [&](const char **value) {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "sc55d: %s needs a value\n", arg.c_str());
                return false;
            }
            *value = argv[++i];
            return true;
        };
        auto take_int = [&](int *out) {
            const char *value = nullptr;
            if (!take(&value))
                return false;
            if (!ParseInt(value, out))
            {
                fprintf(stderr, "sc55d: %s: \"%s\" is not a number\n", arg.c_str(), value);
                return false;
            }
            return true;
        };

        const char *value = nullptr;

        if (arg == "-h" || arg == "--help")
        {
            PrintUsage();
            *done = true;
            return true;
        }
        else if (arg == "--list-models")
        {
            Core::PrintModels();
            *done = true;
            return true;
        }
        else if (arg == "--bench")
            options->bench = true;
        else if (arg == "--bench-histogram")
            options->bench_histogram = true;
        else if (arg == "--bench-ring")
            options->bench_ring = true;
        else if (arg == "--no-realtime")
            options->realtime = false;
        else if (arg == "--no-verify-roms")
            options->verify_roms = false;
        else if (arg == "--quiet-core")
            options->quiet_core = true;
        else if (arg == "--core-log-limit")
        {
            int limit = 0;
            if (!take_int(&limit))
                return false;
            if (limit < 0)
            {
                fprintf(stderr, "sc55d: --core-log-limit cannot be negative\n");
                return false;
            }
            options->core_log_limit = limit;
        }
        else if (arg == "--gm")
            options->reset = Core::Reset::GM;
        else if (arg == "--gs")
            options->reset = Core::Reset::GS;
        else if (arg == "--roms")
        {
            if (!take(&value))
                return false;
            options->rom_dir = value;
        }
        else if (arg == "--model")
        {
            if (!take(&value))
                return false;
            options->model = value;
        }
        else if (arg == "--audio-device")
        {
            if (!take(&value))
                return false;
            options->audio_device = value;
        }
        else if (arg == "--client-name")
        {
            if (!take(&value))
                return false;
            options->client_name = value;
        }
        else if (arg == "--period-frames")
        {
            if (!take_int(&options->period_frames))
                return false;
        }
        else if (arg == "--periods")
        {
            if (!take_int(&options->periods))
                return false;
        }
        else if (arg == "--render-ahead")
        {
            if (!take_int(&options->render_ahead))
                return false;
            options->render_ahead_set = true;
        }
        else if (arg == "--cpu")
        {
            if (!take_int(&options->cpu))
                return false;
        }
        else if (arg == "--output-cpu")
        {
            if (!take_int(&options->output_cpu))
                return false;
        }
        else if (arg == "--priority")
        {
            if (!take_int(&options->priority))
                return false;
        }
        else if (arg == "--selftest")
        {
            if (!take(&value))
                return false;
            if (!ParseDouble(value, &options->selftest) || options->selftest <= 0.0)
            {
                fprintf(stderr, "sc55d: --selftest: \"%s\" is not a positive number\n", value);
                return false;
            }
        }
        else if (arg == "--bench-seconds")
        {
            if (!take(&value))
                return false;
            if (!ParseDouble(value, &options->bench_seconds))
            {
                fprintf(stderr, "sc55d: --bench-seconds: \"%s\" is not a number\n", value);
                return false;
            }
        }
        else if (arg == "--bench-warmup")
        {
            if (!take(&value))
                return false;
            if (!ParseDouble(value, &options->bench_warmup) || options->bench_warmup < 0.0)
            {
                fprintf(stderr, "sc55d: --bench-warmup: \"%s\" is not a non-negative number\n", value);
                return false;
            }
        }
        else
        {
            fprintf(stderr, "sc55d: unknown option %s\n", arg.c_str());
            return false;
        }
    }

    if (options->period_frames < 16 || options->period_frames > 16384)
    {
        fprintf(stderr, "sc55d: --period-frames must be between 16 and 16384\n");
        return false;
    }
    if (options->periods < 2 || options->periods > 64)
    {
        fprintf(stderr, "sc55d: --periods must be between 2 and 64\n");
        return false;
    }
    if (options->render_ahead < 0 || options->render_ahead > 64)
    {
        fprintf(stderr, "sc55d: --render-ahead must be between 0 and 64\n");
        return false;
    }
    if (options->bench_seconds <= 0.0)
    {
        fprintf(stderr, "sc55d: --bench-seconds must be positive\n");
        return false;
    }
    if (options->bench_ring && options->render_ahead < 1)
    {
        fprintf(stderr, "sc55d: --bench-ring needs --render-ahead of at least 1\n");
        return false;
    }
    return true;
}

/*
 * Renders one period, letting MIDI in as it goes.
 *
 * The drain has to happen more often than once a period.  A period is 3.9 ms
 * at the defaults, and quantising every note-on to that boundary would be
 * audible -- worse than the latency the ring adds, because it is jitter
 * rather than delay.  Sixty-four frames is under a millisecond, and the check
 * costs one atomic load when there is nothing waiting, which is almost
 * always.
 *
 * This is also the only place the core is handed MIDI, which is the point:
 * see midi_queue.h.
 */
void RenderPeriod(int page)
{
    const size_t kMidiChunk = 64;

    while (Core::FramesReady() < (size_t)page)
    {
        MidiIn::DrainToCore();

        const size_t target = std::min((size_t)page, Core::FramesReady() + kMidiChunk);
        while (Core::FramesReady() < target)
            Core::Step();
    }
}

/*
 * Thread priorities, highest first: output, MIDI, render.  The output thread
 * must never miss its wake-up, and its work is bounded and small -- the plug
 * layer's rate conversion, which runs inside snd_pcm_writei(), around 1% of a
 * core with the default converter.  MIDI is idle until an event arrives and
 * only adds latency if it is made to wait.  The renderer is the one that wants
 * every cycle it can get, so it goes last.
 */
int OutputPriority(const Options &options) { return options.priority + 2; }
int MidiPriority(const Options &options) { return options.priority + 1; }

/*
 * Plays the benchmark's stress sequence into the daemon, in realtime, from a
 * thread that is not the render thread -- so it goes through MidiQueue exactly
 * as sequencer input does.
 *
 * This is the only way to exercise the whole path end to end without an
 * external MIDI source, and on a headless box it answers the first question
 * anyone actually has: does this thing make a sound?
 */
void SelftestThread(const Options &options, int rate)
{
    Rt::BlockSignals();
    Rt::NameThread("sc55d-selftest");
    if (options.realtime)
        Rt::RequestFifoPriority("selftest", MidiPriority(options));

    const std::vector<Bench::Event> events = Bench::Sequence(options.selftest, rate);

    /* Let the firmware finish booting.  MIDI sent to an SC-55 that is still
     * coming up is simply dropped, which looks like silence rather than an
     * error. */
    const auto started = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    for (const Bench::Event &event : events)
    {
        if (g_quit.load(std::memory_order_relaxed))
            return;

        const auto due = started + std::chrono::nanoseconds(
                                       (long long)(1e9 * (double)event.frame / rate));
        std::this_thread::sleep_until(due);
        MidiIn::Inject(event.data, event.length);
    }

    printf("sc55d: selftest sequence finished (%zu events)\n", events.size());
    fflush(stdout);

    /* End the run.  --selftest is a one-shot check, not a mode to be killed
     * out of: without this the render and output loops keep spinning on
     * !g_quit and the daemon plays on until a signal arrives.  The tail is
     * for the last notes' release envelopes and for the render-ahead ring to
     * drain, so the check does not end by cutting itself off. */
    for (int i = 0; i < 20 && !g_quit.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    g_quit.store(true, std::memory_order_relaxed);
}

void MidiThread(const Options &options)
{
    Rt::BlockSignals();
    Rt::NameThread("sc55d-midi");
    if (options.realtime)
        Rt::RequestFifoPriority("MIDI", MidiPriority(options));
    MidiIn::Run(g_quit);
}

/* Fills the ring until we are asked to stop.  This is the thread that wants a
 * core to itself: --cpu pins it there. */
void RenderThread(const Options &options, PeriodRing *ring, int page)
{
    Rt::BlockSignals();
    Rt::NameThread("sc55d-render");
    if (options.realtime)
        Rt::RequestFifoPriority("render", options.priority);
    if (options.cpu >= 0)
        Rt::PinToCpu("render", options.cpu);

    while (!g_quit.load(std::memory_order_relaxed))
    {
        int16_t *slot = ring->BeginWrite();
        if (!slot)
            break;

        /* One period is bounded work, so the quit flag only needs checking
         * here rather than on every instruction. */
        RenderPeriod(page);

        /* A period is 1 KiB at the default settings -- a few hundred KiB a
         * second, and cheaper than teaching the core's sample callback to
         * handle a slot boundary landing mid-instruction. */
        memcpy(slot, Core::Frames(), (size_t)page * 2 * sizeof(int16_t));
        Core::Consume(page);
        ring->CommitWrite();
    }

    /* Whatever ended the loop, the consumer must not be left waiting. */
    ring->Shutdown();
}

/* Two threads on two cores: this one only ever blocks in snd_pcm_writei(). */
bool RunPipelined(const Options &options, int page)
{
    PeriodRing ring;
    ring.Init((unsigned)options.render_ahead, (unsigned)page);

    std::thread render([&] { RenderThread(options, &ring, page); });

    Rt::NameThread("sc55d-output");
    if (options.realtime)
        Rt::RequestFifoPriority("output", OutputPriority(options));
    if (options.output_cpu >= 0)
        Rt::PinToCpu("output", options.output_cpu);

    /* Start playback from a full ring: opening the card and starting to write
     * with one period in hand throws away the head start the ring exists for. */
    PeriodRing::Fill fill = PeriodRing::Fill::TimedOut;
    while (fill == PeriodRing::Fill::TimedOut && !g_quit.load(std::memory_order_relaxed))
        fill = ring.WaitPrefilled(100);

    bool ok = true;
    while (fill == PeriodRing::Fill::Ready && !g_quit.load(std::memory_order_relaxed))
    {
        const int16_t *slot = ring.BeginRead();
        if (!slot)
            break;
        ok = AudioOut::Write(slot, (unsigned)page);
        ring.CommitRead();
        if (!ok)
            break;
    }

    g_quit.store(true, std::memory_order_relaxed);
    ring.Shutdown();
    render.join();

    printf("sc55d: render-ahead: %lu starves, %u of %u periods still queued "
           "at the worst moment\n",
           ring.Starves(), ring.MinFill(), ring.Slots());
    if (ring.Starves())
        fprintf(stderr,
                "sc55d: the renderer ran dry: the core is not sustaining "
                "realtime on this machine\n");
    return ok;
}

/* One thread: render a period, write it, repeat.  Lowest latency, and the only
 * thing that works on a single core. */
bool RunSerial(int page)
{
    while (!g_quit.load(std::memory_order_relaxed))
    {
        RenderPeriod(page);

        const bool ok = AudioOut::Write(Core::Frames(), (unsigned)page);
        Core::Consume(page);
        if (!ok)
            return false;
    }
    return true;
}

int RunDaemon(const Options &options)
{
    /* MidiQueue takes one producer.  In selftest mode that producer is the
     * selftest thread, so the sequencer stays shut -- two of them pushing at
     * once would corrupt the queue, and a diagnostic that also accepts live
     * input is a diagnostic you cannot trust. */
    if (options.selftest > 0.0)
        printf("sc55d: selftest mode: sequencer input is disabled for this run\n");
    else if (!MidiIn::Open(options.client_name.c_str()))
        fprintf(stderr, "sc55d: warning: continuing without MIDI input\n");

    if (!AudioOut::Open(options.audio_device.c_str(), (unsigned)Core::SampleRate(),
                        (unsigned)options.period_frames, (unsigned)options.periods))
    {
        MidiIn::Close();
        return 1;
    }

    /* Before any other thread exists, because this one goes straight into the
     * core rather than through the queue. */
    Core::PostReset(options.reset);

    std::thread midi_thread;
    std::thread selftest_thread;
    if (options.selftest > 0.0)
    {
        printf("sc55d: selftest: %.0f s of the stress sequence, starting in 3 s\n",
               options.selftest);
        selftest_thread = std::thread([&] { SelftestThread(options, Core::SampleRate()); });
    }
    else
    {
        midi_thread = std::thread([&] { MidiThread(options); });
    }

    const int page = Core::PageFrames();
    const double period_ms = 1000.0 * (double)page / (double)Core::SampleRate();

    if (options.render_ahead > 0)
        printf("sc55d: running (%s, %d Hz native), rendering up to %d periods "
               "ahead (%.1f ms)\n",
               Core::ModelName(), Core::SampleRate(), options.render_ahead,
               options.render_ahead * period_ms);
    else
        printf("sc55d: running (%s, %d Hz native), single-threaded render\n",
               Core::ModelName(), Core::SampleRate());
    fflush(stdout);

    bool ok;
    if (options.render_ahead > 0)
    {
        ok = RunPipelined(options, page);
    }
    else
    {
        /* Nothing else is going to use the core, so this thread takes the
         * realtime settings meant for the renderer. */
        Rt::NameThread("sc55d-render");
        if (options.realtime)
            Rt::RequestFifoPriority("render", options.priority);
        if (options.cpu >= 0)
            Rt::PinToCpu("render", options.cpu);
        ok = RunSerial(page);
    }

    printf("\nsc55d: shutting down (%lu xruns)\n", AudioOut::Xruns());
    if (Core::Overruns())
        fprintf(stderr, "sc55d: %lu frames dropped inside the core buffer\n", Core::Overruns());
    if (MidiIn::Dropped())
        fprintf(stderr, "sc55d: %lu MIDI bytes dropped: the render thread could not "
                        "keep up with the sequencer\n", MidiIn::Dropped());
    if (Core::SuppressedMessages())
        fprintf(stderr, "sc55d: %lu core messages suppressed\n", Core::SuppressedMessages());
    fflush(stdout);

    g_quit.store(true, std::memory_order_relaxed);
    if (selftest_thread.joinable())
        selftest_thread.join();
    if (midi_thread.joinable())
        midi_thread.join();

    AudioOut::Close();
    MidiIn::Close();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    Options options;
    bool done = false;
    if (!ParseOptions(argc, argv, &options, &done))
        return 2;
    if (done)
        return 0;

    /*
     * The stalls a ring absorbs are a fixed number of milliseconds long: they
     * are scheduling artefacts, not emulator work.  Measured over 80 minutes
     * of rendered audio, the emulator's own worst period never exceeded 0.8x
     * of its budget, while the worst *observed* period was 4-70 ms regardless
     * of whether a period was 1.9 ms or 7.7 ms of audio -- and an
     * emulator-free control loop on the same machine stalled by the same
     * amounts.
     *
     * So the useful depth is a fixed amount of time, and the number of periods
     * that takes scales with 1/period_frames.  Four is right at 256 frames;
     * this keeps the same ~15 ms at any other size, which is where the default
     * was silently wrong before -- 4 periods at 128 frames is 7.7 ms, less
     * than the median stall.
     */
    if (!options.render_ahead_set)
        options.render_ahead = std::clamp(4 * 256 / options.period_frames, 2, 16);

    /* A ring only buys anything if there is a second core to put the renderer
     * on.  With one core the two threads just take turns, and pay for a mutex,
     * a condition variable and a period of latency to do it.  Not in a
     * benchmark, though: --bench-ring exists precisely to measure that cost. */
    if (!options.render_ahead_set && !options.bench
        && std::thread::hardware_concurrency() < 2)
    {
        options.render_ahead = 0;
        printf("sc55d: single core: rendering and writing on one thread "
               "(--render-ahead to override)\n");
    }

    Rt::InstallSignalHandlers();
    Core::SetDiagnostics((unsigned long)options.core_log_limit, options.quiet_core);

    if (!Core::Load(options.rom_dir, options.model, options.verify_roms))
        return 1;

    if (!Core::Start(options.period_frames))
        return 1;

    if (options.realtime)
    {
        Rt::LockMemory();
        Rt::WarnAboutRtThrottle();
    }

    int status;
    if (options.bench)
    {
        /* No output thread in a benchmark: this one is the renderer. */
        if (options.realtime)
            Rt::RequestFifoPriority("render", options.priority);
        if (options.cpu >= 0)
            Rt::PinToCpu("render", options.cpu);

        Bench::Options bench;
        bench.seconds = options.bench_seconds;
        bench.warmup_seconds = options.bench_warmup;
        bench.histogram = options.bench_histogram;
        bench.ring_slots = options.bench_ring ? options.render_ahead : 0;
        status = Bench::Run(bench);
    }
    else
    {
        status = RunDaemon(options);
    }

    return status;
}
