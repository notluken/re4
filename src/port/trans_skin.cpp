// TARGET_PC C port of src/game/trans.cpp's CalcSk1_x/CalcSk1_x2/setupGQR6 -- see
// include/port/trans_skin.h and docs/port-boot.md section 46.
//
// Independently derived instruction-by-instruction from the vendor's own asm (still present,
// unchanged, in src/game/trans.cpp's `#else` branch for the matching build) and cross-checked
// against it by executing that exact asm text in a from-scratch PowerPC interpreter on
// randomized synthetic inputs: tools/port/ppcsim/test_calc_sk1.py, whose generated reference
// vectors tests/port/test_trans_skin.cpp checks this file's compiled output against, bit-for-bit.
#include "port/trans_skin.h"
#include "port/gqr.h"

#include <dolphin/os/OSCache.h>

#include <cmath>

namespace {

// Mirrors real hardware's GQR6, since arm64 has no such register: the value last passed to
// setupGQR6(), read by CalcSk1_x/CalcSk1_x2 below.
u32 g_gqr6;

// `src`/`dst` are raw big-endian vertex buffers (real hardware's native byte order: model
// geometry loaded straight off disc into `d->vtxOrig`/`d->nrmOrig`, GameCube-native the whole
// way, and `info->pPosBuf`/`pNrmBuf` feed GX display lists, which always expect big-endian
// vertex data regardless of host CPU) -- opaque `u8*`/`void*` buffers in include/model.h, not a
// BE<T>-wrapped struct (there is no per-field struct here, just packed s16/s8 records), so this
// file swaps by hand instead of going through include/port/be.h's BE<T>. Only the s16 case
// (CalcSk1_x's vertex positions/normals) needs it; CalcSk1_x2's s8 elements are single bytes.
inline s16 LoadBE16(const void* p)
{
    const u8* b = (const u8*) p;
    return (s16) ((b[0] << 8) | b[1]);
}
inline void StoreBE16(void* p, s16 v)
{
    u8* b = (u8*) p;
    b[0] = (u8) ((u16) v >> 8);
    b[1] = (u8) v;
}

// `M` is the ROMtx at LCGetBase()+idx*0x30 that src/game/trans.cpp's ReorderMtxToLC() wrote: 12
// floats, column-major (M[col*3+row]) -- the layout the vendor's `ps_madds0`/`ps_madds1` chain
// reads. Row r of the plain 3x4 affine transform `out = Mtx * (x,y,z,1)` is the three chained
// fused multiply-adds real hardware performs (order matters for bit-exactness: this is NOT
// `M[r]*x + M[3+r]*y + M[6+r]*z + M[9+r]` evaluated with unspecified rounding/contraction).
inline void SkinVertexPC(const f32* M, f32 x, f32 y, f32 z, f32 out[3])
{
    for (int r = 0; r < 3; r++) {
        f32 acc = fmaf(M[0 * 3 + r], x, M[9 + r]);
        acc = fmaf(M[1 * 3 + r], y, acc);
        acc = fmaf(M[2 * 3 + r], z, acc);
        out[r] = acc;
    }
}

} // namespace

extern "C" {

void setupGQR6(u32 v)
{
    g_gqr6 = v;
}

// Skin `n` vertices (s16 x/y/z + s16 matrix index, 8 bytes) from src into dst (s16 x/y/z,
// 6 bytes) with the matrix palette in locked cache (LCGetBase(), ROMtx 0x30 each). Both call
// sites (src/game/trans.cpp's commonScreenMatSub) always set GQR6 to S16 on load and store --
// the fixed 2-byte element stride below only makes sense for that type, matching real hardware's
// own fixed instruction encoding for this function.
void CalcSk1_x(void* dst, void* src, u32 n)
{
    const u8* s = (const u8*) src;
    u8* d = (u8*) dst;
    int ldScale = port::GqrLdScale(g_gqr6);
    int stType = port::GqrStType(g_gqr6), stScale = port::GqrStScale(g_gqr6);
    for (u32 i = 0; i < n; i++, s += 8, d += 6) {
        f32 x = port::GqrDequantize(LoadBE16(s + 0), ldScale);
        f32 y = port::GqrDequantize(LoadBE16(s + 2), ldScale);
        f32 z = port::GqrDequantize(LoadBE16(s + 4), ldScale);
        u16 idx = (u16) LoadBE16(s + 6);
        const f32* M = (const f32*) ((const u8*) LCGetBase() + idx * 0x30);
        f32 out[3];
        SkinVertexPC(M, x, y, z, out);
        StoreBE16(d + 0, (s16) port::GqrQuantize(out[0], stType, stScale));
        StoreBE16(d + 2, (s16) port::GqrQuantize(out[1], stType, stScale));
        StoreBE16(d + 4, (s16) port::GqrQuantize(out[2], stType, stScale));
    }
}

// Same for s8 normals (s8 x/y/z + u8 matrix index, 4 bytes) into s8 x/y/z (3 bytes).
void CalcSk1_x2(void* dst, void* src, u32 n)
{
    const s8* s = (const s8*) src;
    s8* d = (s8*) dst;
    int ldScale = port::GqrLdScale(g_gqr6);
    int stType = port::GqrStType(g_gqr6), stScale = port::GqrStScale(g_gqr6);
    for (u32 i = 0; i < n; i++, s += 4, d += 3) {
        f32 x = port::GqrDequantize(s[0], ldScale);
        f32 y = port::GqrDequantize(s[1], ldScale);
        f32 z = port::GqrDequantize(s[2], ldScale);
        u8 idx = (u8) s[3];
        const f32* M = (const f32*) ((const u8*) LCGetBase() + idx * 0x30);
        f32 out[3];
        SkinVertexPC(M, x, y, z, out);
        d[0] = (s8) port::GqrQuantize(out[0], stType, stScale);
        d[1] = (s8) port::GqrQuantize(out[1], stType, stScale);
        d[2] = (s8) port::GqrQuantize(out[2], stType, stScale);
    }
}

} // extern "C"
