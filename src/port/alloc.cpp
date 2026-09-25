// See include/port/alloc.h for the problem this solves and the rules it implements.
#ifndef TARGET_PC
#error "src/port/alloc.cpp is host-only (TARGET_PC)"
#endif

#include "port/alloc.h"
#include "port/arena.h"

#include <atomic>
#include <cstdint>

namespace re4_port {

namespace {
thread_local bool t_isGameThread = false;
thread_local int t_hostAllocDepth = 0;
std::atomic<bool> g_heapsReady{false};
} // namespace

void MarkCurrentThreadGame()
{
    t_isGameThread = true;
}

bool IsGameThread()
{
    return t_isGameThread;
}

void MarkHeapsReady()
{
    g_heapsReady.store(true, std::memory_order_release);
}

bool HeapsReady()
{
    return g_heapsReady.load(std::memory_order_acquire);
}

bool ShouldUseGameHeap()
{
    return IsGameThread() && HeapsReady() && t_hostAllocDepth == 0;
}

bool ShouldUseGameHeapFor(const void* callerAddr)
{
    return ShouldUseGameHeap() && IsGameCodeAddress(callerAddr);
}

HostAllocScope::HostAllocScope()
{
    ++t_hostAllocDepth;
}

HostAllocScope::~HostAllocScope()
{
    --t_hostAllocDepth;
}

bool InHostAllocScope()
{
    return t_hostAllocDepth != 0;
}

bool IsArenaPointer(const void* p)
{
    if (p == nullptr) {
        return false;
    }
    auto addr = reinterpret_cast<std::uintptr_t>(p);
    auto base = reinterpret_cast<std::uintptr_t>(GetArenaBase());
    return addr >= base && addr - base < GetArenaSize();
}

// ld64 (macOS's linker) synthesizes these two pseudo-symbols for any named section that actually
// exists in the final image -- "section$start$<seg>$<sect>" / "section$end$<seg>$<sect>" -- giving
// the section's bounds without a custom linker script. Declared here (not a public dolphin/Aurora
// header) because nothing else needs them; __asm__ binds the C++ name to the literal linker symbol.
// `weak`: this file (src/port/alloc.cpp) is part of the `re4_port` static library, linked into
// several targets (test_arena, test_ptr32, ...) that never compile anything with
// include/port/game_section.h's pragma -- i.e. no `__TEXT,__re4game` section ever exists in those
// binaries. Verified live: when the section is absent, ld64 still resolves a *weak* start/end pair
// (to the same address as each other, not to a link error), which makes `start == end` -- an empty
// range that correctly never matches any address, exactly the right answer for "no game code was
// ever built into this binary" without a separate ifdef per target.
extern "C" char __attribute__((weak)) re4game_section_start[] __asm__(
    "section$start$__TEXT$__re4game");
extern "C" char __attribute__((weak)) re4game_section_end[] __asm__(
    "section$end$__TEXT$__re4game");

bool IsGameCodeAddress(const void* addr)
{
    auto a = reinterpret_cast<std::uintptr_t>(addr);
    auto start = reinterpret_cast<std::uintptr_t>(re4game_section_start);
    auto end = reinterpret_cast<std::uintptr_t>(re4game_section_end);
    return a >= start && a < end;
}

} // namespace re4_port
