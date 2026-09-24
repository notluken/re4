# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A **complete, byte-identical decompilation** of *Resident Evil 4* (GameCube, `G4BE08` debug build,
"Nov 25 2004"). Every one of the 1083 objects compiles to the original bytes; `main.dol` and all 114
REL overlays are reproduced exactly. There is no work left to "make it build" — the work is
readability (names, comments, notes) and tooling.

The consequence that governs everything: **the output bytes never change.** Not for a rename, not for
a comment, not for a refactor. A change that alters a single byte of any binary is wrong, however
clean it looks. This inverts the usual instincts — do not "improve" code, do not reformat, do not
simplify a construct that looks redundant, do not delete an apparently dead statement. Most odd
shapes in this tree are load-bearing for the compiler.

## Build

Requires Python 3, ninja, the native SN GCC (built once from SN's GPL drop, `tools/sn-gcc/build.sh`),
and your own disc images in `orig/G4BE08/`. Everything else downloads on first configure.

```sh
python3 configure.py && ninja     # first time
ninja                             # afterwards — it reruns configure.py itself when a config file changes
```

**Never run `python3 configure.py` by hand before ninja**, and never run two ninja processes at once
(they clobber `.ninja_deps`/`.ninja_log` and send the manifest into a rebuild loop).

Useful `configure.py` flags: `--map` (linker maps), `--non-matching`, `--debug`, `--warn {all,off,error}`,
`--prodg-driver {native,ngccc}` (default `native`: SN cpp + natively-built cc1plus; `ngccc` uses
ngccc.exe v1.76 under wibo), `--build-dir`, `--verbose`.

This checkout currently has only **disc 1** in `orig/G4BE08/`. The four island-stage RELs
(`st3_0..st3_3`) come from disc 2, so the SHA-1 check will report 111 of the 115 lines until disc 2 is
added; that is expected, not a regression.

## Verify — three checks, all must pass

```sh
# 1. byte identity of every binary, on a CLEAN rebuild (the ProDG compile rule has no depfile,
#    so an incremental ninja after a header edit can leave stale objects behind)
find build/G4BE08 -path '*/obj/*' -prune -o -name '*.o' -print | xargs rm -f
ninja
build/tools/dtk shasum -c config/G4BE08/build.sha1     # every line must be OK

# 2. the unit you are working on
python3 tools/bytecmp.py <mod>/<unit>                  # must say IDENTICAL — this is THE judge
python3 tools/fdiff.py <mod>/<unit> <mangled_symbol>   # side-by-side when it does not

# 3. no instruction came from an asm template
python3 tools/asmcheck.py --all      # total and per-unit hits must be unchanged from before your edit
```

`bytecmp.py` IDENTICAL is authoritative; `unit_info.py`'s 100% is neither necessary nor sufficient.

## Working on one unit

```sh
python3 tools/unit_info.py game/foo                             # functions, sizes, names, match %
sed -n '/^\.fn NAME/,/^\.endfn/p' build/G4BE08/asm/game/foo.s   # target asm for one function
ninja build/G4BE08/src/game/foo.o                               # compile
python3 tools/sync_symbols.py build/G4BE08/src/game/foo.o       # placeholder -> mangled names (DOL)
ninja                                                            # rebuild + refresh the report
python3 tools/fdiff.py game/foo <mangled_symbol>
python3 tools/bytecmp.py game/foo
python3 tools/sync_data_symbols.py                              # once IDENTICAL and marked complete
```

For a REL unit, substitute `<mod>/<file>` (target asm at `build/G4BE08/<mod>/asm/<mod>/<file>.s`), use
`tools/sync_rel_symbols.py` instead of `sync_symbols.py`, and `python3 tools/make_rel.py --verify`.
`config/G4BE08/modules.py` maps a module unit to its source (stage rooms share `src/st<n>/rNNN.cpp`).

While a unit is in flight, list it in `NON_MATCHING` (`config/G4BE08/objects.py` for the DOL,
`modules.py` for a REL) so the build links the original object and the SHA-1 check keeps passing.

To try a source variant without touching `build/` (~1 s per variant):

```sh
tools/research/kit/variant.sh <unit> <variant-src> [FUNC]
```

## Two compilers, two rule sets

| Code | Compiler | Style |
|---|---|---|
| `src/game/`, `src/em*/`, `src/pl*/`, `src/wep*/`, `src/st*/`, `src/t_*/`, `src/tools/`, `src/tools_mod/`, `src/Sscrn/` | SN ProDG 3.9.3 = GCC 2.95.3 "SN BUILD v1.79", `-O2 -mfast-cast` | 4-space, `.clang-format` |
| `src/lib/adx_*`, `sfd_*`, `mpv_*`, `mps_*`, `dct_*`, `gcci`, `cri_cvfs` (CRI middleware) | Metrowerks CodeWarrior 2.4.7 | vendor's tabs, leave as-is |
| `src/lib/OS*`, `GX*`, … (Nintendo SDK) | CodeWarrior GC/1.2.5n, dolsdk2004 sources | vendor's tabs, leave as-is |

Not every game unit is `-O2`: the sound driver (`snd_iss*/seq*/str*/sub*/main/efx/ram`) is `-O0`
(`UNIT_CFLAG_OVERRIDES` in `objects.py`). If every function starts `stwu; mflr; stw r31; mr r31,r1`
and reloads parameters from the stack, suspect `-O0` before hunting for an `-O2` source shape.

Game code is C++ because the vendor's symbols are C++-mangled (GNU v2); the SDK, CRI and newlib units
are C because theirs are not.

## Hard rules

- **Bytes never change.** Clean rebuild + `dtk shasum` is the judge.
- **No asm that emits an instruction.** The only inline asm allowed is what the vendor also had to
  write in assembly (paired-single, GQR, cache, SPR, the exception handler) plus the codeless MWCC
  pins. Register and schedule differences are steered from C and tagged `// COMPILER-DIFF: <n>`
  with the mechanism named. `asmcheck.py` enforces this. Never a whole-function asm body for a function
  the vendor wrote in C; never a `.s` file.
- **`#line` directives** (738 of them) reproduce the vendor's line numbers inside assert/HALT strings,
  which are part of the original bytes. Do not move or remove them. Same for `// COMPILER-DIFF:` tags.
- **Plain member stores.** `pG->x = v`, `work->field = p`. Do not reintroduce reference-view setters
  (`U32Set`, `FSet`, `PSet`), struct views of a global pointer (`pGS`), or one-member `XxxWorkPtr`
  wrappers. The few `BitOn`/`BitOff16`/`U16Set` helpers left in `global.h` are register-choice levers,
  not aliasing workarounds, and say so.
- **Constants** are the PS2 build's enums, imported as declared; `pG` flag bits go through
  `XxxFlagChk/On/Off(pG, NAME)` from `include/global.h`. Do not add a `#define` or local enum for a
  value the PS2 dump already names, and do not rename an imported enumerator.
- **Names.** Vendor names keep the vendor's spelling — the tree mixes conventions on purpose. A
  placeholder (`xNN`, `pad_NN`, `lbl_*`, `fn_*`) is renamed only with evidence: `tools/ps2sym.py
  --struct <Name>` alignment, or use sites you quote by file and function. A name that also appears in
  a mangled symbol in `config/G4BE08/sym_map.tsv` cannot change — the link would fail.
- **Never edit by hand:** `build/`, `build.ninja`, `objdiff.json`, `config/G4BE08/splits.txt`, or
  `config/G4BE08/modules/<mod>/splits.txt` (change `modules.py`, then re-run `tools/gen_rel_config.py`).
- **Formatting.** New game code follows `.clang-format` (4-space, brace on its own line, `T* x`, 110
  cols). Do not run the formatter over existing files.
- **Never conclude "compiler-side difference."** Every such verdict in this project has been
  overturned. An RTL-level impossibility proof is right about the *source shape*, not the compiler:
  the original had a different structure (one shared pointer instead of two, a different inline
  boundary, an argument expression instead of a variable, a different declaration order).

### Two traps worth knowing

- A module source that declares a DOL C-linkage function **without `extern "C"`** makes
  `sync_rel_symbols.py` rename the DOL symbol in `config/G4BE08/symbols.txt` and break every other
  module's import. After any module sync, check `git diff config/G4BE08/symbols.txt`.
- After `sync_rel_symbols.py`, ninja does not re-split the module objects; delete
  `build/G4BE08/config.json` or `unit_info`/`fdiff` report stale names.

## Where the answers live

| Question | Read |
|---|---|
| What a subsystem does and where its entry points are | `docs/overview.md` (166 lines — read it before touching game code) |
| A byte difference → which compiler pass caused it → which source shape fixes it | `docs/matching.md`, "Lever catalogue" (indexed by the symptom you see in `fdiff`); "Conventions", "Facts you need", "Don'ts" |
| Where a given name came from, and what evidence a rename needs | `docs/naming.md`, then `CONTRIBUTING.md` "Proposing a rename" |
| Why a specific unit is shaped the way it is | `docs/unit-notes.md` |
| The evidence behind a `// COMPILER-DIFF:` tag | `docs/research/` — `dol.md` (GCC game units), `rel-rooms.md`, `rel-tools.md`, `cri.md` (MWCC), `compiler.md` (the toolchain itself) |
| REL layout, DRS archives, module unit boundaries | `docs/matching.md`, "REL modules" |

## Source layout

`src/game/` is the DOL (C++ plus a few newlib C units). The rest are REL overlays, one directory per
module: `em10..em3e` enemies (the 16 Ganado modules share `em10.cpp` plus one `emNN_set.cpp` each),
`pl02..pl14` player and NPC characters, `wep00..wep47` weapons, `st1..st4` stage rooms (one REL per
room group, `st3` is disc 2), `Sscrn` the sub-screens, `Tools` + `t_*` the developers' in-game
editors, `st/` shared room helpers, `wep/` shared weapon helpers, `lib/` SDK + CRI + runtime.
`include/` holds the reconstructed struct layouts.

Two structural ideas recur everywhere and are worth internalising before reading any game unit:

- **The object chain** `cUnit -> cCoord -> cModel` underlies everything drawn; `cEm` (enemies and the
  player), `cObj`, `cLight` and the effects all derive from it and live in `cManager<T>` pools.
- **The routine encoding** is identical across every class: `r_no_0` picks the R0 table (0 init,
  1 move, 2 damage, 3 die, 4 scenario), `r_no_1` the entry in it, `r_no_2` the step, `r_no_3` a
  variant. Once you can read that, most `emNN.cpp` and `rNNN.cpp` files read the same way.

## Port rules
- Working branch: port/macos-arm64. The original (matching) build must never break.
- All port code goes behind #ifdef TARGET_PC.
- Never commit orig/, the ISO, or any file extracted from the disc.
- Target: macOS arm64, clang, CMake, Aurora (../aurora) for GX/PAD/DVD/CARD.
- If something can't be verified, mark it "TO VERIFY"; don't guess.
- Small commits, one per logical change.

## Commits

One line, factual, saying what changed and that bytes are unchanged when that is the point:
`em10: name the Em10Work motion tables from the PS2 symbols (bytes unchanged)`. No trailers, no
signatures. One topic per PR; the description states what changed and how it was verified (the clean
rebuild with every line `OK`, and `asmcheck.py --all` unchanged).
