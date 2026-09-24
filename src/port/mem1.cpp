// See include/port/mem1.h.
#ifndef TARGET_PC
#error "src/port/mem1.cpp is host-only (TARGET_PC)"
#endif

#include "port/mem1.h"
#include "port/arena.h"

#include <aurora/aurora.h> // AuroraConfig's real layout (a plain C-compatible struct -- the
                            // header has no `namespace aurora` at all, that wrapper only exists
                            // around `g_config` itself, in lib/aurora.cpp)
#include <cstdint>

// aurora::g_config (lib/aurora.cpp): ordinary external linkage, not declared in any public
// header -- redeclared here (matching name/type) to reach the real symbol in libaurora_core.a.
namespace aurora {
extern AuroraConfig g_config;
} // namespace aurora

// Aurora's own globals (lib/dolphin/os/OSMemory.cpp) -- see include/port/mem1.h for why these
// extern declarations, not a source patch, are the right hook. Not declared in any Aurora header;
// redeclaring them here (same names, same types, ordinary external linkage) is enough to link
// against the real symbols in libaurora_os.a.
extern void* MEM1Start;
extern void* MEM1End;
// OSBaseAddress is also `extern`-declared in the public <dolphin/os.h>; re-declaring it here would
// conflict if that header were included in this TU too, so this file deliberately does not include
// it and relies on this matching declaration instead.
extern std::uintptr_t OSBaseAddress;

namespace re4_port {

void InitMem1()
{
    void* base = GetArenaBase();
    std::size_t size = GetArenaSize();

    MEM1Start = base;
    MEM1End = static_cast<char*>(base) + size;
    OSBaseAddress = reinterpret_cast<std::uintptr_t>(base);
    aurora::g_config.mem1Size = static_cast<std::uint32_t>(size);

    // ARAM (mem2Size): unrelated to MEM1/GC32/GCPTR (lib/dolphin/AR.cpp's own emulation is a
    // separate, independently malloc'd buffer addressed by ARAM-relative offsets, never by a
    // real host or GC pointer) -- just needs to be nonzero before SndInit()'s ARInit() call
    // (src/game/snd.cpp) or ARAlloc() dereferences an unset AR_BlockLength. 16 MiB, matching
    // real GameCube hardware ARAM.
    aurora::g_config.mem2Size = 16u * 1024u * 1024u;
}

} // namespace re4_port
