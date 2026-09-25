// Hand-written stubs for re4_boot's undefined-symbol holdouts that tools/port/gen_boot_stubs.py's
// regex-based header parser can't confidently classify on its own -- mostly function-pointer-typed
// parameters (`void (*)(void*)`) which break its single-level paren matcher, plus a handful of
// genuinely special cases documented inline. See docs/port-boot.md for the undefined-symbol survey
// this file resolves the tail of.
#ifdef TARGET_PC

#include "stub_common.h"
#include "dolphin/os/OSThread.h"
#include "sce_at.h"
#include "trans.h"
#include "tpl.h"

extern "C" {

// TEXGet: the vendor's own real one-line body (src/lib/texPalette.c: `return
// &pal->descriptorArray[id];`, behind an ASSERTMSGLINE bounds check). That .c file is a
// CodeWarrior/charPipeline unit (its own texPalette.h declares TEXPalettePtr etc., a different set
// of C types from this tree's own tpl.h) and is not built for the port (CMakeLists.txt only globs
// src/game/*.cpp); rather than compile the mismatched CodeWarrior unit, its logic is reproduced
// here directly against tpl.h's own (already Ptr32<T>/BE<T>-audited, docs/port-phase3.md) types --
// same real body, this tree's types, no game logic invented. The previous auto-generated stub
// (src/port/stubs/generated_c_stubs.cpp) always returned NULL, which crashed the very first real
// caller (cTexSys::TexRegist, texture.cpp) dereferencing a null TEXDescriptor*.
TEXDescriptor* TEXGet(TEXPalette* pal, u32 id)
{
    if ((u32) pal->numDescriptors <= id) {
        std::fprintf(stderr, "TEXGet(): id[%u] >= numDescriptors[%u]\n", id, (u32) pal->numDescriptors);
        return &pal->descriptorArray[0];
    }
    return &pal->descriptorArray[id];
}

// -- sound/CRI: function-pointer parameters (docs/port-boot.md, "Aurora mismatches" -- Aurora
// declares dolphin/ax.h but implements none of it; ADX/mwPly are CRI middleware, not SDK) --
void ADXM_SetCbErr(void (*)(void*, const char*), void*) {}
AXVPB* AXAcquireVoice(u32, void (*)(void*), u32) { return nullptr; }
void AXFXSetHooks(void* (*)(u32), void (*)(void*)) {}
void AXRegisterAuxACallback(void (*)(void*, void*), void*) {}
void AXRegisterAuxBCallback(void (*)(void*, void*), void*) {}

// -- espgen: these wrapped onto multiple source lines in include/espgen.h, which
// tools/port/gen_boot_stubs.py's grep-based declaration finder (single-line matches) doesn't
// join back together; hand-copied from the real declarations instead.
int Espgen02_SetFreeWork(EspgenWork*, EspGenWork*, EspSeqData*, cModel*, u16, Mtx*, Vec*, Vec*, EspSeqOpt*, int) { return 0; }
int Espgen42_SetFreeWork(EspgenWork*, EspGenWork*, EspSeqData*, cModel*, u16, Mtx*, Vec*, Vec*, EspSeqOpt*) { return 0; }
int Espgen43_SetFreeWork(EspgenWork*, EspGenWork*, EspSeqData*, cModel*, u16, Mtx*, Vec*, Vec*, EspSeqOpt*) { return 0; }
int Espgen45_SetFreeWork(EspgenWork*, EspGenWork*, EspSeqData*, cModel*, u16, Mtx*, Vec*, Vec*, EspSeqOpt*) { return 0; }

// OSCreateThread: real semantics now in src/port/os_thread.cpp (docs/port-phase3.md's
// continuation -- this stub returning 0 without ever starting `func` was the root cause of no
// task's code ever running, which in turn tripped scheduler.cpp's stack-overflow guard).

// SetPlDamage / SetSubBulldozer: now defined for real by src/game/pl_sub.cpp (un-excluded,
// cmake/boot_exclude.txt), removed from here to avoid a duplicate-symbol link error.

// -- SndCall: declared as plain C++ (include/snd.h, no asm label), but the undefined symbol the
// linker reports is `SndCall__FUsUsP3VeciiP5cUnit` -- a GNU v2 (old-style) mangled name, not the
// Itanium name clang would generate for that same C++ declaration. TO VERIFY: which TU's call site
// forces the old mangling (a residual asm-label alias somewhere on this boot path, not found this
// pass); defining a same-named function under the literal old-mangled symbol side-steps it either
// way, matching the pattern already used by GameCube-target `asm("...")` aliases elsewhere in this
// tree (docs/port.md, "t_esp.cpp's operator new").
u32 SndCall__FUsUsP3VeciiP5cUnit(u16, u16, Vec*, int, int, cUnit*) { return 0; }

} // extern "C"

// -- GXWGFifo: superseded by a real implementation, src/port/gx_wgfifo.cpp (docs/port-boot.md
// section 30) -- every `GXWGFifo->field = v` write now genuinely reaches Aurora's GX FIFO via a
// GXParam1xx()/GXCmd1xx() call, not a discarded plain global.

// PlReloadSpeedTbl / PlShotFrameTbl: now defined for real by src/game/pl_class.cpp (was never
// excluded), removed from here to avoid a duplicate-symbol link error (surfaced once some other
// still-excluded-unit callee in this same TU forced the archive member that carried them into the
// link for the first time).

// -- pSUB: asm-aliased global (include/player.h) -- the linker name is the alias string, not the
// C++ identifier, so it needs the same `asm("...")` binding the header already declares, not a
// plain extern "C" definition. (pSys, declared the same way in include/main.h, is already defined
// by src/game/main.cpp itself -- no stub needed, confirmed by a duplicate-symbol link error when
// one was added here.)
cEm* pSubEm asm("pSUB") = nullptr;

// cPlayer::SPEED_WALK_TURN / SPEED_RUN_TURN: now defined for real by src/game/pl_class.cpp,
// removed from here to avoid a duplicate-symbol link error.

// vtable "key function" stubs: cPlayer/cPlLeon/cPlAshley/cObjRocket/cObjLauncher's real key
// functions and virtual overrides are all now compiled for real (pl_class.cpp was never excluded;
// pl_leon.cpp/pl_ashley.cpp/objRocket.cpp un-excluded this pass, cmake/boot_exclude.txt) --
// removed from here to avoid a duplicate-symbol link error. cObjRobo::move() stays: objRobo.cpp is
// still excluded (Phase 5, real PPC asm elsewhere in that file).
void cObjRobo::move()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cObjRobo::move() called\n"); warned = true; }
}

// -- main_sub.cpp: newly un-excluded this pass (docs/port-boot.md section 26 -- Render_before()
// is now real code, no longer a stub), which surfaced these previously-unreached undefined
// symbols for the first time (nothing on the boot path called into main_sub.cpp's own body
// before, so the linker never needed them).
extern "C" {
// OSStopwatch: real timing utility, safe as a no-op (docs/port.md convention -- a stat/debug
// facility with no gameplay-visible side effect; StopwatchInit()/etc. just read back whatever
// these leave in `sw`, and a zeroed OSStopwatch reads as "0 time elapsed", not a crash).
void OSInitStopwatch(OSStopwatch* sw, char* name)
{
    if (sw) {
        *sw = OSStopwatch{};
    }
}
void OSResetStopwatch(OSStopwatch* sw) { (void) sw; }
void OSStartStopwatch(OSStopwatch* sw) { (void) sw; }
void OSStopStopwatch(OSStopwatch* sw) { (void) sw; }

// OSLink/OSUnlink: now defined for real by src/port/rel.cpp (the host module registry, see
// include/port/rel.h), removed from here to avoid a duplicate-symbol link error.

// VISetBlack/VISetNextFrameBuffer/VIGetNextField: real implementations, src/port/vi.cpp
// (docs/port-boot.md's frame-presentation milestone).
} // extern "C"

// SceSys / cSceSys::checkCTaskRange(): now defined for real by src/game/sce_sys.cpp (un-excluded
// this pass, cmake/boot_exclude.txt), removed from here to avoid a duplicate-symbol link error.

// Game: now defined for real by src/game/game.cpp (un-excluded this pass, cmake/boot_exclude.txt),
// removed from here to avoid a duplicate-symbol link error. read.cpp's own `GameWork` view struct
// (docs/port-boot.md section 35) stays -- it is a separate, deliberately narrower type ReadWepData
// still uses, not a redeclaration of this one.

// SpecularInit / GlobalIlmTexInit: now defined for real by src/game/trans.cpp (un-excluded,
// cmake/boot_exclude.txt -- docs/port-boot.md section 46/47), removed from here to avoid a
// duplicate-symbol link error.

// SceAtHitCheck/SceAtCreateExecAt: real bodies live in src/game/sce_at.cpp, still on
// cmake/boot_exclude.txt (unrelated asm-register errors, `docs/port-boot.md`'s `re4_game_all -k 0`
// baseline -- "unknown register name 'fr0' in asm", not a REL-plan concern). Every st1_0/st2_x/
// st3_x/st4_x room source calls at least one of these (`include/sce_at.h`), so linking ANY REL
// module -- st1_0 first, docs/port-boot.md's REL plan -- needs a real (if inert) definition for
// re4_boot to link at all until sce_at.cpp's own asm ports. No `extern "C"` here: sce_at.h declares
// both as plain (mangled) C++ functions, matching what the linker actually asks for.
int SceAtHitCheck(u32 at_no)
{
    (void) at_no;
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: SceAtHitCheck() called\n"); warned = true; }
    return 0;
}
int SceAtCreateExecAt(cModel* m, Vec* pos, int a, int b, int c, f32 h, int d, f32 ang, f32 range,
                       int e, int prio, TaskFunc func, int arg, u8 flag)
{
    (void) m; (void) pos; (void) a; (void) b; (void) c; (void) h; (void) d; (void) ang;
    (void) range; (void) e; (void) prio; (void) func; (void) arg; (void) flag;
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: SceAtCreateExecAt() called\n"); warned = true; }
    return 0;
}

#endif // TARGET_PC
