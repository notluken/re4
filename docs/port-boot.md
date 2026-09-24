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
