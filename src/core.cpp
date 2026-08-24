/*
 * Thin wrapper over the Nuked-SC55 backend library.
 *
 * The core pushes finished stereo frames through a sample callback; we clamp
 * them to 16-bit and append them to a small linear buffer that the render loop
 * hands straight to ALSA.  Because the loop only steps until one period is
 * ready, and a single instruction can yield at most two frames, the buffer
 * never holds much more than a period -- so there is no ring, no wrap handling
 * and no copy between here and snd_pcm_writei().
 */
#include "core.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "audio.h"
#include "diagnostics.h"
#include "emu.h"
#include "pcm.h"
#include "rom_loader.h"

namespace Core {
namespace {

Emulator g_emu;

/* Holds the ROM images the emulator was handed: LoadRoms() does not promise to
 * copy them, so this has to outlive g_emu. */
common::LoadRomsetResult g_romset;
bool g_have_romset = false;

unsigned long g_diag_limit = 0;
unsigned long g_diag_seen = 0;
unsigned long g_diag_suppressed = 0;

/* The core logs from whichever thread is inside it, which for sc55d is only
 * ever the render thread. */
void DiagCallback(Diag_Category category, std::string_view message)
{
    if (g_diag_limit && g_diag_seen >= g_diag_limit)
    {
        g_diag_suppressed++;
        return;
    }

    g_diag_seen++;
    fprintf(stderr, "sc55d: core: %s: %.*s\n", ToCString(category),
            (int)message.size(), message.data());

    if (g_diag_limit && g_diag_seen == g_diag_limit)
        fprintf(stderr, "sc55d: core: further messages suppressed "
                        "(raise --core-log-limit to see them)\n");
}

int g_page_frames = 0;
std::vector<int16_t> g_buffer;
size_t g_frames = 0;
unsigned long g_overruns = 0;

void SampleCallback(void * /*userdata*/, const AudioFrame<int32_t> &frame)
{
    if (g_frames * 2 + 2 > g_buffer.size())
    {
        g_overruns++;
        return;
    }

    AudioFrame<int16_t> out;
    Normalize(frame, out);
    g_buffer[g_frames * 2 + 0] = out.left;
    g_buffer[g_frames * 2 + 1] = out.right;
    g_frames++;
}

} // namespace

void SetDiagnostics(unsigned long limit, bool quiet)
{
    g_diag_limit = limit;
    Diag_SetCallback(quiet ? nullptr : DiagCallback);
}

unsigned long SuppressedMessages()
{
    return g_diag_suppressed;
}

bool Load(const std::string &rom_dir, const std::string &model, bool verify)
{
    const common::RomOverrides overrides{};
    const common::RomLoader loader =
        verify ? common::RomLoader::Hashing : common::RomLoader::Legacy;

    const common::LoadRomsetError error =
        common::LoadRomset(rom_dir, model, loader, overrides, g_romset);

    common::PrintLoadRomsetDiagnostics(stderr, error, g_romset);

    if (error != common::LoadRomsetError{})
    {
        fprintf(stderr,
                "sc55d: no usable ROM set in %s.\n"
                "sc55d: put the Nuked-SC55 ROM files there, or pass --roms <dir>.\n"
                "sc55d: --list-models shows the sets the core knows about, and\n"
                "       --no-verify-roms falls back to matching by file name.\n",
                rom_dir.c_str());
        return false;
    }

    g_have_romset = true;
    return true;
}

bool Start(int page_frames)
{
    if (!g_have_romset)
        return false;

    g_page_frames = page_frames;

    /* One instruction can emit two frames when the core is oversampling, so a
     * period plus a little slack is all the headroom the buffer needs. */
    g_buffer.assign((size_t)(page_frames + 8) * 2, 0);
    g_frames = 0;

    EMU_Options options;
    options.lcd_backend = nullptr; // headless: no LCD emulation at all

    if (!g_emu.Init(options))
    {
        fprintf(stderr, "sc55d: cannot initialise the emulator\n");
        return false;
    }

    if (!g_emu.LoadRoms(g_romset.romset, g_romset.romset_info))
    {
        fprintf(stderr, "sc55d: the emulator rejected the ROM set\n");
        return false;
    }

    g_emu.Reset();
    g_emu.SetSampleCallback(SampleCallback, nullptr);
    return true;
}

const char *ModelName()
{
    return g_have_romset ? RomsetName(g_romset.romset) : "none";
}

int SampleRate()
{
    return (int)PCM_GetOutputFrequency(g_emu.GetPCM());
}

int PageFrames()
{
    return g_page_frames;
}

void Step()
{
    g_emu.Step();
}

size_t FramesReady()
{
    return g_frames;
}

const int16_t *Frames()
{
    return g_buffer.data();
}

void Consume(int frames)
{
    const size_t taken = (size_t)frames;
    const size_t left = g_frames - taken;
    if (left)
        memmove(g_buffer.data(), g_buffer.data() + taken * 2, left * 2 * sizeof(int16_t));
    g_frames = left;
}

/*
 * Render thread only.  This is not a preference.
 *
 * The backend's MIDI FIFO -- mcu_t::uart_buffer with its uart_write_ptr and
 * uart_read_ptr -- has no synchronisation whatsoever, so calling this from a
 * second thread is a data race, and on a weakly ordered machine a real one.
 * sc55d used to do exactly that, as upstream's RtMidi callback still does.
 * Everything that arrives from elsewhere goes through MidiQueue and is handed
 * over here by the render thread; see midi_queue.h for why the fix lives on
 * our side of the boundary rather than in the core.
 */
void PostMidi(const uint8_t *data, size_t length)
{
    g_emu.PostMIDI(std::span<const uint8_t>(data, length));
}

void PostReset(Reset type)
{
    switch (type)
    {
        case Reset::GM:
            g_emu.PostSystemReset(EMU_SystemReset::GM_RESET);
            break;
        case Reset::GS:
            g_emu.PostSystemReset(EMU_SystemReset::GS_RESET);
            break;
        case Reset::None:
            break;
    }
}

unsigned long Overruns()
{
    return g_overruns;
}

void PrintModels()
{
    common::PrintRomsets(stdout);
}

} // namespace Core
