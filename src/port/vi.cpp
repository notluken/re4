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
#include <cstdio>
#include <mutex>
#include <condition_variable>

namespace {

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

void RunPresentLoop(const char* appName, std::atomic<bool>* shouldExit)
{
    AuroraConfig config{};
    config.appName = appName;
    config.vsync = true; // paces this loop off the real display refresh (aurora_end_frame()
                          // blocks on the swapchain present) instead of a manual sleep.
    config.mem1Size = 0; // re4_port::InitMem1() (src/game/main.cpp's TARGET_PC OSInit() branch)
    config.mem2Size = 0; // already owns MEM1/ARAM sizing -- do not let Aurora allocate its own.

    aurora_initialize(0, nullptr, &config);
    std::fprintf(stderr, "re4_boot: Aurora window opened\n");

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

        if (aurora_begin_frame()) {
            aurora_end_frame();
        }

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
    }

    aurora_shutdown();
}

} // namespace re4_port
