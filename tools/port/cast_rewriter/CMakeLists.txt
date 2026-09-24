# Standalone build for re4_cast_rewriter (docs/port-phase2.md section 8).
#
# Built against Homebrew LLVM (needs libTooling; Apple's bundled clang does not ship the
# libTooling headers/libs -- verified 2026-09-24, see docs/port-phase2.md). Not part of the main
# CMakeLists.txt's configure (different toolchain); build it once, separately:
#
#   cmake -S tools/port/cast_rewriter -B build-pc-tool -G Ninja \
#         -DCMAKE_PREFIX_PATH=$(brew --prefix llvm) -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-pc-tool
#
# The resulting tools/port/cast_rewriter binary path is what the main CMakeLists.txt's
# RE4_REWRITE_CASTS option invokes per source file.
cmake_minimum_required(VERSION 3.20)
project(re4_cast_rewriter LANGUAGES C CXX) # C needed only for LLVMConfig's LibEdit try_compile check

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Clang REQUIRED CONFIG)
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Found Clang: ${Clang_DIR}")

add_executable(re4_cast_rewriter CastRewriter.cpp)

target_include_directories(re4_cast_rewriter PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(re4_cast_rewriter PRIVATE ${LLVM_DEFS_LIST})
target_compile_options(re4_cast_rewriter PRIVATE -fno-rtti)

target_link_libraries(re4_cast_rewriter PRIVATE
  clangTooling
  clangFrontend
  clangSerialization
  clangDriver
  clangParse
  clangSema
  clangAnalysis
  clangRewriteFrontend
  clangRewrite
  clangEdit
  clangAST
  clangASTMatchers
  clangLex
  clangBasic
  clangSupport
)
