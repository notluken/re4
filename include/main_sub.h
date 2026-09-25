#ifndef MAIN_SUB_H
#define MAIN_SUB_H

#include "types.h"
#include "gx.h"
#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_  // the game's types.h supplies the SDK basic types (as scheduler.h does)
#endif
#include <dolphin/os/OSModule.h>

// Render target description (game/main.cpp `Screen`, 0x18 bytes; layout partially known).
struct ScreenInfo {
    f32 x;       // 0x00
    f32 y;       // 0x04
    f32 width;   // 0x08 (512.0f)
    f32 height;  // 0x0C (448.0f)
    u8 pad_10[0x18 - 0x10];
};

extern ScreenInfo Screen;
#define SCR_W ((u32) Screen.width)
#define SCR_H ((u32) Screen.height)
extern GXRenderModeObj Rmode;  // game/main_sub.cpp

// game/main.cpp frame buffers
extern void* pFrame_buff[2];
extern void* pCurrent_buff;

// A linked REL's prolog / epilog: the header stores offsets that OSLink turns into addresses.
#ifdef TARGET_PC
// OSLink() is a stub on this host (src/port/stubs/manual_stubs.cpp) -- no REL module is ever really
// relocated, so `(m)->prolog`/`(m)->epilog` are still raw, un-relocated file offsets, not valid code
// addresses. Route through re4_port::RelEntry() (include/port/rel.h) for a log message, then
// evaluate to a captureless no-op lambda (implicitly convertible to `TaskFunc`/`void (*)()`, the
// same type the vendor's macro produced) instead of ever jumping into that offset.
#include "port/rel.h"
#define DLL_PROLOG(m) ((re4_port::RelEntry((m), 0)), +[]() {})
#define DLL_EPILOG(m) ((re4_port::RelEntry((m), 1)), +[]() {})
#else
#define DLL_PROLOG(m) ((void (*)()) (m)->prolog)
#define DLL_EPILOG(m) ((void (*)()) (m)->epilog)
#endif

// A raw REL data-section size field (module bssOffset-style pattern: `*(u32*) (data + 4)`,
// src/game/read.cpp's readEmData/PlDataRead/ReadWepData), read straight out of an on-disc REL
// buffer that this port never byte-swaps as a whole (Phase 4+ territory -- CLAUDE.md's "REL
// fixups" row). On this little-endian host that raw big-endian u32 needs exactly one swap at the
// point it's read, the same single-purpose fix include/port/be.h documents for every other on-disc
// integer field; kept as its own macro (not BE<T>, which wraps a *stored* field, not a one-off
// `*(u32*)ptr` expression) so the three read.cpp call sites stay one-line edits with stable line
// numbers (read.cpp has #line directives reproducing the vendor's HALT()/assert line numbers --
// CLAUDE.md).
#ifdef TARGET_PC
#define REL_READ_OFFSET32(p) (__builtin_bswap32(*(u32*) (p)))
#else
#define REL_READ_OFFSET32(p) (*(u32*) (p))
#endif

// game/main_sub.cpp
int Render_checkBlurPermission();
extern "C" {
void Render_init();
void Render_before();
void Render_done();
void Render_swap();
void UpdateNearClipDist();
void SetNearClipDist(f32 dist);
void Render_DrawSyncCallback(u16 token);
void systemVISetBlack(int sw);
void SetScissorState();
void SetNoScissor();
void ScreenGXSet();
void ScreenReSize(u16 w, u16 h);
void EFBReSize(int w, int h);
void SecToTime(u32 sec, u32* ret_time, u32* ret_min, u32* ret_sec);
void InitGameTime();
u32 GetGameTime(u32* ret_time, u32* ret_min, u32* ret_sec);
void SetGameTime();
void ScreenShotStart(char* file_name, int start_frame, int flag336);
void ScreenShotEnd();
void SelfScreenShotInit();
void StopwatchInit();
void StopwatchStart();
u32 StopwatchStop(const char* pName);
void after_render_proc();
void Bg_brightness_set(f32 bright);
void DrawTpl(struct TEXPalette* tpl, int x, int y, int w, int h);
void DrawTexture(GXTexObj* texobj, s16 x, s16 y, s16 z, s16 w, s16 h);
void DLL_Unlink(OSModuleHeader* pModule);
void DLL_Link(OSModuleHeader* pModule, void* pBss);
}
// main_sub.cpp also owns flag_render_after, AutoScreenShotExec, ScreenShotExec,
// ScreenShotTriggerType, ScreenShotFilename[11]; declare them extern locally where needed
// (a header extern would change main_sub's .sbss order).
// game/TmpBuf.cpp
void* GetDrawTmpBufAddr(int type);

#endif
