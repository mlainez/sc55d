/* Realtime setup and signal handling.  Every step here is best effort: without
 * SPDX-License-Identifier: MIT
 *
 * CAP_SYS_NICE or RLIMIT_MEMLOCK we warn and keep going. */
#include "rt.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

std::atomic<bool> g_quit(false);

namespace Rt {
namespace {

extern "C" void OnSignal(int /*signum*/)
{
    g_quit.store(true, std::memory_order_relaxed);
}

} // namespace

void InstallSignalHandlers()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);

    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

void BlockSignals()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void NameThread(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

/*
 * The kernel will not let SCHED_FIFO threads monopolise a CPU: by default it
 * reserves 50 ms of every second for everything else, and a realtime thread
 * that is still runnable when the budget is gone is simply stopped until the
 * next period.
 *
 * That is a sensible protection against a runaway RT thread wedging the
 * machine, and it is poison for this program.  sc55d's render thread saturates
 * its core precisely when the board is struggling -- which is when a 50 ms
 * hole is guaranteed to be an xrun no matter how deep --render-ahead goes.
 * The throttle bites hardest exactly when you can least afford it.
 *
 * Measured here on an otherwise ordinary machine: a CPU-saturating SCHED_FIFO
 * loop containing no emulator at all stalls for up to 54 ms about once a
 * second.  The same loop at normal priority never exceeds 1.6 ms.
 */
void WarnAboutRtThrottle()
{
    FILE *file = fopen("/proc/sys/kernel/sched_rt_runtime_us", "r");
    if (!file)
        return;

    long runtime = -1;
    const int read = fscanf(file, "%ld", &runtime);
    fclose(file);

    if (read != 1 || runtime < 0)
        return; /* -1 means unlimited, which is what we want */

    long period = 1000000;
    file = fopen("/proc/sys/kernel/sched_rt_period_us", "r");
    if (file)
    {
        if (fscanf(file, "%ld", &period) != 1 || period <= 0)
            period = 1000000;
        fclose(file);
    }

    if (runtime >= period)
        return;

    fprintf(stderr,
            "sc55d: warning: the kernel RT throttle will stop the render thread\n"
            "sc55d:   for %.1f ms out of every %.1f ms whenever it saturates its core.\n"
            "sc55d:   That is longer than the audio buffer, so it is an xrun every time.\n"
            "sc55d:   Disable it with:  sudo sysctl -w kernel.sched_rt_runtime_us=-1\n"
            "sc55d:   (or run with --no-realtime, which avoids SCHED_FIFO entirely)\n",
            (double)(period - runtime) / 1000.0, (double)period / 1000.0);
}

void LockMemory()
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "sc55d: warning: mlockall failed (%s)\n", strerror(errno));
}

void RequestFifoPriority(const char *who, int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    const int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (err != 0)
    {
        fprintf(stderr, "sc55d: warning: %s thread: SCHED_FIFO priority %d denied (%s); "
                        "running at normal priority\n",
                who, priority, strerror(err));
        return;
    }
    printf("sc55d: %s thread running SCHED_FIFO at priority %d\n", who, priority);
}

void PinToCpu(const char *who, int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    const int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (err != 0)
    {
        fprintf(stderr, "sc55d: warning: cannot pin %s thread to CPU %d (%s)\n",
                who, cpu, strerror(err));
        return;
    }
    printf("sc55d: %s thread pinned to CPU %d\n", who, cpu);
}

} // namespace Rt
