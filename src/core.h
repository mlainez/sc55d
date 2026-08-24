// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Core {

/* Routes the core's own log messages through sc55d, capped at `limit` messages
 * so a misbehaving ROM cannot flood the journal (0 = no cap).  `quiet` silences
 * them outright.  Call before Load(). */
void SetDiagnostics(unsigned long limit, bool quiet);

/* Core messages dropped by that cap. */
unsigned long SuppressedMessages();

enum class Reset {
    None,
    GM,
    GS,
};

/* Finds and loads a ROM set from `rom_dir`.  `model` is a romset name
 * ("mk2", "st", "mk1", ...); empty means autodetect.  `verify` picks the
 * content-hashing loader, which identifies ROMs by SHA-256 regardless of file
 * name; without it the loader falls back to upstream's file-name convention.
 * Prints the core's own diagnostics and returns false on failure. */
bool Load(const std::string &rom_dir, const std::string &model, bool verify);

/* Brings up the machine.  `page_frames` is the chunk the render loop works in.
 * No LCD backend is installed, so the core skips LCD emulation entirely. */
bool Start(int page_frames);

const char *ModelName();

/* Native output rate of the loaded romset. */
int SampleRate();

int PageFrames();

/* Runs one instruction and the peripherals hanging off it.  Samples land in
 * the output buffer through the core's sample callback. */
void Step();

/* Stereo frames rendered but not yet consumed. */
size_t FramesReady();

/* Rendered frames, interleaved and contiguous.  Valid up to FramesReady(). */
const int16_t *Frames();

/* Drops `frames` frames off the front, after they have been written out. */
void Consume(int frames);

void PostMidi(const uint8_t *data, size_t length);
void PostReset(Reset type);

/* Frames dropped because the render loop did not keep up with the callback.
 * Should stay zero; non-zero means a bug in the loop, not an xrun. */
unsigned long Overruns();

void PrintModels();

} // namespace Core
