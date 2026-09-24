// GC-faithful low memory (docs/port-boot.md section 28, plan "A*"): a small, NON-zerofill 16 KiB
// region placed first in the exe's __DATA segment (`__DATA,__re4low`, src/port/lowmem.cpp), used as
// the anchor for re4_port::g_base (include/port/ptr32.h) instead of the 1 GiB arena.
//
// Why: real GameCube low memory (OSBootInfo, the exception vectors, `__OSBusClock` at
// OS_BASE_CACHED|0xF8, ...) sits at GC address 0x80000000, *below* every other global the game or
// SDK ever declares. Anchoring g_base at the arena (as this port did before this section) put every
// ordinary global (main.cpp's `MainOt`, every other plain `static`/file-scope variable in the whole
// tree) at GC addresses *below* 0x80000000 -- GC32() of one of those mathematically underflows (or
// hits its own "outside the window" assert), which is exactly why `DrawOTag`'s OT pointer-in-u32
// trick (docs/port-boot.md section 27) produced garbage: `MainOt`'s own address was never
// representable as a GC handle at all. Anchoring g_base at this file's `s_lowmem` instead makes
// every ordinary global's GC32() address a plausible, positive 0x80xxxxxx value (bit 31 set,
// matching real GC RAM), the same way it already worked for arena-allocated (heap) data.
#ifndef TARGET_PC
#error "include/port/lowmem.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_LOWMEM_H
#define RE4_PORT_LOWMEM_H

#include <cstddef>

namespace re4_port {

inline constexpr std::size_t kLowMemSize = 16 * 1024;

// [GetLowMemBase(), GetLowMemBase() + GetLowMemSize()) -- GC address 0x80000000 upward. Must be
// called after nothing (this is static storage, valid from process start); InitArena() reads it to
// compute g_base.
void* GetLowMemBase();
std::size_t GetLowMemSize();

} // namespace re4_port

#endif // RE4_PORT_LOWMEM_H
