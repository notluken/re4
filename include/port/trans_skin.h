// TARGET_PC-only C port of src/game/trans.cpp's whole-function vendor PPC asm for vertex
// skinning (CalcSk1_x/CalcSk1_x2/setupGQR6) -- split into its own small translation unit
// (docs/port-boot.md section 46) so it can be unit-tested (tests/port/test_trans_skin.cpp)
// without pulling in the rest of trans.cpp's dependency graph. trans.cpp itself declares these
// same three names (extern "C", include/trans.h) and calls straight into this file's
// definitions under TARGET_PC; the matching build never sees this header at all (its `#else`
// branches keep the original whole-function asm, unchanged, directly in trans.cpp).
#ifndef PORT_TRANS_SKIN_H
#define PORT_TRANS_SKIN_H

#include "types.h"

extern "C" {
void CalcSk1_x(void* dst, void* src, u32 n);
void CalcSk1_x2(void* dst, void* src, u32 n);
void setupGQR6(u32 v);
}

#endif
