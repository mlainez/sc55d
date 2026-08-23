#pragma once

namespace Bench {

/* Renders `seconds` of audio from a dense generated MIDI sequence as fast as
 * the machine allows and prints the realtime ratio: rendered seconds per
 * wall-clock second.  Anything above 1.0 means the core keeps up.  Returns the
 * process exit status. */
int Run(double seconds, double warmup_seconds);

} // namespace Bench
