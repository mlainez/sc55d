#pragma once

#include <cstddef>
#include <cstdint>

#include "mcu.h"

/*
 * Symbols the Nuked-SC55 core exports but does not declare in a header.  They
 * live in mcu.cpp; sc55d needs them because upstream drives them from its own
 * main()/work thread, which we do not build.
 */
void MCU_Init(void);
void MCU_Reset(void);
void MCU_PatchROM(void);
void MCU_ReadInstruction(void);
void MCU_UpdateAnalog(uint64_t cycles);
void MCU_UpdateUART_RX(void);
void MCU_UpdateUART_TX(void);
int MCU_OpenAudio(int deviceIndex, int pageSize, int pageNum);
void MCU_CloseAudio(void);
void unscramble(uint8_t *src, uint8_t *dst, int len);

extern uint8_t rom1[];
extern uint8_t rom2[];
extern uint8_t tempbuf[];
extern int rom2_mask;
extern const char *roms[ROM_SET_COUNT][6];

namespace Core {

/* Allocates the core's sample ring and resets the machine.  ROMs must already
 * be loaded (see romset.h).  `page_frames` is how many stereo frames one
 * PullPage() call yields; `pages` sizes the ring at 2 * pages * page_frames
 * frames.  Returns false if the ring could not be allocated. */
bool Start(int page_frames, int pages);
void Stop();

/* Native output rate of the loaded romset: 66207 Hz for the SC-55mk2 family,
 * 64000 Hz for the mk1 and JV-880. */
int SampleRate();

int PageFrames();

/* Runs one instruction and the peripherals that hang off it.  This is the body
 * of upstream's work_thread(), minus its SDL ring bookkeeping. */
void Step();

/* Stereo frames rendered but not yet pulled. */
uint64_t FramesReady();

/* Drains exactly PageFrames() frames into `dst` (PageFrames() * 2 int16_t).
 * Step() until FramesReady() >= PageFrames() first. */
void PullPage(int16_t *dst);

/* Feeds raw MIDI bytes to the emulated serial port, the way upstream's RtMidi
 * callback does. */
void PostMidi(const uint8_t *data, size_t length);

enum class Reset {
    None,
    GM,
    GS,
};

void PostReset(Reset type);

} // namespace Core
