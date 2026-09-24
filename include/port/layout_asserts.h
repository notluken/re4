// Cross-TU layout guard for the runtime (never-on-disc) SDK/game structs that cross the
// game<->re4_port<->Aurora call boundary: OSThread, OSContext, OSMutex, OSMessageQueue,
// DVDFileInfo, DVDCommandBlock, a couple of GX objects, the scheduler's TASK, and cDvdQueue.
//
// Why this exists (docs/port-boot.md section 25): `re4_port` (src/port/os_thread.cpp) and every
// game TU (src/game/scheduler.cpp, ...) both embed an `OSThread` inside their own structs
// (`TASK::Thread`), but `re4_port`'s own `target_compile_definitions()` once carried `TARGET_PC`
// without `RE4_U32_32` -- so `dolphin/types.h`'s `u32`/`s32` silently became the host's 8-byte
// `long` there while every game TU's stayed the project's 4-byte `int`. Two translation units
// disagreeing about `sizeof(OSThread)` is an ODR violation the compiler has no way to catch on its
// own (each TU compiles in isolation) -- `OSCreateThread`'s writes, computed against the wrong
// (larger) layout, walked past the real, smaller `OSThread` embedded in `TASK` and corrupted
// whatever followed it in `Task[]`.
//
// This header cannot compare two TUs' `sizeof()` against each other directly (they are separate
// compiler invocations) -- instead, like `tools/port/gen_static_asserts.py`'s on-disc-struct
// checks, it hardcodes the one authoritative size for each config (`RE4_U32_32` ON vs. the
// default OFF -- both are real, supported configurations of this host build, so both get their
// own literal) and asserts every TU that embeds/touches one of these types agrees with it. Force-
// included (`-include`, CMakeLists.txt) into every TARGET_PC host TU in both `re4_port` and the
// game targets, so a future divergence (a target that forgets a define, a header edit that changes
// a field's type) fails the build immediately, in every TU, instead of corrupting memory at
// runtime in whichever TU happens to embed the type.
//
// Sizes below are for arm64 macOS (8-byte pointers, 8-byte `long`); confirmed via a throwaway
// probe compiled with this repo's exact headers and flags (`clang++ -DTARGET_PC
// [-DRE4_U32_32] -I include -I src -I build/G4BE08/include`), not derived by hand. Re-run that
// probe if this header ever needs a new platform's numbers -- do not guess at the arithmetic.
#ifndef TARGET_PC
#error "include/port/layout_asserts.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_LAYOUT_ASSERTS_H
#define RE4_PORT_LAYOUT_ASSERTS_H

// Deliberately does NOT include <dolphin/gx.h> here: force-including it ahead of a game TU's own
// #include "gx.h" (include/gx.h, the project's higher-level wrapper) breaks the GXPosition3f32-
// style paired-single-write macros that header defines over dolphin/gx.h's plain functions --
// confirmed the hard way (redefinition errors the moment this file force-included dolphin/gx.h
// into every re4_boot_game TU). GX object sizes are checked instead by the standalone
// `re4_port_static_asserts` executable (CMakeLists.txt), which already exists for exactly this
// "assert host layout matches, in one clean throwaway TU" purpose (tools/port/gen_static_asserts.py).
#include "types.h"
#include "global.h"
#define _DOLPHIN_TYPES_H_
#include <dolphin/dvd.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSThread.h>

// Not "dvd.h" -- a quoted #include from a file inside include/port/ resolves relative to this
// file's own directory first, which would find include/port/dvd.h (a different, port-specific
// header) instead of the game's include/dvd.h (cDvdQueue) -- confirmed the hard way (a `-include`
// force-include of this header failed with "undeclared identifier 'cDvdQueue'" while a plain
// #include of the same text one directory up compiled cleanly).
#include "../dvd.h"
#include "../scheduler.h"

#include <cstddef>

#ifdef RE4_U32_32
#define RE4_LAYOUT_ASSERT(type, size) \
    static_assert(sizeof(type) == (size), #type " layout disagreement between TUs (RE4_U32_32=ON) -- see include/port/layout_asserts.h")
#else
#define RE4_LAYOUT_ASSERT(type, size) \
    static_assert(sizeof(type) == (size), #type " layout disagreement between TUs (RE4_U32_32=OFF, default host build) -- see include/port/layout_asserts.h")
#endif

#ifdef RE4_U32_32
RE4_LAYOUT_ASSERT(OSContext, 712);
RE4_LAYOUT_ASSERT(OSThread, 856);
RE4_LAYOUT_ASSERT(OSMutex, 48);
RE4_LAYOUT_ASSERT(OSMessageQueue, 56);
RE4_LAYOUT_ASSERT(DVDFileInfo, 88);
RE4_LAYOUT_ASSERT(DVDCommandBlock, 72);
RE4_LAYOUT_ASSERT(TASK, 936);
RE4_LAYOUT_ASSERT(cDvdQueue, 840);
#else
RE4_LAYOUT_ASSERT(OSContext, 912);
RE4_LAYOUT_ASSERT(OSThread, 1072);
RE4_LAYOUT_ASSERT(OSMutex, 48);
RE4_LAYOUT_ASSERT(OSMessageQueue, 64);
RE4_LAYOUT_ASSERT(DVDFileInfo, 120);
RE4_LAYOUT_ASSERT(DVDCommandBlock, 96);
RE4_LAYOUT_ASSERT(TASK, 1152);
RE4_LAYOUT_ASSERT(cDvdQueue, 1480);
#endif

#undef RE4_LAYOUT_ASSERT

#endif // RE4_PORT_LAYOUT_ASSERTS_H
