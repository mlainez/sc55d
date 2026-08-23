/* Implementation of the SDL2 subset declared in sdl_compat.h. */
#include "sdl_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct SDL_mutex {
    pthread_mutex_t handle;
};

struct SDL_Thread {
    pthread_t handle;
    SDL_ThreadFunction fn;
    void *data;
    int result;
};

namespace {

const SDL_AudioSpec *g_spec_valid = nullptr;
SDL_AudioSpec g_spec;

void *ThreadTrampoline(void *arg)
{
    SDL_Thread *thread = static_cast<SDL_Thread *>(arg);
    thread->result = thread->fn(thread->data);
    return nullptr;
}

} // namespace

int SDL_Init(Uint32 /*flags*/)
{
    return 0;
}

void SDL_Quit(void)
{
}

const char *SDL_GetError(void)
{
    return "";
}

void SDL_Delay(Uint32 ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *mutex = new SDL_mutex;
    pthread_mutex_init(&mutex->handle, nullptr);
    return mutex;
}

int SDL_LockMutex(SDL_mutex *mutex)
{
    return mutex ? pthread_mutex_lock(&mutex->handle) : -1;
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
    return mutex ? pthread_mutex_unlock(&mutex->handle) : -1;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
    if (!mutex)
        return;
    pthread_mutex_destroy(&mutex->handle);
    delete mutex;
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char * /*name*/, void *data)
{
    SDL_Thread *thread = new SDL_Thread;
    thread->fn = fn;
    thread->data = data;
    thread->result = 0;
    if (pthread_create(&thread->handle, nullptr, ThreadTrampoline, thread) != 0)
    {
        delete thread;
        return nullptr;
    }
    return thread;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (!thread)
        return;
    pthread_join(thread->handle, nullptr);
    if (status)
        *status = thread->result;
    delete thread;
}

int SDL_GetNumAudioDevices(int /*iscapture*/)
{
    return 1;
}

const char *SDL_GetAudioDeviceName(int /*index*/, int /*iscapture*/)
{
    return "sc55d internal ring";
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char * /*device*/, int /*iscapture*/,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int /*allowed_changes*/)
{
    if (!desired || !desired->callback)
        return 0;

    g_spec = *desired;
    g_spec_valid = &g_spec;

    if (obtained)
        *obtained = g_spec;

    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID /*dev*/, int /*pause_on*/)
{
}

void SDL_CloseAudio(void)
{
    g_spec_valid = nullptr;
}

namespace SdlCompat {

const SDL_AudioSpec *OpenedSpec()
{
    return g_spec_valid;
}

void PullFrames(int16_t *dst, int frames)
{
    if (!g_spec_valid)
    {
        memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    g_spec.callback(g_spec.userdata, reinterpret_cast<Uint8 *>(dst),
                    frames * 2 * (int)sizeof(int16_t));
}

} // namespace SdlCompat
