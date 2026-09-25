# macOS port — handoff (2026-09-25)

Read this first when resuming the port. Then `CLAUDE.md`, `docs/port.md` (plan), and the last
sections of `docs/port-boot.md` (boot log). Branch `port/macos-arm64`, remote `fork`
(github.com/notluken/re4); never push to `origin`. HEAD at handoff: `1dc0b2f2`.

## Where it stands

- The title screen renders correctly (logo, art, START/LOAD/OPTIONS), keyboard works, the
  memory-card prompt and save/load menu work.
- New Game → debug AREA JUMP menu → room OPENING (st1 room 0x20, room_id 0x120) → `gameRoomInit`:
  REL modules `st1_0` (room) and `wep02` (Leon's handgun, id 4) link and run their prologs, Leon
  is constructed, the gameplay loop starts (footwork, IK, floor collision), room models load and the
  first real GX draw (a triangle strip) is issued.
- **Current blocker:** Aurora aborts with `invalid texture source for content hash`
  (`aurora::gx::texture::resolve_static_texture` → `hash_texture_source`), called from
  `commonModelTrans` on the first real model draw.
- Also seen: hundreds of `cLightInfo::init2() PTR ERROR` per frame once the room draws (a light
  pointer/format bug, not investigated), which makes realtime frames very slow.

## Build, run, verify

Build and run: `docs/port-boot.md` §12 (Homebrew deps, cast-rewriter tool, `build-pc-boot`).

```sh
./build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso   # from the repo root
```

Keys: Enter=Start, Z=A, X=B, C=X, V=Y, arrows. Debug env vars:

| Variable | Effect |
|---|---|
| `RE4_PORT_INPUT="300:UP,340:A,400:DOWN,440:DOWN,480:START"` | scripted presses; this one reaches room 0x120 |
| `RE4_PORT_FIXED_VI=1` | deterministic: one retrace per VIWaitForRetrace (use for scripted runs) |
| `RE4_PORT_SCREENSHOT=<png>` + `RE4_PORT_SCREENSHOT_AT_FRAME=N` | window-only capture at VI retrace N (`N ≈ 2 × input_frame + 1`); never capture the full screen |
| `RE4_PORT_TITLE_TRACE=1`, `RE4_PORT_MEM_TRACE=1` | title state machine / heap allocation logs |

Backtraces: `lldb -s <script>` (script mode) works better than `--batch` after a crash; macOS crash
reports land in `~/Library/Logs/DiagnosticReports/re4_boot-*.ips`.

Original-build byte identity, required before any commit that touches `src/` or `include/`
outside `src/port/` and `include/port/` reaches `port/macos-arm64`: push to `fork/port/wip-phase1`,
then on the x86_64 verifier:

```sh
ssh root@100.90.198.42 'cd /root/re4-work/re4 && git fetch fork && git checkout -f port/wip-phase1 && git reset --hard fork/port/wip-phase1 && docker run --rm -v "$PWD":/re4 -w /re4 -v re4-build-vol:/re4/build re4-build bash -lc "find build/G4BE08 -path \"*/obj/*\" -prune -o -name \"*.o\" -print | xargs rm -f; python3 configure.py && ninja && build/tools/dtk shasum -c config/G4BE08/build.sha1 | grep -vc \": OK\$\"; python3 tools/asmcheck.py --all | tail -3"'
```

Required: `0` non-OK lines and `asmcheck` `TOTAL 231`; then fast-forward `port/macos-arm64` and
push. Host regression checks: `build-pc` `re4_game_all -k 0` = 25 failing files (diff the list),
ctests pass, a realtime run still reaches the save menu past the card prompt.

## Settled design (don't re-litigate)

- All port code behind `#ifdef TARGET_PC`; GameCube bytes never change; don't shift physical lines
  in files with `#line` unless the verifier proves bytes unchanged.
- Memory: 4-byte `u32`; GC-faithful layout inside the executable — 16 KB lowmem at GC 0x80000000,
  game globals and REL module data after it, 2 MiB game stack, 1 GB arena last; `Ptr32<T>`
  compressed handles (always stored big-endian); every game-visible address stays in the window.
- Byte order: on-disc data stays big-endian in memory; on-disc struct fields are `BE<T>` /
  `BeVec` / `SatVec`; saves stay GameCube-compatible (GlobalWork save/load conversion not done yet).
- Casts: build-time libTooling rewriter (`tools/port/cast_rewriter`, tests in its `tests/`). Avoid
  integer round-trips of host pointers (the rewriter wraps them in `GCPTR`); use pointer arithmetic.
- Threads: all OSThreads are ucontext fibers on one host thread; VI callbacks and Aurora completions
  are delivered on the game thread; the host main thread runs Aurora's window/present loop;
  `g_gxHostMutex` serializes game GX frames against `aurora_update()` and is released during
  retrace waits; `aurora_shutdown` never runs with a frame open.
- Allocation: `operator new` routes by caller (game code lives in `__TEXT,__re4game,regular,pure_instructions`).
- REL modules: statically linked, one `ld -r` per module, data in per-module sections; host
  `OSLink` restores pristine data on a fresh link and preserves it on relink. Adding a module is one
  entry in `RE4_REL_MODULES` (CMakeLists.txt; today `st1_0 wep02`). Tools / `t_*` are out of scope.
- Sound and FMV are inert (Phase 5); Aurora changes go only as patches in `tools/port/aurora-patches/`.
- Commits: one line, no trailers, explicit `git add`; never commit `.claude/`, build dirs, `orig/`,
  screenshots or disc bytes. Don't change game control flow to work around a port bug.

## Next steps, in order

1. **Texture fatal** (`hash_texture_source`, from `commonModelTrans`): find which texture the room
   model binds — TPL descriptor/header fields not yet `BE<T>` in the room's TPLs or in
   `cModelData`'s texture table, a `GXInitTexObj` data pointer outside Aurora-readable memory, or a
   texture/palette format Aurora doesn't handle. Fix at the root.
2. **`cLightInfo::init2() PTR ERROR` spam**: light data read raw or a pointer check failing on
   host addresses; fix, then re-measure realtime frame speed.
3. **Room renders**: get Leon / level geometry on screen and take a window-only screenshot with
   `RE4_PORT_SCREENSHOT_AT_FRAME`.
4. **Remaining raw room formats** (docs/port-phase3.md table: EAT, SHD, EMI, STB, SMX, …): convert
   in one sweep; next missing REL modules (enemies of r120, st1_1..st1_3) → add to `RE4_REL_MODULES`.
5. **Layout parity debt**: `cUnit::pNext` / `cCoord::pParent` → `Ptr32<T>` sweep (root cause of
   several struct-view bugs); `cModelExt` / `MODEL_EXT` struct-view cast (docs/port-layout-parity.md,
   "Struct-view casts"); other unions of narrow fields under a wide view (listed TO VERIFY in
   docs/port-boot.md §49).
6. **Later**: GameCube-compatible save conversion, memory-card prompt text rendering (currently
   black), sound (Phase 5), FMV, CI on GitHub Actions for the byte-identity check.
