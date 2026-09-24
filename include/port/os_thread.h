// Real host semantics for the game's cooperative task scheduler (src/game/scheduler.cpp) on top of
// Dolphin's OSThread API (Phase 4, docs/port-phase3.md's continuation -- see src/port/os_thread.cpp
// for the full design writeup). TARGET_PC-only.
#ifndef TARGET_PC
#error "include/port/os_thread.h is host-only (TARGET_PC); it has no meaning for the original target"
#endif

#ifndef RE4_PORT_OS_THREAD_H
#define RE4_PORT_OS_THREAD_H

namespace re4_port {

// Registers the calling host thread as the game's "main" OSThread identity (the thread
// systemStartInit()/the frame loop itself runs on) -- must be called exactly once, before the
// first TaskScheduler()/TaskSchedulerInit() call, from the same thread that will run main_game().
void MarkCurrentThreadAsMainOSThread();

// The scheduler's driving thread (always the caller of TaskSchedulerMain -- real hardware: the
// main thread) calls this right after every OSResumeThread()/OSWakeupThread() call that hands
// control to a task thread, to block until that task thread hands control back (TaskSleep/
// TaskExit/TaskChain all do, via their own OSResumeThread(pParentThread) call -- scheduler.cpp).
// Not part of the real Dolphin OS API: real hardware achieves the same "only one task's code
// genuinely runs at a time" effect through true preemptive scheduling and thread priorities,
// which this host port cannot reproduce at instruction granularity; this explicit call is the
// TARGET_PC-only mechanism that makes the same intended behavior deterministic on a hosted OS
// instead (see the design note in src/port/os_thread.cpp).
void WaitForHandback();

} // namespace re4_port

#endif // RE4_PORT_OS_THREAD_H
