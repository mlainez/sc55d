/* Realtime setup and signal handling.  Every step here is best effort: without
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

void LockMemory()
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "sc55d: warning: mlockall failed (%s)\n", strerror(errno));
}

void RequestFifoPriority(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    const int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (err != 0)
    {
        fprintf(stderr, "sc55d: warning: SCHED_FIFO priority %d denied (%s); "
                        "running at normal priority\n",
                priority, strerror(err));
        return;
    }
    printf("sc55d: render thread running SCHED_FIFO at priority %d\n", priority);
}

void PinToCpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    const int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (err != 0)
    {
        fprintf(stderr, "sc55d: warning: cannot pin to CPU %d (%s)\n", cpu, strerror(err));
        return;
    }
    printf("sc55d: render thread pinned to CPU %d\n", cpu);
}

} // namespace Rt
