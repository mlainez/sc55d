// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>

/* Set by SIGINT/SIGTERM once InstallSignalHandlers() has run. */
extern std::atomic<bool> g_quit;

namespace Rt {

void InstallSignalHandlers();

/* Keeps SIGINT/SIGTERM off the calling thread so they always land on the one
 * that can act on them.  Called first thing in every thread we spawn. */
void BlockSignals();

/* Shows up in ps, top and the ftrace logs -- worth having when you are trying
 * to work out which of three threads missed its deadline. */
void NameThread(const char *name);

/* All three warn and carry on when the kernel says no -- sc55d still works
 * without privileges, it just gets less predictable.  `who` names the calling
 * thread in the messages. */
/* Checks whether the kernel's realtime throttle is on.  It stops a saturating
 * SCHED_FIFO thread for a slice of every second, which is longer than any
 * sensible audio buffer -- so it turns "slightly too slow" into "xruns every
 * second".  Warns and explains rather than failing. */
void WarnAboutRtThrottle();

void LockMemory();
void RequestFifoPriority(const char *who, int priority);
void PinToCpu(const char *who, int cpu);

} // namespace Rt
