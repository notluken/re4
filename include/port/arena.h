// Phase 2 step 3 (docs/port-phase2.md): the fixed host arena that gives include/port/ptr32.h's
// compressed handles a window to live in. Implementation: src/port/arena.cpp.
//
// Rule (docs/port-phase2.md, "the host arena"): no game-visible pointer may ever point at host
// malloc()'d memory or at the host main thread's stack. Both are placed by the OS wherever it wants,
// with no guarantee of staying inside the compressed-handle window (and, before this file's second
// revision, a naive "reserve a fixed VM range" approach that assumed a stable address near them
// ended up silently clobbering live malloc bookkeeping instead -- see the "why VM_FLAGS_OVERWRITE
// was rejected" note in docs/port-phase2.md, never repeat that approach). The arena instead lives
// inside the executable's own image (a zerofill BSS-like section, ASLR-slides with the rest of the
// image, never overlaps anything else because dyld places it as part of loading the exe, before any
// of this code runs), so *only* memory carved from GetArena()/CreateArenaThread() below is ever
// legal to expose to the game as a Ptr32<T> or push onto a stack the game can see.
#ifndef TARGET_PC
#error "include/port/arena.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_ARENA_H
#define RE4_PORT_ARENA_H

#include <cstddef>

namespace re4_port {

// Total size of the embedded arena (1 GiB). A compile-time constant, not a runtime option: the
// arena is a static array in the exe image, not a dynamic reservation, so its size is fixed at link
// time.
inline constexpr std::size_t kArenaSize = std::size_t(1) << 30;

// Sets g_base (include/port/ptr32.h) from the embedded arena's own address, then asserts (aborts if
// not) that the whole exe image and the whole arena are inside the resulting 4 GiB window. Never
// allocates or maps anything -- the arena already exists as part of the exe image before main()
// runs; this only computes g_base and checks it. Idempotent: safe to call more than once.
void InitArena();

// The arena's bounds, for carving out sub-allocations (thread stacks, the eventual host heap
// backing Ptr32<T>-visible allocations). [GetArenaBase(), GetArenaBase() + GetArenaSize()).
void* GetArenaBase();
std::size_t GetArenaSize();

// Starts a detached pthread whose stack is carved from the arena (pthread_attr_setstack), not from
// the host's normal thread-stack allocator (which, like malloc, is not guaranteed to land inside the
// window). This is the only supported way to run game code: the real game main loop (Phase 4) must
// run this way, never on the process's own main thread. `stack_size` bytes are taken from the arena
// starting at `stack_offset`; the caller is responsible for not overlapping two stacks (no allocator
// here yet -- Phase 4's heap design decides that). Returns false (does not abort) if pthread creation
// itself fails; the arena/window invariants this file exists for are unaffected by that failure.
bool CreateArenaThread(std::size_t stack_offset, std::size_t stack_size, void* (*start)(void*),
                       void* arg);

// Real (machine) pthread stacks for the per-task fibers, pinned at fixed GC address 0x81800000 --
// past src/game/main_mem.cpp's `SysMem.heap_end` (0x817F4000), inside VALID_PTR's (include/main_mem.h)
// 0x80000000-0x82FFFFFF window. The old arena-end slot computed a GC32() handle near 0xBF000000,
// outside that window, so any fiber local's address (e.g. cLightInfo::init2()'s `&size`) failed
// VALID_PTR. macOS `pthread_attr_setstack` needs page-aligned, page-sized memory (docs/port-boot.md
// section 29). 1 MiB per slot.
inline constexpr std::size_t kTaskStackSlotSize = 1024 * 1024; // 1 MiB
inline constexpr std::size_t kTaskStackSlots = 18;             // include/scheduler.h's TASK_NUM
inline constexpr std::size_t kTaskStackPoolSize = kTaskStackSlotSize * kTaskStackSlots;

// [base, base + kTaskStackPoolSize) -- src/port/os_thread.cpp assigns one kTaskStackSlotSize slot
// per distinct OSThread* it ever sees (at most kTaskStackSlots, matching TASK_NUM).
void* GetTaskStackPoolBase();

// Starts a detached pthread whose real (machine) stack is exactly [stack_base, stack_base +
// stack_size) -- the general form CreateArenaThread() and src/port/boot_main.cpp (the main game
// thread, on include/port/game_stack.h's dedicated pre-arena region) and src/port/os_thread.cpp
// (OSCreateThread, on the game-provided/heap-allocated stack buffer scheduler.cpp already passes it)
// both build on. `stack_base` must be the LOWEST address of the stack (pthread_attr_setstack's own
// convention), not the top. Returns false (does not abort) on failure.
bool CreateThreadOnStack(void* stack_base, std::size_t stack_size, void* (*start)(void*), void* arg);

} // namespace re4_port

#endif // RE4_PORT_ARENA_H
