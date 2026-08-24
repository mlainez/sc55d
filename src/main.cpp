/*
 * sc55d -- headless Roland SC-55 emulator daemon.
 *
 * MIDI comes in over the ALSA sequencer, audio goes out over ALSA, and the
 * Nuked-SC55 core is driven from this thread: render one period, hand it to the
 * sound card, repeat.  The blocking write is what paces the emulation.
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "audio_out.h"
#include "bench.h"
#include "core.h"
#include "midi_in.h"
#include "rt.h"

namespace {

struct Options {
    std::string rom_dir = "roms";
    std::string model;
    std::string audio_device = "default";
    std::string client_name = "sc55d";
    int period_frames = 256;
    int periods = 3;
    int cpu = -1;
    int priority = 70;
    bool realtime = true;
    bool verify_roms = true;
    bool quiet_core = false;
    long core_log_limit = 100;
    bool bench = false;
    double bench_seconds = 30.0;
    double bench_warmup = 4.0;
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
        "  --periods <n>           Periods in the buffer (default: 3)\n"
        "\n"
        "MIDI:\n"
        "  --client-name <name>    ALSA sequencer client name (default: sc55d)\n"
        "  --gm                    Send a GM reset at startup\n"
        "  --gs                    Send a GS reset at startup\n"
        "\n"
        "Realtime:\n"
        "  --cpu <n>               Pin the render thread to CPU n\n"
        "  --priority <n>          SCHED_FIFO priority (default: 70)\n"
        "  --no-realtime           Skip mlockall and SCHED_FIFO\n"
        "\n"
        "Other:\n"
        "  --bench                 Render a stress sequence as fast as possible\n"
        "                          and report the realtime ratio\n"
        "  --bench-seconds <n>     Seconds of audio to render (default: 30)\n"
        "  --bench-warmup <n>      Seconds rendered before timing starts, to let\n"
        "                          the firmware boot (default: 4). Too short and\n"
        "                          the run measures silence.\n"
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
        else if (arg == "--cpu")
        {
            if (!take_int(&options->cpu))
                return false;
        }
        else if (arg == "--priority")
        {
            if (!take_int(&options->priority))
                return false;
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
    if (options->bench_seconds <= 0.0)
    {
        fprintf(stderr, "sc55d: --bench-seconds must be positive\n");
        return false;
    }
    return true;
}

int RunDaemon(const Options &options)
{
    if (!MidiIn::Open(options.client_name.c_str()))
        fprintf(stderr, "sc55d: warning: continuing without MIDI input\n");

    if (!AudioOut::Open(options.audio_device.c_str(), (unsigned)Core::SampleRate(),
                        (unsigned)options.period_frames, (unsigned)options.periods))
    {
        MidiIn::Close();
        return 1;
    }

    std::thread midi_thread([]() { MidiIn::Run(g_quit); });

    if (options.realtime)
        Rt::RequestFifoPriority(options.priority);

    Core::PostReset(options.reset);

    printf("sc55d: running (%s, %d Hz native)\n", Core::ModelName(), Core::SampleRate());
    fflush(stdout);

    const int page = Core::PageFrames();

    while (!g_quit.load(std::memory_order_relaxed))
    {
        /* One period is bounded work, so the quit flag only needs checking
         * here rather than on every instruction. */
        while (Core::FramesReady() < (size_t)page)
            Core::Step();

        const bool ok = AudioOut::Write(Core::Frames(), (unsigned)page);
        Core::Consume(page);
        if (!ok)
            break;
    }

    printf("\nsc55d: shutting down (%lu xruns)\n", AudioOut::Xruns());
    if (Core::Overruns())
        fprintf(stderr, "sc55d: %lu frames dropped inside the core buffer\n", Core::Overruns());
    if (Core::SuppressedMessages())
        fprintf(stderr, "sc55d: %lu core messages suppressed\n", Core::SuppressedMessages());
    fflush(stdout);

    g_quit.store(true, std::memory_order_relaxed);
    midi_thread.join();

    AudioOut::Close();
    MidiIn::Close();
    return 0;
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

    Rt::InstallSignalHandlers();
    Core::SetDiagnostics((unsigned long)options.core_log_limit, options.quiet_core);

    if (!Core::Load(options.rom_dir, options.model, options.verify_roms))
        return 1;

    if (!Core::Start(options.period_frames))
        return 1;

    if (options.realtime)
        Rt::LockMemory();
    if (options.cpu >= 0)
        Rt::PinToCpu(options.cpu);

    int status;
    if (options.bench)
    {
        if (options.realtime)
            Rt::RequestFifoPriority(options.priority);
        status = Bench::Run(options.bench_seconds, options.bench_warmup);
    }
    else
    {
        status = RunDaemon(options);
    }

    return status;
}
