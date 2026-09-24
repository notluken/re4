#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_

typedef signed   char          s8;
typedef unsigned char          u8;
typedef signed   short int     s16;
typedef unsigned short int     u16;
#if defined(TARGET_PC) && defined(RE4_U32_32)
// See include/types.h for why: the SDK's s32/u32 must match types.h's exactly, or a TU that
// includes both (most of the game does) gets a conflicting-typedef error.
typedef signed   int           s32;
typedef unsigned int           u32;
#else
typedef signed   long          s32;
typedef unsigned long          u32;
#endif
typedef signed   long long int s64;
typedef unsigned long long int u64;

typedef float  f32;
typedef double f64;

typedef char *Ptr;

typedef int BOOL;

#define FALSE 0
#define TRUE 1

#if defined(__MWERKS__)
#define AT_ADDRESS(addr) : (addr)
#elif defined(__GNUC__)
//#define AT_ADDRESS(addr) __attribute__((address((addr))))
#define AT_ADDRESS(addr)  // was removed in GCC. define in linker script instead.
#else
#error unknown compiler
#endif

#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

#define INT_MAX 2147483647

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __MWERKS__
#include "libc/stdio.h"
#include "libc/stdarg.h"
#include "libc/string.h"
#include "libc/ctype.h"
#else
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#endif

#ifdef __MWERKS__
#include "cmath.h"
#else
#define fabsf fabsf_libm  // math_sub.h defines its own inline fabsf
#include <math.h>
#undef fabsf
#endif

#endif
