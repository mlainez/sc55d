#pragma once

#include <cstdint>

namespace AudioOut {

/* Opens `device` (a plug-layer name such as "default") for stereo 16-bit
 * playback at `rate`, the core's native rate.  Resampling is left enabled, so
 * the plug layer converts to whatever the hardware actually supports. */
bool Open(const char *device, unsigned rate, unsigned period_frames, unsigned periods);

/* Blocking write of `frames` stereo frames.  Recovers from xruns and counts
 * them.  Returns false only on an error we cannot recover from. */
bool Write(const int16_t *frames, unsigned count);

unsigned long Xruns();

void Close();

} // namespace AudioOut
