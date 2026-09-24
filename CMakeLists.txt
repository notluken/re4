# Host build scaffolding for the macOS arm64 port (Phase 1, docs/port.md).
#
# This file is NOT read by configure.py/ninja (the matching build); it only exists so `src/game`
# can be compiled with clang for a host target, gated behind -DTARGET_PC. It never touches the
# original build's inputs (config/, build/, objdiff.json, splits.txt) and produces its own build
# tree under build-pc/ (gitignored).
#
# Usage:
#   cmake -S . -B build-pc -G Ninja
#   cmake --build build-pc --target re4_game_core          # small representative set (Phase 1 goal)
#   cmake --build build-pc --target re4_game_all -- -k 0    # every src/game unit, error inventory only
#   cmake --build build-pc --target re4_rel_all -- -k 0     # every REL module unit, error inventory only
#
# build-pc/compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS below) is for editor tooling
# (clangd) only, so `types.h`/size_t etc. resolve while editing game code; it is regenerated on
# every configure and is not meant to be committed. Point clangd at it either by opening the editor
# from a shell that has run cmake at least once (most clangd setups auto-discover
# build-pc/compile_commands.json by walking up from the file, no symlink needed), or, if your setup
# insists on a repo-root compile_commands.json, `ln -s build-pc/compile_commands.json .` (also
# gitignored: it is a build artifact, and matching-only clangd users should not see it).
cmake_minimum_required(VERSION 3.25)
project(re4-pc LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS ON)   # GCC 2.95 game code leans on GNU extensions (statement exprs, etc.)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

# Widen to every unit under src/game (used by the re4_game_all error-inventory target below);
# the small re4_game_core target always stays as its own explicit list.
option(RE4_PC_BUILD_ALL_GAME "Add re4_game_all: every src/game/*.c(pp) as its own OBJECT library, -k-style, error inventory only" ON)

# Phase 2 step 1 (docs/port-phase2.md): s32/u32 as the host's 4-byte int/unsigned int instead of
# GameCube `long` (8 bytes on this LP64 host, wrong struct layout everywhere, silently). Off by
# default: most of the tree does not build yet with a 4-byte s32/u32 (~460 TUs, until Phase 2's
# later steps land the Ptr32<T>/host-arena work this depends on). Turn on to see today's diagnostic
# set for that switch; the error-inventory targets below are still expected to fail widely with it on.
option(RE4_U32_32 "s32/u32 = 4-byte int/unsigned int under TARGET_PC (Phase 2 step 1, off by default)" OFF)

# One global setting, not a per-target list a new target can forget to opt into (docs/port-boot.md
# section 25: re4_port's own target_compile_definitions() once carried TARGET_PC but not
# RE4_U32_32, so os_thread.cpp's #include <dolphin/os/OSThread.h> silently fell back to
# dolphin/types.h's u32=`unsigned long` (8 bytes) branch while every other TU's OSThread was 4-byte
# u32/856 bytes -- an ODR violation that corrupted the scheduler's Task[] array). add_compile_
# definitions() here applies from this point in the directory scope onward, so it reaches every
# target this file defines below *and* Aurora's add_subdirectory() (RE4_BUILD_BOOT) -- verified
# intentional, not incidental: Aurora's own dolphin/types.h (a different copy, ../aurora/include/
# dolphin/types.h) has the identical #ifdef TARGET_PC branch, <stdint.h> fixed-width types, so
# without this Aurora's own compiled objects (aurora_os/aurora_gx/...) were using 8-byte u32/s32
# throughout while every game/port TU used 4-byte -- the same class of bug, one level up, at every
# Dolphin-shaped struct that crosses the game<->Aurora call boundary (GXTexObj, OSThread, ...).
add_compile_definitions(TARGET_PC)
if (RE4_U32_32)
  add_compile_definitions(RE4_U32_32)
endif()

set(RE4_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Phase 2 section 8 (docs/port-phase2.md): the build-time cast rewriter. Implied by RE4_U32_32
# (that is the config where the ~1,693 direct pointer<->integer casts actually need to become
# GC32()/GCPTR<T>() to compile at all), but can be forced independently for testing.
#
# tools/port/cast_rewriter/ is NOT built by this configure -- it needs libTooling, which Apple's
# bundled clang does not ship (verified 2026-09-24); build it once, separately, against Homebrew
# LLVM (see tools/port/cast_rewriter/CMakeLists.txt's header comment), then point
# RE4_CAST_REWRITER_BIN at the resulting binary. tools/port/rewrite_casts.py is the actual driver
# (also invokable by hand): it reads this configure's own compile_commands.json, runs the rewriter
# once per src/game source with RE4_U32_32 OFF (the cast still type-checks there -- see the
# script's own header comment for why, and docs/port-phase2.md section 8), and writes the
# rewritten copies to build-pc/gen/. This is a whole-tree regeneration step run at configure time,
# not a per-file incremental custom command yet -- see docs/port-phase2.md, "steps not started",
# for that gap.
option(RE4_REWRITE_CASTS "Run the Phase 2 cast rewriter and compile build-pc/gen/ instead of src/ for host game targets" ${RE4_U32_32})
set(RE4_CAST_REWRITER_BIN "${RE4_ROOT}/build-pc-tool/re4_cast_rewriter" CACHE FILEPATH
    "Path to the built tools/port/cast_rewriter binary (built separately, see its CMakeLists.txt)")
if (RE4_REWRITE_CASTS)
  if (NOT EXISTS ${RE4_CAST_REWRITER_BIN})
    message(WARNING "RE4_REWRITE_CASTS is on but ${RE4_CAST_REWRITER_BIN} does not exist -- "
                     "build tools/port/cast_rewriter first (see its CMakeLists.txt), or the "
                     "re4_game_all/re4_rel_all sources below fall back to the unrewritten originals.")
    set(RE4_REWRITE_CASTS OFF)
  endif()
endif()

# Matches configure.py's include_dirs (project_root/include, project_root/src,
# project_root/build/<version>/include) plus the version defines cflags_game carries.
set(RE4_GAME_INCLUDES
  ${RE4_ROOT}/include
  ${RE4_ROOT}/src
  ${RE4_ROOT}/build/G4BE08/include
)
set(RE4_GAME_DEFINES
  BUILD_VERSION=0
  VERSION_G4BE08
  NDEBUG=1
)
# TARGET_PC and (conditionally) RE4_U32_32 are global (add_compile_definitions() above), not
# listed here -- kept as a plain list of the *other* per-target defines game code needs.
# -Wno-address-of-temporary: `&((Vec){...})` compound-literal-address idiom (GNU C, several em*/wep*
# units), legal for the vendor's GCC 2.95 and alive for the length of the enclosing full expression;
# clang's default diagnostic for it is an error, not a warning, so it needs the flag, not -Wno-error.
# -mllvm -global-isel=false: Phase 2 step 5 (docs/port-phase2.md) hit a reproducible clang/LLVM
# backend crash (AArch64RegisterBankInfo::hasFPConstraints, GlobalISel's RegBankSelect pass) compiling
# src/game/cam_ctrl.cpp's CameraControl::calcAddr with RE4_U32_32 on -- confirmed a codegen-only bug,
# not a diagnostic (-fsyntax-only accepts the same source cleanly), and confirmed fixed by forcing the
# older SelectionDAG instruction selector instead. Applied to every host target so any other unit that
# hits the same GlobalISel bug is covered too, not just the one found so far.
set(RE4_GAME_OPTIONS
  -Wno-invalid-offsetof
  -Wno-narrowing
  -Wno-address-of-temporary
  -mllvm -global-isel=false
)

# --- re4_game_core: a small representative slice of src/game (Phase 1 goal: compiles, not links) ---
# One from the object chain (cCoord/cModel), one cManager<T> consumer, math (C++), the DVD loader
# and its higher-level reader, and a plain utility class. No src/game/*.c file: every single one is
# a newlib libc reimplementation (see RE4_LIBC_UNITS below) and the port uses the system libc, not a
# C representative from this tree.
set(RE4_GAME_CORE_SOURCES
  src/game/model.cpp        # object chain: cCoord, cModel, cModelInfo, PartsMgr/ModInfoMgr
  src/game/light.cpp        # cManager<T> consumer (light pool)
  src/game/math_sub.cpp     # math (C++)
  src/game/dvd.cpp          # DVD loader
  src/game/read.cpp         # higher-level file reader built on dvd.cpp
  src/game/cString.cpp      # plain utility class
  src/game/item.cpp         # object pool / manager consumer
)

add_library(re4_game_core OBJECT ${RE4_GAME_CORE_SOURCES})
target_include_directories(re4_game_core PRIVATE ${RE4_GAME_INCLUDES})
target_compile_definitions(re4_game_core PRIVATE ${RE4_GAME_DEFINES})
target_compile_options(re4_game_core PRIVATE ${RE4_GAME_OPTIONS})

# --- re4_game_all: every src/game/*.cpp as its own OBJECT library ---
# Not meant to succeed yet: build with `-- -k 0` (Ninja) so one unit's errors don't stop the rest,
# and read the diagnostics to write docs/port-phase1-errors.md. `game/memset_2.cpp` and
# `game/yz2asm.cpp` are the two Capcom whole-function-asm units (tools/asmcheck.py ASM_BODIED);
# they are real vendor assembly and not expected to translate, so they are skipped here too.
#
# Every src/game/*.c file (not *.cpp) is excluded outright, not attempted: each one opens with a
# "newlib 1.8.2 libc/..." provenance comment (printf.c, fprintf.c, sprintf.c, sscanf.c, memcpy.c,
# strlen.c, math_support.c built for vfprintf's float conversion, ...) -- the game's own reimplementation
# of the C runtime against a custom newlib_stdio.h/_reent/FILE layout. The port uses the host's
# system libc instead of building any of this; see docs/port.md.
if (RE4_PC_BUILD_ALL_GAME)
  file(GLOB RE4_GAME_ALL_SOURCES CONFIGURE_DEPENDS
    ${RE4_ROOT}/src/game/*.cpp
  )
  list(FILTER RE4_GAME_ALL_SOURCES EXCLUDE REGEX ".*/(memset_2|yz2asm)\\.cpp$")

  # Per-file incremental cast rewriting: one add_custom_command per source (DEPENDS the source and
  # the rewriter binary), not a single whole-tree custom target -- editing one file only
  # regenerates that file's build-pc/gen/ copy, not all 293. This bootstraps cleanly within a
  # single configure: CMAKE_EXPORT_COMPILE_COMMANDS writes compile_commands.json during the
  # Generate step, which happens before ninja ever runs a build command, so the `-p` compilation
  # database the rewriter needs already exists by the time any of these rules execute -- no
  # two-pass workflow needed any more (superseded the previous re4_rewrite_casts custom target).
  if (RE4_REWRITE_CASTS)
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE RE4_SYSROOT
                     OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND /opt/homebrew/opt/llvm/bin/clang -print-resource-dir
                     OUTPUT_VARIABLE RE4_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

    set(RE4_GAME_ALL_SOURCES_REWRITTEN "")
    foreach(_src IN LISTS RE4_GAME_ALL_SOURCES)
      file(RELATIVE_PATH _rel ${RE4_ROOT} ${_src})
      set(_gen "${CMAKE_BINARY_DIR}/gen/${_rel}")
      # `re4_cast_rewriter`'s own exit code reflects whether the *parse* it ran to build the AST
      # had zero diagnostics -- not whether rewriting itself succeeded (it always writes -o as long
      # as parsing got far enough to build an AST at all, errors and all: see
      # EndSourceFileAction()). Real, pre-existing errors in the tree (unrelated undeclared
      # identifiers, casts to plain `int`/`short` that Sema itself won't turn into a rewritable
      # AST node even at RE4_U32_32=OFF -- docs/port-phase2.md section 9's newest finding) are
      # expected and do not mean this step failed, so success here is "did -o get written",
      # not the tool's own exit code.
      add_custom_command(
        OUTPUT ${_gen}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/gen/src/game"
        # -URE4_U32_32 always, regardless of what this build tree's own compile_commands.json bakes
        # in (build-pc-u32on's does, since that is the point of that tree): the rewriter needs
        # RE4_U32_32 OFF to see the cast AST nodes it exists to rewrite at all -- with it ON, the
        # narrowing pointer<->integer cast is a hard Sema error and clang builds a RecoveryExpr
        # instead of a real CStyleCastExpr, so the rewriter silently sees (and rewrites) almost
        # nothing (measured: running this rule against build-pc-u32on's own database without the
        # -U produced unrewritten-looking output and a 1535-error aggregate instead of ~170 --
        # docs/port-phase2.md section 9). A trailing -U after -D undefines it on the same command
        # line regardless of argument order in the base compile command.
        # -fms-extensions, rewriter parse only, never the real compile: makes a narrowing
        # pointer<->smaller-int C-style cast a warning instead of a hard error in clang (the same
        # flag the original tree-wide 1,693-site cast measurement used), so a CStyleCastExpr node
        # actually gets built for `(int) ptr`/`(short) ptr`-shaped casts (the literal-host-type gap:
        # `int` is always 4 bytes regardless of RE4_U32_32, so -URE4_U32_32 alone does not help
        # these). -Wno-error keeps it a warning even if some other flag upstream promotes warnings.
        COMMAND bash -c "${RE4_CAST_REWRITER_BIN} -p ${CMAKE_BINARY_DIR} ${_src} -o ${_gen} --report ${_gen}.report.txt --resource-dir=${RE4_CLANG_RESOURCE_DIR} --extra-arg=-isysroot --extra-arg=${RE4_SYSROOT} --extra-arg=-URE4_U32_32 --extra-arg=-fms-extensions --extra-arg=-Wno-error --extra-arg=-ferror-limit=0 ; test -f ${_gen}"
        DEPENDS ${_src} ${RE4_CAST_REWRITER_BIN} ${CMAKE_BINARY_DIR}/compile_commands.json
        COMMENT "cast-rewrite ${_rel}"
        VERBATIM
      )
      list(APPEND RE4_GAME_ALL_SOURCES_REWRITTEN ${_gen})
    endforeach()
    set(RE4_GAME_ALL_SOURCES ${RE4_GAME_ALL_SOURCES_REWRITTEN})
  endif()

  add_library(re4_game_all OBJECT ${RE4_GAME_ALL_SOURCES})
  target_include_directories(re4_game_all PRIVATE ${RE4_GAME_INCLUDES})
  target_compile_definitions(re4_game_all PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_game_all PRIVATE ${RE4_GAME_OPTIONS})
  if (RE4_REWRITE_CASTS)
    # Forced include instead of patching an #include into every generated copy: simplest way to
    # get re4_port::GC32/GCPTR/Ptr32<T> declared in every rewritten TU regardless of whether its
    # original #include set happened to pull in ptr32.h transitively (measured gap, 8 sites,
    # docs/port-phase2.md section 9).
    target_compile_options(re4_game_all PRIVATE -include ${RE4_ROOT}/include/port/ptr32.h)
  endif()
endif()

# --- re4_rel_all: every REL module unit (em*/pl*/wep*/st*/Sscrn/Tools/t_*, plus the shared
# st/, wep/ sources), one OBJECT library, again for the error inventory only ---
# The source list comes from cmake/gen_rel_sources.py, which reads config/G4BE08/{config.yml,
# modules.py} the same way configure.py's own REL object loop does (module list + unit boundaries),
# without needing the toolchain/disc images configure.py itself requires. It also prints, per
# source, the first module that owns it, so a shared source (st/em_wrap.cpp, wep/pl_handgun.cpp,
# ...) gets a real -DREL_MODULE=<mod> instead of a placeholder (it is only compiled once here,
# where configure.py compiles it once per owning module -- fine for an error inventory, not for
# matching). Every REL unit is included: the "Tools" module's own units (Tools/t_prim.cpp,
# Tools/t_util.cpp, Tools/tools.cpp) used to collide on a case-insensitive filesystem with three
# same-named, differently-bodied files under src/tools/ (the shared debug-editor sources); the
# Tools-module files now live in src/tools_mod/ (git mv + a matching config/G4BE08/modules.py
# source-override update, unit names unchanged), so there is no longer a collision -- see
# docs/port.md, "the Tools/tools APFS fold".
option(RE4_PC_BUILD_ALL_REL "Add re4_rel_all: every REL module unit as its own OBJECT library, -k-style, error inventory only" ON)
if (RE4_PC_BUILD_ALL_REL)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${RE4_ROOT}/cmake/gen_rel_sources.py
    WORKING_DIRECTORY ${RE4_ROOT}
    OUTPUT_VARIABLE RE4_REL_SOURCES_RAW
    ERROR_VARIABLE RE4_REL_SOURCES_SKIPPED
    RESULT_VARIABLE RE4_REL_GEN_RESULT
  )
  if (NOT RE4_REL_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "cmake/gen_rel_sources.py failed: ${RE4_REL_SOURCES_SKIPPED}")
  endif()
  if (RE4_REL_SOURCES_SKIPPED)
    message(STATUS "re4_rel_all: ${RE4_REL_SOURCES_SKIPPED}")
  endif()

  string(REPLACE "\n" ";" RE4_REL_SOURCE_LINES "${RE4_REL_SOURCES_RAW}")
  set(RE4_REL_ALL_SOURCES "")
  foreach(_line IN LISTS RE4_REL_SOURCE_LINES)
    if (_line STREQUAL "")
      continue()
    endif()
    string(FIND "${_line}" " " _sp)
    string(SUBSTRING "${_line}" 0 ${_sp} _relpath)
    math(EXPR _modstart "${_sp} + 1")
    string(SUBSTRING "${_line}" ${_modstart} -1 _mod)
    list(APPEND RE4_REL_ALL_SOURCES "${RE4_ROOT}/${_relpath}")
    set_property(SOURCE "${RE4_ROOT}/${_relpath}" PROPERTY COMPILE_DEFINITIONS "REL_MODULE=${_mod}")
  endforeach()

  add_library(re4_rel_all OBJECT ${RE4_REL_ALL_SOURCES})
  target_include_directories(re4_rel_all PRIVATE ${RE4_GAME_INCLUDES})
  target_compile_definitions(re4_rel_all PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_rel_all PRIVATE ${RE4_GAME_OPTIONS})
endif()

# --- re4_port: the host-side Phase 2 support library (include/port/, src/port/) ---
# Not part of re4_game_core/re4_game_all/re4_rel_all (those compile the vendor's own tree, unmodified
# except by #ifdef TARGET_PC); this is new, port-only code with no original-target counterpart.
add_library(re4_port STATIC
  src/port/arena.cpp
  src/port/alloc.cpp
  src/port/dvd_root.cpp
  src/port/os_thread.cpp
)
target_include_directories(re4_port PUBLIC ${RE4_GAME_INCLUDES})
# TARGET_PC/RE4_U32_32 are global (add_compile_definitions() near the top of this file) -- see
# docs/port-boot.md section 25 for why a per-target copy of this list is exactly the bug class
# that global directive exists to make impossible. Force-included cross-TU layout guard
# (include/port/layout_asserts.h) for the same reason: this is exactly the library whose TUs
# (os_thread.cpp) embed/touch OSThread/OSContext/TASK et al., and exactly the target that once
# silently disagreed with the game side about their size.
target_compile_options(re4_port PUBLIC -include ${RE4_ROOT}/include/port/layout_asserts.h)
find_package(Threads REQUIRED)
target_link_libraries(re4_port PUBLIC Threads::Threads)

# --- tests/port: host unit tests for re4_port (Phase 2 step 2/3, docs/port-phase2.md) ---
option(RE4_PC_BUILD_TESTS "Add the tests/port/ host unit tests (ctest)" ON)
if (RE4_PC_BUILD_TESTS)
  # src/port/lowmem.cpp listed FIRST in every one of these (docs/port-boot.md section 28): ld orders
  # same-segment sections by first appearance among the *directly listed* link inputs, so this keeps
  # `__re4low` ahead of these tests' own ordinary `.data`/`.bss` the same way it must be in re4_boot.
  add_executable(test_ptr32 src/port/lowmem.cpp tests/port/test_ptr32.cpp)
  target_link_libraries(test_ptr32 PRIVATE re4_port)
  add_test(NAME test_ptr32 COMMAND test_ptr32)

  add_executable(test_arena src/port/lowmem.cpp tests/port/test_arena.cpp)
  target_link_libraries(test_arena PRIVATE re4_port)
  add_test(NAME test_arena COMMAND test_arena)

  add_executable(test_be src/port/lowmem.cpp tests/port/test_be.cpp)
  target_link_libraries(test_be PRIVATE re4_port)
  add_test(NAME test_be COMMAND test_be)
endif()

# --- re4_port_static_asserts: offsetof() checks for Phase 2 step 5's Ptr32<T>-converted on-disc
# structs (docs/port-phase2.md), generated from their own "// 0xNN" comments ---
# Only meaningful with RE4_U32_32 ON: a Ptr32<T> field is always 4 bytes, but the *other* plain u32
# fields interleaved in these same structs are only 4 bytes (matching the documented GameCube
# offsets) when RE4_U32_32 is also on; with it off they are still the host's 8-byte `long`, so the
# struct layout is not yet the on-disc one and these asserts would fail immediately -- correctly, not
# spuriously, but there is no host build config where they should run except RE4_U32_32=ON.
if (RE4_U32_32)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/gen/port_static_asserts.cpp
    COMMAND ${Python3_EXECUTABLE} ${RE4_ROOT}/tools/port/gen_static_asserts.py
            > ${CMAKE_BINARY_DIR}/gen/port_static_asserts.cpp
    DEPENDS ${RE4_ROOT}/tools/port/gen_static_asserts.py
            ${RE4_ROOT}/include/model.h ${RE4_ROOT}/include/tpl.h ${RE4_ROOT}/include/cam_ctrl.h
            ${RE4_ROOT}/include/game.h ${RE4_ROOT}/include/room_jmp.h ${RE4_ROOT}/include/atari.h
            ${RE4_ROOT}/src/game/mes.cpp
    VERBATIM
  )
  add_executable(re4_port_static_asserts ${CMAKE_BINARY_DIR}/gen/port_static_asserts.cpp
    tests/port/gx_layout_asserts.cpp)
  target_include_directories(re4_port_static_asserts PRIVATE ${RE4_GAME_INCLUDES})
  target_compile_definitions(re4_port_static_asserts PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_port_static_asserts PRIVATE ${RE4_GAME_OPTIONS})
  add_test(NAME re4_port_static_asserts COMMAND re4_port_static_asserts)
endif()

# --- re4_boot: "first boot" milestone (docs/port.md) -- one executable, real host entry point,
# src/game minus cmake/boot_exclude.txt's still-failing units, Aurora for GX/PAD/DVD/CARD/VI/OS,
# a stub library for whatever the linker still can't find. Aim: LINK, then run once against
# orig/G4BE08/files and see where it dies (docs/port-boot.md's call chain). Not wired into the
# default build (RE4_PC_BUILD_ALL_GAME's re4_game_all etc. above are unaffected either way):
# opt-in via RE4_BUILD_BOOT, off by default (pulls in Aurora's own dependency tree -- Dawn/nod/
# SDL3 -- which is heavy for a target most of this repo's work does not need).
option(RE4_BUILD_BOOT "Add the re4_boot executable (first-boot milestone, docs/port.md)" OFF)
if (RE4_BUILD_BOOT)
  if (NOT RE4_U32_32)
    message(FATAL_ERROR "RE4_BUILD_BOOT needs RE4_U32_32=ON (the boot-path units only compile with it)")
  endif()

  set(RE4_AURORA_DIR "${RE4_ROOT}/../aurora" CACHE PATH "Path to the Aurora checkout (docs/port.md Phase 1)")
  if (NOT EXISTS "${RE4_AURORA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "RE4_BUILD_BOOT: ${RE4_AURORA_DIR} is not an Aurora checkout")
  endif()
  set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)

  # Apply this repo's Aurora patches (tools/port/aurora-patches/*.patch) to the checkout's
  # working tree, idempotently (git apply --reverse --check tells us a patch is already in),
  # every configure. Never committed into Aurora's own history -- see the port rules and the
  # patch files' own header comments for why each one exists.
  file(GLOB RE4_AURORA_PATCHES "${RE4_ROOT}/tools/port/aurora-patches/*.patch")
  list(SORT RE4_AURORA_PATCHES)
  foreach(_patch IN LISTS RE4_AURORA_PATCHES)
    execute_process(
      COMMAND git apply --reverse --check "${_patch}"
      WORKING_DIRECTORY "${RE4_AURORA_DIR}"
      RESULT_VARIABLE _patch_already_applied
      OUTPUT_QUIET ERROR_QUIET
    )
    if (NOT _patch_already_applied EQUAL 0)
      message(STATUS "RE4_BUILD_BOOT: applying Aurora patch ${_patch}")
      execute_process(
        COMMAND git apply "${_patch}"
        WORKING_DIRECTORY "${RE4_AURORA_DIR}"
        RESULT_VARIABLE _patch_apply_result
      )
      if (NOT _patch_apply_result EQUAL 0)
        message(FATAL_ERROR "RE4_BUILD_BOOT: failed to apply Aurora patch ${_patch} to ${RE4_AURORA_DIR}")
      endif()
    endif()
  endforeach()

  add_subdirectory("${RE4_AURORA_DIR}" aurora_build EXCLUDE_FROM_ALL)

  # cmake/boot_exclude.txt: one path (relative to repo root) per line, blank lines and #-comments
  # ignored -- units that do not yet compile with RE4_U32_32=ON, reviewed and categorized by why in
  # the file itself (docs/port-phase2.md section 9/10's error breakdown), excluded rather than
  # hacked around per the coordinator's instruction.
  file(STRINGS "${RE4_ROOT}/cmake/boot_exclude.txt" RE4_BOOT_EXCLUDE_RAW)
  set(RE4_BOOT_EXCLUDE "")
  foreach(_line IN LISTS RE4_BOOT_EXCLUDE_RAW)
    string(STRIP "${_line}" _line)
    if (_line STREQUAL "" OR _line MATCHES "^#")
      continue()
    endif()
    list(APPEND RE4_BOOT_EXCLUDE "${RE4_ROOT}/${_line}")
  endforeach()

  set(RE4_BOOT_SOURCES ${RE4_GAME_ALL_SOURCES}) # already rewritten (build-pc/gen/...) when
                                                 # RE4_REWRITE_CASTS is on, which RE4_U32_32=ON implies
  list(REMOVE_ITEM RE4_BOOT_SOURCES ${RE4_BOOT_EXCLUDE})
  # RE4_GAME_ALL_SOURCES holds gen/ paths, but boot_exclude.txt is written in terms of the real
  # src/game/ paths (reviewable without knowing about build-pc/gen/); also strip by that name.
  set(RE4_BOOT_SOURCES_FILTERED "")
  foreach(_src IN LISTS RE4_BOOT_SOURCES)
    get_filename_component(_name "${_src}" NAME)
    set(_excluded FALSE)
    foreach(_ex IN LISTS RE4_BOOT_EXCLUDE)
      get_filename_component(_exname "${_ex}" NAME)
      if (_name STREQUAL _exname)
        set(_excluded TRUE)
      endif()
    endforeach()
    if (NOT _excluded)
      list(APPEND RE4_BOOT_SOURCES_FILTERED "${_src}")
    endif()
  endforeach()

  add_library(re4_boot_game OBJECT ${RE4_BOOT_SOURCES_FILTERED})
  target_include_directories(re4_boot_game PRIVATE ${RE4_GAME_INCLUDES} ${RE4_AURORA_DIR}/include)
  target_compile_definitions(re4_boot_game PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_boot_game PRIVATE ${RE4_GAME_OPTIONS}
    "SHELL:-include ${RE4_ROOT}/include/port/ptr32.h"
    "SHELL:-include ${RE4_ROOT}/include/port/layout_asserts.h")
  # main.cpp's own `main()` becomes `main_game()` at compile time for this target only (src/port/
  # boot_main.cpp is re4_boot's real host entry point) -- a symbol rename via -D, not a source edit,
  # so re4_game_all's own copy of the same file (error-inventory target, unaffected) still has a
  # plain `main()`.
  set_source_files_properties("${CMAKE_BINARY_DIR}/gen/src/game/main.cpp" PROPERTIES
    COMPILE_DEFINITIONS "main=main_game")

  add_library(re4_boot_stubs STATIC
    src/port/stubs/generated_c_stubs.cpp
    src/port/stubs/generated_cpp_stubs.cpp
    src/port/stubs/manual_stubs.cpp
    src/port/stubs/host_new_delete.cpp
  )
  target_include_directories(re4_boot_stubs PRIVATE ${RE4_GAME_INCLUDES} ${RE4_AURORA_DIR}/include)
  target_compile_definitions(re4_boot_stubs PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_boot_stubs PRIVATE ${RE4_GAME_OPTIONS})

  # src/port/lowmem.cpp and src/port/game_stack.cpp listed FIRST (docs/port-boot.md section 28) --
  # ld orders same-segment sections by first appearance among the *directly listed* link inputs, so
  # this keeps `__re4low` (then `__re4stack`) ahead of every other object's `.data`/`.bss`/`.common`,
  # and ahead of `__re4arena` (which stays last -- it lives inside the re4_port static library,
  # linked after every source listed directly here).
  add_executable(re4_boot src/port/lowmem.cpp src/port/game_stack.cpp src/port/boot_main.cpp
    src/port/mem1.cpp src/port/dvd.cpp src/port/vi.cpp $<TARGET_OBJECTS:re4_boot_game>)
  target_include_directories(re4_boot PRIVATE ${RE4_GAME_INCLUDES} ${RE4_AURORA_DIR}/include)
  target_compile_definitions(re4_boot PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_boot PRIVATE ${RE4_GAME_OPTIONS})
  target_link_libraries(re4_boot PRIVATE
    re4_port
    re4_boot_stubs
    aurora_os aurora_vi aurora_gx aurora_pad aurora_dvd aurora_card aurora_mtx aurora_si aurora_core
  )
endif()
