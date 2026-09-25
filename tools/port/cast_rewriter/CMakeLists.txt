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

# Regression test (tests/run_test.sh): a fixture with the three macro shapes that produced real
# bugs (docs/port-phase2.md section 8's macro-range fix) -- runs the just-built binary, checks the
# rewritten output both by text pattern and by actually compiling it against a stub re4_port.
enable_testing()
execute_process(COMMAND /opt/homebrew/opt/llvm/bin/clang -print-resource-dir
                 OUTPUT_VARIABLE RE4_CAST_REWRITER_TEST_RESOURCE_DIR
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
add_test(NAME re4_cast_rewriter_macro_ranges
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_test.sh
                 $<TARGET_FILE:re4_cast_rewriter>
                 ${RE4_CAST_REWRITER_TEST_RESOURCE_DIR})
