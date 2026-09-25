#ifndef _DOLPHIN_GX_GXSTRUCT_H_
#define _DOLPHIN_GX_GXSTRUCT_H_

#include <dolphin/gx/GXEnum.h>
#include <dolphin/vi/vitypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GXRenderModeObj {
    /* 0x00 */ VITVMode viTVmode;
    /* 0x04 */ u16 fbWidth;
    /* 0x06 */ u16 efbHeight;
    /* 0x08 */ u16 xfbHeight;
    /* 0x0A */ u16 viXOrigin;
    /* 0x0C */ u16 viYOrigin;
    /* 0x0E */ u16 viWidth;
    /* 0x10 */ u16 viHeight;
    /* 0x14 */ VIXFBMode xFBmode;
    /* 0x18 */ u8 field_rendering;
    /* 0x19 */ u8 aa;
    /* 0x20 */ u8 sample_pattern[12][2];
    /* 0x38 */ u8 vfilter[7];
} GXRenderModeObj;

typedef struct _GXColor {
    u8 r, g, b, a;
} GXColor;

typedef struct _GXColorS10 {
    s16 r, g, b, a;
} GXColorS10;

// GXTexObj/GXTlutObj are opaque to the game on real hardware -- 32 and 12 bytes there, respected
// unchanged below for the matching build. Aurora's host-side reinterpretation (GXTexObj_/
// GXTlutObj_, ../aurora/lib/gfx/texture.hpp) needs more room per object than real GX hardware ever
// did (a cached host pointer, width/height, a texture-cache id, ...) -- Aurora's OWN copy of this
// exact header (../aurora/include/dolphin/gx/GXStruct.h) already widens these two under TARGET_PC
// for exactly that reason; this repo's copy did not, and because this repo's include/ is searched
// before Aurora's own (src/port/dvd.cpp's comment on the same shadowing effect), game code was
// still compiling against the narrow, real-hardware size here -- a real, silent buffer overflow
// (GXInitTexObj/GXInitTexObjCI writing up to sizeof(GXTexObj_) into a `GXTexObj fontTexObj;` that
// was really only 32 bytes) found live with lldb (docs/port-boot.md): `obj.width()` read back
// wrong on Aurora's own FIFO thread even though the value passed into GXInitTexObj was confirmed
// correct at the call site. Mirrors Aurora's own header exactly, so both sides finally agree.
typedef struct _GXTexObj {
#ifdef TARGET_PC
    u32 dummy[16];
#else
    u32 dummy[8];
#endif
} GXTexObj;

typedef struct _GXLightObj {
    u32 dummy[16];
} GXLightObj;

typedef struct _GXTexRegion {
    u32 dummy[4];
} GXTexRegion;

typedef struct _GXTlutObj {
#ifdef TARGET_PC
    u32 dummy[10];
#else
    u32 dummy[3];
#endif
} GXTlutObj;

typedef struct _GXTlutRegion {
    u32 dummy[4];
} GXTlutRegion;

typedef struct _GXFogAdjTable {
    u16 r[10];
} GXFogAdjTable;

typedef struct _GXVtxDescList {
    GXAttr attr;
    GXAttrType type;
} GXVtxDescList;

typedef struct _GXVtxAttrFmtList {
    GXAttr attr;
    GXCompCnt cnt;
    GXCompType type;
    u8 frac;
} GXVtxAttrFmtList;

#ifdef __cplusplus
}
#endif

#endif
