"""Gekko (PowerPC 750CL) paired-single GQR quantize/dequantize.

Formula, per the IBM PowerPC 750CL RISC Microprocessor User's Manual (fail0verflow copy,
https://fail0verflow.com/media/files/ppc_750cl.pdf), the "Paired Single Load and Store
Instructions" section: for an integer value I loaded from memory, the float F placed in the
FPR is

    F = I * 2**(-S)

where S is the two's-complement value of the LD_SCALE field of the selected GQR (six bits,
range -32..31). The inverse holds for stores: I = round(F * 2**S), saturated to the
destination integer type's range. GQR_TYPE (LD_TYPE/ST_TYPE, three bits) selects the memory
element's representation:

    0 = f32 (no quantization: the scale field is ignored, the value passes through)
    4 = u8
    5 = u16
    6 = s8
    7 = s16

(types 1-3 are reserved/undefined; unused by this codebase). Bit layout of a 32-bit GQR value
(this matches Dolphin's `UGQR` bitfield, cross-checked against the manual's own field diagram):

    bits 0-2   ST_TYPE
    bits 8-13  ST_SCALE (six-bit two's complement)
    bits 16-18 LD_TYPE
    bits 24-29 LD_SCALE (six-bit two's complement)
"""

TYPE_F32 = 0
TYPE_U8 = 4
TYPE_U16 = 5
TYPE_S8 = 6
TYPE_S16 = 7

_RANGES = {
    TYPE_U8: (0, 255),
    TYPE_U16: (0, 65535),
    TYPE_S8: (-128, 127),
    TYPE_S16: (-32768, 32767),
}


def sext6(v: int) -> int:
    """Sign-extend a 6-bit field to a Python int."""
    v &= 0x3F
    return v - 64 if v & 0x20 else v


def gqr_fields(gqr: int):
    st_type = gqr & 0x7
    st_scale = sext6((gqr >> 8) & 0x3F)
    ld_type = (gqr >> 16) & 0x7
    ld_scale = sext6((gqr >> 24) & 0x3F)
    return ld_type, ld_scale, st_type, st_scale


def dequantize(raw: int, elem_type: int, scale: int) -> float:
    if elem_type == TYPE_F32:
        raise ValueError("f32 elements are not quantized; read the float directly")
    return float(raw) * (2.0 ** (-scale))


def quantize(value: float, elem_type: int, scale: int) -> int:
    if elem_type == TYPE_F32:
        raise ValueError("f32 elements are not quantized; write the float directly")
    lo, hi = _RANGES[elem_type]
    q = value * (2.0 ** scale)
    # PPC paired-single stores round to nearest (ties handled by the hardware's own convention;
    # this codebase's data never lands exactly on a tie in practice, so plain round() is enough
    # for this simulator's cross-check purpose) and saturate.
    i = int(round(q))
    if i < lo:
        i = lo
    if i > hi:
        i = hi
    return i
