// Host crash reporting.
//
// Guest faults are already caught and symbolised, but a fault in host code -
// a thunk handed a bad length, or a driver walking off the end of a buffer -
// just kills the process, and the log ends mid-line with no clue where. This
// turns that into a report: the faulting host function, the host stack, and
// what the guest was doing at the time.
//
// Written against the Windows unhandled-exception filter today; the Switch
// build gets the same report from libnx's fatal handler, which is why the
// formatting lives here rather than in main.
#pragma once

namespace wb {

class Env;

// Installs the handler. Safe to call once, before anything runs.
void crash_install(Env* env);

// Called at the top of every guest thread. The stack reservation the handler
// needs in order to report a stack overflow is per-thread, and the guest
// threads are the ones that overflow.
void crash_arm_thread();

// Suspends every other thread in turn and prints where it is. For hangs,
// which no fault handler ever sees: nothing crashes, so nothing reports, and
// the guest is not executing either so the sampling profiler is silent too.
void crash_dump_all_threads(const char* why);

}  // namespace wb
