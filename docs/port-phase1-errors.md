# Phase 1 error inventory — `src/game` on host clang/arm64

Generated 2026-09-24 with the `re4_game_all` CMake target (`CMakeLists.txt`, `-k 0`): every
`src/game/*.c(pp)` except the two Capcom whole-function-asm units (`memset_2.cpp`, `yz2asm.cpp`,
`tools/asmcheck.py` `ASM_BODIED`) compiled individually, clang stopping only unit-by-unit. Toolchain:
Apple clang 17 (`clang-1700.6.3.2`), `-arch arm64 -std=gnu++17` / `-std=gnu11`, `-DTARGET_PC` plus the
same `-I`/version defines `configure.py` uses for the ProDG game units.

**Result: 308 / 351 units (~88%) compile clean after the fixes below. 43 units still fail.**

## Fixes already applied in this slice (all behind `#ifdef TARGET_PC`, bytes unchanged for the
original build — verify with the Docker check before this lands on `port/macos-arm64`)

These were chosen because each one unblocked many units, not because they were the easiest:

1. **`include/cManager.h`** — the placement `operator new(unsigned int, void*)` and `cUnit`'s
   `operator delete(void*, unsigned int)` are only recognized as the standard placement-new /
   sized-deallocation overloads because `unsigned int` *is* `size_t` on the GameCube target. On a
   64-bit host `size_t` is `unsigned long`, so every `cUnit`-derived class (`cCoord`, `cModel`,
   `cLight`, `cEm`, `cObj`, ...) lost its deallocation function and every derived virtual destructor
   failed to compile. Spelled as `std::size_t` under `TARGET_PC`; same no-op semantics.
2. **`include/esp.h`** (`cEsp::operator new(unsigned int)`, declaration + `src/game/esp.cpp`
   definition) — same size_t-width issue; alone worth ~118 of the pre-fix error lines because every
   `espNN.cpp`/`espgenNN.cpp`/`EspgenNN.cpp` unit includes it.
3. **`include/card.h`** (`cCardWork`'s member `operator new`/`operator delete`) and
   **`include/cam_extra.h`** (`cCamera`, `IDApplication` placement new/delete) — same fix, smaller
   blast radius.
4. **`src/game/main_mem.cpp`** — the game's *replacement* global `operator new`/`operator new[]`
   (`unsigned int size` → `std::size_t size` under `TARGET_PC`); a non-member replacement operator
   new is required by ISO to take `size_t` as its first parameter, so this one was rejected outright
   on host, not just losing recognition as a special form.
5. **`include/joy.h`** — dropped the file's own `extern "C" void* memcpy(void*, const void*,
   unsigned int)` redeclaration under `TARGET_PC` (conflicts with the host libc's `size_t`-width
   prototype); falls through to `<cstring>` instead.
6. **`include/math_sub.h`** `fabsf` — the SDK-style single `fabs` PPC instruction has no arm64
   equivalent; swapped for `__builtin_fabsf` under `TARGET_PC` inside the same rename-macro wrapper
   that already worked around the `<math.h>` name clash.
7. **`asm(".section .sdata|.rodata|.bss ...")` alignment pragmas** (60 call sites across
   `src/game/*.cpp`, e.g. `src/game/item.cpp:3239`, `src/game/dvd.cpp:2082`,
   `src/game/read.cpp:1027`) — pad the DOL's small-data/rodata/bss layout for the ProDG linker; they
   emit no instruction and have no host meaning, so each is now `#ifndef TARGET_PC`-gated (mechanical,
   same one-line pattern every time).

## Remaining error categories (43 units), by count

| # | Category | Example (file:line) | Notes |
|---|---|---|---|
| 42 | **Cast from pointer to smaller type** (`(int)ptr`, `(u32)ptr` address tricks) | `src/game/model.cpp:151`, `src/game/model.cpp:944`, `src/game/sce_at.cpp` (×6), `src/game/sce_sys.cpp` (×5), `src/game/dbmodule.cpp`, `objRobo.cpp`, `block.cpp` | The Phase 2 problem (`docs/port.md` "64-bit pointers in loaded data") arriving early: on-disc/relocated addresses stored in 32-bit fields, `Mtx`/model reordering tricks like `(f32(*)[3])(0xE0000000 + i*0x30)`. Not a Phase 1 fix — needs the struct/pointer-representation decision from Phase 2. |
| 34 | **`invalid output constraint '=f'/'+f' in asm`** | `src/game/math_sub.cpp` (`SQRTF`/`SINF`/`COSF`/`LIMIT_ANGLE`, paired-single Taylor series), `src/game/at_mod.cpp:1086`, `src/game/dbmodule.cpp:1393` | Real PPC paired-single/GQR asm (`frsqrte`, `ps_madd`, `psq_l`...) — the vendor's own hardware code per CLAUDE.md, explicitly slated for Phase 5 ("C equivalents under TARGET_PC"), not this slice. |
| 11 | **`invalid input constraint` in asm** | `src/game/cam_qfps.cpp:1679`, `src/game/db_cam.cpp:625`, `src/game/esp04.cpp:116` | Same family as above (register-class inputs `"f"`/`"b"` for paired-single ops). |
| 10 | **`use of unknown builtin` / `[-Wimplicit-function-declaration]`** | `src/game/printf.c:10`, `src/game/fprintf.c:10` | The four newlib libc reimplementations (`printf.c`, `fprintf.c`, `sprintf.c`, `sscanf.c`) build on a custom `newlib_stdio.h`/`_reent`/`FILE` layout that collides with the host's `<stdio.h>`. Needs a real libc-shim decision (Phase 2/3 territory: this is exactly the kind of "on-disc struct on a 64-bit host" work, just for the CRT instead of game data), not a one-line fix. |
| 7 | **`unknown register name` in asm** | `src/game/esp04.cpp:109`, `src/game/esp12.cpp:89`, `src/game/esp16.cpp:157` | PPC register names (`fr2`, `r9`, ...) in clobber lists of the same paired-single asm blocks. |
| 5 | **`incompatible integer to pointer conversion` [-Wint-conversion]** | `src/game/printf.c:10`, `src/game/printf.c:22` | Same newlib-libc-reimplementation units as above. |
| 4 | **`array is too large (18446744073709551424 elements)`** | `src/game/Espgen42.cpp:763`, `src/game/Espgen43.cpp:314`, `src/game/cloth.cpp:676`, `src/game/espgen45.cpp:490` | All the same line shape: `u8 modelPad[0x320 - sizeof(cModel)]`. `sizeof(cModel)` already exceeds `0x320` on a 64-bit host (bigger pointers/`u32`-as-`unsigned long` fields), so the padding computation underflows. Direct evidence for Phase 2's struct-size inventory. |
| 4 | **`assigning to 'char *' from 'const char *' discards qualifiers`** | `src/game/xml.cpp:15`, `src/game/xml.cpp:40`, `src/game/main_mem.cpp:411` | `strstr`/`strrchr` results assigned straight to `char*`. The GCC 2.95/MSL libc these were written against returns non-const `char*` from a single overload; libc++ has the standard const-correct overload pair. Needs an explicit cast at each call site (small, but touches game logic files, so left for a later, reviewed pass rather than done blind here). |
| 2 | **`no matching function for call to`** | `src/game/esp0a.cpp:104` (`AddOtWorldPos`), `src/game/main.cpp:577` (`VISetPostRetraceCallback`) | SDK-shaped call sites where the GameCube-specific overload/signature (from `include/dolphin/...` or the game's own SDK-mirroring headers) doesn't exist yet on host — this is squarely Aurora's/Phase 4's job (GX/VI stand-ins), not a header tweak. |
| 1 | **`use of undeclared identifier 'strcmp'`** | `src/game/emwindow.cpp:748` | Missing `<string.h>`/`<cstring>` include reached only through a header chain that differs on host; needs a one-line `#include` fix, not attempted here to keep this slice's fix list short. |
| 1 | **`argument to 'section' attribute is not valid for this target`** | `src/game/motion.cpp:2195` (`__attribute__((section(".sdata")))`) | Same family as the 60 `asm(".section ...")` sites above but spelled as a GCC attribute instead of inline asm; Mach-O needs `"segment,section"`. One more mechanical `#ifdef TARGET_PC` candidate for a follow-up pass. |

## Full-tree pass not attempted

`src/lib/` (Nintendo SDK, CRI middleware, the sound DSP driver) was intentionally left out of both
CMake targets — Aurora and host stand-ins take over GX/PAD/DVD/CARD (Phase 4), and the DSP driver
waits for Phase 5's host audio backend. Its own error inventory is future work.
