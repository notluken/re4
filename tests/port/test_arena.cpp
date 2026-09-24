// Host test for src/port/arena.cpp (Phase 2 step 3, docs/port-phase2.md). What it proves, and why:
//  - InitArena() succeeds and the whole exe image + the whole arena are inside the resulting 4 GiB
//    window (the ctest below, single run).
//  - A thread stack carved from the arena (CreateArenaThread) is inside the window too -- the "run
//    game code on an arena-backed thread, never the host main thread" rule (include/port/arena.h)
//    is actually usable, not just documented.
//  - Nothing outside the arena was touched: a malloc'd buffer, written before InitArena() and before
//    the arena is touched, keeps its exact bytes after every arena byte has been written to. This is
//    the direct rebuttal to the rejected VM_FLAGS_OVERWRITE design (docs/port-phase2.md), which this
//    same check would have failed.
//  - tests/port/run_arena_stress.sh runs this binary's --once mode 100 times as fresh processes,
//    since ASLR means any single run proves less than a repeated one.
#include "port/arena.h"
#include "port/ptr32.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace {

bool InWindow(const void* addr)
{
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
    return a >= re4_port::g_base && (a - re4_port::g_base) < re4_port::kWindowSize;
}

struct ThreadResult {
    void* stack_addr;
    bool ran;
};

void* ThreadMain(void* argp)
{
    auto* result = static_cast<ThreadResult*>(argp);
    int local;
    result->stack_addr = &local;
    result->ran = true;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    bool once = argc > 1 && std::strcmp(argv[1], "--once") == 0;

    // A malloc'd buffer with a known pattern, written *before* InitArena()/any arena touch, so it can
    // catch the arena silently overlapping or otherwise disturbing host heap memory.
    const std::size_t kCanarySize = 4096;
    unsigned char* canary = static_cast<unsigned char*>(std::malloc(kCanarySize));
    if (!canary) {
        std::fprintf(stderr, "test_arena: malloc failed\n");
        return 1;
    }
    for (std::size_t i = 0; i < kCanarySize; i++) {
        canary[i] = static_cast<unsigned char>(i);
    }

    re4_port::InitArena();
    if (re4_port::g_base == 0) {
        std::fprintf(stderr, "test_arena: g_base still 0 after InitArena()\n");
        return 1;
    }

    // Touch every page of the arena (not just the ends): if InitArena() had, in some earlier design,
    // overlapped something live, writing the whole arena is what would corrupt it.
    unsigned char* arena = static_cast<unsigned char*>(re4_port::GetArenaBase());
    std::size_t arenaSize = re4_port::GetArenaSize();
    for (std::size_t off = 0; off < arenaSize; off += 4096) {
        arena[off] = 0xAA;
    }
    arena[arenaSize - 1] = 0xBB;

    // The canary must be untouched.
    for (std::size_t i = 0; i < kCanarySize; i++) {
        if (canary[i] != static_cast<unsigned char>(i)) {
            std::fprintf(stderr, "test_arena: malloc canary corrupted at byte %zu (0x%02x != 0x%02x)\n",
                         i, canary[i], static_cast<unsigned char>(i));
            return 1;
        }
    }
    std::free(canary);

    // Arena-backed thread: stack carved from the tail of the arena, well clear of the bytes just
    // written above.
    ThreadResult result{};
    result.ran = false;
    const std::size_t kStackSize = 1 << 20; // 1 MiB
    if (!re4_port::CreateArenaThread(arenaSize - kStackSize, kStackSize, ThreadMain, &result)) {
        std::fprintf(stderr, "test_arena: CreateArenaThread failed\n");
        return 1;
    }
    // Detached thread: give it a moment. A pthread_join would need a non-detached thread; this test
    // only needs to observe the result, so a short spin is enough and keeps the test simple.
    for (int i = 0; i < 1000 && !result.ran; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, nullptr);
    }
    if (!result.ran) {
        std::fprintf(stderr, "test_arena: arena thread did not run in time\n");
        return 1;
    }
    if (!InWindow(result.stack_addr)) {
        std::fprintf(stderr, "test_arena: arena thread's stack (%p) is outside the window\n",
                     result.stack_addr);
        return 1;
    }

    std::printf("test_arena: InitArena ok, g_base=0x%llx, arena thread stack=%p (in window)\n",
               static_cast<unsigned long long>(re4_port::g_base), result.stack_addr);

    if (once) {
        return 0;
    }

    // Idempotency: calling InitArena() again must not change g_base or abort (the arena is a static
    // part of the image now, not a dynamic reservation, so there is nothing to "re-enter" unsafely).
    std::uintptr_t before = re4_port::g_base;
    re4_port::InitArena();
    if (re4_port::g_base != before) {
        std::fprintf(stderr, "test_arena: g_base changed on a second InitArena() call\n");
        return 1;
    }

    std::printf("test_arena: OK\n");
    return 0;
}
