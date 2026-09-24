#ifndef MATH_SUB_H
#define MATH_SUB_H

#include "types.h"
#include "vec.h"
#include "db_log.h"

// libm's own declaration of fabsf clashes with the inline below (C linkage against static), so it is
// renamed out of the way while <math.h> is read.
#define fabsf fabsf_libm
#include <math.h>
#undef fabsf

// Float abs as the original SDK header defines it: a volatile asm, which also acts as a
// scheduling barrier (loads after it are not hoisted above it).
static inline f32 fabsf(f32 x)
{
#ifdef TARGET_PC
    // No PPC `fabs` on the host; same single-instruction op via the compiler builtin.
    return __builtin_fabsf(x);
#else
    f32 r;
    asm volatile("fabs %0,%1" : "=f"(r) : "f"(x));
    return r;
#endif
}

// Column `c` of a matrix read into a Vec.
static inline void getColumn(Mtx m, int c, Vec* v) { v->x = m[0][c]; v->y = m[1][c]; v->z = m[2][c]; }

#include "math_sub_decl.h"

#endif
