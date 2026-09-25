// Forced-included (CMakeLists.txt, re4_boot_game's target_compile_options, "-include") into every
// real vendor game translation unit compiled for `re4_boot`, and nothing else -- this is how
// include/port/alloc.h's caller-based split-allocator check (docs/port-boot.md section 40) knows
// "this call to `new` came from actual game code" instead of guessing by thread alone (section 39's
// bug: Aurora's own aurora_begin_frame() runs synchronously on the game thread too, so
// IsGameThread() can't tell them apart).
//
// #pragma clang section text=... is a compiler-wide directive, not a per-function attribute: once
// active, every function *textually defined from this point in the same translation unit onward*
// -- including template instantiations and inline functions from headers this TU includes after
// this one -- gets its emitted machine code placed in the named Mach-O section. That is exactly the
// property this needs (a TU-granularity tag, not a painful per-function one) and exactly why this
// header must be the *first* thing `-include`d (before ptr32.h/layout_asserts.h): anything already
// emitted before this pragma runs keeps the compiler's default section instead.
//
// Not undone at the end of this header on purpose -- the whole rest of the TU (the actual game
// source file) is meant to be tagged too.
#ifndef RE4_PORT_GAME_SECTION_H
#define RE4_PORT_GAME_SECTION_H

#ifdef TARGET_PC
#pragma clang section text="__TEXT,__re4game"
#endif

#endif // RE4_PORT_GAME_SECTION_H
