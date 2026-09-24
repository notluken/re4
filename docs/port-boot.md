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
