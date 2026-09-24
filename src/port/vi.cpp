// See include/port/vi.h for the threading design this implements.
#ifndef TARGET_PC
#error "src/port/vi.cpp is host-only (TARGET_PC)"
#endif

#include "port/vi.h"

#include <dolphin/vi/vifuncs.h>
#include <dolphin/vi/vitypes.h>

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

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

} // namespace

extern "C" {

void VIWaitForRetrace(void)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    u32 start = g_retraceCount.load();
    g_cv.wait(lock, [start] { return g_retraceCount.load() != start; });
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
        aurora_end_frame();
        t_gxFrameActive = false;
    }
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

        // One "retrace": bump the counter and run the game's registered callbacks, on this thread
        // (see include/port/vi.h for why this is deliberately not the game thread).
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
        g_cv.notify_all();

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
