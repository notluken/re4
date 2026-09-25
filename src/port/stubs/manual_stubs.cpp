// Hand-written stubs for re4_boot's undefined-symbol holdouts that tools/port/gen_boot_stubs.py's
// regex-based header parser can't confidently classify on its own -- mostly function-pointer-typed
// parameters (`void (*)(void*)`) which break its single-level paren matcher, plus a handful of
// genuinely special cases documented inline. See docs/port-boot.md for the undefined-symbol survey
// this file resolves the tail of.
#ifdef TARGET_PC

#include "stub_common.h"
#include "dolphin/os/OSThread.h"
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

// -- pl_sub: function-pointer parameters --
void SetPlDamage(cEm*, void (*)(cPlayer*)) {}
void SetSubBulldozer(void (*)(cEm*), void (*)(cEm*)) {}

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

// -- PlReloadSpeedTbl / PlShotFrameTbl: 2-D const f32 tables (include/player.h), normally defined
// in pl_class.cpp (excluded, cmake/boot_exclude.txt). Zero-filled placeholders -- gameplay-wrong,
// link-correct; real values need pl_class.cpp un-excluded or the tables copied out by hand.
const f32 PlReloadSpeedTbl[45][3] = {};
const f32 PlShotFrameTbl[45][5] = {};

// -- pSUB: asm-aliased global (include/player.h) -- the linker name is the alias string, not the
// C++ identifier, so it needs the same `asm("...")` binding the header already declares, not a
// plain extern "C" definition. (pSys, declared the same way in include/main.h, is already defined
// by src/game/main.cpp itself -- no stub needed, confirmed by a duplicate-symbol link error when
// one was added here.)
cEm* pSubEm asm("pSUB") = nullptr;

// cPlayer::SPEED_WALK_TURN / SPEED_RUN_TURN: static const f32 class members (include/player.h),
// normally defined in pl_class.cpp (excluded). Zero placeholders, same caveat as the reload/shot
// tables above.
const f32 cPlayer::SPEED_WALK_TURN = 0.0f;
const f32 cPlayer::SPEED_RUN_TURN = 0.0f;

// -- vtable "key function" stubs -- the Itanium ABI emits a class's vtable in whichever TU defines
// its *key function* (the first virtual member function the class itself declares that isn't
// defined inline in the class body, include/player.h's "Vtable order" comment) -- not just in
// whichever TU happens to call one of the class's virtuals. All six of these classes' own key
// functions are defined in cmake/boot_exclude.txt files (pl_class.cpp/pl_leon.cpp/pl_ashley.cpp/
// objRobo.cpp/pl_wep.cpp), and (except cPlayer::beginEvent, which is real gameplay logic never
// otherwise stubbed) are never themselves called on this boot path, so tools/port/
// gen_boot_stubs.py's linker-driven survey never saw them as undefined on their own -- only the
// resulting "vtable for X" did. Spot-checked against each class's declaration order in
// include/player.h / include/objRobo.h / include/pl_wep.h.
void cPlayer::beginEvent(u32 flag)
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cPlayer::beginEvent() called\n"); warned = true; }
}
void cPlLeon::move()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cPlLeon::move() called\n"); warned = true; }
}
void cPlAshley::move()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cPlAshley::move() called\n"); warned = true; }
}
void cObjRobo::move()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cObjRobo::move() called\n"); warned = true; }
}
void cObjRocket::beginEvent(u32 flag)
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cObjRocket::beginEvent() called\n"); warned = true; }
}
cObjLauncher::~cObjLauncher()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cObjLauncher::~cObjLauncher() called\n"); warned = true; }
}

// Once a class's vtable is emitted at all (the key-function stubs just above), every virtual slot
// in it needs a definition, not just the key one -- the rest of each of these six classes' own
// overrides, same excluded-file provenance as above.
#define RE4_STUB_VOID(sig) void sig { static bool w = false; if (!w) { std::fprintf(stderr, "STUB: " #sig " called\n"); w = true; } }
RE4_STUB_VOID(cObjRocket::move())
RE4_STUB_VOID(cObjLauncher::init(cModel*))
RE4_STUB_VOID(cObjLauncher::moveDrop())
RE4_STUB_VOID(cObjLauncher::moveFire())
RE4_STUB_VOID(cObjLauncher::interrupt())
RE4_STUB_VOID(cObjLauncher::setMotion(cPlayer*))
int cObjLauncher::keyKamae()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cObjLauncher::keyKamae() called\n"); warned = true; }
    return 0;
}
RE4_STUB_VOID(cPlLeon::setLeftHand(unsigned int))
RE4_STUB_VOID(cPlLeon::setRightHand(int))
RE4_STUB_VOID(cPlLeon::setFace(int))
RE4_STUB_VOID(cPlLeon::setHead(void*, void*))
RE4_STUB_VOID(cPlLeon::setHead(int))
RE4_STUB_VOID(cPlLeon::setModel())
RE4_STUB_VOID(cPlLeon::setWound())
RE4_STUB_VOID(cPlLeon::setMotion())
int cPlLeon::checkXbutton()
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: cPlLeon::checkXbutton() called\n"); warned = true; }
    return 0;
}
RE4_STUB_VOID(cPlayer::setNoSuspend(int))
RE4_STUB_VOID(cPlayer::endEvent(unsigned int))
RE4_STUB_VOID(cPlAshley::setLeftHand(unsigned int))
RE4_STUB_VOID(cPlAshley::setRightHand(int))
RE4_STUB_VOID(cPlAshley::moveMatCalcBefore())
RE4_STUB_VOID(cPlAshley::setFace(int))
RE4_STUB_VOID(cPlAshley::setModel())
#undef RE4_STUB_VOID

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

// OSLink/OSUnlink: real DLL-module relocation (config/G4BE08's REL loader) -- no host equivalent
// exists yet (Phase 4+ territory, module RELs are not loaded by re4_boot at all). BOOL FALSE (0)
// is the documented "failed" return every real caller of these already checks (main_sub.cpp's
// own DLL_EPILOG-guarded call sites), not a silent success.
BOOL OSLink(OSModuleInfo* newModule, void* bss)
{
    (void) newModule;
    (void) bss;
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: OSLink() called\n"); warned = true; }
    return 0;
}
BOOL OSUnlink(OSModuleInfo* oldModule)
{
    (void) oldModule;
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: OSUnlink() called\n"); warned = true; }
    return 0;
}

// VISetBlack/VISetNextFrameBuffer/VIGetNextField: real implementations, src/port/vi.cpp
// (docs/port-boot.md's frame-presentation milestone).
} // extern "C"

// SceSys: global cSceSys instance, normally defined in sce_sys.cpp (still excluded,
// cmake/boot_exclude.txt -- pointer<->integer cast category, not yet reached). cSceSys's own
// declaration (include/sce_sys.h) is a plain aggregate (no virtuals, no user constructor), so a
// zero-initialized instance here is exactly what sce_sys.cpp's own `cSceSys SceSys;` would have
// produced before SceSysInit() ever touches it -- main_sub.cpp only reads its debug-display
// fields, never calls a method on it.
cSceSys SceSys;

// cSceSys::checkCTaskRange(): also normally defined in sce_sys.cpp (excluded, see above);
// main_sub.cpp's Render_done() only checks the returned range for a debug on-screen prim count --
// 0 ("no scenario task currently running") is a safe, always-valid answer.
int cSceSys::checkCTaskRange()
{
    return 0;
}

// Game: global GAME_WORK instance, normally defined in game.cpp (still excluded,
// cmake/boot_exclude.txt -- pointer<->integer cast category). read.cpp (now real, docs/port-boot.md
// section 35) declares its own view struct (`GameWork`, 0x1C bytes: Rno_bak/Map_addr/Map_size/
// Option_addr/Option_size/Swap_addr/omake_wep_addr) matching game.cpp's real GAME_WORK
// byte-for-byte; only `ReadWepData` reads it (Game.omake_wep_addr), and only for the extra-mode
// weapon-data buffer set up by gameInit(), not on the boot path. A plain global-scope C++ variable
// is not Itanium-mangled (docs/port-boot.md section 7), so this definition binds to the same link
// name (`_Game`) game.cpp's own `GAME_WORK Game;` would have produced.
struct GameWork {
    u32 Rno_bak;
    u32 Map_addr;
    u32 Map_size;
    u32 Option_addr;
    u32 Option_size;
    u32 Swap_addr;
    void* omake_wep_addr;
};
GameWork Game;

// SpecularInit / GlobalIlmTexInit: normally defined in trans.cpp (still excluded,
// cmake/boot_exclude.txt -- Phase 5, real paired-single asm elsewhere in that same file blocks the
// whole translation unit even though these two functions themselves are plain C++). Called from
// read.cpp's now-real CoreDataRead() right after it loads the core archive off disc, to bind the
// specular/indirect/thermal and global-illumination TPL textures via GXInitTexObj/GXInitTlutObj.
// Stubbed here as logging no-ops: the GX texture objects they would fill in stay zero-initialized,
// which is wrong for later specular/GI rendering but does not crash CoreDataRead itself (the real
// blocker this pass fixes is the null message-table read, not texture binding) -- bringing in the
// real logic needs trans.cpp's Phase 5 paired-single material ported to C first (coordinator's
// item 2 in a later pass), not a one-line fix here.
void SpecularInit(TEXPalette*, TEXPalette*, TEXPalette*, TEXPalette*)
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: SpecularInit() called\n"); warned = true; }
}
void GlobalIlmTexInit(TEXPalette*)
{
    static bool warned = false;
    if (!warned) { std::fprintf(stderr, "STUB: GlobalIlmTexInit() called\n"); warned = true; }
}

#endif // TARGET_PC
