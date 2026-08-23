#include "core.h"

#include <cstdio>

#include "mcu_interrupt.h"
#include "mcu_timer.h"
#include "pcm.h"
#include "sdl_compat.h"
#include "stubs.h"
#include "submcu.h"

namespace Core {
namespace {

int g_page_frames = 0;
uint64_t g_produced = 0;
uint64_t g_consumed = 0;

} // namespace

bool Start(int page_frames, int pages)
{
    g_page_frames = page_frames;
    g_produced = 0;
    g_consumed = 0;

    /* MCU_OpenAudio() takes a page size in int16_t samples: one page is one
     * callback's worth, i.e. page_frames stereo frames.  It picks the native
     * sample rate for the loaded romset and hands the callback to our SDL
     * shim, which is where PullPage() gets it from. */
    if (!MCU_OpenAudio(0, page_frames * 4, pages))
        return false;

    MCU_Init();
    MCU_PatchROM();
    MCU_Reset();
    SM_Reset();
    PCM_Reset();
    return true;
}

void Stop()
{
    MCU_CloseAudio();
}

int SampleRate()
{
    const SDL_AudioSpec *spec = SdlCompat::OpenedSpec();
    return spec ? spec->freq : 0;
}

int PageFrames()
{
    return g_page_frames;
}

void Step()
{
    if (!mcu.ex_ignore)
        MCU_Interrupt_Handle();
    else
        mcu.ex_ignore = 0;

    if (!mcu.sleep)
        MCU_ReadInstruction();

    mcu.cycles += 12; // FIXME: assume 12 cycles per instruction (as upstream)

    /* PCM_Update() posts one frame per tick, or two when the oversampling bit
     * is set.  A tick spans at least 50 MCU cycles and we call this after every
     * instruction, so at most one tick can fall out of a single call -- which
     * is how we count frames without reaching into mcu.cpp's private ring. */
    const uint64_t pcm_cycles = pcm.cycles;
    const int frames_per_tick = (pcm.config_reg_3c & 0x40) ? 2 : 1;
    PCM_Update(mcu.cycles);
    if (pcm.cycles != pcm_cycles)
        g_produced += frames_per_tick;

    TIMER_Clock(mcu.cycles);

    if (!mcu_mk1 && !mcu_jv880 && !mcu_scb55)
    {
        SM_Update(mcu.cycles);
    }
    else
    {
        MCU_UpdateUART_RX();
        MCU_UpdateUART_TX();
    }

    MCU_UpdateAnalog(mcu.cycles);

    if (mcu_mk1)
        LcdStub_Tick();
}

uint64_t FramesReady()
{
    return g_produced - g_consumed;
}

void PullPage(int16_t *dst)
{
    SdlCompat::PullFrames(dst, g_page_frames);
    g_consumed += (uint64_t)g_page_frames;
}

/* Called from the MIDI thread while the render thread reads the same FIFO.
 * The core's uart_buffer is a plain single-producer ring with unsynchronised
 * indices; upstream's RtMidi callback posts into it the same way. */
void PostMidi(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++)
        MCU_PostUART(data[i]);
}

void PostReset(Reset type)
{
    static const uint8_t gm_reset[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
    static const uint8_t gs_reset[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                        0x00, 0x7f, 0x00, 0x41, 0xf7 };

    if (type == Reset::GM)
        PostMidi(gm_reset, sizeof(gm_reset));
    else if (type == Reset::GS)
        PostMidi(gs_reset, sizeof(gs_reset));
}

} // namespace Core
