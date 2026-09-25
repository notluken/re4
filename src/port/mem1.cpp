// See include/port/mem1.h.
#ifndef TARGET_PC
#error "src/port/mem1.cpp is host-only (TARGET_PC)"
#endif

#include "port/mem1.h"
#include "port/arena.h"
#include "port/lowmem.h"

#include <aurora/aurora.h> // AuroraConfig's real layout (a plain C-compatible struct -- the
                            // header has no `namespace aurora` at all, that wrapper only exists
                            // around `g_config` itself, in lib/aurora.cpp)
#include <dolphin/os.h>     // OSBaseAddress (extern, real declaration), OSSetArenaLo/Hi,
                            // OSBootInfo (this repo's own copy -- byte-identical to Aurora's,
                            // docs/port-boot.md section 14)
#include <cstddef>
#include <cstdint>
#include <cstring>

// aurora::g_config (lib/aurora.cpp): ordinary external linkage, not declared in any public
// header -- redeclared here (matching name/type) to reach the real symbol in libaurora_core.a.
namespace aurora {
extern AuroraConfig g_config;
} // namespace aurora

// Aurora's own globals (lib/dolphin/os/OSMemory.cpp) -- see include/port/mem1.h for why these
// extern declarations, not a source patch, are the right hook. Not declared in any Aurora header;
// redeclaring them here (same names, same types, ordinary external linkage) is enough to link
// against the real symbols in libaurora_os.a. This repo's own <dolphin/os.h> (included above, for
// OSSetArenaLo/Hi and OSBootInfo -- real hardware declarations, byte-identical to the vendor's own
// header) does NOT declare OSBaseAddress at all (a host/Aurora-only concept, real hardware has no
// such global) -- so it still needs its own manual redeclaration here, same as before this section.
extern void* MEM1Start;
extern void* MEM1End;
extern std::uintptr_t OSBaseAddress;

namespace re4_port {

void InitMem1()
{
    // GC-faithful layout (docs/port-boot.md section 28, plan "A*"): MEM1 now spans from __re4low
    // (GC 0x80000000 -- real GameCube low memory, OSBootInfo/the exception vectors/__OSBusClock)
    // through the end of the 1 GiB arena, not just the arena alone -- this is what lets every
    // ordinary exe global (not just arena/heap data) have a valid GC32() address; see
    // include/port/lowmem.h.
    void* low = GetLowMemBase();
    void* arenaBase = GetArenaBase();
    std::size_t arenaSize = GetArenaSize();
    void* arenaEnd = static_cast<char*>(arenaBase) + arenaSize;

    MEM1Start = low;
    MEM1End = arenaEnd;
    OSBaseAddress = reinterpret_cast<std::uintptr_t>(low);
    aurora::g_config.mem1Size =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arenaEnd) -
                                    reinterpret_cast<std::uintptr_t>(low));

    // ARAM (mem2Size): unrelated to MEM1/GC32/GCPTR (lib/dolphin/AR.cpp's own emulation is a
    // separate, independently malloc'd buffer addressed by ARAM-relative offsets, never by a
    // real host or GC pointer) -- just needs to be nonzero before SndInit()'s ARInit() call
    // (src/game/snd.cpp) or ARAlloc() dereferences an unset AR_BlockLength. 16 MiB, matching
    // real GameCube hardware ARAM.
    aurora::g_config.mem2Size = 16u * 1024u * 1024u;

    // Aurora's own AuroraInitArena() (../aurora/lib/dolphin/os/OSArena.cpp, called from OSInit())
    // already ran once with mem1Size == 0 by the time this executes (this file's own ordering
    // note, include/port/mem1.h) -- so it never set OSGetArenaLo()/OSGetArenaHi() at all. Set them
    // explicitly to the real usable arena, not `MEM1Start + ARENA_START_OFFSET` (Aurora's own
    // default, 0x4000 past MEM1Start): under this GC-faithful layout MEM1Start is __re4low, and
    // 0x4000 past it is still inside the exe's own low-memory/`__data` region, not the real arena.
    // Real per-task machine stacks live at a fixed GC address past the vendor's own heap end
    // (include/port/arena.h's GetTaskStackPoolBase()), not carved out of this OS-level arena at all
    // -- see that function's own comment for why. OSSetArenaLo/Hi here only feed
    // src/game/main_mem.cpp's SystemMemInit() sanity check and its OSInitAlloc() fallback bound; the
    // game's own hardcoded SysMem map (weapon..heap_end) is what actually bounds heap 0.
    OSSetArenaLo(arenaBase);
    OSSetArenaHi(arenaEnd);

    // OSBootInfo::memorySize (include/dolphin/os.h, offset 0x28 -- DVDDiskID is 0x20 bytes, +
    // magic (4) + version (4) = 0x28, confirmed against the struct directly, not assumed) and
    // `__OSBusClock` (offset 0xF8, OS_BASE_CACHED|0xF8) are both normally filled by Aurora's own
    // AuroraFillBootInfo()/AuroraInitClock() (OSInit()) -- but both ran during this process's only
    // OSInit() call, before OSBaseAddress was set (both guard on `OSBaseAddress == 0`), so neither
    // wrote anything. `src/game/main_sub.cpp`'s own OS_BUS_CLOCK macro reads this low-memory
    // location directly (bypassing Aurora's OS_TIMER_CLOCK entirely, vendor code, byte-identical)
    // -- without this, it silently reads 0 and a later OSTicksToSeconds()/divide-by-OS_TIMER_CLOCK
    // is a divide-by-zero waiting to happen. Written directly (native host endianness both ways --
    // this is live host memory both written and read on this same host, not an on-disc/BE-swapped
    // format).
    unsigned char* lowBytes = static_cast<unsigned char*>(low);
    std::uint32_t memorySize = aurora::g_config.mem1Size;
    std::memcpy(lowBytes + offsetof(OSBootInfo, memorySize), &memorySize, sizeof(memorySize));
    std::uint32_t busClock = 162000000u; // real GameCube bus clock (Aurora's own OS_BUS_CLOCK
                                          // macro, ../aurora/include/dolphin/os.h)
    std::memcpy(lowBytes + 0xF8, &busClock, sizeof(busClock));
}

} // namespace re4_port
