// Real host semantics for src/game/scheduler.cpp's cooperative task scheduler, replacing the
// generated logging-only stubs for OSCreateThread/OSResumeThread/OSSuspendThread/OSSleepThread/
// OSWakeupThread/OSGetCurrentThread/OSExitThread/OSInitThreadQueue (docs/port-phase3.md's
// continuation -- the root cause of "Stack overflow in Thread 0": OSCreateThread was a pure no-op,
// so no task's real thread ever ran, and its stack's OS_THREAD_STACK_MAGIC guard word -- normally
// written by a real OSCreateThread -- was never set, so scheduler.cpp's own StackOverflowCheck()
// tripped on the very first task dispatched).
//
// Design (read this before touching any function below):
//
// Every "task" (src/game/scheduler.cpp's TASK/OSThread pair) becomes a real host std::thread. The
// scheduler's whole design assumes exactly one of these threads is ever doing meaningful work at a
// time (a single Gekko core; TaskScheduler() explicitly runs each slot "one after another" every
// frame) -- game code is not written to be safe under genuine concurrent execution of two tasks'
// bodies. Real hardware gets that exclusivity from true OS-level preemptive scheduling with
// priorities (OSResumeThread on a higher-or-equal-priority thread effects an immediate switch away
// from the caller); this host cannot reproduce that at instruction granularity with plain host
// threads. Instead, a single explicit "whose turn is it" token (`g_holder`) is passed by hand:
//
//   - OSResumeThread(target)/OSWakeupThread(queue): non-blocking, exactly like the real API --
//     decrement a suspend counter or find the thread asleep on a queue, and if it becomes
//     runnable, set g_holder = target and wake every thread waiting on the shared condvar. This
//     matches scheduler.cpp's own TaskSleep/TaskExit/TaskChain, which call OSResumeThread on
//     `pParentThread` themselves to hand control *back* -- if these blocked their caller, that
//     hand-back would deadlock (the caller is often the task about to sleep or exit, which must
//     keep running a few more lines, or must be allowed to never run again at all).
//   - Every thread, whenever it is about to actually execute task code (fresh from OSCreateThread,
//     or woken from OSSleepThread), calls BecomeRunner(self): block until g_holder == self.
//   - The *driving* thread (real hardware: main, running TaskSchedulerMain) cannot rely on any of
//     the above to know when the task it just resumed has handed control back -- so
//     WaitForHandback() (include/port/os_thread.h) is called explicitly, from three TARGET_PC-only
//     lines added to scheduler.cpp (docs/port-phase3.md), right after each of its own
//     OSResumeThread()/OSWakeupThread() calls: it simply calls BecomeRunner(t_self) again, waiting
//     for the token to come back. This is not part of the real Dolphin OS API (real hardware has
//     no equivalent call) -- it is this host's stand-in for "wait for the preemption/priority
//     mechanism to give the CPU back", made explicit and deterministic instead of implicit and
//     timing-dependent.
//
// Known limitation, accepted for now (not a correctness bug, a resource leak): a task thread that
// terminates via TaskExit()/TaskChain() calls OSResumeThread(pParentThread) (handing the token
// back) and then OSExitThread() (this file: throws a ThreadExitException to unwind the thread's
// C++ stack safely, caught in OSCreateThread's thread body) -- but if a *different* task slot's
// OSCreateThread later reuses the same OSThread* address before this thread has actually finished
// unwinding, nothing currently joins the old std::thread first (it is detached, matching
// arena.cpp's own precedent). In practice this is harmless (the old thread finishes on its own,
// detached destruction is safe) but is not a provably bounded design -- fine for reaching a
// renderable frame, flagged here for whoever revisits this after first boot.
#ifdef TARGET_PC

#include "port/arena.h"
#include "port/os_thread.h"

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unordered_map>

#include "types.h"
#include <dolphin/os/OSThread.h>

namespace re4_port {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
OSThread* g_holder = nullptr; // whose "turn" it is; see the file header design note

// Which OSThread* (if any) is currently asleep on a given OSThreadQueue*, and whether it has been
// told to wake -- OSSleepThread/OSWakeupThread's bookkeeping. A side table, not the OSThreadQueue's
// own head/tail links (those stay real-but-unused, same as OSInitThreadQueue already set them up;
// game code never dereferences a queue's internals directly, only ever passes the pointer around).
std::unordered_map<OSThreadQueue*, OSThread*> g_sleepers;
std::unordered_map<OSThread*, bool> g_wakeFlag;

thread_local OSThread* t_self = nullptr;

OSThread g_mainThread{}; // storage for the main game thread's OSThread identity (never read back
                          // as an on-disc/relocated value, so an ordinary static object is fine --
                          // no layout concerns beyond matching the header's field types).

struct ThreadExitException {}; // OSExitThread's unwind mechanism -- see the file header.

// Every task thread this file starts runs a real pthread whose *machine* stack is one slot of
// include/port/arena.h's dedicated task-stack pool (docs/port-boot.md section 29), not the game's
// own small per-task stack buffer (scheduler.cpp's `TaskSchedulerInit()` -> `MEM_ALLOC`, still used
// unchanged for the vendor's own `StackOverflowCheck()` bookkeeping, `thread->stackBase`/`stackEnd`
// below) and not a plain host-allocated stack either. Root cause found live, this pass:
// `pthread_attr_setstack()` on macOS requires both the address and size to be page-aligned
// (16 KiB), and every one of the vendor's own stack sizes (`GetStackSize()`, 0x1800/0x2000/0x3000
// bytes -- the original PPC target's tiny stack budget) is neither, so it failed for every task
// thread, every run, not intermittently. Any local variable's address on this thread's real
// execution stack must itself be a valid, GC32()-able pointer for the vendor's own "pointer stored
// in a u32" idioms (docs/port-boot.md section 27's `DrawOTag` bug and its kin) to work -- a
// host-allocated stack is not guaranteed to land inside the compressed-handle window at all, so
// there is no host-stack fallback here: if a pool slot can't be used, this aborts instead
// (RE4_PORT_CHECK-style, loud and immediate, not a silent correctness gap).
std::unordered_map<OSThread*, void*> g_taskStackSlots; // OSThread* -> its assigned pool slot base
                                                        // (include/port/arena.h's
                                                        // GetTaskStackPoolBase()), assigned once per
                                                        // distinct OSThread* (at most
                                                        // re4_port::kTaskStackSlots, TASK_NUM)
std::size_t g_nextTaskStackSlot = 0;

std::unordered_map<OSThread*, bool> g_threads; // presence only -- no join primitive yet (arena.cpp's
                                                // own CreateArenaThread precedent: detached,
                                                // fire-and-forget)

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

struct OSTaskThreadArgs {
    OSThread* thread;
    void* (*func)(void*);
    void* param;
};

void BecomeRunner(OSThread* self)
{
    std::unique_lock<std::mutex> lk(g_mutex);
    g_cv.wait(lk, [&] { return g_holder == self; });
}

void* OSTaskThreadEntry(void* p)
{
    std::unique_ptr<OSTaskThreadArgs> args(static_cast<OSTaskThreadArgs*>(p));
    OSThread* thread = args->thread;
    void* (*func)(void*) = args->func;
    void* param = args->param;
    t_self = thread;
    BecomeRunner(thread);
    try {
        func(param);
    } catch (const ThreadExitException&) {
        // Normal termination path (OSExitThread) -- see the file header's design note.
    }
    thread->state = OS_THREAD_STATE_MORIBUND;
    return nullptr;
}

} // namespace

void MarkCurrentThreadAsMainOSThread()
{
    t_self = &g_mainThread;
    g_mainThread.suspend = 0;
    g_mainThread.priority = 0;
    g_mainThread.state = OS_THREAD_STATE_RUNNING;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_holder = &g_mainThread;
}

void WaitForHandback()
{
    BecomeRunner(t_self);
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
    return re4_port::t_self;
}

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize,
                    OSPriority priority, u16 attr)
{
    if (!thread) {
        return 0;
    }
    thread->suspend = 1; // created suspended, matching real OSCreateThread -- a subsequent
                          // OSResumeThread() is required before it actually runs
    thread->priority = priority;
    thread->base = priority;
    thread->state = OS_THREAD_STATE_READY;
    thread->stackBase = (u8*) stack;
    thread->stackEnd = (u32*) ((u8*) stack - stackSize);
    if (thread->stackEnd != nullptr) {
        // scheduler.cpp's StackOverflowCheck() reads exactly this word -- the whole reason this
        // stub needed real semantics in the first place.
        *thread->stackEnd = OS_THREAD_STACK_MAGIC;
    }

    // Real (machine) stack: one page-aligned, page-sized slot of the dedicated task-stack pool --
    // see this file's own comment on `g_taskStackSlots` above for why not the game's own buffer.
    void* slotBase = re4_port::AssignTaskStackSlot(thread);
    auto* targs = new re4_port::OSTaskThreadArgs{thread, func, param};
    bool started = re4_port::CreateThreadOnStack(
        slotBase, re4_port::kTaskStackSlotSize, re4_port::OSTaskThreadEntry, targs);
    if (!started) {
        std::fprintf(stderr,
                     "OSCreateThread: pthread_attr_setstack failed for pool slot=[%p, %p) -- no "
                     "host-stack fallback for a game task thread (docs/port-boot.md section 29), "
                     "aborting\n",
                     slotBase, static_cast<char*>(slotBase) + re4_port::kTaskStackSlotSize);
        delete targs;
        std::abort();
    }
    {
        std::lock_guard<std::mutex> lk(re4_port::g_mutex);
        re4_port::g_threads[thread] = true;
    }
    return 1;
}

s32 OSResumeThread(OSThread* thread)
{
    if (!thread) {
        return 0;
    }
    s32 old;
    bool ready;
    {
        std::lock_guard<std::mutex> lk(re4_port::g_mutex);
        old = thread->suspend;
        thread->suspend--;
        ready = thread->suspend <= 0;
        if (ready) {
            re4_port::g_holder = thread;
        }
    }
    if (ready) {
        re4_port::g_cv.notify_all();
    }
    return old;
}

s32 OSSuspendThread(OSThread* thread)
{
    if (!thread) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(re4_port::g_mutex);
    s32 old = thread->suspend;
    thread->suspend++;
    return old;
}

void OSSleepThread(OSThreadQueue* queue)
{
    OSThread* self = re4_port::t_self;
    {
        std::lock_guard<std::mutex> lk(re4_port::g_mutex);
        re4_port::g_sleepers[queue] = self;
        re4_port::g_wakeFlag[self] = false;
    }
    {
        std::unique_lock<std::mutex> lk(re4_port::g_mutex);
        re4_port::g_cv.wait(lk, [&] { return re4_port::g_wakeFlag[self]; });
    }
    re4_port::BecomeRunner(self); // idempotent: OSWakeupThread already set g_holder == self in the
                                  // same critical section as the wake flag above
}

void OSWakeupThread(OSThreadQueue* queue)
{
    OSThread* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(re4_port::g_mutex);
        auto it = re4_port::g_sleepers.find(queue);
        if (it != re4_port::g_sleepers.end()) {
            target = it->second;
            re4_port::g_sleepers.erase(it);
            re4_port::g_wakeFlag[target] = true;
            re4_port::g_holder = target;
        }
    }
    if (target != nullptr) {
        re4_port::g_cv.notify_all();
    }
}

void OSExitThread(void* val)
{
    (void) val;
    throw re4_port::ThreadExitException{};
}

} // extern "C"

#endif // TARGET_PC
