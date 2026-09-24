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

set(RE4_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Matches configure.py's include_dirs (project_root/include, project_root/src,
# project_root/build/<version>/include) plus the version defines cflags_game carries.
set(RE4_GAME_INCLUDES
  ${RE4_ROOT}/include
  ${RE4_ROOT}/src
  ${RE4_ROOT}/build/G4BE08/include
)
set(RE4_GAME_DEFINES
  TARGET_PC
  BUILD_VERSION=0
  VERSION_G4BE08
  NDEBUG=1
)
if (RE4_U32_32)
  list(APPEND RE4_GAME_DEFINES RE4_U32_32)
endif()
# -Wno-address-of-temporary: `&((Vec){...})` compound-literal-address idiom (GNU C, several em*/wep*
# units), legal for the vendor's GCC 2.95 and alive for the length of the enclosing full expression;
# clang's default diagnostic for it is an error, not a warning, so it needs the flag, not -Wno-error.
set(RE4_GAME_OPTIONS
  -Wno-invalid-offsetof
  -Wno-narrowing
  -Wno-address-of-temporary
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

  add_library(re4_game_all OBJECT ${RE4_GAME_ALL_SOURCES})
  target_include_directories(re4_game_all PRIVATE ${RE4_GAME_INCLUDES})
  target_compile_definitions(re4_game_all PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_game_all PRIVATE ${RE4_GAME_OPTIONS})
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
)
target_include_directories(re4_port PUBLIC ${RE4_GAME_INCLUDES})
target_compile_definitions(re4_port PUBLIC TARGET_PC)
find_package(Threads REQUIRED)
target_link_libraries(re4_port PUBLIC Threads::Threads)

# --- tests/port: host unit tests for re4_port (Phase 2 step 2/3, docs/port-phase2.md) ---
option(RE4_PC_BUILD_TESTS "Add the tests/port/ host unit tests (ctest)" ON)
if (RE4_PC_BUILD_TESTS)
  add_executable(test_ptr32 tests/port/test_ptr32.cpp)
  target_link_libraries(test_ptr32 PRIVATE re4_port)
  add_test(NAME test_ptr32 COMMAND test_ptr32)

  add_executable(test_arena tests/port/test_arena.cpp)
  target_link_libraries(test_arena PRIVATE re4_port)
  add_test(NAME test_arena COMMAND test_arena)
endif()
