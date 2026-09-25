// Real host semantics for src/game/scheduler.cpp's cooperative task scheduler, on top of
// include/port/fiber.h.
//
// History (docs/port-boot.md sections 24-33): the first working version modeled each OSThread as a
// real host std::thread plus a single "whose turn is it" token (`g_holder`), hand-passed between
// threads with a condvar. That design had two confirmed real races (section 33): a missed-wakeup
// (fixed there with a per-queue "pending wake" mark) and a second, unresolved one where two real
// host threads were briefly both touching scheduler.cpp's shared `pCTask` global -- the token
// model made "only one task's code genuinely runs at a time" an invariant this file had to
// maintain by hand, and it had at least one more gap in it than found.
//
// This version instead makes that invariant true by construction: every OSThread (the game's own
// per-task identity, `TASK::Thread`, plus the "main"/driving identity `MarkCurrentThreadAsMainOSThread()`
// wraps) is backed by a `re4_port::Fiber` (include/port/fiber.h), and every one of those fibers runs
// on the SAME real host thread -- the one that calls `MarkCurrentThreadAsMainOSThread()` (in
// practice, src/port/boot_main.cpp's game thread). A fiber switch is not a thread switch: nothing
// else can be executing while one fiber's code runs, so there is no data race left to have on any
// of this file's own bookkeeping (no mutex anywhere below -- not an oversight, the whole point).
//
// The scheduling algorithm itself (run queues by priority, `SelectThread`'s "only preempt on a
// strictly-better-priority thread becoming ready" rule, `OSSleepThread`/`OSWakeupThread`'s plain
// FIFO sleep queues) is modeled directly on the real, already-decompiled Nintendo SDK source in
// this tree (src/lib/OSThread.c) -- read that file's `SetRun`/`UnsetRun`/`SelectThread`/
// `OSResumeThread`/`OSSuspendThread`/`OSSleepThread`/`OSWakeupThread` before changing any of the
// functions below; each one below is commented with which real function it mirrors and what is
// simplified out (mainly: no OSDisableInterrupts()-style critical section anywhere, since nothing
// here can ever be interrupted mid-function by another fiber -- there is no other execution context
// to interrupt it with; and no mutex priority-inheritance/priority-promotion machinery, since
// OSMutex is not implemented by this file at all yet -- see the header comment on that below).
//
// Real semantics gap accepted, found live this pass, and fixed at its OWN true location (not by
// adding a lock here): src/game/main.cpp's real, byte-matching `postVSyncCallback()` calls
// `iTaskSuspend()` -> `OSSuspendThread()`, and is registered as a VI retrace callback
// (`VISetPostRetraceCallback`) -- include/port/vi.h's own design runs that callback on the host
// **process's real main thread** (Aurora's present loop), a genuinely different real OS thread than
// the one every fiber in this file lives on. Calling into this file's unsynchronized run-queue
// bookkeeping from that other thread while a fiber here is mid-reschedule would be exactly the kind
// of two-real-threads-touching-shared-state bug this whole rewrite exists to eliminate -- so
// src/port/vi.cpp's RunPresentLoop() no longer calls a registered VI callback directly; it queues
// it (a real, small, mutex-protected queue -- the only lock in this whole subsystem, and it never
// touches anything in this file) and VIWaitForRetrace() (called every frame BY the game thread,
// src/game/main.cpp) drains and runs it there instead, right after the same real vsync tick it was
// registered against. See src/port/vi.cpp's own comment for the detail.
//
// Not implemented this pass (documented, not silently dropped -- OSMutex/OSMessageQueue): no game
// code calls OSInitMutex/OSLockMutex/OSUnlockMutex/OSInitMessageQueue/OSSendMessage/
// OSReceiveMessage/OSJamMessage today (grepped the whole tree; only src/lib/OSMutex.c, the real SDK
// source, and this file's own header comments mention them) -- they stay on the generic
// logging-only stub path (src/port/stubs/generated_c_stubs.cpp) rather than getting an unexercised,
// unverified fiber-based implementation now. If a future unit needs one, model it on src/lib/
// OSMutex.c the same deliberate way this file's semaphore functions are modeled on src/lib/
// OSSemaphore.c (both real, already-decompiled SDK sources in this tree).
#ifdef TARGET_PC

#include "port/arena.h"
#include "port/fiber.h"
#include "port/os_thread.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "types.h"
#include <dolphin/os/OSSemaphore.h>
#include <dolphin/os/OSThread.h>

namespace re4_port {
namespace {

// One FIFO per priority level (0 = highest .. 31 = lowest), exactly src/lib/OSThread.c's own
// `RunQueue[32]` -- built directly out of OSThread's own real `link`/`queue` fields (not a side
// table), the same fields the real SDK uses, so nothing here disagrees with what scheduler.cpp
// itself might ever read back from those fields (it doesn't, today, but OSCheckActiveThreads-shaped
// future debugging would want them real).
OSThreadQueue g_runQueue[32];

void EnqueueThread(OSThread* t, OSThreadQueue* q)
{
    OSThread* prev = q->tail;
    if (prev == nullptr) {
        q->head = t;
    } else {
        prev->link.next = t;
    }
    t->link.prev = prev;
    t->link.next = nullptr;
    q->tail = t;
    t->queue = q;
}

void DequeueThread(OSThread* t, OSThreadQueue* q)
{
    OSThread* next = t->link.next;
    OSThread* prev = t->link.prev;
    if (next == nullptr) {
        q->tail = prev;
    } else {
        next->link.prev = prev;
    }
    if (prev == nullptr) {
        q->head = next;
    } else {
        prev->link.next = next;
    }
    t->queue = nullptr;
}

void SetRun(OSThread* t)
{
    EnqueueThread(t, &g_runQueue[t->priority]);
}

void UnsetRun(OSThread* t)
{
    DequeueThread(t, &g_runQueue[t->priority]);
}

// The best (numerically lowest = highest real priority) non-empty run queue's priority, or -1 if
// every queue is empty -- src/lib/OSThread.c's `__cntlzw(RunQueueBits)`, done here by a plain scan
// (32 entries, called a handful of times per frame -- not worth a bitmask for this host).
int BestReadyPriority()
{
    for (int p = 0; p < 32; ++p) {
        if (g_runQueue[p].head != nullptr) {
            return p;
        }
    }
    return -1;
}

OSThread g_mainThread{}; // MarkCurrentThreadAsMainOSThread()'s identity -- see include/port/os_thread.h

struct ThreadRec {
    Fiber* fiber = nullptr;
    void* (*func)(void*) = nullptr;
    void* param = nullptr;
};
std::unordered_map<OSThread*, ThreadRec> g_recs;

OSThread* g_current = nullptr; // whichever OSThread's fiber is presently OS_THREAD_STATE_RUNNING --
                                // the ONE fiber actually executing right now, always exactly one
                                // once MarkCurrentThreadAsMainOSThread() has run.

std::unordered_map<OSThread*, void*> g_taskStackSlots; // OSThread* -> its assigned pool slot base
                                                        // (include/port/arena.h's
                                                        // GetTaskStackPoolBase()), same pooling
                                                        // scheme the old thread-based version used
                                                        // (docs/port-boot.md section 29) -- a fiber
                                                        // still needs its own real machine stack,
                                                        // this just no longer needs to be
                                                        // page-aligned (ucontext, unlike
                                                        // pthread_attr_setstack, has no such
                                                        // requirement), kept anyway for simplicity
                                                        // and because every game-visible pointer
                                                        // must still live in the arena window.
std::size_t g_nextTaskStackSlot = 0;

void* AssignTaskStackSlot(OSThread* thread)
{
    auto it = g_taskStackSlots.find(thread);
    if (it != g_taskStackSlots.end()) {
        return it->second;
    }
    if (g_nextTaskStackSlot >= re4_port::kTaskStackSlots) {
        std::fprintf(stderr,
                     "OSCreateThread: task-stack pool exhausted (%zu slots, include/port/arena.h's "
                     "kTaskStackSlots) -- more distinct OSThread* values than TASK_NUM were seen; "
                     "aborting rather than falling back to an unsafe host stack (docs/port-boot.md "
                     "section 29)\n",
                     re4_port::kTaskStackSlots);
        std::abort();
    }
    void* slot = static_cast<char*>(re4_port::GetTaskStackPoolBase()) +
                 g_nextTaskStackSlot * re4_port::kTaskStackSlotSize;
    ++g_nextTaskStackSlot;
    g_taskStackSlots[thread] = slot;
    return slot;
}

struct ThreadExitException {}; // OSExitThread's unwind mechanism -- see the file header design note
                                // ("permanently switch away", below) for why this stays a C++
                                // exception rather than the fiber trampoline just returning.

// The single scheduling decision point every state-changing OSThread call below ends with --
// mirrors src/lib/OSThread.c's `SelectThread()` (its non-idle-loop half; there is no idle thread in
// this port, see the deadlock abort below) with one deliberate behavioral difference documented at
// each call site: this function's caller has already decided WHAT changed (a thread became ready,
// or the currently running one blocked/yielded) and already updated that thread's own `state`/
// queue membership; this function only ever decides WHETHER to switch fibers and to WHICH thread.
//
//   - `selfBlocked == true`: `self` (must be `g_current`) has already been fully removed from
//     runnability by the caller (OSSleepThread/OSExitThread's trampoline/OSSuspendThread(self)) --
//     find the next-best ready thread and switch to it unconditionally (aborting if none exists:
//     a real deadlock, never valid for this game's own call patterns).
//   - `selfBlocked == false`: some OTHER thread was just made ready by the caller (OSResumeThread/
//     OSWakeupThread/OSSetThreadPriority) -- switch away from `self` only if the best ready
//     priority is STRICTLY better than `self`'s own (`SelectThread`'s exact `currentThread->
//     priority <= priority: return` short-circuit -- ties and worse-priority-becoming-ready do NOT
//     preempt). This is the exact mechanism that makes scheduler.cpp's own call order safe without
//     any change to its (byte-identical) source: TaskSchedulerMain's `OSResumeThread(&pT->Thread)`
//     (task priority 0xF/15 or better, this port's `g_mainThread.priority` is SDK's real default,
//     0x10/16 -- see MarkCurrentThreadAsMainOSThread()) always preempts immediately, while a task's
//     own `OSResumeThread(pParentThread)` inside TaskSleep/TaskChain/TaskExit (resuming a
//     WORSE-priority thread) never does -- the task keeps running its own next statement
//     (OSSleepThread/OSExitThread) exactly as real hardware would, instead of racing a second real
//     thread to it (section 33's actual bug).
//   - `yield == true` (OSYieldThread): like selfBlocked, but if nothing else is ready this returns
//     immediately instead of switching to itself.
void Reschedule(OSThread* self, bool selfBlocked, bool yield)
{
    if (!selfBlocked) {
        int best = BestReadyPriority();
        if (best < 0 || self->priority <= best) {
            return; // nothing better ready -- `self` keeps running, unchanged
        }
        self->state = OS_THREAD_STATE_READY;
        SetRun(self);
    } else if (yield) {
        if (BestReadyPriority() < 0) {
            return; // yielding with nothing else ready is legal -- keep running
        }
        self->state = OS_THREAD_STATE_READY;
        SetRun(self);
    }
    // else: selfBlocked && !yield -- the caller already updated self's state/queue membership
    // itself (asleep on a real queue, suspended, or about to go inert via OSExitThread); nothing to
    // enqueue here.

    int best = BestReadyPriority();
    if (best < 0) {
        std::fprintf(stderr,
                     "re4_port: scheduler deadlock -- every OSThread is blocked, none runnable "
                     "(self=%p state=%d) -- this game never legitimately reaches this state; a "
                     "real bug upstream of this file\n",
                     (void*) self, self ? self->state : -1);
        std::abort();
    }
    OSThread* next = g_runQueue[best].head;
    DequeueThread(next, &g_runQueue[best]);
    if (next == self) {
        // Only reachable via the yield path picking itself back up (nothing of equal-or-better
        // priority was actually ready besides `self`) -- no real fiber switch needed.
        next->state = OS_THREAD_STATE_RUNNING;
        return;
    }
    next->state = OS_THREAD_STATE_RUNNING;
    Fiber* fromFiber = g_recs[self].fiber;
    Fiber* toFiber = g_recs[next].fiber;
    g_current = next;
    FiberSwitchTo(fromFiber, toFiber);
    // Control returns here once some LATER Reschedule() call switches back to `self` -- `self` is
    // g_current again at that point (whichever call made that switch set it).
}

// Every task fiber's real entry point (include/port/fiber.h's FiberEntry) -- calls the game's own
// task function, catches OSExitThread's unwind exception (letting this fiber's own C++ stack frames
// destruct normally first, a host-only nicety with no real-hardware equivalent but harmless: real
// hardware just abandons the stack), then permanently switches away. Never returns in practice (see
// `on_finish` below, wired only as a defensive abort if it somehow did).
void TaskFiberEntry(void* p)
{
    OSThread* thread = static_cast<OSThread*>(p);
    ThreadRec rec = g_recs[thread]; // copy: rec.func/param never change again for this incarnation
    try {
        rec.func(rec.param);
    } catch (const ThreadExitException&) {
        // Normal termination path (OSExitThread) -- see the file header's design note.
    }
    // Real OSExitThread()/OSCancelThread() semantics for a DETACHED thread (src/lib/OSThread.c:
    // `if (thread->attr & 1) { DEQUEUE_THREAD(&__OSActiveThreadQueue); state = 0; }` --
    // scheduler.cpp always passes `attr=1` to OSCreateThread, so this is the only branch that
    // matters here): fully inert, no queue membership, never runnable again.
    thread->state = 0;
    Reschedule(thread, /*selfBlocked=*/true, /*yield=*/false);
    std::fprintf(stderr, "re4_port: TaskFiberEntry: Reschedule() returned to an exited thread -- "
                          "should never happen\n");
    std::abort();
}

void AbortOnFiberReturn(Fiber*, void*)
{
    std::fprintf(stderr, "re4_port: a task fiber's entry function returned instead of calling "
                          "OSExitThread()/OSExitThread() unwinding through TaskFiberEntry\n");
    std::abort();
}

} // namespace

void MarkCurrentThreadAsMainOSThread()
{
    // src/lib/OSThread.c's `__OSThreadInit()`'s own `DefaultThread`: state RUNNING, priority/base
    // 0x10 (16) -- the real SDK's default main-thread priority, numerically worse than every task
    // priority this game ever creates (0xF/15 or better -- see include/scheduler.h, `TASK::Priority`,
    // and every call site that sets it), which is exactly the fact Reschedule()'s "not selfBlocked"
    // rule above depends on for scheduler.cpp's task<->parent hand-back ordering to stay correct.
    g_mainThread.priority = 0x10;
    g_mainThread.base = 0x10;
    g_mainThread.suspend = 0;
    g_mainThread.state = OS_THREAD_STATE_RUNNING;
    g_current = &g_mainThread;
    g_recs[&g_mainThread].fiber = FiberFromCurrentContext();
}

} // namespace re4_port

extern "C" {

void OSInitThreadQueue(OSThreadQueue* queue)
{
    if (queue) {
        queue->head = nullptr;
        queue->tail = nullptr;
    }
}

OSThread* OSGetCurrentThread(void)
{
    return re4_port::g_current;
}

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize,
                    OSPriority priority, u16 attr)
{
    if (!thread) {
        return 0;
    }
    // Mirrors src/lib/OSThread.c's own OSCreateThread(): state READY but suspend=1 (so NOT yet
    // enqueued into any run queue -- a subsequent OSResumeThread() is what actually makes it
    // runnable, exactly like real hardware).
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->base = priority;
    thread->priority = priority;
    thread->suspend = 1;
    thread->queue = nullptr;
    thread->stackBase = (u8*) stack;
    thread->stackEnd = (u32*) ((u8*) stack - stackSize);
    if (thread->stackEnd != nullptr) {
        // scheduler.cpp's StackOverflowCheck() reads exactly this word.
        *thread->stackEnd = OS_THREAD_STACK_MAGIC;
    }

    void* slotBase = re4_port::AssignTaskStackSlot(thread);
    re4_port::ThreadRec& rec = re4_port::g_recs[thread];
    if (rec.fiber) {
        // This OSThread* (a TASK slot) is being reused for a new incarnation (TaskExec/TaskChain
        // respawning it) -- the previous fiber is provably dead (its own OSExitThread has already
        // run, or it was OSCancelThread()'d) and never switched to again; free its bookkeeping
        // before replacing it so repeated respawns don't leak one Fiber object each time.
        re4_port::FiberDestroy(rec.fiber);
    }
    rec.func = func;
    rec.param = param;
    rec.fiber = re4_port::FiberCreate(slotBase, re4_port::kTaskStackSlotSize, &re4_port::TaskFiberEntry,
                                       thread, &re4_port::AbortOnFiberReturn);
    return 1;
}

s32 OSResumeThread(OSThread* thread)
{
    if (!thread) {
        return 0;
    }
    s32 old = thread->suspend--;
    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_READY:
            re4_port::SetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            // Was asleep with suspend>0 (suspended while sleeping) -- becomes runnable in place:
            // move it from its sleep queue to its priority's run queue. (Real OSThread.c
            // re-inserts priority-ordered on the sleep queue too on a plain resume that doesn't
            // clear suspend to 0; not reproduced here -- this port's sleep queues are plain FIFOs,
            // matching scheduler.cpp's own single-sleeper-per-queue usage.)
            re4_port::DequeueThread(thread, thread->queue);
            thread->state = OS_THREAD_STATE_READY;
            re4_port::SetRun(thread);
            break;
        default:
            break;
        }
        re4_port::Reschedule(re4_port::g_current, /*selfBlocked=*/false, /*yield=*/false);
    }
    return old;
}

s32 OSSuspendThread(OSThread* thread)
{
    if (!thread) {
        return 0;
    }
    s32 old = thread->suspend++;
    if (old == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_RUNNING:
            // Suspending the CURRENTLY RUNNING thread (self-suspend, or -- in this port -- only
            // ever reachable if `thread == re4_port::g_current`, since exactly one OSThread is
            // ever RUNNING at a time): stays READY-but-unqueued (matches real OSSuspendThread's
            // RUNNING case: state set to READY, no SetRun -- suspend>0 keeps it out of every run
            // queue until resumed) and yields the CPU.
            thread->state = OS_THREAD_STATE_READY;
            re4_port::Reschedule(thread, /*selfBlocked=*/true, /*yield=*/false);
            break;
        case OS_THREAD_STATE_READY:
            re4_port::UnsetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            // Stays asleep on its own queue; just marked suspended so a wakeup will mark it READY
            // without making it runnable until also resumed (see OSWakeupThread's own suspend<=0
            // check below) -- matches real OSSuspendThread's WAITING case in effect, if not in the
            // (unused here) priority-requeue mechanics.
            break;
        default:
            break;
        }
    }
    return old;
}

void OSSleepThread(OSThreadQueue* queue)
{
    OSThread* self = re4_port::g_current;
    self->state = OS_THREAD_STATE_WAITING;
    re4_port::EnqueueThread(self, queue);
    re4_port::Reschedule(self, /*selfBlocked=*/true, /*yield=*/false);
}

void OSWakeupThread(OSThreadQueue* queue)
{
    // Real semantics: wakes up EVERY thread currently asleep on this queue (src/lib/OSThread.c's
    // own `while (queue->head)` loop), not just one -- scheduler.cpp only ever has one task asleep
    // per TASK::Queue at a time in practice, but this stays correct for any other caller (this
    // file's own OSSemaphore functions, below).
    bool anyWoken = false;
    while (queue->head != nullptr) {
        OSThread* t = queue->head;
        re4_port::DequeueThread(t, queue);
        t->state = OS_THREAD_STATE_READY;
        if (t->suspend <= 0) {
            re4_port::SetRun(t);
            anyWoken = true;
        }
    }
    if (anyWoken) {
        re4_port::Reschedule(re4_port::g_current, /*selfBlocked=*/false, /*yield=*/false);
    }
}

void OSExitThread(void* val)
{
    (void) val;
    throw re4_port::ThreadExitException{};
}

void OSCancelThread(OSThread* thread)
{
    if (!thread) {
        return;
    }
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (thread->suspend <= 0) {
            re4_port::UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_WAITING:
        re4_port::DequeueThread(thread, thread->queue);
        break;
    default:
        break;
    }
    // Detached (scheduler.cpp always passes attr=1 to OSCreateThread) -- goes fully inert, matching
    // OSCancelThread's/OSExitThread's own `thread->attr & 1` branch in src/lib/OSThread.c. Its
    // fiber's own C++ stack is abandoned without unwinding (same as real hardware abandoning the
    // thread's real stack outright -- there is no "currently executing" context here to safely
    // unwind, unlike OSExitThread's own-thread case above).
    thread->state = 0;
    re4_port::Reschedule(re4_port::g_current, /*selfBlocked=*/false, /*yield=*/false);
}

void OSYieldThread(void)
{
    re4_port::Reschedule(re4_port::g_current, /*selfBlocked=*/true, /*yield=*/true);
}

int OSSetThreadPriority(OSThread* thread, OSPriority priority)
{
    if (!thread || priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) {
        return 0;
    }
    if (thread->base == priority) {
        return 1;
    }
    thread->base = priority;
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (thread->suspend <= 0) {
            re4_port::UnsetRun(thread);
            thread->priority = priority;
            re4_port::SetRun(thread);
        } else {
            thread->priority = priority;
        }
        break;
    case OS_THREAD_STATE_WAITING:
        thread->priority = priority;
        break;
    case OS_THREAD_STATE_RUNNING:
        thread->priority = priority;
        break;
    default:
        thread->priority = priority;
        break;
    }
    re4_port::Reschedule(re4_port::g_current, /*selfBlocked=*/false, /*yield=*/false);
    return 1;
}

s32 OSGetThreadPriority(OSThread* thread)
{
    return thread ? thread->base : 0;
}

BOOL OSIsThreadSuspended(OSThread* thread)
{
    return (thread && thread->suspend > 0) ? 1 : 0;
}

BOOL OSIsThreadTerminated(OSThread* thread)
{
    return (!thread || thread->state == OS_THREAD_STATE_MORIBUND || thread->state == 0) ? 1 : 0;
}

// ---- OSSemaphore: modeled directly on src/lib/OSSemaphore.c (real, already-decompiled SDK
// source in this tree) -- the only difference is no OSDisableInterrupts()/OSRestoreInterrupts()
// pair (nothing here can be interrupted mid-function; see this file's own header comment).

void OSInitSemaphore(OSSemaphore* sem, s32 count)
{
    OSInitThreadQueue(&sem->queue);
    sem->count = count;
}

s32 OSWaitSemaphore(OSSemaphore* sem)
{
    s32 count;
    while ((count = sem->count) <= 0) {
        OSSleepThread(&sem->queue);
    }
    sem->count = count - 1;
    return count;
}

s32 OSTryWaitSemaphore(OSSemaphore* sem)
{
    s32 count = sem->count;
    if (count > 0) {
        sem->count = count - 1;
    }
    return count;
}

s32 OSSignalSemaphore(OSSemaphore* sem)
{
    s32 count = sem->count;
    sem->count = count + 1;
    OSWakeupThread(&sem->queue);
    return count;
}

s32 OSGetSemaphoreCount(OSSemaphore* sem)
{
    return sem->count;
}

} // extern "C"

#endif // TARGET_PC
