// Phase 2 step 3 (docs/port-phase2.md): the fixed host arena that gives include/port/ptr32.h's
// compressed handles a window to live in. Implementation: src/port/arena.cpp.
#ifndef TARGET_PC
#error "include/port/arena.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_ARENA_H
#define RE4_PORT_ARENA_H

namespace re4_port {

// Reserves the fixed 4 GiB-window arena and sets g_base (include/port/ptr32.h). Aborts (does not
// return) if the address is taken or if the current exe image / stack turn out to be outside the
// resulting window. RE4_ARENA_BASE / RE4_ARENA_SIZE environment variables override the defaults
// (hex or decimal, strtoull base 0) for testing.
void InitArena();

// Releases the arena and resets g_base to 0. Mainly for tests that call InitArena() more than once
// in the same process.
void ShutdownArena();

} // namespace re4_port

#endif // RE4_PORT_ARENA_H
