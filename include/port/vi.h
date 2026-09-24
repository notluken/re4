// Real VI (video interface) presentation for re4_boot: frame-presentation entry points Aurora's own
// aurora_vi library declares (<dolphin/vi.h>) but does not implement (docs/port-boot.md section 26's
// GX/VI parity survey) -- VIWaitForRetrace, VISetPreRetraceCallback/VISetPostRetraceCallback,
// VISetNextFrameBuffer, VISetBlack, VIGetNextField, VIGetRetraceCount.
//
// Threading design (docs/port-boot.md's "first visible output" milestone):
//
// macOS requires the window/event loop (SDL's underlying NSApplication run loop, driven here by
// aurora_update()/aurora_begin_frame()/aurora_end_frame(), lib/main.cpp's own examples/simple.c
// shows the expected call shape) to run on the process's real main thread. This repo's game code
// runs on a separate thread instead (src/port/boot_main.cpp's CreateArenaThread, so every
// game-visible pointer stays inside the compressed-handle arena). So:
//
//   - The host **main thread** owns aurora_initialize()/aurora_update()/aurora_begin_frame()/
//     aurora_end_frame() -- re4_port::RunPresentLoop(), called from boot_main.cpp's main() after
//     starting the game thread. With AuroraConfig::vsync = true, aurora_end_frame() itself blocks
//     until the real display's next vsync (Aurora's swapchain present call) -- that is this port's
//     ~60 Hz tick source, not a manual sleep.
//   - Each iteration of that loop is one "retrace": RunPresentLoop() bumps an atomic retrace
//     counter and invokes whichever pre/post-retrace callback the game registered (VISetPre/
//     PostRetraceCallback), **on the main thread**, not the game thread. This matches real VI
//     hardware more closely than "on the game thread" would: a real retrace interrupt preempts
//     whatever the CPU is doing (here, the game thread's own `while (vsync_cnt < ...) {}` busy-wait,
//     main.cpp) rather than waiting for it to reach a scheduling point. Chosen deliberately over
//     "invoke the callback from the game thread the next time it calls something VI-ish", which
//     cannot advance vsync_cnt while the game thread is itself spinning on that same variable, per
//     the busy-wait's own text (verified by reading main.cpp before choosing this design, not
//     assumed). TO VERIFY / open risk: the callback (postVSyncCallback, main.cpp) touches game
//     globals (`pG`, `vsync_cnt`) with no lock, so this is a genuine cross-thread data race --
//     accepted for this milestone on the same grounds real VI hardware's own ISR would touch the
//     same variables from an interrupt context with no lock either; a future pass may want to
//     route this through the cooperative OSThread token instead if it proves unstable in practice.
//   - VIWaitForRetrace() (called directly by the game thread, e.g. Render_init()'s boot-time call,
//     src/game/main_sub.cpp) blocks the **calling** thread on a condition variable until the next
//     tick RunPresentLoop() produces.
//   - GX submission itself (Render()/Render_swap()/GXCopyDisp, still on the game thread, unchanged)
//     is **not** synchronized with aurora_begin_frame()/aurora_end_frame() by this file -- a real,
//     not-yet-verified risk (Aurora's GX/Dawn command encoding may assume single-threaded use inside
//     one begin/end pair) flagged here rather than guessed at; not reached in this session's actual
//     run (see docs/port-boot.md for the current blocker upstream of any GX submission).
#ifndef TARGET_PC
#error "include/port/vi.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_VI_H
#define RE4_PORT_VI_H

#include <atomic>

namespace re4_port {

// Runs Aurora's window/event/present pump on the calling thread until Aurora reports AURORA_EXIT or
// `*shouldExit` becomes true (checked once per iteration -- e.g. the game thread finishing). Must be
// called from the host process's real main thread. Calls aurora_initialize() itself (so the window
// is created here, not in boot_main.cpp) and aurora_shutdown() before returning.
void RunPresentLoop(const char* appName, std::atomic<bool>* shouldExit);

// Brackets one GX "recording session" (aurora_begin_frame()/aurora_end_frame()) around the game
// thread's own real GX submission for a frame -- called from src/game/main_sub.cpp's
// Render_before()/Render_swap() (TARGET_PC branch), NOT from RunPresentLoop(). Found necessary
// live: Aurora's GX command recorder (`g_recorder`, ../aurora/lib/gfx/recording.cpp) is a plain,
// non-thread-local global with no "active session" outside a begin_frame()/end_frame() pair, and it
// asserts ("No active recording session") the moment any real GX draw command runs without one --
// which happens on the game thread (src/game/libgpu.cpp's DrawOTag and friends), not the host main
// thread RunPresentLoop() runs on. BeginGxFrame()/EndGxFrame() must therefore run on the same
// thread as the GX submission they bracket (the game thread), while RunPresentLoop() keeps owning
// only the window/event pump and the ~60 Hz retrace tick on the host main thread.
void BeginGxFrame();
void EndGxFrame();

} // namespace re4_port

#endif // RE4_PORT_VI_H
