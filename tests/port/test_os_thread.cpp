// Host test for src/port/os_thread.cpp's fiber-based OSThread scheduler (docs/port-boot.md section
// 34). Exercises it directly (not through src/game/scheduler.cpp -- that needs the whole game
// booted) but with the SAME call shapes scheduler.cpp itself uses, so a regression here is a real
// regression there too:
//  - "task sleep" ping-pong: OSCreateThread + OSResumeThread (first dispatch) then repeated
//    OSWakeupThread (subsequent dispatches), the task itself calling OSResumeThread(parent) then
//    OSSleepThread(&queue) each round -- this is scheduler.cpp's TaskSchedulerMain/TaskSleep call
//    order exactly (docs/port-boot.md section 33's actual bug shape), run 5 times in a row.
//  - Priority ordering: three threads at different priorities, all created suspended; only the
//    WORST-priority one is ever resumed directly (by main, which is worse-priority than all three)
//    -- it resumes the other two itself before recording its own turn, so the only way the
//    observed order can come out priority-sorted is if OSResumeThread's "immediate switch on a
//    strictly-better-priority thread becoming ready" rule is actually implemented.
//  - Semaphore blocking/signalling with a priority-driven wakeup-vs-continue race (the consumer
//    is better priority than the producer, so signalling it must switch to it immediately, mid-
//    OSSignalSemaphore, not after the producer finishes).
//  - Thread exit (OSExitThread's unwind-then-terminate path) as part of every scenario above.
#include "port/arena.h"
#include "port/os_thread.h"

#include "types.h"
#define _DOLPHIN_TYPES_H_
#include <dolphin/os/OSSemaphore.h>
#include <dolphin/os/OSThread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool Fail(const char* what)
{
    std::fprintf(stderr, "test_os_thread: FAILED: %s\n", what);
    return false;
}

// ---- Scenario 1: task-sleep ping-pong ----

OSThread g_worker{};
OSThreadQueue g_workerQueue;
int g_pingCounter = 0;
bool g_workerDone = false;

void* WorkerFunc(void* param)
{
    OSThread* parent = static_cast<OSThread*>(param);
    for (int i = 0; i < 5; i++) {
        g_pingCounter++;
        OSResumeThread(parent);       // scheduler.cpp's TaskSleep: hand back to the driving thread
        OSSleepThread(&g_workerQueue); // ... THEN register asleep (section 33's exact ordering)
    }
    g_workerDone = true;
    OSResumeThread(parent);
    OSExitThread(nullptr);
    return nullptr;
}

bool RunPingPongScenario()
{
    OSThread* self = OSGetCurrentThread();
    OSInitThreadQueue(&g_workerQueue);
    static char stack[16 * 1024];
    OSCreateThread(&g_worker, WorkerFunc, self, stack + sizeof(stack), sizeof(stack), 5, 1);

    OSResumeThread(&g_worker); // dispatch #1 (TaskSchedulerMain's TASK_EXEC branch)
    if (g_pingCounter != 1) {
        return Fail("ping-pong: counter != 1 after first dispatch");
    }
    for (int frame = 1; frame < 5; frame++) {
        OSWakeupThread(&g_workerQueue); // dispatch #2..5 (TaskSchedulerMain's TASK_SLEEP branch)
        if (g_pingCounter != frame + 1) {
            return Fail("ping-pong: counter mismatch on a later dispatch");
        }
    }
    if (g_workerDone) {
        return Fail("ping-pong: worker finished too early");
    }
    OSWakeupThread(&g_workerQueue); // final dispatch -- worker exits
    if (!g_workerDone) {
        return Fail("ping-pong: worker never finished");
    }
    std::printf("test_os_thread: ping-pong (5 rounds) OK\n");
    return true;
}

// ---- Scenario 2: priority ordering ----

OSThread g_tLo{}, g_tMid{}, g_tHi{};
int g_order[3];
int g_orderIdx = 0;

void* HiFunc(void*)
{
    g_order[g_orderIdx++] = 1;
    OSExitThread(nullptr);
    return nullptr;
}
void* MidFunc(void*)
{
    g_order[g_orderIdx++] = 2;
    OSExitThread(nullptr);
    return nullptr;
}
void* LoFunc(void*)
{
    OSResumeThread(&g_tHi);  // priority 2: strictly better than Lo's 10 -- preempts immediately
    OSResumeThread(&g_tMid); // priority 6: also better than Lo's 10 -- preempts immediately
    g_order[g_orderIdx++] = 3;
    OSExitThread(nullptr);
    return nullptr;
}

bool RunPriorityScenario()
{
    static char sHi[8192], sMid[8192], sLo[8192];
    OSCreateThread(&g_tHi, HiFunc, nullptr, sHi + sizeof(sHi), sizeof(sHi), 2, 1);
    OSCreateThread(&g_tMid, MidFunc, nullptr, sMid + sizeof(sMid), sizeof(sMid), 6, 1);
    OSCreateThread(&g_tLo, LoFunc, nullptr, sLo + sizeof(sLo), sizeof(sLo), 10, 1);

    OSResumeThread(&g_tLo); // priority 10, strictly better than main's 0x10/16 -- preempts

    if (g_orderIdx != 3 || g_order[0] != 1 || g_order[1] != 2 || g_order[2] != 3) {
        std::fprintf(stderr, "test_os_thread: priority order was [%d,%d,%d] (idx=%d), want [1,2,3]\n",
                     g_order[0], g_order[1], g_order[2], g_orderIdx);
        return false;
    }
    std::printf("test_os_thread: priority ordering OK\n");
    return true;
}

// ---- Scenario 3: semaphore ----

OSSemaphore g_sem;
int g_seq = 0;

void* ConsumerFunc(void*)
{
    OSWaitSemaphore(&g_sem); // blocks (count starts at 0) -- yields to main
    g_seq = 2;
    OSExitThread(nullptr);
    return nullptr;
}
void* ProducerFunc(void*)
{
    g_seq = 1;
    OSSignalSemaphore(&g_sem); // consumer (better priority) must run BEFORE this call returns
    if (g_seq != 2) {
        std::fprintf(stderr,
                     "test_os_thread: FAILED: producer resumed before consumer ran (g_seq=%d)\n",
                     g_seq);
        std::abort();
    }
    OSExitThread(nullptr);
    return nullptr;
}

bool RunSemaphoreScenario()
{
    OSInitSemaphore(&g_sem, 0);
    OSThread consumer{}, producer{};
    static char sC[8192], sP[8192];
    OSCreateThread(&consumer, ConsumerFunc, nullptr, sC + sizeof(sC), sizeof(sC), 5, 1);
    OSCreateThread(&producer, ProducerFunc, nullptr, sP + sizeof(sP), sizeof(sP), 6, 1);

    OSResumeThread(&consumer); // blocks immediately on OSWaitSemaphore -- returns to main
    if (g_seq != 0) {
        return Fail("semaphore: consumer ran past its wait before being signalled");
    }
    OSResumeThread(&producer); // signals; consumer (better priority) preempts producer mid-call
    if (g_seq != 2) {
        return Fail("semaphore: final sequence value wrong");
    }
    std::printf("test_os_thread: semaphore OK\n");
    return true;
}

} // namespace

int main()
{
    re4_port::InitArena(); // OSCreateThread's fibers get their real (machine) stack from the
                            // arena's task-stack pool (include/port/arena.h) regardless of the
                            // `stack`/`stackSize` arguments passed to it (those only feed
                            // scheduler.cpp's own StackOverflowCheck bookkeeping).
    re4_port::MarkCurrentThreadAsMainOSThread();

    bool ok = true;
    ok &= RunPingPongScenario();
    ok &= RunPriorityScenario();
    ok &= RunSemaphoreScenario();

    if (!ok) {
        return 1;
    }
    std::printf("test_os_thread: OK\n");
    return 0;
}
