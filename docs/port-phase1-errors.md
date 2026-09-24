# Phase 1 error inventory — `src/game` and REL modules on host clang/arm64

Generated 2026-09-24, updated same day (second pass) with the `re4_game_all`/`re4_rel_all` CMake
targets (`CMakeLists.txt`, `-- -k 0`): every `src/game/*.c(pp)` except the two Capcom whole-function-
asm units (`memset_2.cpp`, `yz2asm.cpp`, `tools/asmcheck.py` `ASM_BODIED`), and every REL module unit
from `cmake/gen_rel_sources.py` except the three APFS-fold-excluded `Tools/` units, compiled
individually, clang stopping only unit-by-unit. Toolchain: Apple clang 17 (`clang-1700.6.3.2`),
`-arch arm64 -std=gnu++17` / `-std=gnu11`, `-DTARGET_PC` plus the same `-I`/version defines
`configure.py` uses for the ProDG game/REL units.

**`src/game`: 312 / 351 units (~89%) compile clean. 39 fail.**
**REL modules: 264 / 302 units (~87%) compile clean. 38 fail** (305 REL units total; 3 skipped, not
attempted — see "APFS Tools/tools fold" below).

## Fixes applied across both passes (all behind `#ifdef TARGET_PC`, bytes unchanged for the original
build — verified with the Docker check, see docs/port.md section 1, before each round landed on
`port/macos-arm64`)

Chosen because each either unblocked many units or is the same one-line mechanical pattern repeated:

1. **`include/cManager.h`, `include/esp.h`, `include/card.h`, `include/cam_extra.h`,
   `src/game/main_mem.cpp`, `src/game/esp.cpp`** — placement/member/replacement `operator new`/
   `delete` re-spelled `std::size_t`: `unsigned int` only equals `size_t` on the GameCube target, so
   every `cUnit`-derived class lost its deallocation function on a 64-bit host (`esp.h` alone was
   ~118 error lines, since every `espNN.cpp`/`espgenNN.cpp`/`EspgenNN.cpp` includes it).
2. **`include/joy.h`, `include/db_toolbase.h`, `include/db_widget.h`** — dropped their own
   `size_t`-mismatched `memcpy`/`strlen` redeclarations (`unsigned int` width) in favour of
   `<cstring>`.
3. **`include/math_sub.h`** `fabsf` → `__builtin_fabsf`; **`include/dbg_tool.h`**'s
   `asm("li %0,0")` (COMPILER-DIFF #13, a pure zero-register scheduling trick for the vendor
   compiler, not a hardware feature) → a plain `= 0` initializer. The only two asm bodies judged
   "small and obviously equivalent" in this slice; every other failing asm block is real PPC
   paired-single/GQR hardware code and is left alone (see the tables below).
4. **`include/dbg_var.h`** — `cVarLoop`'s out-of-class members reference `cVarRange<T>`'s members
   unqualified; GCC 2.95 resolves that into the dependent base at first-phase lookup (non-conformant,
   but what the original text relies on), ISO two-phase lookup (clang) requires `this->`. Qualified
   through a macro (`DBG_VAR_BASE`) that is empty for the original target and `this->` under
   `TARGET_PC` — one header, unblocked every REL unit that instantiates `cVarLoop<u8>`
   (`db_light.cpp` and its many transitive includers).
5. **`src/t_esp/t_esp.cpp`** — `operator new(unsigned n) asm("__builtin_new")` (an asm-label alias to
   the vendor compiler's builtin-new symbol, with its own COMPILER-DIFF-adjacent rationale comment
   left untouched): the `TARGET_PC` branch drops the asm-label, takes `std::size_t`, and forwards to
   `::operator new`.
6. **`src/game/xml.cpp`, `src/game/main_mem.cpp`, `src/st3/r332.cpp`** — `strstr`/`strrchr` results
   assigned to `char*`. The GCC 2.95/MSL libc these were written against has a single
   non-const-correct overload (always `char*`); libc++ has the standard const-correct pair. Local
   `strstr_host`/`strrchr_host` wrappers, `#define`d over the plain name for the rest of each file,
   restore the old behaviour.
7. **`src/game/emwindow.cpp`** — missing `<cstring>` (reached transitively through a header chain
   that differs on host).
8. **`src/game/motion.cpp`** — one `__attribute__((section(".sdata")))` (Mach-O needs
   `"segment,section"`) dropped for `TARGET_PC`.
9. **64 `asm(".section .sdata|.rodata|.bss|.data ...")` alignment pragmas** across `src/game/*.cpp`
   and the REL sources (pad the DOL/REL's small-data/rodata/bss/data layout for the ProDG linker; no
   Mach-O meaning), each `#ifndef TARGET_PC`-gated. One of these (`src/tools/t_prim.cpp`, the shared
   body compiled into `t_camera`/`t_emlist`) needed extra care — see "APFS Tools/tools fold" below.
10. **`-Wno-address-of-temporary`** added to every CMake target's compile options (not a source
    change): `&((Vec){...})` compound-literal-address idiom, used in several `em*`/`wep*` units, is
    legal for GCC 2.95 and alive for the enclosing full expression; clang's default diagnostic for it
    is a hard error, not a warning, so it needs the flag rather than a source edit.

## APFS `Tools`/`tools` fold — what it actually breaks

`src/Tools/` (the `Tools` REL module's own 3-line wrappers) and `src/tools/` (shared bodies compiled
into the `t_camera`/`t_emlist`/`t_esp`/`t_event`/`t_id`/`t_light`/`t_movie`/`t_sce` modules) are
distinct directories in git. On this case-insensitive host (APFS) they are the same directory, and
exactly three filenames exist in both with *different* content: `t_prim.cpp`, `t_util.cpp`,
`tools.cpp`. Reading `src/Tools/t_prim.cpp` here silently returns whichever of the two the filesystem
folded onto the shared inode — currently the `src/tools/` (shared-body) bytes, which is why `git
status` always shows those three paths modified even with no edits (pre-existing, `docs/port.md`
section 1; never stage them). Every other filename in either directory is unaffected, since only
these three collide.

`cmake/gen_rel_sources.py` therefore **excludes the `Tools`-module copies of these three units**
(`Tools/t_prim.cpp`, `Tools/t_util.cpp`, `Tools/tools.cpp`) rather than guessing which content is
really on disk under that path — this is the "unit impossible to build correctly on macOS, excluded
with a note" the port rules call for, not a rename or other workaround. The correctly-resolving
`src/tools/` shared copies (`t_camera/t_camera.cpp` → `#include "tools/t_prim.cpp"`, etc.) are
unaffected and are in `re4_rel_all`. One of them, `src/tools/t_prim.cpp` itself, did need the
`.section` pragma fix (point 9 above); it was made and verified exclusively through the lowercase
`src/tools/t_prim.cpp` path (read, edit, and `git add` all used that spelling), never
`src/Tools/t_prim.cpp`.

An earlier attempt at this same fix accidentally globbed `src/Tools/t_prim.cpp` (the folded path)
first, before this exclusion existed, wrote a valid but wrongly-labeled edit through it, and was
reverted by restoring `src/Tools/{t_prim,t_util,tools}.cpp`'s on-disk bytes to exactly
`git show HEAD:src/tools/<name>.cpp` (i.e., undoing the edit and returning to the pre-existing
fold state) before anything was staged. No `src/Tools/{t_prim,t_util,tools}.cpp` diff was ever
committed.

## `src/game` remaining error categories (39 units), by count

| # | Category | Example (file:line) | Phase / reasoning |
|---|---|---|---|
| 42 | **Cast from pointer to smaller type** (`(int)ptr`, address tricks) | `src/game/model.cpp:151`, `src/game/act_btn.cpp:82`, `src/game/block.cpp` (×2), `src/game/sce_at.cpp` (×6), `src/game/sce_sys.cpp` (×5) | **Phase 2.** On-disc/relocated addresses stored in 32-bit fields, `Mtx`/model reordering tricks like `(f32(*)[3])(0xE0000000 + i*0x30)`. Needs the struct/pointer-representation decision from Phase 2, not a cast. |
| 34 | **`invalid output constraint '=f'/'+f'` in asm** | `math_sub.cpp` (`SQRTF`/`SINF`/`COSF`/`LIMIT_ANGLE`, paired-single Taylor series), `at_mod.cpp:1086`, `dbmodule.cpp:1393` | **Phase 5.** Real PPC paired-single/GQR asm (`frsqrte`, `ps_madd`, `psq_l`...), the vendor's own hardware code — multi-instruction routines, not "a single fabs/sqrt/frsqrte", so not in scope for a drive-by C rewrite this round. |
| 11 | **`invalid input constraint` in asm** | `cam_qfps.cpp:1679`, `db_cam.cpp:625`, `esp04.cpp:116`, `esp08.cpp:532` | **Phase 5.** Same family (register-class inputs `"f"`/`"b"` for paired-single ops). |
| 10 | **`use of unknown builtin` / `[-Wimplicit-function-declaration]`** | `printf.c:10`, `fprintf.c:10` | **Later, unscoped.** The four newlib libc reimplementations (`printf.c`, `fprintf.c`, `sprintf.c`, `sscanf.c`) build on a custom `newlib_stdio.h`/`_reent`/`FILE` layout colliding with the host's `<stdio.h>`; needs a real libc-shim decision, the same kind of work as Phase 2 but for the CRT instead of game data. |
| 7 | **`unknown register name` in asm** | `esp04.cpp:109`, `esp12.cpp:89`, `esp16.cpp:157`, `espgen02.cpp:139` | **Phase 5.** PPC register names (`fr2`, `r9`, ...) in clobber lists of the same paired-single blocks. |
| 5 | **`incompatible integer to pointer conversion` [-Wint-conversion]** | `printf.c:10`, `printf.c:22`, `sprintf.c:15` | **Later, unscoped.** Same newlib-libc units as above. |
| 4 | **`array is too large (18446744073709551424 elements)`** | `Espgen42.cpp:763`, `Espgen43.cpp:314`, `cloth.cpp:676`, `espgen45.cpp:490` | **Phase 2.** Same line shape everywhere: `u8 modelPad[0x320 - sizeof(cModel)]`. `sizeof(cModel)` already exceeds `0x320` on a 64-bit host, so the padding computation underflows — direct evidence for Phase 2's struct-size inventory, not a padding-constant edit. |
| 2 | **`no matching function for call to`** | `esp0a.cpp:104` (`AddOtWorldPos`), `main.cpp:577` (`VISetPostRetraceCallback`) | **Phase 4.** SDK-shaped call sites needing Aurora's/the host stand-ins' GX/VI signatures, not a header tweak. |

## REL modules remaining error categories (38 units), by count

| # | Category | Example (file:line) | Phase / reasoning |
|---|---|---|---|
| 96 | **Cast from pointer to smaller type** | `src/st/em_wrap.cpp:117`, `:156`, `:228`, `src/Sscrn/ss_map.cpp:2084`, every `r1xx`/`r2xx`/`r3xx`/`r402` room in the failing-unit list below | **Phase 2.** Same category as `src/game`'s 42, at much greater volume: `em_wrap.cpp` (in every stage module) and most room objects store/recover addresses through 32-bit fields. |
| 22 | **`unknown register name` in asm** | `src/em2d/em2d.cpp:2756` (`register f32 ny asm("fr0")`), `em39.cpp:3044`/`:3046` | **Phase 5.** Real PPC hardware register binding (paired-single register pinning), not a scheduling trick — left alone. |
| 12 | **`invalid input constraint` in asm** | `em2d.cpp:2760`, `em2b.cpp:971`, `em39.cpp:1320`/`:3167` | **Phase 5.** Same family. |
| 3 | **`invalid output constraint '+f'` in asm** | `r113.cpp:134`, `r11d.cpp:376`, `r203.cpp:276` (`asm("" : "+f"(spd)); // COMPILER-DIFF: candidate #9`) | **Phase 5, deliberately not touched.** An empty-template `asm` is a pure optimization/scheduling barrier (no instruction), which makes it *look* like the "obviously equivalent" case this round allowed — but every site carries its own `COMPILER-DIFF: candidate #N` tag, i.e. it is already on record as steering the vendor compiler's scheduling for a specific matched function. Swapping it for a generic host barrier changes what is being asserted about the original build without the matching context to justify it, so it is left for whoever next touches these `r1xx` rooms with `fdiff` open, not hacked from here. |

Full failing-unit list (38): `em10/em10.cpp`, `em2b/em2b.cpp`, `em2c/em2c.cpp`, `em2d/em2d.cpp`,
`em39/em39.cpp`, `Sscrn/ss_map.cpp`, `st/em_wrap.cpp`, `st/em_wrap_v3.cpp`,
`st1/{r101,r103,r106,r10c,r10f,r113,r11d}.cpp`,
`st2/{r202,r203,r207,r209,r20d,r214,r21d,r221,r225,r226,r22c}.cpp`,
`st3/{r30c,r317,r318,r31a,r31c,r329}.cpp`, `st4/r402.cpp`, `t_esp/db_port.cpp`, `t_esp/db_widget.cpp`,
`tools/db_mod.cpp`, `tools/db_sctrl.cpp`, `wep/pl_shotgun.cpp`.

## Full-tree pass not attempted

`src/lib/` (Nintendo SDK, CRI middleware, the sound DSP driver) is intentionally left out of every
CMake target — Aurora and host stand-ins take over GX/PAD/DVD/CARD (Phase 4), and the DSP driver
waits for Phase 5's host audio backend. Its own error inventory is future work.
