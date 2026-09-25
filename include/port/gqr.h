// Host (TARGET_PC) equivalents of the Gekko (PowerPC 750CL) paired-single GQR quantize/
// dequantize operations `psq_l`/`psq_lu`/`psq_st`/`psq_stu` perform on real hardware, for the
// small number of src/game units whose whole-function vendor asm reads/writes memory through a
// GQR (currently src/game/trans.cpp's CalcSk1_x/CalcSk1_x2 -- see that file for how the GQR6
// value in effect at the time is tracked on the host, since arm64 has no GQR of its own).
//
// Formula, per the IBM PowerPC 750CL RISC Microprocessor User's Manual (the "Paired Single Load
// and Store Instructions" section): for an integer value I read from/written to memory, the
// float F held in the FPR is F = I * 2**(-S), where S is the two's-complement value of the
// LD_SCALE (load) or ST_SCALE (store) field of the selected GQR (six bits, signed range
// -32..31). Cross-checked against Dolphin's own interpreter
// (Source/Core/Core/PowerPC/Interpreter/Interpreter_LoadStorePaired.cpp,
// `m_dequantizeTable`/`m_quantizeTable`: table[i] = 1/2**i for i in 0..31, 2**(64-i) for i in
// 32..63 -- algebraically the same two's-complement formula, just via a lookup table) and
// against this project's own `include/dolphin/base/PPCArch.h` (`GQR_LOAD_SCALE_MASK` etc,
// `PPC_GQR_t`), which independently confirms the bit layout used below. Also mirrored, with the
// same citations, in tools/port/ppcsim/gqr.py (the Python reference used to cross-check
// CalcSk1_x/CalcSk1_x2's C port against the vendor's own asm executed by a from-scratch PPC
// instruction interpreter -- see tools/port/ppcsim/test_calc_sk1.py).
//
// GQR type codes (LD_TYPE/ST_TYPE, three bits): 0 = f32 (no quantization), 4 = u8, 5 = u16,
// 6 = s8, 7 = s16 (1-3 reserved, unused anywhere in this codebase).
//
// Bit layout of a 32-bit GQR value: bits 0-2 ST_TYPE, bits 8-13 ST_SCALE, bits 16-18 LD_TYPE,
// bits 24-29 LD_SCALE.
#ifndef PORT_GQR_H
#define PORT_GQR_H

#include "types.h"

#include <cmath>
#include <cstdint>

namespace port {

enum GqrType { GQR_TYPE_F32 = 0, GQR_TYPE_U8 = 4, GQR_TYPE_U16 = 5, GQR_TYPE_S8 = 6, GQR_TYPE_S16 = 7 };

inline int GqrLdType(u32 gqr) { return (int) ((gqr >> 16) & 0x7); }
inline int GqrLdScale(u32 gqr)
{
    int s = (int) ((gqr >> 24) & 0x3F);
    return (s & 0x20) ? s - 64 : s; // sign-extend the 6-bit field
}
inline int GqrStType(u32 gqr) { return (int) (gqr & 0x7); }
inline int GqrStScale(u32 gqr)
{
    int s = (int) ((gqr >> 8) & 0x3F);
    return (s & 0x20) ? s - 64 : s;
}

// Dequantizes one already-read integer element (sign already applied by the caller for s8/s16).
inline f32 GqrDequantize(s32 raw, int scale) { return (f32) raw * powf(2.0f, (f32) -scale); }

// Quantizes one f32 to the destination integer type, saturating -- the direction real hardware's
// `psq_st`/`psq_stu` uses (inverse scale from GqrDequantize, since dequant/quant are exact
// inverses at the same scale by construction: F = I * 2**-S, I = round(F * 2**S)).
inline s32 GqrQuantize(f32 value, int type, int scale)
{
    f32 q = value * powf(2.0f, (f32) scale);
    s32 i = (s32) lroundf(q);
    switch (type) {
    case GQR_TYPE_U8:
        return (i < 0) ? 0 : (i > 255 ? 255 : i);
    case GQR_TYPE_S8:
        return (i < -128) ? -128 : (i > 127 ? 127 : i);
    case GQR_TYPE_U16:
        return (i < 0) ? 0 : (i > 65535 ? 65535 : i);
    case GQR_TYPE_S16:
    default:
        return (i < -32768) ? -32768 : (i > 32767 ? 32767 : i);
    }
}

} // namespace port

#endif
