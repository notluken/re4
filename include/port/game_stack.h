// A dedicated ~2 MiB zerofill region for the main game thread's real (pthread/machine) stack,
// placed in the exe's own __DATA segment, *before* the 1 GiB arena (docs/port-boot.md section 28,
// plan "A*"'s "game thread stack" decision).
//
// Why not just carve it from the arena the way src/port/arena.cpp's CreateArenaThread already can:
// `SystemMemInit()` (src/game/main_mem.cpp) calls `OSSetArenaLo(arenaBase)` (src/port/mem1.cpp) so
// the game's own heap allocator (`OSAllocFromHeap`/`MEM_ALLOC`) is free to use the *entire* 1 GiB
// arena, starting from offset 0 -- the same bytes a `CreateArenaThread(0, ...)` call would have
// carved the game thread's own machine stack from. Reusing arena offset 0 for the running thread's
// stack while the heap allocator is also configured to start handing out memory from that exact
// offset is a real, live collision (the heap would eventually allocate over the executing thread's
// own stack) -- this region exists so the arena is available to the heap allocator, undivided, with
// no reserved-for-the-stack carve-out inside it at all.
#ifndef TARGET_PC
#error "include/port/game_stack.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_GAME_STACK_H
#define RE4_PORT_GAME_STACK_H

#include <cstddef>

namespace re4_port {

inline constexpr std::size_t kGameStackSize = 2 * 1024 * 1024; // 2 MiB

// [GetGameStackBase(), GetGameStackBase() + GetGameStackSize()) -- the lowest address first (matches
// pthread_attr_setstack's own convention, and src/port/arena.cpp's CreateThreadOnStack()).
void* GetGameStackBase();
std::size_t GetGameStackSize();

} // namespace re4_port

#endif // RE4_PORT_GAME_STACK_H
