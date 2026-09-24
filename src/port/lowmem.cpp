// See include/port/lowmem.h. Must be the FIRST object file listed in every executable/test target
// that links re4_port (CMakeLists.txt) -- ld orders same-segment sections by first appearance among
// the *directly listed* link inputs (confirmed by this port's own precedent, src/port/arena.cpp's
// `__re4arena` staying last by being inside the re4_port archive, linked after everything else), so
// this guarantees `__re4low` sits at the lowest address in `__DATA`, before the exe's own ordinary
// `__data`/`__bss`/`__common` and before `__re4arena`.
//
// Deliberately NOT `,zerofill`: a zerofill section's *placement* still respects link order the same
// way (arena.cpp relies on exactly that), but this file wants a small, unambiguous 16 KiB of real
// file content (all zero, but written, not implicit) so nothing about "is this the first section"
// depends on how the linker groups zerofill sections among themselves -- see arena.cpp's own comment
// for why 1 GiB of *this* (non-zerofill) would be a real problem; 16 KiB is not.
#ifndef TARGET_PC
#error "src/port/lowmem.cpp is host-only (TARGET_PC)"
#endif

#include "port/lowmem.h"

namespace re4_port {

namespace {
__attribute__((aligned(16384), section("__DATA,__re4low"))) unsigned char s_lowmem[kLowMemSize] = {
    0};
} // namespace

void* GetLowMemBase()
{
    return s_lowmem;
}

std::size_t GetLowMemSize()
{
    return kLowMemSize;
}

} // namespace re4_port
