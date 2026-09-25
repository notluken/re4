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

} // namespace re4_port
