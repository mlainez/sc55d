#pragma once

#include <cstdint>
#include <vector>

namespace Bench {

/* One MIDI message, timed in frames from the start of the sequence. */
struct Event {
    uint64_t frame;
    uint8_t data[3];
    uint8_t length;
};

/* The dense sixteen-part stress sequence the benchmark renders: a note grid
 * hard enough to hold the SC-55's voice pool saturated, plus continuous
 * controller and pitch bend traffic.  Generated rather than loaded from a file
 * so it is reproducible and sc55d stays self-contained.  --selftest plays the
 * same thing through the daemon's real MIDI path. */
std::vector<Event> Sequence(double seconds, int rate);

struct Options {
    double seconds = 30.0;
    double warmup_seconds = 4.0;

    /* Time every period and print the distribution of render times against
     * the period's realtime budget.  This is the measurement that says how
     * deep --render-ahead has to be. */
    bool histogram = false;

    /* When non-zero, every rendered period is handed to a consumer thread
     * through a PeriodRing of this many slots, which then throws it away.
     * The point is not the audio -- it is to put the memcpy, the mutex and
     * the condvar wake-up of the pipelined path into the measured loop, so
     * the hand-off can be priced against the same run without it. */
    int ring_slots = 0;
};

/* Renders `seconds` of audio from a dense generated MIDI sequence as fast as
 * the machine allows and prints the realtime ratio: rendered seconds per
 * wall-clock second.  Anything above 1.0 means the core keeps up.  Returns the
 * process exit status. */
int Run(const Options &options);

} // namespace Bench
