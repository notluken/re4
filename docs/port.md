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

### Phase 1 — a host build that compiles

- Clone Aurora into `../aurora`; pin a commit.
- `CMakeLists.txt` (top level, ignored by `configure.py`) building `src/game` with clang for
  `arm64-apple-macos`, `-DTARGET_PC`. Exclude the Nintendo SDK and CRI units (Aurora and host
  replacements take their place), the sound DSP driver and the asm-bodied units.
- Headers first: `include/` types (`u32`, pointer-sized fields, `#pragma pack`, MWCC/GCC extensions)
  made host-clean behind `TARGET_PC`.
- Collect the real error list; do not guess its size.

Exit: `src/game` compiles (not links) on macOS arm64; the error inventory for phases 2-3 is written down.

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
