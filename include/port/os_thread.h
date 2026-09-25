// Real host semantics for the game's cooperative task scheduler (src/game/scheduler.cpp) on top of
// Dolphin's OSThread API. TARGET_PC-only. See src/port/os_thread.cpp for the full design writeup
// (Phase 4 continuation, docs/port-boot.md section 34: replaced a real-std::thread-plus-token
// emulation, which had two confirmed races, with fibers -- src/port/fiber.h -- so only one
// call stack of native instructions ever actually runs, matching the real single-core target by
// construction instead of by hand-synchronized threads).
#ifndef TARGET_PC
#error "include/port/os_thread.h is host-only (TARGET_PC); it has no meaning for the original target"
#endif

#ifndef RE4_PORT_OS_THREAD_H
#define RE4_PORT_OS_THREAD_H

namespace re4_port {

// Registers the calling host thread/fiber as the game's "main" OSThread identity (the thread
// systemStartInit()/the frame loop itself runs on) -- must be called exactly once, before the
// first TaskScheduler()/TaskSchedulerInit() call, from the same (real host) thread that will run
// main_game(). Every OSThread the game creates afterward (src/game/scheduler.cpp's TASK slots)
// becomes a fiber running on this same real host thread; this call's only job is to wrap that
// thread's own already-running execution context as the first such fiber (src/port/fiber.h's
// FiberFromCurrentContext()).
void MarkCurrentThreadAsMainOSThread();

} // namespace re4_port

#endif // RE4_PORT_OS_THREAD_H
