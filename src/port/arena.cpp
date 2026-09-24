// Phase 2 step 3 (docs/port-phase2.md): the fixed host arena strategy D's compressed handles
// (include/port/ptr32.h) depend on, and re4_port::g_base from it. See include/port/arena.h for the
// "never point at host malloc/main-stack memory" rule this file exists to make possible to follow.
//
// History (full details in docs/port-phase2.md, "the host arena"): the first version of this file
// used mach_vm_allocate(VM_FLAGS_FIXED) to reserve a 1 GiB region at a fixed address, matching the
// original plan. Plain VM_FLAGS_FIXED turned out unreliable (measured 37/50 single-shot failures --
// small ASLR-placed malloc bookkeeping regions land in the requested range often enough). Adding
// VM_FLAGS_OVERWRITE "fixed" the reliability number (100/100) but for the wrong reason: it does not
// check that the range is free, it unconditionally unmaps whatever was already there. In the 37/50
// runs where plain FIXED had refused, OVERWRITE was silently clobbering live malloc guard
// pages/metadata -- corrupting the allocator, to fail later, nondeterministically, in code that has
// nothing to do with the arena. That was never shippable and this file never ships that version
// again.
//
// This version reserves nothing at runtime. The arena is a static array placed in the executable's
// own image (an explicit zerofill Mach-O section, `__DATA,__re4arena`, `,zerofill` in the `section`
// attribute below -- costs no file size, like ordinary BSS, confirmed with `size -m`: a plain
// `static char[1<<30]` with no explicit section name also lands in zerofill `__DATA,__bss`
// automatically, but naming it without the `,zerofill` suffix does *not* stay zerofill, ballooning
// the binary to a full 1 GiB on disk -- tried, rejected, worth remembering). dyld places it as part
// of loading the exe, before any of this code runs, at whatever address ASLR slides the image to; it
// can never overlap malloc's or anything else's later allocations because it is not one.
#ifndef TARGET_PC
#error "src/port/arena.cpp is host-only (TARGET_PC)"
#endif

#include "port/alloc.h"
#include "port/arena.h"
#include "port/lowmem.h"
#include "port/ptr32.h"

#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace re4_port {

std::uintptr_t g_base = 0;

namespace {

__attribute__((aligned(16384), section("__DATA,__re4arena,zerofill"))) char s_arena[kArenaSize];

// GC-faithful layout budget (docs/port-boot.md section 28, plan "A*"): every ordinary exe global
// (this file's own `s_bssProbe` stands in for "the whole image's .data/.bss/.common", same
// precedent as before this section) must land between `__re4low` (GC 0x80000000) and this address --
// the fixed literal GC addresses several vendor units hardcode as raw pointers (DVD_BUFF
// 0x80350000, SND_DATA_TOP 0x80370000, the GX FIFO 0x803F0000, the XFBs at 0x80460000, the game heap
// 0x80974000-0x817F4000, ...) must fall *inside* `__re4arena`, not inside the exe's own ordinary
// data. Measured this pass: the exe's own .data/.bss/.common (excluding __re4low/__re4stack) is
// ~0x88000 -- comfortably under this budget; re-checked live below, not just assumed.
constexpr std::uint32_t kArenaBudgetGC = 0x80350000u;

void CheckInWindow(const char* what, const void* addr)
{
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
    if (a < g_base) {
        std::fprintf(stderr, "InitArena: %s (%p) is below g_base (0x%llx)\n", what, addr,
                     static_cast<unsigned long long>(g_base));
        std::abort();
    }
    if (a - g_base >= kWindowSize) {
        std::fprintf(stderr,
                     "InitArena: %s (%p) is outside the 4 GiB compressed-handle window starting at "
                     "0x%llx\n",
                     what, addr, static_cast<unsigned long long>(g_base));
        std::abort();
    }
}

// A .bss variable and a function, in this TU, as stand-ins for "the exe image has data/code here"
// until Phase 4 links the real game (main.cpp's Global etc.) and can check that directly instead.
int s_bssProbe;
void SomeFunctionProbe() {}

} // namespace

void InitArena()
{
    // g_base anchored at __re4low (src/port/lowmem.cpp), not the arena: real GameCube low memory
    // (OSBootInfo, __OSBusClock, ...) sits at GC 0x80000000, below every other global the game or
    // SDK ever declares -- anchoring here instead of at the arena is what makes GC32() of an
    // *ordinary* global (not heap/arena data) a valid, positive 0x80xxxxxx handle. See
    // include/port/lowmem.h for the full rationale (this is the direct fix for docs/port-boot.md
    // section 27's `DrawOTag` crash: `MainOt`, a plain global, was not representable as a GC handle
    // under the old arena-anchored g_base at all).
    g_base = reinterpret_cast<std::uintptr_t>(GetLowMemBase()) - 0x80000000u;

    CheckInWindow("lowmem start", GetLowMemBase());
    CheckInWindow("arena start", s_arena);
    CheckInWindow("arena end", s_arena + kArenaSize - 1);
    CheckInWindow("exe .bss probe", &s_bssProbe);
    CheckInWindow("exe code probe", reinterpret_cast<void*>(&SomeFunctionProbe));

    // Layout invariant #1: lowmem is genuinely first -- every ordinary global's GC32() address must
    // be >= 0x80000000 (lowmem's own GC address). If this fires, __re4low is not actually first in
    // link order (CMakeLists.txt's source ordering regressed) -- see include/port/lowmem.h.
    std::uint32_t bssGC = GC32(&s_bssProbe);
    if (bssGC < 0x80000000u) {
        std::fprintf(stderr,
                     "InitArena: exe .bss probe (GC 0x%08x) is BELOW lowmem (GC 0x80000000) -- "
                     "__re4low is not first in link order; check CMakeLists.txt's source ordering "
                     "(docs/port-boot.md section 28)\n",
                     bssGC);
        std::abort();
    }

    // Layout invariant #2: the arena starts inside the fixed-GC-address budget.
    std::uint32_t arenaGC = GC32(s_arena);
    if (arenaGC > kArenaBudgetGC) {
        std::fprintf(stderr,
                     "InitArena: arena start (GC 0x%08x) exceeds the 0x%08x budget for image data "
                     "before the arena (docs/port-boot.md section 28) -- a fixed-GC-address literal "
                     "(DVD_BUFF/SND_DATA_TOP/the GX FIFO/the XFBs/...) would land outside the arena\n",
                     arenaGC, kArenaBudgetGC);
        std::abort();
    }

    if (std::getenv("RE4_PORT_DEBUG_LAYOUT") != nullptr) {
        std::fprintf(stderr,
                     "re4_port: layout: lowmem=%p (GC 0x%08x) bss_probe=%p (GC 0x%08x) arena=%p (GC "
                     "0x%08x) arena_end=%p (GC 0x%08x)\n",
                     GetLowMemBase(), GC32(GetLowMemBase()), &s_bssProbe, bssGC, static_cast<void*>(s_arena),
                     arenaGC, static_cast<void*>(s_arena + kArenaSize), GC32(s_arena + kArenaSize));
    }
}

void* GetArenaBase()
{
    return s_arena;
}

std::size_t GetArenaSize()
{
    return kArenaSize;
}

namespace {

// CreateArenaThread's real entry point: marks the new thread as "the game runs here" (so
// include/port/alloc.h's split allocator routes its `new`s to the game heap once one exists)
// before handing off to the caller's own start function. Heap-allocated (not stack/arena -- it
// must outlive this function's own return, and it's tiny, one-shot, host malloc is fine for it),
// freed by the trampoline itself once the real start function is reached.
struct ThreadTrampolineArgs {
    void* (*start)(void*);
    void* arg;
};

void* ThreadTrampoline(void* p)
{
    ThreadTrampolineArgs* args = static_cast<ThreadTrampolineArgs*>(p);
    void* (*start)(void*) = args->start;
    void* arg = args->arg;
    delete args;
    MarkCurrentThreadGame();
    return start(arg);
}

} // namespace

bool CreateThreadOnStack(void* stack_base, std::size_t stack_size, void* (*start)(void*), void* arg)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    bool ok = pthread_attr_setstack(&attr, stack_base, stack_size) == 0;
    if (ok) {
        ok = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0;
    }
    pthread_t thread;
    ThreadTrampolineArgs* targs = ok ? new ThreadTrampolineArgs{start, arg} : nullptr;
    if (ok) {
        ok = pthread_create(&thread, &attr, ThreadTrampoline, targs) == 0;
        if (!ok) {
            delete targs;
        }
    }
    pthread_attr_destroy(&attr);
    return ok;
}

bool CreateArenaThread(std::size_t stack_offset, std::size_t stack_size, void* (*start)(void*),
                       void* arg)
{
    if (stack_offset + stack_size > kArenaSize) {
        std::fprintf(stderr, "CreateArenaThread: [0x%zx, 0x%zx) does not fit the %zu-byte arena\n",
                     stack_offset, stack_offset + stack_size, kArenaSize);
        return false;
    }
    return CreateThreadOnStack(s_arena + stack_offset, stack_size, start, arg);
}

} // namespace re4_port
