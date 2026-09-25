// Phase "first boot" (docs/port.md, docs/port-boot.md section 8): the split allocator that lets
// host libraries (Aurora, SDL, libc++'s own internals) and the ported game code share one process
// without either one touching the other's memory.
//
// The problem this exists to fix: src/game/main_mem.cpp's `operator new`/`operator delete`
// globally override the process-wide `::operator new`/`::operator delete` (C++ gives user code
// that right, and the original game relies on it -- every `new` in the game goes through its own
// tagged heap). On a hosted process that is now sharing the executable with Aurora and libc++,
// this means *any* allocation anywhere in the process -- including inside Aurora's own C++
// static initializers, which run before the game's heaps exist -- gets routed into the game's
// heap code. docs/port-boot.md section 8 has the resulting crash: a `std::vector<bool>` inside
// Aurora's `FrameSlotPool` global constructor called `::operator new`, which called into
// `mem_alloc`'s logging path, which dereferenced a game-global (`cLog`) that had not been
// constructed yet, because Aurora's static initializers run before the game's own `main()` (let
// alone `SystemMemInit()`) does.
//
// The fix is not "never let the game override operator new" (the game genuinely needs its own
// heap -- Phase 2's compressed-handle Ptr32<T> scheme requires every game-visible allocation to
// live inside the arena, docs/port-phase2.md) and not "always use the game heap" (nothing outside
// a game thread may touch it -- the heap code (`OSAlloc`-based) is not thread-safe, and it does
// not exist at all before `SystemMemInit()` runs). Instead: allocate from the game heap only when
// both (a) the calling thread is a thread this file's re4_port::MarkCurrentThreadGame() has
// marked as "the game runs here" (set once, inside re4_port::CreateArenaThread's trampoline) and
// (b) the game's heaps have been initialized (re4_port::MarkHeapsReady(), called at the end of
// SystemMemInit() under TARGET_PC); anywhere else, use the host's malloc. Freeing routes by
// *pointer identity*, not by thread or call site: everything the game heap ever hands out lives
// inside the embedded arena (include/port/arena.h) by construction (SystemMemMap's fixed
// 0x80xxxxxx-shaped addresses map into it), so `re4_port::IsArenaPointer(p)` alone tells you
// which allocator owns a given pointer, regardless of which thread is freeing it.
#ifndef TARGET_PC
#error "include/port/alloc.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_ALLOC_H
#define RE4_PORT_ALLOC_H

#include <cstddef>

namespace re4_port {

// Called once, from inside re4_port::CreateArenaThread's trampoline (src/port/arena.cpp), before
// the caller-supplied thread entry point runs. Not for use anywhere else.
void MarkCurrentThreadGame();

// True on a thread MarkCurrentThreadGame() has been called on.
bool IsGameThread();

// Called once, at the end of SystemMemInit() (src/game/main_mem.cpp, TARGET_PC branch), after
// Heap[0] exists and mem_calloc()/Mem_free() are safe to call.
void MarkHeapsReady();

// True once MarkHeapsReady() has run.
bool HeapsReady();

// True when a `new` on the current thread right now should come from the game heap (IsGameThread()
// && HeapsReady()) rather than the host's malloc.
bool ShouldUseGameHeap();

// True when `p` falls inside the embedded arena (include/port/arena.h) -- i.e. the game heap, not
// the host's malloc, owns it. nullptr is never an arena pointer.
bool IsArenaPointer(const void* p);

// Scoped override for ShouldUseGameHeap(), for host code the game thread calls *synchronously*
// in-line (Aurora's aurora_begin_frame()/aurora_end_frame(), src/port/vi.cpp) -- not a separate
// thread (IsGameThread() alone can't tell these apart: both run on the same, real game thread),
// so `new` inside that call has no other way to know it is host bookkeeping, not a game
// allocation. Nests (thread_local depth counter); host-heap `new` inside is always std::malloc
// regardless of IsGameThread()/HeapsReady(). Found live: Aurora's aurora::gfx::begin_frame()
// (renderer-internal command/resource pools, no GameCube equivalent) was routing into the game's
// fixed-size heaps every frame through the global operator new override, permanently consuming
// real GameCube-budgeted memory for host-GPU-backend bookkeeping that doesn't exist on real
// hardware -- exactly the "6.8 KB short" card-heap symptom (docs/port-boot.md).
class HostAllocScope {
public:
    HostAllocScope();
    ~HostAllocScope();
    HostAllocScope(const HostAllocScope&) = delete;
    HostAllocScope& operator=(const HostAllocScope&) = delete;
};

// True when the current thread is inside a HostAllocScope right now.
bool InHostAllocScope();

} // namespace re4_port

#endif // RE4_PORT_ALLOC_H
