// re4_boot's host entry point (docs/port.md, "First boot" milestone, step (d)).
//
// Not the game's own main() (src/game/main.cpp) -- this is the *host process* entry point. It
// initializes the compressed-handle arena (docs/port-phase2.md strategy D), then runs the game's
// real main() on a thread whose stack is carved from the arena (CreateArenaThread), since no
// game-visible pointer may point at the host main thread's own stack (include/port/arena.h's rule).
#ifdef TARGET_PC

#include "port/arena.h"
#include "port/dvd.h"
#include "port/dvd_root.h"
#include "port/game_stack.h"
#include "port/os_thread.h"
#include "port/ptr32.h"
#include "port/rel.h"
#include "port/vi.h"

#include <cstdio>

int main_game(); // src/game/main.cpp's `main()`, renamed at link time (see CMakeLists.txt:
                  // re4_boot can't have two `main`s, and the vendor's own main() has the
                  // real boot sequence in it unchanged -- renaming the *symbol*, not the
                  // source, is done via `-Dmain=main_game` on this one translation unit's
                  // compile of src/game/main.cpp, not a source edit). NOT `extern "C"`: the
                  // special "no mangling" treatment C++ gives a function literally spelled
                  // `main` does not carry over once the preprocessor has already renamed it
                  // to `main_game` before Sema ever sees it -- it mangles as an ordinary C++
                  // global function (`_Z9main_gamev`, verified with `nm` on the .o), so this
                  // declaration must use plain C++ linkage to match, not `extern "C"`.

#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::atomic<bool> g_gameThreadDone{false};

void* GameThreadEntry(void*)
{
    // Registers this thread as the "main" OSThread identity for src/game/scheduler.cpp's task
    // scheduler (include/port/os_thread.h) -- must happen before systemStartInit() reaches
    // TaskSchedulerInit()/the first TaskScheduler() call inside main_game()'s frame loop.
    re4_port::MarkCurrentThreadAsMainOSThread();
    main_game(); // never expected to return in practice (src/game/main.cpp's main() is an infinite
                 // frame loop) -- this is just the "if it somehow does" case.
    std::fprintf(stderr, "re4_port: GameThreadEntry: main_game() RETURNED -- this should never "
                          "happen in practice; RunPresentLoop() will now tear Aurora down\n");
    g_gameThreadDone.store(true);
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    re4_port::InitDvdRoot(argc, argv); // argv[1], else $RE4_DVD_ROOT, else orig/G4BE08/files --
                                        // logged only; not the active DVD backend (see InitDvd)
    std::fprintf(stderr, "re4_boot: DVD root=%s\n", re4_port::GetDvdRoot());
    re4_port::InitDvd(argc, argv); // argv[2], else $RE4_DISC, else orig/G4BE08/re4_debug_disc1.iso
                                    // -- aurora_dvd_open() on the real disc image (must run before
                                    // the game thread's first DVDOpen/DVDRead)
    re4_port::InitArena(); // aborts internally on failure (see include/port/arena.h)
    // re4_port::InitMem1() (include/port/mem1.h) is NOT called here: OSInit() (src/game/main.cpp,
    // TARGET_PC branch) calls it itself, right after Aurora's own OSInit() has run once with
    // mem1Size still 0 -- calling it earlier, before that first (no-op) OSInit() pass, would set
    // mem1Size nonzero too soon and make OSInit()'s internal AuroraOSInitMemory() call allocate
    // its own MEM1 block on top of what this would have set up.
    std::fprintf(stderr, "re4_boot: arena base=%p size=%zu\n", re4_port::GetArenaBase(),
                 re4_port::GetArenaSize());
    re4_port::RegisterBuiltRelModules(); // include/port/rel.h -- before the game thread starts, so
                                          // every id it might OSLink() is already registered

    // The game thread's real (machine) stack is include/port/game_stack.h's dedicated ~2 MiB
    // pre-arena region, not carved from the 1 GiB arena (docs/port-boot.md section 28) -- so the
    // arena is available to SystemMemInit()'s heap allocator (OSSetArenaLo(arenaBase),
    // src/port/mem1.cpp) in full, with no reserved-for-the-stack carve-out inside it that the heap
    // could otherwise grow into while this thread is still running on it.
    // CreateThreadOnStack is detached (include/port/arena.h) -- no join primitive is exposed yet.
    if (!re4_port::CreateThreadOnStack(re4_port::GetGameStackBase(), re4_port::GetGameStackSize(),
                                        GameThreadEntry, nullptr)) {
        std::fprintf(stderr, "re4_boot: CreateThreadOnStack (game thread) failed\n");
        return 1;
    }
    // The host process's real main thread owns Aurora's window/event/present loop (AppKit requires
    // this on macOS) -- see include/port/vi.h for the full design and why VI's retrace callback
    // runs on this thread, not the game thread. Returns once aurora_update() reports AURORA_EXIT or
    // the game thread finishes.
    re4_port::RunPresentLoop("re4", &g_gameThreadDone);
    return 0;
}

#endif // TARGET_PC
