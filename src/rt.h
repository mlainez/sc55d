#pragma once

#include <atomic>

/* Set by SIGINT/SIGTERM once InstallSignalHandlers() has run. */
extern std::atomic<bool> g_quit;

namespace Rt {

void InstallSignalHandlers();

/* All three warn and carry on when the kernel says no -- sc55d still works
 * without privileges, it just gets less predictable. */
void LockMemory();
void RequestFifoPriority(int priority);
void PinToCpu(int cpu);

} // namespace Rt
