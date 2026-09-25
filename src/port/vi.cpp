// See include/port/vi.h for the threading design this implements.
#ifndef TARGET_PC
#error "src/port/vi.cpp is host-only (TARGET_PC)"
#endif

#include "port/alloc.h"
#include "port/vi.h"
#include "port/screenshot.h"

#include <dolphin/vi/vifuncs.h>
#include <dolphin/vi/vitypes.h>

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

thread_local bool t_gxFrameActive = false;

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<u32> g_retraceCount{0};
VIRetraceCallback g_preRetrace = nullptr;
VIRetraceCallback g_postRetrace = nullptr;
std::atomic<u32> g_nextField{0};
void* g_nextFrameBuffer = nullptr;
std::atomic<bool> g_black{false};

// Advances virtual time by exactly one retrace, synchronously, on the calling (game) thread: bumps
// the counter/field, then invokes the registered pre/post callbacks directly (safe here the same
// way DrainPendingCallbacks() already is -- only ever called from the one real thread that is
// allowed to touch the fiber scheduler). No wall-clock wait, no dependency on RunPresentLoop().
void TickOneRetraceNow()
{
    VIRetraceCallback pre;
    VIRetraceCallback post;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pre = g_preRetrace;
        post = g_postRetrace;
    }
    u32 count = g_retraceCount.fetch_add(1) + 1;
    g_nextField.fetch_add(1);
    if (pre) {
        pre(count);
    }
    if (post) {
        post(count);
    }
}

// Real, byte-matching game code registers callbacks here (src/game/main.cpp's postVSyncCallback,
// via VISetPostRetraceCallback) that call straight into src/port/os_thread.cpp's fiber scheduler
// (postVSyncCallback -> iTaskSuspend() -> OSSuspendThread()) -- that scheduler has exactly one
// real host thread it is ever safe to call into (docs/port-boot.md section 34's whole reason for
// existing: no lock anywhere in it, by construction, since only one fiber is ever running). This
// present loop runs on a DIFFERENT real host thread (the process main thread, AppKit's event
// loop -- see include/port/vi.h's own design note), so calling a registered callback directly from
// here would be exactly the two-real-threads-touching-scheduler-state bug that whole rewrite
// exists to eliminate. Fixed the way the coordinator's design asked for interrupt-context
// callbacks in general: queue them here (this mutex is the only lock touching this queue -- it
// never reaches into os_thread.cpp's own state) and let VIWaitForRetrace() -- called by the GAME
// thread once per frame, src/game/main.cpp's own real, unchanged call sites -- drain and run them
// there instead, right after the very retrace tick they were queued for. This is a real, if bounded
// (well under one frame, in practice the very next VIWaitForRetrace() call), latency shift on
// exactly when postVSyncCallback's vsync_cnt++/haltExecCheck() side effects land relative to a real
// hardware interrupt; every call still happens, in order, exactly once per real tick.
std::mutex g_cbMutex;
std::deque<std::pair<VIRetraceCallback, u32>> g_pendingCallbacks;

// Drains and runs whatever is currently queued -- shared by VIWaitForRetrace() (which additionally
// blocks until the next tick first) and re4_port::PumpPendingVICallbacks() (which does not block at
// all -- see its own header comment in include/port/vi.h for why a non-blocking drain point is
// needed too: main.cpp's own `while (vsync_cnt < ...) {}` spins never call VIWaitForRetrace()).
void DrainPendingCallbacks()
{
    std::deque<std::pair<VIRetraceCallback, u32>> due;
    {
        std::lock_guard<std::mutex> lock(g_cbMutex);
        due.swap(g_pendingCallbacks);
    }
    for (auto& [cb, count] : due) {
        if (cb) {
            cb(count);
        }
    }
}

} // namespace

namespace re4_port {

// RE4_PORT_FIXED_VI=1 (docs/port-boot.md, coordinator's determinism request): read once, lazily,
// the first time any of VIWaitForRetrace()/PumpPendingVICallbacks()/RunPresentLoop() runs. In this
// mode RunPresentLoop() (the real host main thread, real wall-clock ~60 Hz pacing) stops being the
// tick source entirely: it still pumps aurora_update() so the window stays responsive and
// screenshots still work, but it neither bumps `g_retraceCount` nor queues a callback. Instead,
// every call that would otherwise wait for/drain a tick becomes the tick itself (TickOneRetraceNow(),
// above) -- a given RE4_PORT_INPUT script then advances through exactly the same sequence of
// retrace/callback events on every run, with no dependency on real elapsed time anywhere in that
// path.
bool IsFixedViMode()
{
    static const bool fixed = [] {
        const char* env = std::getenv("RE4_PORT_FIXED_VI");
        return env != nullptr && env[0] == '1' && env[1] == '\0';
    }();
    return fixed;
}

} // namespace re4_port

extern "C" {

void VIWaitForRetrace(void)
{
    if (re4_port::IsFixedViMode()) {
        TickOneRetraceNow();
        return;
    }
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        u32 start = g_retraceCount.load();
        g_cv.wait(lock, [start] { return g_retraceCount.load() != start; });
    }
    // Drain and run whatever RunPresentLoop() queued for the tick(s) up to and including the one
    // that just woke this wait -- on THIS (the game) thread, safe to call into the fiber scheduler.
    DrainPendingCallbacks();
}

u32 VIGetRetraceCount(void)
{
    return g_retraceCount.load();
}

u32 VIGetNextField(void)
{
    // Real hardware alternates the interlaced field each retrace; toggle the same way so callers
    // that use this only to pick a debug on-screen coordinate (main_sub.cpp's Render_before()) see
    // a real alternating value instead of a fixed 0.
    return g_nextField.load() & 1;
}

void VISetNextFrameBuffer(void* fb)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_nextFrameBuffer = fb;
}

void VISetBlack(BOOL black)
{
    g_black.store(black != 0);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    VIRetraceCallback old = g_preRetrace;
    g_preRetrace = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    VIRetraceCallback old = g_postRetrace;
    g_postRetrace = cb;
    return old;
}

} // extern "C"

namespace re4_port {

void BeginGxFrame()
{
    // include/port/alloc.h's HostAllocScope: aurora_begin_frame() runs synchronously on the game
    // thread (real vendor Render_before() calls it inline, not a separate thread) and allocates its
    // own renderer-internal bookkeeping (command/resource pools) via plain `new` -- without this
    // guard that lands in the game's fixed-size GameCube heaps every frame (main_mem.cpp's global
    // operator new override can't otherwise tell "host GPU backend bookkeeping" apart from a real
    // game allocation on the same thread), silently eating into the tiny per-heap budgets the
    // vendor's own code sizes exactly (docs/port-boot.md's card-heap "6.8 KB short" symptom).
    re4_port::HostAllocScope hostAlloc;
    t_gxFrameActive = aurora_begin_frame();
    if (!t_gxFrameActive) {
        std::fprintf(stderr, "re4_port: aurora_begin_frame() returned false -- this frame's GX "
                              "submission has no active recording session (window minimized/GPU "
                              "not ready); Render()/DrawOTag still run unconditionally (main.cpp's "
                              "own frame loop, unchanged) -- TO VERIFY whether that risks the same "
                              "\"No active recording session\" abort this function exists to avoid\n");
    }
}

void EndGxFrame()
{
    if (t_gxFrameActive) {
        re4_port::HostAllocScope hostAlloc; // see BeginGxFrame()
        aurora_end_frame();
        t_gxFrameActive = false;
    }
}

void PumpPendingVICallbacks()
{
    if (IsFixedViMode()) {
        // main.cpp's own `while (vsync_cnt < GetSystemVcnt() - 1) { ...; PumpPendingVICallbacks(); }`
        // spins (include/port/vi.h's own header comment on this function) are this mode's other real
        // tick source besides VIWaitForRetrace() itself -- RunPresentLoop() no longer produces
        // anything for them to drain, so each call here has to be the tick, the same as
        // VIWaitForRetrace(), or those spins would never terminate in fixed mode.
        TickOneRetraceNow();
        return;
    }
    DrainPendingCallbacks();
}

void RunPresentLoop(const char* appName, std::atomic<bool>* shouldExit)
{
    AuroraConfig config{};
    config.appName = appName;
    config.vsync = true;
    config.mem1Size = 0; // re4_port::InitMem1() (src/game/main.cpp's TARGET_PC OSInit() branch)
    config.mem2Size = 0; // already owns MEM1/ARAM sizing -- do not let Aurora allocate its own.

    aurora_initialize(0, nullptr, &config);
    std::fprintf(stderr, "re4_boot: Aurora window opened\n");

    // Screenshot-from-inside-the-process (docs/port-boot.md section 29): the boot sequence
    // currently crashes within a few real seconds of the window opening, too fast for an external
    // `screencapture` invocation to reliably beat -- so, only if RE4_PORT_SCREENSHOT is set (a
    // destination .png path), fire a detached thread that sleeps briefly (RE4_PORT_SCREENSHOT_DELAY_MS,
    // default 1500) then captures just this process's own window (re4_port::CaptureOwnWindowScreenshot,
    // src/port/screenshot.cpp -- finds the window by CGWindowID via this process's own PID, falls
    // back to a whole-screen capture if that fails) while the process (and its window) is still
    // alive. Best-effort: if the process has already crashed by the time this fires, the capture
    // simply shows the desktop -- `view` the PNG afterward to tell which happened, don't assume.
    if (const char* path = std::getenv("RE4_PORT_SCREENSHOT")) {
        int delayMs = 1500;
        if (const char* delayEnv = std::getenv("RE4_PORT_SCREENSHOT_DELAY_MS")) {
            delayMs = std::atoi(delayEnv);
        }
        std::string dest(path);
        std::fprintf(stderr, "re4_boot: screenshot thread armed, firing in %d ms -> %s\n", delayMs,
                     dest.c_str());
        std::thread([dest, delayMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            re4_port::CaptureOwnWindowScreenshot(dest.c_str());
        }).detach();
    }

    // Paces this loop's retrace tick to ~59.94 Hz (real NTSC field rate, VIGetTvFormat()==0) with an
    // explicit deadline, not a plain fixed sleep_for() (which would drift) and NOT
    // aurora_end_frame()'s own real-vsync block (this loop no longer calls aurora_begin_frame()/
    // aurora_end_frame() at all -- see BeginGxFrame()/EndGxFrame() above for why those moved to the
    // game thread). Without this, main.cpp's own hang detector (haltExecCheck(), vsync_cnt > 3599)
    // fires within a few seconds: found live, this session, after moving begin/end_frame off this
    // loop removed its only previous pacing source and this loop free-ran as fast as aurora_update()
    // itself could spin.
    using clock = std::chrono::steady_clock;
    constexpr auto kFieldPeriod =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1001.0 / 60000.0));
    auto nextTick = clock::now();

    bool exiting = false;
    while (!exiting && !shouldExit->load()) {
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                exiting = true;
            }
            ++event;
        }
        if (exiting) {
            break;
        }

        // aurora_begin_frame()/aurora_end_frame() are NOT called here (contrary to this loop's
        // first version) -- see include/port/vi.h's BeginGxFrame()/EndGxFrame() for why: Aurora's
        // GX recording session must be active on whichever thread issues the real GX submission
        // calls (the game thread), not this one.

        // RE4_PORT_FIXED_VI=1: this loop no longer drives game time at all -- VIWaitForRetrace()/
        // PumpPendingVICallbacks() self-tick on the game thread instead (TickOneRetraceNow(), above).
        // Skip the counting/queueing below entirely so the two tick sources can never double-count
        // or race each other; still run aurora_update() every iteration (window stays responsive,
        // screenshots still fire) at this same real ~60 Hz pace, just for presentation, not timing.
        if (!IsFixedViMode()) {
            // One "retrace": bump the counter and QUEUE the game's registered callbacks (they run on
            // the game thread instead, drained by VIWaitForRetrace() -- see g_pendingCallbacks' own
            // comment above for why this stopped calling them directly on this thread).
            VIRetraceCallback pre;
            VIRetraceCallback post;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                pre = g_preRetrace;
                post = g_postRetrace;
            }
            u32 count = g_retraceCount.fetch_add(1) + 1;
            g_nextField.fetch_add(1);
            if (pre || post) {
                std::lock_guard<std::mutex> lock(g_cbMutex);
                if (pre) {
                    g_pendingCallbacks.emplace_back(pre, count);
                }
                if (post) {
                    g_pendingCallbacks.emplace_back(post, count);
                }
            }
            g_cv.notify_all();
        }

        nextTick += kFieldPeriod;
        auto now = clock::now();
        if (nextTick > now) {
            std::this_thread::sleep_until(nextTick);
        } else {
            nextTick = now; // fell behind (e.g. a slow aurora_update()) -- don't try to catch up by
                             // bursting retraces, just resume pacing from here.
        }
    }

    aurora_shutdown();
}

} // namespace re4_port
