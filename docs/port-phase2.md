# Phase 2 — 64-bit pointers in loaded data

Status 2026-09-24: steps 1-3 done (this document), step 4+ not started. Strategy D approved by the
user; option (ii) (build-time cast rewriter) approved for the ~1,693 direct pointer<->integer casts;
the save-game format stays GameCube-compatible (on-disc bytes, not just in-memory layout).

## 1. The inventory

Every GameCube on-disc/relocated format embeds 32-bit pointer fields that get fixed up in place at
load time (file offset -> absolute address) and fixed back before being written out again. On a
64-bit host none of these fields can hold a real pointer without changing the struct's size, which
is fatal for anything that is literally read from disc byte-for-byte.

| Format | Where | Relocator | Marker |
|---|---|---|---|
| Model BIN (`cModelData`) | `include/model.h:93` (layout, offsets 0x00-0x34), `src/game/model.cpp:925-994` (`calcModelAddr`/`calcModelOffset`/`slideModelAddr`) | `calcModelAddr`/`calcModelOffset`, called per model load/save | `pClr`'s sign bit: `(int) d->pClr < 0` means "already a pointer" |
| TPL (texture palette) | `include/model.h`, `src/game/model.cpp:1019` (`calcTplAddr`), `src/game/card.cpp:3058` (`cCard::calcTplAddr`) | At least 7 separate relocator call sites/copies across `model.cpp`/`card.cpp`/`mes.cpp`/`trans.cpp` (`calcTplAddr`/`calcTplOffset`, `grep -rln` count) — TPL relocation is duplicated per caller rather than shared | Same family as model BIN |
| CAM (camera data, "B400".."B404") | `src/game/cam_ctrl.cpp:287` (`CameraControl::calcAddr`) | `calcAddr`/its offset counterpart, per area-polygon and cut-key array | Version-tagged header, relocated-or-not tracked by the caller |
| Save game (`SAVE_DATA_HEAD`) | `src/game/game.cpp:900` (`cGameSave::calcOffset`), `:925` (`cGameSave::calcAddr`) | `calcOffset`/`calcAddr`, `pGlobal`/`pRm`/`pSscrn`/`pMr`/`pItm` fields | `head->base == 0` means "already offsets" |
| Room-to-room jump table (`room_jmp`) | `src/game/room_jmp.cpp` | offset-only format (no live pointers persisted, just table indices) | n/a — no relocator, not a Phase 2 pointer problem |
| SAT (collision, `cSat`) | `src/game/atari.cpp:1141` (`cSat::blockInit`) | `blockInit`, relative block links -> pointers once | `VALID_PTR(pBlock->m_pList)`: same 0x80000000-0x817FFFFF SysMem-range test every relocated-or-not check in the tree uses |
| FCV (motion curve tables) | `src/game/motion.cpp:234` | inline in `MotionSetCore`-adjacent code, `tbl[i] += (u32) w->pMot` | Sign bit of the first table entry (`(s32) tbl[0] >= 0`) |
| Camera motion (`cam_motion.cpp`) | `src/game/cam_motion.cpp` | same family as CAM/FCV, offset tables fixed up once per load | Sign-bit / version-tagged, consistent with the rest |
| Shape (vertex delta) tables | `src/game/shape.cpp`, `cModelData::shapeOfs` (`include/model.h:106`) | offset-only field (`shapeOfs`, a `u32` byte offset from the model's base, never promoted to a pointer) | n/a — already Phase-2-safe as an offset, not a pointer |
| REL fixups (module relocation records) | `src/lib`/dtk-generated, loaded by `OSLink` | Not touched in Phase 2: REL loading itself is replaced wholesale in **Phase 4** (RELs linked statically into the host executable, `OSLink` replaced by a jump table) | — |

Two formats above (`room_jmp`, `shapeOfs`) are **offset-only**: they never hold a relocated absolute
pointer, only a byte offset applied at every use. They need no `Ptr32<T>` treatment; they are already
what strategy D turns every *other* format's fields into, just without the sign-bit-address trick
(nothing needs them to look like a real GameCube address, since they are never compared against
`VALID_PTR`/`SysMem` ranges).

Everything else in the table is exactly the "sign bit set = already relocated" shape `VALID_PTR` and
`SysMem`'s `0x80000000`-`0x817F4000` address map (`src/game/main_mem.cpp`) already rely on — the
reason strategy D's compressed handles are built to look like real GameCube addresses (section 3).

## 2. macOS facts (this host: macOS 26.6.2, Apple Silicon, arm64)

- **`__PAGEZERO` below 4 GiB is killed.** A Mach-O with `__PAGEZERO` smaller than `0x100000000`
  exits with rc 137 (SIGKILL) before `main()`. This rules out "just link low" — the low 4 GiB is
  categorically unavailable to a normal executable's own mappings.
- **No `MAP_32BIT`, and `mmap()`/`malloc()` never return below 4 GiB.** Every general-purpose
  allocation on this host lands above `0x100000000`. Confirmed here (all addresses in one process,
  no options): exe image `~0x100000000-0x1045000000`, main stack `~0x16Bxxxxxxx-0x16Fxxxxxxx`, dyld
  shared cache `~0x182000000` upward. So the exe image and stack are already, incidentally,
  well-behaved 32-bit-*offset*-from-4GiB citizens.
- **A reliable free window exists in the ~2.5-6.5 GiB range**, i.e. `[0x0A0000000, 0x1A0000000)`
  (above `__PAGEZERO`, below the dyld shared cache): both the exe image and the main stack fall
  inside it on every run observed.
- **`mach_vm_allocate(..., VM_FLAGS_FIXED)` (no `VM_FLAGS_OVERWRITE`) is unreliable for a large
  (256 MiB-1 GiB) fixed reservation in that window, measured on this host** (this refines/corrects
  the Phase 2 planning note that a single fixed 1 GiB request at `0x120000000` succeeded 6/6): it
  refuses if *any* byte of the requested range is already backed, and small ASLR-placed allocator
  bookkeeping regions (`vmmap` on a live process: "MALLOC guard page"/"MALLOC metadata", a few KiB to
  a few MiB) land inside `[0x120000000, 0x160000000)` often enough that:
  - a single fixed 1 GiB request at `0x120000000` failed 37/50 single-shot fresh-process runs;
  - even trying 12 candidate addresses 128 MiB apart across ~2.5 GiB of the window still failed
    37/50 (each candidate's 1 GiB span overlaps its neighbours too much to actually diversify away
    from one obstacle);
  - `VM_FLAGS_ANYWHERE` (mach or plain `mmap()` with a low hint, no `MAP_FIXED`) does not help either:
    this host's allocator picks between two placement modes (one clustered near the exe image, one
    far away around `0x300000000`+) **once per process**, applying to every such call in that
    process — a retry loop inside one process keeps hitting the same mode (measured 50/50 across 100
    fresh-process runs, 8 retries each, not the ~99.6% independent retries would predict).
- **`VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE` was tried, measured 100/100, and rejected anyway — it does
  not check the range is free, it unconditionally unmaps whatever is already there.** This is the
  Mach equivalent of `mmap()`'s unconditionally-claiming `MAP_FIXED`, and 100/100 is the *wrong*
  signal: in exactly the runs where plain `VM_FLAGS_FIXED` had correctly refused (the 37/50 above),
  `OVERWRITE` was silently unmapping the live malloc guard page/metadata region that was blocking it
  — corrupting the host allocator's own bookkeeping, to surface as a crash later, nondeterministically,
  in code that never touches the arena. The first version of `src/port/arena.cpp` shipped this for
  one round-trip of this document before it was caught; **never repeat this approach.** No amount of
  passing tests on the arena itself would have caught it (the bug is in what it clobbers, not in the
  arena), which is why `tests/port/test_arena.cpp` now includes a `malloc()`'d canary buffer, written
  before `InitArena()` and checked for corruption after every arena byte is touched (section 5, step
  3) — a real, if narrow, regression test for exactly this mistake.
- **The arena instead lives inside the executable's own image: a zerofill BSS-like section, not a
  runtime reservation at all.** `static char s_arena[1<<30]` with
  `__attribute__((aligned(16384), section("__DATA,__re4arena,zerofill")))` — the `,zerofill` suffix
  matters: naming the section without it compiles and links, but the linker then treats it as
  ordinary initialized data and the binary balloons to a full 1 GiB on disk (measured, rejected); with
  it, `size -m` reports `__re4arena (zerofill)` and the binary stays a few tens of KiB, exactly like a
  plain unnamed `static char[1<<30]` (which lands in the default `__DATA,__bss` zerofill section
  automatically — also measured, works, just without a distinguishing name). dyld places this section
  as part of loading the exe, before any of this code runs, at wherever ASLR slides the image to; it
  cannot overlap malloc's or any other later allocation because it was never a separate allocation to
  begin with. Measured **100/100** fresh-process runs: the whole exe image (a `.bss` probe and a code
  probe) and the whole arena (both ends) inside the resulting window, a thread whose stack is carved
  from the arena (`pthread_attr_setstack`, `CreateArenaThread` in `src/port/arena.h`) also inside it,
  and — the check the `OVERWRITE` design would have failed — a `malloc()`'d canary buffer intact after
  every arena page is written to. A direct `vmmap` before/after comparison of a process that touches
  every arena page (same region count, zero diff, both outside the arena region) corroborates this
  for the general "did anything else change" question, not just the canary's specific bytes.
  **TO VERIFY**: still a single-host (this machine, this macOS build) empirical result for the exact
  addresses/ranges measured; `tests/port/run_arena_stress.sh` exists to keep re-checking the 100/100
  claim, but the zerofill-section mechanism itself (not a specific address) is what makes this design
  safe by construction rather than by measurement, unlike the rejected one.
- **Rule, going forward (`include/port/arena.h`)**: no game-visible pointer may ever point at host
  `malloc()`'d memory or at the host main thread's stack — neither is guaranteed to be inside the
  window, and (see above) trying to force a fixed VM reservation near them is how the `OVERWRITE`
  mistake happened in the first place. Game code (Phase 4's main loop, and anything before it that
  needs to be a real, dereferenceable pointer, e.g. `GCPTR` results) must run on a thread created with
  `CreateArenaThread`, whose stack is carved from the arena, never the process's own main thread.

## 3. Strategy D: compressed 32-bit handles relative to a host base

**Design**: a fixed 4 GiB-window arena embedded in the executable's own image at host address `A`
(`src/port/arena.cpp`; see section 2 for why it is embedded rather than reserved at runtime), and
`g_base = A - 0x80000000`. Every on-disc pointer field becomes a `Ptr32<T>`
(`include/port/ptr32.h`): a 4-byte handle `h = host_addr - g_base`. As long as everything the game
can ever point at — the exe image and the arena (**not** the host main stack or host `malloc()`
memory — section 2's rule) — lives in the 4 GiB window `[g_base, g_base + 0x100000000)`, `h` looks
exactly like the GameCube address the field would have held: same sign bit (so
`(int) d->pClr < 0` keeps meaning "already relocated"), same `0x80000000-0x817FFFFF`
`SysMem`/`VALID_PTR` range for anything actually inside the arena (which is where `SysMem`'s
addresses live). `GCPTR(h)` recovers a real, dereferenceable host pointer via
`g_base + h`. Typed `T*` members of runtime-only structs (never read from or written to disc) stay
native 8-byte pointers — only on-disc/relocated fields change type.

**Why not the pure alternatives**:
- **Widen every field to a real 8-byte pointer** (store the host `T*` directly): breaks the on-disc
  byte layout of every format in the table above outright — every struct changes size, and a model
  BIN or save file becomes a different, incompatible format the moment any of these fields exist in
  memory the same way they exist on disc. Since the save format must stay GameCube-compatible
  (decision below), this is a non-starter for `SAVE_DATA_HEAD` specifically, and it would need a
  wholesale disc-format-vs-memory-format split for every other one too (see the next option).
- **Never relocate; always add a per-container base at each access** (i.e., keep the file-relative
  offsets these formats already start as, and never promote them to absolute addresses at all): this
  avoids the "compress an absolute address" problem, but loses the sign-bit "already relocated"
  trick and `VALID_PTR`/`SysMem`-range compatibility entirely (an offset is never in that range), and
  every dereference site would need to know which container's base to add — there is no `g_base`-style
  single answer, since e.g. two different `cModelData` instances have two different bases. It also
  does not help the formats that are not relative to a *single* container at all (SAT block links,
  motion curve tables indexed from a shared pool). It would need essentially the same amount of
  site-by-site rewriting as strategy D, for a worse result.
- **Full 64-bit widening with separate on-disc/in-memory struct types** (serialize/deserialize at
  load/save time instead of relocating in place): doubles every format's type definitions, risks the
  two drifting apart, and every relocator function (the table above) becomes a real (de)serializer
  instead of a few pointer arithmetic lines — much larger surface, and still needs a GameCube-
  compatible on-disc representation for the save format, so the "disc struct" half of the split would
  end up being exactly strategy D's `Ptr32<T>` layout anyway.

Strategy D keeps every format's on-disc byte layout untouched (a `Ptr32<T>` is exactly 4 bytes,
trivially copyable, in exactly the field position the original 32-bit pointer was), keeps the
sign-bit/`VALID_PTR`/`SysMem`-range tricks working unmodified, and needs no host-side arena beyond a
single fixed reservation and a runtime `g_base`. Its cost is entirely engineering, not architecture:
a macOS-specific arena reservation (section 2, done) and — separately from `Ptr32<T>` fields — every
existing `(u32)`/`(int)`/`(void*)` cast between a pointer and a 32-bit integer elsewhere in the tree
needs the same host-vs-original-width treatment. That second cost is why option (ii) exists.

## 4. Decisions taken

- **Option (ii): a build-time cast rewriter**, not hand-editing the original tree's casts. The
  ~1,693 direct pointer<->integer casts (`PL_ARC_PTR`/`ROOM_ARC_PTR`/`FlagChk` family macros account
  for 88.5% of the ~14,741 total sites measured with `-fms-extensions`) stay exactly as they are in
  `git`; a build step run only for the host CMake targets generates a rewritten copy under
  `build-pc/gen/` with `#line` pointing back at the real source, so every diagnostic and debugger
  session still reports the vendor's file/line. Section 6 has the design.
- **GameCube-compatible save format.** `SAVE_DATA_HEAD` and everything it points at (section 1) keep
  their exact on-disc byte layout — a save file produced or read by the host port must be
  byte-identical to what the original GameCube build would produce/accept (modulo Phase 3's
  endianness swap, which is a separate, already-planned step). `Ptr32<T>` is what makes this possible
  without a disc-format/memory-format split: the field's bytes on disc and the field's bytes in a
  live `Ptr32<T>` are the same 4 bytes, interpreted the same way (a `g_base`-relative handle, not a
  real pointer), so there is no conversion step needed purely to change struct layout — only the
  usual relocate/unrelocate the vendor's own `calcAddr`/`calcOffset` functions already do.

## 5. Steps 1-3 (done)

### Step 1 — `s32`/`u32` as the host's 4-byte `int`/`unsigned int`

`include/types.h`, `include/dolphin/types.h`: under `TARGET_PC` *and* a new CMake option
`RE4_U32_32` (default **OFF**), `s32`/`u32` become `int`/`unsigned int` instead of GameCube `long`
(4 bytes there; `long` is 8 bytes on this LP64 host, so every struct with an `s32`/`u32` field
currently has the wrong layout, silently — no compile error catches it without this switch).
`s64`/`u64` (`long long`) are already 4/8-consistent both ways; `f32`/`f64`/`BOOL` too. Both headers
are edited identically (a TU that includes both, most of the game does, would otherwise get a
conflicting-typedef error).

Off by default because most of the tree does not build yet with a 4-byte `s32`/`u32` — turning it on
today does not fix anything by itself, it only makes the *next* problem (every pointer<->`s32`/`u32`
cast, now genuinely narrowing on a 64-bit host) visible, which is exactly what it is for: measuring
where step 4+ (the rewriter, `Ptr32<T>` field-by-field conversion) has to go.

**Acceptance, measured**:
- `RE4_U32_32=OFF` (default): `re4_game_core`/`re4_game_all`/`re4_rel_all` unchanged from before this
  step — 5/7, 258/293, 268/305 respectively (docs/port-phase1-errors.md).
- `RE4_U32_32=ON`: `re4_game_all` 173/293 units newly fail (1,506 error lines, 1,436 of them "cast
  from pointer to smaller type"), `re4_rel_all` 289/305 newly fail (4,061 error lines, 4,016 the same
  category) — the same order of magnitude and same dominant category the planner's
  `-fms-extensions` measurement found tree-wide, confirming this is the same problem, not a
  measurement artifact of this narrower (`src/game`+REL only, not `src/lib`) sweep.
- Remote: 115/115 OK, `asmcheck.py --all` TOTAL 231 (the default-OFF build is what ships; ON is a
  measurement tool, not linked into anything).

### Step 2 — `include/port/ptr32.h`

`TARGET_PC`-only (the header `#error`s otherwise — it has no meaning for the original target). Under
namespace `re4_port`:
- `extern std::uintptr_t g_base;` — 0 until `InitArena()` (step 3) sets it.
- `GC32(T* p)` — host pointer -> handle. `nullptr` -> 0. Debug builds `assert` the pointer is inside
  the 4 GiB window (`g_base <= addr < g_base + 2^32`); a pointer outside it would silently alias a
  different handle, corrupting whatever on-disc format it's being written into.
- `GCPTR<T>(std::uint32_t h)` — handle -> host pointer. `0` -> `nullptr`.
- `Ptr32<T>` — the 4-byte field type: implicit to/from `T*` (so existing field reads/writes need no
  syntax change), explicit-only to the raw handle (`std::uint32_t`/`int`, since the raw value is
  meaningless without `g_base`), `operator->`, `operator[]`. `static_assert`ed `sizeof == 4` and
  `std::is_trivially_copyable`.

**Acceptance**: `tests/port/test_ptr32.cpp` (ctest `test_ptr32`) — round trip (`GCPTR(GC32(p)) == p`),
`NULL` round trip, `(int) GC32(arena) < 0` (the arena's own base looks like a negative/high-bit
GameCube address), `sizeof(Ptr32<T>) == 4`, `is_trivially_copyable`, plus `Ptr32<T>`'s `->`/`[]`/
implicit-conversion behaviour on a small linked-list-shaped fixture. **Passed** (`ctest`: 1/1 this
test, 0.01 s).

### Step 3 — the host arena

`src/port/arena.cpp` (new `re4_port` static library target), revised after the `VM_FLAGS_OVERWRITE`
mistake (section 2): no runtime reservation. `static char s_arena[1<<30]` lives in a `,zerofill`
Mach-O section (`__DATA,__re4arena`) that is part of the executable image itself. `InitArena()` just
computes `g_base = (uintptr_t) s_arena - 0x80000000` and asserts (abort if not) that the exe image
(a `.bss` probe and a code probe in this TU today; Phase 4's linked `main.cpp` `Global` once it
exists) and both ends of the arena are inside the resulting window — it allocates nothing and can
fail nothing except those assertions. `GetArenaBase()`/`GetArenaSize()` expose the arena's bounds for
carving out sub-allocations; `CreateArenaThread(offset, size, start, arg)` starts a detached pthread
whose stack is carved from the arena via `pthread_attr_setstack`, the only supported way to run game
code (section 2's rule) since the process's own main thread stack is not guaranteed inside the
window. Because the arena's address now comes from wherever ASLR slid the exe image (not a fixed
request), `g_base` varies run to run — it was a constant `0xA0000000` under the old (rejected)
design; under this one a fresh run measured `g_base = 0x8243c000` instead, and that is expected, not
a regression (the resulting window still needs to cover exe+arena either way, and `InitArena()`'s own
assertions are what guarantee that per-run, not a fixed constant).

**Acceptance**: `tests/port/test_arena.cpp` (ctest `test_arena`) — single-run `InitArena()` succeeds,
`g_base` non-zero; a `malloc()`'d canary buffer written before `InitArena()`, checked byte-for-byte
after every page of the arena is written to (the direct rebuttal to the `OVERWRITE` mistake: this
check would have failed under the old design, in the ~74% of runs where it silently clobbered live
malloc bookkeeping); an arena-backed thread (`CreateArenaThread`) whose stack address is confirmed
inside the window; an idempotency check (`InitArena()` called twice leaves `g_base` unchanged).
**Passed.** `tests/port/run_arena_stress.sh` (not a ctest — a 100x-slower stress run, run by
hand/CI periodically): runs the built `test_arena --once` binary as 100 independent fresh processes
and tallies exit codes. **Run now: 100/100 ok, 0 failed.** A separate, manual `vmmap` before/after
comparison of a process that writes every arena page (not part of the automated tests: exploratory,
recorded here for the record) showed zero difference in every other mapping — same region count
(334), no address/size changes outside the arena's own region.

Remote (both steps): pushed to `port/wip-phase1`(see report for the actual verification branch),
Docker rebuild clean, `dtk shasum` 115/115 OK, `asmcheck.py --all` TOTAL 231 unchanged (`src/port/`,
`tests/port/`, and the `CMakeLists.txt`/`include/types.h`/`include/dolphin/types.h` edits are either
new files outside `configure.py`'s tree entirely or `#ifdef TARGET_PC`/`RE4_U32_32`-gated with the
`#else` branch byte-identical to before).

## 6. Steps not started

Step 4 (per the planner; not begun): convert the on-disc struct fields in the section 1 table to
`Ptr32<T>` one format at a time, each with its own `RE4_U32_32`-style rollout and acceptance check.
Later steps (link the rewritten casts into the real build, wire `InitArena()` into an actual startup
path, endianness (Phase 3), REL loading (Phase 4)) are unscoped until step 4 lands.

## 7. Build-time cast rewriter — design (not implemented yet)

Goal: every direct pointer<->integer cast in the original tree (the ~1,693 measured, plus whatever
step 4's `Ptr32<T>` conversions do not absorb) needs to become `GC32`/`GCPTR` under `TARGET_PC`
*without* the original tree's text changing — the same "bytes never change, edit generates a copy"
shape the REL/DOL build already has zero tolerance for deviating from, just applied to a build
artifact instead of a checked-in file.

**Mechanism**: a `libTooling`-based clang tool (not `-ast-dump=json` text-scraping — `libTooling`
gives a real AST with source locations and lets a `MatchFinder` target exactly
`castExpr(hasCastKind(CK_PointerToIntegral))` and `castExpr(hasCastKind(CK_IntegralToPointer))`,
which is a much smaller and more precise net than grepping for `(u32)`/`(void*)` text patterns — the
1,693 count already comes from a real cast-kind measurement, not text matching, so the rewriter
should use the same query). Run once per source file that CMake is about to compile for a host
target (`re4_game_core`/`re4_game_all`/`re4_rel_all`, and later whichever `Ptr32<T>`-converted units
need it), *before* the real compile step:
1. Parse the file with the exact same `-D`/`-I` flags CMake would give clang for it (both tools share
   `compile_commands.json`, already generated — `CMAKE_EXPORT_COMPILE_COMMANDS`, Phase 1).
2. For each matched cast, replace `(int) p` / `(T*) x` with `re4_port::GC32(p)` /
   `re4_port::GCPTR<T>(x)` (or leave it alone if it is already inside a `Ptr32<T>`-typed context,
   once step 4 exists — avoiding a double-conversion).
3. Emit the rewritten text to `build-pc/gen/<same relative path>`, with a `#line 1 "<original absolute
   path>"` at the top and a `#line N "<original absolute path>"` reinserted after every edit that
   changed the line count, so every diagnostic and every debugger `file:line` reports the real
   source, never the generated copy.
4. CMake compiles `build-pc/gen/<path>` instead of the original, for host targets only; `configure.py`
   never sees `build-pc/`, so the original build is untouched by construction, not by convention.

**Wiring into CMake**: a custom command per source file (`add_custom_command(OUTPUT
build-pc/gen/<path> ... DEPENDS <original path> re4_cast_rewriter)`), generated the same way
`cmake/gen_rel_sources.py` generates the REL source list today — a small Python (or CMake
`file(GENERATE)`) step that, for every source already in `RE4_GAME_CORE_SOURCES`/
`RE4_GAME_ALL_SOURCES`/`RE4_REL_ALL_SOURCES`, adds the matching rewrite rule and swaps the compiled
path. The rewriter tool itself needs building first (either vendored via `FetchContent` of upstream
LLVM/clang, which is a heavy dependency for this repo, or built against the system's `libclang`/
`clang-cli` if Xcode's toolchain exposes enough of libTooling — **TO VERIFY** whether Apple's
bundled clang ships the libTooling headers/libs needed, or whether a separate LLVM checkout is
required; this is the first thing to resolve before writing the tool).

**Macros (`PL_ARC_PTR`, `ROOM_ARC_PTR`, `FlagChk`/`On`/`Off`, `SS_ARC_PTR`, `VALID_PTR`,
`FLAG_WORD_VAR`, `MOT_SET`, ...): not rewritten by the tool.** They live in headers, which have no
`#line`-back-to-original-file problem (a header is its own file; rewriting it in place *is* editing
the real source location) and account for 88.5% of the measured sites — one `TARGET_PC` branch per
macro converts far more sites per edit than the rewriter's per-call-site approach, and can be
verified by hand the way every other Phase 1/2 header fix has been (this is the planner's step 4,
not this rewriter). The rewriter is for the remaining ~1,693 *direct* casts that are not behind a
macro at all — inline `(u32) p` / `(void*) x` in game logic.

**Hardest parts, flagged**:
- **Telling "on-disc field access" apart from "ordinary integer math that happens to touch a
  pointer-sized value" automatically.** A cast-kind match alone does not know whether `(u32) p` is
  about to be written into a `Ptr32<T>`-backed struct field (wants `GC32`) or is, say, hashing a
  pointer for a debug log (wants a real integer identity, not a `g_base`-relative one — though on
  this host those are the same up to the `g_base` offset, so it is not *wrong*, just wasted
  complexity). Getting this right needs real type information at the assignment/argument site, not
  just the cast node — likely `hasParent()`/`hasAncestor()` matchers tied to the `Ptr32<T>` field's
  declared type once step 4 exists, with anything left over defaulting to a plain, honest
  `GC32`/`GCPTR` (correct either way, since both meanings resolve through the same `g_base`).
- **`#line` correctness across macro-expanded casts.** A cast inside a macro invocation (there will
  be some even after the header-macro carve-out above, since e.g. `PL_ARC_PTR`'s expansion itself
  contains further casts that a caller might additionally cast around) has a spelling location
  different from its expansion location; the rewriter must edit at the spelling location's line in
  the *caller's* file, not silently rewrite the macro's own definition through the expansion.
- **Idempotency and incremental builds.** The rewriter needs to be safe to run on its own already-
  generated output (a no-op) and needs correct CMake dependency edges so touching one header does not
  force-regenerate every `build-pc/gen/` file, only the ones that actually `#include` it (the same
  depfile problem `configure.py`'s own ProDG rule has, called out in CLAUDE.md's "Build" section, for
  a much smaller and slower-to-run toolchain).
- **Case where a cast result feeds both a `Ptr32<T>` field and a plain integer use in the same
  expression** (measured: some `casts_src.txt`/`u32sites_src.txt` sites chain a `PL_ARC_PTR` result
  through more than one cast before it lands anywhere) — the rewriter's replacement must compose
  (`GCPTR<T>(GC32(...))`-shaped chains do not collapse for free the way the original `(T*)(u32)`
  double-cast idiom does), or it will need a small peephole pass to fold them back down.
