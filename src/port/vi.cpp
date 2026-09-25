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

// Set once by RunPresentLoop() (host main thread) right after aurora_initialize() returns -- i.e.
// once the window/surface genuinely exist. BeginGxFrame() (game thread) waits on this before its
// very first call: the game thread is started (boot_main.cpp's CreateThreadOnStack) BEFORE
// RunPresentLoop() is even entered, so without this wait the game's first GX frame can (and, timed
// live, reliably does -- docs/port-boot.md section 44) reach aurora_begin_frame() before Aurora's
// window exists at all, which is a strictly stronger failure than "window not presentable yet"
// (is_presentable() would still see g_window == nullptr and return false the same way, but the
// real bug is the missing wait, not is_presentable()'s own logic). This is a one-time gate, not a
// per-frame cost: once true it stays true for the rest of the process.
std::atomic<bool> g_windowReady{false};

// Serializes the game thread's active GX-frame span (aurora_begin_frame()..aurora_end_frame(), i.e.
// BeginGxFrame()..EndGxFrame() below) against the host main thread's aurora_update() calls in
// RunPresentLoop(). Found necessary live: aurora_update() processes SDL window events synchronously,
// including a resize (SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED -- fires once, early, as macOS settles the
// real content-view size a few frames after the window opens) that calls into Aurora's
// resize_swapchain()/clear_caches(), which synchronizes with (drains) Aurora's own asynchronous
// "render worker" thread and then clears its bind-group cache. That drain only waits for whatever
// was already queued for the worker at the moment it runs -- it has no way to know the game thread
// (a second, independent real host thread in this port's design, unlike Aurora's own single-
// game-thread assumption) is concurrently inside gx::render() doing a bind_group_ref() cache HIT
// (reusing a still-cached id, no recreation) and about to hand a new frame packet to that same
// worker referencing that id. If the resize's clear_bind_group_cache() lands in the gap between that
// hit and the new packet actually being enqueued, the worker later calls find_bind_group() on an id
// that no longer exists -- "get_bind_group: failed to locate ..." fatal, confirmed live by tracing
// bind_group_ref/clear_bind_group_cache/find_bind_group frame numbers and thread names (the crash
// itself stacks through aurora::gfx::render_worker, not the game thread). Root cause is the same
// class already documented for the GX FIFO worker (docs/port-boot.md section 43) and the
// begin_frame/window-ready race (section 44): Aurora's internal handshakes assume one thread drives
// both window events and GX submission; this port deliberately splits them across two. Holding this
// mutex for the whole game-thread GX-frame span, and around each aurora_update() call, makes the two
// mutually exclusive so a resize (or any other Aurora-internal synchronize()+cache-clear sequence)
// can never race a not-yet-enqueued draw the way it did here -- no Aurora patch needed, this is
// purely about which of the port's own two threads may run Aurora entry points at a given moment.
std::mutex g_gxHostMutex;
// Set once, by RunPresentLoop() (host main thread), right before it takes g_gxHostMutex to begin
// tearing Aurora down (see the invariant documented at that call site). Checked by BeginGxFrame()
// (game thread) AFTER it has acquired g_gxHostMutex -- the mutex hand-off is what makes the flag
// safe to read without its own synchronization: the store below happens-before the lock() that
// follows it in program order on this thread, and any thread that later acquires the same mutex
// (including one that was already blocked waiting for it) is guaranteed to observe every write
// that happened-before that lock, per the mutex's own acquire/release semantics. So a frame that
// is already in flight when shutdown starts runs to completion unaffected (it acquired the lock
// before the flag was ever set); the next one to acquire the lock always sees the flag if shutdown
// has started, and skips touching Aurora at all instead of racing its teardown.
std::atomic<bool> g_shuttingDown{false};
// Held by BeginGxFrame() and released by EndGxFrame() -- these are two separate calls from two
// separate call sites (src/game/main_sub.cpp's Render_before()/Render_swap()) on the same (game)
// thread, so the lock has to outlive the function that acquires it; thread_local because only the
// game thread ever calls either function (same rule as t_gxFrameActive above).
thread_local std::unique_lock<std::mutex> t_gxHostLock(g_gxHostMutex, std::defer_lock);

// Drops g_gxHostMutex (if this thread holds it) for the scope's lifetime and retakes it after.
// Used around the game thread's waits for a retrace: the tick is produced by RunPresentLoop() on
// the host main thread, which must take g_gxHostMutex for aurora_update() first.
struct ReleaseGxHostLockWhileWaiting
{
    bool held = t_gxHostLock.owns_lock();
    ReleaseGxHostLockWhileWaiting()
    {
        if (held) {
            t_gxHostLock.unlock();
        }
    }
    ~ReleaseGxHostLockWhileWaiting()
    {
        if (held) {
            t_gxHostLock.lock();
        }
    }
};

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
        // The retrace tick comes from RunPresentLoop() on the host main thread, which needs
        // g_gxHostMutex for aurora_update(); holding it across this wait deadlocks both threads.
        ReleaseGxHostLockWhileWaiting unlocked;
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
    // One-time wait for the host main thread to actually create Aurora's window/surface (see
    // g_windowReady's own comment above) -- only ever blocks on this frame's first call, in
    // practice the game's very first GX frame of the whole process. Bounded (5 s) so a genuinely
    // broken/headless environment still reaches the discard path below instead of hanging forever;
    // logs once either way so a real hang here is diagnosable rather than silent.
    if (!g_windowReady.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "re4_port: BeginGxFrame: waiting for Aurora window to open...\n");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!g_windowReady.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "re4_port: BeginGxFrame: %s\n",
                     g_windowReady.load(std::memory_order_acquire)
                         ? "window ready, proceeding"
                         : "timed out waiting for window -- proceeding anyway (this frame's GX "
                           "work will be safely discarded, not fatal, if the session still can't "
                           "open -- see aurora-patches/0007)");
    }

    // Acquired AFTER the window-ready wait above, never before: RunPresentLoop() (host thread) is
    // what actually creates the window during that wait window (its own aurora_update()/
    // aurora_initialize() calls), and holding g_gxHostMutex earlier would deadlock against
    // RunPresentLoop()'s own lock around aurora_update() below.
    t_gxHostLock.lock();
    if (g_shuttingDown.load(std::memory_order_relaxed)) {
        // RunPresentLoop() has started (or finished) tearing Aurora down -- see g_shuttingDown's own
        // comment. Never call into Aurora again once that has happened: this is exactly the race that
        // used to crash the game thread inside aurora_end_frame()/ImGui::Render() with the host main
        // thread concurrently inside aurora_shutdown() (docs/port-boot.md's New Game crash). The frame
        // is simply not presented -- correct the same way a real console dropping a field nobody is
        // watching is (BeginGxFrame()'s own no-session comment below, same policy).
        t_gxFrameActive = false;
        return;
    }
    re4_port::HostAllocScope hostAlloc;
    t_gxFrameActive = aurora_begin_frame();
    if (!t_gxFrameActive) {
        // Real hardware always has somewhere to render, so the vendor's own Render()/DrawOTag call
        // sites run unconditionally every frame regardless of what this returns (main.cpp's frame
        // loop, unchanged). When there genuinely is no presentable surface this frame (window
        // minimized/occluded, or -- see above -- still opening), aurora-patches/0007-gx-fifo-
        // discard-without-session.patch makes Aurora's own fifo::drain()/process_to() discard
        // whatever GX bytes this frame writes instead of forwarding them to the gfx recording layer
        // with no active session (which is what used to fatal here, docs/port-boot.md section 43).
        // This is a policy choice, not a workaround: a frame with no presentable target has no
        // correct place to put its draw output anyway (no offscreen target is maintained for this
        // case -- TO VERIFY whether the title screen ever needs one, e.g. for a screenshot taken
        // while occluded), so dropping it is the same "this field never got displayed" behavior a
        // real console has when nothing is watching the video output.
        std::fprintf(stderr, "re4_port: aurora_begin_frame() returned false -- this frame's GX "
                              "work will be discarded (no active recording session; window "
                              "minimized/GPU not ready)\n");
    }
}

void EndGxFrame()
{
    if (t_gxFrameActive) {
        re4_port::HostAllocScope hostAlloc; // see BeginGxFrame()
        aurora_end_frame();
        t_gxFrameActive = false;
    }
    // Release g_gxHostMutex unconditionally, matching BeginGxFrame()'s unconditional lock() above
    // (t_gxFrameActive only gates the aurora_end_frame() call itself, not the lock span -- a frame
    // discarded for lacking a session, section above, still held the lock the whole time).
    if (t_gxHostLock.owns_lock()) {
        t_gxHostLock.unlock();
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
    // main.cpp's vsync busy-wait calls this in a tight loop while the game thread may still hold
    // g_gxHostMutex from BeginGxFrame(); give the host main thread a window to run aurora_update()
    // and produce the tick the spin is waiting for, or neither thread ever advances.
    if (t_gxHostLock.owns_lock()) {
        ReleaseGxHostLockWhileWaiting unlocked;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
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
    g_windowReady.store(true, std::memory_order_release); // unblocks BeginGxFrame() on the game
                                                            // thread -- see its own comment
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
        {
            // See g_gxHostMutex's own comment (above BeginGxFrame()/EndGxFrame()): aurora_update()
            // can process a window event (resize) that synchronizes with and clears Aurora's
            // internal caches -- must not run concurrently with the game thread's active GX-frame
            // span. Scoped to just this call (not the whole iteration, i.e. not held across the
            // pacing sleep below) so the host thread never holds it longer than Aurora's own entry
            // point actually needs.
            std::lock_guard<std::mutex> gxHostLock(g_gxHostMutex);
            const AuroraEvent* event = aurora_update();
            while (event != nullptr && event->type != AURORA_NONE) {
                if (event->type == AURORA_EXIT) {
                    std::fprintf(stderr, "re4_port: RunPresentLoop: aurora_update() reported "
                                          "AURORA_EXIT -- host main thread will now tear Aurora "
                                          "down\n");
                    exiting = true;
                }
                ++event;
            }
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

    // Invariant: no aurora_* entry point may ever run concurrently with aurora_shutdown() -- the game
    // thread's BeginGxFrame()/EndGxFrame() span (src/game/main_sub.cpp's Render_before()/Render_swap())
    // is the only other caller of one, and it serializes against this loop's aurora_update() through
    // g_gxHostMutex already, but shutdown used to run with no such guard at all: this loop could reach
    // here and call aurora_shutdown() while the game thread was already inside aurora_end_frame() (a
    // real, reproduced crash -- host main thread inside aurora::gfx::render_worker::synchronize() from
    // aurora_shutdown(), game thread inside ImGui::Render() from aurora_end_frame(), null ImGui context
    // torn down mid-frame; New Game's title->game transition changes render state often enough to make
    // the timing window easy to hit interactively, but the race exists on any frame boundary, not just
    // that transition). Fixed with two parts: g_shuttingDown (set here, checked by BeginGxFrame() while
    // it holds g_gxHostMutex -- see that flag's own comment for why the mutex hand-off makes this safe
    // without extra synchronization) makes every BeginGxFrame() from this point on a no-op instead of a
    // real Aurora call; taking g_gxHostMutex here, AFTER setting the flag, blocks until any frame that
    // was already genuinely in flight (started before the flag existed) finishes its own EndGxFrame()
    // and releases the lock, so aurora_shutdown() below never overlaps a real aurora_end_frame() call.
    g_shuttingDown.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> gxHostLock(g_gxHostMutex);
        aurora_shutdown();
    }
}

} // namespace re4_port
