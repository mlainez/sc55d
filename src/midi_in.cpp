/*
 * ALSA sequencer MIDI input.  Sequencer events are decoded back into the raw
 * MIDI byte stream and handed to the core one byte at a time, exactly as
 * upstream's RtMidi callback does -- the emulated MCU reads them off its
 * serial port.
 */
#include "midi_in.h"

#include <alsa/asoundlib.h>

#include <cstdio>
#include <vector>

#include "core.h"

namespace MidiIn {
namespace {

/* Big enough for any sysex the SC-55 accepts; the core's own UART FIFO is
 * 8 KiB. */
const size_t kDecodeBufferSize = 8192;

snd_seq_t *g_seq = nullptr;
snd_midi_event_t *g_parser = nullptr;
int g_port = -1;

} // namespace

bool Open(const char *client_name)
{
    int err = snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_INPUT, 0);
    if (err < 0)
    {
        fprintf(stderr, "sc55d: cannot open ALSA sequencer: %s\n", snd_strerror(err));
        g_seq = nullptr;
        return false;
    }

    snd_seq_set_client_name(g_seq, client_name);

    g_port = snd_seq_create_simple_port(g_seq, "midi in",
                                        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC
                                            | SND_SEQ_PORT_TYPE_SYNTHESIZER
                                            | SND_SEQ_PORT_TYPE_APPLICATION);
    if (g_port < 0)
    {
        fprintf(stderr, "sc55d: cannot create sequencer port: %s\n", snd_strerror(g_port));
        Close();
        return false;
    }

    err = snd_midi_event_new(kDecodeBufferSize, &g_parser);
    if (err < 0)
    {
        fprintf(stderr, "sc55d: cannot create MIDI event decoder: %s\n", snd_strerror(err));
        Close();
        return false;
    }
    /* Emit a status byte on every message: the emulated UART has no notion of
     * the sequencer's running-status state. */
    snd_midi_event_no_status(g_parser, 1);

    printf("sc55d: MIDI input on %d:%d (\"%s\":\"midi in\")\n",
           snd_seq_client_id(g_seq), g_port, client_name);
    fflush(stdout);
    return true;
}

void Run(const std::atomic<bool> &quit)
{
    if (!g_seq)
        return;

    const int fd_count = snd_seq_poll_descriptors_count(g_seq, POLLIN);
    std::vector<struct pollfd> fds((size_t)fd_count);
    std::vector<uint8_t> bytes(kDecodeBufferSize);

    while (!quit.load(std::memory_order_relaxed))
    {
        snd_seq_poll_descriptors(g_seq, fds.data(), (unsigned)fd_count, POLLIN);
        /* Time out regularly so shutdown does not wait for the next event. */
        if (poll(fds.data(), (nfds_t)fd_count, 100) <= 0)
            continue;

        snd_seq_event_t *event = nullptr;
        while (snd_seq_event_input(g_seq, &event) >= 0)
        {
            long length = snd_midi_event_decode(g_parser, bytes.data(),
                                                (long)bytes.size(), event);
            if (length > 0)
                Core::PostMidi(bytes.data(), (size_t)length);
            else if (length == -ENOMEM)
                fprintf(stderr, "sc55d: dropped an oversized MIDI message\n");

            if (snd_seq_event_input_pending(g_seq, 0) <= 0)
                break;
        }
    }
}

void Close()
{
    if (g_parser)
    {
        snd_midi_event_free(g_parser);
        g_parser = nullptr;
    }
    if (g_seq)
    {
        snd_seq_close(g_seq);
        g_seq = nullptr;
        g_port = -1;
    }
}

} // namespace MidiIn
