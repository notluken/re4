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
