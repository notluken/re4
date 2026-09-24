// Host implementations of the GCC 2.95 builtin operator-new/delete aliases (`__builtin_new`,
// `__builtin_delete`, `__builtin_vec_new`, `__builtin_vec_delete`) that src/game/puzzle.cpp (not
// excluded, cmake/boot_exclude.txt) declares and calls directly instead of `operator new`/`delete`.
// docs/port.md's "t_esp.cpp's operator new(...) asm(\"__builtin_new\")" note documents the same
// vendor idiom aliased through an asm-label; puzzle.cpp's copy calls the plain names with no asm
// label, so the fix here is just a normal definition forwarding to the host's real operator new/
// delete -- same mechanism, no alias needed since the name itself is what puzzle.cpp calls.
//
// Undefined-symbol note: clang does not Itanium-mangle these (`___builtin_new`, not
// `__Z12__builtin_newj`, confirmed with `nm` on puzzle.cpp.o) -- an implementation-reserved
// `__`-prefixed global function name gets the same "no mangling" treatment as a literal `main`
// once it stops being a recognized compiler intrinsic name and becomes an ordinary declared
// function; TO VERIFY against the C++ standard's exact rule, this is inferred from the linker's
// symbol table, not derived from the standard text.
#ifdef TARGET_PC

#include <cstddef>
#include <new>

void* __builtin_new(unsigned int size) { return ::operator new(size); }
void __builtin_delete(void* p) { ::operator delete(p); }
void* __builtin_vec_new(unsigned int size) { return ::operator new[](size); }
void __builtin_vec_delete(void* p) { ::operator delete[](p); }

#endif // TARGET_PC
