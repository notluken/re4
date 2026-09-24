#ifndef GX_H
#define GX_H

// GX types needed by game code. dolphin/gx.h pulls in the CodeWarrior libc and cannot be
// compiled by ProDG/GCC; shares the dolphin/gx/GXStruct.h guard so both can coexist.

#include "types.h"

#ifndef _DOLPHIN_GX_GXSTRUCT_H_
#define _DOLPHIN_GX_GXSTRUCT_H_

typedef struct {
    u8 r, g, b, a;
} GXColor;

typedef struct {
    s16 r, g, b, a;
} GXColorS10;

typedef struct {
    u32 dummy[8];
} GXTexObj;  // 0x20

typedef struct {
    u32 dummy[3];
} GXTlutObj;  // 0x0C

typedef struct {
    u32 dummy[16];
} GXLightObj;  // 0x40

typedef struct {
    int viTVmode;               // 0x00
    u16 fbWidth;                // 0x04
    u16 efbHeight;              // 0x06
    u16 xfbHeight;              // 0x08
    u16 viXOrigin;              // 0x0A
    u16 viYOrigin;              // 0x0C
    u16 viWidth;                // 0x0E
    u16 viHeight;               // 0x10
    int xFBmode;                // 0x14
    u8 field_rendering;         // 0x18
    u8 aa;                      // 0x19
    u8 sample_pattern[12][2];   // 0x1A
    u8 vfilter[7];              // 0x32
} GXRenderModeObj;              // 0x3C

#endif

// Immediate-mode vertex FIFO (dolphin/gx/GXVert.h). Writes are volatile, so GCC reloads
// everything between them.
#ifndef __GXVERT_H__
#define __GXVERT_H__

#ifdef TARGET_PC
// Aurora (../aurora/lib/dolphin/gx/GXVert.cpp) provides real, external-linkage implementations of
// every one of these functions -- same names/signatures, dolphin-SDK-compatible -- that write into
// its own software GX FIFO. This repo's own `static inline` copies below wrote straight to
// `GXWGFifo`, a real hardware MMIO absolute address (harmlessly meaningless as a plain global on
// host, `src/port/stubs/manual_stubs.cpp`'s `GXWGFifo[1]`) -- but `static inline` gives them
// internal linkage, so every TU that includes this header got its OWN copy that silently discarded
// the write, shadowing Aurora's real, external symbol entirely. Meanwhile any OTHER GX vertex-write
// call not covered by this header's small subset (declared further down, e.g. GXBegin) resolved to
// Aurora's real definition normally -- so a single draw call could have SOME of its vertex fields
// vanish into the dummy array while others genuinely reached Aurora's FIFO, desyncing the vertex
// byte stream (docs/port-boot.md's "indexed XF load from unmapped array 24" investigation). Fixed
// by declaring these here instead of defining them, so the real symbols in libaurora_gx.a satisfy
// them for every TU, the same way GXBegin/GXEnd/GXSetArray/... already do below.
extern "C" {
void GXPosition3f32(f32 x, f32 y, f32 z);
void GXPosition3s16(s16 x, s16 y, s16 z);
void GXColor4u8(u8 r, u8 g, u8 b, u8 a);
void GXNormal3f32(f32 x, f32 y, f32 z);
void GXNormal3s8(s8 x, s8 y, s8 z);
void GXTexCoord2f32(f32 s, f32 t);
void GXPosition2u16(u16 x, u16 y);
void GXTexCoord2s16(s16 s, s16 t);
// GXParam1xx/GXCmd1xx: real, exported Aurora symbols (../aurora/lib/dolphin/gx/GXVert.cpp) that
// each append one raw, appropriately-sized value to the same software GX FIFO buffer every
// function above writes into internally (aurora::gx::fifo::write_u8/u16/u32/f32/...) -- the public
// equivalent of a raw `GXWGFifo->field = value` write on real hardware. No same-named Aurora
// symbol exists for a per-vertex matrix-index write (the real SDK has no dedicated GX*() entry
// point for one either -- games write that byte straight to GXWGFifo, confirmed there is no
// `GXMatrixIndex1x8`/similar symbol anywhere in Aurora, grepped ../aurora/include, ../aurora/lib),
// so GXMatrixIndex1u8 (this repo's own name for that write) below routes through GXParam1u8
// instead, which does the identical FIFO append.
void GXParam1u8(u8 x);
void GXParam1u16(u16 x);
void GXParam1u32(u32 x);
void GXParam1s8(s8 x);
void GXParam1s16(s16 x);
void GXParam1s32(s32 x);
void GXParam1f32(f32 x);
void GXCmd1u64(u64 x);
}

static inline void GXMatrixIndex1u8(u8 idx)
{
    GXParam1u8(idx);
}

// Some units (src/game/mes.cpp's RomFont::draw()/MessageFont-family code, src/game/dbmodule.cpp)
// bypass every GX*() vertex-write wrapper above entirely and write straight to `GXWGFifo->field`
// themselves (the same real-hardware idiom this header's own -- now-fixed -- static inline copies
// used) -- confirmed by grep, not assumed (`grep -rn GXWGFifo src/game`). Those call sites cannot
// change (vendor code, byte-identical requirement) and there is no reason to hunt down every
// current and future occurrence of this idiom individually. Instead, under TARGET_PC, `GXWGFifo`'s
// element type itself is a set of small proxy objects whose `operator=` calls the matching
// GXParam1xx/GXCmd1xx function above -- so `GXWGFifo->s16 = x;` (unchanged source text) now
// genuinely appends `x` to Aurora's real FIFO, the same as if the call site had used one of the
// GXParam1xx functions directly. `u64`/`s64` route through GXCmd1u64 (Aurora has no
// GXParam1u64/s64 -- GXCmd1xx and GXParam1xx both call the identical aurora::gx::fifo::write_*
// primitive, confirmed by reading ../aurora/lib/dolphin/gx/GXVert.cpp, so this is not a semantic
// downgrade). `f64` has no real FIFO primitive at all on either real hardware or Aurora (the real
// SDK's FIFO is 32-bit-word-oriented) and is written by nothing in this tree (confirmed by grep) --
// its proxy aborts if ever exercised rather than silently doing the wrong thing.
struct WGPipe {
    struct U8Proxy {
        void operator=(u8 v) const { GXParam1u8(v); }
    } u8;
    struct U16Proxy {
        void operator=(u16 v) const { GXParam1u16(v); }
    } u16;
    struct U32Proxy {
        void operator=(u32 v) const { GXParam1u32(v); }
    } u32;
    struct U64Proxy {
        void operator=(u64 v) const { GXCmd1u64(v); }
    } u64;
    struct S8Proxy {
        void operator=(s8 v) const { GXParam1s8(v); }
    } s8;
    struct S16Proxy {
        void operator=(s16 v) const { GXParam1s16(v); }
    } s16;
    struct S32Proxy {
        void operator=(s32 v) const { GXParam1s32(v); }
    } s32;
    struct S64Proxy {
        void operator=(s64 v) const { GXCmd1u64(static_cast<::u64>(v)); }
    } s64;
    struct F32Proxy {
        void operator=(f32 v) const { GXParam1f32(v); }
    } f32;
    struct F64Proxy {
        void operator=(f64) const;
    } f64;
};
extern WGPipe GXWGFifo[1];
#else
typedef union {
    u8 u8;
    u16 u16;
    u32 u32;
    u64 u64;
    s8 s8;
    s16 s16;
    s32 s32;
    s64 s64;
    f32 f32;
    f64 f64;
} WGPipe;

// The FIFO is a linker-provided absolute symbol (`GXWGFifo = 0xCC008000` in
// config/G4BE08/ldscript.ld), the way the SDK's GXVert.h declares it for non-CodeWarrior
// compilers. The address must be a SYMBOL_REF, not a constant: the scheduler then issues
// `lis rX, GXWGFifo@ha` before the `lis/lfs` of the other globals in the block and the
// `(u32)` float conversions share their `lis @ha` copies the way the original does. A
// constant address (`(*(volatile WGPipe*)0xCC008000)` or a struct member at 0xCC000000)
// reorders those loads. Declared as an incomplete array because an 8-byte extern object
// would be placed in small data (`@sda21`).
extern volatile WGPipe GXWGFifo[];

static inline void GXPosition3f32(f32 x, f32 y, f32 z)
{
    GXWGFifo->f32 = x;
    GXWGFifo->f32 = y;
    GXWGFifo->f32 = z;
}

static inline void GXPosition3s16(s16 x, s16 y, s16 z)
{
    GXWGFifo->s16 = x;
    GXWGFifo->s16 = y;
    GXWGFifo->s16 = z;
}

static inline void GXColor4u8(u8 r, u8 g, u8 b, u8 a)
{
    GXWGFifo->u8 = r;
    GXWGFifo->u8 = g;
    GXWGFifo->u8 = b;
    GXWGFifo->u8 = a;
}

static inline void GXNormal3f32(f32 x, f32 y, f32 z)
{
    GXWGFifo->f32 = x;
    GXWGFifo->f32 = y;
    GXWGFifo->f32 = z;
}

static inline void GXNormal3s8(s8 x, s8 y, s8 z)
{
    GXWGFifo->s8 = x;
    GXWGFifo->s8 = y;
    GXWGFifo->s8 = z;
}

static inline void GXTexCoord2f32(f32 s, f32 t)
{
    GXWGFifo->f32 = s;
    GXWGFifo->f32 = t;
}

static inline void GXMatrixIndex1u8(u8 idx)
{
    GXWGFifo->u8 = idx;
}

static inline void GXPosition2u16(u16 x, u16 y)
{
    GXWGFifo->u16 = x;
    GXWGFifo->u16 = y;
}

static inline void GXTexCoord2s16(s16 s, s16 t)
{
    GXWGFifo->s16 = s;
    GXWGFifo->s16 = t;
}
#endif
#endif

// GX API entry points used by game code. Enum parameters are declared as plain ints: the
// SDK enum headers pull in the CodeWarrior libc.
#ifdef __cplusplus
extern "C" {
#endif
void GXSetBlendMode(int type, int src_factor, int dst_factor, int op);
void GXSetColorUpdate(u8 update_enable);
void GXSetCullMode(int mode);
void GXSetZMode(u8 compare_enable, int func, u8 update_enable);
void GXSetNumTexGens(u8 n);
void GXSetNumTevStages(u8 n);
void GXSetTevOp(int id, int mode);
void GXSetZCompLoc(u8 before_tex);
void GXSetTevOrder(int stage, int coord, int map, int color);
void GXEnableTexOffsets(int coord, u8 line_enable, u8 point_enable);
void GXSetNumChans(u8 n);
void GXSetChanMatColor(int chan, GXColor color);
void GXSetChanCtrl(int chan, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn);
void GXSetLineWidth(u8 width, int tex_offsets);
void GXSetAlphaCompare(int comp0, u8 ref0, int op, int comp1, u8 ref1);
void GXClearVtxDesc(void);
void GXSetVtxDesc(int attr, int type);
void GXSetVtxAttrFmt(int vtxfmt, int attr, int cnt, int type, u8 frac);
void GXSetArray(int attr, void* base_ptr, u8 stride);
void GXCallDisplayList(void* list, u32 nbytes);
void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id);
void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id);
void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, int type);
void GXSetCurrentMtx(u32 id);
void GXSetProjection(const f32 mtx[4][4], int type);
void GXBegin(int type, int vtxfmt, u16 nverts);
void GXInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap);
void GXInitTexObjCI(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap, u32 tlut_name);
void GXInitTexObjLOD(GXTexObj* obj, int min_filt, int mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, u8 bias_clamp, u8 do_edge_lod, int max_aniso);
void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, int fmt, u16 n_entries);
void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name);
// filter units
void GXLoadTexObj(GXTexObj* obj, int id);
void GXSetTevColor(int id, GXColor color);
void GXSetTevColorIn(int stage, int a, int b, int c, int d);
void GXSetTevAlphaIn(int stage, int a, int b, int c, int d);
void GXSetTevColorOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg);
void GXSetTevAlphaOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg);
void GXSetTexCoordGen2(int dst_coord, int func, int src_param, u32 mtx, u8 normalize, u32 pt_texmtx);
void GXSetAlphaUpdate(u8 update_enable);
void GXSetCopyFilter(u8 aa, const u8 sample_pattern[12][2], u8 vf, const u8 vfilter[7]);
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht);
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht);
void GXSetTexCopyDst(u16 wd, u16 ht, int fmt, u8 mipmap);
void GXCopyTex(void* dest, u8 clear);
void GXPixModeSync(void);
void GXInvalidateTexAll(void);
void GXDrawDone(void);
void GXPeekZ(u16 x, u16 y, u32* z);
void GXSetNumIndStages(u8 nstages);
// sofdec
void GXSetTevSwapMode(int stage, int ras_sel, int tex_sel);
void GXSetTevSwapModeTable(int table, int red, int green, int blue, int alpha);
void GXSetTevKColor(int id, GXColor color);
void GXSetTevKColorSel(int stage, int sel);
void GXSetTevKAlphaSel(int stage, int sel);
void GXSetTevColorS10(int id, GXColorS10 color);
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod);
void GXDrawTorus(f32 rc, u8 numc, u8 numt);
void GXSetTevDirect(int tev_stage);
void GXSetFog(int type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);
void GXSetChanAmbColor(int chan, GXColor color);
void GXSetCopyClear(GXColor clear_clr, u32 clear_z);
// indirect texturing (id_sys)
void GXSetIndTexOrder(int ind_stage, int tex_coord, int tex_map);
void GXSetIndTexCoordScale(int ind_stage, int scale_s, int scale_t);
void GXSetIndTexMtx(int mtx_id, const f32 offset[2][3], s8 scale_exp);
void GXSetTevIndWarp(int tev_stage, int ind_stage, u8 signed_offset, u8 replace_mode, int matrix_sel);
// lighting (trans_lit)
void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2);
void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2);
void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, int spot_func);
void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_distance, f32 ref_brightness, int dist_func);
void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z);
void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz);
void GXInitLightColor(GXLightObj* lt_obj, GXColor color);
void GXLoadLightObjImm(GXLightObj* lt_obj, u32 light);
// shadow
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);
void GXSetDstAlpha(u8 enable, u8 alpha);
void GXClearBoundingBox(void);
// water (Espgen42/espgen45)
void* GXGetTexObjData(GXTexObj* obj);
// filter08
void GXSetDither(u8 dither);
// display copy and EFB setup (main_sub)
u32 GXSetDispCopyYScale(f32 yscale);
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht);
void GXSetDispCopyDst(u16 wd, u16 ht);
void GXSetPixelFmt(int pix_fmt, int z_fmt);
void GXCopyDisp(void* dest, u8 clear);
void GXSetDispCopyGamma(int gamma);
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field);
void GXInvalidateVtxCache(void);
// projection queries (sub2)
void GXGetProjectionv(f32* p);
void GXGetViewportv(f32* vp);
void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz);
// indirect texturing (trans)
void __GXSetIndirectMask(u32 mask);
void GXSetTevIndBumpXYZ(int tev_stage, int ind_stage, int matrix_sel);
#ifdef __cplusplus
}
#endif
#define GXSetTexCoordGen(dst_coord, func, src_param, mtx) GXSetTexCoordGen2(dst_coord, func, src_param, mtx, 0, 125)

#endif
