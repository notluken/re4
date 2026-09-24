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
