// Host test for src/port/arena.cpp (Phase 2 step 3, docs/port-phase2.md). Two things it needs to
// prove, for different reasons:
//  - InitArena() succeeds and g_base ends up sane in a single run (the ctest below).
//  - The fixed address stays free across many independent fresh process launches, since that is
//    what "safe to hard-code" actually means on a system with ASLR and a shared dyld cache
//    (tests/port/run_arena_stress.sh runs this binary's --once mode 100 times in a loop and checks
//    every exit code; not wired as a ctest by default since it is a 100x-slower stress run, not a
//    unit test).
#include "port/arena.h"
#include "port/ptr32.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    bool once = argc > 1 && std::strcmp(argv[1], "--once") == 0;

    re4_port::InitArena();
    if (re4_port::g_base == 0) {
        std::fprintf(stderr, "test_arena: g_base still 0 after InitArena()\n");
        return 1;
    }
    std::printf("test_arena: InitArena ok, g_base=0x%llx\n",
                static_cast<unsigned long long>(re4_port::g_base));

    if (once) {
        // The stress-test mode: one InitArena() per process, matching how the real game will call
        // it exactly once at startup.
        return 0;
    }

    // Re-entry: InitArena()/ShutdownArena() a few more times in the same process (not something the
    // real game does, but cheap insurance that ShutdownArena() actually frees the region).
    for (int i = 0; i < 5; i++) {
        re4_port::ShutdownArena();
        if (re4_port::g_base != 0) {
            std::fprintf(stderr, "test_arena: g_base not reset after ShutdownArena()\n");
            return 1;
        }
        re4_port::InitArena();
        if (re4_port::g_base == 0) {
            std::fprintf(stderr, "test_arena: re-InitArena() (iteration %d) left g_base at 0\n", i);
            return 1;
        }
    }

    std::printf("test_arena: OK\n");
    return 0;
}
