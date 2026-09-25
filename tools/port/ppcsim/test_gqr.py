#!/usr/bin/env python3
"""Unit tests for tools/port/ppcsim/gqr.py's quantize/dequantize -- known values, run directly
(no pytest dependency): `python3 tools/port/ppcsim/test_gqr.py`.

Cross-checked against two independent, authoritative sources (both cited in gqr.py's own
docstring): the IBM PowerPC 750CL user's manual's stated formula F = I * 2**(-S), and Dolphin's
`m_dequantizeTable`/`m_quantizeTable` (Source/Core/Core/PowerPC/Interpreter/
Interpreter_LoadStorePaired.cpp) -- table[i] for i in 0..31 is 1/2**i (positive/small-index
scale = divide), for i in 32..63 is 2**(64-i) (large-index scale = multiply); this project's own
`include/dolphin/base/PPCArch.h` (`GQR_LOAD_SCALE_MASK` etc, `PPC_GQR_t`) independently confirms
the bit-field layout (bits 0-2 store type, 8-13 store scale, 16-18 load type, 24-29 load scale).
"""
import sys

sys.path.insert(0, __file__.rsplit("/tools/port/ppcsim/", 1)[0] + "/tools/port")

from ppcsim import gqr

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        print(f"FAIL: {msg}")
        failures += 1


# --- sext6: two's complement of a 6-bit field ---------------------------------------------
check(gqr.sext6(0) == 0, "sext6(0)")
check(gqr.sext6(31) == 31, "sext6(31)")
check(gqr.sext6(32) == -32, "sext6(32)")
check(gqr.sext6(63) == -1, "sext6(63)")
check(gqr.sext6(50) == -14, "sext6(50)")  # the value trans.cpp's 0x32073207 actually encodes

# --- gqr_fields: bit layout, cross-checked against include/dolphin/base/PPCArch.h's masks ---
check(gqr.gqr_fields(0x00070007) == (7, 0, 7, 0), "gqr_fields(0x00070007)")
check(gqr.gqr_fields(0x32073207) == (7, -14, 7, -14), "gqr_fields(0x32073207)")
check(gqr.gqr_fields(0x20062006) == (6, -32, 6, -32), "gqr_fields(0x20062006)")
# A value with distinct fields, hand-picked to catch a transposed-field bug: ST_TYPE=4(u8),
# ST_SCALE=3, LD_TYPE=7(s16), LD_SCALE=10.
v = (10 << 24) | (7 << 16) | (3 << 8) | 4
check(gqr.gqr_fields(v) == (7, 10, 4, 3), f"gqr_fields(0x{v:08x}) distinct-fields")

# --- dequantize: known values, both scale directions ---------------------------------------
check(gqr.dequantize(128, gqr.TYPE_S16, 7) == 1.0, "dequantize s16 scale=7 (divide by 128)")
check(gqr.dequantize(-256, gqr.TYPE_S16, 8) == -1.0, "dequantize s16 scale=8 (divide by 256)")
check(gqr.dequantize(1, gqr.TYPE_S16, -4) == 16.0, "dequantize s16 scale=-4 (multiply by 16)")
check(gqr.dequantize(127, gqr.TYPE_S8, 7) == 127.0 / 128.0, "dequantize s8 scale=7")
check(gqr.dequantize(255, gqr.TYPE_U8, 8) == 255.0 / 256.0, "dequantize u8 scale=8")

# --- quantize: inverse of dequantize at the same scale, plus saturation --------------------
check(gqr.quantize(1.0, gqr.TYPE_S16, 7) == 128, "quantize s16 scale=7")
check(gqr.quantize(100.0, gqr.TYPE_S16, 7) == 12800, "quantize s16 scale=7, larger value")
check(gqr.quantize(1000.0, gqr.TYPE_S8, 0) == 127, "quantize s8 saturates to max")
check(gqr.quantize(-1000.0, gqr.TYPE_S8, 0) == -128, "quantize s8 saturates to min")
check(gqr.quantize(-1.0, gqr.TYPE_U8, 0) == 0, "quantize u8 saturates to min (unsigned)")
check(gqr.quantize(300.0, gqr.TYPE_U8, 0) == 255, "quantize u8 saturates to max")

# --- round-trip: dequantize(quantize(x)) ~= x for values that fit exactly ------------------
for x in (0, 1, -1, 100, -100, 32767, -32768):
    q = gqr.quantize(float(x), gqr.TYPE_S16, 0)
    check(q == x, f"round-trip s16 scale=0: quantize({x}) == {x}, got {q}")

if failures:
    print(f"{failures} FAILED")
    sys.exit(1)
print("test_gqr: all checks passed")
