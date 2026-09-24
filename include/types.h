#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef signed short s16;
#if defined(TARGET_PC) && defined(RE4_U32_32)
// GameCube s32/u32 are `long` (4 bytes there, GCC 2.95 ILP32). On a 64-bit host `long` is 8 bytes
// (LP64), which silently gives every struct with a s32/u32 field the wrong layout -- not a
// compile error, so nothing catches it without this switch. RE4_U32_32 (off by default: most of the
// tree does not yet build with a 4-byte s32/u32, see docs/port-phase2.md step 1) makes s32/u32 the
// host's 4-byte int/unsigned int instead, matching the GameCube's width.
typedef signed int s32;
typedef unsigned int u32;
#else
typedef signed long s32;
typedef unsigned long u32;
#endif
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef int BOOL;

#ifndef NULL
#define NULL 0
#endif

#endif
