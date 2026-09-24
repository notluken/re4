# Phase 1 error inventory — `src/game` and REL modules on host clang/arm64

Generated 2026-09-24, updated twice more the same day. Third pass adds the `src/Tools` →
`src/tools_mod` rename (docs/port.md, "the Tools/tools APFS fold"), two `ASM_BARRIER_F` fixes, and
excludes `src/game`'s newlib libc reimplementation from every CMake target. `re4_game_all`/
`re4_rel_all` (`CMakeLists.txt`, `-- -k 0`): every `src/game/*.cpp` except the two Capcom
whole-function-asm units (`memset_2.cpp`, `yz2asm.cpp`, `tools/asmcheck.py` `ASM_BODIED`) and every
`src/game/*.c` file, and every REL module unit from `cmake/gen_rel_sources.py` (all 305, none
excluded any more), compiled individually, clang stopping only unit-by-unit. Toolchain: Apple clang
17 (`clang-1700.6.3.2`), `-arch arm64 -std=gnu++17`, `-DTARGET_PC` plus the same `-I`/version defines
`configure.py` uses for the ProDG game/REL units.

**`src/game`: 258 / 293 `.cpp` units (~88%) compile clean. 35 fail.** (351 units were attempted
before this pass; the 58 `src/game/*.c` newlib units are now excluded by design, not attempted — see
"libc exclusion" below. Not a regression: a narrower, more honest count.)
**REL modules: 268 / 305 units (~88%) compile clean. 37 fail.** (Every REL unit is attempted now;
the 3-unit Tools/tools exclusion from the previous pass is gone, see "the Tools/tools APFS fold"
below.)

## Fixes applied across all three passes (all behind `#ifdef TARGET_PC`, bytes unchanged for the
original build — verified with the Docker check, docs/port.md section 1, before each round landed
on `port/macos-arm64`)

Chosen because each either unblocked many units, is the same one-line mechanical pattern repeated,
or (the rename) removes a whole category of risk rather than working around it:

1. **`include/cManager.h`, `include/esp.h`, `include/card.h`, `include/cam_extra.h`,
   `src/game/main_mem.cpp`, `src/game/esp.cpp`** — placement/member/replacement `operator new`/
   `delete` re-spelled `std::size_t`: `unsigned int` only equals `size_t` on the GameCube target, so
   every `cUnit`-derived class lost its deallocation function on a 64-bit host.
2. **`include/joy.h`, `include/db_toolbase.h`, `include/db_widget.h`** — dropped their own
   `size_t`-mismatched `memcpy`/`strlen` redeclarations in favour of `<cstring>`.
3. **`include/math_sub.h`** `fabsf` → `__builtin_fabsf`; **`include/dbg_tool.h`**'s
   `asm("li %0,0")` (COMPILER-DIFF #13, a pure zero-register scheduling trick, not hardware) → a
   plain `= 0` initializer.
4. **`include/dbg_var.h`** — `cVarLoop`'s out-of-class members reference `cVarRange<T>`'s members
   unqualified (GCC 2.95's non-conformant dependent-base lookup); qualified through a macro
   (`DBG_VAR_BASE`) that is empty for the original target and `this->` under `TARGET_PC`.
5. **`src/t_esp/t_esp.cpp`** — `operator new(unsigned n) asm("__builtin_new")` (an asm-label alias to
   the vendor compiler's builtin-new symbol): the `TARGET_PC` branch drops the asm-label, takes
   `std::size_t`, forwards to `::operator new`.
6. **`src/game/xml.cpp`, `src/game/main_mem.cpp`, `src/st3/r332.cpp`** — `strstr`/`strrchr` results
   assigned to `char*`, valid against the GCC 2.95/MSL libc's single non-const-correct overload; local
   `strstr_host`/`strrchr_host` wrappers restore that under `TARGET_PC`.
7. **`src/game/emwindow.cpp`** — missing `<cstring>` (reached transitively through a header chain
   that differs on host). **`src/game/motion.cpp`** — one
   `__attribute__((section(".sdata")))` (Mach-O needs `"segment,section"`) dropped for `TARGET_PC`.
8. **64 `asm(".section .sdata|.rodata|.bss|.data ...")` alignment pragmas** across `src/game/*.cpp`
   and the REL sources (pad the DOL/REL's small-data/rodata/bss/data layout for the ProDG linker; no
   Mach-O meaning), each `#ifndef TARGET_PC`-gated.
9. **`-Wno-address-of-temporary`** added to every CMake target's compile options (not a source
   change): `&((Vec){...})` compound-literal-address idiom, used in several `em*`/`wep*` units, is
   legal for GCC 2.95 and alive for the enclosing full expression; clang's default diagnostic for it
   is a hard error, so it needs the flag rather than a source edit.
10. **`src/Tools` → `src/tools_mod` (directory rename, `git mv`)** — see docs/port.md, "the
    Tools/tools APFS fold" for the full byte-impact analysis, the choice of which directory to
    rename and why, and the exact move sequence (the 3 colliding filenames needed `git rm --cached` +
    `git show HEAD:... > new/path` instead of a plain `git mv`, to avoid `rename(2)`-ing the shared
    inode out from under the still-tracked `src/tools/` path). `config/G4BE08/modules.py`'s `Tools`
    module block (17 units) and one cross-reference (`t_event/db_toolbase.cpp`) now carry an explicit
    `tools_mod/<f>.cpp` source override; every `unit_name` string is untouched, so `splits.txt`/
    `sym_map.tsv`/`symbols.txt` needed no regeneration. `cmake/gen_rel_sources.py`'s Tools-unit skip
    list is gone: all 305 REL units are attempted now, the 3 formerly-excluded ones compile clean.
11. **`st1/r113.cpp`, `st1/r11d.cpp`** — `asm("" : "+f"(spd)); // COMPILER-DIFF: candidate #9` (empty
    template, no instruction, a pure scheduling barrier) replaced with `ASM_BARRIER_F(spd)`, a macro
    defined once near the top of each file (before that file's single `#line` directive) that expands
    to the same `asm(...)` for the original target and to nothing under `TARGET_PC`. The
    `COMPILER-DIFF` comment and its line are untouched; verified by keeping the physical-line gap
    between the `#line` directive and the macro's call site identical before/after the edit (62 lines
    in `r113.cpp`, 325 in `r11d.cpp`), and with `bytecmp.py` on the remote host. `r11d.cpp` now
    compiles clean; `r113.cpp` still fails, but only for the unrelated, deferred pointer-cast reason
    (see the REL table below) — the asm fix did its job.
12. **libc exclusion** — every `src/game/*.c` file (58 of them) is excluded from both `re4_game_core`
    and `re4_game_all`, not attempted: each opens with a `newlib 1.8.2 libc/...` provenance comment
    (`printf.c`, `fprintf.c`, `sprintf.c`, `sscanf.c`, `memcpy.c`, `strlen.c`, `math_support.c` built
    for `vfprintf`'s float conversion, ...) — the game's own C runtime, built against a custom
    `newlib_stdio.h`/`_reent`/`FILE` layout that conflicts with the host's `<stdio.h>`/`<string.h>`.
    The port uses the host's system libc instead (docs/port.md). No REL module references any
    `src/game/*.c` file (checked against `modules.py`), so nothing else needed excluding.
    `re4_game_core` lost its one C-file representative (`math_support.c`) as a result — there is no
    non-libc `.c` file anywhere in `src/game` to replace it with.

## The Tools/tools APFS fold — an earlier, reverted near-miss

Before the rename above existed, an earlier attempt at fix 8 (the `.section` pragmas) globbed
`src/Tools/t_prim.cpp` — at the time still a folded, case-insensitive path — first. That glob
resolved to whatever the filesystem had folded onto the shared inode (the `src/tools/` shared-body
bytes, not the `Tools` module's own 3-line wrapper), and the script wrote a valid edit back through
that wrong label. It was caught immediately (the diff for `src/Tools/{t_prim,t_util,tools}.cpp` was
hundreds of lines instead of the expected few) and reverted by writing `git show
HEAD:src/tools/<name>.cpp`'s bytes back over the three phantom-modified paths, before anything was
`git add`ed — so no bad state was ever staged or committed. The rename above (fix 10) removes the
underlying hazard entirely rather than requiring this kind of care on every future edit near those
three filenames.

## `src/game` remaining error categories (35 units), by count

| # | Category | Example (file:line) | Phase / reasoning |
|---|---|---|---|
| 42 | **Cast from pointer to smaller type** (`(int)ptr`, address tricks) | `model.cpp:151`, `act_btn.cpp:82`, `block.cpp` (×2), `sce_at.cpp` (×6), `sce_sys.cpp` (×5) | **Phase 2.** On-disc/relocated addresses stored in 32-bit fields, `Mtx`/model reordering tricks like `(f32(*)[3])(0xE0000000 + i*0x30)`. Needs the struct/pointer-representation decision from Phase 2, not a cast. |
| 34 | **`invalid output constraint '=f'/'+f'` in asm** | `math_sub.cpp` (`SQRTF`/`SINF`/`COSF`/`LIMIT_ANGLE`, paired-single Taylor series), `at_mod.cpp:1086`, `dbmodule.cpp:1393`, `Espgen42.cpp:591` | **Phase 5.** Real PPC paired-single/GQR asm (`frsqrte`, `ps_madd`, `psq_l`...), the vendor's own hardware code — multi-instruction routines, not "a single fabs/sqrt/frsqrte", so not in scope for a drive-by C rewrite. |
| 11 | **`invalid input constraint` in asm** | `cam_qfps.cpp:1679`, `db_cam.cpp:625`, `esp04.cpp:116`, `esp08.cpp:532` | **Phase 5.** Same family (register-class inputs `"f"`/`"b"` for paired-single ops). |
| 7 | **`unknown register name` in asm** | `esp04.cpp:109`, `esp12.cpp:89`, `esp16.cpp:157`, `espgen02.cpp:139` | **Phase 5.** PPC register names (`fr2`, `r9`, ...) in clobber lists of the same paired-single blocks. |
| 4 | **`array is too large (18446744073709551424 elements)`** | `Espgen42.cpp:763`, `Espgen43.cpp:314`, `cloth.cpp:676`, `espgen45.cpp:490` | **Phase 2.** Same line shape everywhere: `u8 modelPad[0x320 - sizeof(cModel)]`. `sizeof(cModel)` already exceeds `0x320` on a 64-bit host, so the padding computation underflows — direct evidence for Phase 2's struct-size inventory, not a padding-constant edit. |
| 2 | **`no matching function for call to`** | `esp0a.cpp:104` (`AddOtWorldPos`), `main.cpp:577` (`VISetPostRetraceCallback`) | **Phase 4.** SDK-shaped call sites needing Aurora's/the host stand-ins' GX/VI signatures, not a header tweak. |

(The newlib-libc categories from the previous pass — `use of unknown builtin`,
`incompatible integer to pointer conversion` in `printf.c`/`fprintf.c`/`sprintf.c`/`sscanf.c` — are
gone from this table because those files are no longer attempted at all; see "libc exclusion" above,
not a fix.)

## REL modules remaining error categories (37 units), by count

| # | Category | Example (file:line) | Phase / reasoning |
|---|---|---|---|
| 96 | **Cast from pointer to smaller type** | `st/em_wrap.cpp:117`/`:156`/`:228`, `Sscrn/ss_map.cpp:2084`, `em10/em10.cpp:11386`, most `r1xx`/`r2xx`/`r3xx`/`r402` rooms in the list below | **Phase 2.** Same category as `src/game`'s 42, at much greater volume: `em_wrap.cpp` (in every stage module) and most room objects store/recover addresses through 32-bit fields. |
| 22 | **`unknown register name` in asm** | `em2d/em2d.cpp:2756` (`register f32 ny asm("fr0")`), `em39/em39.cpp:3044`/`:3046` | **Phase 5.** Real PPC hardware register binding (paired-single register pinning), not a scheduling trick — left alone. |
| 12 | **`invalid input constraint` in asm** | `em2b/em2b.cpp:971`, `em2d/em2d.cpp:2760`, `em39/em39.cpp:1320`/`:3167` | **Phase 5.** Same family. |
| 1 | **`invalid output constraint '=f'` in asm** | `st2/r203.cpp:276` (`asm("" : "=f"(fpin))`, immediately below `register f32 fpin asm("fr31"); // COMPILER-DIFF: candidate #17`) | **Phase 5, deliberately not touched.** Superficially like the `r113.cpp`/`r11d.cpp` barrier fixed this round, but this one preserves the value of a *register-pinned* variable (`register ... asm("fr31")` two lines above, itself an "unknown register name" failure) rather than an ordinary local — wrapping just the barrier would not make the unit compile, since the real register-pinning declaration above it still fails, and doing so anyway would misrepresent what the barrier is for. |

Full failing-unit list (37): `em10/em10.cpp`, `em2b/em2b.cpp`, `em2c/em2c.cpp`, `em2d/em2d.cpp`,
`em39/em39.cpp`, `Sscrn/ss_map.cpp`, `st/em_wrap.cpp`, `st/em_wrap_v3.cpp`,
`st1/{r101,r103,r106,r10c,r10f,r113}.cpp`,
`st2/{r202,r203,r207,r209,r20d,r214,r21d,r221,r225,r226,r22c}.cpp`,
`st3/{r30c,r317,r318,r31a,r31c,r329}.cpp`, `st4/r402.cpp`, `t_esp/db_port.cpp`, `t_esp/db_widget.cpp`,
`tools/db_mod.cpp`, `tools/db_sctrl.cpp`, `wep/pl_shotgun.cpp`. (`st1/r11d.cpp` compiled clean this
pass and dropped off this list.)

## Full-tree pass not attempted

`src/lib/` (Nintendo SDK, CRI middleware, the sound DSP driver) is intentionally left out of every
CMake target — Aurora and host stand-ins take over GX/PAD/DVD/CARD (Phase 4), and the DSP driver
waits for Phase 5's host audio backend. Its own error inventory is future work.
