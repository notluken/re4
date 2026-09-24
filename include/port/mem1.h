// Makes Aurora's MEM1 emulation *be* re4_port's own embedded arena (docs/port-boot.md, "Boot
// progress" -- the user decision this implements: "our embedded arena is the single owner of the
// GameCube address space").
//
// Aurora's own OS layer (lib/dolphin/os/OSArena.cpp -- OSGetArenaLo/Hi, OSSetArenaLo/Hi,
// OSAllocFromArenaLo/Hi -- and OSAlloc.cpp's OSInitAlloc/OSCreateHeap/OSAllocFromHeap) is a real
// implementation, not a stub, and this repo links it (the "don't stub what Aurora already
// implements" rule). It works over real host pointers (no relation to the game's own 32-bit
// address space) and gates its bounds checks (OSSetArenaLo/Hi's asserts, OSPhysicalToCached's) on
// two globals -- `MEM1Start`/`MEM1End` -- that only Aurora's own `AuroraOSInitMemory()` sets, and
// only when `aurora::g_config.mem1Size > 0` (never set by this repo, so those bounds default to
// null and every OSSetArenaLo/Hi call fails its own assert).
//
// Rather than letting Aurora allocate its own MEM1 block (a plain `calloc()` on this host, per
// lib/dolphin/os/OSMemory.cpp's non-Windows branch -- unrelated to, and potentially overlapping,
// this repo's own arena), this file points MEM1Start/MEM1End/OSBaseAddress and
// aurora::g_config.mem1Size directly at re4_port's arena (include/port/arena.h) instead. No Aurora
// source patch needed: `MEM1Start`/`MEM1End` (lib/dolphin/os/OSMemory.cpp) and `aurora::g_config`
// (lib/aurora.cpp) are ordinary global-linkage symbols (not `static`, not exported via a public
// header either, but linkable all the same -- this file's .cpp declares `extern` matching
// declarations for them); `OSBaseAddress` is already `extern`-declared in the public
// `dolphin/os.h`. This is the "via its config/hook if one exists" branch the design decision
// asked for -- no tools/port/aurora-patches/ file needed.
//
// The result: MEM1Start == re4_port::GetArenaBase() (the same host pointer InitArena() computes
// g_base from), so Aurora's `OSCachedToPhysical(p) = p - MEM1Start` and re4_port's own
// `GC32(p) = p - g_base` agree up to the fixed 0x80000000 GameCube virtual-vs-physical offset
// (`GC32(p) == OSCachedToPhysical(p) + 0x80000000`) -- one arena, one address-space owner, exactly
// as required.
#ifndef TARGET_PC
#error "include/port/mem1.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_MEM1_H
#define RE4_PORT_MEM1_H

namespace re4_port {

// Call once, after InitArena() and before any code that touches OSGetArenaLo/Hi, OSInitAlloc,
// OSCreateHeap, or OSPhysicalToCached/OSCachedToPhysical (i.e. before CreateArenaThread's start
// function runs OSInit()/SystemMemInit()).
void InitMem1();

} // namespace re4_port

#endif // RE4_PORT_MEM1_H
