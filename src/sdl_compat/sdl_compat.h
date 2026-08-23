/*
 * Minimal stand-in for the small slice of SDL2 that the Nuked-SC55 core
 * translation units reference.  The core is compiled straight out of the
 * submodule, which we never patch, so the SDL symbols it names have to exist
 * somewhere: mcu.h needs SDL_atomic_t, submcu.cpp includes SDL_audio.h, and
 * mcu.cpp's own (unused) SDL frontend main() names the audio, thread and mutex
 * calls.
 *
 * Threads, mutexes and atomics are implemented for real on top of pthreads.
 * The audio device is virtual: MCU_OpenAudio() still allocates the core's
 * sample ring and hands us its callback, which sc55d pumps by hand from its
 * render loop.  See SdlCompat below.
 */
#pragma once

#include <stdint.h>

typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef uint32_t Uint32;

#define SDLCALL

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u

/* Values match SDL2 so the core's own log lines stay meaningful. */
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_U16MSB 0x1010
#define AUDIO_S16MSB 0x9010
#define AUDIO_S32LSB 0x8020
#define AUDIO_S32MSB 0x9020
#define AUDIO_F32LSB 0x8120
#define AUDIO_F32MSB 0x9120
#define AUDIO_S16SYS AUDIO_S16LSB /* sc55d only targets little-endian hosts */

/* --- atomics ------------------------------------------------------------ */

typedef struct SDL_atomic_t {
    int value;
} SDL_atomic_t;

inline int SDL_AtomicGet(SDL_atomic_t *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

inline int SDL_AtomicSet(SDL_atomic_t *a, int v)
{
    return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

/* --- init, errors, timing ----------------------------------------------- */

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_Delay(Uint32 ms);

/* --- mutexes and threads ------------------------------------------------ */

struct SDL_mutex;
struct SDL_Thread;

typedef int (SDLCALL *SDL_ThreadFunction)(void *data);

SDL_mutex *SDL_CreateMutex(void);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);
void SDL_DestroyMutex(SDL_mutex *mutex);

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void SDL_WaitThread(SDL_Thread *thread, int *status);

/* --- audio -------------------------------------------------------------- */

typedef Uint32 SDL_AudioDeviceID;
typedef void (SDLCALL *SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
};

int SDL_GetNumAudioDevices(int iscapture);
const char *SDL_GetAudioDeviceName(int index, int iscapture);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudio(void);

/* --- sc55d side of the virtual audio device ------------------------------ */

namespace SdlCompat {

/* Spec the core asked for in MCU_OpenAudio(), or null if it has not run yet. */
const SDL_AudioSpec *OpenedSpec();

/* Run the core's audio callback, draining `frames` stereo frames from its
 * sample ring into `dst`.  The core's callback does not handle ring wrap, so
 * `frames` must always be the page size it asked for (spec.samples). */
void PullFrames(int16_t *dst, int frames);

} // namespace SdlCompat
