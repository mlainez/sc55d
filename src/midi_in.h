#pragma once

#include <atomic>

namespace MidiIn {

/* Creates an ALSA sequencer client with one writable port, so MIDI can be
 * routed in with aconnect. */
bool Open(const char *client_name);

/* Blocks decoding sequencer events into raw MIDI bytes and feeding them to the
 * core, until `quit` goes true.  Meant to run on its own thread. */
void Run(const std::atomic<bool> &quit);

void Close();

} // namespace MidiIn
