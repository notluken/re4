// See include/port/game_stack.h. Listed right after src/port/lowmem.cpp in every executable target
// that needs it (CMakeLists.txt) -- zerofill, like src/port/arena.cpp's own `__re4arena`, so it costs
// no file size; placed between `__re4low` and the exe's ordinary `__data`/`__bss`/`__common` by link
// order the same way arena.cpp's section is placed last.
#ifndef TARGET_PC
#error "src/port/game_stack.cpp is host-only (TARGET_PC)"
#endif

#include "port/game_stack.h"

namespace re4_port {

namespace {
__attribute__((aligned(16384),
               section("__DATA,__re4stack,zerofill"))) char s_gameStack[kGameStackSize];
} // namespace

void* GetGameStackBase()
{
    return s_gameStack;
}

std::size_t GetGameStackSize()
{
    return kGameStackSize;
}

} // namespace re4_port
