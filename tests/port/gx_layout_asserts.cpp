// GX object sizeof() checks for the game<->Aurora call boundary (coordinator's ask, docs/
// port-boot.md section 25/26: "GX/VI SDK parity table (signatures, GXTexObj/GXTlutObj/GXFifoObj/
// GXRenderModeObj/GXColor layouts) vs Aurora"). Compiled into the standalone `re4_port_static_
// asserts` executable (CMakeLists.txt), not force-included into every game TU: an earlier attempt
// to force-include <dolphin/gx.h> into every re4_boot_game TU alongside the OSThread/TASK checks
// (include/port/layout_asserts.h) broke include/gx.h's own paired-single-write macros
// (GXPosition3f32 et al. redefinition errors) for any game file that also does its own
// #include "gx.h" -- this file avoids that entirely by living in its own clean throwaway TU,
// the same pattern tools/port/gen_static_asserts.py already uses for the on-disc-struct checks.
//
// Sizes are for arm64 macOS (RE4_U32_32=ON, the only config this ever runs under -- see this
// target's own CMakeLists.txt guard), confirmed via the same throwaway-probe method as
// include/port/layout_asserts.h -- not derived by hand.
#ifndef TARGET_PC
#error "tests/port/gx_layout_asserts.cpp is host-only (TARGET_PC)"
#endif
#ifndef RE4_U32_32
#error "gx_layout_asserts.cpp only runs under RE4_U32_32=ON -- see CMakeLists.txt's re4_port_static_asserts guard"
#endif

#include <dolphin/gx.h>

#include <cstddef>

static_assert(sizeof(GXTexObj) == 32, "GXTexObj layout disagreement -- see tests/port/gx_layout_asserts.cpp");
static_assert(sizeof(GXTlutObj) == 12, "GXTlutObj layout disagreement -- see tests/port/gx_layout_asserts.cpp");
static_assert(sizeof(GXColor) == 4, "GXColor layout disagreement -- see tests/port/gx_layout_asserts.cpp");
static_assert(sizeof(GXFifoObj) == 128, "GXFifoObj layout disagreement -- see tests/port/gx_layout_asserts.cpp");
static_assert(sizeof(GXRenderModeObj) == 60, "GXRenderModeObj layout disagreement -- see tests/port/gx_layout_asserts.cpp");
