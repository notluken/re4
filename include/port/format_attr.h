#ifndef RE4_PORT_FORMAT_ATTR_H
#define RE4_PORT_FORMAT_ATTR_H

// Task 1 (port audit): Ptr32<T>/BE<T> (include/port/ptr32.h, include/port/be.h) convert to their
// underlying value only through an implicit/explicit conversion *operator*. A C varargs call
// (eprintf/OSReport/OSPanic/sprintf/db_log's mes-err-warn/the cManager<T>::log family/...) never
// triggers that conversion for the "..." arguments -- the callee has no declared parameter type to
// convert to, so the vendor's Ptr32<T>/BE<T> object is passed as its raw bytes (a struct, not the
// T it wraps). `eprintf("%s", info->name)` (title.cpp, a Ptr32<char>) is exactly this bug, found and
// fixed by hand before this macro existed. This macro turns on `-Wformat` for every declaration it
// tags so `-Werror=format` (host-only, CMakeLists.txt) fails the build at every such site instead
// of silently passing raw bytes to the callee.
//
// TARGET_PC/host (clang or GCC) only: the attribute is spelled identically in the original SN GCC
// 2.95.3 toolchain too, but this header (and every declaration it decorates) is never reachable from
// that build -- `RE4_FORMAT_PRINTF` sites are exclusively under `#ifdef TARGET_PC`, which the
// matching build never defines. No original-target byte can change from this header existing.
#ifdef TARGET_PC
#if defined(__GNUC__) || defined(__clang__)
#define RE4_FORMAT_PRINTF(fmt_idx, first_arg_idx) \
    __attribute__((format(printf, fmt_idx, first_arg_idx)))
#else
#define RE4_FORMAT_PRINTF(fmt_idx, first_arg_idx)
#endif
#else
#define RE4_FORMAT_PRINTF(fmt_idx, first_arg_idx)
#endif

#endif // RE4_PORT_FORMAT_ATTR_H
