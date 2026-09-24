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
cmake_minimum_required(VERSION 3.25)
project(re4-pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS ON)   # GCC 2.95 game code leans on GNU extensions (statement exprs, etc.)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Widen to every unit under src/game (used by the re4_game_all error-inventory target below);
# the small re4_game_core target always stays as its own explicit list.
option(RE4_PC_BUILD_ALL_GAME "Add re4_game_all: every src/game/*.c(pp) as its own OBJECT library, -k-style, error inventory only" ON)

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

# --- re4_game_core: a small representative slice of src/game (Phase 1 goal: compiles, not links) ---
# One from the object chain (cCoord/cModel), one cManager<T> consumer, math (C++ and C), the DVD
# loader and its higher-level reader, and a plain utility class.
set(RE4_GAME_CORE_SOURCES
  src/game/model.cpp        # object chain: cCoord, cModel, cModelInfo, PartsMgr/ModInfoMgr
  src/game/light.cpp        # cManager<T> consumer (light pool)
  src/game/math_sub.cpp     # math (C++)
  src/game/math_support.c   # math (C, wraps lib/fdlibm/*.c)
  src/game/dvd.cpp          # DVD loader
  src/game/read.cpp         # higher-level file reader built on dvd.cpp
  src/game/cString.cpp      # plain utility class
  src/game/item.cpp         # object pool / manager consumer
)

add_library(re4_game_core OBJECT ${RE4_GAME_CORE_SOURCES})
target_include_directories(re4_game_core PRIVATE ${RE4_GAME_INCLUDES})
target_compile_definitions(re4_game_core PRIVATE ${RE4_GAME_DEFINES})
target_compile_options(re4_game_core PRIVATE
  -Wno-invalid-offsetof
  -Wno-narrowing
)

# --- re4_game_all: every src/game/*.c(pp) as its own OBJECT library ---
# Not meant to succeed yet: build with `-- -k 0` (Ninja) so one unit's errors don't stop the rest,
# and read the diagnostics to write docs/port-phase1-errors.md. `game/memset_2.cpp` and
# `game/yz2asm.cpp` are the two Capcom whole-function-asm units (tools/asmcheck.py ASM_BODIED);
# they are real vendor assembly and not expected to translate, so they are skipped here too.
if (RE4_PC_BUILD_ALL_GAME)
  file(GLOB RE4_GAME_ALL_SOURCES CONFIGURE_DEPENDS
    ${RE4_ROOT}/src/game/*.cpp
    ${RE4_ROOT}/src/game/*.c
  )
  list(FILTER RE4_GAME_ALL_SOURCES EXCLUDE REGEX ".*/(memset_2|yz2asm)\\.cpp$")

  add_library(re4_game_all OBJECT ${RE4_GAME_ALL_SOURCES})
  target_include_directories(re4_game_all PRIVATE ${RE4_GAME_INCLUDES})
  target_compile_definitions(re4_game_all PRIVATE ${RE4_GAME_DEFINES})
  target_compile_options(re4_game_all PRIVATE
    -Wno-invalid-offsetof
    -Wno-narrowing
  )
endif()
