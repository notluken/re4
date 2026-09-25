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
// The trailing `,regular,pure_instructions` is load-bearing, not decorative: without it ld64 does
// not recognize `__re4game` as a code section (it only infers that from the conventional
// `__text`'s name otherwise) and silently skips generating compact-unwind/`__unwind_info` entries
// for anything placed in it -- confirmed with a standalone repro (a `try`/`catch` in a
// `#pragma clang section text="__TEXT,__re4game"`-tagged TU calling a throwing function in the
// same section: the exception escapes uncaught, `libc++abi: terminating due to uncaught
// exception`, exactly this bug) and by `image show-unwind` on a real `re4_boot`: functions in the
// old, attribute-less `__re4game` section had no "sourced from the compiler: yes" compact-unwind
// plan at all (only the generic arm64 default), while ordinary `__text` functions did. The one
// ld64 warning this used to produce ("missing 'regular,pure_instructions' section flag",
// docs/port-boot.md section 40) was NOT cosmetic -- it was ld64 accurately describing the exact
// defect this comment fixes. Any exception that needs to unwind through `src/game` code (every
// `OSExitThread()` call via scheduler.cpp's `TaskExit`/`TaskChain`, at minimum) was silently
// fatal before this fix.
#pragma clang section text="__TEXT,__re4game,regular,pure_instructions"
#endif

#endif // RE4_PORT_GAME_SECTION_H
