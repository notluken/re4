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
- **Case-sensitive filesystem is required.** `src/Tools/` (3-line module wrappers) and `src/tools/`
  (shared bodies) are distinct directories in git. On APFS they fold into one, so a macOS checkout shows
  `src/Tools/{t_prim,t_util,tools}.cpp` as modified (they hold the `src/tools/` bytes) and dtk warns about
  11 such pairs. Never commit those three "changes"; never build from a bind-mounted macOS tree.
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

### Phase 1 — a host build that compiles (2026-09-24, two slices)

- **Aurora**: cloned into `../aurora`, pinned at `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` (main,
  2026-09-23). `cmake -S . -B build -DAURORA_ENABLE_TESTS=OFF -DAURORA_ENABLE_EXAMPLES=OFF` then
  `cmake --build build -j4` succeeds standalone on this host (Apple clang 17, CMake 4.4.3):
  Dawn resolves to a prebuilt package (`dawn-darwin-arm64.tar.gz`, provider `package`, no local Dawn
  build), nod likewise prebuilt; SDL3 resolves to the system install (Homebrew). Configure ~10 s,
  full build (`aurora_gx`/`aurora_gd`/`aurora_pad`/...) a few minutes; total checkout + build tree
  160 MB. Not yet wired into the game-code CMake build (that's Phase 4, GX/PAD/DVD/CARD).
- **`CMakeLists.txt`** (top level, ignored by `configure.py`/`ninja`; build tree `build-pc/`,
  gitignored; `CMAKE_EXPORT_COMPILE_COMMANDS ON` for clangd, not committed — see the file's header
  comment for how to point an editor at it): four targets, `re4_game_core` (8 representative
  `src/game` units: `model.cpp` object chain, `light.cpp` manager consumer, `math_sub.cpp`/
  `math_support.c` math, `dvd.cpp`/`read.cpp` loader, `cString.cpp` utility, `item.cpp` pool),
  `re4_game_all` (every other `src/game/*.c(pp)` except the two whole-function-asm units),
  `re4_rel_all` (every REL module unit — `em*`/`pl*`/`wep*`/`st*`/`Sscrn`/`Tools`/`t_*`, plus the
  shared `st/`, `wep/` sources — from `cmake/gen_rel_sources.py`, which reads
  `config/G4BE08/{config.yml,modules.py}` the same way `configure.py`'s own REL object loop does).
  The last two are error-inventory-only (`-- -k 0`). Same include paths and version defines
  `configure.py` uses for the ProDG game units, `-DTARGET_PC` added.
- **The `Tools`/`tools` APFS fold, concretely**: three filenames exist in both `src/Tools/` (the
  "Tools" REL module's own 3-line wrappers) and `src/tools/` (shared bodies compiled into the `t_*`
  tool modules) — `t_prim.cpp`, `t_util.cpp`, `tools.cpp`. On a case-insensitive filesystem they are
  the same directory entry, so reading `src/Tools/t_prim.cpp` on this host silently returns whichever
  content last got checked out — currently the `src/tools/` (shared-body) bytes, which is *wrong* for
  the `Tools` module and is exactly why `git status` always shows those 3 paths modified (pre-existing,
  documented in section 1; never stage them). `cmake/gen_rel_sources.py` excludes the `Tools`-module
  copies of these 3 units rather than guessing which content is on disk (see its docstring); every
  other unit, including the correctly-resolving `src/tools/` shared copies used by `t_camera`/
  `t_emlist`/..., is unaffected, since no other filename collides. One real edit was needed in the
  shared `src/tools/t_prim.cpp` itself (a `.section` pragma, see below) — made and verified through
  the lowercase path only, `git add src/tools/t_prim.cpp` explicitly, never the `Tools/` spelling.
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
- **Result**: `re4_game_core` compiles clean except `math_sub.cpp` (paired-single asm) and
  `model.cpp` (pointer-to-int casts) — both genuine Phase 2/5 material, not header bugs.
  `re4_game_all`: **312 / 351 units (~89%) compile clean**, 39 fail. `re4_rel_all`: **264 / 302 units
  (~87%) compile clean**, 38 fail (out of 305 REL units total; 3 skipped, see the Tools/tools point
  above). Full categorized inventory of both remainders, with every deferral's reasoning:
  `docs/port-phase1-errors.md`.

**Phase 1 exit** (all four conditions now hold):
1. `src/game` compiles (not links) on macOS arm64 for a representative slice (`re4_game_core`, 6/8
   units clean) and the wide sweep (`re4_game_all`, 312/351).
2. Every REL module compiles for at least its own unit boundaries where the source is host-clean
   (`re4_rel_all`, 264/302; the `Tools`-module APFS-fold exclusion is the only unit skipped outright,
   not attempted and failed).
3. Every remaining failure is categorized in `docs/port-phase1-errors.md` with a named cause and an
   explicit phase: **Phase 2** (pointer-to-int/int-to-pointer casts and struct-size assumptions in
   loaded/relocated data — `model.cpp`, `sce_at.cpp`, `sce_sys.cpp`, `st/em_wrap.cpp`, the
   `0x320 - sizeof(cModel)` padding units, ...), **Phase 5** (real PPC paired-single/GQR asm —
   `math_sub.cpp`'s `SQRTF`/`SINF`/`COSF`, `dbmodule.cpp`'s `PSQ_L_S16`/`PSQ_L_U8_TO` family,
   `em2d.cpp`'s `register ... asm("fr0")`, ...), or **later, unscoped** (the four newlib libc
   reimplementations `printf.c`/`fprintf.c`/`sprintf.c`/`sscanf.c`, which need a real libc-shim
   decision, and the two SDK-shaped call sites `AddOtWorldPos`/`VISetPostRetraceCallback` that are
   Aurora's/Phase 4's job).
4. Nothing in this slice hacked around a Phase 2/3/5 problem to make a unit compile: every asm/cast
   left failing is left failing, with its reasoning on record instead.

Exit (original wording, still true): `src/game` compiles (not links) on macOS arm64; the error
inventory for phases 2-3 is written down. Now additionally true for the REL modules.

### Phase 2 — 64-bit pointers in loaded data

The disc formats (models, motions, DAT/DRS archives, REL fixups, save data) embed 32-bit pointers and
offsets that are relocated in place after loading. On a 64-bit host those structs change size.

- Inventory every struct loaded from disc that contains a pointer (start from what
  `tools/motion/host` already had to solve).
- Pick one strategy and apply it consistently: 32-bit offset fields plus accessors under `TARGET_PC`,
  or load-time conversion into host-layout structs. Decide with the inventory in hand.
- `static_assert` the on-disc struct sizes under `TARGET_PC`.

Exit: every on-disc struct has a checked size and a defined host representation.

### Phase 3 — endianness

GameCube is big-endian, arm64 is little-endian. **TO VERIFY**: which formats are read field-by-field
versus mapped in place; the earlier survey only looked at code, not at data formats.

- Byte-swap at load time per format (one swapper per file type, next to its loader), not at every use.
- Save data (CARD) needs a decision: keep GameCube-compatible big-endian or host-native.

Exit: each loaded format has a swapper with a test that round-trips a real file from `orig/`.

### Phase 4 — link and boot

- RELs linked statically into the executable; replace the REL loader (`OSLink`) with a table of the
  modules' prolog/epilog/unresolved entry points.
- Aurora for GX, PAD, DVD (reading from an extracted disc directory), CARD.
- Stubs for everything else (OS threads/alarms onto host threads/timers, ARAM, VI).

Exit: the executable starts, runs the boot sequence and reaches the title screen, even without sound.

### Phase 5 — subsystems

- Sound: the `snd_*` driver targets the DSP and ARAM; rewrite the output stage on a host audio API
  (CoreAudio, or SDL if Aurora already uses it). ADX streams decoded on the host.
- FMV: CRI SFD/ADX, which needs a host decoder (or skip the FMVs at first).
- Paired-single / GQR math units: C equivalents under `TARGET_PC`.

Exit: a room is playable with graphics, input and sound.

### Phase 6 — play-through and polish

- Play through both discs' content; fix crashes and rendering differences against Dolphin.
- Frame pacing, resolution, save compatibility, packaging (`.app`).

## 4. Invariants (from CLAUDE.md)

- The original bytes never change; every port change sits behind `#ifdef TARGET_PC`.
- Never commit `orig/`, a disc image, or anything extracted from the disc.
- Small commits, one per logical change.
