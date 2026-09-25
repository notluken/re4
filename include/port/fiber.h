// A minimal user-mode context-switching primitive (Phase 4 continuation, docs/port-boot.md section
// 34): the building block src/port/os_thread.cpp's OSThread scheduler is built on, replacing the
// previous real-std::thread-plus-"turn token" emulation (docs/port-boot.md section 33 found a
// second, unresolved missed-token race in that design -- two real host threads briefly both
// touching scheduler.cpp's shared `pCTask`). A fiber is a saved CPU context (registers + its own
// separate stack) that only ever runs when this file's own FiberSwitchTo() jumps to it, on the
// SAME real host thread as every other fiber -- so "exactly one fiber's code is ever executing"
// holds by construction, with no lock needed anywhere in this file or in os_thread.cpp's scheduler
// built on it (real concurrency never happens; there is only ever one call stack of native
// instructions actually running).
//
// Mechanism chosen: POSIX ucontext (getcontext/makecontext/swapcontext), not a hand-written arm64
// context switch. Verified working on this host (Darwin 25.6, arm64, Xcode command-line-tools
// clang) with a standalone test: 6 swaps between a "main" context and a fiber touching float
// locals survived with values intact, and tests/port/test_fiber.cpp's own 10000-swap stress test
// (below) additionally exercises NEON (float32x4_t) values live across a swap. The libc headers
// mark getcontext/makecontext/swapcontext "deprecated ... No longer supported" on this SDK, but
// they are still present, still exported, and still behave correctly for this exact
// save-full-context/restore-full-context use (not signal handling, which is what the deprecation
// note is actually about -- sigaltstack-based signal delivery details, irrelevant here). A
// hand-written arm64 context switch (save/restore x19-x28, sp, lr, fp, plus the callee-saved NEON
// d8-d15 halves) would be a few dozen lines of inline asm and marginally faster, but ucontext is
// the simpler, already-correct, already-tested choice for a single real host thread that is not a
// hot loop (a handful of switches per frame) -- revisit only if profiling ever shows this matters.
#ifndef TARGET_PC
#error "include/port/fiber.h is host-only (TARGET_PC); it has no meaning for the original target"
#endif

#ifndef RE4_PORT_FIBER_H
#define RE4_PORT_FIBER_H

#include <cstddef>

namespace re4_port {

struct Fiber;

using FiberEntry = void (*)(void* arg);

// Wraps the CALLING execution context (whatever real host thread/stack this runs on) as a Fiber,
// so it can be FiberSwitchTo()'d away from and back to like any other. Does not allocate a stack
// (the caller's own stack, whatever it is, is used as-is). One of these must exist before the
// first FiberSwitchTo() on a given real host thread; typically the game host thread's own entry
// point calls this once, immediately, and never anything else touches the returned Fiber's
// identity directly (it is only ever a FiberSwitchTo() target/source).
Fiber* FiberFromCurrentContext();

// Creates a fiber whose real (machine) stack is [stack_base, stack_base + stack_size) --
// stack_base is the LOWEST address (this file's own convention, matching
// include/port/arena.h's CreateThreadOnStack). `entry` does not run until the first
// FiberSwitchTo() targeting the returned Fiber*; `arg` is passed to it verbatim.
//
// `on_finish`, if non-null, is called (as `on_finish(f, arg)`) if and when `entry` itself returns
// (normal C++ return, not a switch away) -- expected never to return in practice (see
// FiberEntry's own doc), but if it does, `on_finish` must not return either (this file aborts,
// loudly, if `entry` returns with `on_finish` null, or if `on_finish` itself returns -- both are
// programming errors in the caller, not a survivable condition: there is no valid CPU context left
// to resume).
Fiber* FiberCreate(void* stack_base, std::size_t stack_size, FiberEntry entry, void* arg,
                    void (*on_finish)(Fiber* self, void* arg) = nullptr);

// Switches execution from `from` (must be the fiber the CALLING code is currently running as) to
// `to`. Returns once some later FiberSwitchTo(..., from) switches back to `from`. The only
// operation in this file that actually transfers control; every other function here just does
// bookkeeping.
void FiberSwitchTo(Fiber* from, Fiber* to);

// Whichever Fiber* the calling code is currently running as (the most recent FiberSwitchTo()
// target on this real host thread, or the FiberFromCurrentContext() result if none yet).
Fiber* FiberCurrent();

// Frees this file's own bookkeeping for `f` (not the stack memory `FiberCreate` was given, which
// its caller still owns) -- call only once `f` is provably never going to be switched to again
// (e.g. its OSThread has gone MORIBUND).
void FiberDestroy(Fiber* f);

} // namespace re4_port

#endif // RE4_PORT_FIBER_H
