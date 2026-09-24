# macOS arm64 port — findings and phased plan

Branch `port/macos-arm64` (fork `notluken/re4`). Target: native macOS arm64, clang, CMake, Aurora
(`../aurora`) for GX/PAD/DVD/CARD. All port code behind `#ifdef TARGET_PC`. The original build must
keep producing the same bytes after every port commit; that is checked with the Docker build below.

Anything not verified is marked **TO VERIFY**.

## 1. Reproducing the original build (verified 2026-09-24)

`Dockerfile` + `docker-entrypoint.sh` + `.dockerignore` at the repo root. On a real x86_64 Linux host
(Ubuntu 26.04, 2 cores, 2 GB RAM + 4 GB swap):

```sh
git clone <repo> re4 && cd re4          # a Linux clone, not a bind mount of a macOS checkout
# orig/G4BE08/: disc images, or the already-extracted files/ + sys/ trees (see below)
docker build -t re4-build .
docker run --rm -v "$PWD":/re4 -w /re4 -v re4-build-vol:/re4/build \
  -v <NGC_GNU_SRC>/NGC:/opt/sn-gcc-src:ro -e SN_GCC_SRC=/opt/sn-gcc-src \
  re4-build bash -lc 'python3 configure.py && ninja && build/tools/dtk shasum -c config/G4BE08/build.sha1'
```

Result: **115/115 OK** (both discs), `asmcheck.py --all` TOTAL 231. Timings: image 47 s, native SN GCC
~39 s, `ninja` ~2m44s, `asmcheck` ~2m25s. The SN GCC mount is only needed the first time; `cc1`/`cc1plus`
are cached in the `re4-build-vol` volume.

What had to be learned to get there:

- **SN GPL source drop.** `archive.org/download/sn_sys_consoles_2/NGC/ProDGforNGCv393_Source_Code.zip`
  (md5 `4c037a84a2d7a675e260ba9fc590281e`, sha1 `01addd9bf5acbad2bf825b8ddf0a4f32db692742`, 28626142 B),
  inner `GC source code/NGC_GNU_SRC.zip` → `NGC/`, the tree `tools/sn-gcc/build.sh` expects.
- **x86_64 hardware is required.** Even with the native `cc1plus`, `tools/ngccc.py` still runs SN's
  `CPP.exe`, `NgcAs.exe` and `ngcld.exe` under wibo, which needs real x86 segmentation. Under amd64
  emulation on Apple Silicon it crashes: Rosetta `invalid gdt selector index 4`, QEMU user-mode SIGSEGV
  on every unit.
- **Case-sensitive filesystem was required; no longer, as of the `src/Tools` → `src/tools_mod`
  rename (below).** `src/Tools/` (3-line module wrappers) and `src/tools/` (shared bodies) used to be
  distinct directories in git that folded into one on APFS, so a macOS checkout showed
  `src/Tools/{t_prim,t_util,tools}.cpp` as modified (they held the `src/tools/` bytes). Verified with
  `git ls-files | tr A-Z a-z | sort | uniq -d` (empty output = no case-colliding path anywhere in the
  tree, repo-wide, not just this pair) after the rename. `tools/project.py`'s `check_path_case` (a
  generic case-insensitive-filesystem safety net used for every unit's `src_path`, unrelated to this
  specific pair) stays, since it is still useful against a future accidental collision. The x86_64
  hardware requirement above is independent of this and still applies.
- **`ulimit -n`.** `ngcld.exe` opens each of `main.elf`'s 674 inputs twice and Docker's default soft
  limit (1024) runs out: wibo logs `Unhandled errno 24`, ngcld exits 99 with no message. The entrypoint
  raises it to 65536.
- **Disc images are optional once extracted.** `configure.py` (lines 648-661) only opens
  `orig/G4BE08/*.iso|*.gcm` when an object listed in `config.yml` is missing. Copying the extracted
  `orig/G4BE08/files/` + `sys/` (~20 MB) is enough; disc 2 as `.gcm` is picked up automatically.
- **Disc 1 only.** README/CLAUDE.md say disc 1 alone gives 111/115. On macOS, `dtk dol split` (v1.8.3)
  instead aborted the whole split with `files/Rel/st3_0.rel not found`, so no `build.ninja` was produced.
  **TO VERIFY** on Linux before changing the docs.

## 2. Port state (2026-09-24)

- No port code yet: 0 `TARGET_PC` in `src/` and `include/`, no CMake, `../aurora` not cloned.
- Prior art: `tools/motion/host/` builds a host `libmotion_host.so` from game motion/IK code, with a
  64-bit host stub (`tools/motion/host/stub/re4_host_stub.h`). It is an export tool, not the port, but it
  is the first place where game code already runs on a 64-bit little-endian host.
- Inline asm that emits instructions is confined to the vendor's hardware code: `asmcheck` TOTAL 231,
  8 asm-bodied units. A broad grep counted ~1077 asm lines. **TO VERIFY**: that number does not agree with
  `asmcheck`.

## 3. Phased plan

Each phase ends with the original build still at 115/115 OK (Docker, clean `build/` volume) and
`asmcheck.py --all` unchanged. Small commits, one logical change each.

### Phase 0 — ground rules and CI (done / in progress)

- [x] Docker image for the original build, verified 115/115 on x86_64.
- [ ] CI job (GitHub Actions, x86_64 runner) running the Docker build on every push to
  `port/macos-arm64`. Needs `orig/G4BE08/{files,sys}` and the SN source as a private artifact or
  secret-backed download; the disc data must never be committed.
- [ ] Settle the disc-1-only question (section 1) and fix README/CLAUDE.md if they are wrong.

Exit: every push gets an automatic byte-identity check.

### Phase 1 — a host build that compiles (2026-09-24, three slices)

- **Aurora**: cloned into `../aurora`, pinned at `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` (main,
  2026-09-23). `cmake -S . -B build -DAURORA_ENABLE_TESTS=OFF -DAURORA_ENABLE_EXAMPLES=OFF` then
  `cmake --build build -j4` succeeds standalone on this host (Apple clang 17, CMake 4.4.3):
  Dawn resolves to a prebuilt package (`dawn-darwin-arm64.tar.gz`, provider `package`, no local Dawn
  build), nod likewise prebuilt; SDL3 resolves to the system install (Homebrew). Configure ~10 s,
  full build (`aurora_gx`/`aurora_gd`/`aurora_pad`/...) a few minutes; total checkout + build tree
  160 MB. Not yet wired into the game-code CMake build (that's Phase 4, GX/PAD/DVD/CARD).
- **`CMakeLists.txt`** (top level, ignored by `configure.py`/`ninja`; build tree `build-pc/`,
  gitignored; `CMAKE_EXPORT_COMPILE_COMMANDS ON` for clangd, not committed — see the file's header
  comment for how to point an editor at it): four targets, `re4_game_core` (7 representative
  `src/game` units: `model.cpp` object chain, `light.cpp` manager consumer, `math_sub.cpp` math,
  `dvd.cpp`/`read.cpp` loader, `cString.cpp` utility, `item.cpp` pool — no `src/game/*.c` unit, see
  the libc point below), `re4_game_all` (every other `src/game/*.cpp` except the two
  whole-function-asm units), `re4_rel_all` (every REL module unit — `em*`/`pl*`/`wep*`/`st*`/
  `Sscrn`/`Tools`/`t_*`, plus the shared `st/`, `wep/`, `tools/` sources — from
  `cmake/gen_rel_sources.py`, which reads `config/G4BE08/{config.yml,modules.py}` the same way
  `configure.py`'s own REL object loop does). The last two are error-inventory-only (`-- -k 0`).
  Same include paths and version defines `configure.py` uses for the ProDG game units, `-DTARGET_PC`
  added.
- **The `Tools`/`tools` APFS fold — fixed by renaming `src/Tools` to `src/tools_mod` (`git mv`)**.
  Three filenames used to exist in both `src/Tools/` (the "Tools" REL module's own 3-line wrappers)
  and `src/tools/` (shared bodies compiled into the `t_*` tool modules) — `t_prim.cpp`, `t_util.cpp`,
  `tools.cpp`. On a case-insensitive filesystem those were the same directory entry, so reading
  `src/Tools/t_prim.cpp` returned whichever content last got checked out.
  - **Byte-impact check, before renaming anything**: every path string that reaches output bytes
    (`#line N "D:/Bio4/Prog/<file>.cpp"`, ~700 of them repo-wide) uses the vendor's original
    `D:/Bio4/Prog/` tree, never the git repo's directory name — `grep -rn '"[^"]*[Tt]ools/[^"]*"'
    src/ include/` turned up nothing but one `#include "tools/t_prim.cpp"` (a compile-time directive,
    not embedded text) and one comment. So a directory rename changes zero output bytes, as long as
    the `#include`s that reference the moved directory are updated and no `#line`/assert string is
    itself edited.
  - **Which directory, and why**: `config/G4BE08/modules.py`'s REL unit tuples are
    `(unit_name, first_function, source_override, [data_starts])`; `splits.txt`/`sym_map.tsv`/
    `symbols.txt` (generated, config/G4BE08/modules/<mod>/) key everything off `unit_name`, a label
    independent of the actual file on disk. Renaming `src/tools` (shared, referenced by ~30
    already-explicit `source_override` strings across nine module blocks) would mean editing existing
    string values scattered through the whole file. Renaming `src/Tools` (the "Tools" module's own
    17 files, all but 3 defaulting `source_override` to `None` i.e. "same as unit_name") means adding
    one explicit override per entry, but every edit lives in one contiguous 24-line block — smaller,
    more easily verified blast radius. Picked `src/Tools` → `src/tools_mod`; `unit_name` strings
    (`"Tools/t_prim.cpp"`, ...) are untouched, so `splits.txt`/`sym_map.tsv`/`symbols.txt` needed no
    regeneration (`tools/gen_rel_config.py` was not run — nothing they store changed).
  - **The move itself**: the 14 non-colliding filenames (`db_toolbase.cpp`, `t_atari.cpp`, ...) took
    a plain `git mv`. The 3 colliding filenames could not: `git mv src/Tools/t_prim.cpp
    src/tools_mod/t_prim.cpp` would `rename(2)` the one physical inode both `src/Tools/t_prim.cpp`
    and `src/tools/t_prim.cpp` pointed to, deleting `src/tools/t_prim.cpp` out from under its own
    git path. Instead: `git rm --cached` the old `src/Tools/` path (index only, working tree
    untouched), `git show HEAD:src/Tools/<f> > src/tools_mod/<f>`, `git add` the new path — writes
    the correct bytes to the new location from git's object store, never touches the shared inode.
    Verified after: `git ls-files src/Tools/` empty, `git diff --stat src/tools/` empty, all 17
    `src/tools_mod/*.cpp` byte-identical to their pre-rename `git show HEAD:src/Tools/*.cpp` blobs,
    `git status` clean except `.claude/` — the three phantom-modified paths are gone (there is no
    `src/Tools/` left to be phantom-modified). One accidental near-miss during this work (an earlier,
    now-reverted pass had globbed the folded `src/Tools/t_prim.cpp` path before this rename existed,
    which would have written a wrongly-labeled edit; caught by the diff size and reverted before
    anything was staged — see `docs/port-phase1-errors.md` for the detail) is why this rename went
    through the untracked-then-rewrite path above instead of a plain `mv`.
  - `config/G4BE08/modules.py`: the "Tools" module's 17 units, plus the one other cross-reference
    (`t_event/db_toolbase.cpp`'s override), now point their `source_override` at `tools_mod/<f>.cpp`.
  - `cmake/gen_rel_sources.py`: the `Tools`-unit skip list is gone, nothing is excluded from
    `re4_rel_all` any more.
  - `tools/project.py`'s `check_path_case` comment (the function itself, a generic case-insensitive-
    filesystem safety net for every unit's `src_path`, is unrelated and stays) no longer cites this
    specific pair.
  - `git ls-files | tr A-Z a-z | sort | uniq -d` is empty (repo-wide, not just this pair) — see
    docs/port.md section 1 for what that means for the original Docker build.
- **Headers/sources fixed this round**, all behind `TARGET_PC`, chosen the same way as before (each
  either unblocks many units or is a one-line mechanical pattern):
  - `cManager.h`/`esp.h`/`card.h`/`cam_extra.h`/`main_mem.cpp`/`esp.cpp`: placement/member/replacement
    `operator new`/`delete` re-spelled with `std::size_t` (only valid deallocation functions because
    `unsigned int` happens to equal `size_t` on the GameCube target).
  - `joy.h`/`db_toolbase.h`/`db_widget.h`: dropped `size_t`-mismatched `memcpy`/`strlen`
    redeclarations in favour of `<cstring>`.
  - `math_sub.h`'s single-instruction `fabs` → `__builtin_fabsf`; `dbg_tool.h`'s `asm("li %0,0")`
    (COMPILER-DIFF #13, a pure zero-register scheduling trick, not hardware) → a plain `= 0`
    initializer — the only two asm bodies judged "small and obviously equivalent" this round.
  - `dbg_var.h`: `cVarLoop`'s out-of-class members call `cVarRange<T>`'s members unqualified, which
    GCC 2.95 resolves into the dependent base at first-phase lookup (non-conformant) but ISO two-phase
    lookup (clang) rejects; qualified with `this->` via a macro that is empty for the original target.
  - `t_esp.cpp`'s `operator new(unsigned n) asm("__builtin_new")` (a documented COMPILER-DIFF-adjacent
    alias trick binding to the compiler's builtin-new symbol): host branch drops the asm-label, takes
    `std::size_t`, forwards to `::operator new`.
  - `xml.cpp`/`main_mem.cpp`/`r332.cpp`: `strstr`/`strrchr` results assigned to `char*` — the GCC
    2.95/MSL libc's single non-const-correct overload always returned `char*`; libc++ has the
    standard const-correct pair. Host-only wrapper functions (`strstr_host`/`strrchr_host`, `#define`d
    over the plain name for the rest of the file) restore the old behaviour.
  - `emwindow.cpp`: missing `<cstring>` (reached transitively on the original target, not on host).
  - `motion.cpp`: one `__attribute__((section(".sdata")))` (Mach-O needs `"segment,section"`) dropped
    under `TARGET_PC`, same family as the next point.
  - 64 mechanical `asm(".section .sdata|.rodata|.bss|.data ...")` alignment pragmas across
    `src/game/*.cpp` and the REL sources (small-data/rodata/bss/data layout for the ProDG linker, no
    host meaning) `#ifndef TARGET_PC`-gated.
  - `-Wno-address-of-temporary` added to every target's compile options: `&((Vec){...})`
    compound-literal-address idiom (GNU C, several `em*`/`wep*` units) is legal for GCC 2.95 and alive
    for the enclosing full expression; clang's default diagnostic for it is a hard error, not a
    warning, hence the flag rather than a source change.
  - `st1/r113.cpp`/`st1/r11d.cpp`: their `asm("" : "+f"(spd)); // COMPILER-DIFF: candidate #9` (an
    empty-template asm — no instruction, a pure GameCube scheduling barrier) is now
    `ASM_BARRIER_F(spd)`, a macro defined once near the top of each file (before that file's single
    `#line` directive, so nothing after it shifts) that expands to the same `asm(...)` normally and
    to nothing under `TARGET_PC`; the `COMPILER-DIFF` comment stays on the same, unmoved line. Verified
    by keeping the physical-line gap between each file's `#line` directive and the `ASM_BARRIER_F`
    call site identical before/after (62 and 325 lines respectively) and rechecked with `bytecmp.py`
    on the remote host. A third, superficially similar site (`st2/r203.cpp`) was left alone: its two
    `asm("" : "=r"(pin))`/`asm("" : "=f"(fpin))` lines are tied to `register int pin asm("r29")` /
    `register f32 fpin asm("fr31")` immediately above them — real PPC register pinning, Phase 5
    material, not a standalone scheduling barrier; wrapping just the barrier lines would not have
    fixed the unit anyway.
- **libc: `src/game`'s newlib reimplementation is excluded from every CMake target, not attempted.**
  Every `src/game/*.c` file (58 of them: `printf.c`, `fprintf.c`, `sprintf.c`, `sscanf.c`, `memcpy.c`,
  `strlen.c`, `math_support.c` built for `vfprintf`'s float conversion, ...) opens with a
  `newlib 1.8.2 libc/...` provenance comment — the game's own C runtime, built against a custom
  `newlib_stdio.h`/`_reent`/`FILE` layout that conflicts with the host's `<stdio.h>`/`<string.h>`.
  The port uses the host's system libc instead of building any of it (no REL module references a
  `src/game/*.c` file either, confirmed against `modules.py`). `re4_game_core` accordingly has no C
  representative unit any more — `src/game/*.c` is entirely newlib, so there is no non-libc `.c` file
  left to pick one from.
- **Result**: `re4_game_core` compiles clean except `math_sub.cpp` (paired-single asm) and
  `model.cpp` (pointer-to-int casts) — both genuine Phase 2/5 material, not header bugs.
  `re4_game_all`: **258 / 293 `.cpp` units (~88%) compile clean**, 35 fail (down from 351 units
  counted before the newlib `.c` exclusion — not a regression, a narrower and more honest count).
  `re4_rel_all`: **268 / 305 units (~88%) compile clean**, 37 fail — all 305 REL units are now
  attempted (the Tools/tools rename removed the 3-unit exclusion). Full categorized inventory of both
  remainders, with every deferral's reasoning: `docs/port-phase1-errors.md`.

**Phase 1 exit** (all four conditions hold):
1. `src/game` compiles (not links) on macOS arm64 for a representative slice (`re4_game_core`, 5/7
   units clean) and the wide sweep (`re4_game_all`, 258/293 `.cpp` units; newlib `.c` units excluded
   by design, see the libc point above).
2. Every REL module compiles for its own unit boundaries where the source is host-clean
   (`re4_rel_all`, 268/305 — every unit is attempted, none skipped).
3. Every remaining failure is categorized in `docs/port-phase1-errors.md` with a named cause and an
   explicit phase: **Phase 2** (pointer-to-int/int-to-pointer casts and struct-size assumptions in
   loaded/relocated data — `model.cpp`, `sce_at.cpp`, `sce_sys.cpp`, `st/em_wrap.cpp`, most `rNNN.cpp`
   rooms, the `0x320 - sizeof(cModel)` padding units, ...), **Phase 5** (real PPC paired-single/GQR
   asm — `math_sub.cpp`'s `SQRTF`/`SINF`/`COSF`, `dbmodule.cpp`'s `PSQ_L_S16`/`PSQ_L_U8_TO` family,
   `em2d.cpp`'s `register ... asm("fr0")`, `st2/r203.cpp`'s register-pinned barrier pair, ...), or
   **Phase 4** (the two SDK-shaped call sites `AddOtWorldPos`/`VISetPostRetraceCallback`).
4. Nothing in this slice hacked around a Phase 2/5 problem to make a unit compile: every asm/cast left
   failing is left failing, with its reasoning on record instead. The one asm rewrite this round
   (`ASM_BARRIER_F`) is a no-op scheduling barrier with zero semantic content, not a stand-in for real
   hardware behaviour.

Exit (original wording, still true): `src/game` compiles (not links) on macOS arm64; the error
inventory for phases 2-3 is written down. Now additionally true for the REL modules.

### Phase 2 — 64-bit pointers in loaded data (started 2026-09-24; steps 1-5 done)

Full inventory, strategy, macOS-specific findings, decisions, and the step-by-step plan with
acceptance criteria: **`docs/port-phase2.md`**. Summary: strategy D (compressed 32-bit handles
relative to a host arena, `include/port/ptr32.h`'s `Ptr32<T>`), a build-time cast rewriter
(`tools/port/cast_rewriter/`, implemented — section 8 of `docs/port-phase2.md`) for the ~1,693 direct
pointer<->integer casts outside the header macros, and a
GameCube-compatible on-disc save format (no disc-format/memory-format split). Steps 1
(`s32`/`u32` as 4-byte `int`/`unsigned int`, `RE4_U32_32` CMake option, off by default),
2 (`include/port/ptr32.h` + `tests/port/test_ptr32.cpp`) and 3 (`src/port/arena.cpp`: the arena is a
zerofill section embedded in the exe image, not a runtime VM reservation — a first version that
reserved memory with `VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE` measured 100/100 but was silently
clobbering live malloc bookkeeping in the process, caught before it shipped, see
`docs/port-phase2.md` "the host arena") are done and verified (115/115, `asmcheck` TOTAL 231, both
`ctest`s passing, 100/100 on `tests/port/run_arena_stress.sh`, a `malloc()` canary check that would
have caught the rejected design). Step 4 (`ARC_PTR`/`Flag*`/`VALID_PTR` macro family, one
`TARGET_PC` branch each, decided per macro whether it is real GameCube-address compression or plain
same-buffer pointer arithmetic) and step 5 (`Ptr32<T>` for `cModelData`/TPL/`CameraAreaInfo`+`Cut`+
`Rec`/`SAVE_DATA_HEAD`/`CRoomInfo`/`cSatBlock`, their relocators, and 93 generated `offsetof()`
static_asserts, all passing) are also done: `RE4_U32_32=ON` error counts fell from 1,506/4,061
(`src/game`/REL, step 1 alone) to 586/1,046 after both steps, `RE4_U32_32=OFF` unchanged at 35/37
throughout (one regression caught and fixed before commit — `docs/port-phase2.md`, "what nearly went
wrong").

### First boot (reordered ahead of the remaining phases, 2026-09-24)

Goal: reach the title screen as fast as possible, deferring everything not on that path. Full
detail: **`docs/port-boot.md`** (call-graph inventory) and `docs/port-phase2.md` section 8 (rewriter
design). Four steps, in order:

- **(a) Cast rewriter** — `tools/port/cast_rewriter/` (libTooling, built against Homebrew LLVM;
  Apple's bundled clang has no libTooling), driven by `tools/port/rewrite_casts.py`, wired into
  `CMakeLists.txt` behind `RE4_REWRITE_CASTS` (implied by `RE4_U32_32`). Rewrites non-macro
  `CK_PointerToIntegral`/`CK_IntegralToPointer` casts into `GC32()`/`GCPTR<T>()` under `build-pc/gen/`;
  the macro family (`ARC_PTR`/`FlagChk`/...) stays hand-fixed (Phase 2 step 4, done).
- **(b) All of `src/game` compiling with `RE4_U32_32=ON`** — units that genuinely need Phase 5's
  paired-single/GQR asm get host stubs or C equivalents only where boot needs them; everything else
  on that path is deferred. RELs stay deferred except whatever boot turns out to need (currently:
  none — `docs/port-boot.md` section 1).
- **(c) Endianness for boot-only formats** — the DVD size table, `CRoomInfo` (`roomInfo.dat`), and
  the title archive's model BIN + TPL contents (`docs/port-boot.md` section 3); not every format in
  `docs/port-phase2.md` section 1's table, just what boot touches.
- **(d) Link and run** — one executable, Aurora for GX/PAD/DVD (from an extracted disc directory,
  `orig/G4BE08/files`)/CARD, stubs for the rest of the SDK (OS threads/alarms/interrupts, ARAM, VI —
  `docs/port-boot.md` section 2's table), run on an arena thread (`CreateArenaThread`) until the
  first crash.

Exit: the executable starts, runs the boot sequence, and reaches the title screen (or a first,
diagnosable crash on the way).

### Phase 3 — endianness (the rest)

Whatever "first boot" above did not need: every other on-disc/relocated format in
docs/port-phase2.md section 1's table. GameCube is big-endian, arm64 is little-endian.

- Byte-swap at load time per format (one swapper per file type, next to its loader), not at every use.
- Save data (CARD) needs a decision: keep GameCube-compatible big-endian or host-native (already
  decided GameCube-compatible for the format itself, docs/port-phase2.md section 4 — this phase is
  the swap-at-load-time mechanics, not the format decision).

Exit: each loaded format has a swapper with a test that round-trips a real file from `orig/`.

### Phase 4 — REL modules

- RELs linked statically into the executable; replace the REL loader (`OSLink`) with a table of the
  modules' prolog/epilog/unresolved entry points.
- Every REL module (`em*`/`pl*`/`wep*`/`st*`/`Sscrn`/`Tools`/`t_*`) compiling and linking, not just
  the boot-path subset "first boot" above needed (which was none).

Exit: a room's REL modules load and run in-process, no `OSLink` left.

### Phase 5 — subsystems

- Sound: the `snd_*` driver targets the DSP and ARAM; rewrite the output stage on a host audio API
  (CoreAudio, or SDL if Aurora already uses it). ADX streams decoded on the host.
- FMV: CRI SFD/ADX, which needs a host decoder (or skip the FMVs at first).
- Paired-single / GQR math units: C equivalents under `TARGET_PC`, for every remaining unit (first
  boot only did the ones actually on its path).

Exit: a room is playable with graphics, input and sound.

### Phase 6 — play-through and polish

- Play through both discs' content; fix crashes and rendering differences against Dolphin.
- Frame pacing, resolution, save compatibility, packaging (`.app`).

## 4. Invariants (from CLAUDE.md)

- The original bytes never change; every port change sits behind `#ifdef TARGET_PC`.
- Never commit `orig/`, a disc image, or anything extracted from the disc.
- Small commits, one per logical change.
