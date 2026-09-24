// Phase 2 step 3 (docs/port-phase2.md): reserve the fixed 4 GiB host arena strategy D's compressed
// handles (include/port/ptr32.h) depend on, and set re4_port::g_base from it.
//
// macOS arm64 facts this is built on (docs/port-phase2.md, "macOS facts"): any __PAGEZERO smaller
// than 4 GiB is killed outright (rc=137), plain mmap()/malloc() never return an address below 4 GiB,
// and there is no MAP_32BIT. The observed free window is ~2.5 GiB-6.5 GiB (above the 4 GiB
// __PAGEZERO, below the dyld shared cache at ~6.5 GiB); the exe image and main stack land inside it
// reliably.
//
// VM_FLAGS_FIXED *without* VM_FLAGS_OVERWRITE (what step 3 first tried, following the plan literally)
// turned out unreliable on this host: mach_vm_allocate refuses if *any* byte of the range is already
// backed, and small ASLR-jittered allocator bookkeeping regions (MALLOC guard pages/metadata a few
// KiB to a few MiB in size, `vmmap` on a live process confirms) land inside [0x120000000,
// 0x160000000) often enough that a bare VM_FLAGS_FIXED request there failed 37/50 single-shot process
// runs, and even trying 12 candidate addresses 128 MiB apart across ~2.5 GiB of the window still
// failed 37/50 (their 1 GiB spans overlap too much to actually diversify away from one obstacle).
// VM_FLAGS_ANYWHERE doesn't help either: this host's generic allocator (both mach_vm_allocate
// ANYWHERE and plain mmap with a hint but no MAP_FIXED) picks between two placement modes that are
// decided once per process and apply to every such call in it, not per call, so a retry loop inside
// one process keeps hitting the same mode (measured 50/50 across 100 runs, 8 retries each, not the
// ~99.6% independent-retries would predict). VM_FLAGS_FIXED **with** VM_FLAGS_OVERWRITE (Mach's
// equivalent of mmap's MAP_FIXED, which unconditionally claims the range) measured 100/100 across
// process runs, including a read/write touch of the freshly mapped page and the exe-image/stack
// window checks below; nothing meaningful is ever observed at this address this early in a fresh
// process (the only occupants found by `vmmap` were the odd small allocator metadata region or,
// once, an unused stack-guard placeholder), so overwriting it is safe in practice. TO VERIFY: whether
// this holds on other macOS versions/hardware -- these are single-host (this machine, this macOS
// build) measurements, not a documented kernel guarantee.

#ifndef TARGET_PC
#error "src/port/arena.cpp is host-only (TARGET_PC)"
#endif

#include "port/arena.h"
#include "port/ptr32.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace re4_port {

std::uintptr_t g_base = 0;

namespace {

// Default fixed address and size: mid-window (docs/port-phase2.md, "macOS facts") so both a 4 GiB
// compressed-handle span below it (unused today; g_base is set to this address itself, so the
// window starts here) and headroom above it for the exe image / stack stay inside
// [~0x0A0000000, ~0x1A0000000). RE4_ARENA_BASE / RE4_ARENA_SIZE let a test override either without
// touching source (the size default is 1 GiB, not the full 4 GiB window, since nothing needs more
// than that yet and a smaller reservation is less likely to collide with something else).
constexpr mach_vm_address_t kDefaultArenaBase = 0x120000000ULL;
constexpr mach_vm_size_t kDefaultArenaSize = 0x40000000ULL; // 1 GiB

mach_vm_address_t ArenaBaseFromEnv()
{
    if (const char* s = std::getenv("RE4_ARENA_BASE")) {
        return static_cast<mach_vm_address_t>(std::strtoull(s, nullptr, 0));
    }
    return kDefaultArenaBase;
}

mach_vm_size_t ArenaSizeFromEnv()
{
    if (const char* s = std::getenv("RE4_ARENA_SIZE")) {
        return static_cast<mach_vm_size_t>(std::strtoull(s, nullptr, 0));
    }
    return kDefaultArenaSize;
}

// A local variable's address as a coarse "is this in the window" proxy for the main stack; a
// function-static variable's address as the same for the exe image (.bss, in this TU). Once Phase 4
// links the actual game (main.cpp's Global etc.) into a host executable, the real thing can be
// checked the same way instead.
static int s_bssProbe;

void CheckInWindow(const char* what, const void* addr, std::uintptr_t base, mach_vm_size_t size)
{
    (void) size; // the window checked is the full 4 GiB compressed-handle range, not just the arena
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
    if (a < base) {
        std::fprintf(stderr, "InitArena: %s (%p) is below the arena base (0x%llx)\n", what, addr,
                     static_cast<unsigned long long>(base));
        std::abort();
    }
    if (a - base >= kWindowSize) {
        std::fprintf(stderr,
                     "InitArena: %s (%p) is outside the 4 GiB compressed-handle window starting at "
                     "0x%llx\n",
                     what, addr, static_cast<unsigned long long>(base));
        std::abort();
    }
}

} // namespace

// Reserves the fixed arena and sets g_base. Aborts loudly (not a silent fallback) if the address is
// already taken or if the exe image / current stack turn out to be outside the resulting window,
// since either means every Ptr32<T> in the process would be silently wrong.
void InitArena()
{
    mach_vm_address_t base = ArenaBaseFromEnv();
    mach_vm_size_t size = ArenaSizeFromEnv();

    mach_vm_address_t addr = base;
    kern_return_t kr =
        mach_vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr,
                     "InitArena: mach_vm_allocate(0x%llx, 0x%llx, VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE) "
                     "failed: %s (%d); see docs/port-phase2.md \"macOS facts\"\n",
                     static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
                     mach_error_string(kr), kr);
        std::abort();
    }
    if (addr != base) {
        // VM_FLAGS_FIXED should never relocate the request; if it did, something is wrong enough
        // that continuing is worse than aborting.
        std::fprintf(stderr, "InitArena: VM_FLAGS_FIXED returned 0x%llx, asked for 0x%llx\n",
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(base));
        std::abort();
    }

    g_base = static_cast<std::uintptr_t>(addr) - 0x80000000u;

    CheckInWindow("arena", reinterpret_cast<void*>(addr), g_base, size);
    CheckInWindow("exe image (.bss probe)", &s_bssProbe, g_base, size);
    int stackProbe;
    CheckInWindow("main stack", &stackProbe, g_base, size);
}

void ShutdownArena()
{
    if (g_base == 0) {
        return;
    }
    mach_vm_address_t addr = static_cast<mach_vm_address_t>(g_base) + 0x80000000u;
    mach_vm_deallocate(mach_task_self(), addr, ArenaSizeFromEnv());
    g_base = 0;
}

} // namespace re4_port
