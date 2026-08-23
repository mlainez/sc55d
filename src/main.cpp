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
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "audio_out.h"
#include "bench.h"
#include "core.h"
#include "midi_in.h"
#include "romset.h"
#include "rt.h"

namespace {

struct Options {
    std::string rom_dir = "roms";
    std::string model;
    std::string audio_device = "default";
    std::string client_name = "sc55d";
    int period_frames = 256;
    int periods = 3;
    int core_pages = 4;
    int cpu = -1;
    int priority = 70;
    bool realtime = true;
    bool bench = false;
    double bench_seconds = 30.0;
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
        "  --model <name>          mk2, st, mk1, cm300, jv880, scb55, rlp3237,\n"
        "                          sc155, sc155mk2.  Default: autodetect, which\n"
        "                          prefers SC-55mk2.\n"
        "\n"
        "Audio:\n"
        "  --audio-device <name>   ALSA device (default: default)\n"
        "  --period-frames <n>     Frames per period (default: 256)\n"
        "  --periods <n>           Periods in the buffer (default: 3)\n"
        "  --core-pages <n>        Depth of the core's sample ring (default: 4)\n"
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

/* Returns false on a bad argument; sets *done when we printed help. */
bool ParseOptions(int argc, char *argv[], Options *options, bool *done)
{
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];

        /* Consumes the next argv entry as the value of `arg`. */
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
        else if (arg == "--bench")
            options->bench = true;
        else if (arg == "--no-realtime")
            options->realtime = false;
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
        else if (arg == "--core-pages")
        {
            if (!take_int(&options->core_pages))
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
    if (options->core_pages < 2 || options->core_pages > 256)
    {
        fprintf(stderr, "sc55d: --core-pages must be between 2 and 256\n");
        return false;
    }
    if (options->bench_seconds <= 0.0)
    {
        fprintf(stderr, "sc55d: --bench-seconds must be positive\n");
        return false;
    }
    return true;
}

bool DirectoryExists(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/* Resolves the romset, or explains what is missing and returns -1. */
int ResolveRomset(const Options &options)
{
    if (!options.model.empty())
    {
        int romset = ROM_SET_MK2;
        if (!Romset::Parse(options.model, &romset))
        {
            fprintf(stderr, "sc55d: unknown model \"%s\"\n", options.model.c_str());
            return -1;
        }
        return romset;
    }

    const int detected = Romset::Autodetect(options.rom_dir);
    if (detected >= 0)
    {
        printf("sc55d: ROM set autodetect: %s\n", Romset::Name(detected));
        return detected;
    }

    if (!DirectoryExists(options.rom_dir))
        fprintf(stderr, "sc55d: ROM directory %s does not exist.\n", options.rom_dir.c_str());
    else
        fprintf(stderr, "sc55d: no complete ROM set found in %s.\n", options.rom_dir.c_str());

    fprintf(stderr,
            "sc55d: copy your Nuked-SC55 ROM files there, or point --roms at the\n"
            "       directory holding them.  File names follow the upstream\n"
            "       Nuked-SC55 convention; --model picks a set explicitly.\n");
    Romset::PrintExpectedFiles(options.rom_dir, ROM_SET_MK2);
    return -1;
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

    printf("sc55d: running (%s, %d Hz native)\n",
           Romset::Name(romset), Core::SampleRate());
    fflush(stdout);

    const int page = Core::PageFrames();
    std::vector<int16_t> buffer((size_t)page * 2);

    while (!g_quit.load(std::memory_order_relaxed))
    {
        /* One period is bounded work, so the quit flag only needs checking
         * here rather than on every instruction. */
        while (Core::FramesReady() < (uint64_t)page)
            Core::Step();

        Core::PullPage(buffer.data());
        if (!AudioOut::Write(buffer.data(), (unsigned)page))
            break;
    }

    printf("\nsc55d: shutting down (%lu xruns)\n", AudioOut::Xruns());
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

    const int selected = ResolveRomset(options);
    if (selected < 0)
        return 1;

    if (!Romset::Load(options.rom_dir, selected))
    {
        Romset::PrintExpectedFiles(options.rom_dir, selected);
        return 1;
    }
    romset = selected;

    if (!Core::Start(options.period_frames, options.core_pages))
    {
        fprintf(stderr, "sc55d: cannot start the emulation core\n");
        return 1;
    }

    if (options.realtime)
        Rt::LockMemory();
    if (options.cpu >= 0)
        Rt::PinToCpu(options.cpu);

    int status;
    if (options.bench)
    {
        if (options.realtime)
            Rt::RequestFifoPriority(options.priority);
        status = Bench::Run(options.bench_seconds, 1.0);
    }
    else
    {
        status = RunDaemon(options);
    }

    Core::Stop();
    return status;
}
