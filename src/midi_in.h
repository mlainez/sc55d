#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MidiIn {

/* Creates an ALSA sequencer client with one writable port, so MIDI can be
 * routed in with aconnect. */
bool Open(const char *client_name);

/* Blocks decoding sequencer events into raw MIDI bytes and feeding them to the
 * core, until `quit` goes true.  Meant to run on its own thread. */
void Run(const std::atomic<bool> &quit);

/* Hands everything Run() has decoded to the core.  Must be called from the
 * render thread and nowhere else: it is the *only* thread allowed to touch
 * the core, which is the point of the queue behind it.  Returns the number of
 * bytes handed over. */
size_t DrainToCore();

/* MIDI bytes dropped because the queue filled up.  Should stay zero. */
unsigned long Dropped();

/* Puts bytes into the same queue the sequencer feeds, from a thread that is
 * not the render thread -- which is exactly what --selftest needs, and exactly
 * what makes it a real test of this path rather than a shortcut past it.
 *
 * The queue takes a single producer, so this and Run() must not be used at the
 * same time; --selftest keeps the sequencer shut for the whole run. */
void Inject(const uint8_t *data, size_t length);

void Close();

} // namespace MidiIn
