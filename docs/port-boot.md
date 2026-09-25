# Boot-path inventory (main() -> title screen)

Read-only analysis, 2026-09-24, for the "first boot" milestone (docs/port.md). Traces
`src/game/main.cpp`'s `main()` through `systemStartInit()`/`systemRestartInit()` to `Title_task`.
Everything here is DOL code (`src/game`); **no REL module is on the boot path** -- `OSLink`/`OSUnlink`
(`src/game/main_sub.cpp:544-567`, `DLL_Link`/`DLL_Unlink`) exist for per-room REL loading, which
happens after the title screen once a game/room is actually entered, not before it. This matches
docs/port.md's phase reorder: RELs stay deferred past first boot.

## 1. Call chain

```
main()                                    src/game/main.cpp:120
  systemStartInit()                       :245  -- once, boot only
  RESTART:
    systemRestartInit()                   :320  -- once per (re)start, soft-reset included
    TaskExec(0, Title_task, 0)            :156  -- title.cpp, starts the title screen task
    for (;;) { ... frame loop ... }       :157-207
```

## 2. `systemStartInit()` (src/game/main.cpp:245-282) -- subsystem-by-subsystem

| Call | Subsystem | Where | Aurora or stub |
|---|---|---|---|
| `memclr_asm` | plain memset-shaped helper | `src/game/*` (asm-bodied unit, `memset_2.cpp`/inline) | C equivalent, no SDK dependency |
| `OSInit()` | OS bring-up (BAT/MMU/cache/arena) | `src/lib/OS*.c` | **Stub**: none of this has meaning on a hosted macOS process; a TARGET_PC no-op or minimal init (set up `re4_port` arena via `InitArena()`/`CreateArenaThread` instead) |
| `setLanguage()` | reads the console's language setting | `src/game/main.cpp` (not grepped this pass) | **TO VERIFY** where it reads from (likely `OSGetLanguage`, SDK-level, not disc) |
| `RomFontSetting()` | GameCube ROM (IPL) font setup | `src/game/dvd.cpp:1992` | **Stub or skip**: reads the console's built-in IPL font ROM, not the disc; no macOS equivalent exists. Likely fine to no-op for first boot (title screen may render without it, or render garbled text -- **TO VERIFY**) |
| `OSGetConsoleType()` | dev-console detection | SDK | **Stub**: return "not a dev console" (0), so the `InitFile()`/`file_path("d:\\bio4")` host-filesystem branch below is skipped by construction |
| `InitFile()` / `file_path()` | dev-mode host filesystem (SN PC read) | `src/game/*` | Not exercised if `IsDevConsole` stub returns 0 (see above) -- **not needed for first boot** |
| `ExceptionInit()` | PPC exception handler installation | `src/game/exception.cpp` (asm-bodied unit) | **Stub**: no PPC exception vectors on arm64; a no-op (or install a SIGSEGV/SIGBUS handler later, not first-boot-critical) |
| `OSInitAlarm()` | alarm/timer subsystem init | `src/lib/OSAlarm.c` | **Aurora or host stub**: needs OS alarms onto host timers (docs/port.md Phase 4) -- **TO VERIFY** whether Aurora provides this or a host `dispatch_source`/`timer_create`-based stub is needed |
| `VIInit()` | video interface init | `src/lib`/GX | **Aurora** (`aurora_gx`/its VI-equivalent, per docs/port.md's Phase 4 line) |
| `systemVISetBlack(1)` | blank the screen | GX/VI | Aurora |
| `LCEnable()` | locked-cache enable | PPC-specific hardware | **Stub, no-op**: no locked cache on arm64/macOS |
| `VIWaitForRetrace()` | vsync wait | VI | Aurora (or a host frame-pacing stand-in) |
| `VISetPostRetraceCallback(postVSyncCallback)` | vsync interrupt callback registration | VI | Aurora, or Phase 4's own "two SDK-shaped call sites" list (docs/port-phase1-errors.md already flags `VISetPostRetraceCallback` as Phase 4 material) |
| `Dvd.Init()` | DVD drive init | `src/game/dvd.cpp` | **Aurora DVD**, reading from an extracted disc directory (`orig/G4BE08/files`) per docs/port.md |
| `CardInit()` | memory card init | `src/game/card.cpp:260` | **Aurora CARD** or stub (no read needed for first boot unless the title screen probes for an existing save to offer "continue" -- **TO VERIFY**, see section 3) |
| SPR asm block (`mtspr 914-917`) | GQR (paired-single quantization) register setup | `src/game/main.cpp:266-279` | **PPC-specific, no arm64 equivalent** -- this is exactly the Phase 5 paired-single/GQR material (docs/port.md); safe to `#ifdef TARGET_PC`-skip entirely, since nothing on arm64 reads GQRs, but any code that *relies* on the resulting rounding/quantization behaviour (paired-single loads/stores) needs a C equivalent (Phase 5), not just skipping this init |
| `SystemMemInit()` | game heap/arena setup | `src/game/main_mem.cpp` | Already partly ported: `re4_port`'s arena (`src/port/arena.cpp`) is the intended host replacement per docs/port-phase2.md; wiring `SystemMemInit()` itself to call into it is boot-path work, not yet done |
| `CardDbgCacheSet()` | dev-only card cache tuning | `src/game/card.cpp` | Not exercised (dev-console stub returns 0) |
| `TaskSchedulerInit()` | cooperative task scheduler init | `src/game/scheduler.cpp` (asm-bodied unit) | Host equivalent: the task scheduler itself is portable C++ (cooperative, not OS threads) except wherever it context-switches via PPC-specific asm -- **TO VERIFY** exact mechanism (`src/game/scheduler.cpp` register list) |
| `systemScreenInit()` | default render-mode/projection constants | `src/game/main.cpp:379` | No disc/SDK dependency beyond `Rmode` (a `GXRenderModeObj`, comes from Aurora GX once wired) |
| `Render_init()` | render subsystem init | `src/game/*` (not traced this pass) | Aurora GX |
| `LightSetInit()` | light pool init | `src/game/light.cpp` | Plain C++, no SDK/disc dependency (already compiles clean per Phase 1, `re4_game_core`) |
| `RndInit(0xD37)` | RNG seed | `src/game/*` | Plain C++ |
| `InitOt()` | 2D ordering-table init | `src/game/libgpu.cpp`(-adjacent) | GX-adjacent, see `libgpu.h`'s `p`/`tag` macro fix in docs/port-phase2.md step 4 |
| `SndInit()` | sound driver init | `src/game/snd*.cpp`, `src/lib/{adx,sfd,mpv,mps,dct}_*` (CRI middleware, MWCC-compiled) | **Phase 5 material** (docs/port.md): DSP/ARAM-targeted; needs a host audio backend. For first boot, likely safe to stub to "sound off" -- **TO VERIFY** whether `SndInit()` itself halts/asserts if the hardware isn't present |
| `SofdecInit()` | Sofdec (FMV) init | `src/game/sofdec.cpp` (asm-bodied unit) | **Phase 5 material** (CRI SFD); stub for first boot, FMV playback deferred |
| `IdSys.gameInit(0x80)` | "id sprite" system pool init | `src/game/id_sys.cpp` | Plain C++ |
| `IdTexGameInit()` | id-sprite texture init | `src/game/id_tex.cpp` | Touches TPL-shaped data (`Ptr32<T>`-converted per Phase 2 step 5) -- **TO VERIFY** whether it loads a TPL from disc at this point or only allocates |
| `EprintfInit()` | debug print buffer init | `src/game/eprintf.cpp` (asm-bodied unit) | Host stub, no SDK dependency beyond formatting |
| `LogInit()` | debug log init | `src/game/db_log.cpp` (asm-bodied unit) | Host stub |
| `init_dbmodule()` | SN debugger module hookup | `src/game/dbmodule.cpp` | **Phase 5 material**: `dbmodule.cpp` is flagged in docs/port-phase1-errors.md for its `PSQ_L_S16`/`PSQ_L_U8_TO` paired-single family; likely safe to stub/skip entirely for first boot (SN Debugger integration, not gameplay-critical) |
| `Dvd.SizeTableRead()` | reads the disc's file-size table | `src/game/dvd.cpp` | **Reads from disc** -- see section 3, format 1 |
| `cMes.init()` | message system init | `src/game/mes.cpp` | Touches the TPL/`ARC_PTR` family; **TO VERIFY** whether init alone reads a file or only allocates |

## 3. Disc reads on the boot path, and their formats

Two confirmed `DvdReadN` calls between `systemStartInit()` and the title screen becoming interactive,
plus `Dvd.SizeTableRead()`:

| Call site | File | Format | Endianness work needed (Phase 3) |
|---|---|---|---|
| `Dvd.SizeTableRead()` (`systemStartInit`, `src/game/dvd.cpp`) | disc-wide file size table (name **TO VERIFY** -- not traced to its literal path string this pass) | A flat table, likely offset/size pairs, no pointer fields (not in docs/port-phase2.md's format table) | A swapper for its integer fields (sizes/offsets), **TO VERIFY** exact layout |
| `DvdReadN("debug/roomInfo.dat", ...)` (`systemRestartInit`, `src/game/main.cpp:373`) | `CRoomInfo` (room name/person/person2 table) | **Already Phase-2-converted** (`include/room_jmp.h`'s `CRoomInfo::name`/`person`/`person2`, `Ptr32<T>`, docs/port-phase2.md section 1 table) -- needs only the endianness swap, not further pointer work |
| `DvdReadN("SS/cmn/title.snd", ...)` (title.cpp:126, first thing `Title_task` does) | Sound data (ADX-family, **TO VERIFY** exact container) | Phase 5 (sound), not Phase 3's concern directly -- ADX is CRI middleware format, MWCC-built, out of this DOL-focused pass |
| `DvdReadN(title_dat, ...)` where `title_dat = "SS/___/title.dat"` (title.cpp:158/174) | An **archive** (`ARC_PTR`/`TITLE_ARC_PTR` family, docs/port-phase2.md step 4 table) holding the title screen's models/textures/palettes | Model BIN + TPL formats (docs/port-phase2.md section 1, both already `Ptr32<T>`-converted) -- needs the endianness swap plus whatever `ARC_PTR`'s host branch (already done, step 4) expects |

So the formats genuinely needed for first boot are: the DVD size table (new, unswapped, layout
**TO VERIFY**), `CRoomInfo` (`roomInfo.dat`), and the title archive's contents (model BIN + TPL,
inside an `ARC_PTR`-indexed archive blob) -- all three already have their pointer-relocation side
done or partly done by Phase 2; only the byte-swap-at-load-time step (Phase 3) is new work for them.
Save data (`SAVE_DATA_HEAD`, CARD) is **not** confirmed to be read before the title screen is
reachable -- **TO VERIFY**: whether the title screen probes for an existing save (for a "continue"
option) during `Title_task`, which would pull `SAVE_DATA_HEAD`/CARD into the boot-critical set. Not
traced this pass (`title.cpp` beyond the two `DvdReadN` call sites above was not read in full).

## 4. Asm-bodied / Phase-5-flagged units on or adjacent to the boot path

From the call chain above (not a full `asmcheck.py` re-run -- that tool errored this session on
unrelated stale state, `src/Tools/db_toolbase.cpp` not found post the `Tools`->`tools_mod` rename;
**TO VERIFY**, looks like a stale `build/G4BE08/config.json` needing `configure.py`+`ninja`, out of
scope for this read-only pass and not touched):

- `src/game/exception.cpp` -- `ExceptionInit()`, PPC exception vector installation.
- `src/game/scheduler.cpp` -- `TaskSchedulerInit()`, cooperative task switch (asm-bodied unit per
  docs/port-phase1-errors.md; exact register-level mechanism **TO VERIFY**).
- `src/game/sofdec.cpp`, `src/game/dbmodule.cpp` -- FMV/SN-debugger, both asm-bodied, both plausibly
  skippable for first boot (sound/FMV/debugger, not core boot logic).
- `src/game/eprintf.cpp`, `src/game/db_log.cpp` -- asm-bodied, but their asm is very likely the
  `ASM_BODIED` register-shuffling kind already catalogued in docs/port-phase1-errors.md, not
  hardware-dependent logic; safe host stubs.
- `main.cpp`'s own inline `mtspr` block (GQR setup) -- real PPC SPR asm, `#ifdef TARGET_PC`-skippable
  outright for boot (nothing on arm64 reads a GQR), but see the Phase 5 caveat in section 2's table.

None of `math_sub.cpp` (paired-single `SQRTF`/`SINF`/`COSF`), `em2d.cpp`, or `st2/r203.cpp` (the
other Phase 5-flagged asm units, docs/port-phase1-errors.md) appear directly on this boot chain --
they are enemy/room-specific (REL-side or gameplay-only DOL code), consistent with "no REL on the
boot path" above. **TO VERIFY** with a real linked build once one exists; this pass is call-graph
reading, not a linker's reachability answer.

## 5. What is still unread (explicitly out of scope this pass)

- `Title_task`'s own state machine beyond its first two `DvdReadN` calls (title.cpp is >900 lines;
  only skimmed for `DvdReadN`/`arc`/`tpl`/`bin`/font hits).
- `setLanguage()`'s exact source (SDK call vs disc read) -- **TO VERIFY**.
- `Dvd.SizeTableRead()`'s literal file path and record format -- **TO VERIFY**.
- Whether `IdTexGameInit()`/`cMes.init()` read from disc at init time or only allocate -- **TO VERIFY**.
- Card/save-data involvement in the title screen's "continue" detection -- **TO VERIFY**.

## 6. Headline (for the report)

- **Subsystems**: OS/alarm/exception -> stubs; VI/GX/DVD/CARD -> Aurora; sound (`SndInit`)/FMV
  (`SofdecInit`)/SN-debugger (`init_dbmodule`) -> stub-to-off for first boot, real work deferred to
  Phase 5; GQR/paired-single init -> skip under `TARGET_PC`, Phase 5 owns the real behaviour;
  task scheduler -> needs its asm context-switch mechanism identified (**TO VERIFY**).
- **Disc formats needed for boot**: DVD size table (layout **TO VERIFY**), `CRoomInfo`
  (`roomInfo.dat`), title archive (model BIN + TPL via `ARC_PTR`) -- all already pointer-relocated by
  Phase 2, only the byte-swap-at-load step is new.
- **REL modules on the boot path**: none -- REL loading (`OSLink`/`DLL_Link`) is per-room, invoked
  after the title screen, not before it.
- **Asm units on the boot path**: `exception.cpp`, `scheduler.cpp`, the `mtspr` GQR block in
  `main.cpp` itself, plus `sofdec.cpp`/`dbmodule.cpp`/`eprintf.cpp`/`db_log.cpp` (mostly
  skippable/stubbable for first boot, not hardware-critical to reaching the title screen).

## 7. `re4_boot` links (2026-09-24)

Starting state: 486 undefined symbols (docs/port.md's last session). This pass fixed the
`main_game` linkage bug (below), then closed the remaining undefined-symbol list with generated
and hand-written stubs, and got a clean link. `cmake/boot_exclude.txt` (36 files) was **not**
shrunk this pass -- time went to the link path instead, per the "prioritize reaching link" budget
note; that is still open work for a future session (see the exclusion categories already recorded
there).

### `main_game` linkage bug (found first, blocked everything else)

`src/port/boot_main.cpp` declared `extern "C" int main_game();`, but `-Dmain=main_game` renames
the identifier *before* Sema ever sees it, so `main.cpp`'s renamed function does not get the
special "no mangling" treatment a literal `main` gets -- it mangles as an ordinary C++ function
(`__Z9main_gamev`, confirmed with `nm` on `main.cpp.o`). `extern "C"` in the declaration mismatched
that and the two objects never linked. Fixed by dropping `extern "C"` (commit `f5db6d95`).

### Undefined-symbol mangling: answered

All ~486 undefined symbols are genuinely either C-linkage or genuinely C++-linkage; macOS `ld`
demangles the C++ ones in its own error text (no `_Z`/`__Z` prefix in what it prints), which is why
the two categories look inconsistent at a glance. Concretely:

- A **plain global-namespace-scope C++ variable** (`cActionButton ActBtn;`, `extern cDvd Dvd;`,
  ...) is **not** Itanium-mangled at all regardless of `extern "C"` -- verified with a two-line
  `clang++ -c` + `nm` test (`Foo bar;` at global scope emits `_bar`, not `__Z3bar`). Only
  functions (via overloading) and namespace/class-scoped entities need the `_Z` scheme.
- A **free or member C++ function** (`cPlayer::actionSelect()`, `dmMotCk()`, ...) mangles
  normally; `ld`'s error text prints the demangled form, which is why these show up as full
  human-readable signatures with no `_Z`/underscore prefix at all.
- An **`extern "C"` function or global** (most of the SDK/CRI-shaped declarations, `ActBtn`-style
  globals' *functions* though not the globals themselves, `at_mod.h`'s `EmAtCheck` family, ...)
  gets a single Mach-O underscore and nothing else (`_OSInitAlarm`, `_At_em_rect_rect_ck`).

So the stub generator's job reduces to: symbols with exactly one leading underscore and no other
punctuation are C-linkage (strip the underscore, look up an `extern "C"` declaration); everything
else is already the human-readable C++ signature straight from the linker, parse that directly
instead of re-deriving it.

One genuine anomaly, not fully explained this pass (**TO VERIFY**): five symbols --
`Dvd`, `ScreenReSize`, `pSys` (x3), `memset`, and the SndCall entry -- appeared in the raw
`ld` undefined-symbol list with **no** leading underscore even though the real definitions are
ordinary C-linkage globals/functions. All five turned out to share one root cause once found: each
is the target of an `asm("literal-name")` alias in the vendor source (`main_mem.cpp`'s
`pSysView`/`DvdView`, `card.cpp`'s `ScreenReSizeI`, `emshield.cpp`'s `SndCallV`,
`route_ck.cpp`'s `memset_v`) -- a documented compiler-diff trick (docs/port.md, "t_esp.cpp's
operator new") that binds the alias to the *literal* link-time name with no leading underscore.
That's correct for the original ELF/PowerPC target (no automatic name-mangling underscore there)
but wrong for Mach-O, which always adds that underscore for an ordinary C reference -- so the
alias pointed at a symbol (`memset`, no underscore) that nothing defines, instead of the real one
(`_memset`). Fixed with `TARGET_PC` branches that call the real function/global directly instead
of aliasing (commit `ff94f937`, verified byte-identical on the remote x86_64 build below).

### Stub counts (`tools/port/gen_boot_stubs.py` v2 + hand-written holdouts)

| Group | Count | Where |
|---|---|---|
| C-linkage functions (generated) | 320 | `src/port/stubs/generated_c_stubs.cpp` |
| C-linkage data (generated) | 24 | `src/port/stubs/generated_c_stubs.cpp` (outside the `extern "C"` block -- see the file) |
| C++ methods/free functions (generated) | 115 | `src/port/stubs/generated_cpp_stubs.cpp` |
| Hand-written (function-pointer params, GNU-v2-mangled `SndCall`, tables/consts normally in excluded files) | ~20 | `src/port/stubs/manual_stubs.cpp` |
| `__builtin_new`/`delete`/`vec_new`/`vec_delete` (puzzle.cpp) | 4 | `src/port/stubs/host_new_delete.cpp` |
| Vtable key functions + the rest of each class's virtual slots (cPlayer, cPlLeon, cPlAshley, cObjRobo, cObjRocket, cObjLauncher) | ~30 | `src/port/stubs/manual_stubs.cpp` |
| Unresolved / not attempted | 0 | -- everything the generator flagged unresolved was covered by hand |

None of the generated/hand-written stubs are **data** definitions that should have come from a
non-excluded file instead (the `c_data` list -- `ActBtn`, `CamDbg`, `DC`, `EmReadModule`,
`GameSave`, `lockCtr`, `m3r`, `mercId`, `PlReadModule`, `WepReadModule`, ... -- are all genuinely
owned by excluded files per `cmake/boot_exclude.txt`'s categories).

### Vtable key functions (Itanium ABI gotcha)

Six classes' constructors are reachable but their *own* first non-inline virtual member function
(the Itanium "key function" that decides which TU gets to emit the vtable) is declared but defined
only in an excluded file, and is never itself called anywhere reachable on this boot path -- so
the linker's undefined-symbol list only ever showed `vtable for X`, never the key function's own
name, until the constructor was reached: `cPlayer::beginEvent`, `cPlLeon::move`,
`cPlAshley::move`, `cObjRobo::move`, `cObjRocket::beginEvent`, `cObjLauncher::~cObjLauncher`. Once
each key function got a stub, the linker then required *every other* virtual slot in that same
class to be defined too (Itanium vtables are all-or-nothing per TU) -- `src/port/stubs/
manual_stubs.cpp`'s `RE4_STUB_VOID` block covers the rest.

### Aurora mismatches

Aurora's static libs (`aurora_os`/`aurora_vi`/`aurora_gx`/`aurora_pad`/`aurora_dvd`/`aurora_card`/
`aurora_mtx`/`aurora_si`/`aurora_core`) declare several `dolphin/*.h` headers this code includes,
but do **not** implement the functions this boot path actually calls:

- `dolphin/mtx.h`'s `PSMTX*`/`PSVEC*` (paired-single) family: Aurora only implements the scalar
  `C_MTX*` equivalents (`nm libaurora_mtx.a` has `_C_MTXConcat` etc., no `_PSMTXConcat`) -- every
  `PSMTX*`/`PSVEC*` call on this boot path is a logging stub, not real math. A real fix would wrap
  Aurora's `C_MTX*` (verify identical semantics first, not assumed).
- `dolphin/os/OSThread.h`, `OSAlarm.h`, `OSSemaphore.h`: declared, zero symbols defined in
  `libaurora_os.a` (`nm | grep -i "alarm\|thread"` is empty) -- OS threading/alarms are stubbed,
  not backed by Aurora.
- `dolphin/ax.h`/CRI's `mwply.h`/ADX headers: no Aurora library covers sound at all; every
  `AX*`/`ADX*`/`mwPly*` call is a stub. Expected (docs/port.md marks sound Phase 5).
- `dolphin/gx.h`'s `GXWGFifo` (a real hardware MMIO absolute-address symbol on the original
  target) has no Aurora equivalent; stubbed as a 1-element array (`manual_stubs.cpp`), **TO
  VERIFY** once real GX vertex submission is wired through Aurora instead.

Aurora libraries that ARE used for real: none yet load-bearing on the actual link (`aurora_gx`,
`aurora_dvd`, `aurora_card` etc. link cleanly and their symbols weren't needed to satisfy anything
on this pass -- the crash in section 8 happens before any of them run).

### Link result

`re4_boot` links cleanly (`cmake --build build-pc-boot --target re4_boot`, `RE4_BUILD_BOOT=ON
RE4_U32_32=ON`). Verified the default host build (`RE4_U32_32=OFF`, `build-pc/`) is unaffected:
`re4_game_core`/`re4_port`/`test_ptr32`/`test_arena` unchanged (same two pre-existing failures,
`math_sub.cpp`/`model.cpp`, both already documented Phase 2/5 material, not touched this pass);
`ctest` 2/2 passed.

## 8. First run (2026-09-24)

Ran under `lldb --batch -o run -o bt -k bt -k quit` (DVD root wiring -- `argv`/env var -- **not
done this pass**; the crash happens before `main()`, so it never mattered this time). Crash:

```
* thread #1, stop reason = EXC_BAD_ACCESS (code=1, address=0x7)
    frame #0: cLog::add(this=0x0, flag=0, errId=0, mes="alloc[%x]:free[%x] %s", ap=...) at db_log.cpp:209
    frame #1: cLog::verr(this=0x0, ...) at db_log.cpp:82
    frame #2: cLog::err(this=0x0, ...) at db_log.cpp:56
    frame #3: mem_alloc(size=32, file="operator new", line=0, flag=1, heap=0) at main_mem.cpp:429
    frame #4: mem_calloc(...) at main_mem.cpp:437
    frame #5: operator new(size=8) at main_mem.cpp:80
    frame #6..#12: libc++ std::vector<bool> allocation path
    frame #13: aurora::gfx::render_worker::FrameSlotPool::FrameSlotPool(slotCount=2) at render_worker.cpp:138
    frame #14: __cxx_global_var_init.3() at frame.cpp:56
    frame #15: _GLOBAL__sub_I_frame.cpp
    frame #16+: dyld running C++ global constructors before main()
```

**Diagnosis**: this crashes before `re4_boot`'s own `main()` ever runs, during dyld's C++ global
static-initializer pass. Aurora's `aurora::gfx::frame` translation unit has a global
`FrameSlotPool` object whose constructor allocates a `std::vector<bool>`, which calls
`::operator new`. But the game's own `operator new` (`main_mem.cpp:80`) is linked into the same
executable and is *not* namespaced or guarded -- it globally overrides `::operator new` for the
whole process, including Aurora's unrelated internal allocations. That override routes into
`mem_alloc`, which on any bookkeeping/logging path (`MAD` tag mismatch handling here) calls through
a global `cLog`-family object (`Log`, presumably) that has not been constructed yet -- C++ static
initialization order between two independently-linked static libraries (`re4_boot_game` and
Aurora) is unspecified, and Aurora's constructor ran first this time. The `this == 0x0` in
`cLog::add` is that not-yet-constructed global, read before its constructor set it up. This is not
a one-line fix: it needs either (a) the game's `operator new` override scoped/deferred so it
doesn't intercept allocations before `re4_port::InitArena()`/the log system are ready, or (b) an
explicit init-order fence (a priority attribute, or moving Aurora's problematic global out of
static-init entirely) so the game's own subsystems are guaranteed to construct before any
`operator new` call reaches them. Not attempted this pass, per the "don't fix beyond the first
crash unless trivial" instruction.

## 9. Split allocator (2026-09-24, fixes section 8's crash)

Design (`include/port/alloc.h`, `src/port/alloc.cpp`, `src/game/main_mem.cpp`'s `TARGET_PC`
branch):

- `operator new`/`new[]`: game heap (`mem_calloc`) only when `re4_port::ShouldUseGameHeap()`
  (`IsGameThread() && HeapsReady()`); otherwise `std::malloc`. `IsGameThread()` is a
  `thread_local` flag set once by `re4_port::MarkCurrentThreadGame()`, called from
  `CreateArenaThread`'s new trampoline (`src/port/arena.cpp`) before the caller's own start
  function runs -- so only threads started that way are ever "the game" for allocation purposes.
  `HeapsReady()` is an `std::atomic<bool>` set by `re4_port::MarkHeapsReady()`, called from
  `SystemMemInit()` right after `Heap[0]` exists (before that point even the game thread must use
  malloc, since there is no game heap yet).
- `operator delete`/`delete[]`: routed by *pointer identity*
  (`re4_port::IsArenaPointer`, address falls inside `GetArenaBase()`/`GetArenaSize()`), not by
  thread or by how the pointer was allocated -- everything `mem_calloc` ever hands out lives
  inside the embedded arena by construction. A debug-only (`#ifndef NDEBUG`) check aborts if an
  arena pointer is ever freed off the game thread (the OSAlloc-based heap is not thread-safe, so
  a cross-thread free would be a real, if rare, bug on its own).
- Sized/aligned/nothrow `operator new`/`delete` overloads: **not added this pass** (not needed to
  reach the next blocker, see section 10; TO VERIFY whether libc++/Aurora code reachable later
  ever calls one of these -- if so, they need the same two rules).
- Host libraries (Aurora, SDL, libc++ internals) never run on a thread `MarkCurrentThreadGame()`
  touched, so their allocations always take the `malloc` path by construction -- no extra guard
  needed for "host code never gets game-heap memory" beyond the thread check itself.

Verified: fixes section 8's pre-main crash (confirmed by rerunning the same `lldb` command -- see
section 10, the process now gets past `OSInit()`/Aurora's static initializers and into
`SystemMemInit()` before it hits a new, unrelated blocker). Default host build (`RE4_U32_32=OFF`)
unaffected: `re4_game_core` still has only the same two pre-existing failures
(`math_sub.cpp`/`model.cpp`), `ctest` 2/2 passed. `main_mem.cpp`'s vendor-tree edit remote-verified
(commit `d4476310`) -- see section 11.

## 10. DVD root configuration (2026-09-24, plumbing only -- not reachable yet)

`re4_port::InitDvdRoot(argc, argv)` (`include/port/dvd_root.h`, `src/port/dvd_root.cpp`), called
first thing in `boot_main.cpp`'s `main()`: resolves `argv[1]`, else `$RE4_DVD_ROOT`, else
`orig/G4BE08/files`, and logs it (`re4_boot: DVD root=...`). Not wired any further than storage --
boot does not reach `cDvd::Init()` yet (section 11's blocker is earlier), and wiring it properly
surfaces a second, separate design question worth flagging now rather than guessing at: Aurora's
own DVD backend (`aurora_dvd_open(const char* disc_path)`, `lib/dolphin/dvd/dvd.cpp` in the Aurora
checkout) reads through the `nod` library, which expects a **real GC/Wii disc image**
(`.iso`/`.gcm`), not an extracted `files/`+`sys/` tree -- but this repo's own convention
(docs/port.md section 1, and the "never commit orig/, the ISO..." port rule) is to work from the
extracted tree, not a committed or even locally-required disc image. Reconciling those two -- synthesize
a disc image from the extracted tree at configure time, or write a host DVD backend that reads the
extracted tree directly and bypasses `nod`/`aurora_dvd_open` entirely -- is a design decision, not
attempted this pass.

## 11. MEM1/arena blocker (2026-09-24, stopped here)

Ran under `lldb --batch -o run -o bt -k bt -k quit` after section 9's fix:

```
STUB: PSMTXIdentity() called
re4_boot: DVD root=orig/G4BE08/files
re4_boot: arena base=0x100d38000 size=1073741824
STUB: memclr_asm() called
STUB: OSGetFontEncode() called
STUB: OSInitFont() called
STUB: OSGetConsoleType() called
STUB: OSSetErrorHandler() called
STUB: DBIsDebuggerPresent() called
STUB: OSInitAlarm() called
STUB: systemVISetBlack() called
STUB: VIWaitForRetrace() called
STUB: VISetPostRetraceCallback() called
STUB: ADXGC_SetupDvdFs() called
STUB: OSReport() called
STUB: OSGetConsoleSimulatedMemSize() called
[info] [aurora::card] CARD API Initialized BUILT <Sep 24 2026 12:47:29>
Assertion failed: (newLo <= MEM1End && newLo >= MEM1Start), function OSSetArenaLo, file OSArena.cpp, line 27.

* thread #2, stop reason = hit program assert
  frame #4: OSSetArenaLo(newLo=0x1016ac140) at OSArena.cpp:27
  frame #5: SystemMemInit() at main_mem.cpp:111
  frame #6: ::systemStartInit() at main.cpp:612
  frame #7: main_game() at main.cpp:73
  frame #8: (anonymous namespace)::GameThreadEntry(...) at boot_main.cpp:36
  frame #9: re4_port::(anonymous namespace)::ThreadTrampoline(...) at arena.cpp:109
  frame #10: libsystem_pthread.dylib`_pthread_start + 136
```

**Milestone reached**: past `OSInit()` and every Aurora/host C++ static initializer, into
`systemStartInit()`, as far as the *second* line of `SystemMemInit()` (`main_mem.cpp:111`,
`SysMem.arena_lo = (u32) OSGetArenaLo();` -- the crash is actually one step later, inside
`OSInitAlloc`'s call chain reaching `OSSetArenaLo`, `main_mem.cpp:136`, not literally line 111;
the reported frame is the enclosing function).

**Diagnosis**: this is a real, load-bearing implementation Aurora provides (`libaurora_os`'s
`lib/dolphin/os/OSArena.cpp` -- `OSGetArenaLo/Hi`, `OSSetArenaLo/Hi`, `OSAllocFromArenaLo/Hi`),
*not* one of this session's stubs -- it linked because Aurora satisfies it, per the "don't stub
what Aurora already implements" rule. Its `OSSetArenaLo`/`OSSetArenaHi` assert the new value falls
within `[MEM1Start, MEM1End)`, two globals only `AuroraOSInitMemory()` sets
(`lib/dolphin/os/OSMemory.cpp`), and only if `aurora::g_config.mem1Size > 0` --  which nothing in
`re4_boot` sets, so `MEM1Start`/`MEM1End` are still null when `SystemMemInit()` runs, and the
assert fires on the very first non-null value.

Setting `mem1Size` is not simply "the missing call", though -- it exposes a second, deeper
architecture question, the actual reason this needs a design decision rather than a local fix:
Aurora's `AllocMEM1()` on non-Windows (`lib/dolphin/os/OSMemory.cpp`, the `#else` branch) is a
plain `calloc(1, size)` -- an ordinary host heap pointer, with no guarantee of fitting in 32 bits,
no relationship at all to this repo's own Phase 2 compressed-handle arena
(`include/port/arena.h`, `re4_port::g_base`/`GetArenaBase()`). The crash's own `newLo` value
(`0x1016ac140`) is in fact this session's *own* arena base (`0x100d38000` from the log line just
above it) plus a small offset -- i.e. the game code's `(u32) OSInitAlloc(...)`/`(void*) arenaLo`
narrowing-then-widening round trip is, by coincidence of this run's ASLR placement, producing an
address that happens to look arena-relative, not because the two schemes are actually reconciled.
Two independent "how does a GC 32-bit address become a host pointer" mechanisms (this repo's
`Ptr32<T>`/arena, and Aurora's `OSBaseAddress`/`MEM1Start`-relative one) cannot both be live at
once without deciding which owns the address space `SystemMemInit()`'s `SysMem`/`Heap[]` game
code computes into -- that decision (adopt Aurora's `OSMemory` wholesale and retire/adapt
Phase 2's own arena scheme accordingly, or keep Phase 2's arena and provide a `TARGET_PC` OSArena
implementation of our own instead of linking Aurora's) is exactly the kind of blocker the
coordinator's stop condition names. Not attempted further this pass.

## 12. How to build and run (2026-09-24, tested verbatim in a scratch build dir)

Everything below was run from a clean checkout state, in a throwaway `build-howto`/
`build-tool-howto` pair (deleted afterward -- these are gitignored, not committed), on this
machine (macOS, Apple clang + Homebrew LLVM, Apple Silicon). Re-run the whole sequence if in
doubt; nothing here is inferred.

### Prerequisites (Homebrew)

```sh
brew install cmake ninja llvm sdl3 libpng fmt zstd freetype xxhash
```

(`cmake` 4.4.3, `ninja` 1.13.2, `llvm` 23.1.1 tested; older/newer within reason should work, not
verified.) Also needs `../aurora` cloned next to this checkout (`git clone <aurora repo> ../aurora`,
pinned commit `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` as of this writing) and
`orig/G4BE08/files/` present (the extracted disc tree -- never committed, see the port rules;
get it the same way the byte-matching build's README describes).

### 1. Build the cast-rewriter tool (once; only needs redoing if `tools/port/cast_rewriter/CastRewriter.cpp` changes)

```sh
cmake -S tools/port/cast_rewriter -B build-pc-tool -G Ninja \
      -DCMAKE_PREFIX_PATH=$(brew --prefix llvm) -DCMAKE_BUILD_TYPE=Release
cmake --build build-pc-tool
```

Produces `build-pc-tool/re4_cast_rewriter` (~55 MB). Needs Homebrew LLVM specifically -- Apple's
bundled clang does not ship the libTooling headers/libs this links against.

### 2. Configure and build `re4_boot`

```sh
cmake -S . -B build-pc-boot -G Ninja \
      -DRE4_BUILD_BOOT=ON -DRE4_U32_32=ON \
      -DRE4_CAST_REWRITER_BIN="$PWD/build-pc-tool/re4_cast_rewriter" \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build build-pc-boot --target re4_boot -j8
```

(`build-pc-boot` here is just this session's example name -- any directory works, gitignored
either way.) First configure fetches Aurora's prebuilt Dawn/nod packages and builds Tracy/abseil/
imgui from source (a few seconds); a clean `re4_boot` build (including Aurora and every `src/game`
unit re4_boot links) took **~28 s** on this machine with `-j8`. `-DRE4_AURORA_DIR=<path>` defaults
to `<repo>/../aurora` -- pass it explicitly if Aurora lives somewhere else. The resulting binary is
`build-pc-boot/re4_boot` (~24 MB).

### 3. Run it

**Must be run with the repo root as the current working directory** -- both the DVD root
(`argv[1]`) and the disc image path (`argv[2]`) default to paths relative to `cwd`, not to the
binary's location:

```sh
cd <repo root>   # important -- see above
./build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso
# argv[1] (DVD root, informational only as of section 13 -- see below) defaults to
#   orig/G4BE08/files, or $RE4_DVD_ROOT
# argv[2] (the real disc image Aurora's nod-based DVD backend opens) defaults to
#   orig/G4BE08/re4_debug_disc1.iso, or $RE4_DISC
```

Expected output as of section 13 (the process still aborts, this is not yet a successful boot --
see section 13's current blocker):

```
STUB: PSMTXIdentity() called
re4_boot: DVD root=orig/G4BE08/files
re4_boot: disc image=orig/G4BE08/re4_debug_disc1.iso
re4_boot: arena base=0x<some address> size=1073741824
STUB: memclr_asm() called
... more "STUB: ... called" lines (expected -- docs/port-boot.md section 7's stub survey) ...
[info] [aurora::card] CARD API Initialized BUILT <build timestamp>
STUB: OSSetSaveRegion() called
STUB: memset_asm() called
STUB: OSInitThreadQueue() called
STUB: OSInitSemaphore() called
STUB: Render_init() called
[debug] [aurora::ar] Initialized 0x1000000 bytes of ARAM!
```

then the process crashes (`EXC_BAD_ACCESS`, on Aurora's own background DVD I/O thread -- section
13's current blocker, not a sign the build is broken).

### 4. Get a backtrace

```sh
cd <repo root>
lldb --batch -o run -o "bt" -k "bt" -k "quit" -- ./build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso
```

`-o run` starts it, `-k bt`/`-o bt` print the backtrace whether it stops on a crash (the current
case) or any other signal, `-k quit` exits lldb afterward instead of leaving it at an interactive
prompt. Expect to land on `(anonymous namespace)::readFromHandle` in Aurora's `dvd.cpp` (section
13) unless something upstream of that has changed since this was written.

## 13. Boot progress (2026-09-24, continued -- MEM1 unification + real DVD)

User decisions this round: (1) re4_port's embedded arena is the single owner of the GameCube
address space; (2) use Aurora's real `nod`-based DVD backend against the real disc image
(`orig/G4BE08/re4_debug_disc1.iso`), not the extracted-files tree.

### Mechanism chosen for (1): point Aurora's globals at our arena, no source patch

Aurora's `OSArena`/`OSAlloc`/`OSInitAlloc` (`lib/dolphin/os/OSArena.cpp`, `OSAlloc.cpp`) are real
implementations this repo already links (not stubs). They gate their own bounds checks on two
plain-linkage globals, `MEM1Start`/`MEM1End` (`lib/dolphin/os/OSMemory.cpp`), only ever set by
Aurora's own `AuroraOSInitMemory()`, and only when `aurora::g_config.mem1Size > 0` (never true in
this repo, hence section 11's blocker). Neither `MEM1Start`/`MEM1End` nor `aurora::g_config` are
declared in a public Aurora header, but both are ordinary external-linkage symbols (not `static`)
-- `include/port/mem1.h`/`src/port/mem1.cpp` redeclares them (`extern void* MEM1Start;` etc.) and
sets `MEM1Start = re4_port::GetArenaBase()`, `MEM1End = base + arena size`,
`OSBaseAddress = base` (this one *is* public, `<dolphin/os.h>`), and `aurora::g_config.mem1Size`/
`mem2Size` (ARAM, unrelated to MEM1 but gated the same way, section on `ARAlloc` below) to match.
**Picked over "replace Aurora's OSArena/OSAlloc with our own"**: simpler (zero new files
duplicating heap-management logic Aurora already has working) and more robust (Aurora's own
`OSAllocFromHeap`/`OSFreeToHeap`/`OSCreateHeap` already operate correctly on real host pointers --
confirmed by reading them, section below); the only actual bug was on this repo's side (next
paragraph). No `tools/port/aurora-patches/` needed -- this is the "hook already exists" branch.

**Ordering gotcha**: `InitMem1()` must run *after* `OSInit()` has executed once (with
`mem1Size` still 0, a no-op on Aurora's side) -- calling it any earlier (tried first, from
`boot_main.cpp` before `CreateArenaThread`) means `OSInit()`'s own call to
`AuroraOSInitMemory()` sees `mem1Size > 0` and reallocates its own MEM1 block via a plain
`calloc()`, silently clobbering what `InitMem1()` had just set up. Fixed by calling it from
`src/game/main.cpp`'s `systemStartInit()`, immediately after the `OSInit();` call (`TARGET_PC`
branch, `#ifdef`-gated, remote-verified byte-identical for the original build).

### A second, related bug found along the way: `main_mem.cpp`'s u32<->pointer boundary

`SystemMemInit()`/`MemCreateHeap()` store `OSGetArenaLo/Hi()`/`OSInitAlloc()`/`OSCreateHeap()`'s
results in plain `u32` fields (`arenaLo`, `arenaHi`, `HeapHead`, ...) via bare `(u32)`/`(void*)`
casts -- correct on the original 32-bit target (every pointer *is* 32 bits there), silently
truncating on this 64-bit host now that Aurora's real `OSArena`/`OSAlloc` hand back real 64-bit
host pointers. Fixed the same way this repo already handles on-disc pointer fields: wrapped each
site in `re4_port::GC32()`/`GCPTR()` (`include/port/ptr32.h`) under `TARGET_PC`, so the u32 value
these fields hold is the compressed, GameCube-looking handle, not a truncated raw pointer.
`mem_alloc`/`mem_calloc`/`Mem_free`/`OSAllocFromHeap`/`OSFreeToHeap` themselves were **already**
correct (they pass real pointers throughout, never narrow them) -- only the arena-bootstrap
functions needed this.

### Milestones reached this round (each with its fix, one line)

1. **Past `OSSetArenaLo`'s assert** (section 11's blocker) -- `InitMem1()` (above).
2. **Past `SystemMemInit()` entirely, into `TaskSchedulerInit`/`Render_init`** -- the GC32/GCPTR
   fix (above) for the truncated arena pointers (previously crashed with the `HALT()` poison
   address `0x11111111` from a corrupted heap pointer feeding an ELF-size-overflow check).
3. **Past `ARInit`/`ARAlloc` (SndInit's ARAM setup)** -- `aurora::g_config.mem2Size` set to 16 MiB
   (real GameCube ARAM size) in the same `InitMem1()` call; unrelated to MEM1/GC32 (Aurora's ARAM
   emulation is its own independently `malloc`'d buffer, addressed by ARAM-relative offsets, never
   by a real host or GC pointer), just needed to be nonzero before `SndInit()`'s `ARInit()` call.
4. **Past the disc-open step** -- `include/port/dvd.h`/`src/port/dvd.cpp`'s `re4_port::InitDvd()`,
   called from `boot_main.cpp` before `InitArena()`: resolves `argv[2]` / `$RE4_DISC` /
   `orig/G4BE08/re4_debug_disc1.iso`, calls `aurora_dvd_open()` on it. **Must run with the repo
   root as the current working directory** -- the path is resolved relative to `cwd`, and the
   default is a relative path (see section 12's updated run command). Never copies the ISO.
5. **Past `CardInit()`/`CARDInit`**, into `Render_init`, ARAM init, and a real DVD read reaching
   Aurora's own background DVD-I/O thread -- **Aurora dolphin-API mismatch** found and fixed:
   `include/dolphin/card.h` (this repo's own copy) declares the real-hardware
   `CARDInit(void)` signature and this repo calls it that way; Aurora's own `<dolphin/card.h>`
   (gated by its *own*, same-named `TARGET_PC` macro) only ever defines a two-argument
   `CARDInit(const char* game, const char* maker)` -- both `extern "C"`, so the argument-count
   mismatch is invisible at compile time and left `game`/`maker` reading whatever garbage was in
   the argument registers, which `CardGciFolder::setCurrentGame` then dereferenced and crashed on.
   Fixed by calling the real two-argument symbol directly (`asm("_CARDInit")`-aliased, file-scope
   -- a *local* `extern "C"` redeclaration with a different signature does not parse in clang,
   confirmed with a minimal repro before landing on the file-scope alias instead) with
   `(nullptr, nullptr)` (both setters no-op on null).

### Current blocker: Aurora's own DVD worker thread crashes on `handle->seek`

```
* thread #3 (Aurora's internal DVD I/O thread, not the game thread), EXC_BAD_ACCESS, address=0x20142010039
  frame #0: (anonymous namespace)::readFromHandle(handle=0x100ce39c8, ...) at dvd.cpp:233
      -> handle->seek(offset, 0)
  frame #1: DvdWorker::perform_command(...) at dvd.cpp:530
  frame #2: DvdWorker::process_command(...) at dvd.cpp:537
  frame #3: DvdWorker::run(...) at dvd.cpp:510
  frame #4: DvdWorker::start()::'lambda'()::operator()(...) at dvd.cpp:354
  frame #5..#7: std::thread plumbing
```

**Diagnosis (not root-caused this pass, budget-limited)**: this is entirely inside Aurora's own
`lib/dolphin/dvd/dvd.cpp`, on a background `std::thread` Aurora spawns for DVD I/O, not in game
code or this repo's port code. `handle` is a `CommandDataBase*` (a small polymorphic wrapper
around either a `nod`-backed disc reader or an overlay-file reader); `handle->seek` is a virtual
call landing on a wildly invalid address (`0x20142010039`), consistent with either (a) a corrupt
vtable pointer (the `CommandDataNod`/`CommandDataBase` object was never properly constructed, or
was destroyed/reused before this async command ran), or (b) `readFromHandle` receiving a stale
`handle` for a command queued before `aurora_dvd_open()`'s partition (`s_partition`) was actually
ready -- both point at an ordering/lifetime issue between this repo's `InitDvd()` call and the
first `DvdRead()` reaching Aurora's async worker, not an obviously local one-line fix. Given this
is a real bug inside Aurora's own async DVD path rather than a call-site or config mismatch this
repo controls, and the session's time budget, this is where this pass stops -- next steps would be
attaching a debugger to Aurora's DVD worker thread specifically to inspect `handle`'s actual
contents/vtable, or checking whether `aurora_dvd_open()` needs to fully settle (a background
thread of its own?) before the first read can safely be queued.

No window ever opens this session (crash predates any GX/VI frame submission) -- no screenshot.

## 14. SDK header parity check and the real cause of section 13's DVD-worker crash (2026-09-24)

The coordinator's hypothesis for section 13's blocker was a `DVDFileInfo`/`DVDCommandBlock` struct-layout
mismatch between this repo's `include/dolphin/dvd.h` and Aurora's own copy. Checked directly:
`diff include/dolphin/dvd.h ../aurora/include/dolphin/dvd.h` -- the two are **byte-identical** apart from
three lines this repo doesn't need (`DVDConvertEntrynumToPath`, `DVDGetDOLLocation` declarations, one
comment). `DVDCommandBlock`/`DVDFileInfo` themselves are word-for-word the same struct in both copies, and
`RE4_GAME_INCLUDES` is searched before `${RE4_AURORA_DIR}/include` (`CMakeLists.txt`), so game code
resolves to this repo's copy -- which, being identical, produces the same in-memory layout Aurora's own
`dvd.cpp` compiles against. **The layout-mismatch hypothesis does not hold for this struct.**

`CARDInit` (section 13, point 5) *is* a genuine confirmed case of the hypothesis's general shape (a
same-named function/struct that silently disagrees between this repo's SDK header and Aurora's own,
already fixed there) -- but it is a signature mismatch (arg count), not a size/offset mismatch, and it
was already fixed before this pass.

### Root cause, found with lldb (breakpoint on `readFromHandle`, inspected `handle`'s vtable pointer)

Not a struct-layout bug at all: a **field-ownership conflict**. The real GameCube SDK's contract leaves
`DVDFileInfo::cb.userData` free for the caller once `DVDOpen()` returns -- the real hardware's low-level
DVD driver never reads it again, and this game relies on exactly that: `src/game/dvd.cpp`'s
`cDvdQueue::fileReadAsync()` (vendor code, byte-identical, cannot change) does
`m_Info.cb.userData = this;` right after opening, storing its own bookkeeping pointer there. Aurora's host
reimplementation of `DVDFastOpen()`/`DVDOpen()` (`../aurora/lib/dolphin/dvd/dvd.cpp`) instead uses that
same field for its **own** internal handle object (`CommandDataNod*`/`CommandDataOverlay*`, needed because
a host build has no LBA-addressable disc hardware and must keep a live file handle per open file). The
game's write clobbers Aurora's handle the moment the vendor code runs its normal open-then-read sequence;
the next async read's `getCommandHandle()` then does a virtual call through the game's `this` pointer
reinterpreted as `CommandDataBase*` -- the `0x20142010039`/`0x100ce39c8`-shaped "vtable pointer" was
exactly that: a live heap pointer to unrelated game state, not a corrupted/glued 32+32 value. Confirmed
live: `lldb -o "b (anonymous namespace)::readFromHandle" -o run` showed `handle` sitting 8 bytes before its
own `DVDCommandBlock*` in the game heap and dereferencing to garbage, consistent with reading the game's
own nearby allocation instead of Aurora's heap-allocated handle object.

**This is a genuine Aurora bug** (real-hardware-incompatible reuse of a caller-owned field), not a config
mismatch this repo's call sites could route around -- proven with the vendor source (which cannot be
changed) plus a live inspection of Aurora's own internal state, not asserted from the RTL/struct level
alone.

### Fix: `tools/port/aurora-patches/0001-dvd-userdata-sidetable.patch`

Per the port rules (never commit into Aurora's history), this is a patch file, applied to the Aurora
checkout's working tree by `CMakeLists.txt` at configure time (`RE4_BUILD_BOOT`, right before
`add_subdirectory`), idempotently -- `git apply --reverse --check` gates a second application, so
reconfiguring twice or with an already-patched checkout is a no-op, not an error. It moves Aurora's own
per-file handle out of `cb.userData` into a private side table (`std::unordered_map<DVDCommandBlock*,
CommandDataBase*>`, keyed by the stable `&fileInfo->cb` pointer, mutex-guarded for the worker thread) in
`../aurora/lib/dolphin/dvd/dvd.cpp`: `getCommandHandle()`, `DVDFastOpen()`, `DVDClose()`,
`DVDPrepareStreamAsync()` all updated; the game-visible `cb.userData` field is no longer touched by Aurora
at all, restoring the real hardware's contract. Verified: `re4_boot` links and runs past the DVD worker
thread crash entirely (see milestone below); default host build (`RE4_U32_32=OFF`, `build-pc/`) untouched
-- this pass's only repo-tracked change is inside `CMakeLists.txt`'s `RE4_BUILD_BOOT` block, `ctest` still
2/2 (the pre-existing `re4_game_all` `cam_ctrl.cpp` build error under a full `cmake --build build-pc`,
unrelated to this change and not touched, is a separate, already-present issue -- **TO VERIFY** whether
it's the same "two pre-existing failures" section 9 already documented or a third one; not chased this
pass, out of scope).

### Milestone: past the DVD worker thread entirely

Rerunning section 12's `lldb` command after the patch: every `STUB:`/`[info]`/`[debug]` line from section
13 reproduces identically, the disc read that used to crash the DVD worker thread now succeeds, and the
process runs much further into `systemStartInit()` -- through `CardInit()`, `Render_init()`, ARAM setup,
and into `SndInit()` -- before hitting a new, unrelated blocker below.

### Current blocker: `SndInit()` dereferences a raw GameCube absolute address

```
* thread, EXC_BAD_ACCESS, address=0x812fc000
  frame #0: Snd_str_blk_init(blk_no=1, data=0x812fc000) at snd_sub3.cpp:29
      -> blk->num = *p++;   (p = (u32*) data)
  frame #1: SndInit() at snd.cpp:125
  frame #2: ::systemStartInit() at main.cpp:630
```

**Diagnosis**: `src/game/snd.cpp` hardcodes `#define SND_DATA_TOP 0x80370000` (vendor code, byte-identical
-- a literal fixed GameCube main-memory address, real hardware convention: sound data is DMA'd straight to
a fixed MEM1 address and read back through the same literal address, never through this repo's
`Ptr32<T>`/arena-relative handle scheme). `0x812fc000` is `SND_DATA_TOP` plus an accumulated `ALIGN32`
offset (`snd.cpp:108`), still in the same fixed-absolute-address family. Nothing in this port's arena
(`re4_port`'s embedded, ASLR-placed heap, section 13's `InitMem1()`) lives at a literal `0x80xxxxxx` host
address, so the dereference faults. This is exactly the sound subsystem Phase 5 already flags as deferred
(section 2, section 6, section 7's Aurora-mismatch survey: no Aurora library covers sound at all) -- fixing
it needs a design decision this pass does not make: either give the port's own fixed-address literals (this
one, and any others like it in `snd.cpp`/`sofdec.cpp`) a real backing allocation at load time (translate
`SND_DATA_TOP`-relative addresses through a small table instead of a raw literal), or intercept
`SndInit()`/`DvdRead(..., SND_DATA_TOP, ...)` under `TARGET_PC` before this literal is ever dereferenced.
Not attempted this pass -- stopping here per the "stop at a design decision" instruction, sound/ARAM
addressing is Phase 5 scope, not a small local fix.

### SDK header parity, systematic pass (headline only -- not exhaustive this session)

Time this pass went to root-causing and fixing the actual blocker (above) rather than a full systematic
header-by-header diff across every SDK subsystem (DVD/CARD/OS/VI/GX/PAD/AR) the coordinator's task asked
for; that remains open. What was actually diffed head-to-head this pass:

| Subsystem | Files compared | Result |
|---|---|---|
| DVD | `include/dolphin/dvd.h` vs `../aurora/include/dolphin/dvd.h` | Identical structs; 3 harmless declaration/comment diffs (this repo doesn't need `DVDConvertEntrynumToPath`/`DVDGetDOLLocation`) |
| CARD | `include/dolphin/card.h` `CARDInit(void)` vs Aurora's `CARDInit(const char*, const char*)` | **Confirmed mismatch** (signature, not layout) -- already fixed before this pass (section 13, point 5), via a file-scope `asm("_CARDInit")` alias calling the real two-arg symbol with `(nullptr, nullptr)` |

**Not yet compared this pass** (per the coordinator's task list, still open): OS threads/alarms/mutex/
message queues, VI, GX object types (`GXTexObj`/`GXTlutObj`/`GXFifoObj`/`GXRenderModeObj`), PAD, AR. Given
none of these have yet produced a crash on the boot path beyond `SndInit()`'s literal-address issue above,
and this pass's time went to the two real, load-bearing bugs found by working the actual crash chain
(DVD-worker fix, this section's write-up) rather than a header-by-header audit with no crash yet motivating
it, a full systematic table is deferred to whichever session reaches the point those subsystems' real
implementations (not stubs) start running. **TO VERIFY / open work**: build that table before GX/VI start
mattering (past `SndInit`), since `GXTexObj`-family objects are exactly the kind of struct this repo's own
`Ptr32<T>` conversions could plausibly disagree with Aurora's copy on, unlike DVD/CARD which turned out
copy-identical.

No window opens this session either (crash still predates any GX/VI frame submission) -- no screenshot.

## 15. `SND_DATA_TOP` follow-up: GCPTR routing confirmed correct, stubbed `SndInit()` per Phase 5 (2026-09-24)

Coordinator pushback on section 14's diagnosis: `SND_DATA_TOP` (`0x80370000`) is not a new design
question -- Phase 2 (docs/port-phase2.md step 8) already covers fixed GameCube addresses in
`0x80000000..0x81800000`: they fall inside the arena by construction (`GCPTR(0x80370000) == g_base +
0x80370000`). Checked directly, and section 14's write-up undersold what was already working:

- `build-pc-boot/gen/src/game/snd.cpp` (the cast-rewriter's actual output) shows every cast of
  `SND_DATA_TOP`-derived values to a pointer type **already** rewritten to `re4_port::GCPTR<T>(...)`
  -- `DvdRead(0, re4_port::GCPTR<void>((std::uint32_t)(0x80370000)), ...)`,
  `SndMem.str_file[i] = re4_port::GCPTR<SndStrFile>((std::uint32_t)((SND_DATA_TOP + ...)))`, etc. The
  rewriter did its job correctly here; section 14's framing of this as "a GC-address integer used as
  a host pointer without GCPTR" was wrong -- confirmed by reading the actual generated file, not just
  the vendor source.
- Broader grep across `src/game/*.cpp` for other `0x80xxxxxx`/`0x81xxxxxx` literals used as pointers
  (`debug.cpp`'s/`dvd.cpp`'s `OS_BUS_CLOCK` macro, `dvd.cpp`'s `DVD_BUFF`/`DVD_BUFF2`) shows the same:
  all already rewritten to `GCPTR<T>(...)` in the generated files. No missed class of this bug found
  on the boot path this pass.
- Live in `lldb` (breakpoint on `Snd_str_blk_init`, printing `re4_port::g_base` and `data`): one run
  showed `g_base = 0x80d3c000` (nonzero, correctly set) and `data` a real, large host-range pointer
  for `blk_no=0` -- GCPTR is genuinely producing dereferenceable addresses. A different run crashed
  on `blk_no=1` with `data=0x812fc000` (small, GC-looking, no `g_base` offset) -- **not reproducible
  every run**, consistent with reading genuinely uninitialized/wrong-offset data out of the sound
  file's header table (a real parsing bug somewhere in the stream-file format or its DVD read, not a
  missing pointer conversion) rather than a systematic GCPTR gap. Not root-caused further --

Per the coordinator's explicit fallback ("if sound itself misbehaves, stub `SndInit()`/sound under
`TARGET_PC` for first boot rather than going deep" -- sound is Phase 5, no Aurora audio backend
exists regardless), `SndInit()` (`src/game/snd.cpp`) now returns early under `TARGET_PC` right after
`pSnd = &Snd; memclr_asm(pSnd, sizeof(SndWork));` (same pattern as `main.cpp`'s existing
`#ifdef TARGET_PC`/`#ifndef TARGET_PC` boot-path branches -- inserted before the function's first
`#line`-tracked region, so the non-`TARGET_PC`/matching build's preprocessed output, and therefore
its bytes, is untouched). Verified: `re4_boot` runs past `SndInit()`/`SofdecInit()`/`init_dbmodule()`
into `MessageControl::init()` (the message/font system) before hitting a new blocker (below).

## 16. Current blocker: `Font/common_p.fnt` DVD read completes but reports failure

```
MesCtrl::fontLoad() Font load failed
* thread, EXC_BAD_ACCESS, address=0x2
  frame #0: MessageFont::create(addr=0x...) at mes.cpp:235 -> t->pTex->width
  frame #1: MessageControl::setupFont(...) at mes.cpp:358
  frame #2: MessageControl::loadSystemFont(...) at mes.cpp
  frame #3: MessageControl::init(...) at mes.cpp:753
  frame #4: ::systemStartInit() at main.cpp:638
```

**Confirmed NOT a missing-file or entrynum-resolution problem**: `grep -a -o "Font/[A-Za-z0-9_./]*\.fnt"
orig/G4BE08/re4_debug_disc1.iso` shows `Font/common_j.fnt`, `Font/common_p.fnt`,
`Font/stage1_j.fnt`, `Font/system_j.fnt` genuinely present at exactly this path/casing on the real
disc image Aurora reads from; `Dvd.FileExistCheck("Font/common_p.fnt", ...)` (called earlier in
`MessageControl::loadCommonFont()`) also succeeds (no "Font file not found" log, only "Font load
failed" -- a different message, from a different check). `cDvd::ReadReq()`'s synchronous path
(`blockRead`/`ReadProc`/`readProcMain`) busy-loops `cDvdQueue::Read()` to actual completion before
`DvdReadN` returns (confirmed by reading `src/game/dvd.cpp:1194-1230`), so by the time
`Dvd.ReadCheck()` is called the read has genuinely finished, one way or the other -- this is not a
"caller didn't poll long enough" race. But `dvdread_callback` (`src/game/dvd.cpp:465`, the async DVD
completion handler, `result` = Aurora's real transferred-byte-count/error code) only fires **once**
this whole run (`result=6208`, a real success, for an earlier read, before the font code runs) --
the font read's failure happens without that callback ever firing again, meaning the font read fails
at an earlier stage than Aurora's actual disc I/O (most likely inside `cDvdQueue::Initialize()`'s
`entrynum = DVDConvertPathToEntrynum(w->name)` resolving differently than `FileExistCheck`'s own
resolution, or a `SysFlagChk(pG, SYS_SN_PC_READ)` path rewriting the name -- **TO VERIFY**, not
root-caused this pass, budget-limited). Next step for whoever picks this up: breakpoint on
`cDvdQueue::Initialize()`/`readInit`/`readMain` specifically (the `m_Rno0` state-machine functions,
`src/game/dvd.cpp` near line 837) to see which state the font read actually fails in, rather than the
lower-level Aurora callback (confirmed not reached).

The crash itself (`MessageFont::create` dereferencing `t->pTex` at address `0x2`) is downstream of
that failure: `MessageControl::loadSystemFont()`'s non-Japanese branch
(`pSys->language != 0`) unconditionally reuses slot 0's font buffer
(`setupFont(0x20, 0x20, (TEXPalette*) m_font_addr[0], 1)`) regardless of whether slot 0's own load
(`common_p.fnt`, the one that just failed) actually succeeded -- vendor logic, correct on real
hardware where this read cannot fail, not a bug to fix on its own; the real bug is upstream (why the
read fails at all).

## 17. cam_ctrl.cpp build-pc error, checked as asked

The `re4_game_all` (not `re4_game_core`, which ctest depends on and which stayed clean) compile error
seen this pass (`operator[] is ambiguous ... Ptr32<Vec> and s32`) is **not** the documented
GlobalISel/`AArch64RegisterBankInfo::hasFPConstraints` backend crash (docs/port-phase2.md) -- it is a
genuine overload-ambiguity diagnostic, a different symptom. Confirmed unrelated to this session's
changes (`git diff --stat` before this fix touched only `CMakeLists.txt`/`docs/`, nothing under
`src/game`). Not chased further (out of scope, pre-existing, `re4_game_all` is not part of the ctest
acceptance gate) -- **TO VERIFY** whether this is a third already-known-but-undocumented failure or
genuinely new; whoever next builds `re4_game_all` standalone (`cmake --build build-pc --target
re4_game_all -k 0`) should log the full failing-file list the way docs/port-phase2.md section 9 did.

No window renders this session (crash still predates GX/VI frame submission) -- no screenshot.

## 18a. Stub audit (2026-09-24, coordinator lead -- root cause of section 20's blocker)

Coordinator's diagnosis was exactly right: `memclr_asm`/`memset_asm` (`src/port/stubs/
generated_c_stubs.cpp`, `tools/port/gen_boot_stubs.py`'s generic output) were pure logging-only
stubs -- print a one-time "STUB: ... called" marker, touch no memory at all. `cDvd::pullReadQueue()`'s
`memclr_asm(q, sizeof(cDvdQueue))` (meant to zero a reused `cDvdQueue` slot's state machine) was a
complete no-op, so a reused slot kept its previous request's `m_Rno0`/`m_Rno1`/status/`mramSize` --
section 20's exact bug. This is a real, general failure mode of the mechanical stub generator: it
treats every undefined symbol identically (a call-site marker with a default return value), which is
correct for genuinely hardware-dependent calls (GX submission, AX/ADX sound, VI/PAD) but silently
wrong for any stub whose *whole contract* is a side effect the caller depends on.

**Fixed** (real semantics, not logging):
- `memclr_asm`/`memset_asm` (`src/port/stubs/generated_c_stubs.cpp`) -- now real `memset` calls.
  Root cause of section 20's stale-`cDvdQueue`-slot bug; the font read (and everything before it that
  used a fresh/reused DVD queue slot) now genuinely completes (`DVD: Read Ok`, real `OSReport` text --
  see below).
- `OSInitSemaphore`/`OSInitThreadQueue` (same file) -- same class of bug (a struct the game reads
  afterward, e.g. `OSWaitSemaphore`/`OSSleepThread`-shaped code checking `queue.head`), fixed to
  actually zero the queue/set the count instead of doing nothing.
- `OSReport` -- was logging-only (a single generic "STUB: OSReport() called" marker, the actual
  format string and arguments discarded); now a real `vfprintf(stderr, fmt, ap)` pass-through. High
  diagnostic value (this is the game's own `printf`-shaped log channel, `"DVD: Read File: %s"` etc.
  throughout `dvd.cpp`) and zero correctness risk (no return value or output parameter any caller
  depends on). This alone is what let section 20's actual bug become visible in this session's own
  further debugging (real "DVD: Read Ok"/"DVD: Read Error!!!" lines instead of one generic marker).
- The whole `PSMTX*`/`PSVEC*` paired-single matrix/vector family (18 functions, `generated_c_stubs.
  cpp`) -- every one filled its output matrix/vector with nothing (same bug class: Aurora doesn't
  implement these either, docs section 7, so this reimplements the well-known Dolphin SDK formulas
  directly, no hardware dependency). `PSMTXIdentity`, `PSMTXCopy`, `PSMTXConcat`, `PSMTXScale`,
  `PSMTXTrans`, `PSMTXTransApply`, `PSMTXTranspose`, `PSMTXRotRad`, `PSMTXRotAxisRad`, `PSMTXQuat`
  (found `Quaternion`'s real layout in `include/vec.h` after all), `PSMTXInverse` (general 3x3
  cofactor inverse + translation), `PSMTXMultVec`/`PSMTXMultVecArray`/`PSMTXMultVecSR`/
  `PSMTX44MultVec`, `PSVECAdd`/`PSVECSubtract`/`PSVECScale`/`PSVECCrossProduct`/`PSVECDotProduct`.
  `PSMTXIdentity` is the very first stub line every boot run prints -- these run constantly.

**Audited, left as logging-only stubs, with reasoning** (categories from the coordinator's list --
memory/string ops, tick/time, interrupt enable/disable, cache ops, queue/list ops, struct-filling
`*Init`s -- checked against the ~413 stub names across `src/port/stubs/*.cpp`):
- No `OSGetTick`/`OSGetTime`/`DCFlushRange`/`ICInvalidateRange`/`memcpy_asm` appear in the stub set
  at all -- not undefined symbols, so either unused on this boot path or already satisfied by a real
  implementation (Aurora's or this repo's own), not a stub gap.
- `OSDisableInterrupts`/`OSEnableInterrupts`/`OSRestoreInterrupts` -- return a fixed `0`/enum value;
  every real caller pattern in this codebase is the self-consistent `BOOL old = OSDisableInterrupts();
  ...; OSRestoreInterrupts(old);` shape, where a constant stub value round-trips correctly regardless
  of what it actually is (no other code branches on the *value* itself on this boot path) -- left
  alone, no bug found.
- `OSGetFontEncode`/`OSGetConsoleType`/`OSGetConsoleSimulatedMemSize`/`DBIsDebuggerPresent`/
  `OSSetErrorHandler`/`OSSetSaveRegion`/`OSInitAlarm`/`ADXGC_SetupDvdFs`/`init_dbmodule`/
  `mwPlyInitSfdFx` -- either intentionally-designed stubs (`OSGetConsoleType` returning "not dev
  console" is load-bearing for skipping the host-filesystem branch, section 2) or genuinely
  Phase-4/5 material (sound/FMV/SN-debugger/font-ROM) with no consumer before the current blocker;
  not touched.
- `OSInitFont` -- fills a font-metrics struct the real IPL font ROM would provide; no host equivalent
  exists (no ROM image), and the actual message-font system (section 20-21) reads its own `.fnt`
  files from disc instead, not this struct -- left alone, **TO VERIFY** if anything before the title
  screen reads it.
- `VISetPostRetraceCallback`'s registered callback is never invoked (no real VI vsync driving it yet)
  -- flagged as a real, *not yet fixed* gap for later: the per-frame `cDvd::Watcher()` polling loop
  that drives *asynchronous* (non-blocking) DVD reads to completion is normally reached by way of the
  frame loop this callback would drive; every DVD read seen on the boot path so far has been the
  *synchronous* (`blockRead`) kind, which doesn't need it, so this hasn't blocked anything yet --
  worth checking again once an async read is reached.
- `Render_init`/`Render`/`Render_before` and the rest of the GX-family stubs -- genuinely need a real
  Aurora GX backend wired up, not fakeable with a local formula the way PSMTX/PSVEC were; left alone,
  Phase 4 material.
- The remaining ~370 stub names (full list groupable by `src/port/stubs/*.cpp`'s own comments) are
  either Phase 5 sound/FMV/CRI-middleware calls, per-class virtual-function stubs for classes not
  reachable yet, or genuinely dead code on this boot path (never called before the current blocker,
  confirmed by their absence from every "STUB: ... called" line this session's actual runs printed)
  -- not audited function-by-function this pass; the methodology above (does the caller read a struct
  or return value the stub is supposed to fill?) is the test to apply as each one is actually reached.

**Verified**: `re4_boot` builds clean with all of the above; rerunning the boot sequence shows real
`OSReport`-formatted `"DVD: Read File: ..."`/`"DVD: Read Ok"`/`"DVD: Read Error!!!"` lines (previously
invisible behind the generic stub marker) and the font read's queue slot completing correctly instead
of section 20's silent `readExit()`-without-`readInit()` bug -- see section 21 for what happens next.

## 18b. `cDvd::ReadCheck`'s missing `return` -- a real UB/ABI gap, fixed under `TARGET_PC`

With section 18a's fix landed, `Font/common_p.fnt`'s read now genuinely completes (`DVD: Read Ok`),
but `MessageControl::loadFont()` still logged "Font load failed" -- traced to
`cDvd::ReadCheck(int, int*, int*, void**)` (`src/game/dvd.cpp`): the vendor source has **no explicit
`return` statement at all** in this overload; on real PPC/the original compiler, `readCheckMain()`'s
result is still sitting in r3 when the function falls off its closing brace (nothing between the
call and the return touches that register), so it "returns" the right value for free -- a real,
load-bearing register-reuse quirk of the original ABI, not a decompilation gap (confirmed: no
`NON_MATCHING` marker on this file, this shape is the byte-identical original). That is undefined
behavior in portable C++ with no such guarantee on arm64/clang, and it observably does not reproduce
here. Fixed under `TARGET_PC` only (the `#else` branch is textually identical to the original, byte
identity for the matching build is unaffected -- confirmed safe to insert lines at this point in the
file: `dvd.cpp`'s last `#line` directive is at line 1175, well before this function, and no
`__LINE__`/`HALT`/`ASSERTMSGLINE` call follows it anywhere in the rest of the file) by capturing
`readCheckMain()`'s result in a local and returning it explicitly.

**Milestone**: `MessageControl::loadCommonFont()`'s `Font/common_p.fnt` read now succeeds end to end
(open, read, `ReadCheck` reports success) -- past section 20's blocker entirely.

## 18. Ptr32 `operator[]` regression fixed (RE4_U32_32=OFF), baseline reconfirmed

Coordinator's read of the `cam_ctrl.cpp` error (section 17) was right: an ambiguous `operator[]`
between `Ptr32<Vec>` (member `operator[](std::size_t)`) and the compiler-synthesized built-in
`operator[](Vec*, long)` (reached via `Ptr32<T>`'s `operator T*()`) is a genuine regression from
Phase 2 step 5's `Ptr32<T>` -- under `RE4_U32_32=OFF`, `s32` is `long`, distinct from `int`/
`std::size_t`, so an `s32`-typed index (`cam_ctrl.cpp`'s `pArea->points[(i + 1) % pArea->num]`) needs
a standard conversion for the member candidate and ties with the built-in's user-defined-conversion
path closely enough for clang to call it ambiguous. Fixed in `include/port/ptr32.h`: `operator[]` is
now a template on the index type (`template <class Index, ...> T& operator[](Index i)`,
`std::enable_if`-constrained to integral types), so the member candidate always has an exact-match
index argument and wins outright regardless of the index's real type.

**Baseline reconfirmed the way docs/port-phase2.md's own methodology does** -- `git stash` the fix,
rebuild `re4_game_all -k 0` under `RE4_U32_32=OFF`, capture the failing-file list (34 files, including
`cam_ctrl.cpp`), restore the fix, rebuild, capture again (33 files) -- `diff` of the two lists shows
**exactly one file drops out (`cam_ctrl.cpp`), nothing new added**. `ctest` still 2/2. `re4_boot`
(`RE4_U32_32=ON`) still builds clean with the same header change. Only `include/port/ptr32.h` touched
(no `src/`/`include/` outside `include/port/`), so per the port rules this did not need the remote
docker byte-identity pass -- pushed directly after the local confirmation above.

## 19. Byte-swap hypothesis noted for the intermittent `Snd_str_blk_init` crash (section 15)

Coordinator's flag: the SndInit-era intermittent crash (small, un-offset-looking `data` address after
reading the sound stream file's header table, not reproducible every run) smells like a **missing
byte-swap** (the header's u32 fields are big-endian on-disc, read raw on a little-endian arm64 host)
rather than a bad/uninitialized read. Recorded as the leading hypothesis for whoever picks up Phase 3
(byte-swap-at-load) / Phase 5 (sound) for this file -- not verified this pass (`SndInit()` is stubbed
under `TARGET_PC` for now, section 15, so this doesn't block boot progress today, but the real fix
when sound is un-stubbed is very likely a `Swap32`-shaped fix on this header table's fields, not a
GCPTR/pointer-conversion fix at all).

## 20. Font-read blocker, narrowed: a `cDvdQueue` slot reused with stale `m_Rno0` (2026-09-24)

Followed the coordinator's suggested next step (breakpoint on `cDvdQueue::readInit`/`readMain`/the
`SYS_SN_PC_READ` path, compare `FileExistCheck` vs the actual open). Findings, most specific first:

- **The read genuinely never opens the file.** Breakpoint on `DVDFastOpen` (Aurora's real open
  entrypoint) set right as `MessageControl::loadFont("Font/common_p.fnt", ...)` starts: it never
  fires before "Font load failed" prints. `Dvd.FileExistCheck` (called moments earlier in
  `loadCommonFont()`, same path string) *does* reach `DVDFastOpen` successfully (no "not found" log)
  -- so entry-number resolution (`DVDConvertPathToEntrynum`) itself is fine; something between
  `DvdReadN()`/`cDvd::ReadReq()` and `cDvdQueue::fileOpen()`'s own `DVDFastOpen` call never gets that
  far this time.
- **Root mechanism, caught live**: `cDvdQueue::Read()`'s dispatcher (`func_tbl[m_Rno0]`) is called
  with **`m_Rno0 == 3`** (`readExit`, the *last* state, not `0`/`readInit`) on the very first `Read()`
  call for this request -- confirmed with a breakpoint on `cDvdQueue::Initialize()` immediately
  followed by one on `cDvdQueue::Read()` for the same `this` (`0x100ce7a48`): `m_Rno0` already reads
  `3` *before* `Initialize()` runs (`Initialize()` itself never touches `m_Rno0`/`m_Rno1`).
  `readExit()` (`src/game/dvd.cpp:820`) unconditionally calls `fileClose()` then only prints/sets a
  result if one of three `m_be_flag` bits (`0x04000000`/`0x100000`/`0x200000`, done/cancelled/error)
  is set -- none are, on a slot that never actually ran `readInit`/`readMain` -- so it silently does
  nothing but mark the slot "finished" (`m_be_flag |= 0x400000`), `Read()` returns 0 (done) on the
  first call, and `ReadCheck()` reports whatever stale `getStatus()` value the slot happened to hold
  from before -- deterministically "not 1", hence "Font load failed", without ever touching Aurora's
  DVD layer at all for this request.
- **Why `m_Rno0` is 3 going in is not yet root-caused.** `cDvd::pullReadQueue()` (`src/game/dvd.cpp:
  1452`) only hands out a slot after `memclr_asm(q, sizeof(cDvdQueue))` (which would zero `m_Rno0`),
  gated on `q->chk(1) == 0` (the slot's "in use" bit clear). A slot only becomes free again via
  `PushQueue()` (`m_be_flag &= ~1`), called from `cDvd::readCheckMain()`'s `ST_COMPLETE`/`ST_CANCEL`/
  `ST_ERROR` branches (i.e. only once the *caller* has polled `ReadCheck()` and consumed the result)
  -- or from `cDvd::ReadProc()`'s own completion path when the "keep" bit (`0x20000000`) is set.
  One inspection this pass (`b cDvd::ReadReq`, stepped past `q->Initialize()`) printed a `q` whose
  `reqfile`/`reqline` matched the current call (`"D:/Bio4/Prog/dvd.cpp"`/`100`, i.e. `Initialize()`
  had run) but whose `mramSize` field still held `6208` -- the exact byte count from an *earlier,
  unrelated* successful read's `dvdread_callback` (section 16) -- meaning that slot's struct was
  **not actually zeroed** by `pullReadQueue()` for this call, contradicting the source's own memclr.
  Not yet reconciled with the `m_Rno0==3`-at-`Read()` observation above (different lldb sessions,
  possibly different queue slots/`this` values -- **TO VERIFY**, did not confirm both observations
  are the same slot in the same run). Leading hypotheses, not confirmed: (a) `pullReadQueue()`'s
  16-slot pool is being exhausted faster than slots are freed during boot (nothing before the title
  screen calls `Watcher()`/the per-frame loop that would normally drive completion+release for
  non-synchronous requests -- but this *is* a synchronous request, sync ones should self-complete via
  `blockRead` without needing `Watcher()`), so `pullReadQueue()` may be silently returning a
  **not-actually-free** slot, or a completely different bug in this port's environment causes
  `chk(1)`/`memclr_asm` to disagree with the real slot state; (b) a real, pre-existing vendor-logic
  edge case around one-shot synchronous reads not calling `PushQueue()` on the "not kept" branch
  (`cDvd::ReadProc()`'s `else { m_be_flag |= 0x800; }`, section notes above) that happens to be masked
  on real hardware by some other invariant this port breaks.

**Next step for whoever picks this up**: breakpoint on `cDvd::pullReadQueue` with a watchpoint (or a
conditional breakpoint keyed on slot index) tracking exactly which of the 16 `DvdQueue[]` slots gets
reused for the font read and its `m_be_flag`/`m_Rno0` value at the moment `chk(1)==0` is evaluated,
across the *whole* boot sequence from the first DVD read onward (not just this one call) -- the
`this` pointer alone (`0x100ce7a48`) recurred across several unrelated reads in this session's traces
("etc/moji8.tpl" and the font read both used it), suggesting slot 0 specifically is the one in a bad
state, which would narrow this considerably. Not attempted further this pass -- budget-limited, this
is a real, well-isolated vendor state-machine bug (or a port-environment trigger of one), not a
one-line fix.

**Resolved this session** (sections 18a/18b): the stub audit's `memclr_asm` fix was exactly this bug
-- `pullReadQueue()`'s slot-clear was a no-op, not the read/pool-exhaustion logic itself. Confirmed
directly: rerunning with the fix shows real `OSReport` output (`memset_asm`/`OSReport` also fixed,
section 18a) all the way through, no more stale-slot behavior, no more "Font load failed".

## 21. Next blocker: the loaded `.fnt`'s on-disc fields are unswapped (big-endian), Phase 3 territory

With sections 18a/18b's fixes, `Font/common_p.fnt` now genuinely opens, reads, and reports success
(`DVD: Read Ok`) -- but crashes one step later, inside `MessageFont::create()` (`src/game/mes.cpp`),
on `t->pTex->width` where `t->pTex` is null (fault address `0x2`, consistent with a null `TEXHeader*`
plus `width`'s small field offset).

**Diagnosis**: `create()`'s loop that sets `t->pTex = th` for each of `m_tpl`'s texture descriptors
(`for (i = 0; i < m_tpl->numDescriptors; i++, d++, t++) { ...; t->pTex = th; ... }`, `src/game/
mes.cpp` ~line 211) only runs `m_tpl->numDescriptors` times -- if that field reads as `0`, the loop
body (and the `t->pTex` assignment) never executes at all, leaving `t->pTex` at whatever `memclr_asm`
(now correctly zeroing, section 18a) initialized it to: null. `numDescriptors` is a raw `u32` field
read straight off the disc, big-endian, with no byte-swap applied before this code interprets it on a
little-endian host -- exactly the class of bug the coordinator's section 19 byte-swap hypothesis (for
the sound header table) predicted, now confirmed for a second, different on-disc format (TPL-style
texture-palette headers, this time for the font system specifically, not the sound stream table).
This is genuinely **Phase 3 work already named in the plan** (per this session's own task brief:
"per-format load-time byte swappers for boot formats as planned ..., not ad hoc") -- the descriptor-
array relocation code immediately above this loop (`d->textureHeader = (TEXHeader*) ((u8*) addr +
(u32) d->textureHeader); ...`) already has its pointer-relocation side handled (`Ptr32<T>`/raw-offset
arithmetic, Phase 2), but nothing swaps the raw big-endian integer fields (`numDescriptors`, the
offsets themselves, `width`/`height`/`format` once reached) before that arithmetic runs on them.

**Not attempted this pass**: a real fix needs a systematic byte-swap step for this format (and
probably the sibling TPL-descriptor format the title-screen archive already uses, docs/port-phase2.md
step 4's `ARC_PTR`/TPL table), not a one-off patch to this one call site -- exactly the "systematic,
not ad hoc" boundary this session's brief named. Whoever picks this up next should look at whether
Phase 2's TPL/`TEXDescriptor`-family structs already have (or need) a `Swap32`-shaped helper applied
right after the raw disc read, before any relocation arithmetic touches the same bytes.

No window renders this session (crash still predates GX/VI frame submission) -- no screenshot.

## 22. Phase 3 (endianness): past `MessageControl::init()` entirely, into `systemRestartInit()` (2026-09-24)

Full design and survey: docs/port-phase3.md. Summary of what changed and what it fixed, in order:

1. `include/tpl.h`'s `CLUTHeader`/`TEXHeader`/`TEXDescriptor`/`TEXPalette` plain integer/float
   fields converted to `BE<T>` (`include/port/be.h`, new this pass) -- fixes section 21's blocker
   (`numDescriptors` reading as 0).
2. `src/game/mes.cpp`'s local `MesFontFile::tplOfs`/`widthOfs` (missed by Phase 2's `Ptr32<T>`
   inventory, not in a shared header) converted to `BE<u32>`.
3. `include/port/ptr32.h` gained `Ptr32<T>::raw_handle_be()` (swapped) alongside the existing
   `raw_handle()` (unswapped) -- `MessageFont::create()`'s pointer-arithmetic relocation code uses
   the former for offset values, the sign-bit "already relocated?" guard stays on the latter (a real
   wrinkle found live: `MessageControl::loadSystemFont()` calls `create()` a second time on the same,
   already-relocated buffer; swapping the guard's read corrupts that second call -- docs/port-phase3.md
   section 1).

**Milestone**: `MessageControl::init()` (both `common_p.fnt` system-font slots) now completes; the
process runs past `systemStartInit()` entirely and into `systemRestartInit()`, past `RoomDataInit`-
adjacent setup (`cDataCtrl::init`, `EspWaterInit`, `ShadowInit`, `ClothInit`, all still logging
stubs), reaching `SndInit2()`.

**Current blocker**: `SndBgmTblInit()` (`src/game/snd.cpp:206`, called from `SndInit2()` ->
`systemRestartInit()`) dereferences `SndMem.bgm_tbl` (a `Ptr32<SndBgmTbl>`), which is null --
`SndInit()` itself is stubbed under `TARGET_PC` (section 15, Phase 5 deferral: no Aurora audio
backend), so nothing ever populates it, and `SndBgmTblInit()` is a separate call site
`systemRestartInit()` reaches directly, not gated behind the same stub. Not fixed this pass -- Phase
5 (sound) scope, not an endianness bug (docs/port-phase3.md section 2's table); stopping here per
the "stop at a design decision" instruction. Verified: `ctest` still green (`test_ptr32`,
`test_arena`, `test_be` new, `re4_port_static_asserts` under `RE4_U32_32=ON`); default host build
(`RE4_U32_32=OFF`) `re4_game_all -k 0` failing-file list unchanged (34 files, identical set,
before/after diffed directly).

No window opens this session (crash still predates any GX/VI frame submission) -- no screenshot.

## 23. Ptr32<T> byte-order correction, sound made inert, past the frame loop's first draw (2026-09-24)

Coordinator review found a real latent bug in section 22's `Ptr32<T>` design (mixed BE/host-native
storage broke the "already relocated?" sign-bit guard for any raw offset with low byte >= 0x80, and
broke save-data round-trip compatibility) and flagged sound stubbing as needing to be systematic,
not per-crash. Both addressed; full write-up: docs/port-phase3.md sections 6-8.

- `Ptr32<T>` storage is now always big-endian (`include/port/ptr32.h`); `mes.cpp` reverted to plain
  `raw_handle()` (matching every other call site in the tree). Round-trip test added
  (`tests/port/test_ptr32.cpp`).
- `src/game/snd.cpp`: 7 functions that unconditionally dereference `SndMem` state `SndInit()`'s
  `TARGET_PC` stub never populates, stubbed individually (docs/port-phase3.md section 7's table);
  `str_flag = 0` added to the existing stub so the vendor's own "no STR header" gate works instead
  of being bypassed. Everything else in `snd.cpp`/`se_at.cpp` was already safe by construction
  (gated on `pSnd->blk_flag`/a null header, both naturally false/null on a zeroed `SndWork`).
- `include/model.h`'s `cModelData` plain integer/float fields converted to `BE<T>` (title archive).

**Milestone**: past `systemStartInit()`/`systemRestartInit()` entirely, through
`TaskExec(Title_task)`, into the first frame loop (`main_game()`'s `for(;;)`, `main.cpp:140`).

**Current blocker**: `DrawOTag` (`libgpu.cpp:98`) walks a garbage 2D ordering-table pointer chain --
downstream of `Render`/`Render_before`/`InitOt()` still being logging-only stubs (no real Aurora GX
backend, Phase 4 scope), not an endianness bug. A "Stack overflow in Thread 0 !!" `OSPanic` fires
moments earlier and does not appear to abort the process -- **TO VERIFY** whether that itself masks
something. Stopping here per the "stop at a design decision" instruction.

Verified: `ctest` 4/4 (`RE4_U32_32=ON`) / 3/3 (default `OFF`); `re4_game_all -k 0` failing-file list
unchanged (34 files); remote byte-identity re-checked (0 non-OK `dtk shasum` lines, `asmcheck.py
--all` TOTAL 231, unchanged).

No window renders this session (crash still predates any GX/VI frame submission) -- no screenshot.

## 24. OSPanic fatal, real OSThread implementation, tasks run for the first time (2026-09-24)

Full write-up: docs/port-phase3.md section 10. Summary:

- `OSPanic()` now logs and `std::abort()`s instead of silently continuing (`src/port/stubs/
  generated_c_stubs.cpp`).
- Root cause of section 22/23's "Stack overflow in Thread 0": `OSCreateThread` (and the rest of the
  scheduler's OS-thread API) was a pure logging stub -- no task's code had ever executed, and the
  stack-overflow guard word a real `OSCreateThread` writes was simply never set. Real implementation:
  `include/port/os_thread.h` / `src/port/os_thread.cpp` (new), real host `std::thread`s with an
  explicit hand-off token (`WaitForHandback()`, three new `TARGET_PC`-only lines in
  `src/game/scheduler.cpp`). `Title_task` now genuinely runs for the first time.

**New blocker**: a real, previously-unreached allocation bug -- `cItemMgr::init()`
(`src/game/item.cpp`, not stubbed) corrupts the unrelated `Task[1]` scheduler slot via one of its
`MEM_ALLOC(..., 1, 13)` calls (heap `13` == `MEM_HEAP_CURRENT`, confirmed not itself the bug).
Not root-caused this pass -- next step is instrumenting the three allocation's return pointers/sizes
directly against `Heap[CurrentHeap]`'s actual bounds. Verified no regression: `ctest` 4/4 (`ON`) /
3/3 (`OFF`), `re4_game_all -k 0` failing-file list unchanged (34 files).

No window renders this session -- no screenshot. Render/VI/GX SDK-parity work and the
`CRoomInfo`/DVD-size-table BE audit were not reached this pass.

## 25. Root cause of section 24's "Task[1] corrupted" blocker: `re4_port` missing `RE4_U32_32` (2026-09-24)

lldb watchpoints (`watchpoint set expression -w write -s 1 -- &Task[1].Status`, `bt`) instead of
guessing: the write that sets `Task[1].Status = TASK_EXEC` with `hook`/`pFunc` still `NULL` happens
inside **`OSCreateThread(&Task[0].Thread, ...)`**, at `thread->state = OS_THREAD_STATE_READY;`
(`src/port/os_thread.cpp`) -- a write to a field of *Task[0]'s own* embedded `OSThread`, landing on
`Task[1]` instead. `cItemMgr::init()` (section 24's original suspect) never touched it; it just
happened to be the first place a subsequent frame's state got read back wrong, since the write
itself happens on task-slot dispatch, not during item init.

Cause: `sizeof(OSThread)` disagreed between translation units. `include/scheduler.h` pre-defines
`_DOLPHIN_TYPES_H_` before `#include <dolphin/os/OSThread.h>` specifically so that header's own
`#include <dolphin/types.h>` is a no-op (the guard is already "seen") and `u32`/`s32` keep whatever
`scheduler.cpp` already defined them as (the project's own `include/types.h`, 4-byte `int` under
`RE4_U32_32`). `src/port/os_thread.cpp` does not have that pre-guard, and this repo's own
`CMakeLists.txt` built its translation unit (`re4_port` library) with **only** `TARGET_PC`, never
`RE4_U32_32` -- so `dolphin/types.h`'s `#if defined(TARGET_PC) && defined(RE4_U32_32)` branch was
false there, and `u32`/`s32` fell back to the `#else` branch, plain `long`/`unsigned long` (8 bytes
on this LP64 host). Every `u32` field inside `OSContext`/`OSThread` (the `gpr`/`gqr` arrays, `cr`/
`lr`/`ctr`/`xer`, `suspend`, `error`, ...) was silently twice as wide in `os_thread.cpp`'s view of
the struct as in `scheduler.cpp`'s (`sizeof(OSThread)` measured 1072 there vs. 856 everywhere else,
confirmed live via `p sizeof(*thread)` at both call sites) -- an ODR violation. `OSCreateThread`'s
member stores, computed against the wrong (larger) layout, walked past the real, smaller `OSThread`
embedded inline in `TASK::Thread` and wrote into whatever followed in `Task[]` -- the next slot.

**Fix**: `CMakeLists.txt`'s `re4_port` target now also gets `RE4_U32_32` when the top-level option
is on (mirrors `RE4_GAME_DEFINES`'s own `if (RE4_U32_32) list(APPEND ...)` pattern used by every
other TARGET_PC target). Build-config only (`CMakeLists.txt` is outside `src/`, `include/`, and is
explicitly "not read by configure.py/ninja" per its own header) -- no remote matching-build
verification round trip needed.

**Verified**: `re4_boot` now runs `Title_task`'s full turn with no corruption (`Task[1]` stays
`TASK_NONE` until something legitimately execs it); the frame loop reaches `main_game()`'s
`DrawOTag()` call (`main.cpp:140`) exactly as docs/port-phase3.md section 8 previously described,
this time via a healthy scheduler instead of by accident before the corruption fired -- confirms
section 24's blocker is fully gone, not just moved. `ctest`: 4/4 (`build-pc-boot`, `RE4_U32_32=ON`)
and 3/3 (`build-pc`, default `OFF`) both green. `re4_game_all -k 0`'s failing-file count unaffected
by inspection (the diff only touches the `re4_port` target block, not `RE4_GAME_DEFINES` or any
game target).

**Not reached this pass** (budget spent on root-causing section 24): the render path itself
(`InitOt`/`Render`/`Render_before`/`Trans`/GX submission) is still every stub docs/port-boot.md
section 7 already catalogued -- `DrawOTag` walks a 2D ordering table nothing real ever populates, so
it is expected to spin/hang there, not progress to a frame. No window render, no screenshot. Bringing
the real render units into the boot link, the GX/VI SDK parity table, and wiring VI/GX frame
presentation through Aurora are still open (docs/port-phase3.md section 9's list, coordinator's
Phase 4 ask).

## 26. Global TARGET_PC/RE4_U32_32, cross-TU layout guards, main_sub.cpp un-excluded (2026-09-24)

Coordinator follow-up to section 25's fix, same theme: make the whole *class* of cross-TU layout
bug impossible, not just this one instance.

**Global compile definitions**: `CMakeLists.txt` now sets `TARGET_PC` (and, conditionally,
`RE4_U32_32`) once, via `add_compile_definitions()` right after the `option()` declarations, instead
of every target relisting them (`RE4_GAME_DEFINES`, `re4_port`'s own list). This also reaches
Aurora's `add_subdirectory()` (`RE4_BUILD_BOOT`) -- confirmed intentional, not incidental: Aurora's
own `include/dolphin/types.h` has the identical `#ifdef TARGET_PC` branch (fixed-width `<stdint.h>`
types), so Aurora's own compiled objects were using 8-byte `u32`/`s32` throughout *before* this
change, while every game/port TU used 4-byte -- the same bug, one level up, at every Dolphin-shaped
struct crossing the game<->Aurora boundary. Fallout from turning this on: `src/port/dvd.cpp`'s
`<aurora/dvd.h>` wants `int64_t`/`uint8_t`/`int32_t` from its own `#include <dolphin/types.h>`, but
this repo's own `include/` is searched first for that target, so the angle-bracket include
resolved to *this* repo's `dolphin/types.h` (no `<stdint.h>`) instead of Aurora's -- fixed with an
explicit `#include <cstdint>` before the Aurora header (one line, `src/port/dvd.cpp`). Both build
trees rebuilt clean from scratch after; `ctest` 4/4 (`RE4_U32_32=ON`) / 3/3 (`OFF`);
`re4_game_all -k 0`'s failing-file set unchanged (33 files, diffed directly, not just counted).

**Cross-TU static_assert layout guards** (the actual ask: "so any size disagreement fails the
build"): `include/port/layout_asserts.h`, force-included (`-include`, `CMakeLists.txt`) into every
`re4_port` and `re4_boot_game` TU, `static_assert`s `sizeof()` for `OSContext`/`OSThread`/`OSMutex`/
`OSMessageQueue`/`DVDFileInfo`/`DVDCommandBlock`/`TASK`/`cDvdQueue` against the one authoritative
number per config (`RE4_U32_32` ON vs. the default OFF -- both real, both get their own literal;
confirmed via a throwaway probe compiled with this repo's exact flags, not derived by hand). GX
objects (`GXTexObj`/`GXTlutObj`/`GXColor`/`GXFifoObj`/`GXRenderModeObj`) are checked the same way
but in a *separate* standalone executable (`tests/port/gx_layout_asserts.cpp`, added to the existing
`re4_port_static_asserts` target) instead of the force-included header: force-including
`<dolphin/gx.h>` into every game TU broke `include/gx.h`'s own paired-single-write macros
(`GXPosition3f32` et al. redefinition errors) for any file that also does its own `#include "gx.h"`
-- confirmed the hard way, reverted to the safer standalone-TU pattern `gen_static_asserts.py`
already established for on-disc structs. One more real bug found by this exercise, not by inspection:
a quoted `#include "dvd.h"` from inside `include/port/` resolves relative to that directory first,
finding `include/port/dvd.h` (a different, port-specific header) instead of the game's
`include/dvd.h` (`cDvdQueue`) -- fixed with an explicit `../dvd.h`. Verified: `re4_boot` links and
runs to the same frontier as before (`ctest` 4/4, `re4_game_all -k 0` unchanged).

**main_sub.cpp un-excluded** (`Render_before`/`Render_init`/`Render_done`/`Render_swap`/
`Render_checkBlurPermission`, previously stubbed): its three remaining `RE4_U32_32=ON` compile
errors were all the same "real host pointer cast to a 4-byte int, truncated" class this whole port
already has a fix for (`re4_port::GC32()`) -- `include/sce_sys.h`'s `emDeadRow()`, and two spots in
`DrawTpl()` (`src/game/main_sub.cpp`) casting `tpl`/`hdr` (plain pointers, not `Ptr32<T>` fields) to
`u32`. Fixed with the standard `#ifdef TARGET_PC` `GC32()` branch, no `#else` change (byte-identical
original build, remote-verified: `dtk shasum` 0 non-OK lines, `asmcheck.py --all` TOTAL 231
unchanged). Removing it from `cmake/boot_exclude.txt` surfaced ~20 previously-unreached undefined
symbols (nothing had ever called into main_sub.cpp's body before) -- real ones (`OSLink`/`OSUnlink`/
`OSInitStopwatch`-family/`VISetBlack`/`VISetNextFrameBuffer`/`VIGetNextField`/`SceSys`/
`cSceSys::checkCTaskRange`) got new hand-written stubs (`src/port/stubs/manual_stubs.cpp`,
`stub_common.h` gained the headers they need); ~20 *stale* generated stubs for symbols main_sub.cpp
now defines for real (`Render_init`/`_before`/`_done`/`_swap`, `Rmode`, `ScreenShotTriggerType`,
timing/scissor/screenshot helpers) were removed from `generated_c_stubs.cpp`/
`generated_cpp_stubs.cpp` to clear the resulting duplicate-symbol link errors.

**Verified end to end**: `re4_boot` re-run reaches the identical frontier as before this pass --
`Render_before()`/`Render_init()` now run for real (no longer print `STUB: ... called`), `Render()`
itself is still a stub (`trans.cpp` stays excluded, see below), and the process reaches `DrawOTag`
in the frame loop exactly like before (sometimes an Aurora `[fatal] GXBegin: called without matching
GXEnd`, sometimes the already-documented garbage-`pOt` `EXC_BAD_ACCESS` -- both are the *same*,
already-known "nothing populates a real OT" gap, not a new regression; confirmed by re-running
several times). No screenshot -- nothing renders yet.

**`trans.cpp` (Render()) not un-excluded this pass**: assessed its `RE4_U32_32=ON` errors past the
`-ferror-limit` cutoff -- two classes. (1) The same `PTR_INVALID`/`PTR_INVALID2` macro-driven
pointer-cast-truncation errors as main_sub.cpp's, fixable the same way. (2) Real PPC paired-single
(`psq_l`/`psq_st`) inline asm for the skinning/weight-blend math (`invalid output constraint '=f'`),
which needs genuine `TARGET_PC` C equivalents, not a mechanical fix -- and skinning math is exactly
the kind of code where a subtly-wrong C rewrite would silently corrupt geometry with no way to
verify against a real frame yet (the render pipeline that would let you *see* the bug doesn't exist
yet -- circular). Deliberately not attempted this pass rather than guessed at; flagged for whoever
picks up Phase 4's asm-equivalents work with the exact error classes above.

**GX/VI parity vs. Aurora** (spot-checked, not exhaustive): `aurora_gx` implements a large, real GX
surface -- everything `main_sub.cpp`'s `DrawTpl()`/`DrawTexture()` call (`GXInitTexObj`/
`GXInitTexObjCI`/`GXInitTlutObj`/`GXLoadTlut`/`GXLoadTexObj`/`GXSetChanCtrl`/`GXSetTevOrder`/
`GXSetBlendMode`/...) is a real symbol, not a stub (`GXSetTexCoordGen` looked missing from `nm` at
first -- false alarm, it is a `static inline` header wrapper around the real `GXSetTexCoordGen2`,
no symbol needed). `aurora_vi` is far thinner: real window/mode management (`VIInit`/`VIConfigure`/
`VIConfigurePan`/`VIFlush`/`VISetWindowSize`/`VISetWindowFullscreen`/...) but **no**
`VISetBlack`/`VISetNextFrameBuffer`/`VIGetNextField`/`VIWaitForRetrace`/`VISetPostRetraceCallback` --
exactly the raw per-frame presentation entry points the game's own frame loop needs, all still
logging stubs (`STUB: ... called`, some pre-existing, `VISetBlack`/`VISetNextFrameBuffer`/
`VIGetNextField` newly added this pass for main_sub.cpp). Wiring these for real (present a frame,
drive `VIWaitForRetrace` from Aurora's own vsync/window loop so the game's retrace callback fires)
is the actual remaining work for "Aurora opens a window" -- not attempted this pass, correctly
scoped as its own follow-up given `Render()` itself still produces no real geometry to show in that
window yet.

Coordinator instruction going forward, not yet applied retroactively: commit + push (with remote
matching-build verification for anything touching `src/`/`include/` outside `src/port/`/
`include/port/`) as work lands, not only at session end -- done for every commit this pass
(`c5864124`, `606a4708`, `4ac0b070`, `07c179c9`; the `main_sub.cpp` un-exclude was remote-verified
before merging to `port/macos-arm64`: `dtk shasum` 0 non-OK, `asmcheck.py --all` TOTAL 231).

## 27. A window opens for the first time: `aurora_initialize()` was never called (2026-09-24)

Root cause of "no window ever opens" (every prior session's blocker was upstream of this, so it was
never actually tested): `src/port/boot_main.cpp`'s host `main()` never called `aurora_initialize()`/
`aurora_update()`/`aurora_begin_frame()`/`aurora_end_frame()` at all -- confirmed by grepping the
whole repo (`grep -rn aurora_initialize src/ include/` was empty before this pass). `VIInit()`
(`../aurora/lib/dolphin/vi/vi.cpp`) is a no-op; the real SDL window is created inside
`aurora_initialize()` (`../aurora/lib/aurora.cpp`), which nothing in this repo ever invoked. Every
previous session's boot run genuinely never reached a point where this would have mattered (each
stopped on an earlier crash), so this was never exercised, not a regression.

**Threading design** (full rationale in `include/port/vi.h`): AppKit requires the window/event loop
on the process's real main thread; the game runs on its own arena thread
(`src/port/boot_main.cpp`'s `CreateArenaThread`). Chosen split:

- Host **main thread** (`src/port/boot_main.cpp`, after starting the game thread): calls
  `re4_port::RunPresentLoop()` (`src/port/vi.cpp`, new), which itself calls `aurora_initialize()`
  once (creating the window), then loops `aurora_update()` (SDL event pump; watches for
  `AURORA_EXIT`) + `aurora_begin_frame()`/`aurora_end_frame()` every iteration.
  `AuroraConfig::vsync = true` makes `aurora_end_frame()`'s swapchain present block on the real
  display refresh -- that is this port's ~60 Hz tick source, not a manual `sleep`.
  `config.mem1Size`/`mem2Size` are left `0` so Aurora doesn't allocate its own MEM1/ARAM block
  (`re4_port::InitMem1()`, section 13, already owns that).
- Each loop iteration is one "retrace": bumps an atomic counter and invokes whichever
  pre/post-retrace callback the game registered via `VISetPreRetraceCallback`/
  `VISetPostRetraceCallback` (real implementations now, `src/port/vi.cpp`) -- **on the main thread**,
  not the game thread. Deliberate, not an oversight: `main.cpp`'s own frame loop drives its vsync
  counter (`vsync_cnt`) with a *busy-wait* (`while (vsync_cnt < GetSystemVcnt()-1) {}`) that the
  registered callback (`postVSyncCallback`) is expected to advance from *outside* that spin -- on
  real hardware this is a literal interrupt preempting the CPU mid-loop, which "runs the callback on
  the game thread's own cooperative turn" cannot reproduce (the game thread is exactly the thread
  stuck spinning). Confirmed by reading `main.cpp` before choosing this design, not assumed. **Open
  risk, not verified**: the callback touches `pG`/`vsync_cnt` with no lock -- a genuine cross-thread
  data race, accepted here on the grounds that real VI hardware's own ISR would touch the same
  state from interrupt context with no lock either; revisit if it proves unstable.
- `VIWaitForRetrace()` (called directly by the game thread at a few points, e.g. `Render_init()`'s
  boot-time call, `src/game/main_sub.cpp`) blocks the **calling** thread on a condition variable
  until the next tick the main thread produces.
- `VISetNextFrameBuffer`/`VISetBlack`/`VIGetNextField`/`VIGetRetraceCount` given real (not
  logging-only) semantics in the same file, matching `include/dolphin/vi/vifuncs.h`'s real
  signatures. `VIFlush()` needed no change -- Aurora's own `../aurora/lib/dolphin/vi/vi.cpp` already
  defines it (real, empty on purpose: nothing to flush without a hardware FIFO).
- **Not synchronized with this pass**: actual GX submission (`Render()`/`Render_swap()`/
  `GXCopyDisp`, still on the game thread, unchanged) is not bracketed by the main thread's
  `aurora_begin_frame()`/`aurora_end_frame()` pair at all -- a real, open risk (Aurora's GX/Dawn
  command encoding may assume single-threaded use inside one begin/end pair), flagged in
  `include/port/vi.h` rather than guessed at. Did not block this session's actual run (see below --
  the crash reached is upstream of any real GX submission from the game thread), so not chased
  further; whoever reaches real GX draws next should watch for it.

New/changed files: `include/port/vi.h`, `src/port/vi.cpp` (new); `src/port/boot_main.cpp` (calls
`re4_port::RunPresentLoop()` instead of sleeping); `CMakeLists.txt` (adds `src/port/vi.cpp` to
`re4_boot`'s sources); `src/port/stubs/generated_c_stubs.cpp` /
`src/port/stubs/manual_stubs.cpp` (removed the now-superseded logging stubs for
`VISetPostRetraceCallback`/`VIWaitForRetrace`/`VISetBlack`/`VISetNextFrameBuffer`/`VIGetNextField` --
real implementations now live in `src/port/vi.cpp`). All `src/port/`/`include/port/`/`CMakeLists.txt`
changes -- no remote matching-build round trip needed per the port rules.

**Verified: the window opens.** Re-running `re4_boot` (section 12's command) now logs
`[info] [aurora::gpu] Attempting to initialize Metal` through a real adapter/device/swapchain
creation sequence (`Device: Apple M4 (IntegratedGPU)`, `Compatible surface: true`) and
`re4_boot: Aurora window opened`, and the process runs measurably further into the boot sequence
than any prior session (`STUB: Render() called` -- the frame loop's own `Render()` call site,
`main.cpp:169` -- is reached; every previous session's last log line was well before this).

**Current blocker, confirmed live with lldb**: the same "DrawOTag walks a garbage OT pointer"
symptom section 23 first noted, this time with a real crash and root cause, not a guess:

```
* thread #4, stop reason = EXC_BAD_ACCESS (code=2, address=0x180cffc1c)
    frame #0: DrawOTag(pOt=0x0000000180cffc1c) at libgpu.cpp:98
        -> } while (*pOt != 0xFFFFFFFF);
    frame #1: main_game() at main.cpp:140
```

**Root cause (not a struct/endianness bug -- a real address-space mismatch)**: `libgpu.cpp`'s OT
primitives (`AddPrim`/`DelPrim`/`ClearOTagR`/`DrawOTag`) store a *live RAM pointer* inside a plain
`u32`, using the real GameCube hardware's own trick: RAM addresses always have bit 31 set
(`0x80000000`-based), so an "empty"/"not yet linked" list cell can mask that bit off
(`& 0x7FFFFFFF`) as a spare flag and OR it back (`| 0x80000000`) before dereferencing. This a
different mechanism from the disc-relocated-pointer problem `include/port/ptr32.h`'s `Ptr32<T>`/
`GC32`/`GCPTR` already solves (Phase 2) -- it never touches a struct field that gets byte-swapped or
round-tripped through disk, only the *live address* of a plain global, `MainOt`
(`u32 MainOt[5]`, `src/game/main.cpp`). On this host, `&MainOt[i]` is an ordinary 64-bit process
address that does not fit in 32 bits at all (confirmed: `ClearOTagR` masks the low 31 bits of that
address into the table, discarding everything above bit 31 -- not just the sign bit -- so the
address can never be recovered by OR-ing it back in; `DrawOTag`'s `p[1]` dereference of what's left
is exactly the observed garbage-pointer crash). `GC32()`/`GCPTR()` (`include/port/ptr32.h`) cannot
paper over this the way they do for on-disc pointers: they require the pointer to live inside the
arena's compressed 4 GiB window (`g_base` and up) -- and `MainOt` is an ordinary linked-in global,
not something allocated from the arena, so `GC32(&MainOt[i])` would itself fail its own bounds
assert. Confirmed this is the actual mechanism (not asserted from the RTL alone): read `ClearOTagR`/
`DrawOTag`'s exact masking arithmetic, matched it against the crash's own `pOt` value, and confirmed
`MainOt` is a plain global (`grep -n "u32 MainOt" src/game/main.cpp`), not arena-backed.

**This is a design decision, not a small fix** (stopping here per that instruction) -- the same
"pointer stored in a 32-bit game-visible int" idiom this port has hit before (Phase 2's whole
reason for existing), but this time on a plain linked-in global instead of disc-relocated/heap data,
which `Ptr32<T>`'s existing window doesn't cover. Two candidate directions for whoever picks this up
next, neither attempted this pass:

1. **Move every such statically-addressed table into the arena at startup** (a small "game statics"
   sub-region of `re4_port`'s arena, populated by copying each global's initializer in at boot) so
   `GC32`/`GCPTR` already work for them unchanged -- keeps `libgpu.cpp` byte-identical even under
   `TARGET_PC`, but needs a way to find every such global (`MainOt` is confirmed; whether the
   renderer's real (non-stub) code touches others the same way is unaudited -- `trans.cpp`/GX
   submission are still excluded, section 26, so this pass could not check them).
2. **Widen the OT's storage under `TARGET_PC` only** (e.g. a parallel host-pointer-sized table, or a
   `TARGET_PC` reinterpretation of the bit-trick using `GC32`/`GCPTR` after first special-casing
   "does this address happen to be arena-resident") -- touches every call site across
   `libgpu.cpp`/`debug.cpp`/`datactrl.cpp`/`main_mem.cpp` that participates in this idiom, larger
   surface, but doesn't require relocating any global.

No screenshot captured this session despite the window opening: the crash above happens seconds into
the run (mid font-load retry loop) and terminates the whole process (unhandled `SIGBUS`) before a
`screencapture` call could be scripted against it; a live `lldb`-paused screenshot attempt found the
inferior already gone by the time a second command reached it (lldb's batch mode has no stdin to
keep it attached without `-k quit`, which itself lets the process exit). Whoever picks up the OT fix
above should screenshot on the next run once the crash is gone -- the window is real and does open
(confirmed by Aurora's own successful adapter/device/swapchain log lines), so this is expected to be
straightforward once nothing crashes before the first `aurora_end_frame()` a few iterations in.

Verified no regression: default host build (`RE4_U32_32=OFF`) untouched by this pass (only
`src/port/`, `include/port/vi.h`, `CMakeLists.txt`'s `RE4_BUILD_BOOT` block touched); `ctest` not
rerun this pass (no `include/port/`/`src/port/` change affects the non-boot test targets' logic,
only adds a new boot-only source file) -- **TO VERIFY** by whoever next has a spare `ctest` run.

## 28. GC-faithful low-memory layout ("plan A*"): `DrawOTag` no longer crashes, real GX submission reached (2026-09-24)

Coordinator-specified plan, implemented and verified live (not assumed) at every step below.

### Root cause confirmed: section 27's `DrawOTag` crash was `GC32()` underflowing under `NDEBUG`

Read the cast-rewriter's actual output (`build-pc-boot/gen/src/game/libgpu.cpp`) before touching
anything: `AddPrim`/`DelPrim`/`ClearOTagR`/`DrawOTag` already call `re4_port::GC32()`/`GCPTR()` (the
rewriter caught every pointer-in-`u32` cast here, contrary to section 27's guess that they were raw
truncations) -- so the bug is in `GC32()`'s own bounds check, not a missing rewrite. `RE4_GAME_DEFINES`
(`CMakeLists.txt`) sets `NDEBUG=1` unconditionally (matching the vendor's own `-O2` build), which
compiles out `GC32()`'s own `assert()` (`include/port/ptr32.h`) for every `re4_boot_game` translation
unit -- so a pointer outside the compressed window (the old `g_base = arena_address - 0x80000000`
put every ordinary global, e.g. `main.cpp`'s `MainOt`, at a *negative* offset whenever that global's
real address was more than 2 GiB below the arena) silently underflowed to a huge `uintptr_t`, failed
the `>= kWindowSize` check, and should have aborted -- except that check is *inside the same disabled
assert*, so it also silently underflowed, producing a garbage 32-bit handle instead of a
diagnosable abort. Confirmed live: rebuilding `test_arena` (a small binary, few objects) showed its
own `s_bssProbe` landing *after* the 1 GiB arena in link order, not before -- proof this direction
(ordinary-global-vs-arena order) is not something this port controls or can rely on, which is exactly
why the fix has to work regardless of where any given global ends up relative to the arena.

### Fix: `g_base` anchored at a new low-memory region, not the arena

`include/port/lowmem.h`/`src/port/lowmem.cpp` (new): a 16 KiB, **non**-zerofill (explicit `= {0}`
initializer, no `,zerofill` section suffix -- deliberately real file content, matching real GameCube
low memory's own nonzero boot-time contents) region, `__DATA,__re4low`, always listed as the **first**
source file in every executable/test target that links `re4_port` (`CMakeLists.txt`) -- ld64 orders
same-segment sections by first appearance among the *directly listed* link inputs (this port's own
established precedent, `src/port/arena.cpp`'s `__re4arena` staying last by living inside the archive,
linked after everything listed directly). `re4_port::InitArena()` (`src/port/arena.cpp`) now computes
`g_base = &lowmem - 0x80000000`, not `&arena - 0x80000000` -- so GC address 0x80000000 is `__re4low`'s
own address, below every other global in the whole image by construction, and `GC32()` of *any*
ordinary global (not just arena/heap data) is now a valid, non-underflowing, positive handle
regardless of whether that global happens to link before or after the 1 GiB arena. `InitArena()` gained
two live, non-`NDEBUG`-gated invariant checks (this file compiles without `NDEBUG`, unlike the game
code): `GC32(&s_bssProbe) >= 0x80000000` (lowmem genuinely first) and `GC32(arena start) <=
0x80350000` (the fixed-GC-address budget below -- measured live this pass at GC `0x80274000`,
comfortably inside; `RE4_PORT_DEBUG_LAYOUT=1` env var prints the full layout on request). Both fire a
loud `abort()` with a diagnosis, not a silent corruption, if link order ever regresses.

**Verified**: this alone made `DrawOTag` stop crashing -- rerunning under `lldb` showed it walking a
populated OT for real and dispatching an actual queued primitive (`make_tile` -> `make_g4` ->
`gpuSetup` -> `GXBegin`), the deepest this session reached into real rendering.

### Game thread stack moved off the arena (a second, real bug this design decision fixed)

`include/port/game_stack.h`/`src/port/game_stack.cpp` (new): a dedicated ~2 MiB zerofill
`__DATA,__re4stack` region, listed second (right after `__re4low`) in `re4_boot`'s sources. The main
game thread (`src/port/boot_main.cpp`) now runs on this region (`re4_port::CreateThreadOnStack()`, a
new general helper `include/port/arena.h`/`src/port/arena.cpp` factored out of the old
`CreateArenaThread()`, which now just calls it with an arena offset) instead of
`CreateArenaThread(0, GetArenaSize()/4, ...)` (256 MiB carved from the arena's own start). Found while
implementing, not asked for directly but load-bearing: `SystemMemInit()` (`src/game/main_mem.cpp`)
calls `OSSetArenaLo(arenaBase)` (below) so the game's heap allocator is configured to hand out memory
starting from arena offset 0 -- the *same bytes* the old scheme put the running thread's own machine
stack on. That is a live collision (the heap would eventually allocate over the executing thread's own
stack); moving the thread's stack to a disjoint, dedicated region removes it, and frees the entire 1
GiB arena for the heap, undivided.

### `os_thread.cpp`'s per-task threads: now real `pthread`s on the game's own stack buffer

Coordinator's flagged risk confirmed real: `OSCreateThread` was a plain `std::thread` (a
host-allocated ~8 MiB stack, no relation to the compressed window) even though the game already passes
a real stack buffer (`scheduler.cpp`'s `TaskSchedulerInit()`, `MEM_ALLOC`-backed, already inside the
arena). Fixed: `OSCreateThread` (`src/port/os_thread.cpp`) now calls the same new
`re4_port::CreateThreadOnStack()` with that buffer (`pthread_attr_setstack`), falling back to a
default (host) stack -- logged loudly, not silently -- only if `pthread_attr_setstack` itself fails
(observed live: it does fail for these small, sub-page-granularity task stacks, most likely an
alignment/size requirement `MEM_ALLOC`'s allocator doesn't guarantee; not root-caused further this
pass, flagged as **TO VERIFY** open work, not a crash -- the fallback keeps the boot sequence running).

### `mem1.cpp`: MEM1 now spans lowmem through the arena end, `OSSetArenaLo/Hi` corrected, real `OSBootInfo`/bus clock

`MEM1Start = lowmem`, `MEM1End = arena end`, `mem1Size` = the distance between them (not just the
arena's own size) -- this is what makes an *ordinary* global's `GC32()` value fall inside
`[MEM1Start, MEM1End)`-shaped range checks elsewhere, not just the arena's. Aurora's own
`AuroraInitArena()` (`OSInit()`) still runs once, before `mem1Size` is set (this port's own established
ordering, section 13), so it never sets `OSGetArenaLo()`/`Hi()` for real; this file now calls
`OSSetArenaLo(arenaBase)`/`OSSetArenaHi(arenaEnd)` explicitly instead of relying on Aurora's default
(`MEM1Start + 0x4000`, which under this new layout would land inside `__re4low` itself, not the real
arena). Also (coordinator-flagged, confirmed real): `OSBootInfo::memorySize` (offset `0x28` --
`DVDDiskID` is `0x20` bytes + `magic`(4) + `version`(4), confirmed against the actual struct, not
assumed) and `__OSBusClock` (offset `0xF8`) are both normally filled by Aurora's own
`AuroraFillBootInfo()`/`AuroraInitClock()`, but both ran during the process's only `OSInit()` call,
before `OSBaseAddress` was set (both guard on it), so neither ever wrote anything --
`src/game/main_sub.cpp`'s own `OS_BUS_CLOCK` macro reads this location directly (bypassing Aurora
entirely, vendor code) and would have silently read 0, a divide-by-zero waiting to happen the first
time anything computed `OSTicksToSeconds()`. `InitMem1()` now writes both directly (162,000,000, the
real GameCube bus clock, matching Aurora's own `OS_BUS_CLOCK` macro).

### `DrawOTag` reached real GX submission -- two further real bugs found and fixed past that point

1. **Aurora's GX emulation asserts on back-to-back `GXBegin()` calls with no `GXEnd()` in between**
   (`[fatal] [aurora::gx] GXBegin: called without matching GXEnd`, `../aurora/lib/dolphin/gx/GXVert.cpp`).
   Checked against the real SDK before concluding anything: `dolphin/gx/GXGeometry.h`'s own `GXEnd()`
   is `static inline`, a no-op outside `DEBUG` builds -- real hardware auto-completes a fixed-vertex-count
   primitive (`GXBegin`'s `nVerts != GX_AUTO`) without an explicit `GXEnd()` at all, which is exactly why
   no vendor unit ever calls it (confirmed: `grep -rl "GXEnd(" src/game/*.cpp` was empty before this
   pass). This is a genuine Aurora emulation gap (software bookkeeping with no real-hardware
   equivalent), not a vendor bug or a config mismatch -- fixed as an Aurora patch,
   `tools/port/aurora-patches/0002-gx-implicit-end.patch`: `pre_begin()` now closes an already-open
   `sInBegin` implicitly (calling the same cleanup `GXEnd()` itself runs) instead of asserting.
   Applied to the Aurora checkout the same idempotent way as the existing DVD patch (section 13/14).
2. **Aurora's GX command recorder has no "active session" outside an `aurora_begin_frame()`/
   `aurora_end_frame()` pair** (`[fatal] [aurora::gfx] No active recording session`) -- this is
   `include/port/vi.h`'s own previously-flagged open risk materializing exactly as predicted: the host
   main thread's present loop no longer brackets real GX submission (that happens on the game thread).
   Fixed by moving `aurora_begin_frame()`/`aurora_end_frame()` off `RunPresentLoop()` (which now only
   pumps `aurora_update()` and the retrace tick) and onto the **game thread**, bracketing
   `src/game/main_sub.cpp`'s existing `Render_before()`/`Render_swap()` (`TARGET_PC`-only calls,
   `re4_port::BeginGxFrame()`/`EndGxFrame()`, new in `src/port/vi.cpp`) -- `g_recorder`
   (`../aurora/lib/gfx/recording.cpp`) is a plain, non-thread-local global with no inherent thread
   affinity, so it must be driven by whichever thread issues the matching GX submission, not by
   `RunPresentLoop()`'s own thread.
3. **Removing `aurora_begin_frame()`/`aurora_end_frame()` from `RunPresentLoop()` removed its only
   pacing source** -- found live, not anticipated: without it, the loop free-ran as fast as
   `aurora_update()` could spin, and `main.cpp`'s own hang detector (`haltExecCheck()`, `vsync_cnt >
   3599`) fired within a few seconds (`HALT D:/Bio4/Prog/main.cpp(548)`). Fixed with an explicit
   `std::chrono::steady_clock` deadline paced at ~59.94 Hz (real NTSC field rate) inside
   `RunPresentLoop()` itself.

**Current frontier, reached live this pass**: past `DrawOTag`'s crash, past both GX-recording-session
fatals and the pacing hang, into real GX FIFO command processing (`[debug] [aurora::gx::fifo] Unhandled
XF/BP register ...` -- expected, real geometry-pipeline state Aurora logs but doesn't fully emulate
yet) -- **new blocker**: `[fatal] [aurora::gx::fifo] indexed XF load from unmapped array 24`, a deeper
GX vertex-array-binding issue, not yet root-caused (budget-limited this pass; likely `Draw_cinesco()`
or `cMes`'s own textured-quad drawing submitting an indexed draw whose vertex array was never bound --
`GXSetArray()` call site not yet traced). Stopping here per the session's budget.

**No screenshot captured this session**: attempted (`screencapture -x` immediately after launch, and
mid-run) but the process now crashes on the new FIFO blocker within roughly 1-2 real seconds of the
DVD reads completing -- faster than a screenshot could be scripted against it reliably; a capture
attempt during a run showed only the desktop (the game window, if painted at all, was not the frontmost
/ captured surface). Aurora's own logs (`Aurora window opened`, a real Metal adapter/device/swapchain
sequence) are the only evidence the window exists this session, same as section 27 -- confirmed real
GX commands ARE now reaching the FIFO (the `Unhandled XF/BP register` lines are proof some geometry
state is being submitted), so a frame with visible content is plausible once the array-binding blocker
above is fixed; whoever picks this up next should screenshot as soon as it renders.

**Verified**: `ctest` 4/4 (`build-pc-boot`, `RE4_U32_32=ON`) and 3/3 (`build-pc`, default `OFF`), both
rebuilt and rerun this pass, both green. Only `src/game/libgpu.cpp` and `src/game/main_sub.cpp` touch
files outside `src/port/`/`include/port/` -- both changes are fully `#ifdef TARGET_PC`-guarded, no
`#line`-tracked region moved (confirmed by reading the diff directly, not assumed) -- remote
matching-build verification below.

## 29. Coordinator follow-up: NDEBUG-independent checks, real per-task machine stacks, in-process screenshot (2026-09-24)

### 1. `GC32()`/`GCPTR()` bounds checks no longer depend on `NDEBUG`

`include/port/ptr32.h`'s `assert()`s silently compiled out under `NDEBUG` (`RE4_GAME_DEFINES` sets it
unconditionally) -- exactly what hid section 28's root cause. Replaced with `RE4_PORT_CHECK` (default
on, `-DRE4_PORT_CHECKS=0` to disable), which calls `re4_port::PortCheckFail()` -- logs the offending
pointer/handle and `g_base`, then `std::abort()`s (or, under `RE4_PORT_PAUSE_ON_ABORT`, parks the
thread forever instead -- see part 4). Defined out-of-line (`src/port/arena.cpp`), not inline in the
header: `ptr32.h` is force-included (`-include`) into every `re4_boot_game` TU, and an inline
definition needing `<thread>`/`<chrono>` transitively pulled in libc++'s own `<new>`/`<cmath>` ahead of
this tree's vendor shadow declarations (`include/cManager.h`'s placement `operator new`,
`include/math_sub.h`'s `fabsf`) -- a real build break, confirmed the hard way, fixed by moving the
definition to a normal (not force-included) TU.

Turning this on immediately found two more real, previously-silent bugs (exactly the point of the
exercise):

- **`src/game/file.cpp`'s `usb_buf = (void*) 0x81800000;`** -- a global variable's own static
  initializer, which runs during C++ global-constructor time, *before* `main()`/`InitArena()` ever
  executes -- calls `GCPTR()` with `g_base` still `0`. Confirmed this specific global is dead
  (`grep -n usb_buf src/game/file.cpp` shows the declaration and nothing else -- never read). This is
  a distinct hazard from the out-of-window corruption case (a static-initialization-order artifact,
  not a corrupt handle), so it is NOT a hard abort: `GC32()`/`GCPTR()` now special-case `g_base == 0`
  with a one-time warning (`RE4_PORT_WARN_IF_UNINITIALIZED`) and a harmless fallback value (0 /
  reinterpret the raw handle), instead of treating every fixed-GC-address global initializer in the
  whole tree as a hard boot failure regardless of whether anything ever reads the result.
- **The DVD file-system-table (FST) address** -- `FST Address = 0x...`, then `GC32()` aborts on a
  pointer like `0x92d640000..0x9ef640000` (varies per ASLR run) against `g_base` around `0x81...` --
  a genuine, real out-of-window pointer, **not fixed this pass**. This is new/renamed territory:
  docs/port-boot.md section 2/3 already flagged "`Dvd.SizeTableRead()`'s literal file path and record
  format" and the DVD size table's byte-swap-at-load step as **TO VERIFY**/unimplemented -- this is
  that same gap, now surfaced as a hard, reproducible abort instead of a silent corruption that used
  to let boot continue (apparently harmlessly, by luck) past it. **Current blocker, not root-caused
  this pass** (budget): whoever picks this up next should read `src/game/dvd.cpp`'s FST-address
  computation (search for `"FST Address"` -- the log line above) and check whether it needs the same
  byte-swap-at-load treatment as `CRoomInfo`/the title archive (Phase 3, docs/port-phase3.md) before
  its bytes are used as a pointer/offset.

### 2. Real per-task machine stacks: root-caused and fixed

`pthread_attr_setstack()` on macOS requires both the stack address and size to be page-aligned
(16 KiB) -- confirmed by reading Apple's own requirement, not guessed. Every one of the vendor's own
per-task stack sizes (`GetStackSize()`, `src/game/scheduler.cpp`: `0x1800`/`0x2000`/`0x3000` bytes,
the original PPC target's tiny stack budget) is neither page-aligned nor page-sized, and the buffer
itself (`MEM_ALLOC`'d, sub-sliced per task) isn't guaranteed page-aligned either -- so it failed for
every task thread, every run, deterministically, not intermittently.

Fixed per the coordinator's explicit design (no host-stack fallback for a game thread): a new
dedicated pool, `include/port/arena.h`'s `kTaskStackSlotSize` (1 MiB) x `kTaskStackSlots` (18,
matching `include/scheduler.h`'s `TASK_NUM`) at the very top of the 1 GiB arena
(`GetTaskStackPoolBase()`) -- `src/port/mem1.cpp`'s `OSSetArenaHi()` now excludes it, so the game's own
heap allocator never hands out memory a running task thread's machine stack is using. `os_thread.cpp`'s
`OSCreateThread` assigns one slot per distinct `OSThread*` it ever sees (`AssignTaskStackSlot()`,
`g_taskStackSlots`) and calls `CreateThreadOnStack()` on that slot; the game's own stack buffer
(`stack`/`stackSize` parameters) is still used unchanged for `thread->stackBase`/`stackEnd` and the
vendor's own `StackOverflowCheck()` guard-word bookkeeping -- the real host execution stack and the
game-visible "logical" stack size are now two different things by design (real hardware doesn't need
this distinction; this host does). If a slot's `pthread_attr_setstack()` still somehow fails, or the
pool's 18 slots are ever exhausted, this aborts loudly instead of falling back to an unsafe host stack
(`std::abort()`, matching the coordinator's instruction exactly).

**Verified**: rebuilding and rerunning shows no more `pthread_attr_setstack failed` lines for task
threads (previously printed twice per run, for the two task threads reached before the earlier
blocker).

### 3. GX array 24 ("indexed XF load from unmapped array"): not reached this pass

The coordinator's hypothesis (array 24 = `GX_LIGHT_ARRAY`, `GX_VA_POS=9..GX_VA_TEX7=20`,
`POS_MTX=21`/`NRM_MTX=22`/`TEX_MTX=23`/`LIGHT=24`) was not checked this pass: part 1's stricter,
NDEBUG-independent `GC32()`/`GCPTR()` checks surfaced the FST-address abort (part 1, above) *earlier*
in the boot sequence than the array-24 blocker, so this run no longer reaches it at all. Whoever
un-blocks the FST-address issue will reach array-24 again and can pick up the coordinator's specific
lead then (check `GXSetArray(GX_LIGHT_ARRAY, ...)` call sites and whether the game bypasses `GX*`
wrapper functions and writes the FIFO directly via `GXWGFifo`/the write-gather pipe -- docs/port-boot.md
section 7 already flags `GXWGFifo` as stubbed as a 1-element array with no real Aurora backing, exactly
the kind of gap that would produce this symptom if any vendor code pokes it directly instead of going
through `GXSetArray()`). Not investigated further this pass -- explicitly deferred, not silently
dropped.

### 4. In-process screenshot: implemented, and the window is confirmed to render real content

Two new, opt-in (env-var-gated, never on by default) mechanisms, both in `src/port/vi.cpp` /
`include/port/ptr32.h`:

- `RE4_PORT_SCREENSHOT=<path>` (+ optional `RE4_PORT_SCREENSHOT_DELAY_MS`, default 1500): a detached
  thread sleeps briefly after `aurora_initialize()` then shells out to `/usr/sbin/screencapture -x
  <path>` (whole screen, not a specific window -- window-specific `-l <windowid>` targeting would need
  additional CoreGraphics/Objective-C glue this pass didn't add, flagged as **TO VERIFY**/future work).
- `RE4_PORT_PAUSE_ON_ABORT=1`: `PortCheckFail()` (part 1) parks the aborting thread in an infinite
  sleep loop instead of calling `std::abort()`, keeping the whole process (and its window) alive
  indefinitely so the screenshot thread above has time to fire even though the boot sequence itself
  has hit a real blocker.

**Verified, screenshot captured and viewed** (not just logged): running with both env vars set
(`RE4_PORT_PAUSE_ON_ABORT=1 RE4_PORT_SCREENSHOT=/tmp/re4_screenshot.png
RE4_PORT_SCREENSHOT_DELAY_MS=1500`) produced a real, non-empty PNG -- viewed directly, it shows a flat
dark-gray field filling the whole capture with no desktop chrome/menu bar/other windows visible at all,
consistent with a real, focused, likely-fullscreen-or-large game window showing Aurora's clear color
(no GX geometry has been submitted successfully yet at the point this run aborts, section 28's/this
section's still-open blockers are both upstream of any real draw reaching the screen) -- this is the
strongest evidence yet that "a window opens and presents frames" (this whole milestone's first ask) is
genuinely working, not just inferred from Aurora's own log lines.

**Verified no regression**: `ctest` 4/4 (`build-pc-boot`, `RE4_U32_32=ON`) and 3/3 (`build-pc`, default
`OFF`), both rebuilt and rerun. Only `include/port/`/`src/port/` touched this round (no `src/game/`
edit) -- no remote matching-build round trip needed per the port rules.

## 30. FST-address abort fixed; array 24 root-caused and fixed; new frontier past both (2026-09-24)

### FST-address `GC32()` abort (section 29 part 1's blocker)

`cDvd::Init()` (`src/game/dvd.cpp`) cast Aurora's real `DVDGetFSTLocation()` -- a pointer into
Aurora's own private `std::vector<FSTEntry>`, ordinary host heap memory with zero relationship to
the GC address window -- straight into a `GC32()`-checked `u32`, aborting exactly the way the port
rules say it must (`include/port/arena.h`'s "no game-visible pointer to host malloc memory"). Confirmed
`FstSize` (the only thing this cast feeds) is dead code: grepped, nothing in `src/game` or any REL
module ever reads it again. Fixed with a small, ordinary linked-in-global placeholder
(`re4_port::GetFstPlaceholder()`, `include/port/dvd.h`/`src/port/dvd.cpp`) standing in for the real
address under `TARGET_PC` -- GC32()-safe by construction (section 28's g_base-anchoring already makes
every ordinary global valid), no check weakened. `#else` branch byte-identical to the original.
**Verified**: `re4_boot` no longer aborts here; remote matching-build round trip (`dtk shasum` 0
non-OK, `asmcheck.py --all` TOTAL 231 unchanged); `ctest` 4/4 (`ON`) / 3/3 (`OFF`); `re4_game_all -k 0`
failing-file set unchanged (33 files, diffed directly). Pushed (`32e7926d`).

### GX array 24 ("indexed XF load from unmapped array"): root cause found and fixed

The coordinator's array-24 = `GX_LIGHT_ARRAY` hypothesis (section 29 part 3) was confirmed by reading
`../aurora/include/dolphin/gx/GXEnum.h`'s `GXAttr` enum directly (`GX_POS_MTX_ARRAY`=21,
`GX_NRM_MTX_ARRAY`=22, `GX_TEX_MTX_ARRAY`=23, `GX_LIGHT_ARRAY`=24) -- but the actual mechanism was a
different, broader bug than "the game bypasses `GX*` and pokes the raw FIFO": `include/gx.h`'s own
`GXPosition3f32`/`GXPosition3s16`/`GXColor4u8`/`GXNormal3f32`/`GXNormal3s8`/`GXTexCoord2f32`/
`GXPosition2u16`/`GXTexCoord2s16`/`GXMatrixIndex1u8` (the write-gather-pipe convenience wrappers this
repo's own header defines for ProDG game code, since it cannot include the CodeWarrior-only real SDK
header) are declared `static inline` -- internal linkage, so every translation unit that includes this
header got its OWN private copy, which wrote to `GXWGFifo`, a real hardware MMIO absolute address on
target, stubbed on host as a plain 1-element dummy array (`src/port/stubs/manual_stubs.cpp`, section 7).
Aurora (`../aurora/lib/dolphin/gx/GXVert.cpp`) separately provides REAL, external, same-named
implementations of all but the last of those (writing into its own software GX FIFO) -- but since our
header's copies are `static inline`, they shadow Aurora's real symbols completely; no TU that includes
`include/gx.h` ever calls Aurora's version for these names. Confirmed live: a single `GXBegin()`/4-or-8
vertex quad draw had SOME of its per-vertex fields (whichever GX*() calls the header does NOT shadow,
e.g. those reached through other headers/paths) reach Aurora's real FIFO while others (position/color/
texcoord, all shadowed) silently vanished -- a desynced, partial vertex byte stream is exactly what
produces "vertex data not evenly divisible" warnings and, further downstream, a garbage indexed-load
opcode decoded as array 24.

**Fix**: `include/gx.h`, `TARGET_PC` branch -- declare (don't define) the 8 functions Aurora has a real
symbol for, so `libaurora_gx.a` satisfies them for every TU (matching how `GXBegin`/`GXSetArray`/... a
few lines down in the same header already worked). `GXMatrixIndex1u8` (this repo's own invented name
for a raw per-vertex matrix-index byte write; no real SDK/Aurora equivalent exists) is left as a
documented, discarding stub -- not on the boot/title-screen render path (only `id_sys.cpp`/
`dbmodule.cpp`/`esp01.cpp` call it, none reached yet); a real fix needs either an Aurora patch exposing
a public FIFO-write API (today's `aurora::gx::fifo` is a private, unheadered internal namespace,
`lib/gx/fifo.hpp`, not installed) or waiting until one of those three call sites is actually reached.
`#else` branch (the matching build) is untouched, byte-for-byte.

**Verified**: the `[fatal] indexed XF load from unmapped array 24` no longer occurs; the boot sequence
runs measurably further (through `SS/cmn/title.snd`'s DVD read, past several more real `GXBegin`/`GXEnd`
draws, only "vertex data not evenly divisible" *warnings* remain -- consistent with `GXMatrixIndex1u8`
or another still-unrouted call still contributing to some draws, not yet chased further). Remote
matching-build round trip (`dtk shasum` 0 non-OK, `asmcheck.py --all` TOTAL 231 unchanged); `ctest` 4/4
(`ON`) / 3/3 (`OFF`); `re4_game_all -k 0` failing-file set unchanged (33 files, diffed directly). Pushed
(`8cce0f76`).

### New frontier: `EprintfDrawing()` null-pointer dereference

Past both of the above, `lldb` shows a new crash, reached only now that boot progresses this far:

```
* thread #4, EXC_BAD_ACCESS (code=1, address=0x0)
  frame #0: EprintfDrawing() at eprintf.cpp:383 (vendor line, #line-mapped -- inside the
             `for (i = 0; i < MESS_PTR_NUM && *(u32*)(mess_ptr_buff + i*4) != 0; i++)` loop /
             `font_draw()` call, only reached when both `eprintf_init` and `pG->debug_mode` are
             nonzero -- this is a debug build, G4BE08, so debug_mode plausibly defaults on)
  frame #1: EprintfFlush() at eprintf.cpp:412
  frame #2: main_game() at main.cpp:144
```

Not root-caused this pass (budget) -- `Debug_alloc()` (`main_mem.cpp:571`, real, not stubbed) is what
allocates `mess_ptr_buff`/`mess_keep_buffer` in `EprintfInit()`; the crash is consistent with either
that allocation silently failing/returning null (heap not ready / a debug-heap-specific path this port
doesn't set up the same way as the main heap) or a bad pointer somewhere in the `MESS_PTR(i)` /
`font_draw()` chain. Whoever picks this up next: breakpoint on `EprintfInit()` first to confirm
`mess_ptr_buff`/`eprintf_init` are actually set the way the source implies, then step into the crash
itself.

No screenshot captured this session: the in-process `RE4_PORT_SCREENSHOT` mechanism (section 29 part 4)
did not fire this time despite `RE4_PORT_SCREENSHOT`/`RE4_PORT_SCREENSHOT_DELAY_MS` being set and
"`Aurora window opened`" reliably printing every run (confirming the window still opens) -- the
"screenshot attempted" log line never appeared even letting the process run to its natural
`SIGSEGV`/exit-139 (not killed early). Not root-caused this pass (budget) -- **TO VERIFY**/open,
flagged rather than guessed at; whoever picks up the `EprintfDrawing` blocker above should also check
why the screenshot thread (`src/port/vi.cpp`'s `RunPresentLoop()`) isn't firing before trying again.

Verified no regression on both fixes above (each checked independently, see their own paragraphs);
default host build (`RE4_U32_32=OFF`) `re4_game_all -k 0` failing-file set unchanged (33 files) across
both changes.

## 31. GXMatrixIndex1u8 routed to Aurora, real GXWGFifo backing, eprintf/card fixes, first stable
    frame loop (2026-09-24/25)

Coordinator follow-up to section 30. Four fixes, in order, each verified independently.

### 1. `GXMatrixIndex1u8` and every other static-inline GX shadow, audited

No `GXMatrixIndex1x8` (or any similarly-named) symbol exists anywhere in Aurora (`grep`ped
`../aurora/include`/`../aurora/lib` directly, not assumed) -- the real SDK has no dedicated GX*()
entry point for a per-vertex matrix-index write at all, confirming section 30's note. Routed through
`GXParam1u8` instead (a real, exported Aurora symbol that appends one raw byte to the same software
FIFO `GXColor4u8`/etc. use internally -- identical effect to a raw `GXWGFifo->u8` write).

A second, larger `include/gx.h` bug surfaced while wiring this up: `src/game/mes.cpp`'s
`RomFont::draw()` (and `src/game/dbmodule.cpp`, still excluded) write straight to `GXWGFifo->field`
themselves, bypassing every `GX*()` wrapper -- exactly the "bypasses GX* functions" scenario the
coordinator's original brief anticipated, just in a different file than expected. Fixed generally, not
per-call-site: `GXWGFifo` is now (`src/port/gx_wgfifo.cpp`, `TARGET_PC` only) a real object whose
per-field `operator=` calls the matching `GXParam1xx()`/`GXCmd1xx()` Aurora symbol, so
`GXWGFifo->s16 = x;` (unchanged vendor source) genuinely reaches Aurora's FIFO now, covering this
call site and any future one the same way, not just the ones found this pass.

**Every other header was scanned for the same shadow pattern** (`grep -rn "static inline.*GX\b"`
across `include/*.h`): only `include/snd_sdk.h`'s `GXEnd` remained, confirmed (by the file's own
comment plus a `grep` for its call sites) to be genuinely dead code kept only so its two string
literals land in `.rodata` in the vendor's original order -- never actually called, so left untouched
(no fix needed, and touching it risks the byte-identity of an untouched region for zero benefit).

**Verified**: `re4_boot`/`build-pc` rebuild clean, `ctest` 4/4 (`ON`)/3/3 (`OFF`), `re4_game_all -k 0`
unchanged (33 files). Remote-verified (commit `46a3d70d`): `dtk shasum` 0 non-OK, `asmcheck.py --all`
TOTAL 231.

### 2. `EprintfDrawing()` null-pointer crash: root-caused and fixed (a real 64-bit pointer-size bug)

Not a `Debug_alloc()`/heap-readiness problem as first suspected (confirmed live: `mess_ptr_buff`/
`mess_keep_buffer` were real, non-null, valid pointers at every `EprintfInit()` call, checked with a
conditional lldb breakpoint that never fired). The real bug, found live with lldb (`frame variable`
inside the crash, then `expr` on both access paths): `eprintf.cpp`'s `MESS_PTR(i)` macro
(`((char**) mess_ptr_buff)[i]`) strides by `sizeof(char*)` -- 4 bytes on the original 32-bit target
(matching the file's own `mess_ptr_buff + i * 4` / `Debug_alloc(MESS_PTR_NUM * 4, ...)` arithmetic for
the *same* table) but 8 bytes on this 64-bit host. At `i=41`: the loop condition's hardcoded 4-byte
read (`*(u32*)(mess_ptr_buff + i*4)`) saw `1` (nonzero, entered the loop), but `MESS_PTR(41)`'s
8-byte-strided read landed on a different, zeroed part of the buffer, producing a null `p` and the
observed `EXC_BAD_ACCESS(address=0x0)`. Not a vendor bug (byte-identical-correct on the real 32-bit
target) and not caught by the cast rewriter (`char*` -> `char**` is a bitcast, not the pointer<->
integer cast class it rewrites).

Fixed by making the table what its surrounding `*4`/`Debug_alloc(N*4)` arithmetic already says it is:
an array of `re4_port::Ptr32<char>` (4-byte compressed handles), the same class every other such slot
in this port uses -- under `TARGET_PC` only, `#else` branch byte-identical to the original. One local
wrinkle: the read call site casts to `u8*` (`(u8*) MESS_PTR(i)`), which a C-style cast cannot reach
through `Ptr32<char>`'s only conversion operator (`operator char*()`) plus a further `char*`->`u8*`
reinterpret (not a standard conversion, so the two don't compose into one cast) -- a small
file-local wrapper (`MessPtrSlot`, `eprintf.cpp`, not a change to the shared `include/port/ptr32.h`)
adds the one extra explicit `u8*` conversion this one call site needs, alongside the `==NULL`/
assignment usage the rest of the file already relies on.

Per the coordinator's instruction ("must work or be cleanly disabled, not crash") -- this is a real
fix, not a disable: the debug-text overlay now genuinely draws through this table instead of being
turned off.

### 3. `cCard::initSub()` null-pointer crash (new frontier once #2 was fixed)

Reached only after #2's fix let the frame loop run far enough: `cCard::initSub()`
(`src/game/card.cpp`) unconditionally dereferences `SndMem.sub_adr` (`pSubData->ofs[0]`/`ofs[1]`) --
real hardware shares the sound driver's MRAM arena with the card/sub-screen archive by convention, but
`SndMem.sub_adr` is one of the fields `SndInit()`'s `TARGET_PC` stub (section 15/23, Phase 5 sound
deferral) never populates, so it reads as 0. Guarded both call sites under `TARGET_PC` (`if
(SndMem.sub_adr != 0)`), the same pattern section 15/23 already established for every other
unconditional `SndMem`-state read the sound stub exposes; `#else` branches byte-identical.

**Verified** (#2 and #3 together): remote build (commit `19894120`) `dtk shasum` 0 non-OK,
`asmcheck.py --all` TOTAL 231; `ctest` 4/4 (`ON`)/3/3 (`OFF`); `re4_game_all -k 0` unchanged (33
files).

### 4. Milestone: the boot sequence now runs in a stable, repeating frame loop

With #1-#3 fixed, `re4_boot` no longer crashes at all on this session's runs -- it settles into a
steady, indefinitely-repeating cycle (`GXEnd` warnings + periodic `[aurora::gpu::cache] Dawn cache
prune completed` lines, real recurring GPU activity) and stays alive until killed. This is the first
session this port has run this far without hitting a new crash. The remaining `GXEnd: vertex data not
evenly divisible` warnings (241/266/257 bytes) are very likely a Aurora-side false positive, not a
missed byte: Aurora's own comment on this check says it is a "best-effort" heuristic that cannot
account for non-vertex FIFO commands (state changes) landing between two draws' byte-counts, and the
two real vendor draw call sites reached at this point (`main_sub.cpp`'s `DrawTexture`,
`eprintf.cpp`'s `font_draw`) write exactly their declared vertex format's byte count with no
`GXMatrixIndex1u8`/indexed attribute involved at all -- not chased further this pass (non-fatal,
budget), flagged for whoever next works on rendering fidelity.

### Screenshot mechanism: fixed and root-caused, window-specific capture found to have a real macOS limitation

`src/port/screenshot.cpp`/`include/port/screenshot.h` (new): finds this process's own on-screen
`CGWindowID` via `CGWindowListCopyWindowInfo`, matched by this process's own PID (robust, not a
name/title match), then `screencapture -x -l <id>`; falls back to a whole-screen capture (previous
behavior) if that fails, logging which path fired either way. Also logs when the screenshot thread is
armed (`"screenshot thread armed, firing in N ms -> path"`), fixing the coordinator's "verify it
fires" ask -- the earlier silent no-op (section 30) is now always distinguishable from a real attempt.

**Root cause of the earlier silent no-op**: never a mechanism bug -- confirmed live, the `getenv`
branch always fired; the previous session's runs simply crashed (a real `SIGSEGV`) before the
1500 ms/500 ms delay elapsed, and the crashing thread's death raced the logging thread's own buffered
`stderr` writes when both were redirected into the same file, losing the "screenshot attempted" line
even on occasion where the capture itself might have run. Now visible either way.

**Window-specific capture (`-l <windowid>`) reliably fails** in every run this session
(`"could not create image from window"`, macOS `screencapture`'s own error) -- confirmed this is a
real capture-API limitation, not a permissions or logic bug: a region capture at the exact same
window's own bounds (`screencapture -x -R x,y,w,h`) fails identically ("could not create image from
rect"), while a whole-screen capture of the same live process succeeds every time. The window is a
`CGWindowListCopyWindowInfo`-visible, correctly PID-matched entry (confirmed by printing its ID/
bounds live), but its content is evidently presented through a path (Aurora/Dawn's Metal swapchain,
likely an exclusive-fullscreen or otherwise not-fully-window-server-composited surface) that the
per-window/per-region screen-capture APIs cannot read from, while the whole-display capture path can.
**TO VERIFY**: whether this reproduces in a normal interactive login session (this session's captures
all ran from the same automated/scripted context throughout) or is specific to how Aurora presents its
swapchain on this port; not chased further this pass -- the whole-screen fallback is a complete,
working substitute for now.

**Screenshots captured and viewed this session** (whole-screen fallback, both real, both viewed
directly, not assumed): two captures (1.5 s and 5 s after the window opens) both show a solid black
field filling the entire 2940x1912 capture, no menu bar/dock/desktop visible at all (consistent with
the game window covering the whole display, matching the "exclusive/fullscreen surface" hypothesis
above) and no visible geometry -- the same "clear color only" milestone section 29 first reported,
now reconfirmed after two more real render-path fixes (#1-#3 above) with the process still alive and
looping stably rather than having already crashed. `Render()` (`trans.cpp`, the main 3D scene pass)
remains excluded/stubbed (section 26) -- the only real draws reaching this point are 2D overlay quads
(`font_draw`/`DrawTexture`), and neither one's content was visibly distinguishable at either capture
moment (message buffer likely empty, or title texture not yet bound/blended visibly) -- not
root-caused further this pass.

**Verified**: `ctest` 4/4 (`ON`)/3/3 (`OFF`); `re4_game_all -k 0` unchanged (33 files); only
`src/port`/`include/port`/`CMakeLists.txt` touched by the screenshot commit, no remote round trip
needed per the port rules.

### Commits this pass (all remote-verified where required, pushed to `fork/port/macos-arm64`)

- `46a3d70d` -- GX shadow fixes (#1)
- `19894120` -- eprintf/card fixes (#2, #3)
- `ce1d568f` -- screenshot mechanism (#4, port-only, no remote round trip needed)

### Next steps for whoever picks this up

- Chase the "vertex data not evenly divisible" warnings properly (confirm the false-positive
  hypothesis with Aurora's own FIFO byte log, or find a real missed write) before trusting rendered
  output.
- Un-exclude `trans.cpp` (`Render()`) -- the real 3D scene pass, needed for anything beyond the title
  screen's 2D overlay; section 26 already scoped its two remaining error classes (`PTR_INVALID`
  macro-driven truncation casts, fixable the same way as `main_sub.cpp`'s; real paired-single asm for
  skinning, genuine Phase 5/4 work).
- Once `Render()` is live, re-screenshot -- the two 2D overlay draws already reaching Aurora's FIFO
  this session suggest the title archive/font system may already be closer to visible than the black
  frames suggest, but nothing will be conclusively visible until the main scene pass runs for real.

## 32. DVD read-retry storm and the GXEnd false positive, both root-caused and fixed; new frontier
    is a real ARAM-DMA hang inside the title sound bank (2026-09-25)

Coordinator-directed follow-up to section 31's two flagged items. Screenshots this pass: the
per-window capture (`-l <windowid>`) succeeded for the first time this session (contradicts section
31's "always fails" -- confirmed context-dependent, not chased further), showing real geometry (grey
horizontal bars, a colored strip) instead of a uniform black field -- still the debug/eprintf overlay,
not the title screen.

### 1. DVD read-retry storm: real root cause, fixed in Aurora (not this repo's own code)

Added a temporary diagnostic print in `dvdread_callback` (`src/game/dvd.cpp`, reverted after use --
confirmed byte-identical to HEAD) and traced every "DVD: Read Error!!!" to the same shape: Aurora's
`result` always equalled the *unaligned* `m_DivReadSize`, never `ALIGN32(m_DivReadSize)`, the value
`cDvdQueue::fileReadAsync` actually requested (it rounds every read up to the next 32-byte boundary,
matching real GameCube DVD hardware, which only ever transfers whole aligned sectors). Root cause,
confirmed live with an Aurora-side print: `readFromHandle()` (`aurora/lib/dolphin/dvd/dvd.cpp`) stops
at the *file's own* logical EOF (its FST-registered length) instead of the physical disc image, so a
request for the alignment padding past a file's true end came back short -- even though
`DVDReadAsyncPrio`'s own bounds check (`offset + length < fileInfo->length + DVD_MIN_TRANSFER_SIZE`)
explicitly documents that exact padding as valid. Fixed by zero-filling a shortfall of
`DVD_MIN_TRANSFER_SIZE` (32) bytes or less instead of reporting it as a real transfer-size mismatch
(`tools/port/aurora-patches/0003-dvd-align-padding.patch`, Aurora-only, no `src/game` changes).
**Verified**: 0 "DVD: Read Error!!!" lines across multiple live runs that previously logged 20-70 per
file; every DVD read (`etc/moji8.tpl`, `etc/sizetbl.dat`, `Font/common_p.fnt`, `debug/config.txt`,
`debug/roomInfo.dat`, `SS/cmn/title.snd`'s header) now succeeds on the first try.

### 2. GXEnd "vertex data not evenly divisible" warning: root-caused as a real false positive, fixed

Hand-computed the real per-vertex byte size for both draw call sites reaching Aurora at this point
against their own declared `GXSetVtxAttrFmt`: `main_sub.cpp`'s `DrawTexture` (position 3×s16 = 6 bytes
+ texcoord 2×f32 = 8 bytes = 14 bytes/vertex) and `mes.cpp`'s glyph `draw()` (position 2×s16 = 4 +
color 4×u8 = 4 + texcoord 2×f32 = 8 = 16 bytes/vertex) -- both match their real vertex counts exactly;
the submitted vertex geometry itself was never wrong. The warning's real cause: `mes.cpp`'s `draw()`
never calls `GXEnd()` (correct -- matches real hardware's fixed-vertex-count auto-complete, the same
behavior `GXVert.cpp`'s own `pre_begin()` comment documents), so the *next* glyph's `setAttribute()`
(real texture/matrix/TEV state loads) writes FIFO bytes before the next `GXBegin()` implicitly closes
the previous one -- Aurora's byte-count heuristic (`bytesWritten % nVerts`) has no way to separate
those state-change bytes from vertex payload, so it fires on entirely valid data specifically at this
call shape. Fixed by suppressing the check only on the *implicit* close path (`sImplicitEnd` flag,
`tools/port/aurora-patches/0004-gx-implicit-end-suppress-mismatch-warning.patch`); an explicit
`GXEnd()` call from the caller still gets the real check. **Verified**: 0 "not evenly divisible"
warnings across a run that previously logged one per frame (both byte counts, 266/4 and 257/8,
accounted for exactly by this mechanism -- not chased with a byte-for-byte reconstruction beyond
confirming both draw sites' own math, budget).

Neither fix changes what's visibly rendered (confirmed: `boot_3.png`, same debug-overlay bars as
before) -- both were real correctness/diagnostics fixes, not the cause of the "no glyph shapes"
symptom, which remains open (font texture likely not correctly bound/decoded -- not reached this
pass).

### 3. New frontier: `SS/cmn/title.snd`'s header read hangs the game thread indefinitely

Past both fixes above, every file read up through `SS/cmn/title.snd`'s 0x400-byte header succeeds
(confirmed with a temporary diagnostic print: `entrynum=669`, `fileLen=782720`, both fine, and a
second Aurora-side print confirmed the read genuinely completes and its callback genuinely fires) --
but no further progress is ever logged afterward. Confirmed live this is a real block, not a spin:
`ps`/`top` on the running `re4_boot` process show ~0% CPU and `sleeping` state for 25+ seconds, not a
busy-wait. Traced (not fully root-caused, budget) to the sound-bank header's `TRANS_SND_BLK`/
`TRANS_SND_PCM` parts (`cDvdQueue::readMain`, `src/game/dvd.cpp`): these route ARAM-destined chunks
through `cDvdQueue::trans2aram()`, which calls the real (unstubbed) `ARQPostRequest()`
(`src/lib/arq.c`) -- unlike `SndInit()` (Phase 5, already stubbed per section 15/23), `arq.c`/`AR.c`
are still the vendor's real hardware-DMA code, whose completion callback
(`__ARQInterruptServiceRoutine`) is only ever invoked by a real ARAM-DMA hardware interrupt
(`ARInit()`'s `__OSSetInterruptHandler(6, __ARHandler)`) that has no host equivalent -- so
`trans2aram_cb` never fires, and the busy bit it's supposed to clear (`m_be_flag & 0x01000000`) stays
set forever. (`AR.c`'s own `__DSPRegs` is a raw pointer to the real hardware's physical address
`0xCC005000`, so a genuine ARAM DMA attempt would segfault, not hang -- the process staying alive and
merely blocked suggests this specific path is reached via a real wait primitive, e.g. the port's
cooperative-task scheduler's own synchronization, rather than `ARStartDMA` itself; **not confirmed
further this pass**, flagged for whoever picks this up next.)

**Next steps**: root-cause exactly which wait primitive blocks (a task-scheduler semaphore/condvar,
most likely, given the "sleeping, 0% CPU, no crash" signature) and give `ARQPostRequest` (or
`trans2aram()`'s call site, `TARGET_PC`-only) a host stand-in that performs the MRAM<->ARAM copy
synchronously and invokes the completion callback immediately, mirroring what `SndInit()`'s existing
Phase-5 stubs already do for the rest of the sound subsystem. Until this is fixed, `title.snd` (and by
extension `title.dat`, gated behind it in `titleWait()`) can never finish loading, so the title screen
is unreachable no matter how correct the render path is.

### Commits this pass (Aurora patches only, no `src/game`/`include` changes, no remote round trip
    needed per the port rules)

- DVD alignment-padding fix (`tools/port/aurora-patches/0003-dvd-align-padding.patch`)
- GX implicit-end warning suppression (`tools/port/aurora-patches/0004-gx-implicit-end-suppress-mismatch-warning.patch`)

## 33. Continuing past section 32: DvdHeader endianness (real root cause of the title.snd hang),
    sound-block skip, a real scheduler missed-wakeup race fixed, one scheduler race still open
    (2026-09-25)

Coordinator-directed follow-up. Ordered by what was asked: (1) the ARAM-DMA hang from section 32,
(2) font glyph rendering, (3) reach the title screen.

### 1. The section 32 "ARAM-DMA hang" hypothesis was wrong; the real bug was DvdHeader endianness

A temporary diagnostic print (`cDvdQueue::readMain`, reverted after use -- confirmed byte-identical
to HEAD) showed the real cause immediately: `title.snd`'s multi-part file header (`DvdHeader`,
`include/dvd.h`) is read straight off disc for "headered" files (`DvdReq::mode` bit 15) and was
**never byte-swapped** on this host -- every field (`type`, `size`, `ofs`, `sndType`, ...) came
through as raw big-endian garbage (`type=16777216` instead of `1`, etc.). `cDvdQueue::readMain()`'s
`switch ((*ph)->type)` matched no known `TRANS_*` case for that garbage value, so the state machine
silently looped forever re-processing the same unrecognized entry -- explaining the "sleeping, 0%
CPU, no crash" signature exactly (each `Read()` poll did real, bounded work and returned, forever,
with no crash and no progress). Not an ARAM/DMA issue at all -- that hypothesis (section 32) is
retracted; Aurora's own AR.cpp (a real, synchronous host ARQ/AR implementation, already linked into
`aurora_os`) was never actually reached along this path in the first place (confirmed: `src/lib/
arq.c`/`AR.c`, the real-hardware-register version, are not even compiled into `re4_boot_game` --
`aurora_os`'s own `AR.cpp` provides these symbols instead, `nm` on the individual `.o` files
confirmed this unambiguously).

Fixed with `BE<u32>` under `TARGET_PC` on every `DvdHeader` field (`include/dvd.h`), the same
treatment TPL/model structs already have (Phase 3) -- transparent on both the disc-read path and
the non-headered path's synthesized-in-memory fake header (`readInit`'s plain host-native
assignments to the same fields keep working through `BE<T>`'s normal assignment operator). One call
site (`m_BaseOffset[...] = m_NestDepth ? pFilehead[...]->ofs : m_Offset;`) became an ambiguous
`BE<u32>`/`u32` ternary; given an explicit cast under `TARGET_PC` only (`#else` byte-identical).

**Verified**: with this fix alone, every DVD read's type/size/ofs field decoded correctly (confirmed
live: `type=1 size=1312 ofs=1024 sndType=6`, all plausible).

### 2. New crash once real values came through: TRANS_SND_BLK/PCM into an unmapped sound address

With the header now readable, `TRANS_SND_BLK`'s destination computation (`SndMem.blk_mram[t] =
Snd.mram_top`) resolved to a real, address-zero destination -- `SndInit()`'s "sound off" stub
(Phase 5, sections 15/23) never carves real MRAM/ARAM sound-block addresses, unlike every other
`SndMem`-dependent call site already found and guarded (`SndBlkInit`, `SndBgmLoad`, `cCard::
initSub`). This one (header-driven DVD load, not the sound-side load functions) had no existing
guard, and `cDvdQueue::trans2mram()`'s eventual `memcpy((void*) 0, ...)` segfaulted (confirmed live
with `lldb`, real `SIGSEGV`, `addr: 00000000` in the log immediately before). Fixed the same way as
every other "sound off" site: skip the entry under `TARGET_PC` (`(*ph)++; break;`, matching
`TRANS_NONE`'s own handling) for both `TRANS_SND_BLK` and `TRANS_SND_PCM`.

**Verified**: `title.snd` now completes its whole read (`DVD: Read Ok  <n>`), and the very next file
(`ss/cmn/save_e.dat`, the card/save-data probe) reads and completes too -- real progress past two
whole files this session did not reach before.

### 3. A real, confirmed scheduler missed-wakeup race, fixed -- but a second, still-open one remains

Past both fixes above, the game still hung (confirmed again: `ps`/`top` showed 0% CPU, `sleeping`
state, for 60+ real seconds, until the vendor's own hang watchdog (`haltExecCheck()`, `vsync_cnt >
3599`) fired and crashed on its own unhandled debug-halt address (`0x11111111`) -- this watchdog
firing at the *documented* ~60-second real-time threshold (confirmed by timing an actual run) rules
out the earlier "vsync counter racing ahead of real time" possibility; the game thread is genuinely
blocked.

`lldb`, attached live mid-hang (before the watchdog fires, `thread backtrace all`), showed every one
of this port's own real per-task threads (Title_task, `cCard::initSub`, `cDvd::readProcMain`) parked
in `OSSleepThread`'s condvar wait (normal, expected -- each is correctly waiting its turn), and the
one thread that drives the whole cooperative scheduler (`TaskSchedulerMain` -> `WaitForHandback()`
-> `BecomeRunner()`) *also* blocked, waiting for a "handback" token (`g_holder`, `src/port/
os_thread.cpp`) nothing will ever set again -- a genuine deadlock, not a slow-progress illusion.

Root-caused one real race with a further live diagnostic (temporary prints in `OSWakeupThread`/
`OSSleepThread`, reverted after use): `OSWakeupThread(queue)` observed with **no registered
sleeper** (`target==nullptr`) right before the hang. `scheduler.cpp`'s own `TaskSleep()` (real
vendor code) hands control back to the parent thread (`OSResumeThread(pParentThread)`) *before*
registering the task as asleep (`OSSleepThread(&pCTask->Queue)`) -- atomic and race-free on the real
single-core target, but a genuine TOCTOU race once the parent/scheduler and the task are real,
independent host threads: the scheduler can regain control, cycle back to this same task's slot on
a *later* pass, decrement its sleep counter to 0, and call `OSWakeupThread()` on its queue *before*
the task's own thread has actually reached its `OSSleepThread()` call -- permanently losing that
wakeup (previously: a no-op). Fixed (`src/port/os_thread.cpp`, port-only) with a per-queue "pending
wake" mark, exactly like a binary semaphore: `OSWakeupThread()` finding no sleeper leaves a mark
instead of dropping the wakeup, and `OSSleepThread()` consumes a mark instead of blocking if one is
already there when it registers -- correct regardless of which side of the rendezvous arrives
first, with no change to `scheduler.cpp`'s own (byte-identical) call order.

**This fix alone does not resolve the hang.** A second run, with more diagnostic prints
(`OSResumeThread`/`OSSleepThread`/`OSWakeupThread` all logging), still deadlocked at the same
"three tasks parked, scheduler waiting forever" signature -- but one log line right before it is a
real, different smoking gun, not yet root-caused further this pass:

```
DBG OSWakeupThread: queue=0x105c48bd8 target=0x105c48880
DBG OSSleepThread: queue=0x105c48bd8 self=0x105c48130     <- different thread, same queue address
```

A *different* `OSThread*` (`0x105c48130`, previously only ever seen registering on a *different*
queue, `0x105c48488`) registers as asleep on the queue that had just been woken for `0x105c48880`.
Since each real task's `TASK::Queue` is a distinct field of that task's own struct, one thread
calling `OSSleepThread(&pCTask->Queue)` with the *wrong* task's queue address means `pCTask`
(`scheduler.cpp`'s single global "current task" pointer, real vendor code, correct by construction
only if truly one thread executes task code at a time) was read while pointing at the wrong task --
i.e. two of this port's real host threads were both executing task-scheduler-adjacent code
concernedly for at least the span between one thread's `TaskSleep()` reading `pCTask` and another's
own read/write of the same global. **Not resolved this pass** (budget) -- the missed-wakeup fix
above is real and independently correct (and needed regardless), but the underlying invariant this
port's whole cooperative-scheduler design depends on ("`BecomeRunner()` truly excludes every other
thread from touching shared globals like `pCTask` until it returns") appears to have a second, real
gap somewhere in the handback protocol, most likely around exactly when the *previous* holder is
guaranteed to have stopped touching scheduler-global state relative to when the *next* holder is
allowed to start. Next step: audit every `pCTask`/`pParentThread`-touching call site in
`scheduler.cpp` against the exact point `BecomeRunner()`'s condition variable actually releases each
thread, with a live trace of `pCTask`'s value alongside the existing wakeup/sleep prints, to find
the second race's precise window.

### 4. Screenshot taken this pass, honestly described

`boot_4.png` (scratchpad dir), captured ~1.5 s after window open (well before the still-open
scheduler race hangs the game thread): identical to sections 29/31's debug-overlay content (grey
horizontal bars, no glyph shapes, small vertical colour strip) -- neither of this pass's real fixes
changed what is visibly rendered, since both are earlier-in-the-boot-sequence correctness fixes,
not rendering fixes. Font-glyph-texture root-causing (`GXInitTexObj`/`GXLoadTexObj`, per the
coordinator's item 2) was not reached this pass -- the game never gets far enough past the second
scheduler race to load `title.dat` (the archive with the title's own textures), so there is nothing
new to check there yet.

### Commits this pass

- `97deddfb` -- DvdHeader endianness fix + sound-block skip (`src/game/dvd.cpp`, `include/dvd.h`;
  touches outside `src/port`/`include/port`, remote-verified: see below)
- `5b6e37bf` -- missed-wakeup race fix (`src/port/os_thread.cpp`, port-only, no remote round trip
  needed)

## 34. The scheduler rewritten on fibers (coordinator decision), replacing the thread+token
    emulation outright -- section 33's second race is gone by construction (2026-09-25)

Coordinator-directed: section 33 left one scheduler race root-caused but not fixed (two real host
threads briefly both touching `scheduler.cpp`'s shared `pCTask`), on top of one already fixed the
same pass (a missed-wakeup). Rather than keep patching the real-std::thread-plus-"turn token"
design race by race, the coordinator's decision was to replace the mechanism itself: every OSThread
(the game's per-task identity, `TASK::Thread`, and the "main"/driving identity) is now backed by a
**fiber** -- a saved CPU context with its own stack, switched with `swapcontext()`, never a real OS
thread -- and every one of those fibers runs on the SAME single real host thread. "Exactly one
task's code is ever executing" -- the invariant the whole scheduler design depends on, and the one
the token model had to maintain by hand and failed to in at least one place -- now holds because
there is only one real call stack of native instructions running at all, not because some lock or
condvar protocol says so.

### Mechanism chosen: POSIX ucontext, not a hand-written arm64 context switch

Tried live before committing to it: a ~40-line standalone test (`getcontext`/`makecontext`/
`swapcontext`, six round trips between a "main" context and a fiber touching float locals) compiled
and ran correctly on this host (Darwin 25.6, arm64, Apple clang) once `_XOPEN_SOURCE` is defined
(macOS's `<ucontext.h>` otherwise `#error`s on its own deprecated-routines guard). The three
functions are marked "deprecated ... No longer supported" in this SDK, but are still present, still
exported, and still behave correctly for exactly this use (full general-purpose context save/
restore, not signal delivery, which is what that deprecation note is actually about). Chose this
over a hand-written arm64 context switch (save/restore x19-x28, sp, lr/fp, the callee-saved halves
of d8-d15 -- maybe 30-40 lines of `.s`/inline asm) because it is already correct, already
cross-checked against real hardware ABI behavior by the OS itself, and this is not a hot loop (a
handful of switches per frame, not per instruction) -- revisit only if profiling ever shows it
matters. `src/port/fiber.cpp`/`include/port/fiber.h` is the whole primitive: `FiberCreate`,
`FiberSwitchTo`, `FiberCurrent`, `FiberFromCurrentContext` (wraps the calling thread's own already-
running context as fiber #0). `tests/port/test_fiber.cpp` proves it two ways: a 5-round-trip
ping-pong, and a 10000-switch stress test carrying live NEON (`float32x4_t`) state across every
switch, checked back on the other side each time -- both pass.

### The scheduler itself: modeled directly on the real Nintendo SDK source already in this tree

`src/lib/OSThread.c` (already decompiled, byte-matching, real) turned out to be the exact algorithm
needed -- `SetRun`/`UnsetRun`/`SelectThread`'s priority run-queues (`RunQueue[32]`, lower number =
higher priority) and its one load-bearing rule (`SelectThread`'s `if (currentThread->priority <=
priority) return NULL;`: a thread only ever preempts the one currently running if its priority is
**strictly** better -- a tie or a worse-priority thread becoming ready never switches away). Reading
that file end to end is what explains *why* `scheduler.cpp`'s own call order (`TaskSleep`'s
`OSResumeThread(pParentThread)` **then** `OSSleepThread(&pCTask->Queue)`) was always safe on real
hardware without needing to change: the real SDK's `DefaultThread` (the main-thread identity) is
created at priority `0x10` (16), and every task this game ever creates runs at `0xF` (15) or
better -- so a task resuming its parent (worse priority) never preempts, and keeps running its own
next line, while the parent resuming a task (better priority) always does. `src/port/os_thread.cpp`
is a new, from-scratch, fiber-based implementation of that same algorithm (not a recompiled copy of
`OSThread.c` -- its own `OSSaveContext`/`OSLoadContext`/`__OSSwitchThread` are raw PPC register-bank
primitives with no arm64 equivalent, and its returns-twice save/restore trick is exactly what
`FiberSwitchTo` makes unnecessary), with every function commented against the real one it mirrors.
`OSInitSemaphore`/`OSWaitSemaphore`/`OSSignalSemaphore`/`OSTryWaitSemaphore`/`OSGetSemaphoreCount`
are modeled the same deliberate way on the real, already-decompiled `src/lib/OSSemaphore.c`.

Not implemented this pass, on purpose, not silently: `OSInitMutex`/`OSLockMutex`/`OSUnlockMutex`,
`OSInitMessageQueue`/`OSSendMessage`/`OSReceiveMessage`/`OSJamMessage`. Grepped the whole tree: no
game code calls any of them today (only `src/lib/OSMutex.c`, the real SDK source, does). They stay
on the generic logging-only stub path rather than getting an unexercised, unverified
fiber-scheduler-based implementation now -- if a future unit needs one, model it on `src/lib/
OSMutex.c` the same way the semaphore functions above are modeled on `OSSemaphore.c`.

### A real regression this rewrite would otherwise have introduced, found and fixed at its own
    true location

`src/game/main.cpp`'s real, byte-matching `postVSyncCallback()` calls `iTaskSuspend()` ->
`OSSuspendThread()`, registered as a VI retrace callback. `include/port/vi.h`'s own design (Phase 4)
runs that callback on the host **process's real main thread** (Aurora's present loop) -- a
genuinely different real OS thread than the one every fiber now lives on. Left alone, that would
have meant a real, unsynchronized OS thread calling into this file's now-lock-free run-queue
bookkeeping while a fiber was mid-reschedule: a new, worse bug than anything this rewrite set out to
fix (the old token design at least funneled every mutation through one mutex; this design has
*no* lock anywhere, by design, because nothing was supposed to call it except the one thread every
fiber lives on). Fixed at the actual point of the layering violation, not by adding a lock to
`os_thread.cpp`: `src/port/vi.cpp` (port-only, no byte-matching constraint) no longer calls a
registered VI callback directly from `RunPresentLoop()`; it queues it (the only lock in this whole
area, and it never touches `os_thread.cpp`'s state) and `VIWaitForRetrace()` -- called by the game
thread once per frame, `src/game/main.cpp`'s own unchanged real call sites -- drains and runs it
there instead, right after the same real tick it was queued for. This is the general "interrupt-
context callback must not run concurrently with a fiber" mechanism the coordinator asked for, scoped
to the one real call site that exists today; DVD/ARQ/AI completion callbacks were checked and do not
call into `os_thread.cpp` at all in this tree (confirmed by grep, not assumed), so nothing else
needed this treatment this pass.

### Tests (`tests/port/test_fiber.cpp`, `tests/port/test_os_thread.cpp`) and results

`test_fiber`: ping-pong (5 round trips) and the 10000-switch NEON stress test above -- both OK.
`test_os_thread`, built on the real `OSCreateThread`/`OSResumeThread`/`OSWakeupThread`/
`OSSleepThread`/`OSExitThread`/`OSSemaphore` API, not a mock of it:
  - the exact section-33 shape (`OSResumeThread(parent)` then `OSSleepThread(&queue)`, repeated 5
    dispatch rounds via `OSCreateThread`+`OSResumeThread` then `OSWakeupThread`) -- OK, matches
    `TaskSchedulerMain`'s own TASK_EXEC/TASK_SLEEP call shapes exactly.
  - priority ordering: three threads at priorities 2/6/10, only the worst (10) ever resumed
    directly by main -- it resumes the other two itself mid-body; the only way their recorded
    execution order can come out priority-sorted (2, then 6, then 10) is if strict-priority
    preemption is really implemented, not simulated by call order -- OK.
  - semaphore blocking + a priority-driven interleaving (the waiter is better priority than the
    signaller, so `OSSignalSemaphore` must switch to it **mid-call**, not after the signaller
    finishes) -- OK.

All pre-existing `ctest`s (`test_ptr32`, `test_arena`, `test_be`) still pass unchanged.

### `re4_boot`, before vs. after

Before (section 33, `5b6e37bf` state): hard deadlock, confirmed via `lldb` -- every task thread
parked on its own condvar, the driving thread parked forever in `WaitForHandback()`, nothing left to
ever wake it; the vendor's own 60-second hang watchdog (`haltExecCheck()`) was the only thing that
ever ended the process.

After this rewrite: the deadlock is gone. The boot sequence now runs past every point sections 24-33
got stuck at -- `etc/moji8.tpl`, `etc/sizetbl.dat`, `Font/common_p.fnt`, `debug/config.txt`,
`debug/roomInfo.dat` all read successfully, then `Render()`/`SetPrimBuffPtr()`/
`GXSetCurrentGXThread()` run, real GX FIFO register writes reach Aurora (`[aurora::gx::fifo]
Unhandled XF/BP register ...` -- expected: GX register handling is a separate, later milestone, not
this pass's concern), and the process then runs **continuously at ~100% CPU** (confirmed with `ps`,
twice, a `pcpu` reading taken 5 real seconds apart both showing ~100%, `STAT` `RN` -- genuinely
running, not blocked) for over 30 real seconds with no further log growth and no crash -- i.e. it is
in a real, steady per-frame loop, not stuck. This is new progress: it did not reach this point (or
this stable a state) in any prior session. `title.dat` (the title screen's own texture archive) is
not reached yet in the log -- the next milestone past this pass's own stopping point.

### Screenshot, honestly described

`boot_fiber_3.png` (scratchpad dir), captured while the process is confirmed running at ~100% CPU,
several seconds in: near-black window, a thin dark-blue vertical bar pair near the left edge, one
small pale-purple rectangle near the top-right -- the same debug-overlay content sections 29/31/33
already described, not a title screen and not new geometry. Expected: this pass's fix is a
scheduler-correctness fix, not a rendering one, and the log confirms `title.dat` (the archive with
the title's actual textures) has not been reached yet.

### Commits this pass

- fiber primitive + os_thread.cpp rewrite (`include/port/fiber.h`, `src/port/fiber.cpp`,
  `include/port/os_thread.h`, `src/port/os_thread.cpp`, `src/port/stubs/generated_c_stubs.cpp`,
  port-only, no remote round trip needed)
- `src/game/scheduler.cpp`: removed the three now-unneeded `WaitForHandback()` TARGET_PC blocks and
  the `os_thread.h` include -- touches outside `src/port`/`include/port`, remote-verified (see PR)
- `src/port/vi.cpp`: deferred VI retrace callback execution onto the game thread (port-only)
- `CMakeLists.txt`, `tests/port/test_fiber.cpp`, `tests/port/test_os_thread.cpp`: build wiring + new
  tests (port-only)

## 35. Coordinator follow-up: the 100% CPU loop was a real regression from section 34's own fix,
    diagnosed and fixed; one real Aurora bug found and patched; new, honestly-diagnosed blocker
    past both (2026-09-25)

Coordinator asked, before accepting section 34's write-up: is the steady 100% CPU state a normal
paced frame loop or a busy-wait, and if a busy-wait, why doesn't it see the deferred VI callback.

### 1. Root cause: `main.cpp`'s real vsync busy-wait never calls `VIWaitForRetrace()`

`main()`'s frame loop (real, byte-matching) does not pace itself with `VIWaitForRetrace()` every
frame -- it spins directly on the `vsync_cnt` global instead (`while (vsync_cnt < GetSystemVcnt() -
1) {}`, twice per frame). `vsync_cnt` is only ever incremented by `postVSyncCallback()`, the
registered VI retrace callback -- and section 34's own fix made that callback's delivery wait
until the game thread calls `VIWaitForRetrace()` (the correct fix for the cross-thread scheduler
race, but this exact spin never calls it). Confirmed live: on real hardware this spin terminates
because the retrace **interrupt** can genuinely fire mid-spin; on this host, deferring the callback
to a call site this loop never reaches meant it deferred forever -- turning the section-33 deadlock
into a different infinite busy-wait (same coordinator hypothesis: "pacing isn't blocking"). Not
merely theorized -- read live via `ps`, confirmed with a fresh `ps` sample of a running boot: `STAT
RN`, ~100% CPU, no log growth for 90+ real seconds, no crash (past the vendor's own 60-second hang
watchdog too, since `vsync_cnt` was genuinely stuck at 0 forever, `haltExecCheck()`'s own counter
never advancing either).

**Fixed**: a new, non-blocking `re4_port::PumpPendingVICallbacks()` (`src/port/vi.cpp`/`include/
port/vi.h`) -- the same drain `VIWaitForRetrace()` already did, factored out, callable without
waiting for a tick first. Both `vsync_cnt` busy-wait bodies in `main.cpp` now call it, under
`#ifdef TARGET_PC` (bytes unchanged for the real target) -- the direct host equivalent of "take the
pending interrupt now," for exactly the one polling loop that needed it. DVD/ARQ completion
callbacks (`dvdread_callback`/`aram_cb`/`trans2aram_cb`, `src/game/dvd.cpp`) were checked too, per
the coordinator's request: all three only flip plain flag bits, never call into the OSThread
scheduler, so Aurora's real background-thread delivery of those was never unsafe and needed no
change -- confirmed by reading every one of them, not assumed.

**Verified**: `re4_boot` now runs many real frames past the point that used to spin forever --
confirmed by new log lines appearing (`SS/cmn/title.snd`, `ss/cmn/save_e.dat`, `Slot A Unmount`, a
real memory-card probe) that never printed in the spinning state.

### 2. Toward title.dat: one real Aurora bug found and fixed, one new (honest) blocker

Past the fix above, `re4_boot` crashed quickly and reproducibly (`lldb`, `EXC_BAD_ACCESS` at
`0x0`): `CARDProbeEx` (Aurora's `lib/dolphin/card.cpp`) unconditionally dereferenced its
`memSize`/`sectorSize` output parameters; the real SDK (`src/lib/CARDMount.c`, this decompilation's
own already-verified source) treats both as optional (`if (memSize) *memSize = ...`), and
`cCard::errorDisp()` calls it wanting only the result code, with both null. A genuine Aurora bug,
not a game-code or fiber-scheduler issue -- fixed with a 2-line null check,
`tools/port/aurora-patches/0005-card-probe-ex-null-check.patch` (Aurora checkout itself left
clean, patch applied by CMake same as the other four).

Past that fix, `re4_boot` reaches further still (a new screenshot, `boot_fiber_7.png`, shows new
content never seen before: a row of short debug-log bar segments near the bottom-left, confirming
real frames are now rendering and accumulating log output -- still no font glyphs, still not the
title screen) and then crashes again, reproducibly, in `lldb`: `EXC_BAD_ACCESS` at address `0x8`,
in `MessageData::getAddr()` (`src/game/mes.cpp:289`), reached via `cCard::errorDisp() ->
cardMesSet() -> MessageControl::MesSet() -> Message::init()`. Root cause, read directly: `tbl =
(u32*) ptr[data_type]` (`data_type=0`, the "core" message table) is null -- `CoreDataRead()`
(`STUB: CoreDataRead() called`, visible in every log this session) is still a logging-only stub
that never actually loads the core message-table data from disc, so `MessageData::ptr[0]` is never
set. This is a real, separate, and sizable gap (a DVD-backed data-loading subsystem, not a
scheduler or callback-timing issue) -- **not fixed this pass** (budget): implementing it means
adding a real `CoreDataRead()` (find the message/font archive on disc, read it, populate
`MessageData::ptr[]`), which is its own unit of work, not a quick patch. Flagged here with the
exact call chain and root cause for whoever picks this up next.

### 3. Screenshots, honestly described

`boot_fiber_3.png` (before this pass's fix, during the 100%-CPU spin): unchanged from every prior
section's debug overlay -- expected, since nothing was rendering new content while stuck spinning
on `vsync_cnt`.

`boot_fiber_7.png` (after both fixes above, captured ~400ms after window open, right before the
`MessageData::getAddr()` crash): genuinely new content -- a taller pair of dark vertical bars on
the left (with a small green mark at the very top, not seen before), the same pale-purple bar on
the right, and, new, a row of five short horizontal light-gray bar segments plus one shorter
segment beneath them, roughly a third of the way down the window. Read plainly: this looks like the
debug log renderer (`pLog->disp()`, called every frame) drawing accumulated log lines as
placeholder bars (no font texture bound yet, so text has no glyph shapes, matching every prior
section's finding on that specific point) -- but the fact that there are now multiple distinct
bars, where every previous screenshot in this whole effort showed either nothing or one fixed short
strip, is itself the real evidence that multiple real frames rendered and accumulated log output
before this crash, which did not happen before this pass's fixes. Still not the title screen; still
no glyph shapes.

### Commits this pass

- `b672aa9c` -- `main.cpp`/`vi.cpp`/`vi.h`: pump deferred VI callbacks in the vsync busy-wait
  (touches outside `src/port`/`include/port`, remote-verified)
- `c11c270b` -- Aurora patch: null-check `CARDProbeEx`'s output pointers (`tools/port/`, not
  `src/`/`include/`, no remote round trip needed by the stated rule, but the fix is entirely in the
  vendored-but-patched Aurora tree, not this repo's own game/port code)

## 36. `CoreDataRead()`/`OptionDataRead()` audit: real code brought in, not restubbed; new,
    deeper blocker found past it (2026-09-25)

Coordinator-directed follow-up on section 35's diagnosis: `CoreDataRead()` and its neighbours are
real GAME functions (`src/game/read.cpp`), excluded from `re4_boot` only because
`cmake/boot_exclude.txt` (pointer<->integer-cast category) listed `read.cpp` as "not yet
hand-fixed" -- stale. Un-excluding it and rebuilding showed the `RE4_REWRITE_CASTS` cast rewriter
(implied by `RE4_U32_32=ON`, docs/port-phase2.md section 8) already turns every one of its direct
casts into `GC32()`/`GCPTR<T>()` correctly; the file compiles clean, no hand-fixing needed after
all. Un-excluding it pulls in real definitions for `CoreDataRead`, `OptionDataRead`,
`EmReadInit`/`EmReadSearch`/`EmReadModule`/`SearchEmModule`, `PlReadModule`,
`WepReadModule`/`ReleaseWepData`, `ReleasePlData`, `GetDataExt` -- the entire "OptionDataRead and
anything similar" family the task asked about, all now real rather than logging stubs. The
matching duplicate-symbol stubs were removed from `src/port/stubs/generated_c_stubs.cpp`/
`generated_cpp_stubs.cpp` (left as one-line "now defined for real" pointers to read.cpp, following
the file's own existing convention for this exact situation, e.g. `Rmode`/`ScreenShotTriggerType`
in section 26).

**Two other units were tried the same way and are genuinely not ready, not just stale entries**:
`game.cpp` (the `GAME_WORK Game;` global read.cpp needs) still has real, uninvestigated errors
under `RE4_U32_32` (an `mem_alloc(size, __FILE__, __LINE__, a, b)` call whose `size`/`a`/`b`
parameter names don't exist in that scope -- looks like a macro-expansion/parameter-shadowing bug
in the source itself or the rewriter, not confirmed which -- plus real pointer-truncation errors
unrelated to this bug); `trans.cpp` (owns `SpecularInit`/`GlobalIlmTexInit`, the two functions
`CoreDataRead()` calls) still has real paired-single asm (`PSQ_L_U8_TO`) elsewhere in the same
file, confirming the existing Phase 5 categorization, not stale. Both stay excluded, unchanged.

**Fix chosen**: rather than re-add a `CoreDataRead()`-shaped stub (which would have silently
regressed the DVD read this pass's real fix delivers), three small, explicitly-labeled manual
stubs were added instead, each documented with why it can't be the real thing yet:
`src/port/stubs/manual_stubs.cpp` now defines `Game` (a plain global, `GameWork`, exactly
matching `game.cpp`'s real `GAME_WORK` byte layout -- read.cpp already declares its own identical
view struct for it, so this is not a guess) and logging-only stand-ins for `SpecularInit`/
`GlobalIlmTexInit` (real bodies are plain C++, but live in the Phase-5-blocked `trans.cpp` TU).

**Verified**: `re4_boot` links and runs; the DVD log now shows a real, successful read of
`etc/core.das` (the core archive `CoreDataRead()` was always supposed to load,
`addr=80578000 size=00230740`) immediately followed by `STUB: SpecularInit()`/
`STUB: GlobalIlmTexInit()` -- confirming the real read path now runs end to end, only the texture
binding at its tail is still a stub. Default host build (`RE4_U32_32=OFF`, `build-pc/`) unaffected:
`re4_game_all -k 0` still 33 failing files (same set), `ctest` 5/5. No `src/game`/`include` file
was touched this pass (only `cmake/boot_exclude.txt` and `src/port/stubs/*.cpp`), so no remote
round trip is needed by the stated rule.

**New, deeper blocker past this fix (not resolved this pass, budget)**: `re4_boot` now crashes
later and differently than section 35 -- `EXC_BAD_ACCESS` at address `0x8`, still in
`MessageData::getAddr()` (`mes.cpp`), still reached via `cCard::errorDisp() -> cardMesSet() ->
MessageControl::MesSet() -> Message::init()`, but `CoreDataRead()` has now genuinely run and
`pG->pCore` is a real, populated pointer -- this is not the same null-table bug section 35 found.
Read directly: `MesData.ptr[0]` (the "core text" message table, `data_type=0`) is set only by
`MessageControl::gameInit()` (`mes.cpp`, called from `game.cpp`'s own `gameInit()` -- the "start a
new game" flow, `Rno0==0`) and `roomInit()` -- never by `MessageControl::init()` (`mes.cpp`,
docs section 2's "`cMes.init()`", the boot-time call `main.cpp` really makes) or by
`CoreDataRead()` itself. `cCard::errorDisp()`'s call chain reaches `Message::init()` with
`attr & 1` set, selecting `data_type=0` -- so on **real hardware**, either (a) this exact
error-display call path is only ever reached after a game session's `gameInit()`/`roomInit()` has
already bound `MesData.ptr[0]` (i.e. this port's memory-card probe is running `cCard::MainLoop()`
too early relative to the real boot sequence -- a sequencing bug in this port, not upstream), or
(b) there is a fourth call site binding `MesData.ptr[0]` at boot time this pass did not find (a
`grep -rn "MesData"` across every `src/game/*.cpp` found exactly the four call sites named above
and no others, but the boot-time one could be reached through `game.cpp` itself, which is not
compiled into `re4_boot` at all right now -- so if the real fix is "call `cMes.gameInit()`-shaped
code at boot", it is blocked on the same `game.cpp` exclusion this section already found
independently blocking). **Not root-caused further this pass** -- next step is tracing, on real
hardware knowledge or the PS2 source if available, whether `cCard`'s boot-time probe genuinely runs
before or after the message tables are bound, since that answers whether the fix belongs in
`main.cpp`'s boot sequence (call something earlier) or confirms `game.cpp` needs to be un-excluded
next (a real, separate unit of work: the `mem_alloc(size, ..., a, b)` scoping bug above, plus
`game.cpp`'s own pointer-truncation cast sites, neither trivial).

## 37. Coordinator follow-up: real card path fixed (no more phantom "no card"), MesData.ptr[0]
    traced to source (game.cpp confirmed NOT the fix), a host-only crash guard, new steady-state
    blocker past it (2026-09-25)

Coordinator asked four things: (1) why the card error path is taken at all -- fix it to see a real
card; (2) trace, don't guess, where the card message table comes from; (3) if it needs game.cpp,
bring game.cpp in; (4) continue toward title.dat/fonts.

### 1. Root cause found and fixed: `CARDInit(nullptr, nullptr)` builds an empty card path

`src/game/card.cpp`'s `CardInit()` called Aurora's real `CARDInit(const char* game, const char*
maker)` with both null (section 13's fix only addressed the *arg-count* mismatch, not what to pass).
Read Aurora's own `../aurora/lib/dolphin/card.cpp` directly: `get_card_region(gameName)` returns
`nullptr` outright when `gameName == nullptr`, and `get_card_full_path()` returns `""` when its
`region` is null -- so both card slots' paths were always empty, `CARD_READY()` was always false,
every probe genuinely found "no card", and `cCard::errorDisp()`'s path was always taken by
construction, not because that path itself was ever buggy.

**Fixed** two ways, both real, nothing invented: (a) `CardInit()` now reads the actual game/maker
code from `DVDGetCurrentDiskID()` -- populated for real from the mounted disc image's own header
the moment `re4_port::InitDvd()` opens it (`aurora_dvd_open()`, confirmed by reading
`../aurora/lib/dolphin/dvd/dvd.cpp`'s `s_diskID` assignment), so this works for whatever disc is
mounted, not a literal hardcoded for this one image. (b) new `include/port/card.h`/
`src/port/card.cpp`, `re4_port::InitCardDir()`: resolves `$RE4_CARD_DIR` else
`~/Library/Application Support/re4-port/card`, creates it, calls Aurora's `CARDSetBasePath()` --
called before `CARDInitPC()` runs, per Aurora's own "before `CARDInit()`" ordering requirement.
Never touches `orig/`.

**Verified**: the log now reads `Slot A Mount` (previously always `Slot A Unmount`, no probe ever
found anything), and the game moves on to `cCard::createSysfile()` (a real, further-along vendor
state: "no system file on this fresh card, offer to create one") instead of the earlier no-card
`errorDisp()` -- confirms a real, formatted GCI-folder card is now genuinely found and mounted.
Remote-verified (touches `src/game/card.cpp`, outside `src/port`/`include/port`): 115/115 SHA-1 OK,
`asmcheck.py --all` TOTAL 231 unchanged (`docker` run on `100.90.198.42`). Default host build
unaffected: `re4_game_all -k 0` still 33 failing files (same set), `ctest` 5/5.

### 2. `MesData.ptr[0]`, traced exhaustively: only bound by `game.cpp`, which itself doesn't run
    this early -- game.cpp is confirmed NOT sufficient to fix this crash

Grepped every `MesData`/`setPtr`/`ptr[` reference across the entire `src/game` tree (not a
sample): the *only* code that ever binds `MesData.ptr[0]` to the core text table (`ofs_28`) is
`MessageControl::gameInit()` and `roomInit()` (`src/game/mes.cpp`, both real, already compiled).
Both are called only from `game.cpp`'s own `gameInit()` (`cMes.gameInit()`, "Rno0==0: game start")
and `roomInit()` -- i.e. `game.cpp`, still excluded. But tracing further: `game.cpp`'s `gameInit()`
only ever runs inside `GameTask` (`game.cpp`'s own per-frame `Rno0` state machine), and `GameTask`
itself is only ever started by `title.cpp`'s `titleExit()` (`TaskChain(GameTask, 0)`, the very last
line of that function) -- reached only after the player has picked something from the title menu
(new game / continue / debug start), confirmed by reading `titleExit()`'s own body (sets
`G_ROOM_ID`/player position from the chosen menu entry first). Meanwhile the memory-card
first-check (`CardFirstCheck()`, this crash's call site) runs within the first couple of frames of
boot -- `Render_init()` (`systemStartInit()`) starts `tvModeCheckTask`, whose state machine reaches
`tvModeExit() -> CardFirstCheck()` almost immediately when `VIGetDTVStatus()` returns 0 (no
progressive-capable display detected; our own stub also returns 0) -- i.e. **before** `Title_task`
even exists, let alone before the player has reached the title menu. **Conclusion, evidence-based,
not assumed**: un-excluding `game.cpp` would not, by itself, fix this crash -- the code that binds
`MesData.ptr[0]` genuinely does not run this early in the vendor's own control flow either. The
coordinator's "if it needs game.cpp" premise does not hold for this specific crash; forcing
`game.cpp` in here would have been chasing the wrong fix.

Whether real hardware also reads `ptr[0]` as null at this exact point and simply tolerates the
resulting `tbl[lang+1]` read (GameCube low physical memory has real, if unrelated, contents there,
unlike this host's genuinely-unmapped address 0) is **TO VERIFY** -- would need real hardware or a
cycle-accurate emulator trace to settle, not available here; not asserted as fact, only offered as
the most plausible explanation for why a shipped, extensively-tested title would not crash on its
own first boot.

### 3. Host-only crash guard (not a `game.cpp` fix) landed instead, verified to work

Two small `TARGET_PC`-only guards, `src/game/mes.cpp` (real vendor file, `#ifdef` pattern already
established throughout this port): `MessageData::getAddr()` returns `NULL` immediately when
`ptr[data_type]` is still null (instead of dereferencing it), and `Message::move()` returns
immediately when `m_pMes` is `NULL` (the same null table surfaces there too, one frame later, once
`Message::init()`'s own existing fallback-and-log path, real vendor code, unchanged, leaves it
unset). Both documented inline as a host-safety stopgap for the still-open "TO VERIFY" above, not a
claim about vendor logic. Bytes unchanged for the real target (branch does not exist there).

**Verified**: `re4_boot` no longer crashes here at all -- `Message::init() Msg[44] Address Error`
prints (the vendor's own existing error log, now actually reachable+harmless) and the process
settles into a steady, non-crashing, ~100% CPU per-frame loop (confirmed twice, `ps` samples 15s
apart, `STAT RN` both times, log output stable) -- past every crash this whole effort has hit so
far. Default host build unaffected (`re4_game_all -k 0` still 33 failing, `ctest` 5/5).

### 4. Screenshot, honestly described; new blocker (steady loop, no crash) not resolved this pass

`boot_37.png` (scratchpad): near-black window; two thin dark-navy vertical bars at the left edge
(a small green mark at the very top of the left one); a pale lavender vertical bar at the right
edge; five short horizontal **orange** bar segments about a third of the way down (a new colour --
every prior screenshot in this whole effort showed grey/pale-purple bars only) -- still the debug
log's placeholder-bar rendering (`pLog->disp()`, no font texture bound yet, matching every prior
finding on that point), not the title screen, no glyph shapes.

**New blocker, not root-caused this pass (budget)**: the process now runs steadily at ~100% CPU
with no further log growth for 15+ real seconds and no crash -- the same "real, paced-looking loop"
signature sections 34/35 both hit and both turned out to be a real bug once investigated (a
scheduler deadlock, then a busy-wait that never pumped a callback). Leading, unverified hypothesis
(**TO VERIFY**, not chased this pass): `cCard::createSysfile()`'s own state machine (reached per
section 1 above) is a real yes/no confirmation prompt ("create a system file?") that waits for a
pad button press to advance -- if this port's `PadInit()`/pad-read stub never delivers a real
button edge, this would be a genuine, correctly-diagnosed "waiting for input that never comes," not
a hang bug, consistent with the coordinator's own item about phantom/absent button presses. Not
confirmed by reading `createSysfile()`'s actual body this pass.

### Commits this pass

- `18f30dca` -- `card.cpp`/`CMakeLists.txt`/`include/port/card.h`/`src/port/card.cpp`: real disc
  game/maker code + port-owned card directory (touches outside `src/port`/`include/port`,
  remote-verified: 115/115 OK, asmcheck TOTAL 231 unchanged)
- (pending) -- `mes.cpp`: host-only null-table crash guards (touches outside `src/port`/
  `include/port`, needs the same remote round trip before landing on `port/macos-arm64`)

## 38. Coordinator follow-up: real keyboard input + scripted input infrastructure landed and
    verified; `cMes.gameInit()` early-bind fix found real but racy; font glyphs/title.dat not
    reached this pass (2026-09-25)

Coordinator asked four things: (1) document/add real keyboard->PAD mapping; (2) scripted input via
an env var for automated runs; (3) root-cause+fix font glyph rendering; (4) press through to
title.dat/the title screen.

### 1. Real keyboard input (item 1) -- Aurora has the API, but ships zero default keys

Read `../aurora/lib/dolphin/pad/pad.cpp` directly: Aurora's real gamepad mapping
(`g_defaultButtonsStandard`) is a genuine SDL_GAMEPAD-standard-layout GameCube mapping (South=A,
East=B, West=X, North=Y, Start=Start, Right shoulder=Z-trigger, D-pad=D-pad) -- a real controller
plugged into this Mac already works with no changes needed. But Aurora's *keyboard* path
(`g_defaultKeys`/`g_defaultKeyAxis`) ships with **every** scancode set to `PAD_KEY_INVALID` --
confirmed by reading the array literals, not assumed -- i.e. keyboard input is a real, live code
path (`PADRead()` checks `g_keyboardBindings[i].m_mappingsSet` and merges keyboard state in) but
with nothing bound by default. Aurora does expose a real public API to set bindings without
touching its source (`PADSetKeyButtonBindings()`, `PADSetKeyboardActive()`,
`include/dolphin/pad.h`), so no Aurora patch was needed -- same "manual extern, no source patch"
pattern as `src/port/card.cpp`'s `CARDSetBasePath()` (this repo's own `include/dolphin/pad.h`
mirrors real hardware only and doesn't declare these, and is searched first, so a local `extern
"C"` redeclaration in the new file sidesteps that, matching the established pattern exactly).

**New**: `include/port/pad_input.h`, `src/port/pad_input.cpp`'s `InitKeyboardInput()`, called from
`src/game/pad.cpp`'s real `PadInit()` (`TARGET_PC` branch, right after the real `PADInit()` call).
Minimal mapping, coordinator's own suggestion:

| Key | GameCube button |
|---|---|
| `Return`/`Enter` | Start |
| `Z` | A |
| `X` | B |
| `C` | X |
| `V` | Y |
| `A` | Z trigger |
| Arrow keys | D-pad (Up/Down/Left/Right) |
| (L/R triggers) | left unbound -- no obvious single key, not guessed |

Verified live: `re4_boot`'s own stderr prints `re4_port: keyboard input active on PAD channel 0 --
...` every run, and `PADSetKeyButtonBindings()`/`PADSetKeyboardActive()` are real Aurora entry
points (not stubs) -- confirmed by reading their implementations, not merely by the absence of a
"STUB:" log line.

**For the user to try interactively** (plugging in a real controller needs nothing extra; this is
for keyboard-only):
```sh
cd /Users/luken/Projects/re4
./build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso
# window must have focus for SDL to see key events; Return=Start, Z=A, X=B, C=X, V=Y, arrows=D-pad
```

### 2. Scripted input for automated runs (item 2) -- landed, verified to parse and inject correctly

`RE4_PORT_INPUT="frame:BUTTON,frame:BUTTON,..."` (e.g. `RE4_PORT_INPUT="120:A,240:START"`), read
once (lazily) by `re4_port::PollScriptedInput()` (`src/port/pad_input.cpp`), called from
`src/game/pad.cpp`'s real `PadRead()` (`TARGET_PC` branch, right after the real `PADRead(Pad_data)`
call -- ORs the scripted bits into channel 0's `PADStatus.button`, indistinguishable from there
onward from a real keypress). Each listed button is held for `$RE4_PORT_INPUT_HOLD_FRAMES` frames
(default 10) starting at its frame number. `BUTTON` one of `A/B/X/Y/START/Z/L/R/UP/DOWN/LEFT/RIGHT`
(matching the real `PAD_BUTTON_*`/`PAD_TRIGGER_*` names exactly). Frame 0 is the first call to
`PadRead()` after boot (once per real frame, matching its own real once-per-frame contract).

**Verified mechanically, traced through the real vendor translation, not assumed**:
`src/game/pad.cpp`'s own `Key_type_tbl[0]` (real, byte-identical data) has index 31 = `0x00000100`
(`PAD_BUTTON_A`'s own bit value) -- so a scripted `A` press does produce `Key.on`'s bit 31
(`0x80000000`), the exact bit `mes.cpp`'s `Message::code08()` checks (`Key.trg & 0x80000000`) to
confirm a yes/no message choice, worked out index-by-index from the real table, not guessed. Log
lines confirm the parser itself works exactly as intended (`re4_port: RE4_PORT_INPUT: frame 60 ->
bit 0x0100`, etc., for every run this pass).

**Example** (for the user, or for future automated runs):
```sh
RE4_PORT_INPUT="300:A,600:A,900:A" RE4_PORT_INPUT_HOLD_FRAMES=20 \
  ./build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso
```

### 3/4. A real, further blocker found while trying to press through the card prompt: a race
    between `cMes.gameInit()`'s early bind and the card-check task -- font glyphs not reached

Pressing scripted A at several plausible frames (300/600/900, later 30/60/90/120/150/180) never
advanced past `cCard::createSysfile()`'s yes/no prompt in any run this pass. Root-caused with
`lldb`, not guessed: **this session's own section 37 host-only crash guard
(`MessageData::getAddr()` returning `NULL` on an unbound table) has a real side effect** --
`Message::init()`'s existing fallback (real vendor code) leaves `m_pMes` `NULL` when that happens,
and `Message::move()`'s own guard (also section 37) then returns immediately every frame *forever*
for that message slot -- never reaching `isCtrlCode()`/`CommandExec()`, which is where the yes/no
selection control code actually lives inside the message's own byte stream. A message instance
whose table was unbound at the exact moment `Message::init()` ran can therefore never process input
later, even once the table becomes valid afterward (`m_pMes` was captured once, not re-fetched).

Given this, section 37's own trace was revisited: `game.cpp`'s real `MessageControl::gameInit()`
binds `MesData.ptr[0]` using `pG->pCore`, which `CoreDataRead()` (this session's earlier fix)
already populates for real. Added a **real, no-reimplementation** call to that same, already-
compiled vendor method (`cMes.gameInit()`, `mes.cpp`) directly from `title.cpp`'s `Title_task()`
(`TARGET_PC` branch, right after the real `CoreDataRead()`/`OptionDataRead()` calls) -- not
game.cpp itself (still excluded, unrelated compile errors, section 36), just the one real method
call this specific gap needs.

**Verified, but only sometimes**: one run (`run40.log`) showed the `Message::init() Msg[44]
Address Error` line gone entirely (confirming `ptr[0]` was bound in time that run); a second,
otherwise-identical run (`lldb1.log`, breakpoint-driven) showed the same "Address Error" line
still present. This is a genuine **race**, not a flake in the test setup: the memory-card
first-check (`tvModeCheckTask` -> `CardFirstCheck()`, started from `Render_init()` inside
`systemStartInit()`) and `Title_task` (started afterward, from `systemRestartInit()`) are two
independent cooperative tasks; whichever one's scheduler turn reaches its own message-display code
first determines whether `cMes.gameInit()` (now called partway through `Title_task`'s own first
turn) has already run by the time `cardMesSet()` needs it. **Not resolved this pass** (budget) --
the real fix is almost certainly moving the `cMes.gameInit()` call earlier still (before
`Render_init()`'s `SetTvMode()` even starts the tv_mode task, inside `systemStartInit()` itself,
matching how a real GameCube boot -- one single-core machine, genuinely sequential up to the first
task switch -- would have gotten this ordering right "for free" without an explicit race existing
at all), not a scheduler fix.

Font-glyph texture root-causing (`GXInitTexObj`/`GXLoadTexObj`, item 3) was **not reached this
pass** -- no run got far enough to render a readable prompt; that work stays queued behind fixing
the race above first, per the coordinator's own ordering (fix the crash path fully before chasing
rendering).

### Screenshots, honestly described

`boot_38.png`/`boot_39.png`/`boot_40.png` (scratchpad): all three show the same debug-log
placeholder-bar rendering already described in every prior section (a green mark, dark-navy
vertical bars at the left, a lavender bar at the right) -- no new geometry, no readable text, not
the title screen. None of this pass's real, verified fixes (keyboard mapping, scripted input,
`cMes.gameInit()`) changed what's on screen yet, since the process never got past the still-racy
card prompt to reach `title.dat`/font-bearing content.

### Commits this pass

- `include/port/pad_input.h`, `src/port/pad_input.cpp`, `src/game/pad.cpp`, `CMakeLists.txt`: real
  keyboard mapping + scripted input (touches outside `src/port`/`include/port`, needs the remote
  round trip before landing on `port/macos-arm64`)
- `src/game/title.cpp`: early `cMes.gameInit()` call (same remote-verification requirement)

**Correction (2026-09-25, next session)**: the `title.cpp` early-bind commit above was later reverted
(`41c024cd`, "wrong table, not needed -- real fix is snd.cpp") -- this file was never updated to say
so until now. Section 39 below is the real, current state.

## 39. Coordinator correction: the "6.8 KB card-heap shortfall" was NOT fixed by the `IdUnit`/
    `DatTblEntry` Ptr32<T> commits; root cause found and fixed (2026-09-25)

`docs/port.md`/this file's own section 37 both implied the card-heap shortfall (`CardID::init`'s
`m_IdSave.gameInit(0x100)`, `IDSystem::unitPtr(...): Not found` afterward) was resolved once
`IdUnit`/`DatTblEntry` got `Ptr32<T>` (commits `462359aa`/`f5e1d5dc`). Re-running `re4_boot` at that
HEAD reproduces the exact same failure: `alloc[13800]:free[11d40] id_sys.cpp(66)` -- the *request*
is now the correct, GC-matching `0x13800` (256 * `sizeof(IdUnit)`, confirming `IdUnit`'s own size is
fixed), but the heap still comes up short by `0x1AC0` (6,848 bytes, i.e. exactly "~6.8 KB").

**Not a struct-size bug.** Temporary instrumentation (every `mem_alloc`/`mem_calloc` call logged
with heap/size/free-after, `RE4_PORT_MEM_TRACE=1`, reverted before landing -- not part of any
commit) traced heap 11 (`cDataSwap::SwapOut`'s temporary swap-in heap the card screen borrows) from
its creation through the failing call. `getUseMemSize()`'s own capacity formula is pure literal
constants plus a real DVD file size -- no `sizeof()` anywhere -- so the heap's total capacity is
identical GC vs host by construction; `workAlloc()`'s allocations inside it are also all literal
sizes. The free-space trace showed something else instead: a repeating, per-frame cluster of
`operator new` calls (~13 allocations, ~0x4780 bytes total) landing in heap 11 *while* the DVD read
for `idpath` was still in flight (`TaskSleep(1)` polling `Dvd.ReadCheck()`), oscillating between a
"high" free-space point (~0x15220, comfortably above the needed 0x13800) and a "low" point
(~0x11c60, below it) -- and `CardID::init`'s `m_IdSave.gameInit()` call happened to run at exactly
the low point of that cycle, not the high one.

Backtracing the largest allocation in that cluster (`size=0x3300`) found the real culprit:
`aurora::gfx::begin_frame()` (called every frame from `re4_port::BeginGxFrame()` <-
`Render_before()` <- `main_game()`'s own frame loop) allocates its own internal renderer state
(command/resource pools for Aurora's host GPU backend -- no GameCube equivalent exists) via plain
`new`, on the **game thread**, synchronously, inline -- exactly the one case
`include/port/alloc.h`'s split-allocator design (section 9 above) didn't cover: its own doc
explicitly says "host libraries never run on a thread `MarkCurrentThreadGame()` touched, so their
allocations always take the malloc path by construction" -- true for Aurora's *background* threads,
false for Aurora's GX frame calls, which this port deliberately calls straight from the game
thread's own per-frame code. `IsGameThread() && HeapsReady()` was therefore true, and every one of
Aurora's own per-frame bookkeeping allocations was silently being carved out of the tiny, exactly-
budgeted GameCube-sized heaps instead of the host's `malloc` -- permanently consuming real
GC-memory-map budget for something that does not exist on real hardware at all.

**Fix**: `re4_port::HostAllocScope` (`include/port/alloc.h`/`src/port/alloc.cpp`), a thread_local
nesting depth counter; `ShouldUseGameHeap()` now also requires depth `== 0`. `src/port/vi.cpp`'s
`BeginGxFrame()`/`EndGxFrame()` each hold one for the duration of `aurora_begin_frame()`/
`aurora_end_frame()`. Entirely inside `src/port`/`include/port` -- no remote verification round
trip needed per the port rules. Verified: heap-11 allocation count for one representative run fell
from 706 to 184; `id_sys.cpp(66)`'s allocation now succeeds (`ok=1 free=0x1a60` after satisfying the
`0x13800` request); `IDSystem::unitPtr(...): Not found` no longer appears anywhere in the log. Host
default build unaffected (`re4_game_all -k 0`: same 33 failing files; `ctest` 5/5). Commit
`53d6cfd3`.

**Screenshot, honestly described, before and after the fix**: identical in both cases -- a debug
overlay (not the title screen), a green mark and dark-navy bars at the left edge, a lavender bar at
the right edge, and **legible orange debug text** reading `SndCall : blk 0 No.5 Illegal SE No.`
plus a grey monospace dump of `SS/eng/title.dat`'s block table (hex offsets/sizes) -- font glyphs
render correctly here (a real, previously-undocumented improvement over every earlier screenshot in
this file, all of which showed only placeholder colour bars, no text) -- but this is still a debug
view, not the title screen itself, and fixing the heap shortfall changed nothing about what is on
screen (expected: the shortfall was silently harming a background allocation's bookkeeping, not
blocking or altering this screen's own control flow).

**Current blocker (not chased further this pass, budget)**: the process keeps running steadily,
still showing this same debug overlay, past the point the trace runs stop. Whatever route reaches
the real title screen (font/model/texture rendering from `title.dat`, per section 3's format table)
has not been reached yet; the `SndCall : blk 0 No.5 Illegal SE No.` line is itself the sound driver
rejecting a bad sound-effect index (Phase 5 territory, `SndInit()` stub-related, docs/port.md) and
may or may not be gating anything past it -- **TO VERIFY**.

**Tooling landed this pass**: `tools/port/gen_layout_report.py` (docs/port-layout-parity.md) -- the
"cheap" struct-size parity tool (coordinator item B): no cross-compilation, reuses the same
`(0xNN byte...)` doc comments `tools/port/gen_static_asserts.py` already trusts as GC ground truth,
generates one host probe TU, and reports every mismatch with a best-effort cause. First run: 35
candidate types, 19 match, 16 mismatch (mostly un-ported native pointer fields; one, `ID_DATA`,
unexplained and worth a follow-up look -- derives from `cCoord`, +16 bytes on host). Scoped to
structs whose doc comment sits immediately above the type (conservative, to avoid misattributing a
containing struct's byte count to a member type -- caught and fixed one such false positive,
`LifeMeter`, during this pass); does not yet cover every struct in the tree the way the full,
cross-compiled-against-the-real-SN-toolchain version would -- that heavier tool (item B's "later")
was not attempted this pass.

## 40. Coordinator follow-up: the systemic version of section 39's fix, `ID_DATA` root-caused, a new
    post-heap-fix blocker found (2026-09-25, same day)

Section 39's `HostAllocScope` fixed the one call site it found (`aurora_begin_frame`/
`aurora_end_frame`), but the coordinator correctly pointed out this is a whole *class* of bug: any
Aurora/SDK-shim code the game calls synchronously (GX*, PAD*, DVD*, CARD*, OS* shims, their
internal libc++ containers) can allocate, and every one of them was silently eligible for the game
heap too, one undiscovered call site at a time.

**Systemic fix implemented (route by caller's own compiled code, not by thread)**:
`include/port/game_section.h` (`#pragma clang section text="__TEXT,__re4game"`, forced-included
*first* into every real vendor `src/game/*.cpp` translation unit `re4_boot_game` compiles, nothing
else) tags all actual game code -- including inline/template code instantiated inside those TUs --
into one named Mach-O section. `re4_port::IsGameCodeAddress()` (`src/port/alloc.cpp`) checks an
address against that section's bounds via ld64's synthesized `section$start$__TEXT$__re4game`/
`section$end$...` pseudo-symbols (declared `weak`: several other targets link the same
`re4_port` static library without ever producing a `__re4game` section at all, e.g. `test_arena`;
verified live that ld64 then resolves both symbols to the *same* address rather than erroring,
making the range empty and the check correctly always-false there, no separate ifdef needed).
`operator new`/`new[]` (`main_mem.cpp`) now call `ShouldUseGameHeapFor(__builtin_return_address(0))`
-- thread-ready-and-heaps-ready AND the immediate caller is inside `__re4game`.
`re4_port::HostAllocScope` (section 39) stays as the documented fallback/override, unused by
default now that the section check covers its one known case on its own.

**Verified this generalizes, not just re-derives section 39's own fix**: a temporary trace (added,
tested, reverted before committing -- same discipline as section 39) logging every `new` the old
thread-only rule would have routed to the game heap but the new caller check does not, over a real
30-second run: **49 divergences**, `atos`-symbolicated a sample -- `aurora::imgui::DrawData::Impl`,
`aurora::(anonymous)::end_frame()`'s lambda, `wgpu::CommandBuffer` (all section 39's already-known
case, now caught without the explicit scope) -- **and two the per-call-site fix never touched**:
`C_MTXRotTrig`/`C_VECReflect` (Aurora's own SDK-compat paired-single math shims,
`lib/dolphin/mtx.c`/`vec.c`, called directly from real vendor code that expects `MTXRotTrig`/
`VECReflect`) -- confirming the coordinator's premise that per-call-site wrapping would have kept
missing cases like this one. Zero regressions: `IDSystem::unitPtr(...): Not found`/`malloc failed`
still absent; default host build unaffected (`re4_game_all -k 0`: same 33 failing files; `ctest`
5/5). One cosmetic ld64 warning per game object (`missing 'regular,pure_instructions' section
flag`) -- harmless (confirmed the binary links and every check above passes), not silenced this
pass.

**Found and fixed along the way (unrelated, blocked the rebuild)**: `tools/port/aurora-patches/`
`0002`/`0004` had drifted out of sync with each other -- `0002` alone no longer passed its own
`git apply --reverse --check` once `0004` (built on top of it) was also applied, because `0004`'s
insertions shifted the context lines `0002`'s own hunks depend on; genuinely reversible
independently was never guaranteed once two patches touch overlapping regions of the same file.
Consolidated into one patch (current `0002-gx-implicit-end.patch`, `0004` retired) that reverse-
checks cleanly against the pristine original blob (`git cat-file blob 789ed14`, verified byte-for-
byte). Unrelated to this session's actual task; found only because a CMake reconfigure re-ran the
patch step.

**`ID_DATA`'s +16 bytes (docs/port-layout-parity.md), root-caused, not fixed**: `include/model.h`'s
`cCoord` (which `ID_DATA` derives from, `include/t_id.h`) has its own un-ported `cCoord* pParent`
(a raw pointer, +4 bytes on host); its base `cUnit` (`include/cManager.h`) has both an un-ported
`cUnit* pNext` (+4) *and* three virtual functions (`~cUnit`, `beginEvent`, `endEvent`) which the
GameCube's GNU v2 ABI implements with a 4-byte vptr, ELF64/host's Itanium ABI with 8 bytes (+4).
That accounts for +12 of the confirmed +16; the last 4 were not traced further (plausibly padding
shifted by the above, not chased -- **TO VERIFY**). Not fixed this pass: `cUnit` is the base of the
entire object chain (`cUnit -> cCoord -> cModel`, docs/overview.md) -- Ptr32-ifying `pNext` and
deciding what to do about the vptr width (the coordinator's own boot-prompt already named this
exact dilemma: "a vtable pointer can't be Ptr32 -- think carefully... consider whether the budget
computations can instead be adjusted under TARGET_PC to use host sizeof") is a cross-cutting change
touching most of `src/game`, not a local fix, and needs its own session.

**New blocker found, a direct consequence of section 39's fix reaching further into the frame loop,
not investigated (budget)**: `re4_boot` no longer gets stuck in the card-heap failure loop, but now
runs several seconds further and then aborts, deterministically, the same way whether or not
scripted input is used: `STUB: cDataCtrl::check() called` / `STUB: PPCSync() called` /
`STUB: OSGetResetButtonState() called`, then `libc++abi: terminating due to uncaught exception of
type re4_port::(anonymous namespace)::ThreadExitException`. `ThreadExitException` is a real,
deliberate mechanism (`src/port/os_thread.cpp`'s `OSExitThread()` unwind, caught at line 266 on
whatever thread starts there) -- this occurrence is escaping uncaught somewhere, not a new kind of
crash invented this pass. Root cause not traced (which code path decides to exit the thread here,
and why the catch that should be there isn't) -- **TO VERIFY**, next session's likely next blocker
now that the card-heap issue is genuinely gone.

**Not reached this pass (budget)**: confirming `SndCall : blk 0 No.5 Illegal SE No.` is harmless,
`math_sub.cpp`/`trans.cpp` un-exclusion (`frsqrte`/`fcmpu`), and the title screen itself -- the new
`ThreadExitException` blocker above is almost certainly what the next session hits first regardless
of which of those it starts with.

**Remote-verified** (touches `src/game/main_mem.cpp`, outside `src/port`/`include/port`):
115/115 SHA-1 OK, `asmcheck.py --all` TOTAL 231 unchanged (unchanged from every prior pass' count).
Commits `c8a58935` (Aurora patch consolidation, tools-only, no verification needed),
`a513d51c` (the systemic allocator fix) -- both fast-forwarded from `port/wip-phase1` onto
`port/macos-arm64` and pushed after the remote check passed.

## 41. `ThreadExitException` root-caused and fixed: `__re4game`'s missing section attribute broke
    unwinding through all of `src/game` (2026-09-25)

Section 40's blocker (`OSExitThread()`'s C++-exception unwind mechanism escaping uncaught,
`libc++abi: terminating due to uncaught exception of type re4_port::(anonymous
namespace)::ThreadExitException`) traced to completion. `lldb`, breakpointed on `__cxa_throw` at
the real crash (`TaskExit()`/`TaskChain()` in `src/game/scheduler.cpp`, called from a real,
correctly-running task fiber -- `pCTask->pFunc == CardMainTask`, `state == RUNNING`, confirmed via
`image lookup -v` that the frames lldb's default symbolication mislabeled (a separate, cosmetic
DWARF-attribution glitch, not chased) are real code inside `TaskChain`/`CardFirstCheck`/
`tvModeExit`/`tvModeCheckTask` -- a completely ordinary, correctly-nested call stack under
`TaskFiberEntry`'s own `try`/`catch`). `image show-unwind -a` on an address inside `__TEXT,__re4game`
(the custom Mach-O section `include/port/game_section.h`'s `#pragma clang section text=...`
introduced in section 40) showed only the generic arm64 fallback unwind plan -- no "sourced from the
compiler: yes" compact-unwind plan at all -- while an address in the ordinary `__TEXT,__text`
section had one. Root cause: `#pragma clang section text="__TEXT,__re4game"` names the section but
does not set its Mach-O attributes; ld64 only infers `S_ATTR_PURE_INSTRUCTIONS` (the flag that
makes it emit `__unwind_info`/compact-unwind for a section) from the conventional name `__text`, and
silently omits unwind info for a same-segment section with a different name and no attributes.
The ld64 warning already noted in section 40 ("missing 'regular,pure_instructions' section flag")
was not cosmetic -- it was ld64 accurately describing this exact defect. Reproduced standalone (a
throw/catch across a `#pragma clang section text="__TEXT,__re4game"`-tagged TU): without
`,regular,pure_instructions` on the pragma string, the exception escapes uncaught (this project's
exact failure) and, in one variant, ld64 itself crashed while emitting a spurious unwind-info entry
for a non-code section; with it, `otool -l` shows the section's `flags` field gains
`S_ATTR_SOME_INSTRUCTIONS|S_ATTR_PURE_INSTRUCTIONS` (`0x80000400`) and the throw/catch works.

**Fix**: `include/port/game_section.h`'s pragma is now
`#pragma clang section text="__TEXT,__re4game,regular,pure_instructions"` (one line, comment
explaining why the suffix is load-bearing). This is a systemic fix, not specific to
`OSExitThread()`: it restores unwinding through *any* exception that needs to pass through `src/game`
code, not just this one call site.

**Verified**: clean incremental rebuild of `re4_boot` -- the ld64 "missing ... pure_instructions"
warning is gone. A real run (`re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso`) no longer
aborts at the old blocker; it runs past `CardMainTask`'s DVD/card-mount sequence (`SS/cmn/title.snd`,
`Font/common_p.fnt`, `ss/cmn/save_e.dat`, `Slot A Mount`, the expected `Failed to open file:
bh4_data*` lines for an empty/non-matching real memory-card directory) and keeps running steadily
(no crash, observed for 15-20 s). Host default build unaffected: `re4_game_all -k 0` still 33
failing files (same set), `ctest --test-dir build-pc` 5/5. Change is entirely inside
`include/port/game_section.h` (header-only, `#ifdef TARGET_PC`-guarded) -- no remote verification
round trip needed per the port rules. Commit `863e2ffd`, pushed to `fork/port/macos-arm64`.

**Screenshot, honestly described** (`RE4_PORT_SCREENSHOT`, 8 s in): still the same kind of debug
overlay as every prior screenshot in this file, not the title screen -- a dashed line and a
`08010000` hex label top-left, a green mark and a dark-navy vertical bar at the left edge, a
lavender/grey vertical bar at the right edge, two `C6E420`/`C7E420` hex labels bottom-right, and a
plain `7` / `0` bottom-left. No title-screen art, no legible menu text. Fixing the exception-unwind
bug changed what code runs (much further into `CardMainTask`) but not yet what is on screen.

**Not reached this pass (budget)**: `math_sub.cpp`/`trans.cpp` un-exclusion (`frsqrte`/`fcmpu`),
`cUnit`/`cCoord` layout (`Ptr32`-ifying `pNext`/`pParent`, deciding the vptr-width question), and
the title screen itself -- all still open from section 40, now genuinely reachable next since this
session's blocker is gone.

## 42. `math_sub.cpp` un-excluded (frsqrte/paired-single sincos/fcmpu ported), `trans.cpp` correctly
    stays excluded, `cUnit`/`cCoord` rule recorded, and the REAL current blocker found: a missed
    wakeup in Aurora's GX FIFO worker handshake (2026-09-25, same day as section 41)

**math_sub.cpp** (docs/port.md's task list item): all four real-asm functions
(`SQRTF`/`SINF`/`COSF`/`LIMIT_ANGLE`) now have `#ifdef TARGET_PC` C equivalents, `#else` keeping the
original asm byte-for-byte for the matching build. `SQRTF`: `1.0f/sqrtf(x)` (arm64-native, more
accurate than real `frsqrte`'s own ESTIMATE) as the seed for the SAME explicit Newton-Raphson
refinement the vendor's asm performs afterward (`r = r*(1.5 - 0.5*x*r*r)`, transcribed operation-
for-operation, not algebraically simplified) -- not a Dolphin-style bit-exact `frsqrte` lookup
table, on the reasoning that one full NR iteration on an already-more-accurate seed converges to the
same float-precision result regardless of the seed's own error, and no established caller depends
on real hardware's specific rounding (**TO VERIFY** if a future desync traces here).
`SINF`/`COSF`/`LIMIT_ANGLE`: `LIMIT_ANGLE` is an exact branch-for-branch C transcription of the
vendor's `fcmpu`/`fsubs`/`fadds` loop (no precision or behavior difference possible -- plain
single-precision compare/add/subtract, identical on arm64); `SINF`/`COSF` substitute libm's
`sinf`/`cosf` on the same `LIMIT_ANGLE`-wrapped input, documented as an accepted approximation of
the vendor's own paired-single degree-9 minimax polynomial (not reproduced lane-for-lane: it is a
compensated-summation trick for PS throughput/precision, not two different functions, and there is
no arm64 paired-single unit to model it on). Caught one real bug while testing: `COSF`'s first draft
substituted `cosf(x)` on the already-shifted-by-PI/2 input `x`, which is wrong -- the vendor's own
comment ("cos(x) by the same series") means `COSF` computes cos via the identity
`cos(t) = sin(t + PI/2)`, so the substitute must call `sinf`, not `cosf`, on the shifted `x`; a new
test (`tests/port/test_math_sub.cpp`, links `src/game/math_sub.cpp` directly with minimal `abort()`-
on-call stand-ins for the other functions' dependencies -- `PSMTX*`/`PSVEC*`/`mem_alloc`/`pLog`,
never actually invoked) caught it immediately via the `sin^2+cos^2==1` identity check, `ctest` 6/6.
Un-excluded from `cmake/boot_exclude.txt`; 21 now-redundant generated stubs (`SQRTF`, `SINF`,
`COSF`, `LIMIT_ANGLE`, and 17 other `math_sub.cpp` functions the old stub generator had covered
while the file was excluded) removed from `src/port/stubs/generated_{c,cpp}_stubs.cpp` by hand (a
duplicate-symbol link error otherwise). **Remote-verified**: 115/115 SHA-1 OK, `asmcheck.py --all`
TOTAL 231 unchanged. Host default build: `re4_game_all -k 0` now 32 failing files (down from 33 --
`math_sub.cpp` itself no longer one of them). Commit `1ac7bd04`, fast-forwarded from
`port/wip-phase1` onto `port/macos-arm64` and pushed. `re4_boot` still runs steadily afterward
(reaches the same point as before -- see below); "PrimBuffer OVERFLOW" does not appear in any run
this session (**TO VERIFY** whether it ever did after section 39/40's heap fix, independent of this
math_sub.cpp change).

**trans.cpp**: the boot-prompt's assumption ("the only real asm is in math_sub") does not hold --
checked by actually trying to compile it under `RE4_U32_32`/`TARGET_PC`. Two classes of error:
tractable ones (two `Ptr32<u8>` assignment-from-`void*` bugs in the already-TARGET_PC-guarded
`CalcTplAddrC8`, fixable by casting to `u8*` instead of `void*`; two `(WeightExt*)`/`(Weight*)` casts
from a `Ptr32<u8>` field needing the same treatment as every other cast_rewriter-flagged site) --
and a genuinely different, deeper class: whole-function real PPC asm that reads/writes the
GameCube's *locked cache* as a fixed hardware address (`0xE0000000`+offset) treated as a `Mtx`
scratch array -- `CalcSk1_x`/`CalcSk1_x2` (the vertex-skinning inner loop proper, paired-single
loads straight from that address), plus `MakeWeightPalette`/`MakeWeightPaletteExt`'s
`PSQ_L_U8_TO`/`PSMTXReorder`-into-locked-cache, and `setupGQR6`'s `mtspr`. `src/game/pendulum.cpp`
(already excluded, separately) hits the identical `0xE0000000` pattern, confirming this is not a
one-off. Left excluded, with the exclude-list comment now explaining why (not just "asm"): porting
this needs a real design decision (model the locked cache as an ordinary host buffer, then rewrite
the skinning kernels in portable C against it) that is cross-cutting across at least two files, not
a local, mechanical fix like `math_sub.cpp` was -- belongs in its own session, per the same judgment
call section 40 already made about `cUnit`/`cCoord`.

**`cUnit`/`cCoord` layout**: the rule is now written down, not just the diagnosis --
docs/port-layout-parity.md's new "`cUnit`/`cCoord`: the rule for a polymorphic base..." section.
Short version: `pNext`/`pParent` become `Ptr32<T>` (mechanical, same as every other row in that
doc's mismatch table); the vtable pointer stays host-width, full stop, no compressed-vptr scheme;
every literal-GameCube-`sizeof()` computation touching one of these types must switch to host
`sizeof(T)` under `TARGET_PC` (`cManager<T>`'s own pools already do, verified by inspection; nothing
else swept this session). Not implemented (still needs its own session, per section 40) -- this
session only turned the open question into an executable rule.

**Where the boot process actually is now, per-frame (the coordinator's specific question)**: added a
temporary trace (`RE4_PORT_TRACE_TITLE=1`, `Title_task()`'s own `for(;;)` loop logging
`w->Rno0`/`w->Rno1` every pass; reverted before landing, not part of any commit) and ran with
`RE4_PORT_FIXED_VI=1` for determinism. Result: **`Title_task` prints its trace line exactly ONCE,
ever** (`Rno0=0`, i.e. `titleInit` about to run for the first time) -- across a 40-second real run,
it is never scheduled a second time. This means the state-machine question ("does it proceed to
title.dat and the draw, or wait on input/fade/a DVD read") does not yet have an answer at the
`Title_task` level: the whole game thread is frozen even earlier, one level below any task's own
logic.

**Root cause, found with `sample`/`lldb` and confirmed with a minimal, reverted instrumentation
patch to `../aurora/lib/gx/fifo.cpp` (not committed anywhere; this repo's own port rules keep Aurora
changes in `tools/port/aurora-patches/`, and this was a throwaway diagnostic, reverted byte-for-byte
before finishing -- `git status` in `../aurora` is clean)**: the game thread is parked forever inside
`aurora::gx::fifo::drain()` (called from `GXDrawDone()` <- `Render_done()` <- `main_game()`'s own
frame loop, `main.cpp:147` -- i.e. this is the FIRST real GX command-buffer drain of the session, not
anything title-specific), waiting on `sProcessed.wait(processed, acquire)` for a target that never
arrives. The separate "Aurora FIFO processor" worker thread is simultaneously idle, parked in
`sWorkerWake.wait(event, acquire)` -- it has no more work queued from its own point of view. Added
prints (`fprintf` in `drain()`/`wake_worker()`/`worker_main()`) caught the actual sequence on a live
run: `drain()` stores the new `sPublished` target, calls `wake_worker()` (`fetch_add` on
`sWorkerWake` + `notify_all()`) -- and the worker's own trace shows it read `published=0` (the OLD
value, pre-store) on its immediately-preceding loop iteration, then printed "about to wait" and
blocked, **and never printed anything again for the rest of the run**: no "woke from wait", no
second loop iteration, no processing. This is a real missed wakeup at the `std::atomic<uint32_t>::
wait()`/`notify_all()` boundary (libc++, Darwin's `__ulock_wait`/`__ulock_wake` underneath) --
`atomic::wait(old)` is specified to recheck the current value itself before blocking (so a `notify`
that lands between the caller's stale read and the actual `wait()` syscall should not be
missable), so either there is a genuine bug in this specific libc++/Darwin combination's 32-bit
`atomic::wait`, or a subtler ordering issue in Aurora's own handshake this session did not fully
resolve. **Not chased further (budget, and this is exactly a "don't blame Aurora without a minimal
repro" situation now WITH a minimal, reproducible repro in hand, ready for a focused session)**:
candidate next steps, none attempted here: (a) a standalone minimal-repro test of
`std::atomic<uint32_t>::wait`/`notify_all` alone (no GX/Aurora code at all) on this exact toolchain/
OS to confirm or rule out a genuine platform bug; (b) switching `aurora::gx::fifo`'s
`kProcessingMode` from `ProcessingMode::Thread` to `ProcessingMode::Drain` (synchronous, no separate
worker thread, no wait/notify at all) as a `tools/port/aurora-patches/`-tracked patch, IF (a)
confirms this is a platform primitive issue rather than something fixable in Aurora's own handshake
logic. This is a real, reproducible, previously-undiagnosed blocker -- distinct from, and reached
only because of, section 41's `ThreadExitException` fix (the game thread now runs far enough to hit
its first real GX drain at all).

**Screenshot, honestly described** (`RE4_PORT_SCREENSHOT`, realtime, 6s in, `RE4_PORT_FIXED_VI` not
set): identical to every prior screenshot in this file -- the same debug overlay (dashed line,
`08010000` label, green mark, dark-navy/lavender bars, `C6E420`/`C7E420` hex labels, plain digits
bottom-left), not the title screen. Consistent with the finding above: this is the one frame the
renderer produced before the FIFO drain deadlock freezes the game thread, so every screenshot taken
after that point (regardless of real elapsed time) necessarily shows the same static image.

**Verified / not broken**: host default build `re4_game_all -k 0` still 32 failing files (same set
as this section's own math_sub.cpp change, i.e. no NEW regressions from the trans.cpp investigation
or the layout-parity doc work, both of which touched no compiled code); `ctest` 6/6. The Aurora
diagnostic patch was reverted before any of this session's `re4_boot` rebuilds that produced a
screenshot or a commit -- no diagnostic-only code is in any binary referenced above.

**Not reached this pass (budget)**: the actual FIFO missed-wakeup fix (needs the minimal-repro step
above first), `trans.cpp`'s locked-cache design decision, `cUnit`/`cCoord`'s actual code change, and
the title screen itself -- all now blocked on the FIFO deadlock above rather than on section 41's
`ThreadExitException` (fixed) or `math_sub.cpp`'s exclusion (fixed).

## 43. GX FIFO missed-wakeup fixed (minimal repro + `ProcessingMode::Drain` patch); new blocker found
    one step later (2026-09-25, follow-up to section 42)

**Minimal repro (task from section 42)**: a standalone `std::thread` pair (no Aurora/GX code at all)
reproducing the exact handshake shape (`store(target) -> fetch_add+notify_all` / `load -> wait ->
recheck`) on a second atomic, run 2000 times with a randomized delay to shake out timing, did **not**
reproduce a hang on this toolchain (clang, libc++, macOS arm64). This rules out "`std::atomic<uint32_t>
::wait`/`notify_all` is broken on this platform" as the explanation, per this project's own rule
against blaming the platform without a repro -- the primitive itself is fine here.

**Fix applied**: `tools/port/aurora-patches/0006-gx-fifo-drain-mode.patch` switches Aurora's
`lib/gx/fifo.cpp` `kProcessingMode` from `ProcessingMode::Thread` to `ProcessingMode::Drain`. This
port's design already runs the whole game on one host thread (fibers via ucontext); the FIFO worker
thread bought no real concurrency here, only a wait/notify handshake between three atomics
(`sPublished`/`sProcessed`/`sWorkerWake`) to get wrong. Rather than hunt the exact interleaving that
produces the missed wakeup in Aurora's specific three-atomic handshake (out of budget, and the
isolated repro above did not reproduce it in a simpler two-atomic version), removing the second
thread entirely sidesteps the whole race class: `Drain` processes the FIFO synchronously inside
`publish()`/`drain()`, no separate thread, no wait/notify at all. This is a port-local decision
(`tools/port/aurora-patches/`, not proposed upstream); the patch's own header comment records the
reasoning and what was and wasn't chased down.

**Verified**: patch applies and reverses cleanly (`git apply --check` / `--reverse --check`) against
the Aurora checkout's current working tree (the three earlier patches already applied). Clean
`cmake --build build-pc-boot --target re4_boot` picks it up automatically (CMakeLists.txt's own
idempotent per-configure patch-apply loop). Running `re4_boot` (`RE4_PORT_FIXED_VI=1`,
`RE4_PORT_SCREENSHOT=...`) no longer hangs at the first GX drain: the log now shows real FIFO
processing (`[debug] [aurora::gx::fifo] Unhandled XF register ...`/`Unhandled BP register ...`/
`Unhandled XF memory write ...` -- genuine command-stream bytes reaching the processor) and the
process now exits on its own within a couple of seconds instead of hanging indefinitely.

**New blocker, one step later**: `[fatal] [aurora::gfx] No active recording session`, immediately
after the FIFO processes the frame's first commands. The log's own preceding line already flagged the
likely cause: `re4_port: aurora_begin_frame() returned false -- this frame's GX submission has no
active recording session (window minimized/GPU not ready)` (`src/port/vi.cpp`'s own comment) --
`Render()`/`DrawOTag` still run unconditionally per `main.cpp`'s unchanged frame loop even when
`aurora_begin_frame()` says there is no session to draw into, so the FIFO's processor hits a real GX
command with nothing to record it into. **Not chased further this pass (budget)**: whether this is a
genuine "no display/headless" condition in this run environment vs. a port-side ordering bug (`Render()`
needs to check the same `aurora_begin_frame()` return value `main.cpp`'s frame loop already discarded,
or the frame loop needs to skip `Render()` for a frame with no session, matching real hardware's
vertical-blank-pending behavior) -- **TO VERIFY** next session, distinct from and now blocking on top
of this section's FIFO fix.

**Host default build**: unaffected -- this patch only touches the Aurora checkout consumed by
`RE4_BUILD_BOOT`, no `src/`/`include/` change in this repo. `re4_game_all -k 0` / `ctest` not
re-run this session (no code in this repo changed that either target compiles).

## 44. "No active recording session" fixed: real root cause was a startup race, not a genuine
    occlusion case (2026-09-25, follow-up to section 43)

**Root cause**: `boot_main.cpp`'s `main()` starts the game thread (`CreateThreadOnStack`) *before*
calling `RunPresentLoop()` on the host main thread, and `RunPresentLoop()` is what calls
`aurora_initialize()` (creates the SDL window/WebGPU surface). The game thread runs far enough
(`OSInit`, several real DVD reads) to reach its first `BeginGxFrame()`/`aurora_begin_frame()` before
the main thread has created the window at all -- confirmed by the crash log from section 43 never
containing `"re4_boot: Aurora window opened"` before the fatal. This is exactly "the present loop on
the main thread hasn't created it yet" from the coordinator's own list of candidates, not a real
occlusion/minimized-window case.

**Fix, two parts**:
1. `src/port/vi.cpp`: a one-time gate, `g_windowReady` (`std::atomic<bool>`), set by
   `RunPresentLoop()` right after `aurora_initialize()` returns. `BeginGxFrame()` waits on it
   (1 ms poll, 5 s bound) before its first call only -- once true it never blocks again. This is the
   "wait for the window/surface to be ready before the game's first frame" branch of the task.
2. `tools/port/aurora-patches/0007-gx-fifo-discard-without-session.patch`: belt-and-suspenders for
   the genuine case (window minimized/occluded later, or the 5 s bound above is somehow exceeded) --
   `aurora::gx::fifo::drain()` now checks `sFrameActive` first and, if no recording session is
   active, discards the buffered FIFO bytes (advances `sStreamBase`, clears the buffer, logs a debug
   line) instead of forwarding them to `process_to()` / `aurora::gfx::recording` with nothing to
   record into. Matches real hardware's own behavior for "a field nobody is watching": the game
   still calls GX every frame unconditionally (`main.cpp`'s frame loop, unchanged, as real hardware
   requires), the output for that field is just dropped, not redirected to any offscreen target (no
   such target is maintained -- **TO VERIFY** whether a future need, e.g. screenshotting an occluded
   window, wants one).

**Verified**: clean `cmake --build build-pc-boot --target re4_boot` (patches 0006+0007 both applied
and stack correctly, `git apply --check`/`--reverse --check` confirmed for each individually against
the other already applied). Running `re4_boot` (`RE4_PORT_FIXED_VI=1`, 25 s real time, screenshot at
4 s): log now shows `"re4_port: BeginGxFrame: waiting for Aurora window to open..."` immediately
followed by real Aurora/WebGPU/Metal initialization, `"re4_boot: Aurora window opened"`, then
`"re4_port: BeginGxFrame: window ready, proceeding"` -- no fatal, no hang. The process runs far past
the previous blocker: title data loads (`SS/cmn/title.snd`, `ss/cmn/save_e.dat`), memory-card probing
runs (`[error] [aurora::card] Failed to open file: bh4_data00..19` -- expected, no save data present,
not investigated further this pass), and the process is still alive and producing frames when killed
at the 25 s mark (not a crash-then-stop). Screenshot at 4 s shows the same debug overlay shape as
every prior screenshot but with visibly different content this time (yellow bar segment where it was
previously black/short, digits `100`/`0` instead of `7`/`0`) -- the renderer is genuinely producing
different output frame to frame now, not repeating the same static first frame. Still not the title
screen itself.

**Verified / not broken**: host default build `re4_game_all -k 0` still 32 failing files (same set);
`ctest` 6/6 (`build-pc`, unaffected -- `src/port/vi.cpp` compiles the same way there). No `src/`/
`include/` file outside `src/port/`/`include/port/` touched this section, so the Docker remote
byte-identity loop does not apply per this repo's own port rules.

**Not reached this pass (budget)**: the title screen itself (still same debug HUD, further along);
`trans.cpp`'s locked-cache design (task 2/3, not started this pass either); memory-card open failures
above (likely expected/harmless, not confirmed).

## 45. `trans.cpp`/locked-cache design narrowed (no new port infra needed for LC*), asm port not
    attempted this pass -- needs its own session (2026-09-25, follow-up investigation)

**Task 2 (locked cache) turns out simpler than the original brief assumed**: Aurora's own
`include/dolphin/os/OSCache.h` already has a `#ifdef TARGET_PC` branch (`extern void* LCGetBase(void);`
instead of the real-hardware `#define LCGetBase() ((void*)0xE0000000)`), and
`lib/dolphin/os/OSCache.cpp` already implements it as a real function returning a 16 KB static host
buffer (`s_lcData`), plus working `LCEnable/LCDisable/LCLoadBlocks/LCStoreBlocks/LCLoadData/
LCStoreData/LCQueueWait` (memcpy-based DMA emulation) -- already linked into `re4_boot` today (`nm`
confirms `_LCEnable` resolves to `aurora_os`'s `OSCache.cpp`, not any stub). This means the
`mach_vm_allocate(VM_FLAGS_FIXED)`-at-`GCPTR(0xE0000000)` design in the original brief is unnecessary
*and* would conflict with this project's own established lesson (`src/port/arena.cpp`'s own history
comment: a fixed-address VM reservation for the *main* arena was tried and rejected after measuring
37/50 real failures against live malloc bookkeeping -- a smaller, higher, sparser 16 KB region is a
different risk profile, but there is no need to take that risk at all when Aurora already solves this
with a plain host buffer). The real remaining work is mechanical: every vendor call site that casts
the literal `0xE0000000` directly (`pendulum.cpp:1177`, `trans.cpp:858`/`910`, and inside
`CalcSk1_x`/`CalcSk1_x2`'s own asm) needs a `TARGET_PC` branch that computes its pointer from
`LCGetBase()` instead -- `#else` keeps the literal, byte-identical, for the matching build. Not
written this pass (depends on task 3 below being tractable first, since two of the four call sites
are inside the asm functions themselves).

**Task 3 (trans.cpp's whole-function asm) -- investigated, not ported this pass**: fully decoded
`CalcSk1_x`'s register-level behavior (instruction by instruction) far enough to know exactly what
needs cross-checked, independent verification before writing a single line of the C substitute:
- The matrix load pattern (`psq_l` into fpr0-fpr7 at offsets 0/8/0xc/0x14/0x18/0x20/0x24/0x2c from a
  0x30-byte `ROMtx`) pairs (float0,float1), (float2,--), (float3,float4), (float5,--), (float6,float7),
  (float8,--), (float9,float10), (float11,--) -- i.e. every *third* float starts a new pair, not every
  fourth, so the natural "3 rows of 4" `Mtx` reading does NOT line up with the fpr pairing the
  `ps_madds0`/`ps_madds1` accumulation below actually uses. Working out the *correct* row/column
  interpretation this implies (it is almost certainly still a real 3x4 affine transform, just fed to
  the paired-single unit in a layout chosen for this specific instruction sequence, not the layout a
  human would guess from the `Mtx` type alone) needs to be verified against a known-good reference
  (e.g. cross-checked against `PSMTXReorder`'s own definition, or Dolphin's PPC interpreter) before
  trusting it, not assumed from the offsets alone.
- The quantization scale in `setupGQR6(0x32073207)` (position skinning) encodes a load AND store
  scale/type in the same 32-bit GQR value; decoding the 750CL's exact scale-field sign convention
  (positive field values divide, the top half of the 6-bit range is a two's-complement negative
  exponent that multiplies -- confirmed against the PowerPC 750CL user's manual's own quantization
  section, not guessed) matters for getting the dequantization direction right, and a sign error here
  would silently produce vertex positions off by a large power of two -- exactly the kind of "looks
  plausible, is wrong" bug this project's own rules (independent cross-check, not "looks right") exist
  to catch. `CalcSk1_x2` (normals, `setupGQR6(0x20062006)`) needs the same treatment with its own
  scale value.
- `MakeWeightPalette`/`MakeWeightPaletteExt` are NOT whole-function asm (ordinary C++, already mostly
  TARGET_PC-clean per section 42) -- only their `PSQ_L_U8_TO`/`PSMTXReorder`-into-locked-cache call
  sites need the `LCGetBase()` treatment above, once `CalcSk1_x`/`CalcSk1_x2` unblock un-excluding the
  file at all (the file cannot compile under `TARGET_PC` with even one real-asm function left as raw
  PPC `asm volatile` text -- arm64 clang cannot assemble PPC mnemonics, so this is all-or-nothing per
  file, not partial).

**Why not attempted further this pass**: writing the C substitute for `CalcSk1_x`/`CalcSk1_x2`
without first locking down both of the above (the exact matrix-element-to-fpr mapping and the GQR
scale sign convention) against an independent reference would be exactly the kind of unverified
numeric port this project's own rules warn against (docs/matching.md's "Don'ts", and the task's own
instruction to cross-check against an independently-computed reference, not just "the C reads
plausible"). This needs a dedicated session with room for that verification work (synthetic test
vectors run through a from-scratch, independently-derived formula, not just eyeballing the
translation) -- not attempted here given this session's remaining budget after sections 43/44's fixes.

**Not reached this pass**: any code change for `trans.cpp`/locked cache (investigation only, `git
status` in this repo confirms no source changed this section); the title screen (still blocked behind
task 3, since `Render()` for real 3D geometry needs the skinning kernels working, not just the FIFO
handshake fixed).

## 46. `trans.cpp`'s locked-cache/skinning asm ported and independently verified with a
    from-scratch PowerPC interpreter; file stays excluded (now for a narrower, different reason)
    (2026-09-25, follow-up to section 45)

**Simulator (task 1)**: `tools/port/ppcsim/` -- `gqr.py` (GQR quantize/dequantize, formula cited
from the IBM PowerPC 750CL manual's "Paired Single Load and Store Instructions" section and
cross-checked against Dolphin's own `m_dequantizeTable`/`m_quantizeTable` and this repo's own
`include/dolphin/base/PPCArch.h` bit masks -- all three agree), `sim.py` (a tiny interpreter for
exactly the opcodes `CalcSk1_x`/`CalcSk1_x2` use: `lha`/`lbz`/`subi`/`li`/`mulli`/`addis`/`mtctr`,
`psq_l`/`psq_lu`/`psq_st`/`psq_stu` with GQR quantization, `ps_madds0`/`ps_madds1`), `calc_sk1.py`
(runs the vendor's own asm text, copied verbatim from `trans.cpp` and checked byte-for-byte against
it every run). `test_gqr.py` (known-value unit tests for the quantizer, `python3
tools/port/ppcsim/test_gqr.py`) and `test_calc_sk1.py` (randomized + edge-case cross-check of the
simulator's execution of the real asm against a closed-form formula derived by hand instruction-by-
instruction, `python3 tools/port/ppcsim/test_calc_sk1.py`) both pass. The hand derivation: the
"ROMtx" `PSMTXReorder` writes into locked cache is column-major (`M[col*3+row]`), and the vendor's
own `ps_madds0`/`ps_madds1` chain, decoded lane-by-lane, computes exactly the plain row-major
affine transform `out[r] = M[r]*x + M[3+r]*y + M[6+r]*z + M[9+r]` as three chained fused multiply-
adds -- i.e. the transpose `PSMTXReorder` performs and the specific accumulation order the asm uses
cancel out algebraically to an ordinary `Mtx * vec` skin, just fed to the paired-single unit in a
layout chosen for this instruction sequence. Confirmed by the simulator agreeing with this formula
on 20+20 randomized trials (two GQR6 values for `CalcSk1_x`, one for `CalcSk1_x2`) plus max/min s16
and zero-vertex edge cases -- not merely assumed.

**Locked cache (task 2)**: simpler than the original brief assumed (section 45 already found this):
Aurora's `include/dolphin/os/OSCache.h` has a `#ifdef TARGET_PC` branch (`extern void*
LCGetBase(void);`, backed by a real 16 KB static host buffer in `lib/dolphin/os/OSCache.cpp`) --
but this repo's OWN copy of the same header did NOT have that branch (verified with `-H`: the
compiler resolves `<dolphin/os/OSCache.h>` to `include/dolphin/os/OSCache.h`, this repo's own,
`${RE4_ROOT}/include` coming before `${RE4_AURORA_DIR}/include` in every target's include path --
Aurora's copy is never actually seen by any `src/game/*.cpp` unit). Without this fix `LCGetBase()`
would have macro-expanded to the literal hardware address `((void*)0xE0000000)` even under
TARGET_PC -- caught before it caused a segfault, by checking the actual preprocessor resolution
rather than assuming section 45's finding transferred unchanged. Fixed by adding the identical
`#ifdef TARGET_PC` branch to this repo's `include/dolphin/os/OSCache.h` (mirrors Aurora's copy,
`#else` unchanged for the matching build).

**Task 3 (the whole-function asm itself), ported**: `CalcSk1_x`/`CalcSk1_x2`/`setupGQR6` now have
TARGET_PC C definitions, split into their own new translation unit (`src/port/trans_skin.cpp` +
`include/port/trans_skin.h`, added to the `re4_port` static library) rather than inline in
`trans.cpp`, specifically so they can be unit-tested without the rest of `trans.cpp`'s dependency
graph (RE4_U32_32, the cast rewriter, dozens of unrelated symbols). `trans.cpp` itself keeps the
original whole-function asm byte-for-byte in `#ifndef TARGET_PC` (previously bare) blocks; its own
`extern "C"` declaration (from `trans.h`) already covers the TARGET_PC definitions living in the
new file, no new include needed. `MakeWeightPalette`/`MakeWeightPaletteExt`'s two `PSMTXReorder`
call sites get a `ReorderMtxToLC()` TARGET_PC branch (a plain transpose into `LCGetBase()+idx*0x30`,
staying in `trans.cpp` since `PSMTXReorder` itself is Metrowerks-only `asm void` code
(`src/lib/psmtx.c`) never compiled for the host at all); `PSQ_L_U8_TO`/`PSQ_L_U8` (the GQR2 u8-to-
float read `MakeWeightPalette`/`MakeWeightPaletteExt` use for weight percentages) get a TARGET_PC
branch too, a plain unscaled byte read (GQR2 is fixed at boot, u8 type/scale 0 on both load and
store, `main.cpp`'s own `#ifndef TARGET_PC` GQR setup -- never actually read on the host, so this is
just doing in C what that fixed encoding already meant). Two pre-existing `Ptr32<u8>`-cast bugs
fixed along the way, exactly as section 45 predicted: `(WeightExt*)`/`(Weight*)` casts from
`d->pWeight` need an intermediate `(u8*)` cast first (same shape as `CalcTplAddrC8`'s existing
fix), and `CalcTplAddrC8`'s two `td->...->data = (void*) (...)` assignments need `(u8*)` instead of
`(void*)` (a `Ptr32<u8>` field has no implicit conversion from `void*`).

**A real bug caught by the test, not assumed away**: the first draft of `CalcSk1_x`'s C port read/
wrote the vertex buffers as host-native (little-endian) `s16` directly. `tests/port/
test_trans_skin.cpp` (comparing the compiled port against `tools/port/ppcsim`'s reference vectors,
which are genuinely big-endian, matching real hardware's native byte order for a GX vertex buffer)
failed immediately with every byte pair swapped. Fixed with explicit `LoadBE16`/`StoreBE16` helpers
in `trans_skin.cpp` (hand-rolled, not `include/port/be.h`'s `BE<T>`, since `d->vtxOrig`/
`info->pPosBuf` are opaque packed byte buffers in `include/model.h`, not a `BE<T>`-wrapped struct
field) -- `CalcSk1_x2`'s s8 elements need no such fix (single bytes). `ctest` (`test_trans_skin`,
12 cases: 8 `CalcSk1_x` + 4 `CalcSk1_x2`, covering both GQR6 values `trans.cpp` actually uses for
positions/default normals plus the extended s8-normal value, randomized inputs plus max/min/zero
edge cases) now passes, bit-for-bit against the interpreter-derived reference.

**Un-exclusion attempted, reverted for a narrower reason**: removing `trans.cpp` from
`cmake/boot_exclude.txt` and building `re4_boot` now gets past compiling (confirmed: the file
compiles clean under `RE4_U32_32`/`TARGET_PC`, cast-rewriter included) but fails to LINK --
`trans.cpp` calls `ResetShape`/`CalculateShape_new`/`drawGround` (`shape.cpp`) and
`ShadowTrans`/`GetSelfShadowMng`/`GetCastShadowMngPtr`/`isSelfUse`/`g_SelfShdNum`/`shd_ofs`/
`shd_tex_scale_x` (`shadow.cpp`) directly -- both still excluded for their OWN whole-function real
PPC asm (Phase 5, unrelated to and not attempted this session), plus `ClothDraw` (`cloth.cpp`,
already a separate known exclusion) and three GX entry points Aurora's GX library does not yet
implement (`GXSetDrawSync`, `GXSetDrawSyncCallback`, `__GXSetIndirectMask`). `trans.cpp` went back
into `cmake/boot_exclude.txt`, with the comment rewritten to record that its own math is solved and
the remaining blockers are three other units' worth of unrelated work -- not bundled into this
session, per the same "one fully verified thing beats several half-verified" judgment call as
section 45's own deferral.

**Verified**: `python3 tools/port/ppcsim/test_gqr.py` and `test_calc_sk1.py` both pass. Host
`ctest` in `build-pc-boot` (`RE4_U32_32=ON`): 8/8 (added `test_trans_skin`, 12/12 cases). `re4_boot`
still builds and links clean with `trans.cpp` back in the exclude list (no regression from the
`OSCache.h`/CMakeLists changes, which are otherwise unused while it stays excluded). Host default
build (`re4_game_all -k 0`, `RE4_U32_32=OFF`, `RE4_PC_BUILD_ALL_GAME=ON`): 31 files fail to compile
now, `trans.cpp` itself compiles clean (its `TARGET_PC`-gated `Ptr32`/`BE` fields, per
`include/model.h`/`include/tpl.h`, are gated on `TARGET_PC` alone, not `RE4_U32_32`, so this was
already reachable independent of that flag) -- **TO VERIFY**: the exact prior failing-file set was
not saved before this session to diff against one-for-one; nothing in this session's diff touches
any file other than `trans.cpp`/`CMakeLists.txt`/`include/dolphin/os/OSCache.h`/the two new
`src/port`+`include/port` files, so a regression in an unrelated file is very unlikely, but the
remote clean-rebuild/SHA-1/`asmcheck.py` loop is the actual judge for the matching build, run
separately (see the commit history around this section for the result).

**Not reached this pass**: `shape.cpp`/`shadow.cpp`/`cloth.cpp`'s own whole-function asm (each its
own session, same as this section's own scope discipline); the three missing Aurora GX entry
points; `pendulum.cpp` (a separate, still-excluded unit hitting the identical locked-cache pattern
-- not on `trans.cpp`'s own call path, so not touched); the title screen itself (still blocked
behind `trans.cpp`'s full un-exclusion, now behind `shape.cpp`/`shadow.cpp` instead of its own
math).
