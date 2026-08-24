/* ALSA playback.  Deliberately plain: one blocking writei per period, which is
 * what paces the render loop. */
#include "audio_out.h"

#include <alsa/asoundlib.h>

#include <cstdio>

namespace AudioOut {
namespace {

snd_pcm_t *g_pcm = nullptr;
unsigned long g_xruns = 0;

bool Recover(int err)
{
    if (err == -EPIPE)
    {
        g_xruns++;
        fprintf(stderr, "sc55d: xrun (%lu total)\n", g_xruns);
        err = snd_pcm_prepare(g_pcm);
        if (err < 0)
        {
            fprintf(stderr, "sc55d: cannot recover from xrun: %s\n", snd_strerror(err));
            return false;
        }
        return true;
    }

    if (err == -ESTRPIPE)
    {
        while ((err = snd_pcm_resume(g_pcm)) == -EAGAIN)
            snd_pcm_wait(g_pcm, 100);
        if (err < 0)
            err = snd_pcm_prepare(g_pcm);
        if (err < 0)
        {
            fprintf(stderr, "sc55d: cannot resume stream: %s\n", snd_strerror(err));
            return false;
        }
        return true;
    }

    fprintf(stderr, "sc55d: write failed: %s\n", snd_strerror(err));
    return false;
}

} // namespace

bool Open(const char *device, unsigned rate, unsigned period_frames, unsigned periods)
{
    int err = snd_pcm_open(&g_pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        fprintf(stderr, "sc55d: cannot open audio device %s: %s\n", device, snd_strerror(err));
        g_pcm = nullptr;
        return false;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);

    unsigned actual_rate = rate;
    snd_pcm_uframes_t period = period_frames;
    snd_pcm_uframes_t buffer = (snd_pcm_uframes_t)period_frames * periods;

    err = snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0)
        err = snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (err >= 0)
        err = snd_pcm_hw_params_set_channels(g_pcm, hw, 2);
    /* The core runs at ~66207 Hz, which no sound card supports.  Ask the plug
     * layer for it anyway and let it resample. */
    if (err >= 0)
        err = snd_pcm_hw_params_set_rate_resample(g_pcm, hw, 1);
    if (err >= 0)
        err = snd_pcm_hw_params_set_rate_near(g_pcm, hw, &actual_rate, nullptr);
    if (err >= 0)
        err = snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, nullptr);
    if (err >= 0)
        err = snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &buffer);
    if (err >= 0)
        err = snd_pcm_hw_params(g_pcm, hw);
    if (err < 0)
    {
        fprintf(stderr, "sc55d: cannot configure audio device: %s\n", snd_strerror(err));
        Close();
        return false;
    }

    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(g_pcm, sw);
    snd_pcm_sw_params_set_start_threshold(g_pcm, sw, buffer);
    snd_pcm_sw_params_set_avail_min(g_pcm, sw, period);
    err = snd_pcm_sw_params(g_pcm, sw);
    if (err < 0)
    {
        fprintf(stderr, "sc55d: cannot set software parameters: %s\n", snd_strerror(err));
        Close();
        return false;
    }

    printf("sc55d: audio %s: %u Hz requested, %u Hz on the device, "
           "period %lu frames, buffer %lu frames (%.1f ms)\n",
           device, rate, actual_rate, (unsigned long)period, (unsigned long)buffer,
           1000.0 * (double)buffer / (double)actual_rate);
    fflush(stdout);
    return true;
}

bool Write(const int16_t *frames, unsigned count)
{
    while (count > 0)
    {
        snd_pcm_sframes_t written = snd_pcm_writei(g_pcm, frames, count);
        if (written < 0)
        {
            if (written == -EAGAIN)
                continue;
            /* A signal cut the write short: not an error, just a chance for
             * the caller to look at the quit flag. */
            if (written == -EINTR)
                return true;
            if (!Recover((int)written))
                return false;
            /* The period we were writing is gone with the xrun; drop it rather
             * than stalling the render loop replaying stale audio. */
            return true;
        }
        frames += (size_t)written * 2;
        count -= (unsigned)written;
    }
    return true;
}

unsigned long Xruns()
{
    return g_xruns;
}

void Close()
{
    if (!g_pcm)
        return;
    snd_pcm_drain(g_pcm);
    snd_pcm_close(g_pcm);
    g_pcm = nullptr;
}

} // namespace AudioOut
